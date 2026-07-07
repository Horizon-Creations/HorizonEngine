#include <UIWidget/UIElements.h>
#include <Renderer/UIFont.h>
#include <nlohmann/json.hpp>
#include <algorithm>

namespace HE {

// ── Factory / registry ───────────────────────────────────────────────────────

std::unique_ptr<UIElement> makeUIElement(UIWidgetType t)
{
    switch (t)
    {
        case UIWidgetType::Panel:       return std::make_unique<UIPanel>();
        case UIWidgetType::Image:       return std::make_unique<UIImage>();
        case UIWidgetType::Text:        return std::make_unique<UIText>();
        case UIWidgetType::Button:      return std::make_unique<UIButton>();
        case UIWidgetType::CheckBox:    return std::make_unique<UICheckBox>();
        case UIWidgetType::Slider:      return std::make_unique<UISlider>();
        case UIWidgetType::ProgressBar: return std::make_unique<UIProgressBar>();
        case UIWidgetType::TextInput:   return std::make_unique<UITextInput>();
        case UIWidgetType::ComboBox:    return std::make_unique<UIComboBox>();
        default:                        return std::make_unique<UIPanel>();
    }
}

const std::vector<UIWidgetType>& uiWidgetTypeRegistry()
{
    static const std::vector<UIWidgetType> kAll = {
        UIWidgetType::Panel, UIWidgetType::Image, UIWidgetType::Text,
        UIWidgetType::Button, UIWidgetType::CheckBox, UIWidgetType::Slider,
        UIWidgetType::ProgressBar, UIWidgetType::TextInput, UIWidgetType::ComboBox };
    return kAll;
}

const char* uiWidgetTypeName(UIWidgetType t)
{
    if (auto e = makeUIElement(t)) return e->typeName();
    return "Panel";
}

UIWidgetType uiWidgetTypeFromName(const std::string& s)
{
    for (UIWidgetType t : uiWidgetTypeRegistry())
        if (s == uiWidgetTypeName(t)) return t;
    return UIWidgetType::Panel;
}

// ── Render helpers ───────────────────────────────────────────────────────────

namespace
{
    void quad(std::vector<UIRenderObject>& out, float x, float y, float w, float h,
              const glm::vec4& color, const HE::UUID& mat = {})
    {
        UIRenderObject ro;
        ro.position = { x, y };
        ro.size     = { w, h };
        ro.color    = color;
        ro.materialAssetId = mat;
        ro.type     = 0;
        out.push_back(std::move(ro));
    }
}

// ── Panel / Image ────────────────────────────────────────────────────────────

void UIPanel::render(const UIWidgetRect& px, const UIElementRenderState&,
                     const HE::UUID& mat, float, std::vector<UIRenderObject>& out) const
{
    quad(out, px.x, px.y, px.w, px.h, color, mat);
}

void UIImage::render(const UIWidgetRect& px, const UIElementRenderState&,
                     const HE::UUID& mat, float, std::vector<UIRenderObject>& out) const
{
    quad(out, px.x, px.y, px.w, px.h, tint, mat);
}

// ── Text ─────────────────────────────────────────────────────────────────────

void UIText::render(const UIWidgetRect& px, const UIElementRenderState&,
                    const HE::UUID&, float pxScaleY, std::vector<UIRenderObject>& out) const
{
    HE::emitUITextGlyphs(text, { px.x, px.y }, { px.w, px.h }, fontSize * pxScaleY,
                         color, 0, /*centerH=*/false, out);
}

// ── Button ───────────────────────────────────────────────────────────────────

void UIButton::render(const UIWidgetRect& px, const UIElementRenderState& st,
                      const HE::UUID& mat, float pxScaleY, std::vector<UIRenderObject>& out) const
{
    glm::vec4 c = color;
    if (st.hovered) c = hoveredColor;
    if (st.pressed) c = pressedColor;
    quad(out, px.x, px.y, px.w, px.h, c, mat);
    if (!text.empty())
        HE::emitUITextGlyphs(text, { px.x, px.y }, { px.w, px.h }, fontSize * pxScaleY,
                             textColor, 0, /*centerH=*/true, out);
}

// ── CheckBox ─────────────────────────────────────────────────────────────────

void UICheckBox::render(const UIWidgetRect& px, const UIElementRenderState& st,
                        const HE::UUID&, float pxScaleY, std::vector<UIRenderObject>& out) const
{
    const float box = px.h;
    glm::vec4 bc = boxColor;
    if (st.hovered) bc = glm::vec4(glm::vec3(boxColor) * 1.3f, boxColor.a);
    quad(out, px.x, px.y, box, box, bc);
    if (checked)
    {
        const float inset = box * 0.22f;
        quad(out, px.x + inset, px.y + inset, box - 2 * inset, box - 2 * inset, checkColor);
    }
    const float lx = px.x + box + 8.0f;
    HE::emitUITextGlyphs(label, { lx, px.y }, { px.w - box - 8.0f, px.h },
                         fontSize * pxScaleY, textColor, 0, /*centerH=*/false, out);
}

// ── Slider ───────────────────────────────────────────────────────────────────

void UISlider::render(const UIWidgetRect& px, const UIElementRenderState& st,
                      const HE::UUID&, float, std::vector<UIRenderObject>& out) const
{
    const float t = normalized();
    const float trackH = std::max(4.0f, px.h * 0.35f);
    const float trackY = px.y + (px.h - trackH) * 0.5f;
    quad(out, px.x, trackY, px.w, trackH, trackColor);
    quad(out, px.x, trackY, px.w * t, trackH, fillColor);
    // Handle: a square centered on the fill position.
    const float hw = px.h * 0.9f;
    const float hx = px.x + px.w * t - hw * 0.5f;
    glm::vec4 hc = handleColor;
    if (st.pressed) hc = glm::vec4(glm::vec3(handleColor) * 0.85f, handleColor.a);
    else if (st.hovered) hc = glm::vec4(glm::min(glm::vec3(handleColor) * 1.1f, glm::vec3(1.0f)), handleColor.a);
    quad(out, hx, px.y + (px.h - hw) * 0.5f, hw, hw, hc);
}

// ── ProgressBar ──────────────────────────────────────────────────────────────

void UIProgressBar::render(const UIWidgetRect& px, const UIElementRenderState&,
                           const HE::UUID&, float, std::vector<UIRenderObject>& out) const
{
    const float t = std::clamp(value, 0.0f, 1.0f);
    quad(out, px.x, px.y, px.w, px.h, backColor);
    quad(out, px.x, px.y, px.w * t, px.h, fillColor);
}

// ── TextInput ────────────────────────────────────────────────────────────────

void UITextInput::render(const UIWidgetRect& px, const UIElementRenderState& st,
                         const HE::UUID&, float pxScaleY, std::vector<UIRenderObject>& out) const
{
    glm::vec4 bg = backColor;
    if (st.focused) bg = glm::vec4(glm::min(glm::vec3(backColor) + 0.06f, glm::vec3(1.0f)), backColor.a);
    quad(out, px.x, px.y, px.w, px.h, bg);
    // Thin focus border (four edge quads).
    if (st.focused)
    {
        const glm::vec4 b(0.35f, 0.55f, 0.90f, 1.0f);
        quad(out, px.x, px.y, px.w, 2.0f, b);
        quad(out, px.x, px.y + px.h - 2.0f, px.w, 2.0f, b);
        quad(out, px.x, px.y, 2.0f, px.h, b);
        quad(out, px.x + px.w - 2.0f, px.y, 2.0f, px.h, b);
    }
    const float pad = 6.0f;
    const glm::vec2 tp{ px.x + pad, px.y };
    const glm::vec2 ts{ px.w - 2 * pad, px.h };
    if (text.empty() && !st.focused)
        HE::emitUITextGlyphs(placeholder, tp, ts, fontSize * pxScaleY,
                             glm::vec4(glm::vec3(textColor) * 0.5f, textColor.a * 0.7f),
                             0, false, out);
    else
    {
        std::string shown = text;
        if (st.focused) shown += "|"; // caret
        HE::emitUITextGlyphs(shown, tp, ts, fontSize * pxScaleY, textColor, 0, false, out);
    }
}

// ── ComboBox ─────────────────────────────────────────────────────────────────

void UIComboBox::render(const UIWidgetRect& px, const UIElementRenderState& st,
                        const HE::UUID&, float pxScaleY, std::vector<UIRenderObject>& out) const
{
    glm::vec4 bg = st.hovered ? highlightColor : backColor;
    quad(out, px.x, px.y, px.w, px.h, bg);
    const float pad = 6.0f;
    HE::emitUITextGlyphs(currentText(), { px.x + pad, px.y }, { px.w - px.h - pad, px.h },
                         fontSize * pxScaleY, textColor, 0, false, out);
    // Dropdown indicator ("v") in the right box.
    HE::emitUITextGlyphs("v", { px.x + px.w - px.h, px.y }, { px.h, px.h },
                         fontSize * pxScaleY, textColor, 0, true, out);
}

// ── JSON (type-specific fields; base fields handled by the tree serializer) ───

namespace
{
    nlohmann::json colJson(const glm::vec4& c) { return { c.x, c.y, c.z, c.w }; }
    glm::vec4 colFrom(const nlohmann::json& j, const glm::vec4& def)
    {
        if (!j.is_array() || j.size() < 4) return def;
        return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>() };
    }
}

