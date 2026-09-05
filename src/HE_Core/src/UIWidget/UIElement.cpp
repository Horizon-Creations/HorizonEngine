#include <UIWidget/UIElements.h>
#include <Renderer/UIFont.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstddef>

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
        case UIWidgetType::ListView:      return std::make_unique<UIListView>();
        case UIWidgetType::WrapBox:       return std::make_unique<UIWrapBox>();
        case UIWidgetType::Grid:          return std::make_unique<UIGrid>();
        case UIWidgetType::TabBox:        return std::make_unique<UITabBox>();
        case UIWidgetType::Splitter:      return std::make_unique<UISplitter>();
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
        UIWidgetType::ScrollBox, UIWidgetType::WidgetRef, UIWidgetType::Spacer,
        UIWidgetType::ListView, UIWidgetType::WrapBox, UIWidgetType::Grid,
        UIWidgetType::TabBox, UIWidgetType::Splitter };
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
        "VerticalBox", "HorizontalBox", "ScrollBox", "WidgetRef", "Spacer",
        "ListView", "WrapBox", "Grid", "TabBox", "Splitter" };
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

// The UTF-8 walk moved to Renderer/UIFont.cpp, where the glyph loops use the
// same one: the caret and the glyphs have to agree about where a character
// begins, and two copies of that rule would eventually disagree.

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

// ── A property value on disk ─────────────────────────────────────────────────
// Written into whatever object the caller hands over, under keys of its own, so
// a value can share an object with the name it belongs to (see
// UIWidgetRef::writeJson) instead of needing a wrapper.
//
// The type travels as its NAME. A number would be one byte shorter and would
// change meaning the day somebody inserts a type into UIPropType instead of
// appending one — the failure being not a parse error but a colour read as a
// string, which is the kind that surfaces months later in someone's project.

namespace
{
    const char* propTypeName(UIPropType t)
    {
        switch (t)
        {
            case UIPropType::Float:      return "float";
            case UIPropType::Int:        return "int";
            case UIPropType::Bool:       return "bool";
            case UIPropType::String:     return "string";
            case UIPropType::Color:      return "color";
            case UIPropType::Vec2:       return "vec2";
            case UIPropType::StringList: return "stringList";
        }
        return "float";
    }
}

void uiPropValueToJson(nlohmann::json& out, const UIPropValue& v)
{
    out["type"] = propTypeName(v.type);
    switch (v.type)
    {
        case UIPropType::Float:  out["value"] = v.f;   break;
        case UIPropType::Int:    out["value"] = v.i;   break;
        case UIPropType::Bool:   out["value"] = v.b;   break;
        case UIPropType::String: out["value"] = v.s;   break;
        case UIPropType::Color:  out["value"] = { v.col.r, v.col.g, v.col.b, v.col.a }; break;
        case UIPropType::Vec2:   out["value"] = { v.v2.x, v.v2.y }; break;
        case UIPropType::StringList: out["value"] = v.list; break;
    }
}

UIPropValue uiPropValueFromJson(const nlohmann::json& o)
{
    const std::string t = o.value("type", std::string("float"));
    const auto it = o.find("value");
    const bool has = it != o.end();
    // An unreadable value falls back to the TYPE's zero rather than to nothing:
    // the parameter still exists and still writes, it just writes a default,
    // which is visible and fixable. Dropping it would leave the component
    // showing whatever it was authored with and no sign that anything was lost.
    if (t == "int")    return UIPropValue::ofInt(has && it->is_number() ? it->get<int>() : 0);
    if (t == "bool")   return UIPropValue::ofBool(has && it->is_boolean() && it->get<bool>());
    if (t == "string") return UIPropValue::ofString(has && it->is_string() ? it->get<std::string>()
                                                                          : std::string());
    if (t == "color")
    {
        glm::vec4 c{ 1.0f };
        if (has && it->is_array() && it->size() == 4)
            c = { (*it)[0].get<float>(), (*it)[1].get<float>(),
                  (*it)[2].get<float>(), (*it)[3].get<float>() };
        return UIPropValue::ofColor(c);
    }
    if (t == "vec2")
    {
        glm::vec2 v{ 0.0f };
        if (has && it->is_array() && it->size() == 2)
            v = { (*it)[0].get<float>(), (*it)[1].get<float>() };
        return UIPropValue::ofVec2(v);
    }
    if (t == "stringList")
    {
        UIPropValue r;
        r.type = UIPropType::StringList;
        if (has && it->is_array())
            for (const auto& s : *it) if (s.is_string()) r.list.push_back(s.get<std::string>());
        return r;
    }
    return UIPropValue::ofFloat(has && it->is_number() ? it->get<float>() : 0.0f);
}

UIPropValue uiPropValueCoerce(const UIPropValue& v, UIPropType want)
{
    if (v.type == want) return v;
    // What the value is worth as a number, whichever slot it came in through.
    // A Bool counts as 0/1 and a String as what std::stof can read off its
    // front — the two conversions a person would expect — and anything else is
    // 0, which is this type's zero and not a reinterpretation.
    const auto asNumber = [&]() -> float
    {
        switch (v.type)
        {
            case UIPropType::Float: return v.f;
            case UIPropType::Int:   return static_cast<float>(v.i);
            case UIPropType::Bool:  return v.b ? 1.0f : 0.0f;
            case UIPropType::String:
                try { return std::stof(v.s); } catch (...) { return 0.0f; }
            default: return 0.0f;
        }
    };
    switch (want)
    {
        case UIPropType::Float:  return UIPropValue::ofFloat(asNumber());
        case UIPropType::Int:    return UIPropValue::ofInt(static_cast<int>(asNumber()));
        case UIPropType::Bool:   return UIPropValue::ofBool(asNumber() != 0.0f);
        case UIPropType::String:
        {
            switch (v.type)
            {
                case UIPropType::Float: return UIPropValue::ofString(std::to_string(v.f));
                case UIPropType::Int:   return UIPropValue::ofString(std::to_string(v.i));
                case UIPropType::Bool:  return UIPropValue::ofString(v.b ? "true" : "false");
                default:                return UIPropValue::ofString(std::string());
            }
        }
        case UIPropType::Color:  return UIPropValue::ofColor(glm::vec4(1.0f));
        case UIPropType::Vec2:   return UIPropValue::ofVec2(glm::vec2(0.0f));
        case UIPropType::StringList:
        {
            UIPropValue r;
            r.type = UIPropType::StringList;
            if (v.type == UIPropType::String && !v.s.empty()) r.list.push_back(v.s);
            return r;
        }
    }
    return v;
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

void UIWidgetRef::writeJson(nlohmann::json& j) const
{
    j["widget"] = widgetPath;
    // Only when this copy was actually told something, like every other optional
    // field: a ref authored before components existed saves byte-identical.
    //
    // An ARRAY rather than an object keyed by name, because the order an author
    // set them in is the order they are shown in, and a JSON object is free to
    // reorder its keys — the same reason the Map container needed its own key
    // list (see the container work).
    if (!paramValues.empty())
    {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& [name, v] : paramValues)
        {
            nlohmann::json e;
            e["name"] = name;
            uiPropValueToJson(e, v);
            arr.push_back(std::move(e));
        }
        j["params"] = std::move(arr);
    }
}
void UIWidgetRef::readJson(const nlohmann::json& j)
{
    widgetPath = j.value("widget", widgetPath);
    paramValues.clear();
    if (const auto it = j.find("params"); it != j.end() && it->is_array())
        for (const auto& e : *it)
        {
            if (!e.is_object()) continue;
            std::string name = e.value("name", std::string());
            if (name.empty()) continue;
            paramValues.emplace_back(std::move(name), uiPropValueFromJson(e));
        }
}

