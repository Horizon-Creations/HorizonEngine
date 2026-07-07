#include "UIEditorPanel.h"
#include "EditorApplication.h"                 // AppContext
#include "GraphEditor.h"                        // shared node-graph canvas
#include "HcClassList.h"                        // Create Object class picker
#include <UIWidget/UIWidgetTree.h>
#include <UIWidget/UIElement.h>
#include <UIWidget/UIElements.h>
#include <UIWidget/UIWidgetBinding.h>
#include <HorizonCode/HorizonCode.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/HAsset.h>
#include <Types/Enums.h>
#include <Diagnostics/Logger.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace
{
using HE::UIElement;
using HE::UIWidgetTree;
using HE::UIWidgetType;
using HE::UIPropDesc;
using HE::UIPropType;
using HE::UIPropValue;
using HE::UIEventDesc;
namespace HC = HorizonCode;
using NT = HC::NodeType;
using PT = HC::PinType;

// ── Per-widget editor state (session-persistent, keyed by asset path) ─────────
struct State
{
	UIWidgetTree tree;
	HC::Graph    graph;               // HorizonCode logic (Graph mode)
	HE::UUID     assetId{};
	bool         loaded = false;
	bool         dirty  = false;     // unsaved-to-disk edits (tree OR graph)
	int          viewMode = 0;       // 0 = Designer, 1 = Graph
	int          selected = 0;       // Designer: selected element id (0 = none)

	// Designer canvas view: fit-to-window base scale × user zoom, plus pan px.
	float  zoom = 1.0f;
	ImVec2 pan  = ImVec2(0.0f, 0.0f);

	// Designer drag. mode: 0 = none, 1 = move element, 2 = resize element.
	int    dragMode = 0;
	int    resizeHandle = -1;        // 0..7: corners+edges (see handleOffsets)
	ImVec2 dragStartMouse;
	float  dragStartPos[2]  = {};
	float  dragStartSize[2] = {};
	bool   dragDidEdit = false;      // push one undo snapshot per completed drag

	// Graph canvas — the shared GraphEditor component owns pan/zoom/selection/
	// drag; these are the host-side bits it can't own.
	int    selectedGraphNode = 0;
	bool   gFocusSelected = false;   // center on selectedGraphNode next frame
	GraphEditor::State geState;
	int    gDropElem = 0;            // element dragged onto the graph (Get/Set popup)
	bool   gOpenDropPopup = false;   // request to open the element Get/Set popup next frame
	std::string selectedVar;        // graph variable selected in the left panel (editing)
	std::string varNameEdit;        // in-progress rename text for the selected variable
	std::string varNameEditFor;     // which variable varNameEdit currently mirrors
	std::string gDropVar;           // variable dragged onto the graph
	bool   gOpenVarDrop = false;     // request to open the variable Get/Set popup next frame

	// Undo/redo: combined snapshots (treeJson + '\x1f' + graphJson).
	std::vector<std::string> undo;
	int undoPos = -1;

	std::string name;                // filename for the header
	std::string relPath;             // content-root-relative path of this asset
};
std::map<std::string, State> g_states;

// ── Layout math (via UIWidgetTree's shared layout, mirrors the runtime) ───────
ImVec2 anchorPoint(uint8_t a)
{
	static const ImVec2 pts[9] = {
		{0.0f,0.0f},{0.5f,0.0f},{1.0f,0.0f},
		{0.0f,0.5f},{0.5f,0.5f},{1.0f,0.5f},
		{0.0f,1.0f},{0.5f,1.0f},{1.0f,1.0f} };
	return pts[a > 8 ? 0 : a];
}

struct Rect { ImVec2 mn, mx; };

// Element rect in CANVAS units — thin wrapper over the shared layout helper so
// the editor and runtime agree pixel-for-pixel.
Rect elementCanvasRect(const UIWidgetTree& tree, const UIElement& e)
{
	const HE::UIWidgetRect r = HE::uiElementRect(tree, e);
	return { { r.x, r.y }, { r.x + r.w, r.y + r.h } };
}

// Parent rect in CANVAS units (canvas root for parentId 0). Used to place the
// anchor marker and drop point.
Rect parentCanvasRect(const UIWidgetTree& tree, int parentId)
{
	Rect parent{ {0.0f, 0.0f}, { tree.canvasWidth, tree.canvasHeight } };
	if (parentId != 0)
		if (const UIElement* p = tree.find(parentId))
			parent = elementCanvasRect(tree, *p);
	return parent;
}

ImU32 toCol32(const glm::vec4& c)
{
	return IM_COL32(int(c.r * 255.0f), int(c.g * 255.0f),
	                int(c.b * 255.0f), int(c.a * 255.0f));
}

const char* typeName(UIWidgetType t) { return HE::uiWidgetTypeName(t); }

// Element display name (custom name, else type name).
std::string elementName(const UIElement& e)
{
	return e.name.empty() ? std::string(e.typeName()) : e.name;
}

// Generic property read helpers (return sensible fallbacks when absent).
glm::vec4 propColorOr(const UIElement& e, const char* name, const glm::vec4& fb)
{
	const UIPropValue v = e.getProp(name);
	return v.type == UIPropType::Color ? v.col : fb;
}
std::string propStringOr(const UIElement& e, const char* name, const std::string& fb)
{
	const UIPropValue v = e.getProp(name);
	return v.type == UIPropType::String ? v.s : fb;
}
float propFloatOr(const UIElement& e, const char* name, float fb)
{
	const UIPropValue v = e.getProp(name);
	return v.type == UIPropType::Float ? v.f : fb;
}
bool propBoolOr(const UIElement& e, const char* name, bool fb)
{
	const UIPropValue v = e.getProp(name);
	return v.type == UIPropType::Bool ? v.b : fb;
}

// ── Undo (combined tree + graph snapshot; '\x1f' = ASCII Unit Separator) ──────
std::string makeSnapshot(const State& st)
{
	return HE::uiWidgetTreeToJson(st.tree) + '\x1f' + HC::toJson(st.graph);
}

void pushUndo(State& st)
{
	const std::string snap = makeSnapshot(st);
	if (st.undoPos >= 0 && st.undoPos < (int)st.undo.size() && st.undo[st.undoPos] == snap)
		return;
	st.undo.resize(st.undoPos + 1);
	st.undo.push_back(snap);
	if (st.undo.size() > 64) st.undo.erase(st.undo.begin());
	st.undoPos = (int)st.undo.size() - 1;
}

bool restoreSnapshot(State& st, int pos)
{
	if (pos < 0 || pos >= (int)st.undo.size()) return false;
	const std::string& snap = st.undo[pos];
	const size_t sep = snap.find('\x1f');
	HE::uiWidgetTreeFromJson(snap.substr(0, sep), st.tree);
	if (sep != std::string::npos)
		HC::fromJson(snap.substr(sep + 1), st.graph);
	st.undoPos = pos;
	if (st.selected != 0 && !st.tree.find(st.selected)) st.selected = 0;
	if (st.selectedGraphNode != 0 && !st.graph.findNode(st.selectedGraphNode))
		st.selectedGraphNode = 0;
	st.dirty = true;
	return true;
}

// ── Asset IO ───────────────────────────────────────────────────────────────────
void loadState(State& st, AppContext& ctx, const std::string& assetPath)
{
	st.name = std::filesystem::path(assetPath).stem().string();
	if (!ctx.contentManager) return;

	std::error_code ec;
	st.relPath = std::filesystem::relative(
		assetPath, ctx.contentManager->contentRoot(), ec).generic_string();
	if (ec) st.relPath.clear();

	st.assetId = ctx.contentManager->loadAsset(st.relPath);
	if (const UIWidgetAsset* a = ctx.contentManager->getWidget(st.assetId))
	{
		if (!a->treeJson.empty())  HE::uiWidgetTreeFromJson(a->treeJson, st.tree);
		if (!a->graphJson.empty()) HC::fromJson(a->graphJson, st.graph);
	}

	st.loaded = true;
	pushUndo(st);           // baseline snapshot
	st.dirty = false;
}

void saveState(State& st, AppContext& ctx)
{
	if (!ctx.contentManager) return;
	UIWidgetAsset* a = ctx.contentManager->getWidgetMutable(st.assetId);
	if (!a)
	{
		Logger::Log(Logger::LogLevel::Error,
			"UIEditorPanel: widget asset vanished — cannot save");
		return;
	}
	a->treeJson  = HE::uiWidgetTreeToJson(st.tree);
	a->graphJson = HC::toJson(st.graph);
	if (ctx.contentManager->saveAsset(*a)) st.dirty = false;
}

// Keep the live asset in sync on every edit, so PIE picks up unsaved edits too
// (mirrors the material editor's live-apply behavior). Disk write happens on Save.
void applyToAsset(State& st, AppContext& ctx)
{
	if (!ctx.contentManager) return;
	if (UIWidgetAsset* a = ctx.contentManager->getWidgetMutable(st.assetId))
	{
		a->treeJson  = HE::uiWidgetTreeToJson(st.tree);
		a->graphJson = HC::toJson(st.graph);
	}
}

void commitEdit(State& st, AppContext& ctx)
{
	pushUndo(st);
	st.dirty = true;
	applyToAsset(st, ctx);
}

// ── Element factory / placement ─────────────────────────────────────────────────
// Add an element under `parentId`, centered in the parent (or at an explicit
// canvas point when provided). Returns the new element id.
int addElementAt(State& st, UIWidgetType type, int parentId, const ImVec2* canvasPt)
{
	std::unique_ptr<UIElement> e = HE::makeUIElement(type);
	if (!e) return 0;
	e->parentId = parentId;
	e->name = std::string(typeName(type));

	const Rect parent = parentCanvasRect(st.tree, parentId);
	if (canvasPt)
	{
		// anchor TopLeft, pivot 0.5: position = drop point relative to parent TL.
		e->anchor = 0;
		e->posX = canvasPt->x - parent.mn.x;
		e->posY = canvasPt->y - parent.mn.y;
	}
	else
	{
		e->anchor = 4; // MiddleCenter
		e->posX = 0.0f; e->posY = 0.0f;
	}
	return st.tree.add(std::move(e));
}

int duplicateSubtree(State& st, int srcId, int parentId)
{
	const UIElement* src = st.tree.find(srcId);
	if (!src) return 0;
	std::unique_ptr<UIElement> copy = src->clone();
	copy->parentId = parentId;
	if (parentId == src->parentId) { copy->posX += 20.0f; copy->posY += 20.0f; }
	const int newId = st.tree.add(std::move(copy));
	for (int c : st.tree.childrenOf(srcId))
		duplicateSubtree(st, c, newId);
	return newId;
}

// ── Asset-slot widget (drag&drop a content-browser asset of a given type) ─────
// Returns true when the path was changed.
bool assetSlot(AppContext& ctx, const char* label, std::string& path,
               HE::AssetType wantType, const char* idSuffix)
{
	bool changed = false;
	ImGui::TextUnformatted(label);
	ImGui::SameLine(80.0f);
	const std::string shown = path.empty()
		? "(none)" : std::filesystem::path(path).stem().string();
	ImGui::Button((shown + "##" + idSuffix).c_str(),
	              ImVec2(ImGui::GetContentRegionAvail().x - 28.0f, 0));
	if (ImGui::IsItemHovered() && !path.empty()) ImGui::SetTooltip("%s", path.c_str());
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_ASSET_PATH"))
		{
			std::error_code ec;
			std::string rel = std::filesystem::relative(
				static_cast<const char*>(p->Data),
				ctx.contentManager ? ctx.contentManager->contentRoot() : "",
				ec).generic_string();
			if (!ec && !rel.empty() && rel.rfind("..", 0) != 0 && ctx.contentManager)
			{
				const HE::UUID id = ctx.contentManager->loadAsset(rel);
				if (id != HE::UUID{} && ctx.contentManager->assetType(id) == wantType)
				{
					path = rel;
					changed = true;
				}
			}
		}
		ImGui::EndDragDropTarget();
	}
	ImGui::SameLine();
	if (ImGui::SmallButton((std::string("x##clr") + idSuffix).c_str()) && !path.empty())
	{
		path.clear();
		changed = true;
	}
	return changed;
}

