#include "LevelScriptPanel.h"
#include <Types/TypeRegistry.h>
#include "EditorToolbar.h"   // shared toolbar strip
#include "EditorWidgets.h"    // primary/danger/cancel buttons
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
#include <HorizonScene/EntityHost.h>   // default component lists per base class
#include <HorizonScene/SceneSerializer.h>
#include "InspectorPanel.h"           // the REAL component editor, over a scratch world
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HcClassResolve.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/HAsset.h>
#include <Application/InputAssets.h>  // shared Input.<Action>.* event naming
#include <Types/Enums.h>
#include <map>
#include <memory>
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
		case NT::Cast:         return HcEditorUtil::castTitle(n.s);
		// The action, not the event names it answers to — those are the wire's
		// business. "(Axis)" because the pin layout differs and the reason for
		// it should be readable off the node.
		case NT::InputAction:  return n.s.empty() ? std::string(base)
		                                          : (n.s + (n.hasArg ? " (Axis)" : ""));
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

// One InputAction asset, as the add menu offers it.
struct InputActionRef
{
	std::string name;    // the action's logical name (its asset stem)
	bool        isAxis = false;
};

// ── Persistent panel state (the panel edits the current scene's graph) ────────
struct LSState
{
	GraphEditor::State ge;
	int         selectedNode = 0;
	bool        focusSelected = false;
	int         currentGraph = 0;   // visible sub-graph: 0 = event graph, else a FunctionEntry id
	std::string selectedVar;        // variable selected in the left panel
	std::string selectedEvent;      // declared event shown in the details pane
	// Which graph everything below belongs to (a class key, or the Level
	// Script / Game Instance title). One panel state serves all of those tabs,
	// and a switch has to wipe what only made sense in the previous one.
	std::string graphFor;
	std::string varNameEdit;        // scratch rename buffer (see the widget editor bug)
	std::string varNameEditFor;
	// The last rejected rename, so the snap-back can say why. `varNameErrorName`
	// is the name that was refused (empty = nothing to report), `varNameError`
	// the ancestor that already uses it ("" = this class's own list does).
	std::string varNameError;
	std::string varNameErrorName;
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
	double      compileAt  = 0.0;   // when the check ran (ImGui::GetTime)
};

// ── One context per graph ────────────────────────────────────────────────────
// None of the state above means anything in a graph other than the one it was
// made in: a sub-graph id, a selection and a scroll position are all in terms of
// node ids, and every graph hands those out starting at 1. A single state behind
// the Level Script, the Game Instance and every class tab therefore did not just
// look wrong after a switch — it named DIFFERENT nodes, and Delete, Cut,
// Duplicate and drag all took it at its word.
//
// So each graph keeps its own, and switching tabs finds it exactly as it was
// left. `g` stays THE state every reader below uses; drawGraphBody swaps the
// right one in. Doing it that way rather than handing a context to a hundred
// call sites is not only the smaller change: it leaves exactly one place where
// a graph's state can be selected, so there is no second path to get wrong.
//
// Keyed by the class asset's path, or — for the two virtual tabs that have no
// asset — the panel title. Not the tab title of a class, which is its BASE
// class's label and shared by every class deriving from it.
std::map<std::string, LSState> s_graphStates;
LSState g;

