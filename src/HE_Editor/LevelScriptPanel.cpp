#include "LevelScriptPanel.h"
#include "EditorApplication.h"   // AppContext
#include "EditorUndo.h"          // scene-undo snapshots (dirty tracking + undo/redo)
#include "GraphEditor.h"         // shared node-graph canvas
#include <HorizonScene/HorizonWorld.h>
#include <HorizonCode/HorizonCode.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

// ── Level Script editor ──────────────────────────────────────────────────────
// A trimmed HorizonCode graph editor for the current scene's level script. It
// shares the GraphEditor canvas with the Material + UI Widget editors but, since
// a level script has no widget/element target, it drops the element machinery:
// the Event node offers a fixed world-event catalog and there are no Get/Set
// Property nodes. The node-plumbing helpers below are small and derive entirely
// from HorizonCode::signatureOf() (the single source of truth in HE_Core), so
// they stay in step with the widget editor's copies without sharing state.
// (Unifying the two HorizonCode frontends is tracked as future work.)

namespace
{
namespace HC = HorizonCode;
using PT = HC::PinType;
using NT = HC::NodeType;

// World events a level script can react to. Free-standing (no element target).
const char* const kLevelEvents[] = { "OnLevelLoaded", "OnLevelUnloaded" };

// ── Node plumbing (all derived from HC::signatureOf) ──────────────────────────

ImU32 pinColor(PT t)
{
	switch (t)
	{
		case PT::Exec:   return IM_COL32(235, 235, 235, 255);
		case PT::Float:  return IM_COL32(160, 200, 120, 255);
		case PT::Bool:   return IM_COL32(210,  90,  90, 255);
		case PT::Int:    return IM_COL32(110, 200, 200, 255);
		case PT::String: return IM_COL32(220, 130, 210, 255);
		case PT::Vec2:   return IM_COL32(120, 200, 210, 255);
		case PT::Color:  return IM_COL32(230, 210, 110, 255);
	}
	return IM_COL32_WHITE;
}

const char* pinTypeName(PT t)
{
	switch (t)
	{
		case PT::Float:  return "Float";
		case PT::Bool:   return "Bool";
		case PT::Int:    return "Int";
		case PT::String: return "String";
		case PT::Vec2:   return "Vec2";
		case PT::Color:  return "Color";
		default:         return "Exec";
	}
}

// Unified pin index layout: [execIns][execOuts][dataIns][dataOuts].
struct PinRanges { int execIn0, execOut0, dataIn0, dataOut0, end; };
PinRanges pinRanges(const HC::Node& n)
{
	const HC::NodeSig s = HC::signatureOf(n);
	PinRanges r;
	r.execIn0  = 0;
	r.execOut0 = r.execIn0  + (int)s.execIns.size();
	r.dataIn0  = r.execOut0 + (int)s.execOuts.size();
	r.dataOut0 = r.dataIn0  + (int)s.dataIns.size();
	r.end      = r.dataOut0 + (int)s.dataOuts.size();
	return r;
}

// Pins for the GraphEditor, in unified index order (positions are laid out by
// the canvas itself, so only id/label/type/side/exec-ness are provided).
std::vector<GraphEditor::Pin> nodePins(const HC::Node& n)
{
	std::vector<GraphEditor::Pin> out;
	const HC::NodeSig s = HC::signatureOf(n);
	int id = 0;
	auto push = [&](const HC::PinDesc& pd, bool input, bool isExec)
	{
		GraphEditor::Pin p;
		p.id = id++; p.label = pd.name ? pd.name : "";
		p.color = pinColor(pd.type); p.input = input; p.isExec = isExec;
		out.push_back(std::move(p));
	};
	for (const auto& pd : s.execIns)  push(pd, true,  true);
	for (const auto& pd : s.execOuts) push(pd, false, true);
	for (const auto& pd : s.dataIns)  push(pd, true,  false);
	for (const auto& pd : s.dataOuts) push(pd, false, false);
	return out;
}

std::string nodeTitle(const HC::Node& n)
{
	const char* base = HC::nodeDisplayName(n.type);
	switch (n.type)
	{
		case NT::Event:        return n.s.empty() ? std::string(base) : n.s;
		case NT::GetVariable:  return "Get " + (n.s.empty() ? std::string("var") : n.s);
		case NT::SetVariable:  return "Set " + (n.s.empty() ? std::string("var") : n.s);
		case NT::FunctionEntry:
		case NT::FunctionCall: return std::string(base) + " " + n.s;
		default:               return base;
	}
}

void removePinLinks(HC::Graph& g, int nodeId, int pin)
{
	g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
		[&](const HC::Link& l){
			return (l.srcNode == nodeId && l.srcPin == pin) ||
			       (l.dstNode == nodeId && l.dstPin == pin);
		}), g.links.end());
}

