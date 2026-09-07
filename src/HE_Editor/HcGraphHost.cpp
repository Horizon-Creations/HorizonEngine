#include "HcGraphHost.h"
#include <Types/TypeRegistry.h>
#include "HcEditorUtil.h"        // HcEditorUtil: colors, tooltips, engine-API menu
#include "HcRename.h"            // which class a Ref points at — one rule for menus and renames
#include "HcGraphClipboard.h"    // shared HorizonCode node clipboard (copy/cut/paste)
#include "EditorHelp.h"          // Help::Scope — the shared node rows key their help here
#include "EditorWidgets.h"       // dangerMenuItem for node deletion
#include "EditorTheme.h"         // the hover tooltip's dim tier
#include "EditorSettingsPanel.h" // which of the two variable-list looks the user picked
#include "DocsPanel.h"           // F1 over a node opens its entry in the manual
#include "HcGraphShortcuts.h"    // the "hold a key, click" node bindings
#include <HorizonScene/EngineApi.h>
#include <HorizonCode/HcClassResolve.h>   // member menus read the FLATTENED class
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <misc/cpp/imgui_stdlib.h>
#include <imgui_internal.h>      // CurrentItemFlags — detect a BeginDisabled scope
#include <algorithm>
#include <cctype>
#include <cstring>       // strcmp — category names are const char*, compared by value
#include <filesystem>
#include <functional>

namespace HcGraphHost
{
namespace
{
using PT = HC::PinType;
using NT = HC::NodeType;

bool listed(const std::vector<NT>& v, NT t)
{
	return std::find(v.begin(), v.end(), t) != v.end();
}

// HorizonCode's container kind → the canvas's own. Spelled out rather than cast:
// the two enums agree today by coincidence of ordering, and the canvas is shared
// with graphs that have never heard of HorizonCode, so nothing keeps them lined
// up. A new kind here is a compile-time gap, not a silently wrong glyph.
GraphEditor::Container containerPin(HC::ContainerKind k)
{
	switch (k)
	{
		case HC::ContainerKind::Array: return GraphEditor::Container::Array;
		case HC::ContainerKind::Set:   return GraphEditor::Container::Set;
		case HC::ContainerKind::Map:   return GraphEditor::Container::Map;
		case HC::ContainerKind::None:  break;
	}
	return GraphEditor::Container::None;
}

// Does the search query hit the words people actually TYPE for this node, as
// opposed to what it is called? A node's display name is its serialisation key
// (fromJson resolves the type by comparing against nodeDisplayName), so a node
// can never be renamed to what someone looks for — "Map Set" is what a search
// for "map add" means. The registry keeps that vocabulary beside the name.
bool aliasHit(NT t, const std::string& lowerQuery)
{
	if (lowerQuery.empty()) return false;   // an empty query already matches everything
	const char* a = HC::nodeSearchAliases(t);
	return a && *a && lower(a).find(lowerQuery) != std::string::npos;
}

// What kind of pin a wire is being dragged off: exec vs data, and for a data pin
// its value type + array-ness. Both the drag-off menu and the quick-spawn keys
// need it to find a compatible pin on the node they are about to create.
// typeName: for an Enum/Struct pin, WHICH definition it is bound to. A node
// spawned off such a pin has to inherit it, or it is born without a definition —
// no field/entry pins, and every panel that resolves it says "definition
// missing" (the add menu already gets this right by construction).
struct DragPin { bool isExec = true; PT type = PT::Float; bool array = false;
                 // Which container the dragged pin carries, RESOLVED — the menu
                 // matches on the kind because Graph::connect does (an array
                 // does not join a set).
                 HC::ContainerKind ctr = HC::ContainerKind::None;
                 std::string typeName; };

DragPin classifyDragPin(const HC::Node& sn, int srcPin)
{
	const HC::NodeSig sig = HC::signatureOf(sn);
	const PinRanges   rr  = pinRanges(sn);
	DragPin d;
	d.isExec = srcPin < rr.dataIn0;
	if (d.isExec) return d;
	auto take = [&d](const HC::PinDesc& pd)
	{ d.type = pd.type; d.array = pd.isArray; d.ctr = pd.kind();
	  if (pd.typeName) d.typeName = pd.typeName; };
	if (srcPin >= rr.dataOut0 && srcPin - rr.dataOut0 < (int)sig.dataOuts.size())
		take(sig.dataOuts[srcPin - rr.dataOut0]);
	else if (srcPin - rr.dataIn0 < (int)sig.dataIns.size())
		take(sig.dataIns[srcPin - rr.dataIn0]);
	return d;
}

// Seed a freshly spawned node from the pin it was dragged off and re-mirror it.
// Returns the pin to wire, recomputed on the REAL node: mirroring can add pins
// (Make Struct grows one data-in per field), which shifts the index a probe of
// an empty node reported.
int seedSpawnedNode(HC::Graph& graph, int id, const DragPin& dp, bool srcInput)
{
	HC::Node* nn = graph.findNode(id);
	if (!nn) return -1;
	if (!dp.isExec)
	{
		nn->propType = dp.type;      // keep the matched signature
		if (nn->typeName.empty() && (dp.type == PT::Enum || dp.type == PT::Struct))
			nn->typeName = dp.typeName;
	}
	HC::syncTypeSignatures(graph);   // user-type nodes gain their field/entry pins
	nn = graph.findNode(id);
	return nn ? HcEditorUtil::dragMatchPinOn(*nn, dp.type, dp.ctr, srcInput, dp.isExec) : -1;
}

// 'B' → ImGuiKey_B. The letter keys are one contiguous run in ImGui's enum, so
// the shortcut table can stay plain ASCII (and ImGui-free, and unit-testable).
ImGuiKey letterKey(char c)
{
	static_assert(ImGuiKey_Z == ImGuiKey_A + 25, "ImGuiKey letters are not contiguous");
	return (ImGuiKey)(ImGuiKey_A + (c - 'A'));
}

// ── Quick-pick popup (G / Shift+G / E) ──────────────────────────────────────
// Those three keys do not stand for a fixed node: they open a small searchable
// list right at the cursor (this graph's variables, or the engine API) because
// the node they create is only useful once it is bound to something. The key
// handler runs inside GraphEditor::draw, so it just records the request and
// opens the popup; handleGraphKeys draws it afterwards in the same window.
struct QuickPick
{
	enum Kind { None = 0, VarGet, VarSet, EngineApi };
	Kind   kind = None;
	ImVec2 pos;      // graph-space drop point
};
QuickPick s_pick;
constexpr const char* kQuickPickPopup = "##hc_quickpick";

// A palette row that also advertises its keyboard shortcut, right-aligned and
// dimmed ("Branch          B") — the shortcuts are only worth having if people
// meet them where they already look for the node. `hovered` reports the hover
// state of the ROW (checking IsItemHovered at the call site would ask about the
// hint text instead, which sits on the same line).
bool menuItemWithHint(const std::string& label, const char* hint, bool* hovered = nullptr)
{
	const float x = ImGui::GetCursorPosX();
	const float w = ImGui::GetContentRegionAvail().x;
	const bool picked = HcEditorUtil::searchMenuItem(label);
	if (hovered) *hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
	if (hint && *hint)
	{
		ImGui::SameLine(x + w - ImGui::CalcTextSize(hint).x);
		ImGui::TextDisabled("%s", hint);
	}
	return picked;
}

// Make `id` the selection on both sides — the canvas draws it selected, the
// frontend's details panel follows.
void selectNode(const Host& h, int id)
{
	h.ge->selected  = id;
	h.ge->selection = { id };
	if (h.selectedNode) *h.selectedNode = id;
}
} // namespace

// ── Node plumbing ────────────────────────────────────────────────────────────

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
		case PT::Enum:   return "Enum";
		case PT::Struct: return "Struct";
		default:         return "Exec";
	}
}

std::vector<GraphEditor::Pin> nodePins(const HC::Node& n)
{
	std::vector<GraphEditor::Pin> out;
	const HC::NodeSig s = HC::signatureOf(n);
	int id = 0;
	auto push = [&](const HC::PinDesc& pd, bool input, bool isExec)
	{
		GraphEditor::Pin p;
		p.id = id++; p.label = pd.name ? pd.name : "";
		p.color = HcEditorUtil::pinTypeColor(pd.type); p.input = input; p.isExec = isExec;
		// The canvas draws one glyph per container KIND, so hand it the resolved
		// kind — pd.kind(), not pd.container, or the legacy "isArray without a
		// kind" rows that every pre-Set/Map graph on disk carries would come out
		// as scalars. isArray stays set beside it: it is the flag the material /
		// particle / animator hosts still speak, and Pin resolves the pair the
		// same way HorizonCode::containerKindOf does.
		const HC::ContainerKind ck = pd.kind();
		p.isArray   = ck != HC::ContainerKind::None;
		p.container = containerPin(ck);
		// A map pin wears BOTH its types: the key color on the left column. Without
		// it Map<String,Int> and Map<Int,Int> are the same glyph, and the key type
		// is readable only in the details panel.
		if (ck == HC::ContainerKind::Map)
			p.keyColor = HcEditorUtil::pinTypeColor(pd.keyType);
		out.push_back(std::move(p));
	};
	for (const auto& pd : s.execIns)  push(pd, true,  true);
	for (const auto& pd : s.execOuts) push(pd, false, true);
	for (const auto& pd : s.dataIns)  push(pd, true,  false);
	for (const auto& pd : s.dataOuts) push(pd, false, false);
	return out;
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

namespace
{
// ── What the menus may offer ─────────────────────────────────────────────────
// A class that derives from another class reads and writes its ancestors'
// variables and calls their public functions under the same names — the runtime
// resolves both across the instance's graph levels. These two put the inherited
// declarations behind the same iteration the menus already do, so every place
// that offers "this graph's variables" offers them without learning what
// inheritance is.
//
// Variables come back as POINTERS into the graph and functions as COPIES, which
// is not an inconsistency: the menu bodies call addNode, and addNode appends to
// `nodes`. A pointer into `variables` survives that; a pointer into `nodes` is
// dangling the moment the vector grows — and the loop reads it again, both to
// mirror the call's pins and to label the remaining entries.
std::vector<const HC::Variable*> offerableVariables(const HC::Graph& g)
{
	std::vector<const HC::Variable*> out;
	out.reserve(g.variables.size() + g.inherited.size());
	for (const auto& v : g.variables) out.push_back(&v);
	for (const auto& v : g.inherited)
	{
		// Private stays private: an ancestor's private variable reserves its
		// NAME here (one store per instance) but is not readable from a derived
		// class, so offering it would offer something that does not work.
		if (v.access != 0) continue;
		// A local declaration of the same name wins — it cannot happen in a class
		// authored since names became unique across the chain, but an older asset
		// may carry one, and showing the variable twice would be worse than
		// showing the nearer one.
		if (g.findVariable(v.name)) continue;
		out.push_back(&v);
	}
	return out;
}

std::vector<HC::Node> offerableFunctions(const HC::Graph& g)
{
	std::vector<HC::Node> out;
	for (const auto& n : g.nodes)
		if (n.type == NT::FunctionEntry && !n.s.empty()) out.push_back(n);
	for (const auto& n : g.inheritedFns)
	{
		bool own = false;
		for (const auto& e : g.nodes)
			if (e.type == NT::FunctionEntry && e.s == n.s) { own = true; break; }
		if (!own) out.push_back(n);   // an override IS the local entry
	}
	return out;
}

// Point a fresh Call node at `fn` and give it the matching pins.
// syncFunctionSignatures mirrors from a FunctionEntry IN THIS GRAPH, and an
// inherited function has none — so the prototype's signature is copied here, and
// sync deliberately leaves such a call alone afterwards ("calls whose function
// lives in another graph keep their own mirror").
void bindCallTo(HC::Graph& g, int callId, const HC::Node& fn)
{
	HC::Node* nn = g.findNode(callId);
	if (!nn) return;
	nn->s       = fn.s;
	nn->params  = fn.params;
	nn->results = fn.results;
	HC::syncFunctionSignatures(g);
}
} // namespace

std::string uniqueVarName(const HC::Graph& g)
{
	for (int i = 1; i < 1000; ++i)
	{
		const std::string name = i == 1 ? "NewVar" : ("NewVar" + std::to_string(i));
		// Inherited names count as taken: one store per instance, so a second
		// declaration of the same name is the base's variable, not a new one.
		// (Graphs that inherit nothing carry an empty list, so this is the old
		// answer everywhere else.)
		if (!g.findVariableOrInherited(name)) return name;
	}
	return "NewVar";
}

int addNode(HC::Graph& g, NT type, const ImVec2& pos, int subgraph)
{
	HC::Node n;
	n.type = type;
	n.x = pos.x; n.y = pos.y;
	n.subgraph = subgraph;    // new nodes belong to the visible sub-graph
	if (type == NT::ConstColor) { n.f[0] = n.f[1] = n.f[2] = n.f[3] = 1.0f; }
	if (type == NT::FunctionEntry) n.s = uniqueFunctionName(g);
	return g.addNode(std::move(n));
}

std::string defaultNodeTitle(const HC::Node& n)
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
		// Which widget / which class, in the header. Both store an asset path in
		// `s`, and without this a graph full of them says only "Create Widget"
		// five times over.
		case NT::CreateWidget:
		case NT::CreateObject: return HcEditorUtil::assetNodeTitle(base, n.s);
		// The action, not the event names it answers to — those are the wire's
		// business. "(Axis)" because the pin layout differs and the reason for
		// it should be readable off the node.
		case NT::InputAction:  return n.s.empty() ? std::string(base)
		                                          : (n.s + (!n.hasArg ? ""
		                                             : n.propType == HC::PinType::Vec2 ? " (Axis 2D)"
		                                                                               : " (Axis)"));
		default:               return base;
	}
}

