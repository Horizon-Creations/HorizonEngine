#pragma once
#include <UIWidget/UIElement.h>
// For UICanvasScaleMode: a WidgetRef's rect is the embedded widget's screen, so
// it carries that widget's scale mode. (No cycle: UIWidgetTree.h includes
// UIElement.h only.)
#include <UIWidget/UIWidgetTree.h>
// UIRichText — a Text element caches its parsed markup (see UIText::parsed).
#include <Renderer/UIFont.h>

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
// One drawn row of a selectable label: which bytes it shows and where it shows
// them, in pixels. The draw, the hit test and the arrow keys all read the same
// list — the reason richLayoutOf exists one screen further down, and the reason
// this does. Two answers to "where does row two start" is a selection that
// highlights a different word than the one under the pointer.
struct UITextSelectRow
{
    std::size_t begin = 0;   // first byte drawn on the row
    std::size_t end   = 0;   // one past the last byte drawn — where End sits
    std::size_t next  = 0;   // first byte of the row below (past what a break ate)
    float       x     = 0.0f;   // left edge of the row's glyphs
    float       top   = 0.0f;   // top of the row's line box
    float       width = 0.0f;   // width of the drawn run
};

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
    // ── More than one voice in one label (docs/he-apps-plan.md B6) ───────────
    // Off, and off is what every existing label means: a Text that has always
    // shown a literal "<" must go on showing it. With it on, Text is markup —
    // `<color=#ff8800>`, `<size=1.5>`, `<link=id>`, closed by `</>`, `<<` for a
    // literal '<' — and the element becomes clickable exactly when the markup
    // declares a link, not before (see interactive()).
    bool        richText = false;

    // ── A label a reader may select and copy (plan B6, the rest) ─────────────
    // Off, and off is what every label authored so far means: a caption on a
    // button must not eat the press meant for the button, and a heading that
    // takes the keyboard focus is a heading that swallowed Tab. With it on the
    // label is still not editable — it takes a caret, a selection, Ctrl+A and
    // Ctrl+C, and nothing that would change a character of it.
    //
    // RICH TEXT WINS. A markup label lays out in runs of different sizes and
    // colours, and a hit test over those is a different problem from a hit test
    // over one font; half-built it would highlight one word and copy another.
    // So `selectionEnabled()` and not `selectable` is what everything asks.
    bool        selectable = false;
    // The same default the text field carries, so a selection looks like a
    // selection wherever the reader made it.
    glm::vec4   selectionColor{ 0.25f, 0.45f, 0.80f, 0.75f };

    // ── Selection state (runtime, never serialized) ──────────────────────────
    // Byte offsets into `text`, always on character boundaries — `caret` is the
    // end the reader is dragging, `selAnchor` the end they started at. Equal
    // means nothing is selected. On the element rather than in the manager for
    // the same reason the field keeps its own: a widget holds a live copy, and
    // two instances of the same label select independently.
    std::size_t caret = 0;
    std::size_t selAnchor = 0;
    // The width the words break against and the size they are measured at, both
    // only knowable while drawing and both needed by an arrow key, which arrives
    // without a viewport. Same reasoning and same mutability as the field's
    // wrapWidthPx/wrapSizePx; before the first draw they are zero and rowRanges()
    // falls back to the authored breaks.
    mutable float rowWidthPx = 0.0f;
    mutable float rowSizePx  = 0.0f;
    // The column Up and Down are aiming for, in pixels, or -1 for "none yet".
    // Walking a paragraph must come back to the same place it left rather than
    // creep left on every short line — the field learned this the same way.
    mutable float preferredCaretX = -1.0f;

    UIText() { sizeX = 200.0f; sizeY = 30.0f; }
    UIWidgetType type() const override { return UIWidgetType::Text; }
    const char*  typeName() const override { return "Text"; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UIText>(*this); }

    const UIPropTable& propTable() const override;

    // Element size implied by the current text/font (see autoSize). Callers apply
    // it before layout so the rect the glyphs lay out in already fits them.
    void applyAutoSize(float resolvedWidth, float fontScale = 1.0f) override;
    // A wrapping label authors its WIDTH — that is the column the words break
    // against — and only its height follows the text.
    int  autoSizedAxes() const override
    { return autoSize ? (((wordWrap ? 0 : kAxisX) | kAxisY) & ~stretchedAxes()) : 0; }

    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;

    // ── The parsed markup, and the three things that read it ─────────────────
    // Parsed on demand and kept until the text changes. Lazy rather than parsed
    // where the text is set, because the text is set from six places (the panel,
    // a Set Property, a component parameter, the loader, an animation, a script)
    // and a cache that any one of them can forget to refresh is a label showing
    // the last thing somebody typed.
    // On the TEXT and not on the base: only a label can hold a link, and an
    // event offered on every element there is would be one more thing to read
    // past in every menu.
    std::vector<UIEventDesc> events() const override
    { return { { "OnLinkClicked", UIPropType::String, true } }; }

    const HE::UIRichText& parsed() const;
    // Which link is at this point, or "" — the same layout the draw uses, which
    // is the whole reason it is a function and not a second calculation.
    std::string linkAt(const UIWidgetRect& px, float pxScaleY, float x, float y,
                       float fontScale = 1.0f) const;
    // A label is inert; a label with a link in it is not. Asking the MARKUP
    // rather than the flag is what keeps a rich label that happens to have no
    // link from swallowing the clicks meant for whatever is behind it.
    // A label is inert; one with a link in it, or one a reader may select in,
    // is not. Both halves are opt-in, so nothing that exists today starts
    // swallowing the presses meant for what it sits on.
    bool interactive() const override
    { return selectionEnabled() || (richText && parsed().hasLinks); }

    // ── Selecting and copying ────────────────────────────────────────────────
    // May this label be selected in at all? Everything asks THIS and not the
    // flag, so the rich-text exception lives in one place (see `selectable`).
    bool selectionEnabled() const { return selectable && !richText; }
    bool hasSelection() const { return caret != selAnchor; }
    std::size_t selMin() const { return caret < selAnchor ? caret : selAnchor; }
    std::size_t selMax() const { return caret < selAnchor ? selAnchor : caret; }
    std::string selectedText() const;
    // The text can be rewritten under a selection by a script, an animation or
    // a component parameter, so every entry point re-clamps rather than trusting
    // offsets taken before the last frame.
    void clampCaret();
    void selectAllText() { selAnchor = 0; caret = text.size(); }
    void clearSelection() { selAnchor = caret; }

    // The rows this label draws, laid out in `px`. Empty when the label is not
    // selectable, has no usable font or no size — the callers all treat that as
    // "no selection to be had", which is exactly what it is.
    std::vector<UITextSelectRow> selectRows(const UIWidgetRect& px, float pxScaleY,
                                            float fontScale = 1.0f) const;
    // Byte offset nearest to a point, in the same pixel space `selectRows` uses.
    // A point above the first row lands on it and one below the last on that —
    // dragging off the top of a paragraph must not drop the selection.
    std::size_t caretAtPoint(const UIWidgetRect& px, float pxScaleY, float x, float y,
                             float fontScale = 1.0f) const;

    // The rows as byte ranges only, without geometry — what Home, End and the
    // up/down arrows need, and they arrive without a viewport. Comes out of the
    // width and size the last draw remembered, so it is the SAME split the
    // glyphs were laid out with; before the first draw it is the authored
    // breaks, which is what a label that has never been on screen has.
    std::vector<HE::UITextVisualLine> rowRanges() const;
    // Where a byte sits across its own row, and which byte is nearest a column
    // — the two halves of walking up and down a paragraph. Both answer 0 and
    // the row's start while the label has never been drawn (no measured size),
    // which is what makes the arrows a no-op there rather than a wrong jump.
    float       caretXInRow(const HE::UITextVisualLine& row, std::size_t byte) const;
    std::size_t byteAtRowX(const HE::UITextVisualLine& row, float x) const;