std::string uniqueFunctionName(const HC::Graph& g)
{
	for (int i = 1; i < 1000; ++i)
	{
		const std::string name = i == 1 ? "NewFunction" : ("NewFunction" + std::to_string(i));
		bool taken = false;
		for (const auto& n : g.nodes)
			if (n.type == NT::FunctionEntry && n.s == name) { taken = true; break; }
		if (!taken) return name;
	}
	return "NewFunction";
}

std::string uniqueVarName(const HC::Graph& g)
{
	for (int i = 1; i < 1000; ++i)
	{
		const std::string name = i == 1 ? "NewVar" : ("NewVar" + std::to_string(i));
		if (!g.findVariable(name)) return name;
	}
	return "NewVar";
}

int addNode(HC::Graph& g, NT type, const ImVec2& pos)
{
	HC::Node n;
	n.type = type;
	n.x = pos.x; n.y = pos.y;
	if (type == NT::ConstColor) { n.f[0] = n.f[1] = n.f[2] = n.f[3] = 1.0f; }
	if (type == NT::FunctionEntry) n.s = uniqueFunctionName(g);
	return g.addNode(std::move(n));
}

// ── Persistent panel state (the panel edits the current scene's graph) ────────
struct LSState
{
	GraphEditor::State ge;
	int         selectedNode = 0;
	bool        focusSelected = false;
	std::string selectedVar;        // variable selected in the left panel
	std::string varNameEdit;        // scratch rename buffer (see the widget editor bug)
	std::string varNameEditFor;
	std::string dropVar;            // variable dragged onto the canvas
	bool        openVarDrop = false;
};
LSState g;

const char* kVarPayload = "HE_LSGRAPH_VAR";

// Search helper for the add-menu.
std::string lower(std::string v)
{
	std::transform(v.begin(), v.end(), v.begin(),
		[](unsigned char c){ return (char)std::tolower(c); });
	return v;
}

// ── Left sidebar: variables + functions + details ─────────────────────────────

void drawVariables(HC::Graph& graph, bool& edited)
{
	ImGui::SeparatorText("Variables");
	if (ImGui::SmallButton("+ Add##var"))
	{
		HC::Variable v;
		v.name = uniqueVarName(graph);
		graph.variables.push_back(v);
		g.selectedVar = v.name;
		g.selectedNode = 0;
		edited = true;
	}
	for (const auto& v : graph.variables)
	{
		ImGui::PushID(v.name.c_str());
		const std::string label = v.name + "  (" + pinTypeName(v.type) + ")";
		if (ImGui::Selectable(label.c_str(), g.selectedVar == v.name))
		{
			g.selectedVar = v.name;
			g.selectedNode = 0;
		}
		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
		{
			char buf[64] = {};
			std::strncpy(buf, v.name.c_str(), sizeof(buf) - 1);
			ImGui::SetDragDropPayload(kVarPayload, buf, sizeof(buf));
			ImGui::TextUnformatted(v.name.c_str());
			ImGui::EndDragDropSource();
		}
		ImGui::PopID();
	}
}

void drawFunctions(HC::Graph& graph, bool& edited)
{
	ImGui::SeparatorText("Functions");
	if (ImGui::SmallButton("+ Add##fn"))
	{
		// Place new function entries in a tidy column near the origin.
		int count = 0;
		for (const auto& n : graph.nodes) if (n.type == NT::FunctionEntry) ++count;
		const int id = addNode(graph, NT::FunctionEntry, ImVec2(40.0f, 260.0f + count * 90.0f));
		g.selectedNode = id;
		g.selectedVar.clear();
		g.focusSelected = true;
		edited = true;
	}
	for (const auto& n : graph.nodes)
	{
		if (n.type != NT::FunctionEntry) continue;
		ImGui::PushID(n.id);
		const std::string label = (n.s.empty() ? "(unnamed)" : n.s) +
		                          (n.access == 1 ? "  [private]" : "");
		if (ImGui::Selectable(label.c_str(), g.selectedNode == n.id))
		{
			g.selectedNode = n.id;
			g.selectedVar.clear();
			g.focusSelected = true;
		}
		ImGui::PopID();
	}
}

