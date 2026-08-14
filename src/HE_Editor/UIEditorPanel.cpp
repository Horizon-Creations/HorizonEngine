#include "UIEditorPanel.h"
#include <Types/TypeRegistry.h>
#include "EditorToolbar.h"   // shared toolbar strip

#include <cstdio>
#include <cstdint>
#include "EditorApplication.h"                 // AppContext
#include "EditorAssetTypeCache.h"               // shared, invalidatable path → AssetType sniff
#include "EditorPanelState.h"                   // shared per-tab state map
#include "EditorWidgets.h"                      // shared Content-Browser asset drop target
#include "GraphEditor.h"                        // shared node-graph canvas
#include "HcGraphHost.h"                        // shared HorizonCode canvas host (pins, menus, clipboard)
#include "HcEditorUtil.h"                       // Create Object class picker
#include <HorizonScene/EngineApi.h>             // HE::api registry (Engine Call nodes)
#include <HorizonScene/HcCodegen.h>             // in-editor compile check (Compile button)
#include <UIWidget/UIWidgetTree.h>
#include <UIWidget/UIElement.h>
#include <UIWidget/UIElements.h>
#include <UIWidget/UIWidgetBinding.h>
#include <HorizonCode/HorizonCode.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
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
namespace HGH = HcGraphHost;
using NT = HC::NodeType;
using PT = HC::PinType;

// ── Per-widget editor state (session-persistent, keyed by asset path) ─────────
struct State
{
	UIWidgetTree tree;
	HC::Graph    graph;               // HorizonCode logic (Graph mode)
	// One asset, two documents: the designer's element tree and the logic graph.
	// They share ONE lock (deleting an element breaks the nodes that reference
	// it) but sync separately, so a delta lands in the right one.
	CollabDocSync::DocMirror collabTreeMirror, collabGraphMirror;
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
	int    currentGraph = 0;         // visible sub-graph: 0 = event graph, else a FunctionEntry id
	bool   gFocusSelected = false;   // center on selectedGraphNode next frame
	GraphEditor::State geState;
	int    gDropElem = 0;            // element dragged onto the graph (Get/Set popup)
	bool   gOpenDropPopup = false;   // request to open the element Get/Set popup next frame
	std::string selectedVar;        // graph variable selected in the left panel (editing)
	std::string varNameEdit;        // in-progress rename text for the selected variable
	std::string varNameEditFor;     // which variable varNameEdit currently mirrors
	std::string gDropVar;           // variable dragged onto the graph
	bool   gOpenVarDrop = false;     // request to open the variable Get/Set popup next frame
	std::string gEvtNameEdit;        // scratch buffer for a widget-scope Event name (uniqueness)
	int    gEvtNameEditFor = 0;
	// In-editor compile check (Compile button in the graph header): the last
	// result for THIS widget's graph; an error anchors to a node (red halo).
	bool        compileHas = false;
	bool        compileOk  = false;
	std::string compileMsg;
	int         compileNode = 0;
	double      compileAt  = 0.0;   // when the check ran (ImGui::GetTime)

	// Undo/redo: combined snapshots (treeJson + '\x1f' + graphJson).
	std::vector<std::string> undo;
	int undoPos = -1;

	std::string name;                // filename for the header
	std::string relPath;             // content-root-relative path of this asset
};
AssetPanelState<State> s_states;

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

	st.relPath = ctx.contentManager->toContentRelativePath(assetPath);

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

