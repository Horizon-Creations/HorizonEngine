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

// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  THESE STRINGS ARE AN ON-DISK FORMAT — uiWidgetTypeFromName reads them   ║
// ║  back, so a saved widget names its element types with them. They must    ║
// ║  stay in step with each subclass's typeName(); the "makeUIElement        ║
// ║  produces the right subclass for every type" test pins the two together. ║
// ╚══════════════════════════════════════════════════════════════════════════╝
const char* uiWidgetTypeName(UIWidgetType t)
{
    static constexpr const char* kNames[] = {
        "Panel", "Image", "Text", "Button", "CheckBox",
        "Slider", "ProgressBar", "TextInput", "ComboBox" };
    static_assert(sizeof(kNames) / sizeof(*kNames) == (size_t)UIWidgetType::COUNT,
                  "uiWidgetTypeName table out of step with UIWidgetType");
    const size_t i = (size_t)t;
    // Out-of-range falls back to Panel, matching makeUIElement's default.
    return i < sizeof(kNames) / sizeof(*kNames) ? kNames[i] : "Panel";
}

UIWidgetType uiWidgetTypeFromName(const std::string& s)
{
    for (UIWidgetType t : uiWidgetTypeRegistry())
        if (s == uiWidgetTypeName(t)) return t;
    return UIWidgetType::Panel;
}

const char* uiCursorName(UICursor c)
{
    switch (c)
    {
        case UICursor::Default:   return "Default";
        case UICursor::Arrow:     return "Arrow";
        case UICursor::Hand:      return "Hand";
        case UICursor::Text:      return "Text";
        case UICursor::Crosshair: return "Crosshair";
        case UICursor::ResizeWE:  return "Resize ⇔";
        case UICursor::ResizeNS:  return "Resize ⇕";
        case UICursor::Move:      return "Move";
        case UICursor::No:        return "No";
        case UICursor::Wait:      return "Wait";
        default:                  return "Default";
    }
}

// ── Property tables (one per widget type) ────────────────────────────────────
// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  THESE NAMES ARE AN ON-DISK FORMAT.                                      ║
// ║                                                                          ║
// ║  A UI Widget asset is serialized with them and HorizonCode graphs         ║
// ║  reference properties by name (Get/Set Property nodes store the string).  ║
// ║  Renaming a row therefore breaks every saved widget and every graph that  ║
// ║  touches that property — silently, since a lookup miss just reads as the  ║
// ║  default value. test_ui_widgets.cpp pins the full name+type list per      ║
// ║  type; if that test fails you changed the format.                         ║
// ╚══════════════════════════════════════════════════════════════════════════╝
// Declaration order is the order the editor's detail panel shows them in.

const UIPropTable& UIPanel::propTable() const
{
    static const UIPropTable t = {
        uiprop::slot<&UIPanel::color>({ "Color", UIPropType::Color }),
    };
    return t;
}

const UIPropTable& UIImage::propTable() const
{
    static const UIPropTable t = {
        uiprop::slot<&UIImage::tint>({ "Tint", UIPropType::Color }),
    };
    return t;
}

const UIPropTable& UIText::propTable() const
{
    static const UIPropTable t = {
        uiprop::slot<&UIText::text>    ({ "Text", UIPropType::String, 0.0f, 0.0f, /*multiline=*/true }),
        uiprop::slot<&UIText::fontSize>({ "FontSize", UIPropType::Float, 4.0f, 200.0f }),
        uiprop::slot<&UIText::color>   ({ "Color", UIPropType::Color }),
        uiprop::slot<&UIText::wordWrap>({ "WordWrap", UIPropType::Bool }),
        uiprop::slot<&UIText::autoSize>({ "AutoSize", UIPropType::Bool }),
        // Not a plain field: the bool the user sees is the 0/1 `align` index.
        uiprop::custom({ "Center", UIPropType::Bool },
            [](const UIElement& e) { return UIPropValue::ofBool(static_cast<const UIText&>(e).align == 1); },
            [](UIElement& e, const UIPropValue& v) { static_cast<UIText&>(e).align = v.b ? 1 : 0; }),
    };
    return t;
}

