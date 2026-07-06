#include "UIEditorPanel.h"
#include "EditorApplication.h"                 // AppContext
#include <UIWidget/UIWidgetTree.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/HAsset.h>
#include <Types/Enums.h>
#include <Diagnostics/Logger.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace
{
using HE::UIWidgetNode;
using HE::UIWidgetTree;
using HE::UIWidgetType;

// ── Per-widget editor state (session-persistent, keyed by asset path) ─────────
struct State
{
	UIWidgetTree tree;
	HE::UUID     assetId{};
	bool         loaded = false;
	bool         dirty  = false;     // unsaved-to-disk tree edits
	int          selected = 0;       // selected node id (0 = none / canvas)

	// Canvas view: fit-to-window base scale × user zoom, plus pan in screen px.
	float  zoom = 1.0f;
	ImVec2 pan  = ImVec2(0.0f, 0.0f);

	// Drag interaction. mode: 0 = none, 1 = move node, 2 = resize node.
	int    dragMode = 0;
	int    resizeHandle = -1;        // 0..7: corners+edges (see handleOffsets)
	ImVec2 dragStartMouse;
	float  dragStartPos[2]  = {};
	float  dragStartSize[2] = {};
	bool   dragDidEdit = false;      // push one undo snapshot per completed drag

	// Undo/redo: JSON snapshots of the whole tree.
	std::vector<std::string> undo;
	int undoPos = -1;

	std::string name;                // filename for the header
	std::string relPath;             // content-root-relative path of this asset
};
std::map<std::string, State> g_states;

// ── Layout math (must mirror UISystem::computeScreenRect) ─────────────────────
ImVec2 anchorPoint(uint8_t a)
{
	static const ImVec2 pts[9] = {
		{0.0f,0.0f},{0.5f,0.0f},{1.0f,0.0f},
		{0.0f,0.5f},{0.5f,0.5f},{1.0f,0.5f},
		{0.0f,1.0f},{0.5f,1.0f},{1.0f,1.0f} };
	return pts[a > 8 ? 0 : a];
}

struct Rect { ImVec2 mn, mx; };

// Node rect in CANVAS units, parent-relative anchoring (same math the runtime
// uses with scale 1). Depth is bounded by tree size (reparent guards cycles).
Rect nodeCanvasRect(const UIWidgetTree& tree, const UIWidgetNode& n)
{
	Rect parent{ {0.0f, 0.0f}, { tree.canvasWidth, tree.canvasHeight } };
	if (n.parentId != 0)
		if (const UIWidgetNode* p = tree.findNode(n.parentId))
			parent = nodeCanvasRect(tree, *p);

	const ImVec2 ap = anchorPoint(n.anchor);
	const ImVec2 parentSize(parent.mx.x - parent.mn.x, parent.mx.y - parent.mn.y);
	const float x = parent.mn.x + ap.x * parentSize.x + n.posX - n.pivotX * n.sizeX;
	const float y = parent.mn.y + ap.y * parentSize.y + n.posY - n.pivotY * n.sizeY;
	return { { x, y }, { x + n.sizeX, y + n.sizeY } };
}

bool nodeVisibleInTree(const UIWidgetTree& tree, const UIWidgetNode& n)
{
	if (!n.visible) return false;
	if (n.parentId == 0) return true;
	const UIWidgetNode* p = tree.findNode(n.parentId);
	return p ? nodeVisibleInTree(tree, *p) : true;
}

ImU32 toCol32(const float c[4])
{
	return IM_COL32(int(c[0] * 255.0f), int(c[1] * 255.0f),
	                int(c[2] * 255.0f), int(c[3] * 255.0f));
}

const char* typeName(UIWidgetType t)
{
	switch (t)
	{
		case UIWidgetType::Panel:  return "Panel";
		case UIWidgetType::Image:  return "Image";
		case UIWidgetType::Text:   return "Text";
		case UIWidgetType::Button: return "Button";
	}
	return "?";
}

// ── Undo ───────────────────────────────────────────────────────────────────────
void pushUndo(State& st)
{
	const std::string snap = HE::uiWidgetTreeToJson(st.tree);
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
	HE::uiWidgetTreeFromJson(st.undo[pos], st.tree);
	st.undoPos = pos;
	if (st.selected != 0 && !st.tree.findNode(st.selected)) st.selected = 0;
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
		if (!a->treeJson.empty())
			HE::uiWidgetTreeFromJson(a->treeJson, st.tree);

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
	a->treeJson = HE::uiWidgetTreeToJson(st.tree);
	if (ctx.contentManager->saveAsset(*a)) st.dirty = false;
}

// Keep the live asset in sync on every edit, so PIE picks up unsaved edits too
// (mirrors the material editor's live-apply behavior). Disk write happens on Save.
void applyToAsset(State& st, AppContext& ctx)
{
	if (!ctx.contentManager) return;
	if (UIWidgetAsset* a = ctx.contentManager->getWidgetMutable(st.assetId))
		a->treeJson = HE::uiWidgetTreeToJson(st.tree);
}

void commitEdit(State& st, AppContext& ctx)
{
	pushUndo(st);
	st.dirty = true;
	applyToAsset(st, ctx);
}

// ── Node factory ───────────────────────────────────────────────────────────────
UIWidgetNode makeNode(UIWidgetType type)
{
	UIWidgetNode n;
	n.type = type;
	switch (type)
	{
	case UIWidgetType::Panel:
		n.sizeX = 300.0f; n.sizeY = 200.0f;
		n.color[0] = n.color[1] = n.color[2] = 0.12f; n.color[3] = 0.85f;
		break;
	case UIWidgetType::Image:
		n.sizeX = 128.0f; n.sizeY = 128.0f;
		break;
	case UIWidgetType::Text:
		n.sizeX = 200.0f; n.sizeY = 30.0f;
		n.text = "Text"; n.fontSize = 22.0f;
		break;
	case UIWidgetType::Button:
		n.sizeX = 180.0f; n.sizeY = 48.0f;
		n.color[0] = n.color[1] = n.color[2] = 0.20f; n.color[3] = 1.0f;
		n.text = "Button"; n.fontSize = 20.0f;
		break;
	}
	return n;
}

// Add a node under `parentId`, centered in the parent (or at an explicit canvas
// point when provided). Returns the new node id.
int addNodeAt(State& st, UIWidgetType type, int parentId, const ImVec2* canvasPt)
{
	UIWidgetNode n = makeNode(type);
	n.parentId = parentId;
	n.name = std::string(typeName(type));

	Rect parent{ {0,0}, { st.tree.canvasWidth, st.tree.canvasHeight } };
	if (parentId != 0)
		if (const UIWidgetNode* p = st.tree.findNode(parentId))
			parent = nodeCanvasRect(st.tree, *p);

	if (canvasPt)
	{
		// anchor TopLeft, pivot 0.5: position = drop point relative to parent TL.
		n.anchor = 0;
		n.posX = canvasPt->x - parent.mn.x;
		n.posY = canvasPt->y - parent.mn.y;
	}
	else
	{
		n.anchor = 4; // MiddleCenter
		n.posX = 0.0f; n.posY = 0.0f;
	}
	return st.tree.addNode(std::move(n));
}

int duplicateSubtree(State& st, int srcId, int parentId)
{
	const UIWidgetNode* src = st.tree.findNode(srcId);
	if (!src) return 0;
	UIWidgetNode copy = *src;
	copy.parentId = parentId;
	if (parentId == src->parentId) { copy.posX += 20.0f; copy.posY += 20.0f; }
	const int newId = st.tree.addNode(std::move(copy));
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
	UIWidgetNode* n = st.tree.findNode(nodeId);
	if (!n) return;

	const auto children = st.tree.childrenOf(nodeId);
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
	                           ImGuiTreeNodeFlags_SpanAvailWidth |
	                           ImGuiTreeNodeFlags_DefaultOpen;
	if (children.empty())      flags |= ImGuiTreeNodeFlags_Leaf;
	if (st.selected == nodeId) flags |= ImGuiTreeNodeFlags_Selected;

	const std::string label = (n->name.empty() ? typeName(n->type) : n->name.c_str())
		+ std::string("##hn") + std::to_string(nodeId);
	const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
	if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
		st.selected = nodeId;

	// Drag source: reparent by dropping onto another node (or the canvas root).
	if (ImGui::BeginDragDropSource())
	{
		ImGui::SetDragDropPayload("HE_UIWIDGET_NODE", &nodeId, sizeof(int));
		ImGui::TextUnformatted(n->name.empty() ? typeName(n->type) : n->name.c_str());
		ImGui::EndDragDropSource();
	}
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_UIWIDGET_NODE"))
		{
			const int dragged = *static_cast<const int*>(p->Data);
			if (dragged != nodeId && !st.tree.isDescendantOf(nodeId, dragged))
			{
				if (UIWidgetNode* d = st.tree.findNode(dragged))
				{
					d->parentId = nodeId;
					structureEdit = true;
				}
			}
		}
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_UIWIDGET_NEW"))
		{
			const int t = *static_cast<const int*>(p->Data);
			st.selected = addNodeAt(st, static_cast<UIWidgetType>(t), nodeId, nullptr);
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

// ── Details panel ──────────────────────────────────────────────────────────────
void drawDetails(State& st, AppContext& ctx)
{
	UIWidgetNode* n = st.tree.findNode(st.selected);
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

	ImGui::TextDisabled("%s", typeName(n->type));
	ImGui::Separator();

	ImGui::InputText("Name", &n->name);
	committed |= ImGui::IsItemDeactivatedAfterEdit();

	// Layout
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

	// Appearance
	ImGui::SeparatorText("Appearance");
	const char* colorLabel =
		n->type == UIWidgetType::Text   ? "Color" :
		n->type == UIWidgetType::Button ? "Normal" : "Tint";
	edit |= ImGui::ColorEdit4(colorLabel, n->color);
	committed |= ImGui::IsItemDeactivatedAfterEdit();

	if (n->type == UIWidgetType::Button)
	{
		edit |= ImGui::ColorEdit4("Hovered", n->hoveredColor);
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		edit |= ImGui::ColorEdit4("Pressed", n->pressedColor);
		committed |= ImGui::IsItemDeactivatedAfterEdit();
	}

	if (n->type == UIWidgetType::Text || n->type == UIWidgetType::Button)
	{
		ImGui::SeparatorText("Text");
		ImGui::InputText("Text", &n->text);
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		edit |= ImGui::DragFloat("Font Size", &n->fontSize, 0.5f, 4.0f, 256.0f);
		committed |= ImGui::IsItemDeactivatedAfterEdit();
		if (n->type == UIWidgetType::Button)
		{
			edit |= ImGui::ColorEdit4("Text Color", n->textColor);
			committed |= ImGui::IsItemDeactivatedAfterEdit();
		}
	}

	// Material (quad types only — text runs have no quad to shade).
	if (n->type != UIWidgetType::Text)
	{
		ImGui::SeparatorText("Material");
		committed |= assetSlot(ctx, "Material", n->materialPath,
		                       HE::AssetType::Material, "mat");
	}

	// Behavior script (every element type can have one).
	ImGui::SeparatorText("Script");
	committed |= assetSlot(ctx, "Script", n->scriptPath,
	                       HE::AssetType::Script, "scr");
	if (!n->scriptPath.empty())
		ImGui::TextDisabled("onClick / onHoverEnter / onHoverExit\nrun on this element in play mode");

	if (edit) st.dirty = true;
	if (committed) commitEdit(st, ctx);
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
	struct DrawItem { const UIWidgetNode* n; int layer; int depth; Rect r; };
	std::vector<DrawItem> items;
	for (const auto& n : st.tree.nodes)
	{
		if (!nodeVisibleInTree(st.tree, n)) continue;
		int depth = 0;
		for (const UIWidgetNode* c = &n; c->parentId != 0 && depth < 255; ++depth)
		{
			const UIWidgetNode* p = st.tree.findNode(c->parentId);
			if (!p) break;
			c = p;
		}
		items.push_back({ &n, n.layer * 256 + depth, depth, nodeCanvasRect(st.tree, n) });
	}
	std::stable_sort(items.begin(), items.end(),
		[](const DrawItem& a, const DrawItem& b){ return a.layer < b.layer; });

	dl->PushClipRect(cTL, ImVec2(cTL.x + canvasPx.x, cTL.y + canvasPx.y), true);
	for (const DrawItem& it : items)
	{
		const ImVec2 mn = toScreen(it.r.mn), mx = toScreen(it.r.mx);
		const UIWidgetNode& n = *it.n;
		switch (n.type)
		{
		case UIWidgetType::Panel:
		case UIWidgetType::Image:
			dl->AddRectFilled(mn, mx, toCol32(n.color));
			if (n.type == UIWidgetType::Image && n.materialPath.empty())
			{
				// Placeholder crossed box so an unstyled image is visible.
				dl->AddRect(mn, mx, IM_COL32(160,160,170,120));
				dl->AddLine(mn, mx, IM_COL32(160,160,170,90));
				dl->AddLine(ImVec2(mn.x, mx.y), ImVec2(mx.x, mn.y), IM_COL32(160,160,170,90));
			}
			if (n.type == UIWidgetType::Image && !n.materialPath.empty())
			{
				const std::string matName =
					std::filesystem::path(n.materialPath).stem().string();
				dl->AddText(nullptr, 12.0f * std::max(0.6f, s),
					ImVec2(mn.x + 3, mn.y + 3), IM_COL32(220,220,230,140), matName.c_str());
			}
			break;
		case UIWidgetType::Button:
		{
			dl->AddRectFilled(mn, mx, toCol32(n.color), 4.0f * s);
			dl->AddRect(mn, mx, IM_COL32(200,200,210,60), 4.0f * s);
			if (!n.text.empty())
			{
				const float fs = n.fontSize * s;
				const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, n.text.c_str());
				dl->AddText(nullptr, fs,
					ImVec2((mn.x + mx.x - ts.x) * 0.5f, (mn.y + mx.y - ts.y) * 0.5f),
					toCol32(n.textColor), n.text.c_str());
			}
			break;
		}
		case UIWidgetType::Text:
		{
			const float fs = n.fontSize * s;
			dl->AddText(nullptr, fs, mn, toCol32(n.color),
			            n.text.empty() ? "(empty)" : n.text.c_str());
			break;
		}
		}
	}
	dl->PopClipRect();

	// ── Selection outline, resize handles, anchor marker ─────────────────────
	const float hs = 4.0f; // handle half-size in px
	int hoveredHandle = -1;
	Rect selRect{};
	UIWidgetNode* sel = st.tree.findNode(st.selected);
	if (sel)
	{
		selRect = nodeCanvasRect(st.tree, *sel);
		const ImVec2 mn = toScreen(selRect.mn), mx = toScreen(selRect.mx);
		dl->AddRect(mn, mx, IM_COL32(255, 170, 40, 255), 0, 0, 2.0f);

		// Anchor marker inside the parent rect.
		Rect parent{ {0,0}, { st.tree.canvasWidth, st.tree.canvasHeight } };
		if (sel->parentId != 0)
			if (const UIWidgetNode* p = st.tree.findNode(sel->parentId))
				parent = nodeCanvasRect(st.tree, *p);
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
				UIWidgetNode* n2 = st.tree.findNode(hit);
				st.dragMode = 1;
				st.dragStartMouse = mouse;
				st.dragStartPos[0] = n2->posX; st.dragStartPos[1] = n2->posY;
				st.dragDidEdit = false;
			}
		}
	}

	if (st.dragMode != 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		UIWidgetNode* n2 = st.tree.findNode(st.selected);
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
				if (it->n->type == UIWidgetType::Panel &&
				    cpt.x >= it->r.mn.x && cpt.x <= it->r.mx.x &&
				    cpt.y >= it->r.mn.y && cpt.y <= it->r.mx.y)
					{ parent = it->n->id; break; }
			st.selected = addNodeAt(st, static_cast<UIWidgetType>(t), parent, &cpt);
			commitEdit(st, ctx);
		}
		ImGui::EndDragDropTarget();
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
	ImGui::SameLine();
	ImGui::TextDisabled("· %.0f × %.0f", st.tree.canvasWidth, st.tree.canvasHeight);

	ImGui::SameLine(ImGui::GetContentRegionAvail().x - 150.0f);
	if (ImGui::Button("Reset View")) { st.zoom = 1.0f; st.pan = ImVec2(0, 0); }
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
		if (st.selected != 0 &&
		    (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)))
		{
			st.tree.removeSubtree(st.selected);
			st.selected = 0;
			commitEdit(st, ctx);
		}
		if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D) && st.selected != 0)
		{
			if (const UIWidgetNode* n = st.tree.findNode(st.selected))
			{
				st.selected = duplicateSubtree(st, st.selected, n->parentId);
				commitEdit(st, ctx);
			}
		}
	}

	// ── Three-pane layout ─────────────────────────────────────────────────────
	const float leftW  = 230.0f;
	const float rightW = 300.0f;

	// Left: palette + hierarchy.
	ImGui::BeginChild("##uiw_left", ImVec2(leftW, 0), ImGuiChildFlags_Borders);
	{
		ImGui::TextDisabled("Palette");
		ImGui::Separator();
		static const UIWidgetType kTypes[] = {
			UIWidgetType::Panel, UIWidgetType::Image,
			UIWidgetType::Text, UIWidgetType::Button };
		for (UIWidgetType t : kTypes)
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
				if (const UIWidgetNode* selN = st.tree.findNode(st.selected))
					parent = selN->type == UIWidgetType::Panel ? selN->id : selN->parentId;
				st.selected = addNodeAt(st, t, parent, nullptr);
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
				if (UIWidgetNode* d = st.tree.findNode(dragged))
				{
					d->parentId = 0;
					commitEdit(st, ctx);
				}
			}
			if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_UIWIDGET_NEW"))
			{
				const int t = *static_cast<const int*>(p->Data);
				st.selected = addNodeAt(st, static_cast<UIWidgetType>(t), 0, nullptr);
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

	// Center: canvas.
	ImGui::BeginChild("##uiw_canvas",
		ImVec2(ImGui::GetContentRegionAvail().x - rightW - ImGui::GetStyle().ItemSpacing.x, 0),
		ImGuiChildFlags_Borders,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	drawCanvas(st, ctx, ImGui::GetContentRegionAvail());
	ImGui::EndChild();

	ImGui::SameLine();

	// Right: details.
	ImGui::BeginChild("##uiw_details", ImVec2(rightW, 0), ImGuiChildFlags_Borders);
	drawDetails(st, ctx);
	ImGui::EndChild();

	ImGui::End();
}

} // namespace UIEditorPanel