private:
    mutable std::string      m_parsedFrom;   // the markup m_parsed was built from
    mutable HE::UIRichText   m_parsed;
    mutable bool             m_parsedOnce = false;
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
    // Drawn as a sliding switch instead of a ticked box. A property and not a
    // type of its own, because it is the same VALUE with a second picture: a
    // switch has nothing a checkbox does not — one bool, one label, one event —
    // and a second type would have copied all of it to change nine lines of
    // drawing. It is the picture that says which of the two a control is:
    // a box means "this will be true when I press OK", a switch means "this is
    // in effect now", and both of those are one bool underneath.
    bool        switchStyle = false;

    // ── Fit the row to the label (docs/he-apps-plan.md D4) ───────────────────
    // Off, and off is what every checkbox authored before this means: 200×28,
    // whatever is written on it. On, the element is exactly the control plus the
    // gap plus the label, so a translated caption cannot run out of its own row
    // and a bigger font cannot cut it in half.
    bool        autoSize = false;

    // ── The one set of numbers the box is drawn and measured from ────────────
    // `render` and `applyAutoSize` ask this, and they have to: a measurement
    // that used its own idea of how wide the tick box is would size the row to a
    // control nobody drew. All three come out in the units the font size is
    // handed in — pixels while drawing, canvas units while measuring.
    //
    // `heightLimit` is the element's height where there IS one (drawing), and 0
    // where the height is what is being computed (measuring): the box is capped
    // by the row so a deliberately tiny checkbox still fits inside itself.
    struct BoxMetrics { float box = 0.0f, ctrl = 0.0f, gap = 0.0f; };
    static BoxMetrics metricsFor(float fontPx, float heightLimit, bool asSwitch)
    {
        BoxMetrics m;
        m.box  = fontPx * 1.15f;
        if (heightLimit > 0.0f && heightLimit < m.box) m.box = heightLimit;
        m.ctrl = asSwitch ? m.box * 1.8f : m.box;
        m.gap  = 0.4f * m.box;   // scales with the box, not a fixed 8 px
        return m;
    }

    UICheckBox() { sizeX = 200.0f; sizeY = 28.0f; }
    UIWidgetType type() const override { return UIWidgetType::CheckBox; }
    const char*  typeName() const override { return "CheckBox"; }
    bool interactive() const override { return true; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UICheckBox>(*this); }

    void applyAutoSize(float resolvedWidth, float fontScale = 1.0f) override;
    int  autoSizedAxes() const override
    { return autoSize ? ((kAxisX | kAxisY) & ~stretchedAxes()) : 0; }

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
    // "Working on it" as opposed to "this far along". The bar stops reporting a
    // fraction and slides a segment across its track instead, which is the one
    // honest picture of a job whose length nobody knows — a determinate bar
    // that has to be invented ends up at 90% for a minute, and everyone who
    // has waited on one has stopped believing it.
    //
    // Value is left alone rather than ignored away: switching back has to show
    // what it showed before, and a graph writing progress into a bar it has not
    // taken off indeterminate yet must not lose the writes.
    bool      indeterminate = false;

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

    // ── More than one line (docs/he-apps-plan.md B1b) ────────────────────────
    // On, the field holds newlines and shows them: Enter inserts one instead of
    // committing, the caret moves up and down, a selection spans lines, and the
    // text scrolls vertically rather than sideways. Off is every field that ever
    // existed here and stays byte-identical.
    //
    // Two consequences worth stating where somebody will read them:
    //
    //   ENTER NO LONGER COMMITS. A multiline field fires OnTextChanged as you
    //   type and OnTextCommitted only when it loses focus — there is no key left
    //   to mean "done" once Enter means "new line", and inventing a modifier
    //   chord for it would be a convention nobody expects from a text box.
    //
    //   The text is drawn from the TOP, not centred. One line centred in its box
    //   is right; ten lines centred means the first one moves every time you add
    //   an eleventh.
    bool        multiline = false;

    // ── …and breaking those lines at the field's own edge ────────────────────
    // On, a paragraph too wide for the box carries on the row below instead of
    // running off to the right. Only meaningful while `multiline`, and off by
    // default so a field that was authored before this existed keeps breaking
    // exactly where somebody pressed Enter and nowhere else.
    //
    // A wrapped row is not an authored line, and every part of the field that
    // thinks in lines knows it: Down goes to the ROW below (which may be the
    // same paragraph), End stops at the row's edge, and a click lands on the
    // row it was aimed at. That is why there is one line source and not three
    // (see visualLines()).
    //
    // A password field ignores this. Its dots are drawn from the text's bytes,
    // so wrapping measured on the dots and addressed in the bytes would be two
    // answers to "where does row two start" — and a password is one line.
    bool        wrapText = false;

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
    // …and downwards, for a multiline field. Same reasoning, same mutability.
    // Exposed through scrollOffsetPtr/maxScrollAmount below so the wheel and the
    // preview's state capture reach it through the machinery they already have
    // rather than through a second one that knows about text fields.
    mutable float scrollPxY = 0.0f;
    mutable float contentHeightPx = 0.0f;   // what render() last measured
    mutable float viewHeightPx    = 0.0f;
    // The width the text has to fit into and the pixel size it is measured at —
    // both only knowable while drawing, and both needed by an arrow key, which
    // arrives without a viewport. Same reasoning and same mutability as the two
    // above; before the first draw they are zero and visualLines() falls back to
    // hard breaks, which is what the field did before wrapping existed.
    mutable float wrapWidthPx = 0.0f;
    mutable float wrapSizePx  = 0.0f;

    // ── Which column an up/down arrow is aiming for ──────────────────────────
    // Moving down through a SHORT line and on again has to come back to the
    // column you started in — without a remembered goal the caret would stick to
    // wherever the short line ended, and three presses of Down would walk it to
    // the left edge. -1 = no goal, take the caret's current column.
    //
    // Runtime state like caret itself: never serialized, cleared by any move
    // that is not an up/down arrow.
    mutable float preferredCaretX = -1.0f;

    // ── Undo history (runtime, never serialized) ─────────────────────────────
    // Per FIELD, not per app: Ctrl+Z inside a text box takes back what was typed
    // into that box, and nothing else. The scene's undo stack and this one never
    // meet — the editor only routes keys here while a field is being edited, and
    // in a packaged app there is no scene undo at all.
    //
    // Each entry is the whole field as it stood BEFORE a group of edits. Whole
    // strings rather than diffs because a text field holds a sentence, not a
    // document, and a hundred copies of a sentence is nothing next to being able
    // to read this code.
    struct EditState { std::string text; size_t caret = 0; size_t selAnchor = 0; };
    // What the group currently being collected consists of. Typing 'a', 'b', 'c'
    // is one step; a Backspace after them starts a second, because undo that
    // walks back through a run of same-kind keystrokes one at a time is not what
    // anybody means by "take that back".
    enum class EditKind : int { None, Insert, DeleteBack, DeleteForward, DeleteWord, Replace };
    std::vector<EditState> undoStack;
    std::vector<EditState> redoStack;
    EditKind openRun = EditKind::None;
    static constexpr size_t kMaxUndoSteps = 100;

    // Close the open group without recording anything: the next edit starts a
    // new one. Every caret move, click and focus change does this, which is what
    // makes "type, click elsewhere, type again" two undo steps instead of one.
    void sealUndoRun() { openRun = EditKind::None; }
    // Remember `before` as the state to come back to. Does nothing when the text
    // did not actually change, and nothing when `coalesce` says this edit
    // continues the group already open. Always clears the redo stack: once you
    // edit again there is only one future left.
    void recordEdit(const EditState& before, EditKind kind, bool coalesce);
    // Step back / forward one group, caret and selection included. False when
    // the respective stack is empty.
    //
    // A script that rewrote `text` from outside leaves a history that no longer
    // describes the field, and undo will put its own older text back over it.
    // That is the price of recording the state before each edit rather than
    // watching the property for changes, and it is the same thing every text
    // box does when its value is set behind its back.
    bool undoEdit();
    bool redoEdit();

    // A multiline field is a scrolling container as far as the wheel is
    // concerned. Answering nullptr while single-line is what keeps a wheel over
    // an ordinary field scrolling the PAGE, which is what it did before.
    float* scrollOffsetPtr() override { return multiline ? &scrollPxY : nullptr; }
    float  maxScrollAmount() const override
    { return multiline ? std::max(0.0f, contentHeightPx - viewHeightPx) : 0.0f; }

    // ── A number field (docs/he-apps-plan.md B9) ─────────────────────────────
    // Two arrows at the right edge that step the value, plus what a step is and
    // how far it may go. Built on the text field rather than beside it: a
    // number field IS a text field somebody can also type into, and every
    // toolkit that made it a separate control ended up reimplementing selection,
    // the clipboard and the caret to get there.
    //
    // Off by default, so every field authored before this draws and saves
    // exactly as it did. Ignored while `multiline`: a number has one line.
    bool        steppers = false;
    float       step     = 1.0f;
    // The range. min >= max means unbounded, which is also the default — a
    // spinner with no ceiling is a legitimate thing to want, and 0..0 is the
    // only pair that could not mean anything else.
    float       minValue = 0.0f;
    float       maxValue = 0.0f;

    // The two arrows, in the field's own pixel space, up first. False when
    // there are none to draw. ONE sum for the drawing and the press, the same
    // rule the scrollbar thumb follows: an arrow that is clicked somewhere
    // other than where it is drawn is worse than no arrow.
    bool stepperRects(const UIWidgetRect& px, UIWidgetRect& up, UIWidgetRect& down) const;

    // ── The cross that empties it (docs/he-apps-plan.md B9) ──────────────────
    // A search field with three letters in it needs a way back to none that is
    // not eight presses of Backspace. On by default nowhere: every field
    // authored before this keeps its whole width, and a form field with a cross
    // beside it is a form field that looks like it can be dismissed.
    //
    // It is a property of the FIELD and not a component drawn beside one,
    // because it needs two things a sibling element cannot have: the text, to
    // know whether to appear at all, and the field itself, to clear it with an
    // undo step. That is the "logic a component does not carry" the search
    // component was left without.
    bool        clearButton = false;

    // Where it sits, in the field's own pixel space. False when there is none
    // to draw — and it is NOT drawn on an empty field, on a read-only one, or
    // on a multiline one, so this answers the "is there one" question as well.
    // The same sum for the drawing and the press, like stepperRects; when a
    // field has arrows too, the cross sits to their left rather than under them.
    bool clearButtonRect(const UIWidgetRect& px, UIWidgetRect& out) const;
    // Empty it, recording one undo step so Ctrl+Z brings the text back. False
    // when there was nothing to clear.
    bool applyClear();
    // Step the value by `dir` steps and write it back as text, clamped to the
    // range. False when nothing changed — at a limit, or with steppers off.
    bool applyStep(int dir);

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
    // Byte offset nearest a point INSIDE the field's text area — what a click
    // has to answer to put the caret where it was aimed. `localY` is measured
    // from the top of that area and only matters while multiline; it takes a
    // real argument rather than defaulting to 0 on purpose, because a defaulted
    // zero would quietly mean "the first line" at every call site that forgot.
    size_t caretAtPoint(float localX, float localY, float pxScaleY,
                        float fontScale = 1.0f) const;
    // The rows the field shows, wrapping included. THE line source: drawing,
    // clicking and Home/End/Up/Down all ask this, because a field where the
    // caret disagrees with the glyphs about where row two starts is a field
    // that is wrong in the way nobody can describe. Uses the width render() last
    // measured; before the first draw, hard breaks only.
    std::vector<HE::UITextVisualLine> visualLines() const;
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

    // ── Fit the box to the options (docs/he-apps-plan.md D4) ─────────────────
    // Off by default, like every other fit-to-content switch here. On, the
    // closed box is measured against ALL the options and not against the
    // selected one: a combo that resized itself every time somebody picked a
    // different entry would move whatever sits next to it, and the widest entry
    // has to fit anyway or the list is wider than the thing it drops out of.
    bool  autoSize = false;

    UIComboBox() { sizeX = 220.0f; sizeY = 32.0f; cornerRadius = glm::vec4(4.0f); }
    // The closed box is the surface; the open list draws over it.
    bool hasSurfaceStyle() const override { return true; }
    UIWidgetType type() const override { return UIWidgetType::ComboBox; }
    const char*  typeName() const override { return "ComboBox"; }
    bool interactive() const override { return true; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UIComboBox>(*this); }

    void applyAutoSize(float resolvedWidth, float fontScale = 1.0f) override;
    int  autoSizedAxes() const override
    { return autoSize ? ((kAxisX | kAxisY) & ~stretchedAxes()) : 0; }

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

    // ── What this copy was told ──────────────────────────────────────────────
    // One value per parameter the referenced widget DECLARES (see
    // UIWidgetParam). Stored by the parameter's name rather than by element and
    // property, so the component may be rebuilt inside without touching a
    // single page that uses it. Only the ones actually set are here: an unset
    // parameter is not an empty value, it is the component's own default.
    //
    // Written into the embedded copy at graft time and never again — the
    // component is a starting state, not a live binding. What changes after
    // that changes through its script, the same way anything else does.
    std::vector<std::pair<std::string, UIPropValue>> paramValues;

    const UIPropValue* paramValue(const std::string& name) const
    {
        for (const auto& [n, v] : paramValues) if (n == name) return &v;
        return nullptr;
    }
    // Set (present) or clear (absent → back to the component's own default).
    void setParamValue(const std::string& name, const UIPropValue* v)
    {
        for (auto it = paramValues.begin(); it != paramValues.end(); ++it)
            if (it->first == name)
            {
                if (v) it->second = *v; else paramValues.erase(it);
                return;
            }
        if (v) paramValues.emplace_back(name, *v);
    }
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
    // the base element's Min/Max values are the floor and the ceiling it stays
    // between. That makes a menu grow and shrink with the number of entries in
    // it instead of being a fixed rectangle that a sixth entry falls out of.
    //
    // A FILLING child cannot be measured (its size is a share of what is left
    // over, which is what this is computing), so it counts as nothing.
    bool  sizeToContent = false;

    bool laysOutChildren() const override { return true; }
    bool acceptsChildren() const override { return true; }
    // Both axes come out of the measurement while it is on — see the container
    // pass in uiApplyAutoSize, which is what actually writes them.
    int  autoSizedAxes() const override
    { return sizeToContent ? (kAxisX | kAxisY) : 0; }
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
    // Only the height, for the reason in the note above this class.
    int  autoSizedAxes() const override { return sizeToContent ? kAxisY : 0; }
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
    bool   scrollBar(UIScrollBarStyle& s) const override
    { s = { barWidth, padding, contentExtent, barColor }; return true; }
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
    // The MEASURED extent, not the cached one: a list's content is worked out
    // from its item count, and the cache is a frame behind on the frame the
    // count changed.
    bool   scrollBar(UIScrollBarStyle& s) const override
    { s = { barWidth, padding, measuredExtent(), barColor }; return true; }

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

