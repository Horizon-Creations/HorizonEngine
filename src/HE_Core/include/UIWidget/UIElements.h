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
    // Horizontal alignment inside the element rect: 0 left, 1 centre.
    int         align = 0;

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
    std::string text = "Button";
    float       fontSize = 20.0f;
    glm::vec4   color{ 0.20f, 0.20f, 0.20f, 1.0f };   // normal
    glm::vec4   hoveredColor{ 0.30f, 0.30f, 0.30f, 1.0f };
    glm::vec4   pressedColor{ 0.15f, 0.15f, 0.15f, 1.0f };
    glm::vec4   textColor{ 1.0f, 1.0f, 1.0f, 1.0f };

    UIButton() { sizeX = 180.0f; sizeY = 48.0f; }
    UIWidgetType type() const override { return UIWidgetType::Button; }
    const char*  typeName() const override { return "Button"; }
    bool hasMaterialSlot() const override { return true; }
    bool hasTextureSlot()  const override { return true; }
    bool interactive() const override { return true; }
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

    UIProgressBar() { sizeX = 240.0f; sizeY = 20.0f; }
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

    UITextInput() { sizeX = 240.0f; sizeY = 32.0f; }
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

    UIComboBox() { sizeX = 220.0f; sizeY = 32.0f; }
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
    void render(const UIWidgetRect& px, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>& out) const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

} // namespace HE