// Returns false when the write did not happen — the close/quit prompt reports a
// failed save instead of quietly continuing and dropping the edits.
bool saveState(State& st, AppContext& ctx)
{
	if (!ctx.contentManager) return false;
	UIWidgetAsset* a = ctx.contentManager->getWidgetMutable(st.assetId);
	if (!a)
	{
		HE_LOG_ERROR(Editor, "%s",
			"UIEditorPanel: widget asset vanished — cannot save");
		return false;
	}
	a->treeJson  = HE::uiWidgetTreeToJson(st.tree);
	a->graphJson = HC::toJson(st.graph);
	if (!ctx.contentManager->saveAsset(*a)) return false;
	st.dirty = false;
	return true;
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

// ── Asset-slot widget: dropdown over the project's assets of a given type ─────
// A combo listing every matching asset (picked by content-relative path), with
// a "(none)" entry to clear the slot. The combo is also a drag-drop target for
// content-browser assets ("HE_ASSET_PATH"), so both workflows write the same
// content-relative path. Returns true when the path was changed.
bool assetSlot(AppContext& ctx, const char* label, std::string& path,
               HE::AssetType wantType, const char* idSuffix)
{
	bool changed = false;
	ImGui::TextUnformatted(label);
	ImGui::SameLine(80.0f);
	const std::string shown = path.empty()
		? "(none)" : std::filesystem::path(path).stem().string();
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
	if (ImGui::BeginCombo((std::string("##combo_") + idSuffix).c_str(), shown.c_str()))
	{
		if (ImGui::Selectable("(none)", path.empty()) && !path.empty())
		{
			path.clear();
			changed = true;
		}
		for (const auto& a : HcEditorUtil::listAssets(ctx.contentManager, wantType))
			if (ImGui::Selectable((a.label + "##" + a.path).c_str(), path == a.path))
			{
				path    = a.path;
				changed = true;
			}
		ImGui::EndCombo();
	}
	// The tooltip exists because the combo shows only the file STEM — the whole
	// content-relative path is what the hover is for, and paths are the longest
	// single strings this editor ever prints. A tooltip window sizes itself to its
	// widest line, so an unwrapped one turns into a ribbon stretched across the
	// display: nothing is cut off, and it looks as cheap as a sideways scrollbar.
	// The wrap column is given as an absolute x because a tooltip auto-fits its
	// width to its contents — asking it to wrap "at the window's right edge" is
	// asking the text to wrap where the text itself decided to end.
	if (ImGui::IsItemHovered() && !path.empty())
	{
		ImGui::BeginTooltip();
		{
			// The guard must pop while the tooltip is still the current window;
			// running the destructor after EndTooltip would pop the panel's stack
			// instead. Same rule at every guard in this file.
			EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 35.0f);
			ImGui::TextUnformatted(path.c_str());
		}
		ImGui::EndTooltip();
	}
	// Path-valued slot: the widget's own undo (committed by the caller) covers it,
	// so only the drop resolution is shared.
	if (const EditorWidgets::AssetDrop drop = EditorWidgets::acceptAssetDrop(ctx, wantType))
	{
		path    = drop.relPath;
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
		if (EditorWidgets::dangerMenuItem("Delete"))
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
		// Multi-line properties (Text) get a real text box so a newline can be
		// typed — a single-line InputText swallows Enter and the value could
		// never contain one.
		const bool changed = pd.multiline
			? ImGui::InputTextMultiline((pd.name + id).c_str(), &v,
			                            ImVec2(-1.0f, ImGui::GetTextLineHeight() * 4.0f))
			: ImGui::InputText((pd.name + id).c_str(), &v);
		if (changed) { e.setProp(pd.name, UIPropValue::ofString(v)); edit = true; }
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
			if (EditorWidgets::dangerSmallButton("\xc3\x97")) { removeAt = i; listCommit = true; }
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

	// Font slot for text-bearing elements (a "FontSize" property marks them).
	bool hasText = false;
	for (const UIPropDesc& pd : props) if (pd.name == "FontSize") { hasText = true; break; }
	if (hasText)
	{
		ImGui::SeparatorText("Font");
		committed |= assetSlot(ctx, "Font", n->font, HE::AssetType::Font, "font");
		ImGui::TextDisabled("Empty = default UI font.");
	}

	// Pointer interaction: hit-testability + the cursor shown on hover.
	ImGui::SeparatorText("Interaction");
	if (ImGui::Checkbox("Hit-testable", &n->hitTestable)) committed = true;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Off = transparent to the mouse (pointer passes through).");
	if (ImGui::BeginCombo("Hover cursor", HE::uiCursorName(n->hoverCursor)))
	{
		for (int c = 0; c < (int)HE::UICursor::COUNT; ++c)
			if (ImGui::Selectable(HE::uiCursorName((HE::UICursor)c), (int)n->hoverCursor == c))
			{ n->hoverCursor = (HE::UICursor)c; committed = true; }
		ImGui::EndCombo();
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

	// Auto-sizing elements fit themselves before the rects resolve, so the
	// designer shows the same box the runtime will (see uiApplyAutoSize).
	HE::uiApplyAutoSize(st.tree);

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

// Shared with the Level Script / Game Instance / HorizonCode Class editors (see
// HcGraphHost): pin layout, node plumbing, the node menus, the clipboard
// shortcuts, and the node-detail rows every frontend spells identically
// (HcGraphHost::drawCommonNodeDetails).
//
// What is still written out in this file, and why:
//   • the canvas hooks — element-bound node titles, the element/variable drops,
//     the widget's own undo commit;
//   • drawGraphNodeDetails — the VARIABLE editor (there is no HcGraphHost
//     counterpart) plus the node rows that read differently here than in a level
//     script: Event (bound to a UI element), Get/Set Property (widget-only),
//     FunctionEntry, FunctionCall and Bind/Emit Event;
//   • drawGraphVariables — a UI-element browser the other frontends have no use
//     for, plus a variables/functions list laid out differently from theirs.
// Every OTHER node type in drawGraphNodeDetails is shared already; do not add a
// case back here without a reason of the same kind.

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
			return "Get " + elemLabel(st, n.elem) + "." + (n.s.empty() ? std::string("prop") : n.s);
		case NT::SetProperty:
			return "Set " + elemLabel(st, n.elem) + "." + (n.s.empty() ? std::string("prop") : n.s);
		case NT::GetVariable:
			return "Get " + (n.s.empty() ? std::string("var") : n.s);
		case NT::SetVariable:
			return "Set " + (n.s.empty() ? std::string("var") : n.s);
		case NT::FunctionEntry:
		case NT::FunctionCall:
			return std::string(base) + " " + n.s;
		case NT::BindEvent:    return "Bind " + (n.s.empty() ? std::string("event") : n.s);
		case NT::EmitEvent:    return "Emit " + (n.s.empty() ? std::string("event") : n.s);
		case NT::CallExternal: return n.s.empty() ? std::string("Call (Ref)") : ("Call " + n.s);
		case NT::GetExternal:  return n.s.empty() ? std::string("Get (Ref)")  : ("Get " + n.s);
		case NT::SetExternal:  return n.s.empty() ? std::string("Set (Ref)")  : ("Set " + n.s);
		case NT::EngineCall:   return HcEditorUtil::engineCallTitle(n.s);
		case NT::Cast:         return HcEditorUtil::castTitle(n.s);
		default:
			return base;
	}
}

// Add a node into the visible sub-graph (the shared helper, bound to this
// widget's current sub-graph).
int addGraphNode(State& st, NT type, const ImVec2& graphPos)
{
	return HGH::addNode(st.graph, type, graphPos, st.currentGraph);
}

// Which node types this frontend offers: a widget graph has a self-widget
// (Show/Hide Self) and element properties, so it lists the Property and Widget
// categories the level-script editor leaves out.
const HGH::MenuOpts kMenus = {
	/*addCategories*/ { "Property", "Flow", "Events", "Reference",
	                    "Literals", "Math", "Logic", "String",
	                    "Widget", "UI", "Array", "Debug" },
	/*addExcluded*/   { NT::Event, NT::FunctionEntry,
	                    NT::GetVariable, NT::SetVariable },
	/*dragExcluded*/  { NT::Event, NT::FunctionEntry, NT::FunctionCall,
	                    NT::FunctionReturn, NT::GetVariable, NT::SetVariable,
	                    NT::GetProperty, NT::SetProperty, NT::EngineCall,
	                    NT::CallExternal, NT::GetExternal, NT::SetExternal,
	                    NT::BindEvent },
};

// Add or focus an Event node for `elem`/`eventName`; switch to Graph mode.
void addOrFocusEvent(State& st, AppContext& ctx, const std::string& eventName,
                     const UIEventDesc& desc, int elem)
{
	for (auto& gn : st.graph.nodes)
		if (gn.type == NT::Event && gn.s == eventName && gn.elem == elem)
		{
			st.selectedGraphNode = gn.id;
			st.currentGraph = 0;   // events live in the event graph
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
	n.subgraph = 0;   // events live in the event graph
	st.selectedGraphNode = st.graph.addNode(std::move(n));
	st.currentGraph = 0;
	st.viewMode = 1;
	st.gFocusSelected = true;
	commitEdit(st, ctx);
}

// ── Graph left panel: element variables + functions ──────────────────────────
// Deliberately NOT shared with LevelScriptPanel's drawVariables: only the type
// label is (HcGraphHost::variableTypeLabel). The lists agree on what a variable
// IS but not on how it is presented — this one opens with a UI-element browser
// the other frontends have nothing to put in, shows the type as a trailing
// TextDisabled + tooltip instead of inside the row label, and drags a
// "HE_UIWGRAPH_ELEM"/"HE_UIWGRAPH_VAR" payload.
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
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight());
	if (EditorWidgets::addButton("##addvar", "Add a variable"))
	{
		HC::Variable v;
		v.name = HGH::uniqueVarName(st.graph);
		v.type = PT::Float;
		st.graph.variables.push_back(v);
		st.selectedVar = v.name;
		st.selectedGraphNode = 0;
		commitEdit(st, ctx);
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a variable");
	ImGui::Separator();

	auto varRow = [&](const HC::Variable& v)
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
		const std::string typeStr = HGH::variableTypeLabel(v);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s — drag to graph for Get/Set", typeStr.c_str());
		ImGui::SameLine();
		ImGui::TextDisabled("%s", typeStr.c_str());
	};
	for (const auto& v : st.graph.variables)
		if (v.scope == 0) varRow(v);

	// Function-locals of the OPEN function sub-graph: fresh per invocation,
	// usable only inside that function.
	if (st.currentGraph != 0)
	{
		ImGui::Spacing();
		ImGui::TextDisabled("Local Variables");
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight());
		if (EditorWidgets::addButton("##addlvar", "Add a local variable — reset to its default on every call"))
		{
			HC::Variable v;
			v.name = HGH::uniqueVarName(st.graph);
			v.type = PT::Float;
			v.scope = st.currentGraph;   // owned by the open function
			st.graph.variables.push_back(v);
			st.selectedVar = v.name;
			st.selectedGraphNode = 0;
			commitEdit(st, ctx);
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Local to this function — reset to its default on every call.");
		ImGui::Separator();
		for (const auto& v : st.graph.variables)
			if (v.scope == st.currentGraph) varRow(v);
	}

	// ── Graphs: the event graph + one sub-graph per function ──────────────────
	ImGui::Spacing();
	if (ImGui::Selectable("Event Graph", st.currentGraph == 0))
	{ st.currentGraph = 0; st.selectedGraphNode = 0; st.selectedVar.clear(); }

	ImGui::Spacing();
	ImGui::TextDisabled("Functions");
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight());
	if (EditorWidgets::addButton("##addfn", "Add a function"))
	{
		// A function is its own sub-graph: a start (entry) + a Return node.
		HC::Node fn; fn.type = NT::FunctionEntry; fn.s = HGH::uniqueFunctionName(st.graph);
		fn.x = 40.0f; fn.y = 40.0f;
		const int fnId = st.graph.addNode(std::move(fn));
		HC::Node* entry = st.graph.findNode(fnId);
		entry->subgraph = fnId;
		const std::string fnName = entry->s;
		st.currentGraph = fnId;
		const int retId = addGraphNode(st, NT::FunctionReturn, ImVec2(420.0f, 40.0f));
		st.graph.findNode(retId)->s = fnName;
		st.selectedGraphNode = fnId;
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
		if (ImGui::Selectable(label.c_str(), st.currentGraph == gn.id))
		{
			st.currentGraph = gn.id;             // open the function's sub-graph
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
		//
		// STILL DUPLICATED, knowingly: LevelScriptPanel::drawVariableDetails is a
		// near-identical copy. Unlike the node-detail rows below it, this one cannot
		// go through HcGraphHost::Host as it stands — it needs three pieces of
		// per-host scratch state the Host does not model (the name-edit buffer, which
		// variable that buffer belongs to, and the host's selected-variable name).
		// Fixing it properly means giving Host a small "selection" block first.
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

			// Searchable type dropdown: default value types + object (class) types.
			const PT oldType = v->type;
			const std::string oldTypeName = v->typeName;
			if (HcEditorUtil::drawTypePicker("Type", ctx.contentManager, v->type, &v->className, &v->typeName))
			{
				// A different Enum/Struct DEFINITION counts as a type change too.
				if (v->type != oldType || v->typeName != oldTypeName)
				{
					v->defaultItems.clear(); // array slots hold the OLD element type
					v->s.clear();            // enum default entry name belongs to the old enum
					// Retype the Get/Set nodes and drop links that no longer typecheck.
					for (auto& gn : st.graph.nodes)
						if ((gn.type == NT::GetVariable || gn.type == NT::SetVariable) && gn.s == v->name)
						{
							gn.propType = v->type;
							gn.typeName = v->typeName;
							const HGH::PinRanges r = HGH::pinRanges(gn);
							const int valuePin = gn.type == NT::GetVariable ? r.dataOut0 : r.dataIn0;
							HGH::removePinLinks(st.graph, gn.id, valuePin);
						}
				}
				commitEdit(st, ctx);
			}

			// Locals have no access modifier — they are never visible outside
			// their function, let alone through a reference.
			if (v->scope != 0)
			{
				const HC::Node* fn = st.graph.findNode(v->scope);
				ImGui::TextDisabled("Local to: %s", fn && !fn->s.empty() ? fn->s.c_str() : "(function)");
			}
			else
			{
				int vaccess = v->access;
				if (ImGui::Combo("Access", &vaccess, "Public\0Private\0"))
					{ v->access = vaccess; commitEdit(st, ctx); }
			}

			// Single value vs an array of the type. Toggling re-types the matching
			// Get/Set nodes' value pins and drops now-mismatched links.
			bool arr = v->isArray;
			if (ImGui::Checkbox("Array", &arr))
			{
				v->isArray = arr;
				for (auto& gn : st.graph.nodes)
					if ((gn.type == NT::GetVariable || gn.type == NT::SetVariable) && gn.s == v->name)
					{
						gn.isArray = arr;
						const HGH::PinRanges r = HGH::pinRanges(gn);
						HGH::removePinLinks(st.graph, gn.id, gn.type == NT::GetVariable ? r.dataOut0 : r.dataIn0);
					}
				commitEdit(st, ctx);
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hold a list of values instead of a single one.");

			if (!v->isArray)
			{
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
					case PT::Transform:
						ed |= ImGui::DragFloat3("Position##vdef", &v->tpos.x, 0.1f);
						ed |= ImGui::DragFloat3("Rotation##vdef", &v->trot.x, 0.5f);
						ed |= ImGui::DragFloat3("Scale##vdef",    &v->tscl.x, 0.05f);
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
								{ v->s = e.name; ed = true; }
							ImGui::EndCombo();
						}
						break;
					}
					case PT::Struct: break;   // drawn below (own section)
					default: break;
				}
				if (v->type == PT::Struct && HcEditorUtil::drawStructDefaultEditor(*v))
					{ ed = true; commitEdit(st, ctx); }
				if (ed) st.dirty = true;
				if (ImGui::IsItemDeactivatedAfterEdit()) commitEdit(st, ctx);
			}
			else if (HcEditorUtil::drawArrayDefaultEditor(*v)) // slot list seeds the array
			{
				st.dirty = true;
				commitEdit(st, ctx);
			}

			ImGui::Spacing();
			ImGui::Separator();
			if (EditorWidgets::dangerButton("Delete Variable"))
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

	// Rows that read the same in every HorizonCode frontend come from HcGraphHost;
	// the switch below only covers what this editor words differently (see the file
	// header above this section for the full list). onEdit maps the shared rows onto
	// this panel's two-stage edit model: a dragged value only marks the asset dirty,
	// a finished one snapshots for undo.
	HGH::Host common;
	common.graph   = &st.graph;
	common.content = ctx.contentManager;
	common.onEdit  = [&](bool c){ if (c) committed = true; else st.dirty = true; };
	if (HGH::drawCommonNodeDetails(common, *n))
	{
		// The single widget-specific addition to a shared row: this editor's variable
		// list is the left-hand panel, so an unset Get/Set Variable points at it.
		if ((n->type == NT::GetVariable || n->type == NT::SetVariable) && n->s.empty())
			ImGui::TextDisabled("Pick a variable from the list on the left.");
	}
	else switch (n->type)
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
			// Widget-scope (Any element): free text plus the lifecycle events every
			// widget fires — Construct (on create), Tick (per frame, dt arg) and
			// Destruct (on destroy). Unique per element: no two handlers of the same
			// (event, element), and a lifecycle event can't be added twice.
			auto used = [&](const std::string& nm) {
				for (const auto& g : st.graph.nodes)
					if (g.type == NT::Event && g.id != n->id && g.elem == n->elem && g.s == nm)
						return true;
				return false;
			};
			if (st.gEvtNameEditFor != n->id) { st.gEvtNameEdit = n->s; st.gEvtNameEditFor = n->id; }
			ImGui::InputText("Event", &st.gEvtNameEdit);
			if (ImGui::IsItemDeactivatedAfterEdit())
			{
				if (!st.gEvtNameEdit.empty() && used(st.gEvtNameEdit)) st.gEvtNameEdit = n->s; // reject dup
				else { n->s = st.gEvtNameEdit; committed = true; }
			}
			static const char* kLifecycle[] = { "Construct", "Tick", "Destruct" };
			for (int k = 0; k < 3; ++k)
			{
				if (k) ImGui::SameLine();
				const bool u = used(kLifecycle[k]);
				if (u) ImGui::BeginDisabled();
				if (ImGui::SmallButton(kLifecycle[k]))
				{
					n->s = kLifecycle[k]; st.gEvtNameEdit = n->s;
					n->hasArg = (n->s == "Tick");
					n->propType = PT::Float;
					n->elem = 0;
					committed = true;
				}
				if (u) ImGui::EndDisabled();
			}
		}
		ImGui::TextDisabled("Fires when the bound element raises this event.");
		break;
	}

	case NT::GetProperty:
	case NT::SetProperty:
	{
		elementCombo("Element", /*includeAny=*/false);
		const UIElement* tgt = st.tree.find(n->elem);
		// allProperties: type-specific props + the shared base ones (Visible,
		// Hit Testable, Position, Size, Layer, Hover Cursor, Material, Font).
		const std::vector<UIPropDesc> props = tgt ? tgt->allProperties() : std::vector<UIPropDesc>{};
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
						const HGH::PinRanges r = HGH::pinRanges(*n);
						const int valuePin = n->type == NT::GetProperty ? r.dataOut0 : r.dataIn0;
						HGH::removePinLinks(st.graph, n->id, valuePin);
					}
					committed = true;
				}
			ImGui::EndCombo();
		}
		break;
	}

	// Kept here: this editor's access labels and the "who can call this" hint are
	// widget-specific (horizon.callWidgetFunction, not Lua/Python).
	case NT::FunctionEntry:
	{
		std::string oldName = n->s;
		ImGui::InputText("Name", &n->s);
		if (ImGui::IsItemDeactivatedAfterEdit())
		{
			// Rename the matching Call + Return nodes so the wiring stays valid.
			if (!n->s.empty() && n->s != oldName)
				for (auto& c : st.graph.nodes)
					if ((c.type == NT::FunctionCall || c.type == NT::FunctionReturn) && c.s == oldName)
						c.s = n->s;
			committed = true;
		}
		int access = n->access;
		if (ImGui::Combo("Access", &access, "Public\0Private\0"))
			{ n->access = access; committed = true; }
		ImGui::TextDisabled(n->access == 0
			? "Callable from scripts via\nhorizon.callWidgetFunction()."
			: "Internal — not script-callable.");
		HcEditorUtil::drawFunctionInterface(st.graph, *n, committed);
		break;
	}

	// Kept here: this copy lists unnamed functions too, the level-script copy skips
	// them. Same widget otherwise.
	case NT::FunctionCall:
	{
		if (ImGui::BeginCombo("Function", n->s.empty() ? "(none)" : n->s.c_str()))
		{
			for (const auto& fn : st.graph.nodes)
				if (fn.type == NT::FunctionEntry)
					if (ImGui::Selectable(fn.s.c_str(), n->s == fn.s))
						{ n->s = fn.s; HC::syncFunctionSignatures(st.graph); committed = true; }
			ImGui::EndCombo();
		}
		break;
	}

	// Kept here: the hint says "widget's Event" where the level script says
	// "script's Event".
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
	default:
		ImGui::TextDisabled("No editable properties.");
		break;
	}

	ImGui::Spacing();
	ImGui::Separator();
	if (EditorWidgets::dangerButton("Delete Node"))
	{
		st.graph.removeNode(n->id);
		st.selectedGraphNode = 0;
		committed = true;
	}

	if (committed) commitEdit(st, ctx);
}