// ── TabBox ───────────────────────────────────────────────────────────────────
// Pages behind a strip of tabs, one showing at a time (docs/he-apps-plan.md B5).
//
// ITS CHILDREN ARE THE PAGES, and each child's NAME is its label. No second
// list: a parallel array of titles beside a list of children is two things that
// have to be kept in step by hand, and they never are — the first time somebody
// reorders the pages, every label belongs to the wrong one.
//
// Overflow is CLIPPED in this version: more tabs than fit are cut off at the
// right edge rather than scrolled or stacked. Said out loud because it is a
// real limit and not an oversight; a scrolling strip belongs with the rest of
// B9's polish.
class HE_API UITabBox final : public UIElement
{
public:
    // Which page shows. Authored (so the designer can work on page three) AND
    // runtime state (so clicking a tab means something). Out of range shows the
    // first page rather than none: a Tab Box with nothing in it is a mistake
    // that should look like one, not like an empty panel.
    int   activeTab = 0;
    float tabHeight = 30.0f;
    float fontSize  = 16.0f;
    // Room either side of a label inside its tab, which is what makes the tabs
    // as wide as what they say instead of all the same width.
    float tabPadding = 14.0f;
    glm::vec4 stripColor   { 0.12f, 0.12f, 0.14f, 1.0f };
    glm::vec4 tabColor     { 0.18f, 0.18f, 0.21f, 1.0f };
    glm::vec4 activeColor  { 0.26f, 0.26f, 0.31f, 1.0f };
    glm::vec4 textColor    { 0.92f, 0.92f, 0.95f, 1.0f };
    glm::vec4 pageColor    { 0.0f, 0.0f, 0.0f, 0.0f };   // behind the page, transparent

