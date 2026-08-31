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
#include <HorizonScene/EngineApi.h>
#include <algorithm>
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
            { "Text Color", UIPropType::Color },
            { "Selection Color", UIPropType::Color },
            { "Max Length", UIPropType::Int },
            { "Password", UIPropType::Bool },
            { "Editable", UIPropType::Bool },
            { "Selectable", UIPropType::Bool },
            { "Input Filter", UIPropType::Int },
            { "Allowed Characters", UIPropType::String } } },
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
            { "Spacing", UIPropType::Float },
            { "Size To Content", UIPropType::Bool },
            { "Min Width", UIPropType::Float },
            { "Min Height", UIPropType::Float } } },
        { UIWidgetType::HorizontalBox, {
            { "Padding", UIPropType::Float },
            { "Spacing", UIPropType::Float },
            { "Size To Content", UIPropType::Bool },
            { "Min Width", UIPropType::Float },
            { "Min Height", UIPropType::Float } } },
        { UIWidgetType::ScrollBox, {
            { "Padding", UIPropType::Float },
            { "Spacing", UIPropType::Float },
            { "Size To Content", UIPropType::Bool },
            { "Min Width", UIPropType::Float },
            { "Min Height", UIPropType::Float },
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
            { "Item Count", UIPropType::Int } } },
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
        HE::UIWidgetType::ScrollBox };
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
    CHECK(back->getPropAny("Min Width").f == doctest::Approx(120.0f));
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
    };
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
    REQUIRE(f.wm.selectWordAtPointer(400.0f, 200.0f, 8.0f));
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
    REQUIRE(f.wm.setCaretFromPointer(400.0f, 200.0f, 390.0f));
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
    // Four hairlines more than before — one per edge.
    CHECK(after.size() == before.size() + 4);

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

    // Row 15 sits at y = box bottom + 15 rows, each as tall as the box.
    clickAt(wm, 100.0f, 20.0f + 15.0f * 20.0f + 10.0f);
    {
        const auto* cb = dynamic_cast<const HE::UIComboBox*>(wm.tree(id)->find(combo));
        REQUIRE(cb);
        CHECK(cb->selectedIndex == 15);
        CHECK_FALSE(cb->open);              // picking closes it
    }

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
