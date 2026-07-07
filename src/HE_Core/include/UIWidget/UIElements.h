#pragma once
#include <UIWidget/UIElement.h>

// Concrete widget element types. Each declares its own data + properties +
// events; render() and JSON live in UIElement.cpp. Field defaults are the
// "sensible defaults" a freshly-added element starts with.

namespace HE {

// Small helpers so the getProp/setProp bodies stay compact.
inline UIPropValue propColor(const glm::vec4& c) { return UIPropValue::ofColor(c); }
inline glm::vec4   asColor(const UIPropValue& v) { return v.col; }

// ── Panel ─────────────────────────────────────────────────────────────────────
class UIPanel final : public UIElement
{
public:
    glm::vec4 color{ 0.12f, 0.12f, 0.12f, 0.85f };

    UIWidgetType type() const override { return UIWidgetType::Panel; }
    const char*  typeName() const override { return "Panel"; }
    bool hasMaterialSlot() const override { return true; }
    std::unique_ptr<UIElement> clone() const override
    { auto p = std::make_unique<UIPanel>(*this); return p; }

    std::vector<UIPropDesc> properties() const override
    { return { { "Color", UIPropType::Color } }; }
    UIPropValue getProp(const std::string& n) const override
    { if (n == "Color") return propColor(color); return {}; }
    void setProp(const std::string& n, const UIPropValue& v) override
    { if (n == "Color") color = asColor(v); }
    std::vector<UIEventDesc> events() const override
    { return { { "OnMouseEnter" }, { "OnMouseLeave" }, { "OnClicked" } }; }

    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

// ── Image ─────────────────────────────────────────────────────────────────────
class UIImage final : public UIElement
{
public:
    glm::vec4 tint{ 1.0f, 1.0f, 1.0f, 1.0f };

    UIImage() { sizeX = 128.0f; sizeY = 128.0f; }
    UIWidgetType type() const override { return UIWidgetType::Image; }
    const char*  typeName() const override { return "Image"; }
    bool hasMaterialSlot() const override { return true; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UIImage>(*this); }

    std::vector<UIPropDesc> properties() const override
    { return { { "Tint", UIPropType::Color } }; }
    UIPropValue getProp(const std::string& n) const override
    { if (n == "Tint") return propColor(tint); return {}; }
    void setProp(const std::string& n, const UIPropValue& v) override
    { if (n == "Tint") tint = asColor(v); }
    std::vector<UIEventDesc> events() const override
    { return { { "OnMouseEnter" }, { "OnMouseLeave" }, { "OnClicked" } }; }

    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

// ── Text ──────────────────────────────────────────────────────────────────────
class UIText final : public UIElement
{
public:
    std::string text = "Text";
    float       fontSize = 22.0f;
    glm::vec4   color{ 1.0f, 1.0f, 1.0f, 1.0f };

    UIText() { sizeX = 200.0f; sizeY = 30.0f; }
    UIWidgetType type() const override { return UIWidgetType::Text; }
    const char*  typeName() const override { return "Text"; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UIText>(*this); }

    std::vector<UIPropDesc> properties() const override
    { return { { "Text", UIPropType::String },
               { "FontSize", UIPropType::Float, 4.0f, 200.0f },
               { "Color", UIPropType::Color } }; }
    UIPropValue getProp(const std::string& n) const override
    {
        if (n == "Text")     return UIPropValue::ofString(text);
        if (n == "FontSize") return UIPropValue::ofFloat(fontSize);
        if (n == "Color")    return propColor(color);
        return {};
    }
    void setProp(const std::string& n, const UIPropValue& v) override
    {
        if (n == "Text")     text = v.s;
        else if (n == "FontSize") fontSize = v.f;
        else if (n == "Color")    color = asColor(v);
    }

    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

// ── Button ────────────────────────────────────────────────────────────────────
class UIButton final : public UIElement
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
    bool interactive() const override { return true; }
    std::unique_ptr<UIElement> clone() const override
    { return std::make_unique<UIButton>(*this); }