    UITabBox() { sizeX = 400.0f; sizeY = 300.0f; }
    UIWidgetType type() const override { return UIWidgetType::TabBox; }
    const char*  typeName() const override { return "TabBox"; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UITabBox>(*this); }

    bool acceptsChildren() const override { return true; }
    bool laysOutChildren() const override { return true; }
    // Unlike every other layout container, this one is NOT transparent to the
    // pointer: its strip is a row of things to click. The pages sit below the
    // strip and are deeper in the tree, so they still win where they are.
    bool interactive() const override { return true; }
    // Every page but the active one. THE reason this is a parent's question:
    // the picture and the pointer both come through uiElementEffectiveVisible,
    // so a button on a hidden page cannot answer a click at its coordinates.
    bool hidesChild(const UIWidgetTree& tree, const UIElement& child) const override;

    // The page names, in order — what the strip says. Filled by the layout pass
    // each frame (uiApplyAutoSize), because render() has no tree to ask.
    //
    // Only the DRAWING depends on this. Which page is shown and which tab a
    // click hit are both answered from the tree directly, so the one thing a
    // stale cache could cost is a label that is one frame old.
    mutable std::vector<std::string> tabLabels;

    // ── One arithmetic, three consumers ──────────────────────────────────────
    // Where each tab sits in the strip, given the labels and the font. render()
    // draws with it, the runtime decides which tab a click hit with it, and the
    // designer previews with it — the same lesson the ComboBox's shared
    // geometry taught, where two copies drifted the moment the corners rounded.
    //
    // `labels` are the page names in order. `outX`/`outW` are filled per tab, in
    // the same pixels the element's rect is in.
    static void tabLayout(const UIWidgetRect& px, float sizePx, float padPx,
                          const std::vector<std::string>& labels,
                          uint64_t fontAtlasKey,
                          std::vector<float>& outX, std::vector<float>& outW);
    // Which tab a point lands on, or -1 for none (below the strip, or past the
    // last tab). Same inputs, so it cannot disagree with the drawing.
    static int tabAtPoint(const UIWidgetRect& px, float sizePx, float padPx, float tabH,
                          const std::vector<std::string>& labels, uint64_t fontAtlasKey,
                          float x, float y);

