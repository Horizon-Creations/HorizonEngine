#include "LevelScriptPanel.h"
#include "GameInstancePanel.h"
#include "HorizonCodeClassPanel.h"
#include "HcClassList.h"
#include "EditorApplication.h"   // AppContext
#include "EditorUndo.h"          // scene-undo snapshots (dirty tracking + undo/redo)
#include "GraphEditor.h"         // shared node-graph canvas
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/EngineApi.h>
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
		p.isArray = pd.isArray;   // array pins draw as a 2×2 grid
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
		case NT::CallExternal: return n.s.empty() ? std::string("Call (Ref)") : ("Call " + n.s);
		case NT::GetExternal:  return n.s.empty() ? std::string("Get (Ref)")  : ("Get " + n.s);
		case NT::SetExternal:  return n.s.empty() ? std::string("Set (Ref)")  : ("Set " + n.s);
		case NT::EngineCall:   return HcEditorUtil::engineCallTitle(n.s);
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

// True if some Event node (other than exceptId) already handles `name`. Events
// must be unique per graph — no two handlers of the same event, and lifecycle
// events (OnLevelLoaded / OnInit / Construct / …) can't be added twice.
bool eventNameUsed(const HC::Graph& g, const std::string& name, int exceptId = 0)
{
	if (name.empty()) return false;
	for (const auto& n : g.nodes)
		if (n.type == NT::Event && n.id != exceptId && n.s == name) return true;
	return false;
}

// ── Persistent panel state (the panel edits the current scene's graph) ────────
struct LSState
{
	GraphEditor::State ge;
	int         selectedNode = 0;
	bool        focusSelected = false;
	int         currentGraph = 0;   // visible sub-graph: 0 = event graph, else a FunctionEntry id
	std::string selectedVar;        // variable selected in the left panel
	std::string varNameEdit;        // scratch rename buffer (see the widget editor bug)
	std::string varNameEditFor;
	std::string evtNameEdit;        // scratch buffer for a custom Event name (uniqueness)
	int         evtNameEditFor = 0;
	std::string dropVar;            // variable dragged onto the canvas
	bool        openVarDrop = false;
};
LSState g;