    std::vector<UIPropDesc> properties() const override
    { return { { "Text", UIPropType::String },
               { "FontSize", UIPropType::Float, 4.0f, 200.0f },
               { "Normal Color", UIPropType::Color },
               { "Hovered Color", UIPropType::Color },
               { "Pressed Color", UIPropType::Color },
               { "Text Color", UIPropType::Color } }; }
    UIPropValue getProp(const std::string& n) const override
    {
        if (n == "Text")          return UIPropValue::ofString(text);
        if (n == "FontSize")      return UIPropValue::ofFloat(fontSize);
        if (n == "Normal Color")  return propColor(color);
        if (n == "Hovered Color") return propColor(hoveredColor);
        if (n == "Pressed Color") return propColor(pressedColor);
        if (n == "Text Color")    return propColor(textColor);
        return {};
    }
    void setProp(const std::string& n, const UIPropValue& v) override
    {
        if (n == "Text")               text = v.s;
        else if (n == "FontSize")      fontSize = v.f;
        else if (n == "Normal Color")  color = asColor(v);
        else if (n == "Hovered Color") hoveredColor = asColor(v);
        else if (n == "Pressed Color") pressedColor = asColor(v);
        else if (n == "Text Color")    textColor = asColor(v);
    }
    std::vector<UIEventDesc> events() const override
    { return { { "OnClicked" }, { "OnPressed" }, { "OnReleased" },
               { "OnHovered" }, { "OnUnhovered" } }; }

    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

// ── CheckBox ──────────────────────────────────────────────────────────────────
class UICheckBox final : public UIElement
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

    std::vector<UIPropDesc> properties() const override
    { return { { "Checked", UIPropType::Bool },
               { "Label", UIPropType::String },
               { "FontSize", UIPropType::Float, 4.0f, 200.0f },
               { "Box Color", UIPropType::Color },
               { "Check Color", UIPropType::Color },
               { "Text Color", UIPropType::Color } }; }
    UIPropValue getProp(const std::string& n) const override
    {
        if (n == "Checked")     return UIPropValue::ofBool(checked);
        if (n == "Label")       return UIPropValue::ofString(label);
        if (n == "FontSize")    return UIPropValue::ofFloat(fontSize);
        if (n == "Box Color")   return propColor(boxColor);
        if (n == "Check Color") return propColor(checkColor);
        if (n == "Text Color")  return propColor(textColor);
        return {};
    }
    void setProp(const std::string& n, const UIPropValue& v) override
    {
        if (n == "Checked")          checked = v.b;
        else if (n == "Label")       label = v.s;
        else if (n == "FontSize")    fontSize = v.f;
        else if (n == "Box Color")   boxColor = asColor(v);
        else if (n == "Check Color") checkColor = asColor(v);
        else if (n == "Text Color")  textColor = asColor(v);
    }
    std::vector<UIEventDesc> events() const override
    { return { { "OnCheckChanged", UIPropType::Bool, true },
               { "OnHovered" }, { "OnUnhovered" } }; }

    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

// ── Slider ────────────────────────────────────────────────────────────────────
class UISlider final : public UIElement
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

    std::vector<UIPropDesc> properties() const override
    { return { { "Value", UIPropType::Float },
               { "Min", UIPropType::Float },
               { "Max", UIPropType::Float },
               { "Track Color", UIPropType::Color },
               { "Fill Color", UIPropType::Color },
               { "Handle Color", UIPropType::Color } }; }
    UIPropValue getProp(const std::string& n) const override
    {
        if (n == "Value")        return UIPropValue::ofFloat(value);
        if (n == "Min")          return UIPropValue::ofFloat(minValue);
        if (n == "Max")          return UIPropValue::ofFloat(maxValue);
        if (n == "Track Color")  return propColor(trackColor);
        if (n == "Fill Color")   return propColor(fillColor);
        if (n == "Handle Color") return propColor(handleColor);
        return {};
    }
    void setProp(const std::string& n, const UIPropValue& v) override
    {
        if (n == "Value")             value = v.f;
        else if (n == "Min")          minValue = v.f;
        else if (n == "Max")          maxValue = v.f;
        else if (n == "Track Color")  trackColor = asColor(v);
        else if (n == "Fill Color")   fillColor = asColor(v);
        else if (n == "Handle Color") handleColor = asColor(v);
    }
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
class UIProgressBar final : public UIElement
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

