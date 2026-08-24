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

// How the authored canvas maps onto the screen it ends up on.
//
// Stretch is what the runtime always did: width and height are scaled
// SEPARATELY so the canvas covers the viewport exactly. On a screen whose
// aspect differs from the authored one that distorts every element — a circle
// becomes an egg, a square button a rectangle. It stays the default so no
// existing widget changes without being asked to.
//
// Every other mode scales UNIFORMLY (one factor for both axes, nothing is
// distorted) and lets the canvas be as large as the screen actually is: the
// authored size is then a REFERENCE that only decides how big things appear,
// and the layout canvas becomes viewport / scale. That is what keeps an
// element anchored to the right edge on the right edge instead of somewhere in
// a letterbox — the anchor rectangles resolve against the real screen.
enum class UICanvasScaleMode : uint8_t
{
    Stretch = 0,     // per-axis fit, the canvas IS the reference (distorts)
    FitInside,       // uniform min(sx, sy): nothing is ever cut off
    FillOutside,     // uniform max(sx, sy): no empty space, edges may be cut off
    MatchWidth,      // uniform, the authored WIDTH is what fits
    MatchHeight,     // uniform, the authored HEIGHT is what fits
    ConstantPixel,   // no scaling: one canvas unit is one pixel
};
HE_API const char* uiCanvasScaleModeName(UICanvasScaleMode m);

// The canvas a widget actually lays out in, for one particular viewport.
// `width`/`height` are canvas units (they are the authored size for Stretch and
// ConstantPixel, and viewport/scale otherwise); `scaleX`/`scaleY` convert a
// canvas unit to a pixel and are equal for every uniform mode.
struct UIWidgetCanvas
{
    float scaleX = 1.0f, scaleY = 1.0f;
    float width  = 1920.0f, height = 1080.0f;
};

struct HE_API UIWidgetTree
{
    // The AUTHORED canvas: the design surface, and the reference resolution the
    // scale modes above measure against.
    float canvasWidth  = 1920.0f;
    float canvasHeight = 1080.0f;
    UICanvasScaleMode scaleMode = UICanvasScaleMode::Stretch;
    int   nextId = 1;
    std::vector<std::unique_ptr<UIElement>> elements; // sibling draw/hierarchy order

    UIWidgetTree() = default;
    UIWidgetTree(const UIWidgetTree& o) { *this = o; }
    UIWidgetTree& operator=(const UIWidgetTree& o)
    {
        if (this == &o) return *this;
        canvasWidth = o.canvasWidth; canvasHeight = o.canvasHeight;
        scaleMode = o.scaleMode; nextId = o.nextId;
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
// The canvas `tree` lays out in on a viewport of this pixel size — the scale
// mode above turned into concrete numbers. Passing the result to the layout
// calls below is what makes anchors resolve against the REAL screen; leaving it
// out lays out on the authored canvas, which is what the designer wants.
HE_API UIWidgetCanvas uiResolveCanvas(const UIWidgetTree& tree, float vpWidth, float vpHeight);

// Element rect in CANVAS units, resolved through the parent chain (roots anchor
// to the canvas). The anchor rectangle is resolved inside the parent's rect;
// position is the offset from it, pivot shifts the rect so the pivot point
// lands there, and on a stretched axis the size adds to the anchored span.
HE_API UIWidgetRect uiElementRect(const UIWidgetTree& tree, const UIElement& e,
                                  const UIWidgetCanvas* canvas = nullptr);

// The element's anchor rectangle itself, in canvas units — what the editor
// draws and what the insets below are measured against.
HE_API UIWidgetRect uiElementAnchorRect(const UIWidgetTree& tree, const UIElement& e,
                                        const UIWidgetCanvas* canvas = nullptr);

// ── Insets (how a stretched axis is authored) ───────────────────────────────
// On a stretched axis "position and size" is a clumsy way to say "how far in
// from each anchored edge", so the editor shows the two insets instead. The
// conversion is defined on any axis — on a point anchor the two insets simply
// describe the element's offset and size from that one point, which is not a
// useful way to author it, so only the editor's stretched fields use them.
HE_API void uiAnchorInsetsX(const UIElement& e, float& left, float& right);
HE_API void uiAnchorInsetsY(const UIElement& e, float& top, float& bottom);
HE_API void uiSetAnchorInsetsX(UIElement& e, float left, float right);
HE_API void uiSetAnchorInsetsY(UIElement& e, float top, float bottom);
// False when the element or any ancestor is invisible.
HE_API bool uiElementEffectiveVisible(const UIWidgetTree& tree, const UIElement& e);

// The rect this element is cut off at, in canvas units: the intersection of the
// rects of every ancestor whose clipChildren is on (an element's own flag clips
// its CHILDREN, never itself). False = no clipping ancestor, `out` untouched.
// An empty intersection comes back as a rect with w or h <= 0, which means the
// element is entirely hidden — the caller skips drawing and hit-testing it.
HE_API bool uiElementClipRect(const UIWidgetTree& tree, const UIElement& e,
                              UIWidgetRect& out, const UIWidgetCanvas* canvas = nullptr);
// Let every element that auto-sizes fit itself to its content. Call BEFORE
// uiElementRect so the rects already reflect the new sizes; cheap enough to run
// each frame (only text elements with AutoSize on do any work).
HE_API void uiApplyAutoSize(UIWidgetTree& tree, const UIWidgetCanvas* canvas = nullptr);

} // namespace HE
