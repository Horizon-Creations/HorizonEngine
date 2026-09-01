#pragma once
#include <UIWidget/UIElement.h>
#include <UIWidget/UITheme.h>
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

// ── A declared parameter ─────────────────────────────────────────────────────
// What a widget offers to whoever EMBEDS it. A component that cannot be told
// what to say is a picture: a form row with a baked-in label is one form row,
// not a component, and a library of them is a catalogue of screenshots.
//
// One parameter names one property of one element inside this tree and gives
// that pair a name of its own — "Label" rather than "element 7's Text". The
// indirection IS the feature: the host stores the parameter's name, so the
// component's author may rename the inner element, move the property onto a
// different one, or rebuild the row from scratch, and every page that uses it
// keeps working. Let the host address element+property directly and every
// internal detail becomes part of the contract.
//
// A parameter carries no default of its own: what the author left in the
// property IS the default, visible in the component's own editor, and there is
// no second place for it to disagree with.
struct UIWidgetParam
{
    std::string name;           // what the host sees, e.g. "Label"
    int         elementId = 0;  // which element inside THIS tree it writes
    std::string property;       // that element's property name (see UIPropSlot)
    std::string help;           // one sentence, shown where the host sets it
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
    // The knobs this widget offers its hosts. Empty for every widget authored
    // before components existed, and empty is what "this is a page, not a
    // component" means.
    std::vector<UIWidgetParam> params;

