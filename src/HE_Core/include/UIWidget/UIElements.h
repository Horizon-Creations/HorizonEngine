#pragma once
#include <UIWidget/UIElement.h>
// For UICanvasScaleMode: a WidgetRef's rect is the embedded widget's screen, so
// it carries that widget's scale mode. (No cycle: UIWidgetTree.h includes
// UIElement.h only.)
#include <UIWidget/UIWidgetTree.h>

// Concrete widget element types. Each declares its own data + properties +
// events; the property tables (propTable), render() and JSON live in
// UIElement.cpp. Field defaults are the "sensible defaults" a freshly-added
// element starts with.

namespace HE {

// ── Panel ─────────────────────────────────────────────────────────────────────
class HE_API UIPanel final : public UIElement
{
public:
    glm::vec4 color{ 0.12f, 0.12f, 0.12f, 0.85f };

    UIWidgetType type() const override { return UIWidgetType::Panel; }
    const char*  typeName() const override { return "Panel"; }
    bool hasMaterialSlot() const override { return true; }
    bool hasTextureSlot()  const override { return true; }
    bool hasSurfaceStyle() const override { return true; }
    bool acceptsChildren() const override { return true; }
    std::unique_ptr<UIElement> clone() const override
    { auto p = std::make_unique<UIPanel>(*this); return p; }

    const UIPropTable& propTable() const override;
    std::vector<UIEventDesc> events() const override
    { return { { "OnMouseEnter" }, { "OnMouseLeave" }, { "OnClicked" } }; }

    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

// ── Image ─────────────────────────────────────────────────────────────────────
class HE_API UIImage final : public UIElement
{
public:
    glm::vec4 tint{ 1.0f, 1.0f, 1.0f, 1.0f };
    // ── 9-slice ──────────────────────────────────────────────────────────────
    // Margins in SOURCE pixels. All zero (the default) draws the texture as one
    // stretched quad. Non-zero cuts it into nine: the four corners keep their
    // pixel size, the edges stretch along one axis and the middle along both.
    // That is what makes one 64x64 texture a frame, a button and a panel at any
    // size, instead of a picture that smears when the element grows.
    float sliceLeft = 0.0f, sliceTop = 0.0f, sliceRight = 0.0f, sliceBottom = 0.0f;
    bool  sliceFillCentre = true;   // off leaves the middle transparent (a frame)

    UIImage() { sizeX = 128.0f; sizeY = 128.0f; }
    UIWidgetType type() const override { return UIWidgetType::Image; }
    const char*  typeName() const override { return "Image"; }
    bool hasMaterialSlot() const override { return true; }
    bool hasTextureSlot()  const override { return true; }
    bool hasSurfaceStyle() const override { return true; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UIImage>(*this); }

    const UIPropTable& propTable() const override;
    std::vector<UIEventDesc> events() const override
    { return { { "OnMouseEnter" }, { "OnMouseLeave" }, { "OnClicked" } }; }

    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

// ── Text ──────────────────────────────────────────────────────────────────────
class HE_API UIText final : public UIElement
{
public:
    std::string text = "Text";
    float       fontSize = 22.0f;
    glm::vec4   color{ 1.0f, 1.0f, 1.0f, 1.0f };
    // Multi-line: '\n' in Text always breaks a line. WordWrap additionally wraps
    // at the element's width — off keeps long lines running past the box, which
    // is what you want for a single-line label.
    bool        wordWrap = false;
    // Grow the element to fit its own text. Height always follows the line count
    // and font size; width follows the widest line UNLESS WordWrap is on (then
    // the authored width is the wrap column and only the height adapts). This is
    // why bumping FontSize used to clip the text — the box stayed 200×30.
    bool        autoSize = true;
    // ── Where the text sits inside its own rect ──────────────────────────────
    // 0/1/2 = left/centre/right and top/middle/bottom, the two together being
    // the nine positions of a 3×3 grid — the editor offers it as exactly that,
    // next to the anchor grid it looks like.
    //
    // This is NOT the anchor: the anchor says where the element hangs in its
    // PARENT, this says where the glyphs sit in the element. A label stretched
    // across a button needs the second one, which is why a caption used to be
    // stuck wherever the text happened to start.
    //
    // alignV starts at MIDDLE because text was always centred vertically before
    // there was a choice; Top would move every existing label.
    int         alignH = 0;
    int         alignV = 1;

    UIText() { sizeX = 200.0f; sizeY = 30.0f; }
    UIWidgetType type() const override { return UIWidgetType::Text; }
    const char*  typeName() const override { return "Text"; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UIText>(*this); }

    const UIPropTable& propTable() const override;

