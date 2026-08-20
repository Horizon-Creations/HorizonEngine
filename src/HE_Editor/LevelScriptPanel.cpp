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
#include "EditorCamera.h"             // the viewport camera — the Scene window's, per tab
#include "EditorViewportNav.h"        // and the Scene window's navigation grammar, shared
#include "EditorTransformGizmo.h"     // …and its move/rotate/scale gizmo
#include "PreviewPick.h"              // click-to-select in an offscreen 3D pane
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
#include <unordered_map>
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

// The level script / GameInstance / class tabs have nothing to add to a node's
// header, so this is the shared table verbatim. It used to be written out here
// AND in the widget editor; a third copy in the animator's sync graph is what
// made it worth folding together.
std::string nodeTitle(const HC::Node& n) { return HGH::defaultNodeTitle(n); }

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

// One InputAction asset, as the add menu offers it. The kind decides the shape
// of the node the menu inserts — press/release pair, one value, or two.
struct InputActionRef
{
	std::string name;    // the action's logical name (its asset stem)
	enum class Kind { Button, Axis, Axis2D };
	Kind        kind = Kind::Button;
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
					// value instead, Float or Vec2. This IS the pin layout, so it
					// is set here rather than left to be discovered.
					nn->hasArg   = ia.kind != InputActionRef::Kind::Button;
					nn->propType = ia.kind == InputActionRef::Kind::Axis2D ? PT::Vec2
					                                                       : PT::Float;
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
	// Which single component of `compSel` is in focus, by its Details-panel
	// label; empty = the whole entity. The tree lists every component as its own
	// row under its entity — that is what "each one individually editable"
	// means — and this is the row that is selected. The labels come from
	// InspectorPanel::listComponents, never from a list kept here.
	std::string compFocus;
	// Which half of the class is on screen. It used to be called
	// "showComponents", which described the middle column rather than the view:
	// the components are only the LIST, and next to it sits the thing they add
	// up to. Unreal calls that half the Viewport and it is the better name — you
	// go there to see the character, not to see a list.
	bool        showViewport = false;
	// The viewport's camera, per tab so switching back finds the view as you
	// left it. A full EditorCamera, not an orbit triple: this viewport navigates
	// with the SAME grammar as the Scene window (fly, orbit, pan, dolly, F), and
	// the scene's camera is exactly what that grammar drives.
	EditorCamera previewCam;
	bool         previewCamFramed = false;   // first frame places it on the class
	// What the renderer drew with, so the transform gizmo lands on the object
	// instead of next to it — see drawWorldPreviewGizmo.
	glm::mat4    previewViewProj{ 1.0f };
	// Whether the mouse is over the preview image, read off the image itself —
	// nothing may be laid over it, or ImGuizmo refuses to grab a handle.
	bool         previewHovered = false;
	// Did the RENDERER draw the world last frame (as opposed to the per-asset
	// fallback)? The camera has to be driven BEFORE the render — the picture is
	// made from it — and at that point this frame's answer does not exist yet.
	// It is a property of the backend, so last frame's cannot be stale.
	bool         previewWorldPath = false;
	// Which operation the gizmo is on (W/E/R), per tab. The class viewport has
	// no toolbar of its own; this is the same state the Scene window's bar edits.
	ViewportToolbar::State previewGizmo;
	// The per-asset FALLBACK preview (backends without a world-preview path)
	// still orbits: it renders one asset framed on its own bounds, and a fly
	// camera has nothing to fly through there.
	float       previewYaw = 0.6f, previewPitch = 0.25f, previewDist = 4.0f;
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
		InputActionRef::Kind kind = InputActionRef::Kind::Button;
		HAsset::Reader r;
		if (r.open(cm->resolveAbsolutePath(ref.path)))
			if (const auto* c = r.findChunk(HAsset::CHUNK_IACT))
			{
				const std::string json(reinterpret_cast<const char*>(c->data.data()),
				                       c->data.size());
				kind = HE::inputActionIsAxis2D(json) ? InputActionRef::Kind::Axis2D
				     : HE::inputActionIsAxis(json)   ? InputActionRef::Kind::Axis
				                                     : InputActionRef::Kind::Button;
			}
		out.push_back({ ref.label, kind });
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
// The Viewport half's middle column. Draws the selected component's asset
// through the renderer's per-asset preview paths (the same ones the Skeletal
// Mesh and Material tabs use), with the orbit those tabs established.
// Draw the subtree's colliders and camera booms over the preview image, in the
// space the renderer just drew it in.
//
// This is deliberately an OVERLAY rather than a second render pass. Putting mesh
// + collider + boom into one picture properly would mean a renderer entry point
// that extracts an arbitrary world into its own target — real work in every
// backend, of which only two are even buildable here, and all of it judged by
// whether the picture looks right, which is exactly what cannot be checked
// without a screen. Projecting a handful of line segments costs nothing, works
// on every backend including the ones nobody can compile here, and draws the
// gizmos THROUGH the mesh, which for a collider is what you want anyway.
//
// The matrix comes from the renderer (RenderSkeletalPreview's outViewProj)
// rather than being rebuilt here: the framing depends on GPU-side bounds only
// the renderer has, and a copy of that rule would drift into looking like a
// wrong collider.
void drawSubtreeGizmos(entt::registry& reg, Entity root, Entity selected,
                       const std::string& focus,
                       const glm::mat4& viewProj, const ImVec2& imgOrigin, const ImVec2& imgSize)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();