const UIPropTable& UIButton::propTable() const
{
    static const UIPropTable t = {
        uiprop::slot<&UIButton::text>        ({ "Text", UIPropType::String }),
        uiprop::slot<&UIButton::fontSize>    ({ "FontSize", UIPropType::Float, 4.0f, 200.0f }),
        uiprop::slot<&UIButton::color>       ({ "Normal Color", UIPropType::Color }),
        uiprop::slot<&UIButton::hoveredColor>({ "Hovered Color", UIPropType::Color }),
        uiprop::slot<&UIButton::pressedColor>({ "Pressed Color", UIPropType::Color }),
        uiprop::slot<&UIButton::textColor>   ({ "Text Color", UIPropType::Color }),
    };
    return t;
}

const UIPropTable& UICheckBox::propTable() const
{
    static const UIPropTable t = {
        uiprop::slot<&UICheckBox::checked>   ({ "Checked", UIPropType::Bool }),
        uiprop::slot<&UICheckBox::label>     ({ "Label", UIPropType::String }),
        uiprop::slot<&UICheckBox::fontSize>  ({ "FontSize", UIPropType::Float, 4.0f, 200.0f }),
        uiprop::slot<&UICheckBox::boxColor>  ({ "Box Color", UIPropType::Color }),
        uiprop::slot<&UICheckBox::checkColor>({ "Check Color", UIPropType::Color }),
        uiprop::slot<&UICheckBox::textColor> ({ "Text Color", UIPropType::Color }),
    };
    return t;
}

const UIPropTable& UISlider::propTable() const
{
    static const UIPropTable t = {
        uiprop::slot<&UISlider::value>      ({ "Value", UIPropType::Float }),
        uiprop::slot<&UISlider::minValue>   ({ "Min", UIPropType::Float }),
        uiprop::slot<&UISlider::maxValue>   ({ "Max", UIPropType::Float }),
        uiprop::slot<&UISlider::trackColor> ({ "Track Color", UIPropType::Color }),
        uiprop::slot<&UISlider::fillColor>  ({ "Fill Color", UIPropType::Color }),
        uiprop::slot<&UISlider::handleColor>({ "Handle Color", UIPropType::Color }),
    };
    return t;
}

const UIPropTable& UIProgressBar::propTable() const
{
    static const UIPropTable t = {
        uiprop::slot<&UIProgressBar::value>    ({ "Value", UIPropType::Float, 0.0f, 1.0f }),
        uiprop::slot<&UIProgressBar::backColor>({ "Back Color", UIPropType::Color }),
        uiprop::slot<&UIProgressBar::fillColor>({ "Fill Color", UIPropType::Color }),
    };
    return t;
}

const UIPropTable& UITextInput::propTable() const
{
    static const UIPropTable t = {
        uiprop::slot<&UITextInput::text>       ({ "Text", UIPropType::String }),
        uiprop::slot<&UITextInput::placeholder>({ "Placeholder", UIPropType::String }),
        uiprop::slot<&UITextInput::fontSize>   ({ "FontSize", UIPropType::Float, 4.0f, 200.0f }),
        uiprop::slot<&UITextInput::backColor>  ({ "Back Color", UIPropType::Color }),
        uiprop::slot<&UITextInput::textColor>  ({ "Text Color", UIPropType::Color }),
    };
    return t;
}

const UIPropTable& UIComboBox::propTable() const
{
    static const UIPropTable t = {
        uiprop::slot<&UIComboBox::options>       ({ "Options", UIPropType::StringList }),
        uiprop::slot<&UIComboBox::selectedIndex> ({ "Selected Index", UIPropType::Int }),
        uiprop::slot<&UIComboBox::fontSize>      ({ "FontSize", UIPropType::Float, 4.0f, 200.0f }),
        uiprop::slot<&UIComboBox::backColor>     ({ "Back Color", UIPropType::Color }),
        uiprop::slot<&UIComboBox::textColor>     ({ "Text Color", UIPropType::Color }),
        uiprop::slot<&UIComboBox::highlightColor>({ "Highlight Color", UIPropType::Color }),
    };
    return t;
}

// The three entry points every table serves. A name that is not in the table
// reads as a default-constructed value and writes nowhere — the same silent
// behaviour the per-class if-chains had.

std::vector<UIPropDesc> UIElement::properties() const
{
    const UIPropTable& t = propTable();
    std::vector<UIPropDesc> out;
    out.reserve(t.size());
    for (const UIPropSlot& s : t) out.push_back(s.desc);
    return out;
}

