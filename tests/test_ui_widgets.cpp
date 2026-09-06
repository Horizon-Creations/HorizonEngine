#include "doctest.h"
#include <nlohmann/json.hpp>
#include <UIWidget/UIElements.h>
#include <UIWidget/UIWidgetTree.h>
#include <UIWidget/UIWidgetBinding.h>
#include <HorizonCode/HorizonCode.h>
#include <Renderer/UIFont.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Scripting/ScriptEngine.h>
#include <HorizonScene/ScriptContext.h>
#include <HorizonScene/ScriptApi.h>
#include <HorizonScene/HorizonWorld.h>
#include <UIWidget/WidgetManager.h>
#include <UIWidget/UIShortcut.h>
#include <HorizonScene/EngineApi.h>
#include <Backends/Software/SoftwareRaster.h>
#include <Hpak/ProjectExporter.h>
#include <Hpak/ProjectConfig.h>
#include <Diagnostics/Log.h>   // the circle guard is checked by what it does NOT say
#include "../src/HE_Editor/UITimelineMath.h"   // header-only: the timeline's seconds ⇄ pixels
#include <cmath>
#include <cstdio>
#include <fstream>
#include <algorithm>
#include <array>
#include <filesystem>

namespace
{
    // Create a widget AND put it on screen. Creating one no longer shows it —
    // Create Widget instantiates the class, Show Widget is what makes it appear,
    // the way every UI framework splits those two. Nearly every test below is
    // about what gets DRAWN or CLICKED, so they want both halves; the handful
    // that test visibility itself call the two steps apart on purpose.
    int createShown(WidgetManager& wm, ContentManager& cm, const char* path)
    {
        const int id = wm.createWidget(cm, path);
        if (id) wm.showWidget(id);
        return id;
    }

    struct TempWidgetDir
    {
        std::filesystem::path path;
        TempWidgetDir()
        {
            path = std::filesystem::temp_directory_path() / "he_test_uiwidgets";
            std::filesystem::remove_all(path);
            std::filesystem::create_directories(path);
        }
        ~TempWidgetDir() { std::filesystem::remove_all(path); }
    };

    int countGlyphs(const std::vector<UIRenderObject>& out)
    {
        int n = 0; for (const auto& ro : out) if (ro.type == 2) ++n; return n;
    }
    int countQuads(const std::vector<UIRenderObject>& out)
    {
        int n = 0; for (const auto& ro : out) if (ro.type == 0) ++n; return n;
    }
}

// ═══ Element hierarchy ═══════════════════════════════════════════════════════

TEST_CASE("makeUIElement produces the right subclass for every type")
{
    for (HE::UIWidgetType t : HE::uiWidgetTypeRegistry())
    {
        auto e = HE::makeUIElement(t);
        REQUIRE(e);
        CHECK(e->type() == t);
        CHECK(std::string(e->typeName()) == HE::uiWidgetTypeName(t));
    }
    CHECK(HE::uiWidgetTypeFromName("Slider") == HE::UIWidgetType::Slider);
    CHECK(HE::uiWidgetTypeFromName("ComboBox") == HE::UIWidgetType::ComboBox);
    CHECK(HE::uiWidgetTypeFromName("nonsense") == HE::UIWidgetType::Panel); // fallback
}

TEST_CASE("clone() is a deep, independent copy")
{
    HE::UIText b;
    b.setProp("Text", HE::UIPropValue::ofString("original"));
    auto c = b.clone();
    c->setProp("Text", HE::UIPropValue::ofString("changed"));
    CHECK(b.getProp("Text").s == "original");
    CHECK(c->getProp("Text").s == "changed");
    CHECK(c->type() == HE::UIWidgetType::Text);
}

TEST_CASE("getProp/setProp round-trip per type")
{
    HE::UICheckBox cb;
    cb.setProp("Checked", HE::UIPropValue::ofBool(true));
    CHECK(cb.getProp("Checked").b);
    CHECK(cb.checked);

    HE::UISlider sl;
    sl.setProp("Min", HE::UIPropValue::ofFloat(10.0f));
    sl.setProp("Max", HE::UIPropValue::ofFloat(20.0f));
    sl.setProp("Value", HE::UIPropValue::ofFloat(15.0f));
    CHECK(sl.getProp("Value").f == doctest::Approx(15.0f));
    CHECK(sl.normalized() == doctest::Approx(0.5f));

    HE::UIComboBox combo;
    combo.setProp("Selected Index", HE::UIPropValue::ofInt(2));
    CHECK(combo.getProp("Selected Index").i == 2);
    CHECK(combo.currentText() == "Option C");

    HE::UITextInput ti;
    ti.setProp("Text", HE::UIPropValue::ofString("hello"));
    CHECK(ti.text == "hello");
}

// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  The property NAMES and TYPES below are an ON-DISK FORMAT.               ║
// ║                                                                          ║
// ║  UI Widget assets are serialized with them and HorizonCode graphs store   ║
// ║  them verbatim in Get/Set Property nodes, so renaming or retyping a row   ║
// ║  silently breaks every saved widget and graph that touches it (a lookup   ║
// ║  miss just reads as the default value — no error anywhere).               ║
// ║                                                                          ║
// ║  If this test fails you changed the format. The fix is to put the name    ║
// ║  back, NOT to update the table below.                                     ║
// ╚══════════════════════════════════════════════════════════════════════════╝
TEST_CASE("element property tables are the pinned on-disk name/type list")
{
    using namespace HE;
    struct Row    { const char* name; UIPropType type; };
    struct Expect { UIWidgetType type; std::vector<Row> props; };

    const std::vector<Expect> kExpected = {
        { UIWidgetType::Panel, {
            { "Color", UIPropType::Color } } },
        { UIWidgetType::Image, {
            { "Tint", UIPropType::Color },
            { "Slice Left", UIPropType::Float },
            { "Slice Top", UIPropType::Float },
            { "Slice Right", UIPropType::Float },
            { "Slice Bottom", UIPropType::Float },
            { "Slice Fill Centre", UIPropType::Bool } } },
        { UIWidgetType::Text, {
            { "Text", UIPropType::String },
            { "FontSize", UIPropType::Float },
            { "Color", UIPropType::Color },
            { "WordWrap", UIPropType::Bool },
            { "AutoSize", UIPropType::Bool },
            // Reads Text as markup instead of as words — off, so every label
            // written before it still shows the '<' it was given.
            { "RichText", UIPropType::Bool },
            // A reader may pick the words up and copy them — off, so nothing
            // written before it starts swallowing the presses meant for what it
            // sits on. Named the way the field names them.
            { "Selectable", UIPropType::Bool },
            { "Selection Color", UIPropType::Color },
            { "Align H", UIPropType::Int },
            { "Align V", UIPropType::Int } } },
        // Three state colours and nothing else: a Button is a surface, and its
        // caption is a child element (Text/FontSize/Text Color went with it).
        { UIWidgetType::Button, {
            { "Normal Color", UIPropType::Color },
            { "Hovered Color", UIPropType::Color },
            { "Pressed Color", UIPropType::Color } } },
        { UIWidgetType::CheckBox, {
            { "Checked", UIPropType::Bool },
            { "Label", UIPropType::String },
            { "FontSize", UIPropType::Float },
            { "Box Color", UIPropType::Color },
            { "Check Color", UIPropType::Color },
            { "Text Color", UIPropType::Color },
            { "Switch", UIPropType::Bool },
            { "AutoSize", UIPropType::Bool } } },
        { UIWidgetType::Slider, {
            { "Value", UIPropType::Float },
            { "Min", UIPropType::Float },
            { "Max", UIPropType::Float },
            { "Track Color", UIPropType::Color },
            { "Fill Color", UIPropType::Color },
            { "Handle Color", UIPropType::Color } } },
        { UIWidgetType::ProgressBar, {
            { "Value", UIPropType::Float },
            { "Indeterminate", UIPropType::Bool },
            { "Back Color", UIPropType::Color },
            { "Fill Color", UIPropType::Color } } },
        { UIWidgetType::TextInput, {
            { "Text", UIPropType::String },
            { "Placeholder", UIPropType::String },
            { "FontSize", UIPropType::Float },
            { "Back Color", UIPropType::Color },
            { "Text Color", UIPropType::Color },
            { "Selection Color", UIPropType::Color },
            { "Max Length", UIPropType::Int },
            { "Password", UIPropType::Bool },
            { "Editable", UIPropType::Bool },
            { "Selectable", UIPropType::Bool },
            { "Multiline", UIPropType::Bool },
            { "Wrap Text", UIPropType::Bool },
            { "Input Filter", UIPropType::Int },
            { "Allowed Characters", UIPropType::String },
            { "Clear Button", UIPropType::Bool },
            { "Steppers", UIPropType::Bool },
            { "Step", UIPropType::Float },
            { "Min Value", UIPropType::Float },
            { "Max Value", UIPropType::Float } } },
        { UIWidgetType::ComboBox, {
            { "Options", UIPropType::StringList },
            { "Selected Index", UIPropType::Int },
            { "FontSize", UIPropType::Float },
            { "Back Color", UIPropType::Color },
            { "Text Color", UIPropType::Color },
            { "Highlight Color", UIPropType::Color },
            { "AutoSize", UIPropType::Bool } } },
        // The layout boxes share one table: the slot algorithm reads these two
        // BY NAME (see boxSlotRect), so renaming them silently un-pads every
        // box in every saved widget.
        { UIWidgetType::VerticalBox, {
            { "Padding", UIPropType::Float },
            { "Spacing", UIPropType::Float },
            { "Size To Content", UIPropType::Bool } } },
        { UIWidgetType::HorizontalBox, {
            { "Padding", UIPropType::Float },
            { "Spacing", UIPropType::Float },
            { "Size To Content", UIPropType::Bool } } },
        { UIWidgetType::ScrollBox, {
            { "Padding", UIPropType::Float },
            { "Spacing", UIPropType::Float },
            { "Size To Content", UIPropType::Bool },
            { "Bar Width", UIPropType::Float },
            { "Bar Color", UIPropType::Color } } },
        { UIWidgetType::WidgetRef, {
            { "Widget", UIPropType::String } } },
        // A Spacer is its rect and nothing else: the size on the box's axis and
        // Slot Fill are base properties, and it has no others by design.
        { UIWidgetType::Spacer, {} },
        // A ListView keeps the container names for Padding and Spacing, and
        // Item Count is here because a graph SETS it — it is runtime state that
        // is still a property, and only the serializer knows the difference.
        { UIWidgetType::ListView, {
            { "Row Widget", UIPropType::String },
            { "Row Height", UIPropType::Float },
            { "Padding", UIPropType::Float },
            { "Spacing", UIPropType::Float },
            { "Back Color", UIPropType::Color },
            { "Row Hover Color", UIPropType::Color },
            { "Row Selected Color", UIPropType::Color },
            { "Selection", UIPropType::Int },
            { "Bar Width", UIPropType::Float },
            { "Bar Color", UIPropType::Color },
            // The header is what turns the same type into a table (plan 13.1):
            // one property and its trimmings, not a second element type.
            { "Show Header", UIPropType::Bool },
            { "Header Height", UIPropType::Float },
            { "Header Color", UIPropType::Color },
            { "Header Text Color", UIPropType::Color },
            { "Header Font Size", UIPropType::Float },
            { "Resizable Columns", UIPropType::Bool },
            { "Column Widths", UIPropType::String },
            { "Sort Column", UIPropType::Int },
            { "Sort Ascending", UIPropType::Bool },
            { "Item Count", UIPropType::Int } } },
        // A WrapBox is a box plus the one number a wrapping row has that a
        // straight one does not: the gap between LINES.
        { UIWidgetType::WrapBox, {
            { "Padding", UIPropType::Float },
            { "Spacing", UIPropType::Float },
            { "Line Spacing", UIPropType::Float },
            { "Size To Content", UIPropType::Bool } } },
        // A Grid's two track lists are its whole vocabulary; Row Spacing is the
        // gap between rows, `Spacing` the one between columns.
        { UIWidgetType::Grid, {
            { "Column Sizes", UIPropType::StringList },
            { "Row Sizes", UIPropType::StringList },
            { "Padding", UIPropType::Float },
            { "Spacing", UIPropType::Float },
            { "Row Spacing", UIPropType::Float },
            { "Size To Content", UIPropType::Bool } } },
        // A Tab Box's PAGES are its children and their names are the labels, so
        // there is no list property here — that is the whole point of the design.
        { UIWidgetType::TabBox, {
            { "Active Tab", UIPropType::Int },
            { "Tab Height", UIPropType::Float },
            { "FontSize", UIPropType::Float },
            { "Tab Padding", UIPropType::Float },
            { "Strip Color", UIPropType::Color },
            { "Tab Color", UIPropType::Color },
            { "Active Tab Color", UIPropType::Color },
            { "Text Color", UIPropType::Color },
            { "Page Color", UIPropType::Color } } },
        { UIWidgetType::Splitter, {
            { "Vertical", UIPropType::Bool },
            { "Ratio", UIPropType::Float },
            { "Divider Size", UIPropType::Float },
            { "Min First", UIPropType::Float },
            { "Min Second", UIPropType::Float },
            { "Divider Color", UIPropType::Color } } },
        // Three ints rather than one date string: a property is written by the
        // editor's number rows and by scripts, and a string would be parsed on
        // every one of those writes. The ISO form is the EVENT's payload.
        { UIWidgetType::DatePicker, {
            { "Year", UIPropType::Int },
            { "Month", UIPropType::Int },
            { "Day", UIPropType::Int },
            { "Monday First", UIPropType::Bool },
            { "FontSize", UIPropType::Float },
            { "Back Color", UIPropType::Color },
            { "Header Color", UIPropType::Color },
            { "Text Color", UIPropType::Color },
            { "Muted Color", UIPropType::Color },
            { "Selected Color", UIPropType::Color },
            { "Hover Color", UIPropType::Color } } },
        { UIWidgetType::ColorPicker, {
            { "Color", UIPropType::Color },
            { "Show Alpha", UIPropType::Bool },
            { "Bar Width", UIPropType::Float },
            { "Gap", UIPropType::Float },
            { "Back Color", UIPropType::Color } } },
        // An Accordion's SECTIONS are its children and their names are the
        // headings, so there is no list property here either — the Tab Box's
        // design, and the whole point of it.
        { UIWidgetType::Accordion, {
            { "Expanded", UIPropType::Int },
            { "Allow Multiple", UIPropType::Bool },
            { "Header Height", UIPropType::Float },
            { "FontSize", UIPropType::Float },
            { "Spacing", UIPropType::Float },
            { "Text Indent", UIPropType::Float },
            { "Header Color", UIPropType::Color },
            { "Open Header Color", UIPropType::Color },
            { "Text Color", UIPropType::Color },
            { "Body Color", UIPropType::Color },
            { "Bar Width", UIPropType::Float },
            { "Bar Color", UIPropType::Color } } },
    };

    // Every registered type is covered, in registry order — a new widget type
    // must land here too, or its property names are unprotected.
    const std::vector<UIWidgetType>& reg = uiWidgetTypeRegistry();
    REQUIRE(kExpected.size() == reg.size());
    for (size_t i = 0; i < reg.size(); ++i) CHECK(kExpected[i].type == reg[i]);

    // Type-specific names must never shadow a base name (getPropAny checks the
    // base list FIRST, so a collision would make the subclass property
    // unreachable through the generic accessors).
    const std::vector<std::string> kBaseNames = {
        "Visible", "Hit Testable", "Position", "Size", "Layer",
        "Hover Cursor", "Material", "Font",
        "Grid Column", "Grid Row", "Column Span", "Row Span" };

    // A value that differs from `cur`, so "did the write land?" is unambiguous.
    auto probe = [](const UIPropValue& cur) {
        UIPropValue v = cur;
        switch (cur.type)
        {
            case UIPropType::Float:      v.f   = cur.f + 3.25f; break;
            case UIPropType::Int:        v.i   = cur.i + 5; break;
            case UIPropType::Bool:       v.b   = !cur.b; break;
            case UIPropType::String:     v.s   = cur.s + "_probe"; break;
            case UIPropType::Color:      v.col = cur.col + glm::vec4(0.125f); break;
            case UIPropType::Vec2:       v.v2  = cur.v2 + glm::vec2(1.5f); break;
            case UIPropType::StringList: v.list = { "probe" }; break;
        }
        return v;
    };
    auto same = [](const UIPropValue& a, const UIPropValue& b) {
        if (a.type != b.type) return false;
        switch (a.type)
        {
            case UIPropType::Float:      return a.f == doctest::Approx(b.f);
            case UIPropType::Int:        return a.i == b.i;
            case UIPropType::Bool:       return a.b == b.b;
            case UIPropType::String:     return a.s == b.s;
            case UIPropType::Color:      return a.col == b.col;
            case UIPropType::Vec2:       return a.v2 == b.v2;
            case UIPropType::StringList: return a.list == b.list;
        }
        return false;
    };

    for (const Expect& ex : kExpected)
    {
        auto e = makeUIElement(ex.type);
        REQUIRE(e != nullptr);
        CAPTURE(e->typeName());

        const std::vector<UIPropDesc> props = e->properties();
        REQUIRE(props.size() == ex.props.size());

        std::vector<std::string> seen;
        for (size_t i = 0; i < props.size(); ++i)
        {
            CAPTURE(ex.props[i].name);
            CHECK(props[i].name == ex.props[i].name);   // name AND order are the format
            CHECK(props[i].type == ex.props[i].type);

            // No duplicate names within a type (a duplicate makes the second row
            // dead — first match wins in every accessor).
            CHECK(std::find(seen.begin(), seen.end(), props[i].name) == seen.end());
            seen.push_back(props[i].name);
            CHECK(std::find(kBaseNames.begin(), kBaseNames.end(), props[i].name) == kBaseNames.end());

            // READABLE with the declared type…
            const UIPropValue cur = e->getProp(props[i].name);
            CHECK(cur.type == props[i].type);
            // …and WRITABLE. A property that is one but not the other is a bug:
            // the editor would show a value a graph cannot set, or vice versa.
            const UIPropValue want = probe(cur);
            e->setProp(props[i].name, want);
            CHECK(same(e->getProp(props[i].name), want));

            // The generic accessors reach the same row.
            CHECK(same(e->getPropAny(props[i].name), want));
        }

        // An unknown name reads as a default value and writes nowhere.
        CHECK(e->getProp("NoSuchProperty").type == UIPropType::Float);
        e->setProp("NoSuchProperty", UIPropValue::ofFloat(1.0f));
        CHECK(e->getProp("NoSuchProperty").f == doctest::Approx(0.0f));
    }
}

TEST_CASE("interactive types declare events; Button fires OnClicked")
{
    HE::UIButton b;
    bool hasClicked = false;
    for (const auto& e : b.events()) if (e.name == "OnClicked") hasClicked = true;
    CHECK(hasClicked);

    HE::UICheckBox cb;
    bool hasCheckChanged = false;
    for (const auto& e : cb.events())
        if (e.name == "OnCheckChanged") { hasCheckChanged = true; CHECK(e.hasArg); CHECK(e.argType == HE::UIPropType::Bool); }
    CHECK(hasCheckChanged);

    // A bar does not change, so its own list is empty. A TEXT has exactly one
    // of its own since rich text: a link inside a label is clicked, and that is
    // a thing only a label can report. It used to be in this line as an example
    // of an element with nothing to say, and that stopped being true.
    CHECK(HE::UIProgressBar{}.events().empty());
    REQUIRE(HE::UIText{}.events().size() == 1);
    CHECK(HE::UIText{}.events()[0].name == "OnLinkClicked");
    CHECK(HE::UIText{}.events()[0].argType == HE::UIPropType::String);
    // …but every element has the base list under it, and since animations
    // exist there is one thing anything can report. This used to say "a Text
    // has no events" and that stopped being true the day a Text could fade.
    bool textCanFinish = false;
    for (const auto& e : HE::UIText{}.allEvents())
        if (e.name == "OnAnimationFinished")
        { textCanFinish = true; CHECK(e.hasArg); CHECK(e.argType == HE::UIPropType::String); }
    CHECK(textCanFinish);
    // The second one of those: anything can be a drop zone, so anything can be
    // told that a file was let go over it.
    bool textCanTakeAFile = false;
    for (const auto& e : HE::UIText{}.allEvents())
        if (e.name == "OnFileDropped")
        { textCanTakeAFile = true; CHECK(e.hasArg); CHECK(e.argType == HE::UIPropType::String); }
    CHECK(textCanTakeAFile);
    // Five on the base — animation, file drop, and the three of the drag — plus
    // whatever the type adds, which for a Text is its link.
    CHECK(HE::UIText{}.allEvents().size() == HE::UIText{}.events().size() + 5);
    CHECK(HE::UIButton{}.allEvents().size() == HE::UIButton{}.events().size() + 5);
    CHECK(HE::UIButton{}.interactive());
    CHECK(!HE::UIText{}.interactive());
}

// ═══ Tree ════════════════════════════════════════════════════════════════════

TEST_CASE("UIWidgetTree add / hierarchy / removeSubtree")
{
    HE::UIWidgetTree t;
    const int panel = t.add(HE::UIWidgetType::Panel);
    REQUIRE(t.find(panel));
    CHECK(t.find(panel)->name == "Panel");

    auto btn = HE::makeUIElement(HE::UIWidgetType::Button);
    btn->parentId = panel;
    const int b = t.add(std::move(btn));
    auto txt = HE::makeUIElement(HE::UIWidgetType::Text);
    txt->parentId = b;
    const int tx = t.add(std::move(txt));

    CHECK(t.childrenOf(0) == std::vector<int>{ panel });
    CHECK(t.childrenOf(panel) == std::vector<int>{ b });
    CHECK(t.isDescendantOf(tx, panel));
    CHECK(!t.isDescendantOf(panel, tx));

    t.removeSubtree(panel);
    CHECK(t.find(panel) == nullptr);
    CHECK(t.find(b) == nullptr);
    CHECK(t.find(tx) == nullptr);
}

TEST_CASE("UIWidgetTree deep copy is independent")
{
    HE::UIWidgetTree a;
    const int b = a.add(HE::UIWidgetType::Text);
    a.find(b)->setProp("Text", HE::UIPropValue::ofString("A"));

    HE::UIWidgetTree copy = a;
    copy.find(b)->setProp("Text", HE::UIPropValue::ofString("B"));
    CHECK(a.find(b)->getProp("Text").s == "A");
    CHECK(copy.find(b)->getProp("Text").s == "B");
}

TEST_CASE("UIWidgetTree JSON round-trip preserves type-specific fields")
{
    HE::UIWidgetTree t;
    t.canvasWidth = 1280.0f;

    const int b = t.add(HE::UIWidgetType::Button);
    t.find(b)->name = "Play";
    t.find(b)->setProp("Normal Color", HE::UIPropValue::ofColor({ 0.9f, 0.1f, 0.2f, 1.0f }));
    t.find(b)->material = "Materials/Glow.hasset";

    const int cb = t.add(HE::UIWidgetType::CheckBox);
    t.find(cb)->parentId = b;
    t.find(cb)->setProp("Checked", HE::UIPropValue::ofBool(true));

    const int sl = t.add(HE::UIWidgetType::Slider);
    t.find(sl)->setProp("Value", HE::UIPropValue::ofFloat(0.75f));

    const int combo = t.add(HE::UIWidgetType::ComboBox);
    { HE::UIPropValue v; v.type = HE::UIPropType::StringList; v.list = { "X", "Y" };
      t.find(combo)->setProp("Options", v); }

    HE::UIWidgetTree r;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), r));
    CHECK(r.canvasWidth == doctest::Approx(1280.0f));
    REQUIRE(r.elements.size() == 4);
    CHECK(r.find(b)->type() == HE::UIWidgetType::Button);
    CHECK(r.find(b)->getProp("Normal Color").col.r == doctest::Approx(0.9f));
    CHECK(r.find(b)->material == "Materials/Glow.hasset");
    CHECK(r.find(cb)->parentId == b);
    CHECK(r.find(cb)->getProp("Checked").b);
    CHECK(r.find(sl)->getProp("Value").f == doctest::Approx(0.75f));
    CHECK(r.find(combo)->getProp("Options").list == std::vector<std::string>{ "X", "Y" });
}

// ═══ HorizonCode ═════════════════════════════════════════════════════════════

using HorizonCode::NodeType;
using HorizonCode::PinType;

TEST_CASE("HorizonCode signatures reflect node instance fields")
{
    HorizonCode::Node ev; ev.type = NodeType::Event; ev.hasArg = true; ev.propType = PinType::Float;
    const auto es = HorizonCode::signatureOf(ev);
    CHECK(es.execOuts.size() == 1);
    CHECK(es.dataOuts.size() == 1);
    CHECK(es.dataOuts[0].type == PinType::Float);

    HorizonCode::Node ev2; ev2.type = NodeType::Event; ev2.hasArg = false;
    CHECK(HorizonCode::signatureOf(ev2).dataOuts.empty());

    HorizonCode::Node br; br.type = NodeType::Branch;
    const auto bs = HorizonCode::signatureOf(br);
    CHECK(bs.execIns.size() == 1);
    CHECK(bs.execOuts.size() == 2);
    CHECK(bs.dataIns.size() == 1);
    CHECK(bs.dataIns[0].type == PinType::Bool);
}

TEST_CASE("HorizonCode connect validates pin direction and type")
{
    HorizonCode::Graph g;
    HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "OnClicked"; ev.elem = 5;
    const int evId = g.addNode(ev);
    HorizonCode::Node set; set.type = NodeType::SetProperty; set.elem = 5; set.s = "Text"; set.propType = PinType::String;
    const int setId = g.addNode(set);
    HorizonCode::Node lit; lit.type = NodeType::ConstString; lit.s = "hi";
    const int litId = g.addNode(lit);
    HorizonCode::Node num; num.type = NodeType::ConstFloat;
    const int numId = g.addNode(num);

    CHECK(g.connect(evId, 0, setId, 0));   // exec → exec
    CHECK(g.connect(litId, 0, setId, 2));  // String → String value-in
    CHECK(!g.connect(numId, 0, setId, 2)); // Float → String rejected

    HorizonCode::Graph r;
    REQUIRE(HorizonCode::fromJson(HorizonCode::toJson(g), r));
    CHECK(r.nodes.size() == 4);
    CHECK(r.links.size() == 2);
}

TEST_CASE("HorizonCode Runner fires generic events into the Context")
{
    HorizonCode::Graph g;
    HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "OnClicked"; ev.elem = 1;
    const int evId = g.addNode(ev);
    HorizonCode::Node lit; lit.type = NodeType::ConstString; lit.s = "clicked";
    const int litId = g.addNode(lit);
    HorizonCode::Node set; set.type = NodeType::SetProperty; set.elem = 2; set.s = "Text"; set.propType = PinType::String;
    const int setId = g.addNode(set);
    REQUIRE(g.connect(evId, 0, setId, 0));
    REQUIRE(g.connect(litId, 0, setId, 2));

    std::string written; int writtenElem = 0; std::string writtenProp;
    HorizonCode::Context ctx;
    ctx.setProperty = [&](int elem, const std::string& prop, const HorizonCode::Value& v)
    { writtenElem = elem; writtenProp = prop; written = v.s; };

    HorizonCode::Runner runner(g, ctx);
    runner.fireEvent("OnClicked", 99); // wrong element
    CHECK(written.empty());
    runner.fireEvent("OnClicked", 1);
    CHECK(written == "clicked");
    CHECK(writtenElem == 2);
    CHECK(writtenProp == "Text");
}

TEST_CASE("HorizonCode Runner: Branch + GetProperty via Context")
{
    HorizonCode::Graph g;
    HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "Tick"; ev.elem = 0;
    const int evId = g.addNode(ev);
    HorizonCode::Node get; get.type = NodeType::GetProperty; get.elem = 1; get.s = "Value"; get.propType = PinType::Float;
    const int getId = g.addNode(get);
    HorizonCode::Node five; five.type = NodeType::ConstFloat; five.f[0] = 5.0f;
    const int fiveId = g.addNode(five);
    HorizonCode::Node gt; gt.type = NodeType::Greater;
    const int gtId = g.addNode(gt);
    HorizonCode::Node br; br.type = NodeType::Branch;
    const int brId = g.addNode(br);
    HorizonCode::Node lit; lit.type = NodeType::ConstString; lit.s = "big";
    const int litId = g.addNode(lit);
    HorizonCode::Node set; set.type = NodeType::SetProperty; set.elem = 1; set.s = "Text"; set.propType = PinType::String;
    const int setId = g.addNode(set);

    REQUIRE(g.connect(evId, 0, brId, 0));
    REQUIRE(g.connect(getId, 0, gtId, 0));
    REQUIRE(g.connect(fiveId, 0, gtId, 1));
    REQUIRE(g.connect(gtId, 2, brId, 3));
    REQUIRE(g.connect(brId, 1, setId, 0));
    REQUIRE(g.connect(litId, 0, setId, 2));

    float propValue = 10.0f;
    std::string written;
    HorizonCode::Context ctx;
    ctx.getProperty = [&](int, const std::string&){ return HorizonCode::Value::ofFloat(propValue); };
    ctx.setProperty = [&](int, const std::string&, const HorizonCode::Value& v){ written = v.s; };

    HorizonCode::Runner runner(g, ctx);
    runner.fireEvent("Tick", 0);
    CHECK(written == "big");
    written.clear();
    propValue = 2.0f;
    runner.fireEvent("Tick", 0);
    CHECK(written.empty());
}

TEST_CASE("HorizonCode functions honor the access modifier")
{
    HorizonCode::Graph g;
    HorizonCode::Node pub; pub.type = NodeType::FunctionEntry; pub.s = "Open"; pub.access = 0;
    const int pubId = g.addNode(pub);
    HorizonCode::Node priv; priv.type = NodeType::FunctionEntry; priv.s = "Secret"; priv.access = 1;
    g.addNode(priv);
    HorizonCode::Node show; show.type = NodeType::ShowSelf;
    const int showId = g.addNode(show);
    REQUIRE(g.connect(pubId, 0, showId, 0));

    bool shown = false;
    HorizonCode::Context ctx;
    ctx.showSelf = [&]{ shown = true; };
    HorizonCode::Runner runner(g, ctx);

    CHECK(!runner.callFunction("Secret", true));
    CHECK(runner.callFunction("Secret", false));
    CHECK(!runner.callFunction("Missing", true));
    CHECK(runner.callFunction("Open", true));
    CHECK(shown);
}

// ═══ WidgetManager ═══════════════════════════════════════════════════════════

namespace
{
HE::UUID registerWidget(ContentManager& cm, const HE::UIWidgetTree& tree,
                        const HorizonCode::Graph* graph = nullptr,
                        const char* path = "mem://w.hasset")
{
    UIWidgetAsset a;
    a.treeJson = HE::uiWidgetTreeToJson(tree);
    if (graph) a.graphJson = HorizonCode::toJson(*graph);
    a.path = path;
    return cm.registerWidget(std::move(a));
}
}

TEST_CASE("WidgetManager lifecycle and z-order")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    t.add(HE::UIWidgetType::Button);
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    CHECK(wm.isAlive(id));
    CHECK(wm.isVisible(id));
    CHECK(wm.count() == 1);

    wm.setZOrder(id, 7);
    CHECK(wm.zOrder(id) == 7);
    wm.hideWidget(id);
    CHECK(!wm.isVisible(id));

    std::vector<UIRenderObject> out;
    wm.extract(1920.0f, 1080.0f, out);
    CHECK(out.empty());
    wm.showWidget(id);
    out.clear();
    wm.extract(1920.0f, 1080.0f, out);
    CHECK(!out.empty());

    wm.destroyWidget(id);
    CHECK(!wm.isAlive(id));
    CHECK(wm.createWidget(cm, "mem://missing.hasset") == 0);
}

TEST_CASE("HorizonWorld: injected app-level WidgetManager persists across clear()")
{
    // The game's GameInstance UI lives in an APP-LEVEL WidgetManager that each
    // world borrows (setWidgetManager). A world clear()/scene switch must NOT drop
    // it — a HUD created in OnInit stays up — whereas a world-OWNED WM clears as
    // before (PIE stop / scene load discard play-created widgets).
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t; t.add(HE::UIWidgetType::Button);
    registerWidget(cm, t);

    // World-owned WM: created in the world, dropped with it.
    {
        HorizonWorld w;
        REQUIRE(w.widgets().createWidget(cm, "mem://w.hasset") != 0);
        CHECK(w.widgets().count() == 1);
        w.clear();
        CHECK(w.widgets().count() == 0);
    }

    // Injected app-level WM: identity preserved, survives the world's clear().
    {
        WidgetManager app;
        HorizonWorld w;
        w.setWidgetManager(&app);
        CHECK(&w.widgets() == &app);
        REQUIRE(w.widgets().createWidget(cm, "mem://w.hasset") != 0);
        CHECK(app.count() == 1);
        w.clear();
        CHECK(&w.widgets() == &app);   // still the same external WM
        CHECK(app.count() == 1);       // app-level UI NOT dropped by the world
    }
}

TEST_CASE("WidgetManager renders every element type")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    for (HE::UIWidgetType ty : HE::uiWidgetTypeRegistry()) t.add(ty);
    registerWidget(cm, t);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    std::vector<UIRenderObject> out;
    wm.extract(1920.0f, 1080.0f, out);
    CHECK(countQuads(out) > 0);
    CHECK(countGlyphs(out) > 0);
}

TEST_CASE("WidgetManager button click fires OnClicked -> SetProperty")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    const int txt = t.add(HE::UIWidgetType::Text);
    t.find(txt)->setProp("Text", HE::UIPropValue::ofString(""));
    const int btn = t.add(HE::UIWidgetType::Button);
    t.find(btn)->setProp("Text", HE::UIPropValue::ofString(""));
    HE::uiSetAnchorPreset(*t.find(btn), 0);
    t.find(btn)->pivotX = 0; t.find(btn)->pivotY = 0;
    t.find(btn)->posX = 0; t.find(btn)->posY = 0;
    t.find(btn)->sizeX = 200; t.find(btn)->sizeY = 50;

    HorizonCode::Graph g;
    HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "OnClicked"; ev.elem = btn;
    const int evId = g.addNode(ev);
    HorizonCode::Node lit; lit.type = NodeType::ConstString; lit.s = "OK";
    const int litId = g.addNode(lit);
    HorizonCode::Node set; set.type = NodeType::SetProperty; set.elem = txt; set.s = "Text"; set.propType = PinType::String;
    const int setId = g.addNode(set);
    REQUIRE(g.connect(evId, 0, setId, 0));
    REQUIRE(g.connect(litId, 0, setId, 2));
    registerWidget(cm, t, &g);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);

    std::vector<UIRenderObject> out;
    wm.extract(1920.0f, 1080.0f, out);
    CHECK(countGlyphs(out) == 0);

    CHECK(wm.processPointer(1920.0f, 1080.0f, 100.0f, 25.0f, true,  true));
    CHECK(wm.processPointer(1920.0f, 1080.0f, 100.0f, 25.0f, false, true));

    out.clear();
    wm.extract(1920.0f, 1080.0f, out);
    CHECK(countGlyphs(out) == 2); // "OK"
}

// A Button is a surface and its caption is a child ON it. Clicking the caption
// is clicking the button — the pointer goes to the topmost element, and from
// there the event bubbles UP to the first thing that reacts. Without that, a
// button with a label is a button with a hole in it.
TEST_CASE("A click on a button's child is a click on the button")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    const int btn = t.add(HE::UIWidgetType::Button);
    HE::uiSetAnchorPreset(*t.find(btn), 0);
    t.find(btn)->pivotX = 0; t.find(btn)->pivotY = 0;
    t.find(btn)->posX = 0;   t.find(btn)->posY = 0;
    t.find(btn)->sizeX = 200; t.find(btn)->sizeY = 50;
    // The caption, stretched across the whole button and hit-testable — so it
    // really is the element the pointer lands on.
    const int cap = t.add(HE::UIWidgetType::Text);
    t.find(cap)->parentId = btn;
    HE::uiSetAnchorPreset(*t.find(cap), 15);
    t.find(cap)->posX = t.find(cap)->posY = 0.0f;
    t.find(cap)->sizeX = t.find(cap)->sizeY = 0.0f;
    t.find(cap)->hitTestable = true;
    t.find(cap)->setProp("Text", HE::UIPropValue::ofString(""));
    t.find(cap)->setProp("AutoSize", HE::UIPropValue::ofBool(false));

    HorizonCode::Graph g;
    HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "OnClicked"; ev.elem = btn;
    const int evId = g.addNode(ev);
    HorizonCode::Node lit; lit.type = NodeType::ConstString; lit.s = "OK";
    const int litId = g.addNode(lit);
    HorizonCode::Node set; set.type = NodeType::SetProperty; set.elem = cap;
    set.s = "Text"; set.propType = PinType::String;
    const int setId = g.addNode(set);
    REQUIRE(g.connect(evId, 0, setId, 0));
    REQUIRE(g.connect(litId, 0, setId, 2));
    registerWidget(cm, t, &g);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);

    // Dead centre of the button, which is dead centre of the caption too.
    CHECK(wm.processPointer(1920.0f, 1080.0f, 100.0f, 25.0f, true,  true));
    CHECK(wm.processPointer(1920.0f, 1080.0f, 100.0f, 25.0f, false, true));

    std::vector<UIRenderObject> out;
    wm.extract(1920.0f, 1080.0f, out);
    CHECK(countGlyphs(out) == 2); // "OK" — the button fired
}

// The other half of the same rule: topmost wins, so something drawn OVER a
// button takes the pointer even when it does nothing with it. A menu that
// covers the screen must not be a menu you can click straight through.
TEST_CASE("An element drawn over a button swallows the click")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    const int txt = t.add(HE::UIWidgetType::Text);
    t.find(txt)->setProp("Text", HE::UIPropValue::ofString(""));
    const int btn = t.add(HE::UIWidgetType::Button);
    HE::uiSetAnchorPreset(*t.find(btn), 0);
    t.find(btn)->pivotX = 0; t.find(btn)->pivotY = 0;
    t.find(btn)->posX = 0;   t.find(btn)->posY = 0;
    t.find(btn)->sizeX = 200; t.find(btn)->sizeY = 50;
    // A sibling added AFTER the button: same depth, so it paints on top of it.
    const int cover = t.add(HE::UIWidgetType::Panel);
    HE::uiSetAnchorPreset(*t.find(cover), 0);
    t.find(cover)->pivotX = 0; t.find(cover)->pivotY = 0;
    t.find(cover)->posX = 0;   t.find(cover)->posY = 0;
    t.find(cover)->sizeX = 200; t.find(cover)->sizeY = 50;

    HorizonCode::Graph g;
    HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "OnClicked"; ev.elem = btn;
    const int evId = g.addNode(ev);
    HorizonCode::Node lit; lit.type = NodeType::ConstString; lit.s = "OK";
    const int litId = g.addNode(lit);
    HorizonCode::Node set; set.type = NodeType::SetProperty; set.elem = txt;
    set.s = "Text"; set.propType = PinType::String;
    const int setId = g.addNode(set);
    REQUIRE(g.connect(evId, 0, setId, 0));
    REQUIRE(g.connect(litId, 0, setId, 2));
    registerWidget(cm, t, &g);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);

    // The pointer IS over the UI (the panel took it) …
    CHECK(wm.processPointer(1920.0f, 1080.0f, 100.0f, 25.0f, true,  true));
    CHECK(wm.processPointer(1920.0f, 1080.0f, 100.0f, 25.0f, false, true));
    // … and the button underneath never heard about it.
    std::vector<UIRenderObject> out;
    wm.extract(1920.0f, 1080.0f, out);
    CHECK(countGlyphs(out) == 0);

    // Turn the cover transparent to the pointer and the button is reachable
    // again — that is what the flag is for.
    wm.clear();
    t.find(cover)->hitTestable = false;
    registerWidget(cm, t, &g);
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    CHECK(wm.processPointer(1920.0f, 1080.0f, 100.0f, 25.0f, true,  true));
    CHECK(wm.processPointer(1920.0f, 1080.0f, 100.0f, 25.0f, false, true));
    out.clear();
    wm.extract(1920.0f, 1080.0f, out);
    CHECK(countGlyphs(out) == 2);
}

// A drag belongs to whatever the press landed on, all the way to the release.
// Now that every hit-testable element blocks the pointer, one lying across the
// track would otherwise take it away mid-drag and the handle would stop dead
// under the cursor.
TEST_CASE("A drag is not stolen by something the pointer crosses")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int sl = t.add(HE::UIWidgetType::Slider);
    { HE::UIElement& e = *t.find(sl);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 30.0f;
      e.setProp("Value", HE::UIPropValue::ofFloat(0.0f)); }
    // A label lying across the right half of the track, added after the slider
    // so it paints — and hit-tests — on top of it.
    const int over = t.add(HE::UIWidgetType::Text);
    { HE::UIElement& e = *t.find(over);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 100.0f; e.posY = 0.0f; e.sizeX = 100.0f; e.sizeY = 30.0f;
      e.hitTestable = true;
      e.setProp("AutoSize", HE::UIPropValue::ofBool(false)); }
    registerWidget(cm, t);

    WidgetManager wm;
    const int wid = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(wid != 0);

    // Press on the left half (the slider is the topmost there) …
    wm.processPointer(400.0f, 400.0f, 20.0f, 15.0f, true, true);
    // … then drag right, under the label, without letting go.
    wm.processPointer(400.0f, 400.0f, 150.0f, 15.0f, true, true);
    const HE::UIWidgetTree* live = wm.tree(wid);
    REQUIRE(live != nullptr);
    CHECK(live->find(sl)->getProp("Value").f == doctest::Approx(0.75f));
    wm.processPointer(400.0f, 400.0f, 150.0f, 15.0f, false, true);
}

// ── Building the interface while it runs ─────────────────────────────────────
// The one thing the widget system could not do: every element had to exist in
// the designer, so a list of unknown length was N pre-made rows with a ceiling.
TEST_CASE("A widget can be grafted into a running one, and taken out again")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    // The row, its own asset with its own label — authored once, used N times.
    HE::UIWidgetTree row;
    row.canvasWidth = 400.0f; row.canvasHeight = 40.0f;
    const int label = row.add(HE::UIWidgetType::Text);
    row.find(label)->name = "Label";
    row.find(label)->setProp("Text", HE::UIPropValue::ofString("row"));
    registerWidget(cm, row, nullptr, "mem://row.hasset");

    // The page: a vertical box named "List" and nothing else.
    HE::UIWidgetTree page;
    page.canvasWidth = 400.0f; page.canvasHeight = 600.0f;
    page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int box = page.add(HE::UIWidgetType::VerticalBox);
    {
        HE::UIElement& e = *page.find(box);
        e.name = "List";
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 400.0f; e.sizeY = 600.0f;
    }
    registerWidget(cm, page, nullptr, "mem://page.hasset");

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://page.hasset");
    REQUIRE(id != 0);
    const std::size_t bare = wm.tree(id)->elements.size();

    // Three rows, added one at a time.
    std::vector<int> rows;
    for (int i = 0; i < 3; ++i)
    {
        const int child = (int)wm.addChild(cm, id, "List", "mem://row.hasset");
        CHECK(child != 0);
        rows.push_back(child);
    }
    // Each brought its own elements in: the ref it hangs under plus the row.
    CHECK(wm.tree(id)->elements.size() > bare + 3);
    // …and every one of them is its own instance, not three names for one.
    CHECK(rows[0] != rows[1]);
    CHECK(rows[1] != rows[2]);

    // The box stacks them: three rows, three different y positions, in order.
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 600.0f, out);
    const int glyphs = countGlyphs(out);
    CHECK(glyphs > 0);          // "row" three times over

    // Out again, by the id the add returned.
    CHECK(wm.removeChild(id, rows[1]));
    CHECK_FALSE(wm.removeChild(id, rows[1]));   // …and only once
    const std::size_t afterOne = wm.tree(id)->elements.size();

    out.clear();
    wm.extract(400.0f, 600.0f, out);
    CHECK(countGlyphs(out) < glyphs);           // one row's text is gone

    // The rest at once.
    CHECK(wm.clearChildren(id, "List") == 2);
    CHECK(wm.tree(id)->elements.size() == bare);
    CHECK(afterOne > bare);
    out.clear();
    wm.extract(400.0f, 600.0f, out);
    CHECK(countGlyphs(out) == 0);
}

TEST_CASE("Grafting refuses what it cannot do, and leaves nothing behind")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree page;
    const int box = page.add(HE::UIWidgetType::VerticalBox);
    page.find(box)->name = "List";
    registerWidget(cm, page, nullptr, "mem://page.hasset");

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://page.hasset");
    REQUIRE(id != 0);
    const std::size_t bare = wm.tree(id)->elements.size();

    // No such asset, no such element, no such widget — all three answer 0, and
    // none of them may leave an empty slot taking up space in the box.
    CHECK(wm.addChild(cm, id, "List", "mem://missing.hasset") == 0);
    CHECK(wm.addChild(cm, id, "Nope", "mem://page.hasset") == 0);
    CHECK(wm.addChild(cm, 999, "List", "mem://page.hasset") == 0);
    CHECK(wm.tree(id)->elements.size() == bare);

    // A widget that grafts ITSELF is a circle, and the graft guard catches it.
    CHECK(wm.addChild(cm, id, "List", "mem://page.hasset") == 0);
    CHECK(wm.tree(id)->elements.size() == bare);
}

// The designer's containment rule. A Button says yes here, which is what lets a
// caption, an icon and a badge live on the same button.
TEST_CASE("Exactly the container types accept children")
{
    const std::vector<HE::UIWidgetType> containers = {
        HE::UIWidgetType::Panel, HE::UIWidgetType::Button,
        HE::UIWidgetType::VerticalBox, HE::UIWidgetType::HorizontalBox,
        HE::UIWidgetType::ScrollBox, HE::UIWidgetType::WrapBox,
        HE::UIWidgetType::Grid,
        // A Tab Box takes children because its children ARE its pages, and a
        // Splitter because its two are its panes.
        // …and an Accordion because its children are its sections.
        HE::UIWidgetType::TabBox, HE::UIWidgetType::Splitter,
        HE::UIWidgetType::Accordion };
    for (HE::UIWidgetType ty : HE::uiWidgetTypeRegistry())
    {
        auto e = HE::makeUIElement(ty);
        REQUIRE(e);
        const bool want = std::find(containers.begin(), containers.end(), ty)
                          != containers.end();
        INFO("type ", e->typeName());
        CHECK(e->acceptsChildren() == want);
        // Anything that PLACES its children must obviously also take them —
        // except a ListView, which places rows it MAKES ITSELF from its template.
        // Dropping an element into one in the designer would create something
        // the next sync throws away, so it says no on purpose.
        if (e->laysOutChildren() && ty != HE::UIWidgetType::ListView)
            CHECK(e->acceptsChildren());
    }
}

TEST_CASE("WidgetManager checkbox click toggles its checked visual")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    const int cb = t.add(HE::UIWidgetType::CheckBox);
    t.find(cb)->setProp("Label", HE::UIPropValue::ofString(""));
    HE::uiSetAnchorPreset(*t.find(cb), 0);
    t.find(cb)->pivotX = 0; t.find(cb)->pivotY = 0;
    t.find(cb)->posX = 0; t.find(cb)->posY = 0; t.find(cb)->sizeX = 200; t.find(cb)->sizeY = 28;
    registerWidget(cm, t);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);

    std::vector<UIRenderObject> out;
    wm.extract(1920.0f, 1080.0f, out);
    const int base = countQuads(out);

    wm.processPointer(1920.0f, 1080.0f, 10.0f, 14.0f, true,  true);
    wm.processPointer(1920.0f, 1080.0f, 10.0f, 14.0f, false, true);
    out.clear();
    wm.extract(1920.0f, 1080.0f, out);
    CHECK(countQuads(out) == base + 1);
}

TEST_CASE("WidgetManager routes public function calls only")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    const int txt = t.add(HE::UIWidgetType::Text);
    t.find(txt)->setProp("Text", HE::UIPropValue::ofString(""));

    HorizonCode::Graph g;
    HorizonCode::Node fn; fn.type = NodeType::FunctionEntry; fn.s = "Fill"; fn.access = 0;
    const int fnId = g.addNode(fn);
    HorizonCode::Node priv; priv.type = NodeType::FunctionEntry; priv.s = "Hidden"; priv.access = 1;
    g.addNode(priv);
    HorizonCode::Node lit; lit.type = NodeType::ConstString; lit.s = "ABC";
    const int litId = g.addNode(lit);
    HorizonCode::Node set; set.type = NodeType::SetProperty; set.elem = txt; set.s = "Text"; set.propType = PinType::String;
    const int setId = g.addNode(set);
    REQUIRE(g.connect(fnId, 0, setId, 0));
    REQUIRE(g.connect(litId, 0, setId, 2));
    registerWidget(cm, t, &g);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    CHECK(!wm.callFunction(id, "Hidden"));
    CHECK(!wm.callFunction(id, "Nope"));
    CHECK(wm.callFunction(id, "Fill"));

    std::vector<UIRenderObject> out;
    wm.extract(1920.0f, 1080.0f, out);
    CHECK(countGlyphs(out) == 3); // "ABC"
}

// ═══ Scripting ═══════════════════════════════════════════════════════════════

TEST_CASE("Lua creates, drives and destroys a widget; world.clear() cleans up")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    const int txt = t.add(HE::UIWidgetType::Text);
    t.find(txt)->setProp("Text", HE::UIPropValue::ofString(""));
    HorizonCode::Graph g;
    HorizonCode::Node fn; fn.type = NodeType::FunctionEntry; fn.s = "Go"; fn.access = 0;
    const int fnId = g.addNode(fn);
    HorizonCode::Node lit; lit.type = NodeType::ConstString; lit.s = "ok";
    const int litId = g.addNode(lit);
    HorizonCode::Node set; set.type = NodeType::SetProperty; set.elem = txt; set.s = "Text"; set.propType = PinType::String;
    const int setId = g.addNode(set);
    REQUIRE(g.connect(fnId, 0, setId, 0));
    REQUIRE(g.connect(litId, 0, setId, 2));
    registerWidget(cm, t, &g, "mem://hud.hasset");

    HorizonWorld world;
    ScriptContext ctx(world);
    ctx.setContentManager(&cm);
    REQUIRE(ctx.loadScript("hud", R"lua(
local M = {}
function M.onStart(self)
    w = horizon.createWidget("mem://hud.hasset")
    -- Creating instantiates; showing is the second step (see createShown above).
    horizon.showWidget(w)
    horizon.setWidgetZOrder(w, 3)
    okPub  = horizon.callWidgetFunction(w, "Go")
    okNone = horizon.callWidgetFunction(w, "Missing")
    vis    = horizon.isWidgetVisible(w)
end
return M
)lua", HE::ScriptLanguage::Lua));
    auto e = world.createEntity("driver");
    const auto inst = ctx.createInstance("hud", e, HE::ScriptLanguage::Lua);
    REQUIRE(inst != ScriptEngine::kInvalidInstance);
    REQUIRE(ctx.callOnStart(inst));

    ScriptEngine& lua = ctx.engine();
    const int w = (int)lua.getGlobalNumber("w");
    CHECK(w != 0);
    CHECK(world.widgets().isAlive(w));
    CHECK(world.widgets().zOrder(w) == 3);
    CHECK(lua.exec("assert(okPub == true)"));
    CHECK(lua.exec("assert(okNone == false)"));
    CHECK(lua.exec("assert(vis == true)"));

    std::vector<UIRenderObject> out;
    world.widgets().extract(1920.0f, 1080.0f, out);
    CHECK(countGlyphs(out) == 2); // "ok"

    lua.exec("horizon.createWidget('mem://hud.hasset')");
    CHECK(world.widgets().count() == 2);
    world.clear();
    CHECK(world.widgets().count() == 0);
}

TEST_CASE("showCursor/hideCursor route through the host-app hook")
{
    bool visible = false; int calls = 0;
    ScriptApi::setCursorHook([&](bool show){ visible = show; ++calls; });

    HorizonWorld world;
    ScriptContext ctx(world);
    REQUIRE(ctx.loadScript("cur", R"lua(
local M = {}
function M.onStart(self) horizon.showCursor() end
function M.onUpdate(self, dt) horizon.hideCursor() end
return M
)lua", HE::ScriptLanguage::Lua));
    auto e = world.createEntity("driver");
    const auto inst = ctx.createInstance("cur", e, HE::ScriptLanguage::Lua);
    REQUIRE(ctx.callOnStart(inst));
    CHECK(visible); CHECK(calls == 1);
    REQUIRE(ctx.callOnUpdate(inst, 0.016f));
    CHECK(!visible); CHECK(calls == 2);

    ScriptApi::setCursorHook(nullptr);
    ScriptApi::setCursorVisible(true);
    CHECK(calls == 2);
}

// ═══ HorizonCode variables ═══════════════════════════════════════════════════

TEST_CASE("HorizonCode variables round-trip through JSON")
{
    HorizonCode::Graph g;
    HorizonCode::Variable a; a.name = "score"; a.type = PinType::Int; a.f[0] = 5.0f;
    HorizonCode::Variable b; b.name = "label"; b.type = PinType::String; b.s = "hi";
    g.variables.push_back(a);
    g.variables.push_back(b);

    HorizonCode::Graph r;
    REQUIRE(HorizonCode::fromJson(HorizonCode::toJson(g), r));
    REQUIRE(r.variables.size() == 2);
    const HorizonCode::Variable* rs = r.findVariable("score");
    REQUIRE(rs);
    CHECK(rs->type == PinType::Int);
    CHECK(rs->f[0] == doctest::Approx(5.0f));
    const HorizonCode::Variable* rl = r.findVariable("label");
    REQUIRE(rl);
    CHECK(rl->type == PinType::String);
    CHECK(rl->s == "hi");
    // Default value helper.
    CHECK(HorizonCode::variableDefaultValue(*rs).i == 5);
    CHECK(HorizonCode::variableDefaultValue(*rl).s == "hi");
}

TEST_CASE("HorizonCode Runner reads/writes variables via the Context")
{
    HorizonCode::Graph g;
    // Event "Set" → SetVariable("x", 42).
    HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "Set"; ev.elem = 0;
    const int evId = g.addNode(ev);
    HorizonCode::Node lit; lit.type = NodeType::ConstFloat; lit.f[0] = 42.0f;
    const int litId = g.addNode(lit);
    HorizonCode::Node setv; setv.type = NodeType::SetVariable; setv.s = "x"; setv.propType = PinType::Float;
    const int setId = g.addNode(setv);
    REQUIRE(g.connect(evId, 0, setId, 0));
    REQUIRE(g.connect(litId, 0, setId, 2)); // value in

    // Event "Read" → SetProperty(1,"Text", ToString(GetVariable("x"))).
    HorizonCode::Node ev2; ev2.type = NodeType::Event; ev2.s = "Read"; ev2.elem = 0;
    const int ev2Id = g.addNode(ev2);
    HorizonCode::Node getv; getv.type = NodeType::GetVariable; getv.s = "x"; getv.propType = PinType::Float;
    const int getId = g.addNode(getv);
    HorizonCode::Node ts; ts.type = NodeType::ToString;
    const int tsId = g.addNode(ts);
    HorizonCode::Node setp; setp.type = NodeType::SetProperty; setp.elem = 1; setp.s = "Text"; setp.propType = PinType::String;
    const int setpId = g.addNode(setp);
    REQUIRE(g.connect(ev2Id, 0, setpId, 0));
    REQUIRE(g.connect(getId, 0, tsId, 0));
    REQUIRE(g.connect(tsId, 1, setpId, 2)); // ToString: dataIn 0, dataOut 1

    std::unordered_map<std::string, HorizonCode::Value> store;
    std::string written;
    HorizonCode::Context ctx;
    ctx.getVariable = [&](const std::string& v){ auto it = store.find(v); return it != store.end() ? it->second : HorizonCode::Value{}; };
    ctx.setVariable = [&](const std::string& v, const HorizonCode::Value& val){ store[v] = val; };
    ctx.setProperty = [&](int, const std::string&, const HorizonCode::Value& val){ written = val.s; };

    HorizonCode::Runner runner(g, ctx);
    runner.fireEvent("Read", 0);
    CHECK(written == "0");     // unset variable → default 0
    runner.fireEvent("Set", 0);
    runner.fireEvent("Read", 0);
    CHECK(written == "42");
}

TEST_CASE("WidgetManager variables persist across separate function calls")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    const int txt = t.add(HE::UIWidgetType::Text);
    t.find(txt)->setProp("Text", HE::UIPropValue::ofString(""));

    HorizonCode::Graph g;
    HorizonCode::Variable msg; msg.name = "msg"; msg.type = PinType::String; msg.s = "";
    g.variables.push_back(msg);

    // "SetIt": SetVariable("msg", "hello").
    HorizonCode::Node fn1; fn1.type = NodeType::FunctionEntry; fn1.s = "SetIt"; fn1.access = 0;
    const int fn1Id = g.addNode(fn1);
    HorizonCode::Node lit; lit.type = NodeType::ConstString; lit.s = "hello";
    const int litId = g.addNode(lit);
    HorizonCode::Node sv; sv.type = NodeType::SetVariable; sv.s = "msg"; sv.propType = PinType::String;
    const int svId = g.addNode(sv);
    REQUIRE(g.connect(fn1Id, 0, svId, 0));
    REQUIRE(g.connect(litId, 0, svId, 2));

    // "ShowIt": SetProperty(txt,"Text", GetVariable("msg")).
    HorizonCode::Node fn2; fn2.type = NodeType::FunctionEntry; fn2.s = "ShowIt"; fn2.access = 0;
    const int fn2Id = g.addNode(fn2);
    HorizonCode::Node gv; gv.type = NodeType::GetVariable; gv.s = "msg"; gv.propType = PinType::String;
    const int gvId = g.addNode(gv);
    HorizonCode::Node sp; sp.type = NodeType::SetProperty; sp.elem = txt; sp.s = "Text"; sp.propType = PinType::String;
    const int spId = g.addNode(sp);
    REQUIRE(g.connect(fn2Id, 0, spId, 0));
    REQUIRE(g.connect(gvId, 0, spId, 2));

    registerWidget(cm, t, &g);
    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);

    std::vector<UIRenderObject> out;
    wm.callFunction(id, "ShowIt");                 // reads the default ""
    wm.extract(1920.0f, 1080.0f, out);
    CHECK(countGlyphs(out) == 0);

    CHECK(wm.callFunction(id, "SetIt"));           // writes msg = "hello"
    CHECK(wm.callFunction(id, "ShowIt"));          // reads it back in a SEPARATE run
    out.clear();
    wm.extract(1920.0f, 1080.0f, out);
    CHECK(countGlyphs(out) == 5);                  // "hello" persisted across calls
}

// ── Shared base properties (getPropAny/setPropAny/allProperties) ─────────────

TEST_CASE("base properties are gettable and settable on every element type")
{
    using namespace HE;
    for (UIWidgetType t : uiWidgetTypeRegistry())
    {
        auto e = makeUIElement(t);
        REQUIRE(e != nullptr);

        // Visible / Hit Testable round-trip
        e->setPropAny("Visible", UIPropValue::ofBool(false));
        CHECK(!e->getPropAny("Visible").b);
        CHECK(!e->visible);
        e->setPropAny("Hit Testable", UIPropValue::ofBool(false));
        CHECK(!e->getPropAny("Hit Testable").b);
        CHECK(!e->hitTestable);

        // Position / Size / Layer
        e->setPropAny("Position", UIPropValue::ofVec2({ 11.0f, 22.0f }));
        CHECK(e->posX == doctest::Approx(11.0f));
        CHECK(e->posY == doctest::Approx(22.0f));
        e->setPropAny("Size", UIPropValue::ofVec2({ 200.0f, 40.0f }));
        CHECK(e->getPropAny("Size").v2.x == doctest::Approx(200.0f));
        e->setPropAny("Layer", UIPropValue::ofInt(7));
        CHECK(e->getPropAny("Layer").i == 7);

        // Hover Cursor: valid index sticks, out-of-range falls back to Default
        e->setPropAny("Hover Cursor", UIPropValue::ofInt((int)UICursor::Hand));
        CHECK(e->hoverCursor == UICursor::Hand);
        e->setPropAny("Hover Cursor", UIPropValue::ofInt(999));
        CHECK(e->hoverCursor == UICursor::Default);

        // Base names must be listed by allProperties (after the type's own)
        const auto all = e->allProperties();
        auto has = [&](const char* n) {
            for (const auto& pd : all) if (pd.name == n) return true;
            return false;
        };
        CHECK(has("Visible"));
        CHECK(has("Hit Testable"));
        CHECK(has("Position"));
        CHECK(has("Size"));
        CHECK(has("Layer"));
        CHECK(has("Hover Cursor"));
        CHECK(has("Material") == e->hasMaterialSlot());

        // Type-specific props still route through the Any accessors
        for (const auto& pd : e->properties())
            CHECK(e->getPropAny(pd.name).type == e->getProp(pd.name).type);
    }
}

TEST_CASE("Material/Font base properties round-trip as strings")
{
    using namespace HE;
    auto e = makeUIElement(UIWidgetType::Image);   // has a material slot
    e->setPropAny("Material", UIPropValue::ofString("Content/M.hasset"));
    CHECK(e->getPropAny("Material").s == "Content/M.hasset");
    CHECK(e->material == "Content/M.hasset");
    e->setPropAny("Font", UIPropValue::ofString("Content/F.hasset"));
    CHECK(e->getPropAny("Font").s == "Content/F.hasset");
    CHECK(e->font == "Content/F.hasset");

    // Font is enumerated only for text-bearing types. A Button is no longer one
    // of them: its caption is a child element, and that child carries the font.
    auto hasFont = [](const UIElement& el) {
        for (const auto& pd : el.allProperties()) if (pd.name == "Font") return true;
        return false;
    };
    CHECK(hasFont(*makeUIElement(UIWidgetType::Text)));
    CHECK(!hasFont(*makeUIElement(UIWidgetType::Panel)));
    CHECK(!hasFont(*makeUIElement(UIWidgetType::Button)));
}

// ── Rich text markup ─────────────────────────────────────────────────────────
// This is a FILE FORMAT: what a saved label means must not shift under it. The
// rules for everything malformed are therefore assertions and not implementation
// details — and there is only one of them, which is why they all fit here.

TEST_CASE("Rich text: the tags, and what they leave behind")
{
    const HE::UIRichText r = HE::uiParseRichText(
        "Hello <color=#ff8800>world</>, read the <link=terms>terms</>.");
    // The tags are gone from the text: what a person reads is what is left.
    CHECK(r.text == "Hello world, read the terms.");
    CHECK(r.hasLinks);
    // The runs cover it exactly once, in order, with no gaps and no empties.
    REQUIRE(r.runs.size() >= 3);
    std::size_t at = 0;
    for (const HE::UITextRun& run : r.runs)
    {
        CHECK(run.begin == at);
        CHECK(run.end > run.begin);
        at = run.end;
    }
    CHECK(at == r.text.size());
    // …and each says what it is.
    auto runAt = [&](const char* needle) {
        const std::size_t pos = r.text.find(needle);
        REQUIRE(pos != std::string::npos);
        for (const HE::UITextRun& run : r.runs)
            if (pos >= run.begin && pos < run.end) return run;
        return HE::UITextRun{};
    };
    CHECK(runAt("world").color == "#ff8800");
    CHECK(runAt("terms").link == "terms");
    CHECK(runAt("Hello").color.empty());
    CHECK(runAt("Hello").link.empty());
}

TEST_CASE("Rich text: nesting is a stack, and a plain label is one run")
{
    const HE::UIRichText r = HE::uiParseRichText("<link=x><color=#ff0000>a</>b</>");
    CHECK(r.text == "ab");
    REQUIRE(r.runs.size() == 2);
    // "a" carries both, "b" only the one still open. That is what a stack means,
    // and anyone who has written markup expects exactly this.
    CHECK(r.runs[0].link == "x");
    CHECK(r.runs[0].color == "#ff0000");
    CHECK(r.runs[1].link == "x");
    CHECK(r.runs[1].color.empty());

    // A label with no markup is ONE run over the whole string — that is what
    // lets every consumer treat plain and rich text with the same code.
    const HE::UIRichText plain = HE::uiParseRichText("just words");
    CHECK(plain.text == "just words");
    REQUIRE(plain.runs.size() == 1);
    CHECK_FALSE(plain.hasLinks);
    CHECK(plain.runs[0].sizeScale == doctest::Approx(1.0f));

    // An empty string has no runs and no text, and nothing downstream may crash
    // on that — it is what a freshly added Text holds before anyone types.
    CHECK(HE::uiParseRichText("").runs.empty());
}

TEST_CASE("Rich text: a tag that is not fully understood IS text")
{
    // The whole malformed-input contract, one rule, every case. The alternative
    // is a table of special cases nobody can remember, and a label that silently
    // loses characters somebody meant to see.
    struct Case { const char* markup; const char* text; };
    const Case cases[] = {
        { "a <unknown=1>b",      "a <unknown=1>b" },   // unknown name
        { "a <color=nope>b",     "a <color=nope>b" },  // the value is not a colour
        { "a <color=#ff00>b",    "a <color=#ff00>b" }, // …nor is a short hex
        { "a <color>b",          "a <color>b" },       // no value at all
        { "a <=red>b",           "a <=red>b" },        // no name at all
        { "a </>b",              "a </>b" },           // closes nothing
        { "a <color=#ff0000 b",  "a <color=#ff0000 b" },// never closed with '>'
        { "5 << 6",              "5 < 6" },            // the escape, the one way in
    };
    for (const Case& c : cases)
        CHECK_MESSAGE(HE::uiParseRichText(c.markup).text == c.text, c.markup);

    // …and an unclosed tag runs to the end rather than dropping the text after it.
    const HE::UIRichText open = HE::uiParseRichText("plain <color=#00ff00>green");
    CHECK(open.text == "plain green");
    REQUIRE(open.runs.size() == 2);
    CHECK(open.runs[1].color == "#00ff00");
}

TEST_CASE("Rich text: size is a SCALE, and colours parse the way the tag promised")
{
    const HE::UIRichText r = HE::uiParseRichText("a<size=2.5>big</>");
    REQUIRE(r.runs.size() == 2);
    CHECK(r.runs[0].sizeScale == doctest::Approx(1.0f));
    CHECK(r.runs[1].sizeScale == doctest::Approx(2.5f));

    glm::vec4 c{};
    REQUIRE(HE::uiParseRichColor("#ff8800", c));
    CHECK(c.r == doctest::Approx(1.0f));
    CHECK(c.g == doctest::Approx(0.533f).epsilon(0.01));
    CHECK(c.b == doctest::Approx(0.0f));
    CHECK(c.a == doctest::Approx(1.0f));       // no alpha given = opaque
    REQUIRE(HE::uiParseRichColor("#00000080", c));
    CHECK(c.a == doctest::Approx(0.502f).epsilon(0.01));
    CHECK_FALSE(HE::uiParseRichColor("#gg0000", c));
    CHECK_FALSE(HE::uiParseRichColor("ff8800", c));    // '#' is not optional
}

TEST_CASE("Rich text: one layout, and plain text lands exactly where it always did")
{
    const HE::BakedUIFont& f = HE::sharedUIFont();
    REQUIRE(f.ok);
    const glm::vec2 pos{ 10.0f, 20.0f }, size{ 300.0f, 80.0f };
    HE::UITextLayout opts;

    // The claim that makes this safe to switch on: with one size throughout, the
    // rich path's mixed-size arithmetic has to collapse to the plain path's
    // formula. If it does not, turning Rich Text on moves every label a little,
    // which is the kind of change nobody can attribute later.
    std::vector<UIRenderObject> plain;
    HE::emitUITextGlyphs(f, 0, "Hello world", pos, size, 24.0f,
                         glm::vec4(1.0f), 0, opts, plain);
    const HE::UIRichText rt = HE::uiParseRichText("Hello world");
    const HE::UIRichLayout lay = HE::uiLayoutRichText(f, rt, pos, size, 24.0f, opts);
    std::vector<UIRenderObject> rich;
    HE::uiEmitRichText(f, 0, rt, lay, glm::vec4(1.0f), 0, rich);
    REQUIRE(rich.size() == plain.size());
    for (std::size_t i = 0; i < rich.size(); ++i)
    {
        CHECK(rich[i].position.x == doctest::Approx(plain[i].position.x));
        CHECK(rich[i].position.y == doctest::Approx(plain[i].position.y));
        CHECK(rich[i].size.x == doctest::Approx(plain[i].size.x));
    }
}

TEST_CASE("Rich text: a bigger word sits ON the line, not above it")
{
    const HE::BakedUIFont& f = HE::sharedUIFont();
    REQUIRE(f.ok);
    HE::UITextLayout opts;
    const HE::UIRichText rt = HE::uiParseRichText("small <size=2>BIG</> small");
    const HE::UIRichLayout lay =
        HE::uiLayoutRichText(f, rt, { 0.0f, 0.0f }, { 600.0f, 100.0f }, 20.0f, opts);
    REQUIRE(lay.pieces.size() >= 3);
    // Every piece of one line shares one baseline. Without that the big word
    // floats and the line reads as two lines badly overlapping — the single
    // most visible way mixed-size text goes wrong.
    const float base = lay.pieces[0].baseline;
    bool sawBig = false;
    for (const HE::UIRichPiece& pc : lay.pieces)
    {
        CHECK(pc.baseline == doctest::Approx(base));
        if (pc.sizePx > 30.0f) sawBig = true;
    }
    CHECK(sawBig);                       // the <size=2> run really is bigger
    // …and the line is as tall as its tallest run, so the block does not clip it.
    CHECK(lay.size.y >= 40.0f);
    // The pieces run left to right without gaps or overlaps.
    for (std::size_t i = 1; i < lay.pieces.size(); ++i)
        if (lay.pieces[i].line == lay.pieces[i - 1].line)
            CHECK(lay.pieces[i].x ==
                  doctest::Approx(lay.pieces[i - 1].x + lay.pieces[i - 1].width));
}

TEST_CASE("Rich text: wrapping breaks inside a run, and the link comes with it")
{
    const HE::BakedUIFont& f = HE::sharedUIFont();
    REQUIRE(f.ok);
    HE::UITextLayout opts; opts.wrap = true;
    const HE::UIRichText rt =
        HE::uiParseRichText("<link=go>one two three four five six seven</>");
    const HE::UIRichLayout lay =
        HE::uiLayoutRichText(f, rt, { 0.0f, 0.0f }, { 120.0f, 200.0f }, 18.0f, opts);
    REQUIRE(!lay.pieces.empty());
    int lastLine = 0;
    for (const HE::UIRichPiece& pc : lay.pieces) lastLine = std::max(lastLine, pc.line);
    CHECK(lastLine > 0);                       // it really did wrap
    // No line runs past the rect, which is what wrapping is for.
    for (const HE::UIRichPiece& pc : lay.pieces)
        CHECK(pc.x + pc.width <= doctest::Approx(120.0f).epsilon(0.02));
    // …and the link survives the break: it is clickable on every line it covers.
    for (const HE::UIRichPiece& pc : lay.pieces)
        CHECK(HE::uiRichLinkAt(rt, lay, pc.x + pc.width * 0.5f,
                               pc.top + pc.height * 0.5f) == "go");
}

TEST_CASE("Rich text: the hit test answers where the link is DRAWN, and nowhere else")
{
    const HE::BakedUIFont& f = HE::sharedUIFont();
    REQUIRE(f.ok);
    HE::UITextLayout opts;
    const HE::UIRichText rt = HE::uiParseRichText("plain <link=terms>terms</> plain");
    const HE::UIRichLayout lay =
        HE::uiLayoutRichText(f, rt, { 0.0f, 0.0f }, { 400.0f, 40.0f }, 20.0f, opts);

    const HE::UIRichPiece* link = nullptr;
    for (const HE::UIRichPiece& pc : lay.pieces)
        if (pc.run < rt.runs.size() && rt.runs[pc.run].link == "terms") link = &pc;
    REQUIRE(link != nullptr);
    const float cy = link->top + link->height * 0.5f;
    CHECK(HE::uiRichLinkAt(rt, lay, link->x + link->width * 0.5f, cy) == "terms");
    // Just outside it on either side is the plain text around it, which is not
    // a link — a link you can click next to is worse than one you cannot click.
    CHECK(HE::uiRichLinkAt(rt, lay, link->x - 2.0f, cy).empty());
    CHECK(HE::uiRichLinkAt(rt, lay, link->x + link->width + 2.0f, cy).empty());
    // …and above/below the line it is on.
    CHECK(HE::uiRichLinkAt(rt, lay, link->x + 1.0f, link->top - 5.0f).empty());
    CHECK(HE::uiRichLinkAt(rt, lay, link->x + 1.0f, link->top + link->height + 5.0f).empty());
}

TEST_CASE("Rich text: a label is inert until its markup says otherwise")
{
    HE::UIText t;
    t.text = "plain <link=go>go</> plain";
    // Off: markup is not read at all, so there is nothing to click and the
    // '<' is a '<'. This is what every label written before today means.
    CHECK_FALSE(t.richText);
    CHECK_FALSE(t.interactive());
    // …and it DRAWS the markup as the characters it is, which is the promise
    // that matters: a label written before today may hold a '<' that means a
    // '<'. (parsed() would strip the tags either way — it is simply not asked
    // while the flag is off, and asserting on it would test the wrong thing.)
    {
        const HE::UIWidgetRect px{ 0.0f, 0.0f, 400.0f, 40.0f };
        std::vector<UIRenderObject> drawn, literal;
        t.render(px, {}, {}, 1.0f, drawn);
        HE::UITextLayout o; o.alignH = t.alignH; o.alignV = t.alignV;
        HE::emitUITextGlyphs(HE::sharedUIFont(), 0, t.text, { px.x, px.y },
                             { px.w, px.h }, t.fontSize, t.color, t.layer, o, literal);
        CHECK(drawn.size() == literal.size());
    }

    // On, with a link: clickable.
    t.richText = true;
    CHECK(t.interactive());

    // On, without a link: still inert. A rich label that merely has a colour in
    // it must not swallow the clicks meant for whatever is behind it, which is
    // why the flag alone is not the question.
    t.text = "just <color=#ff0000>red</>";
    CHECK_FALSE(t.interactive());

    // The parse follows the text rather than being cached once — the text is set
    // from six places and a stale cache is a label showing the last thing
    // somebody typed.
    t.text = "<link=a>a</>";
    CHECK(t.interactive());
}

TEST_CASE("Rich text: what a label is sized for is what it SHOWS")
{
    HE::UIText plain, rich;
    plain.autoSize = rich.autoSize = true;
    plain.wordWrap = rich.wordWrap = false;
    plain.fontSize = rich.fontSize = 20.0f;
    // The same words, one written as markup. Sized for the tags, the rich one
    // would come out much wider than the words it shows.
    plain.text = "Hello world";
    rich.text  = "Hello <color=#ff8800>world</>";
    rich.richText = true;
    plain.applyAutoSize(0.0f);
    rich.applyAutoSize(0.0f);
    CHECK(rich.sizeX == doctest::Approx(plain.sizeX).epsilon(0.02));

    // …and a bigger run really does ask for more room.
    HE::UIText big;
    big.autoSize = true; big.richText = true; big.fontSize = 20.0f;
    big.text = "Hello <size=2>world</>";
    big.applyAutoSize(0.0f);
    CHECK(big.sizeX > plain.sizeX);
    CHECK(big.sizeY > plain.sizeY);
}

TEST_CASE("Rich text: clicking a link fires with its id, clicking the words beside it fires nothing")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 200.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int label = t.add(HE::UIWidgetType::Text);
    {
        auto* e = dynamic_cast<HE::UIText*>(t.find(label));
        e->name = "Label";
        e->richText = true;
        e->autoSize = false;
        e->fontSize = 20.0f;
        e->setProp("Text", HE::UIPropValue::ofString("plain <link=terms>terms</> tail"));
        HE::uiSetAnchorPreset(*e, 0); e->pivotX = e->pivotY = 0.0f;
        e->posX = 0.0f; e->posY = 0.0f; e->sizeX = 400.0f; e->sizeY = 60.0f;
    }
    // What the graph does with it: write the id it was handed into a variable.
    HorizonCode::Graph g;
    HorizonCode::Variable v; v.name = "clicked"; v.type = PinType::String;
    g.variables.push_back(v);
    HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "OnLinkClicked";
    ev.elem = label; ev.hasArg = true; ev.propType = PinType::String;
    const int evId = g.addNode(ev);
    HorizonCode::Node set; set.type = NodeType::SetVariable; set.s = "clicked";
    set.propType = PinType::String;
    const int setId = g.addNode(set);
    REQUIRE(g.connect(evId, 0, setId, 0));
    REQUIRE(g.connect(evId, 1, setId, 2));
    registerWidget(cm, t, &g);

    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    const auto inst = static_cast<HorizonCode::InstanceId>(id);

    // Where the link actually IS, asked of the same layout the draw uses.
    const auto* live = dynamic_cast<const HE::UIText*>(wm.tree(id)->find(label));
    REQUIRE(live);
    const HE::UIRichLayout lay = HE::uiLayoutRichText(
        HE::sharedUIFont(), live->parsed(), { 0.0f, 0.0f }, { 400.0f, 60.0f },
        20.0f, [&]{ HE::UITextLayout o; o.alignH = live->alignH;
                    o.alignV = live->alignV; o.wrap = live->wordWrap; return o; }());
    const HE::UIRichPiece* link = nullptr;
    for (const HE::UIRichPiece& pc : lay.pieces)
        if (pc.run < live->parsed().runs.size() &&
            live->parsed().runs[pc.run].link == "terms") link = &pc;
    REQUIRE(link != nullptr);
    const float lx = link->x + link->width * 0.5f;
    const float ly = link->top + link->height * 0.5f;

    auto clickAt = [&](float x, float y) {
        wm.processPointer(400.0f, 200.0f, x, y, true,  true);
        wm.processPointer(400.0f, 200.0f, x, y, false, true);
        return rt.getVariable(inst, "clicked").s;
    };
    CHECK(clickAt(lx, ly) == "terms");

    // The plain words before it are not a button, and a label that fired for
    // them would make every sentence a click target.
    rt.setVariable(inst, "clicked", HorizonCode::Value::ofString(""));
    CHECK(clickAt(2.0f, ly).empty());

    // …and the pointer says so before the click does: hand on the link, plain
    // arrow beside it.
    wm.processPointer(400.0f, 200.0f, lx, ly, false, true);
    CHECK(wm.hoverCursor() == HE::UICursor::Hand);
    wm.processPointer(400.0f, 200.0f, 2.0f, ly, false, true);
    CHECK(wm.hoverCursor() != HE::UICursor::Hand);
}

// ── A label somebody may select in (plan B6, the rest) ───────────────────────

TEST_CASE("Selectable label: off is off, and off is what every existing label is")
{
    HE::UIText t;
    t.text = "Hello world";
    CHECK_FALSE(t.selectable);
    CHECK_FALSE(t.selectionEnabled());
    // Not interactive: a caption on a button must not eat the press meant for
    // the button, and a heading must not take the keyboard focus.
    CHECK_FALSE(t.interactive());
    // …and it has no rows to select in, so nothing downstream can act as if it
    // did.
    CHECK(t.selectRows({ 0.0f, 0.0f, 400.0f, 40.0f }, 1.0f).empty());

    // The file it saves is the file it always saved: neither key is written.
    nlohmann::json j;
    t.writeJson(j);
    CHECK_FALSE(j.contains("selectable"));
    CHECK_FALSE(j.contains("selectionColor"));

    // On, it is interactive — and that is the ONLY thing the flag changes about
    // what a label is.
    t.selectable = true;
    CHECK(t.selectionEnabled());
    CHECK(t.interactive());
    nlohmann::json j2;
    t.writeJson(j2);
    CHECK(j2.value("selectable", false));
    HE::UIText back;
    back.readJson(j2);
    CHECK(back.selectable);
    CHECK(back.selectionColor.r == doctest::Approx(t.selectionColor.r));

    // Rich text WINS. Markup lays out in runs of several sizes and a hit test
    // over those is a different problem; half-built it would highlight one word
    // and copy another.
    t.richText = true;
    CHECK(t.selectable);            // the flag is still what the author set
    CHECK_FALSE(t.selectionEnabled());
    CHECK(t.selectRows({ 0.0f, 0.0f, 400.0f, 40.0f }, 1.0f).empty());
}

TEST_CASE("Selectable label: the rows the glyphs are drawn from are the rows the caret is found in")
{
    REQUIRE(HE::sharedUIFont().ok);
    HE::UIText t;
    t.selectable = true;
    t.autoSize   = false;
    t.fontSize   = 20.0f;
    t.alignV     = 0;                  // top, so row 0 starts at the rect's top
    t.text = "one two three four five six seven eight nine";
    t.wordWrap = true;
    const HE::UIWidgetRect px{ 0.0f, 0.0f, 160.0f, 200.0f };

    const std::vector<HE::UITextSelectRow> rows = t.selectRows(px, 1.0f);
    REQUIRE(rows.size() > 1);          // it really did wrap
    // The rows partition the text: no gap and no overlap, and the last one ends
    // at the end. A split that lost a byte is a word you cannot select.
    CHECK(rows.front().begin == 0);
    CHECK(rows.back().end == t.text.size());
    for (size_t i = 1; i < rows.size(); ++i)
        CHECK(rows[i].begin == rows[i - 1].next);

    // Aiming at the middle of a character on row 1 lands INSIDE row 1 — the
    // whole point of one split: with two, the caret would come from a row the
    // glyphs were never drawn on.
    const HE::UITextSelectRow& r1 = rows[1];
    const float midY = r1.top + t.fontSize * 0.5f;
    const size_t at = t.caretAtPoint(px, 1.0f, r1.x + 1.0f, midY);
    CHECK(at >= r1.begin);
    CHECK(at <= r1.end);

    // Far left of a row is its start, far right its end.
    CHECK(t.caretAtPoint(px, 1.0f, -50.0f, midY) == r1.begin);
    CHECK(t.caretAtPoint(px, 1.0f, 1000.0f, midY) == r1.end);
    // Above the first row lands on it, below the last on that one: dragging off
    // the edge of a paragraph must not drop the selection.
    CHECK(t.caretAtPoint(px, 1.0f, -50.0f, -500.0f) == rows.front().begin);
    CHECK(t.caretAtPoint(px, 1.0f, 1000.0f, 5000.0f) == rows.back().end);
}

TEST_CASE("Selectable label: a centred label is hit where it is drawn, not where it would start")
{
    REQUIRE(HE::sharedUIFont().ok);
    // The trap alignment brings: the glyphs of a centred or right-aligned label
    // do not begin at the rect's left edge, so a hit test that assumed they did
    // would answer several characters out on every label that is not left-aligned.
    const HE::UIWidgetRect px{ 0.0f, 0.0f, 400.0f, 40.0f };
    for (int align : { 0, 1, 2 })
    {
        HE::UIText t;
        t.selectable = true;
        t.autoSize   = false;
        t.fontSize   = 20.0f;
        t.alignH     = align;
        t.text       = "abcdef";
        const std::vector<HE::UITextSelectRow> rows = t.selectRows(px, 1.0f);
        REQUIRE(rows.size() == 1);
        const float y = rows[0].top + 1.0f;
        // Left of the glyphs is offset 0 whatever the alignment, and one pixel
        // into them is still 0 — the midpoint rule.
        CHECK(t.caretAtPoint(px, 1.0f, rows[0].x - 5.0f, y) == 0u);
        // Past the last glyph is the end of the text, and the x that means
        // "past" moves with the alignment. Asking at the rect's right edge is
        // what a right-aligned label would fail if x were ignored.
        CHECK(t.caretAtPoint(px, 1.0f, rows[0].x + rows[0].width + 5.0f, y) == t.text.size());
        // A centred label really is centred: its glyphs start away from 0.
        if (align == 1) CHECK(rows[0].x > 1.0f);
        if (align == 2) CHECK(rows[0].x > 1.0f);
        if (align == 0) CHECK(rows[0].x == doctest::Approx(0.0f));
    }
}

TEST_CASE("Selectable label: drawing it is drawing the same glyphs, plus a selection behind them")
{
    REQUIRE(HE::sharedUIFont().ok);
    const HE::UIWidgetRect px{ 0.0f, 0.0f, 400.0f, 60.0f };
    HE::UIText plain, sel;
    for (HE::UIText* t : { &plain, &sel })
    {
        t->autoSize = false; t->fontSize = 20.0f;
        t->text = "Hello world";
    }
    sel.selectable = true;

    std::vector<UIRenderObject> a, b;
    plain.render(px, {}, {}, 1.0f, a);
    sel.render(px, {}, {}, 1.0f, b);
    // Same glyphs in the same places: the row path exists to agree with the
    // block path, not to move the text.
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i)
    {
        CHECK(a[i].position.x == doctest::Approx(b[i].position.x));
        CHECK(a[i].position.y == doctest::Approx(b[i].position.y));
    }

    // With something selected there is one quad more, and it is BEHIND the
    // glyphs — a highlight drawn over the words is a highlight that hides them.
    sel.selAnchor = 0; sel.caret = 5;
    HE::UIElementRenderState focused; focused.focused = true;
    std::vector<UIRenderObject> c;
    sel.render(px, focused, {}, 1.0f, c);
    CHECK(c.size() == b.size() + 1);
    CHECK(c.front().type != 2);          // the quad, before any glyph
    CHECK(c.front().color.a == doctest::Approx(sel.selectionColor.a));

    // …and NOT while the focus is somewhere else. A paragraph somebody selected
    // in and then clicked away from would otherwise stay highlighted for the
    // rest of the run, and two of them at once would claim the reader has two
    // selections. The offsets survive, so coming back finds what was left.
    std::vector<UIRenderObject> d;
    sel.render(px, {}, {}, 1.0f, d);
    CHECK(d.size() == b.size());
    CHECK(sel.hasSelection());
}

TEST_CASE("Selectable label: the hit test holds up when the canvas is not 1:1")
{
    REQUIRE(HE::sharedUIFont().ok);
    // Every other case here runs at scale 1, and step 20 found real bugs in
    // exactly this gap: the glyphs are laid out at the scaled size and a hit
    // test that measured at the authored one lands several characters short.
    HE::UIText t;
    t.selectable = true; t.autoSize = false; t.fontSize = 20.0f; t.alignH = 1;
    t.text = "alpha beta";

    for (float scale : { 1.0f, 2.0f, 0.75f })
    {
        // The rect arrives in PIXELS, so it scales with everything else.
        const HE::UIWidgetRect px{ 0.0f, 0.0f, 400.0f * scale, 40.0f * scale };
        const std::vector<HE::UITextSelectRow> rows = t.selectRows(px, scale);
        REQUIRE(rows.size() == 1);
        const float y = rows[0].top + t.fontSize * scale * 0.5f;
        // The whole run, from in front of the first glyph to past the last.
        CHECK(t.caretAtPoint(px, scale, rows[0].x - 4.0f, y) == 0u);
        CHECK(t.caretAtPoint(px, scale, rows[0].x + rows[0].width + 4.0f, y) == t.text.size());
        // …and the boundary between the two words is where the words meet,
        // whatever the scale: measured, not assumed.
        const float mid = rows[0].x + t.caretXInRow({ 0, t.text.size(), t.text.size(), true }, 5);
        CHECK(t.caretAtPoint(px, scale, mid + 1.0f, y) == 5u);
        // The row really did grow with the scale, or the check above proves
        // nothing about scaling.
        CHECK(rows[0].width == doctest::Approx(t.fontSize * scale * 0.5f * 10.0f).epsilon(0.6));
    }
}

TEST_CASE("Selectable label: a multi-line selection is one run, not several unrelated ones")
{
    REQUIRE(HE::sharedUIFont().ok);
    HE::UIText t;
    t.selectable = true; t.autoSize = false; t.fontSize = 18.0f; t.alignV = 0;
    t.text = "first\n\nthird";
    const HE::UIWidgetRect px{ 0.0f, 0.0f, 400.0f, 120.0f };
    const std::vector<HE::UITextSelectRow> rows = t.selectRows(px, 1.0f);
    REQUIRE(rows.size() == 3);           // the blank line in the middle is a row

    t.selAnchor = 0; t.caret = t.text.size();
    HE::UIElementRenderState focused; focused.focused = true;
    std::vector<UIRenderObject> out;
    t.render(px, focused, {}, 1.0f, out);
    int quads = 0;
    for (const UIRenderObject& o : out) if (o.type != 2) ++quads;
    // One per row INCLUDING the empty one: a selected blank line drawn as
    // nothing makes a run read as two.
    CHECK(quads == 3);
}

TEST_CASE("Selectable label: a trailing newline does not push the text half a line up")
{
    REQUIRE(HE::sharedUIFont().ok);
    // layoutUITextLines drops exactly one trailing empty line, because an empty
    // last row is half the block's height. The row split has to drop the same
    // one, or ticking Selectable would move a centred label.
    HE::UIText a, b;
    for (HE::UIText* t : { &a, &b })
    { t->selectable = true; t->autoSize = false; t->fontSize = 18.0f; }
    a.text = "Option 2";
    b.text = "Option 2\n";
    const HE::UIWidgetRect px{ 0.0f, 0.0f, 300.0f, 60.0f };
    CHECK(a.selectRows(px, 1.0f).size() == 1);
    CHECK(b.selectRows(px, 1.0f).size() == 1);
    CHECK(a.selectRows(px, 1.0f)[0].top == doctest::Approx(b.selectRows(px, 1.0f)[0].top));
    // …but a DELIBERATE blank line survives: only one is dropped.
    HE::UIText c;
    c.selectable = true; c.autoSize = false; c.fontSize = 18.0f;
    c.text = "a\n\n";
    CHECK(c.selectRows(px, 1.0f).size() == 2);
}

TEST_CASE("Selectable label: what a reader selects with the pointer is what lands in the clipboard")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 200.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int label = t.add(HE::UIWidgetType::Text);
    {
        auto* e = dynamic_cast<HE::UIText*>(t.find(label));
        e->name = "Label"; e->selectable = true; e->autoSize = false;
        e->fontSize = 20.0f; e->alignV = 0;
        e->setProp("Text", HE::UIPropValue::ofString("alpha beta gamma"));
        HE::uiSetAnchorPreset(*e, 0); e->pivotX = e->pivotY = 0.0f;
        e->posX = 0.0f; e->posY = 0.0f; e->sizeX = 400.0f; e->sizeY = 60.0f;
    }
    registerWidget(cm, t);

    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    const auto* live = dynamic_cast<const HE::UIText*>(wm.tree(id)->find(label));
    REQUIRE(live);

    // Where the row actually is, asked of the same list the draw uses.
    const HE::UIWidgetRect px{ 0.0f, 0.0f, 400.0f, 60.0f };
    const std::vector<HE::UITextSelectRow> rows = live->selectRows(px, 1.0f);
    REQUIRE(rows.size() == 1);
    const float y = rows[0].top + live->fontSize * 0.5f;
    const float xEnd = rows[0].x + rows[0].width;

    // Press at the start, drag to the end: that is a selection, and it is what
    // a copy would take.
    wm.processPointer(400.0f, 200.0f, rows[0].x, y, true, true);
    CHECK(wm.isSelectingText());
    CHECK(wm.focusedElement() == label);
    // …and NOT "editing": a label has nothing to type into, and a host that
    // routed every key here would let it swallow Return.
    CHECK_FALSE(wm.isEditingText());
    wm.processPointer(400.0f, 200.0f, xEnd, y, true, true);
    wm.processPointer(400.0f, 200.0f, xEnd, y, false, true);
    CHECK(wm.focusedSelection() == "alpha beta gamma");

    // The pointer says so before the press does.
    wm.processPointer(400.0f, 200.0f, rows[0].x + 5.0f, y, false, true);
    CHECK(wm.hoverCursor() == HE::UICursor::Text);

    // A double-click takes the word under it, not the line.
    wm.processPointer(400.0f, 200.0f, rows[0].x + 2.0f, y, true, true);
    wm.processPointer(400.0f, 200.0f, rows[0].x + 2.0f, y, false, true);
    CHECK(wm.selectWordAtPointer(400.0f, 200.0f, rows[0].x + 2.0f, y));
    CHECK(wm.focusedSelection() == "alpha");

    // Ctrl+A takes all of it; the caret keys move without changing a character.
    CHECK(wm.selectAllFocused());
    CHECK(wm.focusedSelection() == "alpha beta gamma");
    CHECK(wm.editFocusedText(WidgetManager::TextEdit::Home, false));
    CHECK(wm.focusedSelection().empty());
    CHECK(wm.editFocusedText(WidgetManager::TextEdit::WordRight, true));
    CHECK(wm.focusedSelection() == "alpha");
    CHECK(wm.editFocusedText(WidgetManager::TextEdit::End, true));
    CHECK(wm.focusedSelection() == "alpha beta gamma");
    // The text itself never moved.
    CHECK(live->text == "alpha beta gamma");
    // …and nothing that would edit it answers at all.
    CHECK_FALSE(wm.editFocusedText(WidgetManager::TextEdit::Delete, false));
    CHECK_FALSE(wm.deleteFocusedSelection());
    CHECK(live->text == "alpha beta gamma");
}

TEST_CASE("Selectable label: static text is not on the Tab route")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 300.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    auto place = [&](int id, float y, float h)
    {
        HE::UIElement* e = t.find(id);
        HE::uiSetAnchorPreset(*e, 0); e->pivotX = e->pivotY = 0.0f;
        e->posX = 0.0f; e->posY = y; e->sizeX = 380.0f; e->sizeY = h;
    };
    const int f1 = t.add(HE::UIWidgetType::TextInput);
    place(f1, 0.0f, 30.0f);
    const int para = t.add(HE::UIWidgetType::Text);
    {
        auto* e = dynamic_cast<HE::UIText*>(t.find(para));
        e->selectable = true; e->autoSize = false;
        e->setProp("Text", HE::UIPropValue::ofString("a paragraph between two fields"));
        place(para, 40.0f, 40.0f);
    }
    const int f2 = t.add(HE::UIWidgetType::TextInput);
    place(f2, 90.0f, 30.0f);
    registerWidget(cm, t);

    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);

    // Tab walks the two FIELDS and steps over the paragraph — no toolkit puts
    // static text in a form's tab order, and paying a Tab per paragraph is what
    // makes a keyboard user give up on a page.
    REQUIRE(wm.setFocus(id, f1));
    CHECK(wm.focusNext(false, 400.0f, 300.0f));
    CHECK(wm.focusedElement() == f2);

    // …and so do the ARROW keys, which matters more than Tab does. A focused
    // label swallows them to move its own caret, so landing on one with Down
    // would be landing somewhere only Tab gets you out of again.
    REQUIRE(wm.setFocus(id, f1));
    CHECK(wm.navigate(WidgetManager::NavDir::Down, 400.0f, 300.0f));
    CHECK(wm.focusedElement() == f2);

    // But it is still reachable — by a click, and by setFocus, which is the
    // difference between "not on the route" and "not focusable".
    CHECK(wm.setFocus(id, para));
    CHECK(wm.isSelectingText());
    // …and Tab from there goes on to the next field rather than back to the top.
    CHECK(wm.focusNext(false, 400.0f, 300.0f));
    CHECK(wm.focusedElement() == f2);
}

TEST_CASE("Selectable label: the up arrow keeps the column it left from")
{
    REQUIRE(HE::sharedUIFont().ok);
    HE::UIText t;
    t.selectable = true; t.autoSize = false; t.fontSize = 18.0f; t.alignV = 0;
    t.text = "aaaaaaaaaa\nbb\ncccccccccc";
    // Never drawn: nothing has been measured, so the arrows say so rather than
    // jumping to the start of the row above.
    CHECK(t.rowSizePx == doctest::Approx(0.0f));
    CHECK(t.rowRanges().size() == 3);

    // Drawn once — that is what remembers the width and the size.
    const HE::UIWidgetRect px{ 0.0f, 0.0f, 400.0f, 120.0f };
    std::vector<UIRenderObject> out;
    t.render(px, {}, {}, 1.0f, out);
    CHECK(t.rowSizePx == doctest::Approx(18.0f));

    const std::vector<HE::UITextVisualLine> rows = t.rowRanges();
    REQUIRE(rows.size() == 3);
    // Column eight of row one, down through the SHORT row, and back up: a walk
    // that forgot the column would come back at the end of "bb" instead.
    const float col = t.caretXInRow(rows[0], 8);
    CHECK(t.byteAtRowX(rows[2], col) == rows[2].begin + 8);
    // The short row has nowhere near that column and clamps to its own end.
    CHECK(t.byteAtRowX(rows[1], col) == rows[1].end);
}

// ── Multi-line text ───────────────────────────────────────────────────────────
// emitUITextGlyphs used to lay out strictly one line and DROP every byte < 32,
// so '\n' silently vanished and the whole string ran together.

TEST_CASE("layoutUITextLines splits on newlines")
{
    const HE::BakedUIFont& f = HE::sharedUIFont();
    REQUIRE(f.ok);
    const auto lines = HE::layoutUITextLines(f, "one\ntwo\nthree", 20.0f, 0.0f, false);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "one");
    CHECK(lines[1] == "two");
    CHECK(lines[2] == "three");
    // CRLF must not leave a stray carriage return in the run.
    const auto crlf = HE::layoutUITextLines(f, "a\r\nb", 20.0f, 0.0f, false);
    REQUIRE(crlf.size() == 2);
    CHECK(crlf[0] == "a");
    CHECK(crlf[1] == "b");
}

TEST_CASE("layoutUITextLines word-wraps within the width and never overflows")
{
    const HE::BakedUIFont& f = HE::sharedUIFont();
    REQUIRE(f.ok);
    const float sizePx = 20.0f, wrapW = 90.0f;
    const auto lines = HE::layoutUITextLines(
        f, "alpha beta gamma delta epsilon", sizePx, wrapW, true);
    CHECK(lines.size() > 1);
    const float scale = sizePx / f.bakePx;
    for (const std::string& l : lines)
    {
        float w = 0.0f;
        for (size_t i = 0; i < l.size(); )
            if (const HE::BakedGlyph* g = f.glyph(HE::uiUtf8Decode(l, i))) w += g->xadvance * scale;
        CHECK(w <= wrapW + 0.01f);
    }
    // Wrapping off leaves the run on one line.
    CHECK(HE::layoutUITextLines(f, "alpha beta gamma delta epsilon",
                                sizePx, wrapW, false).size() == 1);
}

TEST_CASE("layoutUITextLines hard-breaks a word wider than the line")
{
    const HE::BakedUIFont& f = HE::sharedUIFont();
    REQUIRE(f.ok);
    const auto lines = HE::layoutUITextLines(f, "supercalifragilistic", 24.0f, 40.0f, true);
    CHECK(lines.size() > 1);
    // Every character survives the break (no glyphs dropped).
    std::string joined;
    for (const std::string& l : lines) joined += l;
    CHECK(joined == "supercalifragilistic");
}

// ── The atlas beyond ASCII ────────────────────────────────────────────────────
TEST_CASE("A word with umlauts is drawn and measured as the word it is")
{
    // "Größe": six characters, eight bytes. Before the atlas carried Latin-1 the
    // three non-ASCII ones were skipped by BOTH the glyph loop and the width, so
    // the label read "Gre" and everything aligned to it sat too far left.
    const std::string umlaut = "Gr\xc3\xb6\xc3\x9f" "e";     // Größe
    std::vector<UIRenderObject> out;
    HE::emitUITextGlyphs(umlaut, { 0.0f, 0.0f }, { 400.0f, 40.0f }, 22.0f,
                         { 1, 1, 1, 1 }, 0, false, out);
    CHECK(out.size() == 5);

    HE::UITextLayout opts;
    const float wide   = HE::measureUIText(umlaut, 22.0f, 0.0f, opts).x;
    const float narrow = HE::measureUIText("Gre", 22.0f, 0.0f, opts).x;
    CHECK(wide > narrow);
    // Width and glyphs have to agree, or the caret lands beside the character.
    float advances = 0.0f;
    const HE::BakedUIFont& f = HE::sharedUIFont();
    for (size_t i = 0; i < umlaut.size(); )
        if (const HE::BakedGlyph* g = f.glyph(HE::uiUtf8Decode(umlaut, i)))
            advances += g->xadvance * (22.0f / f.bakePx);
    CHECK(advances == doctest::Approx(wide));
}

TEST_CASE("A password field draws its dots")
{
    // The dot is U+2022, three bytes, none of them ASCII. With an ASCII-only
    // atlas the field drew NOTHING and measured zero, so the caret could not
    // move either — a login screen with no visible input and no test that said so.
    HE::UITextInput ti;
    ti.text     = "abc";
    ti.password = true;
    std::vector<UIRenderObject> out;
    ti.render({ 0.0f, 0.0f, 200.0f, 30.0f }, {}, HE::UUID{}, 1.0f, out);
    int glyphs = 0;
    for (const UIRenderObject& o : out) if (o.type == 2) ++glyphs;
    CHECK(glyphs == 3);
    CHECK(ti.caretAtPoint(200.0f, 15.0f, 1.0f) == 3);   // click past the end → after the last
}

TEST_CASE("A character the atlas does not carry costs nothing, and costs it once")
{
    const HE::BakedUIFont& f = HE::sharedUIFont();
    REQUIRE(f.ok);
    // U+4E00 is outside every baked range; the font's own missing glyph must not
    // stand in for it, because a box would claim the character was drawn.
    CHECK(f.glyph(0x4E00) == nullptr);
    HE::UITextLayout opts;
    const float withCjk = HE::measureUIText("a\xE4\xB8\x80" "b", 20.0f, 0.0f, opts).x;
    const float without = HE::measureUIText("ab", 20.0f, 0.0f, opts).x;
    CHECK(withCjk == doctest::Approx(without));
}

TEST_CASE("Wrapping breaks between characters, never inside one")
{
    const HE::BakedUIFont& f = HE::sharedUIFont();
    REQUIRE(f.ok);
    // One long word of two-byte characters, hard-broken because it cannot fit.
    std::string word;
    for (int i = 0; i < 20; ++i) word += "\xc3\xa4";      // ä
    const auto lines = HE::layoutUITextLines(f, word, 24.0f, 40.0f, true);
    CHECK(lines.size() > 1);
    std::string joined;
    for (const std::string& l : lines)
    {
        // Every line is whole characters: walking it by codepoint has to land
        // exactly on its end. Half an ä is not a narrow ä, it is nothing at all.
        size_t i = 0;
        while (i < l.size()) i = HE::uiUtf8Next(l, i);
        CHECK(i == l.size());
        CHECK(l.size() % 2 == 0);
        joined += l;
    }
    CHECK(joined == word);
}

TEST_CASE("Rich text measures a character once, not once per byte")
{
    // The rich path walks bytes to know which run a character belongs to, so it
    // is the one most easily made to count a two-byte character twice: the
    // second byte of an ä is 0xA4, which the Latin-1 range now carries a glyph
    // for. Stepping by byte would draw it AND add its width.
    const HE::BakedUIFont& f = HE::sharedUIFont();
    REQUIRE(f.ok);
    const std::string s = "\xc3\xa4\xc3\xa4";              // ää
    const HE::UIRichText rt = HE::uiParseRichText(s);
    HE::UITextLayout opts;
    const HE::UIRichLayout rl =
        HE::uiLayoutRichText(f, rt, { 0.0f, 0.0f }, { 400.0f, 40.0f }, 22.0f, opts);
    // The same two characters through the plain path, which is the reference the
    // whole rich layout is held to.
    const float plain = HE::measureUIText(s, 22.0f, 0.0f, opts).x;
    CHECK(rl.size.x == doctest::Approx(plain));
    // …and the pieces the layout hands the renderer are that same width: the
    // block and the piece are measured by two loops, and the day they disagree
    // is the day a coloured run is drawn wider than the text under it.
    float pieces = 0.0f;
    for (const HE::UIRichPiece& p : rl.pieces) pieces += p.width;
    CHECK(pieces == doctest::Approx(plain));

    std::vector<UIRenderObject> out;
    HE::uiEmitRichText(f, 0, rt, rl, { 1, 1, 1, 1 }, 0, out);
    CHECK(out.size() == 2);
}

TEST_CASE("<b> is a scope like every other, and closes like every other")
{
    const HE::UIRichText rt = HE::uiParseRichText("plain <b>bold</> plain");
    CHECK(rt.text == "plain bold plain");
    REQUIRE(rt.runs.size() == 3);
    CHECK(rt.runs[0].face == HE::UIFontFace::Base);
    CHECK(rt.runs[1].face == HE::UIFontFace::Bold);
    CHECK(rt.runs[2].face == HE::UIFontFace::Base);

    // It nests with the others rather than beside them: a bold link stays a link.
    const HE::UIRichText both = HE::uiParseRichText("<link=x>see <b>this</></>");
    REQUIRE(both.runs.size() == 2);
    CHECK(both.runs[1].face == HE::UIFontFace::Bold);
    CHECK(both.runs[1].link == "x");

    // And the format's one rule still holds: a tag that is not understood is
    // text, so a `<bb>` in a sentence is a `<bb>`.
    CHECK(HE::uiParseRichText("a<bb>c").text == "a<bb>c");
}

TEST_CASE("With bold as the base weight, <b> is a visible no-op")
{
    // The engine's default weight IS bold, and there is nothing bolder. Saying so
    // by drawing the same atlas is the honest answer; the alternative is a second
    // atlas holding identical glyphs and a tag that pretends to do something.
    //
    // The weight is per PROCESS, and ctest gives this file one of its own. Run
    // the binary by hand with test_ui_font_weight.cpp in the same run and that
    // file may have got there first, so step aside rather than report its choice
    // as this file's failure.
    if (!HE::uiFontWeightBold())
    {
        MESSAGE("this process was baked regular — the no-op only exists with bold as the base");
        return;
    }
    CHECK(HE::uiEngineFaceKey(HE::UIFontFace::Bold) == 0u);

    const HE::BakedUIFont& f = HE::sharedUIFont();
    const HE::UIRichText rt = HE::uiParseRichText("a<b>a</>a");
    HE::UITextLayout opts;
    const HE::UIRichLayout lay =
        HE::uiLayoutRichText(f, rt, { 0.0f, 0.0f }, { 400.0f, 40.0f }, 24.0f, opts);
    REQUIRE(lay.pieces.size() == 3);
    CHECK(lay.pieces[1].width == doctest::Approx(lay.pieces[0].width));
    std::vector<UIRenderObject> out;
    HE::uiEmitRichText(f, 0, rt, lay, { 1, 1, 1, 1 }, 0, out);
    REQUIRE(out.size() == 3);
    CHECK(out[1].fontAtlasKey == out[0].fontAtlasKey);
}

TEST_CASE("An icon is a name, and a name the font does not have is text")
{
    CHECK(HE::uiIconCodepoint("home") == 0xE88A);
    CHECK(HE::uiIconCodepoint("settings") == 0xE8B8);
    CHECK(HE::uiIconCodepoint("no_such_icon") == 0u);
    CHECK(HE::uiIconCount() > 2000);

    // The tag INSERTS a character rather than colouring the next ones, which is
    // what makes it the only self-contained tag in the format.
    const HE::UIRichText rt = HE::uiParseRichText("a<icon=home>b");
    REQUIRE(rt.runs.size() == 3);
    CHECK(rt.runs[1].face == HE::UIFontFace::Icon);
    CHECK(rt.text.size() == 5);                       // 'a' + three bytes + 'b'
    std::size_t at = rt.runs[1].begin;
    CHECK(HE::uiUtf8Decode(rt.text, at) == 0xE88A);
    CHECK(at == rt.runs[1].end);                      // exactly one character

    // A misspelled name falls under the format's one rule, like every other tag
    // that is not fully understood: it is the text somebody typed.
    CHECK(HE::uiParseRichText("a<icon=hmoe>b").text == "a<icon=hmoe>b");
}

TEST_CASE("An icon is drawn from its own atlas, measured at its own bake size")
{
    const HE::BakedUIFont& base = HE::sharedUIFont();
    const std::uint32_t iconKey = HE::uiEngineFaceKey(HE::UIFontFace::Icon);
    REQUIRE(iconKey != 0);
    const HE::BakedUIFont* icons = HE::UIFontCache::find(iconKey);
    REQUIRE(icons != nullptr);
    REQUIRE(icons->ok);
    // Its own atlas, at its own size: the icons are baked smaller than the text,
    // which is exactly why the layout has to divide by the FACE's bake size.
    CHECK(icons->bakePx != base.bakePx);
    CHECK(icons->glyph(0xE88A) != nullptr);

    const HE::UIRichText rt = HE::uiParseRichText("<icon=home> Start");
    HE::UITextLayout opts;
    const HE::UIRichLayout lay =
        HE::uiLayoutRichText(base, rt, { 0.0f, 0.0f }, { 400.0f, 40.0f }, 24.0f, opts);
    REQUIRE(lay.pieces.size() >= 2);
    CHECK(lay.pieces[0].face == HE::UIFontFace::Icon);
    CHECK(lay.pieces[0].width ==
          doctest::Approx(icons->glyph(0xE88A)->xadvance * (24.0f / icons->bakePx)));
    // Same line, same baseline as the words beside it.
    CHECK(lay.pieces[0].baseline == doctest::Approx(lay.pieces[1].baseline));

    std::vector<UIRenderObject> out;
    HE::uiEmitRichText(base, 0, rt, lay, { 1, 1, 1, 1 }, 0, out);
    REQUIRE(out.size() >= 2);
    CHECK(out[0].fontAtlasKey == iconKey);
    CHECK(out[1].fontAtlasKey == 0u);
}

TEST_CASE("An icon inside a link is part of the link")
{
    // It inherits colour, size and link from where it stands. Anything else would
    // need a rule about which attributes reach an icon and which do not, and
    // nobody would remember that rule.
    const HE::BakedUIFont& f = HE::sharedUIFont();
    const HE::UIRichText rt =
        HE::uiParseRichText("<link=save><color=#ff8800><icon=save> Speichern</></>");
    REQUIRE(rt.hasLinks);
    REQUIRE(rt.runs.size() >= 2);
    CHECK(rt.runs[0].face == HE::UIFontFace::Icon);
    CHECK(rt.runs[0].link == "save");
    CHECK(rt.runs[0].color == "#ff8800");

    HE::UITextLayout opts;
    const HE::UIRichLayout lay =
        HE::uiLayoutRichText(f, rt, { 0.0f, 0.0f }, { 400.0f, 40.0f }, 24.0f, opts);
    REQUIRE_FALSE(lay.pieces.empty());
    const HE::UIRichPiece& icon = lay.pieces[0];
    CHECK(HE::uiRichLinkAt(rt, lay, icon.x + icon.width * 0.5f,
                           icon.top + icon.height * 0.5f) == "save");
}

TEST_CASE("The chosen scripts travel into the exported application")
{
    // The shipped app bakes its own atlas on a machine that never saw the
    // editor, so the answer has to be in project.hcfg — and it rides in the
    // existing flags word, which means no format version moves and an older
    // runtime reads the same file it always did.
    const auto dir = std::filesystem::temp_directory_path() / "he_fontscripts_hcfg";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    ProjectConfig cfg;
    cfg.projectName = "Greek";
    cfg.fontScripts = HE::UIFontScriptGreek | HE::UIFontScriptCyrillic;
    REQUIRE(ProjectConfigLoader::save(dir, cfg));
    ProjectConfig back;
    REQUIRE(ProjectConfigLoader::load(dir, back));
    CHECK(back.fontScripts == (HE::UIFontScriptGreek | HE::UIFontScriptCyrillic));

    // A build written before this existed has the bits clear, and clear is also
    // what it should mean: the base set, which is what it had.
    ProjectConfig plain;
    plain.projectName = "Plain";
    REQUIRE(ProjectConfigLoader::save(dir, plain));
    ProjectConfig plainBack;
    REQUIRE(ProjectConfigLoader::load(dir, plainBack));
    CHECK(plainBack.fontScripts == 0u);
    CHECK(plainBack.allowFiles == false);      // the neighbouring bits are unmoved
    // The weight rides in the same word, negated: absent means bold, which is
    // what a build written before the setting existed drew.
    CHECK(plainBack.fontWeightBold);
    cfg.fontWeightBold = false;
    REQUIRE(ProjectConfigLoader::save(dir, cfg));
    ProjectConfig regular;
    REQUIRE(ProjectConfigLoader::load(dir, regular));
    CHECK_FALSE(regular.fontWeightBold);
    CHECK(regular.fontScripts == (HE::UIFontScriptGreek | HE::UIFontScriptCyrillic));
    std::filesystem::remove_all(dir);
}

TEST_CASE("The scripts are chosen before the first bake, and say so afterwards")
{
    // The backends upload each atlas once per key. A mask that changed after the
    // shared font was baked would leave them holding a texture whose glyphs have
    // moved, so the request is refused rather than half-applied — and the caller
    // can tell, which is what lets the settings page say "after a restart".
    REQUIRE(HE::sharedUIFont().ok);                       // forces the bake
    const std::uint32_t now = HE::uiFontScripts();
    CHECK(HE::uiSetFontScripts(now));                     // asking for what it has
    CHECK_FALSE(HE::uiSetFontScripts(now ^ HE::UIFontScriptGreek));
    CHECK(HE::uiFontScripts() == now);                    // and nothing moved
}

TEST_CASE("emitUITextGlyphs emits both lines of a two-line run")
{
    std::vector<UIRenderObject> one, two;
    HE::emitUITextGlyphs("ab", { 0.0f, 0.0f }, { 200.0f, 60.0f }, 16.0f,
                         { 1, 1, 1, 1 }, 0, false, one);
    HE::emitUITextGlyphs("ab\ncd", { 0.0f, 0.0f }, { 200.0f, 60.0f }, 16.0f,
                         { 1, 1, 1, 1 }, 0, false, two);
    CHECK(one.size() == 2);
    CHECK(two.size() == 4);              // the newline is no longer swallowed
    // The second line sits BELOW the first.
    CHECK(two[2].position.y > two[0].position.y);
    // A single line still lays out exactly where it always did.
    CHECK(two[0].position.y < one[0].position.y);   // block centred → line 1 moves up
}

// ── Auto-size ─────────────────────────────────────────────────────────────────
TEST_CASE("UIText auto-size grows the element with the font size")
{
    HE::UIText t;
    t.text = "Hello";
    t.autoSize = true;
    t.fontSize = 20.0f;
    t.applyAutoSize(t.sizeX);
    const float w20 = t.sizeX, h20 = t.sizeY;
    CHECK(w20 > 0.0f);
    CHECK(h20 > 0.0f);

    t.fontSize = 60.0f;
    t.applyAutoSize(t.sizeX);
    CHECK(t.sizeX > w20);
    CHECK(t.sizeY > h20);

    // More lines → taller, same width class.
    t.text = "Hello\nWorld";
    t.applyAutoSize(t.sizeX);
    CHECK(t.sizeY > 60.0f);

    // Off = the authored box is left alone.
    HE::UIText fixed;
    fixed.autoSize = false;
    fixed.fontSize = 80.0f;
    const float sx = fixed.sizeX, sy = fixed.sizeY;
    fixed.applyAutoSize(fixed.sizeX);
    CHECK(fixed.sizeX == doctest::Approx(sx));
    CHECK(fixed.sizeY == doctest::Approx(sy));
}

TEST_CASE("UIText auto-size with WordWrap keeps the authored width")
{
    HE::UIText t;
    t.text     = "a long sentence that certainly wraps at this width";
    t.autoSize = true;
    t.wordWrap = true;
    t.sizeX    = 120.0f;
    t.fontSize = 18.0f;
    t.applyAutoSize(t.sizeX);
    CHECK(t.sizeX == doctest::Approx(120.0f)); // width is the wrap column
    CHECK(t.sizeY > 18.0f);                    // height grew to hold the lines
}

TEST_CASE("UIText multi-line properties round-trip through JSON")
{
    HE::UIText t;
    t.text = "a\nb"; t.wordWrap = true; t.autoSize = true; t.alignH = 1;
    nlohmann::json j;
    t.writeJson(j);
    HE::UIText r;
    r.readJson(j);
    CHECK(r.text == "a\nb");
    CHECK(r.wordWrap);
    CHECK(r.autoSize);
    CHECK(r.alignH == 1);

    // Pre-auto-size widgets keep their hand-set box (autoSize defaults off).
    nlohmann::json legacy = { { "text", "x" }, { "fontSize", 22.0f } };
    HE::UIText old;
    old.readJson(legacy);
    CHECK(!old.autoSize);
}

// ═══ Tree container semantics ════════════════════════════════════════════════

TEST_CASE("UIWidgetTree never reuses an element id after removeSubtree")
{
    // Same rule as the node graphs (GraphCommon/GraphModel.h): nextId only moves
    // forward, so a stale id can never re-bind to a different element.
    HE::UIWidgetTree t;
    const int a = t.add(HE::UIWidgetType::Panel);
    const int b = t.add(HE::UIWidgetType::Text);
    t.removeSubtree(a);
    t.removeSubtree(b);
    const int c = t.add(HE::UIWidgetType::Image);
    CHECK(c != a);
    CHECK(c != b);
    CHECK(c > b);
}

TEST_CASE("uiWidgetTreeFromJson loads a hand-written OLD-format document")
{
    // The shape uiWidgetTreeToJson writes: pretty dump, element type by NAME,
    // pos/size/pivot as 2-arrays, optional material/font/hitTestable/hoverCursor
    // keys omitted at their defaults. Kept as a literal so refactoring the writer
    // can never quietly redefine what still loads.
    const std::string old =
        R"J({"canvasWidth":1280.0,"canvasHeight":720.0,"nextId":3,)J"
        R"J("elements":[{"id":1,"parent":0,"type":"Panel","name":"Root","pos":[0.0,0.0],)J"
        R"J("size":[400.0,300.0],"pivot":[0.0,0.0],"anchor":0,"layer":0,"visible":true},)J"
        R"J({"id":2,"parent":1,"type":"Text","name":"Label","pos":[10.0,10.0],)J"
        R"J("size":[100.0,24.0],"pivot":[0.0,0.0],"anchor":4,"layer":1,"visible":false,)J"
        R"J("text":"hi"}]})J";

    HE::UIWidgetTree t;
    REQUIRE(HE::uiWidgetTreeFromJson(old, t));
    CHECK(t.canvasWidth == doctest::Approx(1280.0f));
    REQUIRE(t.elements.size() == 2);
    CHECK(t.find(1) != nullptr);
    CHECK(t.find(2)->parentId == 1);
    // The 9-point "anchor" field still reads as the point anchor it named.
    CHECK(HE::uiAnchorLegacyPointOf(*t.find(2)) == 4);
    CHECK(t.find(2)->anchorMinX == doctest::Approx(0.5f));
    CHECK(t.find(2)->anchorMaxY == doctest::Approx(0.5f));
    CHECK(t.find(2)->layer == 1);
    CHECK_FALSE(t.find(2)->visible);
    CHECK(t.find(1)->hitTestable);                             // absent key → default
    CHECK(t.find(1)->hoverCursor == HE::UICursor::Default);    // absent key → default
    CHECK(t.nextId == 3);
    const std::vector<int> kids = t.childrenOf(1);
    REQUIRE(kids.size() == 1);
    CHECK(kids[0] == 2);
}

TEST_CASE("uiWidgetTreeFromJson repairs nextId when a saved id is >= it")
{
    const std::string json =
        R"J({"canvasWidth":1920.0,"canvasHeight":1080.0,"nextId":1,)J"
        R"J("elements":[{"id":9,"parent":0,"type":"Panel","name":"A"}]})J";
    HE::UIWidgetTree t;
    REQUIRE(HE::uiWidgetTreeFromJson(json, t));
    CHECK(t.nextId == 10);
    CHECK(t.add(HE::UIWidgetType::Panel) == 10);
}

// ═══ HorizonCode ⇄ UI property coercion ══════════════════════════════════════

TEST_CASE("uiHcValueToProp follows HorizonCode's coercion rule")
{
    using HCV = HorizonCode::Value;
    // Only Float↔Int↔Bool convert.
    CHECK(HE::uiHcValueToProp(HCV::ofInt(3),      HE::UIPropType::Float).f == doctest::Approx(3.0f));
    CHECK(HE::uiHcValueToProp(HCV::ofBool(true),  HE::UIPropType::Float).f == doctest::Approx(1.0f));
    CHECK(HE::uiHcValueToProp(HCV::ofFloat(2.7f), HE::UIPropType::Int).i == 2);
    CHECK(HE::uiHcValueToProp(HCV::ofFloat(0.0f), HE::UIPropType::Bool).b == false);
    CHECK(HE::uiHcValueToProp(HCV::ofFloat(0.5f), HE::UIPropType::Bool).b == true);
    // Any other mismatch yields the target's zero.
    CHECK(HE::uiHcValueToProp(HCV::ofString("x"), HE::UIPropType::Float).f == doctest::Approx(0.0f));
}

TEST_CASE("uiHcValueToProp passes ARRAYS through uncoerced")
{
    // REGRESSION: arrays are never scalar-coerced (HorizonCode.cpp `coerce`, and
    // hc::coerce* in HorizonCodeGenSupport.h) — the wanted type's RAW field is
    // read. `type` on an array names the ELEMENT type, so cross-converting off it
    // produced garbage: an Int-element array landing on a Float property used to
    // be read as (float)v.i instead of v.f.
    HorizonCode::Value arr;
    arr.isArray = true;
    arr.type    = HorizonCode::PinType::Int;   // element type
    arr.i       = 42;                          // would leak in via the old Int→Float rule
    arr.f       = 1.5f;                        // the raw field a passthrough must read
    arr.items   = { HorizonCode::Value::ofInt(1), HorizonCode::Value::ofInt(2) };

    CHECK(HE::uiHcValueToProp(arr, HE::UIPropType::Float).f == doctest::Approx(1.5f));

    HorizonCode::Value arrF;
    arrF.isArray = true;
    arrF.type    = HorizonCode::PinType::Float;
    arrF.f       = 3.9f;
    arrF.i       = 7;
    arrF.b       = true;
    CHECK(HE::uiHcValueToProp(arrF, HE::UIPropType::Int).i == 7);
    CHECK(HE::uiHcValueToProp(arrF, HE::UIPropType::Bool).b == true);
}

// ═══ Anchors ═════════════════════════════════════════════════════════════════
// An anchor used to be one of nine POINTS: an element hung off it and kept its
// own size, so a menu bar meant to span its parent had to be resized by hand
// every time the parent changed. The anchor is a RECTANGLE now — a point when
// its two corners meet, a whole side when it spans one axis, the whole parent
// when it spans both — and an element anchored to a span grows with it.

namespace
{
    // A tree with one panel, so a child has a parent whose size can change
    // under it. Returns the panel's id.
    int panelTree(HE::UIWidgetTree& t, float w = 400.0f, float h = 300.0f)
    {
        t.canvasWidth = 1000.0f; t.canvasHeight = 800.0f;
        const int p = t.add(HE::UIWidgetType::Panel);
        HE::UIElement& pe = *t.find(p);
        HE::uiSetAnchorPreset(pe, 0);          // top-left point
        pe.pivotX = pe.pivotY = 0.0f;
        pe.posX = 100.0f; pe.posY = 50.0f;
        pe.sizeX = w;     pe.sizeY = h;
        return p;
    }
}

TEST_CASE("uiElementRect: a point anchor lays out exactly as it always did")
{
    HE::UIWidgetTree t;
    const int p = panelTree(t);
    const int c = t.add(HE::UIWidgetType::Button);
    HE::UIElement& e = *t.find(c);
    e.parentId = p;
    HE::uiSetAnchorPreset(e, 5);               // middle centre
    e.pivotX = e.pivotY = 0.5f;
    e.posX = 0.0f; e.posY = 0.0f;
    e.sizeX = 100.0f; e.sizeY = 40.0f;

    // Parent spans (100,50)-(500,350); its centre is (300,200).
    const HE::UIWidgetRect r = HE::uiElementRect(t, e);
    CHECK(r.x == doctest::Approx(250.0f));
    CHECK(r.y == doctest::Approx(180.0f));
    CHECK(r.w == doctest::Approx(100.0f));
    CHECK(r.h == doctest::Approx(40.0f));

    // A point anchor ignores the parent's size: it only moves with the point.
    t.find(p)->sizeX = 800.0f;
    const HE::UIWidgetRect r2 = HE::uiElementRect(t, e);
    CHECK(r2.w == doctest::Approx(100.0f));
    CHECK(r2.x == doctest::Approx(450.0f));
}

TEST_CASE("uiElementRect: an element anchored to a whole side grows with it")
{
    HE::UIWidgetTree t;
    const int p = panelTree(t);
    const int c = t.add(HE::UIWidgetType::Panel);
    HE::UIElement& e = *t.find(c);
    e.parentId = p;
    HE::uiSetAnchorPreset(e, 3);               // the parent's whole TOP side
    e.pivotX = e.pivotY = 0.0f;
    HE::uiSetAnchorInsetsX(e, 20.0f, 30.0f);   // 20 in from the left, 30 from the right
    e.posY = 0.0f; e.sizeY = 48.0f;            // the Y axis is still a point

    HE::UIWidgetRect r = HE::uiElementRect(t, e);
    CHECK(r.x == doctest::Approx(120.0f));     // parent.x + 20
    CHECK(r.w == doctest::Approx(350.0f));     // parent.w - 20 - 30
    CHECK(r.y == doctest::Approx(50.0f));
    CHECK(r.h == doctest::Approx(48.0f));

    // The whole point: widen the parent and the bar follows, margins intact.
    t.find(p)->sizeX = 900.0f;
    r = HE::uiElementRect(t, e);
    CHECK(r.x == doctest::Approx(120.0f));
    CHECK(r.w == doctest::Approx(850.0f));
    CHECK(r.h == doctest::Approx(48.0f));      // …and nothing happened to Y

    // The insets read back as they were set, whatever the pivot is.
    e.pivotX = 0.3f;
    HE::uiSetAnchorInsetsX(e, 12.0f, 34.0f);
    float left = 0.0f, right = 0.0f;
    HE::uiAnchorInsetsX(e, left, right);
    CHECK(left  == doctest::Approx(12.0f));
    CHECK(right == doctest::Approx(34.0f));
    r = HE::uiElementRect(t, e);
    CHECK(r.x == doctest::Approx(112.0f));
    CHECK(r.w == doctest::Approx(900.0f - 12.0f - 34.0f));
}

TEST_CASE("uiElementRect: the fill anchor takes the whole available space")
{
    HE::UIWidgetTree t;
    const int p = panelTree(t);
    const int c = t.add(HE::UIWidgetType::Image);
    HE::UIElement& e = *t.find(c);
    e.parentId = p;
    HE::uiSetAnchorPreset(e, HE::kUIAnchorFill);
    HE::uiSetAnchorInsetsX(e, 0.0f, 0.0f);
    HE::uiSetAnchorInsetsY(e, 0.0f, 0.0f);

    HE::UIWidgetRect r = HE::uiElementRect(t, e);
    const HE::UIWidgetRect pr = HE::uiElementRect(t, *t.find(p));
    CHECK(r.x == doctest::Approx(pr.x));
    CHECK(r.y == doctest::Approx(pr.y));
    CHECK(r.w == doctest::Approx(pr.w));
    CHECK(r.h == doctest::Approx(pr.h));

    // With a margin all round, and through a parent resize.
    HE::uiSetAnchorInsetsX(e, 10.0f, 10.0f);
    HE::uiSetAnchorInsetsY(e, 10.0f, 10.0f);
    t.find(p)->sizeX = 640.0f; t.find(p)->sizeY = 480.0f;
    r = HE::uiElementRect(t, e);
    CHECK(r.w == doctest::Approx(620.0f));
    CHECK(r.h == doctest::Approx(460.0f));

    // A root element fills the CANVAS the same way — "the whole available
    // space" is the canvas for anything not inside a panel.
    const int root = t.add(HE::UIWidgetType::Panel);
    HE::UIElement& re = *t.find(root);
    HE::uiSetAnchorPreset(re, HE::kUIAnchorFill);
    HE::uiSetAnchorInsetsX(re, 0.0f, 0.0f);
    HE::uiSetAnchorInsetsY(re, 0.0f, 0.0f);
    const HE::UIWidgetRect rr = HE::uiElementRect(t, re);
    CHECK(rr.x == doctest::Approx(0.0f));
    CHECK(rr.y == doctest::Approx(0.0f));
    CHECK(rr.w == doctest::Approx(t.canvasWidth));
    CHECK(rr.h == doctest::Approx(t.canvasHeight));
}

TEST_CASE("uiElementRect: a stretched element carries its children along")
{
    HE::UIWidgetTree t;
    const int p = panelTree(t);
    const int bar = t.add(HE::UIWidgetType::Panel);
    HE::UIElement& b = *t.find(bar);
    b.parentId = p;
    HE::uiSetAnchorPreset(b, 3);                  // whole top side
    HE::uiSetAnchorInsetsX(b, 0.0f, 0.0f);
    b.pivotY = 0.0f; b.posY = 0.0f; b.sizeY = 60.0f;

    const int btn = t.add(HE::UIWidgetType::Button);
    HE::UIElement& e = *t.find(btn);
    e.parentId = bar;
    HE::uiSetAnchorPreset(e, 6);                  // its parent's top-RIGHT corner
    e.pivotX = 1.0f; e.pivotY = 0.0f;
    e.posX = -10.0f; e.posY = 10.0f;
    e.sizeX = 80.0f; e.sizeY = 30.0f;

    HE::UIWidgetRect r = HE::uiElementRect(t, e);
    CHECK(r.x == doctest::Approx(410.0f));        // parent right (500) - 10 - 80
    t.find(p)->sizeX = 900.0f;
    r = HE::uiElementRect(t, e);
    CHECK(r.x == doctest::Approx(910.0f));        // the bar grew, the button rode along
}

TEST_CASE("uiAnchorPreset: the sixteen rectangles name themselves")
{
    HE::UIWidgetTree t;
    const int c = t.add(HE::UIWidgetType::Panel);
    HE::UIElement& e = *t.find(c);
    for (int p = 0; p < HE::kUIAnchorPresetCount; ++p)
    {
        HE::uiSetAnchorPreset(e, p);
        CHECK(HE::uiAnchorPresetOf(e) == p);
    }
    // The nine points keep the legacy numbering the "anchor" field spells.
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
        {
            HE::uiSetAnchorPreset(e, row * 4 + col);
            CHECK(HE::uiAnchorLegacyPointOf(e) == row * 3 + col);
        }
    // A span has no 9-point name — and must not be given one.
    HE::uiSetAnchorPreset(e, HE::kUIAnchorFill);
    CHECK(HE::uiAnchorLegacyPointOf(e) == -1);
    HE::uiSetAnchorPreset(e, 3);
    CHECK(HE::uiAnchorLegacyPointOf(e) == -1);
    // A rectangle none of them names stays unnamed rather than being rounded
    // to the nearest button.
    e.anchorMinX = 0.2f; e.anchorMaxX = 0.7f;
    CHECK(HE::uiAnchorPresetOf(e) == -1);
    // Out of range is the top-left point, never a crash.
    HE::uiSetAnchorPreset(e, 99);
    CHECK(HE::uiAnchorPresetOf(e) == 0);
}

TEST_CASE("uiReanchorKeepingRect: re-anchoring does not move the element")
{
    HE::UIWidgetTree t;
    const int p = panelTree(t);
    const int c = t.add(HE::UIWidgetType::Button);
    HE::UIElement& e = *t.find(c);
    e.parentId = p;
    e.pivotX = 0.25f; e.pivotY = 0.75f;
    e.posX = 40.0f; e.posY = 30.0f;
    e.sizeX = 120.0f; e.sizeY = 32.0f;

    const HE::UIWidgetRect before = HE::uiElementRect(t, e);
    for (int preset = 0; preset < HE::kUIAnchorPresetCount; ++preset)
    {
        HE::uiReanchorKeepingRect(t, e, preset);
        const HE::UIWidgetRect after = HE::uiElementRect(t, e);
        CHECK(after.x == doctest::Approx(before.x));
        CHECK(after.y == doctest::Approx(before.y));
        CHECK(after.w == doctest::Approx(before.w));
        CHECK(after.h == doctest::Approx(before.h));
        CHECK(HE::uiAnchorPresetOf(e) == preset);
    }
    // …and once it is anchored to the whole parent, THEN the parent's size
    // starts to matter.
    HE::uiReanchorKeepingRect(t, e, HE::kUIAnchorFill);
    t.find(p)->sizeX = 800.0f;
    const HE::UIWidgetRect grown = HE::uiElementRect(t, e);
    CHECK(grown.w == doctest::Approx(before.w + 400.0f));
}

TEST_CASE("UIWidgetTree JSON: a span anchor round-trips, a point one stays old-format")
{
    HE::UIWidgetTree t;
    const int a = t.add(HE::UIWidgetType::Panel);
    HE::uiSetAnchorPreset(*t.find(a), 5);            // the middle-centre POINT
    const std::string pointOnly = HE::uiWidgetTreeToJson(t);
    // Nothing new is written for a document of point anchors — an existing
    // asset saves byte-identical to what earlier versions wrote, down to the
    // 9-point number (grid preset 5 is the old anchor 4).
    CHECK(pointOnly.find("anchorMin") == std::string::npos);
    CHECK(pointOnly.find("\"anchor\": 4") != std::string::npos);

    const int b = t.add(HE::UIWidgetType::Panel);
    HE::UIElement& be = *t.find(b);
    HE::uiSetAnchorPreset(be, HE::kUIAnchorFill);
    HE::uiSetAnchorInsetsX(be, 8.0f, 12.0f);
    HE::uiSetAnchorInsetsY(be, 4.0f, 6.0f);

    HE::UIWidgetTree r;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), r));
    CHECK(HE::uiAnchorPresetOf(*r.find(a)) == 5);
    CHECK(HE::uiAnchorPresetOf(*r.find(b)) == HE::kUIAnchorFill);
    float left = 0.0f, right = 0.0f;
    HE::uiAnchorInsetsX(*r.find(b), left, right);
    CHECK(left  == doctest::Approx(8.0f));
    CHECK(right == doctest::Approx(12.0f));

    // The single-element form collaboration sends carries it too.
    const std::unique_ptr<HE::UIElement> one =
        HE::uiElementFromJson(HE::uiElementToJson(be));
    REQUIRE(one != nullptr);
    CHECK(HE::uiAnchorPresetOf(*one) == HE::kUIAnchorFill);
}

// ═══ Canvas scale modes ══════════════════════════════════════════════════════
// The runtime used to scale the canvas per axis so it always covered the
// viewport exactly — which distorts every element the moment the screen's
// aspect differs from the authored one. Every mode but Stretch scales both axes
// by ONE factor and hands the layout a canvas as large as the screen actually
// is, so nothing is squashed and an edge-anchored element still reaches the
// edge.

TEST_CASE("uiResolveCanvas: Stretch is the old per-axis fit")
{
    HE::UIWidgetTree t;
    t.canvasWidth = 1920.0f; t.canvasHeight = 1080.0f;
    REQUIRE(t.scaleMode == HE::UICanvasScaleMode::Stretch);

    const HE::UIWidgetCanvas c = HE::uiResolveCanvas(t, 2560.0f, 1080.0f);
    CHECK(c.width  == doctest::Approx(1920.0f));   // the canvas IS the authored one
    CHECK(c.height == doctest::Approx(1080.0f));
    CHECK(c.scaleX == doctest::Approx(2560.0f / 1920.0f));
    CHECK(c.scaleY == doctest::Approx(1.0f));
    CHECK(c.scaleX != doctest::Approx(c.scaleY));  // …and that is the distortion
}

TEST_CASE("uiResolveCanvas: the uniform modes never distort")
{
    HE::UIWidgetTree t;
    t.canvasWidth = 1920.0f; t.canvasHeight = 1080.0f;
    const float vw = 2560.0f, vh = 1080.0f;   // 21:9, wider than authored

    t.scaleMode = HE::UICanvasScaleMode::FitInside;
    HE::UIWidgetCanvas c = HE::uiResolveCanvas(t, vw, vh);
    CHECK(c.scaleX == doctest::Approx(c.scaleY));
    CHECK(c.scaleX == doctest::Approx(1.0f));                 // min(1.333, 1.0)
    CHECK(c.width  == doctest::Approx(2560.0f));              // the canvas GREW sideways
    CHECK(c.height == doctest::Approx(1080.0f));

    t.scaleMode = HE::UICanvasScaleMode::FillOutside;
    c = HE::uiResolveCanvas(t, vw, vh);
    CHECK(c.scaleX == doctest::Approx(c.scaleY));
    CHECK(c.scaleX == doctest::Approx(2560.0f / 1920.0f));     // max
    CHECK(c.width  == doctest::Approx(1920.0f));
    CHECK(c.height == doctest::Approx(810.0f));               // less canvas vertically

    t.scaleMode = HE::UICanvasScaleMode::MatchWidth;
    c = HE::uiResolveCanvas(t, vw, vh);
    CHECK(c.width == doctest::Approx(1920.0f));               // authored width fits

    t.scaleMode = HE::UICanvasScaleMode::MatchHeight;
    c = HE::uiResolveCanvas(t, vw, vh);
    CHECK(c.height == doctest::Approx(1080.0f));              // authored height fits

    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    c = HE::uiResolveCanvas(t, vw, vh);
    CHECK(c.scaleX == doctest::Approx(1.0f));
    CHECK(c.width  == doctest::Approx(vw));                   // one unit = one pixel
    CHECK(c.height == doctest::Approx(vh));

    // A degenerate viewport resolves to something usable rather than dividing
    // by zero — the editor asks for one every time a panel is dragged shut.
    t.scaleMode = HE::UICanvasScaleMode::FitInside;
    c = HE::uiResolveCanvas(t, 0.0f, 0.0f);
    CHECK(c.scaleX > 0.0f);
    CHECK(c.width  > 0.0f);
}

// ── HiDPI (docs/he-apps-plan.md F2) ─────────────────────────────────────────
// A display at 200% is two things at once, and SDL keeps them apart: the window
// may simply HAVE twice the pixels (macOS Retina, pixel density 2), or it may
// have the same pixels and a content scale of 2 telling the app to draw bigger
// (Windows, X11). SDL_GetWindowDisplayScale is the product, and it is the only
// number a layout needs.
//
// Five of the six modes measure against the viewport, so more pixels is all
// they ever see and the scaling arrives on its own. ConstantPixel is the one
// that has to be told — and it was the one that was wrong on EVERY platform,
// macOS included: "one unit is one pixel" drew a 200% display's UI at half
// size, which is the "ignoring the content scale makes graphics appear tiny"
// case SDL's own README-highdpi warns about.
TEST_CASE("Canvas scale: only ConstantPixel reads the display scale")
{
    HE::UIWidgetTree t;
    t.canvasWidth = 1920.0f; t.canvasHeight = 1080.0f;
    const float vw = 3840.0f, vh = 2160.0f;   // a 4K panel at 200%

    // The five viewport-relative modes are byte-identical either way: they
    // already counted the pixels, and counting the scaling too would double it.
    for (const HE::UICanvasScaleMode m : { HE::UICanvasScaleMode::Stretch,
                                           HE::UICanvasScaleMode::FitInside,
                                           HE::UICanvasScaleMode::FillOutside,
                                           HE::UICanvasScaleMode::MatchWidth,
                                           HE::UICanvasScaleMode::MatchHeight })
    {
        t.scaleMode = m;
        const HE::UIWidgetCanvas a = HE::uiResolveCanvas(t, vw, vh, 1.0f);
        const HE::UIWidgetCanvas b = HE::uiResolveCanvas(t, vw, vh, 2.0f);
        CHECK(a.scaleX == doctest::Approx(b.scaleX));
        CHECK(a.scaleY == doctest::Approx(b.scaleY));
        CHECK(a.width  == doctest::Approx(b.width));
        CHECK(a.height == doctest::Approx(b.height));
    }

    // ConstantPixel: one unit is one DEVICE-INDEPENDENT pixel. At 200% it is
    // worth two real ones, so a 100-unit button covers 200 pixels and keeps the
    // width the author drew instead of shrinking to half of it.
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    HE::UIWidgetCanvas c = HE::uiResolveCanvas(t, vw, vh, 2.0f);
    CHECK(c.scaleX == doctest::Approx(2.0f));
    CHECK(c.scaleY == doctest::Approx(2.0f));
    CHECK(c.width  == doctest::Approx(1920.0f));   // …and the canvas is in DIPs,
    CHECK(c.height == doctest::Approx(1080.0f));   // so anchors still reach the edge

    // Windows' 150% is the common case, not the tidy 2×.
    c = HE::uiResolveCanvas(t, 2880.0f, 1620.0f, 1.5f);
    CHECK(c.scaleX == doctest::Approx(1.5f));
    CHECK(c.width  == doctest::Approx(1920.0f));

    // An unscaled display is what the default argument means, and it has to be
    // exactly what the mode did before any of this existed.
    c = HE::uiResolveCanvas(t, vw, vh);
    CHECK(c.scaleX == doctest::Approx(1.0f));
    CHECK(c.width  == doctest::Approx(vw));

    // A platform that answers 0 (or worse) must not collapse the layout into a
    // single point: the clamp is the difference between a wrong size and a
    // division that takes the canvas with it.
    c = HE::uiResolveCanvas(t, vw, vh, 0.0f);
    CHECK(c.scaleX > 0.0f);
    CHECK(c.width  > 0.0f);
    CHECK(std::isfinite(c.width));
}

TEST_CASE("Canvas scale: an embedded ConstantPixel widget inherits, it does not re-apply")
{
    // A WidgetRef's slot is measured in the HOST's canvas units, not in pixels,
    // so a nested ConstantPixel widget means "unscaled relative to whoever
    // embedded me". The host's own factor already carries the display scale;
    // handing it to the slot as well would apply 200% twice and draw the
    // embedded component at four times its size.
    HE::UIWidgetTree page;
    page.canvasWidth = 1920.0f; page.canvasHeight = 1080.0f;
    page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;

    const int refId = page.add(HE::UIWidgetType::WidgetRef);
    auto* r = dynamic_cast<HE::UIWidgetRef*>(page.find(refId));
    REQUIRE(r != nullptr);
    HE::uiSetAnchorPreset(*r, 0); r->pivotX = r->pivotY = 0.0f;
    r->posX = 0.0f; r->posY = 0.0f; r->sizeX = 400.0f; r->sizeY = 200.0f;
    r->contentW = 400.0f; r->contentH = 200.0f;
    r->contentMode = HE::UICanvasScaleMode::ConstantPixel;

    const int childId = page.add(HE::UIWidgetType::Button);
    page.find(childId)->parentId = refId;

    const HE::UIWidgetCanvas hostAt2 = HE::uiResolveCanvas(page, 3840.0f, 2160.0f, 2.0f);
    float us = 0.0f, vs = 0.0f;
    HE::uiElementUnitScale(page, *page.find(childId), us, vs, &hostAt2);
    CHECK(us == doctest::Approx(1.0f));   // one sub-unit is one HOST unit…
    CHECK(vs == doctest::Approx(1.0f));
    // …and the host unit is worth two pixels, so the composition is 2, not 4.
    CHECK(us * hostAt2.scaleX == doctest::Approx(2.0f));
}

TEST_CASE("Canvas scale: a square stays square, and the edge stays the edge")
{
    HE::UIWidgetTree t;
    t.canvasWidth = 1920.0f; t.canvasHeight = 1080.0f;
    t.scaleMode = HE::UICanvasScaleMode::FitInside;

    // A square badge in the middle…
    const int badge = t.add(HE::UIWidgetType::Panel);
    { HE::UIElement& e = *t.find(badge);
      HE::uiSetAnchorPreset(e, 5);
      e.posX = e.posY = 0.0f; e.sizeX = e.sizeY = 200.0f; }
    // …and a bar pinned to the whole bottom side.
    const int bar = t.add(HE::UIWidgetType::Panel);
    { HE::UIElement& e = *t.find(bar);
      HE::uiSetAnchorPreset(e, 11);                  // bottom row, stretched width
      HE::uiSetAnchorInsetsX(e, 0.0f, 0.0f);
      e.pivotY = 1.0f; e.posY = 0.0f; e.sizeY = 80.0f; }

    const float vw = 2560.0f, vh = 1080.0f;
    const HE::UIWidgetCanvas c = HE::uiResolveCanvas(t, vw, vh);

    const HE::UIWidgetRect b = HE::uiElementRect(t, *t.find(badge), &c);
    CHECK(b.w * c.scaleX == doctest::Approx(b.h * c.scaleY));  // square on screen
    CHECK(b.w * c.scaleX == doctest::Approx(200.0f));          // and its authored size

    const HE::UIWidgetRect r = HE::uiElementRect(t, *t.find(bar), &c);
    CHECK(r.x * c.scaleX == doctest::Approx(0.0f));            // reaches the left edge
    CHECK((r.x + r.w) * c.scaleX == doctest::Approx(vw));      // …and the right one
    CHECK((r.y + r.h) * c.scaleY == doctest::Approx(vh));      // sits on the bottom
    CHECK(r.h * c.scaleY == doctest::Approx(80.0f));           // authored thickness

    // Under Stretch the same bar is just as wide, but the badge is an egg.
    t.scaleMode = HE::UICanvasScaleMode::Stretch;
    const HE::UIWidgetCanvas s = HE::uiResolveCanvas(t, vw, vh);
    const HE::UIWidgetRect bs = HE::uiElementRect(t, *t.find(badge), &s);
    CHECK(bs.w * s.scaleX != doctest::Approx(bs.h * s.scaleY));
}

TEST_CASE("UIWidgetTree JSON: the scale mode survives, Stretch writes nothing")
{
    HE::UIWidgetTree t;
    t.add(HE::UIWidgetType::Panel);
    CHECK(HE::uiWidgetTreeToJson(t).find("scaleMode") == std::string::npos);

    t.scaleMode = HE::UICanvasScaleMode::FitInside;
    const std::string json = HE::uiWidgetTreeToJson(t);
    CHECK(json.find("scaleMode") != std::string::npos);
    HE::UIWidgetTree r;
    REQUIRE(HE::uiWidgetTreeFromJson(json, r));
    CHECK(r.scaleMode == HE::UICanvasScaleMode::FitInside);

    // A copy carries it (the runtime holds a deep copy per live widget).
    HE::UIWidgetTree copy = t;
    CHECK(copy.scaleMode == HE::UICanvasScaleMode::FitInside);

    // Nonsense in the file reads as the default rather than as an enum that
    // does not exist.
    HE::UIWidgetTree bad;
    REQUIRE(HE::uiWidgetTreeFromJson(
        R"J({"canvasWidth":800.0,"canvasHeight":600.0,"scaleMode":99,"nextId":1,"elements":[]})J", bad));
    CHECK(bad.scaleMode == HE::UICanvasScaleMode::Stretch);
}

TEST_CASE("WidgetManager: the hit test follows the canvas scale mode")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 1920.0f; t.canvasHeight = 1080.0f;
    t.scaleMode = HE::UICanvasScaleMode::FitInside;
    const int btn = t.add(HE::UIWidgetType::Button);
    HE::UIElement& e = *t.find(btn);
    e.setProp("Text", HE::UIPropValue::ofString(""));
    HE::uiSetAnchorPreset(e, 0);                    // top-left point anchor
    e.pivotX = e.pivotY = 0.0f;
    e.posX = 0.0f; e.posY = 0.0f;
    e.sizeX = 200.0f; e.sizeY = 100.0f;
    registerWidget(cm, t);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);

    // On a 21:9 screen the uniform factor is 1.0, so the button covers exactly
    // its authored 200x100 pixels — a per-axis stretch would have made it 266
    // wide and the point at (240, 50) would have been inside it.
    CHECK(wm.processPointer(2560.0f, 1080.0f, 190.0f, 50.0f, true, true));
    CHECK_FALSE(wm.processPointer(2560.0f, 1080.0f, 240.0f, 50.0f, true, true));
}

// ═══ Texture on a quad ═══════════════════════════════════════════════════════
// An Image element had a tint and a material slot and nothing else, so showing
// a PNG in the UI meant authoring a material graph with a texture node in it.
// The texture slot is the plain path; a material still wins where both are set.

TEST_CASE("UIElement: the texture slot round-trips and reaches the quad")
{
    HE::UIWidgetTree t;
    const int img = t.add(HE::UIWidgetType::Image);
    HE::UIElement& e = *t.find(img);
    CHECK(e.hasTextureSlot());
    CHECK(t.find(t.add(HE::UIWidgetType::Panel))->hasTextureSlot());
    CHECK(t.find(t.add(HE::UIWidgetType::Button))->hasTextureSlot());
    // A text run has no quad to put a picture on.
    CHECK_FALSE(t.find(t.add(HE::UIWidgetType::Text))->hasTextureSlot());

    // Nothing is written while it is empty.
    CHECK(HE::uiWidgetTreeToJson(t).find("texture") == std::string::npos);
    e.texture = "Textures/Logo.hasset";
    HE::UIWidgetTree r;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), r));
    CHECK(r.find(img)->texture == "Textures/Logo.hasset");

    // By name, like every other asset slot (HorizonCode, scripts) …
    e.setPropAny("Texture", HE::UIPropValue::ofString("Textures/Other.hasset"));
    CHECK(e.getPropAny("Texture").s == "Textures/Other.hasset");
    // … and it is offered as a property only where the slot exists.
    auto has = [](const HE::UIElement& el, const char* name)
    {
        for (const HE::UIPropDesc& d : el.allProperties()) if (d.name == name) return true;
        return false;
    };
    CHECK(has(e, "Texture"));
    CHECK_FALSE(has(*t.find(4), "Texture"));   // the Text element added above

    // The resolved id is what render() puts on the quad — the path is authoring
    // data, the id is what the backend can bind.
    e.textureAssetId = HE::UUID{ 7, 9 };
    std::vector<UIRenderObject> out;
    e.render({ 0.0f, 0.0f, 64.0f, 64.0f }, {}, HE::UUID{}, 1.0f, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].textureAssetId == HE::UUID{ 7, 9 });
    CHECK(out[0].uvMax.x == doctest::Approx(1.0f));   // full source rect
    CHECK(out[0].uvMax.y == doctest::Approx(1.0f));
    // A clone carries both (the runtime renders from a deep copy).
    CHECK(e.clone()->textureAssetId == HE::UUID{ 7, 9 });
    CHECK(e.clone()->texture == "Textures/Other.hasset");
}

TEST_CASE("WidgetManager: an element's texture path is resolved for the draw")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 100.0f; t.canvasHeight = 100.0f;
    const int img = t.add(HE::UIWidgetType::Image);
    { HE::UIElement& e = *t.find(img);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = e.posY = 0.0f; e.sizeX = e.sizeY = 100.0f;
      // A path that resolves to nothing must leave a NIL id rather than a
      // stale/garbage one — the backend then draws the plain tint.
      e.texture = "Textures/DoesNotExist.hasset"; }
    registerWidget(cm, t);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    std::vector<UIRenderObject> out;
    wm.extract(100.0f, 100.0f, out);
    REQUIRE(out.size() >= 1);
    CHECK(out[0].textureAssetId == HE::UUID{});
}

// ═══ Layout boxes ════════════════════════════════════════════════════════════
// Everything used to be placed by hand: a menu of five buttons was five sets of
// coordinates, and inserting one meant moving four. A box places its children
// itself — they keep their own size along its axis unless they fill, and they
// span it across.

namespace
{
    // A vertical box at (0,0) 200x400 in a 1000x1000 canvas, plus `n` children
    // of the given height. Returns the box id; children follow it in the tree.
    int boxWithChildren(HE::UIWidgetTree& t, HE::UIWidgetType boxType,
                        int n, float childExtent, float padding, float spacing)
    {
        t.canvasWidth = 1000.0f; t.canvasHeight = 1000.0f;
        const int box = t.add(boxType);
        HE::UIElement& b = *t.find(box);
        HE::uiSetAnchorPreset(b, 0);
        b.pivotX = b.pivotY = 0.0f;
        b.posX = 0.0f; b.posY = 0.0f; b.sizeX = 200.0f; b.sizeY = 400.0f;
        b.setProp("Padding", HE::UIPropValue::ofFloat(padding));
        b.setProp("Spacing", HE::UIPropValue::ofFloat(spacing));
        for (int i = 0; i < n; ++i)
        {
            const int c = t.add(HE::UIWidgetType::Panel);
            HE::UIElement& e = *t.find(c);
            e.parentId = box;
            e.sizeX = e.sizeY = childExtent;
            // Deliberately absurd anchors and position: a box child must ignore
            // both, and this is what proves it.
            HE::uiSetAnchorPreset(e, HE::kUIAnchorFill);
            e.posX = 999.0f; e.posY = -999.0f;
        }
        return box;
    }
}

TEST_CASE("VerticalBox: children stack in order and ignore their own anchors")
{
    HE::UIWidgetTree t;
    const int box = boxWithChildren(t, HE::UIWidgetType::VerticalBox, 3, 50.0f,
                                    /*padding=*/10.0f, /*spacing=*/5.0f);
    const std::vector<int> kids = t.childrenOf(box);
    REQUIRE(kids.size() == 3);

    for (size_t i = 0; i < kids.size(); ++i)
    {
        const HE::UIWidgetRect r = HE::uiElementRect(t, *t.find(kids[i]));
        CHECK(r.x == doctest::Approx(10.0f));            // padded from the left…
        CHECK(r.w == doctest::Approx(180.0f));           // …and spanning the rest
        CHECK(r.h == doctest::Approx(50.0f));            // its own height
        CHECK(r.y == doctest::Approx(10.0f + static_cast<float>(i) * 55.0f));
    }

    // Hiding one closes the gap instead of leaving a hole.
    t.find(kids[0])->visible = false;
    CHECK(HE::uiElementRect(t, *t.find(kids[1])).y == doctest::Approx(10.0f));
    CHECK(HE::uiElementRect(t, *t.find(kids[2])).y == doctest::Approx(65.0f));
}

TEST_CASE("HorizontalBox: the same, along the other axis")
{
    HE::UIWidgetTree t;
    const int box = boxWithChildren(t, HE::UIWidgetType::HorizontalBox, 2, 40.0f,
                                    /*padding=*/0.0f, /*spacing=*/10.0f);
    const std::vector<int> kids = t.childrenOf(box);
    REQUIRE(kids.size() == 2);

    const HE::UIWidgetRect a = HE::uiElementRect(t, *t.find(kids[0]));
    const HE::UIWidgetRect b = HE::uiElementRect(t, *t.find(kids[1]));
    CHECK(a.x == doctest::Approx(0.0f));
    CHECK(a.w == doctest::Approx(40.0f));
    CHECK(a.h == doctest::Approx(400.0f));   // spans the box across the axis
    CHECK(b.x == doctest::Approx(50.0f));    // 40 + spacing
}

// A Spacer is a gap that is an element: nothing on screen, nothing to click,
// and its whole effect is on where its siblings end up.
TEST_CASE("Spacer: it pushes its siblings along and draws nothing")
{
    HE::UIWidgetTree t;
    const int box = boxWithChildren(t, HE::UIWidgetType::VerticalBox, 2, 50.0f,
                                    /*padding=*/0.0f, /*spacing=*/0.0f);
    const std::vector<int> kids = t.childrenOf(box);
    REQUIRE(kids.size() == 2);
    CHECK(HE::uiElementRect(t, *t.find(kids[1])).y == doctest::Approx(50.0f));

    // Slide a 30-unit spacer in between them.
    const int sp = t.add(HE::UIWidgetType::Spacer);
    { HE::UIElement& e = *t.find(sp);
      e.parentId = box; e.sizeX = e.sizeY = 30.0f; }
    // It is added last, so it lands after both — the second child does not move.
    CHECK(HE::uiElementRect(t, *t.find(kids[1])).y == doctest::Approx(50.0f));
    CHECK(HE::uiElementRect(t, *t.find(sp)).y == doctest::Approx(100.0f));
    CHECK(HE::uiElementRect(t, *t.find(sp)).h == doctest::Approx(30.0f));

    // In a horizontal box the very same element pushes along x instead — the
    // box's axis decides, which is what makes one Spacer type enough.
    t.find(box)->setProp("Padding", HE::UIPropValue::ofFloat(0.0f));
    HE::UIWidgetTree h;
    const int hbox = boxWithChildren(h, HE::UIWidgetType::HorizontalBox, 1, 50.0f, 0.0f, 0.0f);
    const int hsp  = h.add(HE::UIWidgetType::Spacer);
    { HE::UIElement& e = *h.find(hsp); e.parentId = hbox; e.sizeX = e.sizeY = 30.0f; }
    const int after = h.add(HE::UIWidgetType::Panel);
    { HE::UIElement& e = *h.find(after); e.parentId = hbox; e.sizeX = e.sizeY = 20.0f; }
    CHECK(HE::uiElementRect(h, *h.find(hsp)).x   == doctest::Approx(50.0f));
    CHECK(HE::uiElementRect(h, *h.find(after)).x == doctest::Approx(80.0f));

    // Nothing of it reaches the screen, and nothing of it takes a click.
    auto e = HE::makeUIElement(HE::UIWidgetType::Spacer);
    REQUIRE(e);
    CHECK_FALSE(e->hitTestable);
    CHECK_FALSE(e->interactive());
    CHECK_FALSE(e->hasMaterialSlot());
    CHECK_FALSE(e->hasSurfaceStyle());
    std::vector<UIRenderObject> out;
    e->render({ 0.0f, 0.0f, 40.0f, 40.0f }, HE::UIElementRenderState{}, HE::UUID{}, 1.0f, out);
    CHECK(out.empty());
}

TEST_CASE("Layout box: filling slots share what is left over")
{
    HE::UIWidgetTree t;
    const int box = boxWithChildren(t, HE::UIWidgetType::VerticalBox, 3, 100.0f,
                                    /*padding=*/0.0f, /*spacing=*/0.0f);
    const std::vector<int> kids = t.childrenOf(box);
    // One fixed 100, two filling 1 and 3 → 300 left, split 75 / 225.
    t.find(kids[1])->slotFill = 1.0f;
    t.find(kids[2])->slotFill = 3.0f;

    const HE::UIWidgetRect a = HE::uiElementRect(t, *t.find(kids[0]));
    const HE::UIWidgetRect b = HE::uiElementRect(t, *t.find(kids[1]));
    const HE::UIWidgetRect c = HE::uiElementRect(t, *t.find(kids[2]));
    CHECK(a.h == doctest::Approx(100.0f));
    CHECK(b.h == doctest::Approx(75.0f));
    CHECK(c.h == doctest::Approx(225.0f));
    CHECK(b.y == doctest::Approx(100.0f));
    CHECK(c.y == doctest::Approx(175.0f));
    // Together they fill the box exactly — no rounding hole at the bottom.
    CHECK(c.y + c.h == doctest::Approx(400.0f));

    // Overfilled: the fixed children already exceed the box, so there is
    // nothing to share and a filling slot collapses instead of going negative.
    t.find(kids[0])->sizeY = 900.0f;
    CHECK(HE::uiElementRect(t, *t.find(kids[1])).h == doctest::Approx(0.0f));
}

TEST_CASE("Layout box: it follows its own anchors, and its children follow it")
{
    HE::UIWidgetTree t;
    const int box = boxWithChildren(t, HE::UIWidgetType::VerticalBox, 1, 50.0f, 0.0f, 0.0f);
    // Anchor the box itself to the whole canvas: the box is placed the ordinary
    // way, it is only its CHILDREN that give up their anchors.
    HE::UIElement& b = *t.find(box);
    HE::uiSetAnchorPreset(b, HE::kUIAnchorFill);
    HE::uiSetAnchorInsetsX(b, 100.0f, 100.0f);
    HE::uiSetAnchorInsetsY(b, 0.0f, 0.0f);

    const HE::UIWidgetRect br = HE::uiElementRect(t, b);
    CHECK(br.x == doctest::Approx(100.0f));
    CHECK(br.w == doctest::Approx(800.0f));

    const HE::UIWidgetRect kid = HE::uiElementRect(t, *t.find(t.childrenOf(box)[0]));
    CHECK(kid.x == doctest::Approx(100.0f));
    CHECK(kid.w == doctest::Approx(800.0f));   // rode along with the box
}

TEST_CASE("Layout box: boxes nest, and a box draws nothing itself")
{
    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    const int outer = t.add(HE::UIWidgetType::VerticalBox);
    { HE::UIElement& e = *t.find(outer);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = e.posY = 0.0f; e.sizeX = 400.0f; e.sizeY = 400.0f;
      e.setProp("Padding", HE::UIPropValue::ofFloat(0.0f));
      e.setProp("Spacing", HE::UIPropValue::ofFloat(0.0f)); }
    const int row = t.add(HE::UIWidgetType::HorizontalBox);
    { HE::UIElement& e = *t.find(row);
      e.parentId = outer; e.sizeY = 100.0f;
      e.setProp("Padding", HE::UIPropValue::ofFloat(0.0f));
      e.setProp("Spacing", HE::UIPropValue::ofFloat(0.0f)); }
    const int cell = t.add(HE::UIWidgetType::Panel);
    { HE::UIElement& e = *t.find(cell); e.parentId = row; e.sizeX = 60.0f; }

    const HE::UIWidgetRect r = HE::uiElementRect(t, *t.find(row));
    CHECK(r.h == doctest::Approx(100.0f));
    CHECK(r.w == doctest::Approx(400.0f));
    const HE::UIWidgetRect c = HE::uiElementRect(t, *t.find(cell));
    CHECK(c.w == doctest::Approx(60.0f));
    CHECK(c.h == doctest::Approx(100.0f));     // the row's full height

    // The boxes themselves emit nothing — only the panel does.
    std::vector<UIRenderObject> out;
    t.find(outer)->render({ 0,0,10,10 }, {}, HE::UUID{}, 1.0f, out);
    t.find(row)->render({ 0,0,10,10 }, {}, HE::UUID{}, 1.0f, out);
    CHECK(out.empty());
    t.find(cell)->render({ 0,0,10,10 }, {}, HE::UUID{}, 1.0f, out);
    CHECK(out.size() == 1);
}

TEST_CASE("Layout box: Size To Content measures the box, Min is the floor")
{
    HE::UIWidgetTree t;
    const int box = boxWithChildren(t, HE::UIWidgetType::VerticalBox, 3, 50.0f,
                                    /*padding=*/10.0f, /*spacing=*/5.0f);
    auto* b = dynamic_cast<HE::UIBoxBase*>(t.find(box));
    REQUIRE(b != nullptr);
    b->sizeToContent = true;
    b->minSizeX = b->minSizeY = 0.0f;
    // The children are 50x50 each (boxWithChildren makes them square).
    HE::uiApplyAutoSize(t);
    // 3 x 50 + 2 gaps of 5 + 2 x 10 padding.
    CHECK(b->sizeY == doctest::Approx(3 * 50.0f + 2 * 5.0f + 20.0f));
    // Across: the widest child plus the padding.
    CHECK(b->sizeX == doctest::Approx(50.0f + 20.0f));

    // One entry more and it grows; one hidden and it shrinks. That is the point
    // — a menu is not a rectangle a sixth entry falls out of.
    const std::vector<int> kids = t.childrenOf(box);
    t.find(kids[0])->visible = false;
    HE::uiApplyAutoSize(t);
    CHECK(b->sizeY == doctest::Approx(2 * 50.0f + 5.0f + 20.0f));

    // The floor holds.
    b->minSizeY = 400.0f;
    HE::uiApplyAutoSize(t);
    CHECK(b->sizeY == doctest::Approx(400.0f));

    // A filling child measures as nothing on the axis (its size IS the leftover
    // this is computing) but still counts across it.
    b->minSizeY = 0.0f;
    t.find(kids[0])->visible = true;
    t.find(kids[1])->slotFill = 1.0f;
    t.find(kids[1])->sizeX = 200.0f;
    HE::uiApplyAutoSize(t);
    CHECK(b->sizeY == doctest::Approx(2 * 50.0f + 2 * 5.0f + 20.0f));
    CHECK(b->sizeX == doctest::Approx(200.0f + 20.0f));

    // Off again: the authored size is the box's own business once more.
    b->sizeToContent = false;
    b->sizeY = 777.0f;
    HE::uiApplyAutoSize(t);
    CHECK(b->sizeY == doctest::Approx(777.0f));
}

TEST_CASE("Layout box: nested Size To Content measures inside out")
{
    HE::UIWidgetTree t;
    t.canvasWidth = 1000.0f; t.canvasHeight = 1000.0f;
    const int outer = t.add(HE::UIWidgetType::VerticalBox);
    { auto* b = dynamic_cast<HE::UIBoxBase*>(t.find(outer));
      b->padding = 0.0f; b->spacing = 0.0f; b->sizeToContent = true; }
    const int inner = t.add(HE::UIWidgetType::VerticalBox);
    { auto* b = dynamic_cast<HE::UIBoxBase*>(t.find(inner));
      b->parentId = outer; b->padding = 0.0f; b->spacing = 0.0f; b->sizeToContent = true; }
    for (int i = 0; i < 2; ++i)
    {
        const int c = t.add(HE::UIWidgetType::Panel);
        t.find(c)->parentId = inner;
        t.find(c)->sizeY = 30.0f;
        t.find(c)->sizeX = 40.0f;
    }

    HE::uiApplyAutoSize(t);
    // The inner box measures its two panels…
    CHECK(t.find(inner)->sizeY == doctest::Approx(60.0f));
    CHECK(t.find(inner)->sizeX == doctest::Approx(40.0f));
    // …and the outer one measures the inner box, in the SAME pass. Measured
    // outside-in it would have been one frame behind.
    CHECK(t.find(outer)->sizeY == doctest::Approx(60.0f));
    CHECK(t.find(outer)->sizeX == doctest::Approx(40.0f));
}

TEST_CASE("Layout box: Size To Content round-trips, off by default")
{
    HE::UIWidgetTree t;
    const int v = t.add(HE::UIWidgetType::VerticalBox);
    auto* b = dynamic_cast<HE::UIBoxBase*>(t.find(v));
    REQUIRE(b != nullptr);
    CHECK_FALSE(b->sizeToContent);
    CHECK(HE::uiWidgetTreeToJson(t).find("sizeToContent") == std::string::npos);

    b->sizeToContent = true;
    b->minSizeX = 120.0f; b->minSizeY = 60.0f;
    HE::UIWidgetTree r;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), r));
    auto* back = dynamic_cast<HE::UIBoxBase*>(r.find(v));
    REQUIRE(back != nullptr);
    CHECK(back->sizeToContent);
    CHECK(back->minSizeX == doctest::Approx(120.0f));
    CHECK(back->minSizeY == doctest::Approx(60.0f));
    // By name too — a script can switch it on for a menu that just grew.
    back->setPropAny("Size To Content", HE::UIPropValue::ofBool(false));
    CHECK_FALSE(back->sizeToContent);
    CHECK(back->getPropAny("Min Size").v2.x == doctest::Approx(120.0f));
}

TEST_CASE("Layout box: the new types round-trip by name and carry their slots")
{
    HE::UIWidgetTree t;
    const int v = t.add(HE::UIWidgetType::VerticalBox);
    const int h = t.add(HE::UIWidgetType::HorizontalBox);
    t.find(h)->parentId = v;
    t.find(h)->slotFill = 2.5f;
    t.find(v)->setProp("Padding", HE::UIPropValue::ofFloat(7.0f));
    t.find(v)->setProp("Spacing", HE::UIPropValue::ofFloat(3.0f));

    HE::UIWidgetTree r;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), r));
    REQUIRE(r.find(v) != nullptr);
    CHECK(r.find(v)->type() == HE::UIWidgetType::VerticalBox);
    CHECK(r.find(h)->type() == HE::UIWidgetType::HorizontalBox);
    CHECK(r.find(h)->slotFill == doctest::Approx(2.5f));
    CHECK(r.find(v)->getProp("Padding").f == doctest::Approx(7.0f));
    CHECK(r.find(v)->getProp("Spacing").f == doctest::Approx(3.0f));
    CHECK(r.find(v)->laysOutChildren());
    CHECK(r.find(v)->stacksVertically());
    CHECK_FALSE(r.find(h)->stacksVertically());
    // …and slotFill is reachable by name like every other base property.
    r.find(h)->setPropAny("Slot Fill", HE::UIPropValue::ofFloat(-4.0f));
    CHECK(r.find(h)->slotFill == doctest::Approx(0.0f));   // clamped, never negative
}

TEST_CASE("WidgetManager: a box lays its children out at runtime too")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 200.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int box = t.add(HE::UIWidgetType::VerticalBox);
    { HE::UIElement& e = *t.find(box);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 400.0f;
      e.setProp("Padding", HE::UIPropValue::ofFloat(0.0f));
      e.setProp("Spacing", HE::UIPropValue::ofFloat(0.0f)); }
    for (int i = 0; i < 2; ++i)
    {
        const int b = t.add(HE::UIWidgetType::Button);
        HE::UIElement& e = *t.find(b);
        e.parentId = box;
        e.setProp("Text", HE::UIPropValue::ofString(""));
        e.sizeY = 100.0f;
    }
    registerWidget(cm, t);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    // The second button sits at y 100..200 — that is where the click lands,
    // and nowhere near the (999, -999) its own position claims.
    CHECK(wm.processPointer(200.0f, 400.0f, 100.0f, 150.0f, true, true));
    CHECK_FALSE(wm.processPointer(200.0f, 400.0f, 100.0f, 300.0f, true, true));
}

// ═══ Widget in widget ════════════════════════════════════════════════════════
// A health bar or a settings row had to be copied into every page that wanted
// one. A WidgetRef embeds another asset instead: its tree is grafted in (with
// renumbered ids, so two copies never collide) and its logic runs as its own
// script instance, which is what keeps events going to the right graph.

namespace
{
    // Register a widget asset under a given mem:// path so it can be embedded.
    HE::UUID registerWidgetAs(ContentManager& cm, const std::string& path,
                              const HE::UIWidgetTree& tree,
                              const HorizonCode::Graph* graph = nullptr)
    {
        UIWidgetAsset a;
        a.type = HE::AssetType::Widget;
        a.name = path;
        a.path = path;
        a.treeJson  = HE::uiWidgetTreeToJson(tree);
        a.graphJson = graph ? HorizonCode::toJson(*graph) : std::string();
        return cm.registerWidget(std::move(a));
    }

    // A tiny "row" widget: one button filling a 100x40 canvas.
    HE::UIWidgetTree rowWidget()
    {
        HE::UIWidgetTree t;
        t.canvasWidth = 100.0f; t.canvasHeight = 40.0f;
        const int b = t.add(HE::UIWidgetType::Button);
        HE::UIElement& e = *t.find(b);
        e.setProp("Text", HE::UIPropValue::ofString(""));
        HE::uiSetAnchorPreset(e, HE::kUIAnchorFill);
        HE::uiSetAnchorInsetsX(e, 0.0f, 0.0f);
        HE::uiSetAnchorInsetsY(e, 0.0f, 0.0f);
        return t;
    }
}

TEST_CASE("WidgetRef: the embedded tree is grafted in and laid out in its slot")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    registerWidgetAs(cm, "mem://row.hasset", rowWidget());

    HE::UIWidgetTree page;
    page.canvasWidth = 400.0f; page.canvasHeight = 400.0f;
    page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int ref = page.add(HE::UIWidgetType::WidgetRef);
    { HE::UIElement& e = *page.find(ref);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 50.0f; e.posY = 100.0f; e.sizeX = 200.0f; e.sizeY = 60.0f;
      e.setProp("Widget", HE::UIPropValue::ofString("mem://row.hasset")); }
    registerWidget(cm, page);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);

    // The button came in and fills the ref element's rect — not the canvas of
    // the widget it was authored in.
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);
    REQUIRE_FALSE(out.empty());
    bool found = false;
    for (const UIRenderObject& o : out)
        if (o.size.x == doctest::Approx(200.0f) && o.size.y == doctest::Approx(60.0f) &&
            o.position.x == doctest::Approx(50.0f) && o.position.y == doctest::Approx(100.0f))
            found = true;
    CHECK(found);
    // …and it is clickable there.
    CHECK(wm.processPointer(400.0f, 400.0f, 100.0f, 120.0f, true, true));
    CHECK_FALSE(wm.processPointer(400.0f, 400.0f, 300.0f, 300.0f, true, true));
}

TEST_CASE("WidgetRef: the slot is the embedded widget's screen")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    // A widget authored on 1000x1000 with a button at an absolute (800, 800):
    // dropped into a 200x200 slot it must be FITTED, not hung inside, or that
    // button would sit four times outside its own frame.
    HE::UIWidgetTree big;
    big.canvasWidth = 1000.0f; big.canvasHeight = 1000.0f;   // Stretch by default
    const int b = big.add(HE::UIWidgetType::Button);
    { HE::UIElement& e = *big.find(b);
      e.setProp("Text", HE::UIPropValue::ofString(""));
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 800.0f; e.posY = 800.0f; e.sizeX = 100.0f; e.sizeY = 100.0f; }
    registerWidgetAs(cm, "mem://big.hasset", big);

    HE::UIWidgetTree page;
    page.canvasWidth = 400.0f; page.canvasHeight = 400.0f;
    page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int ref = page.add(HE::UIWidgetType::WidgetRef);
    { HE::UIElement& e = *page.find(ref);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 200.0f;
      e.setProp("Widget", HE::UIPropValue::ofString("mem://big.hasset")); }
    registerWidget(cm, page);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);

    // Scaled by 200/1000: the button lands at (160, 160), 20x20 — inside the
    // 200x200 slot, where it belongs.
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);
    REQUIRE_FALSE(out.empty());
    bool found = false;
    for (const UIRenderObject& o : out)
        if (o.size.x == doctest::Approx(20.0f) && o.size.y == doctest::Approx(20.0f) &&
            o.position.x == doctest::Approx(160.0f) && o.position.y == doctest::Approx(160.0f))
            found = true;
    CHECK(found);
    // Nothing it emitted leaves the frame.
    for (const UIRenderObject& o : out)
    {
        CHECK(o.position.x >= -0.01f);
        CHECK(o.position.x + o.size.x <= doctest::Approx(200.0f));
        CHECK(o.position.y + o.size.y <= doctest::Approx(200.0f));
    }
    // …and it is clickable where it is drawn.
    CHECK(wm.processPointer(400.0f, 400.0f, 170.0f, 170.0f, true, true));
    CHECK_FALSE(wm.processPointer(400.0f, 400.0f, 850.0f, 850.0f, true, true));

    // Growing the slot scales EVERYTHING in it, not just some of it.
    page.find(ref)->sizeX = 400.0f;
    page.find(ref)->sizeY = 400.0f;
    registerWidget(cm, page);
    WidgetManager wide;
    REQUIRE(createShown(wide, cm, "mem://w.hasset") != 0);
    out.clear();
    wide.extract(400.0f, 400.0f, out);
    found = false;
    for (const UIRenderObject& o : out)
        if (o.size.x == doctest::Approx(40.0f) && o.position.x == doctest::Approx(320.0f))
            found = true;
    CHECK(found);
}

TEST_CASE("WidgetRef: an embedded widget's own scale mode decides")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    // The same widget, but authored to keep its units: its anchors do the
    // placing inside the slot and nothing is scaled — which is exactly what
    // anchors are for, and what ConstantPixel means.
    HE::UIWidgetTree fixed;
    fixed.canvasWidth = 1000.0f; fixed.canvasHeight = 1000.0f;
    fixed.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int b = fixed.add(HE::UIWidgetType::Button);
    { HE::UIElement& e = *fixed.find(b);
      e.setProp("Text", HE::UIPropValue::ofString(""));
      HE::uiSetAnchorPreset(e, 2);                 // top-RIGHT corner
      e.pivotX = 1.0f; e.pivotY = 0.0f;
      e.posX = -10.0f; e.posY = 10.0f; e.sizeX = 80.0f; e.sizeY = 30.0f; }
    registerWidgetAs(cm, "mem://fixed.hasset", fixed);

    HE::UIWidgetTree page;
    page.canvasWidth = 400.0f; page.canvasHeight = 400.0f;
    page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int ref = page.add(HE::UIWidgetType::WidgetRef);
    { HE::UIElement& e = *page.find(ref);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 300.0f; e.sizeY = 200.0f;
      e.setProp("Widget", HE::UIPropValue::ofString("mem://fixed.hasset")); }
    registerWidget(cm, page);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);
    REQUIRE_FALSE(out.empty());
    // Its own size in pixels, hung on the slot's right edge: 300 - 10 - 80.
    bool found = false;
    for (const UIRenderObject& o : out)
        if (o.size.x == doctest::Approx(80.0f) && o.size.y == doctest::Approx(30.0f) &&
            o.position.x == doctest::Approx(210.0f) && o.position.y == doctest::Approx(10.0f))
            found = true;
    CHECK(found);

    // A wider slot pushes it further out and does NOT stretch it — the anchor
    // does the work, which is the whole point of the mode.
    page.find(ref)->sizeX = 380.0f;
    registerWidget(cm, page);
    WidgetManager wider;
    REQUIRE(createShown(wider, cm, "mem://w.hasset") != 0);
    out.clear();
    wider.extract(400.0f, 400.0f, out);
    found = false;
    for (const UIRenderObject& o : out)
        if (o.size.x == doctest::Approx(80.0f) && o.position.x == doctest::Approx(290.0f))
            found = true;
    CHECK(found);
}

TEST_CASE("WidgetRef: the slot is measured in canvas units, not in pixels")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    // The page itself is scaled (a bigger screen, or the designer zoomed in).
    // An embedded ConstantPixel widget keeps its units relative to the PAGE and
    // is then carried to the screen by the page's factor — it must not read
    // "one unit is one screen pixel" and come out 1/scale too big.
    HE::UIWidgetTree sub;
    sub.canvasWidth = 1000.0f; sub.canvasHeight = 1000.0f;
    sub.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int b = sub.add(HE::UIWidgetType::Button);
    { HE::UIElement& e = *sub.find(b);
      e.setProp("Text", HE::UIPropValue::ofString(""));
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 100.0f; e.sizeY = 50.0f; }
    registerWidgetAs(cm, "mem://sub.hasset", sub);

    HE::UIWidgetTree page;
    page.canvasWidth = 1000.0f; page.canvasHeight = 1000.0f;
    page.scaleMode = HE::UICanvasScaleMode::MatchWidth;    // a factor of 2 below
    const int ref = page.add(HE::UIWidgetType::WidgetRef);
    { HE::UIElement& e = *page.find(ref);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 500.0f; e.sizeY = 500.0f;
      e.setProp("Widget", HE::UIPropValue::ofString("mem://sub.hasset")); }
    registerWidget(cm, page);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    std::vector<UIRenderObject> out;
    wm.extract(2000.0f, 2000.0f, out);       // page scale = 2000/1000 = 2
    REQUIRE_FALSE(out.empty());
    // 100x50 of ITS units = 100x50 page units = 200x100 pixels.
    bool found = false;
    for (const UIRenderObject& o : out)
        if (o.size.x == doctest::Approx(200.0f) && o.size.y == doctest::Approx(100.0f))
            found = true;
    CHECK(found);

    // The same widget authored as Stretch is fitted into the slot instead:
    // 500/1000 → 50x25 page units → 100x50 pixels.
    sub.scaleMode = HE::UICanvasScaleMode::Stretch;
    registerWidgetAs(cm, "mem://sub.hasset", sub);
    registerWidget(cm, page);
    WidgetManager fitted;
    REQUIRE(createShown(fitted, cm, "mem://w.hasset") != 0);
    out.clear();
    fitted.extract(2000.0f, 2000.0f, out);
    found = false;
    for (const UIRenderObject& o : out)
        if (o.size.x == doctest::Approx(100.0f) && o.size.y == doctest::Approx(50.0f))
            found = true;
    CHECK(found);
}

TEST_CASE("WidgetRef: text inside an embedded widget scales with it")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    // The one part of an element that is not a rectangle: a font size is
    // authored in the widget's own units, so it has to be carried by the same
    // factor its rect is. A checkbox in a half-size slot whose label stayed
    // full size is exactly what this catches.
    HE::UIWidgetTree sub;
    sub.canvasWidth = 1000.0f; sub.canvasHeight = 1000.0f;
    const int txt = sub.add(HE::UIWidgetType::Text);
    { HE::UIElement& e = *sub.find(txt);
      e.setProp("Text", HE::UIPropValue::ofString("Ay"));
      e.setProp("FontSize", HE::UIPropValue::ofFloat(100.0f));
      e.setProp("AutoSize", HE::UIPropValue::ofBool(false));
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 400.0f; e.sizeY = 200.0f; }
    registerWidgetAs(cm, "mem://sub.hasset", sub);

    // Straight up: the same tree as a page of its own, one unit = one pixel.
    HE::UIWidgetTree direct = sub;
    direct.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    registerWidget(cm, direct);
    WidgetManager plain;
    REQUIRE(createShown(plain, cm, "mem://w.hasset") != 0);
    std::vector<UIRenderObject> a;
    plain.extract(1000.0f, 1000.0f, a);
    float glyphH = 0.0f;
    for (const UIRenderObject& o : a) if (o.type == 2) glyphH = std::max(glyphH, o.size.y);
    REQUIRE(glyphH > 0.0f);

    // Embedded into a slot half the size: every glyph must be half as tall.
    HE::UIWidgetTree page;
    page.canvasWidth = 1000.0f; page.canvasHeight = 1000.0f;
    page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int ref = page.add(HE::UIWidgetType::WidgetRef);
    { HE::UIElement& e = *page.find(ref);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 500.0f; e.sizeY = 500.0f;
      e.setProp("Widget", HE::UIPropValue::ofString("mem://sub.hasset")); }
    registerWidget(cm, page);
    WidgetManager embedded;
    REQUIRE(createShown(embedded, cm, "mem://w.hasset") != 0);
    std::vector<UIRenderObject> b;
    embedded.extract(1000.0f, 1000.0f, b);
    float embeddedGlyphH = 0.0f;
    for (const UIRenderObject& o : b)
        if (o.type == 2) embeddedGlyphH = std::max(embeddedGlyphH, o.size.y);
    REQUIRE(embeddedGlyphH > 0.0f);
    CHECK(embeddedGlyphH == doctest::Approx(glyphH * 0.5f).epsilon(0.02));
}

// ═══ Text field editing ══════════════════════════════════════════════════════
// The field could append and backspace, nothing else: no caret, no selection,
// no way to fix a typo in the middle of a name. All of it is byte offsets on
// character boundaries, so an accented letter moves as one thing.

namespace
{
    // A widget with one focused text field, ready to be typed into.
    struct TextFieldFixture
    {
        TempWidgetDir dir;
        ContentManager cm{ dir.path.string() };
        HE::UIWidgetTree authored;
        WidgetManager wm;
        int elem = 0, widget = 0;

        explicit TextFieldFixture(const std::string& initial = "",
                                  int maxLength = 0, bool editable = true,
                                  bool selectable = true)
        {
            authored.canvasWidth = 400.0f; authored.canvasHeight = 200.0f;
            authored.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
            elem = authored.add(HE::UIWidgetType::TextInput);
            auto* e = dynamic_cast<HE::UITextInput*>(authored.find(elem));
            e->text = initial;
            e->maxLength = maxLength;
            e->editable = editable;
            e->selectable = selectable;
            HE::uiSetAnchorPreset(*e, 0); e->pivotX = e->pivotY = 0.0f;
            e->posX = 0.0f; e->posY = 0.0f; e->sizeX = 400.0f; e->sizeY = 40.0f;
            registerWidget(cm, authored);
            widget = createShown(wm, cm, "mem://w.hasset");
            REQUIRE(widget != 0);
            // Click it to take the focus, at the far right so the caret lands
            // at the end of the text.
            REQUIRE(wm.processPointer(400.0f, 200.0f, 395.0f, 20.0f, true, true));
            wm.processPointer(400.0f, 200.0f, 395.0f, 20.0f, false, true);
            REQUIRE(wm.hasFocusedTextField());
        }
        const HE::UITextInput* field() const
        {
            const HE::UIWidgetTree* t = wm.tree(widget);
            return t ? dynamic_cast<const HE::UITextInput*>(t->find(elem)) : nullptr;
        }
        std::string text() const { const auto* f = field(); return f ? f->text : std::string(); }
        size_t caret() const     { const auto* f = field(); return f ? f->caret : 0; }
        float  scrollPx() const  { const auto* f = field(); return f ? f->scrollPx : 0.0f; }

        // The live field, to switch it to multiline or read its vertical scroll.
        HE::UITextInput* live()
        {
            auto* t = const_cast<HE::UIWidgetTree*>(wm.tree(widget));
            return t ? dynamic_cast<HE::UITextInput*>(t->find(elem)) : nullptr;
        }
    };
}

// ═══ B10: the reader's text size ═════════════════════════════════════════════
// One factor on every authored font size. The whole risk of the feature is that
// it reaches SOME of the places a font size is used: then the glyphs are one
// size and the arithmetic about them another, and the caret lands beside the
// character it was aimed at while everything still looks fine.

TEST_CASE("The text size is clamped where it stops being a text size")
{
    WidgetManager wm;
    CHECK(wm.fontScale() == doctest::Approx(1.0f));   // the default draws what was drawn
    wm.setFontScale(1.25f);
    CHECK(wm.fontScale() == doctest::Approx(1.25f));
    // 0 and negative are not "smaller", they are text that has stopped existing.
    wm.setFontScale(0.0f);
    CHECK(wm.fontScale() == doctest::Approx(1.0f));
    wm.setFontScale(-3.0f);
    CHECK(wm.fontScale() == doctest::Approx(1.0f));
    wm.setFontScale(0.1f);
    CHECK(wm.fontScale() == doctest::Approx(0.5f));
    wm.setFontScale(99.0f);
    CHECK(wm.fontScale() == doctest::Approx(3.0f));
}

TEST_CASE("Auto-size fits the text as DRAWN, not as authored")
{
    // The first of the two quiet breakers: a box that fits at 100 % cuts the
    // label in half at 150 %, and nothing about the picture says why.
    HE::UIText t;
    t.text = "Hello";
    t.autoSize = true;
    t.fontSize = 20.0f;
    t.applyAutoSize(t.sizeX);
    const float w1 = t.sizeX, h1 = t.sizeY;

    t.applyAutoSize(t.sizeX, 1.5f);
    CHECK(t.sizeX > w1);
    CHECK(t.sizeY > h1);
    // …and it is exactly the box a 30 px label gets, because 20 at 150 % IS 30.
    HE::UIText ref;
    ref.text = "Hello";
    ref.autoSize = true;
    ref.fontSize = 30.0f;
    ref.applyAutoSize(ref.sizeX);
    CHECK(t.sizeX == doctest::Approx(ref.sizeX));
    CHECK(t.sizeY == doctest::Approx(ref.sizeY));
}

TEST_CASE("The caret is measured at the size the glyphs were drawn at")
{
    // The second quiet breaker. Wider glyphs mean fewer of them fit under the
    // same click, so the answer has to MOVE — and it has to move to exactly the
    // answer a field with that font size would have given.
    HE::UITextInput ti;
    ti.text     = "abcdefghij";
    ti.fontSize = 20.0f;
    const size_t at100 = ti.caretAtPoint(60.0f, 0.0f, 1.0f);
    const size_t at200 = ti.caretAtPoint(60.0f, 0.0f, 1.0f, 2.0f);
    REQUIRE(at100 > 0);
    CHECK(at200 < at100);

    HE::UITextInput big;
    big.text     = "abcdefghij";
    big.fontSize = 40.0f;
    CHECK(at200 == big.caretAtPoint(60.0f, 0.0f, 1.0f));
}

TEST_CASE("The reader's text size reaches the picture AND the pointer")
{
    // The end-to-end one: through the manager, which is the only place that
    // knows the setting. If the two halves ever disagree, this is what says so.
    auto firstGlyphHeight = [](const std::vector<UIRenderObject>& v)
    {
        for (const UIRenderObject& o : v) if (o.type == 2) return o.size.y;
        return 0.0f;
    };

    TextFieldFixture f("abcdefghij");
    std::vector<UIRenderObject> out;
    f.wm.extract(400.0f, 200.0f, out);
    const float h100 = firstGlyphHeight(out);
    REQUIRE(h100 > 0.0f);
    REQUIRE(f.wm.setCaretFromPointer(400.0f, 200.0f, 60.0f, 20.0f));
    const size_t caret100 = f.caret();
    REQUIRE(caret100 > 0);

    f.wm.setFontScale(1.5f);
    out.clear();
    f.wm.extract(400.0f, 200.0f, out);
    // The picture.
    CHECK(firstGlyphHeight(out) > h100);
    // …and the pointer, in the same breath.
    REQUIRE(f.wm.setCaretFromPointer(400.0f, 200.0f, 60.0f, 20.0f));
    CHECK(f.caret() < caret100);
    // Not merely "different": the same offset the field itself works out at
    // that size, 6 being the field's own padding.
    CHECK(f.caret() == f.live()->caretAtPoint(60.0f - 6.0f, 20.0f - 6.0f, 1.0f, 1.5f));
}

TEST_CASE("A tab is hit where it is drawn, on a canvas that scales")
{
    // The tab strip asked its hit test with the element's units and drew with
    // canvas pixels, so on any canvas other than 1:1 a click landed on the tab
    // NEXT to the one under the pointer. Found while threading the text size
    // through the same three numbers.
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 200.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int tabs = t.add(HE::UIWidgetType::TabBox);
    auto* tb = dynamic_cast<HE::UITabBox*>(t.find(tabs));
    HE::uiSetAnchorPreset(*tb, 0); tb->pivotX = tb->pivotY = 0.0f;
    tb->posX = 0.0f; tb->posY = 0.0f; tb->sizeX = 380.0f; tb->sizeY = 180.0f;
    for (int i = 0; i < 3; ++i)
    {
        const int page = t.add(HE::UIWidgetType::Panel);
        t.find(page)->parentId = tabs;
        t.find(page)->name = i == 0 ? "One" : (i == 1 ? "Two" : "Three");
    }
    registerWidget(cm, t);

    WidgetManager wm;
    wm.setDisplayScale(2.0f);          // one unit is two pixels
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    std::vector<UIRenderObject> out;
    wm.extract(800.0f, 400.0f, out);

    // Where the SECOND tab was drawn, asked of the same layout the draw used —
    // of the LIVE tree, because that is the one on screen.
    const HE::UIWidgetTree& lt = *wm.tree(id);
    const auto* ltb = dynamic_cast<const HE::UITabBox*>(lt.find(tabs));
    REQUIRE(ltb != nullptr);
    const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(lt, 800.0f, 400.0f, 2.0f);
    const HE::UIWidgetRect r = HE::uiElementRect(lt, *ltb, &canvas);
    const HE::UIWidgetRect pxr{ r.x * canvas.scaleX, r.y * canvas.scaleY,
                                r.w * canvas.scaleX, r.h * canvas.scaleY };
    std::vector<float> tx, tw;
    HE::UITabBox::tabLayout(pxr, ltb->fontSize * canvas.scaleY,
                            ltb->tabPadding * canvas.scaleY,
                            { "One", "Two", "Three" }, ltb->fontAtlasKey, tx, tw);
    REQUIRE(tx.size() == 3);
    const float midX = tx[1] + tw[1] * 0.5f;
    const float midY = pxr.y + ltb->tabHeight * canvas.scaleY * 0.5f;

    REQUIRE(wm.processPointer(800.0f, 400.0f, midX, midY, true, true));
    wm.processPointer(800.0f, 400.0f, midX, midY, false, true);
    const auto* live = dynamic_cast<const HE::UITabBox*>(wm.tree(id)->find(tabs));
    REQUIRE(live != nullptr);
    CHECK(live->activeTab == 1);
}

// ═══ B1b: more than one line ═════════════════════════════════════════════════
// A field that holds newlines. Everything below is about the two things that
// makes hard: a caret is a byte offset and has to survive being talked about in
// lines and columns, and a click has to land on the line it was aimed at.

TEST_CASE("Text lines: the ranges partition the string, and keep the last empty one")
{
    // Every byte belongs to exactly one line, and the ranges never include the
    // '\n' — that is what lets a caret address any position in the text.
    {
        const auto ls = HE::uiTextLineRanges("ab\ncd");
        REQUIRE(ls.size() == 2);
        CHECK(ls[0].begin == 0); CHECK(ls[0].end == 2);
        CHECK(ls[1].begin == 3); CHECK(ls[1].end == 5);
    }
    // The divergence from the label splitter, and the reason this function
    // exists: pressing Enter at the end must leave somewhere to put the caret.
    {
        const auto ls = HE::uiTextLineRanges("ab\n");
        REQUIRE(ls.size() == 2);
        CHECK(ls[1].begin == 3); CHECK(ls[1].end == 3);   // the empty last line
        // …and layoutUITextLines still drops it, on purpose — a label ending in
        // a stray newline must not be drawn half a line too high.
        CHECK(HE::layoutUITextLines(HE::sharedUIFont(), "ab\n", 16.0f, 0.0f, false).size() == 1);
    }
    // An empty string is one empty line, the way an empty field still has a
    // place for the caret.
    {
        const auto ls = HE::uiTextLineRanges("");
        REQUIRE(ls.size() == 1);
        CHECK(ls[0].begin == 0); CHECK(ls[0].end == 0);
    }
    // A blank line in the middle survives as a line of its own.
    {
        const auto ls = HE::uiTextLineRanges("a\n\nb");
        REQUIRE(ls.size() == 3);
        CHECK(ls[1].begin == ls[1].end);
    }
    // CRLF: the '\r' is not part of the line's TEXT but the next line still
    // starts after the '\n', so the ranges stay a partition.
    {
        const auto ls = HE::uiTextLineRanges("ab\r\ncd");
        REQUIRE(ls.size() == 2);
        CHECK(ls[0].end == 2);      // without the '\r'
        CHECK(ls[1].begin == 4);
    }
    // A caret at the END of a line belongs to that line, not to the start of
    // the next — the position it sits in most often.
    {
        const auto ls = HE::uiTextLineRanges("ab\ncd");
        CHECK(HE::uiLineOfOffset(ls, 0) == 0);
        CHECK(HE::uiLineOfOffset(ls, 2) == 0);
        CHECK(HE::uiLineOfOffset(ls, 3) == 1);
        CHECK(HE::uiLineOfOffset(ls, 99) == 1);   // past the end still lands
    }
}

TEST_CASE("Multiline: Enter inserts a line instead of committing")
{
    TextFieldFixture f("ab");
    // Single-line first: Enter commits and changes nothing about the text.
    f.wm.inputSubmit();
    CHECK(f.text() == "ab");

    f.live()->multiline = true;
    f.live()->caret = f.live()->selAnchor = 1;
    f.wm.inputSubmit();
    CHECK(f.text() == "a\nb");
    CHECK(f.caret() == 2);

    // The filter still applies. A field that only takes digits must not gain a
    // newline through the one key that skipped the check.
    f.live()->text = "12"; f.live()->caret = f.live()->selAnchor = 1;
    f.live()->inputFilter = HE::UITextInput::FilterInteger;
    f.wm.inputSubmit();
    CHECK(f.text() == "12");
}

TEST_CASE("Multiline: the caret walks up and down, and remembers its column")
{
    TextFieldFixture f("long line here\nx\nanother long one");
    f.live()->multiline = true;
    // Column 9 on the first line.
    f.live()->caret = f.live()->selAnchor = 9;

    f.wm.editFocusedText(WidgetManager::TextEdit::Down, false);
    // The middle line is one character long, so the caret lands at its end.
    CHECK(f.caret() == 16);

    // …and down again returns to column 9, not to column 1. Without a
    // remembered goal, three presses of Down would walk the caret to the left
    // edge and stay there.
    f.wm.editFocusedText(WidgetManager::TextEdit::Down, false);
    CHECK(f.caret() == 17 + 9);

    // Any other move forgets the goal.
    f.wm.editFocusedText(WidgetManager::TextEdit::Home, false);
    CHECK(f.caret() == 17);
    f.wm.editFocusedText(WidgetManager::TextEdit::Up, false);
    CHECK(f.caret() == 15);        // column 0 of the middle line
}

TEST_CASE("Multiline: Home and End are per line, and single-line is unchanged")
{
    TextFieldFixture f("abc\ndefgh");
    f.live()->multiline = true;
    f.live()->caret = f.live()->selAnchor = 6;      // inside "defgh"
    f.wm.editFocusedText(WidgetManager::TextEdit::Home, false);
    CHECK(f.caret() == 4);
    f.wm.editFocusedText(WidgetManager::TextEdit::End, false);
    CHECK(f.caret() == 9);

    // The very same operations on a single-line field still mean the whole
    // field, because one line IS the whole field.
    TextFieldFixture g("abcdef");
    g.wm.editFocusedText(WidgetManager::TextEdit::Home, false);
    CHECK(g.caret() == 0);
    g.wm.editFocusedText(WidgetManager::TextEdit::End, false);
    CHECK(g.caret() == 6);
    // And Up/Down do nothing at all there.
    CHECK_FALSE(g.wm.editFocusedText(WidgetManager::TextEdit::Up, false));
    CHECK(g.caret() == 6);
}

TEST_CASE("Multiline: a click lands on the line it was aimed at")
{
    TextFieldFixture f("first\nsecond\nthird");
    f.live()->multiline = true;
    f.live()->sizeY = 120.0f;     // room for three lines
    f.live()->caret = f.live()->selAnchor = 0;

    // Draw once so the field measures itself (render is what works out the
    // line step and the scroll).
    std::vector<UIRenderObject> out;
    f.wm.extract(400.0f, 200.0f, out);

    const float step = f.live()->fontSize * 1.15f;
    // Click near the left edge of the third line. 6 is the field's own padding.
    REQUIRE(f.wm.setCaretFromPointer(400.0f, 200.0f, 8.0f, 6.0f + step * 2.0f + step * 0.5f));
    const auto ls = HE::uiTextLineRanges(f.text());
    CHECK(HE::uiLineOfOffset(ls, f.caret()) == 2);

    // …and the first line, to prove the arithmetic is not simply "the last one".
    REQUIRE(f.wm.setCaretFromPointer(400.0f, 200.0f, 8.0f, 6.0f + step * 0.5f));
    CHECK(HE::uiLineOfOffset(ls, f.caret()) == 0);
}

TEST_CASE("Multiline: it draws every line, and scrolls to keep the caret in view")
{
    TextFieldFixture f("one\ntwo\nthree\nfour\nfive\nsix\nseven\neight");
    f.live()->multiline = true;
    f.live()->sizeY = 60.0f;      // shows two or three lines of eight
    // The fixture focuses by clicking at the far right, which leaves the caret
    // at the END — and a multiline field then rightly scrolls down to it. Start
    // from the top, which is what this case is about.
    f.live()->caret = f.live()->selAnchor = 0;

    std::vector<UIRenderObject> out;
    f.wm.extract(400.0f, 200.0f, out);
    const int shownAtTop = countGlyphs(out);
    // Not all of it: a field this short cannot be showing thirty-odd characters.
    CHECK(shownAtTop > 0);
    CHECK(shownAtTop < 30);
    CHECK(f.live()->scrollPxY == doctest::Approx(0.0f));

    // Put the caret on the last line and draw again — the field has to scroll
    // down to it rather than leaving it below the edge.
    f.live()->caret = f.live()->selAnchor = f.text().size();
    out.clear();
    f.wm.extract(400.0f, 200.0f, out);
    CHECK(f.live()->scrollPxY > 0.0f);

    // A multiline field is a scrolling container as far as the wheel is
    // concerned — that is what makes the wheel and the preview's state capture
    // work with no wiring of their own.
    CHECK(f.live()->scrollOffsetPtr() != nullptr);
    CHECK(f.live()->maxScrollAmount() > 0.0f);
    // …and a single-line one is not.
    TextFieldFixture g("plain");
    CHECK(g.live()->scrollOffsetPtr() == nullptr);
}

TEST_CASE("Multiline: a selection across lines is drawn as one run per line")
{
    TextFieldFixture f("aaa\nbbb\nccc");
    f.live()->multiline = true;
    f.live()->sizeY = 120.0f;
    f.live()->selAnchor = 1;                  // inside line 0
    f.live()->caret     = 9;                  // inside line 2

    std::vector<UIRenderObject> out;
    f.wm.extract(400.0f, 200.0f, out);

    // Three highlight quads, one per line the selection touches: a partial
    // first, a whole middle, a partial last. One quad spanning the block would
    // paint over the margins of the lines it does not actually cover.
    const glm::vec4 sel = f.live()->selectionColor;
    int highlights = 0;
    for (const auto& ro : out)
        if (ro.type == 0 && ro.color == sel) ++highlights;
    CHECK(highlights == 3);
}

TEST_CASE("Multiline: the caret is reachable on the empty line a trailing Enter makes")
{
    TextFieldFixture f("ab");
    f.live()->multiline = true;
    f.live()->caret = f.live()->selAnchor = 2;
    f.wm.inputSubmit();                        // Enter at the very end
    REQUIRE(f.text() == "ab\n");
    CHECK(f.caret() == 3);
    // The line the caret sits on has to exist. The label splitter drops exactly
    // this line, which is why the editor has its own.
    const auto ls = HE::uiTextLineRanges(f.text());
    REQUIRE(ls.size() == 2);
    CHECK(HE::uiLineOfOffset(ls, f.caret()) == 1);
    // …and typing there lands on the new line rather than on the old one.
    f.wm.inputText("c");
    CHECK(f.text() == "ab\nc");
}

// ═══ B1b, the rest: breaking a line the author did not break ═════════════════
// A wrapped row is not an authored line, and the whole difficulty is that a
// caret cannot tell the two apart by itself. Every case below is about a byte
// staying reachable across a break the author never typed.

TEST_CASE("Wrapped rows: the break eats the spaces and leaves no byte between rows")
{
    const HE::BakedUIFont& font = HE::sharedUIFont();
    REQUIRE(font.ok);
    HE::UITextLayout opts;
    const float sizePx = 16.0f;
    const std::string text = "alpha beta gamma";
    // A width that takes two of the three words. Measured with the same
    // function the splitter measures with, so this cannot drift with the font.
    const float w2 = HE::measureUIText(font, "alpha beta", sizePx, 0.0f, opts).x;
    const float w3 = HE::measureUIText(font, text, sizePx, 0.0f, opts).x;
    REQUIRE(w3 > w2);
    const float wrapAt = (w2 + w3) * 0.5f;

    const auto rows = HE::uiTextWrapRanges(font, text, sizePx, wrapAt);
    REQUIRE(rows.size() == 2);
    // The row stops IN FRONT of the space the break ate, and the next one
    // starts behind it. That is the whole contract: `end` is what gets drawn,
    // `next` is where the row below begins, and the bytes in between are still
    // addressable rather than lost at a seam.
    CHECK(rows[0].begin == 0);
    CHECK(rows[0].end   == 10);              // "alpha beta"
    CHECK(rows[0].next  == 11);              // past the one space
    CHECK_FALSE(rows[0].hard);               // nobody typed this break
    CHECK(rows[1].begin == 11);
    CHECK(rows[1].end   == text.size());
    CHECK(rows[1].hard);                     // …but the end of the text is hard

    // Said as a rule rather than as three numbers: every offset in the text,
    // the one past the end included, lands on a row that actually contains it.
    for (std::size_t b = 0; b <= text.size(); ++b)
    {
        const std::size_t r = HE::uiVisualLineOfOffset(rows, b);
        CHECK(b >= rows[r].begin);
        CHECK((b <= rows[r].end || b < rows[r].next));
    }
    // The row a caret at the break belongs to is the one it was typed on…
    CHECK(HE::uiVisualLineOfOffset(rows, 10) == 0);
    // …and the row below starts one byte later.
    CHECK(HE::uiVisualLineOfOffset(rows, 11) == 1);
}

TEST_CASE("Wrapped rows: a word wider than the field breaks INSIDE, by characters")
{
    const HE::BakedUIFont& font = HE::sharedUIFont();
    REQUIRE(font.ok);
    HE::UITextLayout opts;
    const float sizePx = 16.0f;
    const std::string text = "unbrechbaresmonsterwort";
    const float wrapAt = HE::measureUIText(font, "unbre", sizePx, 0.0f, opts).x;

    const auto rows = HE::uiTextWrapRanges(font, text, sizePx, wrapAt);
    REQUIRE(rows.size() > 2);
    // Nothing is dropped and nothing is duplicated: the rows follow one another
    // without a gap. A word broken by bytes rather than by rows is the way this
    // goes wrong, and it goes wrong silently.
    CHECK(rows.front().begin == 0);
    CHECK(rows.back().end == text.size());
    for (std::size_t i = 0; i + 1 < rows.size(); ++i)
    {
        CHECK(rows[i].end > rows[i].begin);      // never an empty row
        CHECK(rows[i].next == rows[i + 1].begin);
    }

    // A two-byte character is never split down the middle — neither half is
    // anything, and the atlas draws a box for both.
    const std::string umlauts = "ääääääääääääääää";
    const auto urows = HE::uiTextWrapRanges(font, umlauts, sizePx,
                                            HE::measureUIText(font, "ää", sizePx, 0.0f, opts).x);
    REQUIRE(urows.size() > 1);
    for (const auto& r : urows)
    {
        CHECK(r.begin % 2 == 0);
        CHECK(r.end % 2 == 0);
    }
}

TEST_CASE("Wrapped rows: an authored break stays an authored break")
{
    const HE::BakedUIFont& font = HE::sharedUIFont();
    REQUIRE(font.ok);
    // Wide enough that nothing wraps: what comes back has to be exactly the
    // authored lines, or turning Wrap Text on would move text in a field that
    // is not too narrow for anything.
    const auto rows = HE::uiTextWrapRanges(font, "ab\ncd", 16.0f, 10000.0f);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].end == 2); CHECK(rows[0].next == 3); CHECK(rows[0].hard);
    CHECK(rows[1].begin == 3); CHECK(rows[1].end == 5);

    // CRLF: two bytes sit between the rows, and `next` knows it.
    const auto crlf = HE::uiTextWrapRanges(font, "ab\r\ncd", 16.0f, 10000.0f);
    REQUIRE(crlf.size() == 2);
    CHECK(crlf[0].end == 2);          // without the '\r'
    CHECK(crlf[0].next == 4);
    CHECK(crlf[1].begin == 4);

    // An unusable width answers like the plain splitter rather than by
    // wrapping everything into nothing.
    CHECK(HE::uiTextWrapRanges(font, "ab\ncd", 16.0f, 0.0f).size() == 2);
    // …and the trailing empty line survives here too, for the same reason it
    // does in uiTextLineRanges: Enter at the end needs somewhere to stand.
    REQUIRE(HE::uiTextVisualLines("ab\n").size() == 2);
    CHECK(HE::uiTextVisualLines("ab\n")[1].begin == 3);
}

TEST_CASE("Wrapping field: Down walks the ROW below, inside one paragraph")
{
    TextFieldFixture f("");
    f.live()->multiline = true;
    f.live()->sizeY = 120.0f;
    // One paragraph and not a single newline in it: without wrapping this is
    // one line, and there is no row for Down to go to.
    f.live()->text = "alpha beta gamma delta epsilon zeta eta theta iota kappa"
                     " lambda mu nu xi omicron pi rho sigma tau upsilon";
    f.live()->caret = f.live()->selAnchor = 0;

    std::vector<UIRenderObject> out;
    f.wm.extract(400.0f, 200.0f, out);
    // Wrap Text off: one row, and the text runs off the right edge as before.
    CHECK(f.live()->visualLines().size() == 1);

    f.live()->wrapText = true;
    out.clear();
    f.wm.extract(400.0f, 200.0f, out);      // …this is where the width is learnt
    const std::size_t rowCount = f.live()->visualLines().size();
    REQUIRE(rowCount > 1);

    // Down moves, and it moves WITHIN the paragraph — the text is untouched.
    REQUIRE(f.wm.editFocusedText(WidgetManager::TextEdit::Down, false));
    CHECK(f.caret() > 0);
    CHECK(f.text().find('\n') == std::string::npos);
    CHECK(HE::uiVisualLineOfOffset(f.live()->visualLines(), f.caret()) == 1);
    // …and back up again lands on the row it started from.
    REQUIRE(f.wm.editFocusedText(WidgetManager::TextEdit::Up, false));
    CHECK(HE::uiVisualLineOfOffset(f.live()->visualLines(), f.caret()) == 0);

    // End is the ROW's edge, not the paragraph's. If it overshot onto the row
    // below, End-then-Down would skip a row — which is how this goes wrong
    // without anybody noticing the End itself was wrong.
    f.live()->caret = f.live()->selAnchor = 0;
    REQUIRE(f.wm.editFocusedText(WidgetManager::TextEdit::End, false));
    const auto rows = f.live()->visualLines();
    CHECK(f.caret() == rows[0].end);
    CHECK(f.caret() < f.text().size());
    CHECK(HE::uiVisualLineOfOffset(rows, f.caret()) == 0);
    // Home comes back to the row's start, which for row zero is the text's.
    REQUIRE(f.wm.editFocusedText(WidgetManager::TextEdit::Home, false));
    CHECK(f.caret() == 0);
}

TEST_CASE("Wrapping field: a click lands on the row it was aimed at")
{
    TextFieldFixture f("");
    f.live()->multiline = true;
    f.live()->wrapText  = true;
    f.live()->sizeY = 120.0f;
    f.live()->text = "alpha beta gamma delta epsilon zeta eta theta iota kappa"
                     " lambda mu nu xi omicron pi rho sigma tau upsilon";
    f.live()->caret = f.live()->selAnchor = 0;

    std::vector<UIRenderObject> out;
    f.wm.extract(400.0f, 200.0f, out);
    REQUIRE(f.live()->visualLines().size() > 1);

    const float step = f.live()->fontSize * 1.15f;
    // The middle of the second row, near its left edge. 6 is the field's own
    // padding, the same number render() draws with.
    REQUIRE(f.wm.setCaretFromPointer(400.0f, 200.0f, 8.0f, 6.0f + step * 1.5f));
    CHECK(HE::uiVisualLineOfOffset(f.live()->visualLines(), f.caret()) == 1);
    // …and the first row, so this is not simply "wherever the last one was".
    REQUIRE(f.wm.setCaretFromPointer(400.0f, 200.0f, 8.0f, 6.0f + step * 0.5f));
    CHECK(HE::uiVisualLineOfOffset(f.live()->visualLines(), f.caret()) == 0);
}

TEST_CASE("Wrapping field: a password field and a field never drawn keep the old answer")
{
    // Before the first draw nothing knows the field's pixel width, and a guess
    // would be a layout that changes the moment it is actually shown.
    TextFieldFixture f("alpha beta gamma delta epsilon zeta eta theta iota kappa"
                       " lambda mu nu xi omicron pi rho sigma tau upsilon");
    f.live()->multiline = true;
    f.live()->wrapText  = true;
    CHECK(f.live()->visualLines().size() == 1);

    // A password is drawn as dots and addressed in bytes; wrapping measured on
    // the one and cut in the other would be two answers to where row two is.
    std::vector<UIRenderObject> out;
    f.wm.extract(400.0f, 200.0f, out);
    REQUIRE(f.live()->visualLines().size() > 1);
    f.live()->password = true;
    CHECK(f.live()->visualLines().size() == 1);
}

TEST_CASE("uiUtf8: cursor movement never lands inside a character")
{
    const std::string s = "aä€b";       // 1 + 2 + 3 + 1 bytes
    CHECK(HE::uiUtf8Next(s, 0) == 1);
    CHECK(HE::uiUtf8Next(s, 1) == 3);   // over the 2-byte ä
    CHECK(HE::uiUtf8Next(s, 3) == 6);   // over the 3-byte €
    CHECK(HE::uiUtf8Next(s, 6) == 7);
    CHECK(HE::uiUtf8Next(s, 7) == 7);   // at the end it stays
    CHECK(HE::uiUtf8Prev(s, 7) == 6);
    CHECK(HE::uiUtf8Prev(s, 6) == 3);
    CHECK(HE::uiUtf8Prev(s, 3) == 1);
    CHECK(HE::uiUtf8Prev(s, 0) == 0);
    // A byte offset in the middle of a character snaps back to its start.
    CHECK(HE::uiUtf8Clamp(s, 2) == 1);
    CHECK(HE::uiUtf8Clamp(s, 4) == 3);
    CHECK(HE::uiUtf8Clamp(s, 99) == s.size());
}

TEST_CASE("UITextInput: selection, deletion and the character count")
{
    HE::UITextInput ti;
    ti.text = "hello";
    ti.caret = 1; ti.selAnchor = 4;
    CHECK(ti.hasSelection());
    CHECK(ti.selMin() == 1);
    CHECK(ti.selMax() == 4);
    CHECK(ti.selectedText() == "ell");
    CHECK(ti.deleteSelection());
    CHECK(ti.text == "ho");
    CHECK(ti.caret == 1);
    CHECK_FALSE(ti.hasSelection());
    CHECK_FALSE(ti.deleteSelection());

    // maxLength counts characters, not bytes.
    ti.text = "aä€";
    CHECK(ti.charCount() == 3);
    CHECK(ti.text.size() == 6);

    // Offsets left pointing anywhere by a script are pulled back onto
    // boundaries rather than splitting a character.
    ti.caret = 2; ti.selAnchor = 99;
    ti.clampCaret();
    CHECK(ti.caret == 1);
    CHECK(ti.selAnchor == 6);
}

TEST_CASE("Text field: typing lands at the caret, not at the end")
{
    TextFieldFixture f("hello");
    using TE = WidgetManager::TextEdit;
    REQUIRE(f.text() == "hello");
    CHECK(f.caret() == 5);                       // clicked past the end

    // Move into the middle and type there.
    CHECK(f.wm.editFocusedText(TE::Home, false));
    CHECK(f.caret() == 0);
    CHECK(f.wm.editFocusedText(TE::Right, false));
    f.wm.inputText("X");
    CHECK(f.text() == "hXello");
    CHECK(f.caret() == 2);

    // Backspace takes the character BEFORE the caret, not the last one.
    f.wm.inputBackspace();
    CHECK(f.text() == "hello");
    // …and Delete the one after it.
    CHECK(f.wm.editFocusedText(TE::Delete, false));
    CHECK(f.text() == "hllo");
    CHECK(f.caret() == 1);

    // End, and the edges do nothing but say so.
    CHECK(f.wm.editFocusedText(TE::End, false));
    CHECK(f.caret() == 4);
    CHECK_FALSE(f.wm.editFocusedText(TE::Right, false));
    CHECK_FALSE(f.wm.editFocusedText(TE::Delete, false));
}

TEST_CASE("Text field: selecting, replacing, and the clipboard's half of it")
{
    TextFieldFixture f("hello world");
    using TE = WidgetManager::TextEdit;

    CHECK(f.wm.editFocusedText(TE::SelectAll, false));
    CHECK(f.wm.focusedSelection() == "hello world");
    // Typing over a selection replaces it.
    f.wm.inputText("bye");
    CHECK(f.text() == "bye");
    CHECK(f.wm.focusedSelection().empty());

    // Shift-arrows extend from the caret; a plain arrow collapses to the near
    // end instead of moving off it.
    CHECK(f.wm.editFocusedText(TE::Home, false));
    CHECK(f.wm.editFocusedText(TE::Right, true));
    CHECK(f.wm.editFocusedText(TE::Right, true));
    CHECK(f.wm.focusedSelection() == "by");
    CHECK(f.wm.editFocusedText(TE::Right, false));
    CHECK(f.wm.focusedSelection().empty());
    CHECK(f.caret() == 2);

    // The cut half: hand out the selection, then drop it.
    CHECK(f.wm.editFocusedText(TE::SelectAll, false));
    CHECK(f.wm.focusedSelection() == "bye");
    CHECK(f.wm.deleteFocusedSelection());
    CHECK(f.text().empty());
    CHECK_FALSE(f.wm.deleteFocusedSelection());
}

// ─── Visual invalidation ─────────────────────────────────────────────────────
// What an event-driven application sleeps on. The interesting half is the
// NEGATIVE case: if this flag were raised unconditionally the app would redraw
// every frame and the whole mechanism would be decoration.

TEST_CASE("Widgets report what changed the picture, and only that")
{
    TextFieldFixture f("hello");
    using TE = WidgetManager::TextEdit;

    // Creation and the first frame are a change by definition.
    CHECK(f.wm.consumeVisualDirty());
    CHECK_FALSE(f.wm.consumeVisualDirty());   // consuming clears

    // Typing changes the glyphs.
    f.wm.inputText("!");
    CHECK(f.wm.consumeVisualDirty());

    // So does moving the caret — the bar is drawn.
    CHECK(f.wm.editFocusedText(TE::Home, false));
    CHECK(f.wm.consumeVisualDirty());

    // Pointer motion that changes NO drawn state must not ask for a frame. Two
    // moves inside the same element: the first settles the hover, the second
    // finds everything already where it was.
    f.wm.processPointer(400.0f, 200.0f, 100.0f, 20.0f, false, true);
    f.wm.consumeVisualDirty();
    f.wm.processPointer(400.0f, 200.0f, 104.0f, 20.0f, false, true);
    CHECK_FALSE(f.wm.consumeVisualDirty());

    // Hiding the widget obviously changes it.
    f.wm.hideWidget(f.widget);
    CHECK(f.wm.consumeVisualDirty());

    // A wheel notch that no scroll box takes is not a change either.
    f.wm.processWheel(400.0f, 200.0f, 100.0f, 20.0f, 0.0f);
    CHECK_FALSE(f.wm.consumeVisualDirty());
}

TEST_CASE("Text field: the input filter judges each character in context")
{
    SUBCASE("whole numbers")
    {
        TextFieldFixture f;
        auto* ti = const_cast<HE::UITextInput*>(f.field());
        REQUIRE(ti);
        ti->inputFilter = HE::UITextInput::FilterInteger;

        f.wm.inputText("12a3");            // letters simply do not arrive
        CHECK(f.text() == "123");

        // A minus is a sign, not a character: only in front, and only one.
        CHECK(f.wm.editFocusedText(WidgetManager::TextEdit::Home, false));
        f.wm.inputText("-");
        CHECK(f.text() == "-123");
        f.wm.inputText("-");
        CHECK(f.text() == "-123");         // a second one is refused
        CHECK(f.wm.editFocusedText(WidgetManager::TextEdit::End, false));
        f.wm.inputText("-");
        CHECK(f.text() == "-123");         // and never in the middle or the end

        f.wm.inputText(".5");              // no decimal point in a whole number
        CHECK(f.text() == "-1235");
    }

    SUBCASE("decimals take exactly one point")
    {
        TextFieldFixture f;
        auto* ti = const_cast<HE::UITextInput*>(f.field());
        REQUIRE(ti);
        ti->inputFilter = HE::UITextInput::FilterDecimal;

        f.wm.inputText("3.14");
        CHECK(f.text() == "3.14");
        f.wm.inputText(".");
        CHECK(f.text() == "3.14");         // the second point is refused
    }

    SUBCASE("a paste keeps what fits instead of being refused whole")
    {
        TextFieldFixture f;
        auto* ti = const_cast<HE::UITextInput*>(f.field());
        REQUIRE(ti);
        ti->inputFilter = HE::UITextInput::FilterInteger;
        // One call, the way a paste arrives — and the judgement is made per
        // character against the text as it grows, so the second '-' is refused
        // even though it is in the same paste as the first.
        f.wm.inputText("-12-34");
        CHECK(f.text() == "-1234");
    }

    SUBCASE("a custom list, and an empty one means no rule")
    {
        TextFieldFixture f;
        auto* ti = const_cast<HE::UITextInput*>(f.field());
        REQUIRE(ti);
        ti->inputFilter  = HE::UITextInput::FilterCustom;
        ti->allowedChars = "ABCDEF0123456789";
        f.wm.inputText("CAFEbabe99");
        CHECK(f.text() == "CAFE99");

        ti->allowedChars.clear();          // no list = nothing to enforce
        f.wm.inputText("xyz");
        CHECK(f.text() == "CAFE99xyz");
    }
}

TEST_CASE("Text field: an input method's unfinished text is held apart from the value")
{
    // The fixture clicks past the right edge, so the caret already sits at the
    // end of this short text — no need to move it there.
    TextFieldFixture f("ab");
    REQUIRE(f.caret() == 2);

    // While composing, the preedit run is NOT part of the field's value — it is
    // the input method's, and only it decides when it becomes text.
    f.wm.inputComposition("nihao", 5);
    CHECK(f.wm.hasComposition());
    CHECK(f.text() == "ab");

    // It IS drawn, though, or the user types into a field that shows nothing:
    // the composing frame emits more glyphs than the quiet one.
    std::vector<UIRenderObject> quiet, composing;
    f.wm.extract(400.0f, 200.0f, composing);
    f.wm.inputComposition("", -1);
    CHECK_FALSE(f.wm.hasComposition());
    f.wm.extract(400.0f, 200.0f, quiet);
    CHECK(countGlyphs(composing) > countGlyphs(quiet));

    // Committing: the OS sends the finished characters as ordinary input, and
    // that has to end the composition or they would be drawn twice.
    f.wm.inputComposition("nihao", 5);
    REQUIRE(f.wm.hasComposition());
    f.wm.inputText("\xE4\xBD\xA0\xE5\xA5\xBD");   // 你好
    CHECK_FALSE(f.wm.hasComposition());
    CHECK(f.text() == "ab\xE4\xBD\xA0\xE5\xA5\xBD");

    // A read-only field refuses a composition too — preedit text that could
    // never land is worse than none.
    TextFieldFixture ro("locked", 0, /*editable=*/false);
    ro.wm.inputComposition("nihao", 5);
    CHECK_FALSE(ro.wm.hasComposition());
}

// ─── Undo and redo, per field ────────────────────────────────────────────────
// The last thing on the B1 list. Everything else about it is arithmetic on two
// stacks; what has to be pinned down is where one step ENDS, because a history
// that takes typing back one letter at a time is not one anybody would use.

TEST_CASE("Text field: a run of typing is one undo step, and a caret move ends it")
{
    TextFieldFixture f;
    using TE = WidgetManager::TextEdit;

    f.wm.inputText("a"); f.wm.inputText("b"); f.wm.inputText("c");
    REQUIRE(f.text() == "abc");
    // One step, not three.
    CHECK(f.wm.undoFocusedText());
    CHECK(f.text().empty());
    CHECK_FALSE(f.wm.undoFocusedText());

    // Redo puts the whole run back, caret included.
    CHECK(f.wm.redoFocusedText());
    CHECK(f.text() == "abc");
    CHECK(f.caret() == 3);
    CHECK_FALSE(f.wm.redoFocusedText());

    // A caret move between two runs makes them two steps.
    CHECK(f.wm.editFocusedText(TE::Home, false));
    f.wm.inputText("x"); f.wm.inputText("y");
    REQUIRE(f.text() == "xyabc");
    CHECK(f.wm.undoFocusedText());
    CHECK(f.text() == "abc");
    CHECK(f.wm.undoFocusedText());
    CHECK(f.text().empty());
}

TEST_CASE("Text field: deleting is its own run, and a new edit drops the redo stack")
{
    TextFieldFixture f("hello");
    f.wm.inputBackspace(); f.wm.inputBackspace();
    REQUIRE(f.text() == "hel");
    // Typing and deleting are different kinds, so they never merge.
    f.wm.inputText("p");
    REQUIRE(f.text() == "help");
    CHECK(f.wm.undoFocusedText());
    CHECK(f.text() == "hel");
    CHECK(f.wm.undoFocusedText());
    CHECK(f.text() == "hello");

    // There is a redo waiting — until something new is typed over it.
    f.wm.inputText("!");
    CHECK(f.text() == "hello!");
    CHECK_FALSE(f.wm.redoFocusedText());
    CHECK(f.wm.undoFocusedText());
    CHECK(f.text() == "hello");
}

TEST_CASE("Text field: undo restores the selection a step was taken over")
{
    TextFieldFixture f("hello world");
    using TE = WidgetManager::TextEdit;
    CHECK(f.wm.editFocusedText(TE::SelectAll, false));
    REQUIRE(f.wm.focusedSelection() == "hello world");
    // Typing over a selection is one step of its own, never folded into a run.
    f.wm.inputText("z");
    REQUIRE(f.text() == "z");
    CHECK(f.wm.undoFocusedText());
    CHECK(f.text() == "hello world");
    CHECK(f.wm.focusedSelection() == "hello world");

    // A cut is a step as well.
    CHECK(f.wm.deleteFocusedSelection());
    CHECK(f.text().empty());
    CHECK(f.wm.undoFocusedText());
    CHECK(f.text() == "hello world");

    // The most ordinary edit there is: select something and type over it. The
    // letters that follow belong to the same step, so one Ctrl+Z brings the
    // original back rather than the first letter of the replacement.
    TextFieldFixture g("hello world");
    CHECK(g.wm.editFocusedText(TE::SelectAll, false));
    g.wm.inputText("a"); g.wm.inputText("b"); g.wm.inputText("c");
    REQUIRE(g.text() == "abc");
    CHECK(g.wm.undoFocusedText());
    CHECK(g.text() == "hello world");
}

TEST_CASE("Text field: a paste comes back in one step, and a word delete in another")
{
    TextFieldFixture f;
    f.wm.inputText("one two");           // a paste: more than one character
    f.wm.inputText("!");                 // a keystroke after it
    REQUIRE(f.text() == "one two!");
    CHECK(f.wm.undoFocusedText());
    CHECK(f.text() == "one two");
    CHECK(f.wm.undoFocusedText());
    CHECK(f.text().empty());

    TextFieldFixture g("alpha beta");
    CHECK(g.wm.editFocusedText(WidgetManager::TextEdit::DeleteWordLeft, false));
    REQUIRE(g.text() == "alpha ");
    CHECK(g.wm.undoFocusedText());
    CHECK(g.text() == "alpha beta");
}

TEST_CASE("Text field: a read-only field has no history, and a filtered keystroke leaves none")
{
    TextFieldFixture ro("locked", 0, /*editable=*/false);
    ro.wm.inputText("x");
    CHECK(ro.text() == "locked");
    CHECK_FALSE(ro.wm.undoFocusedText());
    CHECK_FALSE(ro.wm.redoFocusedText());

    // A character the filter refuses changed nothing, so there is nothing to
    // take back — an undo step for a keystroke that never landed would take
    // back the one before it instead.
    TextFieldFixture f;
    f.live()->inputFilter = HE::UITextInput::FilterInteger;
    f.wm.inputText("4");
    f.wm.inputText("q");
    REQUIRE(f.text() == "4");
    CHECK(f.wm.undoFocusedText());
    CHECK(f.text().empty());
    CHECK_FALSE(f.wm.undoFocusedText());
}

TEST_CASE("Text field: a new line is one step of its own")
{
    TextFieldFixture f;
    f.live()->multiline = true;
    f.wm.inputText("one");
    f.wm.inputSubmit();                  // Enter, which here means a new line
    f.wm.inputText("two");
    REQUIRE(f.text() == "one\ntwo");
    CHECK(f.wm.undoFocusedText());
    CHECK(f.text() == "one\n");
    CHECK(f.wm.undoFocusedText());
    CHECK(f.text() == "one");
    CHECK(f.wm.undoFocusedText());
    CHECK(f.text().empty());
}

// ─── Schicht 0: borders ──────────────────────────────────────────────────────
// A border is an element-level style, so no widget type knows about it: the
// manager stamps it onto the SURFACE the element drew. The interesting half is
// which quad counts as the surface — a progress bar's fill must not be outlined
// along with its track.

TEST_CASE("Surface styling is offered exactly where it would land")
{
    using namespace HE;
    // The pinned list. Adding a type here without giving it a real background
    // (or the other way round) is the bug this test exists to catch: the
    // properties would be offered and quietly do nothing, or an element with a
    // perfectly good surface would never be offered them.
    const std::vector<UIWidgetType> kSurfaces = {
        UIWidgetType::Panel, UIWidgetType::Image, UIWidgetType::Button,
        UIWidgetType::ProgressBar, UIWidgetType::TextInput, UIWidgetType::ComboBox,
        // A list is a panel with rows in it: it draws its own rectangle first
        // (even fully transparent) so the border and the rounding have something
        // to land on.
        UIWidgetType::ListView,
        // Both pickers draw their own rectangle first, for the same reason: a
        // calendar and a colour field are cards, and a card without a rounding
        // or a border is not what anybody puts in a dialog.
        UIWidgetType::DatePicker, UIWidgetType::ColorPicker,
        // An accordion, for the list's reason: it emits its own rectangle first
        // so the heading bands have a card to sit on.
        UIWidgetType::Accordion,
    };

    for (int t = 0; t < static_cast<int>(UIWidgetType::COUNT); ++t)
    {
        UIWidgetTree tree;
        tree.canvasWidth = 400.0f; tree.canvasHeight = 200.0f;
        tree.scaleMode = UICanvasScaleMode::ConstantPixel;
        const int id = tree.add(static_cast<UIWidgetType>(t));
        UIElement* e = tree.find(id);
        REQUIRE(e);
        uiSetAnchorPreset(*e, 0); e->pivotX = e->pivotY = 0.0f;
        e->posX = 0.0f; e->posY = 0.0f; e->sizeX = 200.0f; e->sizeY = 60.0f;

        const bool expected =
            std::find(kSurfaces.begin(), kSurfaces.end(),
                      static_cast<UIWidgetType>(t)) != kSurfaces.end();
        CAPTURE(e->typeName());
        CHECK(e->hasSurfaceStyle() == expected);

        // The rows that ride on a surface, pinned by name. They appear together
        // and nowhere else, so a new "Schicht 0" property has to be added here
        // on purpose rather than quietly showing up on a Text label.
        static const std::vector<std::string> kSurfaceRows = {
            "Corner Radius", "Corner TL", "Corner TR", "Corner BR", "Corner BL",
            "Border Width", "Border Color",
            "Gradient", "Gradient Color", "Gradient Angle", "Gradient Shape",
            "Shadow", "Shadow Color", "Shadow Blur", "Shadow Offset",
            "Inner Shadow", "Inner Shadow Color", "Inner Shadow Blur" };
        const std::vector<UIPropDesc> all = e->allProperties();
        for (const std::string& row : kSurfaceRows)
        {
            CAPTURE(row);
            CHECK(std::any_of(all.begin(), all.end(),
                              [&](const UIPropDesc& d){ return d.name == row; }) == expected);
        }

        // …and the claim has to be TRUE: whatever says it has a surface must
        // actually draw one that covers its whole rect, or the stamp misses it.
        std::vector<UIRenderObject> out;
        UIElementRenderState st;
        UIWidgetRect px{ 0.0f, 0.0f, 200.0f, 60.0f };
        e->render(px, st, UUID{}, 1.0f, out);
        if (!e->hasSurfaceStyle()) continue;
        REQUIRE_FALSE(out.empty());
        CHECK(out[0].type == 0);
        CHECK(out[0].size.x == doctest::Approx(px.w));
        CHECK(out[0].size.y == doctest::Approx(px.h));
    }
}

TEST_CASE("A border lands on the element's surface and nowhere else")
{
    TempWidgetDir dir;
    ContentManager cm{ dir.path.string() };
    HE::UIWidgetTree authored;
    authored.canvasWidth = 400.0f; authored.canvasHeight = 200.0f;
    authored.scaleMode = HE::UICanvasScaleMode::ConstantPixel;

    const int panel = authored.add(HE::UIWidgetType::Panel);
    {
        HE::UIElement* e = authored.find(panel);
        HE::uiSetAnchorPreset(*e, 0); e->pivotX = e->pivotY = 0.0f;
        e->posX = 0.0f; e->posY = 0.0f; e->sizeX = 200.0f; e->sizeY = 100.0f;
        e->borderWidth = 2.0f;
        e->borderColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    }
    // A progress bar draws a track AND a fill on top of it. Only the track is
    // the surface; outlining the fill would draw a box around a moving bar.
    const int bar = authored.add(HE::UIWidgetType::ProgressBar);
    {
        HE::UIElement* e = authored.find(bar);
        HE::uiSetAnchorPreset(*e, 0); e->pivotX = e->pivotY = 0.0f;
        e->posX = 0.0f; e->posY = 120.0f; e->sizeX = 200.0f; e->sizeY = 20.0f;
        e->borderWidth = 3.0f;
        if (auto* pb = dynamic_cast<HE::UIProgressBar*>(e)) pb->value = 0.5f;
    }
    registerWidget(cm, authored);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 200.0f, out);

    int bordered = 0;
    for (const UIRenderObject& o : out)
        if (o.borderWidth > 0.0f) ++bordered;
    // One per element, not one per quad: the panel's background and the bar's
    // track. The fill drawn on top of the track is NOT the surface.
    CHECK(bordered == 2);

    // The panel's own quad carries the authored width and colour.
    bool sawPanel = false;
    for (const UIRenderObject& o : out)
        if (o.borderWidth > 0.0f && o.size.x == doctest::Approx(200.0f) &&
            o.size.y == doctest::Approx(100.0f))
        {
            sawPanel = true;
            CHECK(o.borderWidth == doctest::Approx(2.0f));
            CHECK(o.borderColor.r == doctest::Approx(1.0f));
            CHECK(o.borderColor.g == doctest::Approx(0.0f));
        }
    CHECK(sawPanel);

    // Glyph quads never carry one — an outlined letter is not a border.
    for (const UIRenderObject& o : out)
        if (o.type == 2) CHECK(o.borderWidth == doctest::Approx(0.0f));
}

// ─── A button is a surface with children ─────────────────────────────────────

TEST_CASE("A button's old caption becomes a Text child, once")
{
    // A widget authored under the old rule: the caption lives on the Button.
    const std::string legacy = R"({
        "canvasWidth": 400, "canvasHeight": 200, "scaleMode": 5, "nextId": 2,
        "elements": [ { "id": 1, "type": "Button", "x": 0, "y": 0, "w": 200, "h": 60,
                        "text": "PLAY", "fontSize": 24,
                        "textColor": [1.0, 0.5, 0.25, 1.0] } ]
    })";

    HE::UIWidgetTree t;
    REQUIRE(HE::uiWidgetTreeFromJson(legacy, t));
    REQUIRE(t.elements.size() == 2);           // the button, plus its new label

    const HE::UIElement* label = nullptr;
    for (const auto& e : t.elements)
        if (e->type() == HE::UIWidgetType::Text) label = e.get();
    REQUIRE(label);
    CHECK(label->parentId == 1);               // a child OF the button
    CHECK_FALSE(label->hitTestable);           // and never steals its click
    const auto* txt = dynamic_cast<const HE::UIText*>(label);
    REQUIRE(txt);
    CHECK(txt->text == "PLAY");
    CHECK(txt->fontSize == doctest::Approx(24.0f));
    CHECK(txt->color.g == doctest::Approx(0.5f));

    // Saving drops the legacy keys, so loading the result again must NOT add a
    // second label. Running twice is the classic migration bug.
    const std::string resaved = HE::uiWidgetTreeToJson(t);
    HE::UIWidgetTree again;
    REQUIRE(HE::uiWidgetTreeFromJson(resaved, again));
    int labels = 0;
    for (const auto& e : again.elements)
        if (e->type() == HE::UIWidgetType::Text) ++labels;
    CHECK(labels == 1);
}

TEST_CASE("A button draws only its surface, and its children draw on it")
{
    TempWidgetDir dir;
    ContentManager cm{ dir.path.string() };
    HE::UIWidgetTree authored;
    authored.canvasWidth = 400.0f; authored.canvasHeight = 200.0f;
    authored.scaleMode = HE::UICanvasScaleMode::ConstantPixel;

    const int btn = authored.add(HE::UIWidgetType::Button);
    {
        HE::UIElement* e = authored.find(btn);
        HE::uiSetAnchorPreset(*e, 0); e->pivotX = e->pivotY = 0.0f;
        e->posX = 0.0f; e->posY = 0.0f; e->sizeX = 200.0f; e->sizeY = 60.0f;
    }
    // Two children, anchored inside the button — an icon left, a caption filling
    // the rest. This is the layout a built-in centred string could never do.
    const int icon = authored.add(HE::UIWidgetType::Image);
    {
        HE::UIElement* e = authored.find(icon);
        e->parentId = btn;
        HE::uiSetAnchorPreset(*e, 0); e->pivotX = e->pivotY = 0.0f;
        e->posX = 8.0f; e->posY = 14.0f; e->sizeX = 32.0f; e->sizeY = 32.0f;
    }
    const int label = authored.add(HE::UIWidgetType::Text);
    {
        HE::UIElement* e = authored.find(label);
        e->parentId = btn;
        HE::uiSetAnchorPreset(*e, 0); e->pivotX = e->pivotY = 0.0f;
        e->posX = 48.0f; e->posY = 18.0f; e->sizeX = 140.0f; e->sizeY = 24.0f;
        if (auto* t = dynamic_cast<HE::UIText*>(e)) { t->text = "Go"; t->autoSize = false; }
    }
    registerWidget(cm, authored);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 200.0f, out);

    // The icon is positioned RELATIVE to the button, not to the canvas.
    bool sawIcon = false;
    for (const UIRenderObject& o : out)
        if (o.type == 0 && o.size.x == doctest::Approx(32.0f) &&
            o.size.y == doctest::Approx(32.0f))
        {
            sawIcon = true;
            CHECK(o.position.x == doctest::Approx(8.0f));
            CHECK(o.position.y == doctest::Approx(14.0f));
        }
    CHECK(sawIcon);
    // …and the caption's glyphs are there, drawn by the Text child.
    CHECK(countGlyphs(out) == 2);   // "Go"

    // The click still belongs to the button: a Text child is not interactive, so
    // it is transparent to the pointer.
    CHECK(wm.processPointer(400.0f, 200.0f, 100.0f, 30.0f, false, true));
}

TEST_CASE("The corner radius is authored, and old widgets keep their look")
{
    // The types that used to hard-code one keep it as their default, so a widget
    // saved before the radius was a property renders unchanged.
    // Every corner, because the default is one number on all four.
    auto allFour = [](const HE::UIElement& e, float v)
    {
        CHECK(e.cornerRadius.x == doctest::Approx(v));
        CHECK(e.cornerRadius.y == doctest::Approx(v));
        CHECK(e.cornerRadius.z == doctest::Approx(v));
        CHECK(e.cornerRadius.w == doctest::Approx(v));
        CHECK(e.uniformCornerRadius());
    };
    allFour(HE::UIButton(),      6.0f);
    allFour(HE::UITextInput(),   4.0f);
    allFour(HE::UIComboBox(),    4.0f);
    allFour(HE::UIProgressBar(), 4.0f);
    // A Panel could not be rounded at all before; it starts square.
    allFour(HE::UIPanel(), 0.0f);

    // An element whose JSON predates the property must not read back as 0 —
    // absent means "the type's default", not "square".
    HE::UIWidgetTree t;
    const int b = t.add(HE::UIWidgetType::Button);
    const std::string json = HE::uiWidgetTreeToJson(t);
    CHECK(json.find("cornerRadius") == std::string::npos);  // 6 is the default, not written
    HE::UIWidgetTree loaded;
    REQUIRE(HE::uiWidgetTreeFromJson(json, loaded));
    allFour(*loaded.find(b), 6.0f);

    // …and an authored one round-trips, still through the ORIGINAL scalar key:
    // one rounding on all four corners is what nearly every widget has, and it
    // must not start costing a four-element array on disk.
    t.find(b)->cornerRadius = glm::vec4(12.0f);
    const std::string uniform = HE::uiWidgetTreeToJson(t);
    CHECK(uniform.find("\"cornerRadius\"") != std::string::npos);
    CHECK(uniform.find("cornerRadii")      == std::string::npos);
    HE::UIWidgetTree loaded2;
    REQUIRE(HE::uiWidgetTreeFromJson(uniform, loaded2));
    allFour(*loaded2.find(b), 12.0f);
}

TEST_CASE("Corners round one at a time")
{
    HE::UIWidgetTree t;
    const int b = t.add(HE::UIWidgetType::Button);
    HE::UIElement& e = *t.find(b);

    // The single name is all four at once — what a script or a theme means when
    // it says "round this" — and reads back as the corner it would have set.
    e.setPropAny("Corner Radius", HE::UIPropValue::ofFloat(9.0f));
    CHECK(e.uniformCornerRadius());
    CHECK(e.getPropAny("Corner Radius").f == doctest::Approx(9.0f));

    // The four named rows address one corner each, in CSS order.
    e.setPropAny("Corner TR", HE::UIPropValue::ofFloat(0.0f));
    e.setPropAny("Corner BR", HE::UIPropValue::ofFloat(0.0f));
    CHECK(e.cornerRadius.x == doctest::Approx(9.0f));   // TL
    CHECK(e.cornerRadius.y == doctest::Approx(0.0f));   // TR
    CHECK(e.cornerRadius.z == doctest::Approx(0.0f));   // BR
    CHECK(e.cornerRadius.w == doctest::Approx(9.0f));   // BL
    CHECK_FALSE(e.uniformCornerRadius());
    CHECK(e.maxCornerRadius() == doctest::Approx(9.0f));

    // Four different corners are the one case that costs the array, and it
    // round-trips.
    const std::string json = HE::uiWidgetTreeToJson(t);
    CHECK(json.find("cornerRadii") != std::string::npos);
    HE::UIWidgetTree loaded;
    REQUIRE(HE::uiWidgetTreeFromJson(json, loaded));
    const HE::UIElement& r = *loaded.find(b);
    CHECK(r.cornerRadius.x == doctest::Approx(9.0f));
    CHECK(r.cornerRadius.y == doctest::Approx(0.0f));
    CHECK(r.cornerRadius.z == doctest::Approx(0.0f));
    CHECK(r.cornerRadius.w == doctest::Approx(9.0f));

    // A widget written before the array existed carries the scalar key, and it
    // still means all four corners.
    HE::UIWidgetTree old;
    REQUIRE(HE::uiWidgetTreeFromJson(
        R"({"canvasWidth":100,"canvasHeight":100,"nextId":2,"elements":[
            {"id":1,"parent":0,"type":"Panel","cornerRadius":7.0}]})", old));
    const HE::UIElement& o = *old.find(1);
    CHECK(o.uniformCornerRadius());
    CHECK(o.cornerRadius.x == doctest::Approx(7.0f));
}

TEST_CASE("The corner radius reaches the quad exactly once")
{
    TempWidgetDir dir;
    ContentManager cm{ dir.path.string() };
    HE::UIWidgetTree authored;
    authored.canvasWidth = 400.0f; authored.canvasHeight = 200.0f;
    authored.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int btn = authored.add(HE::UIWidgetType::Button);
    {
        HE::UIElement* e = authored.find(btn);
        HE::uiSetAnchorPreset(*e, 0); e->pivotX = e->pivotY = 0.0f;
        e->posX = 0.0f; e->posY = 0.0f; e->sizeX = 200.0f; e->sizeY = 60.0f;
        e->cornerRadius = glm::vec4(10.0f);
    }
    registerWidget(cm, authored);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 200.0f, out);

    // The background carries the authored radius — the element's render() no
    // longer passes one of its own, so this cannot be a doubled value.
    REQUIRE_FALSE(out.empty());
    bool sawSurface = false;
    for (const UIRenderObject& o : out)
        if (o.type == 0 && o.size.x == doctest::Approx(200.0f) &&
            o.size.y == doctest::Approx(60.0f))
        {
            sawSurface = true;
            CHECK(o.cornerRadius.x == doctest::Approx(10.0f));
            CHECK(o.cornerRadius.z == doctest::Approx(10.0f));
        }
    CHECK(sawSurface);
}

TEST_CASE("A gradient rides on the same surface as the border")
{
    TempWidgetDir dir;
    ContentManager cm{ dir.path.string() };
    HE::UIWidgetTree authored;
    authored.canvasWidth = 400.0f; authored.canvasHeight = 200.0f;
    authored.scaleMode = HE::UICanvasScaleMode::ConstantPixel;

    const int panel = authored.add(HE::UIWidgetType::Panel);
    {
        HE::UIElement* e = authored.find(panel);
        HE::uiSetAnchorPreset(*e, 0); e->pivotX = e->pivotY = 0.0f;
        e->posX = 0.0f; e->posY = 0.0f; e->sizeX = 200.0f; e->sizeY = 100.0f;
        e->gradient      = true;
        e->gradientColor = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
        e->gradientAngle = 90.0f;
    }
    registerWidget(cm, authored);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 200.0f, out);

    int gradients = 0;
    for (const UIRenderObject& o : out)
        if (o.gradient)
        {
            ++gradients;
            CHECK(o.gradientColor.g == doctest::Approx(1.0f));
            // The angle is not a length: a scaled canvas must not turn it.
            CHECK(o.gradientAngleDeg == doctest::Approx(90.0f));
        }
    CHECK(gradients == 1);
}

// The second gradient shape. Linear is what every gradient authored so far is,
// so "absent" has to keep meaning exactly that on disk.
TEST_CASE("A gradient can be radial instead of linear")
{
    HE::UIWidgetTree t;
    const int id = t.add(HE::UIWidgetType::Panel);
    HE::UIElement& e = *t.find(id);
    e.gradient = true;
    e.gradientColor = glm::vec4(0.1f, 0.2f, 0.3f, 1.0f);
    CHECK(e.gradientShape == 0);                       // linear by default

    const std::string linear = HE::uiWidgetTreeToJson(t);
    CHECK(linear.find("gradientShape") == std::string::npos);

    e.setPropAny("Gradient Shape", HE::UIPropValue::ofInt(1));
    CHECK(e.gradientShape == 1);
    // Anything that is not the radial one is the linear one — the shape is a
    // choice of two, not an int a graph can put 7 into.
    e.setPropAny("Gradient Shape", HE::UIPropValue::ofInt(7));
    CHECK(e.gradientShape == 0);
    e.setPropAny("Gradient Shape", HE::UIPropValue::ofInt(1));

    HE::UIWidgetTree loaded;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), loaded));
    CHECK(loaded.find(id)->gradientShape == 1);
    CHECK(loaded.find(id)->getPropAny("Gradient Shape").i == 1);
}

// A drop shadow is one more quad, emitted UNDER the element and grown by its
// own blur so the falloff is not cut off at the shape's edge.
TEST_CASE("A drop shadow is a quad of its own, under the element")
{
    TempWidgetDir dir;
    ContentManager cm{ dir.path.string() };
    HE::UIWidgetTree authored;
    authored.canvasWidth = 400.0f; authored.canvasHeight = 200.0f;
    authored.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int panel = authored.add(HE::UIWidgetType::Panel);
    {
        HE::UIElement* e = authored.find(panel);
        HE::uiSetAnchorPreset(*e, 0); e->pivotX = e->pivotY = 0.0f;
        e->posX = 50.0f; e->posY = 40.0f; e->sizeX = 200.0f; e->sizeY = 60.0f;
        e->cornerRadius = glm::vec4(6.0f);
        e->shadow = true;
        e->shadowColor = glm::vec4(0.0f, 0.0f, 0.0f, 0.5f);
        e->shadowBlur = 8.0f;
        e->shadowOffsetX = 2.0f; e->shadowOffsetY = 4.0f;
    }
    registerWidget(cm, authored);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 200.0f, out);
    REQUIRE(out.size() >= 2);

    // First the shadow, then the surface — painter order, no insert.
    const UIRenderObject& sh = out[0];
    const UIRenderObject& surf = out[1];
    CHECK(sh.blur == doctest::Approx(8.0f));
    CHECK(sh.color.a == doctest::Approx(0.5f));
    // Offset applied, then grown by the blur on every side.
    CHECK(sh.position.x == doctest::Approx(50.0f + 2.0f - 8.0f));
    CHECK(sh.position.y == doctest::Approx(40.0f + 4.0f - 8.0f));
    CHECK(sh.size.x == doctest::Approx(200.0f + 16.0f));
    CHECK(sh.size.y == doctest::Approx(60.0f + 16.0f));
    // Same rounding as the thing casting it.
    CHECK(sh.cornerRadius.x == doctest::Approx(6.0f));
    // The element itself is untouched by any of it.
    CHECK(surf.blur == doctest::Approx(0.0f));
    CHECK(surf.position.x == doctest::Approx(50.0f));
    CHECK(surf.size.x == doctest::Approx(200.0f));

    // Switched off, there is no extra quad at all — a shadow nobody asked for
    // must not cost a draw.
    wm.clear();
    authored.find(panel)->shadow = false;
    registerWidget(cm, authored);
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    out.clear();
    wm.extract(400.0f, 200.0f, out);
    CHECK(out.size() == 1);
    CHECK(out[0].blur == doctest::Approx(0.0f));
}

// The inner one is not a second quad: it has to be cut off by the surface's own
// shape, so it rides on the surface quad like the border and the gradient do.
TEST_CASE("An inner shadow rides on the surface it darkens")
{
    TempWidgetDir dir;
    ContentManager cm{ dir.path.string() };
    HE::UIWidgetTree authored;
    authored.canvasWidth = 400.0f; authored.canvasHeight = 200.0f;
    authored.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    // A progress bar draws a track AND a fill. Only the track is the surface.
    const int bar = authored.add(HE::UIWidgetType::ProgressBar);
    {
        HE::UIElement* e = authored.find(bar);
        HE::uiSetAnchorPreset(*e, 0); e->pivotX = e->pivotY = 0.0f;
        e->posX = 0.0f; e->posY = 0.0f; e->sizeX = 200.0f; e->sizeY = 20.0f;
        e->innerShadow = true;
        e->innerShadowBlur = 5.0f;
        e->innerShadowColor = glm::vec4(0.0f, 0.0f, 0.0f, 0.6f);
        if (auto* pb = dynamic_cast<HE::UIProgressBar*>(e)) pb->value = 0.5f;
    }
    registerWidget(cm, authored);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 200.0f, out);

    int shaded = 0;
    for (const UIRenderObject& o : out)
        if (o.innerShadowBlur > 0.0f)
        {
            ++shaded;
            CHECK(o.innerShadowBlur == doctest::Approx(5.0f));
            CHECK(o.innerShadowColor.a == doctest::Approx(0.6f));
            CHECK(o.size.x == doctest::Approx(200.0f));   // the track, not the fill
        }
    CHECK(shaded == 1);
}

TEST_CASE("Both shadows survive a save and a load")
{
    HE::UIWidgetTree t;
    const int id = t.add(HE::UIWidgetType::Panel);
    HE::UIElement& e = *t.find(id);

    // Nothing switched on writes nothing — an old widget saves byte-identically.
    CHECK(HE::uiWidgetTreeToJson(t).find("shadow") == std::string::npos);

    e.setPropAny("Shadow", HE::UIPropValue::ofBool(true));
    e.setPropAny("Shadow Blur", HE::UIPropValue::ofFloat(12.0f));
    e.setPropAny("Shadow Offset", HE::UIPropValue::ofVec2({ 3.0f, -2.0f }));
    e.setPropAny("Shadow Color", HE::UIPropValue::ofColor({ 0.1f, 0.2f, 0.3f, 0.7f }));
    e.setPropAny("Inner Shadow", HE::UIPropValue::ofBool(true));
    e.setPropAny("Inner Shadow Blur", HE::UIPropValue::ofFloat(4.0f));
    // A blur is a distance and cannot be negative, whoever asks.
    e.setPropAny("Shadow Blur", HE::UIPropValue::ofFloat(-5.0f));
    CHECK(e.shadowBlur == doctest::Approx(0.0f));
    e.setPropAny("Shadow Blur", HE::UIPropValue::ofFloat(12.0f));

    HE::UIWidgetTree loaded;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), loaded));
    const HE::UIElement& r = *loaded.find(id);
    CHECK(r.shadow);
    CHECK(r.shadowBlur == doctest::Approx(12.0f));
    CHECK(r.shadowOffsetX == doctest::Approx(3.0f));
    CHECK(r.shadowOffsetY == doctest::Approx(-2.0f));
    CHECK(r.shadowColor.a == doctest::Approx(0.7f));
    CHECK(r.innerShadow);
    CHECK(r.innerShadowBlur == doctest::Approx(4.0f));
}

TEST_CASE("A border survives a save and a load")
{
    HE::UIWidgetTree t;
    const int id = t.add(HE::UIWidgetType::Panel);
    t.find(id)->borderWidth = 4.0f;
    t.find(id)->borderColor = glm::vec4(0.2f, 0.4f, 0.6f, 0.8f);

    HE::UIWidgetTree loaded;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), loaded));
    const HE::UIElement* e = loaded.find(id);
    REQUIRE(e);
    CHECK(e->borderWidth == doctest::Approx(4.0f));
    CHECK(e->borderColor.b == doctest::Approx(0.6f));
    CHECK(e->borderColor.a == doctest::Approx(0.8f));

    // A gradient round-trips the same way.
    HE::UIWidgetTree g;
    const int gid = g.add(HE::UIWidgetType::Panel);
    g.find(gid)->gradient      = true;
    g.find(gid)->gradientColor = glm::vec4(0.1f, 0.2f, 0.3f, 0.4f);
    g.find(gid)->gradientAngle = 45.0f;
    HE::UIWidgetTree gLoaded;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(g), gLoaded));
    REQUIRE(gLoaded.find(gid));
    CHECK(gLoaded.find(gid)->gradient);
    CHECK(gLoaded.find(gid)->gradientAngle == doctest::Approx(45.0f));
    CHECK(gLoaded.find(gid)->gradientColor.b == doctest::Approx(0.3f));

    // An element without either writes nothing, so a widget authored before
    // these existed saves byte-identically.
    HE::UIWidgetTree plainTree;
    plainTree.add(HE::UIWidgetType::Panel);
    const std::string json = HE::uiWidgetTreeToJson(plainTree);
    CHECK(json.find("borderWidth") == std::string::npos);
    CHECK(json.find("gradient") == std::string::npos);
}

TEST_CASE("Creating a widget does not show it")
{
    TempWidgetDir dir;
    ContentManager cm{ dir.path.string() };
    HE::UIWidgetTree authored;
    authored.canvasWidth = 400.0f; authored.canvasHeight = 200.0f;
    authored.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int panel = authored.add(HE::UIWidgetType::Panel);
    HE::uiSetAnchorPreset(*authored.find(panel), 0);
    authored.find(panel)->posX = authored.find(panel)->posY = 0.0f;
    authored.find(panel)->sizeX = authored.find(panel)->sizeY = 100.0f;
    registerWidget(cm, authored);

    WidgetManager wm;
    const int id = wm.createWidget(cm, "mem://w.hasset");
    REQUIRE(id != 0);

    // It exists…
    CHECK(wm.count() == 1);
    CHECK(wm.isAlive(id));
    // …and it is NOT on screen, which is the whole point: Create Widget makes an
    // instance of the class, Show Widget is what puts it up.
    CHECK_FALSE(wm.isVisible(id));
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 200.0f, out);
    CHECK(out.empty());

    wm.showWidget(id);
    CHECK(wm.isVisible(id));
    out.clear();
    wm.extract(400.0f, 200.0f, out);
    CHECK_FALSE(out.empty());
}

TEST_CASE("Text field: word-wise movement and deletion")
{
    TextFieldFixture f("the quick brown fox");
    using TE = WidgetManager::TextEdit;
    REQUIRE(f.caret() == 19);

    // Ctrl+Left walks back over whole words, including the separator it starts in.
    CHECK(f.wm.editFocusedText(TE::WordLeft, false));
    CHECK(f.caret() == 16);                       // start of "fox"
    CHECK(f.wm.editFocusedText(TE::WordLeft, false));
    CHECK(f.caret() == 10);                       // start of "brown"

    // …and Ctrl+Right forward to the END of a word, not to the next start —
    // that is the difference nobody notices until the caret lands wrong.
    CHECK(f.wm.editFocusedText(TE::WordRight, false));
    CHECK(f.caret() == 15);                       // end of "brown"

    // With shift it drags a selection along instead of collapsing it.
    CHECK(f.wm.editFocusedText(TE::WordRight, true));
    CHECK(f.wm.focusedSelection() == " fox");

    // The edges say "nothing happened" rather than pretending.
    CHECK(f.wm.editFocusedText(TE::Home, false));
    CHECK_FALSE(f.wm.editFocusedText(TE::WordLeft, false));
    CHECK(f.wm.editFocusedText(TE::End, false));
    CHECK_FALSE(f.wm.editFocusedText(TE::WordRight, false));

    // Ctrl+Backspace takes the word before the caret.
    CHECK(f.wm.editFocusedText(TE::DeleteWordLeft, false));
    CHECK(f.text() == "the quick brown ");
    CHECK(f.caret() == 16);
    CHECK(f.wm.editFocusedText(TE::DeleteWordLeft, false));
    CHECK(f.text() == "the quick ");
}

TEST_CASE("Text field: dragging selects, double-click takes a word")
{
    TextFieldFixture f("hello world");
    // The fixture's click left the caret at the end. Press near the left edge to
    // put the anchor there, then drag right without releasing.
    REQUIRE(f.wm.processPointer(400.0f, 200.0f, 8.0f, 20.0f, true, true));
    const std::string dragged = [&] {
        // A held pointer that moves extends the selection; the anchor stays put.
        f.wm.processPointer(400.0f, 200.0f, 60.0f, 20.0f, true, true);
        return f.wm.focusedSelection();
    }();
    CHECK_FALSE(dragged.empty());
    CHECK(std::string("hello world").rfind(dragged, 0) == 0);   // a prefix of the text
    f.wm.processPointer(400.0f, 200.0f, 60.0f, 20.0f, false, true);

    // A double-click selects the word under it rather than a run of pixels.
    REQUIRE(f.wm.selectWordAtPointer(400.0f, 200.0f, 8.0f, 20.0f));
    CHECK(f.wm.focusedSelection() == "hello");

    // …and the triple-click path takes everything.
    CHECK(f.wm.selectAllFocused());
    CHECK(f.wm.focusedSelection() == "hello world");
}

TEST_CASE("Text field: long text scrolls under the caret and clicks still land")
{
    // A field far narrower than its text: 400 px of canvas, and enough text that
    // the end of it cannot be on screen at the same time as the start.
    // ~740 px of Roboto at the field's 18 px against 388 px of visible strip:
    // comfortably past the edge, so the scroll cannot be an accident of rounding.
    TextFieldFixture f("the quick brown fox jumps over the lazy dog again and again, "
                       "and once more for good measure, and then a little further");
    using TE = WidgetManager::TextEdit;
    std::vector<UIRenderObject> out;
    // The fixture's click landed wherever 395 px into the text is, which for a
    // string this long is the middle — so the caret is asked to the END first.
    // That is the position the field cannot show without scrolling.
    CHECK(f.wm.editFocusedText(TE::End, false));
    f.wm.extract(400.0f, 200.0f, out);
    CHECK(f.scrollPx() > 0.0f);

    // Every glyph the field emits is clipped to the field, so nothing spills out
    // sideways over whatever sits next to it.
    bool sawClippedGlyph = false;
    for (const UIRenderObject& o : out)
        if (o.type == 2 && o.clipRect.z > 0.0f) sawClippedGlyph = true;
    CHECK(sawClippedGlyph);

    // Home brings it back to the start on the next draw.
    CHECK(f.wm.editFocusedText(TE::Home, false));
    out.clear();
    f.wm.extract(400.0f, 200.0f, out);
    CHECK(f.scrollPx() == doctest::Approx(0.0f));

    // Back to the end, then click near the right edge: the caret must land in
    // the LAST word, not where that pixel would be in unscrolled text.
    CHECK(f.wm.editFocusedText(TE::End, false));
    out.clear();
    f.wm.extract(400.0f, 200.0f, out);
    REQUIRE(f.scrollPx() > 0.0f);
    REQUIRE(f.wm.setCaretFromPointer(400.0f, 200.0f, 390.0f, 20.0f));
    CHECK(f.caret() > 40);
}

TEST_CASE("Text field: multi-byte characters move and delete as one")
{
    TextFieldFixture f("aäb");
    using TE = WidgetManager::TextEdit;
    CHECK(f.text().size() == 4);                 // a + 2-byte ä + b

    CHECK(f.wm.editFocusedText(TE::Home, false));
    CHECK(f.wm.editFocusedText(TE::Right, false));
    CHECK(f.caret() == 1);
    CHECK(f.wm.editFocusedText(TE::Right, false));
    CHECK(f.caret() == 3);                       // stepped OVER the ä, not into it
    f.wm.inputBackspace();
    CHECK(f.text() == "ab");                     // the whole ä went

    // Typing one is one character too.
    f.wm.inputText("ü");
    CHECK(f.text() == "aüb");
    CHECK(f.caret() == 3);
}

TEST_CASE("Text field: Max Length counts characters, and stops typing")
{
    TextFieldFixture f("", /*maxLength=*/3);
    f.wm.inputText("ab");
    f.wm.inputText("cd");                        // only "c" fits
    CHECK(f.text() == "abc");
    f.wm.inputText("x");
    CHECK(f.text() == "abc");

    // Characters, not bytes: three accented letters fit just as three plain
    // ones do.
    TextFieldFixture g("", 3);
    g.wm.inputText("äöüß");
    CHECK(g.text() == "äöü");
}

TEST_CASE("Text field: Editable and Selectable are switches, not decoration")
{
    using TE = WidgetManager::TextEdit;
    // Read-only: shows and selects, takes nothing in.
    TextFieldFixture ro("locked", 0, /*editable=*/false, /*selectable=*/true);
    ro.wm.inputText("x");
    CHECK(ro.text() == "locked");
    ro.wm.inputBackspace();
    CHECK(ro.text() == "locked");
    CHECK_FALSE(ro.wm.editFocusedText(TE::Delete, false));
    CHECK(ro.wm.editFocusedText(TE::SelectAll, false));      // …but selecting works
    CHECK(ro.wm.focusedSelection() == "locked");
    CHECK_FALSE(ro.wm.deleteFocusedSelection());             // and cutting does not
    CHECK(ro.text() == "locked");
    // The caret still moves — that is what makes it readable rather than dead.
    CHECK(ro.wm.editFocusedText(TE::Home, false));
    CHECK(ro.caret() == 0);

    // Selection off: typing works, selecting does not.
    TextFieldFixture ns("abc", 0, true, /*selectable=*/false);
    CHECK_FALSE(ns.wm.editFocusedText(TE::SelectAll, false));
    CHECK(ns.wm.focusedSelection().empty());
    CHECK(ns.wm.editFocusedText(TE::Home, false));
    CHECK(ns.wm.editFocusedText(TE::Right, true));           // shift does not extend
    CHECK(ns.wm.focusedSelection().empty());
    CHECK(ns.caret() == 1);
    ns.wm.inputText("Z");
    CHECK(ns.text() == "aZbc");
}

TEST_CASE("UITextInput: the new switches round-trip, defaults write nothing")
{
    HE::UIWidgetTree t;
    const int id = t.add(HE::UIWidgetType::TextInput);
    auto* ti = dynamic_cast<HE::UITextInput*>(t.find(id));
    REQUIRE(ti != nullptr);
    CHECK(ti->editable);
    CHECK(ti->selectable);
    CHECK_FALSE(ti->password);
    const std::string plain = HE::uiWidgetTreeToJson(t);
    CHECK(plain.find("password") == std::string::npos);
    CHECK(plain.find("editable") == std::string::npos);
    CHECK(plain.find("selectable") == std::string::npos);
    CHECK(plain.find("maxLength") == std::string::npos);

    ti->password = true; ti->editable = false; ti->selectable = false;
    ti->maxLength = 12;
    HE::UIWidgetTree r;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), r));
    auto* back = dynamic_cast<HE::UITextInput*>(r.find(id));
    REQUIRE(back != nullptr);
    CHECK(back->password);
    CHECK_FALSE(back->editable);
    CHECK_FALSE(back->selectable);
    CHECK(back->maxLength == 12);
    // …and by name, so a script can lock a field while a dialog is busy.
    back->setPropAny("Editable", HE::UIPropValue::ofBool(true));
    CHECK(back->editable);
    CHECK(back->getPropAny("Password").b);
}

TEST_CASE("UICheckBox: the tick box is sized by the label, not by the element")
{
    HE::UICheckBox cb;
    cb.label = "";
    cb.fontSize = 20.0f;

    // A checkbox stretched over a whole side — to place its label, say — used
    // to grow a tick box as tall as the element, which is what a stretched
    // anchor makes it. It stays label-sized.
    std::vector<UIRenderObject> tall;
    cb.render({ 0.0f, 0.0f, 400.0f, 300.0f }, {}, HE::UUID{}, 1.0f, tall);
    REQUIRE_FALSE(tall.empty());
    CHECK(tall[0].size.x == doctest::Approx(23.0f));   // 20 * 1.15
    CHECK(tall[0].size.y == doctest::Approx(23.0f));
    // …and it sits in the middle of the element rather than at its top.
    CHECK(tall[0].position.y == doctest::Approx((300.0f - 23.0f) * 0.5f));

    // A deliberately tiny one is still capped by the element.
    std::vector<UIRenderObject> small;
    cb.render({ 0.0f, 0.0f, 100.0f, 10.0f }, {}, HE::UUID{}, 1.0f, small);
    REQUIRE_FALSE(small.empty());
    CHECK(small[0].size.y == doctest::Approx(10.0f));

    // The pixel scale reaches it too: at half scale the box halves with the
    // text instead of staying put.
    std::vector<UIRenderObject> scaled;
    cb.render({ 0.0f, 0.0f, 400.0f, 300.0f }, {}, HE::UUID{}, 0.5f, scaled);
    REQUIRE_FALSE(scaled.empty());
    CHECK(scaled[0].size.x == doctest::Approx(11.5f));
}

TEST_CASE("WidgetRef: a same-size slot draws exactly what the widget draws alone")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    // The strongest statement there is about embedding: put a widget in a slot
    // the size of its own canvas and every quad has to land where it lands when
    // that widget IS the page. A checkbox anchored to the whole right side is
    // in here on purpose — a stretched anchor is where an off-by-a-factor hides.
    HE::UIWidgetTree sub;
    sub.canvasWidth = 1000.0f; sub.canvasHeight = 600.0f;
    sub.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    {
        const int cb = sub.add(HE::UIWidgetType::CheckBox);
        HE::UIElement& e = *sub.find(cb);
        e.setProp("Label", HE::UIPropValue::ofString("Ay"));
        HE::uiSetAnchorPreset(e, 14);                 // right edge, stretched down
        HE::uiSetAnchorInsetsY(e, 280.0f, 280.0f);    // → 40 tall
        e.pivotX = 1.0f;
        e.posX = -20.0f; e.sizeX = 160.0f;
        const int b = sub.add(HE::UIWidgetType::Button);
        HE::UIElement& eb = *sub.find(b);
        eb.setProp("Text", HE::UIPropValue::ofString("Go"));
        HE::uiSetAnchorPreset(eb, 5);                 // centre point
        eb.posX = 0.0f; eb.posY = 0.0f; eb.sizeX = 200.0f; eb.sizeY = 60.0f;
    }
    registerWidgetAs(cm, "mem://sub.hasset", sub);

    // …as a page of its own.
    registerWidget(cm, sub);
    WidgetManager alone;
    REQUIRE(createShown(alone, cm, "mem://w.hasset") != 0);
    std::vector<UIRenderObject> a;
    alone.extract(1000.0f, 600.0f, a);
    REQUIRE_FALSE(a.empty());

    // …and embedded in a slot of exactly that size, at an offset.
    HE::UIWidgetTree page;
    page.canvasWidth = 2000.0f; page.canvasHeight = 1000.0f;
    page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int ref = page.add(HE::UIWidgetType::WidgetRef);
    { HE::UIElement& e = *page.find(ref);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 100.0f; e.posY = 50.0f; e.sizeX = 1000.0f; e.sizeY = 600.0f;
      e.setProp("Widget", HE::UIPropValue::ofString("mem://sub.hasset")); }
    registerWidget(cm, page);
    WidgetManager embedded;
    REQUIRE(createShown(embedded, cm, "mem://w.hasset") != 0);
    std::vector<UIRenderObject> b;
    embedded.extract(2000.0f, 1000.0f, b);

    REQUIRE(b.size() == a.size());
    for (size_t i = 0; i < a.size(); ++i)
    {
        INFO("quad ", i);
        CHECK(b[i].size.x == doctest::Approx(a[i].size.x));
        CHECK(b[i].size.y == doctest::Approx(a[i].size.y));
        CHECK(b[i].position.x == doctest::Approx(a[i].position.x + 100.0f));
        CHECK(b[i].position.y == doctest::Approx(a[i].position.y + 50.0f));
    }
}

TEST_CASE("WidgetRef: two copies of one widget do not share element ids")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    registerWidgetAs(cm, "mem://row.hasset", rowWidget());

    HE::UIWidgetTree page;
    page.canvasWidth = 400.0f; page.canvasHeight = 400.0f;
    page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    for (int i = 0; i < 2; ++i)
    {
        const int ref = page.add(HE::UIWidgetType::WidgetRef);
        HE::UIElement& e = *page.find(ref);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = static_cast<float>(i) * 100.0f;
        e.sizeX = 200.0f; e.sizeY = 60.0f;
        e.setProp("Widget", HE::UIPropValue::ofString("mem://row.hasset"));
    }
    registerWidget(cm, page);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);

    // Both rows exist and answer at their own place — if the ids had collided,
    // the second graft would have overwritten the first.
    CHECK(wm.processPointer(400.0f, 400.0f, 100.0f,  30.0f, true, true));
    CHECK(wm.processPointer(400.0f, 400.0f, 100.0f, 130.0f, true, true));
    CHECK_FALSE(wm.processPointer(400.0f, 400.0f, 100.0f, 250.0f, true, true));
}

// Two copies of one component beside each other are two copies, not a circle.
// The guard used to keep ONE chain and push a path before recursing — but the
// recursion re-scanned the whole tree, so it met the SIBLING references of the
// one it had just expanded while that one's path was still on the chain. Two
// cards of the same component on one page was enough, and the second one was
// refused with its content dropped.
TEST_CASE("WidgetRef: the same component twice on a page is not a circle")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    registerWidgetAs(cm, "mem://row.hasset", rowWidget());

    HE::UIWidgetTree page;
    page.canvasWidth = 400.0f; page.canvasHeight = 400.0f;
    page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    for (int i = 0; i < 3; ++i)
    {
        const int ref = page.add(HE::UIWidgetType::WidgetRef);
        HE::UIElement& e = *page.find(ref);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = static_cast<float>(i) * 100.0f;
        e.sizeX = 200.0f; e.sizeY = 60.0f;
        e.setProp("Widget", HE::UIPropValue::ofString("mem://row.hasset"));
    }
    registerWidget(cm, page);

    // What actually broke was the MESSAGE, so that is what this asks about.
    // The old code recovered — a reference refused inside the recursion was
    // grafted anyway by the outer loop on its next turn — so every count and
    // every hit test came out right while the console filled with reports of a
    // circle that was not there. A false alarm about a broken document is worse
    // than a quiet bug: it sends somebody looking for damage that never
    // happened.
    int circleErrors = 0;
    const int sink = HE::Log::addSink([](const HE::Log::Record& r, void* user)
    {
        if (r.level >= HE::Log::Level::Error && r.message &&
            std::string(r.message).find("embeds itself") != std::string::npos)
            ++*static_cast<int*>(user);
    }, &circleErrors);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    HE::Log::removeSink(sink);
    REQUIRE(id != 0);
    CHECK(circleErrors == 0);

    // …and all three really are there, which is the half the old code also got
    // right and which must not regress while fixing the half it did not.
    CHECK(wm.processPointer(400.0f, 400.0f, 100.0f,  30.0f, true, true));
    CHECK(wm.processPointer(400.0f, 400.0f, 100.0f, 130.0f, true, true));
    CHECK(wm.processPointer(400.0f, 400.0f, 100.0f, 230.0f, true, true));
}

// …and one component embedding another, twice over, still is not. A page with
// two Cards where a Card holds a Section Header is the ordinary shape of the
// component library, and it exercises the chain two levels down.
TEST_CASE("WidgetRef: a component that embeds a component, used twice")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    registerWidgetAs(cm, "mem://leaf.hasset", rowWidget());

    // The middle widget: a page-sized panel holding one leaf.
    HE::UIWidgetTree mid;
    mid.canvasWidth = 200.0f; mid.canvasHeight = 60.0f;
    mid.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    { const int r = mid.add(HE::UIWidgetType::WidgetRef);
      HE::UIElement& e = *mid.find(r);
      HE::uiSetAnchorPreset(e, HE::kUIAnchorFill);
      HE::uiSetAnchorInsetsX(e, 0.0f, 0.0f);
      HE::uiSetAnchorInsetsY(e, 0.0f, 0.0f);
      e.setProp("Widget", HE::UIPropValue::ofString("mem://leaf.hasset")); }
    registerWidgetAs(cm, "mem://mid.hasset", mid);

    HE::UIWidgetTree page;
    page.canvasWidth = 400.0f; page.canvasHeight = 400.0f;
    page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    for (int i = 0; i < 2; ++i)
    {
        const int ref = page.add(HE::UIWidgetType::WidgetRef);
        HE::UIElement& e = *page.find(ref);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posY = static_cast<float>(i) * 100.0f;
        e.sizeX = 200.0f; e.sizeY = 60.0f;
        e.setProp("Widget", HE::UIPropValue::ofString("mem://mid.hasset"));
    }
    registerWidget(cm, page);

    int circleErrors = 0;
    const int sink = HE::Log::addSink([](const HE::Log::Record& r, void* user)
    {
        if (r.level >= HE::Log::Level::Error && r.message &&
            std::string(r.message).find("embeds itself") != std::string::npos)
            ++*static_cast<int*>(user);
    }, &circleErrors);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    HE::Log::removeSink(sink);
    REQUIRE(id != 0);
    CHECK(circleErrors == 0);

    // FOUR grafts: two mids, and the leaf each of them brings. A chain that
    // described "what is open" instead of "what am I inside of" refused one of
    // them and left its page a row short.
    const HE::UIWidgetTree* t = wm.tree(id);
    REQUIRE(t);
    int refs = 0, embedded = 0;
    for (const auto& ep : t->elements)
        if (const auto* r = dynamic_cast<const HE::UIWidgetRef*>(ep.get()))
        { ++refs; if (r->embedded) ++embedded; }
    CHECK(refs == 4);
    CHECK(embedded == 4);
}

TEST_CASE("WidgetRef: nesting works and a circle is refused")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    // inner ← middle ← page, three levels deep.
    registerWidgetAs(cm, "mem://inner.hasset", rowWidget());
    HE::UIWidgetTree middle;
    middle.canvasWidth = 200.0f; middle.canvasHeight = 100.0f;
    { const int r = middle.add(HE::UIWidgetType::WidgetRef);
      HE::UIElement& e = *middle.find(r);
      HE::uiSetAnchorPreset(e, HE::kUIAnchorFill);
      HE::uiSetAnchorInsetsX(e, 0.0f, 0.0f);
      HE::uiSetAnchorInsetsY(e, 0.0f, 0.0f);
      e.setProp("Widget", HE::UIPropValue::ofString("mem://inner.hasset")); }
    registerWidgetAs(cm, "mem://middle.hasset", middle);

    HE::UIWidgetTree page;
    page.canvasWidth = 400.0f; page.canvasHeight = 400.0f;
    page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    { const int r = page.add(HE::UIWidgetType::WidgetRef);
      HE::UIElement& e = *page.find(r);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 100.0f;
      e.setProp("Widget", HE::UIPropValue::ofString("mem://middle.hasset")); }
    registerWidget(cm, page);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    // The button three levels down arrived and fills the outermost slot.
    CHECK(wm.processPointer(400.0f, 400.0f, 100.0f, 50.0f, true, true));

    // A widget that embeds ITSELF is refused instead of expanding forever.
    HE::UIWidgetTree loop;
    loop.canvasWidth = 100.0f; loop.canvasHeight = 100.0f;
    { const int r = loop.add(HE::UIWidgetType::WidgetRef);
      loop.find(r)->setProp("Widget", HE::UIPropValue::ofString("mem://loop.hasset")); }
    registerWidgetAs(cm, "mem://loop.hasset", loop);
    WidgetManager wm2;
    const int loopId = wm2.createWidget(cm, "mem://loop.hasset");
    CHECK(loopId != 0);        // it still exists…
    std::vector<UIRenderObject> out;
    wm2.extract(400.0f, 400.0f, out);
    CHECK(out.empty());        // …it just brought nothing in
}

TEST_CASE("WidgetRef: a missing or empty reference is survivable")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree page;
    page.canvasWidth = 400.0f; page.canvasHeight = 400.0f;
    const int empty = page.add(HE::UIWidgetType::WidgetRef);   // no path at all
    const int gone  = page.add(HE::UIWidgetType::WidgetRef);
    page.find(gone)->setProp("Widget", HE::UIPropValue::ofString("mem://nope.hasset"));
    (void)empty;
    registerWidget(cm, page);

    WidgetManager wm;
    CHECK(createShown(wm, cm, "mem://w.hasset") != 0);
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);
    CHECK(out.empty());
}

TEST_CASE("WidgetRef: each copy's logic runs on its own copy")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    // A row that reacts to itself: clicking its button writes "OK" into its own
    // text. Two copies of it must not write into each other.
    HE::UIWidgetTree row;
    row.canvasWidth = 200.0f; row.canvasHeight = 100.0f;
    const int txt = row.add(HE::UIWidgetType::Text);
    { HE::UIElement& e = *row.find(txt);
      e.setProp("Text", HE::UIPropValue::ofString(""));
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 50.0f; }
    const int btn = row.add(HE::UIWidgetType::Button);
    { HE::UIElement& e = *row.find(btn);
      e.setProp("Text", HE::UIPropValue::ofString(""));
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 0.0f; e.posY = 50.0f; e.sizeX = 200.0f; e.sizeY = 50.0f; }

    HorizonCode::Graph g;
    HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "OnClicked"; ev.elem = btn;
    const int evId = g.addNode(ev);
    HorizonCode::Node lit; lit.type = NodeType::ConstString; lit.s = "OK";
    const int litId = g.addNode(lit);
    HorizonCode::Node set; set.type = NodeType::SetProperty; set.elem = txt;
    set.s = "Text"; set.propType = PinType::String;
    const int setId = g.addNode(set);
    REQUIRE(g.connect(evId, 0, setId, 0));
    REQUIRE(g.connect(litId, 0, setId, 2));
    registerWidgetAs(cm, "mem://row.hasset", row, &g);

    // Two of them, stacked. The page itself has no logic at all.
    HE::UIWidgetTree page;
    page.canvasWidth = 400.0f; page.canvasHeight = 400.0f;
    page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    for (int i = 0; i < 2; ++i)
    {
        const int r = page.add(HE::UIWidgetType::WidgetRef);
        HE::UIElement& e = *page.find(r);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = static_cast<float>(i) * 100.0f;
        e.sizeX = 200.0f; e.sizeY = 100.0f;
        e.setProp("Widget", HE::UIPropValue::ofString("mem://row.hasset"));
    }
    registerWidget(cm, page);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);

    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);
    CHECK(countGlyphs(out) == 0);        // both texts still empty

    // Click the FIRST row's button (y 50..100 of the first slot).
    CHECK(wm.processPointer(400.0f, 400.0f, 100.0f, 75.0f, true,  true));
    CHECK(wm.processPointer(400.0f, 400.0f, 100.0f, 75.0f, false, true));
    out.clear();
    wm.extract(400.0f, 400.0f, out);
    // Exactly one "OK" — the embedded graph wrote into ITS copy's text and not
    // into the other one's, which is the whole point of a script instance per
    // embed.
    CHECK(countGlyphs(out) == 2);

    // Now the second row: two "OK"s in total, not three.
    CHECK(wm.processPointer(400.0f, 400.0f, 100.0f, 175.0f, true,  true));
    CHECK(wm.processPointer(400.0f, 400.0f, 100.0f, 175.0f, false, true));
    out.clear();
    wm.extract(400.0f, 400.0f, out);
    CHECK(countGlyphs(out) == 4);
}

TEST_CASE("WidgetRef: the type round-trips and keeps its path")
{
    HE::UIWidgetTree t;
    const int r = t.add(HE::UIWidgetType::WidgetRef);
    t.find(r)->setProp("Widget", HE::UIPropValue::ofString("UI/HealthBar.hasset"));

    HE::UIWidgetTree back;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), back));
    REQUIRE(back.find(r) != nullptr);
    CHECK(back.find(r)->type() == HE::UIWidgetType::WidgetRef);
    CHECK(back.find(r)->getProp("Widget").s == "UI/HealthBar.hasset");
    // It draws nothing of its own — what shows up is the embedded tree.
    std::vector<UIRenderObject> out;
    back.find(r)->render({ 0,0,10,10 }, {}, HE::UUID{}, 1.0f, out);
    CHECK(out.empty());
}

// ═══ Keyboard / gamepad navigation ═══════════════════════════════════════════
// A menu has to be usable without a mouse. The focus moves SPATIALLY — the
// nearest interactive element in the pressed direction — so a grid of buttons
// navigates the way it looks instead of in the order it was authored in.

namespace
{
    // Four buttons at the corners of a 400x400 canvas, ids returned in the
    // order top-left, top-right, bottom-left, bottom-right.
    std::array<int, 4> cornerButtons(HE::UIWidgetTree& t)
    {
        t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
        t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
        const float xs[4] = { 20.0f, 250.0f, 20.0f, 250.0f };
        const float ys[4] = { 20.0f,  20.0f, 250.0f, 250.0f };
        std::array<int, 4> ids{};
        for (int i = 0; i < 4; ++i)
        {
            const int b = t.add(HE::UIWidgetType::Button);
            HE::UIElement& e = *t.find(b);
            e.setProp("Text", HE::UIPropValue::ofString(""));
            HE::uiSetAnchorPreset(e, 0);
            e.pivotX = e.pivotY = 0.0f;
            e.posX = xs[i]; e.posY = ys[i];
            e.sizeX = 120.0f; e.sizeY = 60.0f;
            ids[i] = b;
        }
        return ids;
    }
}

TEST_CASE("Navigation: the focus goes where the direction points")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    const std::array<int, 4> b = cornerButtons(t);
    registerWidget(cm, t);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    using Nav = WidgetManager::NavDir;

    // Nothing focused yet: the first press takes the top-left one.
    CHECK(wm.focusedElement() == 0);
    CHECK(wm.navigate(Nav::Down, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == b[0]);

    CHECK(wm.navigate(Nav::Right, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == b[1]);
    CHECK(wm.navigate(Nav::Down, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == b[3]);
    CHECK(wm.navigate(Nav::Left, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == b[2]);
    CHECK(wm.navigate(Nav::Up, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == b[0]);

    // At the edge there is nothing in that direction: the key is NOT consumed,
    // which is how a caller knows it may still act on it.
    CHECK_FALSE(wm.navigate(Nav::Up, 400.0f, 400.0f));
    CHECK_FALSE(wm.navigate(Nav::Left, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == b[0]);
}

TEST_CASE("Navigation: disabled, hidden and clipped-away elements are skipped")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    const std::array<int, 4> b = cornerButtons(t);
    // The one to the right is disabled; the far one below it is hidden.
    t.find(b[1])->enabled = false;
    t.find(b[3])->visible = false;
    registerWidget(cm, t);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    using Nav = WidgetManager::NavDir;

    REQUIRE(wm.navigate(Nav::Down, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == b[0]);
    // Right is disabled → nothing to go to.
    CHECK_FALSE(wm.navigate(Nav::Right, 400.0f, 400.0f));
    // Down still works: b[2] is neither disabled nor hidden.
    CHECK(wm.navigate(Nav::Down, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == b[2]);
    // …and from there, right would be the hidden one.
    CHECK_FALSE(wm.navigate(Nav::Right, 400.0f, 400.0f));
}

TEST_CASE("Navigation: activating does what a click does")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int cb = t.add(HE::UIWidgetType::CheckBox);
    { HE::UIElement& e = *t.find(cb);
      e.setProp("Label", HE::UIPropValue::ofString(""));
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 40.0f; }
    registerWidget(cm, t);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);

    // Nothing focused → nothing happens.
    CHECK_FALSE(wm.activateFocused());
    REQUIRE(wm.navigate(WidgetManager::NavDir::Down, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == cb);
    // The checkbox toggles, exactly as a click would toggle it.
    CHECK(wm.activateFocused());
    CHECK(wm.activateFocused());
}

// ═══ Animation ═══════════════════════════════════════════════════════════════
// docs/he-apps-plan.md B8. An interface without transitions reads as cheap, and
// not because it is undecorated: a value that JUMPS makes the eye ask what
// happened, a value that moves answers it on the way.

TEST_CASE("Easing: the curve names are pinned, and every curve starts and ends where it must")
{
    // ╔══════════════════════════════════════════════════════════════════════╗
    // ║  A graph node stores the easing by NAME. Renaming one here turns     ║
    // ║  every animation that used it into Linear, silently.                 ║
    // ╚══════════════════════════════════════════════════════════════════════╝
    const std::array<const char*, 8> kNames = {
        "Linear", "In Quad", "Out Quad", "In Out Quad",
        "In Cubic", "Out Cubic", "In Out Cubic", "Out Back",
    };
    REQUIRE(static_cast<int>(HE::UIEase::COUNT) == static_cast<int>(kNames.size()));
    for (int i = 0; i < static_cast<int>(kNames.size()); ++i)
    {
        const auto e = static_cast<HE::UIEase>(i);
        CAPTURE(i);
        CHECK(std::string(HE::uiEaseName(e)) == kNames[i]);
        CHECK(HE::uiEaseFromName(kNames[i]) == e);
        // Both ends are exact, whatever the curve does between them: an
        // animation that starts at 0.001 of the way there has already jumped.
        CHECK(HE::uiEaseApply(e, 0.0f) == doctest::Approx(0.0f));
        CHECK(HE::uiEaseApply(e, 1.0f) == doctest::Approx(1.0f));
        // Out of range is clamped on the way IN, so a caller cannot drive a
        // curve past its own end by handing it a t that ran over.
        CHECK(HE::uiEaseApply(e, -1.0f) == doctest::Approx(0.0f));
        CHECK(HE::uiEaseApply(e, 2.0f) == doctest::Approx(1.0f));
    }
    // A name that no longer resolves is Linear, not nothing: an animation with
    // a misspelled curve should still play.
    CHECK(HE::uiEaseFromName("Ease In Out Sine") == HE::UIEase::Linear);
    CHECK(HE::uiEaseFromName("") == HE::UIEase::Linear);

    // The curves differ where it matters — halfway. An "eased" set that is
    // eight names for a straight line is the failure this catches.
    CHECK(HE::uiEaseApply(HE::UIEase::Linear,  0.5f) == doctest::Approx(0.5f));
    CHECK(HE::uiEaseApply(HE::UIEase::InQuad,  0.5f) == doctest::Approx(0.25f));
    CHECK(HE::uiEaseApply(HE::UIEase::OutQuad, 0.5f) == doctest::Approx(0.75f));
    CHECK(HE::uiEaseApply(HE::UIEase::InOutQuad, 0.5f) == doctest::Approx(0.5f));
    // Out Back overshoots on purpose — that is what makes a dialog LAND.
    CHECK(HE::uiEaseApply(HE::UIEase::OutBack, 0.7f) > 1.0f);
}

TEST_CASE("Animation: a property moves along its curve and reports when it lands")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int panel = t.add(HE::UIWidgetType::Panel);
    {
        HE::UIElement& e = *t.find(panel);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 100.0f; e.sizeY = 100.0f;
        e.renderOpacity = 0.0f;
    }
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    auto opacity = [&]{ return wm.tree(id)->find(panel)->renderOpacity; };

    CHECK_FALSE(wm.isAnimating());
    REQUIRE(wm.animate(id, panel, "Render Opacity", HE::UIPropValue::ofFloat(1.0f),
                       1.0f, HE::UIEase::Linear));
    CHECK(wm.isAnimating());
    // Nothing has moved before the first tick: starting an animation is not
    // the same as playing one frame of it.
    CHECK(opacity() == doctest::Approx(0.0f));

    wm.tick(0.25f);
    CHECK(opacity() == doctest::Approx(0.25f));
    wm.tick(0.25f);
    CHECK(opacity() == doctest::Approx(0.5f));

    // …and the whole time it is running, the picture is changing, which an
    // event-driven application only finds out because the flag says so.
    wm.consumeVisualDirty();
    wm.tick(0.1f);
    CHECK(wm.consumeVisualDirty());

    wm.tick(1.0f);                      // past the end
    CHECK(opacity() == doctest::Approx(1.0f));
    CHECK_FALSE(wm.isAnimating());
    // Ticking after the end does nothing at all — no drift past the target.
    wm.tick(1.0f);
    CHECK(opacity() == doctest::Approx(1.0f));

}

TEST_CASE("Animation: colours, positions, and what cannot be animated at all")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int panel = t.add(HE::UIWidgetType::Panel);
    const int text  = t.add(HE::UIWidgetType::Text);
    {
        HE::UIElement& e = *t.find(panel);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 100.0f; e.sizeY = 100.0f;
        e.setProp("Color", HE::UIPropValue::ofColor({ 0.0f, 0.0f, 0.0f, 1.0f }));
    }
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);

    // A colour, both ends of it.
    REQUIRE(wm.animate(id, panel, "Color", HE::UIPropValue::ofColor({1.0f, 0.0f, 0.0f, 1.0f}),
                       1.0f, HE::UIEase::Linear));
    wm.tick(0.5f);
    CHECK(wm.tree(id)->find(panel)->getPropAny("Color").col.r == doctest::Approx(0.5f));

    // A position — a Vec2, which is the type "slide it in" needs and the reason
    // three kinds are interpolated instead of one.
    REQUIRE(wm.animate(id, panel, "Position", HE::UIPropValue::ofVec2({ 200.0f, 100.0f }),
                       1.0f, HE::UIEase::Linear));
    wm.tick(0.5f);
    CHECK(wm.tree(id)->find(panel)->posX == doctest::Approx(100.0f));
    CHECK(wm.tree(id)->find(panel)->posY == doctest::Approx(50.0f));

    // What has no halfway is refused rather than snapped at the end: a string,
    // a bool, and a property this element does not have at all.
    CHECK_FALSE(wm.animate(id, text, "Text", HE::UIPropValue::ofString("x"), 1.0f));
    CHECK_FALSE(wm.animate(id, text, "WordWrap", HE::UIPropValue::ofBool(true), 1.0f));
    CHECK_FALSE(wm.animate(id, text, "Normal Color",
                           HE::UIPropValue::ofColor({1,1,1,1}), 1.0f));   // a Button's
    // A target of the wrong type for a real property is refused too — that is
    // the same check, and it is what catches "fade the Text" (a String).
    CHECK_FALSE(wm.animate(id, panel, "Color", HE::UIPropValue::ofFloat(1.0f), 1.0f));
    CHECK_FALSE(wm.animate(id, 999, "Color", HE::UIPropValue::ofColor({1,1,1,1}), 1.0f));
    CHECK_FALSE(wm.animate(0, panel, "Color", HE::UIPropValue::ofColor({1,1,1,1}), 1.0f));
}

// "Fade it out, then hide it" is the first thing anybody builds with this, and
// it needs an end that can be reacted to. The payload is the PROPERTY, because
// an element being faded AND slid ends twice and a graph has to tell them apart.
TEST_CASE("Animation: the end fires an event, and a cancelled one does not")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int panel = t.add(HE::UIWidgetType::Panel);
    {
        HE::UIElement& e = *t.find(panel);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 100.0f; e.sizeY = 100.0f;
        e.renderOpacity = 0.0f;
    }
    const int txt = t.add(HE::UIWidgetType::Text);
    t.find(txt)->setProp("Text", HE::UIPropValue::ofString(""));

    // OnAnimationFinished on the panel writes the property's name into the Text,
    // so the test can read BOTH that it fired and what it said.
    HorizonCode::Graph g;
    HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "OnAnimationFinished"; ev.elem = panel;
    // The event carries the property's name, and a node only grows the pin
    // for it when it says so — the editor sets these from the event table.
    ev.hasArg = true; ev.propType = PinType::String;
    const int evId = g.addNode(ev);
    HorizonCode::Node set; set.type = NodeType::SetProperty; set.elem = txt;
    set.s = "Text"; set.propType = PinType::String;
    const int setId = g.addNode(set);
    REQUIRE(g.connect(evId, 0, setId, 0));   // exec → exec
    REQUIRE(g.connect(evId, 1, setId, 2));   // the event's String arg → the value
    registerWidget(cm, t, &g);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    auto said = [&]{ return wm.tree(id)->find(txt)->getPropAny("Text").s; };

    REQUIRE(wm.animate(id, panel, "Render Opacity", HE::UIPropValue::ofFloat(1.0f), 1.0f));
    wm.tick(0.5f);
    CHECK(said().empty());               // not there yet
    wm.tick(0.6f);
    CHECK(said() == "Render Opacity");   // …and it names which one ended

    // A cancelled one reports nothing. Cancelled is not finished, and a graph
    // that hides the dialog "when the fade ends" must not hide it because
    // somebody interrupted the fade. Asked by "did anything ELSE arrive": the
    // Text is written by the event itself and the test cannot clear it, so the
    // question is whether a SECOND report ever lands.
    REQUIRE(wm.animate(id, panel, "Color", HE::UIPropValue::ofColor({1,0,0,1}), 1.0f));
    wm.tick(0.5f);
    CHECK(wm.stopAnimations(id, panel, "Color") == 1);
    wm.tick(1.0f);
    CHECK(said() == "Render Opacity");   // still the FIRST one's name

    // Replacing one mid-flight is a cancel too, and only the REPLACEMENT's end
    // is announced — once, not twice.
    REQUIRE(wm.animate(id, panel, "Color", HE::UIPropValue::ofColor({0,1,0,1}), 1.0f));
    wm.tick(0.5f);
    REQUIRE(wm.animate(id, panel, "Color", HE::UIPropValue::ofColor({0,0,1,1}), 1.0f));
    CHECK(said() == "Render Opacity");   // the replaced one said nothing
    wm.tick(1.5f);
    CHECK(said() == "Color");            // the replacement did

    // Zero seconds still announces: a duration turned down to nothing is still
    // an animation that ended, and a graph waiting on it must not hang.
    REQUIRE(wm.animate(id, panel, "Corner Radius", HE::UIPropValue::ofFloat(8.0f), 0.0f));
    CHECK(said() == "Corner Radius");
}

TEST_CASE("Animation: retargeting is smooth, stopping is not a rewind, zero is instant")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int panel = t.add(HE::UIWidgetType::Panel);
    {
        HE::UIElement& e = *t.find(panel);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 100.0f; e.sizeY = 100.0f;
        e.renderOpacity = 0.0f;
    }
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    auto opacity = [&]{ return wm.tree(id)->find(panel)->renderOpacity; };

    // Halfway to 1, then told to go to 0 instead: the new one starts from 0.5,
    // where the value actually IS, not from where the first one began.
    REQUIRE(wm.animate(id, panel, "Render Opacity", HE::UIPropValue::ofFloat(1.0f), 1.0f));
    wm.tick(0.5f);
    CHECK(opacity() == doctest::Approx(0.5f));
    REQUIRE(wm.animate(id, panel, "Render Opacity", HE::UIPropValue::ofFloat(0.0f), 1.0f));
    // Only ONE is running — asked BEFORE the next tick, because a tick would
    // retire the stacked leftover and the count would look right either way.
    // A retarget replaces; it does not stack a second writer on the property.
    CHECK(wm.stopAnimations(id, panel, "Render Opacity") == 1);
    REQUIRE(wm.animate(id, panel, "Render Opacity", HE::UIPropValue::ofFloat(0.0f), 1.0f));
    wm.tick(0.5f);
    CHECK(opacity() == doctest::Approx(0.25f));   // half of the way from 0.5 to 0
    CHECK(wm.stopAnimations(id, panel, "Render Opacity") == 1);
    CHECK_FALSE(wm.isAnimating());
    // A stop leaves the value where it got to. Rewinding would undo a fade
    // somebody interrupted on purpose.
    CHECK(opacity() == doctest::Approx(0.25f));

    // Zero seconds writes at once — a duration is a knob you can turn to zero.
    REQUIRE(wm.animate(id, panel, "Render Opacity", HE::UIPropValue::ofFloat(1.0f), 0.0f));
    CHECK(opacity() == doctest::Approx(1.0f));
    CHECK_FALSE(wm.isAnimating());

    // Stopping by element and by widget, not only by property.
    REQUIRE(wm.animate(id, panel, "Render Opacity", HE::UIPropValue::ofFloat(0.0f), 1.0f));
    REQUIRE(wm.animate(id, panel, "Color", HE::UIPropValue::ofColor({1,0,0,1}), 1.0f));
    CHECK(wm.stopAnimations(id, panel) == 2);
    REQUIRE(wm.animate(id, panel, "Render Opacity", HE::UIPropValue::ofFloat(0.0f), 1.0f));
    CHECK(wm.stopAnimations(id) == 1);
    CHECK_FALSE(wm.isAnimating());

    // A destroyed widget takes its animations with it — they live on the
    // instance, so nothing has to remember to sweep.
    REQUIRE(wm.animate(id, panel, "Render Opacity", HE::UIPropValue::ofFloat(1.0f), 1.0f));
    CHECK(wm.isAnimating());
    wm.destroyWidget(id);
    CHECK_FALSE(wm.isAnimating());
    wm.tick(1.0f);   // must not touch freed memory
}

// ── Clips: an animation you author instead of one you write ──────────────────

namespace
{
    // A clip with one Float track, the shape almost every test below wants.
    HE::UIAnimClip fadeClip(int elem, const char* name = "FadeIn",
                            float from = 0.0f, float to = 1.0f, float dur = 1.0f)
    {
        HE::UIAnimClip c;
        c.name = name; c.duration = dur;
        HE::UIAnimTrack tr;
        tr.element = elem; tr.prop = "Render Opacity";
        tr.keys.push_back({ 0.0f,   HE::UIPropValue::ofFloat(from), HE::UIEase::Linear });
        tr.keys.push_back({ dur,    HE::UIPropValue::ofFloat(to),   HE::UIEase::Linear });
        c.tracks.push_back(std::move(tr));
        return c;
    }
}

TEST_CASE("Clips: what a timeline says at a moment, including the boring ends")
{
    HE::UIAnimClip c = fadeClip(1);
    std::vector<HE::UIAnimSample> s;

    auto at = [&](float t) { s.clear(); HE::uiAnimEvaluate(c, t, s); REQUIRE(s.size() == 1);
                             return s[0].value.f; };
    CHECK(at(0.0f)  == doctest::Approx(0.0f));
    CHECK(at(0.5f)  == doctest::Approx(0.5f));
    CHECK(at(1.0f)  == doctest::Approx(1.0f));
    // Before the first key and after the last one: HOLD, not the element's own
    // value — scrubbing to 0 and back would otherwise quietly lose it.
    CHECK(at(-1.0f) == doctest::Approx(0.0f));
    CHECK(at(99.0f) == doctest::Approx(1.0f));
    CHECK(s[0].element == 1);
    CHECK(s[0].prop == "Render Opacity");

    // The easing belongs to the key being moved TO: "slow at the end" is a
    // property of the arrival, not of the departure.
    c.tracks[0].keys[1].ease = HE::UIEase::InQuad;
    CHECK(at(0.5f) == doctest::Approx(0.25f));

    // One key is a constant, not a jump at the end.
    c.tracks[0].keys.pop_back();
    CHECK(at(0.0f) == doctest::Approx(0.0f));
    CHECK(at(9.0f) == doctest::Approx(0.0f));

    // Two keys at one time evaluate deterministically — the later in the list
    // wins — because a hand-edited file must not read differently twice.
    c.tracks[0].keys.push_back({ 0.5f, HE::UIPropValue::ofFloat(0.2f), HE::UIEase::Linear });
    c.tracks[0].keys.push_back({ 0.5f, HE::UIPropValue::ofFloat(0.8f), HE::UIEase::Linear });
    CHECK(at(0.5f) == doctest::Approx(0.8f));
    CHECK(at(0.6f) == doctest::Approx(0.8f));

    // A track with no keys says nothing at all rather than a default.
    HE::UIAnimClip empty;
    empty.tracks.push_back({ 1, "Render Opacity", {} });
    s.clear(); HE::uiAnimEvaluate(empty, 0.5f, s);
    CHECK(s.empty());

    CHECK(HE::uiAnimFind({ c }, "FadeIn") != nullptr);
    CHECK(HE::uiAnimFind({ c }, "fadein") == nullptr);   // names are exact
}

TEST_CASE("Clips: they survive a save, and a widget without any saves as before")
{
    HE::UIWidgetTree t;
    const int panel = t.add(HE::UIWidgetType::Panel);
    CHECK(HE::uiWidgetTreeToJson(t).find("animations") == std::string::npos);

    HE::UIAnimClip c = fadeClip(panel, "FadeIn", 0.0f, 1.0f, 2.5f);
    c.loop = true;
    c.tracks[0].keys[1].ease = HE::UIEase::OutBack;
    // A second track, on a colour, so the round-trip covers more than one type.
    HE::UIAnimTrack col;
    col.element = panel; col.prop = "Color";
    col.keys.push_back({ 0.0f, HE::UIPropValue::ofColor({1,0,0,1}), HE::UIEase::Linear });
    col.keys.push_back({ 2.5f, HE::UIPropValue::ofColor({0,0,1,1}), HE::UIEase::OutQuad });
    c.tracks.push_back(std::move(col));
    t.animations.push_back(std::move(c));

    HE::UIWidgetTree back;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), back));
    REQUIRE(back.animations.size() == 1);
    const HE::UIAnimClip& r = back.animations[0];
    CHECK(r.name == "FadeIn");
    CHECK(r.duration == doctest::Approx(2.5f));
    CHECK(r.loop);
    REQUIRE(r.tracks.size() == 2);
    CHECK(r.tracks[0].element == panel);
    CHECK(r.tracks[0].keys[1].ease == HE::UIEase::OutBack);
    REQUIRE(r.tracks[1].keys.size() == 2);
    CHECK(r.tracks[1].keys[1].value.col.b == doctest::Approx(1.0f));
    CHECK(r.tracks[1].keys[1].ease == HE::UIEase::OutQuad);

    // Junk is dropped rather than loaded as something that cannot be played: a
    // clip with no name has no identity, one with no duration has no timeline,
    // and a track with no keys is a row the editor could not evaluate.
    HE::UIWidgetTree junk;
    REQUIRE(HE::uiWidgetTreeFromJson(
        R"({"canvasWidth":100,"canvasHeight":100,"elements":[],"animations":[
            {"name":"","duration":1},
            {"name":"Zero","duration":0},
            {"name":"Empty","duration":1,"tracks":[{"elem":1,"prop":"X","keys":[]}]}]})",
        junk));
    REQUIRE(junk.animations.size() == 1);
    CHECK(junk.animations[0].name == "Empty");
    CHECK(junk.animations[0].tracks.empty());
}

TEST_CASE("Clips: playing one moves the widget, and looping never ends")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int panel = t.add(HE::UIWidgetType::Panel);
    {
        HE::UIElement& e = *t.find(panel);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 100.0f; e.sizeY = 100.0f;
        e.renderOpacity = 0.5f;      // deliberately NOT where the clip starts
    }
    t.animations.push_back(fadeClip(panel));
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    auto opacity = [&]{ return wm.tree(id)->find(panel)->renderOpacity; };

    CHECK_FALSE(wm.isPlayingAnimation(id));
    CHECK_FALSE(wm.playAnimation(id, "Nope"));
    REQUIRE(wm.playAnimation(id, "FadeIn"));
    CHECK(wm.isPlayingAnimation(id, "FadeIn"));
    CHECK(wm.isAnimating());              // the frame pump has to keep running
    CHECK(opacity() == doctest::Approx(0.5f));   // starting is not playing a frame

    wm.tick(0.25f);
    CHECK(opacity() == doctest::Approx(0.25f));
    CHECK(wm.animationTime(id, "FadeIn") == doctest::Approx(0.25f));
    wm.tick(0.5f);
    CHECK(opacity() == doctest::Approx(0.75f));

    wm.tick(0.5f);                        // past the end
    CHECK(opacity() == doctest::Approx(1.0f));
    CHECK_FALSE(wm.isPlayingAnimation(id, "FadeIn"));
    CHECK(wm.animationTime(id, "FadeIn") == doctest::Approx(-1.0f));

    // Playing it again rewinds rather than stacking a second player.
    REQUIRE(wm.playAnimation(id, "FadeIn"));
    wm.tick(0.5f);
    REQUIRE(wm.playAnimation(id, "FadeIn"));
    CHECK(wm.animationTime(id, "FadeIn") == doctest::Approx(0.0f));
    CHECK(wm.stopAnimationClip(id, "FadeIn") == 1);
    CHECK_FALSE(wm.isAnimating());

    // A looping clip wraps instead of ending, and keeps the pump alive.
    const bool loop = true;
    REQUIRE(wm.playAnimation(id, "FadeIn", &loop));
    wm.tick(1.25f);
    CHECK(wm.animationTime(id, "FadeIn") == doctest::Approx(0.25f));   // wrapped
    CHECK(wm.isPlayingAnimation(id, "FadeIn"));
    wm.tick(10.0f);
    CHECK(wm.isPlayingAnimation(id, "FadeIn"));
    CHECK(wm.stopAnimationClip(id) == 1);
}

// An animation is over when its last key is, not when its length is. The tail
// after it is a wait with nothing in it: nothing moves there, and a clip that
// reported finished a second late would make every graph waiting on it late.
TEST_CASE("Clips: playing stops at the last key, not at the authored length")
{
    HE::UIAnimClip c = fadeClip(1, "FadeIn", 0.0f, 1.0f, /*dur=*/0.25f);
    c.duration = 4.0f;                       // room to work in, not what plays
    CHECK(HE::uiAnimPlayEnd(c) == doctest::Approx(0.25f));

    // A second track pushes the end out; the LAST key of any track decides.
    HE::UIAnimTrack tr;
    tr.element = 1; tr.prop = "Render Opacity";
    tr.keys.push_back({ 1.5f, HE::UIPropValue::ofFloat(0.0f), HE::UIEase::Linear });
    c.tracks.push_back(tr);
    CHECK(HE::uiAnimPlayEnd(c) == doctest::Approx(1.5f));

    // A key hidden past the length cannot stretch playback past it — hiding is
    // what shortening a clip means.
    c.duration = 1.0f;
    CHECK(HE::uiAnimPlayEnd(c) == doctest::Approx(1.0f));

    // Nothing keyed at all ends at zero, so playing it is a no-op that reports
    // finished at once rather than an empty wait of `duration` seconds.
    HE::UIAnimClip empty;
    empty.name = "Empty"; empty.duration = 3.0f;
    CHECK(HE::uiAnimPlayEnd(empty) == doctest::Approx(0.0f));
}

TEST_CASE("Clips: the runtime ends one at its last key, and loops there too")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int panel = t.add(HE::UIWidgetType::Panel);
    {
        HE::UIElement& e = *t.find(panel);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = e.posY = 0.0f; e.sizeX = e.sizeY = 100.0f;
    }
    // Keyed over a quarter second, inside a clip four seconds long.
    HE::UIAnimClip c = fadeClip(panel, "FadeIn", 0.0f, 1.0f, 0.25f);
    c.duration = 4.0f;
    t.animations.push_back(c);
    // A clip with no keys at all, to check that playing one is not a four
    // second silence with a pump running behind it.
    HE::UIAnimClip hollow; hollow.name = "Hollow"; hollow.duration = 4.0f;
    t.animations.push_back(hollow);
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    auto opacity = [&]{ return wm.tree(id)->find(panel)->renderOpacity; };

    REQUIRE(wm.playAnimation(id, "FadeIn"));
    wm.tick(0.125f);
    CHECK(opacity() == doctest::Approx(0.5f));
    CHECK(wm.isPlayingAnimation(id, "FadeIn"));

    // One more tick past the last key: over, at the last key's value. Under the
    // old rule this widget would have gone on "playing" for another 3.6 s.
    wm.tick(0.2f);
    CHECK(opacity() == doctest::Approx(1.0f));
    CHECK_FALSE(wm.isPlayingAnimation(id, "FadeIn"));
    CHECK_FALSE(wm.isAnimating());

    // Looping wraps at the last key as well, so a pulse authored in 250 ms
    // pulses four times a second instead of once every four.
    const bool loop = true;
    REQUIRE(wm.playAnimation(id, "FadeIn", &loop));
    wm.tick(0.3f);
    CHECK(wm.animationTime(id, "FadeIn") == doctest::Approx(0.05f));
    CHECK(wm.isPlayingAnimation(id, "FadeIn"));
    CHECK(wm.stopAnimationClip(id, "FadeIn") == 1);

    // And a clip with nothing in it finishes on the first tick, looping or not:
    // there is nothing to loop, and a widget looping over nothing would ask for
    // frames forever.
    REQUIRE(wm.playAnimation(id, "Hollow", &loop));
    wm.tick(0.016f);
    CHECK_FALSE(wm.isPlayingAnimation(id, "Hollow"));
    CHECK_FALSE(wm.isAnimating());
}

// The stored names, pinned like the easing ones: a graph node keeps the
// direction by NAME, so renaming one silently turns every animation that used
// it into Forward.
TEST_CASE("Clips: the play directions are a closed, named vocabulary")
{
    CHECK(std::string(HE::uiAnimDirectionName(HE::UIAnimDirection::Forward)) == "Forward");
    CHECK(std::string(HE::uiAnimDirectionName(HE::UIAnimDirection::Backward)) == "Backward");
    CHECK(std::string(HE::uiAnimDirectionName(HE::UIAnimDirection::PingPong)) == "Ping Pong");
    for (int i = 0; i < (int)HE::UIAnimDirection::COUNT; ++i)
    {
        const auto d = (HE::UIAnimDirection)i;
        CHECK(HE::uiAnimDirectionFromName(HE::uiAnimDirectionName(d)) == d);
    }
    // Unknown plays forwards rather than nothing, like an unknown easing.
    CHECK(HE::uiAnimDirectionFromName("sideways") == HE::UIAnimDirection::Forward);
    CHECK(HE::uiAnimDirectionFromName("") == HE::UIAnimDirection::Forward);

    // One pass: the clip's length, or twice it out and back.
    CHECK(HE::uiAnimPlaySpan(HE::UIAnimDirection::Forward, 0.5f) == doctest::Approx(0.5f));
    CHECK(HE::uiAnimPlaySpan(HE::UIAnimDirection::Backward, 0.5f) == doctest::Approx(0.5f));
    CHECK(HE::uiAnimPlaySpan(HE::UIAnimDirection::PingPong, 0.5f) == doctest::Approx(1.0f));

    // Which moment of the clip a pass is showing.
    auto at = [](HE::UIAnimDirection d, float t){ return HE::uiAnimDirectedTime(d, t, 1.0f); };
    CHECK(at(HE::UIAnimDirection::Forward, 0.25f)  == doctest::Approx(0.25f));
    CHECK(at(HE::UIAnimDirection::Backward, 0.0f)  == doctest::Approx(1.0f));
    CHECK(at(HE::UIAnimDirection::Backward, 0.25f) == doctest::Approx(0.75f));
    CHECK(at(HE::UIAnimDirection::Backward, 1.0f)  == doctest::Approx(0.0f));
    CHECK(at(HE::UIAnimDirection::PingPong, 0.5f)  == doctest::Approx(0.5f));
    CHECK(at(HE::UIAnimDirection::PingPong, 1.0f)  == doctest::Approx(1.0f));
    CHECK(at(HE::UIAnimDirection::PingPong, 1.5f)  == doctest::Approx(0.5f));
    CHECK(at(HE::UIAnimDirection::PingPong, 2.0f)  == doctest::Approx(0.0f));
    // Past the pass, and before it: clamped into the clip, never outside it.
    CHECK(at(HE::UIAnimDirection::Forward, 99.0f)  == doctest::Approx(1.0f));
    CHECK(at(HE::UIAnimDirection::Backward, -9.0f) == doctest::Approx(1.0f));
}

TEST_CASE("Clips: backwards, out and back, and putting it back afterwards")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int panel = t.add(HE::UIWidgetType::Panel);
    {
        HE::UIElement& e = *t.find(panel);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = e.posY = 0.0f; e.sizeX = e.sizeY = 100.0f;
        e.renderOpacity = 0.3f;      // the authored value, and what a restore owes
    }
    t.animations.push_back(fadeClip(panel, "FadeIn", 0.0f, 1.0f, 1.0f));
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    auto opacity = [&]{ return wm.tree(id)->find(panel)->renderOpacity; };

    // Backwards: the clip read from its end towards zero.
    REQUIRE(wm.playAnimation(id, "FadeIn", nullptr, HE::UIAnimDirection::Backward));
    wm.tick(0.25f);
    CHECK(opacity() == doctest::Approx(0.75f));
    wm.tick(0.75f);
    CHECK(opacity() == doctest::Approx(0.0f));
    CHECK_FALSE(wm.isPlayingAnimation(id, "FadeIn"));

    // Out and back: twice as long, and it ends where it started.
    REQUIRE(wm.playAnimation(id, "FadeIn", nullptr, HE::UIAnimDirection::PingPong));
    wm.tick(1.0f);
    CHECK(opacity() == doctest::Approx(1.0f));   // the far end
    wm.tick(0.5f);
    CHECK(opacity() == doctest::Approx(0.5f));   // on the way back
    CHECK(wm.isPlayingAnimation(id, "FadeIn"));
    wm.tick(0.5f);
    CHECK(opacity() == doctest::Approx(0.0f));
    CHECK_FALSE(wm.isPlayingAnimation(id, "FadeIn"));

    // Restore After Completed: back to 0.3, the value the widget was AUTHORED
    // with — not the 0.0 the last two plays left behind. That is the whole
    // difference between "the way it was" and "the way it was a moment ago".
    REQUIRE(wm.playAnimation(id, "FadeIn", nullptr, HE::UIAnimDirection::Forward,
                             /*restore=*/true));
    wm.tick(0.5f);
    CHECK(opacity() == doctest::Approx(0.5f));
    wm.tick(0.6f);
    CHECK(opacity() == doctest::Approx(0.3f));
    CHECK_FALSE(wm.isPlayingAnimation(id, "FadeIn"));

    // A clip STOPPED by hand did not finish, so it does not restore: that is
    // what the explicit Restore Original State node is for.
    REQUIRE(wm.playAnimation(id, "FadeIn", nullptr, HE::UIAnimDirection::Forward, true));
    wm.tick(0.5f);
    CHECK(wm.stopAnimationClip(id, "FadeIn") == 1);
    CHECK(opacity() == doctest::Approx(0.5f));
    CHECK(wm.restoreOriginalState(id) == 1);
    CHECK(opacity() == doctest::Approx(0.3f));
    // Nothing left to remember once it has been put back.
    CHECK(wm.restoreOriginalState(id) == 0);
}

TEST_CASE("Clips: stop all, and what 'original' means after two animations")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int panel = t.add(HE::UIWidgetType::Panel);
    const int label = t.add(HE::UIWidgetType::Text);
    {
        HE::UIElement& e = *t.find(panel);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = e.posY = 0.0f; e.sizeX = e.sizeY = 100.0f;
        e.renderOpacity = 0.4f;
    }
    t.find(label)->rotation = 10.0f;
    t.animations.push_back(fadeClip(panel, "FadeIn", 0.0f, 1.0f, 1.0f));
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    auto opacity  = [&]{ return wm.tree(id)->find(panel)->renderOpacity; };
    auto rotation = [&]{ return wm.tree(id)->find(label)->rotation; };

    // A clip and a tween on two different properties: Stop All takes both, and
    // does not need to be told what is running.
    REQUIRE(wm.playAnimation(id, "FadeIn"));
    REQUIRE(wm.animate(id, label, "Rotation", HE::UIPropValue::ofFloat(90.0f), 1.0f));
    wm.tick(0.5f);
    CHECK(wm.stopAllAnimations(id) == 2);
    CHECK_FALSE(wm.isAnimating());
    // Stopped, not rewound.
    CHECK(opacity() == doctest::Approx(0.5f));
    CHECK(rotation() > 10.0f);

    // A SECOND animation on a property must not record the first one's
    // mid-flight value as the original — the memory is of the widget, not of
    // the last thing that touched it.
    REQUIRE(wm.animate(id, panel, "Render Opacity", HE::UIPropValue::ofFloat(1.0f), 1.0f));
    wm.tick(0.5f);
    CHECK(wm.restoreOriginalState(id) == 2);
    CHECK(opacity() == doctest::Approx(0.4f));
    CHECK(rotation() == doctest::Approx(10.0f));
}

TEST_CASE("Clips: a clip and a tween never write the same property at once")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int panel = t.add(HE::UIWidgetType::Panel);
    {
        HE::UIElement& e = *t.find(panel);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 100.0f; e.sizeY = 100.0f;
    }
    t.animations.push_back(fadeClip(panel));
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);

    // A tween is running; the clip that drives the same property takes it over.
    REQUIRE(wm.animate(id, panel, "Render Opacity", HE::UIPropValue::ofFloat(1.0f), 10.0f));
    REQUIRE(wm.playAnimation(id, "FadeIn"));
    CHECK(wm.stopAnimations(id, panel, "Render Opacity") == 0);   // the clip took it

    // …and a tween started afterwards wins the property back, while the clip
    // keeps playing its other tracks. Two writers on one property is the fight
    // this rule exists to prevent, whichever side starts second.
    REQUIRE(wm.animate(id, panel, "Render Opacity", HE::UIPropValue::ofFloat(0.0f), 1.0f));
    CHECK(wm.isPlayingAnimation(id, "FadeIn"));
    CHECK(wm.stopAnimations(id, panel, "Render Opacity") == 1);
}

// The loop the whole feature closes: authored in the timeline, played from a
// graph. Through the registry rows a graph actually calls, not the manager
// directly — a row that compiles and does nothing is the failure this catches.
TEST_CASE("Clips: a graph plays one by name through the engine API")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HorizonWorld world;
    WidgetManager wm;
    world.setWidgetManager(&wm);

    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int panel = t.add(HE::UIWidgetType::Panel);
    {
        HE::UIElement& e = *t.find(panel);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 100.0f; e.sizeY = 100.0f;
        e.renderOpacity = 1.0f;
    }
    t.animations.push_back(fadeClip(panel, "FadeIn", 0.0f, 1.0f, 1.0f));
    registerWidget(cm, t);

    const int id = wm.createWidget(cm, "mem://w.hasset");
    REQUIRE(id != 0);
    wm.showWidget(id);

    HE::api::Ctx c;
    c.world = &world;
    c.content = &cm;
    CHECK_FALSE(HE::api::widget::isPlayingAnimation(c, id, "FadeIn"));
    CHECK_FALSE(HE::api::widget::playAnimation(c, id, "Nope"));
    REQUIRE(HE::api::widget::playAnimation(c, id, "FadeIn"));
    CHECK(HE::api::widget::isPlayingAnimation(c, id, "FadeIn"));
    wm.tick(0.5f);
    CHECK(wm.tree(id)->find(panel)->renderOpacity == doctest::Approx(0.5f));
    CHECK(HE::api::widget::stopAnimationClip(c, id, "FadeIn") == 1);
    CHECK_FALSE(HE::api::widget::isPlayingAnimation(c, id, "FadeIn"));

    // The looped row overrides what the clip says, which is the only reason it
    // exists beside the plain one.
    REQUIRE(HE::api::widget::playAnimationLooped(c, id, "FadeIn", true));
    wm.tick(2.5f);
    CHECK(HE::api::widget::isPlayingAnimation(c, id, "FadeIn"));   // still going
    CHECK(HE::api::widget::stopAnimationClip(c, id, "") == 1);

    // Stop All and Restore, through the rows a graph calls. Restore puts the
    // opacity back to the 1.0 the widget was authored with, from wherever the
    // half-played clip left it.
    REQUIRE(HE::api::widget::playAnimation(c, id, "FadeIn"));
    wm.tick(0.5f);
    CHECK(HE::api::widget::stopAllAnimations(c, id) == 1);
    CHECK(wm.tree(id)->find(panel)->renderOpacity == doctest::Approx(0.5f));
    CHECK(HE::api::widget::restoreOriginalState(c, id) == 1);
    CHECK(wm.tree(id)->find(panel)->renderOpacity == doctest::Approx(1.0f));

    // Backwards and "put it back afterwards", the two knobs the Play node has,
    // through the row rather than through the manager.
    REQUIRE(HE::api::widget::playAnimation(c, id, "FadeIn", /*restore=*/true, "Backward"));
    wm.tick(0.25f);
    CHECK(wm.tree(id)->find(panel)->renderOpacity == doctest::Approx(0.75f));
    wm.tick(1.0f);
    CHECK(wm.tree(id)->find(panel)->renderOpacity == doctest::Approx(1.0f));   // restored

    // Nobody wired the Widget pin: with no calling instance either, there is no
    // widget to guess and the call is a no-op rather than a wrong guess.
    CHECK_FALSE(HE::api::widget::playAnimation(c, 0, "FadeIn"));
    CHECK(HE::api::widget::stopAllAnimations(c, 0) == 0);
}

// A component's clips are the component's: its tracks name elements by the ids
// they had in ITS asset, and a graft renumbers everything. Without the offset a
// clip inside an embedded widget animates whatever now happens to have id 1.
TEST_CASE("Clips: an embedded component's animation plays on its own elements")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    // The component: one panel, one clip that fades it.
    HE::UIWidgetTree comp;
    comp.canvasWidth = 100.0f; comp.canvasHeight = 100.0f;
    comp.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int inner = comp.add(HE::UIWidgetType::Panel);
    {
        HE::UIElement& e = *comp.find(inner);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 100.0f; e.sizeY = 100.0f;
        e.renderOpacity = 1.0f;
    }
    comp.animations.push_back(fadeClip(inner, "FadeIn", 1.0f, 0.0f, 1.0f));
    registerWidgetAs(cm, "mem://comp.hasset", comp);

    // The page: something else with id 1, then the component. If the offset were
    // forgotten, the clip would fade THIS panel instead.
    HE::UIWidgetTree page;
    page.canvasWidth = 400.0f; page.canvasHeight = 400.0f;
    page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int decoy = page.add(HE::UIWidgetType::Panel);
    page.find(decoy)->name = "Slot";   // somewhere for addChild to graft into
    {
        HE::UIElement& e = *page.find(decoy);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 200.0f; e.posY = 0.0f; e.sizeX = 100.0f; e.sizeY = 100.0f;
        e.renderOpacity = 1.0f;
    }
    const int refId = page.add(HE::UIWidgetType::WidgetRef);
    {
        auto* r = dynamic_cast<HE::UIWidgetRef*>(page.find(refId));
        r->widgetPath = "mem://comp.hasset";
        HE::uiSetAnchorPreset(*r, 0); r->pivotX = r->pivotY = 0.0f;
        r->posX = 0.0f; r->posY = 0.0f; r->sizeX = 100.0f; r->sizeY = 100.0f;
    }
    registerWidgetAs(cm, "mem://page.hasset", page);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://page.hasset");
    REQUIRE(id != 0);
    REQUIRE(decoy == 1);   // the whole point: local id 1 exists in BOTH trees

    // The page does not own the clip, but the component it embeds does, and
    // playing by name finds it there.
    REQUIRE(wm.playAnimation(id, "FadeIn"));
    wm.tick(0.5f);
    // The component's panel moved…
    const HE::UIElement* grafted = nullptr;
    for (const auto& ep : wm.tree(id)->elements)
        if (ep && ep->id > refId) { grafted = ep.get(); break; }
    REQUIRE(grafted);
    CHECK(grafted->renderOpacity == doctest::Approx(0.5f));
    // …and the page's own panel, which has the id the clip names, did not.
    CHECK(wm.tree(id)->find(decoy)->renderOpacity == doctest::Approx(1.0f));

    // "This widget" for an engine call with the Widget pin left unwired. For an
    // EMBEDDED component's graph that is the page it was grafted into: the page
    // is what owns the elements and the clips its offset points at, so playing
    // its own "FadeIn" from inside the component has to land on the same tree.
    const HorizonCode::InstanceId embedded =
        wm.addChild(cm, id, "Slot", "mem://comp.hasset");
    CHECK(embedded != 0);
    CHECK(wm.widgetIdForScript(embedded) == id);
    // Nothing to resolve is not a wrong guess: no instance, no widget.
    CHECK(wm.widgetIdForScript(0) == 0);
    CHECK(wm.widgetIdForScript(999999) == 0);
}

// Get/Set Property reach an element of the widget the graph belongs to, by the
// id it has there. The by-REFERENCE pair reaches one through a widget reference
// and by NAME, which is what lets a graph act on a widget it did not author —
// and what makes the id offset of an embedded component matter again.
TEST_CASE("Get/Set Property (Ref): by name, inside whatever the reference points at")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    // A component with a "Title" of its own…
    HE::UIWidgetTree comp;
    comp.canvasWidth = 100.0f; comp.canvasHeight = 100.0f;
    comp.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int compTitle = comp.add(HE::UIWidgetType::Text);
    {
        HE::UIElement& e = *comp.find(compTitle);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = e.posY = 0.0f; e.sizeX = 100.0f; e.sizeY = 20.0f;
        e.name = "Title";
        e.renderOpacity = 1.0f;
    }
    // Its graph: two functions, one writing an element that exists in it and one
    // writing a name that does not. Both leave Target unwired, which means the
    // instance that is running — the component, not the page it sits in.
    HorizonCode::Graph cg;
    {
        HorizonCode::Node entry; entry.type = NodeType::FunctionEntry; entry.s = "Go";
        const int entryId = cg.addNode(entry);
        HorizonCode::Node set; set.type = NodeType::SetPropertyOn;
        set.s = "Render Opacity"; set.propType = PinType::Float;
        HorizonCode::Value nameV; nameV.type = PinType::String; nameV.s = "Title";
        set.pinDefaults[1] = nameV;
        const int setId = cg.addNode(set);
        HorizonCode::Node lit; lit.type = NodeType::ConstFloat; lit.f[0] = 0.25f;
        const int litId = cg.addNode(lit);
        REQUIRE(cg.connect(entryId, 0, setId, 0));   // exec
        // Unified pin index: [exec in][exec out][Target][Element][Value].
        REQUIRE(cg.connect(litId, 0, setId, 4));

        HorizonCode::Node entry2; entry2.type = NodeType::FunctionEntry; entry2.s = "Missing";
        const int entry2Id = cg.addNode(entry2);
        HorizonCode::Node set2; set2.type = NodeType::SetPropertyOn;
        set2.s = "Render Opacity"; set2.propType = PinType::Float;
        HorizonCode::Value nope; nope.type = PinType::String; nope.s = "Nope";
        set2.pinDefaults[1] = nope;
        const int set2Id = cg.addNode(set2);
        HorizonCode::Node lit2; lit2.type = NodeType::ConstFloat; lit2.f[0] = 0.0f;
        const int lit2Id = cg.addNode(lit2);
        REQUIRE(cg.connect(entry2Id, 0, set2Id, 0));
        REQUIRE(cg.connect(lit2Id, 0, set2Id, 4));
    }
    registerWidgetAs(cm, "mem://comp.hasset", comp, &cg);

    // …inside a page that also has one. Same name, two trees: the whole reason
    // the lookup is scoped to what the reference stands for.
    HE::UIWidgetTree page;
    page.canvasWidth = 400.0f; page.canvasHeight = 400.0f;
    page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int pageTitle = page.add(HE::UIWidgetType::Text);
    {
        HE::UIElement& e = *page.find(pageTitle);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 200.0f; e.sizeX = 100.0f; e.sizeY = 20.0f;
        e.name = "Title";
        e.renderOpacity = 1.0f;
    }
    // The slot, placed in the designer: a WidgetRef with a NAME, which is what
    // makes the component in it addressable from the graph.
    const int slot = page.add(HE::UIWidgetType::WidgetRef);
    {
        auto* r = dynamic_cast<HE::UIWidgetRef*>(page.find(slot));
        r->widgetPath = "mem://comp.hasset";
        r->name = "Card";
        HE::uiSetAnchorPreset(*r, 0); r->pivotX = r->pivotY = 0.0f;
        r->posX = 0.0f; r->posY = 0.0f; r->sizeX = 100.0f; r->sizeY = 100.0f;
    }
    registerWidgetAs(cm, "mem://page.hasset", page);

    // The runtime is held here so the test can call the COMPONENT's own
    // function: its graph is its own instance, which nothing outside reaches
    // through the page.
    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);
    const int id = createShown(wm, cm, "mem://page.hasset");
    REQUIRE(id != 0);
    const HorizonCode::InstanceId child = wm.childInstance(id, "Card");
    REQUIRE(child != 0);

    // The page's own "Title" and the component's are two different elements
    // with one name; the grafted one sits past the page's ids.
    const HE::UIElement* grafted = nullptr;
    for (const auto& ep : wm.tree(id)->elements)
        if (ep && ep->name == "Title" && ep->id != pageTitle) grafted = ep.get();
    REQUIRE(grafted);

    // The component's own graph, run through the runtime: Target left unwired,
    // so it means the instance that is calling — the COMPONENT, whose "Title"
    // is not the page's. Get the value back the same way and compare, so the
    // read is checked against the same scoping as the write.
    REQUIRE(rt.callFunction(child, "Go"));
    CHECK(grafted->renderOpacity == doctest::Approx(0.25f));
    CHECK(wm.tree(id)->find(pageTitle)->renderOpacity == doctest::Approx(1.0f));

    // A name that is not in THAT tree writes nothing: a graph pointed at the
    // wrong widget must not be able to invent an element in it.
    REQUIRE(rt.callFunction(child, "Missing"));
    CHECK(grafted->renderOpacity == doctest::Approx(0.25f));
    CHECK(wm.tree(id)->find(pageTitle)->renderOpacity == doctest::Approx(1.0f));

    // The other kind of reference: what SITS IN the slot. A component is an
    // instance, so the slot's name yields the handle its functions and its
    // public variables are reached through — and, wired into a Set Property
    // (Ref), the page can drive the component's element from outside.
    HorizonWorld world;
    world.setWidgetManager(&wm);
    HE::api::Ctx c;
    c.world = &world;
    c.content = &cm;
    CHECK(HE::api::widget::childRef(c, id, "Card") == child);
    // The slot has to be a slot: a plain element is not an instance, and a name
    // nobody carries is not one either.
    CHECK(HE::api::widget::childRef(c, id, "Title") == 0u);
    CHECK(HE::api::widget::childRef(c, id, "Nope") == 0u);
    CHECK(HE::api::widget::childRef(c, id, "") == 0u);
    CHECK(HE::api::widget::childRef(c, 0, "Card") == 0u);
}

// The layer decides what the arrows MEAN. With a list hanging open, up and down
// step through its entries; moving the focus to the next button would be
// answering a question nobody asked — and that button is not even reachable,
// because the open list has taken the whole input.
TEST_CASE("Navigation: an open list takes the arrows, the buttons behind it do not")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 800.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int combo = t.add(HE::UIWidgetType::ComboBox);
    {
        auto* cb = dynamic_cast<HE::UIComboBox*>(t.find(combo));
        HE::uiSetAnchorPreset(*cb, 0); cb->pivotX = cb->pivotY = 0.0f;
        cb->posX = 0.0f; cb->posY = 0.0f; cb->sizeX = 200.0f; cb->sizeY = 20.0f;
        cb->options = { "Zero", "One", "Two", "Three" };
        cb->selectedIndex = 1;
    }
    // Something to navigate to, so "the focus did not move" means something.
    const int below = t.add(HE::UIWidgetType::Button);
    {
        HE::UIElement& e = *t.find(below);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 400.0f; e.sizeX = 200.0f; e.sizeY = 40.0f;
    }
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    using Nav = WidgetManager::NavDir;
    auto box = [&]{ return dynamic_cast<const HE::UIComboBox*>(wm.tree(id)->find(combo)); };

    // Open it with the keyboard, which is the case this is about.
    CHECK(wm.navigate(Nav::Down, 400.0f, 800.0f));
    CHECK(wm.focusedElement() == combo);
    CHECK(wm.activateFocused());
    REQUIRE(box()->open);
    CHECK(wm.hasOpenDropdown());
    // Opening highlights what is already selected, so the first arrow steps
    // from there rather than from the top of the list.
    CHECK(box()->hoverIndex == 1);

    CHECK(wm.navigate(Nav::Down, 400.0f, 800.0f));
    CHECK(box()->hoverIndex == 2);
    // …and the focus did NOT go to the button below. That is the whole point.
    CHECK(wm.focusedElement() == combo);

    CHECK(wm.navigate(Nav::Up, 400.0f, 800.0f));
    CHECK(box()->hoverIndex == 1);
    // Left and right belong to nobody here: refused rather than falling through
    // to the elements underneath the open list.
    CHECK_FALSE(wm.navigate(Nav::Left, 400.0f, 800.0f));
    CHECK_FALSE(wm.navigate(Nav::Right, 400.0f, 800.0f));
    CHECK(box()->hoverIndex == 1);

    // Clamped at both ends, not wrapped: a highlight that reappears at the other
    // end is a jump you have to watch to understand.
    CHECK(wm.navigate(Nav::Up, 400.0f, 800.0f));
    CHECK(box()->hoverIndex == 0);
    CHECK_FALSE(wm.navigate(Nav::Up, 400.0f, 800.0f));
    CHECK(box()->hoverIndex == 0);

    // Enter takes the highlighted entry and closes the list — the same thing
    // letting the button go over it does.
    CHECK(wm.navigate(Nav::Down, 400.0f, 800.0f));
    CHECK(box()->hoverIndex == 1);
    CHECK(wm.navigate(Nav::Down, 400.0f, 800.0f));
    CHECK(wm.activateFocused());
    CHECK(box()->selectedIndex == 2);
    CHECK_FALSE(box()->open);
    CHECK_FALSE(wm.hasOpenDropdown());

    // With the list shut, the arrows are the focus's again.
    CHECK(wm.navigate(Nav::Down, 400.0f, 800.0f));
    CHECK(wm.focusedElement() == below);

    // ── Tab belongs to the list too ─────────────────────────────────────────
    // The layer on top decides what a key means, and that is as true of Tab as
    // of the arrows: it used to close the list and move on, which walked out of
    // a list somebody had just opened. Inside one it is simply the down arrow.
    CHECK(wm.setFocus(id, combo));
    CHECK(wm.activateFocused());
    REQUIRE(box()->open);
    const int from = box()->hoverIndex;
    CHECK(wm.focusNext(false, 400.0f, 800.0f));
    CHECK(box()->hoverIndex == from + 1);
    CHECK(box()->open);                        // …and it is still open
    CHECK(wm.focusedElement() == combo);       // …and the focus never left
    CHECK(wm.focusNext(true, 400.0f, 800.0f));
    CHECK(box()->hoverIndex == from);
    // Escape is still how you leave without choosing.
    CHECK(wm.closeTopLayer());
    CHECK_FALSE(box()->open);
    CHECK(wm.focusNext(false, 400.0f, 800.0f));
    CHECK(wm.focusedElement() == below);
}

// A focus ring made of four edge quads can only draw a square, so a rounded
// search field wore a square ring with its corners poking out of it. A ring
// whose shape disagrees with the thing it marks reads as a drawing mistake, not
// as focus.
TEST_CASE("Focus: the ring follows the field's own corners")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int field = t.add(HE::UIWidgetType::TextInput);
    {
        HE::UIElement& e = *t.find(field);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 40.0f;
        e.cornerRadius = glm::vec4(12.0f);   // a search field, as they are drawn
    }
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    REQUIRE(wm.setFocus(id, field));

    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);

    // Exactly ONE outlined object — the shared ring, and no second one of the
    // field's own — round, and drawn ON the element's edge rather than outside
    // it: a text field clips its own glyphs to its rect, so a ring two pixels
    // beyond that edge is two pixels of clipped-away nothing.
    int rings = 0;
    for (const UIRenderObject& ro : out)
    {
        if (ro.borderWidth <= 0.0f || ro.color.a > 0.001f) continue;
        CHECK(ro.position.x == doctest::Approx(0.0f));
        CHECK(ro.position.y == doctest::Approx(0.0f));
        CHECK(ro.size.x == doctest::Approx(200.0f));
        CHECK(ro.size.y == doctest::Approx(40.0f));
        CHECK(ro.cornerRadius.x == doctest::Approx(12.0f));
        CHECK(ro.borderColor.r > ro.borderColor.b);   // the focus amber
        ++rings;
    }
    CHECK(rings == 1);

    // …and none of the old edge slivers: a 2 px band the width of the field is
    // what a square ring is made of, and there must not be one left.
    int slivers = 0;
    for (const UIRenderObject& ro : out)
        if (ro.type == 0 && std::fabs(ro.size.y - 2.0f) < 0.01f &&
            ro.size.x > 100.0f)
        {
            ++slivers;
            MESSAGE("sliver at " << ro.position.x << "," << ro.position.y
                    << " size " << ro.size.x << "x" << ro.size.y
                    << " colour a=" << ro.color.a);
        }
    CHECK(slivers == 0);

    // ── …and around the FRAME when an ancestor claims to be the control ──────
    // A search field is a rounded frame with an icon and a text field inset
    // between them. The field takes the keyboard, so the ring landed on the
    // field: a small rectangle floating inside a pill, marking a part nobody
    // thinks of as the control.
    {
        HE::UIWidgetTree st;
        st.canvasWidth = 400.0f; st.canvasHeight = 400.0f;
        st.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
        const int frame = st.add(HE::UIWidgetType::Panel);
        {
            HE::UIElement& e = *st.find(frame);
            HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
            e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 300.0f; e.sizeY = 34.0f;
            e.cornerRadius = glm::vec4(17.0f);   // the pill
            e.focusFrame = true;
        }
        const int inner = st.add(HE::UIWidgetType::TextInput);
        {
            HE::UIElement& e = *st.find(inner);
            e.parentId = frame;
            HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
            e.posX = 14.0f; e.posY = 2.0f; e.sizeX = 272.0f; e.sizeY = 30.0f;
        }
        TempWidgetDir dir2;
        ContentManager cm2(dir2.path.string());
        registerWidgetAs(cm2, "mem://search.hasset", st);

        WidgetManager wm2;
        const int sid = createShown(wm2, cm2, "mem://search.hasset");
        REQUIRE(sid != 0);
        REQUIRE(wm2.setFocus(sid, inner));

        std::vector<UIRenderObject> so;
        wm2.extract(400.0f, 400.0f, so);
        int framed = 0;
        for (const UIRenderObject& ro : so)
        {
            if (ro.borderWidth <= 0.0f || ro.color.a > 0.001f) continue;
            // The frame's rect, not the field's — and the frame's rounding.
            CHECK(ro.position.x == doctest::Approx(0.0f));
            CHECK(ro.size.x == doctest::Approx(300.0f));
            CHECK(ro.size.y == doctest::Approx(34.0f));
            CHECK(ro.cornerRadius.x == doctest::Approx(17.0f));
            ++framed;
        }
        CHECK(framed == 1);
    }

    // Nothing of the sort while it is not focused.
    REQUIRE(wm.setFocus(id, 0));
    out.clear();
    wm.extract(400.0f, 400.0f, out);
    for (const UIRenderObject& ro : out)
    {
        const bool outlined = ro.borderWidth > 0.0f && ro.color.a <= 0.001f;
        CHECK_FALSE(outlined);
    }
}

// An application calls processPointer every frame whether the mouse stirred or
// not. The open list took "where the mouse is" from every one of those calls,
// so the arrow keys set the highlight and the very next frame put it back — the
// row flashed and the selection snapped home, which is what it looked like.
TEST_CASE("Navigation: a mouse that has not moved does not take the highlight back")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 800.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int combo = t.add(HE::UIWidgetType::ComboBox);
    {
        auto* cb = dynamic_cast<HE::UIComboBox*>(t.find(combo));
        HE::uiSetAnchorPreset(*cb, 0); cb->pivotX = cb->pivotY = 0.0f;
        cb->posX = 0.0f; cb->posY = 0.0f; cb->sizeX = 200.0f; cb->sizeY = 20.0f;
        cb->options = { "Zero", "One", "Two", "Three" };
        cb->selectedIndex = 0;
    }
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    using Nav = WidgetManager::NavDir;
    auto box = [&]{ return dynamic_cast<const HE::UIComboBox*>(wm.tree(id)->find(combo)); };
    // The pointer parked well away from the list, which is where it is when
    // somebody is using the keyboard.
    auto idleFrame = [&]{ wm.processPointer(400.0f, 800.0f, 380.0f, 700.0f, false, true); };

    idleFrame();
    REQUIRE(wm.setFocus(id, combo));
    REQUIRE(wm.activateFocused());   // the keyboard's way of opening one
    REQUIRE(box()->open);
    CHECK(box()->hoverIndex == 0);

    // Two arrows and a frame between them, exactly as an application runs it.
    CHECK(wm.navigate(Nav::Down, 400.0f, 800.0f));
    CHECK(box()->hoverIndex == 1);
    idleFrame();
    CHECK(box()->hoverIndex == 1);   // …and it stays there
    CHECK(wm.navigate(Nav::Down, 400.0f, 800.0f));
    idleFrame();
    idleFrame();
    CHECK(box()->hoverIndex == 2);

    // A mouse that MOVES takes it back, because then it is the mouse's again.
    // Over the second row of the list, which hangs under the closed box.
    wm.processPointer(400.0f, 800.0f, 100.0f, 30.0f, false, true);
    CHECK(box()->hoverIndex == 0);
    wm.processPointer(400.0f, 800.0f, 100.0f, 50.0f, false, true);
    CHECK(box()->hoverIndex == 1);
}

// "Something is focused" and "a text field owns the keyboard" are different
// questions, and answering the second with the first is why menu navigation
// worked exactly once: both apps gate the arrow keys on this, the first press
// focused a button, and from then on every key was routed into the widget as if
// somebody were typing. It also started SDL text input, and swallowed the
// movement keys from gameplay, for a focused BUTTON.
TEST_CASE("Navigation: a focused button does not claim the keyboard")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int btn = t.add(HE::UIWidgetType::Button);
    {
        HE::UIElement& e = *t.find(btn);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 40.0f;
    }
    const int field = t.add(HE::UIWidgetType::TextInput);
    {
        HE::UIElement& e = *t.find(field);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 100.0f; e.sizeX = 200.0f; e.sizeY = 40.0f;
    }
    const int readOnly = t.add(HE::UIWidgetType::TextInput);
    {
        auto* e = dynamic_cast<HE::UITextInput*>(t.find(readOnly));
        e->editable = false;                 // still selectable
        HE::uiSetAnchorPreset(*e, 0); e->pivotX = e->pivotY = 0.0f;
        e->posX = 0.0f; e->posY = 200.0f; e->sizeX = 200.0f; e->sizeY = 40.0f;
    }
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);

    CHECK_FALSE(wm.hasFocusedTextField());
    wm.setFocus(id, btn);
    CHECK(wm.focusedElement() == btn);
    CHECK_FALSE(wm.hasFocusedTextField());   // a button is not a text field

    wm.setFocus(id, field);
    CHECK(wm.hasFocusedTextField());
    // Read-only but selectable: it takes the arrows to move its selection and
    // Ctrl+C to copy out, so it owns the keyboard just as much.
    wm.setFocus(id, readOnly);
    CHECK(wm.hasFocusedTextField());

    // …and back to a button, which hands the keyboard back. The arrows keep
    // working, which is the behaviour this whole thing is about.
    wm.setFocus(id, btn);
    CHECK_FALSE(wm.hasFocusedTextField());
    CHECK(wm.navigate(WidgetManager::NavDir::Down, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == field);
}

// Standing on a field and typing into it are two states, and the tab order dies
// in the first search box it meets unless they are. Reaching one with Tab used
// to hand it the keyboard on the spot, so every key after that was text — Tab
// included, which is why there was no way back out.
TEST_CASE("Navigation: a field takes the keyboard only when it is confirmed")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int field = t.add(HE::UIWidgetType::TextInput);
    {
        HE::UIElement& e = *t.find(field);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 40.0f;
    }
    const int btn = t.add(HE::UIWidgetType::Button);
    {
        HE::UIElement& e = *t.find(btn);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 100.0f; e.sizeX = 200.0f; e.sizeY = 40.0f;
    }
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    auto text = [&]{ return dynamic_cast<const HE::UITextInput*>(wm.tree(id)->find(field))->text; };

    // Tab onto it: focused, ringed, and NOT taking keystrokes.
    REQUIRE(wm.focusNext(false, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == field);
    CHECK(wm.hasFocusedTextField());     // a text field HAS the focus…
    CHECK_FALSE(wm.isEditingText());     // …and does not own the keyboard
    wm.inputText("no");
    CHECK(text().empty());

    // …so Tab still means Tab. This is the bug: it used to stay here for ever.
    REQUIRE(wm.focusNext(false, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == btn);

    // Back, and confirmed this time.
    REQUIRE(wm.focusNext(true, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == field);
    CHECK(wm.activateFocused());
    CHECK(wm.isEditingText());
    wm.inputText("yes");
    CHECK(text() == "yes");

    // Escape leaves the field WITHOUT moving the focus, so the next Tab carries
    // on from here rather than from the top of the form.
    CHECK(wm.stopEditingText());
    CHECK_FALSE(wm.isEditingText());
    CHECK(wm.focusedElement() == field);
    CHECK_FALSE(wm.stopEditingText());   // nothing left to leave
    wm.inputText("!");
    CHECK(text() == "yes");
    REQUIRE(wm.focusNext(false, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == btn);

    // A CLICK is its own confirmation: pointing at a field and pressing is
    // nobody's idea of "select it for later".
    wm.processPointer(400.0f, 400.0f, 100.0f, 20.0f, true, true);
    CHECK(wm.focusedElement() == field);
    CHECK(wm.isEditingText());
    wm.processPointer(400.0f, 400.0f, 100.0f, 20.0f, false, true);

    // And walking away drops it again, whatever it was doing.
    REQUIRE(wm.focusNext(false, 400.0f, 400.0f));
    CHECK_FALSE(wm.isEditingText());
}

TEST_CASE("Navigation: Tab walks the form in hierarchy order and wraps")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    // Built so that HIERARCHY order and ID order disagree, which is the whole
    // claim: the box comes first in the tree but its children are created last,
    // so walking the element vector would visit the loose button before them.
    // Ids: box 1, loose 2, first 3, second 4, hidden 5.
    // Hierarchy: first, second, then loose.  Ids: loose, first, second.
    const int box = t.add(HE::UIWidgetType::VerticalBox);
    {
        HE::UIElement& e = *t.find(box);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 200.0f;
    }
    const int loose = t.add(HE::UIWidgetType::Button);
    {
        HE::UIElement& e = *t.find(loose);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 300.0f; e.sizeX = 100.0f; e.sizeY = 40.0f;
    }
    const int first  = t.add(HE::UIWidgetType::Button);
    const int second = t.add(HE::UIWidgetType::CheckBox);
    const int hidden = t.add(HE::UIWidgetType::Button);
    for (int c : { first, second, hidden }) t.find(c)->parentId = box;
    t.find(hidden)->visible = false;   // not reachable, so not in the order
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);

    // Nothing focused: the first Tab takes the first element of the TREE, which
    // is the box's first child — not the loose button, which has the lower id.
    CHECK(wm.focusNext(false, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == first);
    CHECK(wm.focusNext(false, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == second);
    // Out of the box and on to the next root. The hidden one is skipped.
    CHECK(wm.focusNext(false, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == loose);
    // The end wraps round to the beginning: a form is a ring you cycle through.
    CHECK(wm.focusNext(false, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == first);
    // Shift+Tab goes back, and wraps the other way.
    CHECK(wm.focusNext(true, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == loose);
    CHECK(wm.focusNext(true, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == second);
    // The box itself is not in the order: a container takes no input, so it is
    // not a stop on the way through.
    CHECK(wm.focusedElement() != box);
}

// Tab Index is the override on top of that order, for the two cases the tree
// cannot express: "this one first, whatever the tree says" and "skip this on the
// way through". Both are checked against the SAME tree as the test above, so a
// difference here can only be the index.
TEST_CASE("Navigation: Tab Index reorders the route and takes elements off it")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int box = t.add(HE::UIWidgetType::VerticalBox);
    {
        HE::UIElement& e = *t.find(box);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 200.0f;
    }
    const int loose = t.add(HE::UIWidgetType::Button);
    {
        HE::UIElement& e = *t.find(loose);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 300.0f; e.sizeX = 100.0f; e.sizeY = 40.0f;
    }
    const int first  = t.add(HE::UIWidgetType::Button);
    const int second = t.add(HE::UIWidgetType::CheckBox);
    const int third  = t.add(HE::UIWidgetType::Button);
    for (int c : { first, second, third }) t.find(c)->parentId = box;

    // The loose button is LAST in the tree and asked to be first; the third
    // child asks to be second, though the tree puts it behind both its
    // siblings; the check box asks to be left out. `first` stays at 0 and keeps
    // the place the tree gave it, which is now the back of the route.
    t.find(loose)->tabIndex  = 1;
    t.find(third)->tabIndex  = 2;
    t.find(second)->tabIndex = -1;
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);

    // Ahead of every 0, so the first Tab lands on it and not on the box's child.
    CHECK(wm.focusNext(false, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == loose);
    // Two positives sort against each other, ascending, and that beats the tree.
    CHECK(wm.focusNext(false, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == third);
    // Then the un-indexed remainder, in tree order.
    CHECK(wm.focusNext(false, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == first);
    // …and round again: the check box behind it is off the route, so the ring
    // wraps past it rather than stopping there.
    CHECK(wm.focusNext(false, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == loose);
    // Backwards skips it too, rather than sticking on it.
    CHECK(wm.focusNext(true, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == first);

    // Off the route is not out of reach: setFocus (and a click, which is the
    // same door) still gets there, and Tab from there carries on from HERE
    // rather than starting the form over.
    CHECK(wm.setFocus(id, second));
    CHECK(wm.focusedElement() == second);
    CHECK(wm.focusNext(false, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == loose);
}

// A base property that is not saved when it is at its default is a base property
// that costs nothing: every widget authored before it existed writes the same
// bytes it always did.
TEST_CASE("Tab Index: written only once set, read back as authored")
{
    HE::UIWidgetTree t;
    const int b = t.add(HE::UIWidgetType::Button);
    const std::string plain = HE::uiWidgetTreeToJson(t);
    CHECK(plain.find("tabIndex") == std::string::npos);

    t.find(b)->tabIndex = -1;
    const std::string withIt = HE::uiWidgetTreeToJson(t);
    CHECK(withIt.find("tabIndex") != std::string::npos);

    HE::UIWidgetTree back;
    REQUIRE(HE::uiWidgetTreeFromJson(withIt, back));
    CHECK(back.find(b)->tabIndex == -1);

    // …and through the property table, which is how a script and a theme reach
    // it. Negative on purpose: nothing clamps it, all three ranges mean something.
    back.find(b)->setPropAny("Tab Index", HE::UIPropValue::ofInt(3));
    CHECK(back.find(b)->getPropAny("Tab Index").i == 3);
    CHECK(back.find(b)->clone()->tabIndex == 3);
}

TEST_CASE("Navigation: Tab shuts an open list, and stays inside a dialog")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    // The page behind, with a button of its own to escape to.
    HE::UIWidgetTree page;
    page.canvasWidth = 400.0f; page.canvasHeight = 400.0f;
    page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int pageBtn = page.add(HE::UIWidgetType::Button);
    {
        HE::UIElement& e = *page.find(pageBtn);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 100.0f; e.sizeY = 40.0f;
    }
    registerWidgetAs(cm, "mem://page.hasset", page);

    HE::UIWidgetTree dlg;
    dlg.canvasWidth = 400.0f; dlg.canvasHeight = 400.0f;
    dlg.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int combo = dlg.add(HE::UIWidgetType::ComboBox);
    {
        auto* cb = dynamic_cast<HE::UIComboBox*>(dlg.find(combo));
        HE::uiSetAnchorPreset(*cb, 0); cb->pivotX = cb->pivotY = 0.0f;
        cb->posX = 100.0f; cb->posY = 100.0f; cb->sizeX = 200.0f; cb->sizeY = 20.0f;
        cb->options = { "A", "B", "C" };
        cb->selectedIndex = 0;
    }
    const int ok = dlg.add(HE::UIWidgetType::Button);
    {
        HE::UIElement& e = *dlg.find(ok);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 100.0f; e.posY = 200.0f; e.sizeX = 100.0f; e.sizeY = 40.0f;
    }
    registerWidgetAs(cm, "mem://dlg.hasset", dlg);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://page.hasset") != 0);
    const int dialog = wm.createWidget(cm, "mem://dlg.hasset");
    REQUIRE(dialog != 0);
    wm.showModal(dialog);

    // Tab stays in the dialog — it never reaches the page's button, which is
    // the focus trap the layer exists for. Asked by WIDGET, not by element id:
    // ids are per widget, so the page's button and the dialog's combo are both
    // element 1 and comparing those would prove nothing.
    for (int i = 0; i < 6; ++i)
    {
        CHECK(wm.focusNext(false, 400.0f, 400.0f));
        CHECK(wm.focusedWidget() == dialog);
    }
    (void)pageBtn;

    // Open the list, then Tab: it steps through the LIST and stays open. The
    // layer on top decides what a key means, and walking out of a list somebody
    // has just opened is answering a question nobody asked — the same reason
    // the arrows belong to it.
    wm.setFocus(dialog, combo);
    CHECK(wm.activateFocused());
    REQUIRE(wm.hasOpenDropdown());
    CHECK(wm.focusNext(false, 400.0f, 400.0f));
    CHECK(wm.hasOpenDropdown());
    CHECK(wm.focusedElement() == combo);
    // Stepping is not choosing: nothing is taken until Enter.
    CHECK(dynamic_cast<const HE::UIComboBox*>(wm.tree(dialog)->find(combo))->selectedIndex == 0);
    // …and once it is shut, Tab is the form's again.
    CHECK(wm.closeTopLayer());
    CHECK(wm.focusNext(false, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == ok);
}

TEST_CASE("Navigation: left and right step a focused slider instead of leaving it")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int sl = t.add(HE::UIWidgetType::Slider);
    { HE::UIElement& e = *t.find(sl);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 30.0f;
      e.setProp("Value", HE::UIPropValue::ofFloat(0.5f)); }
    const int btn = t.add(HE::UIWidgetType::Button);
    { HE::UIElement& e = *t.find(btn);
      e.setProp("Text", HE::UIPropValue::ofString(""));
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 250.0f; e.posY = 0.0f; e.sizeX = 100.0f; e.sizeY = 30.0f; }
    registerWidget(cm, t);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    using Nav = WidgetManager::NavDir;
    REQUIRE(wm.navigate(Nav::Down, 400.0f, 400.0f));
    REQUIRE(wm.focusedElement() == sl);

    // Right steps the value and keeps the focus — the button next to it does
    // NOT steal it, which is the whole point.
    CHECK(wm.navigate(Nav::Right, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == sl);
    for (int i = 0; i < 40; ++i) wm.navigate(Nav::Right, 400.0f, 400.0f);
    // At the maximum the key stops being consumed.
    CHECK_FALSE(wm.navigate(Nav::Right, 400.0f, 400.0f));
    CHECK(wm.focusedElement() == sl);
}

TEST_CASE("Navigation: the focused element gets a ring drawn around it")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    const std::array<int, 4> b = cornerButtons(t);
    (void)b;
    registerWidget(cm, t);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);

    std::vector<UIRenderObject> before;
    wm.extract(400.0f, 400.0f, before);
    REQUIRE(wm.navigate(WidgetManager::NavDir::Down, 400.0f, 400.0f));
    std::vector<UIRenderObject> after;
    wm.extract(400.0f, 400.0f, after);
    // One object more than before: the ring is a single outlined rectangle that
    // follows the element's corners, not four hairlines that can only be square.
    CHECK(after.size() == before.size() + 1);

    // Clearing the focus takes them away again.
    CHECK(wm.setFocus(1, 0));
    std::vector<UIRenderObject> cleared;
    wm.extract(400.0f, 400.0f, cleared);
    CHECK(cleared.size() == before.size());
    CHECK(wm.focusedElement() == 0);
}

// ═══ ScrollBox ═══════════════════════════════════════════════════════════════
// A vertical box whose content may be taller than it is: it clips and shifts
// the stack up by the current offset. The offset is runtime state — a menu that
// reopens where it was last scrolled to would be a bug.

namespace
{
    // A 200x200 scroll box at the canvas origin with `n` children of `h` each.
    int scrollBoxWith(HE::UIWidgetTree& t, int n, float h)
    {
        t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
        const int box = t.add(HE::UIWidgetType::ScrollBox);
        HE::UIElement& b = *t.find(box);
        HE::uiSetAnchorPreset(b, 0);
        b.pivotX = b.pivotY = 0.0f;
        b.posX = b.posY = 0.0f; b.sizeX = 200.0f; b.sizeY = 200.0f;
        b.setProp("Padding", HE::UIPropValue::ofFloat(0.0f));
        b.setProp("Spacing", HE::UIPropValue::ofFloat(0.0f));
        b.setProp("Bar Width", HE::UIPropValue::ofFloat(0.0f)); // measured separately
        for (int i = 0; i < n; ++i)
        {
            const int c = t.add(HE::UIWidgetType::Panel);
            HE::UIElement& e = *t.find(c);
            e.parentId = box;
            e.sizeY = h;
        }
        return box;
    }
}

TEST_CASE("ScrollBox: the offset moves the stack and is clamped to the content")
{
    HE::UIWidgetTree t;
    const int box = scrollBoxWith(t, 5, 100.0f);   // 500 of content in 200
    const std::vector<int> kids = t.childrenOf(box);
    HE::uiUpdateScrollExtents(t);

    auto* sb = dynamic_cast<HE::UIScrollBox*>(t.find(box));
    REQUIRE(sb != nullptr);
    CHECK(sb->contentExtent == doctest::Approx(500.0f));
    CHECK(sb->maxScroll() == doctest::Approx(300.0f));
    CHECK(HE::uiElementRect(t, *t.find(kids[0])).y == doctest::Approx(0.0f));

    // Scrolling shifts every child up by the same amount.
    CHECK(HE::uiScrollBy(t, box, 150.0f));
    CHECK(HE::uiElementRect(t, *t.find(kids[0])).y == doctest::Approx(-150.0f));
    CHECK(HE::uiElementRect(t, *t.find(kids[2])).y == doctest::Approx(50.0f));

    // It stops at the end, and says so by returning false — that is how the
    // caller knows the wheel was not consumed.
    CHECK(HE::uiScrollBy(t, box, 1000.0f));
    CHECK(sb->scrollOffset == doctest::Approx(300.0f));
    CHECK_FALSE(HE::uiScrollBy(t, box, 10.0f));
    CHECK_FALSE(HE::uiScrollBy(t, box, 0.0f));
    // …and at the top the same.
    CHECK(HE::uiScrollBy(t, box, -1000.0f));
    CHECK(sb->scrollOffset == doctest::Approx(0.0f));
    CHECK_FALSE(HE::uiScrollBy(t, box, -5.0f));

    // Content that fits scrolls not at all.
    for (size_t i = 1; i < kids.size(); ++i) t.find(kids[i])->visible = false;
    HE::uiUpdateScrollExtents(t);
    CHECK(sb->maxScroll() == doctest::Approx(0.0f));
    CHECK_FALSE(HE::uiScrollBy(t, box, 50.0f));

    // A box that shrank below its offset is pulled back, not left past its end.
    for (size_t i = 1; i < kids.size(); ++i) t.find(kids[i])->visible = true;
    HE::uiUpdateScrollExtents(t);
    REQUIRE(HE::uiScrollBy(t, box, 300.0f));
    t.find(kids[4])->visible = false;
    t.find(kids[3])->visible = false;
    HE::uiUpdateScrollExtents(t);
    CHECK(sb->scrollOffset == doctest::Approx(100.0f));   // the new maximum

    // Not a scroll box → nothing to scroll, and no crash on an unknown id.
    CHECK_FALSE(HE::uiScrollBy(t, kids[0], 10.0f));
    CHECK_FALSE(HE::uiScrollBy(t, 9999, 10.0f));
}

TEST_CASE("ScrollBox: it clips by default and draws a bar only when it can scroll")
{
    HE::UIWidgetTree t;
    const int box = scrollBoxWith(t, 4, 100.0f);
    auto* sb = dynamic_cast<HE::UIScrollBox*>(t.find(box));
    REQUIRE(sb != nullptr);
    CHECK(sb->clipChildren);              // the whole point of the type
    CHECK(sb->laysOutChildren());
    CHECK(sb->stacksVertically());

    sb->barWidth = 8.0f;
    std::vector<UIRenderObject> out;
    HE::uiUpdateScrollExtents(t);
    sb->render({ 0.0f, 0.0f, 200.0f, 200.0f }, {}, HE::UUID{}, 1.0f, out);
    REQUIRE(out.size() == 1);             // the thumb
    const float topY = out[0].position.y;
    CHECK(out[0].position.x == doctest::Approx(192.0f));   // right edge, inset by the bar
    CHECK(out[0].size.x == doctest::Approx(8.0f));

    // Scrolled to the end the thumb sits at the bottom of the track.
    REQUIRE(HE::uiScrollBy(t, box, 1000.0f));
    out.clear();
    sb->render({ 0.0f, 0.0f, 200.0f, 200.0f }, {}, HE::UUID{}, 1.0f, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].position.y > topY);
    CHECK(out[0].position.y + out[0].size.y == doctest::Approx(200.0f));

    // Nothing to scroll → no bar at all, rather than a bar that does nothing.
    for (int id : t.childrenOf(box)) t.find(id)->visible = false;
    HE::uiUpdateScrollExtents(t);
    out.clear();
    sb->render({ 0.0f, 0.0f, 200.0f, 200.0f }, {}, HE::UUID{}, 1.0f, out);
    CHECK(out.empty());
}

TEST_CASE("ScrollBox: the offset is runtime state, the look is authored")
{
    HE::UIWidgetTree t;
    const int box = scrollBoxWith(t, 4, 100.0f);
    auto* sb = dynamic_cast<HE::UIScrollBox*>(t.find(box));
    REQUIRE(sb != nullptr);
    sb->barWidth = 9.0f;
    sb->barColor = glm::vec4(0.1f, 0.2f, 0.3f, 0.4f);
    HE::uiUpdateScrollExtents(t);
    REQUIRE(HE::uiScrollBy(t, box, 100.0f));

    const std::string json = HE::uiWidgetTreeToJson(t);
    CHECK(json.find("scrollOffset") == std::string::npos);
    CHECK(json.find("contentExtent") == std::string::npos);

    HE::UIWidgetTree r;
    REQUIRE(HE::uiWidgetTreeFromJson(json, r));
    auto* rb = dynamic_cast<HE::UIScrollBox*>(r.find(box));
    REQUIRE(rb != nullptr);
    CHECK(rb->barWidth == doctest::Approx(9.0f));
    CHECK(rb->barColor.b == doctest::Approx(0.3f));
    CHECK(rb->scrollOffset == doctest::Approx(0.0f));   // reopened at the top
}

TEST_CASE("WidgetManager: the wheel scrolls the box under the cursor")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int box = t.add(HE::UIWidgetType::ScrollBox);
    { HE::UIElement& e = *t.find(box);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 200.0f;
      e.setProp("Padding", HE::UIPropValue::ofFloat(0.0f));
      e.setProp("Spacing", HE::UIPropValue::ofFloat(0.0f)); }
    for (int i = 0; i < 4; ++i)
    {
        const int b = t.add(HE::UIWidgetType::Button);
        HE::UIElement& e = *t.find(b);
        e.parentId = box;
        e.setProp("Text", HE::UIPropValue::ofString(""));
        e.sizeY = 100.0f;
    }
    registerWidget(cm, t);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);

    // The second button covers y 100..200 before scrolling…
    CHECK(wm.processPointer(400.0f, 400.0f, 50.0f, 150.0f, true, true));
    // At the top, a notch AWAY from the user (positive, "scroll up") has
    // nowhere to go and is left for whoever else wants the wheel.
    CHECK_FALSE(wm.processWheel(400.0f, 400.0f, 50.0f, 150.0f, 1.0f));
    // Towards the user scrolls down: the content moves up by 48 units.
    CHECK(wm.processWheel(400.0f, 400.0f, 50.0f, 150.0f, -1.0f));
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);
    REQUIRE_FALSE(out.empty());
    // Everything the box emitted is clipped to it — including the parts that
    // are now scrolled out of view.
    for (const UIRenderObject& o : out)
        if (o.clipRect.z > 0.0f)
        {
            CHECK(o.clipRect.w == doctest::Approx(200.0f));
            CHECK(o.position.y >= -200.0f);
        }

    // The wheel outside the box is not consumed — it belongs to the camera.
    CHECK_FALSE(wm.processWheel(400.0f, 400.0f, 350.0f, 350.0f, -1.0f));
    // Scrolled to the end, the wheel stops being consumed too.
    for (int i = 0; i < 20; ++i) wm.processWheel(400.0f, 400.0f, 50.0f, 150.0f, -1.0f);
    CHECK_FALSE(wm.processWheel(400.0f, 400.0f, 50.0f, 150.0f, -1.0f));
}

// ── B9: the number field ─────────────────────────────────────────────────────

TEST_CASE("TextInput: the steppers step, clamp, and stay out of the text's way")
{
    HE::UITextInput ti;
    ti.sizeX = 120.0f; ti.sizeY = 24.0f;
    const HE::UIWidgetRect px{ 0.0f, 0.0f, 120.0f, 24.0f };
    HE::UIWidgetRect up{}, down{};

    // Off by default, and off means there is nothing there to click.
    CHECK_FALSE(ti.stepperRects(px, up, down));
    CHECK_FALSE(ti.applyStep(1));

    ti.steppers = true;
    REQUIRE(ti.stepperRects(px, up, down));
    // Stacked, at the right edge, inside the field.
    CHECK(up.y < down.y);
    CHECK(up.x == doctest::Approx(down.x));
    CHECK(up.x + up.w <= 120.0f);
    CHECK(up.x > 60.0f);

    // An empty field steps from zero rather than refusing to move.
    CHECK(ti.applyStep(1));
    CHECK(ti.text == "1");
    CHECK(ti.caret == ti.text.size());
    ti.step = 0.25f;
    CHECK(ti.applyStep(1));
    CHECK(ti.text == "1.25");      // no trailing zeros, and no locale comma
    CHECK(ti.applyStep(-1));
    CHECK(ti.text == "1");         // …and no decimal tail on a whole number

    // A range clamps, and a step that would not move it reports nothing.
    ti.step = 10.0f; ti.minValue = 0.0f; ti.maxValue = 5.0f;
    CHECK(ti.applyStep(1));
    CHECK(ti.text == "5");
    CHECK_FALSE(ti.applyStep(1));

    // Text that is not a number steps from zero too, rather than being kept.
    ti.text = "abc"; ti.step = 1.0f; ti.minValue = ti.maxValue = 0.0f;
    CHECK(ti.applyStep(1));
    CHECK(ti.text == "1");

    // A number field has one line, so a multiline one has no arrows.
    ti.multiline = true;
    CHECK_FALSE(ti.stepperRects(px, up, down));
    CHECK_FALSE(ti.applyStep(1));
    ti.multiline = false;

    // Written only when it is on, like every other switch on this type.
    nlohmann::json j;
    HE::UITextInput plain;
    plain.writeJson(j);
    CHECK_FALSE(j.contains("steppers"));
    j = nlohmann::json{};
    ti.step = 0.5f; ti.minValue = -2.0f; ti.maxValue = 2.0f;
    ti.writeJson(j);
    HE::UITextInput back;
    back.readJson(j);
    CHECK(back.steppers);
    CHECK(back.step == doctest::Approx(0.5f));
    CHECK(back.minValue == doctest::Approx(-2.0f));
    CHECK(back.maxValue == doctest::Approx(2.0f));
}

// ── B9: the cross that empties a field ───────────────────────────────────────

TEST_CASE("TextInput: the clear cross appears only when there is something to clear")
{
    HE::UITextInput ti;
    ti.sizeX = 200.0f; ti.sizeY = 28.0f;
    const HE::UIWidgetRect px{ 0.0f, 0.0f, 200.0f, 28.0f };
    HE::UIWidgetRect cr{};

    // Off by default, and off means nothing to click even with text in it.
    ti.text = "hello";
    CHECK_FALSE(ti.clearButtonRect(px, cr));

    ti.clearButton = true;
    REQUIRE(ti.clearButtonRect(px, cr));
    CHECK(cr.x + cr.w <= 200.0f);
    CHECK(cr.x > 100.0f);              // at the right edge, out of the way
    CHECK(cr.y >= 0.0f);
    CHECK(cr.y + cr.h <= 28.0f);

    // The three states in which there is none, each for its own reason: an
    // empty field has nothing to clear, a read-only one cannot be emptied by
    // anybody, and a multiline field is a little document.
    ti.text.clear();
    CHECK_FALSE(ti.clearButtonRect(px, cr));
    ti.text = "hello";
    ti.editable = false;
    CHECK_FALSE(ti.clearButtonRect(px, cr));
    ti.editable = true;
    ti.multiline = true;
    CHECK_FALSE(ti.clearButtonRect(px, cr));
    ti.multiline = false;

    // With arrows as well, the cross sits to their LEFT rather than on them.
    ti.steppers = true;
    HE::UIWidgetRect up{}, down{};
    REQUIRE(ti.stepperRects(px, up, down));
    REQUIRE(ti.clearButtonRect(px, cr));
    CHECK(cr.x + cr.w <= up.x);
    ti.steppers = false;

    // Clearing is ONE undo step, and it gives the whole sentence back rather
    // than its last letter.
    ti.caret = ti.selAnchor = 3;
    CHECK(ti.applyClear());
    CHECK(ti.text.empty());
    CHECK(ti.caret == 0);
    CHECK_FALSE(ti.applyClear());      // nothing left to clear
    CHECK(ti.undoEdit());
    CHECK(ti.text == "hello");

    // The text ends where the cross begins: a word running underneath it is the
    // first thing a field with one gets wrong.
    HE::UIElementRenderState st;
    std::vector<UIRenderObject> without, with;
    ti.clearButton = false;
    ti.render(px, st, {}, 1.0f, without);
    ti.clearButton = true;
    ti.render(px, st, {}, 1.0f, with);
    CHECK(with.size() > without.size());     // the cross is drawn
    // Every glyph in the version with a cross stops short of it.
    REQUIRE(ti.clearButtonRect(px, cr));
    for (const UIRenderObject& o : with)
        if (o.type == 2) CHECK(o.position.x + o.size.x <= cr.x + 0.001f);

    // Written only when it is on, like every other switch on this type.
    nlohmann::json j;
    HE::UITextInput plain;
    plain.writeJson(j);
    CHECK_FALSE(j.contains("clearButton"));
    j = nlohmann::json{};
    ti.writeJson(j);
    HE::UITextInput back;
    back.readJson(j);
    CHECK(back.clearButton);
}

TEST_CASE("A click on the clear cross empties the field and does not move the caret")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree tree;
    tree.canvasWidth = 400.0f; tree.canvasHeight = 300.0f;
    const int field = tree.add(HE::UIWidgetType::TextInput);
    {
        auto* ti = dynamic_cast<HE::UITextInput*>(tree.find(field));
        REQUIRE(ti);
        ti->posX = 40.0f; ti->posY = 40.0f; ti->sizeX = 200.0f; ti->sizeY = 28.0f;
        ti->text = "search term";
        ti->clearButton = true;
    }
    registerWidget(cm, tree, nullptr);

    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);

    // Asked of the tree rather than assumed: the field's rect is what the
    // anchors made of the numbers above, and a click aimed at a guess would
    // pass for the wrong reason.
    HE::UIWidgetRect pxr{};
    HE::UIWidgetRect cr{};
    {
        const auto* ti = dynamic_cast<const HE::UITextInput*>(
            wm.tree(id)->find(field));
        REQUIRE(ti);
        pxr = HE::uiElementRect(*wm.tree(id), *ti, nullptr);
        REQUIRE(ti->clearButtonRect(pxr, cr));
    }
    // A press somewhere in the TEXT puts the caret there, as it always did.
    wm.processPointer(400.0f, 300.0f, 60.0f, 54.0f, true, true);
    wm.processPointer(400.0f, 300.0f, 60.0f, 54.0f, false, true);
    {
        const auto* ti = dynamic_cast<const HE::UITextInput*>(
            wm.tree(id)->find(field));
        CHECK(ti->text == "search term");
    }
    // …and a press on the cross empties it instead.
    wm.processPointer(400.0f, 300.0f, cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f, true, true);
    wm.processPointer(400.0f, 300.0f, cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f, false, true);
    {
        const auto* ti = dynamic_cast<const HE::UITextInput*>(
            wm.tree(id)->find(field));
        REQUIRE(ti);
        CHECK(ti->text.empty());
        // The caret went with the text. Left where it was it would point past
        // the end of a field that no longer has one.
        CHECK(ti->caret == 0);
        CHECK(ti->selAnchor == 0);
        // …and there is no cross any more, because there is nothing to clear.
        HE::UIWidgetRect gone{};
        CHECK_FALSE(ti->clearButtonRect(pxr, gone));
    }

    // The promise the shipped component's help text makes: Ctrl+Z brings the
    // words back. Checked through the manager and not only through applyClear,
    // because undo walks the FOCUSED field's history — a press that cleared the
    // field but left the focus somewhere else would take the sentence away with
    // no way back to it.
    CHECK(wm.undoFocusedText());
    {
        const auto* ti = dynamic_cast<const HE::UITextInput*>(
            wm.tree(id)->find(field));
        REQUIRE(ti);
        CHECK(ti->text == "search term");
    }
}

// ── B9: a bar with no end to report ──────────────────────────────────────────

TEST_CASE("ProgressBar: indeterminate slides instead of filling")
{
    HE::UIProgressBar pb;
    pb.sizeX = 200.0f; pb.sizeY = 20.0f;
    pb.value = 0.25f;
    const HE::UIWidgetRect px{ 0.0f, 0.0f, 200.0f, 20.0f };

    // Determinate: the track, then a fill that IS the fraction.
    HE::UIElementRenderState st;
    std::vector<UIRenderObject> det;
    pb.render(px, st, {}, 1.0f, det);
    REQUIRE(det.size() == 2);
    CHECK(det[1].size.x == doctest::Approx(50.0f));
    CHECK(det[1].position.x == doctest::Approx(0.0f));

    // Indeterminate: a segment a third of the track wide, and where it is
    // depends on the clock and on nothing else.
    pb.indeterminate = true;
    std::vector<UIRenderObject> early, later;
    st.time = 0.5f;
    pb.render(px, st, {}, 1.0f, early);
    st.time = 0.9f;
    pb.render(px, st, {}, 1.0f, later);
    REQUIRE(early.size() == 2);
    REQUIRE(later.size() == 2);
    CHECK(early[1].size.x == doctest::Approx(200.0f * 0.33f));
    CHECK(later[1].position.x > early[1].position.x);
    // Never outside its own track, at any moment of the cycle: an element's own
    // quads are not covered by clipChildren.
    for (float t = 0.0f; t < 3.0f; t += 0.05f)
    {
        std::vector<UIRenderObject> o;
        st.time = t;
        pb.render(px, st, {}, 1.0f, o);
        for (const UIRenderObject& q : o)
        {
            CHECK(q.position.x >= -0.001f);
            CHECK(q.position.x + q.size.x <= 200.001f);
        }
    }

    // The value it was showing before is still there when it comes back: a
    // graph writing progress into a bar nobody has switched over yet must not
    // lose the writes.
    CHECK(pb.value == doctest::Approx(0.25f));
    // And it only reaches the file when it is on, so every bar authored before
    // this saves byte for byte what it saved yesterday.
    nlohmann::json j;
    HE::UIProgressBar plain;
    plain.writeJson(j);
    CHECK_FALSE(j.contains("indeterminate"));
    j = nlohmann::json{};
    pb.writeJson(j);
    CHECK(j.value("indeterminate", false));
    HE::UIProgressBar back;
    back.readJson(j);
    CHECK(back.indeterminate);
}

TEST_CASE("CheckBox: the switch is the same value with a second picture")
{
    HE::UICheckBox cb;
    cb.sizeX = 200.0f; cb.sizeY = 28.0f; cb.label.clear();
    const HE::UIWidgetRect px{ 0.0f, 0.0f, 200.0f, 28.0f };
    const HE::UIElementRenderState st;

    // A box is a square; a switch is a track and a knob, and the track is
    // wider than the box would have been.
    std::vector<UIRenderObject> box;
    cb.render(px, st, {}, 1.0f, box);
    REQUIRE(box.size() == 1);            // unchecked box: no tick
    const float boxW = box[0].size.x;

    cb.switchStyle = true;
    std::vector<UIRenderObject> off;
    cb.render(px, st, {}, 1.0f, off);
    REQUIRE(off.size() == 2);            // track + knob, in both states
    CHECK(off[0].size.x == doctest::Approx(boxW * 1.8f));
    CHECK(off[0].cornerRadius.x == doctest::Approx(off[0].size.y * 0.5f));

    cb.checked = true;
    std::vector<UIRenderObject> on;
    cb.render(px, st, {}, 1.0f, on);
    REQUIRE(on.size() == 2);
    // The knob is at the far end and the track has taken the check colour: the
    // TRACK carries the state, which reads from across the room.
    CHECK(on[1].position.x > off[1].position.x);
    CHECK(on[0].color.g == doctest::Approx(cb.checkColor.g));
    // The knob is the one part that does not change colour.
    CHECK(on[1].color.r == doctest::Approx(off[1].color.r));

    // Same rule as the spinner's: only written when it is on.
    nlohmann::json j;
    HE::UICheckBox plain;
    plain.writeJson(j);
    CHECK_FALSE(j.contains("switch"));
    j = nlohmann::json{};
    cb.writeJson(j);
    HE::UICheckBox back;
    back.readJson(j);
    CHECK(back.switchStyle);
}

// ── B9: the glide, and the bar as a handle ───────────────────────────────────
// Four buttons of 100 in a box of 200: the content is 400 units tall, so it can
// be scrolled by 200. The same shape as the wheel test above, and the two things
// the wheel test does not ask about.

namespace {

// The box the two cases below share: 200×200 at the origin, no padding, four
// 100-tall buttons in it.
int makeScrollFixture(ContentManager& cm, WidgetManager& wm, int& boxOut)
{
    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int box = t.add(HE::UIWidgetType::ScrollBox);
    { HE::UIElement& e = *t.find(box);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 200.0f;
      e.setProp("Padding", HE::UIPropValue::ofFloat(0.0f));
      e.setProp("Spacing", HE::UIPropValue::ofFloat(0.0f)); }
    for (int i = 0; i < 4; ++i)
    {
        const int b = t.add(HE::UIWidgetType::Button);
        HE::UIElement& e = *t.find(b);
        e.parentId = box;
        e.setProp("Text", HE::UIPropValue::ofString(""));
        e.sizeY = 100.0f;
    }
    registerWidget(cm, t);
    boxOut = box;
    return createShown(wm, cm, "mem://w.hasset");
}

float scrollOf(WidgetManager& wm, int widget, int box)
{
    auto* live = const_cast<HE::UIWidgetTree*>(wm.tree(widget));
    if (!live) return -1.0f;
    HE::UIElement* e = live->find(box);
    const float* off = e ? e->scrollOffsetPtr() : nullptr;
    return off ? *off : -1.0f;
}

} // namespace

TEST_CASE("WidgetManager: a notch keeps moving after the wheel has stopped")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    WidgetManager wm;
    int box = 0;
    const int wid = makeScrollFixture(cm, wm, box);
    REQUIRE(wid != 0);

    // The notch moves it NOW — a wheel that only started a glide would feel
    // like lag, and this is the number the wheel test above pins.
    CHECK(wm.processWheel(400.0f, 400.0f, 50.0f, 150.0f, -1.0f));
    CHECK(scrollOf(wm, wid, box) == doctest::Approx(48.0f));
    // …and it is still moving with nobody touching the machine, which is what
    // an event-driven application has to be told so it does not fall asleep in
    // the middle of the throw.
    CHECK(wm.isAnimating());

    wm.tick(1.0f / 60.0f);
    const float afterOne = scrollOf(wm, wid, box);
    CHECK(afterOne > 48.0f);

    // A second of frames is far longer than the damping takes: it settles, the
    // entry is swept, and nothing asks for another frame.
    for (int i = 0; i < 60; ++i) wm.tick(1.0f / 60.0f);
    const float settled = scrollOf(wm, wid, box);
    CHECK(settled > afterOne);
    CHECK(settled < 100.0f);          // a glide, not a jump to the end
    CHECK_FALSE(wm.isAnimating());
    wm.tick(1.0f / 60.0f);
    CHECK(scrollOf(wm, wid, box) == doctest::Approx(settled));
}

TEST_CASE("WidgetManager: the scrollbar thumb is a handle, not a picture")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    WidgetManager wm;
    int box = 0;
    const int wid = makeScrollFixture(cm, wm, box);
    REQUIRE(wid != 0);

    // One frame first. How tall the content is is a MEASUREMENT the layout
    // takes, so a box that has never been drawn does not know it has anything
    // to scroll — and a bar nobody has drawn yet is nothing to grab either.
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);

    // The bar sits in the last 6 units of the box's width, and the thumb is
    // half the track (200 of 400 units of content are in view), so it covers
    // y 0..100 while the box is at the top.
    const HE::UIWidgetRect px{ 0.0f, 0.0f, 200.0f, 200.0f };
    HE::UIWidgetRect th{};
    const auto* live = wm.tree(wid);
    REQUIRE(live != nullptr);
    REQUIRE(HE::uiScrollThumbRect(*live->find(box), px, th));
    CHECK(th.x == doctest::Approx(194.0f));
    CHECK(th.w == doctest::Approx(6.0f));
    CHECK(th.y == doctest::Approx(0.0f));
    CHECK(th.h == doctest::Approx(100.0f));

    // Take hold of it 50 pixels below its top, then pull down by 50. The thumb
    // top follows the pointer minus that grab, and the offset that puts it at
    // 50 in a 100-pixel travel is half of the 200 units it can scroll.
    wm.processPointer(400.0f, 400.0f, 197.0f, 50.0f, true, true);
    wm.processPointer(400.0f, 400.0f, 197.0f, 100.0f, true, true);
    CHECK(scrollOf(wm, wid, box) == doctest::Approx(100.0f));

    // Past the end of the track is the end of the track, not past it.
    wm.processPointer(400.0f, 400.0f, 197.0f, 900.0f, true, true);
    CHECK(scrollOf(wm, wid, box) == doctest::Approx(200.0f));

    // Letting go leaves it exactly where the hand put it: a bar that threw the
    // list on afterwards would undo the aim the drag just took.
    wm.processPointer(400.0f, 400.0f, 197.0f, 900.0f, false, true);
    CHECK_FALSE(wm.isAnimating());
    for (int i = 0; i < 10; ++i) wm.tick(1.0f / 60.0f);
    CHECK(scrollOf(wm, wid, box) == doctest::Approx(200.0f));

    // And the pointer moving on with the button up does not still scroll it.
    wm.processPointer(400.0f, 400.0f, 197.0f, 20.0f, false, true);
    CHECK(scrollOf(wm, wid, box) == doctest::Approx(200.0f));
}

// ═══ Opacity + enabled ═══════════════════════════════════════════════════════
// Fading a menu meant animating every colour in it, and a button could only be
// "disabled" by a script remembering not to react. Both are inherited now: one
// value on the root panel decides for the whole subtree.

TEST_CASE("uiElementEffectiveOpacity: the chain multiplies, and clamps")
{
    HE::UIWidgetTree t;
    const int root  = t.add(HE::UIWidgetType::Panel);
    const int mid   = t.add(HE::UIWidgetType::Panel);
    const int leaf  = t.add(HE::UIWidgetType::Text);
    t.find(mid)->parentId  = root;
    t.find(leaf)->parentId = mid;

    CHECK(HE::uiElementEffectiveOpacity(t, *t.find(leaf)) == doctest::Approx(1.0f));
    t.find(root)->renderOpacity = 0.5f;
    t.find(mid)->renderOpacity  = 0.5f;
    CHECK(HE::uiElementEffectiveOpacity(t, *t.find(root)) == doctest::Approx(0.5f));
    CHECK(HE::uiElementEffectiveOpacity(t, *t.find(mid))  == doctest::Approx(0.25f));
    CHECK(HE::uiElementEffectiveOpacity(t, *t.find(leaf)) == doctest::Approx(0.25f));

    // Values outside 0..1 cannot brighten anything.
    t.find(root)->renderOpacity = 4.0f;
    t.find(mid)->renderOpacity  = -1.0f;
    CHECK(HE::uiElementEffectiveOpacity(t, *t.find(leaf)) == doctest::Approx(0.0f));
}

TEST_CASE("uiElementEffectiveEnabled: a disabled ancestor disables the subtree")
{
    HE::UIWidgetTree t;
    const int root = t.add(HE::UIWidgetType::Panel);
    const int leaf = t.add(HE::UIWidgetType::Button);
    t.find(leaf)->parentId = root;

    CHECK(HE::uiElementEffectiveEnabled(t, *t.find(leaf)));
    t.find(root)->enabled = false;
    CHECK_FALSE(HE::uiElementEffectiveEnabled(t, *t.find(root)));
    CHECK_FALSE(HE::uiElementEffectiveEnabled(t, *t.find(leaf)));
    t.find(root)->enabled = true;
    t.find(leaf)->enabled = false;
    CHECK(HE::uiElementEffectiveEnabled(t, *t.find(root)));   // …and only downward
    CHECK_FALSE(HE::uiElementEffectiveEnabled(t, *t.find(leaf)));
}

TEST_CASE("WidgetManager: opacity fades the quads, disabled dims and deadens them")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 200.0f; t.canvasHeight = 200.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int root = t.add(HE::UIWidgetType::Panel);
    { HE::UIElement& e = *t.find(root);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = e.posY = 0.0f; e.sizeX = e.sizeY = 200.0f;
      e.setProp("Color", HE::UIPropValue::ofColor({ 1.0f, 1.0f, 1.0f, 1.0f })); }
    const int btn = t.add(HE::UIWidgetType::Button);
    { HE::UIElement& e = *t.find(btn);
      e.parentId = root;
      e.setProp("Text", HE::UIPropValue::ofString(""));
      e.setProp("Normal Color", HE::UIPropValue::ofColor({ 1.0f, 1.0f, 1.0f, 1.0f }));
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = e.posY = 0.0f; e.sizeX = e.sizeY = 100.0f; }
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);

    std::vector<UIRenderObject> out;
    wm.extract(200.0f, 200.0f, out);
    REQUIRE(out.size() >= 2);
    for (const UIRenderObject& o : out)
    {
        CHECK(o.color.a == doctest::Approx(1.0f));
        CHECK(o.color.r == doctest::Approx(1.0f));
    }
    CHECK(wm.processPointer(200.0f, 200.0f, 50.0f, 50.0f, true, true));  // button answers

    // Half opacity on the ROOT reaches the button's own quads: the fade is
    // inherited, not a property of the panel that happens to be behind them.
    t.find(root)->renderOpacity = 0.5f;
    registerWidget(cm, t);
    WidgetManager faded;
    REQUIRE(createShown(faded, cm, "mem://w.hasset") != 0);
    out.clear();
    faded.extract(200.0f, 200.0f, out);
    REQUIRE(out.size() >= 2);
    for (const UIRenderObject& o : out)
    {
        CHECK(o.color.a == doctest::Approx(0.5f));
        CHECK(o.color.r == doctest::Approx(1.0f));   // faded, not darkened
    }
    // Opacity alone does not stop the clicks — only 0 does.
    CHECK(faded.processPointer(200.0f, 200.0f, 50.0f, 50.0f, true, true));

    t.find(root)->renderOpacity = 0.0f;
    registerWidget(cm, t);
    WidgetManager gone;
    REQUIRE(createShown(gone, cm, "mem://w.hasset") != 0);
    CHECK_FALSE(gone.processPointer(200.0f, 200.0f, 50.0f, 50.0f, true, true));

    // Disabled on the root: dimmed instead of faded, and inert all the way down.
    t.find(root)->renderOpacity = 1.0f;
    t.find(root)->enabled = false;
    registerWidget(cm, t);
    WidgetManager off;
    REQUIRE(createShown(off, cm, "mem://w.hasset") != 0);
    out.clear();
    off.extract(200.0f, 200.0f, out);
    REQUIRE(out.size() >= 2);
    for (const UIRenderObject& o : out)
    {
        CHECK(o.color.a == doctest::Approx(1.0f));                 // still opaque
        CHECK(o.color.r == doctest::Approx(HE::kUIDisabledDim));   // …but knocked back
    }
    CHECK_FALSE(off.processPointer(200.0f, 200.0f, 50.0f, 50.0f, true, true));
}

TEST_CASE("UIElement: opacity and enabled round-trip and are scriptable")
{
    HE::UIWidgetTree t;
    const int p = t.add(HE::UIWidgetType::Panel);
    HE::UIElement& e = *t.find(p);
    CHECK(e.enabled);
    CHECK(e.renderOpacity == doctest::Approx(1.0f));
    // Defaults write nothing — an existing widget saves byte-identical.
    CHECK(HE::uiWidgetTreeToJson(t).find("renderOpacity") == std::string::npos);
    CHECK(HE::uiWidgetTreeToJson(t).find("enabled") == std::string::npos);

    e.renderOpacity = 0.25f;
    e.enabled = false;
    HE::UIWidgetTree r;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), r));
    CHECK(r.find(p)->renderOpacity == doctest::Approx(0.25f));
    CHECK_FALSE(r.find(p)->enabled);

    // By name, and clamped where a script hands over nonsense.
    e.setPropAny("Render Opacity", HE::UIPropValue::ofFloat(2.0f));
    CHECK(e.renderOpacity == doctest::Approx(1.0f));
    e.setPropAny("Render Opacity", HE::UIPropValue::ofFloat(-3.0f));
    CHECK(e.renderOpacity == doctest::Approx(0.0f));
    e.setPropAny("Enabled", HE::UIPropValue::ofBool(true));
    CHECK(e.getPropAny("Enabled").b);
    CHECK(e.clone()->renderOpacity == doctest::Approx(0.0f));
}

// ═══ Rotation ════════════════════════════════════════════════════════════════
// A render transform: layout stays unrotated (a tilted element does not shove
// its neighbours around) and the finished rect is turned about the pivot. It is
// inherited, and a chain of rotations about different points is again ONE
// rotation about a point — which is why two numbers per quad are enough.

TEST_CASE("uiElementRotation: nothing rotating costs nothing")
{
    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    const int p = t.add(HE::UIWidgetType::Panel);
    const int c = t.add(HE::UIWidgetType::Panel);
    t.find(c)->parentId = p;

    HE::UIRotation r;
    CHECK_FALSE(HE::uiElementRotation(t, *t.find(c), r));
    CHECK_FALSE(HE::uiElementRotation(t, *t.find(p), r));
}

TEST_CASE("uiElementRotation: a quarter turn about the pivot, and back")
{
    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    const int e = t.add(HE::UIWidgetType::Panel);
    HE::UIElement& el = *t.find(e);
    HE::uiSetAnchorPreset(el, 0);
    el.pivotX = el.pivotY = 0.0f;          // top-left corner is the centre of the turn
    el.posX = 100.0f; el.posY = 100.0f; el.sizeX = 200.0f; el.sizeY = 100.0f;
    el.rotation = 90.0f;

    HE::UIRotation r;
    REQUIRE(HE::uiElementRotation(t, el, r));
    CHECK(r.degrees == doctest::Approx(90.0f));
    // Nothing above it rotates, so the pivot stays where it is.
    CHECK(r.srcX == doctest::Approx(100.0f));
    CHECK(r.srcY == doctest::Approx(100.0f));
    CHECK(r.dstX == doctest::Approx(r.srcX));
    CHECK(r.dstY == doctest::Approx(r.srcY));

    // A point 200 to the right of the pivot ends up 200 BELOW it (clockwise, y
    // down) — and unrotating that point gives the original back.
    float ux = 0.0f, uy = 0.0f;
    HE::uiUnrotatePoint(r, 100.0f, 300.0f, ux, uy);
    CHECK(ux == doctest::Approx(300.0f));
    CHECK(uy == doctest::Approx(100.0f));
}

TEST_CASE("uiElementRotation: it is inherited, and the angles add up")
{
    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    const int outer = t.add(HE::UIWidgetType::Panel);
    { HE::UIElement& e = *t.find(outer);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 200.0f;
      e.rotation = 30.0f; }
    const int inner = t.add(HE::UIWidgetType::Panel);
    { HE::UIElement& e = *t.find(inner);
      e.parentId = outer;
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 50.0f; e.posY = 0.0f; e.sizeX = 50.0f; e.sizeY = 50.0f;
      e.rotation = 60.0f; }

    HE::UIRotation r;
    REQUIRE(HE::uiElementRotation(t, *t.find(inner), r));
    CHECK(r.degrees == doctest::Approx(90.0f));          // 30 + 60
    // Its own pivot is at (50, 0) unrotated; the parent's 30° about the origin
    // carries it to (50cos30, 50sin30).
    CHECK(r.srcX == doctest::Approx(50.0f));
    CHECK(r.srcY == doctest::Approx(0.0f));
    CHECK(r.dstX == doctest::Approx(50.0f * std::cos(30.0f * 3.14159265f / 180.0f)));
    CHECK(r.dstY == doctest::Approx(50.0f * std::sin(30.0f * 3.14159265f / 180.0f)));

    // A child that does not rotate itself still inherits the parent's turn.
    const int plain = t.add(HE::UIWidgetType::Panel);
    { HE::UIElement& e = *t.find(plain);
      e.parentId = outer; e.rotation = 0.0f; }
    HE::UIRotation r2;
    REQUIRE(HE::uiElementRotation(t, *t.find(plain), r2));
    CHECK(r2.degrees == doctest::Approx(30.0f));

    // Layout itself is untouched: the rect is the unrotated one.
    const HE::UIWidgetRect rect = HE::uiElementRect(t, *t.find(inner));
    CHECK(rect.x == doctest::Approx(50.0f));
    CHECK(rect.y == doctest::Approx(0.0f));
}

TEST_CASE("WidgetManager: rotation reaches the quads and the pointer follows it")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int btn = t.add(HE::UIWidgetType::Button);
    HE::UIElement& e = *t.find(btn);
    e.setProp("Text", HE::UIPropValue::ofString(""));
    HE::uiSetAnchorPreset(e, 0);
    e.pivotX = e.pivotY = 0.0f;
    e.posX = 100.0f; e.posY = 100.0f; e.sizeX = 200.0f; e.sizeY = 40.0f;
    registerWidget(cm, t);

    WidgetManager upright;
    REQUIRE(createShown(upright, cm, "mem://w.hasset") != 0);
    std::vector<UIRenderObject> out;
    upright.extract(400.0f, 400.0f, out);
    REQUIRE_FALSE(out.empty());
    for (const UIRenderObject& o : out) CHECK(o.rotation == doctest::Approx(0.0f));
    // A wide, short button covering x 100..300, y 100..140. This point is
    // inside it upright…
    CHECK(upright.processPointer(400.0f, 400.0f, 280.0f, 120.0f, true, true));
    // …and this one is not.
    CHECK_FALSE(upright.processPointer(400.0f, 400.0f, 80.0f, 280.0f, true, true));

    // Turned a quarter about its top-left corner, both statements swap.
    e.rotation = 90.0f;
    registerWidget(cm, t);
    WidgetManager turned;
    REQUIRE(createShown(turned, cm, "mem://w.hasset") != 0);
    out.clear();
    turned.extract(400.0f, 400.0f, out);
    REQUIRE_FALSE(out.empty());
    bool anyRotated = false;
    for (const UIRenderObject& o : out)
        if (o.rotation != 0.0f)
        {
            anyRotated = true;
            CHECK(o.rotation == doctest::Approx(90.0f * 3.14159265f / 180.0f));
            CHECK(o.rotationPivot.x == doctest::Approx(100.0f));
            CHECK(o.rotationPivot.y == doctest::Approx(100.0f));
        }
    CHECK(anyRotated);
    // Turned about its top-left corner it now covers x 60..100, y 100..300 —
    // so the two statements above swap over.
    CHECK_FALSE(turned.processPointer(400.0f, 400.0f, 280.0f, 120.0f, true, true));
    CHECK(turned.processPointer(400.0f, 400.0f, 80.0f, 280.0f, true, true));
}

TEST_CASE("UIElement: rotation round-trips and is scriptable")
{
    HE::UIWidgetTree t;
    const int p = t.add(HE::UIWidgetType::Panel);
    CHECK(HE::uiWidgetTreeToJson(t).find("rotation") == std::string::npos);

    t.find(p)->rotation = 45.0f;
    HE::UIWidgetTree r;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), r));
    CHECK(r.find(p)->rotation == doctest::Approx(45.0f));

    HE::UIElement& e = *t.find(p);
    e.setPropAny("Rotation", HE::UIPropValue::ofFloat(-90.0f));
    CHECK(e.getPropAny("Rotation").f == doctest::Approx(-90.0f));
    CHECK(e.clone()->rotation == doctest::Approx(-90.0f));
}

// ═══ 9-slice ═════════════════════════════════════════════════════════════════
// One 64x64 texture has to be able to be a frame, a button and a panel at any
// size. Stretched as a single quad it smears; sliced, the corners keep their
// pixel size and only what is between them grows.

TEST_CASE("UIImage 9-slice: nine pieces, corners at their source size")
{
    HE::UIImage img;
    img.textureAssetId = HE::UUID{ 1, 2 };
    img.textureW = img.textureH = 64;
    img.sliceLeft = img.sliceTop = img.sliceRight = img.sliceBottom = 16.0f;

    std::vector<UIRenderObject> out;
    img.render({ 0.0f, 0.0f, 200.0f, 100.0f }, {}, HE::UUID{}, 1.0f, out);
    REQUIRE(out.size() == 9);

    // Top-left corner: 16x16 of destination, and the top-left quarter of the
    // source in UVs.
    const UIRenderObject& tl = out[0];
    CHECK(tl.position.x == doctest::Approx(0.0f));
    CHECK(tl.size.x == doctest::Approx(16.0f));
    CHECK(tl.size.y == doctest::Approx(16.0f));
    CHECK(tl.uvMin.x == doctest::Approx(0.0f));
    CHECK(tl.uvMax.x == doctest::Approx(0.25f));
    CHECK(tl.uvMax.y == doctest::Approx(0.25f));

    // The top edge between the corners stretches on X only.
    const UIRenderObject& top = out[1];
    CHECK(top.position.x == doctest::Approx(16.0f));
    CHECK(top.size.x == doctest::Approx(168.0f));   // 200 - 16 - 16
    CHECK(top.size.y == doctest::Approx(16.0f));
    CHECK(top.uvMin.x == doctest::Approx(0.25f));
    CHECK(top.uvMax.x == doctest::Approx(0.75f));

    // Bottom-right corner sits at the far end, still 16x16.
    const UIRenderObject& br = out[8];
    CHECK(br.position.x == doctest::Approx(184.0f));
    CHECK(br.position.y == doctest::Approx(84.0f));
    CHECK(br.size.x == doctest::Approx(16.0f));
    CHECK(br.uvMax.x == doctest::Approx(1.0f));

    // Growing the element does not grow the corners — that is the whole point.
    out.clear();
    img.render({ 0.0f, 0.0f, 600.0f, 400.0f }, {}, HE::UUID{}, 1.0f, out);
    REQUIRE(out.size() == 9);
    CHECK(out[0].size.x == doctest::Approx(16.0f));
    CHECK(out[8].size.x == doctest::Approx(16.0f));
    CHECK(out[4].size.x == doctest::Approx(568.0f));   // the middle took the rest
}

TEST_CASE("UIImage 9-slice: the degenerate cases stay sane")
{
    HE::UIImage img;
    img.textureAssetId = HE::UUID{ 1, 2 };
    img.textureW = img.textureH = 64;
    std::vector<UIRenderObject> out;

    // No margins → one stretched quad, exactly as before slicing existed.
    img.render({ 0.0f, 0.0f, 200.0f, 100.0f }, {}, HE::UUID{}, 1.0f, out);
    CHECK(out.size() == 1);

    // Margins but no texture size known (not loaded yet) → still one quad.
    out.clear();
    img.sliceLeft = img.sliceRight = 16.0f;
    img.textureW = img.textureH = 0;
    img.render({ 0.0f, 0.0f, 200.0f, 100.0f }, {}, HE::UUID{}, 1.0f, out);
    CHECK(out.size() == 1);

    // Margins wider than the element: they shrink together instead of
    // overlapping into a flipped corner, and the middle collapses.
    out.clear();
    img.textureW = img.textureH = 64;
    img.sliceLeft = img.sliceRight = 80.0f;   // 160 across a 100-wide element
    img.render({ 0.0f, 0.0f, 100.0f, 40.0f }, {}, HE::UUID{}, 1.0f, out);
    REQUIRE_FALSE(out.empty());
    for (const UIRenderObject& o : out)
    {
        CHECK(o.size.x >= 0.0f);
        CHECK(o.size.y >= 0.0f);
        CHECK(o.position.x >= -0.001f);
        CHECK(o.position.x + o.size.x <= doctest::Approx(100.0f));
    }

    // A frame: the middle piece is left out.
    out.clear();
    img.sliceLeft = img.sliceTop = img.sliceRight = img.sliceBottom = 16.0f;
    img.sliceFillCentre = false;
    img.render({ 0.0f, 0.0f, 200.0f, 100.0f }, {}, HE::UUID{}, 1.0f, out);
    CHECK(out.size() == 8);
}

TEST_CASE("UIImage 9-slice: the margins round-trip, an unsliced image writes none")
{
    HE::UIWidgetTree t;
    const int id = t.add(HE::UIWidgetType::Image);
    CHECK(HE::uiWidgetTreeToJson(t).find("slice") == std::string::npos);

    auto* img = dynamic_cast<HE::UIImage*>(t.find(id));
    REQUIRE(img != nullptr);
    img->sliceLeft = 4.0f; img->sliceTop = 5.0f;
    img->sliceRight = 6.0f; img->sliceBottom = 7.0f;
    img->sliceFillCentre = false;

    HE::UIWidgetTree r;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), r));
    auto* back = dynamic_cast<HE::UIImage*>(r.find(id));
    REQUIRE(back != nullptr);
    CHECK(back->sliceLeft == doctest::Approx(4.0f));
    CHECK(back->sliceBottom == doctest::Approx(7.0f));
    CHECK_FALSE(back->sliceFillCentre);
    // The source size is transient: it is measured, not authored.
    CHECK(back->textureW == 0);
}

// ═══ Clipping ════════════════════════════════════════════════════════════════
// "Clip children" cuts a subtree off at the clipping element's own rect. It is
// what a list longer than its box, a text wider than its field and eventually a
// scroll box all rest on — and it has to hold for the POINTER too, or half a
// row is invisible but still clickable.

TEST_CASE("uiElementClipRect: only a clipping ancestor clips, and they intersect")
{
    HE::UIWidgetTree t;
    t.canvasWidth = 1000.0f; t.canvasHeight = 1000.0f;

    auto addBox = [&](int parent, float x, float y, float w, float h, bool clip)
    {
        const int id = t.add(HE::UIWidgetType::Panel);
        HE::UIElement& e = *t.find(id);
        e.parentId = parent;
        HE::uiSetAnchorPreset(e, 0);
        e.pivotX = e.pivotY = 0.0f;
        e.posX = x; e.posY = y; e.sizeX = w; e.sizeY = h;
        e.clipChildren = clip;
        return id;
    };

    const int outer = addBox(0,     0.0f,   0.0f, 400.0f, 400.0f, true);
    const int inner = addBox(outer, 100.0f, 0.0f, 400.0f, 200.0f, true);
    const int leaf  = addBox(inner, 0.0f,   0.0f, 400.0f, 400.0f, false);
    const int loose = addBox(0,     0.0f,   0.0f, 100.0f, 100.0f, false);

    HE::UIWidgetRect c{};
    // Nothing above it clips → not clipped at all.
    CHECK_FALSE(HE::uiElementClipRect(t, *t.find(loose), c));
    // An element's OWN flag never clips itself.
    CHECK_FALSE(HE::uiElementClipRect(t, *t.find(outer), c));
    // One clipping ancestor: its rect.
    REQUIRE(HE::uiElementClipRect(t, *t.find(inner), c));
    CHECK(c.x == doctest::Approx(0.0f));
    CHECK(c.w == doctest::Approx(400.0f));
    // Two of them: the intersection, not the nearest one.
    REQUIRE(HE::uiElementClipRect(t, *t.find(leaf), c));
    CHECK(c.x == doctest::Approx(100.0f));         // inner starts at 100…
    CHECK(c.w == doctest::Approx(300.0f));         // …and outer ends at 400
    CHECK(c.y == doctest::Approx(0.0f));
    CHECK(c.h == doctest::Approx(200.0f));         // inner is the shorter one
}

TEST_CASE("uiElementClipRect: disjoint clippers hide the element entirely")
{
    HE::UIWidgetTree t;
    t.canvasWidth = 1000.0f; t.canvasHeight = 1000.0f;
    auto addBox = [&](int parent, float x, float w, bool clip)
    {
        const int id = t.add(HE::UIWidgetType::Panel);
        HE::UIElement& e = *t.find(id);
        e.parentId = parent;
        HE::uiSetAnchorPreset(e, 0);
        e.pivotX = e.pivotY = 0.0f;
        e.posX = x; e.posY = 0.0f; e.sizeX = w; e.sizeY = 100.0f;
        e.clipChildren = clip;
        return id;
    };
    const int a = addBox(0, 0.0f,   100.0f, true);
    const int b = addBox(a, 300.0f, 100.0f, true);   // starts past a's right edge
    const int c = addBox(b, 0.0f,   100.0f, false);

    HE::UIWidgetRect r{};
    REQUIRE(HE::uiElementClipRect(t, *t.find(c), r));
    CHECK(r.w <= 0.0f);     // nothing survives → the caller drops the element
}

TEST_CASE("WidgetManager: clipped quads carry the scissor, clipped pixels are dead")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 1000.0f; t.canvasHeight = 1000.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;   // 1 unit = 1 pixel

    const int box = t.add(HE::UIWidgetType::Panel);
    { HE::UIElement& e = *t.find(box);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 100.0f;
      e.clipChildren = true; }

    // A button twice as tall as the box it sits in: the lower half is cut off.
    const int btn = t.add(HE::UIWidgetType::Button);
    { HE::UIElement& e = *t.find(btn);
      e.parentId = box;
      e.setProp("Text", HE::UIPropValue::ofString(""));
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 200.0f; }

    // …and one entirely below it, which must not be drawn at all.
    const int gone = t.add(HE::UIWidgetType::Panel);
    { HE::UIElement& e = *t.find(gone);
      e.parentId = box;
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 0.0f; e.posY = 400.0f; e.sizeX = 50.0f; e.sizeY = 50.0f; }

    registerWidget(cm, t);
    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);

    std::vector<UIRenderObject> out;
    wm.extract(1000.0f, 1000.0f, out);

    int clipped = 0, unclipped = 0;
    for (const UIRenderObject& o : out)
    {
        if (o.clipRect.z > 0.0f)
        {
            ++clipped;
            CHECK(o.clipRect.x == doctest::Approx(0.0f));
            CHECK(o.clipRect.y == doctest::Approx(0.0f));
            CHECK(o.clipRect.z == doctest::Approx(200.0f));
            CHECK(o.clipRect.w == doctest::Approx(100.0f));
        }
        else ++unclipped;
    }
    CHECK(clipped   >= 1);   // the button's quads inherited the box's rect
    CHECK(unclipped >= 1);   // the box itself is not clipped by anything

    // An element completely outside its clipper is not emitted at all: the
    // panel below the box would have been one more unclipped-looking quad.
    for (const UIRenderObject& o : out)
        CHECK(o.position.y < 400.0f);

    // The pointer obeys the same cut: inside the box the button answers, in the
    // half that is clipped away it does not.
    CHECK(wm.processPointer(1000.0f, 1000.0f, 100.0f,  50.0f, true, true));
    CHECK_FALSE(wm.processPointer(1000.0f, 1000.0f, 100.0f, 150.0f, true, true));
}

TEST_CASE("UIElement: clipChildren round-trips and is scriptable")
{
    HE::UIWidgetTree t;
    const int p = t.add(HE::UIWidgetType::Panel);
    CHECK_FALSE(t.find(p)->clipChildren);              // off by default
    CHECK(HE::uiWidgetTreeToJson(t).find("clipChildren") == std::string::npos);

    t.find(p)->clipChildren = true;
    HE::UIWidgetTree r;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), r));
    CHECK(r.find(p)->clipChildren);

    // Reachable by name, like every other base property (HorizonCode, scripts).
    HE::UIElement& e = *t.find(p);
    e.setPropAny("Clip Children", HE::UIPropValue::ofBool(false));
    CHECK_FALSE(e.clipChildren);
    CHECK_FALSE(e.getPropAny("Clip Children").b);
    e.setPropAny("Clip Children", HE::UIPropValue::ofBool(true));
    CHECK(e.getPropAny("Clip Children").b);
    // …and a clone carries it (the runtime holds a deep copy).
    CHECK(e.clone()->clipChildren);
}

TEST_CASE("uiApplyAutoSize: content does not resize an axis the anchor stretches")
{
    HE::UIWidgetTree t;
    const int p = panelTree(t);

    const int fixed = t.add(HE::UIWidgetType::Text);
    { HE::UIElement& e = *t.find(fixed);
      e.parentId = p;
      HE::uiSetAnchorPreset(e, 0);
      e.setProp("Text", HE::UIPropValue::ofString("a short label"));
      e.setProp("AutoSize", HE::UIPropValue::ofBool(true)); }

    const int spanning = t.add(HE::UIWidgetType::Text);
    { HE::UIElement& e = *t.find(spanning);
      e.parentId = p;
      HE::uiSetAnchorPreset(e, 3);                 // the parent's whole top side
      HE::uiSetAnchorInsetsX(e, 0.0f, 0.0f);
      e.setProp("Text", HE::UIPropValue::ofString("a short label"));
      e.setProp("AutoSize", HE::UIPropValue::ofBool(true)); }

    HE::uiApplyAutoSize(t);

    // The point-anchored one fits its text, as it always did…
    const HE::UIWidgetRect a = HE::uiElementRect(t, *t.find(fixed));
    CHECK(a.w > 1.0f);
    CHECK(a.w < 400.0f);
    // …the anchored-to-a-side one keeps the side's width. Measured against its
    // parent rather than a fixed number, so the font never decides the case.
    const HE::UIWidgetRect b = HE::uiElementRect(t, *t.find(spanning));
    const HE::UIWidgetRect pr = HE::uiElementRect(t, *t.find(p));
    CHECK(b.w == doctest::Approx(pr.w));
    CHECK(b.h > 1.0f);          // the height is still the text's own
}

TEST_CASE("WidgetManager: a filled element is hit across the whole viewport")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 1920.0f; t.canvasHeight = 1080.0f;
    const int btn = t.add(HE::UIWidgetType::Button);
    HE::UIElement& e = *t.find(btn);
    e.setProp("Text", HE::UIPropValue::ofString(""));
    HE::uiSetAnchorPreset(e, HE::kUIAnchorFill);
    HE::uiSetAnchorInsetsX(e, 0.0f, 0.0f);
    HE::uiSetAnchorInsetsY(e, 0.0f, 0.0f);
    registerWidget(cm, t);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    // The far corner of the viewport is inside it, which for the old
    // point-anchored 120×32 default it never was.
    CHECK(wm.processPointer(1920.0f, 1080.0f, 1900.0f, 1050.0f, true, true));
    CHECK(wm.processPointer(1920.0f, 1080.0f,   20.0f,   20.0f, true, true));
}

// ═══ ListView (docs/he-apps-plan.md B2) ══════════════════════════════════════
// The whole claim of this type is a NEGATIVE one — "ten thousand rows are not
// ten thousand elements" — so most of what follows counts things that must NOT
// happen: elements that are not created, rows that are not rebuilt, frames that
// are not redrawn.

TEST_CASE("ListView: the arithmetic it is all derived from")
{
    HE::UIListView lv;
    lv.sizeY = 200.0f; lv.padding = 0.0f; lv.spacing = 0.0f; lv.rowHeight = 20.0f;
    lv.itemCount = 100;

    CHECK(lv.rowStep() == doctest::Approx(20.0f));
    CHECK(lv.innerHeight() == doctest::Approx(200.0f));
    CHECK(lv.measuredExtent() == doctest::Approx(2000.0f));
    CHECK(lv.maxScroll() == doctest::Approx(1800.0f));

    // The gaps are BETWEEN: five rows have four of them, not five.
    lv.spacing = 4.0f;
    lv.itemCount = 5;
    CHECK(lv.measuredExtent() == doctest::Approx(5 * 20.0f + 4 * 4.0f));
    // …and everything fits, so it does not scroll at all.
    CHECK(lv.maxScroll() == doctest::Approx(0.0f));

    // rowAt: a gap is NOTHING, not the nearest row. Two pixels of background
    // must not pick a neighbour.
    lv.itemCount = 100; lv.spacing = 4.0f; lv.scrollOffset = 0.0f;
    CHECK(lv.rowAt(0.0f) == 0);
    CHECK(lv.rowAt(19.0f) == 0);
    CHECK(lv.rowAt(22.0f) == -1);     // in the 4-unit gap after row 0
    CHECK(lv.rowAt(24.0f) == 1);
    CHECK(lv.rowAt(-3.0f) == -1);
    // Scrolled, the same point is a different item — that is what scrolling is.
    lv.scrollOffset = 24.0f;
    CHECK(lv.rowAt(0.0f) == 1);

    // scrollToItem moves only when it has to, and to the near edge.
    lv.scrollOffset = 0.0f;
    CHECK_FALSE(lv.scrollToItem(2));            // already fully in view
    CHECK(lv.scrollOffset == doctest::Approx(0.0f));
    CHECK(lv.scrollToItem(20));
    // Its bottom lands on the view's bottom edge.
    CHECK(lv.scrollOffset == doctest::Approx(20 * 24.0f + 20.0f - 200.0f));
    CHECK(lv.scrollToItem(0));
    CHECK(lv.scrollOffset == doctest::Approx(0.0f));
    CHECK_FALSE(lv.scrollToItem(-1));
    CHECK_FALSE(lv.scrollToItem(100));
}

TEST_CASE("ListView: what the three selection modes mean")
{
    HE::UIListView lv;
    lv.itemCount = 10;

    lv.selectionMode = 0;                       // none
    CHECK_FALSE(lv.setSelected(3, true));
    CHECK(lv.firstSelected() == -1);

    lv.selectionMode = 1;                       // single: picking REPLACES
    CHECK(lv.setSelected(3, true));
    CHECK(lv.firstSelected() == 3);
    CHECK_FALSE(lv.setSelected(3, true));       // already picked: no change
    CHECK(lv.setSelected(5, true));
    CHECK(lv.selection.size() == 1);
    CHECK(lv.firstSelected() == 5);

    lv.selectionMode = 2;                       // multiple: picking ADDS
    CHECK(lv.setSelected(2, true));
    CHECK(lv.selection.size() == 2);
    CHECK(lv.firstSelected() == 2);             // ascending, so the lowest
    CHECK(lv.setSelected(5, false));
    CHECK(lv.selection.size() == 1);
    CHECK(lv.clearSelection());
    CHECK_FALSE(lv.clearSelection());

    // Out of range is not a selection of anything.
    CHECK_FALSE(lv.setSelected(-1, true));
    CHECK_FALSE(lv.setSelected(10, true));
}

TEST_CASE("ListView: the count, the offset and the selection are runtime state")
{
    HE::UIWidgetTree t;
    const int id = t.add(HE::UIWidgetType::ListView);
    auto* lv = dynamic_cast<HE::UIListView*>(t.find(id));
    REQUIRE(lv);
    lv->rowWidget = "mem://row.hasset";
    lv->rowHeight = 33.0f;
    lv->selectionMode = 2;
    // …and the things a run puts there.
    lv->itemCount = 5000;
    lv->scrollOffset = 400.0f;
    lv->setSelected(12, true);

    HE::UIWidgetTree r;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), r));
    auto* rl = dynamic_cast<HE::UIListView*>(r.find(id));
    REQUIRE(rl);
    CHECK(rl->rowWidget == "mem://row.hasset");
    CHECK(rl->rowHeight == doctest::Approx(33.0f));
    CHECK(rl->selectionMode == 2);
    // A list that reopened pre-scrolled to row 400 of data nobody has loaded yet
    // would be a picture of the last run.
    CHECK(rl->itemCount == 0);
    CHECK(rl->scrollOffset == doctest::Approx(0.0f));
    CHECK(rl->selection.empty());
}

namespace
{
    // A page with one ListView called "List", filling a 400×400 canvas, and a
    // one-label row asset at mem://row.hasset. Rows are 40 tall with no gap and
    // no padding, so exactly ten fit and the arithmetic in the tests is legible.
    void buildListPage(ContentManager& cm)
    {
        HE::UIWidgetTree row;
        row.canvasWidth = 400.0f; row.canvasHeight = 40.0f;
        row.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
        const int label = row.add(HE::UIWidgetType::Text);
        row.find(label)->name = "Label";
        row.find(label)->setProp("Text", HE::UIPropValue::ofString("x"));
        registerWidget(cm, row, nullptr, "mem://row.hasset");

        HE::UIWidgetTree page;
        page.canvasWidth = 400.0f; page.canvasHeight = 400.0f;
        page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
        const int list = page.add(HE::UIWidgetType::ListView);
        {
            auto* lv = dynamic_cast<HE::UIListView*>(page.find(list));
            lv->name = "List";
            HE::uiSetAnchorPreset(*lv, 0); lv->pivotX = lv->pivotY = 0.0f;
            lv->posX = 0.0f; lv->posY = 0.0f; lv->sizeX = 400.0f; lv->sizeY = 400.0f;
            lv->rowWidget = "mem://row.hasset";
            lv->rowHeight = 40.0f; lv->spacing = 0.0f; lv->padding = 0.0f;
        }
        registerWidget(cm, page, nullptr, "mem://page.hasset");
    }
}

// The sentence the whole type exists for, as an assertion.
TEST_CASE("ListView: ten thousand items are not ten thousand elements")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    buildListPage(cm);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://page.hasset");
    REQUIRE(id != 0);
    const std::size_t bare = wm.tree(id)->elements.size();

    CHECK(wm.setListCount(id, "List", 10000));
    CHECK(wm.listCount(id, "List") == 10000);

    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);

    // Ten rows fit, plus the one spare for the sliver at the bottom edge. Each
    // brings its ref element and the row asset's own elements — a handful, and
    // nowhere near four figures.
    const std::size_t elems = wm.tree(id)->elements.size();
    CHECK(elems > bare);
    CHECK(elems - bare < 40);
    // …and it says so about itself: the count is what it was told, not what it
    // built.
    CHECK(wm.listCount(id, "List") == 10000);

    // The rows on screen are the ones at the top, and nothing beyond them.
    CHECK(wm.listRow(id, "List", 0) != 0);
    CHECK(wm.listRow(id, "List", 9) != 0);
    CHECK(wm.listRow(id, "List", 500) == 0);
    CHECK(wm.listRow(id, "List", 9999) == 0);
}

TEST_CASE("ListView: scrolling re-points the rows it already has")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    buildListPage(cm);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://page.hasset");
    REQUIRE(id != 0);
    REQUIRE(wm.setListCount(id, "List", 10000));
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);

    // Every row instance that exists right now.
    std::set<int> before;
    for (int i = 0; i < 12; ++i)
        if (const int r = (int)wm.listRow(id, "List", i)) before.insert(r);
    REQUIRE(before.size() >= 10);
    const std::size_t elemsBefore = wm.tree(id)->elements.size();

    // Jump a long way down.
    CHECK(wm.scrollListToItem(id, "List", 400));
    out.clear();
    wm.extract(400.0f, 400.0f, out);

    std::set<int> after;
    for (int i = 380; i < 405; ++i)
        if (const int r = (int)wm.listRow(id, "List", i)) after.insert(r);
    CHECK(after.size() >= 10);
    // THE point: the same handful of live widgets, pointed at different items.
    // Rebuilding them would allocate new instances and new element ids, and a
    // list that does that at 60 Hz is a list that stutters while it scrolls.
    CHECK(after == before);
    CHECK(wm.tree(id)->elements.size() == elemsBefore);
    // The top of the list is no longer realized, which is what "virtualized"
    // means and what a plain vertical box could never do.
    CHECK(wm.listRow(id, "List", 0) == 0);
}

TEST_CASE("ListView: a list that is not moving does not redraw")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    buildListPage(cm);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://page.hasset");
    REQUIRE(id != 0);
    REQUIRE(wm.setListCount(id, "List", 500));

    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);
    wm.consumeVisualDirty();          // settle whatever the setup raised

    // A second frame with nothing happening: no row changed the item it stands
    // on, so nothing was bound again and there is nothing to draw. This is the
    // real proof that the sync is idempotent — a rebind raises the flag, so a
    // false here means it did not happen.
    out.clear();
    wm.extract(400.0f, 400.0f, out);
    CHECK_FALSE(wm.consumeVisualDirty());

    // Scrolling does move rows onto other items, and that IS a redraw.
    CHECK(wm.processWheel(400.0f, 400.0f, 200.0f, 200.0f, -3.0f));
    CHECK(wm.consumeVisualDirty());
}

TEST_CASE("ListView: the click picks the row under the pointer")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    buildListPage(cm);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://page.hasset");
    REQUIRE(id != 0);
    REQUIRE(wm.setListCount(id, "List", 100));
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);
    CHECK(wm.listSelected(id, "List") == -1);

    // Rows are 40 tall from the top: y=100 is inside row 2.
    CHECK(wm.processPointer(400.0f, 400.0f, 200.0f, 100.0f, true, true));
    CHECK(wm.listSelected(id, "List") == 2);
    wm.processPointer(400.0f, 400.0f, 200.0f, 100.0f, false, true);

    // Single mode replaces rather than adds.
    wm.processPointer(400.0f, 400.0f, 200.0f, 220.0f, true, true);
    CHECK(wm.listSelected(id, "List") == 5);
    wm.processPointer(400.0f, 400.0f, 200.0f, 220.0f, false, true);

    // Scrolled, the same pixel is a different item — the pick follows what is
    // DRAWN there, not what index the row happens to be counted at.
    REQUIRE(wm.scrollListToItem(id, "List", 50));
    wm.extract(400.0f, 400.0f, out);
    wm.processPointer(400.0f, 400.0f, 200.0f, 100.0f, true, true);
    CHECK(wm.listSelected(id, "List") > 5);
    wm.processPointer(400.0f, 400.0f, 200.0f, 100.0f, false, true);

    // …and by hand, which is what a script does.
    CHECK(wm.setListSelected(id, "List", 7, true));
    CHECK(wm.listSelected(id, "List") == 7);
    CHECK(wm.setListSelected(id, "List", -1, true));    // -1 clears
    CHECK(wm.listSelected(id, "List") == -1);
}

// The other half of the contract: the list holds no items, so it has to ASK.
TEST_CASE("ListView: the owner is asked to fill in each row it puts up")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    // The page's own graph: On Row Bind → write into a label on the page. It
    // proves the event arrives, addressed by the element the list was authored
    // as. Built by hand rather than by the helper, because the graph has to name
    // that element id and a marker label has to exist beside it.
    HE::UIWidgetTree row;
    row.canvasWidth = 400.0f; row.canvasHeight = 40.0f;
    row.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    row.add(HE::UIWidgetType::Text);
    registerWidget(cm, row, nullptr, "mem://row.hasset");

    HE::UIWidgetTree page;
    page.canvasWidth = 400.0f; page.canvasHeight = 400.0f;
    page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int list = page.add(HE::UIWidgetType::ListView);
    {
        auto* lv = dynamic_cast<HE::UIListView*>(page.find(list));
        lv->name = "List";
        HE::uiSetAnchorPreset(*lv, 0); lv->pivotX = lv->pivotY = 0.0f;
        lv->posX = 0.0f; lv->posY = 0.0f; lv->sizeX = 400.0f; lv->sizeY = 400.0f;
        lv->rowWidget = "mem://row.hasset";
        lv->rowHeight = 40.0f; lv->spacing = 0.0f; lv->padding = 0.0f;
    }
    // A label OFF the list, so its glyphs cannot be confused with a row's.
    const int mark = page.add(HE::UIWidgetType::Text);
    {
        HE::UIElement& e = *page.find(mark);
        e.setProp("Text", HE::UIPropValue::ofString(""));
        e.hitTestable = false;
    }

    HorizonCode::Graph g;
    HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "OnRowBind"; ev.elem = list;
    const int evId = g.addNode(ev);
    HorizonCode::Node lit; lit.type = NodeType::ConstString; lit.s = "BOUND";
    const int litId = g.addNode(lit);
    HorizonCode::Node set; set.type = NodeType::SetProperty; set.elem = mark;
    set.s = "Text"; set.propType = PinType::String;
    const int setId = g.addNode(set);
    REQUIRE(g.connect(evId, 0, setId, 0));
    REQUIRE(g.connect(litId, 0, setId, 2));
    registerWidget(cm, page, &g, "mem://page.hasset");

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://page.hasset");
    REQUIRE(id != 0);
    CHECK(wm.tree(id)->find(mark)->getProp("Text").s.empty());

    REQUIRE(wm.setListCount(id, "List", 50));
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);
    // The list asked, and the owner answered.
    CHECK(wm.tree(id)->find(mark)->getProp("Text").s == "BOUND");

    // And the row it asked about is reachable, which is how the answer is
    // written into the row in a real application.
    CHECK(wm.listRow(id, "List", 0) != 0);

    // Refresh asks again for everything on screen without moving anything: the
    // list never saw the data, so an edit to it is invisible until told.
    CHECK(wm.refreshList(id, "List"));
    CHECK(wm.consumeVisualDirty());
    CHECK(wm.listRow(id, "List", 0) != 0);
}

// OnRowBind runs the OWNER'S GRAPH, and the most ordinary thing for that graph
// to do is change the list: a filter that shrinks the count, or "the last row
// was bound, so load fifty more". Both land back in setListCount — which syncs —
// while the sync that fired the bind is still walking its rows. Removing a row
// destroys elements, so without a latch this is a use-after-free anybody could
// author by accident.
TEST_CASE("ListView: a bind that changes the list does not eat the rows it is walking")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree row;
    row.canvasWidth = 400.0f; row.canvasHeight = 40.0f;
    row.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    row.add(HE::UIWidgetType::Text);
    registerWidget(cm, row, nullptr, "mem://row.hasset");

    HE::UIWidgetTree page;
    page.canvasWidth = 400.0f; page.canvasHeight = 400.0f;
    page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int list = page.add(HE::UIWidgetType::ListView);
    {
        auto* lv = dynamic_cast<HE::UIListView*>(page.find(list));
        lv->name = "List";
        HE::uiSetAnchorPreset(*lv, 0); lv->pivotX = lv->pivotY = 0.0f;
        lv->posX = 0.0f; lv->posY = 0.0f; lv->sizeX = 400.0f; lv->sizeY = 400.0f;
        lv->rowWidget = "mem://row.hasset";
        lv->rowHeight = 40.0f; lv->spacing = 0.0f; lv->padding = 0.0f;
    }

    // On Row Bind → Refresh List(self, "List"). A refresh asks every row to be
    // filled in again — from inside the very walk that is filling them in. It is
    // the graph a filter or a sort is written with, and without a latch it is
    // also the graph that never returns: each refresh binds, each bind refreshes.
    HorizonCode::Graph g;
    HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "OnRowBind"; ev.elem = list;
    ev.hasArg = true; ev.propType = PinType::Int;
    const int evId = g.addNode(std::move(ev));

    const HE::api::ApiFn* fn = HE::api::find("widget.refreshList");
    REQUIRE(fn != nullptr);
    HorizonCode::Node call; call.type = NodeType::EngineCall; call.s = fn->id;
    call.hasArg = fn->isExec;
    for (const auto& p : fn->params)  call.params.push_back({ p.name, p.type, p.isArray });
    for (const auto& r : fn->results) call.results.push_back({ r.name, r.type, r.isArray });
    const int callId = g.addNode(std::move(call));
    {
        // Inline pin values are keyed by PARAMETER index (0 = widget, wired
        // below), while a LINK names the absolute pin. The two numberings differ
        // by the exec pins, and mixing them up hands the call a string where it
        // wanted a number — silently, as a zero.
        HorizonCode::Node* n = g.findNode(callId);
        n->pinDefaults[1] = HorizonCode::Value::ofString("List");
    }
    HorizonCode::Node self; self.type = NodeType::GetSelf;
    const int selfId = g.addNode(std::move(self));
    REQUIRE(g.connect(evId, 0, callId, 0));     // exec
    // An exec call's pins run exec-in, exec-out, then the data inputs — so the
    // first parameter is pin 2, while pinDefaults above are indexed by parameter.
    REQUIRE(g.connect(selfId, 0, callId, 2));   // the widget this graph runs on
    registerWidget(cm, page, &g, "mem://page.hasset");

    WidgetManager wm;
    int id = 0;
    // The dispatch an application installs, standing in for the world the
    // registry row would otherwise need: an Engine Call node lands on the same
    // WidgetManager method HE::api::widget::setListCount lands on. That is the
    // whole point of the test — the call arrives from INSIDE a bind.
    HorizonCode::Runtime rt;
    int calls = 0;
    {
        HorizonCode::Runtime::Services svc;
        svc.callApi = [&](HorizonCode::InstanceId, const std::string& apiId,
                          const std::vector<HorizonCode::Value>& args)
            -> std::vector<HorizonCode::Value>
        {
            if (apiId != "widget.refreshList" || args.size() < 2) return {};
            // A runaway must fail the test, not hang the suite.
            if (++calls > 200) return {};
            return { HorizonCode::Value::ofBool(wm.refreshList(id, args[1].s)) };
        };
        rt.setServices(std::move(svc));
    }
    wm.setRuntime(&rt);

    id = createShown(wm, cm, "mem://page.hasset");
    REQUIRE(id != 0);

    // Two hundred items: ten rows go up, and the first of them asks the list to
    // start over while the sync that put it there is still walking its rows.
    REQUIRE(wm.setListCount(id, "List", 200));
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);

    CHECK(calls > 0);            // the bind really did run
    // THE assertion. The refresh is recorded and acted on by the NEXT sync, so
    // this frame binds each row it put up once. Without the latch the first
    // bind's refresh re-enters, that sync binds every row again, each of those
    // refreshes again, and it does not stop — the number runs straight into the
    // guard above.
    CHECK(calls < 100);

    // Two more frames, and it is still a list rather than a recursion.
    out.clear(); wm.extract(400.0f, 400.0f, out);
    out.clear(); wm.extract(400.0f, 400.0f, out);
    CHECK(calls < 100);
    CHECK(wm.listCount(id, "List") == 200);
    CHECK(wm.listRow(id, "List", 0) != 0);
    CHECK(wm.listRow(id, "List", 2) != 0);

    // …and it still takes instructions from outside afterwards, rather than
    // being a heap full of holes.
    CHECK(wm.setListSelected(id, "List", 1, true));
    CHECK(wm.listSelected(id, "List") == 1);
}

TEST_CASE("ListView: a row is placed by the item it shows, not by stacking")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    buildListPage(cm);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://page.hasset");
    REQUIRE(id != 0);
    REQUIRE(wm.setListCount(id, "List", 1000));
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);

    // Scroll to the middle: the realized rows are somewhere around item 300,
    // and each one sits at its OWN item's offset — not at the offset it would
    // have if the rows before it had been stacked, because they do not exist.
    REQUIRE(wm.scrollListToItem(id, "List", 300));
    wm.extract(400.0f, 400.0f, out);

    const HE::UIWidgetTree* tree = wm.tree(id);
    REQUIRE(tree);
    const HE::UIListView* lv = nullptr;
    for (const auto& ep : tree->elements)
        if (ep && ep->name == "List") lv = dynamic_cast<const HE::UIListView*>(ep.get());
    REQUIRE(lv);

    const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(*tree, 400.0f, 400.0f);
    int checked = 0;
    for (const auto& ep : tree->elements)
    {
        const auto* r = dynamic_cast<const HE::UIWidgetRef*>(ep.get());
        if (!r || r->parentId != lv->id || r->rowIndex < 0) continue;
        const HE::UIWidgetRect rect = HE::uiElementRect(*tree, *r, &canvas);
        CHECK(rect.y == doctest::Approx(r->rowIndex * lv->rowStep() - lv->scrollOffset));
        CHECK(rect.h == doctest::Approx(lv->rowHeight));
        ++checked;
    }
    CHECK(checked >= 10);
}

// ═══ The header that makes it a table (docs/he-apps-plan.md 13.1) ════════════
// One property on the same type, not a second type. What follows therefore
// mostly checks that the band SHIFTS everything it should: a header that moves
// four of the five things is an offset nobody sees until a click lands on the
// row above the one under the pointer.

TEST_CASE("Table: the header takes its band off the top, everywhere")
{
    HE::UIListView lv;
    lv.sizeY = 200.0f; lv.padding = 0.0f; lv.spacing = 0.0f; lv.rowHeight = 20.0f;
    lv.itemCount = 100;
    REQUIRE(lv.innerHeight() == doctest::Approx(200.0f));
    REQUIRE(lv.rowAt(0.0f) == 0);

    lv.showHeader = true;
    lv.headerHeight = 30.0f;
    CHECK(lv.headerExtent() == doctest::Approx(30.0f));
    // 1. the room the rows have…
    CHECK(lv.innerHeight() == doctest::Approx(170.0f));
    // …and therefore how far it scrolls.
    CHECK(lv.maxScroll() == doctest::Approx(2000.0f - 170.0f));
    // 2. where the first row starts. The band itself is not a row.
    CHECK(lv.rowAt(0.0f) == -1);
    CHECK(lv.rowAt(29.0f) == -1);
    CHECK(lv.rowAt(30.0f) == 0);
    CHECK(lv.rowAt(49.0f) == 0);
    CHECK(lv.rowAt(50.0f) == 1);
    // 3. the scrollbar's top, which is its own number now that the two ends of
    //    the track are not the same inset.
    lv.padding = 4.0f;
    HE::UIScrollBarStyle sb;
    REQUIRE(lv.scrollBar(sb));
    CHECK(sb.inset == doctest::Approx(4.0f));
    CHECK(sb.topInset == doctest::Approx(34.0f));
    // 4. and how far in from its own top edge the rows are cut off, so a row
    //    scrolled half out of view cannot paint over the titles.
    CHECK(lv.childClipTopInset() == doctest::Approx(34.0f));

    // Off again and every one of them is what it was.
    lv.showHeader = false;
    lv.padding = 0.0f;
    CHECK(lv.innerHeight() == doctest::Approx(200.0f));
    CHECK(lv.rowAt(0.0f) == 0);
    CHECK(lv.childClipTopInset() == doctest::Approx(0.0f));
    REQUIRE(lv.scrollBar(sb));
    CHECK(sb.topInset == doctest::Approx(0.0f));
}

TEST_CASE("Table: Column Widths is read as proportions and repairs itself")
{
    HE::UIListView lv;
    lv.showHeader = true;
    lv.headerLabels = { "A", "B", "C" };

    // Nothing said: equal shares.
    auto fr = lv.columnFractions();
    REQUIRE(fr.size() == 3);
    for (const float f : fr) CHECK(f == doctest::Approx(1.0f / 3.0f));

    // Proportions, not pixels: the same string on a list of any width gives the
    // same shares, and they always add up to the whole.
    lv.columnWidths = "100,50,50";
    fr = lv.columnFractions();
    REQUIRE(fr.size() == 3);
    CHECK(fr[0] == doctest::Approx(0.5f));
    CHECK(fr[1] == doctest::Approx(0.25f));
    CHECK(fr[2] == doctest::Approx(0.25f));

    // Too many entries fall away — the same repair a selection past the end
    // gets, and the string is not refused for it.
    lv.columnWidths = "100,50,50,999,999";
    fr = lv.columnFractions();
    REQUIRE(fr.size() == 3);
    CHECK(fr[0] == doctest::Approx(0.5f));

    // Too few: the missing ones take the average share of the ones that ARE
    // there, so a cell added to the row template arrives the size of its
    // neighbours instead of hair-thin.
    lv.columnWidths = "100,100";
    fr = lv.columnFractions();
    REQUIRE(fr.size() == 3);
    for (const float f : fr) CHECK(f == doctest::Approx(1.0f / 3.0f));

    // Rubbish is the same as absent, entry by entry.
    lv.columnWidths = "100,,-4,abc,100";
    fr = lv.columnFractions();
    REQUIRE(fr.size() == 3);
    CHECK(fr[0] == doctest::Approx(1.0f / 3.0f));

    // No columns at all: no shares, and nothing divides by zero on the way.
    lv.headerLabels.clear();
    CHECK(lv.columnFractions().empty());
}

TEST_CASE("Table: the titles, the seams and the hits come from one arithmetic")
{
    HE::UIListView lv;
    lv.sizeX = 400.0f; lv.sizeY = 200.0f;
    lv.padding = 0.0f; lv.showHeader = true; lv.headerHeight = 30.0f;
    lv.headerLabels = { "Name", "Size", "Kind" };
    lv.columnWidths = "200,100,100";
    lv.columnCanvasW = 400.0f;

    // At its authored size first.
    const HE::UIWidgetRect px{ 0.0f, 0.0f, 400.0f, 200.0f };
    std::vector<float> hx, hw;
    lv.headerLayout(px, hx, hw);
    REQUIRE(hx.size() == 3);
    CHECK(hx[0] == doctest::Approx(0.0f));
    CHECK(hw[0] == doctest::Approx(200.0f));
    CHECK(hx[1] == doctest::Approx(200.0f));
    CHECK(hw[1] == doctest::Approx(100.0f));
    CHECK(hx[2] == doctest::Approx(300.0f));
    // They fill the width exactly — there is no sideways scrolling for a
    // leftover to go into.
    CHECK(hx[2] + hw[2] == doctest::Approx(400.0f));

    // A press in the band is the header's, whether or not it found a title.
    CHECK(lv.headerAtPoint(px, 100.0f, 15.0f).column == 0);
    CHECK(lv.headerAtPoint(px, 250.0f, 15.0f).column == 1);
    CHECK(lv.headerAtPoint(px, 250.0f, 15.0f).inBand);
    CHECK_FALSE(lv.headerAtPoint(px, 250.0f, 15.0f).divider);
    // Below the band is a row, not a title.
    CHECK_FALSE(lv.headerAtPoint(px, 100.0f, 60.0f).inBand);
    // The seam wins over the titles either side of it, or a divider a few
    // pixels wide could never be grabbed.
    CHECK(lv.headerAtPoint(px, 200.0f, 15.0f).divider);
    CHECK(lv.headerAtPoint(px, 200.0f, 15.0f).column == 0);
    // …but not after the last column: there is nothing on the other side of it.
    CHECK_FALSE(lv.headerAtPoint(px, 400.0f, 15.0f).divider);

    // AT SCALE. The rect is twice as big, so every answer is, and a hit test
    // that had its own copy of the sums would be a column out here.
    const HE::UIWidgetRect px2{ 0.0f, 0.0f, 800.0f, 400.0f };
    lv.headerLayout(px2, hx, hw);
    REQUIRE(hx.size() == 3);
    CHECK(hw[0] == doctest::Approx(400.0f));
    CHECK(hx[1] == doctest::Approx(400.0f));
    CHECK(lv.headerAtPoint(px2, 200.0f, 30.0f).column == 0);
    CHECK(lv.headerAtPoint(px2, 500.0f, 30.0f).column == 1);
    CHECK_FALSE(lv.headerAtPoint(px2, 200.0f, 70.0f).inBand);   // below the band
    CHECK(lv.headerAtPoint(px2, 400.0f, 30.0f).divider);
}

TEST_CASE("Table: dragging a seam moves it and keeps the total")
{
    HE::UIListView lv;
    lv.sizeX = 400.0f; lv.sizeY = 200.0f;
    lv.padding = 0.0f; lv.showHeader = true; lv.headerHeight = 30.0f;
    lv.headerLabels = { "A", "B", "C" };
    lv.columnWidths = "200,100,100";
    lv.columnCanvasW = 400.0f;
    const HE::UIWidgetRect px{ 0.0f, 0.0f, 400.0f, 200.0f };

    CHECK(lv.dragColumnDivider(px, 0, 150.0f));
    std::vector<float> hx, hw;
    lv.headerLayout(px, hx, hw);
    REQUIRE(hw.size() == 3);
    CHECK(hw[0] == doctest::Approx(150.0f));
    // What the first column lost, the second one got. Nothing else moved.
    CHECK(hw[1] == doctest::Approx(150.0f));
    CHECK(hw[2] == doctest::Approx(100.0f));
    CHECK(hw[0] + hw[1] + hw[2] == doctest::Approx(400.0f));

    // Dragged past its neighbour, the neighbour keeps a minimum rather than
    // disappearing: a column dragged to nothing is one nobody can get back.
    CHECK(lv.dragColumnDivider(px, 0, 5000.0f));
    lv.headerLayout(px, hx, hw);
    CHECK(hw[1] > 0.0f);
    CHECK(hw[0] + hw[1] == doctest::Approx(300.0f));

    // A seam that does not exist is not a drag, and neither is one that moved
    // nowhere.
    CHECK_FALSE(lv.dragColumnDivider(px, 2, 380.0f));
    CHECK_FALSE(lv.dragColumnDivider(px, -1, 100.0f));
    lv.headerLayout(px, hx, hw);
    CHECK_FALSE(lv.dragColumnDivider(px, 0, hx[0] + hw[0]));
}

TEST_CASE("Table: a list with the header off saves exactly what it always did")
{
    HE::UIWidgetTree t;
    const int id = t.add(HE::UIWidgetType::ListView);
    auto* lv = dynamic_cast<HE::UIListView*>(t.find(id));
    REQUIRE(lv);
    lv->rowWidget = "mem://row.hasset";

    // Not one of the nine keys while it is off, so every widget authored before
    // tables existed writes byte-identical.
    nlohmann::json j;
    lv->writeJson(j);
    for (const char* k : { "showHeader", "headerHeight", "headerColor",
                           "headerTextColor", "headerFontSize", "resizableColumns",
                           "columnWidths", "sortColumn", "sortAscending" })
        CHECK_FALSE(j.contains(k));

    lv->showHeader = true;
    lv->headerHeight = 33.0f;
    lv->columnWidths = "120,80,200";
    lv->resizableColumns = false;
    // The owner's two, set per run like the item count.
    lv->sortColumn = 1;
    lv->sortAscending = false;

    HE::UIWidgetTree r;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), r));
    auto* rl = dynamic_cast<HE::UIListView*>(r.find(id));
    REQUIRE(rl);
    CHECK(rl->showHeader);
    CHECK(rl->headerHeight == doctest::Approx(33.0f));
    CHECK(rl->columnWidths == "120,80,200");
    CHECK_FALSE(rl->resizableColumns);
    // A table that reopened sorted by a column nobody sorted would be a picture
    // of the last run, exactly like a pre-scrolled offset.
    CHECK(rl->sortColumn == -1);
    CHECK(rl->sortAscending);
}

namespace
{
    // The same page buildListPage makes, but with a row template shaped like a
    // table row: a Horizontal Box whose three children are the columns. Their
    // NAMES are the column titles — there is no second list of them.
    void buildTablePage(ContentManager& cm, int badCell = 0,   // 0 none, 1 Slot Fill, 2 Auto Size
                        float rootPadding = 0.0f, float rootSpacing = 0.0f,
                        float rowSpacing = 0.0f)
    {
        HE::UIWidgetTree row;
        row.canvasWidth = 400.0f; row.canvasHeight = 40.0f;
        row.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
        const int box = row.add(HE::UIWidgetType::HorizontalBox);
        {
            HE::UIElement& e = *row.find(box);
            HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
            e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 400.0f; e.sizeY = 40.0f;
            e.setProp("Padding", HE::UIPropValue::ofFloat(rootPadding));
            e.setProp("Spacing", HE::UIPropValue::ofFloat(rootSpacing));
        }
        const char* names[3] = { "Name", "Size", "Kind" };
        for (int i = 0; i < 3; ++i)
        {
            const int cell = row.add(HE::UIWidgetType::Text);
            HE::UIElement& e = *row.find(cell);
            e.parentId = box;
            e.name = names[i];
            e.sizeX = 100.0f; e.sizeY = 40.0f;
            // A cell is a COLUMN, so it does not get to decide its own width.
            // Auto Size is on by default for a text element, which is exactly
            // the trap the precondition below exists to name.
            e.setProp("AutoSize", HE::UIPropValue::ofBool(badCell == 2 && i == 1));
            if (badCell == 1 && i == 1) e.slotFill = 1.0f;
            e.setProp("Text", HE::UIPropValue::ofString(names[i]));
        }
        registerWidget(cm, row, nullptr, "mem://row.hasset");

        HE::UIWidgetTree page;
        page.canvasWidth = 400.0f; page.canvasHeight = 400.0f;
        page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
        const int list = page.add(HE::UIWidgetType::ListView);
        {
            auto* lv = dynamic_cast<HE::UIListView*>(page.find(list));
            lv->name = "List";
            HE::uiSetAnchorPreset(*lv, 0); lv->pivotX = lv->pivotY = 0.0f;
            lv->posX = 0.0f; lv->posY = 0.0f; lv->sizeX = 400.0f; lv->sizeY = 400.0f;
            lv->rowWidget = "mem://row.hasset";
            lv->rowHeight = 40.0f; lv->spacing = rowSpacing; lv->padding = 0.0f;
            lv->showHeader = true; lv->headerHeight = 40.0f;
            lv->columnWidths = "200,100,100";
        }
        registerWidget(cm, page, nullptr, "mem://page.hasset");
    }
}

TEST_CASE("Table: the columns are the row template's cells, read from the asset")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    buildTablePage(cm);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://page.hasset");
    REQUIRE(id != 0);
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);

    const HE::UIListView* lv = nullptr;
    for (const auto& ep : wm.tree(id)->elements)
        if (ep && ep->name == "List") lv = dynamic_cast<const HE::UIListView*>(ep.get());
    REQUIRE(lv);
    // A table with NO items still has a header: the titles come from the asset,
    // not from a realized row, or a table you have not filled yet would have no
    // shape to look at.
    CHECK(lv->itemCount == 0);
    REQUIRE(lv->headerLabels.size() == 3);
    CHECK(lv->headerLabels[0] == "Name");
    CHECK(lv->headerLabels[2] == "Kind");

    // …and with items, the cells of every realized row are given the width of
    // the column above them.
    REQUIRE(wm.setListCount(id, "List", 20));
    wm.extract(400.0f, 400.0f, out);
    const int rowRef = wm.listRow(id, "List", 0);
    REQUIRE(rowRef != 0);
    const HE::UIWidgetTree& t = *wm.tree(id);
    const HE::UIElement* rowRoot = nullptr;
    for (const auto& ep : t.elements)
        if (ep && ep->parentId == rowRef) { rowRoot = ep.get(); break; }
    REQUIRE(rowRoot);
    std::vector<float> cellW;
    for (const auto& ep : t.elements)
        if (ep && ep->parentId == rowRoot->id) cellW.push_back(ep->sizeX);
    REQUIRE(cellW.size() == 3);
    CHECK(cellW[0] == doctest::Approx(200.0f));
    CHECK(cellW[1] == doctest::Approx(100.0f));
    CHECK(cellW[2] == doctest::Approx(100.0f));
}

// The titles are parsed once per template PATH and then kept, and the designer's
// preview destroys and recreates its widgets on the SAME manager. A cache that
// outlived that would show the old column title after the cell was renamed, and
// go on showing it until somebody restarted the editor.
TEST_CASE("Table: renaming a cell and running again renames the column")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    buildTablePage(cm);

    WidgetManager wm;
    const int first = createShown(wm, cm, "mem://page.hasset");
    REQUIRE(first != 0);
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);

    auto titleOf = [&](int id, int col) -> std::string
    {
        for (const auto& ep : wm.tree(id)->elements)
            if (ep && ep->name == "List")
                if (const auto* lv = dynamic_cast<const HE::UIListView*>(ep.get());
                    lv && col < static_cast<int>(lv->headerLabels.size()))
                    return lv->headerLabels[static_cast<std::size_t>(col)];
        return {};
    };
    CHECK(titleOf(first, 1) == "Size");

    // The author renames the cell in the row template and runs the preview
    // again: same manager, same path, a different asset behind it.
    HE::UIWidgetTree row;
    row.canvasWidth = 400.0f; row.canvasHeight = 40.0f;
    row.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int box = row.add(HE::UIWidgetType::HorizontalBox);
    {
        HE::UIElement& e = *row.find(box);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 400.0f; e.sizeY = 40.0f;
        e.setProp("Padding", HE::UIPropValue::ofFloat(0.0f));
        e.setProp("Spacing", HE::UIPropValue::ofFloat(0.0f));
    }
    const char* renamed[3] = { "Name", "Bytes", "Kind" };
    for (int i = 0; i < 3; ++i)
    {
        const int cell = row.add(HE::UIWidgetType::Text);
        HE::UIElement& e = *row.find(cell);
        e.parentId = box;
        e.name = renamed[i];
        e.sizeX = 100.0f; e.sizeY = 40.0f;
        e.setProp("AutoSize", HE::UIPropValue::ofBool(false));
        e.setProp("Text", HE::UIPropValue::ofString(renamed[i]));
    }
    registerWidget(cm, row, nullptr, "mem://row.hasset");

    wm.destroyWidget(first);
    const int again = createShown(wm, cm, "mem://page.hasset");
    REQUIRE(again != 0);
    wm.extract(400.0f, 400.0f, out);
    CHECK(titleOf(again, 1) == "Bytes");
}

TEST_CASE("Table: a row template that cannot carry columns says so and shows none")
{
    // Both ways a cell can insist on its own width. Slot Fill takes a share of
    // what is left over; Auto Size measures the text in it — and Auto Size is
    // ON by default for a text element, so it is the one an author meets first.
    for (const int bad : { 1, 2 })
    {
        TempWidgetDir dir;
        ContentManager cm(dir.path.string());
        buildTablePage(cm, bad);

        WidgetManager wm;
        const int id = createShown(wm, cm, "mem://page.hasset");
        REQUIRE(id != 0);
        REQUIRE(wm.setListCount(id, "List", 20));
        std::vector<UIRenderObject> out;
        wm.extract(400.0f, 400.0f, out);

        const HE::UIListView* lv = nullptr;
        for (const auto& ep : wm.tree(id)->elements)
            if (ep && ep->name == "List") lv = dynamic_cast<const HE::UIListView*>(ep.get());
        REQUIRE(lv);
        // No columns, and the list is otherwise exactly the list it always was.
        CHECK(lv->headerLabels.empty());
        CHECK(lv->columnFractions().empty());
        CHECK(wm.listRow(id, "List", 0) != 0);
    }
}

TEST_CASE("Table: a press on the header is the header's, not the first row's")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    buildTablePage(cm);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://page.hasset");
    REQUIRE(id != 0);
    REQUIRE(wm.setListCount(id, "List", 50));
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);

    // The band is the top 40; row 0 starts under it. Picking row 0 first, so
    // "nothing changed" cannot be confused with "nothing was picked".
    wm.processPointer(400.0f, 400.0f, 300.0f, 60.0f, true, true);
    wm.processPointer(400.0f, 400.0f, 300.0f, 60.0f, false, true);
    CHECK(wm.listSelected(id, "List") == 0);

    // A press on a column TITLE leaves it alone. Without the header taking the
    // press first, rowAt would answer -1 up there and clear the selection every
    // time somebody reached for a title.
    wm.processPointer(400.0f, 400.0f, 100.0f, 20.0f, true, true);
    wm.processPointer(400.0f, 400.0f, 100.0f, 20.0f, false, true);
    CHECK(wm.listSelected(id, "List") == 0);

    // And a drag on the seam between the first two columns moves it — the
    // widths are rewritten, and by the same arithmetic that drew them.
    wm.processPointer(400.0f, 400.0f, 200.0f, 20.0f, true, true);
    wm.processPointer(400.0f, 400.0f, 140.0f, 20.0f, true, true);
    wm.processPointer(400.0f, 400.0f, 140.0f, 20.0f, false, true);

    const HE::UIListView* lv = nullptr;
    for (const auto& ep : wm.tree(id)->elements)
        if (ep && ep->name == "List") lv = dynamic_cast<const HE::UIListView*>(ep.get());
    REQUIRE(lv);
    const auto fr = lv->columnFractions();
    REQUIRE(fr.size() == 3);
    CHECK(fr[0] == doctest::Approx(140.0f / 400.0f));
    CHECK(fr[0] + fr[1] + fr[2] == doctest::Approx(1.0f));
    // …and the selection survived the whole gesture.
    CHECK(wm.listSelected(id, "List") == 0);
}

// A seam is a few pixels wide and sits on the edge of two titles. With the drag
// turned off it has to belong to the title it is on, or a table with fixed
// columns has a dead strip beside every one of them that answers nothing.
TEST_CASE("Table: a seam that cannot be dragged is still part of its title")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    buildTablePage(cm);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://page.hasset");
    REQUIRE(id != 0);
    HE::UIListView* lv = nullptr;
    for (const auto& ep : wm.tree(id)->elements)
        if (ep && ep->name == "List")
            lv = const_cast<HE::UIListView*>(dynamic_cast<const HE::UIListView*>(ep.get()));
    REQUIRE(lv);
    lv->resizableColumns = false;
    REQUIRE(wm.setListCount(id, "List", 50));
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);

    const std::string before = lv->columnWidths;
    // Right on the seam, then pulled: nothing may move, and nothing may be
    // selected either — a press in the band is still the header's.
    wm.processPointer(400.0f, 400.0f, 300.0f, 60.0f, true, true);
    wm.processPointer(400.0f, 400.0f, 300.0f, 60.0f, false, true);
    REQUIRE(wm.listSelected(id, "List") == 0);
    wm.processPointer(400.0f, 400.0f, 200.0f, 20.0f, true, true);
    wm.processPointer(400.0f, 400.0f, 120.0f, 20.0f, true, true);
    wm.processPointer(400.0f, 400.0f, 120.0f, 20.0f, false, true);
    CHECK(lv->columnWidths == before);
    CHECK(wm.listSelected(id, "List") == 0);
}

TEST_CASE("Table: a row scrolled half out of view stops below the titles")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    buildTablePage(cm, /*badCell=*/0, 0.0f, 0.0f, /*rowSpacing=*/6.0f);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://page.hasset");
    REQUIRE(id != 0);
    REQUIRE(wm.setListCount(id, "List", 200));
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);
    // Scrolled to an item whose top does NOT land on the band, so the row at the
    // top is cut by the header rather than sitting neatly under it.
    REQUIRE(wm.scrollListToItem(id, "List", 12));
    wm.extract(400.0f, 400.0f, out);

    const HE::UIWidgetTree& t = *wm.tree(id);
    const HE::UIListView* lv = nullptr;
    for (const auto& ep : t.elements)
        if (ep && ep->name == "List") lv = dynamic_cast<const HE::UIListView*>(ep.get());
    REQUIRE(lv);
    const HE::UIWidgetCanvas canvas =
        HE::uiResolveCanvas(t, 400.0f, 400.0f);
    const HE::UIWidgetRect lr = HE::uiElementRect(t, *lv, &canvas);
    const float bandBottom = lr.y + lv->padding + lv->headerExtent();

    // Every realized row is clipped to below the band. The element itself draws
    // the titles BEFORE its children, so without this a row would paint on top
    // of the column it belongs to.
    int checked = 0;
    for (const auto& ep : t.elements)
    {
        const auto* ref = dynamic_cast<const HE::UIWidgetRef*>(ep.get());
        if (!ref || ref->rowIndex < 0 || ref->parentId != lv->id) continue;
        HE::UIWidgetRect clip{};
        REQUIRE(HE::uiElementClipRect(t, *ref, clip, &canvas));
        CHECK(clip.y >= doctest::Approx(bandBottom));
        ++checked;
    }
    CHECK(checked > 0);
}

// ═══ Layers: dialogs, popups, menus (docs/he-apps-plan.md B4) ════════════════
// Three things that look different and are one thing: while a layer is up, the
// input belongs to it. Most of what follows therefore counts arrivals that must
// NOT happen — the click that does not reach the page behind the dialog, the
// wheel that does not scroll it, the focus that does not walk out of it.

namespace
{
    // A page with one full-width button called "Btn" whose click writes "HIT"
    // into a label. Reading that label afterwards is how these tests ask
    // "did the click arrive?" without needing to see anything drawn.
    void buildClickPage(ContentManager& cm, const char* path, int& outLabel,
                        float bx = 0.0f, float by = 0.0f,
                        float bw = 400.0f, float bh = 400.0f,
                        bool withScroll = false)
    {
        HE::UIWidgetTree t;
        t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
        t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
        const int label = t.add(HE::UIWidgetType::Text);
        t.find(label)->setProp("Text", HE::UIPropValue::ofString(""));
        t.find(label)->hitTestable = false;
        const int btn = t.add(HE::UIWidgetType::Button);
        {
            HE::UIElement& e = *t.find(btn);
            e.name = "Btn";
            HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
            e.posX = bx; e.posY = by; e.sizeX = bw; e.sizeY = bh;
        }
        // A scroll box with more in it than fits, for the tests that ask whether
        // the WHEEL got through as well as the click. Without content taller
        // than the box, uiScrollBy refuses on its own and the question is never
        // really asked.
        if (withScroll)
        {
            const int sb = t.add(HE::UIWidgetType::ScrollBox);
            {
                HE::UIElement& e = *t.find(sb);
                HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
                e.posX = 200.0f; e.posY = 200.0f; e.sizeX = 180.0f; e.sizeY = 180.0f;
            }
            for (int i = 0; i < 12; ++i)
            {
                const int row = t.add(HE::UIWidgetType::Panel);
                t.find(row)->parentId = sb;
                t.find(row)->sizeY = 40.0f;
            }
        }

        HorizonCode::Graph g;
        HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "OnClicked"; ev.elem = btn;
        const int evId = g.addNode(ev);
        HorizonCode::Node lit; lit.type = NodeType::ConstString; lit.s = "HIT";
        const int litId = g.addNode(lit);
        HorizonCode::Node set; set.type = NodeType::SetProperty; set.elem = label;
        set.s = "Text"; set.propType = PinType::String;
        const int setId = g.addNode(set);
        g.connect(evId, 0, setId, 0);
        g.connect(litId, 0, setId, 2);
        registerWidget(cm, t, &g, path);
        outLabel = label;
    }

    void clickAt(WidgetManager& wm, float x, float y)
    {
        wm.processPointer(400.0f, 400.0f, x, y, true,  true);
        wm.processPointer(400.0f, 400.0f, x, y, false, true);
    }
}

TEST_CASE("Modal: nothing underneath is reachable, by any route in")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    int pageLabel = 0, dlgLabel = 0;
    buildClickPage(cm, "mem://page.hasset", pageLabel,
                   0.0f, 0.0f, 400.0f, 400.0f, /*withScroll=*/true);
    // The dialog is SMALL and in the corner. That is the whole design of this
    // test: if it covered the screen it would take every click by being on top,
    // and the test would pass without the input trap existing at all.
    buildClickPage(cm, "mem://dialog.hasset", dlgLabel, 0.0f, 0.0f, 80.0f, 80.0f);

    WidgetManager wm;
    const int page = createShown(wm, cm, "mem://page.hasset");
    const int dlg  = wm.createWidget(cm, "mem://dialog.hasset");
    REQUIRE(page != 0);
    REQUIRE(dlg  != 0);

    // Without a dialog the page takes its click AND its wheel, which is the
    // baseline the rest of this test is measured against.
    // The click lands on the button clear of the scroll box; the wheel lands
    // inside the box. Two different points, because they are two questions.
    clickAt(wm, 300.0f, 100.0f);
    CHECK(wm.tree(page)->find(pageLabel)->getProp("Text").s == "HIT");
    CHECK(wm.processWheel(400.0f, 400.0f, 300.0f, 300.0f, -1.0f));

    wm.showModal(dlg);
    CHECK(wm.hasModal());
    // Showing it also PUT IT UP: a dialog that blocks the input while drawing
    // behind something is the worst of both.
    CHECK(wm.isVisible(dlg));
    CHECK(wm.zOrder(dlg) > wm.zOrder(page));

    // A click INSIDE the dialog reaches it.
    clickAt(wm, 40.0f, 40.0f);
    CHECK(wm.tree(dlg)->find(dlgLabel)->getProp("Text").s == "HIT");

    // …and a click OUTSIDE it, where the page's own button plainly is, reaches
    // nothing at all. Reset first so this cannot pass on the baseline's "HIT".
    {
        HE::UIWidgetTree* live = const_cast<HE::UIWidgetTree*>(wm.tree(page));
        live->find(pageLabel)->setProp("Text", HE::UIPropValue::ofString(""));
    }
    clickAt(wm, 300.0f, 100.0f);
    CHECK(wm.tree(page)->find(pageLabel)->getProp("Text").s.empty());

    // The dim covers the whole screen and is drawn by the manager, so there is
    // no element under that click — and something still has to say it belonged
    // to the UI, or it fires into the game behind the dialog.
    CHECK(wm.pointerOverUI());

    // The wheel is the same question asked a different way: the page's scroll
    // box is under the pointer and must not move.
    CHECK_FALSE(wm.processWheel(400.0f, 400.0f, 300.0f, 300.0f, 1.0f));

    // And it draws the scrim: a full-screen quad, before the dialog's own.
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);
    bool scrim = false;
    for (const UIRenderObject& ro : out)
        if (ro.size.x >= 400.0f && ro.size.y >= 400.0f && ro.color.a > 0.1f &&
            ro.color.r < 0.1f && ro.color.g < 0.1f)
            { scrim = true; break; }
    CHECK(scrim);
}

// The OK button everybody writes is "OnClicked -> Hide Widget (Get Self)".
// Hiding a dialog is closing it, and if the grab outlived the picture the whole
// application would be inert with nothing on screen to blame for it.
TEST_CASE("Modal: hiding it lets go of the input, like closing it does")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    int pageLabel = 0, dlgLabel = 0;
    buildClickPage(cm, "mem://page.hasset", pageLabel);
    buildClickPage(cm, "mem://dialog.hasset", dlgLabel, 0.0f, 0.0f, 80.0f, 80.0f);

    WidgetManager wm;
    const int page = createShown(wm, cm, "mem://page.hasset");
    const int dlg  = wm.createWidget(cm, "mem://dialog.hasset");
    REQUIRE(page != 0);
    REQUIRE(dlg  != 0);

    wm.showModal(dlg);
    clickAt(wm, 300.0f, 300.0f);
    CHECK(wm.tree(page)->find(pageLabel)->getProp("Text").s.empty());   // blocked

    // Hidden, not closed — the difference the manager must not care about.
    wm.hideWidget(dlg);
    CHECK_FALSE(wm.hasLayer());
    CHECK_FALSE(wm.hasModal());

    clickAt(wm, 300.0f, 300.0f);
    CHECK(wm.tree(page)->find(pageLabel)->getProp("Text").s == "HIT");
}

TEST_CASE("Popup: a root anchored to fill its widget still gets a size")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    // A root anchored to fill with a 50-unit inset on every side — ordinary
    // authoring, and the case that breaks a naive placement. On a stretched axis
    // sizeX/sizeY are not a size at all: they are the DIFFERENCE to the anchored
    // span, so here they are -100. Re-anchoring to a point makes the span zero,
    // and a width of "span + size" is then NEGATIVE. It has to be measured
    // before the anchors are touched.
    HE::UIWidgetTree menu;
    menu.canvasWidth = 400.0f; menu.canvasHeight = 400.0f;
    menu.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int root = menu.add(HE::UIWidgetType::Panel);
    {
        HE::UIElement& e = *menu.find(root);
        HE::uiSetAnchorPreset(e, HE::kUIAnchorFill);
        HE::uiSetAnchorInsetsX(e, 50.0f, 50.0f);
        HE::uiSetAnchorInsetsY(e, 50.0f, 50.0f);
        CHECK(e.sizeX < 0.0f);          // the thing that makes this test the test
    }
    registerWidget(cm, menu, nullptr, "mem://menu.hasset");

    WidgetManager wm;
    const int pop = wm.createWidget(cm, "mem://menu.hasset");
    REQUIRE(pop != 0);
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);

    wm.openPopupAt(pop, 30.0f, 40.0f);
    const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(*wm.tree(pop), 400.0f, 400.0f);
    const HE::UIWidgetRect r =
        HE::uiElementRect(*wm.tree(pop), *wm.tree(pop)->find(root), &canvas);
    CHECK(r.w == doctest::Approx(300.0f));
    CHECK(r.h == doctest::Approx(300.0f));
    CHECK(r.x == doctest::Approx(30.0f));
    CHECK(r.y == doctest::Approx(40.0f));
}

TEST_CASE("Layers: a dialog can be cancelled from inside its own text field")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    int pageLabel = 0;
    buildClickPage(cm, "mem://page.hasset", pageLabel);

    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int field = t.add(HE::UIWidgetType::TextInput);
    {
        HE::UIElement& e = *t.find(field);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 40.0f;
    }
    registerWidget(cm, t, nullptr, "mem://ask.hasset");

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://page.hasset") != 0);
    const int dlg = wm.createWidget(cm, "mem://ask.hasset");
    REQUIRE(dlg != 0);

    wm.showModal(dlg);
    clickAt(wm, 100.0f, 20.0f);            // into the field
    CHECK(wm.hasFocusedTextField());
    // The manager's half of it: Escape has something to close even while a field
    // holds the keyboard. (The apps' half is that the key is not routed behind
    // the same "is something typing?" gate — see GameApplication/EditorApplication.)
    CHECK(wm.closeTopLayer());
    CHECK_FALSE(wm.hasLayer());
}

TEST_CASE("Layers: Escape closes one, and the focus goes back where it was")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    int a = 0, b = 0, c = 0;
    buildClickPage(cm, "mem://page.hasset", a);
    buildClickPage(cm, "mem://one.hasset", b);
    buildClickPage(cm, "mem://two.hasset", c);

    WidgetManager wm;
    const int page = createShown(wm, cm, "mem://page.hasset");
    const int one  = wm.createWidget(cm, "mem://one.hasset");
    const int two  = wm.createWidget(cm, "mem://two.hasset");
    REQUIRE(page != 0);
    REQUIRE(one  != 0);
    REQUIRE(two  != 0);

    // The focus is somewhere on the page before any of this.
    const int pageBtn = wm.tree(page)->elements.back()->id;
    REQUIRE(wm.setFocus(page, pageBtn));
    CHECK(wm.focusedElement() == pageBtn);

    wm.showModal(one);
    wm.showModal(two);            // a confirmation over a settings dialog
    CHECK(wm.hasLayer());

    // One at a time, top first. A stack that closed wholesale would take the
    // settings dialog away with the confirmation that was asked about it.
    CHECK(wm.closeTopLayer());
    CHECK_FALSE(wm.isVisible(two));
    CHECK(wm.isVisible(one));
    CHECK(wm.hasLayer());

    CHECK(wm.closeTopLayer());
    CHECK_FALSE(wm.isVisible(one));
    CHECK_FALSE(wm.hasLayer());
    // Back where it started, rather than nowhere.
    CHECK(wm.focusedElement() == pageBtn);

    // Nothing open: the key belongs to whoever asked, which is what lets a game
    // keep Escape for its own pause menu.
    CHECK_FALSE(wm.closeTopLayer());
}

TEST_CASE("Popup: placed at a point, kept on screen, dismissed by looking away")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    int pageLabel = 0;
    buildClickPage(cm, "mem://page.hasset", pageLabel);

    // The menu: one small panel, deliberately anchored in the MIDDLE and
    // stretched, so the placement cannot be passing by accident of authoring.
    HE::UIWidgetTree menu;
    menu.canvasWidth = 400.0f; menu.canvasHeight = 400.0f;
    menu.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int root = menu.add(HE::UIWidgetType::Panel);
    {
        HE::UIElement& e = *menu.find(root);
        HE::uiSetAnchorPreset(e, 5);              // centre
        e.pivotX = e.pivotY = 0.5f;
        e.sizeX = 120.0f; e.sizeY = 80.0f;
    }
    // …and a label on it, so its own graph has something to answer with.
    const int tag = menu.add(HE::UIWidgetType::Text);
    menu.find(tag)->parentId = root;
    menu.find(tag)->setProp("Text", HE::UIPropValue::ofString(""));
    HorizonCode::Graph g;
    HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "OnDismissed";
    const int evId = g.addNode(ev);
    HorizonCode::Node lit; lit.type = NodeType::ConstString; lit.s = "GONE";
    const int litId = g.addNode(lit);
    HorizonCode::Node set; set.type = NodeType::SetProperty; set.elem = tag;
    set.s = "Text"; set.propType = PinType::String;
    const int setId = g.addNode(set);
    g.connect(evId, 0, setId, 0);
    g.connect(litId, 0, setId, 2);
    registerWidget(cm, menu, &g, "mem://menu.hasset");

    WidgetManager wm;
    const int page = createShown(wm, cm, "mem://page.hasset");
    const int pop  = wm.createWidget(cm, "mem://menu.hasset");
    REQUIRE(page != 0);
    REQUIRE(pop  != 0);
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);           // so the manager knows the viewport

    // Opened at a point: the root lands THERE, whatever it was anchored to.
    wm.openPopupAt(pop, 40.0f, 60.0f);
    CHECK(wm.isVisible(pop));
    {
        const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(*wm.tree(pop), 400.0f, 400.0f);
        const HE::UIWidgetRect r =
            HE::uiElementRect(*wm.tree(pop), *wm.tree(pop)->find(root), &canvas);
        CHECK(r.x == doctest::Approx(40.0f));
        CHECK(r.y == doctest::Approx(60.0f));
    }
    CHECK(wm.closeTopLayer());

    // Opened in the bottom-right corner: pushed back so all of it is on screen.
    // A menu that opens off the edge is a menu with no last entry.
    wm.openPopupAt(pop, 395.0f, 395.0f);
    {
        const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(*wm.tree(pop), 400.0f, 400.0f);
        const HE::UIWidgetRect r =
            HE::uiElementRect(*wm.tree(pop), *wm.tree(pop)->find(root), &canvas);
        CHECK(r.x + r.w <= doctest::Approx(400.0f));
        CHECK(r.y + r.h <= doctest::Approx(400.0f));
        CHECK(r.x >= 0.0f);
        CHECK(r.y >= 0.0f);
    }

    // A click on the menu does NOT dismiss it…
    {
        const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(*wm.tree(pop), 400.0f, 400.0f);
        const HE::UIWidgetRect r =
            HE::uiElementRect(*wm.tree(pop), *wm.tree(pop)->find(root), &canvas);
        clickAt(wm, (r.x + r.w * 0.5f) * canvas.scaleX, (r.y + r.h * 0.5f) * canvas.scaleY);
    }
    CHECK(wm.hasLayer());

    // …and a click anywhere else does, exactly once, and tells its own graph.
    clickAt(wm, 5.0f, 5.0f);
    CHECK_FALSE(wm.hasLayer());
    CHECK_FALSE(wm.isVisible(pop));
    CHECK(wm.tree(pop)->find(tag)->getProp("Text").s == "GONE");
    // …and the click that dismissed it did not also press the page underneath.
    CHECK(wm.tree(page)->find(pageLabel)->getProp("Text").s.empty());
}

TEST_CASE("ComboBox: it opens a list and you pick an entry, instead of cycling")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 800.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int combo = t.add(HE::UIWidgetType::ComboBox);
    {
        auto* cb = dynamic_cast<HE::UIComboBox*>(t.find(combo));
        HE::uiSetAnchorPreset(*cb, 0); cb->pivotX = cb->pivotY = 0.0f;
        cb->posX = 0.0f; cb->posY = 0.0f; cb->sizeX = 200.0f; cb->sizeY = 20.0f;
        cb->options.clear();
        for (int i = 0; i < 20; ++i) cb->options.push_back("Option " + std::to_string(i));
        cb->selectedIndex = 0;
    }
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 800.0f, out);
    const std::size_t closedQuads = out.size();

    // Click the box: it OPENS. It used to advance to option 1, which is the
    // counter-baseline for this whole test — with twenty entries, cycling means
    // fifteen clicks to reach the fifteenth, and no way back.
    clickAt(wm, 100.0f, 10.0f);
    {
        const auto* cb = dynamic_cast<const HE::UIComboBox*>(wm.tree(id)->find(combo));
        REQUIRE(cb);
        CHECK(cb->open);
        CHECK(cb->selectedIndex == 0);      // opening picks nothing
    }
    // The list is drawn, and it is drawn by the MANAGER: it hangs below the
    // element's own rect, where no element could put it.
    out.clear();
    wm.extract(400.0f, 800.0f, out);
    CHECK(out.size() > closedQuads);

    // Row 15 sits at y = box bottom + the 4-unit gap + 15 rows, each as tall as
    // the box. Pressed and released as two separate steps, because THAT is the
    // thing under test: pressing aims, letting go decides.
    const float row15 = 20.0f + 4.0f + 15.0f * 20.0f + 10.0f;
    wm.processPointer(400.0f, 800.0f, 100.0f, row15, true, true);
    {
        const auto* cb = dynamic_cast<const HE::UIComboBox*>(wm.tree(id)->find(combo));
        // Still open, still nothing chosen: the button is down, not lifted.
        CHECK(cb->open);
        CHECK(cb->selectedIndex == 0);
    }
    // …and moving before letting go changes the mind, which is the whole reason
    // a choice belongs on the release.
    wm.processPointer(400.0f, 800.0f, 100.0f, row15 - 20.0f, true, true);
    wm.processPointer(400.0f, 800.0f, 100.0f, row15 - 20.0f, false, true);
    {
        const auto* cb = dynamic_cast<const HE::UIComboBox*>(wm.tree(id)->find(combo));
        REQUIRE(cb);
        CHECK(cb->selectedIndex == 14);
        CHECK_FALSE(cb->open);              // picking closes it
    }
    // Put it back on 15 with a plain click, for what follows.
    clickAt(wm, 100.0f, 10.0f);
    clickAt(wm, 100.0f, row15);
    CHECK(dynamic_cast<const HE::UIComboBox*>(wm.tree(id)->find(combo))->selectedIndex == 15);

    // Open again and click somewhere else: closed, and nothing picked.
    clickAt(wm, 100.0f, 10.0f);
    CHECK(dynamic_cast<const HE::UIComboBox*>(wm.tree(id)->find(combo))->open);
    clickAt(wm, 350.0f, 700.0f);
    {
        const auto* cb = dynamic_cast<const HE::UIComboBox*>(wm.tree(id)->find(combo));
        CHECK_FALSE(cb->open);
        CHECK(cb->selectedIndex == 15);
    }
}

// The real project this was reported from authors on a 1280x720 canvas with
// Fit Inside, so nothing is at scale 1 and the list geometry has to survive
// going through the canvas twice: once to place the rows, once to decide which
// row the pointer is over. Those are two call sites of the same arithmetic, and
// two call sites are where a scale factor gets applied once or three times.
TEST_CASE("ComboBox: the open list still picks the right row on a scaled canvas")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 1280.0f; t.canvasHeight = 720.0f;
    t.scaleMode = HE::UICanvasScaleMode::FitInside;
    const int combo = t.add(HE::UIWidgetType::ComboBox);
    {
        auto* cb = dynamic_cast<HE::UIComboBox*>(t.find(combo));
        HE::uiSetAnchorPreset(*cb, 0); cb->pivotX = cb->pivotY = 0.0f;
        cb->posX = 100.0f; cb->posY = 100.0f; cb->sizeX = 220.0f; cb->sizeY = 32.0f;
        cb->options.clear();
        for (int i = 0; i < 8; ++i) cb->options.push_back("Option " + std::to_string(i));
    }
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);

    // Half size: every canvas unit is half a pixel, so anything that forgot the
    // factor lands one whole list away from where it looks.
    const float vpW = 640.0f, vpH = 360.0f;
    const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(*wm.tree(id), vpW, vpH);
    REQUIRE(canvas.scaleX == doctest::Approx(0.5f));

    std::vector<UIRenderObject> out;
    wm.extract(vpW, vpH, out);
    // Open it: press and release on the closed box, in PIXELS.
    wm.processPointer(vpW, vpH, 150.0f * 0.5f, 110.0f * 0.5f, true,  true);
    wm.processPointer(vpW, vpH, 150.0f * 0.5f, 110.0f * 0.5f, false, true);
    REQUIRE(dynamic_cast<const HE::UIComboBox*>(wm.tree(id)->find(combo))->open);

    // Row 5 sits five rows below the box's bottom edge, in canvas units, then
    // halved into pixels.
    const float rowY = (100.0f + 32.0f) + 5.0f * 32.0f + 16.0f;
    wm.processPointer(vpW, vpH, 150.0f * 0.5f, rowY * 0.5f, true,  true);
    wm.processPointer(vpW, vpH, 150.0f * 0.5f, rowY * 0.5f, false, true);
    {
        const auto* cb = dynamic_cast<const HE::UIComboBox*>(wm.tree(id)->find(combo));
        CHECK(cb->selectedIndex == 5);
        CHECK_FALSE(cb->open);
    }
}

TEST_CASE("Tooltip: the wait is the feature, and waiting is a redraw")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int btn = t.add(HE::UIWidgetType::Button);
    {
        HE::UIElement& e = *t.find(btn);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 60.0f;
        e.tooltip = "Saves the document";
    }
    // A caption ON the button, hit-testable, so the walk upwards is exercised:
    // the pointer lands on the label and the tooltip belongs to the button.
    const int cap = t.add(HE::UIWidgetType::Text);
    {
        HE::UIElement& e = *t.find(cap);
        e.parentId = btn;
        HE::uiSetAnchorPreset(e, HE::kUIAnchorFill);
        HE::uiSetAnchorInsetsX(e, 0.0f, 0.0f);
        HE::uiSetAnchorInsetsY(e, 0.0f, 0.0f);
        e.setProp("Text", HE::UIPropValue::ofString("Save"));
    }
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);

    auto glyphsNow = [&]
    {
        std::vector<UIRenderObject> out;
        wm.extract(400.0f, 400.0f, out);
        return countGlyphs(out);
    };

    // Hover the caption. Nothing yet: a hint that appears the moment the pointer
    // crosses a button is a hint in the way of using it.
    wm.processPointer(400.0f, 400.0f, 100.0f, 30.0f, false, true);
    const int bare = glyphsNow();
    wm.tick(0.2f);
    CHECK(glyphsNow() == bare);

    // Past the delay it appears — and the frame it appears in must be a frame
    // the application knows to draw. An event-driven app redraws on CHANGE, and
    // the change here is time passing, which nothing but the tick reports.
    wm.consumeVisualDirty();
    wm.tick(0.4f);
    CHECK(wm.consumeVisualDirty());
    CHECK(glyphsNow() > bare);

    // Still hovering, nothing moving: nothing to redraw.
    wm.processPointer(400.0f, 400.0f, 100.0f, 30.0f, false, true);
    wm.consumeVisualDirty();
    wm.tick(0.4f);
    CHECK_FALSE(wm.consumeVisualDirty());

    // Off the button: gone again, and that too is a change.
    wm.processPointer(400.0f, 400.0f, 350.0f, 350.0f, false, true);
    CHECK(wm.consumeVisualDirty());
    wm.tick(1.0f);
    CHECK(glyphsNow() == bare);
}

TEST_CASE("Right-click is its own event, and reaches the element that listens")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int label = t.add(HE::UIWidgetType::Text);
    t.find(label)->setProp("Text", HE::UIPropValue::ofString(""));
    t.find(label)->hitTestable = false;
    const int panel = t.add(HE::UIWidgetType::Panel);
    {
        HE::UIElement& e = *t.find(panel);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 400.0f; e.sizeY = 400.0f;
    }
    // Something ON the panel that takes the hit, so the walk upwards matters.
    const int inner = t.add(HE::UIWidgetType::Text);
    {
        HE::UIElement& e = *t.find(inner);
        e.parentId = panel;
        HE::uiSetAnchorPreset(e, HE::kUIAnchorFill);
        HE::uiSetAnchorInsetsX(e, 0.0f, 0.0f);
        HE::uiSetAnchorInsetsY(e, 0.0f, 0.0f);
        e.setProp("Text", HE::UIPropValue::ofString("x"));
    }

    HorizonCode::Graph g;
    HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "OnRightClicked"; ev.elem = panel;
    const int evId = g.addNode(ev);
    HorizonCode::Node lit; lit.type = NodeType::ConstString; lit.s = "MENU";
    const int litId = g.addNode(lit);
    HorizonCode::Node set; set.type = NodeType::SetProperty; set.elem = label;
    set.s = "Text"; set.propType = PinType::String;
    const int setId = g.addNode(set);
    g.connect(evId, 0, setId, 0);
    g.connect(litId, 0, setId, 2);
    registerWidget(cm, t, &g);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);

    // A LEFT click is not a right click, however many of them there are.
    clickAt(wm, 200.0f, 200.0f);
    CHECK(wm.tree(id)->find(label)->getProp("Text").s.empty());

    // The right button, landing on the inner text and bubbling to the panel that
    // listens for it.
    wm.processPointer(400.0f, 400.0f, 200.0f, 200.0f, false, true, /*secondary=*/true);
    CHECK(wm.tree(id)->find(label)->getProp("Text").s == "MENU");

    // Held, not pressed again: one menu per press, not one per frame.
    {
        HE::UIWidgetTree* live = const_cast<HE::UIWidgetTree*>(wm.tree(id));
        live->find(label)->setProp("Text", HE::UIPropValue::ofString(""));
    }
    wm.processPointer(400.0f, 400.0f, 200.0f, 200.0f, false, true, true);
    CHECK(wm.tree(id)->find(label)->getProp("Text").s.empty());
}

// From a real project (AppTest/RootWidget.hasset): two buttons in a vertical
// box, each with a Text child anchored middle-left, authored identically — and
// the second one drawn hard against the top of its button. The two labels
// differed in exactly one byte: the second ended in a newline.
//
// An empty last line is half a two-line block's height, so centring puts the
// visible line in the upper half. Pressing Enter in the Text box to mean "done"
// is how one gets there — and the designer showed it correctly because ImGui
// drops a trailing break, which made the designer disagree with the engine.
TEST_CASE("A label's trailing newline does not push it to the top")
{
    HE::UITextLayout opts; opts.alignV = 1;
    const glm::vec2 plain = HE::measureUIText("Option 2",   20.0f, 0.0f, opts);
    const glm::vec2 tail  = HE::measureUIText("Option 2\n", 20.0f, 0.0f, opts);
    CHECK(tail.y == doctest::Approx(plain.y));
    CHECK(tail.x == doctest::Approx(plain.x));

    // …and a deliberate blank line still counts, so this is not "newlines are
    // ignored" — exactly one trailing one is.
    const glm::vec2 two = HE::measureUIText("a\nb", 20.0f, 0.0f, opts);
    CHECK(two.y > plain.y);
    const glm::vec2 gap = HE::measureUIText("a\n\n", 20.0f, 0.0f, opts);
    CHECK(gap.y == doctest::Approx(two.y));
    // An empty label is still one (empty) line, not none.
    CHECK(HE::measureUIText("", 20.0f, 0.0f, opts).y == doctest::Approx(plain.y));

    // The whole point, in the terms the bug was reported in: the same label with
    // and without the stray break lands in the same place inside a 48-unit
    // button, and that place is the middle.
    for (const char* text : { "Option 2", "Option 2\n" })
    {
        std::vector<UIRenderObject> out;
        HE::emitUITextGlyphs(HE::sharedUIFont(), 0, text, { 0.0f, 0.0f }, { 180.0f, 48.0f },
                             20.0f, glm::vec4(1.0f), 0, opts, out);
        REQUIRE_FALSE(out.empty());
        float top = 1e9f;
        for (const UIRenderObject& ro : out) top = std::min(top, ro.position.y);
        INFO("text := ", text);
        CHECK(top > 10.0f);      // not jammed against the top edge
        CHECK(top < 24.0f);
    }
}

// ── The open list at three roundings, looked at ──────────────────────────────
// Reported from a running app: with a heavily rounded ComboBox the list looked
// wrong and the option text hung out over the left curve. Two causes, one
// assumption — the list copied the box's rounding verbatim, and the text inset
// was a fixed number. This renders the whole thing through the real extract path
// and rasterizes it, because "does it look right" is a question about an image.
TEST_CASE("ComboBox: the open list survives a pill-shaped box")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 640.0f; t.canvasHeight = 300.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    // The third asks for MORE rounding than half a row — the case the clamp
    // exists for, and the one an author reaches by typing a big number or by
    // binding the radius to a theme's Large step.
    const float radii[3] = { 4.0f, 12.0f, 40.0f };   // subtle, round, over the top
    int combo[3] = {};
    for (int i = 0; i < 3; ++i)
    {
        combo[i] = t.add(HE::UIWidgetType::ComboBox);
        auto* cb = dynamic_cast<HE::UIComboBox*>(t.find(combo[i]));
        HE::uiSetAnchorPreset(*cb, 0); cb->pivotX = cb->pivotY = 0.0f;
        cb->posX = 20.0f + 210.0f * i; cb->posY = 20.0f;
        cb->sizeX = 190.0f; cb->sizeY = 44.0f;
        cb->cornerRadius = glm::vec4(radii[i]);
        cb->options = { "Option A", "Option B", "Option C" };
        // The LAST one is picked and the FIRST is hovered below, so the sheet
        // shows both ends of the card — where a row's corners have to follow the
        // card's or it pokes out of it.
        cb->selectedIndex = 2;
    }
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    std::vector<UIRenderObject> out;
    wm.extract(640.0f, 300.0f, out);
    // Open the pill-shaped one and hover its middle row.
    clickAt(wm, 20.0f + 420.0f + 90.0f, 40.0f);
    {
        const auto* cb = dynamic_cast<const HE::UIComboBox*>(wm.tree(id)->find(combo[2]));
        REQUIRE(cb->open);
    }
    wm.processPointer(640.0f, 300.0f, 20.0f + 420.0f + 90.0f, 20.0f + 44.0f + 4.0f + 22.0f,
                      false, true);

    out.clear();
    wm.extract(640.0f, 300.0f, out);

    // The two claims that are not a matter of taste.
    const auto* cb = dynamic_cast<const HE::UIComboBox*>(wm.tree(id)->find(combo[2]));
    REQUIRE(cb);
    // 1. The card is never rounder than half a row, whatever the box asked for.
    const float rowH = cb->optionHeight();
    const float rad  = HE::UIComboBox::listRadius(cb->maxCornerRadius(), rowH, rowH * 3.0f);
    CHECK(rad == doctest::Approx(rowH * 0.5f));
    CHECK(rad < cb->maxCornerRadius());          // it really was cut down
    // 2. The text starts clear of that curve rather than at a fixed inset.
    CHECK(HE::UIComboBox::contentInset(rad) > 6.0f);
    CHECK(HE::UIComboBox::contentInset(0.0f) == doctest::Approx(6.0f));

    HE::sw::Image img;
    img.resize(640, 300);
    img.clear(24, 26, 32, 255);
    HE::sw::draw(img, out);
    const std::filesystem::path shot =
        std::filesystem::temp_directory_path() / "he_combo_rounding.ppm";
    if (FILE* f = std::fopen(shot.string().c_str(), "wb"))
    {
        std::fprintf(f, "P6\n%d %d\n255\n", img.width, img.height);
        for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4)
            std::fwrite(&img.rgba[i], 1, 3, f);
        std::fclose(f);
        MESSAGE("rounding sheet written to " << shot.string());
    }
}

// ═══ WrapBox (docs/he-apps-plan.md B3) ═══════════════════════════════════════
// A row until it cannot be one. The assertions are all about the BREAK: where it
// happens, what it costs in height, and what it does when a child disappears.

namespace
{
    // A wrap box `w` wide with `n` children of the given size, in a canvas that
    // does not scale. Returns the tree plus the child ids in order.
    struct WrapCase { HE::UIWidgetTree t; int box = 0; std::vector<int> kids; };
    WrapCase makeWrap(float boxW, float boxH, int n, float kidW, float kidH,
                      float spacing = 10.0f, float lineSpacing = 20.0f,
                      float padding = 0.0f)
    {
        WrapCase c;
        c.t.canvasWidth = 800.0f; c.t.canvasHeight = 600.0f;
        c.t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
        c.box = c.t.add(HE::UIWidgetType::WrapBox);
        {
            auto* wb = dynamic_cast<HE::UIWrapBox*>(c.t.find(c.box));
            HE::uiSetAnchorPreset(*wb, 0); wb->pivotX = wb->pivotY = 0.0f;
            wb->posX = 0.0f; wb->posY = 0.0f; wb->sizeX = boxW; wb->sizeY = boxH;
            wb->spacing = spacing; wb->lineSpacing = lineSpacing; wb->padding = padding;
        }
        for (int i = 0; i < n; ++i)
        {
            const int k = c.t.add(HE::UIWidgetType::Panel);
            c.t.find(k)->parentId = c.box;
            c.t.find(k)->sizeX = kidW; c.t.find(k)->sizeY = kidH;
            c.kids.push_back(k);
        }
        return c;
    }
}

TEST_CASE("WrapBox: it breaks when the next child would not fit")
{
    // 320 wide, children 100 wide with a 10 gap: they sit at 0, 110 and 220,
    // the third ending exactly on the edge. The fourth would end at 330 and
    // goes to the next line.
    WrapCase c = makeWrap(320.0f, 400.0f, 5, 100.0f, 40.0f);
    auto rect = [&](int i){ return HE::uiElementRect(c.t, *c.t.find(c.kids[i])); };

    CHECK(rect(0).x == doctest::Approx(0.0f));
    CHECK(rect(1).x == doctest::Approx(110.0f));
    CHECK(rect(2).x == doctest::Approx(220.0f));
    // All three on the first line…
    CHECK(rect(0).y == doctest::Approx(0.0f));
    CHECK(rect(2).y == doctest::Approx(0.0f));
    // …and the fourth starts a new one, at the line's height plus the LINE gap
    // (20), not the item gap (10). Two numbers because a row of chips wants
    // tighter spacing sideways than downwards.
    CHECK(rect(3).x == doctest::Approx(0.0f));
    CHECK(rect(3).y == doctest::Approx(60.0f));
    CHECK(rect(4).x == doctest::Approx(110.0f));
    CHECK(rect(4).y == doctest::Approx(60.0f));
    // Every child keeps its own size on BOTH axes — that is the difference to a
    // stacked box, where the cross axis belongs to the box.
    CHECK(rect(0).w == doctest::Approx(100.0f));
    CHECK(rect(0).h == doctest::Approx(40.0f));
}

TEST_CASE("WrapBox: a line is as tall as its own tallest child")
{
    WrapCase c = makeWrap(300.0f, 400.0f, 4, 100.0f, 40.0f);
    // One tall child on the FIRST line only.
    c.t.find(c.kids[1])->sizeY = 90.0f;
    auto rect = [&](int i){ return HE::uiElementRect(c.t, *c.t.find(c.kids[i])); };
    // The fourth child drops below the tall one, not below a 40-tall line.
    CHECK(rect(3).y == doctest::Approx(110.0f));

    // Move the tall one to the second line instead: the first line is short
    // again, so the break comes earlier. A single tall child must not push every
    // other line apart as well — that is what "as tall as ITS OWN line" means.
    c.t.find(c.kids[1])->sizeY = 40.0f;
    c.t.find(c.kids[3])->sizeY = 90.0f;
    CHECK(rect(3).y == doctest::Approx(60.0f));
}

TEST_CASE("WrapBox: a hidden child closes the gap, like everywhere else")
{
    WrapCase c = makeWrap(320.0f, 400.0f, 4, 100.0f, 40.0f);
    auto rect = [&](int i){ return HE::uiElementRect(c.t, *c.t.find(c.kids[i])); };
    REQUIRE(rect(3).y == doctest::Approx(60.0f));   // on the second line

    // Hide the first: everything moves up one slot and the fourth comes back
    // onto the first line. The box invariant, and the reason it is tested here
    // too: a wrap walk that counted invisible children would leave a hole.
    c.t.find(c.kids[0])->visible = false;
    CHECK(rect(1).x == doctest::Approx(0.0f));
    CHECK(rect(3).x == doctest::Approx(220.0f));
    CHECK(rect(3).y == doctest::Approx(0.0f));
}

TEST_CASE("WrapBox: a child wider than the box gets its own line, not an empty one")
{
    WrapCase c = makeWrap(300.0f, 400.0f, 3, 100.0f, 40.0f);
    c.t.find(c.kids[1])->sizeX = 500.0f;   // wider than the whole box
    auto rect = [&](int i){ return HE::uiElementRect(c.t, *c.t.find(c.kids[i])); };
    CHECK(rect(0).y == doctest::Approx(0.0f));
    // It breaks once (something was already on the line) and then does NOT break
    // again on itself — otherwise it would sit under an empty line.
    CHECK(rect(1).x == doctest::Approx(0.0f));
    CHECK(rect(1).y == doctest::Approx(60.0f));
    CHECK(rect(2).y == doctest::Approx(120.0f));
}

TEST_CASE("WrapBox: Size To Content measures the HEIGHT, never the width")
{
    WrapCase c = makeWrap(300.0f, 400.0f, 5, 100.0f, 40.0f, 10.0f, 20.0f, /*padding=*/8.0f);
    auto* wb = dynamic_cast<HE::UIWrapBox*>(c.t.find(c.box));
    REQUIRE(wb);
    wb->sizeToContent = true;

    const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(c.t, 800.0f, 600.0f);
    HE::uiApplyAutoSize(c.t, &canvas);

    // 300 wide minus 8 padding on each side = 284 of room: two children per line
    // (100, 110, and 220 would exceed 284), so three lines for five children.
    // Height = 3*40 + 2*20 line gaps + 2*8 padding.
    CHECK(wb->sizeY == doctest::Approx(3 * 40.0f + 2 * 20.0f + 2 * 8.0f));
    // The width is what the children were wrapped AGAINST, so measuring it from
    // them would be the question answering itself. It stays as authored.
    CHECK(wb->sizeX == doctest::Approx(300.0f));
}

TEST_CASE("WrapBox: it round-trips, and an old asset is untouched")
{
    HE::UIWidgetTree t;
    const int box = t.add(HE::UIWidgetType::WrapBox);
    auto* wb = dynamic_cast<HE::UIWrapBox*>(t.find(box));
    REQUIRE(wb);
    wb->padding = 7.0f; wb->spacing = 3.0f; wb->lineSpacing = 11.0f;
    wb->sizeToContent = true; wb->minSizeY = 40.0f;

    HE::UIWidgetTree r;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), r));
    const auto* rb = dynamic_cast<const HE::UIWrapBox*>(r.find(box));
    REQUIRE(rb);
    CHECK(rb->padding == doctest::Approx(7.0f));
    CHECK(rb->spacing == doctest::Approx(3.0f));
    CHECK(rb->lineSpacing == doctest::Approx(11.0f));
    CHECK(rb->sizeToContent);
    CHECK(rb->minSizeY == doctest::Approx(40.0f));
    CHECK(rb->type() == HE::UIWidgetType::WrapBox);
}

// ═══ Grid (docs/he-apps-plan.md B3) ══════════════════════════════════════════
// The container a form is made of. Every assertion below is about a CELL: which
// one a child ends up in, how big it came out, and what happens when the answer
// is contested — a span in the way, a pinned neighbour, a hidden sibling.

namespace
{
    struct GridCase { HE::UIWidgetTree t; int grid = 0; std::vector<int> kids; };
    GridCase makeGrid(std::vector<std::string> cols, std::vector<std::string> rows,
                      int n, float w = 400.0f, float h = 300.0f,
                      float spacing = 0.0f, float rowSpacing = 0.0f)
    {
        GridCase c;
        c.t.canvasWidth = 800.0f; c.t.canvasHeight = 600.0f;
        c.t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
        c.grid = c.t.add(HE::UIWidgetType::Grid);
        {
            auto* g = dynamic_cast<HE::UIGrid*>(c.t.find(c.grid));
            HE::uiSetAnchorPreset(*g, 0); g->pivotX = g->pivotY = 0.0f;
            g->posX = 0.0f; g->posY = 0.0f; g->sizeX = w; g->sizeY = h;
            g->padding = 0.0f; g->spacing = spacing; g->rowSpacing = rowSpacing;
            g->columns = std::move(cols); g->rows = std::move(rows);
            g->reparse();
        }
        for (int i = 0; i < n; ++i)
        {
            const int k = c.t.add(HE::UIWidgetType::Panel);
            c.t.find(k)->parentId = c.grid;
            c.t.find(k)->sizeX = 50.0f; c.t.find(k)->sizeY = 20.0f;
            c.kids.push_back(k);
        }
        return c;
    }
}

// The one that goes red if the dispatch is missing and children fall through to
// a box-ish path: in a grid the second child sits BESIDE the first, not below.
TEST_CASE("Grid: the second child is beside the first, not under it")
{
    GridCase c = makeGrid({ "*", "*" }, { "*" }, 2);
    auto rect = [&](int i){ return HE::uiElementRect(c.t, *c.t.find(c.kids[i])); };
    CHECK(rect(0).x == doctest::Approx(0.0f));
    CHECK(rect(1).x == doctest::Approx(200.0f));
    CHECK(rect(0).y == doctest::Approx(rect(1).y));
}

TEST_CASE("Grid: the track grammar, including what it does with nonsense")
{
    using K = HE::UIGridTrack::Kind;
    CHECK(HE::uiParseGridTrack("120").kind == K::Fixed);
    CHECK(HE::uiParseGridTrack("120").value == doctest::Approx(120.0f));
    CHECK(HE::uiParseGridTrack("*").kind == K::Weight);
    CHECK(HE::uiParseGridTrack("*").value == doctest::Approx(1.0f));
    CHECK(HE::uiParseGridTrack("2*").value == doctest::Approx(2.0f));
    CHECK(HE::uiParseGridTrack("auto").kind == K::Auto);
    CHECK(HE::uiParseGridTrack("  Auto ").kind == K::Auto);   // trimmed, folded

    // Anything unreadable is ONE SHARE, never nothing. A track you can see and
    // fix beats one that collapsed and hid the typo — the same rule a theme role
    // that no longer resolves obeys.
    for (const char* junk : { "", "12px", "abc", "-5", "1.2.3", "**" })
    {
        INFO("token := ", junk);
        CHECK(HE::uiParseGridTrack(junk).kind == K::Weight);
        CHECK(HE::uiParseGridTrack(junk).value == doctest::Approx(1.0f));
    }
}

TEST_CASE("Grid: fixed, weighted and auto tracks, side by side")
{
    // 400 wide: a 100 fixed column, an auto column, and the rest.
    GridCase c = makeGrid({ "100", "auto", "*" }, { "*" }, 3);
    c.t.find(c.kids[1])->sizeX = 60.0f;      // the auto column's only occupant
    auto rect = [&](int i){ return HE::uiElementRect(c.t, *c.t.find(c.kids[i])); };

    CHECK(rect(0).x == doctest::Approx(0.0f));
    CHECK(rect(0).w == doctest::Approx(100.0f));
    CHECK(rect(1).x == doctest::Approx(100.0f));
    CHECK(rect(1).w == doctest::Approx(60.0f));      // as wide as what is in it
    CHECK(rect(2).x == doctest::Approx(160.0f));
    CHECK(rect(2).w == doctest::Approx(240.0f));     // whatever is left

    // Two shares against one: the weighted column takes twice as much.
    auto* g = dynamic_cast<HE::UIGrid*>(c.t.find(c.grid));
    g->columns = { "*", "2*" }; g->reparse();
    CHECK(rect(0).w == doctest::Approx(400.0f / 3.0f));
    CHECK(rect(1).w == doctest::Approx(800.0f / 3.0f));
}

TEST_CASE("Grid: a span takes the cells it covers, and the gap between them")
{
    GridCase c = makeGrid({ "100", "100" }, { "50", "50" }, 3,
                          400.0f, 300.0f, /*spacing=*/10.0f, /*rowSpacing=*/6.0f);
    c.t.find(c.kids[0])->gridColumnSpan = 2;
    auto rect = [&](int i){ return HE::uiElementRect(c.t, *c.t.find(c.kids[i])); };

    // Two 100 columns plus the 10 between them: a span of two is 210, not 200.
    CHECK(rect(0).w == doctest::Approx(210.0f));
    CHECK(rect(0).y == doctest::Approx(0.0f));
    // …and the next two children were pushed onto the second row by it.
    CHECK(rect(1).y == doctest::Approx(56.0f));
    CHECK(rect(1).x == doctest::Approx(0.0f));
    CHECK(rect(2).x == doctest::Approx(110.0f));
    CHECK(rect(2).y == doctest::Approx(56.0f));
}

TEST_CASE("Grid: a pinned child keeps its cell whoever else wants it")
{
    GridCase c = makeGrid({ "100", "100" }, { "50", "50" }, 3);
    // The LAST child names cell (0,0) — the one the first would otherwise take.
    c.t.find(c.kids[2])->gridColumn = 0;
    c.t.find(c.kids[2])->gridRow    = 0;
    auto rect = [&](int i){ return HE::uiElementRect(c.t, *c.t.find(c.kids[i])); };

    // Pinned children are placed FIRST, whatever their order in the tree.
    CHECK(rect(2).x == doctest::Approx(0.0f));
    CHECK(rect(2).y == doctest::Approx(0.0f));
    // The automatic ones step over the taken cell.
    CHECK(rect(0).x == doctest::Approx(100.0f));
    CHECK(rect(0).y == doctest::Approx(0.0f));
    CHECK(rect(1).x == doctest::Approx(0.0f));
    CHECK(rect(1).y == doctest::Approx(50.0f));
}

TEST_CASE("Grid: more children than declared rows grows more of the last one")
{
    // One declared row of 40. Five children in two columns need three rows, and
    // the two it did not declare are 40 as well — a settings form with twenty
    // rows must not have to declare twenty.
    GridCase c = makeGrid({ "*", "*" }, { "40" }, 5);
    auto rect = [&](int i){ return HE::uiElementRect(c.t, *c.t.find(c.kids[i])); };
    CHECK(rect(0).y == doctest::Approx(0.0f));
    CHECK(rect(2).y == doctest::Approx(40.0f));
    CHECK(rect(4).y == doctest::Approx(80.0f));
    CHECK(rect(4).h == doctest::Approx(40.0f));
}

TEST_CASE("Grid: a hidden child gives its cell back")
{
    GridCase c = makeGrid({ "100", "100" }, { "50", "50" }, 3);
    auto rect = [&](int i){ return HE::uiElementRect(c.t, *c.t.find(c.kids[i])); };
    REQUIRE(rect(2).y == doctest::Approx(50.0f));     // third child, second row

    // Hiding the first moves everything up one cell — the box invariant, and it
    // has to hold here too or a form with an optional field leaves a hole.
    c.t.find(c.kids[0])->visible = false;
    CHECK(rect(1).x == doctest::Approx(0.0f));
    CHECK(rect(1).y == doctest::Approx(0.0f));
    CHECK(rect(2).x == doctest::Approx(100.0f));
    CHECK(rect(2).y == doctest::Approx(0.0f));
}

TEST_CASE("Grid: Size To Content adds up the tracks it can measure")
{
    GridCase c = makeGrid({ "100", "auto", "*" }, { "40", "30" }, 2,
                          400.0f, 300.0f, /*spacing=*/10.0f, /*rowSpacing=*/6.0f);
    auto* g = dynamic_cast<HE::UIGrid*>(c.t.find(c.grid));
    g->padding = 5.0f;
    g->sizeToContent = true;
    c.t.find(c.kids[1])->sizeX = 60.0f;      // fills the auto column

    const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(c.t, 800.0f, 600.0f);
    HE::uiApplyAutoSize(c.t, &canvas);

    // 100 + 60 for the two measurable columns, two gaps of 10, padding twice.
    // The weighted column counts as NOTHING — a share of the leftover is what
    // this measurement is trying to produce, exactly as with a filling child.
    CHECK(g->sizeX == doctest::Approx(100.0f + 60.0f + 2 * 10.0f + 2 * 5.0f));
    CHECK(g->sizeY == doctest::Approx(40.0f + 30.0f + 6.0f + 2 * 5.0f));
}

TEST_CASE("Grid: it round-trips, and a cell-less element saves nothing extra")
{
    HE::UIWidgetTree t;
    const int grid = t.add(HE::UIWidgetType::Grid);
    {
        auto* g = dynamic_cast<HE::UIGrid*>(t.find(grid));
        g->columns = { "auto", "2*", "80" };
        g->rows = { "30" };
        g->rowSpacing = 9.0f;
        g->reparse();
    }
    const int pinned = t.add(HE::UIWidgetType::Panel);
    t.find(pinned)->parentId = grid;
    t.find(pinned)->gridColumn = 1; t.find(pinned)->gridRow = 2;
    t.find(pinned)->gridColumnSpan = 2;
    const int plain = t.add(HE::UIWidgetType::Panel);
    t.find(plain)->parentId = grid;

    const std::string json = HE::uiWidgetTreeToJson(t);
    HE::UIWidgetTree r;
    REQUIRE(HE::uiWidgetTreeFromJson(json, r));
    const auto* rg = dynamic_cast<const HE::UIGrid*>(r.find(grid));
    REQUIRE(rg);
    CHECK(rg->columns == std::vector<std::string>{ "auto", "2*", "80" });
    CHECK(rg->rowSpacing == doctest::Approx(9.0f));
    // The parsed tracks come back with them — a load that forgot to re-parse
    // would lay the grid out on the DEFAULT columns and look almost right.
    REQUIRE(rg->colTracks.size() == 3);
    CHECK(rg->colTracks[0].kind == HE::UIGridTrack::Kind::Auto);
    CHECK(rg->colTracks[1].value == doctest::Approx(2.0f));
    CHECK(rg->colTracks[2].kind == HE::UIGridTrack::Kind::Fixed);

    CHECK(r.find(pinned)->gridColumn == 1);
    CHECK(r.find(pinned)->gridRow == 2);
    CHECK(r.find(pinned)->gridColumnSpan == 2);
    CHECK(r.find(plain)->gridColumn == -1);
    CHECK(r.find(plain)->gridRowSpan == 1);
    // An element that names no cell writes no cell keys at all, so every widget
    // authored before grids existed saves byte-identical.
    CHECK(json.find("gridColSpan") != std::string::npos);   // the pinned one did
    CHECK(json.find("\"gridRowSpan\"") == std::string::npos);
}

// A cell IS the slot, exactly as a box's slot is: the child's own anchors and
// position are not consulted. One placement rule for every container, or an
// author has to learn which container reads which fields.
TEST_CASE("Grid: a cell is the slot, and anchors inside it are ignored")
{
    GridCase c = makeGrid({ "100", "100" }, { "50" }, 1);
    HE::UIElement& k = *c.t.find(c.kids[0]);
    HE::uiSetAnchorPreset(k, 10);          // middle-right, if it were consulted
    k.posX = 999.0f; k.posY = -999.0f;
    k.sizeX = 7.0f;  k.sizeY = 7.0f;

    const HE::UIWidgetRect r = HE::uiElementRect(c.t, k);
    CHECK(r.x == doctest::Approx(0.0f));
    CHECK(r.y == doctest::Approx(0.0f));
    CHECK(r.w == doctest::Approx(100.0f));
    CHECK(r.h == doctest::Approx(50.0f));
}

// ═══ Component parameters (D2) ═══════════════════════════════════════════════
// A WidgetRef grafts another asset in. Until now it grafted it EXACTLY as it was
// authored, which makes a component library a catalogue of screenshots: a form
// row with a baked-in label is one form row, not a form row.
//
// A parameter names one property of one element inside the component and gives
// that pair a name of its own. The host stores the NAME — which is the whole
// point, because it means the component may be rebuilt inside without breaking
// a single page that uses it.

namespace
{
    // A component with one knob: a label whose Text is exposed as "Label".
    HE::UIWidgetTree labelledRow(const char* authoredText = "Default")
    {
        HE::UIWidgetTree t;
        t.canvasWidth = 200.0f; t.canvasHeight = 40.0f;
        t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
        const int txt = t.add(HE::UIWidgetType::Text);
        HE::UIElement& e = *t.find(txt);
        e.name = "Caption";
        e.setProp("Text", HE::UIPropValue::ofString(authoredText));
        HE::uiSetAnchorPreset(e, HE::kUIAnchorFill);
        HE::uiSetAnchorInsetsX(e, 0.0f, 0.0f);
        HE::uiSetAnchorInsetsY(e, 0.0f, 0.0f);

        HE::UIWidgetParam p;
        p.name = "Label"; p.elementId = txt; p.property = "Text";
        p.help = "What the row says.";
        t.params.push_back(p);
        return t;
    }

    // How many glyphs were drawn between two heights. The live tree is the
    // manager's own business (Instance is private, and rightly so), so what a
    // parameter did is asked of the PICTURE — which is the only place it has to
    // be true anyway, and it walks the whole path from the stored value through
    // the graft to the renderer instead of stopping halfway.
    int glyphsBetween(const std::vector<UIRenderObject>& out, float y0, float y1)
    {
        int n = 0;
        for (const auto& ro : out)
            if (ro.type == 2 && ro.position.y >= y0 && ro.position.y < y1) ++n;
        return n;
    }
}

// uiBaseProperties() is the same list getBaseProp/setBaseProp spell as an
// if-chain, written a second time so something can enumerate it. A name added
// to one and not the other would simply never be offered as a parameter — a
// gap that looks exactly like "that property does not exist", which is the
// hardest kind of nothing to notice.
TEST_CASE("Base properties: the enumerable list and the if-chain agree")
{
    HE::UIPanel e;
    for (const HE::UIPropDesc& d : HE::uiBaseProperties())
    {
        // Readable under that name, and answering with the type the list
        // promises — the type is what decides which editor the panel draws.
        const HE::UIPropValue v = e.getPropAny(d.name);
        CHECK_MESSAGE(v.type == d.type, d.name);
        // …and writable: a knob that reads but does not write is worse than one
        // that is not offered.
        e.setPropAny(d.name, v);
    }
    // Not in either list, so it falls through to the TYPE's table and misses
    // there too — which is what the panel reports as "no longer exists".
    CHECK(HE::uiBaseProperties().size() == 46);
}

TEST_CASE("Parameters: a declaration writes the property it names")
{
    HE::UIWidgetTree t = labelledRow();
    CHECK(HE::uiApplyWidgetParams(t, { { "Label", HE::UIPropValue::ofString("Name") } }) == 1);
    CHECK(t.find(1)->getProp("Text").s == "Name");
}

// The rule that makes a component's insides its own business: what the host
// stored is a NAME, and a name nobody declares any more is dropped rather than
// guessed at. Writing it into whatever now sits in that place would be worse
// than a label that stays as its author left it.
TEST_CASE("Parameters: a value nobody declares is dropped, not guessed at")
{
    HE::UIWidgetTree t = labelledRow("Authored");
    CHECK(HE::uiApplyWidgetParams(t, { { "Titel", HE::UIPropValue::ofString("x") } }) == 0);
    CHECK(t.find(1)->getProp("Text").s == "Authored");

    // …and a declaration pointing at an element that is gone writes nothing
    // instead of reaching into the next one along.
    t.params[0].elementId = 999;
    CHECK(HE::uiApplyWidgetParams(t, { { "Label", HE::UIPropValue::ofString("x") } }) == 0);
    CHECK(t.find(1)->getProp("Text").s == "Authored");
}

// A parameter that has not been set is not an empty value: it is what the
// component's author left in the property. That is the ONLY place a default
// lives, so there is no second one to disagree with it.
TEST_CASE("Parameters: an unset one keeps what the component was authored with")
{
    HE::UIWidgetTree t = labelledRow("As authored");
    CHECK(HE::uiApplyWidgetParams(t, {}) == 0);
    CHECK(t.find(1)->getProp("Text").s == "As authored");
}

// The value on disk carries its own type; the property has one too, and the two
// can disagree the day a component's author turns a caption into a number. The
// property's type wins, because it is the one the element actually reads.
TEST_CASE("Parameters: the property's type wins over the stored one")
{
    HE::UIWidgetTree t;
    const int txt = t.add(HE::UIWidgetType::Text);
    t.find(txt)->name = "T";
    { HE::UIWidgetParam p; p.name = "Size"; p.elementId = txt; p.property = "FontSize";
      t.params.push_back(p); }

    // A string where a float is wanted: read as the number it spells.
    CHECK(HE::uiApplyWidgetParams(t, { { "Size", HE::UIPropValue::ofString("31") } }) == 1);
    CHECK(t.find(txt)->getProp("FontSize").f == doctest::Approx(31.0f));

    // …and one that spells nothing at all is that type's zero, not the bytes of
    // the string reinterpreted as a float.
    CHECK(HE::uiApplyWidgetParams(t, { { "Size", HE::UIPropValue::ofString("nonsense") } }) == 1);
    CHECK(t.find(txt)->getProp("FontSize").f == doctest::Approx(0.0f));
}

// Telling a component what colour to be LOCKS that colour against the theme.
// Without this the two fight: the parameter writes at graft time and the theme
// pass overwrites it one line later, so a page that sets an accent colour would
// show the theme's — silently, and only in the runtime, because the designer
// runs no theme pass at all.
//
// Locked and not merely un-bound, which is the part a style changed: un-binding
// only takes the ROLE away, and a style answers for properties nobody bound. An
// un-bound parameter would be painted over by the style of the element's type
// at the very next uiApplyTheme.
TEST_CASE("Parameters: setting a themed colour releases it from the theme")
{
    HE::UIWidgetTree t;
    const int p = t.add(HE::UIWidgetType::Panel);
    t.find(p)->setThemeRole("Color", "Surface");
    { HE::UIWidgetParam d; d.name = "Tint"; d.elementId = p; d.property = "Color";
      t.params.push_back(d); }
    REQUIRE(t.find(p)->themeRoleFor("Color") == "Surface");

    const glm::vec4 red{ 1.0f, 0.0f, 0.0f, 1.0f };
    CHECK(HE::uiApplyWidgetParams(t, { { "Tint", HE::UIPropValue::ofColor(red) } }) == 1);
    CHECK(t.find(p)->themeRoleFor("Color") == HE::kUIThemeLiteral);

    HE::UITheme theme;
    HE::uiApplyTheme(t, theme, HE::UIThemeMode::Dark);
    CHECK(t.find(p)->getProp("Color").col.r == doctest::Approx(1.0f));
    CHECK(t.find(p)->getProp("Color").col.g == doctest::Approx(0.0f));

    // And the case the lock exists for: a theme that dresses every Panel still
    // leaves this one alone, because somebody told it what colour to be.
    HE::UIThemeStyleValue green;
    green.color[0] = green.color[1] = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
    theme.styleMut("Panel").set("Color", green);
    REQUIRE(t.find(p)->themeStyled);          // it would otherwise prove nothing
    HE::uiApplyTheme(t, theme, HE::UIThemeMode::Dark);
    CHECK(t.find(p)->getProp("Color").col.r == doctest::Approx(1.0f));
    CHECK(t.find(p)->getProp("Color").col.g == doctest::Approx(0.0f));
}

// The case the whole feature exists for: one component asset, two copies, two
// different labels. Before parameters this page showed the same word twice.
TEST_CASE("Parameters: two copies of one component say different things")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    registerWidgetAs(cm, "mem://row.hasset", labelledRow("Default"));

    HE::UIWidgetTree page;
    page.canvasWidth = 400.0f; page.canvasHeight = 400.0f;
    page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    // Deliberately different LENGTHS: the picture is what gets asked, and a
    // glyph count is what a picture can answer.
    const char* said[2] = { "A", "BBB" };
    for (int i = 0; i < 2; ++i)
    {
        const int ref = page.add(HE::UIWidgetType::WidgetRef);
        auto* r = dynamic_cast<HE::UIWidgetRef*>(page.find(ref));
        REQUIRE(r);
        HE::uiSetAnchorPreset(*r, 0); r->pivotX = r->pivotY = 0.0f;
        r->posX = 0.0f; r->posY = static_cast<float>(i) * 60.0f;
        r->sizeX = 200.0f; r->sizeY = 40.0f;
        r->widgetPath = "mem://row.hasset";
        const HE::UIPropValue v = HE::UIPropValue::ofString(said[i]);
        r->setParamValue("Label", &v);
    }
    registerWidget(cm, page);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);

    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);

    // One glyph in the first row's band and three in the second's. Told nothing,
    // both would say "Default" and both bands would hold seven — which is why
    // the two values differ in LENGTH and not just in spelling.
    CHECK(glyphsBetween(out,  0.0f, 50.0f) == 1);
    CHECK(glyphsBetween(out, 55.0f, 105.0f) == 3);
}

// A ref that sets nothing gets the component as authored — the same widget,
// used as a picture, which is what every WidgetRef written before parameters
// existed is.
TEST_CASE("Parameters: a ref that sets nothing still shows the component's own text")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    registerWidgetAs(cm, "mem://row.hasset", labelledRow("Authored"));

    HE::UIWidgetTree page;
    page.canvasWidth = 400.0f; page.canvasHeight = 400.0f;
    page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int ref = page.add(HE::UIWidgetType::WidgetRef);
    { auto* r = dynamic_cast<HE::UIWidgetRef*>(page.find(ref));
      HE::uiSetAnchorPreset(*r, 0); r->pivotX = r->pivotY = 0.0f;
      r->sizeX = 200.0f; r->sizeY = 40.0f;
      r->widgetPath = "mem://row.hasset"; }
    registerWidget(cm, page);

    WidgetManager wm;
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);
    CHECK(glyphsBetween(out, 0.0f, 400.0f) == 8);   // "Authored"
}

TEST_CASE("Parameters: they round-trip, and a page without them saves nothing extra")
{
    HE::UIWidgetTree comp = labelledRow("Default");
    const std::string cjson = HE::uiWidgetTreeToJson(comp);
    HE::UIWidgetTree back;
    REQUIRE(HE::uiWidgetTreeFromJson(cjson, back));
    REQUIRE(back.params.size() == 1);
    CHECK(back.params[0].name == "Label");
    CHECK(back.params[0].elementId == 1);
    CHECK(back.params[0].property == "Text");
    CHECK(back.params[0].help == "What the row says.");

    // The values on the host side, one of each shape that can carry one.
    HE::UIWidgetTree page;
    const int ref = page.add(HE::UIWidgetType::WidgetRef);
    { auto* r = dynamic_cast<HE::UIWidgetRef*>(page.find(ref));
      r->widgetPath = "mem://row.hasset";
      const HE::UIPropValue s = HE::UIPropValue::ofString("Hello");
      const HE::UIPropValue f = HE::UIPropValue::ofFloat(2.5f);
      const HE::UIPropValue b = HE::UIPropValue::ofBool(true);
      const HE::UIPropValue c = HE::UIPropValue::ofColor({ 0.25f, 0.5f, 0.75f, 1.0f });
      r->setParamValue("Label", &s);
      r->setParamValue("Size",  &f);
      r->setParamValue("On",    &b);
      r->setParamValue("Tint",  &c); }
    const std::string pjson = HE::uiWidgetTreeToJson(page);
    HE::UIWidgetTree pback;
    REQUIRE(HE::uiWidgetTreeFromJson(pjson, pback));
    const auto* rr = dynamic_cast<const HE::UIWidgetRef*>(pback.find(ref));
    REQUIRE(rr);
    REQUIRE(rr->paramValues.size() == 4);
    // The ORDER an author set them in is the order they come back — a JSON
    // object keyed by name would be free to sort them, and the host's panel
    // would reshuffle itself on every save.
    CHECK(rr->paramValues[0].first == "Label");
    CHECK(rr->paramValues[3].first == "Tint");
    CHECK(rr->paramValue("Label")->s == "Hello");
    CHECK(rr->paramValue("Size")->f == doctest::Approx(2.5f));
    CHECK(rr->paramValue("On")->b);
    CHECK(rr->paramValue("Tint")->col.b == doctest::Approx(0.75f));
    CHECK(rr->paramValue("Missing") == nullptr);

    // The type travels as a NAME, so inserting a type into UIPropType instead of
    // appending one cannot silently turn a colour into a string.
    CHECK(pjson.find("\"color\"") != std::string::npos);

    // …and a widget that declares nothing and sets nothing writes neither key,
    // so every widget authored before components existed saves byte-identical.
    HE::UIWidgetTree plain;
    plain.add(HE::UIWidgetType::WidgetRef);
    const std::string plainJson = HE::uiWidgetTreeToJson(plain);
    CHECK(plainJson.find("params") == std::string::npos);
}

// ═══ Keeping the preview's state across a reload (E4, Stufe 3) ═══════════════
// The live preview is rebuilt from the assets on every save. Correct for
// "restart", wrong for "I fixed a label" — which is most saves, and which used
// to empty a half-filled form.

namespace
{
    // A page with the four things a person can put something INTO, plus a
    // scroll box to scroll.
    HE::UIWidgetTree statefulPage()
    {
        HE::UIWidgetTree t;
        t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
        t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
        const int ti = t.add(HE::UIWidgetType::TextInput);
        t.find(ti)->name = "Field";
        const int cb = t.add(HE::UIWidgetType::CheckBox);
        t.find(cb)->name = "Tick";
        const int sl = t.add(HE::UIWidgetType::Slider);
        t.find(sl)->name = "Dial";
        const int sb = t.add(HE::UIWidgetType::ScrollBox);
        t.find(sb)->name = "Scroller";
        t.find(sb)->sizeX = t.find(sb)->sizeY = 100.0f;
        return t;
    }
}

TEST_CASE("Preview state: what somebody typed survives the widget being rebuilt")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    registerWidget(cm, statefulPage());

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);

    // Put the widget into a state, the way a person would.
    {
        const HE::UIWidgetTree* t = wm.tree(id);
        REQUIRE(t);
        // Written through the manager's own copy: this is the live tree.
        auto* live = const_cast<HE::UIWidgetTree*>(t);
        auto* ti = dynamic_cast<HE::UITextInput*>(live->find(1));
        REQUIRE(ti);
        ti->text = "half a form"; ti->caret = 4;
        live->find(2)->setPropAny("Checked", HE::UIPropValue::ofBool(true));
        live->find(3)->setPropAny("Value", HE::UIPropValue::ofFloat(0.7f));
        *live->find(4)->scrollOffsetPtr() = 42.0f;
    }

    const WidgetManager::StateSnapshot snap = wm.captureState();
    CHECK(snap.elements.size() >= 4);

    // The rebuild: everything down, everything up again from the asset.
    wm.clear();
    const int again = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(again != 0);
    // Freshly built, so it holds what the ASSET says — nothing.
    CHECK(wm.tree(again)->find(1)->getPropAny("Text").s.empty());

    CHECK(wm.restoreState(snap) > 0);

    const HE::UIWidgetTree* t = wm.tree(again);
    REQUIRE(t);
    const auto* ti = dynamic_cast<const HE::UITextInput*>(t->find(1));
    REQUIRE(ti);
    CHECK(ti->text == "half a form");
    CHECK(ti->caret == 4);
    CHECK(t->find(2)->getPropAny("Checked").b);
    CHECK(t->find(3)->getPropAny("Value").f == doctest::Approx(0.7f));
    CHECK(*const_cast<HE::UIElement*>(t->find(4))->scrollOffsetPtr() == doctest::Approx(42.0f));
}

// The half that has to be a DROP rather than a guess. A widget whose elements
// were restructured has ids that now mean something else, and writing yesterday's
// text into whatever sits at that number is worse than an empty field.
TEST_CASE("Preview state: a restructured widget keeps nothing rather than the wrong thing")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    registerWidget(cm, statefulPage());

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    {
        auto* live = const_cast<HE::UIWidgetTree*>(wm.tree(id));
        dynamic_cast<HE::UITextInput*>(live->find(1))->text = "typed";
        live->find(2)->setPropAny("Checked", HE::UIPropValue::ofBool(true));
    }
    const WidgetManager::StateSnapshot snap = wm.captureState();
    wm.clear();

    // The author edited the widget: element 1 is a Button now, and the CheckBox
    // is gone entirely.
    {
        HE::UIWidgetTree t;
        t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
        t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
        t.add(HE::UIWidgetType::Button);      // id 1, was a TextInput
        registerWidget(cm, t, nullptr, "mem://w2.hasset");
    }
    const int again = createShown(wm, cm, "mem://w2.hasset");
    REQUIRE(again != 0);

    // A different asset path: nothing is even offered a home. This is the
    // ordinary case for "I pointed the preview at another widget".
    CHECK(wm.restoreState(snap) == 0);
    // …and the Button is untouched — no text was pasted into it, and it has no
    // Text property to paste into in the first place.
    CHECK(wm.tree(again)->find(1)->type() == HE::UIWidgetType::Button);
}

// Same asset, same ids, but the TYPE at an id changed — the case that gets past
// a naive "does this id exist" check and writes a string into a number.
TEST_CASE("Preview state: a property that changed type is left alone")
{
    TempWidgetDir dir;
    // Two content managers, one path. That is how "the asset on disk changed
    // under it" is reproduced without registering two assets at one path, which
    // is a state the editor never produces and the manager never has to answer.
    ContentManager before(dir.path.string()), after(dir.path.string());

    // Round 1: id 1 is a TextInput holding something.
    {
        HE::UIWidgetTree t;
        t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
        t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
        t.add(HE::UIWidgetType::TextInput);
        registerWidget(before, t);
    }
    WidgetManager wm;
    const int id = createShown(wm, before, "mem://w.hasset");
    REQUIRE(id != 0);
    dynamic_cast<HE::UITextInput*>(
        const_cast<HE::UIWidgetTree*>(wm.tree(id))->find(1))->text = "typed";
    const WidgetManager::StateSnapshot snap = wm.captureState();
    REQUIRE_FALSE(snap.elements.empty());
    wm.clear();

    // Round 2: SAME path and SAME id, but a ComboBox — whose state property is
    // "Selected Index", an Int. The snapshot's "Text" has no counterpart.
    {
        HE::UIWidgetTree t;
        t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
        t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
        t.add(HE::UIWidgetType::ComboBox);
        registerWidget(after, t);
    }
    const int again = createShown(wm, after, "mem://w.hasset");
    REQUIRE(again != 0);
    // The element matches by id, the property does not match by type, so
    // nothing lands. Not a crash and not a silently wrong value.
    CHECK(wm.restoreState(snap) == 0);
    CHECK(wm.tree(again)->find(1)->getPropAny("Selected Index").i == 0);
}

// The other half, and the one whose rule is different: a variable is matched by
// NAME, because a graph has no stable id for one. So a renamed variable is
// indistinguishable from a deleted one, and its value is gone — which has to be
// a real drop and not an accidental re-creation.
TEST_CASE("Preview state: a widget's variables come back, unless the graph dropped them")
{
    TempWidgetDir dir;
    ContentManager before(dir.path.string()), after(dir.path.string());

    HE::UIWidgetTree page;
    page.canvasWidth = 400.0f; page.canvasHeight = 400.0f;
    page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    page.add(HE::UIWidgetType::Panel);

    // Round 1: two variables. The one that gets renamed is a FLOAT, deliberately
    // — a String here would prove nothing. Asking the rebuilt instance for a name
    // it no longer declares answers with a default-constructed Value, whose type
    // is Float, so ONLY a float variable exposes the difference between "the type
    // matches" and "the variable exists".
    {
        HorizonCode::Graph g;
        HorizonCode::Variable score; score.name = "score"; score.type = PinType::Int;
        HorizonCode::Variable draft; draft.name = "draft"; draft.type = PinType::Float;
        g.variables.push_back(score);
        g.variables.push_back(draft);
        registerWidget(before, page, &g);
    }
    // The manager's own runtime is its business; a test that wants to read the
    // variable store injects one it holds itself, which is exactly what
    // HorizonWorld does in the real thing.
    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);
    const int id = createShown(wm, before, "mem://w.hasset");
    REQUIRE(id != 0);
    rt.setVariable(static_cast<HorizonCode::InstanceId>(id),
                   "score", HorizonCode::Value::ofInt(42));
    rt.setVariable(static_cast<HorizonCode::InstanceId>(id),
                   "draft", HorizonCode::Value::ofFloat(0.5f));

    const WidgetManager::StateSnapshot snap = wm.captureState();
    REQUIRE(snap.vars.size() == 1);
    wm.clear();

    // Round 2: the author kept `score` and renamed `draft` to `ratio`.
    {
        HorizonCode::Graph g;
        HorizonCode::Variable score; score.name = "score"; score.type = PinType::Int;
        HorizonCode::Variable ratio; ratio.name = "ratio"; ratio.type = PinType::Float;
        g.variables.push_back(score);
        g.variables.push_back(ratio);
        registerWidget(after, page, &g);
    }
    const int again = createShown(wm, after, "mem://w.hasset");
    REQUIRE(again != 0);
    CHECK(wm.restoreState(snap) > 0);

    const auto inst = static_cast<HorizonCode::InstanceId>(again);
    CHECK(rt.getVariable(inst, "score").i == 42);
    // The renamed one did NOT follow: `ratio` is a new variable and starts at its
    // declared default, not at what `draft` was holding.
    CHECK(rt.getVariable(inst, "ratio").f == doctest::Approx(0.0f));
    // And the part that actually matters — `draft` was not quietly re-created in
    // the store under its old name. This is what a type check alone does not
    // catch, because an undeclared name reads back as a Float and a Float value
    // matches it.
    const auto declared = rt.variablesSnapshot(inst);
    CHECK(declared.find("draft") == declared.end());
}

// ═══ B5: tabs and splitters ══════════════════════════════════════════════════
// Two containers, and the same question underneath both: which child is this,
// among its parent's? A Tab Box answers "the one that shows", a Splitter "which
// side of the divider".

namespace
{
    // A Tab Box `n` pages deep, on a canvas that does not scale.
    struct TabCase
    {
        HE::UIWidgetTree t;
        int box = 0;
        std::vector<int> pages;
    };
    TabCase makeTabs(int n, float w = 400.0f, float h = 300.0f)
    {
        TabCase c;
        c.t.canvasWidth = w; c.t.canvasHeight = h;
        c.t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
        c.box = c.t.add(HE::UIWidgetType::TabBox);
        HE::UIElement& b = *c.t.find(c.box);
        HE::uiSetAnchorPreset(b, 0); b.pivotX = b.pivotY = 0.0f;
        b.posX = b.posY = 0.0f; b.sizeX = w; b.sizeY = h;
        for (int i = 0; i < n; ++i)
        {
            const int p = c.t.add(HE::UIWidgetType::Panel);
            c.t.find(p)->parentId = c.box;
            c.t.find(p)->name = "Page " + std::to_string(i + 1);
            c.pages.push_back(p);
        }
        HE::uiApplyAutoSize(c.t, nullptr);
        return c;
    }
}

TEST_CASE("TabBox: one page shows, and the others are invisible to everything")
{
    TabCase c = makeTabs(3);
    auto* tb = dynamic_cast<HE::UITabBox*>(c.t.find(c.box));
    REQUIRE(tb);

    CHECK(HE::uiElementEffectiveVisible(c.t, *c.t.find(c.pages[0])));
    CHECK_FALSE(HE::uiElementEffectiveVisible(c.t, *c.t.find(c.pages[1])));
    CHECK_FALSE(HE::uiElementEffectiveVisible(c.t, *c.t.find(c.pages[2])));

    tb->activeTab = 2;
    CHECK_FALSE(HE::uiElementEffectiveVisible(c.t, *c.t.find(c.pages[0])));
    CHECK(HE::uiElementEffectiveVisible(c.t, *c.t.find(c.pages[2])));

    // It reaches DOWN: something on a hidden page is hidden too, which is the
    // half that matters for the pointer.
    const int deep = c.t.add(HE::UIWidgetType::Button);
    c.t.find(deep)->parentId = c.pages[0];
    CHECK_FALSE(HE::uiElementEffectiveVisible(c.t, *c.t.find(deep)));
    tb->activeTab = 0;
    CHECK(HE::uiElementEffectiveVisible(c.t, *c.t.find(deep)));

    // An out-of-range active tab shows the FIRST page, not none: a container
    // that blanks itself on a stray Set Property hides its own mistake.
    tb->activeTab = 99;
    CHECK(HE::uiElementEffectiveVisible(c.t, *c.t.find(c.pages[0])));
}

TEST_CASE("TabBox: every page gets the area under the strip, shown or not")
{
    TabCase c = makeTabs(2);
    auto* tb = dynamic_cast<HE::UITabBox*>(c.t.find(c.box));
    tb->tabHeight = 30.0f;

    // Both pages, because a page that is not showing still has to have been
    // laid out — switching to it must not have to build it from nothing.
    for (int p : c.pages)
    {
        const HE::UIWidgetRect r = HE::uiElementRect(c.t, *c.t.find(p));
        CHECK(r.y == doctest::Approx(30.0f));
        CHECK(r.h == doctest::Approx(270.0f));
        CHECK(r.w == doctest::Approx(400.0f));
    }
}

TEST_CASE("TabBox: a tab is as wide as its label, and a click finds it")
{
    TabCase c = makeTabs(3);
    auto* tb = dynamic_cast<HE::UITabBox*>(c.t.find(c.box));
    // The outer two say the SAME thing, so "equally wide" is a claim about the
    // layout and not about how wide an A happens to be next to a C.
    tb->tabLabels = { "A", "Wide label here", "A" };

    const HE::UIWidgetRect px{ 0.0f, 0.0f, 400.0f, 300.0f };
    std::vector<float> x, w;
    HE::UITabBox::tabLayout(px, 16.0f, 14.0f, tb->tabLabels, 0, x, w);
    REQUIRE(w.size() == 3);
    // The middle one says more, so it is wider — all-equal tabs look tidy until
    // one page is called "Settings" and another "A".
    CHECK(w[1] > w[0]);
    CHECK(w[0] == doctest::Approx(w[2]));
    // They sit end to end.
    CHECK(x[1] == doctest::Approx(x[0] + w[0]));

    // …and the hit test agrees with the drawing, because it is the same
    // arithmetic and not a second copy of it.
    CHECK(HE::UITabBox::tabAtPoint(px, 16.0f, 14.0f, 30.0f, tb->tabLabels, 0,
                                   x[1] + 1.0f, 10.0f) == 1);
    CHECK(HE::UITabBox::tabAtPoint(px, 16.0f, 14.0f, 30.0f, tb->tabLabels, 0,
                                   x[2] + 1.0f, 10.0f) == 2);
    // Below the strip is the page, not a tab.
    CHECK(HE::UITabBox::tabAtPoint(px, 16.0f, 14.0f, 30.0f, tb->tabLabels, 0,
                                   x[0] + 1.0f, 50.0f) == -1);
    // Past the last tab is nothing.
    CHECK(HE::UITabBox::tabAtPoint(px, 16.0f, 14.0f, 30.0f, tb->tabLabels, 0,
                                   x[2] + w[2] + 5.0f, 10.0f) == -1);
}

TEST_CASE("Splitter: two panes, the divider between them, and the minimums bite")
{
    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 200.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int sp = t.add(HE::UIWidgetType::Splitter);
    { HE::UIElement& e = *t.find(sp);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = e.posY = 0.0f; e.sizeX = 400.0f; e.sizeY = 200.0f; }
    auto* s = dynamic_cast<HE::UISplitter*>(t.find(sp));
    s->dividerSize = 8.0f; s->ratio = 0.5f;
    s->minFirst = s->minSecond = 40.0f;

    const int a = t.add(HE::UIWidgetType::Panel); t.find(a)->parentId = sp;
    const int b = t.add(HE::UIWidgetType::Panel); t.find(b)->parentId = sp;

    {
        const HE::UIWidgetRect ra = HE::uiElementRect(t, *t.find(a));
        const HE::UIWidgetRect rb = HE::uiElementRect(t, *t.find(b));
        CHECK(ra.x == doctest::Approx(0.0f));
        CHECK(ra.w == doctest::Approx(196.0f));      // (400 - 8) / 2
        CHECK(rb.x == doctest::Approx(204.0f));      // past the divider
        CHECK(rb.w == doctest::Approx(196.0f));
        // The two panes and the divider add up to the whole thing, with no
        // overlap and no gap — the one arithmetic mistake nobody sees until a
        // pane's background bleeds under the divider.
        CHECK(ra.w + 8.0f + rb.w == doctest::Approx(400.0f));
    }

    // An authored ratio outside the minimums is clamped IN THE LAYOUT, not only
    // while dragging: otherwise the designer and the runtime disagree the
    // moment the file loads.
    s->ratio = 0.0f;
    {
        const HE::UIWidgetRect ra = HE::uiElementRect(t, *t.find(a));
        CHECK(ra.w == doctest::Approx(40.0f));
    }
    s->ratio = 1.0f;
    {
        const HE::UIWidgetRect rb = HE::uiElementRect(t, *t.find(b));
        CHECK(rb.w == doctest::Approx(40.0f));
    }

    // Vertical splits the other way and the divider runs across.
    s->ratio = 0.5f; s->vertical = true;
    {
        const HE::UIWidgetRect ra = HE::uiElementRect(t, *t.find(a));
        const HE::UIWidgetRect rb = HE::uiElementRect(t, *t.find(b));
        CHECK(ra.w == doctest::Approx(400.0f));
        CHECK(ra.h == doctest::Approx(96.0f));       // (200 - 8) / 2
        CHECK(rb.y == doctest::Approx(104.0f));
    }

    // A third child is not placed and not seen. Refused at the drop would mean
    // stopping a gesture; this way it is simply obvious.
    const int cc = t.add(HE::UIWidgetType::Panel); t.find(cc)->parentId = sp;
    CHECK_FALSE(HE::uiElementEffectiveVisible(t, *t.find(cc)));
    const HE::UIWidgetRect rc = HE::uiElementRect(t, *t.find(cc));
    CHECK(rc.w == doctest::Approx(0.0f));
}

TEST_CASE("Splitter: one inside another is the three-pane layout, and it nests")
{
    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 200.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int outer = t.add(HE::UIWidgetType::Splitter);
    { HE::UIElement& e = *t.find(outer);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.sizeX = 400.0f; e.sizeY = 200.0f; }
    auto* so = dynamic_cast<HE::UISplitter*>(t.find(outer));
    so->dividerSize = 0.0f; so->ratio = 0.5f; so->minFirst = so->minSecond = 0.0f;

    const int left = t.add(HE::UIWidgetType::Panel); t.find(left)->parentId = outer;
    const int inner = t.add(HE::UIWidgetType::Splitter); t.find(inner)->parentId = outer;
    auto* si = dynamic_cast<HE::UISplitter*>(t.find(inner));
    si->dividerSize = 0.0f; si->ratio = 0.5f; si->minFirst = si->minSecond = 0.0f;
    const int mid   = t.add(HE::UIWidgetType::Panel); t.find(mid)->parentId = inner;
    const int right = t.add(HE::UIWidgetType::Panel); t.find(right)->parentId = inner;

    // The inner splitter's panes are halves of the OUTER's right half, not of
    // the canvas — the mistake a rect-space slip makes, and the reason nesting
    // has its own case.
    CHECK(HE::uiElementRect(t, *t.find(left)).w  == doctest::Approx(200.0f));
    CHECK(HE::uiElementRect(t, *t.find(mid)).x   == doctest::Approx(200.0f));
    CHECK(HE::uiElementRect(t, *t.find(mid)).w   == doctest::Approx(100.0f));
    CHECK(HE::uiElementRect(t, *t.find(right)).x == doctest::Approx(300.0f));
    CHECK(HE::uiElementRect(t, *t.find(right)).w == doctest::Approx(100.0f));
}

// The half that geometry cannot answer: what the POINTER does. A hidden page is
// only really hidden if a button on it stops answering clicks at its own
// coordinates — the exact failure this codebase has had before, where the
// picture and the hit test went through different resolutions.
TEST_CASE("TabBox: a button on a hidden page takes no click")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 300.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int box = t.add(HE::UIWidgetType::TabBox);
    { HE::UIElement& e = *t.find(box);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.sizeX = 400.0f; e.sizeY = 300.0f; }
    // Two pages, each with a button filling it. Same coordinates, so the only
    // thing that can tell them apart is which page is showing.
    int buttons[2] = {};
    for (int i = 0; i < 2; ++i)
    {
        const int page = t.add(HE::UIWidgetType::Panel);
        t.find(page)->parentId = box;
        t.find(page)->name = i == 0 ? "First" : "Second";
        buttons[i] = t.add(HE::UIWidgetType::Button);
        HE::UIElement& b = *t.find(buttons[i]);
        b.parentId = page;
        HE::uiSetAnchorPreset(b, HE::kUIAnchorFill);
        HE::uiSetAnchorInsetsX(b, 0.0f, 0.0f);
        HE::uiSetAnchorInsetsY(b, 0.0f, 0.0f);
    }
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    auto* live = dynamic_cast<HE::UITabBox*>(
        const_cast<HE::UIWidgetTree*>(wm.tree(id))->find(box));
    REQUIRE(live);
    live->tabHeight = 30.0f;

    // A click in the middle of the page area lands on SOMETHING (the visible
    // page's button) — the point is which one, so first prove one is hit.
    CHECK(wm.processPointer(400.0f, 300.0f, 200.0f, 150.0f, true, true));
    wm.processPointer(400.0f, 300.0f, 200.0f, 150.0f, false, true);

    // Now the real claim. Whichever page is active, only ITS button may be
    // pressed — and `pressedElem` is what the manager writes when a press lands.
    auto pressedAt = [&](float x, float y)
    {
        wm.processPointer(400.0f, 300.0f, x, y, true, true);
        const HE::UIWidgetTree* tr = wm.tree(id);
        int found = 0;
        for (const auto& ep : tr->elements)
            if (ep && ep->type() == HE::UIWidgetType::Button &&
                HE::uiElementEffectiveVisible(*tr, *ep)) found = ep->id;
        wm.processPointer(400.0f, 300.0f, x, y, false, true);
        return found;
    };
    live->activeTab = 0;
    CHECK(pressedAt(200.0f, 150.0f) == buttons[0]);
    live->activeTab = 1;
    CHECK(pressedAt(200.0f, 150.0f) == buttons[1]);

    // …and the strip switches pages by being clicked, which is the whole point
    // of a tab.
    live->activeTab = 0;
    wm.processPointer(400.0f, 300.0f, 8.0f, 10.0f, true, true);
    wm.processPointer(400.0f, 300.0f, 8.0f, 10.0f, false, true);
    CHECK(live->activeTab == 0);              // clicking the active tab changes nothing
    // The labels the STRIP draws are filled by the layout pass, so ask after a
    // frame — the hit test does not need this (it reads the tree), but working
    // out where tab two starts does.
    std::vector<UIRenderObject> frame;
    wm.extract(400.0f, 300.0f, frame);
    std::vector<float> tx, tw;
    HE::UITabBox::tabLayout({ 0.0f, 0.0f, 400.0f, 300.0f }, live->fontSize,
                            live->tabPadding, live->tabLabels, live->fontAtlasKey, tx, tw);
    REQUIRE(tx.size() == 2);
    wm.processPointer(400.0f, 300.0f, tx[1] + 4.0f, 10.0f, true, true);
    wm.processPointer(400.0f, 300.0f, tx[1] + 4.0f, 10.0f, false, true);
    CHECK(live->activeTab == 1);
}

TEST_CASE("Splitter: dragging the divider moves it, and only the divider does")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 200.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int sp = t.add(HE::UIWidgetType::Splitter);
    { HE::UIElement& e = *t.find(sp);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.sizeX = 400.0f; e.sizeY = 200.0f; }
    { auto* s = dynamic_cast<HE::UISplitter*>(t.find(sp));
      s->dividerSize = 10.0f; s->ratio = 0.5f; s->minFirst = s->minSecond = 40.0f; }
    for (int i = 0; i < 2; ++i)
    { const int p = t.add(HE::UIWidgetType::Panel); t.find(p)->parentId = sp; }
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    auto* live = dynamic_cast<HE::UISplitter*>(
        const_cast<HE::UIWidgetTree*>(wm.tree(id))->find(sp));
    REQUIRE(live);

    // The divider sits at (400 - 10) / 2 = 195. Press on it and drag right.
    wm.processPointer(400.0f, 200.0f, 198.0f, 100.0f, true, true);
    wm.processPointer(400.0f, 200.0f, 300.0f, 100.0f, true, true);
    CHECK(live->ratio > 0.6f);
    wm.processPointer(400.0f, 200.0f, 300.0f, 100.0f, false, true);

    // Dragging PAST a minimum stops at it, in the layout — the ratio itself may
    // run to the edge, but the panes never do.
    wm.processPointer(400.0f, 200.0f,
                      live->clampedRatio(400.0f) * 390.0f + 4.0f, 100.0f, true, true);
    wm.processPointer(400.0f, 200.0f, 399.0f, 100.0f, true, true);
    wm.processPointer(400.0f, 200.0f, 399.0f, 100.0f, false, true);
    CHECK(live->clampedRatio(400.0f) * 390.0f == doctest::Approx(350.0f));   // 390 - 40

    // A press that is NOT on the divider must not start a drag: the panes have
    // to stay clickable, and grabbing the whole container would end that.
    live->ratio = 0.5f;
    const float before = live->ratio;
    wm.processPointer(400.0f, 200.0f, 50.0f, 100.0f, true, true);
    wm.processPointer(400.0f, 200.0f, 300.0f, 100.0f, true, true);
    CHECK(live->ratio == doctest::Approx(before));
    wm.processPointer(400.0f, 200.0f, 300.0f, 100.0f, false, true);
}

// The divider is the only part of a splitter you can grab, so it is the only
// part that may say so. A resize cursor over the panes would promise a drag
// that the press there does not start.
TEST_CASE("Splitter: the divider asks for the resize cursor, and keeps it while dragged")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    auto build = [&](bool vertical) {
        HE::UIWidgetTree t;
        t.canvasWidth = 400.0f; t.canvasHeight = 200.0f;
        t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
        const int sp = t.add(HE::UIWidgetType::Splitter);
        { HE::UIElement& e = *t.find(sp);
          HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
          e.sizeX = 400.0f; e.sizeY = 200.0f; }
        { auto* s = dynamic_cast<HE::UISplitter*>(t.find(sp));
          s->vertical = vertical; s->dividerSize = 10.0f; s->ratio = 0.5f;
          s->minFirst = s->minSecond = 40.0f; }
        for (int i = 0; i < 2; ++i)
        { const int p = t.add(HE::UIWidgetType::Panel); t.find(p)->parentId = sp; }
        return t;
    };

    {
        // Panes side by side: the divider is the band x = 195…205.
        HE::UIWidgetTree t = build(false);
        registerWidget(cm, t, nullptr, "mem://split-h.hasset");
        WidgetManager wm;
        const int id = createShown(wm, cm, "mem://split-h.hasset");
        REQUIRE(id != 0);

        wm.processPointer(400.0f, 200.0f, 198.0f, 100.0f, false, true);
        CHECK(wm.hoverCursor() == HE::UICursor::ResizeWE);
        // Over a pane it is a plain pointer again.
        wm.processPointer(400.0f, 200.0f, 50.0f, 100.0f, false, true);
        CHECK(wm.hoverCursor() == HE::UICursor::Default);

        // Grab it and pull the pointer clean off the widget: the arrows stay,
        // because the grab does. This is the case a hover-only rule gets wrong,
        // and every fast drag hits it.
        wm.processPointer(400.0f, 200.0f, 198.0f, 100.0f, true, true);
        CHECK(wm.hoverCursor() == HE::UICursor::ResizeWE);
        wm.processPointer(400.0f, 200.0f, 300.0f, 900.0f, true, true);
        CHECK(wm.hoverCursor() == HE::UICursor::ResizeWE);
        wm.processPointer(400.0f, 200.0f, 300.0f, 900.0f, false, true);
        CHECK(wm.hoverCursor() == HE::UICursor::Default);

        // An authored cursor is a decision and outranks the automatic one —
        // the same rule the text field's I-beam follows.
        auto* live = dynamic_cast<HE::UISplitter*>(
            const_cast<HE::UIWidgetTree*>(wm.tree(id))->find(t.elements[0]->id));
        REQUIRE(live);
        live->ratio = 0.5f;
        live->hoverCursor = HE::UICursor::Hand;
        wm.processPointer(400.0f, 200.0f, 198.0f, 100.0f, false, true);
        CHECK(wm.hoverCursor() == HE::UICursor::Hand);
    }
    {
        // Panes stacked: the divider moves up and down, so the arrows do too.
        HE::UIWidgetTree t = build(true);
        registerWidget(cm, t, nullptr, "mem://split-v.hasset");
        WidgetManager wm;
        const int id = createShown(wm, cm, "mem://split-v.hasset");
        REQUIRE(id != 0);
        wm.processPointer(400.0f, 200.0f, 200.0f, 98.0f, false, true);
        CHECK(wm.hoverCursor() == HE::UICursor::ResizeNS);
        wm.processPointer(400.0f, 200.0f, 200.0f, 20.0f, false, true);
        CHECK(wm.hoverCursor() == HE::UICursor::Default);
    }
}

TEST_CASE("TabBox and Splitter: they round-trip")
{
    HE::UIWidgetTree t;
    const int tb = t.add(HE::UIWidgetType::TabBox);
    { auto* b = dynamic_cast<HE::UITabBox*>(t.find(tb));
      b->activeTab = 2; b->tabHeight = 44.0f; b->tabPadding = 9.0f;
      b->activeColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f); }
    const int sp = t.add(HE::UIWidgetType::Splitter);
    { auto* s = dynamic_cast<HE::UISplitter*>(t.find(sp));
      s->vertical = true; s->ratio = 0.25f; s->minFirst = 77.0f; }

    HE::UIWidgetTree r;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), r));
    const auto* rb = dynamic_cast<const HE::UITabBox*>(r.find(tb));
    REQUIRE(rb);
    CHECK(rb->activeTab == 2);
    CHECK(rb->tabHeight == doctest::Approx(44.0f));
    CHECK(rb->tabPadding == doctest::Approx(9.0f));
    CHECK(rb->activeColor.r == doctest::Approx(1.0f));
    const auto* rs = dynamic_cast<const HE::UISplitter*>(r.find(sp));
    REQUIRE(rs);
    CHECK(rs->vertical);
    CHECK(rs->ratio == doctest::Approx(0.25f));
    CHECK(rs->minFirst == doctest::Approx(77.0f));
}

// ═══ Accordion (plan §13.2) ══════════════════════════════════════════════════

namespace
{
    struct AccCase
    {
        HE::UIWidgetTree t;
        int box = 0;
        std::vector<int> sections;
    };

    // `n` sections, each `bodyH` tall, in an accordion filling the canvas.
    AccCase makeAccordion(int n, float bodyH = 60.0f,
                          float w = 400.0f, float h = 300.0f)
    {
        AccCase c;
        c.t.canvasWidth = w; c.t.canvasHeight = h;
        c.t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
        c.box = c.t.add(HE::UIWidgetType::Accordion);
        HE::UIElement& b = *c.t.find(c.box);
        HE::uiSetAnchorPreset(b, 0); b.pivotX = b.pivotY = 0.0f;
        b.posX = b.posY = 0.0f; b.sizeX = w; b.sizeY = h;
        for (int i = 0; i < n; ++i)
        {
            const int p = c.t.add(HE::UIWidgetType::Panel);
            HE::UIElement& s = *c.t.find(p);
            s.parentId = c.box;
            s.name = "Section " + std::to_string(i + 1);
            s.sizeY = bodyH;
            c.sections.push_back(p);
        }
        HE::uiApplyAutoSize(c.t, nullptr);
        HE::uiUpdateScrollExtents(c.t);
        return c;
    }
}

TEST_CASE("Accordion: opening a section pushes the ones below it down")
{
    AccCase c = makeAccordion(3, 60.0f);
    auto* ac = dynamic_cast<HE::UIAccordion*>(c.t.find(c.box));
    REQUIRE(ac);
    ac->headerHeight = 30.0f;
    ac->spacing = 0.0f;
    HE::uiUpdateScrollExtents(c.t);

    // Only the first is open (the default mask), so section two's heading is at
    // 30 + 60 and section three's straight after it.
    auto bodyTop = [&](int i)
    { return HE::uiElementRect(c.t, *c.t.find(c.sections[i])).y; };
    CHECK(bodyTop(0) == doctest::Approx(30.0f));
    CHECK(bodyTop(1) == doctest::Approx(30.0f + 60.0f + 30.0f));
    CHECK(bodyTop(2) == doctest::Approx(150.0f));
    // A folded body is ZERO high, which is the whole difference from a Tab Box.
    CHECK(HE::uiElementRect(c.t, *c.t.find(c.sections[0])).h == doctest::Approx(60.0f));
    CHECK(HE::uiElementRect(c.t, *c.t.find(c.sections[1])).h == doctest::Approx(0.0f));

    // Open the second one as well: the third moves down by exactly its height.
    const float wasThird = bodyTop(2);
    ac->expanded = 0b011;
    HE::uiUpdateScrollExtents(c.t);
    // Its own body has not moved (the heading above it did not change height);
    // everything AFTER it has, by exactly the height that opened up.
    CHECK(bodyTop(1) == doctest::Approx(120.0f));
    CHECK(bodyTop(2) == doctest::Approx(wasThird + 60.0f));
    CHECK(HE::uiElementRect(c.t, *c.t.find(c.sections[1])).h == doctest::Approx(60.0f));

    // …and the content extent is the sum of what is showing, which is what the
    // scrollbar and the wheel clamp against.
    CHECK(ac->contentExtent == doctest::Approx(3 * 30.0f + 2 * 60.0f));
}

TEST_CASE("Accordion: Allow Multiple off leaves exactly one bit, however it was set")
{
    AccCase c = makeAccordion(4);
    auto* ac = dynamic_cast<HE::UIAccordion*>(c.t.find(c.box));
    REQUIRE(ac);

    // A CLICK closes the others…
    CHECK(HE::UIAccordion::toggledMask(0b0001u, 2, 4, false) == 0b0100u);
    // …and so does a stray Set Property, which is the half a toggle-only rule
    // would miss: the mask is normalized wherever it is READ.
    ac->allowMultiple = false;
    ac->expanded = 0b1010;
    CHECK(ac->effectiveMask(4) == 0b0010u);
    CHECK_FALSE(HE::uiElementEffectiveVisible(c.t, *c.t.find(c.sections[3])));
    CHECK(HE::uiElementEffectiveVisible(c.t, *c.t.find(c.sections[1])));

    // On, several at once is the point.
    ac->allowMultiple = true;
    CHECK(ac->effectiveMask(4) == 0b1010u);
    CHECK(HE::UIAccordion::toggledMask(0b0001u, 2, 4, true) == 0b0101u);
    // Closing the last open one is allowed either way: a heading you cannot
    // un-press is worse than an empty stack.
    CHECK(HE::UIAccordion::toggledMask(0b0100u, 2, 4, false) == 0u);
}

TEST_CASE("Accordion: a 33rd section is always folded, and the warning latches")
{
    AccCase c = makeAccordion(33, 10.0f);
    auto* ac = dynamic_cast<HE::UIAccordion*>(c.t.find(c.box));
    REQUIRE(ac);
    ac->expanded = -1;                       // every bit there is

    // Thirty-two open, and the thirty-third folded — there is no bit for it.
    CHECK(HE::uiElementEffectiveVisible(c.t, *c.t.find(c.sections[31])));
    CHECK_FALSE(HE::uiElementEffectiveVisible(c.t, *c.t.find(c.sections[32])));
    // A shift of 32 is undefined behaviour, so the full mask is spelled out.
    // This is the assertion that catches it going back to (1u << n) - 1.
    CHECK(ac->effectiveMask(33) == 0xFFFFFFFFu);

    // Said once and not once a frame: the flag latches, so a second pass is
    // silent. (The log line itself is not capturable here; the guard is.)
    ac->warnedOverflow = false;
    HE::uiUpdateScrollExtents(c.t);
    CHECK(ac->warnedOverflow);
    HE::uiUpdateScrollExtents(c.t);
    CHECK(ac->warnedOverflow);
}

// The half geometry cannot answer: what the POINTER does — and at a scale that
// is not 1, which is where the picture and the hit test have gone through
// different resolutions in this codebase before.
TEST_CASE("Accordion: headings fold where they are drawn, and a folded body takes no click")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    // Authored at 200×150, shown at 400×300: every canvas unit is two pixels.
    t.canvasWidth = 200.0f; t.canvasHeight = 150.0f;
    t.scaleMode = HE::UICanvasScaleMode::Stretch;
    const int box = t.add(HE::UIWidgetType::Accordion);
    { HE::UIElement& e = *t.find(box);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.sizeX = 200.0f; e.sizeY = 150.0f; }
    int buttons[2] = {};
    for (int i = 0; i < 2; ++i)
    {
        const int sec = t.add(HE::UIWidgetType::Panel);
        HE::UIElement& s = *t.find(sec);
        s.parentId = box;
        s.name = i == 0 ? "First" : "Second";
        s.sizeY = 40.0f;
        buttons[i] = t.add(HE::UIWidgetType::Button);
        HE::UIElement& b = *t.find(buttons[i]);
        b.parentId = sec;
        HE::uiSetAnchorPreset(b, HE::kUIAnchorFill);
        HE::uiSetAnchorInsetsX(b, 0.0f, 0.0f);
        HE::uiSetAnchorInsetsY(b, 0.0f, 0.0f);
    }
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    auto* live = dynamic_cast<HE::UIAccordion*>(
        const_cast<HE::UIWidgetTree*>(wm.tree(id))->find(box));
    REQUIRE(live);
    live->headerHeight = 20.0f;
    live->spacing = 0.0f;
    live->allowMultiple = true;
    live->expanded = 0b01;

    // Which button a press landed on — the same probe the Tab Box test uses.
    auto pressedAt = [&](float x, float y)
    {
        wm.processPointer(400.0f, 300.0f, x, y, true, true);
        const HE::UIWidgetTree* tr = wm.tree(id);
        int found = 0;
        for (const auto& ep : tr->elements)
            if (ep && ep->type() == HE::UIWidgetType::Button &&
                HE::uiElementEffectiveVisible(*tr, *ep)) found = ep->id;
        wm.processPointer(400.0f, 300.0f, x, y, false, true);
        return found;
    };
    // The first section's body runs 20..60 in canvas units = 40..120 in pixels.
    CHECK(pressedAt(200.0f, 80.0f) == buttons[0]);
    // The second is folded, so the button in it answers nothing anywhere.
    live->expanded = 0b01;
    wm.processPointer(400.0f, 300.0f, 200.0f, 160.0f, true, true);
    CHECK_FALSE(HE::uiElementEffectiveVisible(*wm.tree(id), *wm.tree(id)->find(buttons[1])));
    wm.processPointer(400.0f, 300.0f, 200.0f, 160.0f, false, true);

    // ── And the headings, at scale 2 ─────────────────────────────────────────
    // The second heading sits at canvas y 60..80 (20 heading + 40 body), which
    // is 120..160 in pixels. A press at 130 must fold it open; a press ten
    // pixels above that is still the first body and must not.
    wm.processPointer(400.0f, 300.0f, 100.0f, 130.0f, true, true);
    wm.processPointer(400.0f, 300.0f, 100.0f, 130.0f, false, true);
    CHECK(live->expanded == 0b11);
    CHECK(HE::uiElementEffectiveVisible(*wm.tree(id), *wm.tree(id)->find(buttons[1])));

    // The first heading (canvas 0..20 = pixels 0..40) folds the first shut.
    wm.processPointer(400.0f, 300.0f, 100.0f, 20.0f, true, true);
    wm.processPointer(400.0f, 300.0f, 100.0f, 20.0f, false, true);
    CHECK(live->expanded == 0b10);
    CHECK_FALSE(HE::uiElementEffectiveVisible(*wm.tree(id), *wm.tree(id)->find(buttons[0])));

    // A press in a BODY is not a heading: it belongs to what the author put
    // there, and folding on it would make a section you cannot use.
    const int before = live->expanded;
    wm.processPointer(400.0f, 300.0f, 100.0f, 80.0f, true, true);
    wm.processPointer(400.0f, 300.0f, 100.0f, 80.0f, false, true);
    CHECK(live->expanded == before);
}

TEST_CASE("Accordion: the mask survives a preview state round-trip")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 300.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int box = t.add(HE::UIWidgetType::Accordion);
    { HE::UIElement& e = *t.find(box);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.sizeX = 400.0f; e.sizeY = 300.0f; }
    for (int i = 0; i < 3; ++i)
    {
        const int sec = t.add(HE::UIWidgetType::Panel);
        t.find(sec)->parentId = box;
        t.find(sec)->name = "S" + std::to_string(i);
    }
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    const_cast<HE::UIWidgetTree*>(wm.tree(id))->find(box)
        ->setPropAny("Expanded", HE::UIPropValue::ofInt(0b101));

    const WidgetManager::StateSnapshot snap = wm.captureState();
    wm.clear();
    const int again = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(again != 0);
    // Freshly built, so it holds what the ASSET says — the authored default.
    CHECK(wm.tree(again)->find(box)->getPropAny("Expanded").i == 1);
    CHECK(wm.restoreState(snap) > 0);
    CHECK(wm.tree(again)->find(box)->getPropAny("Expanded").i == 0b101);
}

TEST_CASE("Accordion: it round-trips")
{
    HE::UIWidgetTree t;
    const int ac = t.add(HE::UIWidgetType::Accordion);
    { auto* a = dynamic_cast<HE::UIAccordion*>(t.find(ac));
      a->expanded = 0b1011; a->allowMultiple = false; a->headerHeight = 33.0f;
      a->spacing = 7.0f; a->textIndent = 21.0f; a->barWidth = 9.0f;
      a->openHeaderColor = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f); }

    HE::UIWidgetTree r;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), r));
    const auto* ra = dynamic_cast<const HE::UIAccordion*>(r.find(ac));
    REQUIRE(ra);
    CHECK(ra->expanded == 0b1011);
    CHECK_FALSE(ra->allowMultiple);
    CHECK(ra->headerHeight == doctest::Approx(33.0f));
    CHECK(ra->spacing == doctest::Approx(7.0f));
    CHECK(ra->textIndent == doctest::Approx(21.0f));
    CHECK(ra->barWidth == doctest::Approx(9.0f));
    CHECK(ra->openHeaderColor.g == doctest::Approx(1.0f));
}

// ═══ The shipped component library ═══════════════════════════════════════════
// EditorDeps/EngineContent/Widgets is generated by widget_gen and COMMITTED, so
// nothing rebuilds it on the way to this test — which is exactly why it needs
// one. Every project inherits these twelve, and a component whose parameter
// points at a property that is not there writes nothing at all, silently.

#ifdef HE_EDITOR_DEPS_DIR
TEST_CASE("Engine components: every shipped one loads, draws, and means what it declares")
{
    const std::filesystem::path dir =
        std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent" / "Widgets";
    REQUIRE(std::filesystem::is_directory(dir));

    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(dir))
        if (entry.path().extension() == ".hasset")
            names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    // The plan's first twelve, plus B9's Divider and Badge. A fifteenth is
    // welcome and has to be looked at: this number is the prompt to check it
    // against the list, not a cap.
    CHECK(names.size() == 14);

    ContentManager cm(dir.string());
    for (const std::string& file : names)
    {
        CAPTURE(file);
        const HE::UUID id = cm.loadAsset(file);
        REQUIRE(id != HE::UUID{});
        const UIWidgetAsset* asset = cm.getWidget(id);
        REQUIRE(asset);

        HE::UIWidgetTree t;
        REQUIRE(HE::uiWidgetTreeFromJson(asset->treeJson, t));
        CHECK_FALSE(t.elements.empty());
        // A component with no knobs is a picture. The generator warns; this is
        // the half that fails the build.
        CHECK_FALSE(t.params.empty());

        for (const HE::UIWidgetParam& p : t.params)
        {
            CAPTURE(p.name);
            HE::UIElement* target = t.find(p.elementId);
            REQUIRE(target);
            // Every knob is explained. The person setting it is looking at the
            // component from the outside and cannot see what it does.
            CHECK_FALSE(p.help.empty());

            // The declaration must actually WRITE. Reading the property back is
            // the only way to tell a wired parameter from one that names a
            // property this element does not have — the second writes nothing
            // and says nothing, which is the failure mode this whole test is
            // about.
            const HE::UIPropValue before = target->getPropAny(p.property);
            HE::UIPropValue after = before;
            switch (before.type)
            {
                case HE::UIPropType::Bool:   after.b = !before.b; break;
                case HE::UIPropType::Float:  after.f = before.f + 7.0f; break;
                case HE::UIPropType::Int:    after.i = before.i + 7; break;
                case HE::UIPropType::String: after.s = before.s + "!"; break;
                case HE::UIPropType::Color:  after.col.r = 1.0f - before.col.r; break;
                case HE::UIPropType::Vec2:   after.v2.x = before.v2.x + 7.0f; break;
                case HE::UIPropType::StringList: after.list.push_back("extra"); break;
            }
            CHECK(HE::uiApplyWidgetParams(t, { { p.name, after } }) == 1);
            const HE::UIPropValue now = t.find(p.elementId)->getPropAny(p.property);
            switch (before.type)
            {
                case HE::UIPropType::Bool:   CHECK(now.b == after.b); break;
                case HE::UIPropType::Float:  CHECK(now.f == doctest::Approx(after.f)); break;
                case HE::UIPropType::Int:    CHECK(now.i == after.i); break;
                case HE::UIPropType::String: CHECK(now.s == after.s); break;
                case HE::UIPropType::Color:  CHECK(now.col.r == doctest::Approx(after.col.r)); break;
                case HE::UIPropType::Vec2:   CHECK(now.v2.x == doctest::Approx(after.v2.x)); break;
                case HE::UIPropType::StringList: CHECK(now.list.size() == after.list.size()); break;
            }
        }
    }
}

// Colours and text sizes in the library come from the THEME, and the literal
// beside each one is only what the designer shows. A component that decided its
// own colours looks right until somebody switches to dark, and then it is a pale
// rectangle on a black page — which is the one failure a library must not have,
// because every project inherits it.
TEST_CASE("Engine components: their colours belong to the theme, not to them")
{
    const std::filesystem::path dir =
        std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent" / "Widgets";
    REQUIRE(std::filesystem::is_directory(dir));
    ContentManager cm(dir.string());

    for (const auto& entry : std::filesystem::directory_iterator(dir))
    {
        if (entry.path().extension() != ".hasset") continue;
        const std::string file = entry.path().filename().string();
        CAPTURE(file);
        const UIWidgetAsset* asset = cm.getWidget(cm.loadAsset(file));
        REQUIRE(asset);
        HE::UIWidgetTree t;
        REQUIRE(HE::uiWidgetTreeFromJson(asset->treeJson, t));

        // Switching the mode has to MOVE something. It cannot say which colour
        // is wrong, but a component that answers "nothing changed" is one that
        // bound no role at all.
        HE::UIWidgetTree light = t, dark = t;
        HE::uiApplyTheme(light, HE::uiDefaultTheme(), HE::UIThemeMode::Light);
        HE::uiApplyTheme(dark,  HE::uiDefaultTheme(), HE::UIThemeMode::Dark);
        CHECK(HE::uiApplyTheme(t, HE::uiDefaultTheme(), HE::UIThemeMode::Dark) > 0);

        bool moved = false;
        for (std::size_t i = 0; i < light.elements.size() && !moved; ++i)
            for (const HE::UIPropDesc& d : light.elements[i]->properties())
                if (d.type == HE::UIPropType::Color &&
                    light.elements[i]->getProp(d.name).col !=
                    dark.elements[i]->getProp(d.name).col)
                { moved = true; break; }
        CHECK(moved);
    }
}
// A contact sheet of the whole library, rendered through the software
// rasterizer and written where a person can look at it. The assertions below can
// say that every component draws SOMETHING; whether it looks like a form row is
// a question about an image, and this is how that question gets asked.
// The report this came from was about the SHIPPED component, so the shipped
// component is what this checks: the flag has to survive the generator, the
// asset, the load and the graft. A rule that only works on a tree built in a
// test is a rule nobody will ever see working.
TEST_CASE("Engine components: the search field wears its ring on the frame")
{
    const std::filesystem::path root =
        std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent";
    REQUIRE(std::filesystem::is_directory(root));
    ContentManager cm(root.string());

    WidgetManager wm;
    const int id = createShown(wm, cm, "Widgets/SearchField.hasset");
    REQUIRE(id != 0);

    // The text field inside, by the name the component gives it.
    int field = 0;
    for (const auto& ep : wm.tree(id)->elements)
        if (ep && ep->name == "Field") field = ep->id;
    REQUIRE(field != 0);
    REQUIRE(wm.setFocus(id, field));

    std::vector<UIRenderObject> out;
    wm.extract(320.0f, 34.0f, out);

    int rings = 0;
    for (const UIRenderObject& ro : out)
    {
        if (ro.borderWidth <= 0.0f || ro.color.a > 0.001f) continue;
        ++rings;
        // The frame is the whole component (320 x 34) and its corner is half its
        // height — a pill. The ring is exactly that, drawn on its edge: NOT the
        // inset field's small rectangle.
        CHECK(ro.size.x == doctest::Approx(320.0f));
        CHECK(ro.size.y == doctest::Approx(34.0f));
        CHECK(ro.cornerRadius.x == doctest::Approx(17.0f));
    }
    CHECK(rings == 1);
}

// …and EMBEDDED in a page, which is how anybody actually uses it. The component
// is grafted into the host tree with its ids shifted and its rect placed by the
// slot, so this is where a rule that reads the tree can go wrong — and where a
// ring drawn two pixels outside the slot can be cut off by it.
TEST_CASE("Engine components: an embedded search field still shows its ring")
{
    const std::filesystem::path root =
        std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent";
    REQUIRE(std::filesystem::is_directory(root));
    ContentManager cm(root.string());

    HE::UIWidgetTree page;
    page.canvasWidth = 600.0f; page.canvasHeight = 200.0f;
    page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int slot = page.add(HE::UIWidgetType::WidgetRef);
    {
        auto* r = dynamic_cast<HE::UIWidgetRef*>(page.find(slot));
        r->widgetPath = "Widgets/SearchField.hasset";
        r->name = "Search";
        HE::uiSetAnchorPreset(*r, 0); r->pivotX = r->pivotY = 0.0f;
        r->posX = 40.0f; r->posY = 30.0f; r->sizeX = 320.0f; r->sizeY = 34.0f;
    }
    registerWidgetAs(cm, "mem://page.hasset", page);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://page.hasset");
    REQUIRE(id != 0);
    int field = 0;
    for (const auto& ep : wm.tree(id)->elements)
        if (ep && ep->name == "Field") field = ep->id;
    REQUIRE(field != 0);
    REQUIRE(wm.setFocus(id, field));

    std::vector<UIRenderObject> out;
    wm.extract(600.0f, 200.0f, out);

    int rings = 0;
    for (const UIRenderObject& ro : out)
    {
        if (ro.borderWidth <= 0.0f || ro.color.a > 0.001f) continue;
        ++rings;
        // Where the slot put it, and the size of the slot: the ring belongs to
        // the component's frame, which fills the slot.
        CHECK(ro.position.x == doctest::Approx(40.0f));
        CHECK(ro.position.y == doctest::Approx(30.0f));
        CHECK(ro.size.x == doctest::Approx(320.0f));
        CHECK(ro.size.y == doctest::Approx(34.0f));
        // …and nothing has clipped it away. A clip rect that excludes the ring
        // is the same as no ring at all, and it is invisible in the numbers
        // above — every one of them would still be right.
        const bool clippedOut = ro.clipRect.z > 0.0f &&
            (ro.position.x + ro.size.x <= ro.clipRect.x ||
             ro.position.x >= ro.clipRect.x + ro.clipRect.z ||
             ro.position.y + ro.size.y <= ro.clipRect.y ||
             ro.position.y >= ro.clipRect.y + ro.clipRect.w);
        CHECK_FALSE(clippedOut);
    }
    CHECK(rings == 1);

    // …and it reaches the SCREEN. Everything above is the model; this is the
    // picture, drawn the way an application draws it. A ring that is emitted
    // correctly and rasterises to nothing is exactly what "no highlight at all"
    // looks like, and none of the numbers above would have noticed.
    HE::sw::Image img;
    img.resize(600, 200);
    img.clear(20, 20, 24, 255);
    HE::sw::draw(img, out);
    int amber = 0;
    for (int y = 20; y < 80; ++y)
        for (int x = 20; x < 400; ++x)
        {
            std::uint8_t r, g, b, a;
            img.pixel(x, y, r, g, b, a);
            if (r > 150 && g > 100 && b < 110) ++amber;
        }
    INFO("amber pixels of the focus ring: " << amber);
    CHECK(amber > 200);   // a 324x38 outline is well over a thousand

    if (const char* dir = std::getenv("HE_UI_DUMP_DIR"); dir && *dir)
        if (FILE* f = std::fopen((std::string(dir) + "/search_focus.ppm").c_str(), "wb"))
        {
            std::fprintf(f, "P6\n%d %d\n255\n", img.width, img.height);
            for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4)
                std::fwrite(&img.rgba[i], 1, 3, f);
            std::fclose(f);
        }
}

TEST_CASE("Engine components: the contact sheet")
{
    const std::filesystem::path dir =
        std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent" / "Widgets";
    REQUIRE(std::filesystem::is_directory(dir));

    std::vector<std::string> names;
    for (const auto& e : std::filesystem::directory_iterator(dir))
        if (e.path().extension() == ".hasset") names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end());

    // The manager reads assets relative to a content root; pointing it at the
    // EngineContent root is what makes "Widgets/Card.hasset" the same file the
    // editor addresses as "Engine/Widgets/Card.hasset".
    ContentManager cm((std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent").string());

    // One column, and each slot is as tall as the component was AUTHORED. A
    // uniform slot would stretch a 62-pixel form row to two hundred and show a
    // label floating in a void — a picture of the sheet's own arithmetic rather
    // than of the component.
    constexpr int kW = 520, kPad = 14;
    struct Slot { std::string path; int y = 0, h = 0; };
    std::vector<Slot> slots;
    int cursor = kPad;
    for (const std::string& file : names)
    {
        const std::string rel = "Widgets/" + file;
        const UIWidgetAsset* a = cm.getWidget(cm.loadAsset(rel));
        REQUIRE(a);
        HE::UIWidgetTree probe;
        // Read out NOW: the next loadAsset moves the dense asset vector and this
        // pointer with it.
        REQUIRE(HE::uiWidgetTreeFromJson(a->treeJson, probe));
        const int h = static_cast<int>(probe.canvasHeight);
        slots.push_back({ rel, cursor, h });
        cursor += h + kPad;
    }
    const int total = cursor;

    HE::UIWidgetTree sheet;
    sheet.canvasWidth = static_cast<float>(kW);
    sheet.canvasHeight = static_cast<float>(total);
    sheet.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    for (const Slot& s : slots)
    {
        const int ref = sheet.add(HE::UIWidgetType::WidgetRef);
        auto* r = dynamic_cast<HE::UIWidgetRef*>(sheet.find(ref));
        REQUIRE(r);
        HE::uiSetAnchorPreset(*r, 0); r->pivotX = r->pivotY = 0.0f;
        r->posX = kPad;
        r->posY = static_cast<float>(s.y);
        r->sizeX = kW - 2.0f * kPad;
        r->sizeY = static_cast<float>(s.h);
        r->widgetPath = s.path;
    }

    registerWidget(cm, sheet);
    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    std::vector<UIRenderObject> out;
    wm.extract(static_cast<float>(kW), static_cast<float>(total), out);

    // Every slot draws something. A component that grafted but laid itself out
    // off its own canvas would leave an empty band, and a band is checkable
    // where "it looks wrong" is not.
    for (const Slot& s : slots)
    {
        CAPTURE(s.path);
        int drawn = 0;
        for (const auto& ro : out)
            if (ro.position.y >= static_cast<float>(s.y) &&
                ro.position.y <  static_cast<float>(s.y + s.h)) ++drawn;
        CHECK(drawn > 0);
    }

    // The manager's OWN mode, not a guess at it: createWidget resolves the roles
    // against whatever mode is in force, so a sheet painted in the other one is
    // dark text on a light page and reads as a broken library. (It read exactly
    // that way the first time, and the components were fine.)
    const glm::vec4 bg = wm.theme().colorFor(HE::UIThemeRole::Background, wm.themeMode());
    HE::sw::Image img;
    img.resize(kW, total);
    img.clear(static_cast<uint8_t>(bg.r * 255.0f), static_cast<uint8_t>(bg.g * 255.0f),
              static_cast<uint8_t>(bg.b * 255.0f), 255);
    HE::sw::draw(img, out);
    const std::filesystem::path shot =
        std::filesystem::temp_directory_path() / "he_components.ppm";
    if (FILE* f = std::fopen(shot.string().c_str(), "wb"))
    {
        std::fprintf(f, "P6\n%d %d\n255\n", img.width, img.height);
        for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4)
            std::fwrite(&img.rgba[i], 1, 3, f);
        std::fclose(f);
        MESSAGE("component sheet written to " << shot.string());
    }
}

// The whole path, end to end, in the one shape that has bitten this codebase
// twice: a PACKAGED build. A page embeds a component by the "Engine/…" path,
// and a shipped game has no EditorDeps at all — the component has to be inside
// the .hpak and the path has to resolve out of its index, or the page draws its
// slot and nothing in it. That is exactly how GameInstance.hcode and the
// path-index bug presented, and neither showed up in the editor.
TEST_CASE("Engine components: a page that embeds one survives being packaged")
{
    const std::filesystem::path src =
        std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent";
    REQUIRE(std::filesystem::is_directory(src / "Widgets"));

    const auto tmp        = std::filesystem::temp_directory_path();
    const auto contentDir = tmp / "he_test_comp_content";
    const auto outputDir  = tmp / "he_test_comp_out";
    std::filesystem::remove_all(contentDir);
    std::filesystem::remove_all(outputDir);
    std::filesystem::create_directories(contentDir);

    // A page with one WidgetRef at a shipped component, told what to say.
    const char* kComponent = "Engine/Widgets/Card.hasset";
    {
        HE::UIWidgetTree page;
        page.canvasWidth = 400.0f; page.canvasHeight = 300.0f;
        page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
        const int ref = page.add(HE::UIWidgetType::WidgetRef);
        auto* r = dynamic_cast<HE::UIWidgetRef*>(page.find(ref));
        REQUIRE(r);
        HE::uiSetAnchorPreset(*r, 0); r->pivotX = r->pivotY = 0.0f;
        r->sizeX = 320.0f; r->sizeY = 180.0f;
        r->widgetPath = kComponent;
        const HE::UIPropValue v = HE::UIPropValue::ofString("Packaged");
        r->setParamValue("Title", &v);

        ContentManager author(contentDir.string());
        UIWidgetAsset a;
        a.type     = HE::AssetType::Widget;
        a.name     = "Page";
        a.path     = "Page.hasset";
        a.id       = HE::UUID{ 0x00000000000009A1ULL, 0x0000000000000001ULL };
        a.treeJson = HE::uiWidgetTreeToJson(page);
        REQUIRE(author.saveAsset(a));
    }

    ExportSettings settings;
    settings.compress         = false;
    settings.engineContentDir = src;
    const auto result = ProjectExporter::exportProject(
        contentDir, "CompGame", "", outputDir, settings);
    REQUIRE(result.success);

    // From here on, only the pak exists — the way a shipped game sees the world.
    ContentManager cm;
    REQUIRE(cm.mountPak((outputDir / "CompGame.hpak").string()));
    REQUIRE(cm.loadAsset(kComponent) != HE::UUID{});

    WidgetManager wm;
    const int id = createShown(wm, cm, "Page.hasset");
    REQUIRE(id != 0);
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 300.0f, out);

    // Eight glyphs for "Packaged". Nine would be the component's own "Card"
    // plus its body line, and zero would be the slot with nothing grafted into
    // it — which is what both of the earlier bugs looked like.
    CHECK(glyphsBetween(out, 0.0f, 300.0f) >= 8);
    int packaged = 0;
    for (const auto& ro : out) if (ro.type == 2 && ro.position.y < 60.0f) ++packaged;
    CHECK(packaged == 8);

    std::filesystem::remove_all(contentDir);
    std::filesystem::remove_all(outputDir);
}
#endif // HE_EDITOR_DEPS_DIR

// A declaration missing either half names nothing. Kept out at LOAD time rather
// than skipped at apply time, so the host's panel never grows a row that writes
// into the void.
TEST_CASE("Parameters: a half-written declaration does not survive loading")
{
    HE::UIWidgetTree t = labelledRow();
    t.params.push_back({ "",     1, "Text", "" });   // no name
    t.params.push_back({ "Nope", 1, "",     "" });   // no property
    t.params.push_back({ "Zero", 0, "Text", "" });   // no element
    HE::UIWidgetTree back;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), back));
    REQUIRE(back.params.size() == 1);
    CHECK(back.params[0].name == "Label");
}

// ── The timeline's arithmetic ────────────────────────────────────────────────
// The strip itself needs a window, but the part of it that can be wrong in a way
// nobody notices — a ruler whose labels crowd, a zoom that slides the view out
// from under the pointer — is arithmetic, and arithmetic can be pinned.
TEST_CASE("Timeline ruler: labels stay on the 1-2-5 ladder and keep their distance")
{
    // The rung below the one that was picked: 1 → 0.5, 2 → 1, 5 → 2. What the
    // function must NOT have returned, because it would crowd the labels.
    auto rungBelow = [](float mant, float decade)
    {
        if (mant < 1.5f) return 0.5f * decade;
        if (mant < 3.0f) return 1.0f * decade;
        return 2.0f * decade;
    };

    // Every rung, at every magnitude, across five orders of pixels-per-second.
    for (float pps = 4.0f; pps < 400000.0f; pps *= 1.17f)
    {
        const float step   = HE::Ed::uiTimelineTickStep(pps, 64.0f);
        const float decade = std::pow(10.0f, std::floor(std::log10(step) + 1e-4f));
        const float mant   = step / decade;

        // The mantissa is 1, 2 or 5 — never 3, never 7. A tick marked 0.3 is
        // one nobody can read a quarter-second off.
        CHECK((std::fabs(mant - 1.0f) < 0.02f ||
               std::fabs(mant - 2.0f) < 0.02f ||
               std::fabs(mant - 5.0f) < 0.05f));
        // Far enough apart to read…
        CHECK(step * pps >= 64.0f * 0.999f);
        // …and no further: the rung below would have crowded them.
        CHECK(rungBelow(mant, decade) * pps < 64.0f * 1.001f);
    }
    // A lane one pixel wide while a panel is being dragged must not divide by
    // zero or hand back an infinity that walks into the ruler's loop.
    const float degenerate = HE::Ed::uiTimelineTickStep(0.0f, 64.0f);
    CHECK(std::isfinite(degenerate));
    CHECK(degenerate > 0.0f);
}

TEST_CASE("Timeline view: seconds and pixels agree in both directions")
{
    HE::Ed::UITimelineView v{ /*laneX*/ 300.0f, /*laneW*/ 600.0f, /*duration*/ 2.0f };

    // At fit, the clip spans the lane exactly.
    CHECK(v.xOf(0.0f) == doctest::Approx(300.0f));
    CHECK(v.xOf(2.0f) == doctest::Approx(900.0f));

    // Zoomed and scrolled, a time still round-trips through pixels.
    v.zoom = 4.0f;
    v.scroll = 0.75f;
    for (float t = 0.75f; t <= 1.25f; t += 0.05f)
        CHECK(v.tOf(v.xOf(t)) == doctest::Approx(t).epsilon(0.001));

    // And what comes back is always a time IN the clip: everything asking is
    // asking "what moment did the pointer land on".
    CHECK(v.tOf(-10000.0f) == doctest::Approx(0.0f));
    CHECK(v.tOf(10000.0f) == doctest::Approx(2.0f));
}

TEST_CASE("Timeline view: zooming leaves the anchored moment under the pointer")
{
    HE::Ed::UITimelineView v{ 300.0f, 600.0f, 10.0f };
    const float anchor = 700.0f;              // some pixel inside the lane
    const float before = v.tOf(anchor);

    v.zoomAt(6.0f, anchor);
    CHECK(v.tOf(anchor) == doctest::Approx(before).epsilon(0.002));
    v.zoomAt(20.0f, anchor);
    CHECK(v.tOf(anchor) == doctest::Approx(before).epsilon(0.002));

    // Zooming out stops at fit rather than sliding off into empty time, and
    // fit has nothing to scroll.
    v.zoomAt(0.2f, anchor);
    CHECK(v.zoom == doctest::Approx(1.0f));
    CHECK(v.scroll == doctest::Approx(0.0f));
}

TEST_CASE("Timeline view: the right edge never runs past the clip")
{
    HE::Ed::UITimelineView v{ 0.0f, 600.0f, 4.0f };
    v.zoom = 2.0f;
    v.scroll = 99.0f;
    v.clampScroll();
    // Half the clip visible → the furthest left edge is halfway in.
    CHECK(v.scroll == doctest::Approx(2.0f));
    CHECK(v.scroll + v.visibleSpan() == doctest::Approx(4.0f));

    // Shortening the clip under a scrolled view pulls it back rather than
    // leaving the lane pointing past the end (Length is a live drag).
    v.duration = 1.0f;
    v.clampScroll();
    CHECK(v.scroll == doctest::Approx(0.5f));
}

TEST_CASE("Timeline view: playback brings the playhead back into view")
{
    HE::Ed::UITimelineView v{ 0.0f, 600.0f, 10.0f };
    v.zoom = 5.0f;                 // two seconds visible
    v.scroll = 0.0f;

    v.reveal(1.0f);                // already visible → nothing moves
    CHECK(v.scroll == doctest::Approx(0.0f));

    v.reveal(5.0f);                // gone past the edge → centred on it
    CHECK(v.scroll == doctest::Approx(4.0f));
    CHECK(v.tOf(v.xOf(5.0f)) == doctest::Approx(5.0f).epsilon(0.001));

    // …and centring never scrolls past the end.
    v.reveal(10.0f);
    CHECK(v.scroll == doctest::Approx(8.0f));
}

// ═══ B7: files dragged in from the desktop ═══════════════════════════════════

namespace
{
// A drop zone with a caption lying across it, an inert panel beside it, and a
// graph that writes what it was given into the caption while counting the
// events. The caption is the point: the pointer meets IT, and the zone is what
// accepts — if the drop did not travel up, nothing would ever hear it.
struct DropFixture
{
    HE::UIWidgetTree  tree;
    HorizonCode::Graph graph;
    int zone = 0, caption = 0, inert = 0;

    DropFixture()
    {
        tree.canvasWidth = 400.0f; tree.canvasHeight = 300.0f;
        tree.scaleMode = HE::UICanvasScaleMode::ConstantPixel;

        zone = tree.add(HE::UIWidgetType::Panel);
        {
            HE::UIElement& e = *tree.find(zone);
            e.name = "Zone"; e.acceptsDrop = true;
            HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
            e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 300.0f;
        }
        caption = tree.add(HE::UIWidgetType::Text);
        {
            HE::UIElement& e = *tree.find(caption);
            e.parentId = zone; e.name = "Caption";
            e.setProp("Text", HE::UIPropValue::ofString(""));
            e.setProp("AutoSize", HE::UIPropValue::ofBool(false));
            // Inset, not filling: the ring has to be provably on the ZONE, and
            // a caption whose rect is the zone's rect could not tell the two
            // apart — the assertion would pass either way, which is the same as
            // not making it.
            HE::uiSetAnchorPreset(e, HE::kUIAnchorFill);
            HE::uiSetAnchorInsetsX(e, 20.0f, 20.0f);
            HE::uiSetAnchorInsetsY(e, 20.0f, 20.0f);
        }
        inert = tree.add(HE::UIWidgetType::Panel);
        {
            HE::UIElement& e = *tree.find(inert);
            e.name = "Inert";
            HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
            e.posX = 200.0f; e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 300.0f;
        }

        HorizonCode::Variable count;
        count.name = "count"; count.type = PinType::Int;
        graph.variables.push_back(count);

        HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "OnFileDropped";
        ev.elem = zone; ev.hasArg = true; ev.propType = PinType::String;
        const int evId = graph.addNode(ev);
        HorizonCode::Node set; set.type = NodeType::SetProperty; set.elem = caption;
        set.s = "Text"; set.propType = PinType::String;
        const int setId = graph.addNode(set);
        REQUIRE(graph.connect(evId, 0, setId, 0));   // exec
        REQUIRE(graph.connect(evId, 1, setId, 2));   // the path → the caption

        // …and one up on the counter, so "one event per file" is a number and
        // not an impression.
        HorizonCode::Node get; get.type = NodeType::GetVariable; get.s = "count";
        get.propType = PinType::Int;
        const int getId = graph.addNode(get);
        HorizonCode::Node one; one.type = NodeType::ConstInt; one.f[0] = 1.0f;
        const int oneId = graph.addNode(one);
        HorizonCode::Node add; add.type = NodeType::Add;
        const int addId = graph.addNode(add);
        HorizonCode::Node inc; inc.type = NodeType::SetVariable; inc.s = "count";
        inc.propType = PinType::Int;
        const int incId = graph.addNode(inc);
        REQUIRE(graph.connect(getId, 0, addId, 0));
        REQUIRE(graph.connect(oneId, 0, addId, 1));
        REQUIRE(graph.connect(setId, 1, incId, 0));  // exec, after the caption
        REQUIRE(graph.connect(addId, 2, incId, 2));
    }
};

// The drop ring: an outlined rectangle with no fill, in the drop colour rather
// than the focus one. Counting THAT is what tells the two rings apart.
int countDropRings(const std::vector<UIRenderObject>& out)
{
    int n = 0;
    for (const UIRenderObject& o : out)
        if (o.borderWidth > 0.0f && o.color.a <= 0.001f && o.borderColor.b > 0.9f)
            ++n;
    return n;
}
}

TEST_CASE("Drop: the file lands on the element that ACCEPTS it, not on the one it was let go over")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    DropFixture f;
    registerWidget(cm, f.tree, &f.graph);

    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    const auto inst = static_cast<HorizonCode::InstanceId>(id);

    // Let go over the CAPTION, which accepts nothing. The zone under it does,
    // and that is where the event has to arrive.
    CHECK(wm.processDrop(400.0f, 300.0f, 100.0f, 150.0f, { "/tmp/a.txt" }));
    auto captionText = [&] { return wm.tree(id)->find(f.caption)->getProp("Text").s; };
    CHECK(captionText() == "/tmp/a.txt");
    CHECK(rt.getVariable(inst, "count").i == 1);

    // Beside it, over a panel that never said it takes drops: nothing hears it,
    // and the caption keeps what it had. An element that has not opted in is
    // transparent to a drop, which is the whole meaning of the flag.
    CHECK_FALSE(wm.processDrop(400.0f, 300.0f, 300.0f, 150.0f, { "/tmp/b.txt" }));
    CHECK(captionText() == "/tmp/a.txt");
    CHECK(rt.getVariable(inst, "count").i == 1);

    // Three files at once are three events. A drop of many is one gesture and
    // many answers — the last one is what the caption ends up showing.
    CHECK(wm.processDrop(400.0f, 300.0f, 100.0f, 150.0f,
                         { "/tmp/1.txt", "/tmp/2.txt", "/tmp/3.txt" }));
    CHECK(rt.getVariable(inst, "count").i == 4);
    CHECK(captionText() == "/tmp/3.txt");

    // An empty drop is not a drop. The OS sends the end of a drag that carried
    // nothing too, and firing on it would be an application opening a file it
    // was never given.
    CHECK_FALSE(wm.processDrop(400.0f, 300.0f, 100.0f, 150.0f, {}));
    CHECK(rt.getVariable(inst, "count").i == 4);
}

// ── The application's menu bar (plan A6) ─────────────────────────────────────
namespace
{
    // A bar with two menus, the first of which has a separator in it — enough
    // shape for every question below and no more.
    std::vector<HE::AppMenu> sampleMenuBar()
    {
        HE::AppMenu file;
        file.id = "file"; file.label = "File";
        file.items.push_back({ "new",  "New",   false });
        file.items.push_back({ "open", "Open…", false });
        file.items.push_back({ "",     "",      true  });
        file.items.push_back({ "quit", "Quit",  false });
        HE::AppMenu edit;
        edit.id = "edit"; edit.label = "Edit";
        edit.items.push_back({ "undo", "Undo", false });
        return { file, edit };
    }
}

TEST_CASE("The menu bar grows with the reader's text size, drawing and hit test together")
{
    // The bar is chrome and does not scale with any canvas — but a reader who
    // enlarged the text and got a menu that stayed tiny has been sold half a
    // feature. The risk is the same one as everywhere else in B10, and it is why
    // the seven constants became one bundle: a bar drawn taller than the band it
    // answers clicks in is worse than a small bar.
    WidgetManager wm;
    wm.setMenuBar(sampleMenuBar());
    const float h100 = wm.menuBarHeight();
    float x100 = 0.0f, w100 = 0.0f;
    REQUIRE(wm.menuTitleBox(1, x100, w100));   // "Edit", the second one

    wm.setFontScale(2.0f);
    const float h200 = wm.menuBarHeight();
    CHECK(h200 > h100);
    float x200 = 0.0f, w200 = 0.0f;
    REQUIRE(wm.menuTitleBox(1, x200, w200));
    CHECK(w200 > w100);   // a wider word wants a wider title
    CHECK(x200 > x100);   // …and pushes the one beside it along

    // What is DRAWN is that same height, which is the whole point of the bundle.
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 300.0f, out);
    bool drawnBar = false;
    for (const UIRenderObject& o : out)
        if (o.size.x == doctest::Approx(400.0f) && o.size.y == doctest::Approx(h200))
            drawnBar = true;
    CHECK(drawnBar);

    // And the pointer agrees. The row that matters is the band the bar GREW
    // into: at 100 % it was under the bar and hit nothing, and it has to open a
    // menu now, or the strip is drawn somewhere it cannot be clicked.
    const float grownY = (h100 + h200) * 0.5f;
    const float midX   = x200 + w200 * 0.5f;
    REQUIRE(wm.processPointer(400.0f, 300.0f, midX, grownY, true, true));
    CHECK(wm.openMenu() == 1);

    // The same point on a bar that was never enlarged opens nothing, which is
    // what makes the line above a statement about the SIZE and not about a
    // manager that opens a menu wherever it is poked.
    WidgetManager small;
    small.setMenuBar(sampleMenuBar());
    small.processPointer(400.0f, 300.0f, midX, grownY, true, true);
    CHECK(small.openMenu() == -1);
}

TEST_CASE("The menu bar opens on a title, chooses on release, and closes on Escape")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    // A page underneath, so "the bar eats its own band" is a claim about
    // something rather than about an empty screen.
    HE::UIWidgetTree tree;
    tree.canvasWidth = 400.0f; tree.canvasHeight = 300.0f;
    tree.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int panel = tree.add(HE::UIWidgetType::Button);
    {
        HE::UIElement& e = *tree.find(panel);
        e.name = "Under";
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 400.0f; e.sizeY = 300.0f;
    }
    registerWidget(cm, tree, nullptr);
    HorizonCode::Runtime rt;
    // The menu belongs to the APPLICATION, so its entries arrive at the
    // application's own script — the same door a drop nobody accepted uses.
    HorizonCode::Graph app;
    {
        HorizonCode::Variable chosen;
        chosen.name = "chosen"; chosen.type = PinType::String;
        app.variables.push_back(chosen);
        HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "OnMenuItem";
        ev.elem = 0; ev.hasArg = true; ev.propType = PinType::String;
        const int evId = app.addNode(ev);
        HorizonCode::Node set; set.type = NodeType::SetVariable; set.s = "chosen";
        set.propType = PinType::String;
        const int setId = app.addNode(set);
        REQUIRE(app.connect(evId, 0, setId, 0));
        REQUIRE(app.connect(evId, 1, setId, 2));
    }
    const HorizonCode::InstanceId gi = rt.setGameInstance(app);
    REQUIRE(gi != 0);

    WidgetManager wm;
    wm.setRuntime(&rt);
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    (void)panel;

    wm.setMenuBar(sampleMenuBar());
    CHECK(wm.menuBarHeight() > 0.0f);
    CHECK(wm.openMenu() == -1);

    const float barMid = wm.menuBarHeight() * 0.5f;
    // Press on "File" opens it, and the press is not a click on the page below:
    // a bar the page can be clicked through would not be a bar.
    CHECK(wm.processPointer(400.0f, 300.0f, 20.0f, barMid, true, true));
    CHECK(wm.openMenu() == 0);
    CHECK(wm.hasLayer());
    // Releasing on the title keeps it open — that release belongs to the press
    // that opened the menu, and closing there makes a menu impossible to use.
    wm.processPointer(400.0f, 300.0f, 20.0f, barMid, false, true);
    CHECK(wm.openMenu() == 0);

    // Down the list to "Open…", and let go: the id arrives, not the label.
    const float itemY = wm.menuBarHeight() + 12.0f + 24.0f;   // second row
    wm.processPointer(400.0f, 300.0f, 30.0f, itemY, true, true);
    wm.processPointer(400.0f, 300.0f, 30.0f, itemY, false, true);
    CHECK(wm.openMenu() == -1);          // choosing closes it
    CHECK_FALSE(wm.hasLayer());
    CHECK(rt.getVariable(gi, "chosen").s == "open");   // the id, not the label

    // Escape closes an open menu, through the same door that closes a dialog.
    wm.processPointer(400.0f, 300.0f, 20.0f, barMid, true, true);
    CHECK(wm.openMenu() == 0);
    CHECK(wm.closeTopLayer());
    CHECK(wm.openMenu() == -1);
    CHECK_FALSE(wm.hasLayer());
}

TEST_CASE("Walking onto the next title switches menus without letting go")
{
    // The one thing a menu BAR does that a dropdown cannot. Without it a menu
    // bar is a row of separate dropdowns, and everyone can feel the difference
    // even if nobody names it.
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree tree;
    tree.canvasWidth = 400.0f; tree.canvasHeight = 300.0f;
    tree.add(HE::UIWidgetType::Panel);
    registerWidget(cm, tree, nullptr);

    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    wm.setMenuBar(sampleMenuBar());

    const float barMid = wm.menuBarHeight() * 0.5f;
    wm.processPointer(400.0f, 300.0f, 20.0f, barMid, true, true);
    REQUIRE(wm.openMenu() == 0);
    // The button is still down, and the pointer moves onto "Edit". (Its box
    // starts where "File"'s ends, which is why the x is asked for rather than
    // guessed — a title is as wide as its word.)
    float ex = 0.0f, ew = 0.0f;
    REQUIRE(wm.menuTitleBox(1, ex, ew));
    wm.processPointer(400.0f, 300.0f, ex + ew * 0.5f, barMid, true, true);
    CHECK(wm.openMenu() == 1);
    // …and there is still exactly ONE layer: switching is not closing and
    // opening, or the page underneath would flicker back into reach between them.
    CHECK(wm.hasLayer());
    wm.closeTopLayer();
    CHECK_FALSE(wm.hasLayer());
}

TEST_CASE("A separator is a line, not a row that can be chosen")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree tree;
    tree.canvasWidth = 400.0f; tree.canvasHeight = 300.0f;
    tree.add(HE::UIWidgetType::Panel);
    registerWidget(cm, tree, nullptr);

    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    wm.setMenuBar(sampleMenuBar());

    // Drawn: the strip, its two titles and — once open — the card and its rows.
    std::vector<UIRenderObject> before;
    wm.extract(400.0f, 300.0f, before);
    const std::size_t closed = before.size();

    wm.processPointer(400.0f, 300.0f, 20.0f, wm.menuBarHeight() * 0.5f, true, true);
    REQUIRE(wm.openMenu() == 0);
    std::vector<UIRenderObject> after;
    wm.extract(400.0f, 300.0f, after);
    CHECK(after.size() > closed);      // the open menu is drawn over the page

    // Releasing on the separator's line chooses nothing and leaves the menu
    // open: it has no id, so there is nothing it could mean.
    const float sepY = wm.menuBarHeight() + 24.0f * 2.0f + 3.0f;
    wm.processPointer(400.0f, 300.0f, 30.0f, sepY, true, true);
    wm.processPointer(400.0f, 300.0f, 30.0f, sepY, false, true);
    CHECK(wm.openMenu() == 0);
}

// ── enabled / checked (plan A6, nachgereicht) ────────────────────────────────
TEST_CASE("A greyed-out entry cannot be chosen, by click or by chord")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree tree;
    tree.canvasWidth = 400.0f; tree.canvasHeight = 300.0f;
    tree.add(HE::UIWidgetType::Panel);
    registerWidget(cm, tree, nullptr);

    HorizonCode::Runtime rt;
    HorizonCode::Graph app;
    {
        HorizonCode::Variable chosen;
        chosen.name = "chosen"; chosen.type = PinType::String;
        app.variables.push_back(chosen);
        HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "OnMenuItem";
        ev.elem = 0; ev.hasArg = true; ev.propType = PinType::String;
        const int evId = app.addNode(ev);
        HorizonCode::Node set; set.type = NodeType::SetVariable; set.s = "chosen";
        set.propType = PinType::String;
        const int setId = app.addNode(set);
        REQUIRE(app.connect(evId, 0, setId, 0));
        REQUIRE(app.connect(evId, 1, setId, 2));
    }
    const HorizonCode::InstanceId gi = rt.setGameInstance(app);
    REQUIRE(gi != 0);

    WidgetManager wm;
    wm.setRuntime(&rt);
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);

    std::vector<HE::AppMenu> bar = sampleMenuBar();
    bar[0].items[1].shortcut = "Ctrl+O";     // "Open…", which is about to go grey
    wm.setMenuBar(bar);
    REQUIRE(wm.setMenuItemEnabled("open", false));
    CHECK_FALSE(wm.menuItemEnabled("open"));
    CHECK(wm.menuItemEnabled("new"));
    // An id nothing carries is not an entry that exists and is off.
    CHECK_FALSE(wm.setMenuItemEnabled("nosuch", false));

    // Releasing on it chooses nothing and leaves the menu open — exactly what a
    // separator does, because neither can be chosen.
    const float barMid = wm.menuBarHeight() * 0.5f;
    wm.processPointer(400.0f, 300.0f, 20.0f, barMid, true, true);
    REQUIRE(wm.openMenu() == 0);
    const float rowY = wm.menuBarHeight() + 24.0f * 1.5f;   // the middle of "Open…"
    wm.processPointer(400.0f, 300.0f, 30.0f, rowY, true, true);
    wm.processPointer(400.0f, 300.0f, 30.0f, rowY, false, true);
    CHECK(wm.openMenu() == 0);
    CHECK(rt.getVariable(gi, "chosen").s.empty());

    // …and the chord is swallowed rather than passed on. TRUE with nothing
    // fired: the key belonged to a menu that has just said it cannot act, and
    // letting it through would fire whatever the game binds to Ctrl+O at
    // exactly that moment.
    CHECK(wm.fireMenuShortcut("O", true, false, false));
    CHECK(rt.getVariable(gi, "chosen").s.empty());
    // Whose chord it is does not change with the state of the entry — that
    // question is about the bar, not about what the row can do today.
    CHECK(wm.menuShortcutTarget("O", true, false, false) == "open");

    // Switched back on, both doors work again.
    REQUIRE(wm.setMenuItemEnabled("open", true));
    CHECK(wm.fireMenuShortcut("O", true, false, false));
    CHECK(rt.getVariable(gi, "chosen").s == "open");
}

TEST_CASE("A tick is drawn, and setting one leaves an open menu open")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree tree;
    tree.canvasWidth = 400.0f; tree.canvasHeight = 300.0f;
    tree.add(HE::UIWidgetType::Panel);
    registerWidget(cm, tree, nullptr);

    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    wm.setMenuBar(sampleMenuBar());

    wm.processPointer(400.0f, 300.0f, 20.0f, wm.menuBarHeight() * 0.5f, true, true);
    REQUIRE(wm.openMenu() == 0);
    std::vector<UIRenderObject> plain;
    wm.extract(400.0f, 300.0f, plain);

    CHECK_FALSE(wm.menuItemChecked("new"));
    REQUIRE(wm.setMenuItemChecked("new", true));
    CHECK(wm.menuItemChecked("new"));
    // The menu the person is reading stays open. This is the whole reason these
    // two do not go through setMenuBar: greying out Paste as the clipboard
    // empties must not shut the menu it is being read in.
    CHECK(wm.openMenu() == 0);

    std::vector<UIRenderObject> ticked;
    wm.extract(400.0f, 300.0f, ticked);
    // Exactly one more quad: the mark. Not a glyph — the shipped font has no
    // check — so it is the same filled square UICheckBox draws.
    CHECK(ticked.size() == plain.size() + 1);

    REQUIRE(wm.setMenuItemChecked("new", false));
    std::vector<UIRenderObject> unticked;
    wm.extract(400.0f, 300.0f, unticked);
    CHECK(unticked.size() == plain.size());
}

// ── Shortcuts (plan B10) ─────────────────────────────────────────────────────
TEST_CASE("A chord is a set of modifiers and one key, however it is spelled")
{
    HE::UIShortcut s;

    // The plain reading, and the canonical spelling back out of it.
    REQUIRE(HE::uiParseShortcut("Ctrl+Shift+S", s));
    CHECK(s.key == "S");
    CHECK(s.ctrl);
    CHECK(s.shift);
    CHECK_FALSE(s.alt);
    CHECK(HE::uiFormatShortcut(s) == "Ctrl+Shift+S");

    // The modifiers are a SET: a file somebody typed in the other order is the
    // same chord, and rejecting it would be rejecting a correct file. Case and
    // spaces go the same way.
    HE::UIShortcut other;
    REQUIRE(HE::uiParseShortcut("shift + ctrl + s", other));
    CHECK(other == s);

    // Cmd IS Ctrl here — one flag, so an application writes the chord once and
    // gets Ctrl on Windows and Command on a Mac out of the same text.
    HE::UIShortcut mac;
    REQUIRE(HE::uiParseShortcut("Cmd+Shift+S", mac));
    CHECK(mac == s);
    for (const char* alias : { "Command+S", "Meta+S", "Super+S", "Win+S", "Control+S" })
    {
        HE::UIShortcut a;
        REQUIRE_MESSAGE(HE::uiParseShortcut(alias, a), alias);
        CHECK_MESSAGE(a.ctrl, alias);
        CHECK_MESSAGE(a.key == "S", alias);
    }
    // …and Option is Alt, from the same platform's vocabulary.
    HE::UIShortcut opt;
    REQUIRE(HE::uiParseShortcut("Option+Left", opt));
    CHECK(opt.alt);
    CHECK(opt.key == "Left");

    // The named keys, in the spelling SDL_GetKeyName gives back — that string
    // is what the host compares against, so a table with its own spellings
    // would match nothing at all.
    const struct { const char* written; const char* canonical; } kNames[] = {
        { "Enter", "Return" },   { "Esc", "Escape" },      { "Del", "Delete" },
        { "PgUp", "PageUp" },    { "page down", "PageDown" },
        { "F5", "F5" },          { "f12", "F12" },         { "Space", "Space" },
        { "Backspace", "Backspace" }, { "Tab", "Tab" },    { "7", "7" },
    };
    for (const auto& n : kNames)
    {
        HE::UIShortcut k;
        REQUIRE_MESSAGE(HE::uiParseShortcut(n.written, k), n.written);
        CHECK_MESSAGE(k.key == n.canonical, n.written);
    }

    // What is NOT a chord. Every one of these used to be a way of writing a
    // shortcut that parses into something plausible and then never fires.
    for (const char* bad : { "", "   ", "Ctrl", "Ctrl+", "+S", "Ctrl+S+A",
                             "Ctrl+F13", "Ctrl+Fx", "Ctrl+Nonsense", "Ctrl+;" })
    {
        HE::UIShortcut b;
        b.key = "sentinel";
        CHECK_MESSAGE(!HE::uiParseShortcut(bad, b), bad);
        CHECK_MESSAGE(b.key == "sentinel", bad);   // a refusal leaves `out` alone
    }

    // Matching compares every modifier, the absent ones included: an
    // application that binds both Ctrl+S and Ctrl+Shift+S must get two
    // different entries out of them.
    CHECK(HE::uiShortcutMatchesText("Ctrl+S", "S", true, false, false));
    CHECK_FALSE(HE::uiShortcutMatchesText("Ctrl+S", "S", true, true, false));
    CHECK_FALSE(HE::uiShortcutMatchesText("Ctrl+S", "S", false, false, false));
    CHECK(HE::uiShortcutMatchesText("Ctrl+S", "s", true, false, false));  // name case
    // A chord that does not parse fires on nothing rather than on everything.
    CHECK_FALSE(HE::uiShortcutMatchesText("Ctrl+Nonsense", "S", true, false, false));
    // The name a host hands over is squeezed too, so the one spelling this
    // engine cannot check at compile time cannot bite either.
    CHECK(HE::uiShortcutMatchesText("Ctrl+PgUp", "PageUp", true, false, false));
    CHECK(HE::uiShortcutMatchesText("Ctrl+PgUp", "Page Up", true, false, false));
    // The keypad's Enter is Enter: a different scancode, a different name out of
    // SDL, and nobody means a different command by pressing it.
    CHECK(HE::uiShortcutMatchesText("Ctrl+Enter", "Keypad Enter", true, false, false));
}

TEST_CASE("A shortcut chooses the entry through the same door the click uses")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree tree;
    tree.canvasWidth = 400.0f; tree.canvasHeight = 300.0f;
    tree.add(HE::UIWidgetType::Panel);
    registerWidget(cm, tree, nullptr);

    // The same OnMenuItem graph a clicked entry arrives at — that is the claim:
    // a shortcut is the entry answered a faster way, not a second event.
    HorizonCode::Runtime rt;
    HorizonCode::Graph app;
    {
        HorizonCode::Variable chosen;
        chosen.name = "chosen"; chosen.type = PinType::String;
        app.variables.push_back(chosen);
        HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "OnMenuItem";
        ev.elem = 0; ev.hasArg = true; ev.propType = PinType::String;
        const int evId = app.addNode(ev);
        HorizonCode::Node set; set.type = NodeType::SetVariable; set.s = "chosen";
        set.propType = PinType::String;
        const int setId = app.addNode(set);
        REQUIRE(app.connect(evId, 0, setId, 0));
        REQUIRE(app.connect(evId, 1, setId, 2));
    }
    const HorizonCode::InstanceId gi = rt.setGameInstance(app);
    REQUIRE(gi != 0);

    WidgetManager wm;
    wm.setRuntime(&rt);
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);

    std::vector<HE::AppMenu> bar = sampleMenuBar();
    bar[0].items[0].shortcut = "Ctrl+N";        // New
    bar[0].items[1].shortcut = "Ctrl+Shift+O";  // Open…
    bar[0].items[3].shortcut = "Ctrl+Q";        // Quit
    bar[1].items[0].shortcut = "F5";            // Undo, a bare key on purpose
    wm.setMenuBar(bar);

    // The id arrives, not the label — the same thing the click test pins.
    CHECK(wm.fireMenuShortcut("N", true, false, false));
    CHECK(rt.getVariable(gi, "chosen").s == "new");

    // A chord in a LATER menu is reached too: the bar is searched, not just the
    // menu that happens to be open.
    CHECK(wm.fireMenuShortcut("F5", false, false, false));
    CHECK(rt.getVariable(gi, "chosen").s == "undo");

    // Ctrl+O and Ctrl+Shift+O are two different chords, and only one of them is
    // bound. Firing the near miss would make the two impossible to tell apart.
    rt.setVariable(gi, "chosen", HorizonCode::Value::ofString(""));
    CHECK_FALSE(wm.fireMenuShortcut("O", true, false, false));
    CHECK(rt.getVariable(gi, "chosen").s.empty());
    CHECK(wm.fireMenuShortcut("O", true, true, false));
    CHECK(rt.getVariable(gi, "chosen").s == "open");

    // Nothing is bound to this one, and nothing must be invented for it.
    CHECK_FALSE(wm.fireMenuShortcut("Z", true, false, false));

    // An entry whose chord cannot be read has no chord: it must not fire on the
    // key it looks like it names, or a typo becomes a shortcut nobody wrote.
    bar[1].items[0].shortcut = "Ctrl+Nonsense";
    wm.setMenuBar(bar);
    rt.setVariable(gi, "chosen", HorizonCode::Value::ofString(""));
    CHECK_FALSE(wm.fireMenuShortcut("Nonsense", true, false, false));
    CHECK(rt.getVariable(gi, "chosen").s.empty());

    // A shortcut is the menu answered without opening it, so an open one is
    // answered too and does not stay hanging over the page.
    wm.processPointer(400.0f, 300.0f, 20.0f, wm.menuBarHeight() * 0.5f, true, true);
    REQUIRE(wm.openMenu() == 0);
    CHECK(wm.fireMenuShortcut("Q", true, false, false));
    CHECK(rt.getVariable(gi, "chosen").s == "quit");
    CHECK(wm.openMenu() == -1);
    CHECK_FALSE(wm.hasLayer());
}

TEST_CASE("With the system bar up a Command chord is owned here and fired there")
{
    // The macOS double-fire, and the only half of it that can be tested without
    // Cocoa. AppKit chooses the entry off its own key equivalent, and SDL
    // reports the very same press anyway — so the manager must answer "yes,
    // that key belongs to a menu" and fire NOTHING. Answering false instead
    // would let the key through to the game behind a menu that just acted on
    // it; firing would choose the entry twice on one keystroke.
    //
    // …and only for the chords AppKit was actually GIVEN, which is the ones
    // with Command in them. A key equivalent grabs its key across the whole
    // application, so a bare F5 handed to AppKit would fire while somebody
    // fills in a form. Those stay here even under a native bar.
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree tree;
    tree.canvasWidth = 400.0f; tree.canvasHeight = 300.0f;
    tree.add(HE::UIWidgetType::Panel);
    registerWidget(cm, tree, nullptr);

    HorizonCode::Runtime rt;
    HorizonCode::Graph app;
    {
        HorizonCode::Variable chosen;
        chosen.name = "chosen"; chosen.type = PinType::String;
        app.variables.push_back(chosen);
        HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "OnMenuItem";
        ev.elem = 0; ev.hasArg = true; ev.propType = PinType::String;
        const int evId = app.addNode(ev);
        HorizonCode::Node set; set.type = NodeType::SetVariable; set.s = "chosen";
        set.propType = PinType::String;
        const int setId = app.addNode(set);
        REQUIRE(app.connect(evId, 0, setId, 0));
        REQUIRE(app.connect(evId, 1, setId, 2));
    }
    const HorizonCode::InstanceId gi = rt.setGameInstance(app);
    REQUIRE(gi != 0);

    WidgetManager wm;
    wm.setRuntime(&rt);
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    std::vector<HE::AppMenu> bar = sampleMenuBar();
    bar[0].items[0].shortcut = "Ctrl+N";
    bar[1].items[0].shortcut = "F5";     // no Command: AppKit is not given this
    wm.setMenuBar(bar);
    wm.setMenuBarNative(true);

    CHECK(wm.fireMenuShortcut("N", true, false, false));       // owned
    CHECK(rt.getVariable(gi, "chosen").s.empty());             // …and not fired

    // The Command-less one still fires here, and that is not a detail: it is
    // the only place it CAN fire, because it was never handed to AppKit.
    CHECK(wm.fireMenuShortcut("F5", false, false, false));
    CHECK(rt.getVariable(gi, "chosen").s == "undo");
    rt.setVariable(gi, "chosen", HorizonCode::Value::ofString(""));

    // Who owns a chord is the same question on every platform, and it answers
    // the same: the bar is the same bar whoever draws it.
    CHECK(wm.menuShortcutTarget("N", true, false, false) == "new");
    CHECK(wm.menuShortcutTarget("N", false, false, false).empty());

    // Back to the drawn strip, and it fires again — which is what makes the
    // line above a statement about the native bar and not about a manager that
    // has stopped firing altogether.
    wm.setMenuBarNative(false);
    CHECK(wm.fireMenuShortcut("N", true, false, false));
    CHECK(rt.getVariable(gi, "chosen").s == "new");
}

TEST_CASE("A row is as wide as its label AND its chord, drawing and hit test together")
{
    // The chord is drawn against the right edge of the row, so it is part of
    // how wide the row is. Painting it over the end of a card measured without
    // it would put the picture and the click at different sizes — the mistake
    // the bar's own metrics bundle exists to prevent.
    WidgetManager plain;
    std::vector<HE::AppMenu> bare = sampleMenuBar();
    // A long label, so the card is wider than its 120 px minimum and the growth
    // below is the chord's doing rather than the floor's.
    bare[0].items[1].label = "Open a document from somewhere";
    plain.setMenuBar(bare);
    plain.processPointer(400.0f, 300.0f, 20.0f, plain.menuBarHeight() * 0.5f, true, true);
    REQUIRE(plain.openMenu() == 0);
    float px = 0.0f, py = 0.0f, pw = 0.0f, ph = 0.0f;
    REQUIRE(plain.menuPopupBox(px, py, pw, ph));

    WidgetManager withChord;
    std::vector<HE::AppMenu> bound = bare;
    bound[0].items[1].shortcut = "Ctrl+Shift+O";
    withChord.setMenuBar(bound);
    withChord.processPointer(400.0f, 300.0f, 20.0f, withChord.menuBarHeight() * 0.5f, true, true);
    REQUIRE(withChord.openMenu() == 0);
    float qx = 0.0f, qy = 0.0f, qw = 0.0f, qh = 0.0f;
    REQUIRE(withChord.menuPopupBox(qx, qy, qw, qh));
    CHECK(qw > pw);          // the chord asked for the room
    CHECK(qh == doctest::Approx(ph));   // …and only sideways: a row is a row

    // And it is really drawn: the same card with the chord emits more glyphs.
    std::vector<UIRenderObject> a, b;
    plain.extract(400.0f, 300.0f, a);
    withChord.extract(400.0f, 300.0f, b);
    CHECK(b.size() > a.size());

    // An unreadable chord is drawn as nothing at all, because the row would not
    // answer to it either — a shortcut printed beside an entry that ignores it
    // is worse than no shortcut.
    WidgetManager broken;
    std::vector<HE::AppMenu> bad = bare;
    bad[0].items[1].shortcut = "Ctrl+Nonsense";
    broken.setMenuBar(bad);
    broken.processPointer(400.0f, 300.0f, 20.0f, broken.menuBarHeight() * 0.5f, true, true);
    REQUIRE(broken.openMenu() == 0);
    float rx = 0.0f, ry = 0.0f, rw = 0.0f, rh = 0.0f;
    REQUIRE(broken.menuPopupBox(rx, ry, rw, rh));
    CHECK(rw == doctest::Approx(pw));
}

TEST_CASE("With a native bar the strip is gone, band and all")
{
    // macOS puts these menus in the SYSTEM bar (HE_Game/AppMacMenu), which is
    // outside the window. The Cocoa half cannot be tested here, but the half
    // that decides whether the window ALSO has a bar can, and that is the half
    // that would show as two menu bars carrying the same entries.
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    // A page that reaches all the way up, and says so when it is clicked: the
    // band the strip used to own is the only place this button is asked about.
    HE::UIWidgetTree tree;
    tree.canvasWidth = 400.0f; tree.canvasHeight = 300.0f;
    tree.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int label = tree.add(HE::UIWidgetType::Text);
    tree.find(label)->setProp("Text", HE::UIPropValue::ofString(""));
    const int btn = tree.add(HE::UIWidgetType::Button);
    {
        HE::UIElement& e = *tree.find(btn);
        e.name = "Under";
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 400.0f; e.sizeY = 300.0f;
    }
    HorizonCode::Graph g;
    {
        HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "OnClicked"; ev.elem = btn;
        const int evId = g.addNode(ev);
        HorizonCode::Node lit; lit.type = NodeType::ConstString; lit.s = "OK";
        const int litId = g.addNode(lit);
        HorizonCode::Node set; set.type = NodeType::SetProperty;
        set.elem = label; set.s = "Text"; set.propType = PinType::String;
        const int setId = g.addNode(set);
        REQUIRE(g.connect(evId, 0, setId, 0));
        REQUIRE(g.connect(litId, 0, setId, 2));
    }
    registerWidget(cm, tree, &g);

    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);
    REQUIRE(createShown(wm, cm, "mem://w.hasset") != 0);
    wm.setMenuBar(sampleMenuBar());

    const float drawnHeight = wm.menuBarHeight();
    REQUIRE(drawnHeight > 0.0f);
    std::vector<UIRenderObject> drawn;
    wm.extract(400.0f, 300.0f, drawn);

    wm.setMenuBarNative(true);
    CHECK(wm.menuBarNative());
    // No band to keep clear: the bar is not in this window, and a page told to
    // leave room for it would leave room for nothing.
    CHECK(wm.menuBarHeight() == 0.0f);
    // …and nothing of it is drawn any more. Fewer objects than with the strip,
    // which is the only way to say "the strip is gone" without naming its parts.
    std::vector<UIRenderObject> nowNative;
    wm.extract(400.0f, 300.0f, nowNative);
    CHECK(nowNative.size() < drawn.size());

    // A click where the strip used to be reaches the page — and opens nothing:
    // a grab for a menu nobody can see would make the whole window inert.
    wm.processPointer(400.0f, 300.0f, 20.0f, drawnHeight * 0.5f, true, true);
    CHECK(wm.openMenu() == -1);
    CHECK_FALSE(wm.hasLayer());
    wm.processPointer(400.0f, 300.0f, 20.0f, drawnHeight * 0.5f, false, true);
    std::vector<UIRenderObject> clicked;
    wm.extract(400.0f, 300.0f, clicked);
    CHECK(countGlyphs(clicked) == 2);   // "OK" — the button under the old band

    // The menus themselves are untouched — they are the data both bars read, and
    // switching who draws them must not lose them.
    CHECK(wm.menuBar().size() == 2u);
}

TEST_CASE("A tray entry arrives as its ID, at the application")
{
    // The tray itself needs a desktop, so what is checked here is the part that
    // does not: the event exists, it carries a String, and firing it reaches a
    // graph. The id and not the label, because a translated menu must keep
    // working — that is the whole reason an entry has two of them.
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree tree;
    tree.canvasWidth = 200.0f; tree.canvasHeight = 100.0f;
    const int caption = tree.add(HE::UIWidgetType::Text);
    tree.find(caption)->name = "Caption";
    tree.find(caption)->setProp("Text", HE::UIPropValue::ofString(""));

    HorizonCode::Graph graph;
    HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "OnTrayItem";
    ev.elem = 0; ev.hasArg = true; ev.propType = PinType::String;
    const int evId = graph.addNode(ev);
    HorizonCode::Node set; set.type = NodeType::SetProperty; set.elem = caption;
    set.s = "Text"; set.propType = PinType::String;
    const int setId = graph.addNode(set);
    REQUIRE(graph.connect(evId, 0, setId, 0));   // exec
    REQUIRE(graph.connect(evId, 1, setId, 2));   // the id → the caption

    registerWidget(cm, tree, &graph);
    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);

    rt.fireOnTrayItem(static_cast<HorizonCode::InstanceId>(id), 0, "quit");
    REQUIRE(wm.tree(id) != nullptr);
    CHECK(wm.tree(id)->find(caption)->getProp("Text").s == "quit");

    // The event is one the graph editor can offer, which is the difference
    // between a runtime path and a feature: it has to be in the table the
    // authoring side reads.
    const HorizonCode::EngineEventDesc* desc = HorizonCode::findEngineEvent("OnTrayItem");
    REQUIRE(desc != nullptr);
    CHECK(desc->arg == PinType::String);
}

TEST_CASE("Drop: what no element accepts, the window took")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    DropFixture f;
    registerWidget(cm, f.tree, &f.graph);

    HorizonCode::Runtime rt;
    // The application's own script: it hears a drop that landed on no drop zone,
    // with elem 0, because no element took it. An app that opens whatever it is
    // handed wants exactly one place to say so.
    HorizonCode::Graph app;
    {
        HorizonCode::Variable taken;
        taken.name = "taken"; taken.type = PinType::String;
        app.variables.push_back(taken);
        HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "OnFileDropped";
        ev.elem = 0; ev.hasArg = true; ev.propType = PinType::String;
        const int evId = app.addNode(ev);
        HorizonCode::Node set; set.type = NodeType::SetVariable; set.s = "taken";
        set.propType = PinType::String;
        const int setId = app.addNode(set);
        REQUIRE(app.connect(evId, 0, setId, 0));
        REQUIRE(app.connect(evId, 1, setId, 2));
    }
    const HorizonCode::InstanceId gi = rt.setGameInstance(app);
    REQUIRE(gi != 0);

    WidgetManager wm;
    wm.setRuntime(&rt);
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);

    // Over the inert half: no element accepts, so the window did.
    CHECK_FALSE(wm.processDrop(400.0f, 300.0f, 300.0f, 150.0f, { "/tmp/loose.txt" }));
    CHECK(rt.getVariable(gi, "taken").s == "/tmp/loose.txt");

    // …and a drop a zone DID take stops there. Both hearing it would mean every
    // application opening the file twice, once per listener.
    CHECK(wm.processDrop(400.0f, 300.0f, 100.0f, 150.0f, { "/tmp/mine.txt" }));
    CHECK(rt.getVariable(gi, "taken").s == "/tmp/loose.txt");
}

TEST_CASE("Drop: the highlight marks the element that would take it")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    DropFixture f;
    registerWidget(cm, f.tree, &f.graph);

    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);

    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 300.0f, out);
    REQUIRE(countDropRings(out) == 0);       // nothing is being dragged yet

    // Over the caption: the mark goes on the ZONE, the same element the drop
    // would reach. A highlight that promises one thing while the drop lands on
    // another is worse than no highlight.
    CHECK(wm.dropHover(400.0f, 300.0f, 100.0f, 150.0f, true));
    out.clear(); wm.extract(400.0f, 300.0f, out);
    CHECK(countDropRings(out) == 1);
    const HE::UIWidgetRect zr =
        HE::uiElementRect(*wm.tree(id), *wm.tree(id)->find(f.zone));
    bool onTheZone = false;
    for (const UIRenderObject& o : out)
        if (o.borderWidth > 0.0f && o.color.a <= 0.001f && o.borderColor.b > 0.9f)
            onTheZone = std::abs(o.position.x - zr.x) < 0.5f &&
                        std::abs(o.size.x - zr.w) < 0.5f;
    CHECK(onTheZone);

    // Dragged over the half that accepts nothing: the mark goes away rather
    // than staying on the last thing it touched.
    CHECK_FALSE(wm.dropHover(400.0f, 300.0f, 300.0f, 150.0f, true));
    out.clear(); wm.extract(400.0f, 300.0f, out);
    CHECK(countDropRings(out) == 0);

    // …and the drag leaving the window ends it too.
    CHECK(wm.dropHover(400.0f, 300.0f, 100.0f, 150.0f, true));
    CHECK_FALSE(wm.dropHover(400.0f, 300.0f, 100.0f, 150.0f, false));
    out.clear(); wm.extract(400.0f, 300.0f, out);
    CHECK(countDropRings(out) == 0);

    // The drop itself clears the mark: the gesture is over, whatever it hit.
    CHECK(wm.dropHover(400.0f, 300.0f, 100.0f, 150.0f, true));
    CHECK(wm.processDrop(400.0f, 300.0f, 100.0f, 150.0f, { "/tmp/a.txt" }));
    out.clear(); wm.extract(400.0f, 300.0f, out);
    CHECK(countDropRings(out) == 0);
}

// ═══ B7: dragging inside the application ═════════════════════════════════════

namespace
{
// A card that can be picked up on the left, a bin that takes things on the
// right, and a graph that writes down everything that happened to either. The
// card carries a caption, because the press has to be able to land on a child
// and still lift the card.
struct DragFixture
{
    HE::UIWidgetTree   tree;
    HorizonCode::Graph graph;
    int card = 0, caption = 0, bin = 0;

    explicit DragFixture(const char* payload = "")
    {
        tree.canvasWidth = 400.0f; tree.canvasHeight = 300.0f;
        tree.scaleMode = HE::UICanvasScaleMode::ConstantPixel;

        card = tree.add(HE::UIWidgetType::Panel);
        {
            HE::UIElement& e = *tree.find(card);
            e.name = "Card"; e.draggable = true; e.dragPayload = payload;
            HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
            e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 180.0f; e.sizeY = 300.0f;
        }
        caption = tree.add(HE::UIWidgetType::Text);
        {
            HE::UIElement& e = *tree.find(caption);
            e.parentId = card; e.name = "Caption";
            e.setProp("AutoSize", HE::UIPropValue::ofBool(false));
            HE::uiSetAnchorPreset(e, HE::kUIAnchorFill);
            HE::uiSetAnchorInsetsX(e, 20.0f, 20.0f);
            HE::uiSetAnchorInsetsY(e, 20.0f, 20.0f);
        }
        bin = tree.add(HE::UIWidgetType::Panel);
        {
            HE::UIElement& e = *tree.find(bin);
            e.name = "Bin"; e.acceptsDrop = true;
            HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
            e.posX = 220.0f; e.posY = 0.0f; e.sizeX = 180.0f; e.sizeY = 300.0f;
        }

        auto counter = [&](const char* name, const char* event, int elem)
        {
            HorizonCode::Variable v; v.name = name; v.type = PinType::Int;
            graph.variables.push_back(v);
            HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = event; ev.elem = elem;
            const int evId = graph.addNode(ev);
            HorizonCode::Node get; get.type = NodeType::GetVariable; get.s = name;
            get.propType = PinType::Int;
            const int getId = graph.addNode(get);
            HorizonCode::Node one; one.type = NodeType::ConstInt; one.f[0] = 1.0f;
            const int oneId = graph.addNode(one);
            HorizonCode::Node add; add.type = NodeType::Add;
            const int addId = graph.addNode(add);
            HorizonCode::Node set; set.type = NodeType::SetVariable; set.s = name;
            set.propType = PinType::Int;
            const int setId = graph.addNode(set);
            REQUIRE(graph.connect(getId, 0, addId, 0));
            REQUIRE(graph.connect(oneId, 0, addId, 1));
            REQUIRE(graph.connect(evId, 0, setId, 0));
            REQUIRE(graph.connect(addId, 2, setId, 2));
            return evId;
        };
        counter("clicks",  "OnClicked",      card);
        counter("started", "OnDragStarted",  card);

        // What the bin was handed…
        {
            HorizonCode::Variable v; v.name = "took"; v.type = PinType::String;
            graph.variables.push_back(v);
            HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "OnDrop"; ev.elem = bin;
            ev.hasArg = true; ev.propType = PinType::String;
            const int evId = graph.addNode(ev);
            HorizonCode::Node set; set.type = NodeType::SetVariable; set.s = "took";
            set.propType = PinType::String;
            const int setId = graph.addNode(set);
            REQUIRE(graph.connect(evId, 0, setId, 0));
            REQUIRE(graph.connect(evId, 1, setId, 2));
        }
        // …and how it went for the card. Both a count and the verdict: "it
        // ended" and "it was taken" are two different questions, and a cancel
        // has to answer the first one too.
        {
            HorizonCode::Variable v; v.name = "accepted"; v.type = PinType::Bool;
            graph.variables.push_back(v);
            HorizonCode::Variable n; n.name = "ends"; n.type = PinType::Int;
            graph.variables.push_back(n);
            HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "OnDragEnded";
            ev.elem = card; ev.hasArg = true; ev.propType = PinType::Bool;
            const int evId = graph.addNode(ev);
            HorizonCode::Node set; set.type = NodeType::SetVariable; set.s = "accepted";
            set.propType = PinType::Bool;
            const int setId = graph.addNode(set);
            REQUIRE(graph.connect(evId, 0, setId, 0));
            REQUIRE(graph.connect(evId, 1, setId, 2));
            HorizonCode::Node get; get.type = NodeType::GetVariable; get.s = "ends";
            get.propType = PinType::Int;
            const int getId = graph.addNode(get);
            HorizonCode::Node one; one.type = NodeType::ConstInt; one.f[0] = 1.0f;
            const int oneId = graph.addNode(one);
            HorizonCode::Node add; add.type = NodeType::Add;
            const int addId = graph.addNode(add);
            HorizonCode::Node inc; inc.type = NodeType::SetVariable; inc.s = "ends";
            inc.propType = PinType::Int;
            const int incId = graph.addNode(inc);
            REQUIRE(graph.connect(getId, 0, addId, 0));
            REQUIRE(graph.connect(oneId, 0, addId, 1));
            REQUIRE(graph.connect(setId, 1, incId, 0));
            REQUIRE(graph.connect(addId, 2, incId, 2));
        }
    }
};
}

TEST_CASE("Drag: a press is still a click, and a press that travels is a drag")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    DragFixture f;
    registerWidget(cm, f.tree, &f.graph);

    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    const auto inst = static_cast<HorizonCode::InstanceId>(id);
    auto var = [&](const char* n) { return rt.getVariable(inst, n).i; };

    // Press and let go where you pressed: a click, and nothing was carried.
    wm.processPointer(400.0f, 300.0f, 90.0f, 150.0f, true,  true);
    CHECK_FALSE(wm.isDragging());
    wm.processPointer(400.0f, 300.0f, 90.0f, 150.0f, false, true);
    CHECK(var("clicks") == 1);
    CHECK(var("started") == 0);

    // A press that wobbles by less than the threshold is still a click. This is
    // the assertion the threshold exists for: a hand is never perfectly still.
    wm.processPointer(400.0f, 300.0f, 90.0f, 150.0f, true, true);
    wm.processPointer(400.0f, 300.0f, 92.0f, 151.0f, true, true);
    CHECK_FALSE(wm.isDragging());
    wm.processPointer(400.0f, 300.0f, 92.0f, 151.0f, false, true);
    CHECK(var("clicks") == 2);
    CHECK(var("started") == 0);

    // Travel far enough and it lifts — from a press on the CAPTION, because a
    // card is picked up by any part of it.
    wm.processPointer(400.0f, 300.0f, 90.0f, 150.0f, true, true);
    wm.processPointer(400.0f, 300.0f, 140.0f, 150.0f, true, true);
    CHECK(wm.isDragging());
    CHECK(wm.dragSourceElement() == f.card);
    CHECK(var("started") == 1);

    // Let go over nothing: it ended, nothing took it, and it was NOT a click.
    wm.processPointer(400.0f, 300.0f, 200.0f, 150.0f, false, true);
    CHECK_FALSE(wm.isDragging());
    CHECK(var("ends") == 1);
    CHECK(rt.getVariable(inst, "accepted").b == false);
    CHECK(rt.getVariable(inst, "took").s.empty());
    CHECK(var("clicks") == 2);          // the drag ate it
}

TEST_CASE("Drag: what lands on a zone arrives there, and says what it is")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    DragFixture f;                       // no payload → the name answers
    registerWidget(cm, f.tree, &f.graph);

    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    const auto inst = static_cast<HorizonCode::InstanceId>(id);

    wm.processPointer(400.0f, 300.0f, 90.0f, 150.0f, true, true);
    wm.processPointer(400.0f, 300.0f, 300.0f, 150.0f, true, true);
    REQUIRE(wm.isDragging());

    // Over the bin, the mark is on the bin: the same ring the file drop uses,
    // answering the same question.
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 300.0f, out);
    CHECK(countDropRings(out) == 1);
    // …and the card is half there, because it is under the hand and not in its
    // place any more.
    const HE::UIWidgetRect cr =
        HE::uiElementRect(*wm.tree(id), *wm.tree(id)->find(f.card));
    bool cardFaded = false;
    for (const UIRenderObject& o : out)
        if (std::abs(o.position.x - cr.x) < 0.5f && std::abs(o.size.x - cr.w) < 0.5f &&
            o.color.a > 0.001f && o.color.a < 0.9f)
            cardFaded = true;
    CHECK(cardFaded);

    wm.processPointer(400.0f, 300.0f, 300.0f, 150.0f, false, true);
    CHECK(rt.getVariable(inst, "took").s == "Card");     // the name, unset payload
    CHECK(rt.getVariable(inst, "accepted").b == true);
    CHECK(rt.getVariable(inst, "ends").i == 1);
    // The mark goes with the gesture.
    out.clear(); wm.extract(400.0f, 300.0f, out);
    CHECK(countDropRings(out) == 0);
}

TEST_CASE("Drag: the payload is what the source says it is")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    DragFixture f("row:7");
    registerWidget(cm, f.tree, &f.graph);

    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);

    wm.processPointer(400.0f, 300.0f, 90.0f, 150.0f, true, true);
    wm.processPointer(400.0f, 300.0f, 300.0f, 150.0f, true, true);
    wm.processPointer(400.0f, 300.0f, 300.0f, 150.0f, false, true);
    CHECK(rt.getVariable(static_cast<HorizonCode::InstanceId>(id), "took").s == "row:7");
}

TEST_CASE("Drag: it cannot be dropped on itself, and Escape puts it back")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    DragFixture f;
    // The card takes drops as well as making them — a reorderable list row is
    // exactly this, and the row under the hand is the one the pointer is on.
    f.tree.find(f.card)->acceptsDrop = true;
    registerWidget(cm, f.tree, &f.graph);

    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    const auto inst = static_cast<HorizonCode::InstanceId>(id);

    // Lift it and stay over itself: no mark, because there is nowhere to land.
    wm.processPointer(400.0f, 300.0f, 40.0f, 150.0f, true, true);
    wm.processPointer(400.0f, 300.0f, 100.0f, 150.0f, true, true);
    REQUIRE(wm.isDragging());
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 300.0f, out);
    CHECK(countDropRings(out) == 0);

    // Letting go there is a cancel, not a drop onto itself.
    wm.processPointer(400.0f, 300.0f, 100.0f, 150.0f, false, true);
    CHECK(rt.getVariable(inst, "took").s.empty());
    CHECK(rt.getVariable(inst, "accepted").b == false);
    CHECK(rt.getVariable(inst, "ends").i == 1);

    // Escape while carrying puts it back, and it is the topmost thing there is:
    // above any dialog, because it was begun last.
    wm.processPointer(400.0f, 300.0f, 40.0f, 150.0f, true, true);
    wm.processPointer(400.0f, 300.0f, 300.0f, 150.0f, true, true);
    REQUIRE(wm.isDragging());
    CHECK(wm.closeTopLayer());
    CHECK_FALSE(wm.isDragging());
    CHECK(rt.getVariable(inst, "ends").i == 2);
    CHECK(rt.getVariable(inst, "accepted").b == false);

    // …and the release that follows the cancel is not a click either. The press
    // became a drag; that it was given up does not turn it back into one.
    wm.processPointer(400.0f, 300.0f, 300.0f, 150.0f, false, true);
    CHECK(rt.getVariable(inst, "clicks").i == 0);
}

// The case B2 and B7 meet in, and the one that pays for making rows draggable.
TEST_CASE("Drag: a draggable row is still a row you can pick")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    {
        // The same list page as everywhere else, except that its rows can be
        // picked up — which is what makes a list reorderable, and which must not
        // cost the list the ability to be selected in.
        HE::UIWidgetTree row;
        row.canvasWidth = 400.0f; row.canvasHeight = 40.0f;
        row.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
        const int label = row.add(HE::UIWidgetType::Text);
        HE::UIElement& lab = *row.find(label);
        lab.name = "Label";
        lab.draggable = true;
        // Filling the row, and NOT auto-sized: an auto-sizing label with no text
        // shrinks to nothing, and a press then sails past it into the list —
        // which would make this test pass by never touching the thing it is
        // about.
        lab.setProp("AutoSize", HE::UIPropValue::ofBool(false));
        HE::uiSetAnchorPreset(lab, HE::kUIAnchorFill);
        HE::uiSetAnchorInsetsX(lab, 0.0f, 0.0f);
        HE::uiSetAnchorInsetsY(lab, 0.0f, 0.0f);
        registerWidget(cm, row, nullptr, "mem://row.hasset");

        HE::UIWidgetTree page;
        page.canvasWidth = 400.0f; page.canvasHeight = 400.0f;
        page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
        const int list = page.add(HE::UIWidgetType::ListView);
        auto* lv = dynamic_cast<HE::UIListView*>(page.find(list));
        lv->name = "List";
        lv->acceptsDrop = true;        // …and rows land back in the list
        HE::uiSetAnchorPreset(*lv, 0); lv->pivotX = lv->pivotY = 0.0f;
        lv->posX = 0.0f; lv->posY = 0.0f; lv->sizeX = 400.0f; lv->sizeY = 400.0f;
        lv->rowWidget = "mem://row.hasset";
        lv->rowHeight = 40.0f; lv->spacing = 0.0f; lv->padding = 0.0f;
        registerWidget(cm, page, nullptr, "mem://page.hasset");
    }

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://page.hasset");
    REQUIRE(id != 0);
    REQUIRE(wm.setListCount(id, "List", 100));
    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 400.0f, out);
    REQUIRE(wm.listSelected(id, "List") == -1);

    // Rows are 40 tall from the top: y=100 is inside row 2. A press that does
    // not travel is still a press on the ROW, and the list still hears it.
    wm.processPointer(400.0f, 400.0f, 200.0f, 100.0f, true, true);
    CHECK(wm.listSelected(id, "List") == 2);
    wm.processPointer(400.0f, 400.0f, 200.0f, 100.0f, false, true);
    CHECK(wm.listSelected(id, "List") == 2);

    // …and a press that DOES travel carries the row instead, without changing
    // the selection on the way: picking something up is not choosing it.
    wm.processPointer(400.0f, 400.0f, 200.0f, 220.0f, true, true);
    CHECK(wm.listSelected(id, "List") == 5);
    wm.processPointer(400.0f, 400.0f, 200.0f, 260.0f, true, true);
    CHECK(wm.isDragging());
    wm.processPointer(400.0f, 400.0f, 200.0f, 100.0f, true, true);
    CHECK(wm.listSelected(id, "List") == 5);
    wm.processPointer(400.0f, 400.0f, 200.0f, 100.0f, false, true);
    CHECK_FALSE(wm.isDragging());
}

TEST_CASE("Transition: a hover travels to its colour instead of jumping there")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int btn = t.add(HE::UIWidgetType::Button);
    {
        HE::UIElement& e = *t.find(btn);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 60.0f;
        // Black at rest, white hovered: every channel travels the whole way, so
        // "somewhere in between" is one number to read and not a judgement.
        e.setProp("Normal Color",  HE::UIPropValue::ofColor({ 0.0f, 0.0f, 0.0f, 1.0f }));
        e.setProp("Hovered Color", HE::UIPropValue::ofColor({ 1.0f, 1.0f, 1.0f, 1.0f }));
        e.transition = 0.2f;
    }
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);

    // The button's own quad: the only 200×60 solid in the frame.
    auto face = [&]
    {
        std::vector<UIRenderObject> out;
        wm.extract(400.0f, 400.0f, out);
        for (const UIRenderObject& o : out)
            if (o.type == 0 && o.size.x == 200.0f && o.size.y == 60.0f) return o.color.r;
        return -1.0f;
    };

    CHECK(face() == 0.0f);                       // untouched

    // Hovered, but not yet arrived. The pointer alone changes nothing: the
    // journey is made of ticks, and the frame it starts in is still the resting
    // colour.
    wm.processPointer(400.0f, 400.0f, 100.0f, 30.0f, false, true);
    CHECK(face() == 0.0f);

    wm.consumeVisualDirty();
    wm.tick(0.1f);
    // Halfway in time, and eased, so the number is not 0.5 — what matters is
    // that it is neither end. A blend that reached white in one tick would pass
    // "is it hovered" and still be the jump this is meant to remove.
    const float mid = face();
    CHECK(mid > 0.0f);
    CHECK(mid < 1.0f);
    // …and the frame it moved in is a frame the application knows to draw.
    CHECK(wm.consumeVisualDirty());
    // Nothing else is moving, and the blend still is: an event-driven app must
    // stay awake for it.
    CHECK(wm.isAnimating());

    wm.tick(0.2f);
    CHECK(face() == 1.0f);                       // arrived
    // Arrived and standing still is not animating any more — the entry stays
    // (it is what the way back out starts from) but nothing asks for frames.
    wm.consumeVisualDirty();
    wm.tick(0.2f);
    CHECK_FALSE(wm.consumeVisualDirty());

    // Away again, and the way back is a journey too.
    wm.processPointer(400.0f, 400.0f, 350.0f, 350.0f, false, true);
    wm.tick(0.1f);
    const float back = face();
    CHECK(back > 0.0f);
    CHECK(back < 1.0f);
    wm.tick(0.2f);
    CHECK(face() == 0.0f);
    // Back at rest the entry is swept, or isAnimating() would be true forever
    // after the first hover and the application would never sleep again.
    CHECK_FALSE(wm.isAnimating());

    // Transition 0 is the old behaviour, exactly: the hover lands on the
    // hovered colour in the frame it happens, with no tick in between.
    REQUIRE(wm.animate(id, btn, "Transition", HE::UIPropValue::ofFloat(0.0f), 0.0f));
    wm.processPointer(400.0f, 400.0f, 100.0f, 30.0f, false, true);
    CHECK(face() == 1.0f);
    CHECK_FALSE(wm.isAnimating());
}

// ═══ B10: contrast ═══════════════════════════════════════════════════════════
// The numbers here are WCAG 2.1's, and they are checked against the values the
// standard itself publishes rather than against what the code happens to
// produce — a luminance formula that is self-consistent and wrong is exactly
// the bug a round-trip test cannot see.
TEST_CASE("Contrast: the ratio is the standard's, not a resemblance of it")
{
    const glm::vec4 black{ 0.0f, 0.0f, 0.0f, 1.0f };
    const glm::vec4 white{ 1.0f, 1.0f, 1.0f, 1.0f };

    CHECK(HE::uiRelativeLuminance(black) == doctest::Approx(0.0f));
    CHECK(HE::uiRelativeLuminance(white) == doctest::Approx(1.0f));
    // The extremes of the scale, and its floor: a colour against itself is 1.
    CHECK(HE::uiContrastRatio(black, white) == doctest::Approx(21.0f));
    CHECK(HE::uiContrastRatio(white, black) == doctest::Approx(21.0f));   // symmetric
    CHECK(HE::uiContrastRatio(white, white) == doctest::Approx(1.0f));

    // Mid grey (#808080): luminance 0.2159, so 3.95 against white and 5.32
    // against black. Worth pinning because it is the value people expect to be
    // 0.5 and is not — the transfer function is the whole difference between
    // this check and a wrong one that averages the channels.
    const glm::vec4 grey{ 128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f, 1.0f };
    CHECK(HE::uiRelativeLuminance(grey) == doctest::Approx(0.2159f).epsilon(0.01));
    CHECK(HE::uiContrastRatio(grey, white) == doctest::Approx(3.95f).epsilon(0.01));
    CHECK(HE::uiContrastRatio(grey, black) == doctest::Approx(5.32f).epsilon(0.01));

    // Green weighs more than blue by a factor of ten in the eye, and that is
    // the whole reason luminance is not the average of the channels.
    CHECK(HE::uiRelativeLuminance({ 0.0f, 1.0f, 0.0f, 1.0f })
          > HE::uiRelativeLuminance({ 0.0f, 0.0f, 1.0f, 1.0f }) * 9.0f);
}

TEST_CASE("Contrast: a label is measured against what it actually stands on")
{
    HE::UIWidgetTree t;
    const int panel = t.add(HE::UIWidgetType::Panel);
    static_cast<HE::UIPanel*>(t.find(panel))->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    const int label = t.add(HE::UIWidgetType::Text);
    t.find(label)->parentId = panel;
    HE::UIText* lab = static_cast<HE::UIText*>(t.find(label));
    lab->fontSize = 16.0f;

    // White on white: the worst there is, and it is the label that is named,
    // not the panel.
    lab->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    auto bad = HE::uiCheckContrast(t, { 0.0f, 0.0f, 0.0f, 1.0f });
    REQUIRE(bad.size() == 1);
    CHECK(bad[0].elementId == label);
    CHECK(bad[0].textProp == "Color");
    CHECK(bad[0].againstId == panel);
    CHECK(bad[0].againstProp == "Color");
    CHECK(bad[0].ratio == doctest::Approx(1.0f));
    CHECK(bad[0].required == doctest::Approx(4.5f));

    // Black on the same white panel is fine — and note the BACKDROP is black,
    // so a check that ignored the panel would have called this one legible too.
    // The panel is what decides, which is the claim.
    lab->color = { 0.0f, 0.0f, 0.0f, 1.0f };
    CHECK(HE::uiCheckContrast(t, { 0.0f, 0.0f, 0.0f, 1.0f }).empty());

    // A container between them changes nothing: a vertical box paints no pixel,
    // so the question passes through it to the panel.
    const int box = t.add(HE::UIWidgetType::VerticalBox);
    t.find(box)->parentId = panel;
    t.find(label)->parentId = box;
    lab->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    REQUIRE(HE::uiCheckContrast(t, { 0.0f, 0.0f, 0.0f, 1.0f }).size() == 1);

    // Large text is held to 3.0 instead of 4.5 — WCAG's own exception, and the
    // reason the finding carries the font size that decided it.
    lab->color = { 0.537f, 0.537f, 0.537f, 1.0f };   // 4.0 against white
    lab->fontSize = 16.0f;
    REQUIRE(HE::uiCheckContrast(t, { 0.0f, 0.0f, 0.0f, 1.0f }).size() == 1);
    lab->fontSize = 24.0f;
    CHECK(HE::uiCheckContrast(t, { 0.0f, 0.0f, 0.0f, 1.0f }).empty());
}

TEST_CASE("Contrast: transparency is composited, not ignored")
{
    HE::UIWidgetTree t;
    const int panel = t.add(HE::UIWidgetType::Panel);
    static_cast<HE::UIPanel*>(t.find(panel))->color = { 0.0f, 0.0f, 0.0f, 1.0f };
    const int label = t.add(HE::UIWidgetType::Text);
    t.find(label)->parentId = panel;
    HE::UIText* lab = static_cast<HE::UIText*>(t.find(label));
    lab->fontSize = 16.0f;

    // The usual way a label is made "muted": white, turned down. At full alpha
    // it is 21:1 on black; at 0.2 the eye sees a very dark grey and the ratio
    // collapses. A check that read the authored colour would pass both.
    lab->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    CHECK(HE::uiCheckContrast(t, { 0.0f, 0.0f, 0.0f, 1.0f }).empty());
    lab->color.a = 0.2f;
    auto bad = HE::uiCheckContrast(t, { 0.0f, 0.0f, 0.0f, 1.0f });
    REQUIRE(bad.size() == 1);
    CHECK(bad[0].ratio < 4.5f);
    CHECK(bad[0].textColor.a == doctest::Approx(1.0f));   // reported as composited

    // …and a see-through panel does not hide what is behind it either: the same
    // white-on-white as before, arrived at through a panel that is half there
    // over a white page.
    static_cast<HE::UIPanel*>(t.find(panel))->color = { 1.0f, 1.0f, 1.0f, 0.5f };
    lab->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    bad = HE::uiCheckContrast(t, { 1.0f, 1.0f, 1.0f, 1.0f });
    REQUIRE(bad.size() == 1);
    CHECK(bad[0].backColor.r == doctest::Approx(1.0f));
    CHECK(bad[0].ratio == doctest::Approx(1.0f));
}

TEST_CASE("Contrast: a field's text stands on the field, a button's on the button")
{
    HE::UIWidgetTree t;
    // A dark page. The field is light and its text is dark: legible on the
    // field, and it would look unreadable to anything that measured it against
    // the page instead.
    const int field = t.add(HE::UIWidgetType::TextInput);
    HE::UITextInput* fi = static_cast<HE::UITextInput*>(t.find(field));
    fi->backColor = { 1.0f, 1.0f, 1.0f, 1.0f };
    fi->textColor = { 0.0f, 0.0f, 0.0f, 1.0f };
    CHECK(HE::uiCheckContrast(t, { 0.0f, 0.0f, 0.0f, 1.0f }).empty());
    fi->textColor = { 1.0f, 1.0f, 1.0f, 1.0f };
    auto bad = HE::uiCheckContrast(t, { 0.0f, 0.0f, 0.0f, 1.0f });
    REQUIRE(bad.size() == 1);
    CHECK(bad[0].againstId == field);
    CHECK(bad[0].againstProp == "Back Color");

    // A button's caption is a Text CHILD (there is no built-in label), so the
    // surface it stands on is the button's normal colour — found by the walk
    // upwards, not by a rule about buttons.
    HE::UIWidgetTree b;
    const int btn = b.add(HE::UIWidgetType::Button);
    static_cast<HE::UIButton*>(b.find(btn))->color = { 0.95f, 0.95f, 0.95f, 1.0f };
    const int cap = b.add(HE::UIWidgetType::Text);
    b.find(cap)->parentId = btn;
    static_cast<HE::UIText*>(b.find(cap))->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    static_cast<HE::UIText*>(b.find(cap))->fontSize = 16.0f;
    auto bb = HE::uiCheckContrast(b, { 0.0f, 0.0f, 0.0f, 1.0f });
    REQUIRE(bb.size() == 1);
    CHECK(bb[0].elementId == cap);
    CHECK(bb[0].againstId == btn);
    CHECK(bb[0].againstProp == "Normal Color");
}

// The check exists to be RUN over what ships, not only over trees a test built.
// This is the run: every component in the engine library, dressed by the
// default theme, in both modes.
#ifdef HE_EDITOR_DEPS_DIR
TEST_CASE("Contrast: every shipped component is legible in both modes")
{
    const std::filesystem::path dir =
        std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent" / "Widgets";
    REQUIRE(std::filesystem::is_directory(dir));
    ContentManager cm(dir.string());

    for (const HE::UIThemeMode mode : { HE::UIThemeMode::Light, HE::UIThemeMode::Dark })
    {
        CAPTURE(HE::uiThemeModeName(mode));
        // The page a component sits on is the theme's Background — the same
        // colour an application paints behind everything, which is what makes
        // this the real question and not a hypothetical one.
        const glm::vec4 page =
            HE::uiDefaultTheme().colorFor(HE::UIThemeRole::Background, mode);

        for (const auto& entry : std::filesystem::directory_iterator(dir))
        {
            if (entry.path().extension() != ".hasset") continue;
            const std::string file = entry.path().filename().string();
            CAPTURE(file);
            const HE::UUID id = cm.loadAsset(file);
            REQUIRE(id != HE::UUID{});
            const UIWidgetAsset* asset = cm.getWidget(id);
            REQUIRE(asset);

            HE::UIWidgetTree t;
            REQUIRE(HE::uiWidgetTreeFromJson(asset->treeJson, t));
            HE::uiApplyTheme(t, HE::uiDefaultTheme(), mode);

            for (const HE::UIContrastFinding& f : HE::uiCheckContrast(t, page))
            {
                const HE::UIElement* e = t.find(f.elementId);
                // No exemptions. There used to be one — text on an Accent
                // surface, four findings across the OK caption and the badge's
                // number — and it was not a rule about contrast but a gap in the
                // vocabulary: `Text` is the reading colour of the PAGE, and
                // there was no role meaning "the colour to write ON the accent".
                // UIThemeRole::AccentText is that role, the two components are
                // bound to it, and the check is strict again.
                CAPTURE(e ? e->name : std::string("?"));
                CAPTURE(f.textProp);
                CAPTURE(f.ratio);
                CAPTURE(f.required);
                CHECK_MESSAGE(false, "text is not legible on what it stands on");
            }
        }
    }
}
#endif

// ═══ B10: the text catalog ═══════════════════════════════════════════════════
// Resolved by ASSIGNMENT, exactly the way a theme is, so everything downstream
// keeps reading plain fields and switching language is one pass.
TEST_CASE("Text catalog: a bound property takes the language's text")
{
    HE::UITextCatalog c;
    c.fallback = "en";
    c.set("en", "greet", "Hello");
    c.set("en", "bye",   "Goodbye");
    c.set("de", "greet", "Hallo");   // half-translated on purpose

    HE::UIWidgetTree t;
    const int a = t.add(HE::UIWidgetType::Text);
    const int b = t.add(HE::UIWidgetType::Text);
    t.find(a)->setTextKey("Text", "greet");
    t.find(b)->setTextKey("Text", "bye");
    t.find(b)->setTextKey("Tooltip", "greet");   // any String property, not just Text
    static_cast<HE::UIText*>(t.find(a))->text = "AUTHORED";
    static_cast<HE::UIText*>(t.find(b))->text = "AUTHORED";

    CHECK(HE::uiApplyTextCatalog(t, c, "en") == 3);
    CHECK(t.find(a)->getPropAny("Text").s == "Hello");
    CHECK(t.find(b)->getPropAny("Text").s == "Goodbye");
    CHECK(t.find(b)->tooltip == "Hello");
    // Nothing to write the second time round — the count is what tells a caller
    // whether anything has to be redrawn, so it must not lie.
    CHECK(HE::uiApplyTextCatalog(t, c, "en") == 0);

    // German has "greet" and not "bye": the first switches, the second falls
    // back to English rather than going blank or showing the key.
    CHECK(HE::uiApplyTextCatalog(t, c, "de") == 2);
    CHECK(t.find(a)->getPropAny("Text").s == "Hallo");
    CHECK(t.find(b)->getPropAny("Text").s == "Goodbye");

    // A key the catalog does not know AT ALL leaves the property alone: "not
    // translated yet" and "translated to nothing" are different answers.
    t.find(a)->setTextKey("Text", "missing");
    CHECK(HE::uiApplyTextCatalog(t, c, "de") == 0);
    CHECK(t.find(a)->getPropAny("Text").s == "Hallo");
    // …and an empty translation IS a translation.
    c.set("de", "missing", "");
    CHECK(HE::uiApplyTextCatalog(t, c, "de") == 1);
    CHECK(t.find(a)->getPropAny("Text").s.empty());

    // A key on a property that is not a string writes nothing rather than
    // coercing — a black label nobody can explain is worse than an untranslated
    // one.
    const int col = t.add(HE::UIWidgetType::Panel);
    t.find(col)->setTextKey("Color", "greet");
    CHECK(HE::uiApplyTextCatalog(t, c, "de") == 0);
}

TEST_CASE("Text catalog: the binding survives the file, and costs nothing unbound")
{
    HE::UIWidgetTree t;
    const int e = t.add(HE::UIWidgetType::Text);
    CHECK(HE::uiWidgetTreeToJson(t).find("textKeys") == std::string::npos);

    t.find(e)->setTextKey("Text", "greet");
    HE::UIWidgetTree back;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), back));
    CHECK(back.find(e)->textKeyFor("Text") == "greet");
    CHECK(back.find(e)->clone()->textKeyFor("Text") == "greet");
    // Unbinding is writing an empty key, the same way a theme role is unbound.
    back.find(e)->setTextKey("Text", "");
    CHECK(back.find(e)->textKeys.empty());

    // The catalog's own file keeps the author's order — it is read by people who
    // translate it, and a map would reshuffle every language on every save.
    HE::UITextCatalog c;
    c.set("en", "zebra", "Zebra");
    c.set("en", "apple", "Apple");
    HE::UITextCatalog round;
    REQUIRE(HE::uiTextCatalogFromJson(HE::uiTextCatalogToJson(c), round));
    REQUIRE(round.languages.size() == 1);
    REQUIRE(round.languages[0].second.size() == 2);
    CHECK(round.languages[0].second[0].first == "zebra");
    CHECK(*round.find("en", "apple") == "Apple");
    CHECK(round.find("en", "nothing") == nullptr);
}

TEST_CASE("Text catalog: switching language re-resolves every live widget")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 400.0f; t.canvasHeight = 400.0f;
    const int label = t.add(HE::UIWidgetType::Text);
    t.find(label)->setTextKey("Text", "greet");
    static_cast<HE::UIText*>(t.find(label))->text = "AUTHORED";
    registerWidget(cm, t);

    HE::UITextCatalog c;
    c.set("en", "greet", "Hello");
    c.set("de", "greet", "Hallo");

    WidgetManager wm;
    wm.setTextCatalog(c);
    // No language asked for yet, so the catalog's fallback answers: an
    // application that never sets one still shows text and not keys.
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    CHECK(wm.tree(id)->find(label)->getPropAny("Text").s == "Hello");

    wm.consumeVisualDirty();
    wm.setLanguage("de");
    CHECK(wm.tree(id)->find(label)->getPropAny("Text").s == "Hallo");
    CHECK(wm.consumeVisualDirty());        // something changed, so redraw
    // …and asking for the same language again is not a reason to wake up.
    wm.setLanguage("de");
    CHECK_FALSE(wm.consumeVisualDirty());
}

// D4, first of four: a gap is not a rounding.
//
// The theme has carried two number scales since it was written, `radius` and
// `spacing`, both editable in the theme editor. Only one of them was ever read:
// every Float binding that was not a font size resolved against `radius`, so
// the spacing steps were a field an author could set and never see. Since the
// two arrays hold the same defaults (4/8/16), nothing looked wrong until a
// theme moved one of them.
TEST_CASE("Theme: padding and spacing read the spacing steps, rounding reads the radius steps")
{
    HE::UITheme theme;
    // Pulled apart on purpose: with the defaults the bug is invisible.
    theme.spacing[static_cast<int>(HE::UIThemeSize::Medium)] = 99.0f;
    theme.radius[static_cast<int>(HE::UIThemeSize::Medium)]  = 7.0f;
    theme.textSize[static_cast<int>(HE::UIThemeTextLevel::Body)] = 21.0f;

    HE::UIWidgetTree t;
    const int box = t.add(HE::UIWidgetType::VerticalBox);
    HE::UIElement* b = t.find(box);
    b->setThemeRole("Padding", "Medium");
    b->setThemeRole("Spacing", "Medium");
    b->setThemeRole("Corner Radius", "Medium");
    const int txt = t.add(HE::UIWidgetType::Text);
    t.find(txt)->setThemeRole("FontSize", "Body");

    HE::uiApplyTheme(t, theme, HE::UIThemeMode::Light);
    CHECK(b->getProp("Padding").f == doctest::Approx(99.0f));
    CHECK(b->getProp("Spacing").f == doctest::Approx(99.0f));
    CHECK(b->getPropAny("Corner Radius").f == doctest::Approx(7.0f));
    CHECK(t.find(txt)->getProp("FontSize").f == doctest::Approx(21.0f));

    // The rule itself, so the designer's role button and the resolver cannot
    // drift apart again: three scales, and the PROPERTY picks which one.
    CHECK(HE::uiThemeScaleFor("Padding")       == HE::UIThemeScale::Spacing);
    CHECK(HE::uiThemeScaleFor("Spacing")       == HE::UIThemeScale::Spacing);
    CHECK(HE::uiThemeScaleFor("FontSize")      == HE::UIThemeScale::Text);
    CHECK(HE::uiThemeScaleFor("Corner Radius") == HE::UIThemeScale::Radius);
    CHECK(HE::uiThemeScaleFor("Border Width")  == HE::UIThemeScale::Radius);
    // A number no vocabulary can name still answers Radius rather than nothing,
    // which is what a hand-written binding on it used to resolve to.
    CHECK(HE::uiThemeScaleFor("Min")           == HE::UIThemeScale::Radius);
    CHECK_FALSE(HE::uiThemeScaleBindable("Min"));
    CHECK(HE::uiThemeScaleBindable("Padding"));
    CHECK(HE::uiThemeScaleBindable("Spacing"));
    CHECK(HE::uiThemeScaleBindable("FontSize"));
}

// D4, second of four: a floor and a ceiling that hold whoever does the placing.
//
// minSize used to live on the four container types and was read in exactly one
// situation, "Size To Content" being on. It is a base property now, together
// with a maxSize that did not exist, and it is applied to the finished RECT —
// which is the only place it can work for a box's child (whose size field the
// box ignores) or for a stretched anchor (whose size field is a negative inset).
TEST_CASE("Layout: Min and Max Size hold the rect, whoever computed it")
{
    HE::UIWidgetCanvas canvas{};
    canvas.width = 800.0f; canvas.height = 600.0f;
    canvas.scaleX = canvas.scaleY = 1.0f;

    SUBCASE("a stretched anchor is held, and the pivot stays put")
    {
        HE::UIWidgetTree t;
        const int p = t.add(HE::UIWidgetType::Panel);
        HE::UIElement* e = t.find(p);
        HE::uiSetAnchorPreset(*e, HE::kUIAnchorFill);
        e->posX = e->posY = 0.0f; e->sizeX = e->sizeY = 0.0f;
        e->pivotX = e->pivotY = 0.5f;
        // Unbound it fills the whole canvas: the size FIELD says 0, which on a
        // stretched axis means "exactly the anchored span".
        CHECK(HE::uiElementRect(t, *e, &canvas).w == doctest::Approx(800.0f));

        e->maxSizeX = 300.0f;
        const HE::UIWidgetRect r = HE::uiElementRect(t, *e, &canvas);
        CHECK(r.w == doctest::Approx(300.0f));
        CHECK(r.h == doctest::Approx(600.0f));      // the other axis is untouched
        // Centred pivot, so the ceiling takes the same amount off each side.
        CHECK(r.x == doctest::Approx(250.0f));
        // And the unbounded answer is still there to be asked for.
        CHECK(HE::uiElementRectUnbounded(t, *e, &canvas).w == doctest::Approx(800.0f));

        // Left-pinned instead: the left edge is what stays.
        e->pivotX = 0.0f;
        CHECK(HE::uiElementRect(t, *e, &canvas).x == doctest::Approx(0.0f));

        // A ceiling below the floor loses.
        e->minSizeX = 500.0f;
        CHECK(HE::uiElementRect(t, *e, &canvas).w == doctest::Approx(500.0f));
    }

    SUBCASE("a box's child is held too, though the box computed its slot")
    {
        HE::UIWidgetTree t;
        const int v = t.add(HE::UIWidgetType::VerticalBox);
        { auto* b = dynamic_cast<HE::UIBoxBase*>(t.find(v));
          b->padding = 0.0f; b->spacing = 0.0f; }
        t.find(v)->sizeX = 400.0f; t.find(v)->sizeY = 300.0f;
        t.find(v)->pivotX = t.find(v)->pivotY = 0.0f;
        const int c = t.add(HE::UIWidgetType::Panel);
        t.find(c)->parentId = v;
        t.find(c)->sizeY = 50.0f;
        t.find(c)->pivotX = 0.0f;
        // Across the axis the box hands over its full inner width…
        CHECK(HE::uiElementRect(t, *t.find(c), &canvas).w == doctest::Approx(400.0f));
        // …and the ceiling holds it anyway, which is the whole point: the size
        // field the author would otherwise reach for is not read here at all.
        t.find(c)->maxSizeX = 120.0f;
        CHECK(HE::uiElementRect(t, *t.find(c), &canvas).w == doctest::Approx(120.0f));
    }

    SUBCASE("a measured box stops at its ceiling, in the size field as well")
    {
        HE::UIWidgetTree t;
        const int v = t.add(HE::UIWidgetType::VerticalBox);
        { auto* b = dynamic_cast<HE::UIBoxBase*>(t.find(v));
          b->padding = 0.0f; b->spacing = 0.0f; b->sizeToContent = true; }
        for (int i = 0; i < 4; ++i)
        {
            const int c = t.add(HE::UIWidgetType::Panel);
            t.find(c)->parentId = v; t.find(c)->sizeY = 30.0f; t.find(c)->sizeX = 40.0f;
        }
        HE::uiApplyAutoSize(t, &canvas);
        CHECK(t.find(v)->sizeY == doctest::Approx(120.0f));

        // The FIELD has to be held, not only the rect: a parent box adds these
        // numbers up when it stacks its children.
        t.find(v)->maxSizeY = 70.0f;
        HE::uiApplyAutoSize(t, &canvas);
        CHECK(t.find(v)->sizeY == doctest::Approx(70.0f));

        t.find(v)->minSizeY = 200.0f;   // the floor still wins over the ceiling
        HE::uiApplyAutoSize(t, &canvas);
        CHECK(t.find(v)->sizeY == doctest::Approx(200.0f));
    }

    SUBCASE("both pairs round-trip, and an unbounded element writes neither key")
    {
        HE::UIWidgetTree t;
        const int p = t.add(HE::UIWidgetType::Button);
        const std::string plain = HE::uiWidgetTreeToJson(t);
        CHECK(plain.find("minSize") == std::string::npos);
        CHECK(plain.find("maxSize") == std::string::npos);

        t.find(p)->setPropAny("Min Size", HE::UIPropValue::ofVec2({ 10.0f, 20.0f }));
        t.find(p)->setPropAny("Max Size", HE::UIPropValue::ofVec2({ 30.0f, 40.0f }));
        // A negative bound is not a third meaning; it lands on "no bound".
        t.find(p)->setPropAny("Max Size", HE::UIPropValue::ofVec2({ 30.0f, -5.0f }));

        HE::UIWidgetTree r;
        REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), r));
        CHECK(r.find(p)->minSizeX == doctest::Approx(10.0f));
        CHECK(r.find(p)->minSizeY == doctest::Approx(20.0f));
        CHECK(r.find(p)->maxSizeX == doctest::Approx(30.0f));
        CHECK(r.find(p)->maxSizeY == doctest::Approx(0.0f));

        // And a clone carries them, which is what a component copy is.
        const auto cl = r.find(p)->clone();
        CHECK(cl->minSizeX == doctest::Approx(10.0f));
        CHECK(cl->maxSizeX == doctest::Approx(30.0f));
    }
}

TEST_CASE("Snapping: the lines an element may line itself up with")
{
    HE::UIWidgetCanvas canvas{};
    canvas.width = 800.0f; canvas.height = 600.0f;
    canvas.scaleX = canvas.scaleY = 1.0f;

    // Two panels side by side under the canvas itself, laid out top-left so the
    // numbers below are the rects and not a pivot puzzle.
    HE::UIWidgetTree t;
    t.canvasWidth = 800.0f; t.canvasHeight = 600.0f;
    const int a = t.add(HE::UIWidgetType::Panel);
    const int b = t.add(HE::UIWidgetType::Panel);
    for (int id : { a, b })
    {
        HE::UIElement* e = t.find(id);
        e->pivotX = e->pivotY = 0.0f;
        HE::uiSetAnchorPreset(*e, 0);          // top-left
        e->sizeX = 100.0f; e->sizeY = 50.0f;
    }
    t.find(a)->posX = 100.0f; t.find(a)->posY = 100.0f;
    t.find(b)->posX = 400.0f; t.find(b)->posY = 300.0f;

    SUBCASE("a sibling's edges and middle, the frame's, and never its own")
    {
        const std::vector<HE::UISnapLine> c = HE::uiSnapCandidates(t, *t.find(a), &canvas);
        const auto has = [&c](float pos, bool vertical, bool center)
        {
            for (const HE::UISnapLine& L : c)
                if (L.vertical == vertical && L.center == center &&
                    std::abs(L.pos - pos) < 0.001f) return true;
            return false;
        };
        // The sibling: left, middle, right — and the same three lying down.
        CHECK(has(400.0f, true,  false));
        CHECK(has(450.0f, true,  true));
        CHECK(has(500.0f, true,  false));
        CHECK(has(300.0f, false, false));
        CHECK(has(325.0f, false, true));
        CHECK(has(350.0f, false, false));
        // The frame around it: no parent, so the canvas.
        CHECK(has(0.0f,   true,  false));
        CHECK(has(400.0f, true,  true));       // the canvas middle, as a middle
        CHECK(has(800.0f, true,  false));
        CHECK(has(300.0f, false, true));
        // Its own edges are not offered: an element cannot align to itself.
        CHECK_FALSE(has(100.0f, true,  false));
        CHECK_FALSE(has(200.0f, true,  false));
        // A hidden sibling offers nothing — the designer does not draw it either.
        t.find(b)->visible = false;
        const std::vector<HE::UISnapLine> c2 = HE::uiSnapCandidates(t, *t.find(a), &canvas);
        CHECK(c2.size() == c.size() - 6);
    }

    SUBCASE("the nearest catch inside the threshold wins, and only inside it")
    {
        const std::vector<HE::UISnapLine> c = HE::uiSnapCandidates(t, *t.find(a), &canvas);
        // Four short of the line at 400 — where the canvas's middle and the
        // sibling's left edge both lie — and the rect's left edge catches it.
        HE::UIWidgetRect r{ 396.0f, 100.0f, 100.0f, 50.0f };
        HE::UISnapResult s = HE::uiSnapDelta(r, c, 6.0f);
        CHECK(s.lineX >= 0);
        CHECK(s.dx == doctest::Approx(4.0f));
        CHECK(c[s.lineX].pos == doctest::Approx(400.0f));
        // …its top is on nothing, so that axis stays where it is.
        CHECK(s.lineY == -1);
        CHECK(s.dy == doctest::Approx(0.0f));

        // Eight short is out of reach: nothing at all, not a smaller nudge.
        r.x = 392.0f;
        s = HE::uiSnapDelta(r, c, 6.0f);
        CHECK(s.lineX == -1);
        CHECK(s.dx == doctest::Approx(0.0f));

        // A threshold of zero is snapping switched off, not "snap exactly".
        r.x = 400.0f;
        CHECK(HE::uiSnapDelta(r, c, 0.0f).lineX == -1);
    }

    SUBCASE("a resize offers only the edge under the cursor")
    {
        const std::vector<HE::UISnapLine> c = HE::uiSnapCandidates(t, *t.find(a), &canvas);
        // One rect with a catch waiting on EACH side, four units away: its left
        // edge under the canvas middle at 400, its right edge under the
        // sibling's middle at 450. Which one is taken says whether the mask is
        // being read, because the distance cannot decide it.
        const HE::UIWidgetRect r{ 396.0f, 100.0f, 50.0f, 50.0f };
        HE::UISnapResult s = HE::uiSnapDelta(r, c, 6.0f, HE::kUISnapMin, 0);
        REQUIRE(s.lineX >= 0);
        CHECK(s.dx == doctest::Approx(4.0f));
        CHECK(c[s.lineX].pos == doctest::Approx(400.0f));
        // The other edge takes the other line, and moves by the same four.
        s = HE::uiSnapDelta(r, c, 6.0f, HE::kUISnapMax, 0);
        REQUIRE(s.lineX >= 0);
        CHECK(s.dx == doctest::Approx(4.0f));
        CHECK(c[s.lineX].pos == doctest::Approx(450.0f));
        // A mask of nothing on an axis is that axis standing still.
        CHECK(HE::uiSnapDelta(r, c, 6.0f, 0, 0).lineX == -1);
    }

    SUBCASE("a child of a box offers its box, not the canvas")
    {
        HE::UIWidgetTree bt;
        bt.canvasWidth = 800.0f; bt.canvasHeight = 600.0f;
        const int panel = bt.add(HE::UIWidgetType::Panel);
        { HE::UIElement* p = bt.find(panel);
          p->pivotX = p->pivotY = 0.0f; HE::uiSetAnchorPreset(*p, 0);
          p->posX = 200.0f; p->posY = 50.0f; p->sizeX = 200.0f; p->sizeY = 100.0f; }
        const int kid = bt.add(HE::UIWidgetType::Button);
        { HE::UIElement* k = bt.find(kid);
          k->parentId = panel; k->pivotX = k->pivotY = 0.0f;
          HE::uiSetAnchorPreset(*k, 0); k->sizeX = 40.0f; k->sizeY = 20.0f; }
        const std::vector<HE::UISnapLine> c = HE::uiSnapCandidates(bt, *bt.find(kid), &canvas);
        // A lone child: six lines, and they are the panel's, not the canvas's.
        REQUIRE(c.size() == 6);
        CHECK(c[0].pos == doctest::Approx(200.0f));   // the panel's left
        CHECK(c[1].pos == doctest::Approx(300.0f));   // its middle
        CHECK(c[2].pos == doctest::Approx(400.0f));   // its right
        CHECK(c[1].center);
        CHECK(c[0].fromId == 0);                      // the frame, not a neighbour
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// F3: the frame a borderless window does not have (docs/he-apps-plan.md)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Window frame: the edge bands, and the corners that reach further")
{
    using HE::UIWindowHit;
    const float W = 800.0f, H = 600.0f, B = 6.0f;   // corner defaults to 2*B = 12

    // The middle is content, and that is the answer that has to be cheap and
    // right: it is what every pixel of an application is.
    CHECK(HE::uiWindowEdgeAt(W, H, 400.0f, 300.0f, B) == UIWindowHit::Normal);

    // Each edge, taken well away from the corners.
    CHECK(HE::uiWindowEdgeAt(W, H,   2.0f, 300.0f, B) == UIWindowHit::ResizeLeft);
    CHECK(HE::uiWindowEdgeAt(W, H, 798.0f, 300.0f, B) == UIWindowHit::ResizeRight);
    CHECK(HE::uiWindowEdgeAt(W, H, 400.0f,   1.0f, B) == UIWindowHit::ResizeTop);
    CHECK(HE::uiWindowEdgeAt(W, H, 400.0f, 599.0f, B) == UIWindowHit::ResizeBottom);

    // …and one pixel further in is already content.
    CHECK(HE::uiWindowEdgeAt(W, H,   7.0f, 300.0f, B) == UIWindowHit::Normal);
    CHECK(HE::uiWindowEdgeAt(W, H, 400.0f,   7.0f, B) == UIWindowHit::Normal);

    // The corners are the reason for the second radius: a point 10 in from the
    // left is not on the left edge at all, but 2 down from the top it is the
    // top-LEFT corner, because the corner reaches along both edges.
    CHECK(HE::uiWindowEdgeAt(W, H,  10.0f,   2.0f, B) == UIWindowHit::ResizeTopLeft);
    CHECK(HE::uiWindowEdgeAt(W, H,   2.0f,  10.0f, B) == UIWindowHit::ResizeTopLeft);
    CHECK(HE::uiWindowEdgeAt(W, H, 790.0f,   2.0f, B) == UIWindowHit::ResizeTopRight);
    CHECK(HE::uiWindowEdgeAt(W, H,   2.0f, 590.0f, B) == UIWindowHit::ResizeBottomLeft);
    CHECK(HE::uiWindowEdgeAt(W, H, 798.0f, 598.0f, B) == UIWindowHit::ResizeBottomRight);

    // Border 0 is what a window that may not be resized asks for, and it must
    // not leave a single grabbable pixel behind.
    CHECK(HE::uiWindowEdgeAt(W, H, 0.0f, 0.0f, 0.0f) == UIWindowHit::Normal);
    CHECK(HE::uiWindowEdgeAt(W, H, 799.0f, 599.0f, 0.0f) == UIWindowHit::Normal);

    // Outside is not an edge. A manual resize asks with a pointer that has left
    // the window every time it makes the window smaller.
    CHECK(HE::uiWindowEdgeAt(W, H, -40.0f, 300.0f, B) == UIWindowHit::Normal);
    CHECK(HE::uiWindowEdgeAt(W, H, 400.0f, 900.0f, B) == UIWindowHit::Normal);

    // A window narrower than two borders: each side keeps its own half, and no
    // point answers both.
    CHECK(HE::uiWindowEdgeAt(8.0f, 400.0f, 1.0f, 200.0f, B) == UIWindowHit::ResizeLeft);
    CHECK(HE::uiWindowEdgeAt(8.0f, 400.0f, 7.0f, 200.0f, B) == UIWindowHit::ResizeRight);

    // The cursors: the diagonals exist so a corner does not promise one axis.
    CHECK(HE::uiWindowHitCursor(UIWindowHit::ResizeLeft)        == HE::UICursor::ResizeWE);
    CHECK(HE::uiWindowHitCursor(UIWindowHit::ResizeTop)         == HE::UICursor::ResizeNS);
    CHECK(HE::uiWindowHitCursor(UIWindowHit::ResizeTopLeft)     == HE::UICursor::ResizeNWSE);
    CHECK(HE::uiWindowHitCursor(UIWindowHit::ResizeBottomRight) == HE::UICursor::ResizeNWSE);
    CHECK(HE::uiWindowHitCursor(UIWindowHit::ResizeTopRight)    == HE::UICursor::ResizeNESW);
    CHECK(HE::uiWindowHitCursor(UIWindowHit::ResizeBottomLeft)  == HE::UICursor::ResizeNESW);
    CHECK(HE::uiWindowHitCursor(UIWindowHit::Drag)              == HE::UICursor::Default);
    CHECK(!HE::uiWindowHitIsResize(UIWindowHit::Drag));
    CHECK(!HE::uiWindowHitIsResize(UIWindowHit::Normal));
    CHECK(HE::uiWindowHitIsResize(UIWindowHit::ResizeBottomLeft));
}

TEST_CASE("Window frame: the minimum bites into the edge that is standing still")
{
    using HE::UIWindowHit;
    const HE::UIWindowRect start{ 100, 100, 800, 600 };

    {   // Right edge: only the width moves.
        HE::UIWindowResizer r;
        CHECK(!r.active());
        r.begin(UIWindowHit::ResizeRight, start, 900, 400);
        REQUIRE(r.active());
        const HE::UIWindowRect grown = r.update(1000, 400, 320, 200);
        CHECK(grown.x == 100); CHECK(grown.y == 100);
        CHECK(grown.w == 900); CHECK(grown.h == 600);
        // Pulled far past the minimum, the width stops and the origin does not
        // move: the left edge was never the one being dragged.
        const HE::UIWindowRect tiny = r.update(0, 400, 320, 200);
        CHECK(tiny.x == 100); CHECK(tiny.w == 320);
        r.end();
        CHECK(!r.active());
    }
    {   // Left edge: the window moves AND changes width, and the right edge —
        // the one nobody is touching — has to end up exactly where it started.
        HE::UIWindowResizer r;
        r.begin(UIWindowHit::ResizeLeft, start, 100, 400);
        const HE::UIWindowRect pulled = r.update(40, 400, 320, 200);
        CHECK(pulled.x == 40);
        CHECK(pulled.w == 860);
        CHECK(pulled.x + pulled.w == start.x + start.w);
        // …and the minimum pins the LEFT edge instead of walking the window.
        const HE::UIWindowRect clamped = r.update(2000, 400, 320, 200);
        CHECK(clamped.w == 320);
        CHECK(clamped.x == start.x + start.w - 320);
        CHECK(clamped.x + clamped.w == start.x + start.w);
    }
    {   // A corner is both at once, each axis with its own minimum.
        HE::UIWindowResizer r;
        r.begin(UIWindowHit::ResizeTopLeft, start, 100, 100);
        const HE::UIWindowRect c = r.update(200, 200, 320, 200);
        CHECK(c.x == 200); CHECK(c.y == 200);
        CHECK(c.w == 700); CHECK(c.h == 500);
        const HE::UIWindowRect cl = r.update(5000, 5000, 320, 200);
        CHECK(cl.w == 320); CHECK(cl.h == 200);
        CHECK(cl.x + cl.w == start.x + start.w);
        CHECK(cl.y + cl.h == start.y + start.h);
    }
    {   // Drag and Normal start nothing: moving a window is the window
        // manager's job even here, and update() then answers with the window
        // unchanged rather than with nonsense.
        HE::UIWindowResizer r;
        r.begin(UIWindowHit::Drag, start, 100, 100);
        CHECK(!r.active());
        r.begin(UIWindowHit::Normal, start, 100, 100);
        CHECK(!r.active());
    }
}

TEST_CASE("Window frame: what the title bar answers, and what keeps its click")
{
    using HE::UIWindowHit;
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    // A window: a 40-tall title bar across the top, with a button in its right
    // corner, and a plain panel filling the rest.
    HE::UIWidgetTree t;
    t.canvasWidth = 800.0f; t.canvasHeight = 600.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int bar = t.add(HE::UIWidgetType::HorizontalBox);
    { HE::UIElement& e = *t.find(bar);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 800.0f; e.sizeY = 40.0f;
      e.windowDrag = true; }
    const int close = t.add(HE::UIWidgetType::Button);
    { HE::UIElement& e = *t.find(close);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 760.0f; e.posY = 4.0f; e.sizeX = 32.0f; e.sizeY = 32.0f; }
    const int body = t.add(HE::UIWidgetType::Panel);
    { HE::UIElement& e = *t.find(body);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.posX = 0.0f; e.posY = 40.0f; e.sizeX = 800.0f; e.sizeY = 560.0f; }
    registerWidget(cm, t, nullptr, "mem://frame.hasset");

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://frame.hasset");
    REQUIRE(id != 0);

    // The bar carries the window — even though a horizontal box is not
    // hit-testable, which is exactly why this is asked geometrically.
    CHECK(wm.windowHitAt(800.0f, 600.0f, 300.0f, 20.0f, 6.0f) == UIWindowHit::Drag);
    // The button on it keeps its click. A close button that moved the window
    // instead of closing it is the whole failure this rule exists for.
    CHECK(wm.windowHitAt(800.0f, 600.0f, 776.0f, 20.0f, 6.0f) == UIWindowHit::Normal);
    // Below the bar there is nothing to carry the window by.
    CHECK(wm.windowHitAt(800.0f, 600.0f, 300.0f, 300.0f, 6.0f) == UIWindowHit::Normal);

    // The edges outrank the picture: the top two pixels of the bar are the top
    // edge, not the caption, because there is nowhere else to put that band.
    CHECK(wm.windowHitAt(800.0f, 600.0f, 300.0f, 2.0f, 6.0f) == UIWindowHit::ResizeTop);
    // …and with resizing off, those same pixels are the caption again.
    CHECK(wm.windowHitAt(800.0f, 600.0f, 300.0f, 2.0f, 0.0f) == UIWindowHit::Drag);
    // An edge wins even over the button, which is what makes the corner of a
    // window grabbable at all.
    CHECK(wm.windowHitAt(800.0f, 600.0f, 776.0f, 2.0f, 6.0f) == UIWindowHit::ResizeTop);

    // A press already in flight on a widget can never turn into a window move:
    // press the button, then let the pointer wander onto the caption.
    wm.processPointer(800.0f, 600.0f, 776.0f, 20.0f, true, true);
    CHECK(wm.windowHitAt(800.0f, 600.0f, 300.0f, 20.0f, 6.0f) == UIWindowHit::Drag);
    wm.processPointer(800.0f, 600.0f, 776.0f, 20.0f, false, true);

    // A hidden bar carries nothing. Same rule the hit test follows, and the
    // reason it is not enough to look the element up by id.
    const_cast<HE::UIWidgetTree*>(wm.tree(id))->find(bar)->visible = false;
    CHECK(wm.windowHitAt(800.0f, 600.0f, 300.0f, 20.0f, 6.0f) == UIWindowHit::Normal);
}

TEST_CASE("Window drag: a base property that costs nothing when it is off")
{
    HE::UIWidgetTree t;
    const int p = t.add(HE::UIWidgetType::Panel);
    // Off by default, and not written: every widget authored before F3 stays
    // byte-identical on disk.
    CHECK(!t.find(p)->windowDrag);
    CHECK(HE::uiWidgetTreeToJson(t).find("windowDrag") == std::string::npos);

    t.find(p)->setPropAny("Window Drag", HE::UIPropValue::ofBool(true));
    CHECK(t.find(p)->windowDrag);
    CHECK(t.find(p)->getPropAny("Window Drag").b);
    CHECK(HE::uiWidgetTreeToJson(t).find("windowDrag") != std::string::npos);

    HE::UIWidgetTree r;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), r));
    CHECK(r.find(p)->windowDrag);
    // …and a clone carries it, which is what a component copy is.
    CHECK(r.find(p)->clone()->windowDrag);

    // It is offered on every type, through the shared base list.
    const auto& base = HE::uiBaseProperties();
    CHECK(std::any_of(base.begin(), base.end(),
                      [](const HE::UIPropDesc& d) { return d.name == "Window Drag"; }));
}

// ── DatePicker / ColorPicker (docs/he-apps-plan.md B9) ───────────────────────

TEST_CASE("The Gregorian calendar, and the century rule nobody remembers")
{
    CHECK(HE::uiIsLeapYear(2024));
    CHECK(HE::uiIsLeapYear(2000));
    CHECK_FALSE(HE::uiIsLeapYear(1900));   // the rule the divisible-by-four half gets wrong
    CHECK_FALSE(HE::uiIsLeapYear(2100));
    CHECK_FALSE(HE::uiIsLeapYear(2026));

    CHECK(HE::uiDaysInMonth(2026, 2) == 28);
    CHECK(HE::uiDaysInMonth(2024, 2) == 29);
    CHECK(HE::uiDaysInMonth(1900, 2) == 28);
    CHECK(HE::uiDaysInMonth(2026, 1) == 31);
    CHECK(HE::uiDaysInMonth(2026, 4) == 30);
    CHECK(HE::uiDaysInMonth(2026, 12) == 31);

    // Dates anybody can check: 0 = Sunday.
    CHECK(HE::uiDayOfWeek(2026, 9, 6) == 0);    // Sunday
    CHECK(HE::uiDayOfWeek(2026, 9, 7) == 1);    // Monday
    CHECK(HE::uiDayOfWeek(2000, 1, 1) == 6);    // Saturday
    CHECK(HE::uiDayOfWeek(1970, 1, 1) == 4);    // Thursday, the epoch
    CHECK(HE::uiDayOfWeek(2024, 2, 29) == 4);   // a leap day, Thursday
    // Every day of a fortnight walks forward by one and wraps.
    for (int d = 1; d <= 14; ++d)
        CHECK(HE::uiDayOfWeek(2026, 9, d) == (HE::uiDayOfWeek(2026, 9, 1) + d - 1) % 7);
}

TEST_CASE("DatePicker: the grid is a continuous calendar, and it clamps at READ")
{
    HE::UIDatePicker dp;
    dp.year = 2026; dp.month = 9; dp.day = 6; dp.mondayFirst = true;

    // 1 September 2026 is a Tuesday, so with Monday first it sits in column 1
    // and August's 31st fills the one before it.
    CHECK(dp.firstCell() == 1);
    int y = 0, m = 0, d = 0;
    dp.dateAtCell(0, y, m, d);
    CHECK(y == 2026); CHECK(m == 8); CHECK(d == 31);
    CHECK_FALSE(dp.cellInMonth(0));
    dp.dateAtCell(1, y, m, d);
    CHECK(m == 9); CHECK(d == 1);
    CHECK(dp.cellInMonth(1));
    // September has 30 days, so cell 1 + 30 = 31 is the 1st of October.
    dp.dateAtCell(31, y, m, d);
    CHECK(y == 2026); CHECK(m == 10); CHECK(d == 1);
    CHECK_FALSE(dp.cellInMonth(31));
    // Every cell of the grid is one day after the one before it.
    for (int i = 1; i < HE::UIDatePicker::kCells; ++i)
    {
        int ay = 0, am = 0, ad = 0, by = 0, bm = 0, bd = 0;
        dp.dateAtCell(i - 1, ay, am, ad);
        dp.dateAtCell(i, by, bm, bd);
        const int next = ad + 1 > HE::uiDaysInMonth(ay, am) ? 1 : ad + 1;
        CHECK(bd == next);
    }
    // Sunday first shifts the whole grid by one column.
    dp.mondayFirst = false;
    CHECK(dp.firstCell() == 2);

    // The picked day is the cell it is drawn in, and the caption says so.
    dp.mondayFirst = true;
    CHECK(dp.selectedCell() == 1 + 6 - 1);
    CHECK(dp.isoDate() == "2026-09-06");
    CHECK(dp.caption() == "September 2026");

    // A property may be written out of range — the editor and HorizonCode have
    // to round-trip what they wrote — and every READ clamps instead.
    dp.setProp("Month", HE::UIPropValue::ofInt(99));
    CHECK(dp.getProp("Month").i == 99);      // stored verbatim
    CHECK(dp.clampedMonth() == 12);
    dp.month = 2; dp.year = 2026; dp.day = 31;
    CHECK(dp.getProp("Day").i == 31);
    CHECK(dp.clampedDay() == 28);            // February does not have a 31st
    CHECK(dp.isoDate() == "2026-02-28");
}

TEST_CASE("DatePicker: paging keeps the day where a short month lets it")
{
    HE::UIDatePicker dp;
    dp.year = 2026; dp.month = 1; dp.day = 31;
    dp.addMonths(1);
    CHECK(dp.month == 2);
    CHECK(dp.day == 28);                     // the 31st of February is the 28th
    dp.addMonths(1);
    CHECK(dp.month == 3);
    CHECK(dp.day == 28);                     // …and it does NOT grow back
    dp.year = 2026; dp.month = 12; dp.day = 15;
    dp.addMonths(1);
    CHECK(dp.year == 2027); CHECK(dp.month == 1);
    dp.addMonths(-1);
    CHECK(dp.year == 2026); CHECK(dp.month == 12);
}

TEST_CASE("DatePicker: the grid is drawn and grabbed by ONE sum")
{
    const HE::UIWidgetRect r{ 100.0f, 50.0f, 280.0f, 240.0f };
    const auto L = HE::UIDatePicker::layoutIn(r);
    // Eight rows' worth: 1.25 caption + 0.75 weekdays + six day rows.
    CHECK(L.header.h == doctest::Approx(240.0f / 8.0f * 1.25f));
    CHECK(L.weekdays.y == doctest::Approx(L.header.y + L.header.h));
    CHECK(L.grid.y == doctest::Approx(L.weekdays.y + L.weekdays.h));
    CHECK(L.grid.y + L.grid.h == doctest::Approx(r.y + r.h));
    CHECK(L.cellW == doctest::Approx(280.0f / 7.0f));
    CHECK(L.cellH * 6.0f == doctest::Approx(L.grid.h));

    // Every cell answers where it is drawn, and only there.
    for (int i = 0; i < HE::UIDatePicker::kCells; ++i)
    {
        const float cx = L.grid.x + L.cellW * (i % 7 + 0.5f);
        const float cy = L.grid.y + L.cellH * (i / 7 + 0.5f);
        CHECK(HE::UIDatePicker::cellAt(r, cx, cy) == i);
    }
    CHECK(HE::UIDatePicker::cellAt(r, r.x + 4.0f, L.header.y + 2.0f) == -1);   // the caption
    CHECK(HE::UIDatePicker::cellAt(r, r.x - 5.0f, L.grid.y + 5.0f) == -1);     // outside

    // The arrows are the two ends of the caption and nothing in between.
    CHECK(HE::UIDatePicker::arrowAt(r, L.prevArrow.x + 2.0f, L.prevArrow.y + 2.0f) == -1);
    CHECK(HE::UIDatePicker::arrowAt(r, L.nextArrow.x + 2.0f, L.nextArrow.y + 2.0f) == 1);
    CHECK(HE::UIDatePicker::arrowAt(r, r.x + r.w * 0.5f, L.header.y + 2.0f) == 0);
    CHECK(HE::UIDatePicker::arrowAt(r, r.x + r.w * 0.5f, L.grid.y + 2.0f) == 0);
}

TEST_CASE("DatePicker: clicking a day picks it, an arrow pages, and the edge days do both")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 280.0f; t.canvasHeight = 240.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int dpId = t.add(HE::UIWidgetType::DatePicker);
    { HE::UIElement& e = *t.find(dpId);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.sizeX = 280.0f; e.sizeY = 240.0f; }
    { auto* d = dynamic_cast<HE::UIDatePicker*>(t.find(dpId));
      d->year = 2026; d->month = 9; d->day = 6; d->mondayFirst = true; }
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    auto* live = dynamic_cast<HE::UIDatePicker*>(
        const_cast<HE::UIWidgetTree*>(wm.tree(id))->find(dpId));
    REQUIRE(live);

    const HE::UIWidgetRect r{ 0.0f, 0.0f, 280.0f, 240.0f };
    const auto L = HE::UIDatePicker::layoutIn(r);
    auto centreOf = [&](int cell, float& x, float& y)
    {
        x = L.grid.x + L.cellW * (cell % 7 + 0.5f);
        y = L.grid.y + L.cellH * (cell / 7 + 0.5f);
    };
    auto click = [&](float x, float y)
    {
        wm.processPointer(280.0f, 240.0f, x, y, true, true);
        wm.processPointer(280.0f, 240.0f, x, y, false, true);
    };

    // Cell 1 is the 1st of September; cell 15 is the 15th.
    float x = 0.0f, y = 0.0f;
    centreOf(15, x, y);
    click(x, y);
    CHECK(live->isoDate() == "2026-09-15");

    // Cell 0 is August's 31st, and clicking it PAGES to August as well as
    // picking it — which is the shortest way to the month before there is.
    centreOf(0, x, y);
    click(x, y);
    CHECK(live->isoDate() == "2026-08-31");
    CHECK(live->month == 8);

    // The right arrow pages forward WITHOUT changing the picked date, and the
    // release must not then land on whatever day is under the arrow.
    live->year = 2026; live->month = 9; live->day = 6;
    click(L.nextArrow.x + 2.0f, L.nextArrow.y + 2.0f);
    CHECK(live->month == 10);
    CHECK(live->day == 6);
    click(L.prevArrow.x + 2.0f, L.prevArrow.y + 2.0f);
    CHECK(live->month == 9);
    CHECK(live->day == 6);
}

TEST_CASE("HSV and RGB are each other's inverse, and grey keeps its hue")
{
    // Round trip through every corner of the cube plus a scatter of the inside.
    const glm::vec3 kProbes[] = {
        { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f },
        { 1.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 1.0f }, { 1.0f, 0.0f, 1.0f },
        { 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 0.0f }, { 0.5f, 0.5f, 0.5f },
        { 0.30f, 0.60f, 0.90f }, { 0.87f, 0.42f, 0.11f }, { 0.02f, 0.55f, 0.31f } };
    for (const glm::vec3& c : kProbes)
    {
        float h = 0.0f, s = 0.0f, v = 0.0f;
        HE::uiRgbToHsv(c, h, s, v);
        const glm::vec3 back = HE::uiHsvToRgb(h, s, v);
        INFO("rgb ", c.r, " ", c.g, " ", c.b);
        CHECK(back.r == doctest::Approx(c.r).epsilon(0.0005));
        CHECK(back.g == doctest::Approx(c.g).epsilon(0.0005));
        CHECK(back.b == doctest::Approx(c.b).epsilon(0.0005));
    }
    // The six pure hues, where the strip's segment boundaries are.
    for (int i = 0; i <= 6; ++i)
    {
        const glm::vec3 rgb = HE::uiHsvToRgb(i * 60.0f, 1.0f, 1.0f);
        CHECK(std::max(rgb.r, std::max(rgb.g, rgb.b)) == doctest::Approx(1.0f));
        CHECK(std::min(rgb.r, std::min(rgb.g, rgb.b)) == doctest::Approx(0.0f));
    }
    // 360 is 0.
    const glm::vec3 a = HE::uiHsvToRgb(0.0f, 1.0f, 1.0f);
    const glm::vec3 b = HE::uiHsvToRgb(360.0f, 1.0f, 1.0f);
    CHECK(a.r == doctest::Approx(b.r));
    CHECK(a.g == doctest::Approx(b.g));

    // The grey trap: dragging the value to black must not lose the hue, or
    // coming back up would land on red.
    HE::UIColorPicker cp;
    cp.setHsv(210.0f, 0.8f, 0.9f);
    CHECK(cp.hueOf() == doctest::Approx(210.0f).epsilon(0.01));
    cp.setHsv(cp.hueOf(), 0.0f, 0.0f);           // black
    CHECK(cp.saturationOf() == doctest::Approx(0.0f));
    CHECK(cp.hueOf() == doctest::Approx(210.0f).epsilon(0.01));
    cp.setHsv(cp.hueOf(), 1.0f, 1.0f);
    CHECK(cp.hueOf() == doctest::Approx(210.0f).epsilon(0.01));
}

TEST_CASE("ColorPicker: the Color property round-trips verbatim and keeps the hue in step")
{
    HE::UIColorPicker cp;
    // What is written is what is read back, bit for bit — no trip through HSV.
    const glm::vec4 want{ 0.123f, 0.456f, 0.789f, 0.5f };
    cp.setProp("Color", HE::UIPropValue::ofColor(want));
    CHECK(cp.getProp("Color").col == want);
    // …and the hue followed it.
    float h = 0.0f, s = 0.0f, v = 0.0f;
    HE::uiRgbToHsv(glm::vec3(want), h, s, v);
    CHECK(cp.hueOf() == doctest::Approx(h));
    // Writing a GREY leaves the remembered hue alone, which is the whole point
    // of the field existing.
    const float was = cp.hueOf();
    cp.setProp("Color", HE::UIPropValue::ofColor(glm::vec4(0.4f, 0.4f, 0.4f, 1.0f)));
    CHECK(cp.getProp("Color").col == glm::vec4(0.4f, 0.4f, 0.4f, 1.0f));
    CHECK(cp.hueOf() == doctest::Approx(was));
    // Alpha is the colour's own and setHsv does not touch it.
    cp.color.a = 0.25f;
    cp.setHsv(90.0f, 1.0f, 1.0f);
    CHECK(cp.color.a == doctest::Approx(0.25f));
}

TEST_CASE("ColorPicker: the field, the strip and the alpha bar are one sum")
{
    const HE::UIWidgetRect r{ 20.0f, 10.0f, 240.0f, 180.0f };
    {
        const auto p = HE::UIColorPicker::partsIn(r, 18.0f, 8.0f, false);
        CHECK_FALSE(p.hasAlpha);
        CHECK(p.sv.w == doctest::Approx(240.0f - 26.0f));
        CHECK(p.hue.x == doctest::Approx(p.sv.x + p.sv.w + 8.0f));
        CHECK(p.hue.x + p.hue.w == doctest::Approx(r.x + r.w));
        CHECK(p.sv.h == doctest::Approx(r.h));
    }
    {
        const auto p = HE::UIColorPicker::partsIn(r, 18.0f, 8.0f, true);
        CHECK(p.hasAlpha);
        CHECK(p.alpha.x + p.alpha.w == doctest::Approx(r.x + r.w));
        CHECK(p.alpha.x == doctest::Approx(p.hue.x + p.hue.w + 8.0f));
        CHECK(p.sv.w == doctest::Approx(240.0f - 2.0f * 26.0f));
    }
    // Squeezed narrower than its own strips, the field is a pixel rather than a
    // negative width — a picker nobody can use, not one that draws backwards.
    const auto tiny = HE::UIColorPicker::partsIn({ 0.0f, 0.0f, 10.0f, 40.0f },
                                                 18.0f, 8.0f, true);
    CHECK(tiny.sv.w >= 1.0f);
}

TEST_CASE("ColorPicker: a drag on the field is saturation and value, on the strip the hue")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 240.0f; t.canvasHeight = 180.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int cpId = t.add(HE::UIWidgetType::ColorPicker);
    { HE::UIElement& e = *t.find(cpId);
      HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
      e.sizeX = 240.0f; e.sizeY = 180.0f; }
    { auto* c = dynamic_cast<HE::UIColorPicker*>(t.find(cpId));
      c->showAlpha = true; c->setHsv(0.0f, 1.0f, 1.0f); }
    registerWidget(cm, t);

    WidgetManager wm;
    const int id = createShown(wm, cm, "mem://w.hasset");
    REQUIRE(id != 0);
    auto* live = dynamic_cast<HE::UIColorPicker*>(
        const_cast<HE::UIWidgetTree*>(wm.tree(id))->find(cpId));
    REQUIRE(live);

    const auto p = HE::UIColorPicker::partsIn({ 0.0f, 0.0f, 240.0f, 180.0f },
                                              live->barWidth, live->gap, true);

    // Halfway down the hue strip is 180°, cyan.
    wm.processPointer(240.0f, 180.0f, p.hue.x + p.hue.w * 0.5f,
                      p.hue.y + p.hue.h * 0.5f, true, true);
    CHECK(live->hueOf() == doctest::Approx(180.0f).epsilon(0.02));
    // Dragging OFF the strip and across the field keeps changing the hue: the
    // part was chosen at the press and a grab is only ended by letting go.
    wm.processPointer(240.0f, 180.0f, p.sv.x + 4.0f, p.hue.y + p.hue.h * 0.25f, true, true);
    CHECK(live->hueOf() == doctest::Approx(90.0f).epsilon(0.02));
    wm.processPointer(240.0f, 180.0f, p.sv.x + 4.0f, p.hue.y + p.hue.h * 0.25f, false, true);

    // The field's top-right corner is S = 1, V = 1 — the pure hue.
    wm.processPointer(240.0f, 180.0f, p.sv.x + p.sv.w - 1.0f, p.sv.y + 1.0f, true, true);
    CHECK(live->saturationOf() == doctest::Approx(1.0f).epsilon(0.02));
    CHECK(live->valueOf() == doctest::Approx(1.0f).epsilon(0.02));
    // …its bottom edge is black, and the hue survives that.
    wm.processPointer(240.0f, 180.0f, p.sv.x + p.sv.w - 1.0f,
                      p.sv.y + p.sv.h - 0.5f, true, true);
    CHECK(live->valueOf() == doctest::Approx(0.0f).epsilon(0.02));
    CHECK(live->hueOf() == doctest::Approx(90.0f).epsilon(0.02));
    // …and its left edge is grey, whatever the hue says.
    wm.processPointer(240.0f, 180.0f, p.sv.x, p.sv.y + 1.0f, true, true);
    CHECK(live->saturationOf() == doctest::Approx(0.0f).epsilon(0.02));
    wm.processPointer(240.0f, 180.0f, p.sv.x, p.sv.y + 1.0f, false, true);

    // The alpha strip runs opaque at the top to nothing at the bottom.
    wm.processPointer(240.0f, 180.0f, p.alpha.x + p.alpha.w * 0.5f,
                      p.alpha.y + p.alpha.h * 0.5f, true, true);
    CHECK(live->color.a == doctest::Approx(0.5f).epsilon(0.02));
    wm.processPointer(240.0f, 180.0f, p.alpha.x + p.alpha.w * 0.5f,
                      p.alpha.y + 1.0f, true, true);
    CHECK(live->color.a == doctest::Approx(1.0f).epsilon(0.02));
    wm.processPointer(240.0f, 180.0f, p.alpha.x + p.alpha.w * 0.5f,
                      p.alpha.y + 1.0f, false, true);
}

TEST_CASE("Both pickers draw a full-rect surface first, so the border finds it")
{
    // The rule WidgetManager::extract enforces: the style is stamped onto the
    // FIRST quad and only when it covers the whole rect. A picker whose first
    // quad were a part would silently lose its rounding and its border.
    const HE::UIWidgetRect px{ 10.0f, 20.0f, 240.0f, 200.0f };
    HE::UIElementRenderState st;
    for (HE::UIWidgetType ty : { HE::UIWidgetType::DatePicker, HE::UIWidgetType::ColorPicker })
    {
        auto e = HE::makeUIElement(ty);
        REQUIRE(e);
        INFO(e->typeName());
        CHECK(e->hasSurfaceStyle());
        std::vector<UIRenderObject> out;
        e->render(px, st, HE::UUID{}, 1.0f, out);
        REQUIRE(!out.empty());
        CHECK(out[0].type == 0);
        CHECK(out[0].position.x == doctest::Approx(px.x));
        CHECK(out[0].position.y == doctest::Approx(px.y));
        CHECK(out[0].size.x == doctest::Approx(px.w));
        CHECK(out[0].size.y == doctest::Approx(px.h));
    }
}

// ═══ "An Inhalt anpassen" for the leaf types (docs/he-apps-plan.md D4) ═══════
// Until here, fitting to content was a container's trick (Size To Content) and a
// label's (AutoSize). A checkbox whose caption was translated and a combo whose
// options came out of a project file were sized by hand, which is a number that
// is right until somebody changes the words. These pin the two measurements and
// the rule that decides who owns an axis.

TEST_CASE("CheckBox: AutoSize fits the row to control, gap and label")
{
    HE::UIWidgetTree t;
    const int a = t.add(HE::UIWidgetType::CheckBox);
    const int b = t.add(HE::UIWidgetType::CheckBox);
    auto* shortOne = dynamic_cast<HE::UICheckBox*>(t.find(a));
    auto* longOne  = dynamic_cast<HE::UICheckBox*>(t.find(b));
    REQUIRE(shortOne); REQUIRE(longOne);
    shortOne->label = "On";
    longOne->label  = "Show the grid while dragging";

    // Off is what every checkbox authored before this means: the size it was
    // given, whatever is written on it.
    HE::uiApplyAutoSize(t);
    CHECK(shortOne->sizeX == doctest::Approx(200.0f));
    CHECK(longOne->sizeX  == doctest::Approx(200.0f));

    shortOne->autoSize = true;
    longOne->autoSize  = true;
    HE::uiApplyAutoSize(t);
    // The longer caption gets the wider row, and both are wider than the tick
    // box alone — measured against each other, so no font decides the case.
    CHECK(longOne->sizeX > shortOne->sizeX);
    const HE::UICheckBox::BoxMetrics m =
        HE::UICheckBox::metricsFor(shortOne->fontSize, 0.0f, false);
    CHECK(shortOne->sizeX > m.ctrl + m.gap);
    // The row is at least as tall as the box drawn in it.
    CHECK(shortOne->sizeY >= doctest::Approx(m.box));
}

TEST_CASE("CheckBox: a switch measures wider than a tick box for the same label")
{
    HE::UIWidgetTree t;
    const int a = t.add(HE::UIWidgetType::CheckBox);
    const int b = t.add(HE::UIWidgetType::CheckBox);
    auto* box = dynamic_cast<HE::UICheckBox*>(t.find(a));
    auto* sw  = dynamic_cast<HE::UICheckBox*>(t.find(b));
    REQUIRE(box); REQUIRE(sw);
    box->label = sw->label = "Wireframe";
    box->autoSize = sw->autoSize = true;
    sw->switchStyle = true;
    HE::uiApplyAutoSize(t);
    // The control is 1.8 boxes wide instead of one, and nothing else changed.
    const HE::UICheckBox::BoxMetrics m =
        HE::UICheckBox::metricsFor(box->fontSize, 0.0f, false);
    CHECK(sw->sizeX == doctest::Approx(box->sizeX + m.box * 0.8f));
}

TEST_CASE("CheckBox: a bigger reader font makes the row bigger, not the label smaller")
{
    HE::UIWidgetTree t;
    const int id = t.add(HE::UIWidgetType::CheckBox);
    auto* cb = dynamic_cast<HE::UICheckBox*>(t.find(id));
    REQUIRE(cb);
    cb->label = "Play sound on completion";
    cb->autoSize = true;
    HE::uiApplyAutoSize(t, nullptr, 1.0f);
    const float w100 = cb->sizeX, h100 = cb->sizeY;
    HE::uiApplyAutoSize(t, nullptr, 1.5f);
    // "Fit your content" is a question about the DRAWN size, which is why the
    // reader's text size reaches this far down at all (B10).
    CHECK(cb->sizeX > w100);
    CHECK(cb->sizeY > h100);
}

TEST_CASE("ComboBox: AutoSize measures every option, not the selected one")
{
    HE::UIWidgetTree t;
    const int id = t.add(HE::UIWidgetType::ComboBox);
    auto* cb = dynamic_cast<HE::UIComboBox*>(t.find(id));
    REQUIRE(cb);
    cb->options = { "Low", "Medium", "Ultra, with every extra turned on" };
    cb->selectedIndex = 0;
    cb->autoSize = true;
    HE::uiApplyAutoSize(t);
    const float w = cb->sizeX;
    // A box that resized itself on every pick would move whatever sits next to
    // it — and the widest entry has to fit anyway, or the open list is wider
    // than the thing it drops out of.
    cb->selectedIndex = 2;
    HE::uiApplyAutoSize(t);
    CHECK(cb->sizeX == doctest::Approx(w));
    cb->selectedIndex = 1;
    HE::uiApplyAutoSize(t);
    CHECK(cb->sizeX == doctest::Approx(w));
    // …and it is wider than the same combo with only the short entries in it.
    cb->options = { "Low", "Medium" };
    HE::uiApplyAutoSize(t);
    CHECK(cb->sizeX < w);
    // The arrow keeps its square: the width always leaves at least the height
    // free on the right, plus the rounded inset on the left.
    CHECK(cb->sizeX > cb->sizeY + HE::UIComboBox::contentInset(cb->cornerRadius.x));
}

TEST_CASE("ComboBox: no options is still a box somebody can click")
{
    HE::UIWidgetTree t;
    const int id = t.add(HE::UIWidgetType::ComboBox);
    auto* cb = dynamic_cast<HE::UIComboBox*>(t.find(id));
    REQUIRE(cb);
    cb->options.clear();
    cb->autoSize = true;
    HE::uiApplyAutoSize(t);
    CHECK(cb->sizeY > 1.0f);
    CHECK(cb->sizeX > cb->sizeY);   // the arrow's square, and room beside it
}

TEST_CASE("A measured leaf is held between Min and Max Size, an authored one is not")
{
    HE::UIWidgetTree t;
    const int id = t.add(HE::UIWidgetType::CheckBox);
    auto* cb = dynamic_cast<HE::UICheckBox*>(t.find(id));
    REQUIRE(cb);
    cb->label = "A caption long enough to need more than sixty units of room";
    cb->autoSize = true;
    cb->maxSizeX = 60.0f;
    HE::uiApplyAutoSize(t);
    // The SIZE FIELD, not just the rect: a parent box adds these up when it
    // stacks its children, so a leaf that measured itself past its ceiling
    // would push its siblings by a width it does not have.
    CHECK(cb->sizeX == doctest::Approx(60.0f));

    cb->maxSizeX = 0.0f;
    cb->minSizeX = 900.0f;
    HE::uiApplyAutoSize(t);
    CHECK(cb->sizeX == doctest::Approx(900.0f));

    // A max under the min loses — "never smaller than that" is the promise a
    // layout can actually keep (same rule as the containers').
    cb->maxSizeX = 100.0f;
    HE::uiApplyAutoSize(t);
    CHECK(cb->sizeX == doctest::Approx(900.0f));

    // With AutoSize off nothing is measured, so nothing is clamped either: the
    // authored number survives a bound that would have bitten.
    cb->autoSize = false;
    cb->minSizeX = 0.0f;
    cb->maxSizeX = 10.0f;
    cb->sizeX = 200.0f;
    HE::uiApplyAutoSize(t);
    CHECK(cb->sizeX == doctest::Approx(200.0f));
}

TEST_CASE("autoSizedAxes: one answer for the clamp and for the designer")
{
    HE::UIWidgetTree t;
    const int textId = t.add(HE::UIWidgetType::Text);
    auto* tx = dynamic_cast<HE::UIText*>(t.find(textId));
    REQUIRE(tx);
    tx->autoSize = true;
    CHECK(tx->autoSizedAxes() == (HE::UIElement::kAxisX | HE::UIElement::kAxisY));
    // A wrapping label authors its width: that IS the column the words break
    // against, so only the height follows the text.
    tx->wordWrap = true;
    CHECK(tx->autoSizedAxes() == HE::UIElement::kAxisY);
    // No axis a stretching anchor owns is ever measured.
    tx->wordWrap = false;
    HE::uiSetAnchorPreset(*tx, 15);            // stretched both ways
    CHECK(tx->autoSizedAxes() == 0);
    tx->autoSize = false;
    HE::uiSetAnchorPreset(*tx, 0);
    CHECK(tx->autoSizedAxes() == 0);

    // The containers answer for the pass that actually writes them.
    const int wrapId = t.add(HE::UIWidgetType::WrapBox);
    auto* wb = dynamic_cast<HE::UIWrapBox*>(t.find(wrapId));
    REQUIRE(wb);
    CHECK(wb->autoSizedAxes() == 0);
    wb->sizeToContent = true;
    CHECK(wb->autoSizedAxes() == HE::UIElement::kAxisY);

    const int boxId = t.add(HE::UIWidgetType::VerticalBox);
    auto* vb = dynamic_cast<HE::UIBoxBase*>(t.find(boxId));
    REQUIRE(vb);
    vb->sizeToContent = true;
    CHECK(vb->autoSizedAxes() == (HE::UIElement::kAxisX | HE::UIElement::kAxisY));

    // And a type that has no content to measure says so.
    const int slider = t.add(HE::UIWidgetType::Slider);
    CHECK(t.find(slider)->autoSizedAxes() == 0);
}

TEST_CASE("Leaf AutoSize round-trips, and an older widget saves byte-identical")
{
    HE::UIWidgetTree t;
    const int c = t.add(HE::UIWidgetType::CheckBox);
    const int b = t.add(HE::UIWidgetType::ComboBox);
    auto* cb = dynamic_cast<HE::UICheckBox*>(t.find(c));
    auto* combo = dynamic_cast<HE::UIComboBox*>(t.find(b));
    REQUIRE(cb); REQUIRE(combo);

    // Off by default and never written, so a widget authored before this saves
    // exactly what it saved yesterday.
    CHECK_FALSE(cb->autoSize);
    CHECK_FALSE(combo->autoSize);
    CHECK(HE::uiWidgetTreeToJson(t).find("autoSize") == std::string::npos);

    cb->autoSize = true;
    combo->autoSize = true;
    HE::UIWidgetTree r;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), r));
    CHECK(dynamic_cast<const HE::UICheckBox*>(r.find(c))->autoSize);
    CHECK(dynamic_cast<const HE::UIComboBox*>(r.find(b))->autoSize);

    // …and through the property table, which is what a graph and the designer
    // both write it with.
    cb->setProp("AutoSize", HE::UIPropValue::ofBool(false));
    CHECK_FALSE(cb->autoSize);
    CHECK_FALSE(cb->getProp("AutoSize").b);
    combo->setProp("AutoSize", HE::UIPropValue::ofBool(false));
    CHECK_FALSE(combo->getProp("AutoSize").b);
}