	// World → screen. Returns false behind the camera, so a segment with one
	// end behind it is dropped rather than drawn mirrored across the pane.
	const auto project = [&](const glm::vec3& p, ImVec2& out) -> bool
	{
		const glm::vec4 clip = viewProj * glm::vec4(p, 1.0f);
		if (clip.w <= 1e-4f) return false;
		const glm::vec3 ndc = glm::vec3(clip) / clip.w;
		out = ImVec2(imgOrigin.x + (ndc.x * 0.5f + 0.5f) * imgSize.x,
		             imgOrigin.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * imgSize.y);
		return true;
	};
	const auto line = [&](const glm::vec3& a, const glm::vec3& b, ImU32 col)
	{
		ImVec2 pa, pb;
		if (project(a, pa) && project(b, pb)) dl->AddLine(pa, pb, col, 1.4f);
	};

	// Local → root space for one entity of the class subtree.
	const auto worldOf = [&](Entity e)
	{
		glm::mat4 m(1.0f);
		for (Entity cur = e; cur != entt::null && reg.valid(cur); )
		{
			if (const auto* t = reg.try_get<TransformComponent>(cur))
			{
				const glm::quat q = glm::quat(glm::radians(t->rotation));
				m = glm::translate(glm::mat4(1.0f), t->position) * glm::mat4_cast(q) *
				    glm::scale(glm::mat4(1.0f), t->scale) * m;
			}
			if (cur == root) break;
			const auto* h = reg.try_get<HierarchyComponent>(cur);
			cur = h ? h->parent : entt::null;
		}
		return m;
	};

	constexpr ImU32 kCollider = IM_COL32(120, 230, 140, 210);
	constexpr ImU32 kSelected = IM_COL32(255, 210, 100, 255);
	constexpr ImU32 kBoom     = IM_COL32(120, 200, 255, 220);
	// Everything that is NOT the selection. It used to keep its full colour, so
	// selecting the camera left a bright green capsule on screen and the eye
	// read THAT as the highlight — pointing at the wrong component. Dim, not
	// hidden: where the collider sits is still context worth seeing while the
	// camera arm is being placed.
	constexpr ImU32 kIdle     = IM_COL32(150, 155, 165, 70);

	// Highlighting follows the selected COMPONENT, not just its entity. One
	// entity commonly carries several of these — a character root with a
	// collider, or a root that also got a camera rig — so entity-only matching
	// lit up the capsule whenever the camera was selected, which reads as the
	// editor pointing at the wrong thing.
	//
	// The labels are the Details panel's own (componentHeader in
	// InspectorPanel.cpp), which is where the tree's rows come from too.
	const auto highlighted = [&](Entity e, const char* label)
	{
		return e == selected && (focus.empty() || focus == label);
	};
	// While the selection names ONE component, everything else steps back. With
	// the whole entity selected there is nothing to step back from, so the
	// gizmos keep their own colours.
	const bool oneInFocus = !focus.empty();

	for (auto [e, col] : reg.view<ColliderComponent>().each())
	{
		const glm::mat4 m = worldOf(e);
		const ImU32 c = highlighted(e, "Collider") ? kSelected
		                                          : (oneInFocus ? kIdle : kCollider);
		const auto at = [&](float x, float y, float z){ return glm::vec3(m * glm::vec4(x, y, z, 1.0f)); };

		if (col.shape == ColliderShape::Capsule || col.shape == ColliderShape::Sphere)
		{
			// Two upright rings and a waist ring read as a capsule without
			// needing a mesh; for a sphere the caps collapse onto the middle.
			const float r  = std::max(0.01f, col.radius);
			const float hy = col.shape == ColliderShape::Capsule
			                 ? std::max(0.0f, col.height * 0.5f - r) : 0.0f;
			constexpr int kSeg = 24;
			for (int ring = 0; ring < 3; ++ring)
			{
				for (int i = 0; i < kSeg; ++i)
				{
					const float a0 = (float)i / kSeg * 6.2831853f;
					const float a1 = (float)(i + 1) / kSeg * 6.2831853f;
					if (ring == 0)   // waist, in XZ
						line(at(std::cos(a0) * r, 0.0f, std::sin(a0) * r),
						     at(std::cos(a1) * r, 0.0f, std::sin(a1) * r), c);
					else if (ring == 1)  // side profile, in XY
						line(at(std::cos(a0) * r, std::sin(a0) * r + (std::sin(a0) > 0 ? hy : -hy), 0.0f),
						     at(std::cos(a1) * r, std::sin(a1) * r + (std::sin(a1) > 0 ? hy : -hy), 0.0f), c);
					else                 // side profile, in ZY
						line(at(0.0f, std::sin(a0) * r + (std::sin(a0) > 0 ? hy : -hy), std::cos(a0) * r),
						     at(0.0f, std::sin(a1) * r + (std::sin(a1) > 0 ? hy : -hy), std::cos(a1) * r), c);
				}
			}
		}
		else   // Box
		{
			const glm::vec3 h = glm::max(col.halfExtents, glm::vec3(0.01f));
			const glm::vec3 v[8] = {
				at(-h.x,-h.y,-h.z), at( h.x,-h.y,-h.z), at( h.x, h.y,-h.z), at(-h.x, h.y,-h.z),
				at(-h.x,-h.y, h.z), at( h.x,-h.y, h.z), at( h.x, h.y, h.z), at(-h.x, h.y, h.z) };
			constexpr int edges[12][2] = { {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4},
			                               {0,4},{1,5},{2,6},{3,7} };
			for (const auto& ed : edges) line(v[ed[0]], v[ed[1]], c);
		}
	}