// ── Graph node canvas ────────────────────────────────────────────────────────
// The canvas itself, the node palette, the drag-off menu and the clipboard
// shortcuts come from HcGraphHost (shared with the Level Script / Game Instance
// / HorizonCode Class editors). Host-specific here: element-bound node titles,
// the widget lifecycle events at the top of the add menu, the element/variable
// drops, and the widget's own undo commit.
void drawGraphCanvas(State& st, AppContext& ctx, const ImVec2& avail)
{
	// Sync the host's selection/focus into the shared canvas state.
	st.geState.selected = st.selectedGraphNode;
	if (st.gFocusSelected) { st.geState.focusNode = st.selectedGraphNode; st.gFocusSelected = false; }
	// Reset a stale sub-graph (e.g. its function was deleted).
	if (st.currentGraph != 0)
	{ const HC::Node* e = st.graph.findNode(st.currentGraph);
	  if (!e || e->type != NT::FunctionEntry) st.currentGraph = 0; }

	HGH::Host host;
	host.graph        = &st.graph;
	host.ge           = &st.geState;
	host.selectedNode = &st.selectedGraphNode;
	host.currentGraph = st.currentGraph;
	host.content      = ctx.contentManager;
	host.giGraph      = ctx.gameInstanceGraph;
	// The last compile check's error node gets a red halo.
	host.errorNode    = (st.compileHas && !st.compileOk) ? st.compileNode : 0;
	host.title        = [&st](const HC::Node& n){ return graphNodeTitle(st, n); };
	// A value still being dragged only marks the tab dirty; a finished edit also
	// pushes an undo snapshot and re-applies the graph to the live asset.
	host.onEdit       = [&st, &ctx](bool committed){
		st.dirty = true;
		if (committed) commitEdit(st, ctx); };
	host.menus        = &kMenus;

	GraphEditor::Model m = HGH::buildModel(host);

	// Searchable add-node palette: the widget lifecycle events + the shared tail
	// (generic node categories + per-function Call + engine API + Get/Set).
	m.drawAddMenu = [&st, &host]() -> int {
		int created = 0;
		const std::string q = HGH::beginAddMenu();
		auto matches = [&](const std::string& name, const std::string& cat)
		{ return q.empty() || HGH::lower(name).find(q) != std::string::npos
		      || HGH::lower(cat).find(q) != std::string::npos; };

		// Widget lifecycle events (Construct on create, Tick per frame, Destruct on
		// destroy) + Custom Event — addable straight from the menu, event graph only,
		// unique per name (element events still come from the Designer).
		if (st.currentGraph == 0)
		{
			auto used = [&st](const std::string& nm){
				for (const auto& gn : st.graph.nodes)
					if (gn.type == NT::Event && gn.elem == 0 && gn.s == nm) return true;
				return false; };
			bool eh = false;
			static const char* kLifecycle[] = { "Construct", "Tick", "Destruct" };
			for (const char* ev : kLifecycle)
			{
				if (!matches(ev, "Events")) continue;
				if (!eh) { ImGui::TextDisabled("Events"); eh = true; }
				const bool u = used(ev);
				if (HcEditorUtil::searchMenuItem(ev, u))
				{
					const int id = addGraphNode(st, NT::Event, st.geState.addMenuGraphPos);
					HC::Node* nn = st.graph.findNode(id);
					nn->s = ev; nn->elem = 0;
					nn->hasArg = std::string(ev) == "Tick";   // Tick carries dt
					nn->propType = PT::Float;
					created = id; ImGui::CloseCurrentPopup();
				}
				if (u) { ImGui::SameLine(); ImGui::TextDisabled("(added)"); }
			}
			if (matches("Custom Event", "Events"))
			{
				if (!eh) { ImGui::TextDisabled("Events"); eh = true; }
				if (HcEditorUtil::searchMenuItem("Custom Event"))
				{ created = addGraphNode(st, NT::Event, st.geState.addMenuGraphPos); ImGui::CloseCurrentPopup(); }
			}
			if (eh) ImGui::Spacing();
		}

		if (const int c = HGH::drawAddMenuTail(host, q)) created = c;
		HGH::endAddMenu();
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

	const ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
	const bool changed = GraphEditor::draw("##hc_graphcanvas", m, st.geState, avail);
	st.selectedGraphNode = st.geState.selected;
	if (changed) commitEdit(st, ctx);
	// Mid-drag: the node has moved, so the document is dirty and a peer should
	// be seeing it. Not commitEdit — that pushes an undo step, and a drag is one
	// step, not one per frame.
	else if (st.geState.liveEdit) st.dirty = true;

	HGH::handleGraphKeys(host, canvasOrigin, avail);

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
		// A function-local can only be placed inside its owning function's graph.
		const bool scopeOk = v && (v->scope == 0 || v->scope == st.currentGraph);
		ImGui::TextDisabled("%s", st.gDropVar.c_str());
		ImGui::Separator();
		auto makeVarNode = [&](NT type)
		{
			const int id = addGraphNode(st, type, st.geState.addMenuGraphPos);
			HC::Node* nn = st.graph.findNode(id);
			nn->s = st.gDropVar;
			nn->propType = v ? v->type : PT::Float;
			nn->isArray = v ? v->isArray : false;
			st.selectedGraphNode = id;
			st.selectedVar.clear();
			commitEdit(st, ctx);
		};
		if (ImGui::MenuItem("Get", nullptr, false, scopeOk)) makeVarNode(NT::GetVariable);
		if (ImGui::MenuItem("Set", nullptr, false, scopeOk)) makeVarNode(NT::SetVariable);
		if (v && !scopeOk)
			ImGui::TextDisabled("Local to another function.");
		ImGui::EndPopup();
	}
}

} // namespace