    const UIPropTable& propTable() const override;
    std::vector<UIEventDesc> events() const override
    { return { { "OnTabChanged", UIPropType::Int, true } }; }
    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

// ── Splitter ─────────────────────────────────────────────────────────────────
// Two panes and a divider you can drag (docs/he-apps-plan.md B5).
//
// EXACTLY two children are placed; a third and beyond get nothing and are not
// drawn. Two rather than N because a splitter with three panes has two dividers
// and a ratio that is no longer one number — and nesting one splitter in
// another says the same thing with the arithmetic already solved.
class HE_API UISplitter final : public UIElement
{
public:
    // Which way the panes sit beside each other. Vertical = one above the other
    // with a horizontal divider, which is what "split vertically" means to
    // everyone except the axis it is named after — so the FIELD says what the
    // panes do, not what the divider looks like.
    bool  vertical = false;
    // Where the divider is, as a share of the length. Clamped by the minimums
    // below in the LAYOUT and not only while dragging: an authored 0.01 with a
    // 100-pixel minimum has to lay out clamped, or the designer and the runtime
    // disagree the moment the file is loaded.
    float ratio = 0.5f;
    float dividerSize = 6.0f;
    // How small each pane may get, in canvas units. A pane dragged to nothing
    // is a pane nobody can get back.
    float minFirst = 40.0f, minSecond = 40.0f;
    glm::vec4 dividerColor{ 0.28f, 0.28f, 0.33f, 1.0f };

