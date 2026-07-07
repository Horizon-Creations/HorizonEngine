#include "LevelScriptPanel.h"
#include "GameInstancePanel.h"
#include "HorizonCodeClassPanel.h"
#include "HcClassList.h"
#include "EditorApplication.h"   // AppContext
#include "EditorUndo.h"          // scene-undo snapshots (dirty tracking + undo/redo)
#include "GraphEditor.h"         // shared node-graph canvas
#include <HorizonScene/HorizonWorld.h>
#include <HorizonCode/HorizonCode.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/HAsset.h>
#include <Types/Enums.h>
#include <filesystem>
#include <map>
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

// ── Node plumbing (all derived from HC::signatureOf) ──────────────────────────

ImU32 pinColor(PT t) { return HcEditorUtil::pinTypeColor(t); }

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
		case PT::Ref:    return "Object";
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
		case NT::BindEvent:    return "Bind " + (n.s.empty() ? std::string("event") : n.s);
		case NT::EmitEvent:    return "Emit " + (n.s.empty() ? std::string("event") : n.s);
		case NT::CallExternal: return "Call " + (n.s.empty() ? std::string("fn") : n.s) + " (Ref)";
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
		// Object variables show their class name as the type, not a bare "Object".
		const std::string typeStr = (v.type == PT::Ref && !v.className.empty())
			? std::filesystem::path(v.className).stem().string()
			: pinTypeName(v.type);
		const std::string label = v.name + "  (" + typeStr + ")";
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
void drawVariableDetails(HC::Graph& graph, ContentManager* content, bool& edited)
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

	// One searchable type dropdown: default value types + object (class) types.
	// Picking an object type sets v->className to the class; the label shows the
	// class name instead of "Object".
	const PT oldType = v->type;
	if (HcEditorUtil::drawTypePicker("Type", content, v->type, &v->className))
	{
		if (v->type != oldType)
			for (auto& n : graph.nodes)
				if ((n.type == NT::GetVariable || n.type == NT::SetVariable) && n.s == v->name)
				{
					n.propType = v->type;
					const PinRanges r = pinRanges(n);
					const int valuePin = n.type == NT::GetVariable ? r.dataOut0 : r.dataIn0;
					removePinLinks(graph, n.id, valuePin);
				}
		edited = true;
	}

	int vaccess = v->access;
	if (ImGui::Combo("Access", &vaccess, "Public\0Private\0")) { v->access = vaccess; edited = true; }

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
void drawNodeDetails(HC::Graph& graph, const std::vector<std::string>& events,
                     bool allowCustomEvents, ContentManager* content, bool& edited)
{
	HC::Node* n = graph.findNode(g.selectedNode);
	if (!n) { g.selectedNode = 0; return; }

	ImGui::TextDisabled("%s", HC::nodeDisplayName(n->type));
	ImGui::Separator();

	switch (n->type)
	{
	case NT::Event:
	{
		if (events.empty() || allowCustomEvents)
		{
			// A HorizonCode class names its own events freely (another class binds
			// to them by name), and can also react to the lifecycle events —
			// "Construct" (fired on create) and "Destruct" (fired on destroy).
			ImGui::InputText("Event", &n->s);
			if (ImGui::IsItemDeactivatedAfterEdit()) edited = true;
			for (size_t k = 0; k < events.size(); ++k)
			{
				if (k) ImGui::SameLine();
				if (ImGui::SmallButton(events[k].c_str())) { n->s = events[k]; edited = true; }
			}
		}
		else if (ImGui::BeginCombo("Event", n->s.empty() ? "(none)" : n->s.c_str()))
		{
			for (const std::string& ev : events)
				if (ImGui::Selectable(ev.c_str(), n->s == ev))
				{
					n->s = ev; n->hasArg = (ev == "OnWindowFocusChanged");
					n->propType = n->hasArg ? PT::Bool : PT::Float; n->elem = 0;
					edited = true;
				}
			ImGui::EndCombo();
		}
		ImGui::TextDisabled("Fires when this class raises this event.");
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
		HcEditorUtil::drawFunctionInterface(graph, *n, edited);
		break;
	}
	case NT::FunctionCall:
	{
		if (ImGui::BeginCombo("Function", n->s.empty() ? "(none)" : n->s.c_str()))
		{
			for (const auto& e : graph.nodes)
				if (e.type == NT::FunctionEntry && !e.s.empty())
					if (ImGui::Selectable(e.s.c_str(), n->s == e.s))
					{ n->s = e.s; HC::syncFunctionSignatures(graph); edited = true; }
			ImGui::EndCombo();
		}
		break;
	}
	case NT::FunctionReturn:
		if (HcEditorUtil::drawReturnFunctionPicker(graph, *n)) edited = true;
		break;
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
	case NT::BindEvent:
	case NT::EmitEvent:
		ImGui::InputText("Event", &n->s);
		if (ImGui::IsItemDeactivatedAfterEdit()) edited = true;
		ImGui::TextDisabled(n->type == NT::BindEvent
			? "When Target fires this event, this\nscript's Event of the same name runs."
			: "Broadcast to everyone bound to this\nscript's event of this name.");
		break;
	case NT::CallExternal:
		ImGui::InputText("Function", &n->s);
		if (ImGui::IsItemDeactivatedAfterEdit()) edited = true;
		ImGui::TextDisabled("Calls a public function on the\nTarget instance (a reference).");
		break;
	case NT::CreateWidget:
	{
		if (ImGui::BeginCombo("Widget", n->s.empty() ? "(none)" : n->s.c_str()))
		{
			for (const auto& a : HcEditorUtil::listAssets(content, HE::AssetType::Widget))
				if (ImGui::Selectable((a.label + "##" + a.path).c_str(), n->s == a.path))
					{ n->s = a.path; edited = true; }
			ImGui::EndCombo();
		}
		ImGui::TextDisabled("Which UI Widget asset to instantiate.\nOutputs the new widget's id.");
		break;
	}
	case NT::CreateObject:
	{
		if (ImGui::BeginCombo("Class", n->s.empty() ? "(none)" : n->s.c_str()))
		{
			for (const auto& c : HcEditorUtil::listHorizonCodeClasses(content))
				if (ImGui::Selectable((c.label + "##" + c.path).c_str(), n->s == c.path))
					{ n->s = c.path; edited = true; }
			ImGui::EndCombo();
		}
		ImGui::TextDisabled("Instantiates a HorizonCode class as a\nlive object. Outputs a reference to it.");
		break;
	}
	case NT::GetExternal:
	case NT::SetExternal:
	{
		ImGui::InputText("Variable", &n->s);
		if (ImGui::IsItemDeactivatedAfterEdit()) edited = true;
		int t = (int)n->propType;
		if (ImGui::Combo("Type", &t, "Exec\0Float\0Bool\0Int\0String\0Vec2\0Color\0Object\0"))
		{
			const PT nt = (PT)t;
			if (nt != PT::Exec && nt != n->propType)
			{
				n->propType = nt;
				const PinRanges r = pinRanges(*n);
				const int valuePin = n->type == NT::GetExternal ? r.dataOut0 : (r.dataIn0 + 1);
				removePinLinks(graph, n->id, valuePin);
				edited = true;
			}
		}
		ImGui::TextDisabled("Reads/writes a public variable on the\nTarget object.");
		break;
	}
	default:
		ImGui::TextDisabled("No parameters.");
		break;
	}
}

