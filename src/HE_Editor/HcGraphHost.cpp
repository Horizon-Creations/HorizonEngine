#include "HcGraphHost.h"
#include "HcEditorUtil.h"        // HcEditorUtil: colors, tooltips, engine-API menu
#include "HcGraphClipboard.h"    // shared HorizonCode node clipboard (copy/cut/paste)
#include <HorizonScene/EngineApi.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <cctype>
#include <filesystem>

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
		p.isArray = pd.isArray;   // array pins draw as a 2×2 grid
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

std::string uniqueVarName(const HC::Graph& g)
{
	for (int i = 1; i < 1000; ++i)
	{
		const std::string name = i == 1 ? "NewVar" : ("NewVar" + std::to_string(i));
		if (!g.findVariable(name)) return name;
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

std::string lower(std::string v)
{
	std::transform(v.begin(), v.end(), v.begin(),
		[](unsigned char c){ return (char)std::tolower(c); });
	return v;
}

std::string variableTypeLabel(const HC::Variable& v)
{
	// An Object variable shows the class it holds, not a bare "Object" — otherwise
	// every reference variable in the list looks the same.
	return ((v.type == PT::Ref && !v.className.empty())
		? std::filesystem::path(v.className).stem().string()
		: std::string(pinTypeName(v.type))) + (v.isArray ? "[]" : "");
}

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

// ── Canvas model ─────────────────────────────────────────────────────────────

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
	m.drawPinInlineEditor = [&h, &graph](int nid, int pin){
		HC::Node* n = graph.findNode(nid); if (!n) return;
		bool committed = false;
		HcEditorUtil::drawPinDefaultEditor(*n, pin, committed);
		if (committed) h.onEdit(true); };
	// Hovering a node shows what it does + its inputs/outputs.
	m.nodeTooltip = [&graph](int id){
		const HC::Node* n = graph.findNode(id);
		return n ? HcEditorUtil::nodeTooltipText(*n) : std::string(); };
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
		if (ImGui::MenuItem(multi ? "Delete Selection" : "Delete Node"))
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

	// Generic node categories. Which ones a frontend lists is host data: the
	// widget editor has Property/Widget (self) nodes, a level script does not —
	// there the id-based widget nodes under "UI" are the only widget access.
	for (const char* cat : h.menus->addCategories)
	{
		bool header = false;
		for (NT t : HC::nodeRegistry())
		{
			if (listed(h.menus->addExcluded, t)) continue;
			if (std::string(HC::nodeCategory(t)) != cat) continue;
			if (!matches(HC::nodeDisplayName(t), cat)) continue;
			if (!header) { ImGui::TextDisabled("%s", cat); header = true; }
			if (HcEditorUtil::searchMenuItem(HC::nodeDisplayName(t)))
			{ created = addNode(graph, t, drop, h.currentGraph); ImGui::CloseCurrentPopup(); }
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
				ImGui::SetTooltip("%s", HcEditorUtil::nodeTooltipText(t).c_str());
		}
		if (header) ImGui::Spacing();
	}

	// Call <function> for each declared function entry, plus a Return node.
	bool fh = false;
	for (const auto& e : graph.nodes)
	{
		if (e.type != NT::FunctionEntry || e.s.empty()) continue;
		const std::string lbl = "Call " + e.s;
		if (!matches(lbl, "Functions")) continue;
		if (!fh) { ImGui::TextDisabled("Functions"); fh = true; }
		if (HcEditorUtil::searchMenuItem(lbl))
		{
			const int id = addNode(graph, NT::FunctionCall, drop, h.currentGraph);
			graph.findNode(id)->s = e.s;
			HC::syncFunctionSignatures(graph); // mirror the function's pins onto the call
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

	// Engine API calls — the HE::api registry surfaced as one generic
	// EngineCall node per function, grouped by subsystem, same search box.
	if (std::string picked = HcEditorUtil::drawEngineApiMenu(q); !picked.empty())
	{
		if (const HE::api::ApiFn* fn = HE::api::find(picked))
		{
			const int id = addNode(graph, NT::EngineCall, drop, h.currentGraph);
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

	// Get/Set for each declared variable (locals only inside their function).
	bool vh = false;
	for (const auto& v : graph.variables)
		for (int k = 0; k < 2; ++k)
		{
			if (v.scope != 0 && v.scope != h.currentGraph) continue;
			const std::string lbl = (k == 0 ? "Get " : "Set ") + v.name;
			if (!matches(lbl, "Variables")) continue;
			if (!vh) { ImGui::TextDisabled("Variables"); vh = true; }
			if (HcEditorUtil::searchMenuItem(lbl))
			{
				const int id = addNode(graph, k == 0 ? NT::GetVariable : NT::SetVariable,
				                       drop, h.currentGraph);
				HC::Node* nn = graph.findNode(id);
				nn->s = v.name; nn->propType = v.type; nn->isArray = v.isArray;
				created = id; ImGui::CloseCurrentPopup();
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

	const std::string q = HcEditorUtil::searchMenuBegin("##dragSearch", "Search…", 232.0f);
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
		const HC::Graph* cls = resolveClassGraph(*sn, graph, h.giGraph, h.content, scratch);
		if (cls)
		{
			bool fh = false;
			for (const auto& fn : cls->nodes)
				if (fn.type == NT::FunctionEntry && fn.access == 0 && !fn.s.empty() &&
				    matches("Call " + fn.s))
				{
					if (!fh) { ImGui::TextDisabled("Functions"); fh = true; }
					if (HcEditorUtil::searchMenuItem("Call " + fn.s))
					{
						const int id = addNode(graph, NT::CallExternal, pos, h.currentGraph);
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
					if (matches("Get " + var.name) && HcEditorUtil::searchMenuItem("Get " + var.name))
					{ const int id = addNode(graph, NT::GetExternal, pos, h.currentGraph); HC::Node* nn = graph.findNode(id); nn->s = var.name; nn->propType = var.type; wire(id); created = id; ImGui::CloseCurrentPopup(); }
					if (matches("Set " + var.name) && HcEditorUtil::searchMenuItem("Set " + var.name))
					{ const int id = addNode(graph, NT::SetExternal, pos, h.currentGraph); HC::Node* nn = graph.findNode(id); nn->s = var.name; nn->propType = var.type; wire(id); created = id; ImGui::CloseCurrentPopup(); }
				}
			if (fh || vh) ImGui::Separator();
		}
		else ImGui::TextDisabled("(untyped object)");

		ImGui::TextDisabled("Reference");
		auto refItem = [&](const char* lbl, NT t){
			if (matches(lbl) && HcEditorUtil::searchMenuItem(lbl))
			{ const int id = addNode(graph, t, pos, h.currentGraph); wire(id); created = id; ImGui::CloseCurrentPopup(); } };
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
			if (listed(h.menus->dragExcluded, t)) continue;
			const int pin = HcEditorUtil::dragMatchPin(t, dragType, dragArray, srcInput, isExecPin);
			if (pin < 0 || !matches(HC::nodeDisplayName(t))) continue;
			if (!gh) { ImGui::TextDisabled("Nodes"); gh = true; }
			if (HcEditorUtil::searchMenuItem(HC::nodeDisplayName(t)))
			{
				const int id = addNode(graph, t, pos, h.currentGraph);
				HC::Node* nn = graph.findNode(id);
				if (!isExecPin) nn->propType = dragType; // keep the matched signature
				wireAt(id, pin); created = id; ImGui::CloseCurrentPopup();
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
				ImGui::SetTooltip("%s", HcEditorUtil::nodeTooltipText(t).c_str());
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
		for (const auto& v : graph.variables)
		{
			if (v.scope != 0 && v.scope != h.currentGraph) continue;
			const bool setOk = (isExecPin && !srcInput) ||
				(!isExecPin && !srcInput && v.type == dragType && v.isArray == dragArray);
			const bool getOk = !isExecPin && srcInput && v.type == dragType && v.isArray == dragArray;
			auto add = [&](bool get){
				const int id = addNode(graph, get ? NT::GetVariable : NT::SetVariable, pos, h.currentGraph);
				HC::Node* nn = graph.findNode(id);
				nn->s = v.name; nn->propType = v.type; nn->isArray = v.isArray;
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
		for (const auto& e : graph.nodes)
		{
			if (e.type != NT::FunctionEntry || e.s.empty() || !matches("Call " + e.s)) continue;
			if (!fh) { ImGui::TextDisabled("Functions"); fh = true; }
			if (HcEditorUtil::searchMenuItem("Call " + e.s))
			{
				const int id = addNode(graph, NT::FunctionCall, pos, h.currentGraph);
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
	HcEditorUtil::searchMenuEnd();
	return created;
}

// ── Node details: the rows every HorizonCode frontend spells the same ────────
// See the header for exactly which node types this covers and which ones each
// frontend still draws itself.

bool drawCommonNodeDetails(const Host& h, HC::Node& n)
{
	if (!h.graph) return false;
	HC::Graph& g = *h.graph;
	// The hosts differ only in HOW an edit is recorded, which is what onEdit is
	// for: committed = the edit is finished (undo/save point), false = a value is
	// still being dragged.
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

	// ── Literal values ───────────────────────────────────────────────────────
	// Dragged widgets report the drag frames as uncommitted and the release as
	// committed, so a host that snapshots for undo gets ONE entry per drag.
	case NT::ConstFloat:
		if (ImGui::DragFloat("Value", &n.f[0], 0.1f)) edit(false);
		if (ImGui::IsItemDeactivatedAfterEdit())      edit(true);
		return true;
	case NT::ConstInt:
	{
		int v = (int)n.f[0];
		if (ImGui::DragInt("Value", &v, 1)) { n.f[0] = (float)v; edit(false); }
		if (ImGui::IsItemDeactivatedAfterEdit()) edit(true);
		return true;
	}
	case NT::ConstBool:
	{
		bool b = n.f[0] != 0.0f;
		if (ImGui::Checkbox("Value", &b)) { n.f[0] = b ? 1.0f : 0.0f; edit(true); }
		return true;
	}
	case NT::ConstString:
		ImGui::InputText("Value", &n.s);
		if (ImGui::IsItemDeactivatedAfterEdit()) edit(true);
		return true;
	case NT::ConstVec2:
		if (ImGui::DragFloat2("Value", n.f, 0.1f)) edit(false);
		if (ImGui::IsItemDeactivatedAfterEdit())   edit(true);
		return true;
	case NT::ConstColor:
		if (ImGui::ColorEdit4("Value", n.f)) edit(false);
		if (ImGui::IsItemDeactivatedAfterEdit()) edit(true);
		return true;

	case NT::GetVariable:
	case NT::SetVariable:
	{
		if (ImGui::BeginCombo("Variable", n.s.empty() ? "(none)" : n.s.c_str()))
		{
			for (const auto& v : g.variables)
			{
				// A function-local is only usable inside its owning sub-graph.
				if (v.scope != 0 && v.scope != n.subgraph) continue;
				if (ImGui::Selectable(v.name.c_str(), n.s == v.name))
				{
					const PT before = n.propType; const bool wasArr = n.isArray;
					n.s        = v.name;
					n.propType = v.type;      // node takes the variable's type…
					n.isArray  = v.isArray;   // …and its array-ness
					if (n.propType != before || n.isArray != wasArr)
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
		ImGui::InputText("Function", &n.s);
		if (ImGui::IsItemDeactivatedAfterEdit()) edit(true);
		ImGui::TextDisabled("Calls a public function on the\nTarget instance (a reference).");
		return true;

	case NT::CreateWidget:
	{
		if (ImGui::BeginCombo("Widget", n.s.empty() ? "(none)" : n.s.c_str()))
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
		if (ImGui::BeginCombo("Class", n.s.empty() ? "(none)" : n.s.c_str()))
		{
			for (const auto& c : HcEditorUtil::listHorizonCodeClasses(h.content))
				if (ImGui::Selectable((c.label + "##" + c.path).c_str(), n.s == c.path))
					{ n.s = c.path; edit(true); }
			ImGui::EndCombo();
		}
		ImGui::TextDisabled("Instantiates a HorizonCode class as a\nlive object. Outputs a reference to it.");
		return true;
	}

	case NT::GetExternal:
	case NT::SetExternal:
	{
		ImGui::InputText("Variable", &n.s);
		if (ImGui::IsItemDeactivatedAfterEdit()) edit(true);
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
		ImGui::TextDisabled("Reads/writes a public variable on the\nTarget object.");
		return true;
	}

	case NT::EngineCall:
		// scene.load / scene.loadAdditive: choose the scene from a dropdown
		// instead of typing the path (a typo silently fails to load).
		if (HcEditorUtil::drawSceneParamPicker(n, h.content)) edit(true);
		else ImGui::TextDisabled("Engine call — inputs are set on the node's pins.");
		return true;

	default:
		return false;   // host-specific (or nothing to edit) — the frontend decides
	}
}

// ── Keyboard: node clipboard + duplicate (Cmd on macOS, Ctrl elsewhere) ──────
// Same shortcut set and the same guard the material graph has had since v3 —
// these graphs only ever had Delete, so every node had to be rebuilt by hand.
// HcClipboard is shared by the Level Script / Game Instance / HC Class graphs
// and the widget graph, so nodes copy across HorizonCode editors.

void handleClipboardKeys(const Host& h, const ImVec2& canvasOrigin, const ImVec2& avail)
{
	HC::Graph& graph = *h.graph;
	GraphEditor::State& ge = *h.ge;

	const ImGuiIO& kio = ImGui::GetIO();
	const bool mod  = kio.KeyCtrl || kio.KeySuper;
	const bool kbOk = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
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
