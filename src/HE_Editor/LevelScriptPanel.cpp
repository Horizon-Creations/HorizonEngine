#include "LevelScriptPanel.h"
#include <cstdint>
#include "GameInstancePanel.h"
#include "HorizonCodeClassPanel.h"
#include "HcEditorUtil.h"
#include "EditorApplication.h"    // AppContext
#include "EditorAssetTypeCache.h" // shared, invalidatable path → AssetType sniff
#include "EditorPanelState.h"     // shared per-tab state map
#include "EditorUndo.h"           // scene-undo snapshots (dirty tracking + undo/redo)
#include <Diagnostics/Logger.h>
#include "GraphEditor.h"         // shared node-graph canvas
#include "HcGraphHost.h"         // shared HorizonCode canvas host (pins, menus, clipboard)
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/HcCodegen.h>   // in-editor compile check (Compile button)
#include <HorizonCode/HorizonCode.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/HAsset.h>
#include <Application/InputAssets.h>  // shared Input.<Action>.* event naming
#include <Types/Enums.h>
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
// shares the GraphEditor canvas with the Material + UI Widget editors and the
// HorizonCode half of that canvas (pin layout, add menu, drag-off menu, node
// clipboard) with the widget editor through HcGraphHost. What stays here is
// what a level script does differently: it has no widget/element target, so the
// element machinery is gone — the Event node offers a fixed world-event catalog
// and there are no Get/Set Property nodes.

namespace
{
namespace HC = HorizonCode;
namespace HGH = HcGraphHost;
using PT = HC::PinType;
using NT = HC::NodeType;
using PinRanges = HGH::PinRanges;

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
	// In-editor compile check (the Compile button): the last result for the
	// graph identified by compileFor. An error anchors to a node — highlighted
	// on the canvas and focused when the check ran.
	bool        compileHas = false;
	bool        compileOk  = false;
	std::string compileMsg;
	int         compileNode = 0;
	std::string compileFor;         // panel title the result belongs to
};
LSState g;

// Run the SINGLE-class compile check the export would run: JSON round-trip
// (what a shipped asset contains), then HE::hccg::generate. Success = the class
// would compile on export; a fallback carries the reason + offending node.
void runCompileCheck(const HC::Graph& graph, const char* title)
{
	HE::hccg::ClassSource src;
	src.key   = title;
	src.label = title;
	HorizonCode::fromJson(HorizonCode::toJson(graph), src.graph);
	const HE::hccg::Result res = HE::hccg::generate({ src }, {});

	g.compileHas  = true;
	g.compileFor  = title;
	g.compileNode = 0;
	if (!res.fallbacks.empty())
	{
		g.compileOk   = false;
		g.compileMsg  = res.fallbacks[0].reason;
		g.compileNode = res.fallbacks[0].node;
		// Jump to the offending node: open its sub-graph and center on it.
		if (const HC::Node* n = graph.findNode(g.compileNode))
		{
			g.currentGraph   = n->subgraph;
			g.selectedNode   = n->id;
			g.ge.focusNode   = n->id;
			g.ge.selected    = n->id;
		}
	}
	else
	{
		size_t lines = 0;
		for (const auto& f : res.files)
			lines += (size_t)std::count(f.contents.begin(), f.contents.end(), '\n');
		g.compileOk  = true;
		g.compileMsg = "compiles clean — " + std::to_string(lines) + " lines of C++";
		if (!res.warnings.empty())
			g.compileMsg += " (" + std::to_string(res.warnings.size()) + " warning(s), see log)";
		for (const auto& w : res.warnings)
			Logger::Log(Logger::LogLevel::Warning, ("HorizonCode compile check: " + w).c_str());
	}
}

// Add a node into the visible sub-graph (the shared helper, bound to this
// panel's current sub-graph).
int addNode(HC::Graph& graph, NT type, const ImVec2& pos)
{
	return HGH::addNode(graph, type, pos, g.currentGraph);
}

const char* kVarPayload = "HE_LSGRAPH_VAR";