// ── Canvas ────────────────────────────────────────────────────────────────────

// Type of a node pin (exec pins report Exec). Used to filter the drag-off menu.
PT pinTypeOf(const HC::Node& n, int pin)
{
	const HC::NodeSig sig = HC::signatureOf(n);
	const PinRanges r = pinRanges(n);
	if (pin >= r.dataIn0  && pin < r.dataOut0) return sig.dataIns [pin - r.dataIn0 ].type;
	if (pin >= r.dataOut0 && pin < r.end)      return sig.dataOuts[pin - r.dataOut0].type;
	return PT::Exec;
}

// Load a class/widget asset's graph (for enumerating its public members). A
// widget is a first-class object too, so its logic graph counts as a "class".
bool loadClassGraph(ContentManager* content, const std::string& path, HC::Graph& out)
{
	if (!content || path.empty()) return false;
	const HE::UUID id = content->loadAsset(path);
	if (const HorizonCodeClassAsset* a = content->getHorizonCodeClass(id); a && !a->graphJson.empty())
		return HC::fromJson(a->graphJson, out);
	if (const UIWidgetAsset* w = content->getWidget(id); w && !w->graphJson.empty())
		return HC::fromJson(w->graphJson, out);
	return false;
}

// The class graph the Ref output of `srcNode` points to (self / GameInstance /
// a typed Object variable / Create Object), or null when the class is unknown.
const HC::Graph* resolveClassGraph(const HC::Node& srcNode, const HC::Graph& selfGraph,
                                   const HC::Graph* giGraph, ContentManager* content,
                                   HC::Graph& scratch)
{
	switch (srcNode.type)
	{
		case NT::GetSelf:         return &selfGraph;
		case NT::GetGameInstance: return giGraph;
		case NT::CreateObject:
		case NT::CreateWidget:
			return loadClassGraph(content, srcNode.s, scratch) ? &scratch : nullptr;
		case NT::GetVariable:
		{
			const HC::Variable* v = selfGraph.findVariable(srcNode.s);
			if (v && v->type == PT::Ref && !v->className.empty())
				return loadClassGraph(content, v->className, scratch) ? &scratch : nullptr;
			return nullptr;
		}
		default: return nullptr;
	}
}