// ── Hierarchy panel (recursive tree) ───────────────────────────────────────────
void drawHierarchyNode(State& st, AppContext& ctx, int nodeId, bool& structureEdit)
{
	UIElement* n = st.tree.find(nodeId);
	if (!n) return;

	const auto children = st.tree.childrenOf(nodeId);
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
	                           ImGuiTreeNodeFlags_SpanAvailWidth |
	                           ImGuiTreeNodeFlags_DefaultOpen;
	if (children.empty())      flags |= ImGuiTreeNodeFlags_Leaf;
	if (st.selected == nodeId) flags |= ImGuiTreeNodeFlags_Selected;

	const std::string label = elementName(*n) + "##hn" + std::to_string(nodeId);
	const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
	if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
		st.selected = nodeId;

	// Drag source: reparent by dropping onto another node (or the canvas root).
	if (ImGui::BeginDragDropSource())
	{
		ImGui::SetDragDropPayload("HE_UIWIDGET_NODE", &nodeId, sizeof(int));
		ImGui::TextUnformatted(elementName(*n).c_str());
		ImGui::EndDragDropSource();
	}
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_UIWIDGET_NODE"))
		{
			const int dragged = *static_cast<const int*>(p->Data);
			// Only Panels may contain children.
			if (dragged != nodeId && !st.tree.isDescendantOf(nodeId, dragged) &&
			    n->type() == UIWidgetType::Panel)
			{
				if (UIElement* d = st.tree.find(dragged))
				{
					d->parentId = nodeId;
					structureEdit = true;
				}
			}
		}
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_UIWIDGET_NEW"))
		{
			const int t = *static_cast<const int*>(p->Data);
			// New elements nest under Panels; otherwise share the target's parent.
			const int parent = n->type() == UIWidgetType::Panel ? nodeId : n->parentId;
			st.selected = addElementAt(st, static_cast<UIWidgetType>(t), parent, nullptr);
			structureEdit = true;
		}
		ImGui::EndDragDropTarget();
	}

	// Context menu: delete / duplicate.
	if (ImGui::BeginPopupContextItem((std::string("##hctx") + std::to_string(nodeId)).c_str()))
	{
		if (ImGui::MenuItem("Duplicate"))
		{
			st.selected = duplicateSubtree(st, nodeId, n->parentId);
			structureEdit = true;
		}
		if (ImGui::MenuItem("Delete"))
		{
			st.tree.removeSubtree(nodeId);
			if (st.selected == nodeId) st.selected = 0;
			structureEdit = true;
		}
		ImGui::EndPopup();
	}

	if (open)
	{
		for (int c : children)
			drawHierarchyNode(st, ctx, c, structureEdit);
		ImGui::TreePop();
	}
}

// ── Generic property editor ─────────────────────────────────────────────────────
// Draws one editable widget for a UIPropDesc; reads via getProp, writes via
// setProp. `edit` set on any change this frame (live view), `committed` when an
// edit finished (undo snapshot + live asset).
void drawPropertyWidget(UIElement& e, const UIPropDesc& pd, bool& edit, bool& committed)
{
	const std::string id = "##p_" + pd.name;
	switch (pd.type)
	{
	case UIPropType::Float:
	{
		float v = e.getProp(pd.name).f;
		const bool ranged = pd.minV < pd.maxV;
		const bool ch = ranged
			? ImGui::SliderFloat((pd.name + id).c_str(), &v, pd.minV, pd.maxV)
			: ImGui::DragFloat((pd.name + id).c_str(), &v, 0.5f);
		if (ch) { e.setProp(pd.name, UIPropValue::ofFloat(v)); edit = true; }
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		break;
	}
	case UIPropType::Int:
	{
		int v = e.getProp(pd.name).i;
		if (ImGui::DragInt((pd.name + id).c_str(), &v, 1))
			{ e.setProp(pd.name, UIPropValue::ofInt(v)); edit = true; }
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		break;
	}
	case UIPropType::Bool:
	{
		bool v = e.getProp(pd.name).b;
		if (ImGui::Checkbox((pd.name + id).c_str(), &v))
			{ e.setProp(pd.name, UIPropValue::ofBool(v)); committed = true; }
		break;
	}
	case UIPropType::String:
	{
		std::string v = e.getProp(pd.name).s;
		if (ImGui::InputText((pd.name + id).c_str(), &v))
			{ e.setProp(pd.name, UIPropValue::ofString(v)); edit = true; }
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		break;
	}
	case UIPropType::Color:
	{
		glm::vec4 v = e.getProp(pd.name).col;
		if (ImGui::ColorEdit4((pd.name + id).c_str(), &v.x))
			{ e.setProp(pd.name, UIPropValue::ofColor(v)); edit = true; }
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		break;
	}
	case UIPropType::Vec2:
	{
		glm::vec2 v = e.getProp(pd.name).v2;
		if (ImGui::DragFloat2((pd.name + id).c_str(), &v.x, 0.5f))
			{ e.setProp(pd.name, UIPropValue::ofVec2(v)); edit = true; }
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		break;
	}
	case UIPropType::StringList:
	{
		// Small list editor: one InputText + delete per row, plus an add button.
		ImGui::TextUnformatted(pd.name.c_str());
		std::vector<std::string> list = e.getProp(pd.name).list;
		bool listEdit = false, listCommit = false;
		int removeAt = -1;
		for (int i = 0; i < (int)list.size(); ++i)
		{
			ImGui::PushID(i);
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28.0f);
			if (ImGui::InputText((id + "_row").c_str(), &list[i])) listEdit = true;
			listCommit |= ImGui::IsItemDeactivatedAfterEdit();
			ImGui::SameLine();
			if (ImGui::SmallButton("x")) { removeAt = i; listCommit = true; }
			ImGui::PopID();
		}
		if (removeAt >= 0) list.erase(list.begin() + removeAt);
		if (ImGui::SmallButton((std::string("+") + id + "_add").c_str()))
			{ list.push_back("Item"); listCommit = true; }
		if (listEdit || listCommit)
		{
			UIPropValue v; v.type = UIPropType::StringList; v.list = list;
			e.setProp(pd.name, v);
			if (listEdit) edit = true;
			if (listCommit) committed = true;
		}
		break;
	}
	}
}

// ── Details panel ──────────────────────────────────────────────────────────────
void drawDetails(State& st, AppContext& ctx)
{
	UIElement* n = st.tree.find(st.selected);
	if (!n)
	{
		// Canvas settings when nothing is selected.
		ImGui::TextDisabled("Canvas");
		ImGui::Separator();
		bool edit = false;
		edit |= ImGui::DragFloat("Width",  &st.tree.canvasWidth,  1.0f, 64.0f, 7680.0f);
		edit |= ImGui::DragFloat("Height", &st.tree.canvasHeight, 1.0f, 64.0f, 4320.0f);
		if (edit) { st.dirty = true; }
		if (ImGui::IsItemDeactivatedAfterEdit()) commitEdit(st, ctx);
		ImGui::Spacing();
		ImGui::TextWrapped("Select an element on the canvas or in the hierarchy "
		                   "to edit its properties.");
		return;
	}

	bool edit      = false; // any value changed this frame (live view update)
	bool committed = false; // an edit finished (undo snapshot + live asset)

	ImGui::TextDisabled("%s", n->typeName());
	ImGui::Separator();

	ImGui::InputText("Name", &n->name);
	committed |= ImGui::IsItemDeactivatedAfterEdit();

	// Layout — shared base fields.
	ImGui::SeparatorText("Layout");
	edit |= ImGui::DragFloat2("Position", &n->posX, 1.0f);
	committed |= ImGui::IsItemDeactivatedAfterEdit();
	edit |= ImGui::DragFloat2("Size", &n->sizeX, 1.0f, 1.0f, 10000.0f);
	committed |= ImGui::IsItemDeactivatedAfterEdit();
	edit |= ImGui::DragFloat2("Pivot", &n->pivotX, 0.01f, 0.0f, 1.0f);
	committed |= ImGui::IsItemDeactivatedAfterEdit();

	// Anchor: 3×3 grid of toggle cells, UMG-style.
	ImGui::TextUnformatted("Anchor");
	ImGui::SameLine(80.0f);
	ImGui::BeginGroup();
	for (int row = 0; row < 3; ++row)
	{
		for (int col = 0; col < 3; ++col)
		{
			if (col > 0) ImGui::SameLine();
			const int a = row * 3 + col;
			const bool active = n->anchor == a;
			if (active) ImGui::PushStyleColor(ImGuiCol_Button,
				ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			char id[16]; std::snprintf(id, sizeof id, "##a%d", a);
			if (ImGui::Button(id, ImVec2(18, 18)))
			{
				n->anchor = static_cast<uint8_t>(a);
				committed = true;
			}
			if (active) ImGui::PopStyleColor();
		}
	}
	ImGui::EndGroup();

	int layer = n->layer;
	if (ImGui::DragInt("Layer", &layer, 1)) { n->layer = layer; edit = true; }
	committed |= ImGui::IsItemDeactivatedAfterEdit();
	if (ImGui::Checkbox("Visible", &n->visible)) committed = true;

	// Type-specific properties (generic, driven by properties()).
	const std::vector<UIPropDesc> props = n->properties();
	if (!props.empty())
	{
		ImGui::SeparatorText("Properties");
		for (const UIPropDesc& pd : props)
			drawPropertyWidget(*n, pd, edit, committed);
	}

	// Material slot (only types that expose one — text runs have no quad).
	if (n->hasMaterialSlot())
	{
		ImGui::SeparatorText("Material");
		committed |= assetSlot(ctx, "Material", n->material,
		                       HE::AssetType::Material, "mat");
	}

	if (edit) st.dirty = true;
	if (committed) commitEdit(st, ctx);
}

// Forward-declared so the Designer details can spawn graph event nodes.
void addOrFocusEvent(State& st, AppContext& ctx, const std::string& eventName,
                     const UIEventDesc& desc, int elem);

// Events section shown under the selected element in the Designer — each button
// adds (or focuses) the matching event node in the logic graph and jumps to
// Graph mode (Unreal-style "add event → wire it up in the graph"). Events come
// from the element type's events() list.
void drawDetailsEvents(State& st, AppContext& ctx)
{
	UIElement* n = st.tree.find(st.selected);
	if (!n) return;
	const std::vector<UIEventDesc> evs = n->events();
	if (evs.empty()) return;

	ImGui::SeparatorText("Events");
	for (const UIEventDesc& d : evs)
	{
		const bool exists = [&]{
			for (const auto& g : st.graph.nodes)
				if (g.type == NT::Event && g.s == d.name && g.elem == n->id) return true;
			return false;
		}();
		const std::string label = "+ " + d.name + "##ev";
		if (ImGui::Button(label.c_str(), ImVec2(-1.0f, 0)))
			addOrFocusEvent(st, ctx, d.name, d, n->id);
		if (exists)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("added");
		}
	}
}