    // Element size implied by the current text/font (see autoSize). Callers apply
    // it before layout so the rect the glyphs lay out in already fits them.
    void applyAutoSize(float resolvedWidth) override;

    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

// ── Button ────────────────────────────────────────────────────────────────────
class HE_API UIButton final : public UIElement
{
public:
    // ── A button is a SURFACE, not a label with a box around it ──────────────
    // It draws its three states and nothing else. What is on it — a caption, an
    // icon, both side by side — is made of CHILD elements, anchored inside the
    // button's rect like children of any other parent. That is what makes an
    // icon button, or a button with the label pushed to the left, possible at
    // all; a built-in centred string could only ever be one layout.
    //
    // A Button loaded from a widget authored before this carries its old caption
    // into a Text child (see uiWidgetTreeFromJson) — nobody's label disappears.
    glm::vec4   color{ 0.20f, 0.20f, 0.20f, 1.0f };   // normal
    glm::vec4   hoveredColor{ 0.30f, 0.30f, 0.30f, 1.0f };
    glm::vec4   pressedColor{ 0.15f, 0.15f, 0.15f, 1.0f };

    // 6 px was hard-coded in render() until the radius became an authored
    // property; kept as the default so existing buttons look unchanged.
    UIButton() { sizeX = 180.0f; sizeY = 48.0f; cornerRadius = glm::vec4(6.0f); }
    UIWidgetType type() const override { return UIWidgetType::Button; }
    const char*  typeName() const override { return "Button"; }
    bool hasMaterialSlot() const override { return true; }
    bool hasTextureSlot()  const override { return true; }
    bool hasSurfaceStyle() const override { return true; }
    bool interactive() const override { return true; }
    // The caption, an icon, a badge: everything on a button is a child of it.
    bool acceptsChildren() const override { return true; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UIButton>(*this); }

    const UIPropTable& propTable() const override;
    std::vector<UIEventDesc> events() const override
    { return { { "OnClicked" }, { "OnPressed" }, { "OnReleased" },
               { "OnHovered" }, { "OnUnhovered" } }; }

    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

// ── CheckBox ──────────────────────────────────────────────────────────────────
class HE_API UICheckBox final : public UIElement
{
public:
    bool        checked = false;
    std::string label = "Checkbox";
    float       fontSize = 18.0f;
    glm::vec4   boxColor{ 0.20f, 0.20f, 0.20f, 1.0f };
    glm::vec4   checkColor{ 0.30f, 0.80f, 0.40f, 1.0f };
    glm::vec4   textColor{ 1.0f, 1.0f, 1.0f, 1.0f };

    UICheckBox() { sizeX = 200.0f; sizeY = 28.0f; }
    UIWidgetType type() const override { return UIWidgetType::CheckBox; }
    const char*  typeName() const override { return "CheckBox"; }
    bool interactive() const override { return true; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UICheckBox>(*this); }

    const UIPropTable& propTable() const override;
    std::vector<UIEventDesc> events() const override
    { return { { "OnCheckChanged", UIPropType::Bool, true },
               { "OnHovered" }, { "OnUnhovered" } }; }

    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

// ── Slider ────────────────────────────────────────────────────────────────────
class HE_API UISlider final : public UIElement
{
public:
    float     value = 0.5f, minValue = 0.0f, maxValue = 1.0f;
    glm::vec4 trackColor{ 0.20f, 0.20f, 0.20f, 1.0f };
    glm::vec4 fillColor{ 0.30f, 0.60f, 0.90f, 1.0f };
    glm::vec4 handleColor{ 0.90f, 0.90f, 0.90f, 1.0f };

    UISlider() { sizeX = 220.0f; sizeY = 24.0f; }
    UIWidgetType type() const override { return UIWidgetType::Slider; }
    const char*  typeName() const override { return "Slider"; }
    bool interactive() const override { return true; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UISlider>(*this); }

    const UIPropTable& propTable() const override;
    std::vector<UIEventDesc> events() const override
    { return { { "OnValueChanged", UIPropType::Float, true } }; }

    // Normalized fill 0..1 for rendering (guards min==max).
    float normalized() const
    {
        const float span = maxValue - minValue;
        if (span <= 0.0f) return 0.0f;
        const float t = (value - minValue) / span;
        return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    }

    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

// ── ProgressBar ───────────────────────────────────────────────────────────────
class HE_API UIProgressBar final : public UIElement
{
public:
    float     value = 0.5f; // 0..1
    glm::vec4 backColor{ 0.15f, 0.15f, 0.15f, 1.0f };
    glm::vec4 fillColor{ 0.30f, 0.70f, 0.40f, 1.0f };

    UIProgressBar() { sizeX = 240.0f; sizeY = 20.0f; cornerRadius = glm::vec4(4.0f); }
    // The TRACK is the surface; the fill drawn on top of it keeps its own colour.
    bool hasSurfaceStyle() const override { return true; }
    UIWidgetType type() const override { return UIWidgetType::ProgressBar; }
    const char*  typeName() const override { return "ProgressBar"; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UIProgressBar>(*this); }