    UISplitter() { sizeX = 400.0f; sizeY = 300.0f; }
    UIWidgetType type() const override { return UIWidgetType::Splitter; }
    const char*  typeName() const override { return "Splitter"; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UISplitter>(*this); }

    bool acceptsChildren() const override { return true; }
    bool laysOutChildren() const override { return true; }
    bool stacksVertically() const override { return vertical; }
    // Same as the Tab Box: a container you can grab. The panes are deeper and
    // cover everything except the divider, so a press only reaches this element
    // where the divider is — which is exactly where it should.
    bool interactive() const override { return true; }
    // Everything after the second child.
    bool hidesChild(const UIWidgetTree& tree, const UIElement& child) const override;

    // The ratio this splitter actually lays out with: what was authored, pulled
    // inside the minimums for a length of `lengthPx`. One function so the slot
    // rect, the drag and the designer all get the same number.
    float clampedRatio(float lengthPx) const;

    const UIPropTable& propTable() const override;
    std::vector<UIEventDesc> events() const override
    { return { { "OnSplitChanged", UIPropType::Float, true } }; }
    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

// ── The calendar arithmetic ──────────────────────────────────────────────────
// Free functions rather than members: they are about the Gregorian calendar and
// not about a widget, a test can pin them without building an element, and the
// day a second thing needs "how many days has February" it must not have to
// reach into a UI type to ask.
HE_API bool uiIsLeapYear(int year);
HE_API int  uiDaysInMonth(int year, int month);   // month 1..12
// Sakamoto's rule. 0 = Sunday … 6 = Saturday, proleptic Gregorian, which is what
// every calendar in a UI shows.
HE_API int  uiDayOfWeek(int year, int month, int day);

// ── DatePicker (docs/he-apps-plan.md B9) ─────────────────────────────────────
// A month at a time: a caption with an arrow either side, a weekday strip, and
// SIX rows of seven days. Six always, even for a February that fits in four —
// a control that changes height when you page through it moves everything below
// it, and a calendar is the one widget people page through fast.
//
// The grid is scale-free on purpose. Its rows are FRACTIONS of the element's
// own rect (a caption of 1.25 rows, a weekday strip of 0.75, six day rows =
// eight in all), so `cellAt` and the draw share one sum without either of them
// having to convert canvas units into pixels first. The lesson is the scroll
// thumb's and the tab strip's: the day drawing and grabbing are two sums is the
// day a click lands on the wrong date.
class HE_API UIDatePicker final : public UIElement
{
public:
    // The date shown AND picked. Three ints rather than one string, because a
    // property is written by a script and by the editor's number rows, and a
    // string would have to be parsed on every one of those writes.
    //
    // NOT clamped on write — deliberately. A property that changes the value it
    // was given is a property the editor and HorizonCode cannot round-trip, so
    // the clamp lives at every READ (clampedMonth/clampedDay), the way a
    // Slider's normalized() guards min == max instead of the setter doing it.
    int   year = 2026, month = 1, day = 1;
    // Which column the week starts in. Monday by default: the calendar this
    // engine's users read starts there, and the American Sunday is the setting.
    bool  mondayFirst = true;
    float fontSize = 14.0f;
    glm::vec4 backColor{ 0.13f, 0.13f, 0.15f, 1.0f };
    glm::vec4 headerColor{ 0.18f, 0.18f, 0.21f, 1.0f };
    glm::vec4 textColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    // The days that belong to the month before or after this one. They are
    // shown rather than left blank so the grid reads as a continuous calendar,
    // and clicking one pages to that month — which is the shortest way there.
    glm::vec4 mutedColor{ 0.55f, 0.55f, 0.60f, 1.0f };
    glm::vec4 selectedColor{ 0.30f, 0.60f, 0.90f, 1.0f };
    glm::vec4 hoverColor{ 0.25f, 0.25f, 0.30f, 1.0f };