// ── Canvas ─────────────────────────────────────────────────────────────────────
// Handle layout: 0..3 corners (TL,TR,BL,BR), 4..7 edges (T,B,L,R).
void handleDelta(int handle, const ImVec2& d, ImVec2& dMin, ImVec2& dMax)
{
	dMin = ImVec2(0, 0); dMax = ImVec2(0, 0);
	switch (handle)
	{
		case 0: dMin = d; break;                          // TL
		case 1: dMin.y = d.y; dMax.x = d.x; break;        // TR
		case 2: dMin.x = d.x; dMax.y = d.y; break;        // BL
		case 3: dMax = d; break;                          // BR
		case 4: dMin.y = d.y; break;                      // T
		case 5: dMax.y = d.y; break;                      // B
		case 6: dMin.x = d.x; break;                      // L
		case 7: dMax.x = d.x; break;                      // R
	}
}

// Draw a simplified WYSIWYG preview of one element from its generic properties.
void drawElementPreview(ImDrawList* dl, const UIElement& n, const ImVec2& mn,
                        const ImVec2& mx, float s)
{
	switch (n.type())
	{
	case UIWidgetType::Panel:
	{
		dl->AddRectFilled(mn, mx, toCol32(propColorOr(n, "Color", { 0.12f,0.12f,0.12f,0.85f })));
		break;
	}
	case UIWidgetType::Image:
	{
		dl->AddRectFilled(mn, mx, toCol32(propColorOr(n, "Tint", { 1,1,1,1 })));
		if (n.material.empty())
		{
			// Placeholder crossed box so an unstyled image is visible.
			dl->AddRect(mn, mx, IM_COL32(160,160,170,120));
			dl->AddLine(mn, mx, IM_COL32(160,160,170,90));
			dl->AddLine(ImVec2(mn.x, mx.y), ImVec2(mx.x, mn.y), IM_COL32(160,160,170,90));
		}
		else
		{
			const std::string matName = std::filesystem::path(n.material).stem().string();
			dl->AddText(nullptr, 12.0f * std::max(0.6f, s),
				ImVec2(mn.x + 3, mn.y + 3), IM_COL32(220,220,230,140), matName.c_str());
		}
		break;
	}
	case UIWidgetType::Text:
	{
		const float fs = propFloatOr(n, "FontSize", 22.0f) * s;
		const std::string txt = propStringOr(n, "Text", "");
		dl->AddText(nullptr, fs, mn, toCol32(propColorOr(n, "Color", { 1,1,1,1 })),
		            txt.empty() ? "(empty)" : txt.c_str());
		break;
	}
	case UIWidgetType::Button:
	{
		dl->AddRectFilled(mn, mx, toCol32(propColorOr(n, "Normal Color", { 0.20f,0.20f,0.20f,1 })), 4.0f * s);
		dl->AddRect(mn, mx, IM_COL32(200,200,210,60), 4.0f * s);
		const std::string txt = propStringOr(n, "Text", "");
		if (!txt.empty())
		{
			const float fs = propFloatOr(n, "FontSize", 20.0f) * s;
			const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, txt.c_str());
			dl->AddText(nullptr, fs,
				ImVec2((mn.x + mx.x - ts.x) * 0.5f, (mn.y + mx.y - ts.y) * 0.5f),
				toCol32(propColorOr(n, "Text Color", { 1,1,1,1 })), txt.c_str());
		}
		break;
	}
	case UIWidgetType::CheckBox:
	{
		// Square box on the left + label to its right.
		const float boxSz = mx.y - mn.y;
		const ImVec2 bmx(mn.x + boxSz, mx.y);
		dl->AddRectFilled(mn, bmx, toCol32(propColorOr(n, "Box Color", { 0.20f,0.20f,0.20f,1 })), 3.0f * s);
		dl->AddRect(mn, bmx, IM_COL32(200,200,210,90), 3.0f * s);
		if (propBoolOr(n, "Checked", false))
		{
			const float pad = boxSz * 0.22f;
			dl->AddRectFilled(ImVec2(mn.x + pad, mn.y + pad), ImVec2(bmx.x - pad, bmx.y - pad),
				toCol32(propColorOr(n, "Check Color", { 0.30f,0.80f,0.40f,1 })), 2.0f * s);
		}
		const std::string lbl = propStringOr(n, "Label", "");
		if (!lbl.empty())
		{
			const float fs = propFloatOr(n, "FontSize", 18.0f) * s;
			dl->AddText(nullptr, fs, ImVec2(bmx.x + 6 * s, (mn.y + mx.y - fs) * 0.5f),
				toCol32(propColorOr(n, "Text Color", { 1,1,1,1 })), lbl.c_str());
		}
		break;
	}
	case UIWidgetType::Slider:
	{
		// Track, fill up to normalized value, round handle at the value.
		const float minV = propFloatOr(n, "Min", 0.0f);
		const float maxV = propFloatOr(n, "Max", 1.0f);
		const float val  = propFloatOr(n, "Value", 0.5f);
		const float span = maxV - minV;
		float t = span > 0.0f ? (val - minV) / span : 0.0f;
		t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
		const float cy = (mn.y + mx.y) * 0.5f;
		const float trackH = std::max(2.0f, (mx.y - mn.y) * 0.28f);
		dl->AddRectFilled(ImVec2(mn.x, cy - trackH * 0.5f), ImVec2(mx.x, cy + trackH * 0.5f),
			toCol32(propColorOr(n, "Track Color", { 0.20f,0.20f,0.20f,1 })), trackH * 0.5f);
		const float hx = mn.x + t * (mx.x - mn.x);
		dl->AddRectFilled(ImVec2(mn.x, cy - trackH * 0.5f), ImVec2(hx, cy + trackH * 0.5f),
			toCol32(propColorOr(n, "Fill Color", { 0.30f,0.60f,0.90f,1 })), trackH * 0.5f);
		dl->AddCircleFilled(ImVec2(hx, cy), std::max(3.0f, (mx.y - mn.y) * 0.4f),
			toCol32(propColorOr(n, "Handle Color", { 0.90f,0.90f,0.90f,1 })));
		break;
	}
	case UIWidgetType::ProgressBar:
	{
		float val = propFloatOr(n, "Value", 0.5f);
		val = val < 0.0f ? 0.0f : (val > 1.0f ? 1.0f : val);
		dl->AddRectFilled(mn, mx, toCol32(propColorOr(n, "Back Color", { 0.15f,0.15f,0.15f,1 })), 3.0f * s);
		dl->AddRectFilled(mn, ImVec2(mn.x + val * (mx.x - mn.x), mx.y),
			toCol32(propColorOr(n, "Fill Color", { 0.30f,0.70f,0.40f,1 })), 3.0f * s);
		break;
	}
	case UIWidgetType::TextInput:
	{
		dl->AddRectFilled(mn, mx, toCol32(propColorOr(n, "Back Color", { 0.10f,0.10f,0.10f,1 })), 3.0f * s);
		dl->AddRect(mn, mx, IM_COL32(200,200,210,70), 3.0f * s);
		const std::string txt = propStringOr(n, "Text", "");
		const bool placeholder = txt.empty();
		const std::string shown = placeholder ? propStringOr(n, "Placeholder", "") : txt;
		if (!shown.empty())
		{
			const float fs = propFloatOr(n, "FontSize", 18.0f) * s;
			dl->AddText(nullptr, fs, ImVec2(mn.x + 4 * s, (mn.y + mx.y - fs) * 0.5f),
				placeholder ? IM_COL32(160,160,170,140)
				            : toCol32(propColorOr(n, "Text Color", { 1,1,1,1 })), shown.c_str());
		}
		break;
	}
	case UIWidgetType::ComboBox:
	{
		dl->AddRectFilled(mn, mx, toCol32(propColorOr(n, "Back Color", { 0.15f,0.15f,0.15f,1 })), 3.0f * s);
		dl->AddRect(mn, mx, IM_COL32(200,200,210,70), 3.0f * s);
		// Current option = Options[Selected Index].
		std::string shown;
		const UIPropValue opts = n.getProp("Options");
		const int idx = n.getProp("Selected Index").i;
		if (opts.type == UIPropType::StringList && idx >= 0 && idx < (int)opts.list.size())
			shown = opts.list[idx];
		const float fs = propFloatOr(n, "FontSize", 18.0f) * s;
		if (!shown.empty())
			dl->AddText(nullptr, fs, ImVec2(mn.x + 4 * s, (mn.y + mx.y - fs) * 0.5f),
				toCol32(propColorOr(n, "Text Color", { 1,1,1,1 })), shown.c_str());
		// Dropdown arrow.
		const float ax = mx.x - (mx.y - mn.y) * 0.5f, ay = (mn.y + mx.y) * 0.5f;
		const float ar = std::max(2.0f, (mx.y - mn.y) * 0.14f);
		dl->AddTriangleFilled(ImVec2(ax - ar, ay - ar * 0.6f), ImVec2(ax + ar, ay - ar * 0.6f),
			ImVec2(ax, ay + ar * 0.6f), IM_COL32(200,200,210,180));
		break;
	}
	default: break;
	}
}