    const UIPropTable& propTable() const override;

    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

// ── TextInput ─────────────────────────────────────────────────────────────────
class HE_API UITextInput final : public UIElement
{
public:
    std::string text;
    std::string placeholder = "Enter text...";
    float       fontSize = 18.0f;
    glm::vec4   backColor{ 0.10f, 0.10f, 0.10f, 1.0f };
    glm::vec4   textColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    glm::vec4   selectionColor{ 0.25f, 0.45f, 0.80f, 0.75f };
    // 0 = no limit. Counted in CHARACTERS, not bytes — a limit of 8 that lets
    // through two accented letters fewer is a limit nobody can explain.
    int         maxLength = 0;
    // Draw every character as a dot (a password field). The text itself is
    // stored as typed; only the drawing changes.
    bool        password = false;
    // Off = the field shows its text but takes no input: typing, deleting and
    // pasting do nothing, while focusing, selecting and copying still work.
    // That is a read-only field, not a disabled one — a disabled element is
    // greyed out and inert altogether (see UIElement::enabled).
    bool        editable = true;
    // Off = no selection can be made at all. The caret still moves, and
    // Select All and shift-arrows simply do nothing.
    bool        selectable = true;

    // ── What may be typed into it ────────────────────────────────────────────
    // A field that asks for a number should not accept letters, and finding that
    // out when the value is parsed is one round trip too late. Filtering happens
    // where text ENTERS the field, so it covers a paste as well as a keystroke —
    // and a pasted "12ab" arrives as "12" rather than being refused whole,
    // because dropping everything over one bad character is the more annoying of
    // the two behaviours.
    //
    // A plain int rather than an enum type: the property system carries Float,
    // Int, Bool, String, Color, Vec2 and StringList, and an Int the designer
    // shows as a number is the honest fit until it grows an enum kind.
    enum Filter : int
    {
        FilterAny     = 0,   // anything — the default, and what every field did before
        FilterInteger = 1,   // digits, and a '-' only in front
        FilterDecimal = 2,   // digits, one '.', and a '-' only in front
        FilterCustom  = 3,   // exactly the characters listed in allowedChars
    };
    int         inputFilter = FilterAny;
    // Only for FilterCustom: the characters that may be typed, listed plainly
    // ("0123456789ABCDEF"). Empty while FilterCustom is set would reject
    // everything — a read-only field said in a confusing way — so it is treated
    // as Any instead.
    std::string allowedChars;

    // Would this one UTF-8 character be accepted, given what the field already
    // holds and where it would land? Lives on the element because the runtime and
    // its tests need the same answer, and because "-" and "." are only valid
    // relative to the rest of the text.
    bool acceptsCharacter(const std::string& ch, size_t atByte) const;

    // ── Editing state (runtime, never serialized) ────────────────────────────
    // Byte offsets into `text`, always on character boundaries. caret is where
    // typing lands; selAnchor is the other end of the selection, equal to caret
    // when nothing is selected. Both live here rather than in the manager
    // because they belong to the field, and a widget holds its own live copy.
    size_t      caret = 0;
    size_t      selAnchor = 0;
    // ── IME composition (runtime, never serialized) ──────────────────────────
    // What the input method is currently building but has not committed yet:
    // typing "nihao" on a Chinese IME shows that as preedit text until a
    // candidate is chosen, at which point the OS sends the finished 你好 as
    // ordinary text input. It is NOT part of `text` — committing it is the input
    // method's decision, not ours — so it is held here and drawn at the caret.
    //
    // `compositionCursor` is a byte offset into `composition` (where the IME puts
    // its own caret); -1 = unspecified, draw it at the end.
    std::string composition;
    int         compositionCursor = -1;

    // How far the text is scrolled sideways under the field, in the same pixels
    // the glyphs are measured in. Kept so a caret past the right edge stays
    // visible instead of typing itself out of the box. Mutable because render()
    // is the only place that can work it out — it is the one that knows the
    // field's pixel width — and render() is const for every other element.
    mutable float scrollPx = 0.0f;