UIPropValue UIElement::getProp(const std::string& name) const
{
    for (const UIPropSlot& s : propTable())
        if (s.desc.name == name) return s.get(*this);
    return {};
}

void UIElement::setProp(const std::string& name, const UIPropValue& v)
{
    for (const UIPropSlot& s : propTable())
        if (s.desc.name == name) { s.set(*this, v); return; }
}

// ── Shared base properties ────────────────────────────────────────────────────
// Get/set of the base fields by name. Kept in one place so allProperties/
// getPropAny/setPropAny stay in step; returns false for non-base names.
namespace
{
bool getBaseProp(const UIElement& e, const std::string& n, UIPropValue& out)
{
    if (n == "Visible")      { out = UIPropValue::ofBool(e.visible);            return true; }
    if (n == "Hit Testable") { out = UIPropValue::ofBool(e.hitTestable);        return true; }
    if (n == "Position")     { out = UIPropValue::ofVec2({ e.posX, e.posY });   return true; }
    if (n == "Size")         { out = UIPropValue::ofVec2({ e.sizeX, e.sizeY }); return true; }
    if (n == "Layer")        { out = UIPropValue::ofInt(e.layer);               return true; }
    if (n == "Hover Cursor") { out = UIPropValue::ofInt((int)e.hoverCursor);    return true; }
    if (n == "Material")     { out = UIPropValue::ofString(e.material);         return true; }
    if (n == "Font")         { out = UIPropValue::ofString(e.font);             return true; }
    return false;
}

bool setBaseProp(UIElement& e, const std::string& n, const UIPropValue& v)
{
    if (n == "Visible")      { e.visible     = v.b; return true; }
    if (n == "Hit Testable") { e.hitTestable = v.b; return true; }
    if (n == "Position")     { e.posX  = v.v2.x; e.posY  = v.v2.y; return true; }
    if (n == "Size")         { e.sizeX = v.v2.x; e.sizeY = v.v2.y; return true; }
    if (n == "Layer")        { e.layer = v.i; return true; }
    if (n == "Hover Cursor")
    {
        const int c = v.i;
        e.hoverCursor = (c >= 0 && c < (int)UICursor::COUNT)
            ? (UICursor)c : UICursor::Default;
        return true;
    }
    if (n == "Material")     { e.material = v.s; return true; }
    if (n == "Font")         { e.font = v.s; return true; }
    return false;
}
} // namespace

std::vector<UIPropDesc> UIElement::allProperties() const
{
    std::vector<UIPropDesc> out = properties();
    out.push_back({ "Visible",      UIPropType::Bool });
    out.push_back({ "Hit Testable", UIPropType::Bool });
    out.push_back({ "Position",     UIPropType::Vec2 });
    out.push_back({ "Size",         UIPropType::Vec2 });
    out.push_back({ "Layer",        UIPropType::Int });
    out.push_back({ "Hover Cursor", UIPropType::Int });
    // Asset slots only where the editor exposes them: Material behind
    // hasMaterialSlot(), Font on text-bearing types (same FontSize heuristic
    // the details panel uses).
    if (hasMaterialSlot())
        out.push_back({ "Material", UIPropType::String });
    for (const UIPropSlot& s : propTable())
        if (s.desc.name == "FontSize")
        {
            out.push_back({ "Font", UIPropType::String });
            break;
        }
    return out;
}

UIPropValue UIElement::getPropAny(const std::string& name) const
{
    UIPropValue v;
    if (getBaseProp(*this, name, v)) return v;
    return getProp(name);
}

void UIElement::setPropAny(const std::string& name, const UIPropValue& v)
{
    if (setBaseProp(*this, name, v)) return;
    setProp(name, v);
}

// ── Render helpers ───────────────────────────────────────────────────────────

namespace
{
    void quad(std::vector<UIRenderObject>& out, float x, float y, float w, float h,
              const glm::vec4& color, const HE::UUID& mat = {}, float cornerRadius = 0.0f)
    {
        UIRenderObject ro;
        ro.position = { x, y };
        ro.size     = { w, h };
        ro.color    = color;
        ro.materialAssetId = mat;
        ro.type     = 0;
        ro.cornerRadius = cornerRadius;
        out.push_back(std::move(ro));
    }
    // Corner radius that matches the editor preview: a small rounding clamped to
    // never exceed half the smaller side.
    float roundedR(float w, float h, float r) { return std::min(r, 0.5f * std::min(w, h)); }
}