	// Camera booms: pivot, the arm, and the camera's own place at the end of it.
	for (auto [e, t, cam, rig] : reg.view<TransformComponent, CameraComponent, CameraRigComponent>().each())
	{
		const ImU32 c = (highlighted(e, "Camera") || highlighted(e, "Camera Rig"))
		                ? kSelected : (oneInFocus ? kIdle : kBoom);
		const glm::vec3 camPos = glm::vec3(worldOf(e)[3]);
		// The target is resolved at runtime; in the class editor the thing it
		// follows is the root, which is what the pivot offset is measured from.
		const glm::vec3 pivot = glm::vec3(worldOf(root)[3]) + rig.pivotOffset;
		line(pivot, camPos, c);
		const float s = 0.08f;
		line(camPos - glm::vec3(s, 0, 0), camPos + glm::vec3(s, 0, 0), c);
		line(camPos - glm::vec3(0, s, 0), camPos + glm::vec3(0, s, 0), c);
		line(camPos - glm::vec3(0, 0, s), camPos + glm::vec3(0, 0, s), c);
	}
}

// The aspect ratio the renderer will use for a pane of this size. Not plain
// av.x / av.y: RenderWorldPreview allocates a whole-pixel target clamped to
// [32, 4096] and builds its projection from THAT, so the sub-pixel remainder
// would be a matrix the picture was not drawn with — and a gizmo built from it
// sits a fraction off the object.
float previewAspect(const ImVec2& av)
{
	const float W = std::clamp(std::floor(av.x), 32.0f, 4096.0f);
	const float H = std::clamp(std::floor(av.y), 32.0f, 4096.0f);
	return W / H;
}

// The whole assembly, drawn by the renderer from the class's OWN scratch world:
// gray backdrop with a ground plane, a grid and the class's origin marked, and
// every component's mesh where its transform actually puts it. Returns false on
// a backend that has no world-preview path (D3D11/D3D12/Vulkan today) — the
// caller then falls back to the per-asset preview below, which is exactly what
// this panel showed before the hook existed.
bool drawWorldPreview(AppContext& ctx, ClassState& st, entt::registry& reg,
                      const ImVec2& av, const ImVec2& org)
{
	if (!ctx.renderer || !ctx.contentManager || !st.compWorld) return false;

	// The origin is the ROOT's own, not the content's centre and not a hardcoded
	// world zero: it is the point every component transform is measured from, so
	// it is what the backdrop marks and what the view frames on. A class whose
	// root carries an offset — the blob stores it, and the spawner honours it —
	// would otherwise sit against an empty patch of grid, which reads as a
	// broken origin marker rather than a moved root.
	glm::vec3 origin(0.0f);
	if (reg.valid(st.compRoot))
		if (const auto* t = reg.try_get<TransformComponent>(st.compRoot))
			origin = t->position;

	// Open framed on the class rather than at the scene camera's default
	// three-quarter view eight metres out, which for a character is a speck.
	if (!st.previewCamFramed)
	{
		// A studio lens, not the scene's. The scene camera's 60° vertical is a
		// wide angle for a viewport that looks at one character from two metres
		// away: everything away from the centre stretches, and the stretch
		// travels across the model as the camera moves. 45° is the angle asset
		// viewers use, and the framing below is derived from it, so the class
		// still fills the pane.
		st.previewCam.setFovDegrees(45.0f);
		st.previewCam.focusOn(origin + glm::vec3(0.0f, 1.0f, 0.0f), 1.4f);
		st.previewCamFramed = true;
	}

	glm::mat4 viewProj(1.0f);
	void* tex = ctx.renderer->RenderWorldPreview(*ctx.contentManager, *st.compWorld,
		static_cast<uint32_t>(av.x), static_cast<uint32_t>(av.y),
		// No sky: the class viewport is a neutral studio, where a sunset would
		// only be a coloured cast over the thing being authored.
		st.previewCam.makeOverride(), origin, WorldPreviewEnv{}, &viewProj);
	if (!tex) return false;

	const bool flipY = (ctx.backend == HE::RendererBackend::OpenGL);
	ImGui::Image(reinterpret_cast<ImTextureID>(tex), av,
		flipY ? ImVec2(0, 1) : ImVec2(0, 0), flipY ? ImVec2(1, 0) : ImVec2(1, 1));
	// Colliders and camera booms stay an overlay even now: they have no mesh to
	// render, and drawing them THROUGH the character is what you want from a
	// collider outline anyway.
	// Hover is taken from the IMAGE, not from an InvisibleButton laid over it.
	// ImGuizmo only lets a handle be grabbed when no ImGui item is hovered or
	// active (its CanActivate), so a full-size button over the viewport made the
	// gizmo undraggable — visible, and dead to every click. ImGui::Image submits
	// an item without an id, which is exactly why the Scene window never had the
	// problem.
	st.previewHovered  = ImGui::IsItemHovered();
	st.previewViewProj = viewProj;
	drawSubtreeGizmos(reg, st.compRoot, st.compSel, st.compFocus, viewProj, org, av);
	return true;
}