    std::vector<UIPropDesc> properties() const override
    { return { { "Value", UIPropType::Float, 0.0f, 1.0f },
               { "Back Color", UIPropType::Color },
               { "Fill Color", UIPropType::Color } }; }
    UIPropValue getProp(const std::string& n) const override
    {
        if (n == "Value")      return UIPropValue::ofFloat(value);
        if (n == "Back Color") return propColor(backColor);
        if (n == "Fill Color") return propColor(fillColor);
        return {};
    }
    void setProp(const std::string& n, const UIPropValue& v) override
    {
        if (n == "Value")           value = v.f;
        else if (n == "Back Color") backColor = asColor(v);
        else if (n == "Fill Color") fillColor = asColor(v);
    }

    void render(const UIWidgetRect&, const UIElementRenderState&, const HE::UUID&,
                float, std::vector<UIRenderObject>&) const override;
    void writeJson(nlohmann::json&) const override;
    void readJson(const nlohmann::json&) override;
};

// ── TextInput ─────────────────────────────────────────────────────────────────
class UITextInput final : public UIElement
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

    std::vector<UIPropDesc> properties() const override
    { return { { "Text", UIPropType::String },
               { "Placeholder", UIPropType::String },
               { "FontSize", UIPropType::Float, 4.0f, 200.0f },
               { "Back Color", UIPropType::Color },
               { "Text Color", UIPropType::Color } }; }
    UIPropValue getProp(const std::string& n) const override
    {
        if (n == "Text")        return UIPropValue::ofString(text);
        if (n == "Placeholder") return UIPropValue::ofString(placeholder);
        if (n == "FontSize")    return UIPropValue::ofFloat(fontSize);
        if (n == "Back Color")  return propColor(backColor);
        if (n == "Text Color")  return propColor(textColor);
        return {};
    }
    void setProp(const std::string& n, const UIPropValue& v) override
    {
        if (n == "Text")             text = v.s;
        else if (n == "Placeholder") placeholder = v.s;
        else if (n == "FontSize")    fontSize = v.f;
        else if (n == "Back Color")  backColor = asColor(v);
        else if (n == "Text Color")  textColor = asColor(v);
    }
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
class UIComboBox final : public UIElement
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

    std::vector<UIPropDesc> properties() const override
    { return { { "Options", UIPropType::StringList },
               { "Selected Index", UIPropType::Int },
               { "FontSize", UIPropType::Float, 4.0f, 200.0f },
               { "Back Color", UIPropType::Color },
               { "Text Color", UIPropType::Color },
               { "Highlight Color", UIPropType::Color } }; }
    UIPropValue getProp(const std::string& n) const override
    {
        if (n == "Options")         { UIPropValue v; v.type = UIPropType::StringList; v.list = options; return v; }
        if (n == "Selected Index")  return UIPropValue::ofInt(selectedIndex);
        if (n == "FontSize")        return UIPropValue::ofFloat(fontSize);
        if (n == "Back Color")      return propColor(backColor);
        if (n == "Text Color")      return propColor(textColor);
        if (n == "Highlight Color") return propColor(highlightColor);
        return {};
    }
    void setProp(const std::string& n, const UIPropValue& v) override
    {
        if (n == "Options")              options = v.list;
        else if (n == "Selected Index")  selectedIndex = v.i;
        else if (n == "FontSize")        fontSize = v.f;
        else if (n == "Back Color")      backColor = asColor(v);
        else if (n == "Text Color")      textColor = asColor(v);
        else if (n == "Highlight Color") highlightColor = asColor(v);
    }
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

} // namespace HE