void drawCanvas(HC::Graph& graph, const std::vector<std::string>& events, bool allowCustomEvents,
                const ImVec2& avail, ContentManager* content, const HC::Graph* giGraph, bool& edited)
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
		return n ? HcEditorUtil::nodeHeaderColor(*n) : GraphEditor::categoryColor(""); };
	m.pins = [&graph](int id){ const HC::Node* n = graph.findNode(id);
		return n ? nodePins(*n) : std::vector<GraphEditor::Pin>{}; };
	m.links = [&graph]{ std::vector<std::array<int,4>> ls; ls.reserve(graph.links.size());
		for (const auto& l : graph.links) ls.push_back({ l.srcNode, l.srcPin, l.dstNode, l.dstPin }); return ls; };
	m.connect = [&graph](int oN, int oP, int iN, int iP){ return graph.connect(oN, oP, iN, iP); };
	m.clearPinLinks = [&graph](int node, int pin, bool){ removePinLinks(graph, node, pin); };
	m.removeNode = [&graph](int id){ graph.removeNode(id); };
	// Right-click a node → context menu.
	m.drawNodeContextMenu = [&graph, &edited](int nodeId)
	{
		if (ImGui::MenuItem("Delete Node"))
		{
			graph.removeNode(nodeId);
			if (g.selectedNode == nodeId) g.selectedNode = 0;
			edited = true;
		}
	};

	// Searchable add-node palette: world events + generic node categories +
	// per-variable Get/Set + per-function Call. Property/Widget nodes and the
	// element machinery are intentionally absent.
	m.drawAddMenu = [&graph, &events, allowCustomEvents]() -> int {
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

		// Events. The catalog holds the fixed world events (level/GI) or the
		// lifecycle events a class exposes (Construct/Destruct); each adds a
		// pre-named Event node. A class (allowCustomEvents) can also add a blank
		// custom Event to name its own dispatcher events.
		bool eh = false;
		for (const std::string& ev : events)
		{
			if (!matches(ev, "Events")) continue;
			if (!eh) { ImGui::TextDisabled("Events"); eh = true; }
			if (ImGui::Selectable(ev.c_str()))
			{
				const int id = addNode(graph, NT::Event, g.ge.addMenuGraphPos);
				HC::Node* nn = graph.findNode(id);
				nn->s = ev; nn->hasArg = (ev == "OnWindowFocusChanged");
				nn->propType = nn->hasArg ? PT::Bool : PT::Float; nn->elem = 0;
				created = id; ImGui::CloseCurrentPopup();
			}
		}
		if ((events.empty() || allowCustomEvents) && matches("Custom Event", "Events"))
		{
			if (!eh) { ImGui::TextDisabled("Events"); eh = true; }
			if (ImGui::Selectable("Custom Event"))
			{ created = addNode(graph, NT::Event, g.ge.addMenuGraphPos); ImGui::CloseCurrentPopup(); }
		}
		if (eh) ImGui::Spacing();

		// Generic node categories (self-widget/property nodes excluded; the id-
		// based widget nodes live under "UI").
		static const char* kCats[] = { "Flow", "Events", "Reference", "UI",
		                               "Literals", "Math", "Logic", "String", "Debug" };
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
				HC::syncFunctionSignatures(graph); // mirror the function's pins onto the call
				created = id; ImGui::CloseCurrentPopup();
			}
		}
		// A Return node (picks its owning function in the details panel).
		if (matches("Return", "Functions"))
		{
			if (!fh) { ImGui::TextDisabled("Functions"); fh = true; }
			if (ImGui::Selectable("Return"))
			{ created = addNode(graph, NT::FunctionReturn, g.ge.addMenuGraphPos); ImGui::CloseCurrentPopup(); }
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

	// Drag a wire off a pin → context-aware menu. For a Ref (object) output whose
	// class is known, surface that class's public functions + variables; each
	// entry creates the matching node and connects the object to its Target.
	m.drawPinDragMenu = [&graph, content, giGraph](int srcNode, int srcPin, bool srcInput, ImVec2 pos) -> int {
		HC::Node* sn = graph.findNode(srcNode);
		if (!sn) return 0;
		int created = 0;
		ImGui::BeginChild("##pindrag", ImVec2(240.0f, 300.0f));

		if (!srcInput && pinTypeOf(*sn, srcPin) == PT::Ref)
		{
			auto wire = [&](int newId){
				HC::Node* nn = graph.findNode(newId);
				if (nn) graph.connect(srcNode, srcPin, newId, pinRanges(*nn).dataIn0); // → Target
			};
			HC::Graph scratch;
			const HC::Graph* cls = resolveClassGraph(*sn, graph, giGraph, content, scratch);
			if (cls)
			{
				bool fh = false;
				for (const auto& fn : cls->nodes)
					if (fn.type == NT::FunctionEntry && fn.access == 0 && !fn.s.empty())
					{
						if (!fh) { ImGui::TextDisabled("Functions"); fh = true; }
						if (ImGui::Selectable(("Call " + fn.s).c_str()))
						{ const int id = addNode(graph, NT::CallExternal, pos); graph.findNode(id)->s = fn.s; wire(id); created = id; ImGui::CloseCurrentPopup(); }
					}
				bool vh = false;
				for (const auto& var : cls->variables)
					if (var.access == 0)
					{
						if (!vh) { ImGui::TextDisabled("Variables"); vh = true; }
						if (ImGui::Selectable(("Get " + var.name).c_str()))
						{ const int id = addNode(graph, NT::GetExternal, pos); HC::Node* nn = graph.findNode(id); nn->s = var.name; nn->propType = var.type; wire(id); created = id; ImGui::CloseCurrentPopup(); }
						if (ImGui::Selectable(("Set " + var.name).c_str()))
						{ const int id = addNode(graph, NT::SetExternal, pos); HC::Node* nn = graph.findNode(id); nn->s = var.name; nn->propType = var.type; wire(id); created = id; ImGui::CloseCurrentPopup(); }
					}
				if (fh || vh) ImGui::Separator();
			}
			else ImGui::TextDisabled("(untyped object)");

			ImGui::TextDisabled("Reference");
			if (ImGui::Selectable("Call Function (Ref)")) { const int id = addNode(graph, NT::CallExternal, pos); wire(id); created = id; ImGui::CloseCurrentPopup(); }
			if (ImGui::Selectable("Bind Event"))          { const int id = addNode(graph, NT::BindEvent, pos);    wire(id); created = id; ImGui::CloseCurrentPopup(); }
			if (ImGui::Selectable("Get (Ref)"))           { const int id = addNode(graph, NT::GetExternal, pos);  wire(id); created = id; ImGui::CloseCurrentPopup(); }
			if (ImGui::Selectable("Set (Ref)"))           { const int id = addNode(graph, NT::SetExternal, pos);  wire(id); created = id; ImGui::CloseCurrentPopup(); }
			if (ImGui::Selectable("Destroy Object"))      { const int id = addNode(graph, NT::DestroyObject, pos); wire(id); created = id; ImGui::CloseCurrentPopup(); }
		}
		else ImGui::TextDisabled("Release on a pin to connect,\nor here to cancel.");

		ImGui::EndChild();
		return created;
	};

	if (GraphEditor::draw("##ls_canvas", m, g.ge, avail)) edited = true;
	g.selectedNode = g.ge.selected;
}

