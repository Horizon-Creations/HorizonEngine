#include "UIEditorPanel.h"
#include <imgui_internal.h>   // ShadeVertsLinearColorGradientKeepAlpha — see drawSurfacePreview
#include <Types/TypeRegistry.h>
#include "EditorToolbar.h"   // shared toolbar strip

#include <cstdio>
#include <cstdint>
#include "EditorApplication.h"                 // AppContext
#include "EditorAssetTypeCache.h"               // shared, invalidatable path → AssetType sniff
#include "AssetThumbnailCache.h"                // texture previews on the designer canvas
#include "EditorHelp.h"                         // "UI Widget/<label>" scopes for the tooltips
#include "EditorPanelState.h"                   // shared per-tab state map
#include "EditorWidgets.h"                      // shared Content-Browser asset drop target
#include "GraphEditor.h"                        // shared node-graph canvas
#include "HcGraphHost.h"                        // shared HorizonCode canvas host (pins, menus, clipboard)
#include "HcEditorUtil.h"                       // Create Object class picker
#include <HorizonScene/EngineApi.h>             // HE::api registry (Engine Call nodes)
#include <HorizonScene/HcCodegen.h>             // in-editor compile check (Compile button)
#include <MaterialGraph/MaterialGraph.h>        // MatDomain (widget materials are UI-domain)
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

	// Preview screen size (Details → Canvas). 0 = draw the authored canvas; a
	// real size lays the tree out exactly as the runtime would on that screen,
	// which is the only way to SEE what a scale mode does. View state, never
	// saved with the asset.
	int    previewIndex = 0;
	float  previewW = 0.0f, previewH = 0.0f;

	// Designer drag. mode: 0 = none, 1 = move element, 2 = resize element.
	int    dragMode = 0;
	int    resizeHandle = -1;        // 0..7: corners+edges (see handleOffsets)
	ImVec2 dragStartMouse;
	float  dragStartPos[2]  = {};
	float  dragStartSize[2] = {};
	bool   dragDidEdit = false;      // push one undo snapshot per completed drag
	// A press inside the current selection keeps that selection so it can be
	// DRAGGED — otherwise an element something else lies over could be selected
	// and then never moved, because the press that starts the drag re-picks the
	// thing on top of it. The pick is not thrown away though, only deferred: if
	// the button comes up without the mouse having travelled, it was a click and
	// this is what it would have selected.
	int    pendingPick = 0;
	bool   hasPendingPick = false;
	// Details panel: is the corner radius shown as one number or as four? A
	// display mode, not a value — an element whose corners already differ is
	// always shown as four whatever this says.
	bool   cornerPerSide = false;

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
struct Rect { ImVec2 mn, mx; };

// Element rect in CANVAS units — thin wrapper over the shared layout helper so
// the editor and runtime agree pixel-for-pixel. `canvas` null = the authored
// canvas; a resolved one lays the tree out for a specific screen (preview).
Rect elementCanvasRect(const UIWidgetTree& tree, const UIElement& e,
                       const HE::UIWidgetCanvas* canvas = nullptr)
{
	const HE::UIWidgetRect r = HE::uiElementRect(tree, e, canvas);
	return { { r.x, r.y }, { r.x + r.w, r.y + r.h } };
}

// Parent rect in CANVAS units (canvas root for parentId 0). Used to place the
// anchor marker and drop point.
Rect parentCanvasRect(const UIWidgetTree& tree, int parentId,
                      const HE::UIWidgetCanvas* canvas = nullptr)
{
	Rect parent{ {0.0f, 0.0f},
	             { canvas ? canvas->width  : tree.canvasWidth,
	               canvas ? canvas->height : tree.canvasHeight } };
	if (parentId != 0)
		if (const UIElement* p = tree.find(parentId))
			parent = elementCanvasRect(tree, *p, canvas);
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
// Its own reader, because these guards are typed and an Int read through
// propFloatOr silently comes back as the FALLBACK — the value lives in `.i`,
// `.f` is zero, and the type check sends it to the default. That is what made
// the designer draw every label left-middle no matter which of the nine cells
// was picked, while the engine drew it correctly.
int propIntOr(const UIElement& e, const char* name, int fb)
{
	const UIPropValue v = e.getProp(name);
	return v.type == UIPropType::Int ? v.i : fb;
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
		HE::uiSetAnchorPreset(*e, 0);
		e->posX = canvasPt->x - parent.mn.x;
		e->posY = canvasPt->y - parent.mn.y;
	}
	else
	{
		HE::uiSetAnchorPreset(*e, 5); // MiddleCenter
		e->posX = 0.0f; e->posY = 0.0f;
	}
	return st.tree.add(std::move(e));
}