// Move / rotate / scale in the class viewport, the Scene window's gizmo under
// the Scene window's keys. Everything is placed relative to the class's own
// root: the gizmo writes back in PARENT space, and this world's parent chain
// ends at that root.
//
// Drawn after the picture, so it sits on top of it — and after the navigation
// was read, so a fly drag cannot grab a handle.
void drawWorldPreviewGizmo(ClassState& st, const ImVec2& av, const ImVec2& org,
                           bool hovered, bool navigating)
{
	if (!st.compWorld) return;
	EditorTransformGizmo::handleOperationKeys(st.previewGizmo, hovered, navigating);

	// The renderer's own projection rule, called directly. It used to be
	// RECOVERED as `previewViewProj * inverse(view)` — algebraically the
	// projection only while the view that produced previewViewProj is the view
	// being divided out, which stops being true the moment the camera moves and
	// leaves a residual rotation inside something ImGuizmo reads as a
	// projection. Sharing the rule (worldPreviewProjection) gets the same "one
	// definition of the framing" the reconstruction was there for, without a
	// matrix whose meaning depends on when it was built.
	const glm::mat4 view = st.previewCam.viewMatrix();
	const glm::mat4 proj = worldPreviewProjection(st.previewCam.makeOverride(),
	                                              previewAspect(av));
	// The Scene window's predicate, literally: Alt+LMB is orbit here too now, so
	// the manipulator must let go of the button for it.
	bool changed = false;
	EditorTransformGizmo::manipulate(*st.compWorld, st.compSel, view, proj,
	                                 org, ImVec2(org.x + av.x, org.y + av.y),
	                                 st.previewGizmo,
	                                 /*enabled=*/!navigating && !ImGui::GetIO().KeyAlt,
	                                 /*undo=*/nullptr, &changed);
	if (changed) st.dirty = true;
}

// One frame of camera navigation for the class viewport, through the SAME
// gesture grammar as the Scene window — Alt+LMB orbit, MMB pan, RMB fly-look
// with WASDQE/Shift, wheel dolly, the trackpad fly toggle, F to frame the
// selection. Shared code, not a lookalike: two navigation implementations would
// answer the same gesture differently the first time either was fixed.
bool driveWorldPreviewCamera(AppContext& ctx, ClassState& st, entt::registry& reg,
                             const glm::vec3& origin, bool hovered, float viewportH)
{
	EditorCamera::Input cin;
	// Owner = this tab's state, so the editor's per-frame "the scene viewport is
	// not drawn, drop its capture" guard cannot end a look that started here.
	// The pane height is passed in rather than read off the last submitted item:
	// this runs BEFORE the image exists (see drawComponentPreview), and pan
	// speed is measured against the pane, not against whatever widget happened
	// to be drawn last.
	const bool navigating = EditorViewportNav::gather(
		ctx, &st, hovered, ImGui::GetIO().DeltaTime,
		std::max(1.0f, viewportH), cin);

	// F frames the selection, exactly as in the Scene window — the class world's
	// transforms are already propagated by the extractor, so the world matrix is
	// this frame's.
	if (hovered && !ImGui::GetIO().WantTextInput && !navigating &&
	    ImGui::IsKeyPressed(ImGuiKey_F) && reg.valid(st.compSel))
	{
		if (const auto* t = reg.try_get<TransformComponent>(st.compSel))
			st.previewCam.focusOn(glm::vec3(t->worldMatrix[3]),
			                      glm::length(t->scale) * 0.75f + 0.5f);
		else
			st.previewCam.focusOn(origin + glm::vec3(0.0f, 1.0f, 0.0f), 1.4f);
	}

	st.previewCam.update(cin);
	// Deliberately NOT SetEditorCamera: that is the SCENE's override. This
	// camera exists only for the picture this panel asks the renderer for.
	return navigating;
}