// Nothing of its own — a Spacer is its rect and nothing else.
const UIPropTable& UISpacer::propTable() const
{
    static const UIPropTable t = {};
    return t;
}

const UIPropTable& UIListView::propTable() const
{
    // "Padding" and "Spacing" keep the names every other container uses: a list
    // is inset and gapped the same way a box is, and a second vocabulary for the
    // same two numbers would be one an author has to learn twice.
    static const UIPropTable t = {
        uiprop::slot<&UIListView::rowWidget>({ "Row Widget", UIPropType::String }),
        uiprop::slot<&UIListView::rowHeight>({ "Row Height", UIPropType::Float, 1.0f, 2000.0f }),
        uiprop::slot<&UIListView::padding>({ "Padding", UIPropType::Float, 0.0f, 200.0f }),
        uiprop::slot<&UIListView::spacing>({ "Spacing", UIPropType::Float, 0.0f, 200.0f }),
        uiprop::slot<&UIListView::backColor>({ "Back Color", UIPropType::Color }),
        uiprop::slot<&UIListView::rowHoverColor>({ "Row Hover Color", UIPropType::Color }),
        uiprop::slot<&UIListView::rowSelectedColor>({ "Row Selected Color", UIPropType::Color }),
        uiprop::slot<&UIListView::selectionMode>({ "Selection", UIPropType::Int, 0.0f, 2.0f }),
        uiprop::slot<&UIListView::barWidth>({ "Bar Width", UIPropType::Float, 0.0f, 40.0f }),
        uiprop::slot<&UIListView::barColor>({ "Bar Color", UIPropType::Color }),
        // The item count is RUNTIME state and still belongs here: it is what a
        // graph sets to fill the list, and a Set Property node reaches it by
        // name like every other property. It is simply never serialized.
        //
        // Not a plain field slot: writing the number alone would leave a
        // selection pointing past the end and an offset beyond the new bottom,
        // so Set Property and Set List Count would do different amounts of work
        // for the same sentence. Both land on setItemCount instead.
        uiprop::custom({ "Item Count", UIPropType::Int, 0.0f, 1.0e9f },
            [](const UIElement& e) -> UIPropValue
            { return UIPropValue::ofInt(static_cast<const UIListView&>(e).itemCount); },
            [](UIElement& e, const UIPropValue& v)
            { static_cast<UIListView&>(e).setItemCount(v.i); }),
    };
    return t;
}

void UIListView::setItemCount(int n)
{
    itemCount = n > 0 ? n : 0;
    selection.erase(std::remove_if(selection.begin(), selection.end(),
        [this](int i){ return i >= itemCount; }), selection.end());
    if (hoveredRow >= itemCount) hoveredRow = -1;
    scrollOffset = std::clamp(scrollOffset, 0.0f, maxScroll());
}

bool UIListView::setSelected(int item, bool on)
{
    if (selectionMode == 0) return false;
    if (item < 0 || item >= itemCount) return false;
    const bool had = isSelected(item);
    if (had == on) return false;
    if (!on)
    {
        selection.erase(std::remove(selection.begin(), selection.end(), item),
                        selection.end());
        return true;
    }
    // Single mode is not "multiple, but please only pick one": picking replaces
    // what was there, which is the whole difference between the two modes.
    if (selectionMode == 1) selection.clear();
    selection.push_back(item);
    std::sort(selection.begin(), selection.end());
    return true;
}

bool UIListView::clearSelection()
{
    if (selection.empty()) return false;
    selection.clear();
    return true;
}