// Which node types this frontend offers. A level script has no self-widget and
// no elements, so Show/Hide Self and Get/Set Property never appear; the
// id-based widget nodes under "UI" are its only widget access.
const HGH::MenuOpts kMenus = {
	/*addCategories*/ { "Flow", "Events", "Reference", "UI",
	                    "Literals", "Math", "Logic", "String", "Array", "Debug" },
	/*addExcluded*/   { NT::Event, NT::FunctionEntry, NT::FunctionCall,
	                    NT::GetVariable, NT::SetVariable,
	                    NT::GetProperty, NT::SetProperty,
	                    NT::ShowSelf, NT::HideSelf },
	/*dragExcluded*/  { NT::Event, NT::FunctionEntry, NT::FunctionCall,
	                    NT::FunctionReturn, NT::GetVariable, NT::SetVariable,
	                    NT::GetProperty, NT::SetProperty, NT::EngineCall,
	                    NT::CallExternal, NT::GetExternal, NT::SetExternal,
	                    NT::BindEvent, NT::ShowSelf, NT::HideSelf },
};

// ── Left sidebar: variables + functions + details ─────────────────────────────
// Deliberately NOT shared with the widget editor's drawGraphVariables: only the
// type label is (HcGraphHost::variableTypeLabel). The two lists agree on what a
// variable IS but not on how it is presented — this one puts the type in the row
// label and drags a "HE_LSGRAPH_VAR" payload, the widget one puts the type in a
// trailing TextDisabled + tooltip, drags "HE_UIWGRAPH_VAR", and leads with a UI
// element browser that has no counterpart here.

void drawVariables(HC::Graph& graph, bool& edited)
{
	// One row: selectable + drag source. Shared by the instance list and the
	// per-function locals list below it.
	auto varRow = [&](const HC::Variable& v)
	{
		ImGui::PushID(v.name.c_str());
		const std::string label = v.name + "  (" + HGH::variableTypeLabel(v) + ")";
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
	};

	ImGui::SeparatorText("Variables");
	if (ImGui::SmallButton("+ Add##var"))
	{
		HC::Variable v;
		v.name = HGH::uniqueVarName(graph);
		graph.variables.push_back(v);
		g.selectedVar = v.name;
		g.selectedNode = 0;
		edited = true;
	}
	for (const auto& v : graph.variables)
		if (v.scope == 0) varRow(v);

	// Function-locals of the OPEN function sub-graph: fresh per invocation,
	// usable only inside that function (menus/drops elsewhere won't offer them).
	if (g.currentGraph != 0)
	{
		ImGui::SeparatorText("Local Variables");
		if (ImGui::SmallButton("+ Add##lvar"))
		{
			HC::Variable v;
			v.name = HGH::uniqueVarName(graph);
			v.scope = g.currentGraph;   // owned by the open function
			graph.variables.push_back(v);
			g.selectedVar = v.name;
			g.selectedNode = 0;
			edited = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Local to this function — reset to its default on every call.");
		for (const auto& v : graph.variables)
			if (v.scope == g.currentGraph) varRow(v);
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
//
// STILL DUPLICATED, knowingly: the widget editor has a near-identical copy in
// UIEditorPanel::drawGraphNodeDetails (its "no node selected but a variable is"
// branch). Unlike the node-detail rows, this one cannot go through
// HcGraphHost::Host as it stands — it needs three pieces of per-host scratch
// state the Host does not model (the name-edit buffer, which variable that
// buffer belongs to, and the host's selected-variable name), and inventing
// out-params for them would trade one duplication for a worse interface.
// Fixing it properly means giving Host a small "selection" block first.
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
					const PinRanges r = HGH::pinRanges(n);
					const int valuePin = n.type == NT::GetVariable ? r.dataOut0 : r.dataIn0;
					HGH::removePinLinks(graph, n.id, valuePin);
				}
		}
		edited = true;
	}

	// Locals have no access modifier — they are never visible outside their
	// function, let alone through a reference.
	if (v->scope != 0)
	{
		const HC::Node* fn = graph.findNode(v->scope);
		ImGui::TextDisabled("Local to: %s", fn && !fn->s.empty() ? fn->s.c_str() : "(function)");
	}
	else
	{
		int vaccess = v->access;
		if (ImGui::Combo("Access", &vaccess, "Public\0Private\0")) { v->access = vaccess; edited = true; }
	}

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
				const PinRanges r = HGH::pinRanges(n);
				HGH::removePinLinks(graph, n.id, n.type == NT::GetVariable ? r.dataOut0 : r.dataIn0);
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
//
// The rows that read the same in every HorizonCode frontend (the Const* literals,
// the array element-type picker, Get/Set Variable, Function Return, Call/Get/Set
// External, Create Widget/Object, Engine Call) come from HcGraphHost. What is
// still spelled out below is what this frontend says DIFFERENTLY from the widget
// editor: Event (an event catalog / free-named custom event, no UI element to
// bind), the Lua/Python wording on FunctionEntry, the unnamed-function filter on
// FunctionCall, and the "script" wording on Bind/Emit Event.
// HcGraphHost::drawCommonNodeDetails lists the same split from the other side.
void drawNodeDetails(HC::Graph& graph, const std::vector<std::string>& events,
                     bool allowCustomEvents, ContentManager* content, bool& edited)
{
	HC::Node* n = graph.findNode(g.selectedNode);
	if (!n) { g.selectedNode = 0; return; }

	ImGui::TextDisabled("%s", HC::nodeDisplayName(n->type));
	ImGui::Separator();

	// This panel's `edited` means "the graph changed": the caller reacts once per
	// frame (undo snapshot / re-save / dirty flag) and always did so from the first
	// dragged frame — so a still-dragging edit deliberately counts like a finished
	// one here. (The CANVAS host in drawCanvas takes committed edits only.)
	HGH::Host common;
	common.graph   = &graph;
	common.content = content;
	common.onEdit  = [&edited](bool){ edited = true; };
	if (HGH::drawCommonNodeDetails(common, *n)) return;

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
	// Kept here: this one skips unnamed functions (an empty Selectable label is
	// unclickable), the widget editor's copy lists them. Same widget otherwise.
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
	// Kept here (not in HcGraphHost) because the widget editor words the hint
	// "widget's Event" where this one says "script's Event".
	case NT::BindEvent:
	case NT::EmitEvent:
		ImGui::InputText("Event", &n->s);
		if (ImGui::IsItemDeactivatedAfterEdit()) edited = true;
		ImGui::TextDisabled(n->type == NT::BindEvent
			? "When Target fires this event, this\nscript's Event of the same name runs."
			: "Broadcast to everyone bound to this\nscript's event of this name.");
		break;
	default:
		ImGui::TextDisabled("No parameters.");
		break;
	}
}

