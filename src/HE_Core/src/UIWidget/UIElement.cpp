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
        case UIWidgetType::VerticalBox:   return std::make_unique<UIVerticalBox>();
        case UIWidgetType::HorizontalBox: return std::make_unique<UIHorizontalBox>();
        case UIWidgetType::ScrollBox:     return std::make_unique<UIScrollBox>();
        case UIWidgetType::WidgetRef:     return std::make_unique<UIWidgetRef>();
        case UIWidgetType::Spacer:        return std::make_unique<UISpacer>();
        default:                        return std::make_unique<UIPanel>();
    }
}

const std::vector<UIWidgetType>& uiWidgetTypeRegistry()
{
    static const std::vector<UIWidgetType> kAll = {
        UIWidgetType::Panel, UIWidgetType::Image, UIWidgetType::Text,
        UIWidgetType::Button, UIWidgetType::CheckBox, UIWidgetType::Slider,
        UIWidgetType::ProgressBar, UIWidgetType::TextInput, UIWidgetType::ComboBox,
        UIWidgetType::VerticalBox, UIWidgetType::HorizontalBox,
        UIWidgetType::ScrollBox, UIWidgetType::WidgetRef, UIWidgetType::Spacer };
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
        "Slider", "ProgressBar", "TextInput", "ComboBox",
        "VerticalBox", "HorizontalBox", "ScrollBox", "WidgetRef", "Spacer" };
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

// ── UTF-8 cursor movement ────────────────────────────────────────────────────
namespace
{
    // A continuation byte is 10xxxxxx: never a character boundary.
    bool isUtf8Cont(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }
}

size_t uiUtf8Prev(const std::string& s, size_t i)
{
    if (i == 0) return 0;
    if (i > s.size()) i = s.size();
    --i;
    while (i > 0 && isUtf8Cont(s[i])) --i;
    return i;
}

size_t uiUtf8Next(const std::string& s, size_t i)
{
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && isUtf8Cont(s[i])) ++i;
    return i;
}