std::string lower(std::string v)
{
	std::transform(v.begin(), v.end(), v.begin(),
		[](unsigned char c){ return (char)std::tolower(c); });
	return v;
}

// An Object variable is named by the class it holds, not by a bare "Object" —
// and an Enum/Struct variable by its definition asset — otherwise every one of
// them looks the same in a list. They are different fields and never both set.
HcEditorUtil::VariableRowDesc variableRowDesc(const HC::Variable& v, const char* note)
{
	HcEditorUtil::VariableRowDesc d;
	d.name        = v.name.c_str();
	d.type        = v.type;
	d.isArray     = v.isArray;
	d.container   = v.container;
	d.keyType     = v.keyType;
	d.typeName    = v.typeName;
	d.keyTypeName = v.keyTypeName;
	d.className   = v.className;
	d.note        = note;
	return d;
}

std::string variableTypeLabel(const HC::Variable& v)
{
	return HcEditorUtil::typeLabel(v.type, v.isArray, v.container, v.keyType,
	                               v.className.empty() ? v.typeName : v.className,
	                               v.keyTypeName);
}

HcEditorUtil::VariableRowStyle variableRowStyle()
{
	// Converted in ONE place: the two variable lists must not be able to disagree
	// about what the user picked, and HcEditorUtil stays free of the settings
	// header.
	return EditorSettingsPanel::hcVariableStyle() == EditorSettingsPanel::HcVariableStyle::Compact
		? HcEditorUtil::VariableRowStyle::Compact
		: HcEditorUtil::VariableRowStyle::Detailed;
}

bool variableRow(const HC::Variable& v, bool selected, HcEditorUtil::VariableRowStyle style,
                 const char* note)
{
	return HcEditorUtil::variableRow(variableRowDesc(v, note), selected, style);
}

bool loadClassGraph(ContentManager* content, const std::string& path, HC::Graph& out)
{
	if (!content || path.empty()) return false;
	const HE::UUID id = content->loadAsset(path);
	// The FLATTENED graph, not the asset's own: this is what member menus are
	// built from, and a class's members include everything it inherits. Reading
	// the raw graphJson here is why a Cast to a derived class showed an empty
	// member list — the child usually adds little of its own, and everything it
	// got from its parent was simply not in the graph being read.
	if (content->assetType(id) == HE::AssetType::HorizonCodeClass)
	{
		HC::ResolvedClass rc = HC::resolveClassAsset(*content, path);
		if (!rc.ok) return false;
		out = std::move(rc.graph);
		return true;
	}
	if (const UIWidgetAsset* w = content->getWidget(id); w && !w->graphJson.empty())
		return HC::fromJson(w->graphJson, out);
	return false;
}

const HC::Graph* resolveClassGraph(const HC::Node& srcNode, const HC::Graph& selfGraph,
                                   const HC::Graph* giGraph, ContentManager* content,
                                   HC::Graph& scratch)
{
	// These two are the graphs already in hand, not something to load.
	if (srcNode.type == NT::GetSelf)         return &selfGraph;
	if (srcNode.type == NT::GetGameInstance) return giGraph;

	// Everything else names an asset, and WHICH asset is decided in exactly one
	// place — the same one the cross-asset rename asks, so a menu can never offer
	// members of a class the rename would then fail to find. An engine class (a
	// Cast to PlayerCharacter, say) resolves to nothing on purpose: the taxonomy
	// rows have no graph, their members come from the table instead.
	const std::string key = HcRename::classOfRefSource(selfGraph, srcNode, {}, {});
	return (!key.empty() && loadClassGraph(content, key, scratch)) ? &scratch : nullptr;
}

namespace
{
// The ENGINE base a class asset lands on, "" when unknown. Resolved through the
// chain rather than read off the asset: with class inheritance `baseClass` may
// name another class, and engineClassMembers would then find nothing — which is
// how a Cast to a derived class lost its inherited members.
std::string assetBaseClass(ContentManager* content, const std::string& path)
{
	if (!content || path.empty()) return {};
	return HC::resolveClassAsset(*content, path).engineBase;
}
}

std::string resolveClassBase(const HC::Node& srcNode, const HC::Graph& selfGraph,
                             const std::string& selfBaseClass, ContentManager* content)
{
	switch (srcNode.type)
	{
		case NT::GetSelf: return selfBaseClass;
		case NT::GetVariable:
		case NT::SetVariable:   // the set node passes the value through
		{
			const HC::Variable* v = selfGraph.findVariable(srcNode.s);
			return (v && v->type == PT::Ref) ? assetBaseClass(content, v->className) : std::string();
		}
		case NT::Cast:
			// A cast to an engine class IS the base; a cast to an asset resolves
			// through that asset.
			if (HC::findEngineClass(srcNode.s) && !srcNode.s.empty()) return srcNode.s;
			return assetBaseClass(content, srcNode.s);
		case NT::CreateObject: return assetBaseClass(content, srcNode.s);
		case NT::ForEach:
			return srcNode.propType == PT::Ref ? assetBaseClass(content, srcNode.s) : std::string();
		default: return {};
	}
}

// ── Canvas model ─────────────────────────────────────────────────────────────

namespace
{
// One quick-spawn: drop `type` at the cursor and, when the shortcut ended a link
// drag, wire it to that pin — the same match-and-connect the drag-off menu does,
// minus the menu. A pin with nothing compatible on the new node (an exec wire
// into a pure data node, say) still leaves the node there, unwired: the key did
// something visible, and one Cmd+Z takes it back.
int quickSpawnNode(const Host& h, NT type, const GraphEditor::QuickSpawnCtx& c)
{
	HC::Graph& graph = *h.graph;
	const int id = addNode(graph, type, c.pos, h.currentGraph);
	HC::Node* nn = graph.findNode(id);
	if (!nn || c.linkNode == 0) return id;

	const HC::Node* sn = graph.findNode(c.linkNode);
	if (!sn) return id;
	const DragPin dp  = classifyDragPin(*sn, c.linkPin);
	// Seed + mirror BEFORE matching: the pin index is only meaningful once the
	// node has the pins its definition gives it.
	const int     pin = seedSpawnedNode(graph, id, dp, c.linkInput);
	if (pin < 0) return id;

	// adoptForEachElementType first: a ForEach on either end takes the array's
	// element type before the typed connect (as in drawPinDragMenu).
	if (c.linkInput)
	{
		HC::adoptForEachElementType(graph, id, pin, c.linkNode, c.linkPin);
		graph.connect(id, pin, c.linkNode, c.linkPin);
	}
	else
	{
		HC::adoptForEachElementType(graph, c.linkNode, c.linkPin, id, pin);
		graph.connect(c.linkNode, c.linkPin, id, pin);
	}
	HC::inferUserTypeNames(graph);   // the new node learns its definition from the wire
	return id;
}
} // namespace