// Detail editor for the selected variable.
void drawVariableDetails(HC::Graph& graph, bool& edited)
{
	HC::Variable* v = graph.findVariable(g.selectedVar);
	if (!v) { g.selectedVar.clear(); return; }

	ImGui::TextDisabled("Variable");
	ImGui::Separator();

	// Edit the name through a scratch buffer, not v->name: the variable is keyed
	// by name (selectedVar) and Get/Set nodes reference it by name, so mutating
	// it per keystroke would make the lookup miss and drop focus. Commit on
	// deactivate. Re-seed the buffer when a different variable is shown.
	if (g.varNameEditFor != v->name)
	{
		g.varNameEdit = v->name;
		g.varNameEditFor = v->name;
	}
	ImGui::InputText("Name", &g.varNameEdit);
	if (ImGui::IsItemDeactivatedAfterEdit())
	{
		const std::string oldName = v->name;
		const std::string nn = g.varNameEdit;
		if (nn.empty() || (nn != oldName && graph.findVariable(nn)))
		{
			g.varNameEdit = oldName; // reject blank/clash
		}
		else if (nn != oldName)
		{
			v->name = nn;
			for (auto& n : graph.nodes)
				if ((n.type == NT::GetVariable || n.type == NT::SetVariable) && n.s == oldName)
					n.s = nn;
			g.selectedVar = nn;
			g.varNameEditFor = nn;
			edited = true;
		}
	}

	int typeIdx = (int)v->type;
	if (ImGui::Combo("Type", &typeIdx, "Exec\0Float\0Bool\0Int\0String\0Vec2\0Color\0"))
	{
		const PT nt = (PT)typeIdx;
		if (nt != PT::Exec && nt != v->type)
		{
			v->type = nt;
			for (auto& n : graph.nodes)
				if ((n.type == NT::GetVariable || n.type == NT::SetVariable) && n.s == v->name)
				{
					n.propType = nt;
					const PinRanges r = pinRanges(n);
					const int valuePin = n.type == NT::GetVariable ? r.dataOut0 : r.dataIn0;
					removePinLinks(graph, n.id, valuePin);
				}
			edited = true;
		}
	}

	ImGui::SeparatorText("Default");
	switch (v->type)
	{
		case PT::Float:  if (ImGui::DragFloat("##vdef", &v->f[0], 0.1f)) edited = true; break;
		case PT::Int:  { int iv = (int)v->f[0]; if (ImGui::DragInt("##vdef", &iv)) { v->f[0] = (float)iv; edited = true; } break; }
		case PT::Bool: { bool b = v->f[0] != 0.0f; if (ImGui::Checkbox("##vdef", &b)) { v->f[0] = b ? 1.0f : 0.0f; edited = true; } break; }
		case PT::String: ImGui::InputText("##vdef", &v->s); if (ImGui::IsItemDeactivatedAfterEdit()) edited = true; break;
		case PT::Vec2:   if (ImGui::DragFloat2("##vdef", v->f, 0.1f)) edited = true; break;
		case PT::Color:  if (ImGui::ColorEdit4("##vdef", v->f)) edited = true; break;
		default: break;
	}

	ImGui::Spacing();
	if (ImGui::Button("Delete Variable"))
	{
		const std::string gone = v->name;
		graph.variables.erase(std::remove_if(graph.variables.begin(), graph.variables.end(),
			[&](const HC::Variable& vv){ return vv.name == gone; }), graph.variables.end());
		g.selectedVar.clear();
		edited = true;
	}
}

