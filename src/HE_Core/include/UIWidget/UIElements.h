#pragma once
#include <UIWidget/UIElement.h>

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

} // namespace HE