GraphEditor::Model buildModel(const Host& h)
{
	HC::Graph& graph = *h.graph;

	GraphEditor::Model m;
	m.multiSelect = true;      // shift-click / box-select; drag + Delete act on all
	m.compactPureNodes = true; // getters/literals draw as compact chips
	// The last compile check's error node gets a red halo.
	m.nodeOutline = [&h](int id) -> ImU32
	{
		return (h.errorNode != 0 && id == h.errorNode) ? IM_COL32(230, 70, 70, 255) : 0;
	};
	m.nodeIds = [&h, &graph]{ std::vector<int> ids; ids.reserve(graph.nodes.size());
		for (const auto& n : graph.nodes) if (n.subgraph == h.currentGraph) ids.push_back(n.id); return ids; };
	m.getPos = [&graph](int id, float& x, float& y){ if (const HC::Node* n = graph.findNode(id)) { x = n->x; y = n->y; } };
	m.setPos = [&graph](int id, float x, float y){ if (HC::Node* n = graph.findNode(id)) { n->x = x; n->y = y; } };
	m.title  = [&h, &graph](int id){ const HC::Node* n = graph.findNode(id);
		return n ? h.title(*n) : std::string(); };
	m.headerColor = [&graph](int id){ const HC::Node* n = graph.findNode(id);
		return n ? HcEditorUtil::nodeHeaderColor(*n) : GraphEditor::categoryColor(""); };
	m.pins = [&graph](int id){ const HC::Node* n = graph.findNode(id);
		return n ? nodePins(*n) : std::vector<GraphEditor::Pin>{}; };
	m.links = [&h, &graph]{ std::vector<std::array<int,4>> ls; ls.reserve(graph.links.size());
		for (const auto& l : graph.links) { const HC::Node* s = graph.findNode(l.srcNode);
			if (s && s->subgraph == h.currentGraph) ls.push_back({ l.srcNode, l.srcPin, l.dstNode, l.dstPin }); }
		return ls; };
	m.connect = [&h, &graph](int oN, int oP, int iN, int iP){
		// ForEach is generic until wired: adopt the source array's element type
		// (Array/Element pins retype + recolor) before the typed connect.
		HC::adoptForEachElementType(graph, oN, oP, iN, iP);
		// A wire the types refused is not always a dead end: when ONE node would
		// carry it (Float into a String pin → To String) that node is built and
		// wired here, half-way down the wire. It also re-infers user types, so
		// this path needs no inferUserTypeNames of its own.
		// The frontend's hidden types are handed over: a conversion must not be a
		// way around a node the palette deliberately does not offer.
		if (!graph.connect(oN, oP, iN, iP))
			return HC::connectWithConversion(graph, oN, oP, iN, iP,
				h.menus ? h.menus->addExcluded : std::vector<HC::NodeType>{});
		// A user-defined type node that had no definition can learn it from what
		// it was just wired to — an "Enum to String" hanging off a Mood output
		// IS a Mood one, and asking the panel to pick would be asking twice.
		HC::inferUserTypeNames(graph);
		return true; };
	m.clearPinLinks = [&graph](int node, int pin, bool){ removePinLinks(graph, node, pin); };
	m.removeNode = [&graph](int id){ graph.removeNode(id); };
	// Literal nodes edit their value inline on the node body.
	m.nodeBodyHeight = [&graph](int id){ const HC::Node* n = graph.findNode(id);
		return n ? HcEditorUtil::literalNodeBodyHeight(*n) : 0.0f; };
	m.drawNodeBody = [&h, &graph](int id, ImVec2, ImVec2, float){
		HC::Node* n = graph.findNode(id); if (!n) return;
		bool committed = false;
		if (HcEditorUtil::drawLiteralNodeBody(*n, committed)) h.onEdit(false);
		if (committed) h.onEdit(true); };
	// Unwired simple inputs (Bool/Int/Float/String) edit their default right on
	// the pin — no literal node needed for a constant.
	m.pinHasInlineEditor = [&graph](int nid, int pin){
		const HC::Node* n = graph.findNode(nid);
		return n && HcEditorUtil::pinSupportsInlineDefault(*n, pin); };
	m.pinInlineEditorWidth = [&graph](int nid, int pin){
		const HC::Node* n = graph.findNode(nid);
		return n ? HcEditorUtil::pinInlineEditorWidth(*n, pin) : 0.0f; };
	// …and a string pin whose values are a LIST edits it as a dropdown instead.
	// Two answers, in this order: the host knows about the asset being edited
	// (which animations this widget carries, what its elements are called), the
	// shared half knows about the engine and the project (curves, directions,
	// scenes, classes, savegame fields). One provider, so every frontend that
	// goes through this file gets the same controls without asking.
	m.drawPinInlineEditor = [&h, &graph](int nid, int pin){
		HC::Node* n = graph.findNode(nid); if (!n) return;
		bool committed = false;
		HcEditorUtil::drawPinDefaultEditor(*n, pin, committed,
			[&h](const HC::Node& node, const std::string& param)
			{
				std::vector<std::string> c;
				if (h.paramChoices) c = h.paramChoices(node, param);
				if (c.empty()) c = HcEditorUtil::engineParamChoices(node, param, h.content);
				return c;
			});
		if (committed) h.onEdit(true); };
	// Hovering a node shows what it does + its inputs/outputs, and F1 while it is
	// up opens the manual at that node's own entry — the reference is generated
	// per function, so this lands on the call itself and not on its category.
	m.drawNodeTooltip = [&graph](int id){
		// A node the host cannot resolve draws nothing rather than an empty
		// tooltip box — the component has already opened one by the time it asks.
		const HC::Node* n = graph.findNode(id);
		if (!n) return;
		const std::string topic = HcEditorUtil::drawNodeDoc(*n);
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextDim);
		ImGui::TextUnformatted("F1  documentation");
		ImGui::PopStyleColor();
		if (!topic.empty() && ImGui::IsKeyPressed(ImGuiKey_F1, false))
			DocsPanel::openTopic(topic.c_str()); };
	// Right-click a node → context menu. When the clicked node is part of a
	// multi-selection, Delete removes the whole selection.
	m.drawNodeContextMenu = [&h, &graph](int nodeId)
	{
		GraphEditor::State& ge = *h.ge;
		const bool inSel = std::find(ge.selection.begin(), ge.selection.end(), nodeId)
			!= ge.selection.end();
		const bool multi = inSel && ge.selection.size() > 1;
		if (ImGui::MenuItem(multi ? "Duplicate Selection" : "Duplicate Node"))
		{
			const std::vector<int> src = multi ? ge.selection : std::vector<int>{ nodeId };
			const std::vector<int> fresh = HC::duplicateNodes(graph, src);
			if (!fresh.empty())
			{
				ge.selection = fresh;          // select the clones (ready to drag)
				*h.selectedNode = fresh.front();
				h.onEdit(true);
			}
		}
		if (EditorWidgets::dangerMenuItem(multi ? "Delete Selection" : "Delete Node"))
		{
			const std::vector<int> doomed = multi ? ge.selection : std::vector<int>{ nodeId };
			for (int id : doomed) graph.removeNode(id);
			ge.selection.clear();
			*h.selectedNode = 0;
			h.onEdit(true);
		}
	};
	// Drag a wire off ANY pin → a filtered menu of everything that can take it.
	m.drawPinDragMenu = [&h](int srcNode, int srcPin, bool srcInput, ImVec2 pos){
		return drawPinDragMenu(h, srcNode, srcPin, srcInput, pos); };

	// ── Quick-spawn keys ────────────────────────────────────────────────────
	// Hold the key and click empty canvas (or hit it mid link-drag, which wires
	// the node up). A type this frontend hides from its palette gets no shortcut
	// either — the keys must not be a back door into nodes the menu denies.
	for (const HcGraphShortcuts::Binding& b : HcGraphShortcuts::bindings())
	{
		if (h.menus && listed(h.menus->addExcluded, b.type)) continue;
		GraphEditor::QuickSpawn qs;
		qs.key   = letterKey(b.key);
		qs.spawn = [&h, t = b.type](const GraphEditor::QuickSpawnCtx& c){
			return quickSpawnNode(h, t, c); };
		m.quickSpawns.push_back(std::move(qs));
	}
	// G / Shift+G / E stand for a node that is useless until it is bound to
	// something, so they open a picker at the cursor instead of dropping one.
	// They cannot end a link drag (the drag context would not survive the menu).
	auto pickerKey = [&m](char key, bool shift, QuickPick::Kind kind)
	{
		GraphEditor::QuickSpawn qs;
		qs.key      = letterKey(key);
		qs.shift    = shift;
		qs.wireable = false;
		qs.spawn    = [kind](const GraphEditor::QuickSpawnCtx& c) -> int {
			s_pick.kind = kind; s_pick.pos = c.pos;
			ImGui::OpenPopup(kQuickPickPopup);
			return 0;   // the popup creates the node, not this callback
		};
		m.quickSpawns.push_back(std::move(qs));
	};
	pickerKey('G', false, QuickPick::VarGet);
	pickerKey('G', true,  QuickPick::VarSet);
	pickerKey('E', false, QuickPick::EngineApi);
	return m;
}

// ── Add menu ─────────────────────────────────────────────────────────────────

std::string beginAddMenu()
{
	const std::string q = HcEditorUtil::searchMenuBegin("##nodeSearch", "Search nodes...", 220.0f);
	ImGui::Separator();
	ImGui::BeginChild("##nodeList", ImVec2(232.0f, 300.0f));
	return q;
}

void endAddMenu() { ImGui::EndChild(); HcEditorUtil::searchMenuEnd(); }