// Shared window body: left sidebar (variables + functions + details) + canvas,
// over one HorizonCode graph with the given event catalog. Used for both the
// Level Script and the Game Instance windows (they differ only in the graph,
// the events, and how a change is committed).
void drawGraphBody(HC::Graph& graph, const std::vector<std::string>& events,
                   bool allowCustomEvents, const char* title, const char* subtitle,
                   ContentManager* content, const HC::Graph* giGraph, bool& edited)
{
	ImGui::BeginChild("##ls_side", ImVec2(220.0f, 0.0f), true);
	ImGui::TextUnformatted(title);
	ImGui::TextDisabled("%s", subtitle);
	ImGui::Spacing();
	drawVariables(graph, edited);
	ImGui::Spacing();
	drawFunctions(graph, edited);
	ImGui::Spacing();
	ImGui::Separator();
	if (g.selectedNode != 0)          drawNodeDetails(graph, events, allowCustomEvents, content, edited);
	else if (!g.selectedVar.empty())  drawVariableDetails(graph, content, edited);
	else ImGui::TextDisabled("Select a node or variable.");
	ImGui::EndChild();

	ImGui::SameLine();

	ImGui::BeginChild("##ls_canvas_host", ImVec2(0.0f, 0.0f), true);
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	drawCanvas(graph, events, allowCustomEvents, avail, content, giGraph, edited);

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
}

} // namespace