// ── Click-to-select ──────────────────────────────────────────────────────────
// Local-space bounds of a mesh asset, cached by UUID — the same two on-disk
// forms the scene's picker reads (a cooked asset carries interleaved vertices
// at stride 8, a loose one the plain position array). Returns nullptr while the
// asset is not loaded, deliberately WITHOUT caching that: a box computed from
// an asset that was not there yet would outlive the load.
const HE::AABB* previewMeshBounds(ContentManager& cm, const HE::UUID& id)
{
	static std::unordered_map<HE::UUID, HE::AABB> s_cache;
	if (id == HE::UUID{}) return nullptr;
	auto it = s_cache.find(id);
	if (it == s_cache.end())
	{
		HE::AABB box;
		if (const StaticMeshAsset* m = cm.getStaticMesh(id))
		{
			if (m->cooked)
				for (uint32_t i = 0; i < m->vertexCount; ++i)
					box.expand({ m->interleaved[i * 8 + 0],
					             m->interleaved[i * 8 + 1],
					             m->interleaved[i * 8 + 2] });
			else
				box = HE::AABB::fromPositions(m->vertices.data(), m->vertices.size() / 3);
		}
		else if (const SkeletalMeshAsset* s = cm.getSkeletalMesh(id))
			box = HE::AABB::fromPositions(s->vertices.data(), s->vertices.size() / 3);
		else
			return nullptr;
		it = s_cache.emplace(id, box).first;
	}
	return it->second.isValid() ? &it->second : nullptr;
}

// What one entity of the class subtree occupies, in ITS OWN space. Everything
// the viewport draws for it counts: its mesh, and the collider outline the
// overlay puts over it — a collider IS the thing you are placing when you place
// a collider, so it has to be clickable even on an entity with no mesh.
HE::AABB previewPickBox(AppContext& ctx, entt::registry& reg, Entity e)
{
	HE::AABB box;
	if (ctx.contentManager)
	{
		if (const auto* m = reg.try_get<MeshComponent>(e))
		{
			if (const HE::AABB* b = previewMeshBounds(*ctx.contentManager, m->meshAssetId))
				box.expand(*b);
			else
			{
				// The fallback cube the renderer draws for a mesh slot with no
				// asset. Clicking what is on screen has to work even when what
				// is on screen is the placeholder.
				box.expand(glm::vec3(-0.5f)); box.expand(glm::vec3(0.5f));
			}
		}
		if (const auto* sm = reg.try_get<SkeletalMeshComponent>(e))
			if (const HE::AABB* b = previewMeshBounds(*ctx.contentManager, sm->meshAssetId))
				box.expand(*b);
	}
	if (const auto* col = reg.try_get<ColliderComponent>(e))
	{
		if (col->shape == ColliderShape::Box)
		{
			const glm::vec3 h = glm::max(col->halfExtents, glm::vec3(0.01f));
			box.expand(-h); box.expand(h);
		}
		else
		{
			// Same convention as the overlay: a capsule's `height` is the total
			// one, hemispheres included.
			const float r  = std::max(0.01f, col->radius);
			const float hy = col->shape == ColliderShape::Capsule
			                 ? std::max(r, col->height * 0.5f) : r;
			box.expand(glm::vec3(-r, -hy, -r)); box.expand(glm::vec3(r, hy, r));
		}
	}
	if (!box.isValid())
	{
		// Everything with nothing to draw — a camera at the end of its boom, a
		// bare child transform — still has to be reachable, or the tree stays
		// the only way to point at it. A small box on its origin, which is what
		// the overlay marks those with anyway.
		constexpr float kMarker = 0.12f;
		box.expand(glm::vec3(-kMarker)); box.expand(glm::vec3(kMarker));
	}
	return box;
}

// A click in the picture selects what is under it. Without this the viewport
// was a picture you could only look at: selecting happened in the tree on the
// left, and a drag on the image landed on the NAVIGATION gestures instead — so
// trying to move a component moved the camera, and the grid sliding past is
// what you saw for it.
//
// Deliberately no InvisibleButton over the image (see drawWorldPreview): an
// ImGui item there would make ImGuizmo refuse every handle. Hover comes off the
// image itself and the click off the global mouse state, gated on that hover.
//
// Runs AFTER the gizmo so ImGuizmo::IsOver() is this frame's answer: a press
// that is grabbing a handle must not also re-select something behind it.
void pickComponentUnderCursor(AppContext& ctx, ClassState& st, entt::registry& reg,
                              const ImVec2& av, const ImVec2& org, bool navigating)
{
	if (!st.compWorld || !st.previewHovered || navigating) return;
	ImGuiIO& io = ImGui::GetIO();
	if (io.KeyAlt) return;                       // Alt+LMB is the orbit gesture
	if (ImGuizmo::IsOver() || ImGuizmo::IsUsing()) return;
	if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;

	std::vector<PreviewPick::Candidate> cands;
	for (auto [e, t] : reg.view<TransformComponent>().each())
	{
		PreviewPick::Candidate c;
		c.id    = static_cast<std::uint32_t>(entt::to_integral(e));
		// The matrix the extractor just propagated for the render, so the boxes
		// sit exactly where the picture put the meshes.
		c.world = t.worldMatrix;
		c.box   = previewPickBox(ctx, reg, e);
		cands.push_back(std::move(c));
	}

	std::uint32_t hit = 0;
	// The renderer's own view-projection for THIS frame's picture — the same
	// matrix the collider overlay is drawn with, so the ray goes where the eye
	// went.
	if (!PreviewPick::pickAtScreen(cands, st.previewViewProj,
	                               glm::vec2(org.x, org.y), glm::vec2(av.x, av.y),
	                               glm::vec2(io.MousePos.x, io.MousePos.y), hit))
		return;   // a click on the backdrop keeps the selection: there is no
		          // "nothing selected" state here, and snapping back to the root
		          // on every stray click would be worse than doing nothing.

	const Entity e = static_cast<Entity>(hit);
	if (!reg.valid(e)) return;
	st.compSel = e;
	// The whole entity, not one of its component rows: the viewport can resolve
	// which THING was clicked, not which of its components — and the whole
	// entity is what the gizmo about to appear moves.
	st.compFocus.clear();
}