    bool   hasSelection() const { return caret != selAnchor; }
    size_t selMin() const { return caret < selAnchor ? caret : selAnchor; }
    size_t selMax() const { return caret < selAnchor ? selAnchor : caret; }
    std::string selectedText() const
    { return hasSelection() ? text.substr(selMin(), selMax() - selMin()) : std::string(); }
    // Drop the selected run and put the caret where it was. False when there
    // was nothing selected.
    bool deleteSelection()
    {
        if (!hasSelection()) return false;
        const size_t a = selMin(), b = selMax();
        text.erase(a, b - a);
        caret = selAnchor = a;
        return true;
    }
    // Keep the two offsets inside the text and on character boundaries —
    // anything that edits `text` from the outside (a script, a Set Property)
    // can leave them pointing anywhere.
    void clampCaret()
    {
        caret     = uiUtf8Clamp(text, caret);
        selAnchor = uiUtf8Clamp(text, selAnchor);
    }
    // Where a click that landed `localX` pixels into the text area puts the
    // caret. Needs the same pixel scale the drawing uses.
    size_t caretAtX(float localX, float pxScaleY) const;
    // Characters (not bytes) currently in the field — what maxLength counts.
    int    charCount() const
    {
        int n = 0;
        for (size_t i = 0; i < text.size(); i = uiUtf8Next(text, i)) ++n;
        return n;
    }

    UITextInput() { sizeX = 240.0f; sizeY = 32.0f; cornerRadius = glm::vec4(4.0f); }
    // A bordered text field is the standard look for one, so this is arguably
    // the type that needed it most.
    bool hasSurfaceStyle() const override { return true; }
    UIWidgetType type() const override { return UIWidgetType::TextInput; }
    const char*  typeName() const override { return "TextInput"; }
    bool interactive() const override { return true; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UITextInput>(*this); }

    const UIPropTable& propTable() const override;
    std::vector<UIEventDesc> events() const override
    { return { { "OnTextChanged", UIPropType::String, true },
               { "OnTextCommitted", UIPropType::String, true },
               { "OnFocused" }, { "OnUnfocused" } }; }

    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

// ── ComboBox ──────────────────────────────────────────────────────────────────
class HE_API UIComboBox final : public UIElement
{
public:
    std::vector<std::string> options{ "Option A", "Option B", "Option C" };
    int         selectedIndex = 0;
    float       fontSize = 18.0f;
    glm::vec4   backColor{ 0.15f, 0.15f, 0.15f, 1.0f };
    glm::vec4   textColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    glm::vec4   highlightColor{ 0.25f, 0.35f, 0.50f, 1.0f };

    // ── The open list (docs/he-apps-plan.md B4) ──────────────────────────────
    // Runtime state, like a slider's value while it is being dragged: which of
    // the options the pointer is over, and whether the list is down at all. The
    // list itself is drawn by the WIDGET MANAGER rather than by render(), for
    // one reason that decides everything: it reaches OUTSIDE this element's
    // rect, and every rect in this system — the hit test, the clip, the draw
    // order — is the element's own. A thing that hangs out of its own frame has
    // to be owned by the layer that owns the screen.
    bool open = false;
    int  hoverIndex = -1;

    // How tall one row of the open list is, in canvas units. Tied to the closed
    // box so a taller combo gets taller rows instead of a cramped list.
    float optionHeight() const { return sizeY > 0.0f ? sizeY : 24.0f; }

    // ── The little triangle on the right ─────────────────────────────────────
    // Its geometry, in whatever units the rect handed in is in. It lives here
    // and not in either drawer because BOTH draw it — the engine out of quads,
    // the designer out of an ImGui triangle — and the two have disagreed about
    // a position before. One set of numbers is the only way that stays true.
    // ── How far in from the edge content has to start ────────────────────────
    // A rounded rectangle takes its corner back by the radius, so text sitting
    // at a fixed inset hangs out over the curve as soon as the rounding grows —
    // which is exactly what a pill-shaped combo looked like: the option's first
    // letters outside the shape, on both the closed box and the open list.
    //
    // 0.8 of the radius rather than all of it: at the vertical middle of a row,
    // where text actually sits, the curve has already come most of the way back,
    // so the full radius would push every label needlessly far in.
    static float contentInset(float radiusPx)
    { return radiusPx * 0.8f > 6.0f ? radiusPx * 0.8f : 6.0f; }

    // ── How round the OPEN LIST may be ───────────────────────────────────────
    // Not "as round as the box". A corner deeper than half a row is a corner
    // eating a row: the first and last entries lose their outer half to the
    // curve, and no amount of padding puts them back. Clamped against the
    // list's own height too, so a one-entry list cannot round itself away.
    static float listRadius(float boxRadiusPx, float rowHeightPx, float listHeightPx)
    {
        float r = boxRadiusPx;
        if (r > rowHeightPx * 0.5f)  r = rowHeightPx * 0.5f;
        if (r > listHeightPx * 0.5f) r = listHeightPx * 0.5f;
        return r > 0.0f ? r : 0.0f;
    }