// Add a WidgetRef that embeds `widgetPath`. Same placement rules as any other
// element; the path is what turns the empty slot into that widget at runtime.
int addWidgetRefAt(State& st, const std::string& widgetPath, int parentId,
                   const ImVec2* canvasPt)
{
	const int id = addElementAt(st, UIWidgetType::WidgetRef, parentId, canvasPt);
	if (UIElement* e = st.tree.find(id))
	{
		e->setProp("Widget", HE::UIPropValue::ofString(widgetPath));
		// Named after the widget it embeds, so the hierarchy reads as a list of
		// what is on the page rather than three rows of "WidgetRef".
		e->name = std::filesystem::path(widgetPath).stem().string();
	}
	return id;
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
// A material this widget may actually use: only the UI domain is drawn in
// screen space, and a Surface material on a widget is shaded by a sun that is
// not there. Anything that cannot be read (not loaded yet) stays listed rather
// than silently vanishing from the picker.
bool isUiDomainMaterial(AppContext& ctx, const std::string& relPath)
{
	if (!ctx.contentManager) return true;
	const HE::UUID id = ctx.contentManager->loadAsset(relPath);
	const MaterialAsset* m = id == HE::UUID{} ? nullptr : ctx.contentManager->getMaterial(id);
	return !m || m->domain == static_cast<uint8_t>(HE::MatDomain::UserInterface);
}

bool assetSlot(AppContext& ctx, const char* label, std::string& path,
               HE::AssetType wantType, const char* idSuffix,
               bool (*accept)(AppContext&, const std::string&) = nullptr)
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
		{
			// A rejected asset is still shown when it is the CURRENT value, so
			// an already-authored reference never disappears from its own slot.
			if (accept && path != a.path && !accept(ctx, a.path)) continue;
			if (ImGui::Selectable((a.label + "##" + a.path).c_str(), path == a.path))
			{
				path    = a.path;
				changed = true;
			}
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
			// Containers take children (see UIElement::acceptsChildren) — a
			// Panel, a layout box, and a Button, whose caption and icon ARE
			// children.
			if (dragged != nodeId && !st.tree.isDescendantOf(nodeId, dragged) &&
			    n->acceptsChildren())
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
			// New elements nest inside a container; otherwise share the
			// target's parent.
			const int parent = n->acceptsChildren() ? nodeId : n->parentId;
			st.selected = addElementAt(st, static_cast<UIWidgetType>(t), parent, nullptr);
			structureEdit = true;
		}
		// …and one of the project's own widgets, embedded as a WidgetRef.
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_UIWIDGET_REF"))
		{
			const int parent = n->acceptsChildren() ? nodeId : n->parentId;
			st.selected = addWidgetRefAt(st, static_cast<const char*>(p->Data), parent, nullptr);
			structureEdit = true;
		}
		ImGui::EndDragDropTarget();
	}

	// Context menu: delete / duplicate.
	if (ImGui::BeginPopupContextItem((std::string("##hctx") + std::to_string(nodeId)).c_str()))
	{
		HE::Ed::Help::Scope helpScope("UI Hierarchy");
		if (EditorWidgets::menuItem("Duplicate"))
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

// ── Text alignment: nine positions, drawn as nine ───────────────────────────
// The first property that could not be a generic UIPropDesc row. "Align H" and
// "Align V" are two ints, but what the author is choosing is ONE thing: where in
// its box the text sits. Two number fields would make them type 0..2 twice and
// work out in their head which is which, so they get the same 3×3 grid the
// anchor picker uses — deliberately, because it is the same kind of question one
// level further in (the anchor places the ELEMENT in its parent, this places the
// GLYPHS in the element).
//
// And because the two ARE that similar, they must not LOOK alike: the anchor
// grid is 4×4 amber dots and bars, this one is 3×3 and draws stacked text lines
// in a cool grey. Two identical grids in one panel is a control the author
// reaches for by position and gets wrong — which is exactly what happened.
void drawTextAlignGrid(UIElement& e, bool& edit, bool& committed)
{
	const int curH = e.getProp("Align H").i;
	const int curV = e.getProp("Align V").i;

	ImGui::TextUnformatted("Text Align");
	ImGui::SameLine(80.0f);
	ImGui::BeginGroup();
	{
		const float cell = 22.0f;
		ImDrawList* dl   = ImGui::GetWindowDrawList();
		for (int row = 0; row < 3; ++row)
		{
			for (int col = 0; col < 3; ++col)
			{
				if (col > 0) ImGui::SameLine();
				const bool active = curH == col && curV == row;
				if (active) ImGui::PushStyleColor(ImGuiCol_Button,
					ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
				char id[16]; std::snprintf(id, sizeof id, "##ta%d", row * 3 + col);
				if (ImGui::Button(id, ImVec2(cell, cell)))
				{
					e.setProp("Align H", UIPropValue::ofInt(col));
					e.setProp("Align V", UIPropValue::ofInt(row));
					edit = committed = true;
				}
				if (active) ImGui::PopStyleColor();

				// The marker says what the cell does: two stacked lines — a
				// paragraph seen from far away — ragged on the side the text
				// hangs off, sitting in that third of the box.
				const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
				const float pad = 5.0f;
				const ImVec2 in0(mn.x + pad, mn.y + pad), in1(mx.x - pad, mx.y - pad);
				const float w = in1.x - in0.x, h = in1.y - in0.y;
				const float gap = 3.0f;
				// Block of two lines, placed top / middle / bottom.
				const float top = in0.y + (row == 0 ? 0.0f : row == 1 ? (h - gap) * 0.5f : h - gap);
				const ImU32 markCol = IM_COL32(205, 210, 225, active ? 255 : 150);
				const float len[2] = { w, w * 0.62f };   // long line, short line
				for (int i = 0; i < 2; ++i)
				{
					const float slack = w - len[i];
					const float x = in0.x + (col == 0 ? 0.0f : col == 1 ? slack * 0.5f : slack);
					const float y = top + static_cast<float>(i) * gap;
					dl->AddLine(ImVec2(x, y), ImVec2(x + len[i], y), markCol, 1.6f);
				}
			}
		}
	}
	ImGui::EndGroup();
	EditorWidgets::helpForLabel("Text Align");
}

// ── Surface style ("Schicht 0", docs/he-apps-plan.md D5) ─────────────────────
// Rounding, border and gradient live on the BASE, not in any type's property
// table, so the generic loop below never saw them and until now they could only
// be set from a graph. This is their editor.
//
// The four radii are the reason this is handwritten. One number is what almost
// every element wants, four is what a tab or a chat bubble needs, and two
// separate controls for one idea is how an author ends up with a rounding they
// cannot explain. So: one field while the corners agree, a 2×2 grid laid out
// like the box itself once they do not, and one checkbox between the two.
void drawSurfaceStyle(State& st, UIElement& n, bool& edit, bool& committed)
{
	// Its own scope, though the only caller already pushes the same one: a
	// helper that names its scope is a helper the audit can place, and one that
	// borrows the caller's is filed under whichever function happens to sit
	// above it in this file.
	HE::Ed::Help::Scope helpScope("UI Widget");
	ImGui::SeparatorText("Surface");

	// Expanded either because the author asked, or because the values ALREADY
	// differ — collapsing that into one field would silently throw three of
	// them away.
	const bool differ = !n.uniformCornerRadius();
	bool perCorner = differ || st.cornerPerSide;
	if (EditorWidgets::checkbox("Per corner", &perCorner))
	{
		st.cornerPerSide = perCorner;
		// Folding back up keeps the top-left corner and gives it to the other
		// three, which is the only answer that does not need a rule nobody can
		// remember.
		if (!perCorner) { n.cornerRadius = glm::vec4(n.cornerRadius.x); committed = true; }
	}
	if (!perCorner)
	{
		float r = n.cornerRadius.x;
		if (ImGui::DragFloat("Corner Radius", &r, 0.5f, 0.0f, 10000.0f))
		{ n.cornerRadius = glm::vec4(std::max(0.0f, r)); edit = true; }
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		EditorWidgets::helpForLabel("Corner Radius");
	}
	else
	{
		// Laid out where the corners ARE: top row top-left/top-right, bottom row
		// bottom-left/bottom-right. Reading the grid is reading the box.
		const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x)
		                * 0.5f;
		float* const cell[4] = { &n.cornerRadius.x, &n.cornerRadius.y,      // TL, TR
		                         &n.cornerRadius.w, &n.cornerRadius.z };    // BL, BR
		static const char* kId[4] = { "##crTL", "##crTR", "##crBL", "##crBR" };
		for (int i = 0; i < 4; ++i)
		{
			if (i % 2) ImGui::SameLine();
			ImGui::SetNextItemWidth(w);
			if (ImGui::DragFloat(kId[i], cell[i], 0.5f, 0.0f, 10000.0f))
			{ *cell[i] = std::max(0.0f, *cell[i]); edit = true; }
			committed |= ImGui::IsItemDeactivatedAfterEdit();
		}
		ImGui::TextDisabled("Corners: top-left / top-right, then bottom.");
		EditorWidgets::helpForLabel("Per corner");
	}

	if (ImGui::DragFloat("Border Width", &n.borderWidth, 0.25f, 0.0f, 1000.0f))
	{ n.borderWidth = std::max(0.0f, n.borderWidth); edit = true; }
	committed |= ImGui::IsItemDeactivatedAfterEdit();
	EditorWidgets::helpForLabel("Border Width");
	if (n.borderWidth > 0.0f)
	{
		edit |= ImGui::ColorEdit4("Border Color", &n.borderColor.r);
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		EditorWidgets::helpForLabel("Border Color");
	}

	if (EditorWidgets::checkbox("Gradient", &n.gradient)) committed = true;
	if (n.gradient)
	{
		edit |= ImGui::ColorEdit4("Gradient Color", &n.gradientColor.r);
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		EditorWidgets::helpForLabel("Gradient Color");
		static const char* kShapes[] = { "Linear", "Radial" };
		const bool shapeOpen = ImGui::BeginCombo("Gradient Shape",
			kShapes[n.gradientShape == 1 ? 1 : 0]);
		if (!shapeOpen) EditorWidgets::helpForLabel("Gradient Shape");
		if (shapeOpen)
		{
			for (int i = 0; i < 2; ++i)
				if (ImGui::Selectable(kShapes[i], n.gradientShape == i))
				{ n.gradientShape = i; committed = true; }
			ImGui::EndCombo();
		}
		// A radial fade has no direction, so the angle is not offered rather
		// than offered and ignored.
		if (n.gradientShape != 1)
		{
			edit |= ImGui::DragFloat("Gradient Angle", &n.gradientAngle, 1.0f,
			                         -360.0f, 360.0f, "%.0f\xc2\xb0");
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			EditorWidgets::helpForLabel("Gradient Angle");
		}
	}

	if (EditorWidgets::checkbox("Shadow", &n.shadow)) committed = true;
	if (n.shadow)
	{
		edit |= ImGui::ColorEdit4("Shadow Color", &n.shadowColor.r);
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		EditorWidgets::helpForLabel("Shadow Color");
		if (ImGui::DragFloat("Shadow Blur", &n.shadowBlur, 0.5f, 0.0f, 500.0f))
		{ n.shadowBlur = std::max(0.0f, n.shadowBlur); edit = true; }
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		EditorWidgets::helpForLabel("Shadow Blur");
		float off[2] = { n.shadowOffsetX, n.shadowOffsetY };
		if (ImGui::DragFloat2("Shadow Offset", off, 0.5f))
		{ n.shadowOffsetX = off[0]; n.shadowOffsetY = off[1]; edit = true; }
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		EditorWidgets::helpForLabel("Shadow Offset");
	}

	if (EditorWidgets::checkbox("Inner Shadow", &n.innerShadow)) committed = true;
	if (n.innerShadow)
	{
		edit |= ImGui::ColorEdit4("Inner Shadow Color", &n.innerShadowColor.r);
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		EditorWidgets::helpForLabel("Inner Shadow Color");
		if (ImGui::DragFloat("Inner Shadow Blur", &n.innerShadowBlur, 0.5f, 0.0f, 500.0f))
		{ n.innerShadowBlur = std::max(0.0f, n.innerShadowBlur); edit = true; }
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		EditorWidgets::helpForLabel("Inner Shadow Blur");
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
		HE::Ed::Help::Scope helpScope("Canvas");
		ImGui::TextDisabled("Canvas");
		ImGui::Separator();
		bool edit = false;
		edit |= ImGui::DragFloat("Width",  &st.tree.canvasWidth,  1.0f, 64.0f, 7680.0f);
		EditorWidgets::helpForLabel("Width");
		edit |= ImGui::DragFloat("Height", &st.tree.canvasHeight, 1.0f, 64.0f, 4320.0f);
		EditorWidgets::helpForLabel("Height");
		if (edit) { st.dirty = true; }
		if (ImGui::IsItemDeactivatedAfterEdit()) commitEdit(st, ctx);

		// How the canvas above meets a screen that is not exactly this size.
		const bool scaleOpen = ImGui::BeginCombo("Scale", HE::uiCanvasScaleModeName(st.tree.scaleMode));
		if (!scaleOpen) EditorWidgets::helpForLabel("Scale");
		if (scaleOpen)
		{
			for (int m = 0; m <= static_cast<int>(HE::UICanvasScaleMode::ConstantPixel); ++m)
			{
				const auto mode = static_cast<HE::UICanvasScaleMode>(m);
				if (ImGui::Selectable(HE::uiCanvasScaleModeName(mode), st.tree.scaleMode == mode))
				{
					st.tree.scaleMode = mode;
					st.dirty = true;
					commitEdit(st, ctx);
				}
			}
			ImGui::EndCombo();
		}
		// The paragraph that used to be written out here is the help entry now —
		// same words, and reachable with F1 instead of only by hovering.

		ImGui::Spacing();
		ImGui::TextDisabled("Preview");
		ImGui::SetNextItemWidth(140.0f);
		{
			// The designer draws the AUTHORED canvas; this is what that canvas
			// turns into on a real screen. Purely a view setting, never saved.
			static const struct { const char* label; float w, h; } kPreviews[] = {
				{ "Authored size", 0.0f, 0.0f },
				{ "1280 x 720",   1280.0f,  720.0f },
				{ "1920 x 1080",  1920.0f, 1080.0f },
				{ "2560 x 1080",  2560.0f, 1080.0f },   // 21:9
				{ "3840 x 2160",  3840.0f, 2160.0f },
				{ "1080 x 1920",  1080.0f, 1920.0f },   // portrait
			};
			const int count = static_cast<int>(sizeof(kPreviews) / sizeof(kPreviews[0]));
			st.previewIndex = std::clamp(st.previewIndex, 0, count - 1);
			if (ImGui::BeginCombo("##previewsize", kPreviews[st.previewIndex].label))
			{
				for (int i = 0; i < count; ++i)
					if (ImGui::Selectable(kPreviews[i].label, st.previewIndex == i))
						st.previewIndex = i;
				ImGui::EndCombo();
			}
			st.previewW = kPreviews[st.previewIndex].w;
			st.previewH = kPreviews[st.previewIndex].h;
			if (st.previewIndex != 0)
			{
				const HE::UIWidgetCanvas c =
					HE::uiResolveCanvas(st.tree, st.previewW, st.previewH);
				ImGui::TextDisabled("Canvas %.0f x %.0f units, scale %.2f x %.2f",
				                    c.width, c.height, c.scaleX, c.scaleY);
			}
		}

		ImGui::Spacing();
		ImGui::TextWrapped("Select an element on the canvas or in the hierarchy "
		                   "to edit its properties.");
		return;
	}

	// Everything below belongs to the selected widget. The layout fields change
	// their MEANING with the anchor — "Position X" becomes "Left/Right" when the
	// element is stretched across that axis — which is the single thing about
	// this panel people get wrong, so each of the shapes has its own entry.
	HE::Ed::Help::Scope helpScope("UI Widget");

	bool edit      = false; // any value changed this frame (live view update)
	bool committed = false; // an edit finished (undo snapshot + live asset)

	ImGui::TextDisabled("%s", n->typeName());
	ImGui::Separator();

	ImGui::InputText("Name", &n->name);
	committed |= ImGui::IsItemDeactivatedAfterEdit();
	EditorWidgets::helpForLabel("Name");

	// Layout — shared base fields.
	ImGui::SeparatorText("Layout");

	// A child of a layout container does not place itself: anchors, position
	// and the size on the box's axis are the box's business. Showing the
	// anchor grid there would be offering a control that does nothing.
	const UIElement* layoutParent = n->parentId != 0 ? st.tree.find(n->parentId) : nullptr;
	if (layoutParent && !layoutParent->laysOutChildren()) layoutParent = nullptr;
	if (layoutParent)
	{
		const bool vert = layoutParent->stacksVertically();
		ImGui::TextDisabled("Placed by the %s above it.", layoutParent->typeName());
		edit |= ImGui::DragFloat("Slot Fill", &n->slotFill, 0.05f, 0.0f, 100.0f);
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		if (n->slotFill < 0.0f) n->slotFill = 0.0f;
		// The tooltip that stood here named the axis ("keep my own height") and
		// the entry cannot, since it is one sentence for both. Worth the trade:
		// the entry is also what F1 opens, and the axis is on screen anyway.
		EditorWidgets::helpForLabel("Slot Fill");
		// The size across the axis is the box's; the one along it is only used
		// while this slot does not fill.
		if (n->slotFill <= 0.0f)
		{
			edit |= vert ? ImGui::DragFloat("Height", &n->sizeY, 1.0f, 1.0f, 10000.0f)
			             : ImGui::DragFloat("Width",  &n->sizeX, 1.0f, 1.0f, 10000.0f);
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			EditorWidgets::helpForLabel(vert ? "Height" : "Width");
		}
		edit |= ImGui::DragFloat2("Pivot", &n->pivotX, 0.01f, 0.0f, 1.0f);
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		EditorWidgets::helpForLabel("Pivot");
	}
	else
	{

	// A stretched axis is not authored as "position and size": the element has
	// no size of its own there, it has two margins from the anchored edges. So
	// the two fields ARE the margins on that axis, and stay position/size on
	// the axis that is still a point. This is what "anchored to a whole side"
	// means in practice, and it is why the fields have to change with the
	// anchor rather than the anchor quietly redefining what they mean.
	const bool stretchX = n->anchorMaxX > n->anchorMinX + 1e-4f;
	const bool stretchY = n->anchorMaxY > n->anchorMinY + 1e-4f;
	{
		float left, right, top, bottom;
		HE::uiAnchorInsetsX(*n, left, right);
		HE::uiAnchorInsetsY(*n, top, bottom);

		if (stretchX && stretchY)
		{
			float lt[2] = { left, top }, rb[2] = { right, bottom };
			bool changed = ImGui::DragFloat2("Offset TL", lt, 1.0f);
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			EditorWidgets::helpForLabel("Offset TL");
			changed |= ImGui::DragFloat2("Offset BR", rb, 1.0f);
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			EditorWidgets::helpForLabel("Offset BR");
			if (changed)
			{
				HE::uiSetAnchorInsetsX(*n, lt[0], rb[0]);
				HE::uiSetAnchorInsetsY(*n, lt[1], rb[1]);
				edit = true;
			}
		}
		else if (stretchX)
		{
			float lr[2] = { left, right };
			if (ImGui::DragFloat2("Left/Right", lr, 1.0f))
			{ HE::uiSetAnchorInsetsX(*n, lr[0], lr[1]); edit = true; }
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			EditorWidgets::helpForLabel("Left/Right");
			edit |= ImGui::DragFloat("Position Y", &n->posY, 1.0f);
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			EditorWidgets::helpForLabel("Position Y");
			edit |= ImGui::DragFloat("Height", &n->sizeY, 1.0f, 1.0f, 10000.0f);
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			EditorWidgets::helpForLabel("Height");
		}
		else if (stretchY)
		{
			edit |= ImGui::DragFloat("Position X", &n->posX, 1.0f);
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			EditorWidgets::helpForLabel("Position X");
			edit |= ImGui::DragFloat("Width", &n->sizeX, 1.0f, 1.0f, 10000.0f);
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			EditorWidgets::helpForLabel("Width");
			float tb[2] = { top, bottom };
			if (ImGui::DragFloat2("Top/Bottom", tb, 1.0f))
			{ HE::uiSetAnchorInsetsY(*n, tb[0], tb[1]); edit = true; }
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			EditorWidgets::helpForLabel("Top/Bottom");
		}
		else
		{
			edit |= ImGui::DragFloat2("Position", &n->posX, 1.0f);
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			EditorWidgets::helpForLabel("Position");
			// A container that sizes itself to its content owns those two
			// numbers: showing them editable would be offering a value that is
			// overwritten before it is ever drawn.
			const bool measured = n->getProp("Size To Content").b;
			ImGui::BeginDisabled(measured);
			edit |= ImGui::DragFloat2("Size", &n->sizeX, 1.0f, 1.0f, 10000.0f);
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			EditorWidgets::helpForLabel("Size");
			ImGui::EndDisabled();
			if (measured)
				ImGui::TextDisabled("Measured from the content (Min Width/Height below).");
		}
	}
	edit |= ImGui::DragFloat2("Pivot", &n->pivotX, 0.01f, 0.0f, 1.0f);
	committed |= ImGui::IsItemDeactivatedAfterEdit();
	EditorWidgets::helpForLabel("Pivot");

	// Anchor: the UMG 4×4 grid. The first three rows and columns are the nine
	// points the anchor has always been; the fourth of each stretches the
	// element across that whole axis of its parent — a complete side, and both
	// together the whole available space. Each cell draws what it does: a dot,
	// a bar along the side it spans, or a filled square.
	ImGui::TextUnformatted("Anchor");
	ImGui::SameLine(80.0f);
	ImGui::BeginGroup();
	{
		const int   current = HE::uiAnchorPresetOf(*n);
		const float cell    = 22.0f;
		ImDrawList* dl      = ImGui::GetWindowDrawList();
		for (int row = 0; row < 4; ++row)
		{
			for (int col = 0; col < 4; ++col)
			{
				if (col > 0) ImGui::SameLine();
				const int a = row * 4 + col;
				const bool active = current == a;
				if (active) ImGui::PushStyleColor(ImGuiCol_Button,
					ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
				char id[16]; std::snprintf(id, sizeof id, "##a%d", a);
				if (ImGui::Button(id, ImVec2(cell, cell)))
				{
					// Keep the element where it is: only what happens when the
					// parent resizes changes.
					HE::uiReanchorKeepingRect(st.tree, *n, a);
					committed = true;
				}
				if (active) ImGui::PopStyleColor();

				// The marker: 0..1 inside the cell, mapping the anchor rect.
				float x0, y0, x1, y1;
				HE::uiAnchorPresetRect(a, x0, y0, x1, y1);
				const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
				const float pad = 5.0f;
				const ImVec2 in0(mn.x + pad, mn.y + pad), in1(mx.x - pad, mx.y - pad);
				const ImVec2 p0(in0.x + x0 * (in1.x - in0.x), in0.y + y0 * (in1.y - in0.y));
				const ImVec2 p1(in0.x + x1 * (in1.x - in0.x), in0.y + y1 * (in1.y - in0.y));
				const ImU32 markCol = IM_COL32(255, 170, 40, active ? 255 : 190);
				if (x0 == x1 && y0 == y1) dl->AddCircleFilled(p0, 2.5f, markCol);
				else if (x0 == x1)        dl->AddLine(p0, p1, markCol, 2.0f);
				else if (y0 == y1)        dl->AddLine(p0, p1, markCol, 2.0f);
				else                      dl->AddRectFilled(p0, p1, IM_COL32(255, 170, 40, active ? 200 : 120));
			}
		}
	}
	ImGui::EndGroup();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Where in the parent this element hangs.\n"
		                  "The last row and column stretch it across that whole\n"
		                  "side; the bottom-right cell fills the parent entirely.\n"
		                  "Re-anchoring keeps the element exactly where it is.");
	} // end of the anchored (non-box-child) branch

	// The four tooltips that stood here are entries now, so F1 reaches them too.
	int layer = n->layer;
	if (ImGui::DragInt("Layer", &layer, 1)) { n->layer = layer; edit = true; }
	committed |= ImGui::IsItemDeactivatedAfterEdit();
	EditorWidgets::helpForLabel("Layer");
	if (EditorWidgets::checkbox("Visible", &n->visible)) committed = true;
	ImGui::SameLine();
	if (EditorWidgets::checkbox("Enabled", &n->enabled)) committed = true;
	edit |= ImGui::DragFloat("Rotation", &n->rotation, 0.5f, -360.0f, 360.0f, "%.1f\xc2\xb0");
	committed |= ImGui::IsItemDeactivatedAfterEdit();
	EditorWidgets::helpForLabel("Rotation");
	edit |= ImGui::SliderFloat("Opacity", &n->renderOpacity, 0.0f, 1.0f);
	committed |= ImGui::IsItemDeactivatedAfterEdit();
	EditorWidgets::helpForLabel("Opacity");

	// Type-specific properties (generic, driven by properties()).
	const std::vector<UIPropDesc> props = n->properties();
	if (!props.empty())
	{
		ImGui::SeparatorText("Properties");
		for (const UIPropDesc& pd : props)
		{
			// "Align H"/"Align V" are one control, not two number fields: which
			// of nine positions the text sits in is a thing you point at. Drawn
			// once, at the H row, and the V row is skipped.
			if (pd.name == "Align V") continue;
			if (pd.name == "Align H") { drawTextAlignGrid(*n, edit, committed); continue; }
			drawPropertyWidget(*n, pd, edit, committed);
		}
	}

	// "Schicht 0": the style of the element's own surface. Only where there IS
	// one — the same question the material slot asks, and the reason a Text
	// label is not offered a border that would outline nothing.
	if (n->hasSurfaceStyle())
		drawSurfaceStyle(st, *n, edit, committed);

	// Material slot (only types that expose one — text runs have no quad).
	if (n->hasMaterialSlot())
	{
		ImGui::SeparatorText("Material");
		committed |= assetSlot(ctx, "Material", n->material,
		                       HE::AssetType::Material, "mat", &isUiDomainMaterial);
		ImGui::TextDisabled("Only User Interface materials are offered here.");
		if (!n->material.empty() && !isUiDomainMaterial(ctx, n->material))
			ImGui::TextColored(ImVec4(0.86f, 0.48f, 0.12f, 1.0f),
				"This is a Surface material: it will not draw correctly here.");
	}

	// The widget a WidgetRef embeds. Picked from the project's widgets, and
	// never itself — a widget that embeds itself is refused at runtime, so the
	// picker does not offer the trap in the first place.
	if (n->type() == UIWidgetType::WidgetRef)
	{
		ImGui::SeparatorText("Embedded widget");
		std::string path = n->getProp("Widget").s;
		if (assetSlot(ctx, "Widget", path, HE::AssetType::Widget, "wref"))
		{
			if (path == st.relPath)
				ImGui::TextColored(ImVec4(0.86f, 0.48f, 0.12f, 1.0f),
					"A widget cannot embed itself.");
			else
			{
				n->setProp("Widget", HE::UIPropValue::ofString(path));
				committed = true;
			}
		}
		ImGui::TextDisabled("Grafted in when the widget is created; the designer\n"
		                    "shows the slot it will fill.");
	}

	// Texture slot: the plain "put this picture on it" path, tinted by the
	// element's own colour. A material, when set, wins — it owns the pixels.
	if (n->hasTextureSlot())
	{
		ImGui::SeparatorText("Texture");
		if (assetSlot(ctx, "Texture", n->texture, HE::AssetType::Texture, "tex"))
		{
			// Resolve straight away so the designer shows the picture without
			// waiting for a play session (the runtime resolves the same way).
			n->textureAssetId = (!n->texture.empty() && ctx.contentManager)
				? ctx.contentManager->loadAsset(n->texture) : HE::UUID{};
			committed = true;
		}
		// The source size, for the same reason the runtime resolves it: 9-slice
		// margins are in source pixels and have to become UVs somewhere.
		if (n->textureAssetId != HE::UUID{} && ctx.contentManager)
		{
			if (const TextureAsset* ta = ctx.contentManager->getTexture(n->textureAssetId))
			{ n->textureW = ta->width; n->textureH = ta->height; }
			if (n->textureW > 0)
				ImGui::TextDisabled("Source %u x %u px", n->textureW, n->textureH);
		}
		if (!n->material.empty())
			ImGui::TextDisabled("A material is set — it draws instead of this.");
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
	if (EditorWidgets::checkbox("Hit-testable", &n->hitTestable)) committed = true;
	if (EditorWidgets::checkbox("Clip children", &n->clipChildren)) committed = true;
	const bool cursorOpen = ImGui::BeginCombo("Hover cursor", HE::uiCursorName(n->hoverCursor));
	if (!cursorOpen) EditorWidgets::helpForLabel("Hover cursor");
	if (cursorOpen)
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

// One rounded rectangle with four independent radii, as an ImDrawList path.
// ImGui's own AddRectFilled takes ONE rounding and a set of corner flags, which
// cannot express "8 at the top, square at the bottom" — the shape a tab is. A
// radius of 0 makes PathArcTo emit the centre point alone, so a square corner
// falls out of the same four calls.
void pathRoundedRect(ImDrawList* dl, const ImVec2& a, const ImVec2& b, const glm::vec4& r)
{
	constexpr float kPi = 3.14159265358979323846f;   // IM_PI is imgui_internal
	const float lim = 0.5f * std::min(b.x - a.x, b.y - a.y);
	const float tl = std::clamp(r.x, 0.0f, lim), tr = std::clamp(r.y, 0.0f, lim);
	const float br = std::clamp(r.z, 0.0f, lim), bl = std::clamp(r.w, 0.0f, lim);
	dl->PathArcTo(ImVec2(a.x + tl, a.y + tl), tl, kPi,          kPi * 1.5f);
	dl->PathArcTo(ImVec2(b.x - tr, a.y + tr), tr, kPi * 1.5f,   kPi * 2.0f);
	dl->PathArcTo(ImVec2(b.x - br, b.y - br), br, 0.0f,         kPi * 0.5f);
	dl->PathArcTo(ImVec2(a.x + bl, b.y - bl), bl, kPi * 0.5f,   kPi);
}

// The element's own surface, exactly as "Schicht 0" describes it: the authored
// rounding, the fill (flat or faded), and the border drawn INSIDE the shape.
//
// Drawn here once for every type that HAS a surface, instead of six types each
// hard-coding a rounding of 3 or 4 and ignoring what the author set. The
// designer's whole job is to show what the engine will draw, and until this it
// showed a rounding nobody had asked for and no border or gradient at all.
void drawSurfacePreview(ImDrawList* dl, const UIElement& n, const ImVec2& mn,
                        const ImVec2& mx, float s, const glm::vec4& fill,
                        float alpha, float dim, void* texHandle, bool drawFill)
{
	const auto C = [&](const glm::vec4& c)
	{
		return toCol32({ c.r * dim, c.g * dim, c.b * dim, c.a * alpha });
	};
	const glm::vec4 radii = n.cornerRadius * s;
	// The drop shadow, under everything this element draws. ImDrawList has no
	// soft edge, so it is approximated by a handful of nested shapes at rising
	// alpha — the same picture, out of coarser material.
	if (drawFill && n.shadow && n.shadowColor.a > 0.001f)
	{
		const float blur = std::max(0.0f, n.shadowBlur * s);
		const ImVec2 o(n.shadowOffsetX * s, n.shadowOffsetY * s);
		// Outermost first, each the same faint alpha: stacked "over", they build
		// up to roughly the authored alpha where they all overlap and fade out
		// where only the widest one reaches. That is a falloff, drawn with the
		// only tool a draw list has.
		constexpr int kSteps = 6;
		glm::vec4 c = n.shadowColor;
		c.a /= kSteps;
		for (int i = kSteps; i >= 1; --i)
		{
			const float g = blur * static_cast<float>(i) / kSteps;
			pathRoundedRect(dl, ImVec2(mn.x + o.x - g, mn.y + o.y - g),
			                    ImVec2(mx.x + o.x + g, mx.y + o.y + g), radii + g);
			dl->PathFillConvex(C(c));
		}
	}
	// A picture on the surface is the surface: the texture is drawn square (a
	// rounded image needs a clip ImDrawList cannot express along a path), and
	// the border still follows the authored shape on top of it.
	if (!drawFill)
	{
		// The caller drew its own picture; only the outline is left to add.
	}
	else if (texHandle)
		dl->AddImage(reinterpret_cast<ImTextureID>(texHandle), mn, mx,
		             ImVec2(0, 0), ImVec2(1, 1), C(fill));
	else
	{
		const int vtx0 = dl->VtxBuffer.Size;
		pathRoundedRect(dl, mn, mx, radii);
		// A radial fade reaches its far colour at the corners, so that is what
		// the shape is filled with and the rings below work inwards from it.
		dl->PathFillConvex(C(n.gradient && n.gradientShape == 1 ? n.gradientColor : fill));
		if (n.gradient && n.gradientShape != 1)
		{
			// The same rule the shaders use: an angle clockwise from "down",
			// projected onto the box. Shading the vertices the fill just wrote
			// is how a path gets a gradient at all — AddRectFilledMultiColor is
			// axis-aligned and square.
			const float rad = n.gradientAngle * 0.017453292f;
			const ImVec2 c((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
			const ImVec2 d(std::sin(rad), std::cos(rad));
			const float  half = 0.5f * (std::abs(d.x) * (mx.x - mn.x) +
			                            std::abs(d.y) * (mx.y - mn.y));
			ImGui::ShadeVertsLinearColorGradientKeepAlpha(
				dl, vtx0, dl->VtxBuffer.Size,
				ImVec2(c.x - d.x * half, c.y - d.y * half),
				ImVec2(c.x + d.x * half, c.y + d.y * half),
				C(fill), C(n.gradientColor));
		}
		else if (n.gradient)
		{
			// Radial. ImGui can shade a path's VERTICES, and this path's are all
			// on its outline — every one of them out at the far stop — so
			// shading it comes out flat. Rings do the fade instead.
			//
			// They stop at the circle INSCRIBED in the box, never at the corner:
			// a circle centred in a rounded rectangle and no wider than its
			// shorter side cannot leave it, and ImDrawList has no way to clip to
			// a path. So the corners keep the far colour and only the last ring
			// is a slightly compressed step. The engine fades all the way to the
			// farthest corner; this is the preview's approximation of it.
			const ImVec2 c((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
			const float w = mx.x - mn.x, h = mx.y - mn.y;
			const float farR = std::max(1e-4f, 0.5f * std::sqrt(w * w + h * h));
			const float inR  = 0.5f * std::min(w, h);
			constexpr int kRings = 24;
			for (int i = kRings; i >= 1; --i)
			{
				const float r = inR * static_cast<float>(i) / kRings;
				const glm::vec4 col = fill + (n.gradientColor - fill) * (r / farR);
				dl->AddCircleFilled(c, r, C(col), 48);
			}
		}
	}
	// The inner shadow, over the fill and under the border: rings drawn just
	// inside the edge, the same stacking trick as the drop shadow. They stay
	// within the shape by construction, so nothing spills.
	if (drawFill && n.innerShadow && n.innerShadowColor.a > 0.001f)
	{
		const float depth = std::max(1.0f, n.innerShadowBlur * s);
		constexpr int kSteps = 5;
		glm::vec4 c = n.innerShadowColor;
		c.a /= kSteps;
		for (int i = 1; i <= kSteps; ++i)
		{
			const float g = depth * static_cast<float>(i) / kSteps;
			if (mx.x - g <= mn.x + g || mx.y - g <= mn.y + g) break;
			pathRoundedRect(dl, ImVec2(mn.x + g * 0.5f, mn.y + g * 0.5f),
			                    ImVec2(mx.x - g * 0.5f, mx.y - g * 0.5f),
			                    radii - g * 0.5f);
			dl->PathStroke(C(c), ImDrawFlags_Closed, g);
		}
	}
	if (n.borderWidth > 0.0f)
	{
		// Inside the shape, like the shaders draw it: the stroke is centred on
		// the path, so the path moves in by half the width.
		const float bw = std::max(1.0f, n.borderWidth * s);
		const ImVec2 im(mn.x + bw * 0.5f, mn.y + bw * 0.5f);
		const ImVec2 ix(mx.x - bw * 0.5f, mx.y - bw * 0.5f);
		if (ix.x > im.x && ix.y > im.y)
		{
			pathRoundedRect(dl, im, ix, radii - bw * 0.5f);
			dl->PathStroke(C(n.borderColor), ImDrawFlags_Closed, bw);
		}
	}
}

// Draw a simplified WYSIWYG preview of one element from its generic properties.
void drawElementPreview(ImDrawList* dl, const UIElement& n, const ImVec2& mn,
                        const ImVec2& mx, float s, void* texHandle = nullptr,
                        float alpha = 1.0f, float dim = 1.0f)
{
	// Every colour of the element itself goes through here: faded by the
	// inherited opacity and knocked back while it is disabled, exactly as
	// WidgetManager does it to the real quads.
	const auto C = [&](const glm::vec4& c)
	{
		return toCol32({ c.r * dim, c.g * dim, c.b * dim, c.a * alpha });
	};
	// The authored surface first, for every type that has one — the same rule
	// WidgetManager stamps onto the first quad. What follows in each case is
	// only what is drawn ON that surface.
	// An Image is left out here and gets its outline AFTER the switch: its
	// surface is the picture, which it draws itself (nine pieces when sliced),
	// and a tinted rectangle under it would be a colour nobody authored.
	if (n.hasSurfaceStyle() && n.type() != UIWidgetType::Image)
	{
		// Which of its colours IS the surface. Every one of these types names it
		// differently, and that name is the only thing they disagree about.
		const char* key =
			n.type() == UIWidgetType::Panel  ? "Color" :
			n.type() == UIWidgetType::Button ? "Normal Color" : "Back Color";
		const glm::vec4 fallback =
			n.type() == UIWidgetType::Panel       ? glm::vec4{ 0.12f, 0.12f, 0.14f, 0.85f } :
			n.type() == UIWidgetType::Button      ? glm::vec4{ 0.20f, 0.20f, 0.20f, 1 } :
			n.type() == UIWidgetType::TextInput   ? glm::vec4{ 0.10f, 0.10f, 0.10f, 1 }
			                                      : glm::vec4{ 0.15f, 0.15f, 0.15f, 1 };
		drawSurfacePreview(dl, n, mn, mx, s, propColorOr(n, key, fallback),
		                   alpha, dim, texHandle, /*drawFill=*/true);
	}
	switch (n.type())
	{
	case UIWidgetType::Panel:
		break;   // nothing but its surface
	case UIWidgetType::Image:
	{
		if (texHandle)
		{
			const ImU32 tint = C(propColorOr(n, "Tint", { 1,1,1,1 }));
			// 9-sliced images are drawn as nine pieces here too, from the same
			// margins the runtime uses — a frame that only looks right in play
			// mode is a frame nobody can author.
			const float sl = propFloatOr(n, "Slice Left",   0.0f);
			const float stp= propFloatOr(n, "Slice Top",    0.0f);
			const float sr = propFloatOr(n, "Slice Right",  0.0f);
			const float sb = propFloatOr(n, "Slice Bottom", 0.0f);
			const bool  sliced = n.textureW > 0 && n.textureH > 0 &&
			                     (sl > 0.0f || stp > 0.0f || sr > 0.0f || sb > 0.0f);
			if (!sliced)
			{
				dl->AddImage(reinterpret_cast<ImTextureID>(texHandle), mn, mx,
				             ImVec2(0, 0), ImVec2(1, 1), tint);
				break;
			}
			const float w = mx.x - mn.x, h = mx.y - mn.y;
			auto fitPair = [](float a, float b, float extent, float& oa, float& ob)
			{
				oa = std::max(0.0f, a); ob = std::max(0.0f, b);
				const float sum = oa + ob;
				if (sum > extent && sum > 0.0f) { const float k = extent / sum; oa *= k; ob *= k; }
			};
			float l, r, t2, b2;
			fitPair(sl * s, sr * s, w, l, r);
			fitPair(stp * s, sb * s, h, t2, b2);
			const float tw = static_cast<float>(n.textureW), th = static_cast<float>(n.textureH);
			const float xs[4] = { mn.x, mn.x + l, mx.x - r, mx.x };
			const float ys[4] = { mn.y, mn.y + t2, mx.y - b2, mx.y };
			const float us[4] = { 0.0f, sl / tw, 1.0f - sr / tw, 1.0f };
			const float vs[4] = { 0.0f, stp / th, 1.0f - sb / th, 1.0f };
			const bool fillCentre = propBoolOr(n, "Slice Fill Centre", true);
			for (int row = 0; row < 3; ++row)
				for (int col = 0; col < 3; ++col)
				{
					if (row == 1 && col == 1 && !fillCentre) continue;
					if (xs[col + 1] <= xs[col] || ys[row + 1] <= ys[row]) continue;
					dl->AddImage(reinterpret_cast<ImTextureID>(texHandle),
					             ImVec2(xs[col], ys[row]), ImVec2(xs[col + 1], ys[row + 1]),
					             ImVec2(us[col], vs[row]), ImVec2(us[col + 1], vs[row + 1]), tint);
				}
			break;
		}
		dl->AddRectFilled(mn, mx, C(propColorOr(n, "Tint", { 1,1,1,1 })));
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
	case UIWidgetType::WidgetRef:
	{
		// Just the outline of the slot: the embedded widget itself is drawn on
		// top of this by the caller, which is also the one that knows whether
		// the reference resolved (and writes the name when it did not).
		dl->AddRect(mn, mx, IM_COL32(150, 210, 160, 110));
		break;
	}
	case UIWidgetType::VerticalBox:
	case UIWidgetType::HorizontalBox:
	{
		// A box draws nothing at runtime; in the designer it shows its bounds
		// and its padding, so an empty one is not an invisible thing you cannot
		// aim at.
		dl->AddRect(mn, mx, IM_COL32(120, 190, 255, 90));
		const float pad = propFloatOr(n, "Padding", 0.0f) * s;
		if (pad > 0.5f)
			dl->AddRect(ImVec2(mn.x + pad, mn.y + pad), ImVec2(mx.x - pad, mx.y - pad),
			            IM_COL32(120, 190, 255, 50));
		break;
	}
	case UIWidgetType::Spacer:
	{
		// Invisible at runtime, and therefore drawn HERE — a gap you cannot see
		// in the designer is a gap you cannot grab, resize or even find again.
		// Dashes plus a double-headed arrow along the axis it pushes on.
		dl->AddRect(mn, mx, IM_COL32(150, 150, 165, 80));
		const float w = mx.x - mn.x, h = mx.y - mn.y;
		const ImU32 arrow = IM_COL32(150, 150, 165, 120);
		const float cx = (mn.x + mx.x) * 0.5f, cy = (mn.y + mx.y) * 0.5f;
		const float tip = std::min(6.0f, std::min(w, h) * 0.3f);
		if (h >= w)   // taller than wide: it pushes downwards
		{
			dl->AddLine(ImVec2(cx, mn.y + 2), ImVec2(cx, mx.y - 2), arrow);
			dl->AddTriangleFilled(ImVec2(cx, mn.y + 1), ImVec2(cx - tip * 0.5f, mn.y + 1 + tip),
			                      ImVec2(cx + tip * 0.5f, mn.y + 1 + tip), arrow);
			dl->AddTriangleFilled(ImVec2(cx, mx.y - 1), ImVec2(cx - tip * 0.5f, mx.y - 1 - tip),
			                      ImVec2(cx + tip * 0.5f, mx.y - 1 - tip), arrow);
		}
		else
		{
			dl->AddLine(ImVec2(mn.x + 2, cy), ImVec2(mx.x - 2, cy), arrow);
			dl->AddTriangleFilled(ImVec2(mn.x + 1, cy), ImVec2(mn.x + 1 + tip, cy - tip * 0.5f),
			                      ImVec2(mn.x + 1 + tip, cy + tip * 0.5f), arrow);
			dl->AddTriangleFilled(ImVec2(mx.x - 1, cy), ImVec2(mx.x - 1 - tip, cy - tip * 0.5f),
			                      ImVec2(mx.x - 1 - tip, cy + tip * 0.5f), arrow);
		}
		break;
	}
	case UIWidgetType::Text:
	{
		const float fs = propFloatOr(n, "FontSize", 22.0f) * s;
		const std::string txt = propStringOr(n, "Text", "");
		const char* shown = txt.empty() ? "(empty)" : txt.c_str();
		// Honour Align H/Align V. This used to draw at the rect's top-left
		// corner unconditionally, which made every label look pinned there and
		// disagree with what the engine actually drew — the designer's whole job
		// is to show what you will get.
		const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, shown);
		const int aH = propIntOr(n, "Align H", 0);
		const int aV = propIntOr(n, "Align V", 1);
		const float slackX = std::max(0.0f, (mx.x - mn.x) - ts.x);
		const float slackY = std::max(0.0f, (mx.y - mn.y) - ts.y);
		const ImVec2 at(mn.x + (aH == 1 ? slackX * 0.5f : aH == 2 ? slackX : 0.0f),
		                mn.y + (aV == 1 ? slackY * 0.5f : aV == 2 ? slackY : 0.0f));
		dl->AddText(nullptr, fs, at, C(propColorOr(n, "Color", { 1,1,1,1 })), shown);
		break;
	}
	case UIWidgetType::Button:
	{
		// Nothing but its surface, which is drawn above from the AUTHORED
		// rounding, border and gradient — this used to hard-code a rounding of 4
		// and an outline the engine never drew.
		//
		// No caption either: a Button is a surface, and what sits on it is a
		// CHILD element that this same function draws in its own turn.
		break;
	}
	case UIWidgetType::CheckBox:
	{
		// Box sized by the LABEL and capped at the element's height, centred in
		// it — the same rule UICheckBox::render uses, so the designer does not
		// show a different checkbox than the game does.
		const float fs = propFloatOr(n, "FontSize", 18.0f) * s;
		const float boxSz = std::min(mx.y - mn.y, fs * 1.15f);
		const float byTop = mn.y + ((mx.y - mn.y) - boxSz) * 0.5f;
		const ImVec2 bmn(mn.x, byTop), bmx(mn.x + boxSz, byTop + boxSz);
		dl->AddRectFilled(bmn, bmx, C(propColorOr(n, "Box Color", { 0.20f,0.20f,0.20f,1 })), 3.0f * s);
		dl->AddRect(bmn, bmx, IM_COL32(200,200,210,90), 3.0f * s);
		if (propBoolOr(n, "Checked", false))
		{
			const float pad = boxSz * 0.22f;
			dl->AddRectFilled(ImVec2(bmn.x + pad, bmn.y + pad), ImVec2(bmx.x - pad, bmx.y - pad),
				C(propColorOr(n, "Check Color", { 0.30f,0.80f,0.40f,1 })), 2.0f * s);
		}
		const std::string lbl = propStringOr(n, "Label", "");
		if (!lbl.empty())
			dl->AddText(nullptr, fs, ImVec2(bmx.x + 0.4f * boxSz, (mn.y + mx.y - fs) * 0.5f),
				C(propColorOr(n, "Text Color", { 1,1,1,1 })), lbl.c_str());
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
			C(propColorOr(n, "Track Color", { 0.20f,0.20f,0.20f,1 })), trackH * 0.5f);
		const float hx = mn.x + t * (mx.x - mn.x);
		dl->AddRectFilled(ImVec2(mn.x, cy - trackH * 0.5f), ImVec2(hx, cy + trackH * 0.5f),
			C(propColorOr(n, "Fill Color", { 0.30f,0.60f,0.90f,1 })), trackH * 0.5f);
		dl->AddCircleFilled(ImVec2(hx, cy), std::max(3.0f, (mx.y - mn.y) * 0.4f),
			C(propColorOr(n, "Handle Color", { 0.90f,0.90f,0.90f,1 })));
		break;
	}
	case UIWidgetType::ProgressBar:
	{
		// The track is the surface (drawn above); only the fill on top of it
		// belongs here.
		float val = propFloatOr(n, "Value", 0.5f);
		val = val < 0.0f ? 0.0f : (val > 1.0f ? 1.0f : val);
		dl->AddRectFilled(mn, ImVec2(mn.x + val * (mx.x - mn.x), mx.y),
			C(propColorOr(n, "Fill Color", { 0.30f,0.70f,0.40f,1 })),
			std::min(n.cornerRadius.x, n.cornerRadius.w) * s);
		break;
	}
	case UIWidgetType::TextInput:
	{
		// Background and outline are the surface, drawn above.
		const std::string txt = propStringOr(n, "Text", "");
		const bool placeholder = txt.empty();
		std::string shown = placeholder ? propStringOr(n, "Placeholder", "") : txt;
		// A password field shows dots here too, or the designer is the one
		// place the secret is on screen.
		if (!placeholder && propBoolOr(n, "Password", false))
		{
			std::string dots;
			for (size_t i = 0; i < shown.size(); i = HE::uiUtf8Next(shown, i))
				dots += "\xE2\x80\xA2";
			shown.swap(dots);
		}
		if (!shown.empty())
		{
			const float fs = propFloatOr(n, "FontSize", 18.0f) * s;
			dl->AddText(nullptr, fs, ImVec2(mn.x + 4 * s, (mn.y + mx.y - fs) * 0.5f),
				placeholder ? IM_COL32(160,160,170,140)
				            : C(propColorOr(n, "Text Color", { 1,1,1,1 })), shown.c_str());
		}
		break;
	}
	case UIWidgetType::ComboBox:
	{
		// Background and outline are the surface, drawn above.
		// Current option = Options[Selected Index].
		std::string shown;
		const UIPropValue opts = n.getProp("Options");
		const int idx = n.getProp("Selected Index").i;
		if (opts.type == UIPropType::StringList && idx >= 0 && idx < (int)opts.list.size())
			shown = opts.list[idx];
		const float fs = propFloatOr(n, "FontSize", 18.0f) * s;
		if (!shown.empty())
			dl->AddText(nullptr, fs, ImVec2(mn.x + 4 * s, (mn.y + mx.y - fs) * 0.5f),
				C(propColorOr(n, "Text Color", { 1,1,1,1 })), shown.c_str());
		// Dropdown arrow.
		const float ax = mx.x - (mx.y - mn.y) * 0.5f, ay = (mn.y + mx.y) * 0.5f;
		const float ar = std::max(2.0f, (mx.y - mn.y) * 0.14f);
		dl->AddTriangleFilled(ImVec2(ax - ar, ay - ar * 0.6f), ImVec2(ax + ar, ay - ar * 0.6f),
			ImVec2(ax, ay + ar * 0.6f), IM_COL32(200,200,210,180));
		break;
	}
	default: break;
	}

	// The Image's outline last, over the picture it just drew — the one type
	// whose surface is not a colour, so the fill half of this is skipped.
	if (n.type() == UIWidgetType::Image && n.borderWidth > 0.0f)
		drawSurfacePreview(dl, n, mn, mx, s, glm::vec4(0.0f), alpha, dim,
		                   nullptr, /*drawFill=*/false);
}

// ── Embedded widgets in the designer ─────────────────────────────────────────
// A WidgetRef is grafted in by the RUNTIME, so the designer used to show only
// the slot it would fill. That made a page of embedded rows an arrangement of
// empty boxes. This draws the referenced widget where it will actually be: its
// tree is laid out against a canvas the size of the ref element's rect, which
// is exactly what the runtime does when it anchors that asset's roots inside it.
//
// The parsed trees are cached by path and re-read whenever the asset's JSON
// changes, so editing the embedded widget in another tab shows up here without
// re-parsing it every frame in every ref.
const HE::UIWidgetTree* embeddedTreeFor(AppContext& ctx, const std::string& path)
{
	if (path.empty() || !ctx.contentManager) return nullptr;
	const HE::UUID id = ctx.contentManager->loadAsset(path);
	const UIWidgetAsset* asset = id == HE::UUID{} ? nullptr : ctx.contentManager->getWidget(id);
	if (!asset) return nullptr;

	struct Cached { std::string json; HE::UIWidgetTree tree; bool ok = false; };
	static std::unordered_map<std::string, Cached> s_cache;
	Cached& c = s_cache[path];
	if (c.json != asset->treeJson)
	{
		c.json = asset->treeJson;
		c.ok   = HE::uiWidgetTreeFromJson(asset->treeJson, c.tree);
	}
	return c.ok ? &c.tree : nullptr;
}

// One element of a tree, with the two things every element needs resolved
// first: its picture (from the thumbnail cache) and the inherited opacity /
// disabled dim. Shared by the page itself and by everything embedded in it.
void drawElementIn(ImDrawList* dl, AppContext& ctx, const HE::UIWidgetTree& tree,
                   const UIElement& n, const ImVec2& mn, const ImVec2& mx, float s)
{
	void* texHandle = nullptr;
	if (n.hasTextureSlot() && !n.texture.empty() && n.material.empty() && ctx.contentManager)
		texHandle = AssetThumbnailCache::get(ctx.contentManager->contentRoot() + "/" + n.texture);
	const float alpha = HE::uiElementEffectiveOpacity(tree, n);
	const float dim   = HE::uiElementEffectiveEnabled(tree, n) ? 1.0f : HE::kUIDisabledDim;
	drawElementPreview(dl, n, mn, mx, s, texHandle, alpha, dim);
}

// Draw `tree` into the rect [mn, mx] — the whole tree, in paint order, with the
// same clipping rule the runtime uses. `depth` bounds the recursion so a circle
// of widgets embedding each other cannot hang the editor.
void drawEmbeddedTree(ImDrawList* dl, AppContext& ctx, const HE::UIWidgetTree& tree,
                      const ImVec2& mn, const ImVec2& mx, float s, int depth)
{
	constexpr int kMaxDepth = 4;
	if (depth > kMaxDepth) return;

	// The ref element's rect is this widget's SCREEN, so its own canvas meets it
	// by exactly the rule a real screen uses — the same call, and therefore the
	// same answer the runtime gives. Stretch scales it into the slot;
	// ConstantPixel leaves its units alone and lets its anchors do the placing.
	//
	// The slot has to be handed over in CANVAS UNITS, not in the screen pixels
	// the designer happens to be zoomed to: under Stretch the zoom cancels out
	// either way, but under ConstantPixel "one unit is one pixel" would have
	// meant one SCREEN pixel, and the embedded widget came out 1/zoom too big.
	const float slotW = std::max(1.0f, (mx.x - mn.x) / std::max(0.0001f, s));
	const float slotH = std::max(1.0f, (mx.y - mn.y) / std::max(0.0001f, s));
	const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(tree, slotW, slotH);
	// canvas.scale converts one of ITS units into a host canvas unit; `s` then
	// takes that to the screen.
	const float subX = canvas.scaleX * s, subY = canvas.scaleY * s;
	auto toScreen = [&](float x, float y)
	{ return ImVec2(mn.x + x * subX, mn.y + y * subY); };

	// A copy, because auto-size mutates the tree it measures and the cached one
	// must stay as the asset wrote it.
	HE::UIWidgetTree laid = tree;
	HE::uiApplyAutoSize(laid, &canvas);
	HE::uiUpdateScrollExtents(laid);

	struct Item { const UIElement* n; int key; HE::UIWidgetRect r; };
	std::vector<Item> items;
	for (const auto& ep : laid.elements)
	{
		const UIElement& e = *ep;
		if (!HE::uiElementEffectiveVisible(laid, e)) continue;
		int d = 0;
		for (const UIElement* c = &e; c->parentId != 0 && d < 255; ++d)
		{
			const UIElement* p = laid.find(c->parentId);
			if (!p) break;
			c = p;
		}
		items.push_back({ &e, e.layer * 256 + d, HE::uiElementRect(laid, e, &canvas) });
	}
	std::stable_sort(items.begin(), items.end(),
		[](const Item& a, const Item& b){ return a.key < b.key; });

	for (const Item& it : items)
	{
		const ImVec2 emn = toScreen(it.r.x, it.r.y);
		const ImVec2 emx = toScreen(it.r.x + it.r.w, it.r.y + it.r.h);
		HE::UIWidgetRect clip{};
		const bool clipped = HE::uiElementClipRect(laid, *it.n, clip, &canvas);
		if (clipped)
		{
			if (clip.w <= 0.0f || clip.h <= 0.0f) continue;
			dl->PushClipRect(toScreen(clip.x, clip.y),
			                 toScreen(clip.x + clip.w, clip.y + clip.h), true);
		}
		// Font sizes and corner radii scale with the EMBEDDED widget's factor,
		// not the page's: inside here, one of its units is subY screen pixels.
		drawElementIn(dl, ctx, laid, *it.n, emn, emx, subY);
		// …and a widget inside the embedded widget, one level further down.
		if (it.n->type() == UIWidgetType::WidgetRef)
			if (const HE::UIWidgetTree* sub =
				embeddedTreeFor(ctx, it.n->getProp("Widget").s))
				drawEmbeddedTree(dl, ctx, *sub, emn, emx, subY, depth + 1);
		if (clipped) dl->PopClipRect();
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

	// What the designer lays out in: the authored canvas, or — once a preview
	// screen is picked in Details → Canvas — exactly what the runtime resolves
	// for that screen. Under a uniform scale mode that canvas has a DIFFERENT
	// size than the authored one, and seeing that is the entire point: it is
	// where an edge-anchored element proves it still reaches the edge.
	const bool previewing = st.previewIndex != 0 && st.previewW > 0.0f && st.previewH > 0.0f;
	const HE::UIWidgetCanvas previewCanvas = previewing
		? HE::uiResolveCanvas(st.tree, st.previewW, st.previewH)
		: HE::UIWidgetCanvas{};
	const HE::UIWidgetCanvas* layoutCanvas = previewing ? &previewCanvas : nullptr;
	const float canvasW = previewing ? previewCanvas.width  : st.tree.canvasWidth;
	const float canvasH = previewing ? previewCanvas.height : st.tree.canvasHeight;

	// Fit scale, then user zoom / pan.
	const float fit = std::min(avail.x / canvasW, avail.y / canvasH) * 0.92f;
	const float s = std::max(0.02f, fit * st.zoom);
	const ImVec2 canvasPx(canvasW * s, canvasH * s);
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
			st.pan.x += mouse.x - (origin.x + (avail.x - canvasW * s2) * 0.5f
			                       + st.pan.x + before.x * s2);
			st.pan.y += mouse.y - (origin.y + (avail.y - canvasH * s2) * 0.5f
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
	HE::uiApplyAutoSize(st.tree, layoutCanvas);

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
		items.push_back({ &n, n.layer * 256 + depth, depth,
		                  elementCanvasRect(st.tree, n, layoutCanvas) });
	}
	std::stable_sort(items.begin(), items.end(),
		[](const DrawItem& a, const DrawItem& b){ return a.layer < b.layer; });

	dl->PushClipRect(cTL, ImVec2(cTL.x + canvasPx.x, cTL.y + canvasPx.y), true);
	for (const DrawItem& it : items)
	{
		const ImVec2 mn = toScreen(it.r.mn), mx = toScreen(it.r.mx);
		// A clipping ancestor cuts this element off in the designer exactly as
		// it will at runtime (the scissor there, an ImGui clip rect here) —
		// otherwise "Clip children" is a setting whose effect you cannot see
		// until you press play.
		HE::UIWidgetRect clip{};
		const bool clipped = HE::uiElementClipRect(st.tree, *it.n, clip, layoutCanvas);
		if (clipped)
		{
			if (clip.w <= 0.0f || clip.h <= 0.0f) continue;
			const ImVec2 cmn = toScreen(ImVec2(clip.x, clip.y));
			const ImVec2 cmx = toScreen(ImVec2(clip.x + clip.w, clip.y + clip.h));
			dl->PushClipRect(cmn, cmx, true);
		}
		// The element itself: its picture from the thumbnail cache, and the
		// inherited opacity / disabled dim the runtime applies — a menu authored
		// at half opacity has to LOOK half here too.
		drawElementIn(dl, ctx, st.tree, *it.n, mn, mx, s);
		// A WidgetRef shows the widget it embeds, laid out in its slot exactly
		// as the runtime will graft it in. Only when the reference does NOT
		// resolve does the slot fall back to naming what it is missing.
		if (it.n->type() == UIWidgetType::WidgetRef)
		{
			const std::string wp = it.n->getProp("Widget").s;
			if (const HE::UIWidgetTree* sub = embeddedTreeFor(ctx, wp))
				drawEmbeddedTree(dl, ctx, *sub, mn, mx, s, 0);
			else
			{
				const std::string label = wp.empty()
					? std::string("(no widget)")
					: std::filesystem::path(wp).stem().string() + " (missing)";
				dl->AddText(nullptr, 12.0f * std::max(0.6f, s), ImVec2(mn.x + 4, mn.y + 4),
				            IM_COL32(190, 230, 200, 200), label.c_str());
			}
		}
		if (clipped) dl->PopClipRect();
	}
	dl->PopClipRect();

	// ── Selection outline, resize handles, anchor marker ─────────────────────
	const float hs = 4.0f; // handle half-size in px
	int hoveredHandle = -1;
	UIElement* sel = st.tree.find(st.selected);
	if (sel)
	{
		const Rect selRect = elementCanvasRect(st.tree, *sel, layoutCanvas);
		const ImVec2 mn = toScreen(selRect.mn), mx = toScreen(selRect.mx);
		dl->AddRect(mn, mx, IM_COL32(255, 170, 40, 255), 0, 0, 2.0f);

		// A child of a layout box is placed BY the box: no anchor marker and no
		// handles, because dragging either would be a control that does nothing.
		const UIElement* boxParent = sel->parentId != 0 ? st.tree.find(sel->parentId) : nullptr;
		if (boxParent && !boxParent->laysOutChildren()) boxParent = nullptr;
		if (boxParent)
		{
			const Rect br = elementCanvasRect(st.tree, *boxParent, layoutCanvas);
			const ImVec2 bmn = toScreen(br.mn), bmx = toScreen(br.mx);
			dl->AddRect(bmn, bmx, IM_COL32(120, 190, 255, 160), 0, 0, 1.5f);
		}
		else
		{

		// Anchor marker inside the parent rect: a crosshair while the anchor is
		// a point, the anchored rectangle itself once it spans a side — that
		// outline IS the thing the element now follows when the parent resizes,
		// so it has to be visible rather than inferred from the details panel.
		const HE::UIWidgetRect ar = HE::uiElementAnchorRect(st.tree, *sel, layoutCanvas);
		const ImVec2 amn = toScreen(ImVec2(ar.x, ar.y));
		const ImVec2 amx = toScreen(ImVec2(ar.x + ar.w, ar.y + ar.h));
		if (ar.w <= 0.01f && ar.h <= 0.01f)
		{
			dl->AddCircle(amn, 5.0f, IM_COL32(255, 170, 40, 200), 0, 1.5f);
			dl->AddLine(ImVec2(amn.x - 8, amn.y), ImVec2(amn.x + 8, amn.y), IM_COL32(255,170,40,160));
			dl->AddLine(ImVec2(amn.x, amn.y - 8), ImVec2(amn.x, amn.y + 8), IM_COL32(255,170,40,160));
		}
		else
		{
			// Inflated by a hair so it reads as the frame around the element
			// rather than as a second outline drawn on top of it.
			dl->AddRect(ImVec2(amn.x - 2.0f, amn.y - 2.0f), ImVec2(amx.x + 2.0f, amx.y + 2.0f),
			            IM_COL32(255, 170, 40, 150), 0, 0, 1.5f);
			const ImVec2 corners[4] = { amn, ImVec2(amx.x, amn.y), ImVec2(amn.x, amx.y), amx };
			for (const ImVec2& c : corners)
				dl->AddCircleFilled(c, 3.0f, IM_COL32(255, 170, 40, 200));
		}

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
		} // end of the freely-placed (non-box-child) branch
	}

	// The cursor says what the handle under it does — a plain arrow over a
	// resize grip is the editor claiming there is nothing to grab. Handles are
	// 0..3 corners (TL, TR, BL, BR) then 4..7 edges (T, B, L, R); while a drag
	// is running the cursor follows the handle being HELD, not the one hovered.
	{
		static const ImGuiMouseCursor kHandleCursor[8] = {
			ImGuiMouseCursor_ResizeNWSE, ImGuiMouseCursor_ResizeNESW,
			ImGuiMouseCursor_ResizeNESW, ImGuiMouseCursor_ResizeNWSE,
			ImGuiMouseCursor_ResizeNS,   ImGuiMouseCursor_ResizeNS,
			ImGuiMouseCursor_ResizeEW,   ImGuiMouseCursor_ResizeEW };
		const int active = (st.dragMode == 2 && st.resizeHandle >= 0)
			? st.resizeHandle : (hovered ? hoveredHandle : -1);
		if (active >= 0 && active < 8)
			ImGui::SetMouseCursor(kHandleCursor[active]);
		else if (st.dragMode == 1)
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);   // moving it
	}

	// ── Mouse interaction ─────────────────────────────────────────────────────
	// `items` is in paint order, so walking it backwards is walking the stack
	// from the top down — the first rect the point falls into is the element
	// that is drawn over all the others there.
	auto topmostAt = [&](const ImVec2& canvasPt) -> int
	{
		for (auto it = items.rbegin(); it != items.rend(); ++it)
		{
			if (canvasPt.x < it->r.mn.x || canvasPt.x > it->r.mx.x ||
			    canvasPt.y < it->r.mn.y || canvasPt.y > it->r.mx.y) continue;
			// The part of an element a clipping ancestor cuts away is not on
			// screen, so it cannot be clicked here either — same rule the
			// runtime hit test follows.
			HE::UIWidgetRect clip{};
			if (HE::uiElementClipRect(st.tree, *it->n, clip, layoutCanvas))
				if (canvasPt.x < clip.x || canvasPt.x > clip.x + clip.w ||
				    canvasPt.y < clip.y || canvasPt.y > clip.y + clip.h) continue;
			return it->n->id;
		}
		return 0;
	};

	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		st.hasPendingPick = false;
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
			// Elements that place themselves are the only draggable ones: inside
			// a layout box the position is computed, so a drag would move a
			// number nothing reads. Re-ordering there is the hierarchy's job.
			auto draggable = [&](const UIElement* n2)
			{
				if (!n2) return false;
				const UIElement* par = n2->parentId != 0 ? st.tree.find(n2->parentId) : nullptr;
				return !par || !par->laysOutChildren();
			};
			const ImVec2 cpt = toCanvas(mouse);
			const int top = topmostAt(cpt);

			// A press inside the CURRENT selection grabs that selection, even
			// when something else lies over it — otherwise an element under
			// another one could be selected from the hierarchy and then never
			// moved, because the very press that starts the drag re-picks the
			// thing on top.
			//
			// The pick is only deferred, not lost: a press that comes up again
			// without the mouse having travelled was a click, and a click still
			// selects what is on top. So the selection is sticky for DRAGGING
			// and never sticky for LOOKING — which is what keeps a full-screen
			// panel from becoming a selection one can no longer get out of.
			bool grabSelection = false;
			if (sel && sel->id != top && draggable(sel))
			{
				const Rect sr = elementCanvasRect(st.tree, *sel, layoutCanvas);
				grabSelection = cpt.x >= sr.mn.x && cpt.x <= sr.mx.x &&
				                cpt.y >= sr.mn.y && cpt.y <= sr.mx.y;
			}
			if (grabSelection)
			{
				st.pendingPick    = top;
				st.hasPendingPick = true;
			}
			else
			{
				st.selected = top;
			}
			if (UIElement* n2 = st.tree.find(st.selected); n2 && draggable(n2))
			{
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
		// Nothing moves until the mouse has actually travelled. A few pixels of
		// slack is what separates a click from a drag — without it every click
		// nudges what it selects by a pixel and pushes an undo step for it, and
		// the deferred pick above would never fire.
		const float travel = std::max(std::abs(mouse.x - st.dragStartMouse.x),
		                              std::abs(mouse.y - st.dragStartMouse.y));
		if (!st.dragDidEdit && travel < 4.0f) n2 = nullptr;
		if (n2)
		{
			const ImVec2 d((mouse.x - st.dragStartMouse.x) / s,
			               (mouse.y - st.dragStartMouse.y) / s);
			st.dragDidEdit = true;
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
				// The floor is on the RESULTING rect, not on the field: on a
				// stretched axis the field is the difference to the anchored
				// span and is normally negative (an inset), so clamping it at 1
				// would make those handles unusable.
				const HE::UIWidgetRect ar = HE::uiElementAnchorRect(st.tree, *n2, layoutCanvas);
				nx = std::max(1.0f - ar.w, nx);
				ny = std::max(1.0f - ar.h, ny);
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
		// It never became a drag, so it was a click after all: the pick the
		// press held back happens now.
		else if (st.hasPendingPick) st.selected = st.pendingPick;
		st.hasPendingPick = false;
		st.dragMode = 0;
		st.resizeHandle = -1;
	}

	// ── Palette drop onto the canvas (new element; nested when over a container) ──
	// Where a drop LANDS: the topmost container under the cursor, 0 = the canvas
	// itself. Same painter order as the pick, so what you see on top is what
	// takes the drop.
	auto containerAt = [&](const ImVec2& canvasPt) -> int
	{
		for (auto it = items.rbegin(); it != items.rend(); ++it)
			if (it->n->acceptsChildren() &&
			    canvasPt.x >= it->r.mn.x && canvasPt.x <= it->r.mx.x &&
			    canvasPt.y >= it->r.mn.y && canvasPt.y <= it->r.mx.y)
				return it->n->id;
		return 0;
	};
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_UIWIDGET_NEW"))
		{
			const int t = *static_cast<const int*>(p->Data);
			const ImVec2 cpt = toCanvas(mouse);
			st.selected = addElementAt(st, static_cast<UIWidgetType>(t), containerAt(cpt), &cpt);
			commitEdit(st, ctx);
		}
		// One of the project's own widgets, dropped where the cursor is.
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_UIWIDGET_REF"))
		{
			const ImVec2 cpt = toCanvas(mouse);
			st.selected = addWidgetRefAt(st, static_cast<const char*>(p->Data),
			                             containerAt(cpt), &cpt);
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

// The widget editor is the one frontend with something to add: a node bound to
// a UI element says WHICH element. Everything else is the shared table, so only
// the three element-bearing cases live here.
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
		default:
			return HGH::defaultNodeTitle(n);
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
	                    "Widget", "UI", "Array", "Set", "Map", "Debug" },
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
// Deliberately NOT shared with LevelScriptPanel's drawVariables, though the ROW
// itself now is (HcGraphHost::variableRow — a variable has to look the same
// wherever it is listed, and the user picks that look once, on Preferences ▸
// Editor ▸ HorizonCode). What differs is everything around the row: this one
// opens with a UI-element browser the other frontends have nothing to put in
// and drags a "HE_UIWGRAPH_ELEM"/"HE_UIWGRAPH_VAR" payload.
void drawGraphVariables(State& st, AppContext& ctx)
{
	// ── Widget elements (drag → Get/Set a UI element property) ────────────────
	// SeparatorText like every other category in this panel, even though this one
	// has nothing to add to it: the sections that DO carry a "+" now draw a rule,
	// and a lone heading in the old style reads as a different kind of thing.
	ImGui::SeparatorText("Widget Elements");
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
	if (EditorWidgets::sectionHeaderAdd("Variables", "##addvar", "Add a variable"))
	{
		HC::Variable v;
		v.name = HGH::uniqueVarName(st.graph);
		v.type = PT::Float;
		st.graph.variables.push_back(v);
		st.selectedVar = v.name;
		st.selectedGraphNode = 0;
		commitEdit(st, ctx);
	}

	// Read once for the whole list, not once per row: it goes through the config
	// store, and every row in a frame has to agree anyway.
	const HcEditorUtil::VariableRowStyle rowStyle = HGH::variableRowStyle();

	auto varRow = [&](const HC::Variable& v)
	{
		if (HGH::variableRow(v, st.selectedVar == v.name, rowStyle))
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
		// The type is on the row itself now, so the hover only has to say what
		// dragging does.
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s — drag to graph for Get/Set",
			                  HGH::variableTypeLabel(v).c_str());
	};
	for (const auto& v : st.graph.variables)
		if (v.scope == 0) varRow(v);

	// Function-locals of the OPEN function sub-graph: fresh per invocation,
	// usable only inside that function.
	if (st.currentGraph != 0)
	{
		if (EditorWidgets::sectionHeaderAdd("Local Variables", "##addlvar", "Add a local variable — reset to its default on every call"))
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
		for (const auto& v : st.graph.variables)
			if (v.scope == st.currentGraph) varRow(v);
	}

	// ── Graphs: the event graph + one sub-graph per function ──────────────────
	// Named, like the level script panel names it: the Event Graph entry used to
	// sit under nothing at all, which left it looking like a stray row belonging
	// to the variable list above it.
	ImGui::SeparatorText("Graphs");
	{
		HE::Ed::Help::Scope helpScope("UI Graph");
		if (EditorWidgets::selectable("Event Graph", st.currentGraph == 0))
		{ st.currentGraph = 0; st.selectedGraphNode = 0; st.selectedVar.clear(); }
	}

	if (EditorWidgets::sectionHeaderAdd("Functions", "##addfn", "Add a function"))
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
			// A variable's Name and a node's Name are not the same question, so
			// they are not the same scope either.
			HE::Ed::Help::Scope helpScope("UI Variable");
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
			EditorWidgets::helpForLabel("Name");
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
				EditorWidgets::helpForLabel("Access");
			}

			// Single value, or a container of the type. Changing it re-types the
			// matching Get/Set nodes' value pins and drops now-mismatched links.
			if (HcEditorUtil::drawContainerPicker("Container", v->isArray, v->container) ||
			    (v->kind() == HorizonCode::ContainerKind::Map &&
			     HcEditorUtil::drawKeyTypePicker("Key", v->keyType, v->keyTypeName)))
			{
				// The authored slots belong to the OLD shape.
				v->defaultItems.clear();
				v->defaultKeys.clear();
				for (auto& gn : st.graph.nodes)
					if ((gn.type == NT::GetVariable || gn.type == NT::SetVariable) && gn.s == v->name)
					{
						gn.isArray = v->isArray; gn.container = v->container;
						gn.keyType = v->keyType; gn.keyTypeName = v->keyTypeName;
						const HGH::PinRanges r = HGH::pinRanges(gn);
						HGH::removePinLinks(st.graph, gn.id, gn.type == NT::GetVariable ? r.dataOut0 : r.dataIn0);
					}
				commitEdit(st, ctx);
			}

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
						EditorWidgets::helpForLabel("Position##vdef");
						ed |= ImGui::DragFloat3("Rotation##vdef", &v->trot.x, 0.5f);
						EditorWidgets::helpForLabel("Rotation##vdef");
						ed |= ImGui::DragFloat3("Scale##vdef",    &v->tscl.x, 0.05f);
						EditorWidgets::helpForLabel("Scale##vdef");
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

	HE::Ed::Help::Scope helpScope("UI Graph Node");

	bool committed = false;
	ImGui::TextDisabled("%s", HC::nodeDisplayName(n->type));
	ImGui::Separator();

	auto elementCombo = [&](const char* label, bool includeAny)
	{
		const std::string cur = includeAny && n->elem == 0 ? std::string("(Any)")
			: elemLabel(st, n->elem);
		if (ImGui::BeginCombo(label, cur.c_str()))
		{
			if (includeAny && EditorWidgets::selectable("(Any)", n->elem == 0))
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
			EditorWidgets::helpForLabel("Event");
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
		const bool propOpen = ImGui::BeginCombo("Property", n->s.empty() ? "(none)" : n->s.c_str());
		if (!propOpen) EditorWidgets::helpForLabel("Property");
		if (propOpen)
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
		EditorWidgets::helpForLabel("Name");
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
		EditorWidgets::helpForLabel("Access");
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
		const bool fnOpen = ImGui::BeginCombo("Function", n->s.empty() ? "(none)" : n->s.c_str());
		if (!fnOpen) EditorWidgets::helpForLabel("Function");
		if (fnOpen)
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
		EditorWidgets::helpForLabel("Event");
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
		HE::Ed::Help::Scope helpScope("UI Graph");
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
		// Same rule as everywhere a submenu carries an entry: ask while the header
		// is still the last item, and only while the submenu is shut.
		const bool getOpen = ImGui::BeginMenu("Get", !props.empty());
		if (!getOpen) EditorWidgets::helpForLabel("Get");
		if (getOpen)
		{
			for (const UIPropDesc& pd : props)
				if (ImGui::MenuItem(pd.name.c_str())) makePropNode(NT::GetProperty, pd);
			ImGui::EndMenu();
		}
		const bool setOpen = ImGui::BeginMenu("Set", !props.empty());
		if (!setOpen) EditorWidgets::helpForLabel("Set");
		if (setOpen)
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
		HE::Ed::Help::Scope helpScope("UI Graph");
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
			if (v)
			{
				nn->typeName = v->typeName;
				nn->container = v->container;   // Array vs Set vs Map, and its key
				nn->keyType = v->keyType; nn->keyTypeName = v->keyTypeName;
			}
			st.selectedGraphNode = id;
			st.selectedVar.clear();
			commitEdit(st, ctx);
		};
		if (EditorWidgets::menuItem("Get", nullptr, false, scopeOk)) makeVarNode(NT::GetVariable);
		if (EditorWidgets::menuItem("Set", nullptr, false, scopeOk)) makeVarNode(NT::SetVariable);
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
				// WidgetRef is not offered as a bare type: every widget of the
				// project is listed by name under "User Defined" below, and an
				// empty reference you then have to point somewhere is the same
				// thing with an extra step. It stays in the REGISTRY, though —
				// that is what loads the type back out of a saved widget.
				if (t == UIWidgetType::WidgetRef) continue;
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
						parent = selN->acceptsChildren() ? selN->id : selN->parentId;
					st.selected = addElementAt(st, t, parent, nullptr);
					commitEdit(st, ctx);
				}
			}

			// ── User Defined ─────────────────────────────────────────────────
			// Every other widget in the project, ready to be dropped in as a
			// WidgetRef. This one is left out: a widget that embeds itself is
			// refused at runtime, so it is not offered here either.
			{
				const auto widgets = HcEditorUtil::listAssets(ctx.contentManager,
				                                              HE::AssetType::Widget);
				int offered = 0;
				for (const auto& a : widgets) if (a.path != st.relPath) ++offered;

				ImGui::Spacing();
				ImGui::TextDisabled("User Defined");
				ImGui::Separator();
				if (offered == 0)
					ImGui::TextDisabled("(no other widgets yet)");
				for (const auto& a : widgets)
				{
					if (a.path == st.relPath) continue;   // never itself
					ImGui::PushID(a.path.c_str());
					const bool clicked = ImGui::Button(a.label.c_str(), ImVec2(-1.0f, 0));
					if (ImGui::BeginDragDropSource())
					{
						// The path travels with the payload (including its
						// terminator) — the drop sites turn it into a WidgetRef.
						ImGui::SetDragDropPayload("HE_UIWIDGET_REF", a.path.c_str(),
						                          a.path.size() + 1);
						ImGui::TextUnformatted(a.label.c_str());
						ImGui::EndDragDropSource();
					}
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
						ImGui::SetTooltip("%s\nEmbedded as a WidgetRef: authored once,\n"
						                  "used here.", a.path.c_str());
					if (clicked)
					{
						int parent = 0;
						if (const UIElement* selN = st.tree.find(st.selected))
							parent = selN->acceptsChildren() ? selN->id : selN->parentId;
						st.selected = addWidgetRefAt(st, a.path, parent, nullptr);
						commitEdit(st, ctx);
					}
					ImGui::PopID();
				}
			}

			ImGui::Spacing();
			ImGui::TextDisabled("Hierarchy");
			ImGui::Separator();

			// Canvas root: select-none target + reparent-to-root drop target.
			HE::Ed::Help::Scope helpScope("UI Hierarchy");
			const bool rootSel = st.selected == 0;
			if (EditorWidgets::selectable("Canvas##uiwroot", rootSel)) st.selected = 0;
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
				if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_UIWIDGET_REF"))
				{
					st.selected = addWidgetRefAt(st, static_cast<const char*>(p->Data), 0, nullptr);
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
					HE::Ed::Help::Scope helpScope("UI Graph");
					if (EditorWidgets::smallButton("Show the node that failed"))
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

	// ── Application projects: save as you design ─────────────────────────────
	// The live preview rebuilds itself when an asset is SAVED, so an unsaved
	// change is an invisible one — and "why is nothing happening" is a bad first
	// experience of a live preview (docs/he-apps-plan.md E2).
	//
	// Held back until the mouse is up and no field is being typed into: saving on
	// every frame of a drag would rebuild the preview dozens of times across one
	// gesture, and each rebuild throws the app's state away.
	if (ctx.appLivePreview && st.dirty &&
	    !ImGui::IsAnyMouseDown() && !ImGui::IsAnyItemActive())
		saveState(st, ctx);

	ImGui::End();
}

} // namespace UIEditorPanel