size_t uiUtf8Clamp(const std::string& s, size_t i)
{
    if (i >= s.size()) return s.size();
    while (i > 0 && isUtf8Cont(s[i])) --i;
    return i;
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

const UIPropTable& UIWidgetRef::propTable() const
{
    static const UIPropTable t = {
        uiprop::slot<&UIWidgetRef::widgetPath>({ "Widget", UIPropType::String }),
    };
    return t;
}

void UIWidgetRef::writeJson(nlohmann::json& j) const { j["widget"] = widgetPath; }
void UIWidgetRef::readJson(const nlohmann::json& j)  { widgetPath = j.value("widget", widgetPath); }

// Nothing of its own — a Spacer is its rect and nothing else.
const UIPropTable& UISpacer::propTable() const
{
    static const UIPropTable t = {};
    return t;
}

const UIPropTable& UIBoxBase::propTable() const
{
    static const UIPropTable t = {
        uiprop::slot<&UIBoxBase::padding>({ "Padding", UIPropType::Float, 0.0f, 200.0f }),
        uiprop::slot<&UIBoxBase::spacing>({ "Spacing", UIPropType::Float, 0.0f, 200.0f }),
        uiprop::slot<&UIBoxBase::sizeToContent>({ "Size To Content", UIPropType::Bool }),
        uiprop::slot<&UIBoxBase::minSizeX>({ "Min Width",  UIPropType::Float }),
        uiprop::slot<&UIBoxBase::minSizeY>({ "Min Height", UIPropType::Float }),
    };
    return t;
}

const UIPropTable& UIScrollBox::propTable() const
{
    // Padding and Spacing keep the names the slot algorithm reads by; the two
    // extra rows are this type's own.
    static const UIPropTable t = {
        uiprop::slot<&UIScrollBox::padding>({ "Padding", UIPropType::Float, 0.0f, 200.0f }),
        uiprop::slot<&UIScrollBox::spacing>({ "Spacing", UIPropType::Float, 0.0f, 200.0f }),
        uiprop::slot<&UIScrollBox::sizeToContent>({ "Size To Content", UIPropType::Bool }),
        uiprop::slot<&UIScrollBox::minSizeX>({ "Min Width",  UIPropType::Float }),
        uiprop::slot<&UIScrollBox::minSizeY>({ "Min Height", UIPropType::Float }),
        uiprop::slot<&UIScrollBox::barWidth>({ "Bar Width", UIPropType::Float, 0.0f, 40.0f }),
        uiprop::slot<&UIScrollBox::barColor>({ "Bar Color", UIPropType::Color }),
    };
    return t;
}

const UIPropTable& UIImage::propTable() const
{
    static const UIPropTable t = {
        uiprop::slot<&UIImage::tint>({ "Tint", UIPropType::Color }),
        uiprop::slot<&UIImage::sliceLeft>  ({ "Slice Left",   UIPropType::Float }),
        uiprop::slot<&UIImage::sliceTop>   ({ "Slice Top",    UIPropType::Float }),
        uiprop::slot<&UIImage::sliceRight> ({ "Slice Right",  UIPropType::Float }),
        uiprop::slot<&UIImage::sliceBottom>({ "Slice Bottom", UIPropType::Float }),
        uiprop::slot<&UIImage::sliceFillCentre>({ "Slice Fill Centre", UIPropType::Bool }),
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
        // 0/1/2 each. The details panel draws these two as ONE 3×3 grid (see
        // UIEditorPanel) rather than as two number fields — the first attribute
        // that needed a hand-built editor, because "which of nine positions" is
        // not a number anybody wants to type.
        uiprop::slot<&UIText::alignH>({ "Align H", UIPropType::Int, 0.0f, 2.0f }),
        uiprop::slot<&UIText::alignV>({ "Align V", UIPropType::Int, 0.0f, 2.0f }),
    };
    return t;
}

const UIPropTable& UIButton::propTable() const
{
    // Three state colours and nothing else of its own: the caption is a child
    // now (see UIButton), and the rest of a button's look — corner radius,
    // border, gradient — are the shared surface properties every surface has.
    static const UIPropTable t = {
        uiprop::slot<&UIButton::color>       ({ "Normal Color", UIPropType::Color }),
        uiprop::slot<&UIButton::hoveredColor>({ "Hovered Color", UIPropType::Color }),
        uiprop::slot<&UIButton::pressedColor>({ "Pressed Color", UIPropType::Color }),
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
    // Caret and selection are runtime state, not properties: they are where the
    // player is, not what the field is.
    static const UIPropTable t = {
        uiprop::slot<&UITextInput::text>       ({ "Text", UIPropType::String }),
        uiprop::slot<&UITextInput::placeholder>({ "Placeholder", UIPropType::String }),
        uiprop::slot<&UITextInput::fontSize>   ({ "FontSize", UIPropType::Float, 4.0f, 200.0f }),
        uiprop::slot<&UITextInput::backColor>  ({ "Back Color", UIPropType::Color }),
        uiprop::slot<&UITextInput::textColor>  ({ "Text Color", UIPropType::Color }),
        uiprop::slot<&UITextInput::selectionColor>({ "Selection Color", UIPropType::Color }),
        uiprop::slot<&UITextInput::maxLength>  ({ "Max Length", UIPropType::Int }),
        uiprop::slot<&UITextInput::password>   ({ "Password", UIPropType::Bool }),
        uiprop::slot<&UITextInput::editable>   ({ "Editable", UIPropType::Bool }),
        uiprop::slot<&UITextInput::selectable> ({ "Selectable", UIPropType::Bool }),
        // 0 anything, 1 whole numbers, 2 decimals, 3 the characters in Allowed
        // Characters. See UITextInput::Filter.
        uiprop::slot<&UITextInput::inputFilter>({ "Input Filter", UIPropType::Int, 0.0f, 3.0f }),
        uiprop::slot<&UITextInput::allowedChars>({ "Allowed Characters", UIPropType::String }),
    };
    return t;
}

// One character at a time, judged against what the field already holds: "-" is
// only a minus sign in front, and a second "." is not a decimal point.
bool UITextInput::acceptsCharacter(const std::string& ch, size_t atByte) const
{
    if (ch.empty()) return false;
    switch (inputFilter)
    {
    case FilterCustom:
        // No list = no rule (see allowedChars).
        if (allowedChars.empty()) return true;
        return allowedChars.find(ch) != std::string::npos;

    case FilterInteger:
    case FilterDecimal:
    {
        // Multi-byte characters are never digits or signs.
        if (ch.size() != 1) return false;
        const char c = ch[0];
        if (c >= '0' && c <= '9') return true;
        if (c == '-')
        {
            // Only as the very first character, and only once. `atByte` is where
            // it would land, so this also refuses a minus typed into the middle.
            return atByte == 0 && text.find('-') == std::string::npos;
        }
        if (c == '.' && inputFilter == FilterDecimal)
        {
            // One point, and never before the sign.
            if (text.find('.') != std::string::npos) return false;
            return !(atByte == 0 && !text.empty() && text[0] == '-');
        }
        return false;
    }

    case FilterAny:
    default:
        return true;
    }
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
    if (n == "Clip Children"){ out = UIPropValue::ofBool(e.clipChildren);       return true; }
    if (n == "Enabled")      { out = UIPropValue::ofBool(e.enabled);            return true; }
    if (n == "Render Opacity"){out = UIPropValue::ofFloat(e.renderOpacity);     return true; }
    if (n == "Slot Fill")    { out = UIPropValue::ofFloat(e.slotFill);          return true; }
    if (n == "Rotation")     { out = UIPropValue::ofFloat(e.rotation);          return true; }
    if (n == "Position")     { out = UIPropValue::ofVec2({ e.posX, e.posY });   return true; }
    if (n == "Size")         { out = UIPropValue::ofVec2({ e.sizeX, e.sizeY }); return true; }
    if (n == "Layer")        { out = UIPropValue::ofInt(e.layer);               return true; }
    if (n == "Hover Cursor") { out = UIPropValue::ofInt((int)e.hoverCursor);    return true; }
    if (n == "Material")     { out = UIPropValue::ofString(e.material);         return true; }
    if (n == "Texture")      { out = UIPropValue::ofString(e.texture);          return true; }
    if (n == "Font")         { out = UIPropValue::ofString(e.font);             return true; }
    // "Corner Radius" is the whole rounding as ONE number, which is what it has
    // always been and what nearly every element wants. Reading it back gives the
    // top-left corner, so a graph that set it reads its own value; the four
    // named rows below are for the elements that round their corners differently.
    if (n == "Corner Radius"){ out = UIPropValue::ofFloat(e.cornerRadius.x);    return true; }
    if (n == "Corner TL")    { out = UIPropValue::ofFloat(e.cornerRadius.x);    return true; }
    if (n == "Corner TR")    { out = UIPropValue::ofFloat(e.cornerRadius.y);    return true; }
    if (n == "Corner BR")    { out = UIPropValue::ofFloat(e.cornerRadius.z);    return true; }
    if (n == "Corner BL")    { out = UIPropValue::ofFloat(e.cornerRadius.w);    return true; }
    if (n == "Border Width") { out = UIPropValue::ofFloat(e.borderWidth);       return true; }
    if (n == "Border Color") { out = UIPropValue::ofColor(e.borderColor);       return true; }
    if (n == "Gradient")     { out = UIPropValue::ofBool(e.gradient);           return true; }
    if (n == "Gradient Color"){out = UIPropValue::ofColor(e.gradientColor);     return true; }
    if (n == "Gradient Angle"){out = UIPropValue::ofFloat(e.gradientAngle);     return true; }
    if (n == "Gradient Shape"){out = UIPropValue::ofInt(e.gradientShape);       return true; }
    return false;
}

bool setBaseProp(UIElement& e, const std::string& n, const UIPropValue& v)
{
    if (n == "Visible")      { e.visible     = v.b; return true; }
    if (n == "Hit Testable") { e.hitTestable = v.b; return true; }
    if (n == "Clip Children"){ e.clipChildren = v.b; return true; }
    if (n == "Enabled")      { e.enabled = v.b; return true; }
    if (n == "Render Opacity"){ e.renderOpacity = v.f < 0.0f ? 0.0f : (v.f > 1.0f ? 1.0f : v.f); return true; }
    if (n == "Slot Fill")    { e.slotFill = v.f < 0.0f ? 0.0f : v.f; return true; }
    if (n == "Rotation")     { e.rotation = v.f; return true; }
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
    // The path is the authored value; the resolved id is transient and the
    // runtime re-resolves it when this changes (WidgetManager watches both).
    if (n == "Texture")      { e.texture = v.s; return true; }
    if (n == "Font")         { e.font = v.s; return true; }
    // Writing the single name rounds ALL FOUR corners — the property a script or
    // a theme sets when it means "round this thing".
    if (n == "Corner Radius"){ e.cornerRadius = glm::vec4(std::max(0.0f, v.f)); return true; }
    if (n == "Corner TL")    { e.cornerRadius.x = std::max(0.0f, v.f); return true; }
    if (n == "Corner TR")    { e.cornerRadius.y = std::max(0.0f, v.f); return true; }
    if (n == "Corner BR")    { e.cornerRadius.z = std::max(0.0f, v.f); return true; }
    if (n == "Corner BL")    { e.cornerRadius.w = std::max(0.0f, v.f); return true; }
    if (n == "Border Width") { e.borderWidth = v.f < 0.0f ? 0.0f : v.f; return true; }
    if (n == "Border Color") { e.borderColor = v.col; return true; }
    if (n == "Gradient")     { e.gradient = v.b; return true; }
    if (n == "Gradient Color"){ e.gradientColor = v.col; return true; }
    if (n == "Gradient Angle"){ e.gradientAngle = v.f; return true; }
    if (n == "Gradient Shape"){ e.gradientShape = (v.i == 1) ? 1 : 0; return true; }
    return false;
}
} // namespace

std::vector<UIPropDesc> UIElement::allProperties() const
{
    std::vector<UIPropDesc> out = properties();
    out.push_back({ "Visible",      UIPropType::Bool });
    out.push_back({ "Hit Testable", UIPropType::Bool });
    out.push_back({ "Clip Children",UIPropType::Bool });
    out.push_back({ "Enabled",      UIPropType::Bool });
    out.push_back({ "Render Opacity", UIPropType::Float, 0.0f, 1.0f });
    out.push_back({ "Slot Fill",    UIPropType::Float });
    out.push_back({ "Rotation",     UIPropType::Float });
    out.push_back({ "Position",     UIPropType::Vec2 });
    out.push_back({ "Size",         UIPropType::Vec2 });
    out.push_back({ "Layer",        UIPropType::Int });
    out.push_back({ "Hover Cursor", UIPropType::Int });
    // Border ("Schicht 0"): a style on the element's own surface. Offered only
    // where there IS a surface — the same test the material slot uses — so a
    // Text label does not grow a border property that outlines nothing.
    if (hasSurfaceStyle())
    {
        out.push_back({ "Corner Radius", UIPropType::Float });
        // The four on top of the one: "Corner Radius" is all of them at once,
        // these address a single corner. Both are real properties a graph can
        // set, which is why they are listed and not just editor state.
        out.push_back({ "Corner TL", UIPropType::Float });
        out.push_back({ "Corner TR", UIPropType::Float });
        out.push_back({ "Corner BR", UIPropType::Float });
        out.push_back({ "Corner BL", UIPropType::Float });
        out.push_back({ "Border Width", UIPropType::Float });
        out.push_back({ "Border Color", UIPropType::Color });
        // Split into three plain properties rather than one gradient object:
        // the designer's property editor is generic over UIPropType, so anything
        // expressed in the existing kinds gets its editor for free.
        out.push_back({ "Gradient",       UIPropType::Bool });
        out.push_back({ "Gradient Color", UIPropType::Color });
        out.push_back({ "Gradient Angle", UIPropType::Float, 0.0f, 360.0f });
        out.push_back({ "Gradient Shape", UIPropType::Int, 0.0f, 1.0f });
    }
    // Asset slots only where the editor exposes them: Material behind
    // hasMaterialSlot(), Font on text-bearing types (same FontSize heuristic
    // the details panel uses).
    if (hasMaterialSlot())
        out.push_back({ "Material", UIPropType::String });
    if (hasTextureSlot())
        out.push_back({ "Texture", UIPropType::String });
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
              const glm::vec4& color, const HE::UUID& mat = {}, float cornerRadius = 0.0f,
              const HE::UUID& tex = {}, const glm::vec2& uv0 = { 0.0f, 0.0f },
              const glm::vec2& uv1 = { 1.0f, 1.0f })
    {
        UIRenderObject ro;
        ro.position = { x, y };
        ro.size     = { w, h };
        ro.color    = color;
        ro.materialAssetId = mat;
        ro.textureAssetId  = tex;
        ro.type     = 0;
        // The helper's callers (a slider handle, a checkbox box) want ONE
        // rounding on all four corners; the four-corner story belongs to the
        // authored surface, which the manager stamps on afterwards.
        ro.cornerRadius = glm::vec4(cornerRadius);
        ro.uvMin    = uv0;
        ro.uvMax    = uv1;
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
    HE::UITextLayout opts; opts.alignH = centerH ? 1 : 0;
    emitTextL(e, text, pos, size, sizePx, color, opts, out);
}

// ── Panel / Image ────────────────────────────────────────────────────────────

void UIPanel::render(const UIWidgetRect& px, const UIElementRenderState&,
                     const HE::UUID& mat, float, std::vector<UIRenderObject>& out) const
{
    quad(out, px.x, px.y, px.w, px.h, color, mat, 0.0f, textureAssetId);
}

void UIImage::render(const UIWidgetRect& px, const UIElementRenderState&,
                     const HE::UUID& mat, float, std::vector<UIRenderObject>& out) const
{
    // Plain stretch when nothing is sliced, when there is no texture to slice,
    // or when the source size is not known yet (the runtime fills it in when it
    // resolves the path — until then a stretched picture beats nothing).
    const bool sliced = textureAssetId != HE::UUID{} && textureW > 0 && textureH > 0 &&
        (sliceLeft > 0.0f || sliceTop > 0.0f || sliceRight > 0.0f || sliceBottom > 0.0f);
    if (!sliced)
    {
        quad(out, px.x, px.y, px.w, px.h, tint, mat, 0.0f, textureAssetId);
        return;
    }

    const float tw = static_cast<float>(textureW), th = static_cast<float>(textureH);
    // Margins can be authored larger than the element is: shrink them together
    // so the two opposite ones never overlap, which would flip a corner.
    auto fitPair = [](float a, float b, float extent, float& outA, float& outB)
    {
        outA = std::max(0.0f, a); outB = std::max(0.0f, b);
        const float sum = outA + outB;
        if (sum > extent && sum > 0.0f)
        {
            const float k = extent / sum;
            outA *= k; outB *= k;
        }
    };
    float l, r, t, b;
    fitPair(sliceLeft, sliceRight,  px.w, l, r);
    fitPair(sliceTop,  sliceBottom, px.h, t, b);

    // Column and row edges, in destination pixels and in source UVs. The
    // corners keep their pixel size; what is between them takes the rest.
    const float xs[4]  = { px.x, px.x + l, px.x + px.w - r, px.x + px.w };
    const float ys[4]  = { px.y, px.y + t, px.y + px.h - b, px.y + px.h };
    const float us[4]  = { 0.0f, sliceLeft / tw, 1.0f - sliceRight / tw, 1.0f };
    const float vs[4]  = { 0.0f, sliceTop / th,  1.0f - sliceBottom / th, 1.0f };

    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
        {
            if (row == 1 && col == 1 && !sliceFillCentre) continue;
            const float w = xs[col + 1] - xs[col];
            const float h = ys[row + 1] - ys[row];
            if (w <= 0.0f || h <= 0.0f) continue;   // a margin ate this piece
            quad(out, xs[col], ys[row], w, h, tint, mat, 0.0f, textureAssetId,
                 { us[col], vs[row] }, { us[col + 1], vs[row + 1] });
        }
}

// ── Text ─────────────────────────────────────────────────────────────────────

// Resize the element to its own content. Height always tracks the line count ×
// font size; the width tracks the widest line unless WordWrap owns it (then the
// authored width IS the wrap column). A small padding keeps descenders and the
// last glyph's side bearing off the edge.
void UIText::applyAutoSize(float resolvedWidth)
{
    if (!autoSize) return;
    HE::UITextLayout opts;
    opts.alignH = alignH;
    opts.alignV = alignV;
    opts.wrap    = wordWrap;
    // The wrap column is the width the element ACTUALLY has: on a stretched
    // axis sizeX is the difference to the anchored span (often negative), so it
    // would wrap the text at one unit.
    const float wrapW = wordWrap ? std::max(1.0f, resolvedWidth) : 0.0f;
    const HE::BakedUIFont* f = HE::UIFontCache::find(fontAtlasKey);
    const glm::vec2 m = f ? HE::measureUIText(*f, text, fontSize, wrapW, opts)
                          : HE::measureUIText(text, fontSize, wrapW, opts);
    // An axis the anchor stretches belongs to the parent — content does not get
    // to resize it, or a label anchored across a side would come out one text
    // width WIDER than the side it is anchored to.
    const bool stretchX = anchorMaxX > anchorMinX + 1e-4f;
    const bool stretchY = anchorMaxY > anchorMinY + 1e-4f;
    if (!wordWrap && !stretchX) sizeX = m.x + fontSize * 0.25f;
    if (!stretchY)              sizeY = m.y + fontSize * 0.35f;
}

void UIText::render(const UIWidgetRect& px, const UIElementRenderState&,
                    const HE::UUID&, float pxScaleY, std::vector<UIRenderObject>& out) const
{
    HE::UITextLayout opts;
    opts.alignH = alignH;
    opts.alignV = alignV;
    opts.wrap   = wordWrap;
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
    // The surface, and only the surface. No radius here either: it is an
    // authored property now, stamped onto this quad by the manager. Whatever is
    // ON the button is made of children, drawn by the same loop that draws every
    // other element.
    quad(out, px.x, px.y, px.w, px.h, c, mat, 0.0f, textureAssetId);
}

// ── CheckBox ─────────────────────────────────────────────────────────────────

void UICheckBox::render(const UIWidgetRect& px, const UIElementRenderState& st,
                        const HE::UUID&, float pxScaleY, std::vector<UIRenderObject>& out) const
{
    // The tick box is sized by the LABEL, not by the element: it used to be a
    // square as tall as the element, so a checkbox stretched over a whole side
    // (to place its label, say) grew a box the size of half the screen. Capped
    // at the element's height so a deliberately tiny one still fits, and
    // centred in it so the box sits with the text rather than above it.
    const float box = std::min(px.h, fontSize * pxScaleY * 1.15f);
    const float by  = px.y + (px.h - box) * 0.5f;
    glm::vec4 bc = boxColor;
    if (st.hovered) bc = glm::vec4(glm::vec3(boxColor) * 1.3f, boxColor.a);
    quad(out, px.x, by, box, box, bc, {}, roundedR(box, box, 4.0f));
    if (checked)
    {
        const float inset = box * 0.22f;
        const float cb = box - 2 * inset;
        quad(out, px.x + inset, by + inset, cb, cb, checkColor, {}, roundedR(cb, cb, 2.0f));
    }
    const float gap = 0.4f * box;   // scales with the box, not a fixed 8 px
    const float lx = px.x + box + gap;
    emitText(*this, label, { lx, px.y }, { px.w - box - gap, px.h },
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
    // The track's radius is stamped (authored property); the FILL keeps its own,
    // because it is a part drawn on the surface rather than the surface itself.
    quad(out, px.x, px.y, px.w,     px.h, backColor, {}, 0.0f);
    quad(out, px.x, px.y, px.w * t, px.h, fillColor, {}, roundedR(px.w * t, px.h, 4.0f));
}

// ── TextInput ────────────────────────────────────────────────────────────────

void UITextInput::render(const UIWidgetRect& px, const UIElementRenderState& st,
                         const HE::UUID&, float pxScaleY, std::vector<UIRenderObject>& out) const
{
    glm::vec4 bg = backColor;
    if (st.focused) bg = glm::vec4(glm::min(glm::vec3(backColor) + 0.06f, glm::vec3(1.0f)), backColor.a);
    quad(out, px.x, px.y, px.w, px.h, bg, {}, 0.0f);   // radius is stamped (authored)
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
    const float sizePx = fontSize * pxScaleY;

    if (text.empty() && !st.focused)
    {
        emitText(*this, placeholder, tp, ts, sizePx,
                 glm::vec4(glm::vec3(textColor) * 0.5f, textColor.a * 0.7f), false, out);
        return;
    }

    // A password field is drawn as dots, one per character — the text itself is
    // stored as typed, only this changes.
    auto shownFor = [&](const std::string& s)
    {
        if (!password) return s;
        std::string dots;
        for (size_t i = 0; i < s.size(); i = uiUtf8Next(s, i)) dots += "\xE2\x80\xA2"; // •
        return dots;
    };
    // Width of the run up to a byte offset, in the same terms the glyphs are
    // emitted with — this is what puts the caret and the selection where the
    // characters actually are.
    const HE::BakedUIFont* f = HE::UIFontCache::find(fontAtlasKey);
    auto widthTo = [&](size_t byteEnd)
    {
        const std::string run = shownFor(text.substr(0, std::min(byteEnd, text.size())));
        if (run.empty()) return 0.0f;
        HE::UITextLayout opts;
        return (f ? HE::measureUIText(*f, run, sizePx, 0.0f, opts)
                  : HE::measureUIText(run, sizePx, 0.0f, opts)).x;
    };

    // ── Sideways scroll ──────────────────────────────────────────────────────
    // Keep the caret inside the visible strip. Without this a field you can type
    // more into than it is wide grows a caret that walks off the right edge and
    // takes the text you are typing with it. Unfocused fields snap back to the
    // start, because that is the half a reader wants to see.
    const float inner = std::max(1.0f, ts.x);
    if (!st.focused) scrollPx = 0.0f;
    else
    {
        const float caretX = widthTo(caret);
        if (caretX - scrollPx > inner) scrollPx = caretX - inner;
        if (caretX - scrollPx < 0.0f)  scrollPx = caretX;
        // Never leave empty space on the right while text hangs off the left —
        // what happens when the field grows or the text shrinks.
        const float total = widthTo(text.size());
        if (total - scrollPx < inner) scrollPx = std::max(0.0f, total - inner);
        if (scrollPx < 0.0f) scrollPx = 0.0f;
    }
    const glm::vec2 sp{ tp.x - scrollPx, tp.y };

    // Selection behind the text, so the glyphs stay readable on top of it.
    if (st.focused && selectable && hasSelection())
    {
        const float x0 = widthTo(selMin()), x1 = widthTo(selMax());
        const float h  = std::min(px.h - 4.0f, sizePx * 1.25f);
        quad(out, sp.x + x0, px.y + (px.h - h) * 0.5f, std::max(1.0f, x1 - x0), h,
             selectionColor);
    }

    emitText(*this, shownFor(text), sp, ts, sizePx, textColor, false, out);

    // ── The IME's unfinished text ────────────────────────────────────────────
    // Drawn AT the caret and pushed in front of whatever follows it, because
    // that is where it will land when the input method commits. Underlined
    // rather than coloured differently: an underline is what every platform's
    // preedit looks like, and it survives a theme that made the text any colour.
    float compositionWidth = 0.0f;
    if (st.focused && !composition.empty())
    {
        const float caretX = widthTo(caret);
        const glm::vec2 cp{ sp.x + caretX, sp.y };
        // Measured the same way the field measures everything else.
        HE::UITextLayout copts;
        compositionWidth = (f ? HE::measureUIText(*f, composition, sizePx, 0.0f, copts)
                              : HE::measureUIText(composition, sizePx, 0.0f, copts)).x;

        emitText(*this, composition, cp, ts, sizePx, textColor, false, out);
        // The underline: one thin quad under the run, at the text's own colour.
        const float h = std::min(px.h - 4.0f, sizePx * 1.25f);
        const float baseY = px.y + (px.h - h) * 0.5f + h;
        quad(out, cp.x, baseY - std::max(1.0f, sizePx * 0.06f),
             std::max(1.0f, compositionWidth), std::max(1.0f, sizePx * 0.06f), textColor);
    }

    // The caret: a thin bar at its offset, not a "|" glued to the end of the
    // string — that could only ever be at the end, which is why the field had
    // no way to edit anywhere else. While an IME is composing it sits inside the
    // composition, where that input method put it.
    if (st.focused)
    {
        const float h = std::min(px.h - 4.0f, sizePx * 1.25f);
        float caretX = widthTo(caret);
        if (!composition.empty())
        {
            // -1 = the IME did not say; the end of its own text is the sane place.
            const size_t upTo = compositionCursor < 0
                ? composition.size()
                : std::min(static_cast<size_t>(compositionCursor), composition.size());
            HE::UITextLayout copts;
            const std::string run = composition.substr(0, upTo);
            const float inner = run.empty() ? 0.0f
                : (f ? HE::measureUIText(*f, run, sizePx, 0.0f, copts)
                     : HE::measureUIText(run, sizePx, 0.0f, copts)).x;
            caretX += inner;
        }
        quad(out, sp.x + caretX, px.y + (px.h - h) * 0.5f,
             std::max(1.0f, sizePx * 0.08f), h, textColor);
    }
}

// Byte offset in `text` nearest to a point `localX` pixels into the field's
// text area — what a click has to answer to put the caret where it was aimed.
size_t UITextInput::caretAtX(float localX, float pxScaleY) const
{
    const float sizePx = fontSize * pxScaleY;
    const HE::BakedUIFont* f = HE::UIFontCache::find(fontAtlasKey);
    HE::UITextLayout opts;
    auto widthTo = [&](size_t byteEnd)
    {
        std::string run = text.substr(0, std::min(byteEnd, text.size()));
        if (password)
        {
            std::string dots;
            for (size_t i = 0; i < run.size(); i = uiUtf8Next(run, i)) dots += "\xE2\x80\xA2";
            run.swap(dots);
        }
        if (run.empty()) return 0.0f;
        return (f ? HE::measureUIText(*f, run, sizePx, 0.0f, opts)
                  : HE::measureUIText(run, sizePx, 0.0f, opts)).x;
    };
    // The click arrives relative to the field's text area; the text inside it may
    // be scrolled, so undo that first or every click past the scroll point lands
    // on the wrong character.
    localX += scrollPx;
    if (localX <= 0.0f) return 0;
    // Walk the boundaries and take the one whose midpoint the click passed —
    // clicking the left half of a character puts the caret before it.
    size_t best = 0;
    for (size_t i = 0; i < text.size(); )
    {
        const size_t next = uiUtf8Next(text, i);
        const float a = widthTo(i), b = widthTo(next);
        if (localX < (a + b) * 0.5f) return i;
        best = next;
        i = next;
    }
    return best;
}

// ── ComboBox ─────────────────────────────────────────────────────────────────

void UIComboBox::render(const UIWidgetRect& px, const UIElementRenderState& st,
                        const HE::UUID&, float pxScaleY, std::vector<UIRenderObject>& out) const
{
    glm::vec4 bg = st.hovered ? highlightColor : backColor;
    quad(out, px.x, px.y, px.w, px.h, bg, {}, 0.0f);   // radius is stamped (authored)
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

void UIBoxBase::writeJson(nlohmann::json& j) const
{
    j["padding"] = padding; j["spacing"] = spacing;
    // Written only once used, so a box authored before this existed stays
    // byte-identical.
    if (sizeToContent) j["sizeToContent"] = true;
    if (minSizeX > 0.0f || minSizeY > 0.0f) j["minSize"] = { minSizeX, minSizeY };
}
void UIBoxBase::readJson(const nlohmann::json& j)
{
    padding = j.value("padding", padding); spacing = j.value("spacing", spacing);
    sizeToContent = j.value("sizeToContent", false);
    if (const auto& m = j.value("minSize", nlohmann::json::array()); m.size() >= 2)
    { minSizeX = m[0].get<float>(); minSizeY = m[1].get<float>(); }
}

// The offset and the measured content extent are runtime state: a menu that
// reopens where it was last scrolled to is a bug, not a feature.
void UIScrollBox::writeJson(nlohmann::json& j) const
{ UIBoxBase::writeJson(j); j["barWidth"] = barWidth; j["barColor"] = colJson(barColor); }
void UIScrollBox::readJson(const nlohmann::json& j)
{
    UIBoxBase::readJson(j);
    barWidth = j.value("barWidth", barWidth);
    barColor = colFrom(j.value("barColor", nlohmann::json()), barColor);
}

// The box itself draws only its scrollbar, and only while there is something to
// scroll — a bar that is always there but sometimes does nothing is furniture.
void UIScrollBox::render(const UIWidgetRect& px, const UIElementRenderState&,
                         const HE::UUID&, float, std::vector<UIRenderObject>& out) const
{
    const float maxOff = maxScroll();
    if (barWidth <= 0.0f || maxOff <= 0.0f || contentExtent <= 0.0f) return;
    const float inner = std::max(1.0f, sizeY - 2.0f * padding);
    // Screen pixels per canvas unit on this axis, so the bar is drawn in the
    // same space as the rect handed in.
    const float scaleY = px.h / std::max(1.0f, sizeY);
    const float scaleX = px.w / std::max(1.0f, sizeX);

    const float visibleFrac = std::min(1.0f, inner / contentExtent);
    const float trackPx     = inner * scaleY;
    const float thumbPx     = std::max(12.0f, trackPx * visibleFrac);
    const float t           = maxOff > 0.0f ? (scrollOffset / maxOff) : 0.0f;
    const float x = px.x + px.w - (barWidth + padding) * scaleX;
    const float y = px.y + padding * scaleY + t * (trackPx - thumbPx);
    quad(out, x, y, barWidth * scaleX, thumbPx, barColor, HE::UUID{},
         barWidth * scaleX * 0.5f);
}

void UIImage::writeJson(nlohmann::json& j) const
{
    j["tint"] = colJson(tint);
    // Only written once sliced, so an image authored before 9-slice existed
    // saves byte-identical.
    if (sliceLeft > 0.0f || sliceTop > 0.0f || sliceRight > 0.0f || sliceBottom > 0.0f)
    {
        j["slice"] = { sliceLeft, sliceTop, sliceRight, sliceBottom };
        j["sliceFillCentre"] = sliceFillCentre;
    }
}
void UIImage::readJson(const nlohmann::json& j)
{
    tint = colFrom(j.value("tint", nlohmann::json()), tint);
    if (const auto& s = j.value("slice", nlohmann::json::array()); s.size() >= 4)
    {
        sliceLeft   = s[0].get<float>(); sliceTop    = s[1].get<float>();
        sliceRight  = s[2].get<float>(); sliceBottom = s[3].get<float>();
    }
    sliceFillCentre = j.value("sliceFillCentre", sliceFillCentre);
}

void UIText::writeJson(nlohmann::json& j) const
{ j["text"] = text; j["fontSize"] = fontSize; j["color"] = colJson(color);
  j["wordWrap"] = wordWrap; j["autoSize"] = autoSize;
  // "align" keeps its name and meaning (the horizontal one), so a widget saved
  // before there was a vertical alignment still loads with its text where it was.
  j["align"] = alignH; j["alignV"] = alignV; }
void UIText::readJson(const nlohmann::json& j)
{ text = j.value("text", text); fontSize = j.value("fontSize", fontSize);
  color = colFrom(j.value("color", nlohmann::json()), color);
  wordWrap = j.value("wordWrap", wordWrap);
  // Widgets authored before auto-size keep their hand-set box: defaulting them
  // to true would resize every existing label on load.
  autoSize = j.value("autoSize", false);
  alignH   = j.value("align", alignH);
  // Absent = middle, which is where text always sat before this existed.
  alignV   = j.value("alignV", alignV); }

void UIButton::writeJson(nlohmann::json& j) const
{
    // "text"/"fontSize"/"textColor" are deliberately NOT written any more: they
    // are the legacy caption, and their absence is what tells the loader that
    // this button has already been migrated to a Text child.
    j["color"] = colJson(color); j["hoveredColor"] = colJson(hoveredColor);
    j["pressedColor"] = colJson(pressedColor);
}
void UIButton::readJson(const nlohmann::json& j)
{
    color = colFrom(j.value("color", nlohmann::json()), color);
    hoveredColor = colFrom(j.value("hoveredColor", nlohmann::json()), hoveredColor);
    pressedColor = colFrom(j.value("pressedColor", nlohmann::json()), pressedColor);
    // The legacy caption is NOT read into this element — it has nowhere to live
    // here any more. uiWidgetTreeFromJson turns it into a Text child, which it
    // can do and this cannot: an element only ever sees its own object.
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
    // Only once used, so a field authored before these existed saves unchanged.
    if (maxLength > 0) j["maxLength"] = maxLength;
    if (password)      j["password"] = true;
    if (!editable)     j["editable"] = false;
    if (!selectable)   j["selectable"] = false;
    if (selectionColor != glm::vec4(0.25f, 0.45f, 0.80f, 0.75f))
        j["selectionColor"] = colJson(selectionColor);
    // Same rule: written only when set, so every field authored before the
    // filter existed still saves byte-identically.
    if (inputFilter != FilterAny)  j["inputFilter"] = inputFilter;
    if (!allowedChars.empty())     j["allowedChars"] = allowedChars;
}
void UITextInput::readJson(const nlohmann::json& j)
{
    text = j.value("text", text); placeholder = j.value("placeholder", placeholder);
    fontSize = j.value("fontSize", fontSize);
    backColor = colFrom(j.value("backColor", nlohmann::json()), backColor);
    textColor = colFrom(j.value("textColor", nlohmann::json()), textColor);
    selectionColor = colFrom(j.value("selectionColor", nlohmann::json()), selectionColor);
    maxLength  = j.value("maxLength", 0);
    password   = j.value("password", false);
    editable   = j.value("editable", true);
    selectable = j.value("selectable", true);
    inputFilter  = j.value("inputFilter", static_cast<int>(FilterAny));
    allowedChars = j.value("allowedChars", std::string());
    // The authored text decides where the caret starts, not a stale offset.
    caret = selAnchor = text.size();
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