    struct Arrow { float cx = 0.0f, cy = 0.0f, halfW = 0.0f, height = 0.0f; };
    static Arrow arrowIn(const UIWidgetRect& r)
    {
        Arrow a;
        // Centred in a square box at the right end, so it sits where the eye
        // looks for it whatever the combo's width.
        a.cx     = r.x + r.w - r.h * 0.5f;
        a.cy     = r.y + r.h * 0.5f;
        a.halfW  = std::max(2.0f, r.h * 0.17f);
        a.height = a.halfW * 1.15f;      // a wide, shallow triangle, not a spike
        return a;
    }

    UIComboBox() { sizeX = 220.0f; sizeY = 32.0f; cornerRadius = glm::vec4(4.0f); }
    // The closed box is the surface; the open list draws over it.
    bool hasSurfaceStyle() const override { return true; }
    UIWidgetType type() const override { return UIWidgetType::ComboBox; }
    const char*  typeName() const override { return "ComboBox"; }
    bool interactive() const override { return true; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UIComboBox>(*this); }

    const UIPropTable& propTable() const override;
    std::vector<UIEventDesc> events() const override
    { return { { "OnSelectionChanged", UIPropType::Int, true } }; }

    const std::string& currentText() const
    {
        static const std::string empty;
        if (selectedIndex < 0 || selectedIndex >= (int)options.size()) return empty;
        return options[selectedIndex];
    }

    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

// ── WidgetRef ─────────────────────────────────────────────────────────────────
// A whole other UI Widget asset, used as one element. It draws nothing itself:
// at creation the runtime copies that asset's tree in under this element (with
// its ids renumbered so two copies of the same widget cannot collide) and gives
// its logic graph a script instance of its own. Its root elements anchor inside
// this element's rect, so where this one sits is where the embedded widget is.
//
// The path is authoring data; `embedded` says whether the runtime managed to
// graft it (false in the designer, which shows a placeholder instead).
class HE_API UIWidgetRef final : public UIElement
{
public:
    std::string widgetPath;        // content-relative path of the widget asset
    bool        embedded = false;  // transient: the runtime grafted it in
    // The canvas the embedded asset was AUTHORED on and the scale mode it was
    // authored with, both filled in when it is grafted. This element's rect is
    // that widget's SCREEN: the same rule that maps a canvas onto a viewport
    // maps it in here, so its own scale mode decides what happens when the slot
    // is a different size — Stretch scales it into the slot, ConstantPixel
    // leaves its units alone and lets its anchors do the placing. 0 = nothing
    // embedded → no scaling at all.
    float             contentW = 0.0f, contentH = 0.0f;
    UICanvasScaleMode contentMode = UICanvasScaleMode::Stretch;

    // ── When this ref is a ROW of a ListView ─────────────────────────────────
    // Which item the row SHOWS (-1 = it is not a list row), and which item it
    // was last told about. Two numbers rather than one because they answer
    // different questions: the first places the row, the second decides whether
    // it still needs to be filled in. Scrolling moves one row's index and
    // leaves the other's alone — that is precisely how a pool of ten rows shows
    // ten thousand items without ever being rebuilt.
    //
    // Both transient: a list is realized from its item count every frame and is
    // never loaded from the asset.
    int rowIndex = -1;
    int rowBound = -1;

    UIWidgetRef() { sizeX = 300.0f; sizeY = 200.0f; hitTestable = false; }
    UIWidgetType type() const override { return UIWidgetType::WidgetRef; }
    const char*  typeName() const override { return "WidgetRef"; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UIWidgetRef>(*this); }

    const UIPropTable& propTable() const override;
    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override {}
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

// ── Layout boxes ──────────────────────────────────────────────────────────────
// A box does not draw anything: it hands each of its direct children a slot,
// one after the other along its axis. A child keeps its own size on that axis
// unless its slotFill is > 0, in which case the filling children share whatever
// space is left; across the axis every child gets the box's full inner extent.
// Invisible children take no space at all, so hiding one closes the gap.
class HE_API UIBoxBase : public UIElement
{
public:
    float padding = 0.0f;  // inset on all four sides, canvas units
    float spacing = 4.0f;  // gap between two children

    // ── Size to content ──────────────────────────────────────────────────────
    // While this is on, the box's size is not authored: it is measured from
    // what is inside it (children plus spacing plus padding) every frame, and
    // the two Min values are the floor it cannot shrink below. That makes a
    // menu grow and shrink with the number of entries in it instead of being a
    // fixed rectangle that a sixth entry falls out of.
    //
    // A FILLING child cannot be measured (its size is a share of what is left
    // over, which is what this is computing), so it counts as nothing.
    bool  sizeToContent = false;
    float minSizeX = 0.0f, minSizeY = 0.0f;

