#pragma once
#include <UIWidget/UIElement.h>
#include <UIWidget/UITheme.h>
#include <UIWidget/UIWidgetAnim.h>   // UIAnimClip — a widget owns its animations
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
//
// ── The system scaling (HiDPI, docs/he-apps-plan.md F2) ─────────────────────
// Every mode above measures against the viewport in PIXELS, so a display that
// simply has more of them (a Retina panel, a 4K screen) is absorbed by the
// scale factor and nothing has to be told about it. ConstantPixel is the one
// exception, and it is the mode the system scaling actually reaches: "one unit
// is one pixel" makes a 200% display draw everything at half its intended
// size, which is exactly the "ignoring the content scale makes graphics appear
// tiny" case SDL's README-highdpi warns about. So the unit is a
// device-independent pixel, and `displayScale` below (SDL's
// `SDL_GetWindowDisplayScale`: pixel density × the display's content scale) is
// how many real pixels one of them is worth. 1.0 means an unscaled display,
// and every other mode ignores the value because the viewport already told it.
enum class UICanvasScaleMode : uint8_t
{
    Stretch = 0,     // per-axis fit, the canvas IS the reference (distorts)
    FitInside,       // uniform min(sx, sy): nothing is ever cut off
    FillOutside,     // uniform max(sx, sy): no empty space, edges may be cut off
    MatchWidth,      // uniform, the authored WIDTH is what fits
    MatchHeight,     // uniform, the authored HEIGHT is what fits
    ConstantPixel,   // uniform, one canvas unit is one DEVICE-INDEPENDENT pixel
};
HE_API const char* uiCanvasScaleModeName(UICanvasScaleMode m);