void UIPanel::writeJson(nlohmann::json& j) const { j["color"] = colJson(color); }
void UIPanel::readJson(const nlohmann::json& j)  { color = colFrom(j.value("color", nlohmann::json()), color); }

void UIImage::writeJson(nlohmann::json& j) const { j["tint"] = colJson(tint); }
void UIImage::readJson(const nlohmann::json& j)  { tint = colFrom(j.value("tint", nlohmann::json()), tint); }

void UIText::writeJson(nlohmann::json& j) const
{ j["text"] = text; j["fontSize"] = fontSize; j["color"] = colJson(color); }
void UIText::readJson(const nlohmann::json& j)
{ text = j.value("text", text); fontSize = j.value("fontSize", fontSize);
  color = colFrom(j.value("color", nlohmann::json()), color); }

void UIButton::writeJson(nlohmann::json& j) const
{
    j["text"] = text; j["fontSize"] = fontSize;
    j["color"] = colJson(color); j["hoveredColor"] = colJson(hoveredColor);
    j["pressedColor"] = colJson(pressedColor); j["textColor"] = colJson(textColor);
}
void UIButton::readJson(const nlohmann::json& j)
{
    text = j.value("text", text); fontSize = j.value("fontSize", fontSize);
    color = colFrom(j.value("color", nlohmann::json()), color);
    hoveredColor = colFrom(j.value("hoveredColor", nlohmann::json()), hoveredColor);
    pressedColor = colFrom(j.value("pressedColor", nlohmann::json()), pressedColor);
    textColor = colFrom(j.value("textColor", nlohmann::json()), textColor);
}

