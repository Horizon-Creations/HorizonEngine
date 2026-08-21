#pragma once
#include <UIWidget/UIElement.h>
#include <Types/Defines.h>
#include <memory>
#include <string>
#include <vector>

namespace HE {

// ── UI widget tree ───────────────────────────────────────────────────────────
// The design-time model of a UI Widget asset: a tree of polymorphic UIElements
// (JSON in CHUNK_UIWT). The widget editor edits it; at runtime WidgetManager
// holds a deep copy per live widget. Elements own their subclass data; this
// container owns the elements and provides id lookup + hierarchy queries.

struct HE_API UIWidgetTree
{
    float canvasWidth  = 1920.0f;
    float canvasHeight = 1080.0f;
    int   nextId = 1;
    std::vector<std::unique_ptr<UIElement>> elements; // sibling draw/hierarchy order

    UIWidgetTree() = default;
    UIWidgetTree(const UIWidgetTree& o) { *this = o; }
    UIWidgetTree& operator=(const UIWidgetTree& o)
    {
        if (this == &o) return *this;
        canvasWidth = o.canvasWidth; canvasHeight = o.canvasHeight; nextId = o.nextId;
        elements.clear();
        elements.reserve(o.elements.size());
        for (const auto& e : o.elements) elements.push_back(e->clone());
        return *this;
    }
    UIWidgetTree(UIWidgetTree&&) noexcept = default;
    UIWidgetTree& operator=(UIWidgetTree&&) noexcept = default;

    UIElement*       find(int id);
    const UIElement* find(int id) const;
    // All direct children of `parentId` (0 = canvas roots), in vector order.
    std::vector<int> childrenOf(int parentId) const;
    // True when `ancestorId` is `id` itself or one of its ancestors.
    bool isDescendantOf(int id, int ancestorId) const;
    // Remove an element and its whole subtree.
    void removeSubtree(int id);
    // Add an owned element (assigns id from nextId, returns it).
    int  add(std::unique_ptr<UIElement> e);
    // Convenience: create + add a default element of `type`.
    int  add(UIWidgetType type);
};

// JSON round-trip (schema-evolution friendly). Returns false on parse failure.
HE_API std::string uiWidgetTreeToJson(const UIWidgetTree& tree);
HE_API bool        uiWidgetTreeFromJson(const std::string& json, UIWidgetTree& out);

// ── Item-level JSON ─────────────────────────────────────────────────────────
// One element, in EXACTLY the form uiWidgetTreeToJson() puts into the tree's
// array — the tree serializers are implemented on top of this. Collaboration
// addresses a single element at a time (CollabDocSync); a separate serializer
// for it would drift from the on-disk one.
//
// Reading yields a NEW element rather than filling an existing one: the widget
// type is part of an element's identity, so a Button cannot become a Slider in
// place. nullptr = the JSON did not parse.
HE_API std::string                 uiElementToJson(const UIElement& e);
HE_API std::unique_ptr<UIElement>  uiElementFromJson(const std::string& json);

// ── Anchors (shared by the editor and the runtime) ──────────────────────────
// The anchor is a rectangle in the parent's space (UIElement::anchorMin/Max).
// These name the sixteen rectangles worth having a button for — the same grid
// UMG shows: index = row * 4 + col, where col 0/1/2 is left/centre/right and
// col 3 stretches across the parent's whole width; row 0/1/2 is top/middle/
// bottom and row 3 stretches down its whole height. So 0 is the top-left
// point, 15 fills the parent, 3 is the whole top side, 12 the whole left side.
inline constexpr int kUIAnchorPresetCount = 16;
inline constexpr int kUIAnchorFill = 15;
HE_API void uiAnchorPresetRect(int preset, float& minX, float& minY, float& maxX, float& maxY);
// Which preset an element's anchor rect is, or -1 for a rectangle none of them
// name (nothing writes one today, but the model allows it and the editor must
// not claim it is something it is not).
HE_API int  uiAnchorPresetOf(const UIElement& e);
// Set the anchor rect, leaving pos/size alone: the element's rect MOVES, the
// way it does in UMG when you re-anchor without holding the layout.
HE_API void uiSetAnchorPreset(UIElement& e, int preset);
// Re-anchor and keep the element exactly where it is: pos/size are recomputed
// against the new anchor rect, so the only thing that changes is what the
// element does when its parent resizes. This is what the editor's anchor grid
// uses — an anchor change that teleports the element is a change nobody can
// aim.
HE_API void uiReanchorKeepingRect(const UIWidgetTree& tree, UIElement& e, int preset);

// The legacy 9-point anchor (0 = TopLeft … 8 = BottomRight), as the on-disk
// "anchor" field still spells it. Reading maps it to a point rectangle;
// writing keeps old documents byte-identical.
HE_API void uiAnchorFromLegacyPoint(UIElement& e, int ninePoint);
HE_API int  uiAnchorLegacyPointOf(const UIElement& e); // -1 = not a 9-point anchor

// ── Layout (shared by the editor and the runtime) ───────────────────────────
// Element rect in CANVAS units, resolved through the parent chain (roots anchor
// to the canvas). The anchor rectangle is resolved inside the parent's rect;
// position is the offset from it, pivot shifts the rect so the pivot point
// lands there, and on a stretched axis the size adds to the anchored span.
HE_API UIWidgetRect uiElementRect(const UIWidgetTree& tree, const UIElement& e);

// The element's anchor rectangle itself, in canvas units — what the editor
// draws and what the insets below are measured against.
HE_API UIWidgetRect uiElementAnchorRect(const UIWidgetTree& tree, const UIElement& e);

// ── Insets (how a stretched axis is authored) ───────────────────────────────
// On a stretched axis "position and size" is a clumsy way to say "how far in
// from each anchored edge", so the editor shows the two insets instead. Both
// helpers are no-ops on an axis that is not stretched.
HE_API void uiAnchorInsetsX(const UIElement& e, float& left, float& right);
HE_API void uiAnchorInsetsY(const UIElement& e, float& top, float& bottom);
HE_API void uiSetAnchorInsetsX(UIElement& e, float left, float right);
HE_API void uiSetAnchorInsetsY(UIElement& e, float top, float bottom);
// False when the element or any ancestor is invisible.
HE_API bool uiElementEffectiveVisible(const UIWidgetTree& tree, const UIElement& e);
// Let every element that auto-sizes fit itself to its content. Call BEFORE
// uiElementRect so the rects already reflect the new sizes; cheap enough to run
// each frame (only text elements with AutoSize on do any work).
HE_API void uiApplyAutoSize(UIWidgetTree& tree);

} // namespace HE