int addNode(HC::Graph& graph, NT type, const ImVec2& pos)
{
	HC::Node n;
	n.type = type;
	n.x = pos.x; n.y = pos.y;
	n.subgraph = g.currentGraph;    // new nodes belong to the visible sub-graph
	if (type == NT::ConstColor) { n.f[0] = n.f[1] = n.f[2] = n.f[3] = 1.0f; }
	if (type == NT::FunctionEntry) n.s = uniqueFunctionName(graph);
	return graph.addNode(std::move(n));
}

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
		const std::string typeStr = ((v.type == PT::Ref && !v.className.empty())
			? std::filesystem::path(v.className).stem().string()
			: std::string(pinTypeName(v.type))) + (v.isArray ? "[]" : "");
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
	// The event graph (top-level Events) + one sub-graph per function; the list
	// switches which sub-graph the canvas shows (Blueprint-style).
	ImGui::SeparatorText("Graphs");
	if (ImGui::Selectable("Event Graph", g.currentGraph == 0))
	{ g.currentGraph = 0; g.selectedNode = 0; g.selectedVar.clear(); }

	ImGui::SeparatorText("Functions");
	if (ImGui::SmallButton("+ Add##fn"))
	{
		// A function is its own sub-graph: a start (FunctionEntry) + a Return node.
		const int fnId = addNode(graph, NT::FunctionEntry, ImVec2(40.0f, 40.0f));
		HC::Node* entry = graph.findNode(fnId);
		entry->subgraph = fnId;                  // the function owns its sub-graph
		const std::string fnName = entry->s;
		g.currentGraph = fnId;                    // so addNode scopes the return here
		const int retId = addNode(graph, NT::FunctionReturn, ImVec2(420.0f, 40.0f));
		graph.findNode(retId)->s = fnName;        // bound to this function (results mirror on sync)
		g.selectedNode = fnId;
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
		if (ImGui::Selectable(label.c_str(), g.currentGraph == n.id))
		{
			g.currentGraph = n.id;                // open the function's sub-graph
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
		{
			v->defaultItems.clear(); // array slots hold the OLD element type
			for (auto& n : graph.nodes)
				if ((n.type == NT::GetVariable || n.type == NT::SetVariable) && n.s == v->name)
				{
					n.propType = v->type;
					const PinRanges r = pinRanges(n);
					const int valuePin = n.type == NT::GetVariable ? r.dataOut0 : r.dataIn0;
					removePinLinks(graph, n.id, valuePin);
				}
		}
		edited = true;
	}

	int vaccess = v->access;
	if (ImGui::Combo("Access", &vaccess, "Public\0Private\0")) { v->access = vaccess; edited = true; }

	// Single value vs an array of the type. Toggling re-types the matching Get/Set
	// nodes' value pins and drops their now-mismatched links.
	bool arr = v->isArray;
	if (ImGui::Checkbox("Array", &arr))
	{
		v->isArray = arr;
		for (auto& n : graph.nodes)
			if ((n.type == NT::GetVariable || n.type == NT::SetVariable) && n.s == v->name)
			{
				n.isArray = arr;
				const PinRanges r = pinRanges(n);
				removePinLinks(graph, n.id, n.type == NT::GetVariable ? r.dataOut0 : r.dataIn0);
			}
		edited = true;
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hold a list of values instead of a single one.");

	if (!v->isArray)
	{
		ImGui::SeparatorText("Default");
		switch (v->type)
		{
			case PT::Float:  if (ImGui::DragFloat("##vdef", &v->f[0], 0.1f)) edited = true; break;
			case PT::Int:  { int iv = (int)v->f[0]; if (ImGui::DragInt("##vdef", &iv)) { v->f[0] = (float)iv; edited = true; } break; }
			case PT::Bool: { bool b = v->f[0] != 0.0f; if (ImGui::Checkbox("##vdef", &b)) { v->f[0] = b ? 1.0f : 0.0f; edited = true; } break; }
			case PT::String: ImGui::InputText("##vdef", &v->s); if (ImGui::IsItemDeactivatedAfterEdit()) edited = true; break;
			case PT::Vec2:   if (ImGui::DragFloat2("##vdef", v->f, 0.1f)) edited = true; break;
			case PT::Color:  if (ImGui::ColorEdit4("##vdef", v->f)) edited = true; break;
			case PT::Transform:
				if (ImGui::DragFloat3("Position##vdef", &v->tpos.x, 0.1f)) edited = true;
				if (ImGui::DragFloat3("Rotation##vdef", &v->trot.x, 0.5f)) edited = true;
				if (ImGui::DragFloat3("Scale##vdef",    &v->tscl.x, 0.05f)) edited = true;
				break;
			default: break;
		}
	}
	else if (HcEditorUtil::drawArrayDefaultEditor(*v)) // slot list seeds the array
		edited = true;

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
			// Edited via a scratch buffer + committed only when unique, so no two
			// Event nodes ever share a name.
			if (g.evtNameEditFor != n->id) { g.evtNameEdit = n->s; g.evtNameEditFor = n->id; }
			ImGui::InputText("Event", &g.evtNameEdit);
			if (ImGui::IsItemDeactivatedAfterEdit())
			{
				if (!g.evtNameEdit.empty() && eventNameUsed(graph, g.evtNameEdit, n->id))
					g.evtNameEdit = n->s; // reject duplicate → keep the old name
				else { n->s = g.evtNameEdit; edited = true; }
			}
			for (size_t k = 0; k < events.size(); ++k)
			{
				if (k) ImGui::SameLine();
				const bool used = eventNameUsed(graph, events[k], n->id);
				if (used) ImGui::BeginDisabled();
				if (ImGui::SmallButton(events[k].c_str()))
				{ n->s = events[k]; g.evtNameEdit = n->s; edited = true; }
				if (used) ImGui::EndDisabled();
			}
		}
		else if (ImGui::BeginCombo("Event", n->s.empty() ? "(none)" : n->s.c_str()))
		{
			for (const std::string& ev : events)
			{
				const bool used = eventNameUsed(graph, ev, n->id); // no duplicate handlers
				if (ImGui::Selectable(ev.c_str(), n->s == ev,
				        used ? ImGuiSelectableFlags_Disabled : 0) && !used)
				{
					n->s = ev; n->hasArg = (ev == "OnWindowFocusChanged");
					n->propType = n->hasArg ? PT::Bool : PT::Float; n->elem = 0;
					edited = true;
				}
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
					if ((c.type == NT::FunctionCall || c.type == NT::FunctionReturn) && c.s == oldName)
						c.s = n->s;
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
					const PT before = n->propType; const bool wasArr = n->isArray;
					n->s = v.name; n->propType = v.type; n->isArray = v.isArray;
					if (n->propType != before || n->isArray != wasArr)
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
	case NT::ArrayMake:
	case NT::ArrayLength:
	case NT::ArrayGet:
	case NT::ArrayAdd:
	case NT::ArraySet:
	case NT::ArrayInsert:
	case NT::ArrayRemove:
	case NT::ArrayContains:
	case NT::ArrayIndexOf:
	case NT::ForEach:
	{
		// Element type — object classes allowed too (the class path rides in s,
		// which array-op nodes don't use otherwise).
		const PT before = n->propType;
		if (HcEditorUtil::drawTypePicker("Element", content, n->propType, &n->s) && n->propType != before)
		{
			graph.links.erase(std::remove_if(graph.links.begin(), graph.links.end(),
				[&](const HC::Link& l){ return l.srcNode == n->id || l.dstNode == n->id; }), graph.links.end());
			edited = true;
		}
		ImGui::TextDisabled("Element type of the array.");
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
		case NT::SetVariable: // the set node passes the value through as its output
		{
			const HC::Variable* v = selfGraph.findVariable(srcNode.s);
			if (v && v->type == PT::Ref && !v->className.empty())
				return loadClassGraph(content, v->className, scratch) ? &scratch : nullptr;
			return nullptr;
		}
		case NT::ForEach: // Element of an object array (class adopted on connect)
			if (srcNode.propType == PT::Ref && !srcNode.s.empty())
				return loadClassGraph(content, srcNode.s, scratch) ? &scratch : nullptr;
			return nullptr;
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
	m.compactPureNodes = true; // getters/literals draw as compact chips
	m.nodeIds = [&graph]{ std::vector<int> ids; ids.reserve(graph.nodes.size());
		for (const auto& n : graph.nodes) if (n.subgraph == g.currentGraph) ids.push_back(n.id); return ids; };
	m.getPos = [&graph](int id, float& x, float& y){ if (const HC::Node* n = graph.findNode(id)) { x = n->x; y = n->y; } };
	m.setPos = [&graph](int id, float x, float y){ if (HC::Node* n = graph.findNode(id)) { n->x = x; n->y = y; } };
	m.title  = [&graph](int id){ const HC::Node* n = graph.findNode(id); return n ? nodeTitle(*n) : std::string(); };
	m.headerColor = [&graph](int id){ const HC::Node* n = graph.findNode(id);
		return n ? HcEditorUtil::nodeHeaderColor(*n) : GraphEditor::categoryColor(""); };
	m.pins = [&graph](int id){ const HC::Node* n = graph.findNode(id);
		return n ? nodePins(*n) : std::vector<GraphEditor::Pin>{}; };
	m.links = [&graph]{ std::vector<std::array<int,4>> ls; ls.reserve(graph.links.size());
		for (const auto& l : graph.links) { const HC::Node* s = graph.findNode(l.srcNode);
			if (s && s->subgraph == g.currentGraph) ls.push_back({ l.srcNode, l.srcPin, l.dstNode, l.dstPin }); }
		return ls; };
	m.connect = [&graph](int oN, int oP, int iN, int iP){
		// ForEach is generic until wired: adopt the source array's element type
		// (Array/Element pins retype + recolor) before the typed connect.
		HC::adoptForEachElementType(graph, oN, oP, iN, iP);
		return graph.connect(oN, oP, iN, iP); };
	m.clearPinLinks = [&graph](int node, int pin, bool){ removePinLinks(graph, node, pin); };
	m.removeNode = [&graph](int id){ graph.removeNode(id); };
	// Literal nodes edit their value inline on the node body.
	m.nodeBodyHeight = [&graph](int id){ const HC::Node* n = graph.findNode(id);
		return n ? HcEditorUtil::literalNodeBodyHeight(*n) : 0.0f; };
	m.drawNodeBody = [&graph, &edited](int id, ImVec2, ImVec2, float){
		HC::Node* n = graph.findNode(id); if (!n) return;
		bool committed = false;
		HcEditorUtil::drawLiteralNodeBody(*n, committed);
		if (committed) edited = true; };
	// Right-click a node → context menu. When the clicked node is part of a
	// multi-selection, Delete removes the whole selection.
	m.drawNodeContextMenu = [&graph, &edited](int nodeId)
	{
		const bool inSel = std::find(g.ge.selection.begin(), g.ge.selection.end(), nodeId)
			!= g.ge.selection.end();
		const bool multi = inSel && g.ge.selection.size() > 1;
		if (ImGui::MenuItem(multi ? "Duplicate Selection" : "Duplicate Node"))
		{
			const std::vector<int> src = multi ? g.ge.selection : std::vector<int>{ nodeId };
			const std::vector<int> fresh = HC::duplicateNodes(graph, src);
			if (!fresh.empty())
			{
				g.ge.selection = fresh;          // select the clones (ready to drag)
				g.selectedNode = fresh.front();
				edited = true;
			}
		}
		if (ImGui::MenuItem(multi ? "Delete Selection" : "Delete Node"))
		{
			const std::vector<int> doomed = multi ? g.ge.selection : std::vector<int>{ nodeId };
			for (int id : doomed) graph.removeNode(id);
			g.ge.selection.clear();
			g.selectedNode = 0;
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

		// Events live only in the event graph (sub-graph 0), never inside a
		// function's sub-graph. The catalog holds the fixed world events (level/GI)
		// or the lifecycle events a class exposes; a class (allowCustomEvents) can
		// also add a blank custom Event.
		bool eh = false;
		for (const std::string& ev : g.currentGraph == 0 ? events : std::vector<std::string>{})
		{
			if (!matches(ev, "Events")) continue;
			if (!eh) { ImGui::TextDisabled("Events"); eh = true; }
			// Each event handler is unique — a catalog event already present is
			// disabled so lifecycle events can't be added twice.
			const bool used = eventNameUsed(graph, ev);
			if (ImGui::Selectable(ev.c_str(), false,
			        used ? ImGuiSelectableFlags_Disabled : 0) && !used)
			{
				const int id = addNode(graph, NT::Event, g.ge.addMenuGraphPos);
				HC::Node* nn = graph.findNode(id);
				nn->s = ev; nn->hasArg = (ev == "OnWindowFocusChanged");
				nn->propType = nn->hasArg ? PT::Bool : PT::Float; nn->elem = 0;
				created = id; ImGui::CloseCurrentPopup();
			}
			if (used) { ImGui::SameLine(); ImGui::TextDisabled("(added)"); }
		}
		if (g.currentGraph == 0 && (events.empty() || allowCustomEvents) && matches("Custom Event", "Events"))
		{
			if (!eh) { ImGui::TextDisabled("Events"); eh = true; }
			if (ImGui::Selectable("Custom Event"))
			{ created = addNode(graph, NT::Event, g.ge.addMenuGraphPos); ImGui::CloseCurrentPopup(); }
		}
		if (eh) ImGui::Spacing();

		// Generic node categories (self-widget/property nodes excluded; the id-
		// based widget nodes live under "UI").
		static const char* kCats[] = { "Flow", "Events", "Reference", "UI",
		                               "Literals", "Math", "Logic", "String", "Array", "Debug" };
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
		// A Return node — only inside a function sub-graph, auto-bound to that
		// function so it gets pins for the declared outputs.
		if (g.currentGraph != 0 && matches("Return", "Functions"))
		{
			if (!fh) { ImGui::TextDisabled("Functions"); fh = true; }
			if (ImGui::Selectable("Return"))
			{
				const int id = addNode(graph, NT::FunctionReturn, g.ge.addMenuGraphPos);
				if (const HC::Node* owner = graph.findNode(g.currentGraph))
				{ HC::Node* rn = graph.findNode(id); rn->s = owner->s; rn->results = owner->results; }
				created = id; ImGui::CloseCurrentPopup();
			}
		}
		if (fh) ImGui::Spacing();

		// Engine API calls — the HE::api registry surfaced as one generic
		// EngineCall node per function, grouped by subsystem, same search box.
		if (std::string picked = HcEditorUtil::drawEngineApiMenu(q); !picked.empty())
		{
			if (const HE::api::ApiFn* fn = HE::api::find(picked))
			{
				const int id = addNode(graph, NT::EngineCall, g.ge.addMenuGraphPos);
				HC::Node* nn = graph.findNode(id);
				nn->s = fn->id;
				nn->hasArg = fn->isExec;             // exec node vs pure data node
				nn->params.clear(); nn->results.clear();
				for (const auto& p : fn->params)  nn->params.push_back({ p.name, p.type, p.isArray });
				for (const auto& r : fn->results) nn->results.push_back({ r.name, r.type, r.isArray });
				created = id;
			}
			ImGui::CloseCurrentPopup();
		}
		ImGui::Spacing();

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
					nn->s = v.name; nn->propType = v.type; nn->isArray = v.isArray;
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

	// Drag a wire off ANY pin → a filtered menu of everything that can take it.
	// Ref outputs lead with the target class's public members; exec pins list
	// every exec-capable node; data pins list nodes with a matching input (or
	// output, when dragging backwards off an input). The pick is auto-wired.
	m.drawPinDragMenu = [&graph, content, giGraph](int srcNode, int srcPin, bool srcInput, ImVec2 pos) -> int {
		HC::Node* sn = graph.findNode(srcNode);
		if (!sn) return 0;
		int created = 0;

		// Classify the dragged pin (exec vs data; data type + array-ness).
		const HC::NodeSig sig = HC::signatureOf(*sn);
		const PinRanges rr = pinRanges(*sn);
		const bool isExecPin = srcPin < rr.dataIn0;
		PT dragType = PT::Float; bool dragArray = false;
		if (!isExecPin)
		{
			if (srcPin >= rr.dataOut0 && srcPin - rr.dataOut0 < (int)sig.dataOuts.size())
			{ const auto& pd = sig.dataOuts[srcPin - rr.dataOut0]; dragType = pd.type; dragArray = pd.isArray; }
			else if (srcPin - rr.dataIn0 < (int)sig.dataIns.size())
			{ const auto& pd = sig.dataIns[srcPin - rr.dataIn0];  dragType = pd.type; dragArray = pd.isArray; }
		}

		static std::string s_dragSearch;
		if (ImGui::IsWindowAppearing()) { s_dragSearch.clear(); ImGui::SetKeyboardFocusHere(); }
		ImGui::SetNextItemWidth(232.0f);
		ImGui::InputTextWithHint("##dragSearch", "Search…", &s_dragSearch);
		const std::string q = lower(s_dragSearch);
		auto matches = [&](const std::string& name){ return q.empty() || lower(name).find(q) != std::string::npos; };

		ImGui::BeginChild("##pindrag", ImVec2(240.0f, 320.0f));

		// Wire the new node to the dragged pin (direction depends on the drag side).
		// adoptForEachElementType first: a ForEach on either end takes the array's
		// element type (and class) before the typed connect.
		auto wireAt = [&](int newId, int pin){
			if (srcInput) { HC::adoptForEachElementType(graph, newId, pin, srcNode, srcPin);
			                graph.connect(newId, pin, srcNode, srcPin); }
			else          { HC::adoptForEachElementType(graph, srcNode, srcPin, newId, pin);
			                graph.connect(srcNode, srcPin, newId, pin); } };

		// ── Ref output: the target class's public members lead ────────────────
		if (!isExecPin && !srcInput && dragType == PT::Ref && !dragArray)
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
					if (fn.type == NT::FunctionEntry && fn.access == 0 && !fn.s.empty() &&
					    matches("Call " + fn.s))
					{
						if (!fh) { ImGui::TextDisabled("Functions"); fh = true; }
						if (ImGui::Selectable(("Call " + fn.s).c_str()))
						{
							const int id = addNode(graph, NT::CallExternal, pos);
							HC::Node* nn = graph.findNode(id);
							nn->s = fn.s; nn->params = fn.params; nn->results = fn.results; // typed signature
							wire(id); created = id; ImGui::CloseCurrentPopup();
						}
					}
				bool vh = false;
				for (const auto& var : cls->variables)
					if (var.access == 0)
					{
						if (!vh && (matches("Get " + var.name) || matches("Set " + var.name)))
						{ ImGui::TextDisabled("Variables"); vh = true; }
						if (matches("Get " + var.name) && ImGui::Selectable(("Get " + var.name).c_str()))
						{ const int id = addNode(graph, NT::GetExternal, pos); HC::Node* nn = graph.findNode(id); nn->s = var.name; nn->propType = var.type; wire(id); created = id; ImGui::CloseCurrentPopup(); }
						if (matches("Set " + var.name) && ImGui::Selectable(("Set " + var.name).c_str()))
						{ const int id = addNode(graph, NT::SetExternal, pos); HC::Node* nn = graph.findNode(id); nn->s = var.name; nn->propType = var.type; wire(id); created = id; ImGui::CloseCurrentPopup(); }
					}
				if (fh || vh) ImGui::Separator();
			}
			else ImGui::TextDisabled("(untyped object)");

			ImGui::TextDisabled("Reference");
			auto refItem = [&](const char* lbl, NT t){
				if (matches(lbl) && ImGui::Selectable(lbl))
				{ const int id = addNode(graph, t, pos); wire(id); created = id; ImGui::CloseCurrentPopup(); } };
			refItem("Call Function (Ref)", NT::CallExternal);
			refItem("Bind Event",          NT::BindEvent);
			refItem("Get (Ref)",           NT::GetExternal);
			refItem("Set (Ref)",           NT::SetExternal);
			refItem("Destroy Object",      NT::DestroyObject);
			ImGui::Separator();
		}

		// ── Generic nodes with a compatible pin ────────────────────────────────
		{
			bool gh = false;
			for (NT t : HC::nodeRegistry())
			{
				if (t == NT::Event || t == NT::FunctionEntry || t == NT::FunctionCall ||
				    t == NT::FunctionReturn || t == NT::GetVariable || t == NT::SetVariable ||
				    t == NT::GetProperty || t == NT::SetProperty || t == NT::EngineCall ||
				    t == NT::CallExternal || t == NT::GetExternal || t == NT::SetExternal ||
				    t == NT::BindEvent || t == NT::ShowWidget || t == NT::HideWidget) continue;
				const int pin = HcEditorUtil::dragMatchPin(t, dragType, dragArray, srcInput, isExecPin);
				if (pin < 0 || !matches(HC::nodeDisplayName(t))) continue;
				if (!gh) { ImGui::TextDisabled("Nodes"); gh = true; }
				if (ImGui::Selectable(HC::nodeDisplayName(t)))
				{
					const int id = addNode(graph, t, pos);
					HC::Node* nn = graph.findNode(id);
					if (!isExecPin) nn->propType = dragType; // keep the matched signature
					wireAt(id, pin); created = id; ImGui::CloseCurrentPopup();
				}
			}
			if (gh) ImGui::Spacing();
		}

		// ── Engine API calls with a compatible pin ─────────────────────────────
		{
			bool eh = false;
			for (const HE::api::ApiFn& fn : HE::api::registry())
			{
				const int pin = HcEditorUtil::dragMatchApiPin(fn, dragType, dragArray, srcInput, isExecPin);
				const char* shown = fn.displayName ? fn.displayName : fn.id;
				if (pin < 0 || !matches(shown)) continue;
				if (!eh) { ImGui::TextDisabled("Engine"); eh = true; }
				if (ImGui::Selectable((std::string(shown) + "##" + fn.id).c_str()))
				{
					const int id = addNode(graph, NT::EngineCall, pos);
					HC::Node* nn = graph.findNode(id);
					nn->s = fn.id; nn->hasArg = fn.isExec;
					nn->params.clear(); nn->results.clear();
					for (const auto& p : fn.params)  nn->params.push_back({ p.name, p.type, p.isArray });
					for (const auto& r : fn.results) nn->results.push_back({ r.name, r.type, r.isArray });
					wireAt(id, pin); created = id; ImGui::CloseCurrentPopup();
				}
			}
			if (eh) ImGui::Spacing();
		}

		// ── This graph's variables (Set on exec/matching value; Get feeds inputs) ──
		{
			bool vh = false;
			for (const auto& v : graph.variables)
			{
				const bool setOk = (isExecPin && !srcInput) ||
					(!isExecPin && !srcInput && v.type == dragType && v.isArray == dragArray);
				const bool getOk = !isExecPin && srcInput && v.type == dragType && v.isArray == dragArray;
				auto add = [&](bool get){
					const int id = addNode(graph, get ? NT::GetVariable : NT::SetVariable, pos);
					HC::Node* nn = graph.findNode(id);
					nn->s = v.name; nn->propType = v.type; nn->isArray = v.isArray;
					const PinRanges r = pinRanges(*nn);
					wireAt(id, get ? r.dataOut0 : (isExecPin ? r.execIn0 : r.dataIn0));
					created = id; ImGui::CloseCurrentPopup(); };
				if (setOk && matches("Set " + v.name))
				{
					if (!vh) { ImGui::TextDisabled("Variables"); vh = true; }
					if (ImGui::Selectable(("Set " + v.name).c_str())) add(false);
				}
				if (getOk && matches("Get " + v.name))
				{
					if (!vh) { ImGui::TextDisabled("Variables"); vh = true; }
					if (ImGui::Selectable(("Get " + v.name).c_str())) add(true);
				}
			}
			if (vh) ImGui::Spacing();
		}

		// ── Declared functions (exec drags call them) ──────────────────────────
		if (isExecPin)
		{
			bool fh = false;
			for (const auto& e : graph.nodes)
			{
				if (e.type != NT::FunctionEntry || e.s.empty() || !matches("Call " + e.s)) continue;
				if (!fh) { ImGui::TextDisabled("Functions"); fh = true; }
				if (ImGui::Selectable(("Call " + e.s).c_str()))
				{
					const int id = addNode(graph, NT::FunctionCall, pos);
					graph.findNode(id)->s = e.s;
					HC::syncFunctionSignatures(graph);
					HC::Node* nn = graph.findNode(id);
					const PinRanges r = pinRanges(*nn);
					wireAt(id, srcInput ? r.execOut0 : r.execIn0);
					created = id; ImGui::CloseCurrentPopup();
				}
			}
		}

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
	// The shared panel state is reused across the Level/GI/Class tabs, so a
	// sub-graph id from another graph (or a deleted function) must reset to the
	// event graph.
	if (g.currentGraph != 0)
	{
		const HC::Node* e = graph.findNode(g.currentGraph);
		if (!e || e->type != NT::FunctionEntry) g.currentGraph = 0;
	}

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
	// Header: which sub-graph is shown.
	if (g.currentGraph == 0) ImGui::TextDisabled("Event Graph");
	else { const HC::Node* e = graph.findNode(g.currentGraph);
		ImGui::TextDisabled("Function: %s", e && !e->s.empty() ? e->s.c_str() : "(unnamed)"); }
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

void HorizonCodeClassPanel::forget(const std::string& path) { g_classStates.erase(path); }

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