// Detail editor for the selected node.
void drawNodeDetails(HC::Graph& graph, bool& edited)
{
	HC::Node* n = graph.findNode(g.selectedNode);
	if (!n) { g.selectedNode = 0; return; }

	ImGui::TextDisabled("%s", HC::nodeDisplayName(n->type));
	ImGui::Separator();

	switch (n->type)
	{
	case NT::Event:
	{
		if (ImGui::BeginCombo("Event", n->s.empty() ? "(none)" : n->s.c_str()))
		{
			for (const char* ev : kLevelEvents)
				if (ImGui::Selectable(ev, n->s == ev))
				{
					n->s = ev; n->hasArg = false; n->elem = 0;
					edited = true;
				}
			ImGui::EndCombo();
		}
		ImGui::TextDisabled("Fires once when the level %s.",
			n->s == "OnLevelUnloaded" ? "unloads" : "loads");
		break;
	}
	case NT::FunctionEntry:
	{
		std::string oldName = n->s;
		ImGui::InputText("Name", &n->s);
		if (ImGui::IsItemDeactivatedAfterEdit())
		{
			if (!n->s.empty() && n->s != oldName)
				for (auto& c : graph.nodes)
					if (c.type == NT::FunctionCall && c.s == oldName) c.s = n->s;
			edited = true;
		}
		int access = n->access;
		if (ImGui::Combo("Access", &access, "public\0private\0"))
		{
			n->access = access; edited = true;
		}
		ImGui::TextDisabled("public functions are callable from Lua/Python.");
		break;
	}
	case NT::FunctionCall:
	{
		if (ImGui::BeginCombo("Function", n->s.empty() ? "(none)" : n->s.c_str()))
		{
			for (const auto& e : graph.nodes)
				if (e.type == NT::FunctionEntry && !e.s.empty())
					if (ImGui::Selectable(e.s.c_str(), n->s == e.s)) { n->s = e.s; edited = true; }
			ImGui::EndCombo();
		}
		break;
	}
	case NT::GetVariable:
	case NT::SetVariable:
	{
		if (ImGui::BeginCombo("Variable", n->s.empty() ? "(none)" : n->s.c_str()))
		{
			for (const auto& v : graph.variables)
				if (ImGui::Selectable(v.name.c_str(), n->s == v.name))
				{
					const PT before = n->propType;
					n->s = v.name; n->propType = v.type;
					if (n->propType != before)
					{
						const PinRanges r = pinRanges(*n);
						const int valuePin = n->type == NT::GetVariable ? r.dataOut0 : r.dataIn0;
						removePinLinks(graph, n->id, valuePin);
					}
					edited = true;
				}
			ImGui::EndCombo();
		}
		break;
	}
	case NT::ConstFloat:
		if (ImGui::DragFloat("Value", &n->f[0], 0.1f)) edited = true; break;
	case NT::ConstInt:
	{
		int v = (int)n->f[0];
		if (ImGui::DragInt("Value", &v, 1)) { n->f[0] = (float)v; edited = true; } break;
	}
	case NT::ConstBool:
	{
		bool b = n->f[0] != 0.0f;
		if (ImGui::Checkbox("Value", &b)) { n->f[0] = b ? 1.0f : 0.0f; edited = true; } break;
	}
	case NT::ConstString:
		ImGui::InputText("Value", &n->s);
		if (ImGui::IsItemDeactivatedAfterEdit()) edited = true; break;
	case NT::ConstVec2:
		if (ImGui::DragFloat2("Value", n->f, 0.1f)) edited = true; break;
	case NT::ConstColor:
		if (ImGui::ColorEdit4("Value", n->f)) edited = true; break;
	default:
		ImGui::TextDisabled("No parameters.");
		break;
	}
}

// ── Canvas ────────────────────────────────────────────────────────────────────