int drawAddMenuTail(const Host& h, const std::string& q)
{
	HC::Graph& graph = *h.graph;
	const ImVec2 drop = h.ge->addMenuGraphPos;
	int created = 0;
	auto matches = [&](const std::string& name, const std::string& cat)
	{ return q.empty() || lower(name).find(q) != std::string::npos
	      || lower(cat).find(q) != std::string::npos; };

	// Spawn the EngineCall node one registry pick stands for. Shared, because the
	// rows now appear in two places: merged into a node category below, and in
	// their own sections for the categories no node uses.
	const std::vector<const char*> apiGroups =
		h.menus ? h.menus->apiGroups : std::vector<const char*>{};
	auto createEngineCall = [&](const std::string& id)
	{
		const HE::api::ApiFn* fn = HE::api::find(id);
		if (!fn) return;
		const int nid = addNode(graph, NT::EngineCall, drop, h.currentGraph);
		HC::Node* nn = graph.findNode(nid);
		nn->s = fn->id;
		nn->hasArg = fn->isExec;             // exec node vs pure data node
		nn->params.clear(); nn->results.clear();
		for (const auto& p : fn->params)  nn->params.push_back({ p.name, p.type, p.isArray });
		for (const auto& r : fn->results) nn->results.push_back({ r.name, r.type, r.isArray });
		created = nid;
	};

	// Generic node categories. Which ones a frontend lists is host data: the
	// widget editor has Property/Widget (self) nodes, a level script does not —
	// there the id-based widget nodes under "UI" are the only widget access.
	//
	// The engine calls of the SAME category are drawn inside this loop, under the
	// same header. A "UI" section holding the UI nodes and the UI engine calls is
	// one place to look; the old split put them in a node section and a second
	// "Engine · UI" section far below it, which asked the reader to know which
	// half a call lives in before they could find it.
	for (const char* cat : h.menus->addCategories)
	{
		bool header = false;
		for (NT t : HC::nodeRegistry())
		{
			if (listed(h.menus->addExcluded, t)) continue;
			if (std::string(HC::nodeCategory(t)) != cat) continue;
			if (!matches(HC::nodeDisplayName(t), cat) && !aliasHit(t, q)) continue;
			if (!header) { ImGui::TextDisabled("%s", cat); header = true; }
			bool hov = false;
			if (menuItemWithHint(HC::nodeDisplayName(t), HcGraphShortcuts::hintFor(t), &hov))
			{ created = addNode(graph, t, drop, h.currentGraph); ImGui::CloseCurrentPopup(); }
			if (hov && ImGui::BeginTooltip())
			{
				// The guard has to expire BEFORE EndTooltip: PopTextWrapPos acts on
				// whatever window is current, and EndTooltip has by then switched back
				// to the canvas — so an unscoped guard pushes on the tooltip and pops
				// on the canvas. That is the imbalance ImGui reports as "Calling
				// PopTextWrapPos() too many times". Same rule EditorWidgets.h states.
				{
					EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 35.0f);
					HcEditorUtil::drawNodeDoc(t);
				}
				ImGui::EndTooltip();
			}
		}
		// …and this category's engine calls, under the header the nodes just
		// drew (or drawing it themselves if no node matched).
		if (std::string picked = HcEditorUtil::drawEngineApiMenu(q, apiGroups, cat, &header);
		    !picked.empty())
		{ createEngineCall(picked); ImGui::CloseCurrentPopup(); }
		if (header) ImGui::Spacing();
	}

	// Call <function> for each declared function entry — this class's own and the
	// public ones it inherits — plus a Return node.
	bool fh = false;
	for (const HC::Node& e : offerableFunctions(graph))
	{
		const std::string lbl = "Call " + e.s;
		if (!matches(lbl, "Functions")) continue;
		if (!fh) { ImGui::TextDisabled("Functions"); fh = true; }
		if (HcEditorUtil::searchMenuItem(lbl))
		{
			const int id = addNode(graph, NT::FunctionCall, drop, h.currentGraph);
			bindCallTo(graph, id, e);
			created = id; ImGui::CloseCurrentPopup();
		}
	}
	// A Return node — only inside a function sub-graph, auto-bound to that
	// function so its pins mirror the declared outputs.
	if (h.currentGraph != 0 && matches("Return", "Functions"))
	{
		if (!fh) { ImGui::TextDisabled("Functions"); fh = true; }
		if (HcEditorUtil::searchMenuItem("Return"))
		{
			const int id = addNode(graph, NT::FunctionReturn, drop, h.currentGraph);
			if (const HC::Node* owner = graph.findNode(h.currentGraph))
			{ HC::Node* rn = graph.findNode(id); rn->s = owner->s; rn->results = owner->results; }
			HC::syncFunctionSignatures(graph);
			created = id; ImGui::CloseCurrentPopup();
		}
	}
	if (fh) ImGui::Spacing();

	// The engine-call categories NO node category covers — Physics, Player, Save,
	// Transform and the rest. Each gets its own section, named after the
	// subsystem and nothing else: a reader looking for "Set Position" should find
	// a "Transform" heading, not have to know that the engine calls used to live
	// in a separate half of this menu.
	for (const char* cat : HcEditorUtil::engineApiCategories(q, apiGroups))
	{
		bool covered = false;
		for (const char* c : h.menus->addCategories)
			if (std::strcmp(c, cat) == 0) { covered = true; break; }
		if (covered) continue;   // already drawn, merged into the loop above

		bool header = false;
		if (std::string picked = HcEditorUtil::drawEngineApiMenu(q, apiGroups, cat, &header);
		    !picked.empty())
		{ createEngineCall(picked); ImGui::CloseCurrentPopup(); }
		if (header) ImGui::Spacing();
	}

	// Get/Set for each declared variable (locals only inside their function).
	bool vh = false;
	for (const HC::Variable* vp : offerableVariables(graph))
	{
		const HC::Variable& v = *vp;
		for (int k = 0; k < 2; ++k)
		{
			if (v.scope != 0 && v.scope != h.currentGraph) continue;
			const std::string lbl = (k == 0 ? "Get " : "Set ") + v.name;
			if (!matches(lbl, "Variables")) continue;
			if (!vh) { ImGui::TextDisabled("Variables"); vh = true; }
			if (menuItemWithHint(lbl, k == 0 ? "G" : "Shift+G"))
			{
				const int id = addNode(graph, k == 0 ? NT::GetVariable : NT::SetVariable,
				                       drop, h.currentGraph);
				HC::Node* nn = graph.findNode(id);
				nn->s = v.name; nn->propType = v.type; nn->isArray = v.isArray;
				// The container half travels with the type — a Get/Set node that
				// kept only `isArray` would draw an ARRAY pin for a set or a map.
				nn->container = v.container; nn->keyType = v.keyType;
				nn->keyTypeName = v.keyTypeName;
				nn->typeName = v.typeName;
				created = id; ImGui::CloseCurrentPopup();
			}
		}
	}
	if (vh) ImGui::Spacing();

	// ── User-defined types: one entry per project Struct/Enum asset ─────────
	// The node is born fully mirrored (params from the definition), so its pins
	// resolve immediately; syncTypeSignatures keeps it fresh on later loads.
	{
		auto& reg = HE::TypeRegistry::instance();
		auto spawn = [&](NT t, const std::string& typeName) {
			const int id = addNode(graph, t, drop, h.currentGraph);
			HC::Node* nn = graph.findNode(id);
			nn->typeName = typeName;
			HC::syncTypeSignatures(graph);
			created = id; ImGui::CloseCurrentPopup();
		};
		// Get/Set for ONE field, pre-picked: the plain "Get X Field" row drops a
		// node you still have to bind in the details panel, which is a detour
		// when you already know the field you want.
		auto spawnField = [&](NT t, const std::string& typeName, const HE::StructField& f) {
			const int id = addNode(graph, t, drop, h.currentGraph);
			HC::Node* nn = graph.findNode(id);
			nn->typeName = typeName;
			nn->params = { { f.name, f.type, f.isArray, f.typeName } };
			HC::syncTypeSignatures(graph);   // revalidates the choice against the def
			created = id; ImGui::CloseCurrentPopup();
		};
		bool sh = false;
		for (const auto& d : reg.structs())
		{
			const struct { const char* fmt; NT t; } rows[] = {
				{ "Make %s",            NT::MakeStruct },
				{ "Break %s",           NT::BreakStruct },
				{ "Get %s Field",       NT::GetStructField },
				{ "Set %s Field",       NT::SetStructField },
			};
			for (const auto& r : rows)
			{
				char lbl[256]; std::snprintf(lbl, sizeof lbl, r.fmt, d.name.c_str());
				if (!matches(lbl, "Structs")) continue;
				if (!sh) { ImGui::TextDisabled("Structs"); sh = true; }
				const bool picked = HcEditorUtil::searchMenuItem(lbl);
				// Same hover help the generic category rows show — these entries
				// are generated per asset, so they need it spelled out here.
				if (ImGui::IsItemHovered())
				{
					HC::Node probe; probe.type = r.t; probe.typeName = d.assetPath;
					if (ImGui::BeginTooltip())
					{
						// Scoped so the pop lands on the tooltip, not on the palette
						// EndTooltip returns to — see the note at the canvas tooltip.
						{
							EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 35.0f);
							HcEditorUtil::drawNodeDoc(probe);
						}
						ImGui::EndTooltip();
					}
				}
				if (picked) spawn(r.t, d.assetPath);
			}
			// One Get/Set row per field of this struct.
			for (const auto& f : d.fields)
			{
				const struct { const char* verb; NT t; } fops[] = {
					{ "Get", NT::GetStructField }, { "Set", NT::SetStructField },
				};
				for (const auto& op : fops)
				{
					char lbl[320];
					std::snprintf(lbl, sizeof lbl, "%s %s \xc2\xb7 %s",
					              op.verb, d.name.c_str(), f.name.c_str());
					if (!matches(lbl, "Structs")) continue;
					if (!sh) { ImGui::TextDisabled("Structs"); sh = true; }
					if (HcEditorUtil::searchMenuItem(lbl)) spawnField(op.t, d.assetPath, f);
				}
			}
		}
		if (sh) ImGui::Spacing();
		bool eh = false;
		for (const auto& d : reg.enums())
		{
			const struct { const char* fmt; NT t; } rows[] = {
				{ "%s Value",           NT::ConstEnum },
				{ "Switch on %s",       NT::SwitchOnEnum },
				{ "%s to Int",          NT::EnumToInt },
				{ "Int to %s",          NT::IntToEnum },
				{ "%s to String",       NT::EnumToString },
			};
			for (const auto& r : rows)
			{
				char lbl[256]; std::snprintf(lbl, sizeof lbl, r.fmt, d.name.c_str());
				if (!matches(lbl, "Enums")) continue;
				if (!eh) { ImGui::TextDisabled("Enums"); eh = true; }
				const bool picked = HcEditorUtil::searchMenuItem(lbl);
				if (ImGui::IsItemHovered())
				{
					HC::Node probe; probe.type = r.t; probe.typeName = d.assetPath;
					if (ImGui::BeginTooltip())
					{
						// Scoped so the pop lands on the tooltip, not on the palette
						// EndTooltip returns to — see the note at the canvas tooltip.
						{
							EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 35.0f);
							HcEditorUtil::drawNodeDoc(probe);
						}
						ImGui::EndTooltip();
					}
				}
				if (picked) spawn(r.t, d.assetPath);
			}
		}
	}
	return created;
}

// ── Drag-off menu ────────────────────────────────────────────────────────────
// Ref outputs lead with the target class's public members; exec pins list every
// exec-capable node; data pins list nodes with a matching input (or output, when
// dragging backwards off an input). The pick is auto-wired.