    bool laysOutChildren() const override { return true; }
    bool acceptsChildren() const override { return true; }
    const UIPropTable& propTable() const override;
    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override {}
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

class HE_API UIVerticalBox final : public UIBoxBase
{
public:
    UIVerticalBox() { sizeX = 300.0f; sizeY = 400.0f; hitTestable = false; }
    UIWidgetType type() const override { return UIWidgetType::VerticalBox; }
    const char*  typeName() const override { return "VerticalBox"; }
    bool stacksVertically() const override { return true; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UIVerticalBox>(*this); }
};

class HE_API UIHorizontalBox final : public UIBoxBase
{
public:
    UIHorizontalBox() { sizeX = 400.0f; sizeY = 60.0f; hitTestable = false; }
    UIWidgetType type() const override { return UIWidgetType::HorizontalBox; }
    const char*  typeName() const override { return "HorizontalBox"; }
    bool stacksVertically() const override { return false; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UIHorizontalBox>(*this); }
};

// ── WrapBox (docs/he-apps-plan.md B3) ─────────────────────────────────────────
// A horizontal box that runs out of room and starts a new line. Tags, chips, a
// row of buttons that has to survive a narrow window — everything that is a row
// until it cannot be one.
//
// Two things it deliberately does NOT do, both because wrapping contradicts
// them:
//   • Slot Fill is ignored. A child that eats the leftover space would take the
//     whole first line and there would never be a second one.
//   • Size To Content sizes the HEIGHT only. The width is what the children are
//     wrapped against, so measuring it from them is the question answering
//     itself; the height, the number of lines they needed, is the useful half.
class HE_API UIWrapBox final : public UIBoxBase
{
public:
    // The gap BETWEEN lines. `spacing` is the gap between two items on a line —
    // two different numbers because a row of chips usually wants tighter
    // horizontal spacing than vertical.
    float lineSpacing = 4.0f;

    UIWrapBox() { sizeX = 400.0f; sizeY = 200.0f; hitTestable = false; }
    UIWidgetType type() const override { return UIWidgetType::WrapBox; }
    const char*  typeName() const override { return "WrapBox"; }
    // It runs along X and breaks along Y. The flag exists for the boxes' single
    // axis and is answered honestly here: a wrap box's MAIN axis is horizontal.
    bool stacksVertically() const override { return false; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UIWrapBox>(*this); }

    const UIPropTable& propTable() const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

// ── Grid (docs/he-apps-plan.md B3) ────────────────────────────────────────────
// Rows and columns, with spans — the container a FORM is made of. Two stacked
// boxes can fake a form only by hand-matching every label's width, and they come
// apart the moment one label grows.
//
// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  THE TRACK TOKENS ARE AN ON-DISK FORMAT.                                 ║
// ║                                                                          ║
// ║  Column Sizes and Row Sizes are lists of them, and a saved widget stores ║
// ║  the strings verbatim. test_ui_widgets.cpp pins the grammar.             ║
// ╚══════════════════════════════════════════════════════════════════════════╝
//   "120"   a fixed size in canvas units
//   "*"     one share of whatever is left over
//   "2*"    two shares
//   "auto"  as wide (tall) as the widest (tallest) thing in it
// Anything else is read as "*", deliberately: a track nobody can read is still a
// track you can see and fix, whereas collapsing it to nothing hides the typo in
// a layout that merely looks wrong. Same rule as a theme role that no longer
// resolves.
struct UIGridTrack
{
    enum class Kind : uint8_t { Fixed, Weight, Auto };
    Kind  kind  = Kind::Weight;
    float value = 1.0f;   // units for Fixed, shares for Weight, unused for Auto
};
HE_API UIGridTrack uiParseGridTrack(const std::string& token);

class HE_API UIGrid final : public UIBoxBase
{
public:
    // Authored as strings, kept parsed alongside: the parse happens when they
    // are SET, not when a rect is asked for — a hit test walks every element and
    // would otherwise re-parse the same words hundreds of times a frame.
    std::vector<std::string> columns{ "auto", "*" };
    std::vector<std::string> rows{ "auto" };
    // The gap between two ROWS. `spacing` is the gap between two columns; two
    // numbers for the same reason a wrap box has two.
    float rowSpacing = 4.0f;

    // Transient, rebuilt from the two lists above. Never serialized.
    std::vector<UIGridTrack> colTracks, rowTracks;

    UIGrid()
    {
        sizeX = 400.0f; sizeY = 300.0f; hitTestable = false;
        reparse();
    }
    void reparse()
    {
        colTracks.clear(); rowTracks.clear();
        for (const std::string& t : columns) colTracks.push_back(uiParseGridTrack(t));
        for (const std::string& t : rows)    rowTracks.push_back(uiParseGridTrack(t));
        if (colTracks.empty()) colTracks.push_back({});
        if (rowTracks.empty()) rowTracks.push_back({});
    }