    // Runtime state, like a ComboBox's hoverIndex and for the same reason: the
    // pointer stays on the same ELEMENT while it travels from day to day, so
    // nothing in the hover machinery notices. -1 = none.
    int hoverCell = -1;

    UIDatePicker() { sizeX = 240.0f; sizeY = 220.0f; cornerRadius = glm::vec4(4.0f); }
    UIWidgetType type() const override { return UIWidgetType::DatePicker; }
    const char*  typeName() const override { return "DatePicker"; }
    bool interactive() const override { return true; }
    bool hasSurfaceStyle() const override { return true; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UIDatePicker>(*this); }

    const UIPropTable& propTable() const override;
    std::vector<UIEventDesc> events() const override
    { return { { "OnDateChanged", UIPropType::String, true } }; }

    // ── The clamps, applied where the value is READ ──────────────────────────
    int clampedMonth() const { return month < 1 ? 1 : (month > 12 ? 12 : month); }
    int clampedYear()  const { return year  < 1 ? 1 : (year  > 9999 ? 9999 : year); }
    int clampedDay()   const
    {
        const int n = uiDaysInMonth(clampedYear(), clampedMonth());
        return day < 1 ? 1 : (day > n ? n : day);
    }
    // "YYYY-MM-DD" — the one form of a date nobody's locale disagrees about,
    // and the payload OnDateChanged carries. The same reason the number field
    // writes its value without a locale: a string a parser cannot read back is
    // not an answer.
    std::string isoDate() const;

    static const int kCols = 7, kRows = 6, kCells = 42;

    // Which cell the 1st of the shown month falls in, 0..6.
    int firstCell() const
    {
        const int dow = uiDayOfWeek(clampedYear(), clampedMonth(), 1);
        return (dow - (mondayFirst ? 1 : 0) + 7) % 7;
    }
    // The date cell `index` (0..41) shows, which may belong to the month before
    // or after. Out-of-range index yields the shown month's 1st.
    void dateAtCell(int index, int& outYear, int& outMonth, int& outDay) const;
    // True when that cell belongs to the month the caption names.
    bool cellInMonth(int index) const;
    // …and which cell holds the picked day, or -1 (never, in practice: the
    // picked day is always in the shown month).
    int selectedCell() const;