bool drawCanvas(HC::Graph& graph, const ImVec2& avail)
{
	g.ge.selected = g.selectedNode;
	if (g.focusSelected) { g.ge.focusNode = g.selectedNode; g.focusSelected = false; }

	GraphEditor::Model m;
	m.multiSelect = true;
	m.nodeIds = [&graph]{ std::vector<int> ids; ids.reserve(graph.nodes.size());
		for (const auto& n : graph.nodes) ids.push_back(n.id); return ids; };
	m.getPos = [&graph](int id, float& x, float& y){ if (const HC::Node* n = graph.findNode(id)) { x = n->x; y = n->y; } };
	m.setPos = [&graph](int id, float x, float y){ if (HC::Node* n = graph.findNode(id)) { n->x = x; n->y = y; } };
	m.title  = [&graph](int id){ const HC::Node* n = graph.findNode(id); return n ? nodeTitle(*n) : std::string(); };
	m.headerColor = [&graph](int id){ const HC::Node* n = graph.findNode(id);
		return GraphEditor::categoryColor(n ? HC::nodeCategory(n->type) : ""); };
	m.pins = [&graph](int id){ const HC::Node* n = graph.findNode(id);
		return n ? nodePins(*n) : std::vector<GraphEditor::Pin>{}; };
	m.links = [&graph]{ std::vector<std::array<int,4>> ls; ls.reserve(graph.links.size());
		for (const auto& l : graph.links) ls.push_back({ l.srcNode, l.srcPin, l.dstNode, l.dstPin }); return ls; };
	m.connect = [&graph](int oN, int oP, int iN, int iP){ return graph.connect(oN, oP, iN, iP); };
	m.clearPinLinks = [&graph](int node, int pin, bool){ removePinLinks(graph, node, pin); };
	m.removeNode = [&graph](int id){ graph.removeNode(id); };

	// Searchable add-node palette: world events + generic node categories +
	// per-variable Get/Set + per-function Call. Property/Widget nodes and the
	// element machinery are intentionally absent.
	m.drawAddMenu = [&graph]() -> int {
		int created = 0;
		static std::string s_search;
		if (ImGui::IsWindowAppearing()) { s_search.clear(); ImGui::SetKeyboardFocusHere(); }
		ImGui::SetNextItemWidth(220.0f);
		ImGui::InputTextWithHint("##nodeSearch", "Search nodes...", &s_search);
		ImGui::Separator();
		const std::string q = lower(s_search);
		auto matches = [&](const std::string& name, const std::string& cat)
		{ return q.empty() || lower(name).find(q) != std::string::npos
		      || lower(cat).find(q) != std::string::npos; };

		ImGui::BeginChild("##nodeList", ImVec2(232.0f, 300.0f));

		// Events (world event catalog).
		bool eh = false;
		for (const char* ev : kLevelEvents)
		{
			if (!matches(ev, "Events")) continue;
			if (!eh) { ImGui::TextDisabled("Events"); eh = true; }
			if (ImGui::Selectable(ev))
			{
				const int id = addNode(graph, NT::Event, g.ge.addMenuGraphPos);
				HC::Node* nn = graph.findNode(id);
				nn->s = ev; nn->hasArg = false; nn->elem = 0;
				created = id; ImGui::CloseCurrentPopup();
			}
		}
		if (eh) ImGui::Spacing();

		// Generic node categories (widget/property nodes excluded).
		static const char* kCats[] = { "Flow", "Literals", "Math", "Logic", "String", "Debug" };
		for (const char* cat : kCats)
		{
			bool header = false;
			for (NT t : HC::nodeRegistry())
			{
				if (t == NT::Event || t == NT::FunctionEntry || t == NT::FunctionCall ||
				    t == NT::GetVariable || t == NT::SetVariable ||
				    t == NT::GetProperty || t == NT::SetProperty ||
				    t == NT::ShowWidget || t == NT::HideWidget) continue;
				if (std::string(HC::nodeCategory(t)) != cat) continue;
				if (!matches(HC::nodeDisplayName(t), cat)) continue;
				if (!header) { ImGui::TextDisabled("%s", cat); header = true; }
				if (ImGui::Selectable(HC::nodeDisplayName(t)))
				{ created = addNode(graph, t, g.ge.addMenuGraphPos); ImGui::CloseCurrentPopup(); }
			}
			if (header) ImGui::Spacing();
		}

		// Call <function> for each declared function entry.
		bool fh = false;
		for (const auto& e : graph.nodes)
		{
			if (e.type != NT::FunctionEntry || e.s.empty()) continue;
			const std::string lbl = "Call " + e.s;
			if (!matches(lbl, "Functions")) continue;
			if (!fh) { ImGui::TextDisabled("Functions"); fh = true; }
			if (ImGui::Selectable(lbl.c_str()))
			{
				const int id = addNode(graph, NT::FunctionCall, g.ge.addMenuGraphPos);
				graph.findNode(id)->s = e.s;
				created = id; ImGui::CloseCurrentPopup();
			}
		}
		if (fh) ImGui::Spacing();

		// Get/Set for each declared variable.
		bool vh = false;
		for (const auto& v : graph.variables)
			for (int k = 0; k < 2; ++k)
			{
				const std::string lbl = (k == 0 ? "Get " : "Set ") + v.name;
				if (!matches(lbl, "Variables")) continue;
				if (!vh) { ImGui::TextDisabled("Variables"); vh = true; }
				if (ImGui::Selectable(lbl.c_str()))
				{
					const int id = addNode(graph, k == 0 ? NT::GetVariable : NT::SetVariable,
					                       g.ge.addMenuGraphPos);
					HC::Node* nn = graph.findNode(id);
					nn->s = v.name; nn->propType = v.type;
					created = id; ImGui::CloseCurrentPopup();
				}
			}
		ImGui::EndChild();
		return created;
	};

	// Variable drag from the left panel → Get/Set popup.
	m.dropPayloads = { kVarPayload };
	m.onDrop = [](const char* type, const void* data, ImVec2 gp){
		(void)type;
		g.ge.addMenuGraphPos = gp;
		g.dropVar = static_cast<const char*>(data);
		g.openVarDrop = true;
	};

	const bool changed = GraphEditor::draw("##ls_canvas", m, g.ge, avail);
	g.selectedNode = g.ge.selected;
	return changed;
}

} // namespace