// Run the SINGLE-class compile check the export would run: JSON round-trip
// (what a shipped asset contains), then HE::hccg::generate. Success = the class
// would compile on export; a fallback carries the reason + offending node.
// `classKey`/`content` are set for a CLASS tab and empty for the level script
// and the GameInstance. With them the check compiles the whole ancestry, which
// is what an export does — a derived class alone cannot be translated at all
// (its C++ base would be missing), and even where it could, a call to an
// inherited function would look like a call to nothing.
void runCompileCheck(const HC::Graph& graph, const char* title,
                     const std::string& classKey, ContentManager* content)
{
	std::vector<HE::hccg::ClassSource> sources;
	const std::string selfKey = classKey.empty() ? std::string(title) : classKey;
	if (!classKey.empty() && content)
	{
		const HorizonCode::ResolvedClass rc = HorizonCode::resolveClassAsset(*content, classKey);
		// The ancestors as they are on disk; only the class itself comes from
		// the tab, unsaved edits included.
		for (auto a = rc.chain.rbegin(); a != rc.chain.rend(); ++a)
		{
			const HorizonCode::ResolvedClass arc = HorizonCode::resolveClassAsset(*content, *a);
			if (!arc.ok || arc.levels.empty()) continue;
			HE::hccg::ClassSource s;
			s.key = s.label = *a;
			s.graph     = arc.levels.back();
			s.baseClass = arc.engineBase;
			s.chain     = arc.chain;
			sources.push_back(std::move(s));
		}
		HE::hccg::ClassSource self;
		self.key = self.label = classKey;
		HorizonCode::fromJson(HorizonCode::toJson(graph), self.graph);
		self.baseClass = rc.engineBase;
		self.chain     = rc.chain;
		sources.push_back(std::move(self));
	}
	else
	{
		HE::hccg::ClassSource src;
		src.key   = title;
		src.label = title;
		HorizonCode::fromJson(HorizonCode::toJson(graph), src.graph);
		sources.push_back(std::move(src));
	}
	const HE::hccg::Result res = HE::hccg::generate(sources, {});

	g.compileHas  = true;
	g.compileFor  = title;
	g.compileAt   = ImGui::GetTime();
	g.compileNode = 0;
	// Only THIS class's verdict is this tab's business: an ancestor that does
	// not compile is reported in its own tab, and blaming this one for it would
	// send the author to the wrong graph.
	const HE::hccg::Result::Fallback* mine = nullptr;
	for (const auto& f : res.fallbacks)
		if (f.key == selfKey) { mine = &f; break; }
	if (mine)
	{
		g.compileOk   = false;
		g.compileMsg  = mine->reason;
		g.compileNode = mine->node;
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
		// The line count covers the whole ancestry when there is one — which is
		// honest: that is what an export builds to make THIS class native.
		g.compileMsg = "compiles clean — " + std::to_string(lines) + " lines of C++";
		if (!res.warnings.empty())
			g.compileMsg += " (" + std::to_string(res.warnings.size()) + " warning(s), see log)";
		for (const auto& w : res.warnings)
			HE_LOG_WARN(Editor, "%s", ("HorizonCode compile check: " + w).c_str());
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

// Which ancestor declared `name`, or "" — only for the hints and the refusal
// message; everything functional reads Graph::inherited.
std::string inheritedFrom(const std::vector<HC::InheritedVariable>& list,
                          const std::string& name)
{
	for (const auto& iv : list)
		if (iv.var.name == name) return iv.fromClass;
	return {};
}

// A name a new or renamed variable may not take: this graph's own, plus every
// one the chain declares. Inherited names are refused even when the ancestor
// keeps the variable PRIVATE — an instance has ONE variable store, so a
// same-named declaration here would not shadow the base's state, it would write
// over it.
bool varNameTaken(const HC::Graph& graph, const std::string& name)
{
	return graph.findVariableOrInherited(name) != nullptr;
}

void drawVariables(HC::Graph& graph, const std::vector<HC::InheritedVariable>& inheritedVars,
                   bool& edited)
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
	if (EditorWidgets::addButton("##var", "Add a variable"))
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

	// ── What the base classes bring ──────────────────────────────────────────
	// Listed apart from this class's own, because that is what they are: read and
	// written under the same name, but declared in another asset and edited
	// there. Public ones drag onto the canvas like any other variable; a private
	// one is shown greyed so the author can see why its NAME is refused without
	// being offered a variable they cannot reach.
	if (!graph.inherited.empty())
	{
		ImGui::SeparatorText("Inherited");
		for (const auto& v : graph.inherited)
		{
			const std::string from = inheritedFrom(inheritedVars, v.name);
			ImGui::PushID(v.name.c_str());
			if (v.access != 0)
			{
				ImGui::BeginDisabled();
				ImGui::Selectable((v.name + "  (private)").c_str(), false);
				ImGui::EndDisabled();
			}
			else
			{
				// Never selected: the details editor below writes into THIS
				// class's variable list, and an inherited declaration is not in
				// it. Dragging is the whole interaction — it makes a Get/Set node
				// that names the variable, which is all a derived class can do.
				ImGui::Selectable((v.name + "  (" + HGH::variableTypeLabel(v) + ")").c_str(), false);
				if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
				{
					char buf[64] = {};
					std::strncpy(buf, v.name.c_str(), sizeof(buf) - 1);
					ImGui::SetDragDropPayload(kVarPayload, buf, sizeof(buf));
					ImGui::TextUnformatted(v.name.c_str());
					ImGui::EndDragDropSource();
				}
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s%s%s",
					v.access != 0 ? "Private to " : "Declared in ",
					from.empty() ? "the base class" : HcEditorUtil::castTargetLabel(from).c_str(),
					v.access != 0 ? " — the name is taken, but the variable is not readable here."
					              : " — edit it there; here it reads and writes the same value.");
			ImGui::PopID();
		}
	}

	// Function-locals of the OPEN function sub-graph: fresh per invocation,
	// usable only inside that function (menus/drops elsewhere won't offer them).
	if (g.currentGraph != 0)
	{
		ImGui::SeparatorText("Local Variables");
		if (EditorWidgets::addButton("##lvar", "Add a local variable — reset to its default on every call"))
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
	{ g.currentGraph = 0; g.selectedNode = 0; g.selectedVar.clear(); g.selectedEvent.clear(); }

	// ── Events this class raises ─────────────────────────────────────────────
	// Declared once here, then PICKED in Emit/Bind/Event nodes instead of typed
	// three times. Rename rewrites every node that used the old name, so the two
	// halves of a binding cannot drift apart.
	ImGui::SeparatorText("Events");
	if (EditorWidgets::addButton("##evt", "Declare an event this class raises"))
	{
		std::string name = "NewEvent";
		for (int i = 2; graph.findEvent(name); ++i) name = "NewEvent" + std::to_string(i);
		HC::EventDecl d; d.name = name;
		graph.events.push_back(std::move(d));
		g.selectedEvent = name;
		edited = true;
	}
	{
		std::string removeEvent;
		for (auto& e : graph.events)
		{
			ImGui::PushID(e.name.c_str());
			const std::string label = e.name + (e.hasArg ? "  (1 arg)" : "");
			if (ImGui::Selectable(label.c_str(), g.selectedEvent == e.name))
			{ g.selectedEvent = e.name; g.selectedVar.clear(); g.selectedNode = 0; }
			if (ImGui::BeginPopupContextItem())
			{
				if (EditorWidgets::dangerMenuItem("Delete")) removeEvent = e.name;
				ImGui::EndPopup();
			}
			ImGui::PopID();
		}
		if (!removeEvent.empty())
		{
			// The nodes keep the name: deleting the declaration says "this is no
			// longer part of the interface", not "rewrite the graph".
			graph.events.erase(std::remove_if(graph.events.begin(), graph.events.end(),
				[&](const HC::EventDecl& d) { return d.name == removeEvent; }),
				graph.events.end());
			if (g.selectedEvent == removeEvent) g.selectedEvent.clear();
			edited = true;
		}
	}

	ImGui::SeparatorText("Functions");
	if (EditorWidgets::addButton("##fn", "Add a function"))
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
		g.selectedEvent.clear();
		g.focusSelected = true;
		edited = true;
	}
	for (const auto& n : graph.nodes)
	{
		if (n.type != NT::FunctionEntry) continue;
		ImGui::PushID(n.id);
		// A duplicate name is dead code: calls resolve by name, first one wins.
		int same = 0;
		for (const auto& o : graph.nodes)
			if (o.type == NT::FunctionEntry && o.s == n.s) ++same;
		const std::string label = (n.s.empty() ? "(unnamed)" : n.s) +
		                          (n.access == 1 ? "  [private]" : "") +
		                          (same > 1 ? "  (duplicate name!)" : "");
		if (same > 1) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(235, 120, 90, 255));
		if (ImGui::Selectable(label.c_str(), g.currentGraph == n.id))
		{
			g.currentGraph = n.id;                // open the function's sub-graph
			g.selectedNode = n.id;
			g.selectedVar.clear();
			g.selectedEvent.clear();
			g.focusSelected = true;
		}
		if (same > 1) ImGui::PopStyleColor();
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
// Every node bound to this event takes the declaration's argument shape: the
// declaration is the interface, so an Emit that carried a Float and a handler
// that read a String can no longer disagree.
void syncEventNodes(HC::Graph& graph, const HC::EventDecl& e)
{
	for (auto& n : graph.nodes)
	{
		if (n.s != e.name) continue;
		if (n.type != NT::Event && n.type != NT::EmitEvent) continue;
		const bool shapeChanged = n.hasArg != e.hasArg || n.propType != e.argType ||
		                          n.typeName != e.typeName;
		n.hasArg   = e.hasArg;
		n.propType = e.argType;
		n.typeName = e.typeName;
		if (shapeChanged)
		{
			// The value pin changed type (or vanished) — its wire no longer
			// typechecks. Event has it as an OUTPUT, Emit as an INPUT.
			const HGH::PinRanges r = HGH::pinRanges(n);
			HGH::removePinLinks(graph, n.id, n.type == NT::Event ? r.dataOut0 : r.dataIn0);
		}
	}
}

// Detail editor for a declared event: its name and its one optional argument.
// Renaming rewrites every node that used the old name — the declaration IS the
// interface, so the nodes follow it rather than the other way round.
void drawEventDetails(HC::Graph& graph, ContentManager* content, bool& edited)
{
	HC::EventDecl* e = graph.findEvent(g.selectedEvent);
	if (!e) { g.selectedEvent.clear(); return; }

	ImGui::TextDisabled("Event");
	static std::string s_nameEdit;
	static std::string s_nameFor;
	if (s_nameFor != e->name) { s_nameEdit = e->name; s_nameFor = e->name; }
	ImGui::InputText("Name", &s_nameEdit);
	if (ImGui::IsItemDeactivatedAfterEdit())
	{
		const std::string nn = s_nameEdit;
		if (nn.empty() || nn == e->name) s_nameEdit = e->name;
		else if (graph.findEvent(nn) || HC::findEngineEvent(nn))
			s_nameEdit = e->name;      // taken, or an engine event — refuse
		else
		{
			const std::string old = e->name;
			e->name = nn;
			for (auto& n : graph.nodes)
				if ((n.type == NT::Event || n.type == NT::EmitEvent ||
				     n.type == NT::BindEvent) && n.s == old)
					n.s = nn;
			g.selectedEvent = nn;
			s_nameFor = nn;
			edited = true;
		}
	}

	bool hasArg = e->hasArg;
	if (ImGui::Checkbox("Carries a value", &hasArg))
	{
		e->hasArg = hasArg;
		syncEventNodes(graph, *e);
		edited = true;
	}
	if (e->hasArg)
	{
		const PT oldType = e->argType;
		const std::string oldTypeName = e->typeName;
		if (HcEditorUtil::drawTypePicker("Value type", content, e->argType,
		                                 nullptr, &e->typeName))
		{
			if (e->argType != oldType || e->typeName != oldTypeName) syncEventNodes(graph, *e);
			edited = true;
		}
	}
	ImGui::TextDisabled("Emit Event raises it; another class binds to it and\n"
	                    "runs its own Event node of this name.");

	ImGui::Spacing();
	if (EditorWidgets::dangerButton("Delete Event"))
	{
		const std::string gone = e->name;
		graph.events.erase(std::remove_if(graph.events.begin(), graph.events.end(),
			[&](const HC::EventDecl& d) { return d.name == gone; }), graph.events.end());
		g.selectedEvent.clear();
		edited = true;
	}
}

void drawVariableDetails(HC::Graph& graph, const std::vector<HC::InheritedVariable>& inheritedVars,
                         ContentManager* content, bool& edited)
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
		g.varNameErrorName.clear();
	}
	ImGui::InputText("Name", &g.varNameEdit);
	if (ImGui::IsItemDeactivatedAfterEdit())
	{
		const std::string oldName = v->name;
		const std::string nn = g.varNameEdit;
		if (nn.empty() || (nn != oldName && varNameTaken(graph, nn)))
		{
			g.varNameEdit = oldName; // reject blank/clash
			// A clash with this class's OWN list is visible in the sidebar above,
			// so silently snapping back explains itself. A clash with a base
			// class's PRIVATE variable does not — that declaration is in another
			// asset and this panel cannot show it — and an unexplained rejection
			// is the kind of thing an author retries four times before giving up.
			g.varNameError = nn.empty() ? std::string()
			                            : inheritedFrom(inheritedVars, nn);
			g.varNameErrorName = nn;
		}
		else if (nn != oldName)
		{
			v->name = nn;
			for (auto& n : graph.nodes)
				if ((n.type == NT::GetVariable || n.type == NT::SetVariable) && n.s == oldName)
					n.s = nn;
			g.selectedVar = nn;
			g.varNameEditFor = nn;
			g.varNameErrorName.clear();
			edited = true;
		}
	}
	if (!g.varNameErrorName.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(235, 120, 90, 255));
		if (g.varNameError.empty())
			ImGui::TextWrapped("\"%s\" is already used by another variable here.",
			                   g.varNameErrorName.c_str());
		else
			ImGui::TextWrapped("\"%s\" is already used by %s. An instance has one "
			                   "variable store, so the same name here would be that "
			                   "variable, not a new one.",
			                   g.varNameErrorName.c_str(),
			                   HcEditorUtil::castTargetLabel(g.varNameError).c_str());
		ImGui::PopStyleColor();
	}

	// One searchable type dropdown: default value types + object (class) types.
	// Picking an object type sets v->className to the class; the label shows the
	// class name instead of "Object".
	const PT oldType = v->type;
	const std::string oldTypeName = v->typeName;
	if (HcEditorUtil::drawTypePicker("Type", content, v->type, &v->className, &v->typeName))
	{
		// A different Enum/Struct DEFINITION is a type change too — the pins
		// keep their PinType but stop typechecking against the old wires.
		if (v->type != oldType || v->typeName != oldTypeName)
		{
			v->defaultItems.clear(); // array slots hold the OLD element type
			v->s.clear();            // enum default entry name belongs to the old enum
			for (auto& n : graph.nodes)
				if ((n.type == NT::GetVariable || n.type == NT::SetVariable) && n.s == v->name)
				{
					n.propType = v->type;
					n.typeName = v->typeName;
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
			case PT::Enum:
			{
				// The default is the entry NAME (renumber-safe; resolved at seed).
				HE::EnumDef def;
				if (!HE::TypeRegistry::instance().getEnum(v->typeName, def) || def.entries.empty())
				{ ImGui::TextDisabled("(no definition)"); break; }
				const char* shown = v->s.empty() ? def.entries.front().name.c_str() : v->s.c_str();
				if (ImGui::BeginCombo("##vdef", shown))
				{
					for (const auto& e : def.entries)
						if (ImGui::Selectable(e.name.c_str(), e.name == v->s))
						{ v->s = e.name; edited = true; }
					ImGui::EndCombo();
				}
				break;
			}
			case PT::Struct: break;   // drawn below (it needs its own section)
			default: break;
		}
		if (v->type == PT::Struct && HcEditorUtil::drawStructDefaultEditor(*v))
			edited = true;
	}
	else if (HcEditorUtil::drawArrayDefaultEditor(*v)) // slot list seeds the array
		edited = true;

	ImGui::Spacing();
	if (EditorWidgets::dangerButton("Delete Variable"))
	{
		const std::string gone = v->name;
		graph.variables.erase(std::remove_if(graph.variables.begin(), graph.variables.end(),
			[&](const HC::Variable& vv){ return vv.name == gone; }), graph.variables.end());
		g.selectedVar.clear();
		g.selectedEvent.clear();
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
// `derivable` = this graph belongs to a CLASS asset, i.e. something another
// class can derive from. Only there does "Overridable" mean anything.
void drawNodeDetails(HC::Graph& graph, const std::vector<std::string>& events,
                     bool allowCustomEvents, ContentManager* content, bool derivable,
                     bool& edited)
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

	// "May a class derived from this one replace this?" — C++'s `virtual`, and
	// opt-in for the same reason: the base author decides what is meant to be
	// replaced. Only a CLASS asset can be a base, so only that frontend shows
	// it. An overridden member is fully replaced: if the child has one, only
	// the child's runs.
	auto overridableRow = [&](HC::Node& node, const char* what)
	{
		if (!derivable) return;
		if (ImGui::Checkbox("Overridable", &node.overridable)) edited = true;
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Classes deriving from this one can replace this %s.\n"
			                  "It then appears in their add menu as an override, and\n"
			                  "only their version runs.", what);
	};

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
		overridableRow(*n, "event");
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
		overridableRow(*n, "function");
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
		if (HGH::drawEventPicker(graph, *n, "Event")) edited = true;
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
                const ImVec2& avail, ContentManager* content, const HC::Graph* giGraph,
                const std::string& baseClass,
                const std::vector<HC::OverridableMember>& overrides,
                const std::vector<InputActionRef>& inputActions, bool& edited)
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
	host.selfBaseClass = baseClass;
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
	m.drawAddMenu = [&graph, &events, allowCustomEvents, &host, &overrides,
	                 &inputActions]() -> int {
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
			if (HcEditorUtil::searchMenuItem(ev, used))
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
			if (HcEditorUtil::searchMenuItem("Custom Event"))
			{ created = addNode(graph, NT::Event, g.ge.addMenuGraphPos); ImGui::CloseCurrentPopup(); }
		}
		if (eh) ImGui::Spacing();

		// ── Input ────────────────────────────────────────────────────────────
		// One entry per InputAction asset, under the action's own name. The node
		// it inserts carries the Pressed and Released chains together, which is
		// why this replaced the two "Input.<action>.Pressed"/".Released" entries
		// that used to sit in the Events list: one action, one thing to pick,
		// and the event-name convention stays where it belongs — on the wire.
		// Like events, they belong in the event graph, not inside a function.
		if (g.currentGraph == 0)
		{
			bool ih = false;
			for (const InputActionRef& ia : inputActions)
			{
				if (!matches(ia.name, "Input")) continue;
				if (!ih) { ImGui::TextDisabled("Input"); ih = true; }
				// One node per action: a second one for the same action would be a
				// second handler for the same events, which is confusing rather
				// than useful (Sequence is how you fan out).
				const bool used = std::any_of(graph.nodes.begin(), graph.nodes.end(),
					[&](const HC::Node& n){ return n.type == NT::InputAction && n.s == ia.name; });
				if (HcEditorUtil::searchMenuItem(ia.name, used))
				{
					const int id = addNode(graph, NT::InputAction, g.ge.addMenuGraphPos);
					HC::Node* nn = graph.findNode(id);
					nn->s = ia.name;
					// An axis action has no press or release — one chain and a
					// value instead. This is the pin layout, so it is set here and
					// not left to be discovered.
					nn->hasArg = ia.isAxis;
					created = id; ImGui::CloseCurrentPopup();
				}
				if (used) { ImGui::SameLine(); ImGui::TextDisabled("(added)"); }
			}
			if (ih) ImGui::Spacing();
		}

		// What this class INHERITS and may replace. Picking one starts a copy of
		// the ancestor's declaration here — same name, same signature — and from
		// then on only this version runs. A member already overridden here is
		// shown as taken rather than hidden, so it is visible that it exists.
		//
		// An EVENT override is a node in the event graph. A FUNCTION override is
		// a function: it gets its own sub-graph and the editor switches to it,
		// exactly as the sidebar's "Add function" does. Dropping a bare entry
		// beside the events instead left the override's body in the middle of
		// the event graph, where it does not belong and where the Graphs list —
		// which is how a function is reopened — could not lead back to it.
		bool oh = false;
		for (const HC::OverridableMember& m2 : overrides)
		{
			const bool isEvent = m2.kind == NT::Event;
			if (isEvent && g.currentGraph != 0) continue;   // events live in the event graph
			const std::string label = "Override " + m2.name;
			if (!matches(label, "Override")) continue;
			const bool used = std::any_of(graph.nodes.begin(), graph.nodes.end(),
				[&](const HC::Node& n){ return n.type == m2.kind && n.s == m2.name; });
			if (!oh) { ImGui::TextDisabled("Inherited"); oh = true; }
			if (HcEditorUtil::searchMenuItem(label, used))
			{
				// The declaration is copied wholesale, then the identity the new
				// node was born with is put back: an id and a position belong to
				// THIS graph, and the sub-graph is decided below.
				auto stamp = [&](int id, float x, float y, int sub)
				{
					HC::Node* nn = graph.findNode(id);
					*nn = m2.prototype;
					nn->id = id; nn->x = x; nn->y = y; nn->subgraph = sub;
				};
				if (isEvent)
				{
					const int id = addNode(graph, m2.kind, g.ge.addMenuGraphPos);
					stamp(id, g.ge.addMenuGraphPos.x, g.ge.addMenuGraphPos.y, g.currentGraph);
					HC::syncFunctionSignatures(graph);
					created = id;
				}
				else
				{
					const int fnId = addNode(graph, NT::FunctionEntry, ImVec2(40.0f, 40.0f));
					stamp(fnId, 40.0f, 40.0f, fnId);    // the function owns its sub-graph
					g.currentGraph = fnId;              // …so addNode scopes the return here
					const int retId = addNode(graph, NT::FunctionReturn, ImVec2(420.0f, 40.0f));
					HC::Node* rn = graph.findNode(retId);
					rn->s       = m2.prototype.s;
					rn->results = m2.prototype.results;
					HC::syncFunctionSignatures(graph);
					g.selectedNode = fnId;
					g.selectedVar.clear();
					g.selectedEvent.clear();
					g.focusSelected = true;
					created = fnId;
				}
				ImGui::CloseCurrentPopup();
			}
			if (used) { ImGui::SameLine(); ImGui::TextDisabled("(overridden)"); }
		}
		if (oh) ImGui::Spacing();

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
	// liveEdit is the mid-drag half of the same answer: a node being moved has
	// already changed the document, and waiting for the mouse to come up is what
	// made a collaborating peer see the move as a jump instead of a movement.
	if (GraphEditor::draw("##ls_canvas", m, g.ge, avail) || g.ge.liveEdit)
		edited = true;
	g.selectedNode = g.ge.selected;

	HGH::handleGraphKeys(host, canvasOrigin, avail);
}