int drawPinDragMenu(const Host& h, int srcNode, int srcPin, bool srcInput, const ImVec2& pos)
{
	HC::Graph& graph = *h.graph;
	HC::Node* sn = graph.findNode(srcNode);
	if (!sn) return 0;
	int created = 0;

	// BY VALUE, deliberately: every pick below addNode()s into graph.nodes, and
	// a vector realloc would leave `sn` dangling for everything drawn after the
	// pick in this same frame (the menu keeps rendering past a pick).
	const HC::Node src = *sn;

	// Classify the dragged pin (exec vs data; data type + which container).
	const DragPin dp = classifyDragPin(src, srcPin);
	const bool isExecPin = dp.isExec;
	const PT   dragType  = dp.type;
	const auto dragCtr   = dp.ctr;

	const std::string q = HcEditorUtil::searchMenuBegin("##dragSearch", "Search…", 232.0f);
	auto matches = [&](const std::string& name){ return q.empty() || lower(name).find(q) != std::string::npos; };

	// Node types the hand-written "Reference" section already offers. The
	// generic registry walk below must skip them, or a Ref-output drag lists
	// e.g. "Get (Ref)" twice — two Selectables with the same label are the
	// same ImGui ID, which trips the ID-conflict detector on hover (and no
	// menus-list exclusion can express "excluded only from the generic half").
	std::vector<NT> refOffered;

	ImGui::BeginChild("##pindrag", ImVec2(240.0f, 320.0f));

	// Wire the new node to the dragged pin (direction depends on the drag side).
	// adoptForEachElementType first: a ForEach on either end takes the array's
	// element type (and class) before the typed connect.
	auto wireAt = [&](int newId, int pin){
		if (srcInput) { HC::adoptForEachElementType(graph, newId, pin, srcNode, srcPin);
		                graph.connect(newId, pin, srcNode, srcPin); }
		else          { HC::adoptForEachElementType(graph, srcNode, srcPin, newId, pin);
		                graph.connect(srcNode, srcPin, newId, pin); }
		HC::inferUserTypeNames(graph); };   // learns its definition from the wire

	// ── Ref output: the target class's public members lead ────────────────
	if (!isExecPin && !srcInput && dragType == PT::Ref && dragCtr == HC::ContainerKind::None)
	{
		auto wire = [&](int newId){
			HC::Node* nn = graph.findNode(newId);
			if (nn) graph.connect(srcNode, srcPin, newId, pinRanges(*nn).dataIn0); // → Target
		};
		HC::Graph scratch;
		const HC::Graph* cls = resolveClassGraph(src, graph, h.giGraph, h.content, scratch);
		// The key behind that graph, recorded on whatever this menu creates: the
		// menu already knows which class it is offering members of, and writing it
		// down here is what lets a rename find these nodes later without re-walking
		// a wire that may since have been re-routed.
		const std::string clsKey = HcRename::classOfRefSource(graph, src, h.selfKey, "Game Instance");
		if (cls)
		{
			// SNAPSHOT the offered signatures first: with a "Get Self" source,
			// `cls` ALIASES `graph`, and a pick addNode()s into that very
			// nodes-vector — iterating it live would leave the loop iterator
			// AND `fn` (read after the add) dangling on reallocation.
			std::vector<HC::Node> fns;
			for (const auto& fn : cls->nodes)
				if (fn.type == NT::FunctionEntry && fn.access == 0 && !fn.s.empty())
					fns.push_back(fn);
			bool fh = false;
			for (const auto& fn : fns)
				if (matches("Call " + fn.s))
				{
					if (!fh) { ImGui::TextDisabled("Functions"); fh = true; }
					if (HcEditorUtil::searchMenuItem("Call " + fn.s))
					{
						const int id = addNode(graph, NT::CallExternal, pos, h.currentGraph);
						HC::Node* nn = graph.findNode(id);
						nn->s = fn.s; nn->params = fn.params; nn->results = fn.results; // typed signature
						nn->className = clsKey;   // whose function this is, while the menu still knows
						wire(id); created = id; ImGui::CloseCurrentPopup();
					}
				}
			bool vh = false;
			for (const auto& var : cls->variables)
				if (var.access == 0)
				{
					if (!vh && (matches("Get " + var.name) || matches("Set " + var.name)))
					{ ImGui::TextDisabled("Variables"); vh = true; }
					if (matches("Get " + var.name) && HcEditorUtil::searchMenuItem("Get " + var.name))
					{ const int id = addNode(graph, NT::GetExternal, pos, h.currentGraph); HC::Node* nn = graph.findNode(id); nn->s = var.name; nn->propType = var.type; nn->className = clsKey; wire(id); created = id; ImGui::CloseCurrentPopup(); }
					if (matches("Set " + var.name) && HcEditorUtil::searchMenuItem("Set " + var.name))
					{ const int id = addNode(graph, NT::SetExternal, pos, h.currentGraph); HC::Node* nn = graph.findNode(id); nn->s = var.name; nn->propType = var.type; nn->className = clsKey; wire(id); created = id; ImGui::CloseCurrentPopup(); }
				}
			if (fh || vh) ImGui::Separator();
		}
		else ImGui::TextDisabled("(untyped object)");

		// What the reference INHERITS: the members its engine base class brings
		// with it. They are HE::api registry rows, not nodes in any graph, so
		// picking one inserts an Engine Call already wired to this reference —
		// which is why the base class needs no dispatch machinery of its own.
		{
			const std::string base =
				resolveClassBase(src, graph, h.selfBaseClass, h.content);
			bool eh = false;
			for (const auto& m : HC::engineClassMembers(base))
			{
				if (!matches(m.label)) continue;
				const HE::api::ApiFn* fn = HE::api::find(m.apiId);
				if (!fn) continue;   // a typo in the table; a test guards it too
				if (!eh) { ImGui::TextDisabled("Inherited"); eh = true; }
				if (!HcEditorUtil::searchMenuItem(m.label)) continue;
				const int id = addNode(graph, NT::EngineCall, pos, h.currentGraph);
				HC::Node* nn = graph.findNode(id);
				nn->s = m.apiId;
				nn->hasArg = fn->isExec;
				for (const auto& p : fn->params)  nn->params.push_back({ p.name, p.type, p.isArray });
				for (const auto& r : fn->results) nn->results.push_back({ r.name, r.type, r.isArray });
				// Wire this reference into the parameter that takes the target,
				// so the node arrives usable rather than as one more thing to
				// connect.
				const PinRanges r = pinRanges(*nn);
				graph.connect(srcNode, srcPin, id, r.dataIn0 + m.targetParam);
				created = id; ImGui::CloseCurrentPopup();
			}
			if (eh) ImGui::Separator();
		}

		ImGui::TextDisabled("Reference");
		auto refItem = [&](const char* lbl, NT t){
			refOffered.push_back(t);   // the generic walk below must skip these
			if (matches(lbl) && HcEditorUtil::searchMenuItem(lbl))
			{ const int id = addNode(graph, t, pos, h.currentGraph); wire(id); created = id; ImGui::CloseCurrentPopup(); } };
		refItem("Call Function (Ref)", NT::CallExternal);
		refItem("Bind Event",          NT::BindEvent);
		refItem("Get (Ref)",           NT::GetExternal);
		refItem("Set (Ref)",           NT::SetExternal);
		refItem("Destroy Object",      NT::DestroyObject);
		ImGui::Separator();
	}

	// ── A struct pin leads with its own FIELDS ─────────────────────────────
	// "Get Struct Field" is a node you still have to bind afterwards; what you
	// actually wanted is the field. Offer those first, already chosen.
	if (!isExecPin && dp.type == PT::Struct && !dp.array && !dp.typeName.empty())
	{
		HE::StructDef def;
		if (HE::TypeRegistry::instance().getStruct(dp.typeName, def) && !def.fields.empty())
		{
			const std::string stem = std::filesystem::path(dp.typeName).stem().string();
			bool fh = false;
			for (const auto& f : def.fields)
			{
				// Dragging OFF a struct output offers readers and writers (both
				// take the struct as their first input); dragging backwards off a
				// struct INPUT offers the ones that produce a struct.
				const struct { const char* verb; NT t; bool wantsOutput; } ops[] = {
					{ "Get", NT::GetStructField, false },
					{ "Set", NT::SetStructField, true  },
				};
				for (const auto& op : ops)
				{
					if (srcInput && !op.wantsOutput) continue;   // Get produces the FIELD, not a struct
					char lbl[320];
					std::snprintf(lbl, sizeof lbl, "%s %s \xc2\xb7 %s",
					              op.verb, stem.c_str(), f.name.c_str());
					if (!matches(lbl)) continue;
					if (!fh) { ImGui::TextDisabled("Fields"); fh = true; }
					if (!HcEditorUtil::searchMenuItem(lbl)) continue;
					const int id = addNode(graph, op.t, pos, h.currentGraph);
					HC::Node* nn = graph.findNode(id);
					nn->typeName = dp.typeName;
					nn->params = { { f.name, f.type, f.isArray, f.typeName } };
					HC::syncTypeSignatures(graph);
					const PinRanges r = pinRanges(*graph.findNode(id));
					// Wire the struct end: its data-in 0 either way, or — dragging
					// backwards — Set's struct OUTPUT into the pin we came from.
					wireAt(id, srcInput ? r.dataOut0 : r.dataIn0);
					created = id; ImGui::CloseCurrentPopup();
				}
			}
			if (fh) ImGui::Spacing();
		}
	}

	// ── Generic nodes with a compatible pin ────────────────────────────────
	{
		bool gh = false;
		for (NT t : HC::nodeRegistry())
		{
			if (listed(h.menus->dragExcluded, t)) continue;
			if (std::find(refOffered.begin(), refOffered.end(), t) != refOffered.end())
				continue;   // the "Reference" section above already offers it
			const int pin = HcEditorUtil::dragMatchPin(t, dragType, dragCtr, srcInput, isExecPin);
			// Same alias vocabulary as the add menu: a search that finds a node in
			// one menu and not in the other is the more confusing of the two.
			if (pin < 0 || (!matches(HC::nodeDisplayName(t)) && !aliasHit(t, q))) continue;
			if (!gh) { ImGui::TextDisabled("Nodes"); gh = true; }
			// The hint is live here too: the same key hit MID-DRAG skips this
			// menu and drops the node already wired.
			bool hov = false;
			if (menuItemWithHint(HC::nodeDisplayName(t), HcGraphShortcuts::hintFor(t), &hov))
			{
				const int id = addNode(graph, t, pos, h.currentGraph);
				// Seed + mirror, then wire the pin the REAL node has: a user-type
				// node bound to a definition carries pins the listing probe above
				// could not know about.
				const int realPin = seedSpawnedNode(graph, id, dp, srcInput);
				if (realPin >= 0) wireAt(id, realPin);
				created = id; ImGui::CloseCurrentPopup();
			}
			if (hov && ImGui::BeginTooltip())
			{
				// The guard has to expire BEFORE EndTooltip: PopTextWrapPos acts on
				// whatever window is current, and EndTooltip has by then switched back
				// to the canvas — so an unscoped guard pushes on the tooltip and pops
				// on the canvas. That is the imbalance ImGui reports as "Calling
				// PopTextWrapPos() too many times". Same rule EditorWidgets.h states.
				{
					EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 35.0f);
					HcEditorUtil::drawNodeDoc(t);
				}
				ImGui::EndTooltip();
			}
		}
		if (gh) ImGui::Spacing();
	}

	// ── Engine API calls with a compatible pin ─────────────────────────────
	{
		bool eh = false;
		for (const HE::api::ApiFn& fn : HE::api::registry())
		{
			// Same allow-list as the add menu — this is the second half of the
			// restriction, and skipping it here would make the first half a
			// decoration you can drag around.
			if (h.menus && !HcEditorUtil::apiGroupAllowed(fn.id, h.menus->apiGroups)) continue;
			const int pin = HcEditorUtil::dragMatchApiPin(fn, dragType, dragCtr, srcInput, isExecPin);
			const char* shown = fn.displayName ? fn.displayName : fn.id;
			if (pin < 0 || !matches(shown)) continue;
			if (!eh) { ImGui::TextDisabled("Engine"); eh = true; }
			if (HcEditorUtil::searchMenuItem(std::string(shown) + "##" + fn.id))
			{
				const int id = addNode(graph, NT::EngineCall, pos, h.currentGraph);
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

	// ── This graph's variables (Set on exec/matching value; Get feeds inputs;
	//    locals only inside their owning function) ──
	{
		bool vh = false;
		for (const HC::Variable* vp : offerableVariables(graph))
		{
			const HC::Variable& v = *vp;
			if (v.scope != 0 && v.scope != h.currentGraph) continue;
			const bool sameShape = v.type == dragType && v.kind() == dragCtr;
			const bool setOk = (isExecPin && !srcInput) || (!isExecPin && !srcInput && sameShape);
			const bool getOk = !isExecPin && srcInput && sameShape;
			auto add = [&](bool get){
				const int id = addNode(graph, get ? NT::GetVariable : NT::SetVariable, pos, h.currentGraph);
				HC::Node* nn = graph.findNode(id);
				nn->s = v.name; nn->propType = v.type; nn->isArray = v.isArray;
				// The container half travels with the type — a Get/Set node that
				// kept only `isArray` would draw an ARRAY pin for a set or a map.
				nn->container = v.container; nn->keyType = v.keyType;
				nn->keyTypeName = v.keyTypeName;
				nn->typeName = v.typeName;   // Enum/Struct: WHICH definition (like the add menu)
				const PinRanges r = pinRanges(*nn);
				wireAt(id, get ? r.dataOut0 : (isExecPin ? r.execIn0 : r.dataIn0));
				created = id; ImGui::CloseCurrentPopup(); };
			if (setOk && matches("Set " + v.name))
			{
				if (!vh) { ImGui::TextDisabled("Variables"); vh = true; }
				if (HcEditorUtil::searchMenuItem("Set " + v.name)) add(false);
			}
			if (getOk && matches("Get " + v.name))
			{
				if (!vh) { ImGui::TextDisabled("Variables"); vh = true; }
				if (HcEditorUtil::searchMenuItem("Get " + v.name)) add(true);
			}
		}
		if (vh) ImGui::Spacing();
	}

	// ── Declared functions (exec drags call them) ──────────────────────────
	if (isExecPin)
	{
		bool fh = false;
		for (const HC::Node& e : offerableFunctions(graph))
		{
			if (!matches("Call " + e.s)) continue;
			if (!fh) { ImGui::TextDisabled("Functions"); fh = true; }
			if (HcEditorUtil::searchMenuItem("Call " + e.s))
			{
				const int id = addNode(graph, NT::FunctionCall, pos, h.currentGraph);
				bindCallTo(graph, id, e);
				HC::Node* nn = graph.findNode(id);
				const PinRanges r = pinRanges(*nn);
				wireAt(id, srcInput ? r.execOut0 : r.execIn0);
				created = id; ImGui::CloseCurrentPopup();
			}
		}
	}

	ImGui::EndChild();
	HcEditorUtil::searchMenuEnd();
	return created;
}

// ── Node details: the rows every HorizonCode frontend spells the same ────────
// See the header for exactly which node types this covers and which ones each
// frontend still draws itself.

// A definition combo for the user-defined-type nodes. Nodes can end up unbound —
// an older editor build spawned them that way off a pin drag, and a node with no
// wires has nothing for inferUserTypeNames to learn from — and until now the
// panel just reported that and dead-ended. Rebinding re-mirrors the pins and
// carries the links over, because the layout changes with the definition.
bool drawDefinitionPicker(HC::Graph& g, HC::Node& n, bool isStruct,
                          const std::function<void(bool)>& edit)
{
	// The same scope every caller has open. Redundant at run time, not on paper:
	// this helper is defined above the node rows that call it, and the coverage
	// scan reads a file from top to bottom.
	HE::Ed::Help::Scope helpScope("HorizonCode Node");

	auto& reg = HE::TypeRegistry::instance();
	const bool bound = isStruct ? reg.hasStruct(n.typeName) : reg.hasEnum(n.typeName);
	const std::string cur = n.typeName.empty()
		? std::string("(pick a definition)")
		: std::filesystem::path(n.typeName).stem().string() + (bound ? "" : "  (missing!)");
	bool changed = false;
	// One control, two labels — so two entries, and the lookup asks for whichever
	// is on screen. The scope is the caller's; every path into here is a node.
	const char* defLabel = isStruct ? "Struct" : "Enum";
	const bool defOpen = ImGui::BeginCombo(defLabel, cur.c_str());
	if (!defOpen) EditorWidgets::helpForLabel(defLabel);
	if (defOpen)
	{
		if (isStruct)
			for (const auto& d : reg.structs())
			{ if (ImGui::Selectable(d.name.c_str(), n.typeName == d.assetPath) &&
				  n.typeName != d.assetPath) { n.typeName = d.assetPath; changed = true; } }
		else
			for (const auto& d : reg.enums())
			{ if (ImGui::Selectable(d.name.c_str(), n.typeName == d.assetPath) &&
				  n.typeName != d.assetPath) { n.typeName = d.assetPath; changed = true; } }
		ImGui::EndCombo();
	}
	if (changed)
	{
		n.params.clear();                      // the old definition's mirror
		HC::remapLinksForMirror(g, { n.id });  // re-mirror + move the links along
		edit(true);
	}
	if (!bound && !n.typeName.empty())
		ImGui::TextDisabled("Not in this project:\n%s", n.typeName.c_str());
	return changed;
}

bool drawEventPicker(HC::Graph& g, HC::Node& n, const char* label)
{
	bool changed = false;
	// Its own scope, and the same one the level-script panel's event details use:
	// this picker is shared by every host, so the entry has to read correctly
	// whether the graph belongs to a widget, a level script or a state machine.
	HE::Ed::Help::Scope helpScope("HorizonCode Event");

	const std::string cur = n.s.empty() ? "(none)" : n.s;
	const bool comboOpen = ImGui::BeginCombo(label, cur.c_str());
	if (!comboOpen) EditorWidgets::helpForLabel(label);
	if (comboOpen)
	{
		if (g.events.empty())
			ImGui::TextDisabled("This class declares no events yet.");
		for (const auto& e : g.events)
			if (ImGui::Selectable(e.name.c_str(), n.s == e.name) && n.s != e.name)
			{
				n.s = e.name;
				n.hasArg   = e.hasArg;      // keep the pin in step with the declaration
				n.propType = e.argType;
				n.typeName = e.typeName;
				changed = true;
			}
		ImGui::EndCombo();
	}
	// A name that is not declared yet: typing it here declares it, so the two
	// halves of a binding cannot drift apart by a typo.
	static std::string s_newName;
	static int s_newFor = 0;
	if (s_newFor != n.id) { s_newName.clear(); s_newFor = n.id; }
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 96.0f);
	ImGui::InputTextWithHint("##newevt", "New event name…", &s_newName);
	ImGui::SameLine();
	const bool canAdd = !s_newName.empty() && !g.findEvent(s_newName) &&
	                    !HC::findEngineEvent(s_newName);
	if (!canAdd) ImGui::BeginDisabled();
	if (EditorWidgets::button("Declare"))
	{
		HC::EventDecl d;
		d.name = s_newName;
		g.events.push_back(d);
		n.s = d.name; n.hasArg = false; n.propType = PT::Float; n.typeName.clear();
		s_newName.clear();
		changed = true;
	}
	if (!canAdd) ImGui::EndDisabled();
	if (!s_newName.empty() && HC::findEngineEvent(s_newName))
		ImGui::TextDisabled("\"%s\" is an engine event — every class can handle it,\n"
		                    "none declares it.", s_newName.c_str());
	return changed;
}

// ── Target Class + member, for the three nodes that reach into another object ─
// Call (Ref), Get (Ref) and Set (Ref) name a member of ANOTHER class, and used to
// do it with a bare text field: the target is whatever the reference turns out to
// be at run time, so there was nothing to offer a list from. There is now,
// because the graph can usually say what the Target is — Create Object and Cast
// name their class, a Ref variable records one, Get Self is this graph — and
// where it cannot, the node can be told and remembers (Node::className).
//
// Recording it is what makes renaming that member possible later: HcRename reads
// exactly this, and a node that knows which class it meant is a node a rename can
// find without guessing.

// The graph whose members are offered. For a target that IS this graph, the copy
// on screen wins over the one on disk: it has the function the user just added.
const HC::Graph* memberSource(const Host& h, const HC::Graph& g, const std::string& cls,
                              HC::Graph& scratch)
{
	if (cls.empty()) return nullptr;
	if (cls == h.selfKey) return &g;
	return loadClassGraph(h.content, cls, scratch) ? &scratch : nullptr;
}

// Which class this node's Target is. Returns it, so the member row below knows
// what to offer; "" when neither the node nor the wire can say.
std::string drawTargetClassRow(const Host& h, HC::Graph& g, HC::Node& n)
{
	// A shared helper scopes itself: the callers push this same scope, but a
	// reader walking this file top to bottom (the help audit does exactly that)
	// would otherwise file these rows under whatever scope was opened last.
	HE::Ed::Help::Scope helpScope("HorizonCode Node");

	const std::string fromWire = n.className.empty()
		? HcRename::targetClassOf(g, n, h.selfKey, "Game Instance")
		: std::string();
	const std::string resolved = n.className.empty() ? fromWire : n.className;

	std::string preview;
	if (!n.className.empty())   preview = HcEditorUtil::castTargetLabel(n.className);
	else if (!fromWire.empty()) preview = "From the wire: " + HcEditorUtil::castTargetLabel(fromWire);
	else                        preview = "(unknown)";

	const bool open = ImGui::BeginCombo("Target Class", preview.c_str());
	if (!open) EditorWidgets::helpForLabel("Target Class");
	if (open)
	{
		// Leaving it to the wire is the DEFAULT, and it is the better answer
		// whenever the wire can answer: one place to change instead of two.
		if (ImGui::Selectable("From the wire", n.className.empty()) && !n.className.empty())
		{ n.className.clear(); if (h.onEdit) h.onEdit(true); }
		ImGui::Separator();
		for (const auto& c : HcEditorUtil::listHorizonCodeClasses(h.content))
			if (ImGui::Selectable((c.label + "##" + c.path).c_str(), n.className == c.path) &&
			    n.className != c.path)
			{ n.className = c.path; if (h.onEdit) h.onEdit(true); }
		ImGui::EndCombo();
	}
	return resolved;
}

// The member itself. False = nothing could be offered, so the caller falls back
// to the text field that was the only thing here before.
bool drawTargetMemberRow(const Host& h, HC::Graph& g, HC::Node& n, const std::string& cls,
                         bool functions)
{
	HE::Ed::Help::Scope helpScope("HorizonCode Node");   // see drawTargetClassRow

	HC::Graph scratch;
	const HC::Graph* src = memberSource(h, g, cls, scratch);
	if (!src) return false;

	// A name the class does not have is not hidden behind a tidy dropdown: it is
	// what a rename somewhere else leaves behind, and it is the one thing worth
	// seeing at a glance.
	const char* label = functions ? "Function" : "Variable";
	bool known = false;
	if (functions)
		for (const HC::Node& fn : src->nodes)
			known = known || (fn.type == NT::FunctionEntry && fn.access == 0 && fn.s == n.s);
	else
		for (const HC::Variable& v : src->variables)
			known = known || (v.access == 0 && v.name == n.s);

	std::string preview = n.s.empty() ? std::string("(none)") : n.s;
	if (!n.s.empty() && !known) preview += "  — not on " + HcEditorUtil::castTargetLabel(cls);

	const bool open = ImGui::BeginCombo(label, preview.c_str());
	if (!open) EditorWidgets::helpForLabel(label);
	if (open)
	{
		if (functions)
		{
			// Snapshot first: picking one edits this very nodes-vector when the
			// target is this graph, and the loop's iterator would not survive it.
			std::vector<HC::Node> fns;
			for (const HC::Node& fn : src->nodes)
				if (fn.type == NT::FunctionEntry && fn.access == 0 && !fn.s.empty()) fns.push_back(fn);
			for (const HC::Node& fn : fns)
				if (ImGui::Selectable(fn.s.c_str(), n.s == fn.s) && n.s != fn.s)
				{
					// The pins ARE the signature, so every data wire on the node was
					// measured against the old one. Target (the first data input)
					// survives; the rest cannot.
					const PinRanges r = pinRanges(n);
					for (int pin = r.dataIn0 + 1; pin < r.end; ++pin) removePinLinks(g, n.id, pin);
					n.s = fn.s; n.params = fn.params; n.results = fn.results;
					if (h.onEdit) h.onEdit(true);
				}
		}
		else
		{
			std::vector<HC::Variable> vars;
			for (const HC::Variable& v : src->variables)
				if (v.access == 0 && v.scope == 0) vars.push_back(v);
			for (const HC::Variable& v : vars)
				if (ImGui::Selectable(v.name.c_str(), n.s == v.name) && n.s != v.name)
				{
					if (v.type != n.propType)
					{
						const PinRanges r = pinRanges(n);
						const int valuePin = n.type == NT::GetExternal ? r.dataOut0 : (r.dataIn0 + 1);
						removePinLinks(g, n.id, valuePin);
						n.propType = v.type;
					}
					n.s = v.name;
					if (h.onEdit) h.onEdit(true);
				}
		}
		ImGui::EndCombo();
	}
	return true;
}

bool drawCommonNodeDetails(const Host& h, HC::Node& n)
{
	if (!h.graph) return false;
	HC::Graph& g = *h.graph;
	// The hosts differ only in HOW an edit is recorded, which is what onEdit is
	// for: committed = the edit is finished (undo/save point), false = a value is
	// still being dragged.
	// The node rows every host shares. Four panels call in here — the level
	// script, the widget designer, the class editor and the animator's sync
	// graph — so every sentence under this scope has to be true in all four.
	HE::Ed::Help::Scope helpScope("HorizonCode Node");

	const auto edit = [&h](bool committed){ if (h.onEdit) h.onEdit(committed); };

	switch (n.type)
	{
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
		const PT before = n.propType;
		if (HcEditorUtil::drawTypePicker("Element", h.content, n.propType, &n.s) && n.propType != before)
		{
			// Retyping the element invalidates every link on the node, in and out.
			g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
				[&](const HC::Link& l){ return l.srcNode == n.id || l.dstNode == n.id; }), g.links.end());
			edit(true);
		}
		ImGui::TextDisabled("Element type of the array.");
		return true;
	}

	case NT::SetMake:
	case NT::SetAdd:
	case NT::SetRemove:
	case NT::SetContains:
	case NT::SetLength:
	case NT::SetClear:
	case NT::SetToArray:
	case NT::ForEachSet:
	// The conversion and the set algebra pick their element the same way: one
	// propType stands for every Set (and Array) pin on the node, so both sides
	// of a union are the same kind of set by construction rather than by check.
	case NT::SetFromArray:
	case NT::SetUnion:
	case NT::SetIntersect:
	case NT::SetDifference:
	{
		const PT before = n.propType;
		if (HcEditorUtil::drawTypePicker("Element", h.content, n.propType, &n.s) && n.propType != before)
		{
			g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
				[&](const HC::Link& l){ return l.srcNode == n.id || l.dstNode == n.id; }), g.links.end());
			edit(true);
		}
		ImGui::TextDisabled("Element type of the set.\nIterates in the order elements were first added.");
		return true;
	}

	case NT::MapMake:
	case NT::MapSet:
	case NT::MapRemove:
	case NT::MapContains:
	case NT::MapLength:
	case NT::MapClear:
	case NT::MapGet:
	case NT::MapKeys:
	case NT::MapValues:
	case NT::ForEachMap:
	// The four new map nodes carry both halves too: Make Map From Arrays takes
	// its key type from the Array<K> input, and Break Map / Find By Value /
	// Remove By Value all lay out a key pin, so none of them can be described
	// by the value picker alone.
	case NT::MapFromArrays:
	case NT::MapBreak:
	case NT::MapFindByValue:
	case NT::MapRemoveByValue:
	{
		// A map node needs BOTH halves picked; either one changing re-lays the
		// pins, so both drop the node's links the way the element picker does.
		bool retyped = false;
		const PT beforeKey = n.keyType;
		const std::string beforeKeyDef = n.keyTypeName;
		if (HcEditorUtil::drawKeyTypePicker("Key", n.keyType, n.keyTypeName) &&
		    (n.keyType != beforeKey || n.keyTypeName != beforeKeyDef))
			retyped = true;
		const PT before = n.propType;
		if (HcEditorUtil::drawTypePicker("Value", h.content, n.propType, &n.s) && n.propType != before)
			retyped = true;
		if (retyped)
		{
			g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
				[&](const HC::Link& l){ return l.srcNode == n.id || l.dstNode == n.id; }), g.links.end());
			edit(true);
		}
		ImGui::TextDisabled("Key and value types of the map.\n"
		                    "Iterates in the order keys were first inserted.");
		return true;
	}

	// ── Literal values ───────────────────────────────────────────────────────
	// Dragged widgets report the drag frames as uncommitted and the release as
	// committed, so a host that snapshots for undo gets ONE entry per drag.
	case NT::ConstFloat:
		if (ImGui::DragFloat("Value", &n.f[0], 0.1f)) edit(false);
		if (ImGui::IsItemDeactivatedAfterEdit())      edit(true);
		EditorWidgets::helpForLabel("Value");
		return true;
	case NT::ConstInt:
	{
		int v = (int)n.f[0];
		if (ImGui::DragInt("Value", &v, 1)) { n.f[0] = (float)v; edit(false); }
		if (ImGui::IsItemDeactivatedAfterEdit()) edit(true);
		EditorWidgets::helpForLabel("Value");
		return true;
	}
	case NT::ConstBool:
	{
		bool b = n.f[0] != 0.0f;
		if (EditorWidgets::checkbox("Value", &b)) { n.f[0] = b ? 1.0f : 0.0f; edit(true); }
		return true;
	}
	case NT::ConstString:
		ImGui::InputText("Value", &n.s);
		if (ImGui::IsItemDeactivatedAfterEdit()) edit(true);
		EditorWidgets::helpForLabel("Value");
		return true;
	case NT::ConstVec2:
		if (ImGui::DragFloat2("Value", n.f, 0.1f)) edit(false);
		if (ImGui::IsItemDeactivatedAfterEdit())   edit(true);
		EditorWidgets::helpForLabel("Value");
		return true;
	case NT::ConstColor:
		if (ImGui::ColorEdit4("Value", n.f)) edit(false);
		if (ImGui::IsItemDeactivatedAfterEdit()) edit(true);
		EditorWidgets::helpForLabel("Value");
		return true;

	// ── User-defined types ───────────────────────────────────────────────────
	case NT::ConstEnum:
	{
		drawDefinitionPicker(g, n, /*isStruct=*/false, edit);
		HE::EnumDef def;
		if (!HE::TypeRegistry::instance().getEnum(n.typeName, def)) return true;
		const HE::EnumEntry* cur = def.findValue((int)n.f[0]);
		const bool valOpen = ImGui::BeginCombo("Value", cur ? cur->name.c_str() : "(pick)");
		if (!valOpen) EditorWidgets::helpForLabel("Value");
		if (valOpen)
		{
			for (const auto& e : def.entries)
				if (ImGui::Selectable(e.name.c_str(), cur && cur->name == e.name))
				{ n.f[0] = (float)e.value; edit(true); }
			ImGui::EndCombo();
		}
		return true;
	}
	case NT::GetStructField:
	case NT::SetStructField:
	{
		const bool isGet = n.type == NT::GetStructField;
		drawDefinitionPicker(g, n, /*isStruct=*/true, edit);
		HE::StructDef def;
		if (!HE::TypeRegistry::instance().getStruct(n.typeName, def)) return true;
		const std::string cur = n.params.empty() ? "(pick a field)" : n.params[0].name;
		const bool fieldOpen = ImGui::BeginCombo("Field", cur.c_str());
		if (!fieldOpen) EditorWidgets::helpForLabel("Field");
		if (fieldOpen)
		{
			for (const auto& f : def.fields)
				if (ImGui::Selectable(f.name.c_str(), !n.params.empty() && n.params[0].name == f.name))
				{
					n.params.clear();
					n.params.push_back({ f.name, f.type, f.isArray, f.typeName });
					// The field pin changed type — drop its link. Get retypes its
					// OUTPUT (the value it reads), Set its second INPUT.
					const PinRanges r = pinRanges(n);
					removePinLinks(g, n.id, isGet ? r.dataOut0 : r.dataIn0 + 1);
					edit(true);
				}
			ImGui::EndCombo();
		}
		ImGui::TextDisabled(isGet ? "Reads this one field out of the struct."
		                          : "Outputs a copy with the field replaced.");
		return true;
	}
	case NT::MakeStruct:
	case NT::BreakStruct:
		drawDefinitionPicker(g, n, /*isStruct=*/true, edit);
		return true;
	case NT::SwitchOnEnum:
	case NT::EnumToInt:
	case NT::IntToEnum:
	case NT::EnumToString:
		drawDefinitionPicker(g, n, /*isStruct=*/false, edit);
		return true;

	case NT::FunctionEntry:
	{
		// Calls resolve BY NAME and the first entry wins, so a second function
		// with the same name is dead code that still looks live on the canvas.
		int same = 0;
		for (const auto& o : g.nodes)
			if (o.type == NT::FunctionEntry && o.s == n.s) ++same;
		if (same > 1)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(235, 120, 90, 255));
			ImGui::TextWrapped("Another function is also called \"%s\". Calls always reach "
			                   "the first one — rename this or delete it.", n.s.c_str());
			ImGui::PopStyleColor();
		}
		return false;   // the hosts still draw the name/access rows themselves
	}
	case NT::GetVariable:
	case NT::SetVariable:
	{
		const bool varOpen = ImGui::BeginCombo("Variable", n.s.empty() ? "(none)" : n.s.c_str());
		if (!varOpen) EditorWidgets::helpForLabel("Variable");
		if (varOpen)
		{
			for (const HC::Variable* vp : offerableVariables(g))
			{
				const HC::Variable& v = *vp;
				// A function-local is only usable inside its owning sub-graph.
				if (v.scope != 0 && v.scope != n.subgraph) continue;
				if (ImGui::Selectable(v.name.c_str(), n.s == v.name))
				{
					const PT before = n.propType; const bool wasArr = n.isArray;
					const std::string beforeTn = n.typeName;
					const auto wasCtr = n.container;
					const PT wasKey = n.keyType;
					const std::string beforeKeyTn = n.keyTypeName;
					n.s        = v.name;
					n.propType = v.type;      // node takes the variable's type…
					n.isArray  = v.isArray;   // …and its container-ness…
					n.container = v.container;// …WHICH container…
					n.keyType = v.keyType;    // …and, for a map, its key
					n.keyTypeName = v.keyTypeName;
					n.typeName = v.typeName;  // …and its enum/struct definition
					if (n.propType != before || n.isArray != wasArr || n.typeName != beforeTn ||
					    n.container != wasCtr || n.keyType != wasKey ||
					    n.keyTypeName != beforeKeyTn)
					{
						const PinRanges r = pinRanges(n);
						const int valuePin = n.type == NT::GetVariable ? r.dataOut0 : r.dataIn0;
						removePinLinks(g, n.id, valuePin);
					}
					edit(true);
				}
			}
			ImGui::EndCombo();
		}
		return true;
	}

	case NT::FunctionReturn:
		if (HcEditorUtil::drawReturnFunctionPicker(g, n)) edit(true);
		return true;

	case NT::CallExternal:
	{
		const std::string cls = drawTargetClassRow(h, g, n);
		if (!drawTargetMemberRow(h, g, n, cls, /*functions=*/true))
		{
			ImGui::InputText("Function", &n.s);
			if (ImGui::IsItemDeactivatedAfterEdit()) edit(true);
			EditorWidgets::helpForLabel("Function");
		}
		ImGui::TextDisabled("Calls a public function on the\nTarget instance (a reference).");
		return true;
	}

	case NT::CreateWidget:
	{
		const bool widgetOpen = ImGui::BeginCombo("Widget", n.s.empty() ? "(none)" : n.s.c_str());
		if (!widgetOpen) EditorWidgets::helpForLabel("Widget");
		if (widgetOpen)
		{
			for (const auto& a : HcEditorUtil::listAssets(h.content, HE::AssetType::Widget))
				if (ImGui::Selectable((a.label + "##" + a.path).c_str(), n.s == a.path))
					{ n.s = a.path; edit(true); }
			ImGui::EndCombo();
		}
		ImGui::TextDisabled("Which UI Widget asset to instantiate.\nOutputs the new widget's id.");
		return true;
	}
	case NT::CreateObject:
	{
		const bool classOpen = ImGui::BeginCombo("Class", n.s.empty() ? "(none)" : n.s.c_str());
		if (!classOpen) EditorWidgets::helpForLabel("Class");
		if (classOpen)
		{
			for (const auto& c : HcEditorUtil::listHorizonCodeClasses(h.content))
				if (ImGui::Selectable((c.label + "##" + c.path).c_str(), n.s == c.path))
					{ n.s = c.path; edit(true); }
			ImGui::EndCombo();
		}
		ImGui::TextDisabled("Instantiates a HorizonCode class as a\nlive object. Outputs a reference to it.\n"
		                    "Wire Location/Rotation to place it;\nleave them unwired to keep the\n"
		                    "placement the class was authored with.");
		return true;
	}

	case NT::Cast:
	{
		// Two sections: the engine taxonomy (Entity, the player classes …) and
		// the project's own HorizonCode classes. Both write the SAME field —
		// n.s is one target-key namespace, and an asset path can never collide
		// with an engine name because it always carries a '/' and '.hasset'.
		const std::string cur = n.s.empty() ? std::string("(pick a class)")
		                                    : HcEditorUtil::castTargetLabel(n.s);
		const bool castOpen = ImGui::BeginCombo("Cast to", cur.c_str());
		if (!castOpen) EditorWidgets::helpForLabel("Cast to");
		if (castOpen)
		{
			ImGui::TextDisabled("Engine");
			for (const auto& c : HC::engineClasses())
				if (ImGui::Selectable(c.name, n.s == c.name) && n.s != c.name)
					{ HcEditorUtil::setCastTarget(n, c.name); edit(true); }
			ImGui::Separator();
			ImGui::TextDisabled("Classes");
			for (const auto& c : HcEditorUtil::listHorizonCodeClasses(h.content))
				if (ImGui::Selectable((c.label + "##" + c.path).c_str(), n.s == c.path) &&
				    n.s != c.path)
					{ HcEditorUtil::setCastTarget(n, c.path); edit(true); }
			ImGui::EndCombo();
		}
		ImGui::TextDisabled("Success when Object really is that class\n"
		                    "(or derives from it). The output is only\n"
		                    "valid on the Success branch.");
		return true;
	}

	case NT::GetExternal:
	case NT::SetExternal:
	{
		const std::string cls = drawTargetClassRow(h, g, n);
		if (!drawTargetMemberRow(h, g, n, cls, /*functions=*/false))
		{
			ImGui::InputText("Variable", &n.s);
			if (ImGui::IsItemDeactivatedAfterEdit()) edit(true);
			EditorWidgets::helpForLabel("Variable");
		}
		int t = (int)n.propType;
		if (ImGui::Combo("Type", &t, "Exec\0Float\0Bool\0Int\0String\0Vec2\0Color\0Object\0"))
		{
			const PT nt = (PT)t;
			if (nt != PT::Exec && nt != n.propType)
			{
				n.propType = nt;
				const PinRanges r = pinRanges(n);
				// Set External's value is its SECOND data input (Target is the first).
				const int valuePin = n.type == NT::GetExternal ? r.dataOut0 : (r.dataIn0 + 1);
				removePinLinks(g, n.id, valuePin);
				edit(true);
			}
		}
		EditorWidgets::helpForLabel("Type");
		ImGui::TextDisabled("Reads/writes a public variable on the\nTarget object.");
		return true;
	}

	case NT::EngineCall:
	{
		// Nothing to edit HERE any more: every input of an engine call is set on
		// its pins, and the ones whose values are a list are dropdowns there
		// (see drawPinInlineEditor). What is left is the sentence saying where
		// a particular row's entries come from.
		if (const char* hint = HcEditorUtil::engineParamHint(n))
			ImGui::TextDisabled("%s", hint);
		else
			ImGui::TextDisabled("Engine call — inputs are set on the node's pins.");
		return true;
	}

	default:
		return false;   // host-specific (or nothing to edit) — the frontend decides
	}
}