void drawCanvas(State& st, AppContext& ctx, const ImVec2& avail)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 origin = ImGui::GetCursorScreenPos();

	// Invisible button = canvas interaction surface (captures mouse, no move of window).
	ImGui::InvisibleButton("##uicanvas", avail,
		ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
		ImGuiButtonFlags_MouseButtonMiddle);
	const bool hovered = ImGui::IsItemHovered();
	const ImVec2 mouse = ImGui::GetMousePos();

	// Fit scale, then user zoom / pan.
	const float fit = std::min(avail.x / st.tree.canvasWidth,
	                           avail.y / st.tree.canvasHeight) * 0.92f;
	const float s = std::max(0.02f, fit * st.zoom);
	const ImVec2 canvasPx(st.tree.canvasWidth * s, st.tree.canvasHeight * s);
	const ImVec2 cTL(origin.x + (avail.x - canvasPx.x) * 0.5f + st.pan.x,
	                 origin.y + (avail.y - canvasPx.y) * 0.5f + st.pan.y);

	auto toScreen = [&](const ImVec2& c) { return ImVec2(cTL.x + c.x * s, cTL.y + c.y * s); };
	auto toCanvas = [&](const ImVec2& p) { return ImVec2((p.x - cTL.x) / s, (p.y - cTL.y) / s); };

	// Zoom around the mouse; pan with middle/right drag.
	if (hovered)
	{
		const float wheel = ImGui::GetIO().MouseWheel;
		if (wheel != 0.0f)
		{
			const ImVec2 before = toCanvas(mouse);
			st.zoom = std::clamp(st.zoom * (1.0f + wheel * 0.1f), 0.15f, 8.0f);
			const float s2 = std::max(0.02f, fit * st.zoom);
			// keep the canvas point under the cursor fixed
			st.pan.x += mouse.x - (origin.x + (avail.x - st.tree.canvasWidth * s2) * 0.5f
			                       + st.pan.x + before.x * s2);
			st.pan.y += mouse.y - (origin.y + (avail.y - st.tree.canvasHeight * s2) * 0.5f
			                       + st.pan.y + before.y * s2);
		}
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
		    ImGui::IsMouseDragging(ImGuiMouseButton_Right))
		{
			const ImVec2 d = ImGui::GetIO().MouseDelta;
			st.pan.x += d.x; st.pan.y += d.y;
		}
	}

	// Background + canvas rect + grid.
	dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y),
	                  IM_COL32(28, 28, 30, 255));
	dl->AddRectFilled(cTL, ImVec2(cTL.x + canvasPx.x, cTL.y + canvasPx.y),
	                  IM_COL32(12, 12, 14, 255));
	const float grid = 64.0f * s;
	if (grid > 8.0f)
	{
		for (float x = cTL.x; x <= cTL.x + canvasPx.x + 0.5f; x += grid)
			dl->AddLine(ImVec2(x, cTL.y), ImVec2(x, cTL.y + canvasPx.y), IM_COL32(255,255,255,10));
		for (float y = cTL.y; y <= cTL.y + canvasPx.y + 0.5f; y += grid)
			dl->AddLine(ImVec2(cTL.x, y), ImVec2(cTL.x + canvasPx.x, y), IM_COL32(255,255,255,10));
	}
	dl->AddRect(cTL, ImVec2(cTL.x + canvasPx.x, cTL.y + canvasPx.y),
	            IM_COL32(90, 90, 100, 255));

	// Paint order: (layer, depth) ascending — same rule as the runtime.
	struct DrawItem { const UIElement* n; int layer; int depth; Rect r; };
	std::vector<DrawItem> items;
	for (const auto& ep : st.tree.elements)
	{
		const UIElement& n = *ep;
		if (!HE::uiElementEffectiveVisible(st.tree, n)) continue;
		int depth = 0;
		for (const UIElement* c = &n; c->parentId != 0 && depth < 255; ++depth)
		{
			const UIElement* p = st.tree.find(c->parentId);
			if (!p) break;
			c = p;
		}
		items.push_back({ &n, n.layer * 256 + depth, depth, elementCanvasRect(st.tree, n) });
	}
	std::stable_sort(items.begin(), items.end(),
		[](const DrawItem& a, const DrawItem& b){ return a.layer < b.layer; });

	dl->PushClipRect(cTL, ImVec2(cTL.x + canvasPx.x, cTL.y + canvasPx.y), true);
	for (const DrawItem& it : items)
	{
		const ImVec2 mn = toScreen(it.r.mn), mx = toScreen(it.r.mx);
		drawElementPreview(dl, *it.n, mn, mx, s);
	}
	dl->PopClipRect();

	// ── Selection outline, resize handles, anchor marker ─────────────────────
	const float hs = 4.0f; // handle half-size in px
	int hoveredHandle = -1;
	UIElement* sel = st.tree.find(st.selected);
	if (sel)
	{
		const Rect selRect = elementCanvasRect(st.tree, *sel);
		const ImVec2 mn = toScreen(selRect.mn), mx = toScreen(selRect.mx);
		dl->AddRect(mn, mx, IM_COL32(255, 170, 40, 255), 0, 0, 2.0f);

		// Anchor marker inside the parent rect.
		const Rect parent = parentCanvasRect(st.tree, sel->parentId);
		const ImVec2 ap = anchorPoint(sel->anchor);
		const ImVec2 apos = toScreen(ImVec2(
			parent.mn.x + ap.x * (parent.mx.x - parent.mn.x),
			parent.mn.y + ap.y * (parent.mx.y - parent.mn.y)));
		dl->AddCircle(apos, 5.0f, IM_COL32(255, 170, 40, 200), 0, 1.5f);
		dl->AddLine(ImVec2(apos.x - 8, apos.y), ImVec2(apos.x + 8, apos.y), IM_COL32(255,170,40,160));
		dl->AddLine(ImVec2(apos.x, apos.y - 8), ImVec2(apos.x, apos.y + 8), IM_COL32(255,170,40,160));

		// Handles: corners + edge midpoints.
		const ImVec2 hpos[8] = {
			mn, ImVec2(mx.x, mn.y), ImVec2(mn.x, mx.y), mx,
			ImVec2((mn.x+mx.x)*0.5f, mn.y), ImVec2((mn.x+mx.x)*0.5f, mx.y),
			ImVec2(mn.x, (mn.y+mx.y)*0.5f), ImVec2(mx.x, (mn.y+mx.y)*0.5f) };
		for (int i = 0; i < 8; ++i)
		{
			const bool hov = std::abs(mouse.x - hpos[i].x) <= hs + 2 &&
			                 std::abs(mouse.y - hpos[i].y) <= hs + 2;
			if (hov) hoveredHandle = i;
			dl->AddRectFilled(ImVec2(hpos[i].x - hs, hpos[i].y - hs),
			                  ImVec2(hpos[i].x + hs, hpos[i].y + hs),
			                  hov ? IM_COL32(255, 210, 120, 255) : IM_COL32(255, 170, 40, 255));
		}
	}

	// ── Mouse interaction ─────────────────────────────────────────────────────
	auto topmostAt = [&](const ImVec2& canvasPt) -> int
	{
		for (auto it = items.rbegin(); it != items.rend(); ++it)
			if (canvasPt.x >= it->r.mn.x && canvasPt.x <= it->r.mx.x &&
			    canvasPt.y >= it->r.mn.y && canvasPt.y <= it->r.mx.y)
				return it->n->id;
		return 0;
	};

	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		if (sel && hoveredHandle >= 0)
		{
			st.dragMode = 2;
			st.resizeHandle = hoveredHandle;
			st.dragStartMouse = mouse;
			st.dragStartPos[0] = sel->posX;  st.dragStartPos[1] = sel->posY;
			st.dragStartSize[0] = sel->sizeX; st.dragStartSize[1] = sel->sizeY;
			st.dragDidEdit = false;
		}
		else
		{
			const int hit = topmostAt(toCanvas(mouse));
			st.selected = hit;
			if (hit != 0)
			{
				UIElement* n2 = st.tree.find(hit);
				st.dragMode = 1;
				st.dragStartMouse = mouse;
				st.dragStartPos[0] = n2->posX; st.dragStartPos[1] = n2->posY;
				st.dragDidEdit = false;
			}
		}
	}

	if (st.dragMode != 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		UIElement* n2 = st.tree.find(st.selected);
		if (n2)
		{
			const ImVec2 d((mouse.x - st.dragStartMouse.x) / s,
			               (mouse.y - st.dragStartMouse.y) / s);
			if (std::abs(d.x) > 0.01f || std::abs(d.y) > 0.01f) st.dragDidEdit = true;
			if (st.dragMode == 1)
			{
				n2->posX = st.dragStartPos[0] + d.x;
				n2->posY = st.dragStartPos[1] + d.y;
			}
			else if (st.dragMode == 2)
			{
				// Per-axis: dragging the min edge shifts pos by d*(1-pivot) and
				// shrinks size; the max edge grows size and shifts pos by d*pivot.
				ImVec2 dMin, dMax;
				handleDelta(st.resizeHandle, d, dMin, dMax);
				float nx = st.dragStartSize[0] - dMin.x + dMax.x;
				float ny = st.dragStartSize[1] - dMin.y + dMax.y;
				nx = std::max(1.0f, nx); ny = std::max(1.0f, ny);
				n2->sizeX = nx;
				n2->sizeY = ny;
				n2->posX = st.dragStartPos[0] + dMin.x * (1.0f - n2->pivotX) + dMax.x * n2->pivotX;
				n2->posY = st.dragStartPos[1] + dMin.y * (1.0f - n2->pivotY) + dMax.y * n2->pivotY;
			}
			st.dirty = true;
		}
	}
	if (st.dragMode != 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
	{
		if (st.dragDidEdit) commitEdit(st, ctx);
		st.dragMode = 0;
		st.resizeHandle = -1;
	}

	// ── Palette drop onto the canvas (new element; nested when over a panel) ──
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_UIWIDGET_NEW"))
		{
			const int t = *static_cast<const int*>(p->Data);
			const ImVec2 cpt = toCanvas(mouse);
			// Drop into the topmost PANEL under the cursor (containers nest).
			int parent = 0;
			for (auto it = items.rbegin(); it != items.rend(); ++it)
				if (it->n->type() == UIWidgetType::Panel &&
				    cpt.x >= it->r.mn.x && cpt.x <= it->r.mx.x &&
				    cpt.y >= it->r.mn.y && cpt.y <= it->r.mx.y)
					{ parent = it->n->id; break; }
			st.selected = addElementAt(st, static_cast<UIWidgetType>(t), parent, &cpt);
			commitEdit(st, ctx);
		}
		ImGui::EndDragDropTarget();
	}
}