namespace UIEditorPanel
{

bool isWidgetAsset(const std::string& path)
{
	return EditorAssetTypeCache::is(path, HE::AssetType::Widget);
}

CollabDocSync::DocBindings collabDocs(const std::string& assetPath)
{
	State* st = s_states.find(assetPath);
	if (!st || !st->loaded) return {};
	CollabDocSync::DocBindings out;
	out.push_back({ CollabDocSync::Scope::Primary,
	                CollabDocSync::forUIWidgetTree(st->tree), &st->collabTreeMirror });
	out.push_back({ CollabDocSync::Scope::LogicGraph,
	                CollabDocSync::forHorizonCodeGraph(st->graph), &st->collabGraphMirror });
	return out;
}

bool isDirty(const std::string& assetPath) { return s_states.dirty(assetPath); }

bool reloadFromDisk(const std::string& assetPath)
{
	// A collaboration peer's change just landed in the file. Dropping `loaded`
	// makes the next frame re-read it while the rest of the State — view, camera,
	// undo — survives. The dirty flag is cleared deliberately: while a peer holds
	// the asset's lock this panel is read-only anyway, so anything "unsaved" here
	// is stale, not precious.
	auto* st = s_states.find(assetPath);
	if (!st) return false;
	st->loaded = false;
	st->dirty = false;
	// The mirror describes the document that is about to be replaced. Leaving
	// it would make the first diff after the reload report the difference
	// between the peer's file and our old graph as OUR edit.
	st->collabTreeMirror = {};
	st->collabGraphMirror = {};
	return true;
}


void appendDirtyPaths(std::vector<std::string>& out) { s_states.appendDirtyPaths(out); }
void forget(const std::string& assetPath) { s_states.forget(assetPath); }

bool save(AppContext& ctx, const std::string& assetPath)
{
	State* st = s_states.find(assetPath);
	// A tab this panel never opened has nothing to write — the caller asks every
	// panel about every path, so "not mine" must read as success.
	if (!st || !st->dirty) return true;
	return saveState(*st, ctx);
}

void render(AppContext& ctx, const std::string& assetPath,
            const ImVec2& pos, const ImVec2& size)
{
	State& st = s_states[assetPath];
	if (!st.loaded) loadState(st, ctx, assetPath);

	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::Begin(("##uiwidget_" + assetPath).c_str(), nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	// ── Toolbar ───────────────────────────────────────────────────────────────
	{
		namespace T = EditorToolbar;
		T::Bar bar;
		T::assetHeader(bar, st.name.c_str(), T::iconWidget, st.dirty);

		// Designer | Graph, the UMG split. Two radio buttons became a segmented
		// pair in one well: they are one choice, and the well is what says so.
		bar.group();
		if (bar.item("##uidesigner", T::iconWidget, "Designer", st.viewMode == 0, true,
		             "Lay the widget out"))
		{
			st.viewMode = 0;
		}
		if (bar.item("##uigraph", T::iconCode, "Graph", st.viewMode == 1, true,
		             "The widget\'s HorizonCode logic"))
		{
			st.viewMode = 1;
		}
		bar.endGroup();

		bar.group();
		if (bar.item("##uifit", T::iconFit, nullptr, false, true,
		             "Reset the view (zoom and pan)"))
		{
			if (st.viewMode == 0) { st.zoom = 1.0f; st.pan = ImVec2(0, 0); }
			else                  { st.geState.zoom = 1.0f; st.geState.pan = ImVec2(60, 60); }
		}
		bar.endGroup();

		if (T::saveButton(bar, st.dirty)) saveState(st, ctx);
	}

	// ── Keyboard shortcuts (skip while typing in a field) ────────────────────
	// WantTextInput as well as IsAnyItemActive: an InputText that has keyboard
	// focus without being "active" this frame still owns the keystrokes.
	const bool typing = ImGui::IsAnyItemActive() || ImGui::GetIO().WantTextInput;
	if (!typing && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
	{
		const bool ctrl = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
		if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S)) saveState(st, ctx);
		if (ctrl && !ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z))
			{ restoreSnapshot(st, st.undoPos - 1); applyToAsset(st, ctx); }
		if ((ctrl && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) ||
		    (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)))
			{ restoreSnapshot(st, st.undoPos + 1); applyToAsset(st, ctx); }

