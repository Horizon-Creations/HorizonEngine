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
#include <HorizonScene/WidgetManager.h>
#include <algorithm>
#include <filesystem>

namespace
{
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
    HE::UIButton b;
    b.setProp("Text", HE::UIPropValue::ofString("original"));
    auto c = b.clone();
    c->setProp("Text", HE::UIPropValue::ofString("changed"));
    CHECK(b.getProp("Text").s == "original");
    CHECK(c->getProp("Text").s == "changed");
    CHECK(c->type() == HE::UIWidgetType::Button);
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
            { "Tint", UIPropType::Color } } },
        { UIWidgetType::Text, {
            { "Text", UIPropType::String },
            { "FontSize", UIPropType::Float },
            { "Color", UIPropType::Color },
            { "WordWrap", UIPropType::Bool },
            { "AutoSize", UIPropType::Bool },
            { "Center", UIPropType::Bool } } },
        { UIWidgetType::Button, {
            { "Text", UIPropType::String },
            { "FontSize", UIPropType::Float },
            { "Normal Color", UIPropType::Color },
            { "Hovered Color", UIPropType::Color },
            { "Pressed Color", UIPropType::Color },
            { "Text Color", UIPropType::Color } } },
        { UIWidgetType::CheckBox, {
            { "Checked", UIPropType::Bool },
            { "Label", UIPropType::String },
            { "FontSize", UIPropType::Float },
            { "Box Color", UIPropType::Color },
            { "Check Color", UIPropType::Color },
            { "Text Color", UIPropType::Color } } },
        { UIWidgetType::Slider, {
            { "Value", UIPropType::Float },
            { "Min", UIPropType::Float },
            { "Max", UIPropType::Float },
            { "Track Color", UIPropType::Color },
            { "Fill Color", UIPropType::Color },
            { "Handle Color", UIPropType::Color } } },
        { UIWidgetType::ProgressBar, {
            { "Value", UIPropType::Float },
            { "Back Color", UIPropType::Color },
            { "Fill Color", UIPropType::Color } } },
        { UIWidgetType::TextInput, {
            { "Text", UIPropType::String },
            { "Placeholder", UIPropType::String },
            { "FontSize", UIPropType::Float },
            { "Back Color", UIPropType::Color },
            { "Text Color", UIPropType::Color } } },
        { UIWidgetType::ComboBox, {
            { "Options", UIPropType::StringList },
            { "Selected Index", UIPropType::Int },
            { "FontSize", UIPropType::Float },
            { "Back Color", UIPropType::Color },
            { "Text Color", UIPropType::Color },
            { "Highlight Color", UIPropType::Color } } },
        // The layout boxes share one table: the slot algorithm reads these two
        // BY NAME (see boxSlotRect), so renaming them silently un-pads every
        // box in every saved widget.
        { UIWidgetType::VerticalBox, {
            { "Padding", UIPropType::Float },
            { "Spacing", UIPropType::Float } } },
        { UIWidgetType::HorizontalBox, {
            { "Padding", UIPropType::Float },
            { "Spacing", UIPropType::Float } } },
        { UIWidgetType::ScrollBox, {
            { "Padding", UIPropType::Float },
            { "Spacing", UIPropType::Float },
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
        "Hover Cursor", "Material", "Font" };

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

    CHECK(HE::UIText{}.events().empty());
    CHECK(HE::UIProgressBar{}.events().empty());
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
    const int b = a.add(HE::UIWidgetType::Button);
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
    t.find(b)->setProp("Text", HE::UIPropValue::ofString("PLAY"));
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
    CHECK(r.find(b)->getProp("Text").s == "PLAY");
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
    const int id = wm.createWidget(cm, "mem://w.hasset");
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
    REQUIRE(wm.createWidget(cm, "mem://w.hasset") != 0);
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
    REQUIRE(wm.createWidget(cm, "mem://w.hasset") != 0);

    std::vector<UIRenderObject> out;
    wm.extract(1920.0f, 1080.0f, out);
    CHECK(countGlyphs(out) == 0);

    CHECK(wm.processPointer(1920.0f, 1080.0f, 100.0f, 25.0f, true,  true));
    CHECK(wm.processPointer(1920.0f, 1080.0f, 100.0f, 25.0f, false, true));

    out.clear();
    wm.extract(1920.0f, 1080.0f, out);
    CHECK(countGlyphs(out) == 2); // "OK"
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
    REQUIRE(wm.createWidget(cm, "mem://w.hasset") != 0);

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
    const int id = wm.createWidget(cm, "mem://w.hasset");
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
    const int id = wm.createWidget(cm, "mem://w.hasset");
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
    auto e = makeUIElement(UIWidgetType::Button); // has material slot + text
    e->setPropAny("Material", UIPropValue::ofString("Content/M.hasset"));
    CHECK(e->getPropAny("Material").s == "Content/M.hasset");
    CHECK(e->material == "Content/M.hasset");
    e->setPropAny("Font", UIPropValue::ofString("Content/F.hasset"));
    CHECK(e->getPropAny("Font").s == "Content/F.hasset");
    CHECK(e->font == "Content/F.hasset");

    // Font is enumerated only for text-bearing types
    auto hasFont = [](const UIElement& el) {
        for (const auto& pd : el.allProperties()) if (pd.name == "Font") return true;
        return false;
    };
    CHECK(hasFont(*e));
    CHECK(!hasFont(*makeUIElement(UIWidgetType::Panel)));
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
        for (unsigned char ch : l) if (ch >= 32 && ch < 128) w += f.glyphs[ch - 32].xadvance * scale;
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
    t.text = "a\nb"; t.wordWrap = true; t.autoSize = true; t.align = 1;
    nlohmann::json j;
    t.writeJson(j);
    HE::UIText r;
    r.readJson(j);
    CHECK(r.text == "a\nb");
    CHECK(r.wordWrap);
    CHECK(r.autoSize);
    CHECK(r.align == 1);

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
    REQUIRE(wm.createWidget(cm, "mem://w.hasset") != 0);

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
    REQUIRE(wm.createWidget(cm, "mem://w.hasset") != 0);
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
    REQUIRE(wm.createWidget(cm, "mem://w.hasset") != 0);
    // The second button sits at y 100..200 — that is where the click lands,
    // and nowhere near the (999, -999) its own position claims.
    CHECK(wm.processPointer(200.0f, 400.0f, 100.0f, 150.0f, true, true));
    CHECK_FALSE(wm.processPointer(200.0f, 400.0f, 100.0f, 300.0f, true, true));
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
    REQUIRE(wm.createWidget(cm, "mem://w.hasset") != 0);

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
    const int id = wm.createWidget(cm, "mem://w.hasset");
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
    REQUIRE(faded.createWidget(cm, "mem://w.hasset") != 0);
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
    REQUIRE(gone.createWidget(cm, "mem://w.hasset") != 0);
    CHECK_FALSE(gone.processPointer(200.0f, 200.0f, 50.0f, 50.0f, true, true));

    // Disabled on the root: dimmed instead of faded, and inert all the way down.
    t.find(root)->renderOpacity = 1.0f;
    t.find(root)->enabled = false;
    registerWidget(cm, t);
    WidgetManager off;
    REQUIRE(off.createWidget(cm, "mem://w.hasset") != 0);
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
    REQUIRE(wm.createWidget(cm, "mem://w.hasset") != 0);

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
    REQUIRE(wm.createWidget(cm, "mem://w.hasset") != 0);
    // The far corner of the viewport is inside it, which for the old
    // point-anchored 120×32 default it never was.
    CHECK(wm.processPointer(1920.0f, 1080.0f, 1900.0f, 1050.0f, true, true));
    CHECK(wm.processPointer(1920.0f, 1080.0f,   20.0f,   20.0f, true, true));
}