bool UIListView::scrollToItem(int item)
{
    if (item < 0 || item >= itemCount) return false;
    const float top    = item * rowStep();
    const float bottom = top + rowHeight;
    const float before = scrollOffset;
    // Above the view: put its top at the top. Below it: put its bottom at the
    // bottom. Already inside: leave it exactly where it is, because a list that
    // re-centres on every keystroke is a list nobody can read while stepping.
    if (top < scrollOffset)                        scrollOffset = top;
    else if (bottom > scrollOffset + innerHeight()) scrollOffset = bottom - innerHeight();
    scrollOffset = std::clamp(scrollOffset, 0.0f, maxScroll());
    return scrollOffset != before;
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

// ── Track tokens ─────────────────────────────────────────────────────────────
UIGridTrack uiParseGridTrack(const std::string& token)
{
    // Trimmed and lowercased first: "  Auto " is what a human types.
    std::string t;
    for (char c : token) if (c != ' ' && c != '\t') t.push_back(static_cast<char>(std::tolower(
        static_cast<unsigned char>(c))));
    if (t == "auto") return { UIGridTrack::Kind::Auto, 0.0f };
    if (!t.empty() && t.back() == '*')
    {
        const std::string head = t.substr(0, t.size() - 1);
        if (head.empty()) return { UIGridTrack::Kind::Weight, 1.0f };
        try
        {
            const float w = std::stof(head);
            return { UIGridTrack::Kind::Weight, w > 0.0f ? w : 1.0f };
        }
        catch (...) { return { UIGridTrack::Kind::Weight, 1.0f }; }
    }
    try
    {
        std::size_t used = 0;
        const float v = std::stof(t, &used);
        // "12px" or "12 nonsense" is not a number anybody meant — fall through
        // to the visible default rather than silently reading the 12.
        if (used == t.size() && v >= 0.0f) return { UIGridTrack::Kind::Fixed, v };
    }
    catch (...) {}
    // Unreadable: one share. A track you can SEE and fix beats one that
    // collapsed to nothing and hid the typo.
    return { UIGridTrack::Kind::Weight, 1.0f };
}

const UIPropTable& UIGrid::propTable() const
{
    // The two lists are custom slots and not plain fields: writing them has to
    // re-parse, or a Set Property from a graph would change the words and leave
    // the layout running on the old ones.
    static const UIPropTable t = {
        uiprop::custom({ "Column Sizes", UIPropType::StringList },
            [](const UIElement& e) -> UIPropValue
            { UIPropValue v; v.type = UIPropType::StringList;
              v.list = static_cast<const UIGrid&>(e).columns; return v; },
            [](UIElement& e, const UIPropValue& v)
            { auto& g = static_cast<UIGrid&>(e); g.columns = v.list; g.reparse(); }),
        uiprop::custom({ "Row Sizes", UIPropType::StringList },
            [](const UIElement& e) -> UIPropValue
            { UIPropValue v; v.type = UIPropType::StringList;
              v.list = static_cast<const UIGrid&>(e).rows; return v; },
            [](UIElement& e, const UIPropValue& v)
            { auto& g = static_cast<UIGrid&>(e); g.rows = v.list; g.reparse(); }),
        uiprop::slot<&UIGrid::padding>({ "Padding", UIPropType::Float, 0.0f, 200.0f }),
        uiprop::slot<&UIGrid::spacing>({ "Spacing", UIPropType::Float, 0.0f, 200.0f }),
        uiprop::slot<&UIGrid::rowSpacing>({ "Row Spacing", UIPropType::Float, 0.0f, 200.0f }),
        uiprop::slot<&UIGrid::sizeToContent>({ "Size To Content", UIPropType::Bool }),
        uiprop::slot<&UIGrid::minSizeX>({ "Min Width",  UIPropType::Float }),
        uiprop::slot<&UIGrid::minSizeY>({ "Min Height", UIPropType::Float }),
    };
    return t;
}

const UIPropTable& UIWrapBox::propTable() const
{
    // Padding and Spacing keep the container names; Line Spacing is the one
    // number a wrapping row has that a straight one does not.
    static const UIPropTable t = {
        uiprop::slot<&UIWrapBox::padding>({ "Padding", UIPropType::Float, 0.0f, 200.0f }),
        uiprop::slot<&UIWrapBox::spacing>({ "Spacing", UIPropType::Float, 0.0f, 200.0f }),
        uiprop::slot<&UIWrapBox::lineSpacing>({ "Line Spacing", UIPropType::Float, 0.0f, 200.0f }),
        uiprop::slot<&UIWrapBox::sizeToContent>({ "Size To Content", UIPropType::Bool }),
        uiprop::slot<&UIWrapBox::minSizeX>({ "Min Width",  UIPropType::Float }),
        uiprop::slot<&UIWrapBox::minSizeY>({ "Min Height", UIPropType::Float }),
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
        uiprop::slot<&UIText::richText>({ "RichText", UIPropType::Bool }),
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
        uiprop::slot<&UITextInput::multiline>  ({ "Multiline", UIPropType::Bool }),
        uiprop::slot<&UITextInput::wrapText>   ({ "Wrap Text", UIPropType::Bool }),
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

void UITextInput::recordEdit(const EditState& before, EditKind kind, bool coalesce)
{
    // An edit that did not change anything is not a step to come back to. This
    // is what lets the callers snapshot unconditionally and hand the snapshot
    // over on every exit path, instead of each of them working out whether the
    // keystroke survived the filter, the length limit and the read-only flag.
    if (before.text == text) return;
    redoStack.clear();
    // Continuing the open group means NOT pushing: the state to come back to is
    // the one from the start of the run, which is already on the stack.
    if (coalesce && openRun == kind && !undoStack.empty()) return;
    undoStack.push_back(before);
    if (undoStack.size() > kMaxUndoSteps)
        undoStack.erase(undoStack.begin(),
                        undoStack.begin() + static_cast<long>(undoStack.size() - kMaxUndoSteps));
    // An edit that refused to be merged INTO a group does not open one either:
    // a paste is a step by itself, and the character typed straight after it
    // must not be swallowed by it. Only a mergeable edit leaves a run open.
    openRun = coalesce ? kind : EditKind::None;
}

namespace
{
// The half of undo and redo that is the same in both directions: hand the state
// you are in to the other stack, take the top of this one, become it.
bool uiTextStep(UITextInput& ti, std::vector<UITextInput::EditState>& from,
                std::vector<UITextInput::EditState>& to)
{
    if (from.empty()) return false;
    to.push_back({ ti.text, ti.caret, ti.selAnchor });
    const UITextInput::EditState s = from.back();
    from.pop_back();
    ti.text      = s.text;
    ti.caret     = s.caret;
    ti.selAnchor = s.selAnchor;
    ti.clampCaret();
    ti.preferredCaretX = -1.0f;
    // Whatever run was open belongs to the text that just went away.
    ti.sealUndoRun();
    return true;
}
} // namespace

bool UITextInput::undoEdit() { return uiTextStep(*this, undoStack, redoStack); }
bool UITextInput::redoEdit() { return uiTextStep(*this, redoStack, undoStack); }

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
    if (n == "Focus Frame")  { out = UIPropValue::ofBool(e.focusFrame);         return true; }
    if (n == "Accepts Drop") { out = UIPropValue::ofBool(e.acceptsDrop);        return true; }
    if (n == "Draggable")    { out = UIPropValue::ofBool(e.draggable);          return true; }
    if (n == "Drag Payload") { out = UIPropValue::ofString(e.dragPayload);      return true; }
    if (n == "Enabled")      { out = UIPropValue::ofBool(e.enabled);            return true; }
    if (n == "Render Opacity"){out = UIPropValue::ofFloat(e.renderOpacity);     return true; }
    if (n == "Transition")   { out = UIPropValue::ofFloat(e.transition);        return true; }
    if (n == "Slot Fill")    { out = UIPropValue::ofFloat(e.slotFill);          return true; }
    if (n == "Grid Column")  { out = UIPropValue::ofInt(e.gridColumn);          return true; }
    if (n == "Grid Row")     { out = UIPropValue::ofInt(e.gridRow);             return true; }
    if (n == "Column Span")  { out = UIPropValue::ofInt(e.gridColumnSpan);      return true; }
    if (n == "Row Span")     { out = UIPropValue::ofInt(e.gridRowSpan);         return true; }
    if (n == "Rotation")     { out = UIPropValue::ofFloat(e.rotation);          return true; }
    if (n == "Position")     { out = UIPropValue::ofVec2({ e.posX, e.posY });   return true; }
    if (n == "Size")         { out = UIPropValue::ofVec2({ e.sizeX, e.sizeY }); return true; }
    if (n == "Layer")        { out = UIPropValue::ofInt(e.layer);               return true; }
    if (n == "Hover Cursor") { out = UIPropValue::ofInt((int)e.hoverCursor);    return true; }
    if (n == "Material")     { out = UIPropValue::ofString(e.material);         return true; }
    if (n == "Texture")      { out = UIPropValue::ofString(e.texture);          return true; }
    if (n == "Font")         { out = UIPropValue::ofString(e.font);             return true; }
    if (n == "Tooltip")      { out = UIPropValue::ofString(e.tooltip);          return true; }
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
    if (n == "Shadow")       { out = UIPropValue::ofBool(e.shadow);             return true; }
    if (n == "Shadow Color") { out = UIPropValue::ofColor(e.shadowColor);       return true; }
    if (n == "Shadow Blur")  { out = UIPropValue::ofFloat(e.shadowBlur);        return true; }
    if (n == "Shadow Offset"){ out = UIPropValue::ofVec2({ e.shadowOffsetX, e.shadowOffsetY });
                               return true; }
    if (n == "Inner Shadow") { out = UIPropValue::ofBool(e.innerShadow);        return true; }
    if (n == "Inner Shadow Color"){ out = UIPropValue::ofColor(e.innerShadowColor); return true; }
    if (n == "Inner Shadow Blur") { out = UIPropValue::ofFloat(e.innerShadowBlur);  return true; }
    return false;
}

} // namespace

const std::vector<UIPropDesc>& uiBaseProperties()
{
    // Same names, same types, same ORDER as getBaseProp above — read together,
    // they are one list written twice, and test_ui_widgets pins that.
    static const std::vector<UIPropDesc> t = {
        { "Visible",             UIPropType::Bool },
        { "Hit Testable",        UIPropType::Bool },
        { "Clip Children",       UIPropType::Bool },
        { "Focus Frame",         UIPropType::Bool },
        { "Accepts Drop",        UIPropType::Bool },
        { "Draggable",           UIPropType::Bool },
        // Beside Draggable rather than down with the other strings: this list is
        // getBaseProp written a second time, and the two are read together.
        { "Drag Payload",        UIPropType::String },
        { "Enabled",             UIPropType::Bool },
        { "Render Opacity",      UIPropType::Float, 0.0f, 1.0f },
        { "Transition",          UIPropType::Float, 0.0f, 2.0f },
        { "Slot Fill",           UIPropType::Float },
        { "Grid Column",         UIPropType::Int },
        { "Grid Row",            UIPropType::Int },
        { "Column Span",         UIPropType::Int },
        { "Row Span",            UIPropType::Int },
        { "Rotation",            UIPropType::Float },
        { "Position",            UIPropType::Vec2 },
        { "Size",                UIPropType::Vec2 },
        { "Layer",               UIPropType::Int },
        { "Hover Cursor",        UIPropType::Int },
        { "Material",            UIPropType::String },
        { "Texture",             UIPropType::String },
        { "Font",                UIPropType::String },
        { "Tooltip",             UIPropType::String },
        { "Corner Radius",       UIPropType::Float },
        { "Corner TL",           UIPropType::Float },
        { "Corner TR",           UIPropType::Float },
        { "Corner BR",           UIPropType::Float },
        { "Corner BL",           UIPropType::Float },
        { "Border Width",        UIPropType::Float },
        { "Border Color",        UIPropType::Color },
        { "Gradient",            UIPropType::Bool },
        { "Gradient Color",      UIPropType::Color },
        { "Gradient Angle",      UIPropType::Float },
        { "Gradient Shape",      UIPropType::Int },
        { "Shadow",              UIPropType::Bool },
        { "Shadow Color",        UIPropType::Color },
        { "Shadow Blur",         UIPropType::Float },
        { "Shadow Offset",       UIPropType::Vec2 },
        { "Inner Shadow",        UIPropType::Bool },
        { "Inner Shadow Color",  UIPropType::Color },
        { "Inner Shadow Blur",   UIPropType::Float },
    };
    return t;
}

namespace
{
bool setBaseProp(UIElement& e, const std::string& n, const UIPropValue& v)
{
    if (n == "Visible")      { e.visible     = v.b; return true; }
    if (n == "Hit Testable") { e.hitTestable = v.b; return true; }
    if (n == "Clip Children"){ e.clipChildren = v.b; return true; }
    if (n == "Focus Frame")  { e.focusFrame = v.b; return true; }
    if (n == "Accepts Drop") { e.acceptsDrop = v.b; return true; }
    if (n == "Draggable")    { e.draggable = v.b; return true; }
    if (n == "Drag Payload") { e.dragPayload = v.s; return true; }
    if (n == "Enabled")      { e.enabled = v.b; return true; }
    if (n == "Render Opacity"){ e.renderOpacity = v.f < 0.0f ? 0.0f : (v.f > 1.0f ? 1.0f : v.f); return true; }
    // A negative transition is a state change running backwards, which is
    // nothing; it lands on 0, the value that means "at once".
    if (n == "Transition")   { e.transition = v.f < 0.0f ? 0.0f : v.f; return true; }
    if (n == "Slot Fill")    { e.slotFill = v.f < 0.0f ? 0.0f : v.f; return true; }
    // -1 stays -1: it is not an out-of-range cell, it is "the next free one".
    if (n == "Grid Column")  { e.gridColumn = v.i < -1 ? -1 : v.i; return true; }
    if (n == "Grid Row")     { e.gridRow    = v.i < -1 ? -1 : v.i; return true; }
    if (n == "Column Span")  { e.gridColumnSpan = v.i < 1 ? 1 : v.i; return true; }
    if (n == "Row Span")     { e.gridRowSpan    = v.i < 1 ? 1 : v.i; return true; }
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
    if (n == "Tooltip")      { e.tooltip = v.s; return true; }
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
    if (n == "Shadow")       { e.shadow = v.b; return true; }
    if (n == "Shadow Color") { e.shadowColor = v.col; return true; }
    if (n == "Shadow Blur")  { e.shadowBlur = std::max(0.0f, v.f); return true; }
    if (n == "Shadow Offset"){ e.shadowOffsetX = v.v2.x; e.shadowOffsetY = v.v2.y; return true; }
    if (n == "Inner Shadow") { e.innerShadow = v.b; return true; }
    if (n == "Inner Shadow Color"){ e.innerShadowColor = v.col; return true; }
    if (n == "Inner Shadow Blur") { e.innerShadowBlur = std::max(0.0f, v.f); return true; }
    return false;
}
} // namespace

std::vector<UIPropDesc> UIElement::allProperties() const
{
    std::vector<UIPropDesc> out = properties();
    out.push_back({ "Visible",      UIPropType::Bool });
    out.push_back({ "Hit Testable", UIPropType::Bool });
    out.push_back({ "Clip Children",UIPropType::Bool });
    out.push_back({ "Focus Frame",  UIPropType::Bool });
    out.push_back({ "Enabled",      UIPropType::Bool });
    out.push_back({ "Render Opacity", UIPropType::Float, 0.0f, 1.0f });
    // Offered on every type, not only the ones that blend today: it is a
    // property of the ELEMENT, a theme sets it once for a whole application,
    // and a row that appears and disappears depending on the widget type is a
    // row nobody trusts. What each type does with it is the renderer's business.
    out.push_back({ "Transition",   UIPropType::Float, 0.0f, 2.0f });
    out.push_back({ "Slot Fill",    UIPropType::Float });
    out.push_back({ "Grid Column",  UIPropType::Int });
    out.push_back({ "Grid Row",     UIPropType::Int });
    out.push_back({ "Column Span",  UIPropType::Int });
    out.push_back({ "Row Span",     UIPropType::Int });
    out.push_back({ "Rotation",     UIPropType::Float });
    out.push_back({ "Position",     UIPropType::Vec2 });
    out.push_back({ "Size",         UIPropType::Vec2 });
    out.push_back({ "Layer",        UIPropType::Int });
    out.push_back({ "Hover Cursor", UIPropType::Int });
    out.push_back({ "Tooltip",      UIPropType::String });
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
        out.push_back({ "Shadow",         UIPropType::Bool });
        out.push_back({ "Shadow Color",   UIPropType::Color });
        out.push_back({ "Shadow Blur",    UIPropType::Float });
        out.push_back({ "Shadow Offset",  UIPropType::Vec2 });
        out.push_back({ "Inner Shadow",       UIPropType::Bool });
        out.push_back({ "Inner Shadow Color", UIPropType::Color });
        out.push_back({ "Inner Shadow Blur",  UIPropType::Float });
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

    // ── A solid triangle, out of the only shape this system has ──────────────
    // Rows of decreasing width, each one an ordinary quad. Built this way rather
    // than as two turned bars (the modern chevron) for a reason that is not
    // taste: WidgetManager::extract folds the whole rotation chain onto EVERY
    // quad an element emits, overwriting whatever the element set — so inside a
    // rotated panel a chevron's two bars would both take the panel's angle and
    // come out parallel. A triangle made of upright rows has no angle to lose.
    //
    // The rows overlap by a hair so no seam shows between them, and there are
    // as many as there are pixels of height, so the staircase is one pixel per
    // step at any size.
    void triangle(std::vector<UIRenderObject>& out, const HE::UIComboBox::Arrow& a,
                  const glm::vec4& color, bool pointUp)
    {
        if (a.halfW <= 0.0f || a.height <= 0.0f) return;
        const int rows = std::clamp(static_cast<int>(std::ceil(a.height)), 3, 32);
        const float step = a.height / static_cast<float>(rows);
        const float top  = a.cy - a.height * 0.5f;
        for (int i = 0; i < rows; ++i)
        {
            // How far along the triangle this row is, measured from its BASE:
            // full width at the base, a sliver at the point.
            const float t = pointUp ? (static_cast<float>(i) + 1.0f) / rows
                                    : 1.0f - static_cast<float>(i) / rows;
            const float w = 2.0f * a.halfW * t;
            if (w <= 0.0f) continue;
            // Each row is a CAPSULE, not a rectangle. The rounding costs nothing
            // (it is the same SDF every quad already goes through) and it is
            // antialiased, so the two diagonal edges come out soft instead of as
            // a hard staircase — which is the whole difference between "a
            // triangle" and "a triangle somebody drew out of blocks".
            const float rh = step + 0.5f;
            quad(out, a.cx - w * 0.5f, top + step * static_cast<float>(i),
                 w, rh, color, HE::UUID{}, roundedR(w, rh, rh * 0.5f));
        }
    }
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
const HE::UIRichText& UIText::parsed() const
{
    if (!m_parsedOnce || m_parsedFrom != text)
    {
        m_parsed = HE::uiParseRichText(text);
        m_parsedFrom = text;
        m_parsedOnce = true;
    }
    return m_parsed;
}

namespace
{
    // The one place that says how a Text's rect and font turn into a rich
    // layout. Three callers — measure, draw, hit test — and they must agree
    // down to the pixel or a link is clickable somewhere it is not drawn.
    HE::UIRichLayout richLayoutOf(const UIText& t, const HE::UIWidgetRect& px,
                                  float pxScaleY)
    {
        HE::UITextLayout opts;
        opts.alignH = t.alignH;
        opts.alignV = t.alignV;
        opts.wrap   = t.wordWrap;
        const HE::BakedUIFont* f = HE::UIFontCache::find(t.fontAtlasKey);
        const HE::BakedUIFont& font = f ? *f : HE::sharedUIFont();
        return HE::uiLayoutRichText(font, t.parsed(), { px.x, px.y }, { px.w, px.h },
                                    t.fontSize * pxScaleY, opts, t.fontAtlasKey);
    }
}

std::string UIText::linkAt(const UIWidgetRect& px, float pxScaleY, float x, float y) const
{
    if (!richText || !parsed().hasLinks) return {};
    return HE::uiRichLinkAt(parsed(), richLayoutOf(*this, px, pxScaleY), x, y);
}

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
    // Rich text measures through the rich layout, or a label would be sized for
    // the markup it is written in rather than the words it shows — the tags
    // would count as characters and a <size=2> word would not count enough.
    glm::vec2 m;
    if (richText)
    {
        const HE::BakedUIFont& font = f ? *f : HE::sharedUIFont();
        m = HE::uiLayoutRichText(font, parsed(), { 0.0f, 0.0f },
                                 { wrapW > 0.0f ? wrapW : 1.0e6f, 0.0f },
                                 fontSize, opts, fontAtlasKey).size;
    }
    else
        m = f ? HE::measureUIText(*f, text, fontSize, wrapW, opts)
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
    if (richText)
    {
        const HE::BakedUIFont* f = HE::UIFontCache::find(fontAtlasKey);
        const HE::BakedUIFont& font = f ? *f : HE::sharedUIFont();
        // `color` is what a run without one of its own draws in, so a rich label
        // with no colour tags is exactly the label it was before the flag.
        HE::uiEmitRichText(font, fontAtlasKey, parsed(),
                           richLayoutOf(*this, px, pxScaleY), color, layer, out);
        return;
    }
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
    // Rest → hover → press, as two mixes rather than two assignments. At 0 and
    // at 1 they give back exactly what the two `if`s gave before (a press wins
    // over a hover either way), and in between they are the transition.
    glm::vec4 c = glm::mix(color, hoveredColor, st.hoverAmount());
    c = glm::mix(c, pressedColor, st.pressAmount());
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
    const glm::vec4 bcHover = glm::vec4(glm::vec3(boxColor) * 1.3f, boxColor.a);
    glm::vec4 bc = glm::mix(boxColor, bcHover, st.hoverAmount());
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
    // No border of its own any more. This type used to draw four blue edge
    // quads, from before the manager drew a focus ring for EVERY element type —
    // and two rings around one field is not twice as clear, it is a field with
    // something wrong with it. The lightened background above stays: that is
    // this type's own answer to "the caret is in here", and it survives being
    // clipped, which a ring drawn outside the box does not.
    const float pad = 6.0f;
    const glm::vec2 tp{ px.x + pad, px.y };
    const glm::vec2 ts{ px.w - 2 * pad, px.h };
    const float sizePx = fontSize * pxScaleY;

    // The placeholder goes away when somebody starts EDITING, not when the tab
    // order walks past: a hint that vanishes because the focus went by leaves an
    // empty box that says nothing about itself.
    if (text.empty() && !st.editing)
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
    auto runWidth = [&](const std::string& shown)
    {
        if (shown.empty()) return 0.0f;
        HE::UITextLayout opts;
        return (f ? HE::measureUIText(*f, shown, sizePx, 0.0f, opts)
                  : HE::measureUIText(shown, sizePx, 0.0f, opts)).x;
    };
    auto widthTo = [&](size_t byteEnd)
    { return runWidth(shownFor(text.substr(0, std::min(byteEnd, text.size())))); };

    // ── More than one line ───────────────────────────────────────────────────
    // Its own path rather than a handful of conditionals in the one below: a
    // multiline field scrolls the other way, aligns to the top, and draws its
    // selection as several rectangles. Sharing the body would mean a `multiline`
    // test on nearly every line of it, and every single-line field — which is
    // every field authored so far — has to keep drawing exactly as it did.
    if (multiline)
    {
        // Measured BEFORE the lines are asked for, because the lines depend on
        // them: this is the one place that knows how wide the text may be.
        wrapWidthPx = ts.x;
        wrapSizePx  = sizePx;
        const std::vector<HE::UITextVisualLine> lines = visualLines();
        HE::UITextLayout lopts;                 // alignH left, alignV TOP
        lopts.alignV = 0;
        const float step = sizePx * lopts.lineSpacing;

        contentHeightPx = static_cast<float>(lines.size() - 1) * step + sizePx;
        viewHeightPx    = std::max(1.0f, ts.y - 2.0f * pad);

        // Keep the caret in view. Only while focused: a reader who scrolled up
        // to look at something should not be yanked back by a caret they are not
        // moving.
        const size_t caretLine = HE::uiVisualLineOfOffset(lines, caret);
        if (st.editing)
        {
            const float top = static_cast<float>(caretLine) * step;
            if (top < scrollPxY)                      scrollPxY = top;
            if (top + step > scrollPxY + viewHeightPx) scrollPxY = top + step - viewHeightPx;
        }
        // Never leave empty space below while text hangs off the top — what
        // happens when the field grows or lines are deleted.
        const float maxScroll = std::max(0.0f, contentHeightPx - viewHeightPx);
        if (scrollPxY > maxScroll) scrollPxY = maxScroll;
        if (scrollPxY < 0.0f)      scrollPxY = 0.0f;

        const float x0   = tp.x;
        const float yTop = px.y + pad - scrollPxY;
        auto lineY = [&](size_t i) { return yTop + static_cast<float>(i) * step; };
        auto shownUpTo = [&](const HE::UITextVisualLine& ln, size_t byte)
        {
            const size_t b = std::min(std::max(byte, ln.begin), ln.end);
            return runWidth(shownFor(text.substr(ln.begin, b - ln.begin)));
        };

        // Selection first, behind the glyphs. Three shapes in one loop: the
        // first line from the anchor to its end, whole lines in between, the
        // last from its start to the caret.
        if (st.editing && selectable && hasSelection())
        {
            const size_t a = selMin(), b = selMax();
            for (size_t i = 0; i < lines.size(); ++i)
            {
                const HE::UITextVisualLine& ln = lines[i];
                if (ln.end < a || ln.begin > b) continue;
                const float xa = shownUpTo(ln, a), xb = shownUpTo(ln, b);
                // A line whose newline is inside the selection gets a small stub
                // past its last character. Without it a selected blank line is
                // invisible, and a multi-line selection looks like several
                // unrelated highlights instead of one run.
                const float stub = (ln.end < b) ? sizePx * 0.35f : 0.0f;
                const float y = lineY(i);
                if (y + sizePx < px.y || y > px.y + px.h) continue;
                quad(out, x0 + xa, y, std::max(1.0f, xb - xa + stub), sizePx, selectionColor);
            }
        }

        // The glyphs, a line at a time, skipping what is scrolled out of sight.
        // Each line is emitted into its OWN one-line rect: handing the whole
        // string to the text layer would let it re-split and re-align, and then
        // two different answers about where line seven is.
        for (size_t i = 0; i < lines.size(); ++i)
        {
            const float y = lineY(i);
            if (y + sizePx < px.y || y > px.y + px.h) continue;
            const HE::UITextVisualLine& ln = lines[i];
            const std::string run = shownFor(text.substr(ln.begin, ln.end - ln.begin));
            if (run.empty()) continue;
            emitTextL(*this, run, { x0, y }, { ts.x, sizePx }, sizePx, textColor, lopts, out);
        }

        // The IME's unfinished text, at the caret, on the caret's line.
        float compW = 0.0f;
        const float caretY = lineY(caretLine);
        const float caretX = shownUpTo(lines[caretLine], caret);
        if (st.editing && !composition.empty())
        {
            HE::UITextLayout copts; copts.alignV = 0;
            compW = runWidth(composition);
            emitTextL(*this, composition, { x0 + caretX, caretY }, { ts.x, sizePx },
                      sizePx, textColor, copts, out);
            quad(out, x0 + caretX, caretY + sizePx - std::max(1.0f, sizePx * 0.06f),
                 std::max(1.0f, compW), std::max(1.0f, sizePx * 0.06f), textColor);
        }

        if (st.editing)
        {
            float cx = caretX;
            if (!composition.empty())
            {
                const size_t upTo = compositionCursor < 0
                    ? composition.size()
                    : std::min(static_cast<size_t>(compositionCursor), composition.size());
                cx += runWidth(composition.substr(0, upTo));
            }
            quad(out, x0 + cx, caretY, std::max(1.0f, sizePx * 0.08f), sizePx, textColor);
        }
        return;
    }

    // ── Sideways scroll ──────────────────────────────────────────────────────
    // Keep the caret inside the visible strip. Without this a field you can type
    // more into than it is wide grows a caret that walks off the right edge and
    // takes the text you are typing with it. Unfocused fields snap back to the
    // start, because that is the half a reader wants to see.
    const float inner = std::max(1.0f, ts.x);
    if (!st.editing) scrollPx = 0.0f;
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
    if (st.editing && selectable && hasSelection())
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
    if (st.editing && !composition.empty())
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
    if (st.editing)
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

// The rows the field shows. One function, three callers — render, caretAtPoint
// and the manager's arrow keys — because the alternative is three answers to
// "where does row two begin", and the caret ends up beside its own glyphs.
std::vector<HE::UITextVisualLine> UITextInput::visualLines() const
{
    // Wrapping needs a width, and the width is only known once the field has
    // been drawn. Not yet drawn, not wrapping, not multiline, or a password
    // (see the header): the authored breaks, exactly as before.
    if (!multiline || !wrapText || password || wrapWidthPx <= 0.0f || wrapSizePx <= 0.0f)
        return HE::uiTextVisualLines(text);
    const HE::BakedUIFont* f = HE::UIFontCache::find(fontAtlasKey);
    const HE::BakedUIFont& font = f ? *f : HE::sharedUIFont();
    return HE::uiTextWrapRanges(font, text, wrapSizePx, wrapWidthPx);
}

// Byte offset in `text` nearest to a point `localX` pixels into the field's
// text area — what a click has to answer to put the caret where it was aimed.
size_t UITextInput::caretAtPoint(float localX, float localY, float pxScaleY) const
{
    const float sizePx = fontSize * pxScaleY;
    const HE::BakedUIFont* f = HE::UIFontCache::find(fontAtlasKey);
    HE::UITextLayout opts;
    auto runWidth = [&](std::string run)
    {
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

    // Which line the click landed on, and where that line starts and ends. A
    // single-line field is the same walk over one range that covers everything,
    // so there is one loop below and not two.
    size_t lo = 0, hi = text.size();
    if (multiline)
    {
        const std::vector<HE::UITextVisualLine> lines = visualLines();
        const float step = sizePx * opts.lineSpacing;
        // Undo the vertical scroll and the top padding the same way render() put
        // them in. A click above the first line lands on it, below the last on
        // that one — dragging off the top of a field should not deselect.
        const float rel = localY + scrollPxY;
        long idx = step > 0.0f ? static_cast<long>(std::floor(rel / step)) : 0;
        if (idx < 0) idx = 0;
        if (idx >= static_cast<long>(lines.size())) idx = static_cast<long>(lines.size()) - 1;
        lo = lines[static_cast<size_t>(idx)].begin;
        hi = lines[static_cast<size_t>(idx)].end;
    }
    else
    {
        // The click arrives relative to the field's text area; a single-line
        // field's text may be scrolled sideways, so undo that first or every
        // click past the scroll point lands on the wrong character.
        localX += scrollPx;
    }

    if (localX <= 0.0f) return lo;
    // Walk the boundaries and take the one whose midpoint the click passed —
    // clicking the left half of a character puts the caret before it.
    size_t best = lo;
    for (size_t i = lo; i < hi; )
    {
        const size_t next = uiUtf8Next(text, i);
        const float a = runWidth(text.substr(lo, i - lo));
        const float b = runWidth(text.substr(lo, next - lo));
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
    // The inset follows the ROUNDING, not a fixed number: on a pill-shaped combo
    // a fixed 6 puts the first letters out over the curve.
    const float pad = contentInset(cornerRadius.x * pxScaleY);
    emitText(*this, currentText(), { px.x + pad, px.y }, { px.w - px.h - pad, px.h },
             fontSize * pxScaleY, textColor, false, out);
    // The indicator. It used to be the LETTER "v" set in the UI font, which is
    // what it looked like: a letter. Now it is the same triangle the designer
    // draws, from the same numbers, and it turns over while the list is down —
    // the one piece of state a combo has that its own rectangle cannot show.
    triangle(out, arrowIn(px), glm::vec4(glm::vec3(textColor), textColor.a * 0.8f), open);
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

void UIGrid::writeJson(nlohmann::json& j) const
{
    UIBoxBase::writeJson(j);
    j["columns"] = columns;
    j["rows"]    = rows;
    j["rowSpacing"] = rowSpacing;
}
void UIGrid::readJson(const nlohmann::json& j)
{
    UIBoxBase::readJson(j);
    if (const auto c = j.find("columns"); c != j.end() && c->is_array())
        columns = c->get<std::vector<std::string>>();
    if (const auto r = j.find("rows"); r != j.end() && r->is_array())
        rows = r->get<std::vector<std::string>>();
    rowSpacing = j.value("rowSpacing", rowSpacing);
    reparse();
}

void UIWrapBox::writeJson(nlohmann::json& j) const
{ UIBoxBase::writeJson(j); j["lineSpacing"] = lineSpacing; }
void UIWrapBox::readJson(const nlohmann::json& j)
{ UIBoxBase::readJson(j); lineSpacing = j.value("lineSpacing", lineSpacing); }

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

// ── ListView ─────────────────────────────────────────────────────────────────
// Three things, in this order: the surface (so the border, the rounding and the
// gradient have a first quad to be stamped onto), the two row states the rows
// themselves cannot know about, and the scrollbar.
//
// The row CONTENT is not drawn here at all — every row is a real widget grafted
// under this element, and the ordinary element loop draws it after this one.
void UIListView::render(const UIWidgetRect& px, const UIElementRenderState&,
                        const HE::UUID& mat, float, std::vector<UIRenderObject>& out) const
{
    // Always emitted, even fully transparent: it is the element's own rectangle
    // and therefore the quad the surface style is applied to. Without it a list
    // could not have a border.
    quad(out, px.x, px.y, px.w, px.h, backColor, mat, 0.0f, textureAssetId);

    const float scaleX = px.w / std::max(1.0f, sizeX);
    const float scaleY = px.h / std::max(1.0f, sizeY);
    const float innerY = px.y + padding * scaleY;
    const float innerH = innerHeight() * scaleY;
    const float rowX   = px.x + padding * scaleX;
    const float rowW   = std::max(0.0f, px.w - 2.0f * padding * scaleX);

    // One row's highlight, clipped to the inner rect BY HAND. The element's own
    // quads are not covered by its clipChildren (that governs its children), so
    // a half-scrolled row would otherwise paint over the border.
    auto rowQuad = [&](int item, const glm::vec4& c)
    {
        if (c.a <= 0.001f || item < 0 || item >= itemCount) return;
        const float top = innerY + (item * rowStep() - scrollOffset) * scaleY;
        const float y0  = std::max(top, innerY);
        const float y1  = std::min(top + rowHeight * scaleY, innerY + innerH);
        if (y1 <= y0 || rowW <= 0.0f) return;
        quad(out, rowX, y0, rowW, y1 - y0, c, HE::UUID{},
             roundedR(rowW, y1 - y0, 3.0f));
    };
    for (const int s : selection) rowQuad(s, rowSelectedColor);
    // Hover under selection would be invisible on the picked row, so it goes on
    // top — and is skipped there, because a lighter version of the same row is
    // noise rather than feedback.
    if (hoveredRow >= 0 && !isSelected(hoveredRow)) rowQuad(hoveredRow, rowHoverColor);

    const float maxOff = maxScroll();
    if (barWidth > 0.0f && maxOff > 0.0f && measuredExtent() > 0.0f)
    {
        const float visibleFrac = std::min(1.0f, innerHeight() / measuredExtent());
        const float trackPx = innerH;
        const float thumbPx = std::max(12.0f, trackPx * visibleFrac);
        const float t = scrollOffset / maxOff;
        quad(out, px.x + px.w - (barWidth + padding) * scaleX,
             innerY + t * (trackPx - thumbPx),
             barWidth * scaleX, thumbPx, barColor, HE::UUID{},
             barWidth * scaleX * 0.5f);
    }
}

// The item count, the offset and the selection are all runtime state: a list
// that reopened pre-scrolled to row 400 of data nobody has loaded yet would be
// showing a picture of the last run.
void UIListView::writeJson(nlohmann::json& j) const
{
    j["rowWidget"] = rowWidget;
    j["rowHeight"] = rowHeight;
    j["padding"] = padding;
    j["spacing"] = spacing;
    j["backColor"] = colJson(backColor);
    j["rowHoverColor"] = colJson(rowHoverColor);
    j["rowSelectedColor"] = colJson(rowSelectedColor);
    j["selectionMode"] = selectionMode;
    j["barWidth"] = barWidth;
    j["barColor"] = colJson(barColor);
}
void UIListView::readJson(const nlohmann::json& j)
{
    rowWidget = j.value("rowWidget", rowWidget);
    rowHeight = j.value("rowHeight", rowHeight);
    padding   = j.value("padding", padding);
    spacing   = j.value("spacing", spacing);
    backColor = colFrom(j.value("backColor", nlohmann::json()), backColor);
    rowHoverColor = colFrom(j.value("rowHoverColor", nlohmann::json()), rowHoverColor);
    rowSelectedColor = colFrom(j.value("rowSelectedColor", nlohmann::json()), rowSelectedColor);
    selectionMode = j.value("selectionMode", selectionMode);
    barWidth = j.value("barWidth", barWidth);
    barColor = colFrom(j.value("barColor", nlohmann::json()), barColor);
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
  j["align"] = alignH; j["alignV"] = alignV;
  // Only when it is on: a label that never heard of markup saves byte-identically.
  if (richText) j["richText"] = true; }
void UIText::readJson(const nlohmann::json& j)
{ text = j.value("text", text); fontSize = j.value("fontSize", fontSize);
  color = colFrom(j.value("color", nlohmann::json()), color);
  wordWrap = j.value("wordWrap", wordWrap);
  // Widgets authored before auto-size keep their hand-set box: defaulting them
  // to true would resize every existing label on load.
  autoSize = j.value("autoSize", false);
  alignH   = j.value("align", alignH);
  // Absent = middle, which is where text always sat before this existed.
  alignV   = j.value("alignV", alignV);
  // Absent = off, which is the only safe default: a label authored before this
  // may hold a literal '<' that markup would eat.
  richText = j.value("richText", false); }

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
    if (multiline)     j["multiline"] = true;
    if (wrapText)      j["wrapText"] = true;
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
    multiline  = j.value("multiline", false);
    wrapText   = j.value("wrapText", false);
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

// ── TabBox ───────────────────────────────────────────────────────────────────

bool UITabBox::hidesChild(const UIWidgetTree& tree, const UIElement& child) const
{
    const int idx = uiChildIndexOf(tree, id, child.id);
    if (idx < 0) return false;                 // not one of my children
    // A Tab Box whose activeTab is out of range shows the FIRST page rather
    // than none: an empty-looking container is a mistake that hides itself,
    // and a stray Set Property should not be able to blank the window.
    const int active = activeTab >= 0 && activeTab < uiChildCountOf(tree, id) ? activeTab : 0;
    return idx != active;
}

void UITabBox::tabLayout(const UIWidgetRect& px, float sizePx, float padPx,
                         const std::vector<std::string>& labels, uint64_t fontAtlasKey,
                         std::vector<float>& outX, std::vector<float>& outW)
{
    outX.clear(); outW.clear();
    outX.reserve(labels.size()); outW.reserve(labels.size());
    const HE::BakedUIFont* f = HE::UIFontCache::find(fontAtlasKey);
    HE::UITextLayout opts;
    float cursor = px.x;
    for (const std::string& label : labels)
    {
        const float textW = label.empty() ? 0.0f
            : (f ? HE::measureUIText(*f, label, sizePx, 0.0f, opts)
                 : HE::measureUIText(label, sizePx, 0.0f, opts)).x;
        // A tab is as wide as what it says. All-equal widths look tidy until one
        // page is called "Settings" and another "A", and then the strip is
        // mostly empty.
        const float w = textW + 2.0f * padPx;
        outX.push_back(cursor);
        outW.push_back(w);
        cursor += w;
    }
}

int UITabBox::tabAtPoint(const UIWidgetRect& px, float sizePx, float padPx, float tabH,
                         const std::vector<std::string>& labels, uint64_t fontAtlasKey,
                         float x, float y)
{
    if (y < px.y || y > px.y + tabH) return -1;      // below the strip is the page
    std::vector<float> tx, tw;
    tabLayout(px, sizePx, padPx, labels, fontAtlasKey, tx, tw);
    for (size_t i = 0; i < tx.size(); ++i)
    {
        // Past the element's own right edge is not a tab, however wide the
        // labels added up to — the strip is clipped there, so a click there
        // lands on nothing rather than on a tab nobody can see.
        if (tx[i] >= px.x + px.w) break;
        if (x >= tx[i] && x < tx[i] + tw[i]) return static_cast<int>(i);
    }
    return -1;
}

const UIPropTable& UITabBox::propTable() const
{
    static const UIPropTable t = {
        uiprop::slot<&UITabBox::activeTab>  ({ "Active Tab", UIPropType::Int }),
        uiprop::slot<&UITabBox::tabHeight>  ({ "Tab Height", UIPropType::Float, 12.0f, 200.0f }),
        uiprop::slot<&UITabBox::fontSize>   ({ "FontSize", UIPropType::Float, 4.0f, 200.0f }),
        uiprop::slot<&UITabBox::tabPadding> ({ "Tab Padding", UIPropType::Float, 0.0f, 100.0f }),
        uiprop::slot<&UITabBox::stripColor> ({ "Strip Color", UIPropType::Color }),
        uiprop::slot<&UITabBox::tabColor>   ({ "Tab Color", UIPropType::Color }),
        uiprop::slot<&UITabBox::activeColor>({ "Active Tab Color", UIPropType::Color }),
        uiprop::slot<&UITabBox::textColor>  ({ "Text Color", UIPropType::Color }),
        uiprop::slot<&UITabBox::pageColor>  ({ "Page Color", UIPropType::Color }),
    };
    return t;
}

void UITabBox::render(const UIWidgetRect& px, const UIElementRenderState&,
                      const HE::UUID&, float pxScaleY, std::vector<UIRenderObject>& out) const
{
    const float tabH   = tabHeight * pxScaleY;
    const float sizePx = fontSize  * pxScaleY;
    const float padPx  = tabPadding * pxScaleY;

    // The page area first, behind everything the pages themselves draw.
    if (pageColor.a > 0.0f)
        quad(out, px.x, px.y + tabH, px.w, std::max(0.0f, px.h - tabH), pageColor);
    quad(out, px.x, px.y, px.w, tabH, stripColor);

    std::vector<float> tx, tw;
    tabLayout(px, sizePx, padPx, tabLabels, fontAtlasKey, tx, tw);
    for (size_t i = 0; i < tx.size(); ++i)
    {
        if (tx[i] >= px.x + px.w) break;                       // clipped away
        const float w = std::min(tw[i], px.x + px.w - tx[i]);  // and the last one trimmed
        const bool  on = static_cast<int>(i) == activeTab;
        quad(out, tx[i], px.y, w, tabH, on ? activeColor : tabColor);
        emitText(*this, tabLabels[i], { tx[i] + padPx, px.y }, { w - 2.0f * padPx, tabH },
                 sizePx, textColor, false, out);
    }
}

void UITabBox::writeJson(nlohmann::json& j) const
{
    j["activeTab"] = activeTab;
    j["tabHeight"] = tabHeight;
    j["fontSize"]  = fontSize;
    j["tabPadding"] = tabPadding;
    j["stripColor"]  = colJson(stripColor);
    j["tabColor"]    = colJson(tabColor);
    j["activeColor"] = colJson(activeColor);
    j["textColor"]   = colJson(textColor);
    j["pageColor"]   = colJson(pageColor);
}

void UITabBox::readJson(const nlohmann::json& j)
{
    activeTab  = j.value("activeTab", 0);
    tabHeight  = j.value("tabHeight", tabHeight);
    fontSize   = j.value("fontSize", fontSize);
    tabPadding = j.value("tabPadding", tabPadding);
    stripColor  = colFrom(j.value("stripColor", nlohmann::json()), stripColor);
    tabColor    = colFrom(j.value("tabColor", nlohmann::json()), tabColor);
    activeColor = colFrom(j.value("activeColor", nlohmann::json()), activeColor);
    textColor   = colFrom(j.value("textColor", nlohmann::json()), textColor);
    pageColor   = colFrom(j.value("pageColor", nlohmann::json()), pageColor);
}

// ── Splitter ─────────────────────────────────────────────────────────────────

bool UISplitter::hidesChild(const UIWidgetTree& tree, const UIElement& child) const
{
    // Exactly two panes. A third child is not an error worth refusing — the
    // designer would have to stop a drop mid-gesture — but it is not drawn and
    // takes no click, which is what makes it obvious rather than confusing.
    return uiChildIndexOf(tree, id, child.id) >= 2;
}

float UISplitter::clampedRatio(float lengthPx) const
{
    if (lengthPx <= 0.0f) return 0.5f;
    const float div = std::min(dividerSize, lengthPx);
    const float usable = std::max(0.0f, lengthPx - div);
    if (usable <= 0.0f) return 0.5f;
    float first = ratio * usable;
    // The minimums, in the order that keeps them honest when they cannot both
    // be met: the FIRST pane's minimum is applied first, then the second's, so
    // a splitter too short for both ends up with the second pane at its floor
    // and the first squeezed — one of them has to give, and picking silently
    // would be the worse answer.
    if (first < minFirst) first = minFirst;
    if (usable - first < minSecond) first = usable - minSecond;
    if (first < 0.0f) first = 0.0f;
    if (first > usable) first = usable;
    return first / usable;
}

const UIPropTable& UISplitter::propTable() const
{
    static const UIPropTable t = {
        uiprop::slot<&UISplitter::vertical>   ({ "Vertical", UIPropType::Bool }),
        uiprop::slot<&UISplitter::ratio>      ({ "Ratio", UIPropType::Float, 0.0f, 1.0f }),
        uiprop::slot<&UISplitter::dividerSize>({ "Divider Size", UIPropType::Float, 1.0f, 40.0f }),
        uiprop::slot<&UISplitter::minFirst>   ({ "Min First", UIPropType::Float, 0.0f, 2000.0f }),
        uiprop::slot<&UISplitter::minSecond>  ({ "Min Second", UIPropType::Float, 0.0f, 2000.0f }),
        uiprop::slot<&UISplitter::dividerColor>({ "Divider Color", UIPropType::Color }),
    };
    return t;
}

void UISplitter::render(const UIWidgetRect& px, const UIElementRenderState& st,
                        const HE::UUID&, float pxScaleY, std::vector<UIRenderObject>& out) const
{
    const float len = vertical ? px.h : px.w;
    const float div = std::min(dividerSize * pxScaleY, len);
    const float first = clampedRatio(len) * std::max(0.0f, len - div);
    // A little brighter while the pointer is on it, so a divider you can grab
    // says so before you try.
    const glm::vec4 c = st.hovered
        ? glm::vec4(glm::min(glm::vec3(dividerColor) + 0.12f, glm::vec3(1.0f)), dividerColor.a)
        : dividerColor;
    if (vertical) quad(out, px.x, px.y + first, px.w, div, c);
    else          quad(out, px.x + first, px.y, div, px.h, c);
}

void UISplitter::writeJson(nlohmann::json& j) const
{
    j["vertical"] = vertical;
    j["ratio"] = ratio;
    j["dividerSize"] = dividerSize;
    j["minFirst"] = minFirst;
    j["minSecond"] = minSecond;
    j["dividerColor"] = colJson(dividerColor);
}

void UISplitter::readJson(const nlohmann::json& j)
{
    vertical = j.value("vertical", false);
    ratio = j.value("ratio", 0.5f);
    dividerSize = j.value("dividerSize", dividerSize);
    minFirst = j.value("minFirst", minFirst);
    minSecond = j.value("minSecond", minSecond);
    dividerColor = colFrom(j.value("dividerColor", nlohmann::json()), dividerColor);
}

} // namespace HE