		// Delete only — Backspace is the text-editing key and a near-miss on it
		// used to destroy the selected element/node (same rule as GraphEditor).
		const bool del = ImGui::IsKeyPressed(ImGuiKey_Delete);
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
		{
			// No wrap guard on this pane, and the reason is worth writing down
			// because the obvious reading of "230 px against long element and
			// variable names" says there should be one. Those names are drawn with
			// Selectable, and Selectable does not consult the wrap position at all:
			// it measures with CalcTextSize(wrap_width = -1) and renders through
			// RenderTextClipped, so it clips whether one is pushed or not. The one
			// line here that IS a sentence — "Drag an element onto the graph…" —
			// uses TextWrapped, which wraps itself.
			//
			// What a pushed wrap position DID reach was the trailing TextDisabled
			// after each row: the variable's type ("Struct InventoryItem") and a
			// function's "public"/"private", both placed with SameLine after a
			// name of unknown length. Whatever the name leaves over is all they
			// have to wrap in, so a long name pushed the type onto a second line — and
			// with a truly long one, ImGui's one-pixel minimum wrap width puts a
			// couple of characters per line and the row becomes a column. The type
			// is in the row's hover tooltip as well, so clipping it costs the
			// reader nothing the pane was not already offering.
			drawGraphVariables(st, ctx);
		}
		ImGui::EndChild();