// ═══════════════════════════════════════════════════════════════════════════════
//  Graph mode — HorizonCode visual-scripting editor
// ═══════════════════════════════════════════════════════════════════════════════

constexpr float kGNodeW  = 168.0f;
constexpr float kGTitleH = 22.0f;
constexpr float kGRowH   = 18.0f;
constexpr float kGPinR   = 4.5f;

struct GPinRanges { int execIn0, execOut0, dataIn0, dataOut0, end; };
GPinRanges graphPinRanges(const HC::Node& n)
{
	const HC::NodeSig sig = HC::signatureOf(n);
	GPinRanges r;
	r.execIn0  = 0;
	r.execOut0 = r.execIn0  + (int)sig.execIns.size();
	r.dataIn0  = r.execOut0 + (int)sig.execOuts.size();
	r.dataOut0 = r.dataIn0  + (int)sig.dataIns.size();
	r.end      = r.dataOut0 + (int)sig.dataOuts.size();
	return r;
}

ImU32 graphPinColor(PT t)
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
		case PT::Ref:    return IM_COL32(180, 140, 240, 255);
	}
	return IM_COL32_WHITE;
}

std::string elemLabel(const State& st, int elemId)
{
	if (elemId == 0) return "(Any)";
	const UIElement* e = st.tree.find(elemId);
	if (!e) return "(missing)";
	return elementName(*e);
}

std::string graphNodeTitle(const State& st, const HC::Node& n)
{
	const char* base = HC::nodeDisplayName(n.type);
	switch (n.type)
	{
		case NT::Event:
			return (n.s.empty() ? std::string(base) : n.s) + " [" + elemLabel(st, n.elem) + "]";
		case NT::GetProperty:
		case NT::SetProperty:
			return elemLabel(st, n.elem) + " " + (n.s.empty() ? std::string(base) : n.s);
		case NT::GetVariable:
			return "Get " + (n.s.empty() ? std::string("var") : n.s);
		case NT::SetVariable:
			return "Set " + (n.s.empty() ? std::string("var") : n.s);
		case NT::FunctionEntry:
		case NT::FunctionCall:
			return std::string(base) + " " + n.s;
		case NT::BindEvent:    return "Bind " + (n.s.empty() ? std::string("event") : n.s);
		case NT::EmitEvent:    return "Emit " + (n.s.empty() ? std::string("event") : n.s);
		case NT::CallExternal: return "Call " + (n.s.empty() ? std::string("fn") : n.s) + " (Ref)";
		default:
			return base;
	}
}

struct GPinInfo { ImVec2 pos; bool input; bool isExec; PT type; };
bool graphPinInfo(const HC::Node& n, int unifiedPin, const ImVec2& nodePos,
                  float s, GPinInfo& out)
{
	const GPinRanges r = graphPinRanges(n);
	const HC::NodeSig sig = HC::signatureOf(n);
	const float width  = kGNodeW * s;
	const float titleH = kGTitleH * s, rowH = kGRowH * s;
	auto rowCenter = [&](int row){ return nodePos.y + titleH + (row + 0.5f) * rowH; };

	if (unifiedPin >= r.execIn0 && unifiedPin < r.execOut0)
	{
		const int i = unifiedPin - r.execIn0;
		out = { ImVec2(nodePos.x, rowCenter(i)), true, true, PT::Exec };
		return true;
	}
	if (unifiedPin >= r.execOut0 && unifiedPin < r.dataIn0)
	{
		const int i = unifiedPin - r.execOut0;
		out = { ImVec2(nodePos.x + width, rowCenter(i)), false, true, PT::Exec };
		return true;
	}
	if (unifiedPin >= r.dataIn0 && unifiedPin < r.dataOut0)
	{
		const int i = unifiedPin - r.dataIn0;
		const int row = (int)sig.execIns.size() + i;
		out = { ImVec2(nodePos.x, rowCenter(row)), true, false, sig.dataIns[i].type };
		return true;
	}
	if (unifiedPin >= r.dataOut0 && unifiedPin < r.end)
	{
		const int i = unifiedPin - r.dataOut0;
		const int row = (int)sig.execOuts.size() + i;
		out = { ImVec2(nodePos.x + width, rowCenter(row)), false, false, sig.dataOuts[i].type };
		return true;
	}
	return false;
}

const char* graphPinLabel(const HC::Node& n, int unifiedPin)
{
	const GPinRanges r = graphPinRanges(n);
	const HC::NodeSig sig = HC::signatureOf(n);
	if (unifiedPin >= r.execIn0  && unifiedPin < r.execOut0) return sig.execIns [unifiedPin - r.execIn0 ].name;
	if (unifiedPin >= r.execOut0 && unifiedPin < r.dataIn0)  return sig.execOuts[unifiedPin - r.execOut0].name;
	if (unifiedPin >= r.dataIn0  && unifiedPin < r.dataOut0) return sig.dataIns [unifiedPin - r.dataIn0 ].name;
	if (unifiedPin >= r.dataOut0 && unifiedPin < r.end)      return sig.dataOuts[unifiedPin - r.dataOut0].name;
	return "";
}

std::string uniqueFunctionName(const State& st)
{
	for (int i = 1; i < 1000; ++i)
	{
		const std::string name = i == 1 ? "NewFunction" : ("NewFunction" + std::to_string(i));
		bool taken = false;
		for (const auto& gn : st.graph.nodes)
			if (gn.type == NT::FunctionEntry && gn.s == name) { taken = true; break; }
		if (!taken) return name;
	}
	return "NewFunction";
}

// Display name for a HorizonCode data pin type (used by the variables UI).
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

std::string uniqueVarName(const State& st)
{
	for (int i = 1; i < 1000; ++i)
	{
		const std::string name = i == 1 ? "NewVar" : ("NewVar" + std::to_string(i));
		if (!st.graph.findVariable(name)) return name;
	}
	return "NewVar";
}

int addGraphNode(State& st, NT type, const ImVec2& graphPos)
{
	HC::Node n;
	n.type = type;
	n.x = graphPos.x; n.y = graphPos.y;
	if (type == NT::ConstColor) { n.f[0] = n.f[1] = n.f[2] = n.f[3] = 1.0f; }
	if (type == NT::FunctionEntry) n.s = uniqueFunctionName(st);
	return st.graph.addNode(std::move(n));
}

void removeGraphPinLinks(HC::Graph& g, int nodeId, int pin)
{
	g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
		[&](const HC::Link& l){
			return (l.srcNode == nodeId && l.srcPin == pin) ||
			       (l.dstNode == nodeId && l.dstPin == pin);
		}), g.links.end());
}

// Add or focus an Event node for `elem`/`eventName`; switch to Graph mode.
void addOrFocusEvent(State& st, AppContext& ctx, const std::string& eventName,
                     const UIEventDesc& desc, int elem)
{
	for (auto& gn : st.graph.nodes)
		if (gn.type == NT::Event && gn.s == eventName && gn.elem == elem)
		{
			st.selectedGraphNode = gn.id;
			st.viewMode = 1;
			st.gFocusSelected = true;
			return;
		}
	// Stagger new event nodes so they don't stack exactly.
	int existing = 0;
	for (const auto& gn : st.graph.nodes)
		if (gn.type == NT::Event) ++existing;
	HC::Node n;
	n.type = NT::Event;
	n.s = eventName;
	n.elem = elem;
	n.hasArg = desc.hasArg;
	n.propType = HE::uiPropTypeToPin(desc.argType);
	n.x = 40.0f;
	n.y = 40.0f + existing * 120.0f;
	st.selectedGraphNode = st.graph.addNode(std::move(n));
	st.viewMode = 1;
	st.gFocusSelected = true;
	commitEdit(st, ctx);
}