// The canvas a widget actually lays out in, for one particular viewport.
// `width`/`height` are canvas units (they are the authored size for Stretch,
// and viewport/scale otherwise); `scaleX`/`scaleY` convert a canvas unit to a
// pixel and are equal for every uniform mode.
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
    // One or two sentences about what this widget IS, shown wherever somebody
    // is about to reach for it — the palette, above all. A component whose name
    // is the only thing said about it is one you have to place to find out what
    // it does, and the twelve shipped ones are exactly the case where that is
    // the difference between a library and a list of words.
    //
    // On the TREE rather than beside it, because it belongs to the widget the
    // same way its canvas size does, and a second file to keep in step is a
    // second file that goes out of step.
    std::string description;

    // ── This widget's own theme, overriding the project's ────────────────────
    // A content-relative path to a Theme asset, or empty for "whatever the
    // application is using". The project names one theme and everything resolves
    // against it; a single widget may say otherwise — a launcher, an overlay, a
    // demo screen that has to look like somebody else's product.
    //
    // It covers everything inside the instance, embedded widgets included: one
    // widget, one theme, no per-element cascade of themes. That is the line
    // between "a widget can differ" and a hierarchy nobody can predict.
    std::string themeAsset;

    // ── Authored animations (docs/he-apps-plan.md B8) ────────────────────────
    // Named clips belonging to this widget, made in the Designer's timeline.
    // On the tree, like the parameters and the description, because they are
    // part of what the widget IS — a second file to keep in step is a second
    // file that goes out of step.
    std::vector<UIAnimClip> animations;

    UIWidgetTree() = default;
    UIWidgetTree(const UIWidgetTree& o) { *this = o; }
    UIWidgetTree& operator=(const UIWidgetTree& o)
    {
        if (this == &o) return *this;
        canvasWidth = o.canvasWidth; canvasHeight = o.canvasHeight;
        scaleMode = o.scaleMode; nextId = o.nextId;
        params = o.params;
        description = o.description; themeAsset = o.themeAsset;
        animations = o.animations;
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
// `displayScale` is the system scaling of the screen this viewport is on (see
// the enum above); only ConstantPixel reads it, and 1.0 is an unscaled display.
HE_API UIWidgetCanvas uiResolveCanvas(const UIWidgetTree& tree, float vpWidth, float vpHeight,
                                      float displayScale = 1.0f);
// The same rule without a tree: an authored canvas, a mode, and the pixel size
// it has to meet. Used for the tree above and for a WidgetRef's slot, which is
// the embedded widget's screen and follows exactly the same rule.
//
// A WidgetRef's slot deliberately passes NO display scale: its rect is in the
// HOST's canvas units, not in pixels, so a nested ConstantPixel widget means
// "unscaled relative to whoever embedded me" and inherits the host's factor —
// which already carries the system scaling. Handing it the scale a second time
// would apply it twice.
HE_API UIWidgetCanvas uiResolveCanvasFor(float authoredW, float authoredH,
                                         UICanvasScaleMode mode,
                                         float vpWidth, float vpHeight,
                                         float displayScale = 1.0f);

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
// The same rectangle before the element's Min/Max bounds are applied to it: the
// anchor solve, or the slot its layout container hands it. Exported so that a
// caller who needs to see what the layout WOULD have produced — how far a bound
// is biting, and on which axis — can ask instead of recomputing the walk.
HE_API UIWidgetRect uiElementRectUnbounded(const UIWidgetTree& tree, const UIElement& e,
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

// …and how much of an element is left where it was while it is being CARRIED.
// A drag whose source stays solid in its old place reads as a copy, and the
// gesture is about to move something. Alpha rather than a dim: it has to look
// like it has left, not like it has been switched off.
inline constexpr float kUIDraggedAlpha = 0.45f;

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

// Every style of `theme` this element follows, LEAST specific first — CSS's
// specificity, in CSS's order:
//
//   "Button"          its type                       (0,0,1)
//   "Card"            the style it points at by name (0,1,0)
//   "Button.success"  its type and its tag           (0,1,1)
//
// They layer property by property, later wins — CSS's cascade, with a
// vocabulary small enough to predict in your head. A variant that names one
// colour keeps everything else its base decided, which is the entire reason to
// write a variant instead of a second whole style.
HE_API std::vector<const UIThemeStyle*> uiThemeStylesFor(const UIElement& e,
                                                         const UITheme& theme);
// The names of every property those styles decide between them, each once. What
// the editor asks to say "this value comes from the theme" without having to
// know how many styles answered.
HE_API std::vector<std::string> uiThemeDecidedProps(const UIElement& e, const UITheme& theme);

// What the theme says `prop` should be on this element, or false when it says
// nothing. THE one place that question is answered: uiApplyTheme writes what
// this returns, and the designer — which creates nothing and so is never
// assigned to — asks it while drawing. Two implementations of this rule is
// exactly how the designer came to show light while the preview showed dark.
//
// Highest wins:
//   1. a per-property role binding — a deliberate exception to the style
//   2. kUIThemeLiteral — this value was decided elsewhere, hands off
//   3. the element's style
HE_API bool uiThemeValueFor(const UIElement& e, const UITheme& theme, UIThemeMode mode,
                            const std::string& prop, UIPropValue& out);

// Measure every scroll box's content and clamp its offset to what there is to
// scroll. Run once per frame after auto-size and before the rects are used —
// a box whose content shrank must not stay scrolled past its own end.
HE_API void uiUpdateScrollExtents(UIWidgetTree& tree);

// Scroll the box `id` by `delta` canvas units (positive = towards the end),
// clamped. False when it is not a scroll box or there is nothing to scroll,
// which is how a caller knows the wheel was NOT consumed.
HE_API bool uiScrollBy(UIWidgetTree& tree, int id, float delta);

// ── The text catalog ─────────────────────────────────────────────────────────
// Every translatable string of an application, by key, in every language it has.
// Deliberately the same shape as a theme, and resolved the same way: an element
// binds a PROPERTY to a key (UIElement::textKeys) and uiApplyTextCatalog writes
// the translation into that property, so switching language is one pass over
// the tree and nothing downstream needs to know a catalog exists.
//
// Ordered vectors and not maps, for the reason UIThemeStyle gives: the order is
// the file's, and a map would sort the keys for a reason no author can see.
struct HE_API UITextCatalog
{
    // What a key falls back to when the language being asked for does not have
    // it. A half-translated application shows English where it has nothing, not
    // a blank or a key — and the fallback is the catalog's decision, not the
    // caller's, because the caller is a language switch and knows nothing.
    std::string fallback = "en";
    // language → (key → text).
    std::vector<std::pair<std::string,
                std::vector<std::pair<std::string, std::string>>>> languages;

    // The text for `key` in `lang`, else in the fallback language, else null.
    // Null and an empty string are different answers: an empty translation is a
    // deliberate "say nothing here".
    const std::string* find(const std::string& lang, const std::string& key) const;
    // Set or replace, creating the language if it is new.
    void set(const std::string& lang, const std::string& key, const std::string& text);
    std::vector<std::string> languageNames() const;
};

HE_API std::string uiTextCatalogToJson(const UITextCatalog& c);
HE_API bool        uiTextCatalogFromJson(const std::string& json, UITextCatalog& out);

// Write the translations into the properties that are bound to keys. Returns how
// many were written, which is what a caller uses to decide whether anything has
// to be redrawn — the same contract uiApplyTheme has, and for the same reason.
//
// A key the catalog does not know at all leaves the property ALONE. That is the
// difference between "not translated yet" and "translated to nothing", and
// blanking the authored text would make an incomplete catalog worse than none.
HE_API int uiApplyTextCatalog(UIWidgetTree& tree, const UITextCatalog& c,
                              const std::string& lang);
HE_API int uiApplyTextCatalog(UIElement& e, const UITextCatalog& c,
                              const std::string& lang);

// ── Contrast (WCAG 2.1) ──────────────────────────────────────────────────────
// Whether the text in this widget can actually be read on what is behind it.
// The numbers are the accessibility standard's, not a taste: relative luminance
// of an sRGB colour, and the ratio (L_lighter + 0.05) / (L_darker + 0.05), which
// runs from 1 (the same colour twice, invisible) to 21 (black on white).
//
// The colours in this tree ARE sRGB — the software rasterizer writes them out
// as `c * 255` with no transfer function anywhere — so the standard's
// linearisation applies to them directly and nothing has to be undone first.
HE_API float uiRelativeLuminance(const glm::vec4& srgb);
HE_API float uiContrastRatio(const glm::vec4& a, const glm::vec4& b);

// One pair the check could not read.
struct UIContrastFinding
{
    int         elementId = 0;      // whose text it is
    std::string textProp;           // the property carrying that text's colour
    int         againstId = 0;      // what it is standing on (0 = the backdrop)
    std::string againstProp;        // …and which property of it
    glm::vec4   textColor{ 1.0f };  // as composited: an alpha below 1 is mixed
    glm::vec4   backColor{ 1.0f };  // into the background before the comparison
    float       ratio = 0.0f;
    float       required = 4.5f;    // 3.0 for large text, which is WCAG's rule
    float       fontSize = 0.0f;    // why `required` is what it is
};

// Every text in `tree` that does not reach the standard, in element order.
//
// `backdrop` is the page BEHIND the widget — pass the theme's Background role.
// It is what a text lands on when nothing between it and the root has a surface
// of its own, which is the common case: a label in a plain vertical box.
//
// An element's text is measured against the nearest surface AT OR ABOVE it — a
// field draws its own background, a label does not, and a button's caption is a
// Text child sitting on the button. Transparency is composited rather than
// ignored, because a muted label is usually muted by its alpha and the whole
// question is what the eye ends up seeing.
//
// `minRatio` is the bar for ordinary text; large text (24 px and up, WCAG's
// threshold) is held to 3.0 instead, scaled down in step if the bar is lowered.
HE_API std::vector<UIContrastFinding> uiCheckContrast(const UIWidgetTree& tree,
                                                      const glm::vec4& backdrop,
                                                      float minRatio = 4.5f);

} // namespace HE