// ── Canvas ────────────────────────────────────────────────────────────────────
// Pin layout, the palette, the drag-off menu and the node clipboard all live in
// HcGraphHost (shared with the UI Widget graph). What is host-specific here is
// the node title, the event catalog at the top of the add menu, the variable
// drag payload, and that an edit bumps the scene-undo revision.

void drawCanvas(HC::Graph& graph, const std::vector<std::string>& events, bool allowCustomEvents,
                const ImVec2& avail, ContentManager* content, const HC::Graph* giGraph, bool& edited)
{
	g.ge.selected = g.selectedNode;
	if (g.focusSelected) { g.ge.focusNode = g.selectedNode; g.focusSelected = false; }

	HGH::Host host;
	host.graph        = &graph;
	host.ge           = &g.ge;
	host.selectedNode = &g.selectedNode;
	host.currentGraph = g.currentGraph;
	host.content      = content;
	host.giGraph      = giGraph;
	// The last compile check's error node gets a red halo.
	host.errorNode    = (g.compileHas && !g.compileOk) ? g.compileNode : 0;
	host.title        = [](const HC::Node& n){ return nodeTitle(n); };
	// Every edit bumps the scene-undo revision (the caller snapshots), so a
	// value still being dragged needs no separate dirty flag.
	host.onEdit       = [&edited](bool committed){ if (committed) edited = true; };
	host.menus        = &kMenus;

	GraphEditor::Model m = HGH::buildModel(host);

	// Searchable add-node palette: world events + the shared tail (generic node
	// categories + per-function Call + engine API + per-variable Get/Set).
	m.drawAddMenu = [&graph, &events, allowCustomEvents, &host]() -> int {
		int created = 0;
		const std::string q = HGH::beginAddMenu();
		auto matches = [&](const std::string& name, const std::string& cat)
		{ return q.empty() || HGH::lower(name).find(q) != std::string::npos
		      || HGH::lower(cat).find(q) != std::string::npos; };

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
				nn->s = ev;
				// Events with a data-out: window focus (Bool), Tick (delta seconds),
				// and Input.<Action>.Axis (the axis value). Names come from the
				// shared HE::inputEvent* helpers so the runtime pump matches.
				const bool axisEvent = ev.rfind("Input.", 0) == 0 && ev.size() >= 5 &&
					ev.compare(ev.size() - 5, 5, ".Axis") == 0;
				nn->hasArg = (ev == "OnWindowFocusChanged") || (ev == "Tick") || axisEvent;
				nn->propType = (ev == "OnWindowFocusChanged") ? PT::Bool : PT::Float;
				nn->elem = 0;
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

		if (const int c = HGH::drawAddMenuTail(host, q)) created = c;
		HGH::endAddMenu();
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

	const ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
	if (GraphEditor::draw("##ls_canvas", m, g.ge, avail)) edited = true;
	g.selectedNode = g.ge.selected;

	HGH::handleClipboardKeys(host, canvasOrigin, avail);
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
	// A stale compile result from another tab must not anchor to this graph.
	if (g.compileHas && g.compileFor != title) g.compileHas = false;
	// Header: which sub-graph is shown + the compile check.
	ImGui::AlignTextToFramePadding();
	if (g.currentGraph == 0) ImGui::TextDisabled("Event Graph");
	else { const HC::Node* e = graph.findNode(g.currentGraph);
		ImGui::TextDisabled("Function: %s", e && !e->s.empty() ? e->s.c_str() : "(unnamed)"); }
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - 64.0f);
	if (ImGui::SmallButton("Compile"))
		runCompileCheck(graph, title);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Translate this class to C++ the way a packaged export would.\n"
		                  "Errors highlight the offending node; a clean result means the\n"
		                  "class ships compiled (everything else runs interpreted).");
	if (g.compileHas)
	{
		if (g.compileOk)
			ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.35f, 1.0f), "Compile: %s", g.compileMsg.c_str());
		else
		{
			ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f),
			                   "Compile error: %s — runs interpreted", g.compileMsg.c_str());
			if (g.compileNode != 0)
			{
				ImGui::SameLine();
				if (ImGui::SmallButton("Show node"))
					if (const HC::Node* n = graph.findNode(g.compileNode))
					{
						g.currentGraph = n->subgraph;
						g.selectedNode = n->id;
						g.ge.focusNode = n->id;
						g.ge.selected  = n->id;
					}
			}
		}
	}
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	drawCanvas(graph, events, allowCustomEvents, avail, content, giGraph, edited);

	// Variable drop → Get/Set popup.
	if (g.openVarDrop) { ImGui::OpenPopup("##ls_var_drop"); g.openVarDrop = false; }
	if (ImGui::BeginPopup("##ls_var_drop"))
	{
		const HC::Variable* v = graph.findVariable(g.dropVar);
		// A function-local can only be placed inside its owning function's graph.
		const bool scopeOk = v && (v->scope == 0 || v->scope == g.currentGraph);
		ImGui::TextDisabled("%s", g.dropVar.c_str());
		ImGui::Separator();
		auto make = [&](NT type)
		{
			const int id = addNode(graph, type, g.ge.addMenuGraphPos);
			HC::Node* nn = graph.findNode(id);
			nn->s = g.dropVar;
			if (v) { nn->propType = v->type; nn->isArray = v->isArray; }
			g.selectedNode = id;
			edited = true;
		};
		if (ImGui::MenuItem("Get", nullptr, false, scopeOk)) make(NT::GetVariable);
		if (ImGui::MenuItem("Set", nullptr, false, scopeOk)) make(NT::SetVariable);
		if (v && !scopeOk)
			ImGui::TextDisabled("Local to another function.");
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
	std::string baseClass;              // "" = plain Object; "PlayerController"/"PlayerCharacter"
	HE::UUID    assetId;
	std::vector<std::string> events;    // event catalog (lifecycle + player input events)
	double      eventsScanTime = -1.0;  // last catalog (re)build, ImGui time
};
AssetPanelState<ClassState> s_classStates;