    UIWidgetType type() const override { return UIWidgetType::Grid; }
    const char*  typeName() const override { return "Grid"; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UIGrid>(*this); }

    const UIPropTable& propTable() const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

// ── ScrollBox ─────────────────────────────────────────────────────────────────
// A vertical box whose content is allowed to be taller than the box: it clips
// (that is what clipChildren is for) and shifts its children up by the current
// offset. The offset is RUNTIME state, like a slider's value while it is being
// dragged: it lives on the live copy the widget manager holds and is not part
// of what the asset saves, so reopening a menu starts it at the top.
class HE_API UIScrollBox final : public UIBoxBase
{
public:
    // How far the content is scrolled, in canvas units, 0 = top. Clamped
    // against contentExtent every frame (uiUpdateScrollExtents).
    float scrollOffset = 0.0f;
    // Total extent of the stacked children, filled in by the layout pass. Not
    // serialized: it is a measurement of the content, not a setting.
    float contentExtent = 0.0f;
    // Width of the scrollbar drawn on the right edge; 0 hides it.
    float barWidth = 6.0f;
    glm::vec4 barColor{ 0.75f, 0.75f, 0.80f, 0.65f };

    UIScrollBox() { sizeX = 300.0f; sizeY = 400.0f; clipChildren = true; hitTestable = false; }
    UIWidgetType type() const override { return UIWidgetType::ScrollBox; }
    const char*  typeName() const override { return "ScrollBox"; }
    bool stacksVertically() const override { return true; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UIScrollBox>(*this); }

    const UIPropTable& propTable() const override;
    // The furthest it may be scrolled — 0 when everything already fits.
    float maxScroll() const
    {
        const float inner = sizeY - 2.0f * padding;
        const float over  = contentExtent - inner;
        return over > 0.0f ? over : 0.0f;
    }
    float* scrollOffsetPtr() override { return &scrollOffset; }
    float  maxScrollAmount() const override { return maxScroll(); }
    void render(const UIWidgetRect& px, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>& out) const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

// ── Spacer ────────────────────────────────────────────────────────────────────
// A gap that is an element. It draws nothing and takes no click; the only thing
// it does is take up its slot, and in a layout box that is exactly what moves
// everything after it along — its Height in a vertical box, its Width in a
// horizontal one. Given a Slot Fill above 0 it swallows the space left over
// instead, which is how one thing goes to the left of a row and another to the
// far right without a single hand-computed offset.
//
// It has no properties of its own on purpose: everything it needs — the size on
// the box's axis and Slot Fill — is on the base and is already in the panel,
// under the right name for the box it sits in.
class HE_API UISpacer final : public UIElement
{
public:
    UISpacer() { sizeX = 40.0f; sizeY = 40.0f; hitTestable = false; }
    UIWidgetType type() const override { return UIWidgetType::Spacer; }
    const char*  typeName() const override { return "Spacer"; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UISpacer>(*this); }

    const UIPropTable& propTable() const override;
    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override {}
    void writeJson(nlohmann::json&) const override {}
    void readJson(const nlohmann::json&) override {}
};

// ── ListView (docs/he-apps-plan.md B2) ────────────────────────────────────────
// The one thing the widget system could not do: show a list whose length is not
// known when it is authored. Every other approach ends in the same place — N
// pre-made rows with a ceiling, or N real elements and a program that stalls at
// ten thousand of them.
//
// It works because it holds NO DATA. It holds
//   • a row template — an ordinary UI Widget asset, authored in the same
//     designer as everything else, which is where a row's look and its logic
//     live (this is what "Zeilenvorlage als WidgetRef" means: a row is a
//     widget, not a special case),
//   • an item COUNT its owner sets, and
//   • how tall a row is.
// From those three it works out which rows the view can actually show, realizes
// only those, and asks its owner to fill each one in (OnRowBind). Scrolling
// re-points the same handful of rows at different items rather than building new
// ones, so the cost is the size of the WINDOW and not the size of the list.
//
// The consequence to be honest about: a list that holds no items cannot sort
// them, and cannot answer what is in row 5 either. Both belong to whoever owns
// the data — it sorts its own array and calls refreshList, which is one call and
// keeps the single source of truth where it already was.
class HE_API UIListView final : public UIElement
{
public:
    // ── Authored ─────────────────────────────────────────────────────────────
    std::string rowWidget;         // content-relative path of the row template
    float rowHeight = 40.0f;       // every row is this tall (canvas units)
    float padding   = 4.0f;        // inset on all four sides
    float spacing   = 2.0f;        // gap between two rows
    glm::vec4 backColor{ 0.0f, 0.0f, 0.0f, 0.0f };
    // The two states a row has that its own widget cannot know about, drawn by
    // the list UNDER the row: which one the pointer is over and which ones are
    // picked. A row template that wants to look different when selected can
    // still do so — it is told, and this is the floor rather than the ceiling.
    glm::vec4 rowHoverColor{ 1.0f, 1.0f, 1.0f, 0.06f };
    glm::vec4 rowSelectedColor{ 0.20f, 0.42f, 0.74f, 0.55f };
    // 0 = none (a display list), 1 = single, 2 = multiple. An int rather than an
    // enum class because that is what UIPropType::Int carries into the panel and
    // into a graph; the three values are named in the editor's dropdown.
    int   selectionMode = 1;
    float barWidth = 6.0f;
    glm::vec4 barColor{ 0.75f, 0.75f, 0.80f, 0.65f };