		ImGui::SameLine();

		ImGui::BeginChild("##uiw_gcanvas",
			ImVec2(ImGui::GetContentRegionAvail().x - rightW - ImGui::GetStyle().ItemSpacing.x, 0),
			ImGuiChildFlags_Borders,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		// Which sub-graph is shown, the compile result, and the check itself —
		// the canvas gets its own strip, same as the HorizonCode class tab.
		namespace T = EditorToolbar;
		std::string uiWhere = "Event Graph";
		if (st.currentGraph != 0)
		{
			const HC::Node* e = st.graph.findNode(st.currentGraph);
			uiWhere = std::string("Function: ") +
			          (e && !e->s.empty() ? e->s.c_str() : "(unnamed)");
		}
		T::Bar uiBar;
		uiBar.group();
		uiBar.readout(st.currentGraph == 0 ? T::iconList : T::iconCode, uiWhere.c_str());
		uiBar.endGroup();
		// Success fades after a few seconds; an error stays until it is fixed
		// or the next check — same reasoning as the class graph's strip.
		const bool showCompile = st.compileHas &&
			(!st.compileOk || ImGui::GetTime() - st.compileAt < 6.0);
		if (showCompile)
		{
			uiBar.group();
			uiBar.readout(st.compileOk ? T::iconCheck : T::iconWarning,
			              st.compileMsg.c_str(), st.compileOk ? T::kGood : T::kBad);
			uiBar.endGroup();
		}
		uiBar.rightGroup(uiBar.labelGroupWidth({ "Compile" }));
		const bool uiCompile = uiBar.item("##uicompile", T::iconHammer, "Compile", false, true,
			"Translate this widget\'s script to C++ the way a packaged export\n"
			"would. Errors highlight the offending node; a clean result means\n"
			"the script ships compiled (otherwise it runs interpreted).");
		uiBar.endGroup();
		if (uiCompile)
		{
			// The single-class check a packaged export would run (JSON round
			// trip, then generate); key = the content-relative asset path.
			HE::hccg::ClassSource src;
			src.key   = st.relPath.empty() ? st.name : st.relPath;
			src.label = st.name;
			HorizonCode::fromJson(HorizonCode::toJson(st.graph), src.graph);
			const HE::hccg::Result res = HE::hccg::generate({ src }, {});
			st.compileHas  = true;
			st.compileAt   = ImGui::GetTime();
			st.compileNode = 0;
			if (!res.fallbacks.empty())
			{
				st.compileOk   = false;
				st.compileMsg  = res.fallbacks[0].reason;
				st.compileNode = res.fallbacks[0].node;
				if (const HC::Node* n = st.graph.findNode(st.compileNode))
				{
					st.currentGraph      = n->subgraph;
					st.selectedGraphNode = n->id;
					st.geState.focusNode = n->id;
					st.geState.selected  = n->id;
				}
			}
			else
			{
				size_t lines = 0;
				for (const auto& f : res.files)
					lines += (size_t)std::count(f.contents.begin(), f.contents.end(), '\n');
				st.compileOk  = true;
				st.compileMsg = "compiles clean — " + std::to_string(lines) + " lines of C++";
				for (const auto& w : res.warnings)
					HE_LOG_WARN(Editor, "%s",
						("HorizonCode compile check: " + w).c_str());
			}
		}
		// The result itself is on the strip above; what stays here is the one
		// thing a strip cannot carry — the jump to the node that failed.
		if (st.compileHas && !st.compileOk)
		{
			{
				if (st.compileNode != 0)
				{
					if (ImGui::SmallButton("Show the node that failed"))
						if (const HC::Node* n = st.graph.findNode(st.compileNode))
						{
							st.currentGraph      = n->subgraph;
							st.selectedGraphNode = n->id;
							st.geState.focusNode = n->id;
							st.geState.selected  = n->id;
						}
				}
			}
		}
		drawGraphCanvas(st, ctx, ImGui::GetContentRegionAvail());
		ImGui::EndChild();

		ImGui::SameLine();

		ImGui::BeginChild("##uiw_gdetails", ImVec2(rightW, 0), ImGuiChildFlags_Borders);
		{
			// Almost everything this pane prints under its widgets is a sentence —
			// "Fires when the bound element raises this event.", "Callable from
			// scripts via horizon.callWidgetFunction()." — and none of them fit in
			// 300 px. Clipped they lose their last words silently, which is worse
			// than not showing them at all.
			EditorWidgets::WrapText wrap;
			drawGraphNodeDetails(st, ctx);
		}
		ImGui::EndChild();
	}

	ImGui::End();
}

} // namespace UIEditorPanel