void drawComponentPreview(AppContext& ctx, ClassState& st, entt::registry& reg)
{
	ImVec2 av = ImVec2(std::max(64.0f, ImGui::GetContentRegionAvail().x),
	                   std::max(64.0f, ImGui::GetContentRegionAvail().y));
	const ImVec2 org = ImGui::GetCursorScreenPos();

	// ── Camera FIRST, picture second ─────────────────────────────────────────
	// The renderer draws from whatever the camera IS when RenderWorldPreview is
	// called, so reading this frame's gestures afterwards left the picture one
	// frame behind the mouse — and worse, the gizmo and the pick ray, which
	// have to agree with the picture pixel for pixel, were then built from a
	// camera the picture had never seen.
	//
	// Both gates are last frame's answers, which is the only kind ImGui has:
	// IsItemHovered is resolved against the previous frame's layout anyway, and
	// WHICH of the two paths draws is a property of the backend, so it cannot
	// change between frames.
	bool navigating = false;
	if (st.previewWorldPath)
	{
		glm::vec3 camOrigin(0.0f);
		if (reg.valid(st.compRoot))
			if (const auto* t = reg.try_get<TransformComponent>(st.compRoot))
				camOrigin = t->position;
		navigating = driveWorldPreviewCamera(ctx, st, reg, camOrigin, st.previewHovered, av.y);
	}

	// The whole class, out of its own world. Everything below is the fallback
	// for the backends that have no world-preview path yet.
	const bool drewWorld = drawWorldPreview(ctx, st, reg, av, org);
	st.previewWorldPath = drewWorld;

	// The MESH is the root's, not the selection's. The renderer draws it at
	// identity, so root space is the space of the picture — and that is the only
	// space in which the subtree's colliders and booms line up with it. Picking
	// a child's mesh instead would draw it at the origin while its gizmo sat at
	// its real offset, which reads as everything being in the wrong place.
	const Entity meshOwner = st.compRoot;
	HE::UUID skeletalId{}, staticId{};
	if (!drewWorld && reg.valid(meshOwner))
	{
		if (const auto* sm = reg.try_get<SkeletalMeshComponent>(meshOwner)) skeletalId = sm->meshAssetId;
		if (const auto* m  = reg.try_get<MeshComponent>(meshOwner))         staticId   = m->meshAssetId;
	}

	if (!drewWorld && skeletalId == HE::UUID{} && staticId == HE::UUID{})
	{
		ImGui::TextDisabled("%s",
			"No mesh on this class yet.\n"
			"Assign one to its root and it shows up here,\n"
			"with the colliders and camera boom over it.");
		return;
	}

	// The material path renders SQUARE (one `size`), the skeletal one takes a
	// width and a height. Squaring the pane for both keeps a mesh from
	// stretching when the column is dragged narrow.
	const float side = std::min(av.x, av.y);
	if (!drewWorld && skeletalId == HE::UUID{}) av = ImVec2(side, side);

	void* tex = nullptr;
	glm::mat4 viewProj(1.0f);
	bool haveViewProj = false;
	if (!drewWorld && ctx.renderer && ctx.contentManager)
	{
		if (skeletalId != HE::UUID{})
		{
			// Empty bone matrices = the bind pose, which is what a class editor
			// should show: the character as authored, not mid-animation.
			static const std::vector<glm::mat4> kBindPose;
			tex = ctx.renderer->RenderSkeletalPreview(*ctx.contentManager, skeletalId, kBindPose,
				static_cast<uint32_t>(av.x), static_cast<uint32_t>(av.y),
				st.previewYaw, st.previewPitch, st.previewDist, /*showSkeleton=*/false,
				&viewProj);
			haveViewProj = (tex != nullptr);
		}
		else
		{
			// A static mesh goes through the MATERIAL preview with a mesh
			// override — that path already frames an arbitrary mesh to its
			// bounds, which is the whole job here.
			HE::UUID materialId{};
			if (const auto* mat = reg.try_get<MaterialComponent>(st.compSel))
				materialId = mat->materialAssetId;
			if (materialId == HE::UUID{}) materialId = HE::kDefaultMaterialId;
			tex = ctx.renderer->RenderMaterialPreview(*ctx.contentManager, materialId,
				static_cast<uint32_t>(side), st.previewYaw, st.previewPitch, st.previewDist,
				/*shape=*/0, staticId);
		}
	}

	if (!drewWorld)
	{
		if (tex)
		{
			const bool flipY = (ctx.backend == HE::RendererBackend::OpenGL);
			ImGui::Image(reinterpret_cast<ImTextureID>(tex), av,
				flipY ? ImVec2(0, 1) : ImVec2(0, 0), flipY ? ImVec2(1, 0) : ImVec2(1, 1));
			// The rest of the class, over the mesh: what it collides with and where
			// its camera sits. Only when the renderer reported its framing — drawing
			// gizmos against a guessed matrix would be worse than drawing none.
			if (haveViewProj)
				drawSubtreeGizmos(reg, st.compRoot, st.compSel, st.compFocus, viewProj, org, av);
		}
		else
			ImGui::TextDisabled("(preview unavailable on this backend)");
	}

	if (drewWorld)
	{
		// The real thing: identical controls to the Scene window. Deliberately
		// NO invisible button over the image — see drawWorldPreview. The
		// navigation for this frame was already read above.
		drawWorldPreviewGizmo(st, av, org, st.previewHovered, navigating);
		// After the gizmo, so a press that is grabbing a handle is not also a
		// pick — ImGuizmo::IsOver() is current only once Manipulate has run.
		pickComponentUnderCursor(ctx, st, reg, av, org, navigating);
		return;
	}

	// An item over the image so the fallback's drag has an owner. (Only in the
	// fallback: over the world preview it would block the gizmo.)
	ImGui::SetCursorScreenPos(org);
	ImGui::InvisibleButton("##hccompOrbit", ImVec2(std::max(av.x, 1.0f), std::max(av.y, 1.0f)),
		ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
	ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);

	// Fallback path only (no world preview on this backend): the per-asset
	// preview frames ONE asset on its own bounds, so it orbits — there is
	// nothing to fly through.
	if (ImGui::IsItemActive() &&
	    (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Right)))
	{
		const ImVec2 md = ImGui::GetIO().MouseDelta;
		st.previewYaw   -= md.x * 0.01f;
		st.previewPitch  = std::clamp(st.previewPitch + md.y * 0.01f, -1.45f, 1.45f);
	}
	if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f)
		st.previewDist = std::clamp(st.previewDist * (1.0f - ImGui::GetIO().MouseWheel * 0.1f),
		                            0.5f, 30.0f);
}