    UIWidgetTree() = default;
    UIWidgetTree(const UIWidgetTree& o) { *this = o; }
    UIWidgetTree& operator=(const UIWidgetTree& o)
    {
        if (this == &o) return *this;
        canvasWidth = o.canvasWidth; canvasHeight = o.canvasHeight;
        scaleMode = o.scaleMode; nextId = o.nextId;
        params = o.params;
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

// ── Which child is this, among its parent's? ─────────────────────────────────
// Tree order, counting VISIBLE and hidden children alike — a page you switched
// away from must not renumber the pages after it, or switching tabs would
// reshuffle them. -1 when it is not a child of that parent at all.
//
// Its own function because two containers ask it (a Tab Box for the active
// page, a Splitter for its two panes) and because `hidesChild` is called for
// every element on the visibility walk: one answer, one place to make it fast
// if it ever needs to be.
HE_API int uiChildIndexOf(const UIWidgetTree& tree, int parentId, int childId);
HE_API int uiChildCountOf(const UIWidgetTree& tree, int parentId);

// ── Setting a component's parameters ─────────────────────────────────────────
// Write a host's values into a freshly parsed copy of the component's OWN tree.
// Called before that tree is renumbered into its host, because the declarations
// address elements by the ids they carry in their own asset — afterwards those
// ids mean something else.
//
// A value whose name no longer matches a declaration is DROPPED, not guessed
// at: the component's author renamed or removed that knob, and quietly writing
// it into whatever now sits at that place would be worse than a label that
// stays as authored. A declaration nobody set keeps the authored value, which
// is precisely what a default is. Returns how many values were written, so a
// caller can notice that none of them were.
HE_API int uiApplyWidgetParams(
    UIWidgetTree& tree,
    const std::vector<std::pair<std::string, UIPropValue>>& values);

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
// The same rule without a tree: an authored canvas, a mode, and the pixel size
// it has to meet. Used for the tree above and for a WidgetRef's slot, which is
// the embedded widget's screen and follows exactly the same rule.
HE_API UIWidgetCanvas uiResolveCanvasFor(float authoredW, float authoredH,
                                         UICanvasScaleMode mode,
                                         float vpWidth, float vpHeight);

// How much an element's OWN numbers (position, size, padding, spacing) are
// scaled before they are used. 1 everywhere except inside an embedded widget:
// there they are in the units that widget was authored on, and the WidgetRef's
// rect is what they have to be fitted into. Only the nearest WidgetRef ancestor
// matters — its own rect already carries the scaling of everything above it.
HE_API void uiElementUnitScale(const UIWidgetTree& tree, const UIElement& e,
                               float& outScaleX, float& outScaleY,
                               const UIWidgetCanvas* canvas = nullptr);

// Element rect in CANVAS units, resolved through the parent chain (roots anchor
// to the canvas). The anchor rectangle is resolved inside the parent's rect;
// position is the offset from it, pivot shifts the rect so the pivot point
// lands there, and on a stretched axis the size adds to the anchored span.
// ── A Grid's resolved tracks (docs/he-apps-plan.md B3) ───────────────────────
// The column and row extents the layout REALLY used, in the same units a rect
// comes back in. Exported for one reason: the designer draws a grid's track
// lines, and drawing them from an even split would show an author a layout the
// runtime does not produce. One solver, two consumers.
class UIGrid;
HE_API void uiGridTracks(const UIWidgetTree& tree, const UIGrid& grid,
                         const UIWidgetCanvas* canvas,
                         std::vector<float>& outColumns,
                         std::vector<float>& outRows);

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

// The opacity this element is actually drawn with: its own times every
// ancestor's, which is what makes one value on a root panel fade a whole menu.
HE_API float uiElementEffectiveOpacity(const UIWidgetTree& tree, const UIElement& e);

// ── Rotation ─────────────────────────────────────────────────────────────────
// A chain of rotations about different points is again ONE rotation about a
// point, which is what makes this expressible in two numbers per quad instead
// of a matrix: a point p of the element ends up at
//     p' = R(degrees) * (p - src) + dst
// where `src` is the element's own pivot in the unrotated layout, and `dst` is
// where every ancestor's rotation carried that pivot to. Both in canvas units.
struct UIRotation
{
    float degrees = 0.0f;
    float srcX = 0.0f, srcY = 0.0f;
    float dstX = 0.0f, dstY = 0.0f;
};
// False when nothing in the chain rotates — the overwhelmingly common case, and
// the one every caller shortcuts on.
HE_API bool uiElementRotation(const UIWidgetTree& tree, const UIElement& e,
                              UIRotation& out, const UIWidgetCanvas* canvas = nullptr);
// Undo it: a point on screen back into the element's unrotated layout space, so
// a hit test can go on comparing against a plain rectangle.
HE_API void uiUnrotatePoint(const UIRotation& r, float x, float y,
                            float& outX, float& outY);

// False when the element or any ancestor is disabled — inert and drawn dimmed.
HE_API bool uiElementEffectiveEnabled(const UIWidgetTree& tree, const UIElement& e);

// How much a disabled element's colour is knocked back. One constant so the
// runtime and the designer preview grey things out identically.
inline constexpr float kUIDisabledDim = 0.55f;

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

// ── Resolve theme roles into ordinary colours ────────────────────────────────
// Every property an element bound to a role (see UIElement::themeRoles) is
// ASSIGNED the role's colour for `mode`. Called when a widget is created, when
// the theme changes and when light/dark flips — and nowhere per frame.
//
// Assignment rather than lookup-at-draw is the whole design: after this the
// runtime, the designer's preview, the asset thumbnails and the software
// rasterizer all see themed colours through the plain field reads they already
// do, and none of them needs to know a theme exists. Resolving inside the
// extractor instead would theme the runtime and leave the designer showing
// something else — the exact split that has bitten this panel twice.
//
// Returns how many properties were written, which is what a caller uses to
// decide whether anything needs redrawing.
HE_API int uiApplyTheme(UIWidgetTree& tree, const UITheme& theme, UIThemeMode mode);
HE_API int uiApplyTheme(UIElement& e, const UITheme& theme, UIThemeMode mode);

// Measure every scroll box's content and clamp its offset to what there is to
// scroll. Run once per frame after auto-size and before the rects are used —
// a box whose content shrank must not stay scrolled past its own end.
HE_API void uiUpdateScrollExtents(UIWidgetTree& tree);

// Scroll the box `id` by `delta` canvas units (positive = towards the end),
// clamped. False when it is not a scroll box or there is nothing to scroll,
// which is how a caller knows the wheel was NOT consumed.
HE_API bool uiScrollBy(UIWidgetTree& tree, int id, float delta);

} // namespace HE