    // ── Runtime ──────────────────────────────────────────────────────────────
    // How many items there are, which the owner sets and nothing here persists:
    // an application that reopens with the last run's row count would be showing
    // rows for data it has not loaded yet.
    int   itemCount = 0;
    float scrollOffset = 0.0f;
    float contentExtent = 0.0f;    // itemCount rows plus their gaps
    std::vector<int> selection;    // item indices, ascending, no duplicates
    int   hoveredRow = -1;

    UIListView()
    {
        sizeX = 320.0f; sizeY = 400.0f;
        clipChildren = true;      // rows outside the view must stop at its edge
        cornerRadius = glm::vec4(4.0f);
    }
    UIWidgetType type() const override { return UIWidgetType::ListView; }
    const char*  typeName() const override { return "ListView"; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UIListView>(*this); }

    // It places its rows itself (from their item index, not from a stack walk),
    // and it takes the click that picks one.
    bool laysOutChildren() const override { return true; }
    bool stacksVertically() const override { return true; }
    bool interactive() const override { return true; }
    // A list has a background, a border and a rounding like any other panel.
    bool hasSurfaceStyle() const override { return true; }
    // Rows come from the template, so a drop into the designer would create an
    // element the next frame throws away. Saying no here is what says that.
    bool acceptsChildren() const override { return false; }

    // ── The three numbers everything else is derived from ────────────────────
    float rowStep()     const { return rowHeight + spacing; }
    float innerHeight() const { return sizeY - 2.0f * padding > 0.0f
                                     ? sizeY - 2.0f * padding : 0.0f; }
    // Total height of `itemCount` rows with their gaps between them — the gaps
    // are BETWEEN, so n rows have n-1 of them.
    float measuredExtent() const
    {
        if (itemCount <= 0) return 0.0f;
        return itemCount * rowHeight + (itemCount - 1) * spacing;
    }
    float maxScroll() const
    {
        const float over = measuredExtent() - innerHeight();
        return over > 0.0f ? over : 0.0f;
    }
    float* scrollOffsetPtr() override { return &scrollOffset; }
    float  maxScrollAmount() const override { return maxScroll(); }

    // Which item is under a point `localY` canvas units below the element's TOP
    // edge; -1 when that is padding, a gap between two rows, or past the end.
    // A gap is deliberately nothing rather than the nearest row: clicking two
    // pixels of background must not pick a neighbour.
    int rowAt(float localY) const
    {
        const float step = rowStep();
        if (step <= 0.0f || itemCount <= 0) return -1;
        const float y = localY - padding + scrollOffset;
        if (y < 0.0f) return -1;
        const int i = static_cast<int>(y / step);
        if (i < 0 || i >= itemCount) return -1;
        return (y - i * step) <= rowHeight ? i : -1;
    }

    // How many items there are, with everything that follows from it: a
    // selection past the new end is DROPPED (not clamped — clamping would move
    // the highlight onto a row nobody picked) and the offset is pulled back into
    // range. The one place the count changes, so the property setter and the
    // script call cannot end up doing different amounts of housekeeping.
    void setItemCount(int n);

    // ── Selection ────────────────────────────────────────────────────────────
    bool isSelected(int item) const
    {
        for (const int i : selection) if (i == item) return true;
        return false;
    }
    // -1 when nothing is picked. "The" selection for the common single case,
    // and the anchor a keyboard step moves from in the multiple case.
    int firstSelected() const { return selection.empty() ? -1 : selection.front(); }
    // True when the set actually changed — the caller fires OnSelectionChanged
    // off that, so re-picking the row that is already picked stays silent.
    bool setSelected(int item, bool on);
    bool clearSelection();
    // Scroll so that `item` is fully inside the view; no-op when it already is.
    // True when the offset moved.
    bool scrollToItem(int item);

    const UIPropTable& propTable() const override;
    std::vector<UIEventDesc> events() const override
    {
        return { { "OnRowBind", UIPropType::Int, true },
                 { "OnSelectionChanged", UIPropType::Int, true },
                 { "OnRowActivated", UIPropType::Int, true } };
    }
    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

} // namespace HE