// Input.<Action>.* event names for every InputAction asset in the project —
// the input-event catalog player classes offer. Walks the content dir (cheap
// header sniffs), so callers cache the result and refresh on a coarse timer.
std::vector<std::string> scanInputEvents(ContentManager* cm)
{
	std::vector<std::string> out;
	if (!cm) return out;
	for (const auto& ref : HcEditorUtil::listAssets(cm, HE::AssetType::InputAction))
	{
		bool axis = false;
		HAsset::Reader r;
		if (r.open(cm->resolveAbsolutePath(ref.path)))
			if (const auto* c = r.findChunk(HAsset::CHUNK_IACT))
				axis = HE::inputActionIsAxis(std::string(
					reinterpret_cast<const char*>(c->data.data()), c->data.size()));
		if (axis)
			out.push_back(HE::inputEventAxis(ref.label));
		else
		{
			out.push_back(HE::inputEventPressed(ref.label));
			out.push_back(HE::inputEventReleased(ref.label));
		}
	}
	return out;
}
}

bool HorizonCodeClassPanel::isClassAsset(const std::string& path)
{
	return EditorAssetTypeCache::is(path, HE::AssetType::HorizonCodeClass);
}

void HorizonCodeClassPanel::forget(const std::string& path) { s_classStates.forget(path); }

