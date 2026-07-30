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
    t.find(btn)->anchor = 0; t.find(btn)->pivotX = 0; t.find(btn)->pivotY = 0;
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
    t.find(cb)->anchor = 0; t.find(cb)->pivotX = 0; t.find(cb)->pivotY = 0;
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
    t.applyAutoSize();
    const float w20 = t.sizeX, h20 = t.sizeY;
    CHECK(w20 > 0.0f);
    CHECK(h20 > 0.0f);

    t.fontSize = 60.0f;
    t.applyAutoSize();
    CHECK(t.sizeX > w20);
    CHECK(t.sizeY > h20);

    // More lines → taller, same width class.
    t.text = "Hello\nWorld";
    t.applyAutoSize();
    CHECK(t.sizeY > 60.0f);

    // Off = the authored box is left alone.
    HE::UIText fixed;
    fixed.autoSize = false;
    fixed.fontSize = 80.0f;
    const float sx = fixed.sizeX, sy = fixed.sizeY;
    fixed.applyAutoSize();
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
    t.applyAutoSize();
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
    CHECK(t.find(2)->anchor == 4);
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
