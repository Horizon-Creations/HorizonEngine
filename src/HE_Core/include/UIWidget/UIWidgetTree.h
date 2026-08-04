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

// ── Layout (shared by the editor and the runtime) ───────────────────────────
// Element rect in CANVAS units, resolved through the parent chain: the anchor
// point lies inside the parent rect (roots anchor to the canvas), position is
// the offset from it, pivot shifts the rect so the pivot point lands there.
HE_API UIWidgetRect uiElementRect(const UIWidgetTree& tree, const UIElement& e);
// False when the element or any ancestor is invisible.
HE_API bool uiElementEffectiveVisible(const UIWidgetTree& tree, const UIElement& e);
// Let every element that auto-sizes fit itself to its content. Call BEFORE
// uiElementRect so the rects already reflect the new sizes; cheap enough to run
// each frame (only text elements with AutoSize on do any work).
HE_API void uiApplyAutoSize(UIWidgetTree& tree);

} // namespace HE