void UICheckBox::writeJson(nlohmann::json& j) const
{
    j["checked"] = checked; j["label"] = label; j["fontSize"] = fontSize;
    j["boxColor"] = colJson(boxColor); j["checkColor"] = colJson(checkColor);
    j["textColor"] = colJson(textColor);
}
void UICheckBox::readJson(const nlohmann::json& j)
{
    checked = j.value("checked", checked); label = j.value("label", label);
    fontSize = j.value("fontSize", fontSize);
    boxColor = colFrom(j.value("boxColor", nlohmann::json()), boxColor);
    checkColor = colFrom(j.value("checkColor", nlohmann::json()), checkColor);
    textColor = colFrom(j.value("textColor", nlohmann::json()), textColor);
}

void UISlider::writeJson(nlohmann::json& j) const
{
    j["value"] = value; j["min"] = minValue; j["max"] = maxValue;
    j["trackColor"] = colJson(trackColor); j["fillColor"] = colJson(fillColor);
    j["handleColor"] = colJson(handleColor);
}
void UISlider::readJson(const nlohmann::json& j)
{
    value = j.value("value", value); minValue = j.value("min", minValue);
    maxValue = j.value("max", maxValue);
    trackColor = colFrom(j.value("trackColor", nlohmann::json()), trackColor);
    fillColor = colFrom(j.value("fillColor", nlohmann::json()), fillColor);
    handleColor = colFrom(j.value("handleColor", nlohmann::json()), handleColor);
}

void UIProgressBar::writeJson(nlohmann::json& j) const
{
    j["value"] = value; j["backColor"] = colJson(backColor); j["fillColor"] = colJson(fillColor);
}
void UIProgressBar::readJson(const nlohmann::json& j)
{
    value = j.value("value", value);
    backColor = colFrom(j.value("backColor", nlohmann::json()), backColor);
    fillColor = colFrom(j.value("fillColor", nlohmann::json()), fillColor);
}

void UITextInput::writeJson(nlohmann::json& j) const
{
    j["text"] = text; j["placeholder"] = placeholder; j["fontSize"] = fontSize;
    j["backColor"] = colJson(backColor); j["textColor"] = colJson(textColor);
}
void UITextInput::readJson(const nlohmann::json& j)
{
    text = j.value("text", text); placeholder = j.value("placeholder", placeholder);
    fontSize = j.value("fontSize", fontSize);
    backColor = colFrom(j.value("backColor", nlohmann::json()), backColor);
    textColor = colFrom(j.value("textColor", nlohmann::json()), textColor);
}

void UIComboBox::writeJson(nlohmann::json& j) const
{
    j["options"] = options; j["selectedIndex"] = selectedIndex; j["fontSize"] = fontSize;
    j["backColor"] = colJson(backColor); j["textColor"] = colJson(textColor);
    j["highlightColor"] = colJson(highlightColor);
}
void UIComboBox::readJson(const nlohmann::json& j)
{
    if (j.contains("options") && j["options"].is_array())
        options = j["options"].get<std::vector<std::string>>();
    selectedIndex = j.value("selectedIndex", selectedIndex);
    fontSize = j.value("fontSize", fontSize);
    backColor = colFrom(j.value("backColor", nlohmann::json()), backColor);
    textColor = colFrom(j.value("textColor", nlohmann::json()), textColor);
    highlightColor = colFrom(j.value("highlightColor", nlohmann::json()), highlightColor);
}

} // namespace HE