    // ── The one sum ──────────────────────────────────────────────────────────
    // Everything the draw and the hit test need, in whatever units `r` is in.
    struct Layout
    {
        UIWidgetRect header, weekdays, grid;
        UIWidgetRect prevArrow, nextArrow;
        float cellW = 0.0f, cellH = 0.0f;
    };
    static Layout layoutIn(const UIWidgetRect& r);
    // The cell a point is on, or -1. Same units as `r`.
    static int    cellAt(const UIWidgetRect& r, float x, float y);
    // -1 = the left arrow, +1 = the right one, 0 = neither.
    static int    arrowAt(const UIWidgetRect& r, float x, float y);
    // The caption, e.g. "September 2026". English month names: the catalog (B10)
    // is what translates a widget's text, and it works on properties, so a name
    // baked into the draw would be the one string nobody can reach. It is here
    // as the readable default, and a localized calendar is its own feature.
    std::string caption() const;
    // The seven weekday initials, in the order this picker's columns run.
    static const char* weekdayInitial(int column, bool mondayFirst);

    // Page by whole months, keeping the day where it can be kept — the 31st of
    // March goes back to the 28th of February, not forward into March again.
    void addMonths(int delta);

    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

// ── HSV ⇄ RGB ────────────────────────────────────────────────────────────────
// Free, like the calendar arithmetic above, and for the same reasons. Both work
// on the sRGB-encoded numbers this UI actually blends, which is what HSV has
// always been defined over — a conversion through linear light would be a
// different colour space wearing the same name.
HE_API glm::vec3 uiHsvToRgb(float hueDeg, float sat, float val);
HE_API void      uiRgbToHsv(const glm::vec3& rgb, float& hueDeg, float& sat, float& val);

// ── ColorPicker (docs/he-apps-plan.md B9) ────────────────────────────────────
// A saturation/value field, a hue strip beside it, and an alpha strip when it is
// asked for. Out of ordinary quads, and EXACTLY so — this is not an
// approximation that looks close enough:
//
//   * At S = V = 1 the hue wheel is piecewise LINEAR in RGB across each 60°
//     segment, so six gradient quads are the hue strip, not a picture of it.
//   * HSV at V = 1 is mix(white, hue, S) exactly, which is one horizontal
//     gradient.
//   * Multiplying by V is the same as laying black over it at alpha 1 - V,
//     which is one vertical gradient with alpha in it. The blend is "over" in
//     sRGB, which is the space the arithmetic above is in.
//
// So the field is two quads and the strip is six, and no backend needs to learn
// anything about colour pickers.
class HE_API UIColorPicker final : public UIElement
{
public:
    // The colour IS the truth, kept verbatim: what a script writes is what it
    // reads back, bit for bit. Storing H/S/V instead and deriving the colour
    // would round-trip through two conversions and hand back something a hair
    // off — which is the sort of thing that shows up as a value drifting every
    // time a panel is reopened.
    glm::vec4 color{ 0.30f, 0.60f, 0.90f, 1.0f };
    // …with ONE exception, and it is why this field exists. A grey has no hue —
    // rgbToHsv can only answer 0 — so dragging the value down to black and back
    // up again would land on red. This remembers the hue the drag came from and
    // is only ever consulted while the saturation is nothing.
    float hue = 210.0f;
    // The fourth channel, off by default: most colours in an app are opaque and
    // a strip for a number nobody sets is a strip in the way.
    bool  showAlpha = false;
    float barWidth = 18.0f;   // canvas units, the hue (and alpha) strip's width
    float gap      = 8.0f;    // between the field and the strips
    glm::vec4 backColor{ 0.13f, 0.13f, 0.15f, 1.0f };

    UIColorPicker() { sizeX = 240.0f; sizeY = 180.0f; cornerRadius = glm::vec4(4.0f); }
    UIWidgetType type() const override { return UIWidgetType::ColorPicker; }
    const char*  typeName() const override { return "ColorPicker"; }
    bool interactive() const override { return true; }
    bool hasSurfaceStyle() const override { return true; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UIColorPicker>(*this); }

    const UIPropTable& propTable() const override;
    std::vector<UIEventDesc> events() const override
    { return { { "OnColorChanged", UIPropType::Color, true } }; }

    // The picked colour as H, S and V. `hueOf` is the only one that consults the
    // remembered hue, and only where the colour cannot answer.
    float saturationOf() const;
    float valueOf() const;
    float hueOf() const;
    // Write H/S/V, keep alpha, and remember the hue. This is what a drag calls.
    void setHsv(float hueDeg, float sat, float val);
    // Write a colour and keep `hue` in step with it — the setter behind the
    // "Color" property, so a script writing a colour and a hand dragging the
    // field leave the element in the same state.
    void setColor(const glm::vec4& c);

    // ── The one sum, again ───────────────────────────────────────────────────
    struct Parts { UIWidgetRect sv, hue, alpha; bool hasAlpha = false; };
    // `barPx`/`gapPx` are the two properties already converted into whatever
    // units `r` is in — the caller knows the scale, this does not.
    static Parts partsIn(const UIWidgetRect& r, float barPx, float gapPx, bool withAlpha);

    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

} // namespace HE