void drawComponentsBody(AppContext& ctx, ClassState& st)
{
	ensureComponentWorld(st, {});
	if (!st.compWorld) return;
	auto& reg = st.compWorld->registry();
	if (!reg.valid(st.compSel)) st.compSel = st.compRoot;
	// A focused component that is no longer there — removed from the tree or
	// from the Details header — would leave the details column blank with no
	// hint why. Fall back to the whole entity instead.
	if (!st.compFocus.empty() && reg.valid(st.compSel))
	{
		std::vector<std::string> have;
		InspectorPanel::listComponents(ctx, *st.compWorld, st.compSel, have);
		if (std::find(have.begin(), have.end(), st.compFocus) == have.end())
			st.compFocus.clear();
	}

	const float listW = 220.0f;
	ImGui::BeginChild("##hccomp_tree", ImVec2(listW, 0), true);
	ImGui::TextDisabled("Components");
	ImGui::Separator();
	// A real tree, not the flat list this used to be. The class HAS a hierarchy
	// — a PlayerCharacter carries a Camera child — and listing parent and child
	// side by side said the opposite of what the subtree actually is.
	//
	// Under each entity hang its COMPONENTS, one row apiece, exactly as Unreal's
	// Blueprint components panel does it: everything the class is made of is
	// visible at once and each piece is selectable on its own, instead of the
	// entity being the smallest thing you can point at.
	{
		// Recursive lambda via an explicit self-parameter: the walk needs to
		// call itself and a capturing lambda cannot name its own type.
		const auto drawNode = [&](auto&& self, Entity e) -> void
		{
			if (!reg.valid(e)) return;
			const auto* nameC = reg.try_get<NameComponent>(e);
			const std::string label = nameC ? nameC->name : std::string("Entity");
			// Copied up front: the popup below can add a component, and the
			// recursion can add or remove a child — either way, iterating the
			// live vector afterwards would be reading a moved-from thing.
			const auto* hier = reg.try_get<HierarchyComponent>(e);
			const std::vector<Entity> kids = hier ? hier->children : std::vector<Entity>{};
			const bool hasKids = !kids.empty();

			// The component labels come from the Details panel itself, so a
			// component added there shows up here without anyone maintaining a
			// second list.
			std::vector<std::string> comps;
			InspectorPanel::listComponents(ctx, *st.compWorld, e, comps);

			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
			                           ImGuiTreeNodeFlags_SpanAvailWidth |
			                           ImGuiTreeNodeFlags_DefaultOpen;
			if (!hasKids && comps.empty())
				flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
			if (e == st.compSel && st.compFocus.empty()) flags |= ImGuiTreeNodeFlags_Selected;

			ImGui::PushID((int)entt::to_integral(e));
			const bool open = ImGui::TreeNodeEx("##n", flags, "%s%s",
			                                    label.c_str(), e == st.compRoot ? "  (root)" : "");
			if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
			{
				st.compSel = e;
				st.compFocus.clear();
			}
			// New components are attached HERE, on the thing they attach to —
			// the same menu the Details panel offers, not a copy of it.
			if (ImGui::BeginPopupContextItem("##addcomp"))
			{
				ImGui::TextDisabled("Add Component");
				ImGui::Separator();
				// The popup is its own window, so the details column's
				// "was anything active in here" heuristic never sees this —
				// the menu says so itself instead.
				if (InspectorPanel::addComponentMenu(*st.compWorld, e, nullptr))
					st.dirty = true;
				ImGui::EndPopup();
			}
			if (open && (hasKids || !comps.empty()))
			{
				for (const std::string& c : comps)
				{
					ImGuiTreeNodeFlags cf = ImGuiTreeNodeFlags_Leaf |
					                        ImGuiTreeNodeFlags_NoTreePushOnOpen |
					                        ImGuiTreeNodeFlags_SpanAvailWidth;
					if (e == st.compSel && st.compFocus == c) cf |= ImGuiTreeNodeFlags_Selected;
					ImGui::TreeNodeEx(c.c_str(), cf, "%s", c.c_str());
					if (ImGui::IsItemClicked()) { st.compSel = e; st.compFocus = c; }
					if (ImGui::BeginPopupContextItem(c.c_str()))
					{
						if (ImGui::MenuItem("Remove Component"))
						{
							InspectorPanel::removeComponent(ctx, *st.compWorld, e, c.c_str());
							if (st.compFocus == c) st.compFocus.clear();
							st.dirty = true;
						}
						ImGui::EndPopup();
					}
				}
				for (Entity c : kids) self(self, c);
				ImGui::TreePop();
			}
			ImGui::PopID();
		};
		drawNode(drawNode, st.compRoot);
	}
	ImGui::Separator();
	if (ImGui::SmallButton("Add Child"))
	{
		// Under the SELECTION, not always under the root: a tree you can only
		// grow one level deep is a list with indentation.
		const Entity parent = reg.valid(st.compSel) ? st.compSel : st.compRoot;
		const Entity child = st.compWorld->createEntity("Child");
		st.compWorld->addComponent(child, TransformComponent{});
		st.compWorld->reparentEntity(child, parent);
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

	// ── Middle: what the components add up to ────────────────────────────────
	// The reason this half is called Viewport. Today it draws the SELECTED
	// component's asset — a skeletal mesh in its bind pose, a static mesh —
	// through the per-asset preview paths the renderer already has.
	//
	// It is deliberately not yet the whole assembly. Mesh AND collider AND
	// camera arm in one image needs a renderer entry point that extracts an
	// arbitrary world into its own target, which is per-backend work; this
	// shows something useful without any of it. The gap is worth naming rather
	// than papering over: what you see is one component, not the character.
	ImGui::SameLine();
	{
		const float detailsW = 320.0f;
		const float previewW = std::max(160.0f, ImGui::GetContentRegionAvail().x - detailsW);
		// NoScrollWithMouse: the wheel is the camera's dolly here, and without
		// this the child eats it before the viewport sees it. (The fallback path
		// used to claim it with SetItemKeyOwner on its invisible button; the
		// world preview has no such button, on purpose.)
		ImGui::BeginChild("##hccomp_preview", ImVec2(previewW, 0), true,
			ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
		drawComponentPreview(ctx, st, reg);
		ImGui::EndChild();
		ImGui::SameLine();
	}

	ImGui::BeginChild("##hccomp_details", ImVec2(0, 0), true);
	if (reg.valid(st.compSel))
	{
		// One component when the tree has one selected, the whole entity
		// otherwise — the same panel either way, just narrowed. It reports
		// added/removed components itself; the heuristic below cannot see
		// those (a popup is its own window, a removal leaves no active item).
		if (InspectorPanel::renderFor(ctx, *st.compWorld, st.compSel, nullptr,
		                              st.compFocus.empty() ? nullptr : st.compFocus.c_str()))
			st.dirty = true;
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
		if (bar.item("##hcmodeviewport", nullptr, "Viewport", st.showViewport, true,
		             "The class's body — its components, and what they add up to"))
			st.showViewport = true;
		if (bar.item("##hcmodecode", nullptr, "Code", !st.showViewport, true,
		             "The class's logic"))
			st.showViewport = false;
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
	if (st.showViewport)
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