// ── Graph left panel: element variables + functions ──────────────────────────
void drawGraphVariables(State& st, AppContext& ctx)
{
	// ── Widget elements (drag → Get/Set a UI element property) ────────────────
	ImGui::TextDisabled("Widget Elements");
	ImGui::Separator();
	ImGui::TextWrapped("Drag an element onto the graph to read or write its properties.");
	ImGui::Spacing();

	// Grouped by widget type. Full-width Selectables (no Bullet + ImVec2(-1)
	// combo, which clipped the label to one character).
	static const UIWidgetType kTypeOrder[] = {
		UIWidgetType::Panel, UIWidgetType::Image, UIWidgetType::Text,
		UIWidgetType::Button, UIWidgetType::CheckBox, UIWidgetType::Slider,
		UIWidgetType::ProgressBar, UIWidgetType::TextInput, UIWidgetType::ComboBox,
	};
	for (UIWidgetType t : kTypeOrder)
	{
		bool header = false;
		for (const auto& ep : st.tree.elements)
		{
			const UIElement& n = *ep;
			if (n.type() != t) continue;
			if (!header) { ImGui::TextDisabled("%s", typeName(t)); header = true; }
			ImGui::PushID(n.id);
			if (ImGui::Selectable(elementName(n).c_str(), st.selected == n.id))
				st.selected = n.id;
			if (ImGui::BeginDragDropSource())
			{
				const int eid = n.id;
				ImGui::SetDragDropPayload("HE_UIWGRAPH_ELEM", &eid, sizeof(int));
				ImGui::Text("%s", elementName(n).c_str());
				ImGui::EndDragDropSource();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s — drag to graph for Get/Set", n.typeName());
			ImGui::PopID();
		}
		if (header) ImGui::Spacing();
	}

	// ── Graph variables (user-defined, persistent per running widget) ─────────
	ImGui::Spacing();
	ImGui::TextDisabled("Variables");
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - 24.0f);
	if (ImGui::SmallButton("+##addvar"))
	{
		HC::Variable v;
		v.name = uniqueVarName(st);
		v.type = PT::Float;
		st.graph.variables.push_back(v);
		st.selectedVar = v.name;
		st.selectedGraphNode = 0;
		commitEdit(st, ctx);
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a variable");
	ImGui::Separator();

	for (const auto& v : st.graph.variables)
	{
		const std::string label = v.name + "##v" + v.name;
		if (ImGui::Selectable(label.c_str(), st.selectedVar == v.name))
		{
			st.selectedVar = v.name;
			st.selectedGraphNode = 0; // editing the variable, not a node
		}
		if (ImGui::BeginDragDropSource())
		{
			// Payload = the variable name (fixed-size buffer for stable copy).
			char buf[64] = {};
			std::strncpy(buf, v.name.c_str(), sizeof(buf) - 1);
			ImGui::SetDragDropPayload("HE_UIWGRAPH_VAR", buf, sizeof(buf));
			ImGui::Text("%s", v.name.c_str());
			ImGui::EndDragDropSource();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s — drag to graph for Get/Set", pinTypeName(v.type));
		ImGui::SameLine();
		ImGui::TextDisabled("%s", pinTypeName(v.type));
	}

	// ── Functions ─────────────────────────────────────────────────────────────
	ImGui::Spacing();
	ImGui::TextDisabled("Functions");
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - 24.0f);
	if (ImGui::SmallButton("+##addfn"))
	{
		const int existing = [&]{ int c = 0; for (const auto& g : st.graph.nodes)
			if (g.type == NT::FunctionEntry) ++c; return c; }();
		HC::Node fn;
		fn.type = NT::FunctionEntry;
		fn.s = uniqueFunctionName(st);
		fn.x = 40.0f; fn.y = 40.0f + existing * 120.0f;
		st.selectedGraphNode = st.graph.addNode(std::move(fn));
		st.selectedVar.clear();
		st.gFocusSelected = true;
		commitEdit(st, ctx);
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a function");
	ImGui::Separator();

	for (const auto& gn : st.graph.nodes)
	{
		if (gn.type != NT::FunctionEntry) continue;
		const std::string label = (gn.s.empty() ? "(unnamed)" : gn.s)
			+ "##fn" + std::to_string(gn.id);
		if (ImGui::Selectable(label.c_str(), st.selectedGraphNode == gn.id))
		{
			st.selectedGraphNode = gn.id;
			st.selectedVar.clear();
			st.gFocusSelected = true;
		}
		ImGui::SameLine();
		ImGui::TextDisabled(gn.access == 0 ? "public" : "private");
	}
}

// ── Graph node details (right panel) ─────────────────────────────────────────
void drawGraphNodeDetails(State& st, AppContext& ctx)
{
	HC::Node* n = st.graph.findNode(st.selectedGraphNode);
	if (!n)
	{
		// A graph variable selected in the left panel → edit it here.
		if (HC::Variable* v = !st.selectedVar.empty() ? st.graph.findVariable(st.selectedVar) : nullptr)
		{
			ImGui::TextDisabled("Variable");
			ImGui::Separator();

			// Edit the name through a scratch buffer, NOT v->name directly: the
			// variable is looked up by name (st.selectedVar), and Get/Set nodes
			// reference it by name too. Mutating v->name per keystroke would make
			// findVariable(selectedVar) miss on the next frame — the editor would
			// vanish and only one character would land. Commit the rename atomically
			// on deactivate instead. Re-seed the buffer whenever a different
			// variable is shown.
			if (st.varNameEditFor != v->name)
			{
				st.varNameEdit = v->name;
				st.varNameEditFor = v->name;
			}
			ImGui::InputText("Name", &st.varNameEdit);
			if (ImGui::IsItemDeactivatedAfterEdit())
			{
				const std::string oldName = v->name;
				const std::string nn = st.varNameEdit;
				// Keep it unique + non-empty, and rename the Get/Set nodes using it.
				if (nn.empty() || (nn != oldName && st.graph.findVariable(nn)))
				{
					st.varNameEdit = oldName; // reject clashes / blanks → revert buffer
				}
				else if (nn != oldName)
				{
					v->name = nn;
					for (auto& gn : st.graph.nodes)
						if ((gn.type == NT::GetVariable || gn.type == NT::SetVariable) && gn.s == oldName)
							gn.s = nn;
					st.selectedVar = nn;
					st.varNameEditFor = nn;
					commitEdit(st, ctx);
				}
			}

			int typeIdx = (int)v->type;
			if (ImGui::Combo("Type", &typeIdx, "Exec\0Float\0Bool\0Int\0String\0Vec2\0Color\0"))
			{
				const PT nt = (PT)typeIdx;
				if (nt != PT::Exec && nt != v->type)
				{
					v->type = nt;
					// Retype the Get/Set nodes and drop links that no longer typecheck.
					for (auto& gn : st.graph.nodes)
						if ((gn.type == NT::GetVariable || gn.type == NT::SetVariable) && gn.s == v->name)
						{
							gn.propType = nt;
							const GPinRanges r = graphPinRanges(gn);
							const int valuePin = gn.type == NT::GetVariable ? r.dataOut0 : r.dataIn0;
							removeGraphPinLinks(st.graph, gn.id, valuePin);
						}
					commitEdit(st, ctx);
				}
			}

			int vaccess = v->access;
			if (ImGui::Combo("Access", &vaccess, "Public\0Private\0"))
				{ v->access = vaccess; commitEdit(st, ctx); }

			// Default value editor (seeds the runtime store at widget creation).
			ImGui::SeparatorText("Default");
			bool ed = false;
			switch (v->type)
			{
				case PT::Float:  ed = ImGui::DragFloat("##vdef", &v->f[0], 0.1f); break;
				case PT::Int:  { int iv = (int)v->f[0]; if (ImGui::DragInt("##vdef", &iv)) { v->f[0] = (float)iv; ed = true; } break; }
				case PT::Bool: { bool b = v->f[0] != 0.0f; if (ImGui::Checkbox("##vdef", &b)) { v->f[0] = b ? 1.0f : 0.0f; ed = true; } break; }
				case PT::String: ImGui::InputText("##vdef", &v->s); break;
				case PT::Vec2:   ed = ImGui::DragFloat2("##vdef", v->f, 0.1f); break;
				case PT::Color:  ed = ImGui::ColorEdit4("##vdef", v->f); break;
				default: break;
			}
			if (ed) st.dirty = true;
			if (ImGui::IsItemDeactivatedAfterEdit()) commitEdit(st, ctx);

			ImGui::Spacing();
			ImGui::Separator();
			if (ImGui::Button("Delete Variable"))
			{
				const std::string gone = v->name;
				st.graph.variables.erase(std::remove_if(st.graph.variables.begin(), st.graph.variables.end(),
					[&](const HC::Variable& vv){ return vv.name == gone; }), st.graph.variables.end());
				// Leave orphaned Get/Set nodes in place (harmless; read a default).
				st.selectedVar.clear();
				commitEdit(st, ctx);
			}
			return;
		}

		ImGui::TextDisabled("Graph");
		ImGui::Separator();
		ImGui::TextWrapped("Right-click the canvas to add a node. Drag between pins to "
		                   "connect. Alt+click a pin clears its links. Select a node to "
		                   "edit it here.");
		return;
	}

	bool committed = false;
	ImGui::TextDisabled("%s", HC::nodeDisplayName(n->type));
	ImGui::Separator();

	auto elementCombo = [&](const char* label, bool includeAny)
	{
		const std::string cur = includeAny && n->elem == 0 ? std::string("(Any)")
			: elemLabel(st, n->elem);
		if (ImGui::BeginCombo(label, cur.c_str()))
		{
			if (includeAny && ImGui::Selectable("(Any)", n->elem == 0))
				{ n->elem = 0; committed = true; }
			for (const auto& ep : st.tree.elements)
			{
				const UIElement& e = *ep;
				const std::string nm = elementName(e);
				if (ImGui::Selectable((nm + "##e" + std::to_string(e.id)).c_str(), n->elem == e.id))
					{ n->elem = e.id; committed = true; }
			}
			ImGui::EndCombo();
		}
	};

	switch (n->type)
	{
	case NT::Event:
	{
		elementCombo("Element", /*includeAny=*/true);
		// Event name from the bound element's events() (or free text when Any).
		const UIElement* tgt = st.tree.find(n->elem);
		const std::vector<UIEventDesc> evs = tgt ? tgt->events() : std::vector<UIEventDesc>{};
		if (!evs.empty())
		{
			if (ImGui::BeginCombo("Event", n->s.empty() ? "(none)" : n->s.c_str()))
			{
				for (const UIEventDesc& d : evs)
					if (ImGui::Selectable(d.name.c_str(), n->s == d.name))
					{
						n->s = d.name;
						n->hasArg = d.hasArg;
						n->propType = HE::uiPropTypeToPin(d.argType);
						committed = true;
					}
				ImGui::EndCombo();
			}
		}
		else
		{
			ImGui::InputText("Event", &n->s);
			committed |= ImGui::IsItemDeactivatedAfterEdit();
		}
		ImGui::TextDisabled("Fires when the bound element raises this event.");
		break;
	}

	case NT::GetProperty:
	case NT::SetProperty:
	{
		elementCombo("Element", /*includeAny=*/false);
		const UIElement* tgt = st.tree.find(n->elem);
		const std::vector<UIPropDesc> props = tgt ? tgt->properties() : std::vector<UIPropDesc>{};
		if (ImGui::BeginCombo("Property", n->s.empty() ? "(none)" : n->s.c_str()))
		{
			for (const UIPropDesc& pd : props)
				if (ImGui::Selectable(pd.name.c_str(), n->s == pd.name))
				{
					const PT before = n->propType;
					n->s = pd.name;
					n->propType = HE::uiPropTypeToPin(pd.type);
					// Value-pin type changed → drop links that no longer typecheck.
					if (n->propType != before)
					{
						const GPinRanges r = graphPinRanges(*n);
						const int valuePin = n->type == NT::GetProperty ? r.dataOut0 : r.dataIn0;
						removeGraphPinLinks(st.graph, n->id, valuePin);
					}
					committed = true;
				}
			ImGui::EndCombo();
		}
		break;
	}

	case NT::ConstFloat:
		if (ImGui::DragFloat("Value", &n->f[0], 0.1f)) st.dirty = true;
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		break;
	case NT::ConstInt:
	{
		int v = (int)n->f[0];
		if (ImGui::DragInt("Value", &v, 1)) { n->f[0] = (float)v; st.dirty = true; }
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		break;
	}
	case NT::ConstBool:
	{
		bool b = n->f[0] != 0.0f;
		if (ImGui::Checkbox("Value", &b)) { n->f[0] = b ? 1.0f : 0.0f; committed = true; }
		break;
	}
	case NT::ConstString:
		ImGui::InputText("Value", &n->s);
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		break;
	case NT::ConstVec2:
		if (ImGui::DragFloat2("Value", n->f, 0.1f)) st.dirty = true;
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		break;
	case NT::ConstColor:
		if (ImGui::ColorEdit4("Value", n->f)) st.dirty = true;
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		break;

	case NT::FunctionEntry:
	{
		std::string oldName = n->s;
		ImGui::InputText("Name", &n->s);
		if (ImGui::IsItemDeactivatedAfterEdit())
		{
			// Rename the matching FunctionCall nodes so the wiring stays valid.
			if (!n->s.empty() && n->s != oldName)
				for (auto& c : st.graph.nodes)
					if (c.type == NT::FunctionCall && c.s == oldName) c.s = n->s;
			committed = true;
		}
		int access = n->access;
		if (ImGui::Combo("Access", &access, "Public\0Private\0"))
			{ n->access = access; committed = true; }
		ImGui::TextDisabled(n->access == 0
			? "Callable from scripts via\nhorizon.callWidgetFunction()."
			: "Internal — not script-callable.");
		break;
	}

	case NT::FunctionCall:
	{
		if (ImGui::BeginCombo("Function", n->s.empty() ? "(none)" : n->s.c_str()))
		{
			for (const auto& fn : st.graph.nodes)
				if (fn.type == NT::FunctionEntry)
					if (ImGui::Selectable(fn.s.c_str(), n->s == fn.s))
						{ n->s = fn.s; committed = true; }
			ImGui::EndCombo();
		}
		break;
	}

	case NT::GetVariable:
	case NT::SetVariable:
	{
		if (ImGui::BeginCombo("Variable", n->s.empty() ? "(none)" : n->s.c_str()))
		{
			for (const auto& v : st.graph.variables)
				if (ImGui::Selectable(v.name.c_str(), n->s == v.name))
				{
					const PT before = n->propType;
					n->s = v.name;
					n->propType = v.type;               // node takes the variable's type
					if (n->propType != before)
					{
						const GPinRanges r = graphPinRanges(*n);
						const int valuePin = n->type == NT::GetVariable ? r.dataOut0 : r.dataIn0;
						removeGraphPinLinks(st.graph, n->id, valuePin);
					}
					committed = true;
				}
			ImGui::EndCombo();
		}
		if (n->s.empty()) ImGui::TextDisabled("Pick a variable from the list on the left.");
		break;
	}

	case NT::BindEvent:
	case NT::EmitEvent:
	{
		ImGui::InputText("Event", &n->s);
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		ImGui::TextDisabled(n->type == NT::BindEvent
			? "When Target fires this event, this\nwidget's Event of the same name runs."
			: "Broadcast to everyone bound to this\nwidget's event of this name.");
		break;
	}
	case NT::CallExternal:
	{
		ImGui::InputText("Function", &n->s);
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		ImGui::TextDisabled("Calls a public function on the\nTarget instance (a reference).");
		break;
	}
	case NT::CreateWidget:
	{
		ImGui::InputText("Widget Asset", &n->s);
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		ImGui::TextDisabled("Content-relative path to a UI Widget\nasset. Outputs the new widget's id.");
		break;
	}
	case NT::CreateObject:
	{
		if (ImGui::BeginCombo("Class", n->s.empty() ? "(none)" : n->s.c_str()))
		{
			for (const auto& c : HcEditorUtil::listHorizonCodeClasses(ctx.contentManager))
				if (ImGui::Selectable((c.label + "##" + c.path).c_str(), n->s == c.path))
					{ n->s = c.path; committed = true; }
			ImGui::EndCombo();
		}
		ImGui::TextDisabled("Instantiates a HorizonCode class as a\nlive object. Outputs a reference to it.");
		break;
	}

	default:
		ImGui::TextDisabled("No editable properties.");
		break;
	}

	ImGui::Spacing();
	ImGui::Separator();
	if (ImGui::Button("Delete Node"))
	{
		st.graph.removeNode(n->id);
		st.selectedGraphNode = 0;
		committed = true;
	}

	if (committed) commitEdit(st, ctx);
}

// ── Graph node canvas ────────────────────────────────────────────────────────
// Build the GraphEditor pin list for a HorizonCode node (unified pin index =
// the Pin id; side/exec/type from the node signature).
std::vector<GraphEditor::Pin> hcNodePins(const HC::Node& n)
{
	std::vector<GraphEditor::Pin> out;
	const GPinRanges r = graphPinRanges(n);
	for (int pin = 0; pin < r.end; ++pin)
	{
		GPinInfo info;
		if (!graphPinInfo(n, pin, ImVec2(0, 0), 1.0f, info)) continue; // pos unused here
		GraphEditor::Pin p;
		p.id     = pin;
		p.label  = graphPinLabel(n, pin);
		p.color  = graphPinColor(info.type);
		p.input  = info.input;
		p.isExec = info.isExec;
		out.push_back(std::move(p));
	}
	return out;
}

void drawGraphCanvas(State& st, AppContext& ctx, const ImVec2& avail)
{
	// Sync the host's selection/focus into the shared canvas state.
	st.geState.selected = st.selectedGraphNode;
	if (st.gFocusSelected) { st.geState.focusNode = st.selectedGraphNode; st.gFocusSelected = false; }

	GraphEditor::Model m;
	m.nodeIds = [&st]{ std::vector<int> ids; ids.reserve(st.graph.nodes.size());
		for (const auto& n : st.graph.nodes) ids.push_back(n.id); return ids; };
	m.getPos = [&st](int id, float& x, float& y){ if (const HC::Node* n = st.graph.findNode(id)) { x = n->x; y = n->y; } };
	m.setPos = [&st](int id, float x, float y){ if (HC::Node* n = st.graph.findNode(id)) { n->x = x; n->y = y; } };
	m.title  = [&st](int id){ const HC::Node* n = st.graph.findNode(id); return n ? graphNodeTitle(st, *n) : std::string(); };
	m.headerColor = [&st](int id){ const HC::Node* n = st.graph.findNode(id);
		return GraphEditor::categoryColor(n ? HC::nodeCategory(n->type) : ""); };
	m.pins = [&st](int id){ const HC::Node* n = st.graph.findNode(id);
		return n ? hcNodePins(*n) : std::vector<GraphEditor::Pin>{}; };
	m.links = [&st]{ std::vector<std::array<int,4>> ls; ls.reserve(st.graph.links.size());
		for (const auto& l : st.graph.links) ls.push_back({ l.srcNode, l.srcPin, l.dstNode, l.dstPin }); return ls; };
	m.connect = [&st](int oN, int oP, int iN, int iP){ return st.graph.connect(oN, oP, iN, iP); };
	m.clearPinLinks = [&st](int node, int pin, bool){ removeGraphPinLinks(st.graph, node, pin); };
	m.removeNode = [&st](int id){ st.graph.removeNode(id); };
	// Searchable add-node palette (mirrors the material editor's). Events +
	// FunctionEntry are created elsewhere (Designer events / the + Function
	// button); Get/Set Variable are offered per declared variable.
	m.drawAddMenu = [&st]() -> int {
		int created = 0;
		static std::string s_search;
		if (ImGui::IsWindowAppearing()) { s_search.clear(); ImGui::SetKeyboardFocusHere(); }
		ImGui::SetNextItemWidth(220.0f);
		ImGui::InputTextWithHint("##nodeSearch", "Search nodes...", &s_search);
		ImGui::Separator();
		auto lower = [](std::string v){ std::transform(v.begin(), v.end(), v.begin(),
			[](unsigned char c){ return (char)std::tolower(c); }); return v; };
		const std::string q = lower(s_search);
		auto matches = [&](const std::string& name, const std::string& cat)
		{ return q.empty() || lower(name).find(q) != std::string::npos
		      || lower(cat).find(q) != std::string::npos; };

		ImGui::BeginChild("##nodeList", ImVec2(232.0f, 300.0f));
		static const char* kCats[] = { "Property", "Flow", "Events", "Reference",
		                               "Literals", "Math", "Logic", "String",
		                               "Widget", "UI", "Debug" };
		for (const char* cat : kCats)
		{
			bool header = false;
			for (NT t : HC::nodeRegistry())
			{
				if (t == NT::Event || t == NT::FunctionEntry ||
				    t == NT::GetVariable || t == NT::SetVariable) continue;
				if (std::string(HC::nodeCategory(t)) != cat) continue;
				if (!matches(HC::nodeDisplayName(t), cat)) continue;
				if (!header) { ImGui::TextDisabled("%s", cat); header = true; }
				if (ImGui::Selectable(HC::nodeDisplayName(t)))
				{ created = addGraphNode(st, t, st.geState.addMenuGraphPos); ImGui::CloseCurrentPopup(); }
			}
			if (header) ImGui::Spacing();
		}
		// Get/Set for each declared variable.
		bool vh = false;
		for (const auto& v : st.graph.variables)
			for (int k = 0; k < 2; ++k)
			{
				const std::string lbl = (k == 0 ? "Get " : "Set ") + v.name;
				if (!matches(lbl, "Variables")) continue;
				if (!vh) { ImGui::TextDisabled("Variables"); vh = true; }
				if (ImGui::Selectable(lbl.c_str()))
				{
					const int id = addGraphNode(st, k == 0 ? NT::GetVariable : NT::SetVariable,
					                            st.geState.addMenuGraphPos);
					HC::Node* nn = st.graph.findNode(id);
					nn->s = v.name; nn->propType = v.type;
					created = id; ImGui::CloseCurrentPopup();
				}
			}
		ImGui::EndChild();
		return created;
	};
	m.dropPayloads = { "HE_UIWGRAPH_ELEM", "HE_UIWGRAPH_VAR" };
	m.onDrop = [&st](const char* type, const void* data, ImVec2 gp){
		st.geState.addMenuGraphPos = gp;
		if (std::string(type) == "HE_UIWGRAPH_ELEM")
			{ st.gDropElem = *static_cast<const int*>(data); st.gOpenDropPopup = true; }
		else
			{ st.gDropVar = static_cast<const char*>(data); st.gOpenVarDrop = true; }
	};

	const bool changed = GraphEditor::draw("##hc_graphcanvas", m, st.geState, avail);
	st.selectedGraphNode = st.geState.selected;
	if (changed) commitEdit(st, ctx);

	// Variables-panel drop → Get/Set popup for the dropped element's properties.
	if (st.gOpenDropPopup) { ImGui::OpenPopup("##graph_elem_drop"); st.gOpenDropPopup = false; }
	if (ImGui::BeginPopup("##graph_elem_drop"))
	{
		ImGui::TextDisabled("%s", elemLabel(st, st.gDropElem).c_str());
		ImGui::Separator();
		const UIElement* tgt = st.tree.find(st.gDropElem);
		const std::vector<UIPropDesc> props = tgt ? tgt->properties() : std::vector<UIPropDesc>{};
		auto makePropNode = [&](NT type, const UIPropDesc& pd)
		{
			const int id = addGraphNode(st, type, st.geState.addMenuGraphPos);
			HC::Node* nn = st.graph.findNode(id);
			nn->elem = st.gDropElem;
			nn->s = pd.name;
			nn->propType = HE::uiPropTypeToPin(pd.type);
			st.selectedGraphNode = id;
			commitEdit(st, ctx);
		};
		if (ImGui::BeginMenu("Get", !props.empty()))
		{
			for (const UIPropDesc& pd : props)
				if (ImGui::MenuItem(pd.name.c_str())) makePropNode(NT::GetProperty, pd);
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Set", !props.empty()))
		{
			for (const UIPropDesc& pd : props)
				if (ImGui::MenuItem(pd.name.c_str())) makePropNode(NT::SetProperty, pd);
			ImGui::EndMenu();
		}
		ImGui::EndPopup();
	}

	// Variables-panel drop → Get/Set node for the dropped variable.
	if (st.gOpenVarDrop) { ImGui::OpenPopup("##graph_var_drop"); st.gOpenVarDrop = false; }
	if (ImGui::BeginPopup("##graph_var_drop"))
	{
		const HC::Variable* v = st.graph.findVariable(st.gDropVar);
		ImGui::TextDisabled("%s", st.gDropVar.c_str());
		ImGui::Separator();
		auto makeVarNode = [&](NT type)
		{
			const int id = addGraphNode(st, type, st.geState.addMenuGraphPos);
			HC::Node* nn = st.graph.findNode(id);
			nn->s = st.gDropVar;
			nn->propType = v ? v->type : PT::Float;
			st.selectedGraphNode = id;
			st.selectedVar.clear();
			commitEdit(st, ctx);
		};
		if (ImGui::MenuItem("Get", nullptr, false, v != nullptr)) makeVarNode(NT::GetVariable);
		if (ImGui::MenuItem("Set", nullptr, false, v != nullptr)) makeVarNode(NT::SetVariable);
		ImGui::EndPopup();
	}
}

} // namespace

namespace UIEditorPanel
{

bool isWidgetAsset(const std::string& path)
{
	static std::map<std::string, bool> s_typeCache;
	if (auto it = s_typeCache.find(path); it != s_typeCache.end()) return it->second;
	HAsset::Reader r;
	const bool isW = r.open(path) &&
		r.assetType() == static_cast<uint16_t>(HE::AssetType::Widget);
	s_typeCache[path] = isW;
	return isW;
}

bool isDirty(const std::string& assetPath)
{
	auto it = g_states.find(assetPath);
	return it != g_states.end() && it->second.dirty;
}

void forget(const std::string& assetPath) { g_states.erase(assetPath); }

void render(AppContext& ctx, const std::string& assetPath,
            const ImVec2& pos, const ImVec2& size)
{
	State& st = g_states[assetPath];
	if (!st.loaded) loadState(st, ctx, assetPath);

	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::Begin(("##uiwidget_" + assetPath).c_str(), nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	// ── Header ────────────────────────────────────────────────────────────────
	ImGui::AlignTextToFramePadding();
	ImGui::Text("%s", st.name.c_str());
	ImGui::SameLine();
	ImGui::TextDisabled("UI Widget%s", st.dirty ? "  (unsaved)" : "");

	// Designer | Graph mode toggle (UMG-style).
	ImGui::SameLine(0.0f, 24.0f);
	if (ImGui::RadioButton("Designer", st.viewMode == 0)) st.viewMode = 0;
	ImGui::SameLine();
	if (ImGui::RadioButton("Graph", st.viewMode == 1)) st.viewMode = 1;

	ImGui::SameLine(ImGui::GetContentRegionAvail().x - 150.0f);
	if (ImGui::Button("Reset View"))
	{
		if (st.viewMode == 0) { st.zoom = 1.0f; st.pan = ImVec2(0, 0); }
		else                  { st.geState.zoom = 1.0f; st.geState.pan = ImVec2(60, 60); }
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(!st.dirty);
	if (ImGui::Button("Save")) saveState(st, ctx);
	ImGui::EndDisabled();
	ImGui::Separator();

	// ── Keyboard shortcuts (skip while typing in a field) ────────────────────
	const bool typing = ImGui::IsAnyItemActive();
	if (!typing && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
	{
		const bool ctrl = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
		if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S)) saveState(st, ctx);
		if (ctrl && !ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z))
			{ restoreSnapshot(st, st.undoPos - 1); applyToAsset(st, ctx); }
		if ((ctrl && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) ||
		    (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)))
			{ restoreSnapshot(st, st.undoPos + 1); applyToAsset(st, ctx); }

		const bool del = ImGui::IsKeyPressed(ImGuiKey_Delete) ||
		                 ImGui::IsKeyPressed(ImGuiKey_Backspace);
		if (st.viewMode == 0)
		{
			if (del && st.selected != 0)
			{
				st.tree.removeSubtree(st.selected);
				st.selected = 0;
				commitEdit(st, ctx);
			}
			if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D) && st.selected != 0)
				if (const UIElement* n = st.tree.find(st.selected))
				{
					st.selected = duplicateSubtree(st, st.selected, n->parentId);
					commitEdit(st, ctx);
				}
		}
		else
		{
			if (del && st.selectedGraphNode != 0)
			{
				st.graph.removeNode(st.selectedGraphNode);
				st.selectedGraphNode = 0;
				commitEdit(st, ctx);
			}
		}
	}

	// ── Three-pane layout ─────────────────────────────────────────────────────
	const float leftW  = 230.0f;
	const float rightW = 300.0f;

	if (st.viewMode == 0)
	{
		// ═══ Designer: palette + hierarchy | canvas | element details ═══
		ImGui::BeginChild("##uiw_left", ImVec2(leftW, 0), ImGuiChildFlags_Borders);
		{
			ImGui::TextDisabled("Palette");
			ImGui::Separator();
			for (UIWidgetType t : HE::uiWidgetTypeRegistry())
			{
				// A plain click adds (centered on the canvas or under the selected
				// panel); dragging the button onto the canvas/hierarchy places it there.
				const bool clicked = ImGui::Button(typeName(t), ImVec2(-1.0f, 0));
				if (ImGui::BeginDragDropSource())
				{
					const int ti = static_cast<int>(t);
					ImGui::SetDragDropPayload("HE_UIWIDGET_NEW", &ti, sizeof(int));
					ImGui::TextUnformatted(typeName(t));
					ImGui::EndDragDropSource();
				}
				if (clicked)
				{
					int parent = 0;
					if (const UIElement* selN = st.tree.find(st.selected))
						parent = selN->type() == UIWidgetType::Panel ? selN->id : selN->parentId;
					st.selected = addElementAt(st, t, parent, nullptr);
					commitEdit(st, ctx);
				}
			}

			ImGui::Spacing();
			ImGui::TextDisabled("Hierarchy");
			ImGui::Separator();

			// Canvas root: select-none target + reparent-to-root drop target.
			const bool rootSel = st.selected == 0;
			if (ImGui::Selectable("Canvas##uiwroot", rootSel)) st.selected = 0;
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_UIWIDGET_NODE"))
				{
					const int dragged = *static_cast<const int*>(p->Data);
					if (UIElement* d = st.tree.find(dragged))
					{
						d->parentId = 0;
						commitEdit(st, ctx);
					}
				}
				if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_UIWIDGET_NEW"))
				{
					const int t = *static_cast<const int*>(p->Data);
					st.selected = addElementAt(st, static_cast<UIWidgetType>(t), 0, nullptr);
					commitEdit(st, ctx);
				}
				ImGui::EndDragDropTarget();
			}

			bool structureEdit = false;
			for (int rootId : st.tree.childrenOf(0))
				drawHierarchyNode(st, ctx, rootId, structureEdit);
			if (structureEdit) commitEdit(st, ctx);
		}
		ImGui::EndChild();

		ImGui::SameLine();

		ImGui::BeginChild("##uiw_canvas",
			ImVec2(ImGui::GetContentRegionAvail().x - rightW - ImGui::GetStyle().ItemSpacing.x, 0),
			ImGuiChildFlags_Borders,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		drawCanvas(st, ctx, ImGui::GetContentRegionAvail());
		ImGui::EndChild();

		ImGui::SameLine();

		ImGui::BeginChild("##uiw_details", ImVec2(rightW, 0), ImGuiChildFlags_Borders);
		drawDetails(st, ctx);
		if (st.selected != 0) drawDetailsEvents(st, ctx);
		ImGui::EndChild();
	}
	else
	{
		// ═══ Graph: variables + functions | node canvas | node details ═══
		ImGui::BeginChild("##uiw_gleft", ImVec2(leftW, 0), ImGuiChildFlags_Borders);
		drawGraphVariables(st, ctx);
		ImGui::EndChild();

		ImGui::SameLine();

		ImGui::BeginChild("##uiw_gcanvas",
			ImVec2(ImGui::GetContentRegionAvail().x - rightW - ImGui::GetStyle().ItemSpacing.x, 0),
			ImGuiChildFlags_Borders,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		drawGraphCanvas(st, ctx, ImGui::GetContentRegionAvail());
		ImGui::EndChild();

		ImGui::SameLine();

		ImGui::BeginChild("##uiw_gdetails", ImVec2(rightW, 0), ImGuiChildFlags_Borders);
		drawGraphNodeDetails(st, ctx);
		ImGui::EndChild();
	}

	ImGui::End();
}

} // namespace UIEditorPanel