// Wrap the shared body in a borderless window filling the tab rect (same
// pattern as the other tab editors).
namespace
{
void beginTabWindow(const char* id, const ImVec2& pos, const ImVec2& size)
{
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::Begin(id, nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
}
}

void LevelScriptPanel::render(AppContext& ctx, const ImVec2& pos, const ImVec2& size)
{
	beginTabWindow("##levelscript_tab", pos, size);
	if (!ctx.world)
	{
		ImGui::TextDisabled("Open a scene to edit its level script.");
		ImGui::End();
		return;
	}
	static const std::vector<std::string> kEvents = { "OnLevelLoaded", "OnLevelUnloaded" };
	bool edited = false;
	drawGraphBody(ctx.world->levelScript(), kEvents, /*allowCustomEvents=*/false, "Level Script",
	              "Reacts to world events.", ctx.contentManager, ctx.gameInstanceGraph, edited);
	// snapshotNow() bumps the undo revision so the level script saves with the
	// scene; self-contained so it doesn't disturb the entity undo.
	if (edited && ctx.undoSys) ctx.undoSys->snapshotNow();
	ImGui::End();
}

void GameInstancePanel::render(AppContext& ctx, const ImVec2& pos, const ImVec2& size)
{
	beginTabWindow("##gameinstance_tab", pos, size);
	if (!ctx.gameInstanceGraph)
	{
		ImGui::TextDisabled("Open a project to edit its Game Instance.");
		ImGui::End();
		return;
	}
	static const std::vector<std::string> kEvents = { "OnInit", "OnShutdown", "OnWindowFocusChanged" };
	bool edited = false;
	drawGraphBody(*ctx.gameInstanceGraph, kEvents, /*allowCustomEvents=*/false, "Game Instance",
	              "App-wide. Runs before anything loads.", ctx.contentManager, ctx.gameInstanceGraph, edited);
	// The GameInstance graph isn't part of a scene — re-register it in the app
	// runtime and persist it via the host callback.
	if (edited && ctx.commitGameInstance) ctx.commitGameInstance();
	ImGui::End();
}

// ── HorizonCode Class tab (a standalone .hasset graph) ────────────────────────
namespace
{
struct ClassState
{
	HorizonCode::Graph graph;
	bool        loaded = false;
	bool        dirty  = false;
	std::string name;
	HE::UUID    assetId;
};
std::map<std::string, ClassState> g_classStates;
}

bool HorizonCodeClassPanel::isClassAsset(const std::string& path)
{
	static std::map<std::string, bool> cache;
	if (auto it = cache.find(path); it != cache.end()) return it->second;
	HAsset::Reader r;
	const bool ok = r.open(path) &&
		r.assetType() == static_cast<uint16_t>(HE::AssetType::HorizonCodeClass);
	cache[path] = ok;
	return ok;
}

bool HorizonCodeClassPanel::isDirty(const std::string& path)
{
	auto it = g_classStates.find(path);
	return it != g_classStates.end() && it->second.dirty;
}

void HorizonCodeClassPanel::render(AppContext& ctx, const std::string& assetPath,
                                   const ImVec2& pos, const ImVec2& size)
{
	ClassState& st = g_classStates[assetPath];
	if (!st.loaded && ctx.contentManager)
	{
		std::error_code ec;
		const std::string rel = std::filesystem::relative(
			assetPath, ctx.contentManager->contentRoot(), ec).generic_string();
		st.assetId = ctx.contentManager->loadAsset(rel);
		if (const HorizonCodeClassAsset* a = ctx.contentManager->getHorizonCodeClass(st.assetId))
		{
			if (!a->graphJson.empty()) HorizonCode::fromJson(a->graphJson, st.graph);
			st.name = a->name;
		}
		st.loaded = true;
	}

	beginTabWindow(("##hcclass_" + assetPath).c_str(), pos, size);
	ImGui::AlignTextToFramePadding();
	ImGui::Text("%s", st.name.c_str());
	ImGui::SameLine();
	ImGui::TextDisabled("HorizonCode Class%s", st.dirty ? "  (unsaved)" : "");
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
	if (ImGui::Button("Save", ImVec2(56.0f, 0.0f)) && ctx.contentManager)
	{
		if (HorizonCodeClassAsset* a = ctx.contentManager->getHorizonCodeClassMutable(st.assetId))
		{
			a->graphJson = HorizonCode::toJson(st.graph);
			if (ctx.contentManager->saveAsset(*a)) st.dirty = false;
		}
	}
	ImGui::Separator();

	// Classes expose the lifecycle events (Construct on create, Destruct on
	// destroy) as a catalog, and can also name their own custom dispatcher events.
	static const std::vector<std::string> kClassEvents = { "Construct", "Destruct" };
	bool edited = false;
	drawGraphBody(st.graph, kClassEvents, /*allowCustomEvents=*/true, "HorizonCode Class",
	              "Reusable class; Construct/Destruct + its own events.", ctx.contentManager,
	              ctx.gameInstanceGraph, edited);
	if (edited) st.dirty = true;
	ImGui::End();
}