bool HorizonCodeClassPanel::isDirty(const std::string& path) { return s_classStates.dirty(path); }

void HorizonCodeClassPanel::appendDirtyPaths(std::vector<std::string>& out) { s_classStates.appendDirtyPaths(out); }
void HorizonCodeClassPanel::render(AppContext& ctx, const std::string& assetPath,
                                   const ImVec2& pos, const ImVec2& size)
{
	ClassState& st = s_classStates[assetPath];
	if (!st.loaded && ctx.contentManager)
	{
		const std::string rel = ctx.contentManager->toContentRelativePath(assetPath);
		st.assetId = ctx.contentManager->loadAsset(rel);
		if (const HorizonCodeClassAsset* a = ctx.contentManager->getHorizonCodeClass(st.assetId))
		{
			if (!a->graphJson.empty()) HorizonCode::fromJson(a->graphJson, st.graph);
			st.name      = a->name;
			st.baseClass = a->baseClass;
		}
		st.loaded = true;
	}

	const bool isPlayer = st.baseClass == "PlayerController" || st.baseClass == "PlayerCharacter";
	const char* kindLabel = st.baseClass == "PlayerController" ? "Player Controller"
	                      : st.baseClass == "PlayerCharacter"  ? "Player Character"
	                      : "HorizonCode Class";

	beginTabWindow(("##hcclass_" + assetPath).c_str(), pos, size);
	ImGui::AlignTextToFramePadding();
	ImGui::Text("%s", st.name.c_str());
	ImGui::SameLine();
	ImGui::TextDisabled("%s%s", kindLabel, st.dirty ? "  (unsaved)" : "");
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
	// Player classes additionally get the game lifecycle (BeginPlay/Tick) and one
	// event set per InputAction asset in the project. Actions can be created while
	// this tab is open, so the input catalog rescans on a coarse timer.
	const double now = ImGui::GetTime();
	if (st.eventsScanTime < 0.0 || (isPlayer && now - st.eventsScanTime > 3.0))
	{
		if (isPlayer)
		{
			st.events = { "Construct", "BeginPlay", "Tick", "Destruct" };
			for (auto& ev : scanInputEvents(ctx.contentManager))
				st.events.push_back(std::move(ev));
		}
		else
			st.events = { "Construct", "Destruct" };
		st.eventsScanTime = now;
	}
	bool edited = false;
	drawGraphBody(st.graph, st.events, /*allowCustomEvents=*/true, kindLabel,
	              isPlayer ? "Player class; lifecycle + input events."
	                       : "Reusable class; Construct/Destruct + its own events.",
	              ctx.contentManager, ctx.gameInstanceGraph, edited);
	if (edited) st.dirty = true;
	ImGui::End();
}