// Shared window body: left sidebar (variables + functions + details) + canvas,
// over one HorizonCode graph with the given event catalog. Used for both the
// Level Script and the Game Instance windows (they differ only in the graph,
// the events, and how a change is committed).
// `baseClass` is the engine base the graph derives from — only the class tab
// has one, which is why it is defaulted rather than required of all three.
// `baseClass` is the engine base the graph derives from and `derivable` says the
// graph belongs to a CLASS asset — only the class tab has either.
void drawGraphBody(HC::Graph& graph, const std::vector<std::string>& events,
                   bool allowCustomEvents, const char* title, const char* subtitle,
                   ContentManager* content, const HC::Graph* giGraph, bool& edited,
                   const std::string& baseClass = {}, bool derivable = false,
                   const std::vector<HC::OverridableMember>& overrides = {},
                   const std::vector<HC::InheritedVariable>& inheritedVars = {},
                   const std::string& classKey = {},
                   const std::vector<InputActionRef>& inputActions = {})
{
	// Swap in this graph's own context (see s_graphStates). The outgoing one is
	// parked under its own key, so coming back to a tab finds the sub-graph it
	// was left in, the same selection and the same scroll position — and,
	// crucially, nothing of it can act on the graph now on screen.
	{
		const std::string key = classKey.empty() ? std::string(title) : classKey;
		if (g.graphFor != key)
		{
			if (!g.graphFor.empty()) s_graphStates[g.graphFor] = std::move(g);
			if (const auto it = s_graphStates.find(key); it != s_graphStates.end())
				g = std::move(it->second);
			else
				g = LSState{};
			g.graphFor = key;
		}
	}
	// A sub-graph id that no longer names a function (it was deleted) must fall
	// back to the event graph too.
	if (g.currentGraph != 0)
	{
		const HC::Node* e = graph.findNode(g.currentGraph);
		if (!e || e->type != NT::FunctionEntry) g.currentGraph = 0;
	}

	ImGui::BeginChild("##ls_side", ImVec2(220.0f, 0.0f), true);
	{
		// 220 px of sidebar, and nearly everything it prints is a sentence: the
		// subtitle under the title, the hint under every detail row ("public
		// functions are callable from Lua/Python."), the "select something" line.
		// Without a wrap position ImGui lays each of those out on one line straight
		// past the right edge and clips whatever hangs over, so the reader is handed
		// the first two thirds of an explanation with nothing to indicate a third is
		// missing — the same defect as a horizontal scrollbar, minus the scrollbar
		// that would at least admit it.
		//
		// It has to be pushed on THIS window: a wrap position lives on the window it
		// was pushed in and does not reach into a child, which is also what keeps it
		// off the canvas next door, where the graph places node bodies, pins and
		// links at computed positions and a wrap would take the layout apart. And it
		// is a scope guard rather than a bare Push/Pop pair because the pop must
		// happen while this child is still the current window — after EndChild it
		// would pop the parent's stack instead.
		//
		// It reaches into everything the detail editors below draw, including the
		// shared ones — and a wrap position is wrong for a row that places its
		// second column with SameLine at a fixed offset, because that column then
		// wraps in whatever few pixels are left of 220. Those tables opt out for
		// themselves: HcEditorUtil::drawStructDefaultEditor does it and says why.
		// Anything added here that lays out by hand has to do the same.
		EditorWidgets::WrapText wrap;

		ImGui::TextUnformatted(title);
		ImGui::TextDisabled("%s", subtitle);
		ImGui::Spacing();
		drawVariables(graph, inheritedVars, edited);
		ImGui::Spacing();
		drawFunctions(graph, edited);
		ImGui::Spacing();
		ImGui::Separator();
		if (g.selectedNode != 0)           drawNodeDetails(graph, events, allowCustomEvents,
		                                                   content, derivable, edited);
		else if (!g.selectedVar.empty())   drawVariableDetails(graph, inheritedVars, content, edited);
		else if (!g.selectedEvent.empty()) drawEventDetails(graph, content, edited);
		else ImGui::TextDisabled("Select a node, variable or event.");
	}
	ImGui::EndChild();

	ImGui::SameLine();

	ImGui::BeginChild("##ls_canvas_host", ImVec2(0.0f, 0.0f), true);
	// A stale compile result from another tab must not anchor to this graph.
	if (g.compileHas && g.compileFor != title) g.compileHas = false;
	// Which sub-graph is shown, and the compile check — the canvas gets its own
	// strip, in the same language as the tab's own bar above it.
	{
		namespace T = EditorToolbar;
		std::string where = "Event Graph";
		if (g.currentGraph != 0)
		{
			const HC::Node* e = graph.findNode(g.currentGraph);
			where = std::string("Function: ") +
			        (e && !e->s.empty() ? e->s.c_str() : "(unnamed)");
		}

		T::Bar bar;
		bar.group();
		bar.readout(g.currentGraph == 0 ? T::iconList : T::iconCode, where.c_str());
		bar.endGroup();

		// The compile result belongs on the strip too: it is a state of this
		// graph, and as a line underneath it pushed the canvas down and up again
		// every time someone pressed the button.
		// A clean result is an answer, not a state: it fades after a few
		// seconds, the way a "saved" toast would. An ERROR stays — it names a
		// node someone still has to fix, and a problem that hides itself on a
		// timer just gets rediscovered the hard way on export.
		const bool showCompile = g.compileHas &&
			(!g.compileOk || ImGui::GetTime() - g.compileAt < 6.0);
		if (showCompile)
		{
			bar.group();
			bar.readout(g.compileOk ? T::iconCheck : T::iconWarning,
			            g.compileMsg.c_str(), g.compileOk ? T::kGood : T::kBad);
			bar.endGroup();
		}

		bar.rightGroup(bar.labelGroupWidth({ "Compile" }));
		if (bar.item("##hccompile", T::iconHammer, "Compile", false, true,
		             "Translate this class to C++ the way a packaged export would.\n"
		             "Errors highlight the offending node; a clean result means the\n"
		             "class ships compiled (everything else runs interpreted)."))
		{
			runCompileCheck(graph, title, classKey, content);
		}
		bar.endGroup();
	}
	if (g.compileHas)
	{
		if (!g.compileOk)
		{
			ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "Runs interpreted.");
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
	drawCanvas(graph, events, allowCustomEvents, avail, content, giGraph, baseClass,
	           overrides, inputActions, edited);

	// Variable drop → Get/Set popup.
	if (g.openVarDrop) { ImGui::OpenPopup("##ls_var_drop"); g.openVarDrop = false; }
	if (ImGui::BeginPopup("##ls_var_drop"))
	{
		// Inherited too: the sidebar hands those out as drag sources, and a Get
		// node made from one has to take ITS type, not fall through untyped.
		const HC::Variable* v = graph.findVariableOrInherited(g.dropVar);
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
	// The "no scene" line is the one thing this panel ever draws at window scope,
	// and it is a full sentence — in a narrow tab it would be clipped mid-word.
	// The early return became an else so the guard's destructor runs before
	// ImGui::End(): popping a wrap position after the window is closed would take
	// it off whatever window happens to be current instead.
	if (!ctx.world)
	{
		EditorWidgets::WrapText wrap;
		ImGui::TextDisabled("Open a scene to edit its level script.");
	}
	else
	{
		static const std::vector<std::string> kEvents = { "OnLevelLoaded", "OnLevelUnloaded" };
		bool edited = false;
		drawGraphBody(ctx.world->levelScript(), kEvents, /*allowCustomEvents=*/false, "Level Script",
		              "Reacts to world events.", ctx.contentManager, ctx.gameInstanceGraph, edited);
		// snapshotNow() bumps the undo revision so the level script saves with the
		// scene; self-contained so it doesn't disturb the entity undo.
		if (edited && ctx.undoSys) ctx.undoSys->snapshotNow();
	}
	ImGui::End();
}

void LevelScriptPanel::forgetAllGraphContexts()
{
	s_graphStates.clear();
	g = LSState{};
}

void GameInstancePanel::render(AppContext& ctx, const ImVec2& pos, const ImVec2& size)
{
	beginTabWindow("##gameinstance_tab", pos, size);
	// Same shape as the Level Script tab above, and for the same two reasons: the
	// "no project" sentence is drawn at window scope, and the guard has to close
	// before ImGui::End().
	if (!ctx.gameInstanceGraph)
	{
		EditorWidgets::WrapText wrap;
		ImGui::TextDisabled("Open a project to edit its Game Instance.");
	}
	else
	{
		static const std::vector<std::string> kEvents = { "OnInit", "OnShutdown", "OnWindowFocusChanged" };
		bool edited = false;
		drawGraphBody(*ctx.gameInstanceGraph, kEvents, /*allowCustomEvents=*/false, "Game Instance",
		              "App-wide. Runs before anything loads.", ctx.contentManager, ctx.gameInstanceGraph, edited);
		// The GameInstance graph isn't part of a scene — re-register it in the app
		// runtime and persist it via the host callback.
		if (edited && ctx.commitGameInstance) ctx.commitGameInstance();
	}
	ImGui::End();
}

// ── HorizonCode Class tab (a standalone .hasset graph) ────────────────────────
namespace
{
struct ClassState
{
	HorizonCode::Graph graph;
	// Last state the peers have seen — see CollabDocSync.
	CollabDocSync::DocMirror collabMirror;
	bool        loaded = false;
	bool        dirty  = false;
	std::string name;
	// "" = plain Object, an engine class name, or — with HorizonCode
	// inheritance — ANOTHER class asset's content-relative path.
	std::string baseClass;
	HE::UUID    assetId;
	std::string path;                   // content-relative, the class KEY
	std::vector<std::string> events;    // event catalog (the base-class chain's)
	// The project's InputAction assets, offered as their own menu section on a
	// player class — one entry per action, not two events per action.
	std::vector<InputActionRef> inputActions;
	double      eventsScanTime = -1.0;  // last catalog (re)build, ImGui time
	// ── Components mode ──────────────────────────────────────────────────
	// The class's component list, edited as a real entity subtree in a world
	// of its own. Going through an actual HorizonWorld is what lets the REAL
	// Details panel draw it (InspectorPanel::renderFor) instead of a second
	// component editor that would drift from the first.
	std::unique_ptr<HorizonWorld> compWorld;
	Entity      compRoot = entt::null;
	Entity      compSel  = entt::null;
	bool        showComponents = false;
	// The base class RESOLVED through the chain: with HorizonCode inheritance
	// `baseClass` may name another class asset, and everything that used to ask
	// the raw string — which lifecycle events to offer, whether this is a
	// player, which components a new body starts with — means the ENGINE row at
	// the root of the chain.
	std::string engineBase;
	std::vector<std::string> ancestors;   // nearest first
	// What the ancestors declare as overridable — the add menu's Inherited
	// section. Rebuilt together with the resolved base, since both read assets.
	std::vector<HorizonCode::OverridableMember> overrides;
	// The chain's variables, with the ancestor each came from. The graph carries
	// the declarations themselves (Graph::inherited) for the menus and pickers;
	// this list keeps the ORIGIN, which only the sidebar and the "that name is
	// taken" message need.
	std::vector<HorizonCode::InheritedVariable> inheritedVars;
	// Borrowed for the component seed above; the panel only ever uses it while
	// the editor owns it.
	ContentManager* contentForSeed = nullptr;
};
AssetPanelState<ClassState> s_classStates;

// Input.<Action>.* event names for every InputAction asset in the project —
// the input-event catalog player classes offer. Walks the content dir (cheap
// header sniffs), so callers cache the result and refresh on a coarse timer.
// Every InputAction asset in the project, as (action name, is it an axis).
// One entry per ACTION — the add menu offers one node for it, with a Pressed
// and a Released exec-out, instead of the two separate "Input.<x>.Pressed" /
// ".Released" events it used to list. The wire format still uses those names
// (HE::inputEvent*), but nobody has to read or pick them any more.
std::vector<InputActionRef> scanInputActions(ContentManager* cm)
{
	std::vector<InputActionRef> out;
	if (!cm) return out;
	for (const auto& ref : HcEditorUtil::listAssets(cm, HE::AssetType::InputAction))
	{
		bool axis = false;
		HAsset::Reader r;
		if (r.open(cm->resolveAbsolutePath(ref.path)))
			if (const auto* c = r.findChunk(HAsset::CHUNK_IACT))
				axis = HE::inputActionIsAxis(std::string(
					reinterpret_cast<const char*>(c->data.data()), c->data.size()));
		out.push_back({ ref.label, axis });
	}
	return out;
}

// Persist a class tab's graph. The header's Save button AND the close/quit
// prompt's "Save All" both come through here, so the two can never drift apart.
// The class's component template, as an entity subtree in a world of its own.
// Created on first use — an empty class seeds from its BASE CLASS's default
// list, which is what "a PlayerCharacter comes with a character controller"
// means in practice.
void ensureComponentWorld(ClassState& st, const std::vector<uint8_t>& blob)
{
	if (st.compWorld) return;
	st.compWorld = std::make_unique<HorizonWorld>();
	SceneSerializer ser;
	// An empty class seeds from what it INHERITS: its nearest ancestor's body
	// if it derives from a class, else the engine base's default list. That is
	// what makes a Goblin start out looking like an Enemy.
	std::vector<uint8_t> inherited;
	if (blob.empty())
	{
		if (!st.ancestors.empty() && st.contentForSeed)
			if (const HorizonCodeClassAsset* parent =
			        st.contentForSeed->getHorizonCodeClass(
			            st.contentForSeed->loadAsset(st.ancestors.front())))
				inherited = parent->componentBlob;
		if (inherited.empty()) inherited = EntityHost::defaultComponents(st.engineBase);
	}
	const std::vector<uint8_t>& seed = blob.empty() ? inherited : blob;
	if (!seed.empty())
		st.compRoot = ser.instantiatePrefab(*st.compWorld, seed);
	if (st.compRoot == entt::null)
	{
		st.compRoot = st.compWorld->createEntity(st.name.empty() ? "Entity" : st.name);
		st.compWorld->addComponent(st.compRoot, TransformComponent{});
	}
	st.compSel = st.compRoot;
}

bool saveClassState(ClassState& st, AppContext& ctx)
{
	if (!ctx.contentManager) return false;
	HorizonCodeClassAsset* a = ctx.contentManager->getHorizonCodeClassMutable(st.assetId);
	if (!a) return false;
	a->graphJson = HorizonCode::toJson(st.graph);
	a->baseClass = st.baseClass;
	// Only overwrite the component list once this tab has actually opened it:
	// saving a class whose Components mode was never touched must not replace
	// its authored subtree with a freshly seeded default.
	if (st.compWorld && st.compRoot != entt::null)
	{
		SceneSerializer ser;
		a->componentBlob = ser.serializeSubtree(*st.compWorld, st.compRoot);
	}
	if (!ctx.contentManager->saveAsset(*a)) return false;
	st.dirty = false;
	return true;
}

// Components mode: the subtree on the left, the REAL Details panel on the
// right. InspectorPanel::renderFor draws it against this scratch world, so
// every component gets the exact editor the scene inspector gives it —
// including its asset slots — instead of a parallel one to keep in step.
// Undo is null: a snapshot here would capture the SCENE, which is not what is
// being edited.
void drawComponentsBody(AppContext& ctx, ClassState& st)
{
	ensureComponentWorld(st, {});
	if (!st.compWorld) return;
	auto& reg = st.compWorld->registry();
	if (!reg.valid(st.compSel)) st.compSel = st.compRoot;

	const float listW = 220.0f;
	ImGui::BeginChild("##hccomp_tree", ImVec2(listW, 0), true);
	ImGui::TextDisabled("Subtree");
	ImGui::Separator();
	for (auto [e, name] : reg.view<NameComponent>().each())
	{
		ImGui::PushID((int)entt::to_integral(e));
		const bool isRoot = (e == st.compRoot);
		if (ImGui::Selectable((isRoot ? name.name + "  (root)" : name.name).c_str(),
		                      e == st.compSel))
			st.compSel = e;
		ImGui::PopID();
	}
	ImGui::Separator();
	if (ImGui::SmallButton("Add Child"))
	{
		const Entity child = st.compWorld->createEntity("Child");
		st.compWorld->addComponent(child, TransformComponent{});
		st.compWorld->reparentEntity(child, st.compRoot);
		st.compSel = child;
		st.dirty = true;
	}
	if (st.compSel != st.compRoot && reg.valid(st.compSel))
	{
		ImGui::SameLine();
		if (EditorWidgets::dangerSmallButton("Remove"))
		{
			st.compWorld->destroyEntity(st.compSel);
			st.compSel = st.compRoot;
			st.dirty = true;
		}
	}
	ImGui::EndChild();

	ImGui::SameLine();
	ImGui::BeginChild("##hccomp_details", ImVec2(0, 0), true);
	if (reg.valid(st.compSel))
	{
		InspectorPanel::renderFor(ctx, *st.compWorld, st.compSel, nullptr);
		// The scratch world has no undo revision to diff against, so the tab
		// takes ImGui's word for it. Scoped to THIS child: IsAnyItemActive is
		// global to the frame, so on its own it would mark the class unsaved
		// because someone was dragging a slider in another panel.
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
		    ImGui::IsAnyItemActive())
			st.dirty = true;
	}
	else ImGui::TextDisabled("(nothing selected)");
	ImGui::EndChild();
}

// "PlayerController" → "Player Controller". The taxonomy stores the C++-ish
// spelling because it is a format (CHUNK_HCBC, and the Cast node's target key);
// this is purely how it reads in the UI, so it stays a formatting rule rather
// than a second column that could drift out of sync with the first.
std::string humanClassName(const std::string& name)
{
	if (name.empty()) return "Object";
	std::string out(1, name[0]);
	for (size_t i = 1; i < name.size(); ++i)
	{
		if (std::isupper((unsigned char)name[i]) && !std::isupper((unsigned char)name[i - 1]))
			out += ' ';
		out += name[i];
	}
	return out;
}
}

bool HorizonCodeClassPanel::isClassAsset(const std::string& path)
{
	return EditorAssetTypeCache::is(path, HE::AssetType::HorizonCodeClass);
}

void HorizonCodeClassPanel::forget(const std::string& path)
{
	// The graph context goes with the tab. It is keyed by the CONTENT-relative
	// path (that is what drawGraphBody is handed), which only the ClassState
	// knows — so read it before forgetting the state that holds it. Leaving it
	// behind would hand a class recreated under the same path the view state of
	// the deleted one, pointing at node ids that are gone.
	if (const ClassState* st = s_classStates.find(path); st && !st->path.empty())
	{
		s_graphStates.erase(st->path);
		if (g.graphFor == st->path) g = LSState{};
	}
	s_classStates.forget(path);
}

bool HorizonCodeClassPanel::isDirty(const std::string& path) { return s_classStates.dirty(path); }

CollabDocSync::DocBindings HorizonCodeClassPanel::collabDocs(const std::string& assetPath)
{
	ClassState* st = s_classStates.find(assetPath);
	if (!st || !st->loaded) return {};
	CollabDocSync::DocBindings out;
	out.push_back({ CollabDocSync::Scope::Primary,
	                CollabDocSync::forHorizonCodeGraph(st->graph), &st->collabMirror });
	return out;
}

bool HorizonCodeClassPanel::reloadFromDisk(const std::string& assetPath)
{
	// A collaboration peer's change just landed in the file. Dropping `loaded`
	// makes the next frame re-read it while the rest of the State survives.
	// Dirty is cleared deliberately: while a peer holds the asset's lock this
	// panel is read-only anyway, so anything "unsaved" here is stale.
	auto* st = s_classStates.find(assetPath);
	if (!st) return false;
	st->loaded = false;
	st->dirty = false;
	// The mirror describes the document that is about to be replaced. Leaving
	// it would make the first diff after the reload report the difference
	// between the peer's file and our old graph as OUR edit.
	st->collabMirror = {};
	return true;
}


void HorizonCodeClassPanel::appendDirtyPaths(std::vector<std::string>& out) { s_classStates.appendDirtyPaths(out); }

bool HorizonCodeClassPanel::save(AppContext& ctx, const std::string& path)
{
	ClassState* st = s_classStates.find(path);
	// A tab this panel never opened has nothing to write — the caller asks every
	// panel about every path, so "not mine" must read as success.
	if (!st || !st->dirty) return true;
	return saveClassState(*st, ctx);
}

void HorizonCodeClassPanel::render(AppContext& ctx, const std::string& assetPath,
                                   const ImVec2& pos, const ImVec2& size)
{
	ClassState& st = s_classStates[assetPath];
	if (!st.loaded && ctx.contentManager)
	{
		const std::string rel = ctx.contentManager->toContentRelativePath(assetPath);
		st.path    = rel;
		st.assetId = ctx.contentManager->loadAsset(rel);
		if (const HorizonCodeClassAsset* a = ctx.contentManager->getHorizonCodeClass(st.assetId))
		{
			if (!a->graphJson.empty()) HorizonCode::fromJson(a->graphJson, st.graph);
			// The FILE STEM, not the asset's stored `name`: that one is written
			// into the META chunk at creation and a rename never touches it, so
			// a renamed class would keep announcing itself as "NewClass" in its
			// own tab header. The stem is what the content browser and every
			// class picker call it.
			st.name      = HcEditorUtil::castTargetLabel(rel);
			st.baseClass = a->baseClass;
			// Seed the component template from what the asset carries. Only
			// classes WITH a body get a world up front; the rest build one the
			// first time Components mode is opened, from the base's defaults.
			if (!a->componentBlob.empty()) ensureComponentWorld(st, a->componentBlob);
		}
		st.loaded = true;
	}

	// Input events belong to the two player classes; everything else comes from
	// the taxonomy chain, so a class one level down would inherit them without
	// this line having to learn its name.
	// Resolve the chain when the base changed (eventsScanTime is the same
	// "something moved" signal the catalog uses) rather than every frame: it
	// reads assets.
	if (st.eventsScanTime < 0.0 && ctx.contentManager)
	{
		const HorizonCode::ResolvedClass rc =
			HorizonCode::resolveClassAsset(*ctx.contentManager, st.path);
		st.engineBase = rc.engineBase;
		st.ancestors  = rc.chain;
		st.overrides  = HorizonCode::overridableMembersOf(*ctx.contentManager, st.path);
		// What the chain contributes by name. Rebuilt here rather than every
		// frame because it reads assets, and refreshed on a base change for
		// free — this block runs exactly when the base moved.
		st.inheritedVars = HorizonCode::inheritedVariables(rc);
		st.graph.inherited.clear();
		st.graph.inheritedFns.clear();
		for (const auto& iv : st.inheritedVars) st.graph.inherited.push_back(iv.var);
		for (const auto& f : HorizonCode::inheritedFunctions(rc))
			st.graph.inheritedFns.push_back(f.proto);
		st.contentForSeed = ctx.contentManager;
	}
	const bool isPlayer = HorizonCode::engineClassIsA(st.engineBase, "PlayerController") ||
	                      HorizonCode::engineClassIsA(st.engineBase, "PlayerCharacter");
	// The header names what this class DERIVES FROM — the class asset when it
	// derives from one, else the engine row.
	const std::string kindLabel = st.baseClass.empty()
		? std::string("Object")
		: (HorizonCode::findEngineClass(st.baseClass) ? humanClassName(st.baseClass)
		                                              : HcEditorUtil::castTargetLabel(st.baseClass));

	beginTabWindow(("##hcclass_" + assetPath).c_str(), pos, size);
	{
		namespace T = EditorToolbar;
		T::Bar bar;
		T::assetHeader(bar, st.name.c_str(), T::iconCode, st.dirty);
		// Everything on this band goes through the Bar's OWN cells. A raw ImGui
		// combo or radio drawn here lays itself out in window coordinates while
		// the Bar places its cells into a draw list — the two do not know about
		// each other, and the widget lands on top of the band's own text.
		bar.group();
		if (bar.item("##hcmodegraph", nullptr, "Graph", !st.showComponents, true,
		             "The class's logic"))
			st.showComponents = false;
		if (bar.item("##hcmodecomp", nullptr, "Components", st.showComponents, true,
		             "The class's body — the components an instance brings with it"))
			st.showComponents = true;
		bar.endGroup();

		// The base class was fixed at creation time before, which made "turn
		// this class into an Entity" a delete-and-recreate. A cell that opens a
		// list now; changing it rebuilds the event catalog below.
		bar.group();
		const bool openBase =
			bar.item("##hcbase", nullptr, kindLabel.c_str(), false, true,
			         "Base class — decides the events, the components and the\n"
			         "members this class inherits");
		bar.endGroup();
		if (openBase) ImGui::OpenPopup("##hcbasepopup");
		if (ImGui::BeginPopup("##hcbasepopup"))
		{
			ImGui::TextDisabled("Engine");
			for (const auto& c : HorizonCode::engineClasses())
			{
				// "Object" is stored as the EMPTY string — that is what every
				// asset predating the taxonomy carries, and writing the word
				// instead would be a needless format change.
				const std::string picked =
					std::string(c.name) == "Object" ? std::string() : c.name;
				if (!ImGui::Selectable(humanClassName(c.name).c_str(), picked == st.baseClass))
					continue;
				if (picked != st.baseClass)
				{
					st.baseClass      = picked;
					st.dirty          = true;
					st.eventsScanTime = -1.0;   // rebuild catalog + resolved base
				}
			}
			// …and the project's own classes. Deriving from another class is what
			// makes `Cast To Enemy` succeed on a Goblin, and what lets a Goblin
			// use everything Enemy already defines.
			ImGui::Separator();
			ImGui::TextDisabled("Classes");
			for (const auto& c : HcEditorUtil::listHorizonCodeClasses(ctx.contentManager))
			{
				// A class cannot derive from itself, nor from anything that already
				// derives from IT — the resolver survives a cycle, but offering one
				// as a choice would be offering a mistake.
				if (c.path == st.path) continue;
				bool wouldCycle = false;
				if (ctx.contentManager)
				{
					const HorizonCode::ResolvedClass other =
						HorizonCode::resolveClassAsset(*ctx.contentManager, c.path);
					for (const std::string& a : other.chain)
						if (a == st.path) { wouldCycle = true; break; }
				}
				if (wouldCycle) ImGui::BeginDisabled();
				if (ImGui::Selectable((c.label + "##" + c.path).c_str(), st.baseClass == c.path) &&
				    st.baseClass != c.path)
				{
					st.baseClass      = c.path;
					st.dirty          = true;
					st.eventsScanTime = -1.0;
				}
				if (wouldCycle)
				{
					ImGui::EndDisabled();
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
						ImGui::SetTooltip("That class already derives from this one.");
				}
			}
			ImGui::EndPopup();
		}
		if (T::saveButton(bar, true)) saveClassState(st, ctx);
	}

	// The event catalog is the base-class chain's events (Object contributes
	// Construct/Destruct, Entity adds BeginPlay/Tick). The project's input
	// actions come alongside it as their OWN menu section rather than as events,
	// one entry per action — they used to be listed here as two events each
	// ("Input.Jump.Pressed", "Input.Jump.Released"), which put the wire format in
	// front of the author and split one action into two things to find. Actions
	// can be created while this tab is open, so that part rescans on a timer.
	const double now = ImGui::GetTime();
	if (st.eventsScanTime < 0.0 || (isPlayer && now - st.eventsScanTime > 3.0))
	{
		st.events.clear();
		for (const char* ev : HorizonCode::engineClassEvents(st.engineBase))
			st.events.emplace_back(ev);
		st.inputActions = isPlayer ? scanInputActions(ctx.contentManager)
		                           : std::vector<InputActionRef>{};
		st.eventsScanTime = now;
	}
	bool edited = false;
	if (st.showComponents)
		drawComponentsBody(ctx, st);
	else
		drawGraphBody(st.graph, st.events, /*allowCustomEvents=*/true, kindLabel.c_str(),
		              isPlayer ? "Player class; lifecycle + input events."
		                       : "Reusable class; lifecycle events + its own.",
			              ctx.contentManager, ctx.gameInstanceGraph, edited, st.baseClass,
		              /*derivable=*/true, st.overrides, st.inheritedVars, st.path,
		              st.inputActions);
	if (edited) st.dirty = true;
	ImGui::End();
}