// Text emit that honors the element's Font asset (fontAtlasKey) when set, else
// the shared default font.
static void emitTextL(const UIElement& e, const std::string& text, const glm::vec2& pos,
                      const glm::vec2& size, float sizePx, const glm::vec4& color,
                      const HE::UITextLayout& opts, std::vector<UIRenderObject>& out)
{
    if (e.fontAtlasKey != 0)
        if (const HE::BakedUIFont* f = HE::UIFontCache::find(e.fontAtlasKey))
        {
            HE::emitUITextGlyphs(*f, e.fontAtlasKey, text, pos, size, sizePx, color, 0, opts, out);
            return;
        }
    HE::emitUITextGlyphs(HE::sharedUIFont(), 0, text, pos, size, sizePx, color, 0, opts, out);
}

static void emitText(const UIElement& e, const std::string& text, const glm::vec2& pos,
                     const glm::vec2& size, float sizePx, const glm::vec4& color,
                     bool centerH, std::vector<UIRenderObject>& out)
{
    HE::UITextLayout opts; opts.centerH = centerH;
    emitTextL(e, text, pos, size, sizePx, color, opts, out);
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

// Resize the element to its own content. Height always tracks the line count ×
// font size; the width tracks the widest line unless WordWrap owns it (then the
// authored width IS the wrap column). A small padding keeps descenders and the
// last glyph's side bearing off the edge.
void UIText::applyAutoSize()
{
    if (!autoSize) return;
    HE::UITextLayout opts;
    opts.centerH = align == 1;
    opts.wrap    = wordWrap;
    const float wrapW = wordWrap ? std::max(1.0f, sizeX) : 0.0f;
    const HE::BakedUIFont* f = HE::UIFontCache::find(fontAtlasKey);
    const glm::vec2 m = f ? HE::measureUIText(*f, text, fontSize, wrapW, opts)
                          : HE::measureUIText(text, fontSize, wrapW, opts);
    if (!wordWrap) sizeX = m.x + fontSize * 0.25f;
    sizeY = m.y + fontSize * 0.35f;
}

void UIText::render(const UIWidgetRect& px, const UIElementRenderState&,
                    const HE::UUID&, float pxScaleY, std::vector<UIRenderObject>& out) const
{
    HE::UITextLayout opts;
    opts.centerH = align == 1;
    opts.wrap    = wordWrap;
    emitTextL(*this, text, { px.x, px.y }, { px.w, px.h }, fontSize * pxScaleY,
              color, opts, out);
}

// ── Button ───────────────────────────────────────────────────────────────────

void UIButton::render(const UIWidgetRect& px, const UIElementRenderState& st,
                      const HE::UUID& mat, float pxScaleY, std::vector<UIRenderObject>& out) const
{
    glm::vec4 c = color;
    if (st.hovered) c = hoveredColor;
    if (st.pressed) c = pressedColor;
    quad(out, px.x, px.y, px.w, px.h, c, mat, roundedR(px.w, px.h, 6.0f));
    if (!text.empty())
        emitText(*this, text, { px.x, px.y }, { px.w, px.h }, fontSize * pxScaleY,
                 textColor, /*centerH=*/true, out);
}

// ── CheckBox ─────────────────────────────────────────────────────────────────

void UICheckBox::render(const UIWidgetRect& px, const UIElementRenderState& st,
                        const HE::UUID&, float pxScaleY, std::vector<UIRenderObject>& out) const
{
    const float box = px.h;
    glm::vec4 bc = boxColor;
    if (st.hovered) bc = glm::vec4(glm::vec3(boxColor) * 1.3f, boxColor.a);
    quad(out, px.x, px.y, box, box, bc, {}, roundedR(box, box, 4.0f));
    if (checked)
    {
        const float inset = box * 0.22f;
        const float cb = box - 2 * inset;
        quad(out, px.x + inset, px.y + inset, cb, cb, checkColor, {}, roundedR(cb, cb, 2.0f));
    }
    const float lx = px.x + box + 8.0f;
    emitText(*this, label, { lx, px.y }, { px.w - box - 8.0f, px.h },
             fontSize * pxScaleY, textColor, /*centerH=*/false, out);
}

// ── Slider ───────────────────────────────────────────────────────────────────

void UISlider::render(const UIWidgetRect& px, const UIElementRenderState& st,
                      const HE::UUID&, float, std::vector<UIRenderObject>& out) const
{
    const float t = normalized();
    const float trackH = std::max(4.0f, px.h * 0.35f);
    const float trackY = px.y + (px.h - trackH) * 0.5f;
    quad(out, px.x, trackY, px.w,     trackH, trackColor, {}, trackH * 0.5f); // pill track
    quad(out, px.x, trackY, px.w * t, trackH, fillColor,  {}, roundedR(px.w * t, trackH, trackH * 0.5f));
    // Handle: a circle centered on the fill position (radius = half its size).
    const float hw = px.h * 0.9f;
    const float hx = px.x + px.w * t - hw * 0.5f;
    glm::vec4 hc = handleColor;
    if (st.pressed) hc = glm::vec4(glm::vec3(handleColor) * 0.85f, handleColor.a);
    else if (st.hovered) hc = glm::vec4(glm::min(glm::vec3(handleColor) * 1.1f, glm::vec3(1.0f)), handleColor.a);
    quad(out, hx, px.y + (px.h - hw) * 0.5f, hw, hw, hc, {}, hw * 0.5f);
}

// ── ProgressBar ──────────────────────────────────────────────────────────────

void UIProgressBar::render(const UIWidgetRect& px, const UIElementRenderState&,
                           const HE::UUID&, float, std::vector<UIRenderObject>& out) const
{
    const float t = std::clamp(value, 0.0f, 1.0f);
    quad(out, px.x, px.y, px.w,     px.h, backColor, {}, roundedR(px.w, px.h, 4.0f));
    quad(out, px.x, px.y, px.w * t, px.h, fillColor, {}, roundedR(px.w * t, px.h, 4.0f));
}

// ── TextInput ────────────────────────────────────────────────────────────────

void UITextInput::render(const UIWidgetRect& px, const UIElementRenderState& st,
                         const HE::UUID&, float pxScaleY, std::vector<UIRenderObject>& out) const
{
    glm::vec4 bg = backColor;
    if (st.focused) bg = glm::vec4(glm::min(glm::vec3(backColor) + 0.06f, glm::vec3(1.0f)), backColor.a);
    quad(out, px.x, px.y, px.w, px.h, bg, {}, roundedR(px.w, px.h, 4.0f));
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
        emitText(*this, placeholder, tp, ts, fontSize * pxScaleY,
                 glm::vec4(glm::vec3(textColor) * 0.5f, textColor.a * 0.7f), false, out);
    else
    {
        std::string shown = text;
        if (st.focused) shown += "|"; // caret
        emitText(*this, shown, tp, ts, fontSize * pxScaleY, textColor, false, out);
    }
}

// ── ComboBox ─────────────────────────────────────────────────────────────────

void UIComboBox::render(const UIWidgetRect& px, const UIElementRenderState& st,
                        const HE::UUID&, float pxScaleY, std::vector<UIRenderObject>& out) const
{
    glm::vec4 bg = st.hovered ? highlightColor : backColor;
    quad(out, px.x, px.y, px.w, px.h, bg, {}, roundedR(px.w, px.h, 4.0f));
    const float pad = 6.0f;
    emitText(*this, currentText(), { px.x + pad, px.y }, { px.w - px.h - pad, px.h },
             fontSize * pxScaleY, textColor, false, out);
    // Dropdown indicator ("v") in the right box.
    emitText(*this, "v", { px.x + px.w - px.h, px.y }, { px.h, px.h },
             fontSize * pxScaleY, textColor, true, out);
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
{ j["text"] = text; j["fontSize"] = fontSize; j["color"] = colJson(color);
  j["wordWrap"] = wordWrap; j["autoSize"] = autoSize; j["align"] = align; }
void UIText::readJson(const nlohmann::json& j)
{ text = j.value("text", text); fontSize = j.value("fontSize", fontSize);
  color = colFrom(j.value("color", nlohmann::json()), color);
  wordWrap = j.value("wordWrap", wordWrap);
  // Widgets authored before auto-size keep their hand-set box: defaulting them
  // to true would resize every existing label on load.
  autoSize = j.value("autoSize", false);
  align    = j.value("align", align); }

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