void LevelScriptPanel::render(AppContext& ctx, bool& open)
{
	if (!open) return;

	ImGui::SetNextWindowSize(ImVec2(920.0f, 560.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Level Script", &open))
	{
		ImGui::End();
		return;
	}

	if (!ctx.world)
	{
		ImGui::TextDisabled("Open a scene to edit its level script.");
		ImGui::End();
		return;
	}

	HC::Graph& graph = ctx.world->levelScript();

	bool edited = false;

	ImGui::BeginChild("##ls_side", ImVec2(220.0f, 0.0f), true);
	ImGui::TextUnformatted("Level Script");
	ImGui::TextDisabled("Reacts to world events.");
	ImGui::Spacing();
	drawVariables(graph, edited);
	ImGui::Spacing();
	drawFunctions(graph, edited);
	ImGui::Spacing();
	ImGui::Separator();
	// Details for whatever is selected (a node takes priority over a variable).
	if (g.selectedNode != 0)      drawNodeDetails(graph, edited);
	else if (!g.selectedVar.empty()) drawVariableDetails(graph, edited);
	else ImGui::TextDisabled("Select a node or variable.");
	ImGui::EndChild();

	ImGui::SameLine();

	ImGui::BeginChild("##ls_canvas_host", ImVec2(0.0f, 0.0f), true);
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	if (drawCanvas(graph, avail)) edited = true;

	// Variable drop → Get/Set popup.
	if (g.openVarDrop) { ImGui::OpenPopup("##ls_var_drop"); g.openVarDrop = false; }
	if (ImGui::BeginPopup("##ls_var_drop"))
	{
		const HC::Variable* v = graph.findVariable(g.dropVar);
		ImGui::TextDisabled("%s", g.dropVar.c_str());
		ImGui::Separator();
		auto make = [&](NT type)
		{
			const int id = addNode(graph, type, g.ge.addMenuGraphPos);
			HC::Node* nn = graph.findNode(id);
			nn->s = g.dropVar;
			if (v) nn->propType = v->type;
			g.selectedNode = id;
			edited = true;
		};
		if (ImGui::MenuItem("Get", nullptr, false, v != nullptr)) make(NT::GetVariable);
		if (ImGui::MenuItem("Set", nullptr, false, v != nullptr)) make(NT::SetVariable);
		ImGui::EndPopup();
	}
	ImGui::EndChild();

	// Push a scene snapshot when the level graph changed. snapshotNow() is
	// self-contained (it doesn't touch the shared capturePre scratch, so it can't
	// disturb the main editor's entity undo) and bumps the undo revision, which
	// marks the scene dirty so the level script saves with the scene (Ctrl+S).
	// Trade-off: because it snapshots the post-edit state, undoing a level-graph
	// change takes one extra Ctrl+Z. Acceptable until the two HorizonCode editors
	// are unified onto a shared, finer-grained undo path.
	if (edited && ctx.undoSys) ctx.undoSys->snapshotNow();

	ImGui::End();
}