// ── Quick-pick popup (opened by the G / Shift+G / E shortcuts) ──────────────
// A searchable list at the cursor, drawn in the canvas window right after
// GraphEditor::draw so the popup id matches the OpenPopup the key handler
// issued from inside it.

namespace
{
void drawQuickPickPopup(const Host& h)
{
	if (s_pick.kind == QuickPick::None) return;
	if (!ImGui::IsPopupOpen(kQuickPickPopup)) { s_pick.kind = QuickPick::None; return; }
	if (!ImGui::BeginPopup(kQuickPickPopup)) return;

	HC::Graph& graph = *h.graph;
	const bool wantSet = s_pick.kind == QuickPick::VarSet;

	if (s_pick.kind == QuickPick::EngineApi)
	{
		const std::string q = HcEditorUtil::searchMenuBegin("##qpEngine", "Engine call…", 232.0f);
		ImGui::Separator();
		ImGui::BeginChild("##qpEngineList", ImVec2(240.0f, 300.0f));
		if (std::string picked = HcEditorUtil::drawEngineApiMenu(q); !picked.empty())
		{
			if (const HE::api::ApiFn* fn = HE::api::find(picked))
			{
				const int id = addNode(graph, NT::EngineCall, s_pick.pos, h.currentGraph);
				HC::Node* nn = graph.findNode(id);
				nn->s = fn->id;
				nn->hasArg = fn->isExec;             // exec node vs pure data node
				nn->params.clear(); nn->results.clear();
				for (const auto& p : fn->params)  nn->params.push_back({ p.name, p.type, p.isArray, {} });
				for (const auto& r : fn->results) nn->results.push_back({ r.name, r.type, r.isArray, {} });
				selectNode(h, id);
				h.onEdit(true);
			}
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndChild();
		HcEditorUtil::searchMenuEnd();
	}
	else
	{
		const std::string q = HcEditorUtil::searchMenuBegin(
			"##qpVar", wantSet ? "Set variable…" : "Get variable…", 232.0f);
		ImGui::Separator();
		ImGui::BeginChild("##qpVarList", ImVec2(240.0f, 300.0f));
		bool any = false;
		for (const HC::Variable* vp : offerableVariables(graph))
		{
			const HC::Variable& v = *vp;
			// Locals belong to their own function only.
			if (v.scope != 0 && v.scope != h.currentGraph) continue;
			if (!q.empty() && lower(v.name).find(q) == std::string::npos) continue;
			any = true;
			if (HcEditorUtil::searchMenuItem(v.name + "  \xe2\x80\x94  " + variableTypeLabel(v)))
			{
				const int id = addNode(graph, wantSet ? NT::SetVariable : NT::GetVariable,
				                       s_pick.pos, h.currentGraph);
				HC::Node* nn = graph.findNode(id);
				nn->s = v.name; nn->propType = v.type; nn->isArray = v.isArray;
				// The container half travels with the type — a Get/Set node that
				// kept only `isArray` would draw an ARRAY pin for a set or a map.
				nn->container = v.container; nn->keyType = v.keyType;
				nn->keyTypeName = v.keyTypeName;
				nn->typeName = v.typeName;   // Enum/Struct: WHICH definition (like the add menu)
				selectNode(h, id);
				h.onEdit(true);
				ImGui::CloseCurrentPopup();
			}
		}
		if (!any) ImGui::TextDisabled(graph.variables.empty() ? "(no variables yet)" : "(no match)");
		ImGui::EndChild();
		HcEditorUtil::searchMenuEnd();
	}

	ImGui::EndPopup();
}
} // namespace

// ── Keyboard: node clipboard + duplicate (Cmd on macOS, Ctrl elsewhere) ──────
// Same shortcut set and the same guard the material graph has had since v3 —
// these graphs only ever had Delete, so every node had to be rebuilt by hand.
// HcClipboard is shared by the Level Script / Game Instance / HC Class graphs
// and the widget graph, so nodes copy across HorizonCode editors.

void handleGraphKeys(const Host& h, const ImVec2& canvasOrigin, const ImVec2& avail)
{
	HC::Graph& graph = *h.graph;
	GraphEditor::State& ge = *h.ge;

	drawQuickPickPopup(h);

	const ImGuiIO& kio = ImGui::GetIO();
	const bool mod  = kio.KeyCtrl || kio.KeySuper;
	// IsWindowHovered knows nothing about BeginDisabled — while a collab peer
	// holds the asset's lock the whole tab renders disabled and the canvas
	// already refuses edits, but these shortcuts would still mutate the graph
	// (and the "anything unsaved here is stale" reload would then silently
	// throw the edit away). Same guard the canvas gets for free via hover.
	const bool disabled =
		(ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
	const bool kbOk = !disabled
	                  && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
	                  && !kio.WantTextInput && !ImGui::IsAnyItemActive();
	std::vector<int>& sel = ge.selection;
	// Fall back to the single selected node when no box-selection is active.
	auto effectiveSel = [&]() -> std::vector<int> {
		if (!sel.empty()) return sel;
		return *h.selectedNode != 0 ? std::vector<int>{ *h.selectedNode } : std::vector<int>{};
	};

	if (kbOk && mod && ImGui::IsKeyPressed(ImGuiKey_C))
		HcClipboard::copy(graph, effectiveSel());

	if (kbOk && mod && ImGui::IsKeyPressed(ImGuiKey_X))
	{
		const std::vector<int> doomed = effectiveSel();
		if (HcClipboard::copy(graph, doomed)) // cut = copy + delete
		{
			for (int id : doomed)
				if (const HC::Node* n = graph.findNode(id);
				    n && n->type != NT::Event && n->type != NT::FunctionEntry)
					graph.removeNode(id);
			sel.clear(); ge.selected = 0; *h.selectedNode = 0;
			h.onEdit(true);
		}
	}

	if (kbOk && mod && ImGui::IsKeyPressed(ImGuiKey_V) && !HcClipboard::empty())
	{
		// Paste at the mouse when it is over the canvas, else into its centre.
		const bool over = kio.MousePos.x >= canvasOrigin.x && kio.MousePos.x <= canvasOrigin.x + avail.x &&
		                  kio.MousePos.y >= canvasOrigin.y && kio.MousePos.y <= canvasOrigin.y + avail.y;
		const float Z  = ge.zoom;
		const float gx = ((over ? kio.MousePos.x : canvasOrigin.x + avail.x * 0.5f)
		                  - canvasOrigin.x - ge.pan.x) / Z;
		const float gy = ((over ? kio.MousePos.y : canvasOrigin.y + avail.y * 0.5f)
		                  - canvasOrigin.y - ge.pan.y) / Z;
		const std::vector<int> fresh = HcClipboard::paste(graph, gx, gy, h.currentGraph);
		if (!fresh.empty())
		{
			HC::syncFunctionSignatures(graph); // pasted FunctionCalls re-mirror their entry
			sel = fresh;
			ge.selected = *h.selectedNode = fresh.front();
			h.onEdit(true);
		}
	}

	if (kbOk && mod && ImGui::IsKeyPressed(ImGuiKey_D))
	{
		const std::vector<int> fresh = HC::duplicateNodes(graph, effectiveSel());
		if (!fresh.empty())
		{
			sel = fresh;
			ge.selected = *h.selectedNode = fresh.front();
			h.onEdit(true);
		}
	}
}

} // namespace HcGraphHost
