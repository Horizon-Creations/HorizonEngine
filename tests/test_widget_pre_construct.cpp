// Widget event PreConstruct (Thema 119, docs/widget-pre-construct-design.md).
//
// PreConstruct is a phase of its own: it runs for the widget AND every embedded
// widget before ANY of them sees Construct. So a Construct that talks to an
// embed meets one that has already set its own values. Both phases run inside
// createWidget, after theme/text/materials and before the first tick or frame.
#include "doctest.h"
#include <UIWidget/UIElements.h>
#include <UIWidget/UIWidgetTree.h>
#include <UIWidget/WidgetManager.h>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/HorizonWorld.h>
#include <Backends/Software/SoftwareRaster.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

using HorizonCode::NodeType;
using HorizonCode::PinType;

namespace
{
struct TempDir
{
    std::filesystem::path path;
    TempDir()
    {
        path = std::filesystem::temp_directory_path() / "he_test_widget_pre_construct";
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
};

void registerWidget(ContentManager& cm, const HE::UIWidgetTree& tree,
                    const HorizonCode::Graph* graph, const char* path)
{
    UIWidgetAsset a;
    a.treeJson = HE::uiWidgetTreeToJson(tree);
    if (graph) a.graphJson = HorizonCode::toJson(*graph);
    a.path = path;
    cm.registerWidget(std::move(a));
}

void addVar(HorizonCode::Graph& g, const char* name, PinType type, float def = 0.0f)
{
    HorizonCode::Variable v; v.name = name; v.type = type; v.f[0] = def;
    g.variables.push_back(v);
}

int addEvent(HorizonCode::Graph& g, const char* name)
{
    HorizonCode::Node e; e.type = NodeType::Event; e.s = name;
    return g.addNode(e);
}

// Set Variable `var` (Int) = `value`, run from exec pin 0 of `from`. Returns
// the Set node's id — the exec node the trace below listens for.
int setIntAfter(HorizonCode::Graph& g, int from, const char* var, int value)
{
    HorizonCode::Node lit; lit.type = NodeType::ConstInt; lit.f[0] = (float)value;
    const int litId = g.addNode(lit);
    HorizonCode::Node set; set.type = NodeType::SetVariable; set.s = var;
    set.propType = PinType::Int;
    const int setId = g.addNode(set);
    REQUIRE(g.connect(from, 0, setId, 0));    // exec
    REQUIRE(g.connect(litId, 0, setId, 2));   // [exec in][exec out][Value]
    return setId;
}
int onEventSetInt(HorizonCode::Graph& g, const char* ev, const char* var, int value)
{
    return setIntAfter(g, addEvent(g, ev), var, value);
}

// The order in which the labelled Set nodes ran. The exec listener sees every
// exec node an interpreted instance runs, with its class key (the widget's
// asset path), which tells host from embed without help from the graphs.
struct Trace
{
    std::map<std::pair<std::string, int>, std::string> labels;
    std::string seen;
    void label(const std::string& key, int node, const std::string& what)
    { labels[{ key, node }] = what; }
    void hook(HorizonCode::Runtime& rt)
    {
        rt.setExecListener([this](HorizonCode::InstanceId, const std::string& key,
                                  size_t, int node)
        {
            const auto it = labels.find({ key, node });
            if (it == labels.end()) return;
            if (!seen.empty()) seen += ' ';
            seen += it->second;
        });
    }
};

// A page with one WidgetRef slot "Card" that embeds `cardPath`.
HE::UIWidgetTree pageWithCard(const char* cardPath)
{
    HE::UIWidgetTree page;
    page.canvasWidth = 400.0f; page.canvasHeight = 300.0f;
    page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int slot = page.add(HE::UIWidgetType::WidgetRef);
    auto* r = dynamic_cast<HE::UIWidgetRef*>(page.find(slot));
    r->widgetPath = cardPath;
    r->name = "Card";
    HE::uiSetAnchorPreset(*r, 0); r->pivotX = r->pivotY = 0.0f;
    r->posX = 0.0f; r->posY = 0.0f; r->sizeX = 200.0f; r->sizeY = 100.0f;
    return page;
}

HE::UIWidgetTree plainCard()
{
    HE::UIWidgetTree card;
    card.canvasWidth = 200.0f; card.canvasHeight = 100.0f;
    card.add(HE::UIWidgetType::Panel);
    return card;
}

void dumpPpm(const HE::sw::Image& img, const char* name)
{
    const char* dir = std::getenv("HE_UI_DUMP_DIR");
    if (!dir || !*dir) return;
    if (FILE* f = std::fopen((std::string(dir) + "/" + name).c_str(), "wb"))
    {
        std::fprintf(f, "P6\n%d %d\n255\n", img.width, img.height);
        for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4)
            std::fwrite(&img.rgba[i], 1, 3, f);
        std::fclose(f);
    }
}
} // namespace

TEST_CASE("PreConstruct: an engine event with its own hook, offered to widgets only")
{
    const HorizonCode::EngineEventDesc* d = HorizonCode::findEngineEvent("PreConstruct");
    REQUIRE(d != nullptr);
    CHECK(std::string(d->hook) == "onPreConstruct");
    CHECK(d->arg == PinType::Exec);
    CHECK_FALSE(d->elem);
    // Not in the class taxonomy: only widgets fire it, so the class editor must
    // not offer it to entities and HC classes where it would never run.
    for (const HorizonCode::EngineClassDesc& c : HorizonCode::engineClasses())
        for (const char* ev : c.events)
            CHECK(std::string(ev) != "PreConstruct");

    // An engine event is never inferred as a custom one.
    HorizonCode::Graph g;
    addEvent(g, "PreConstruct");
    HorizonCode::inferEventDecls(g);
    CHECK(g.findEvent("PreConstruct") == nullptr);
}

TEST_CASE("PreConstruct: the whole family runs it before any Construct")
{
    TempDir dir;
    ContentManager cm(dir.path.string());
    Trace trace;

    HorizonCode::Graph cardG;
    addVar(cardG, "ready", PinType::Int);
    addVar(cardG, "built", PinType::Int);
    trace.label("mem://card.hasset", onEventSetInt(cardG, "PreConstruct", "ready", 7), "P(embed)");
    trace.label("mem://card.hasset", onEventSetInt(cardG, "Construct", "built", 1), "C(embed)");
    registerWidget(cm, plainCard(), &cardG, "mem://card.hasset");

    HorizonCode::Graph pageG;
    addVar(pageG, "pre", PinType::Int);
    addVar(pageG, "built", PinType::Int);
    trace.label("mem://page.hasset", onEventSetInt(pageG, "PreConstruct", "pre", 1), "P(host)");
    trace.label("mem://page.hasset", onEventSetInt(pageG, "Construct", "built", 1), "C(host)");
    registerWidget(cm, pageWithCard("mem://card.hasset"), &pageG, "mem://page.hasset");

    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);
    trace.hook(rt);

    const int id = wm.createWidget(cm, "mem://page.hasset");
    REQUIRE(id != 0);
    // Two phases, host first in each, as Construct always was. Without the
    // phase this would read "C(host) C(embed)".
    CHECK(trace.seen == "P(host) P(embed) C(host) C(embed)");

    const HorizonCode::InstanceId card = wm.childInstance(id, "Card");
    REQUIRE(card != 0);
    CHECK(rt.getVariable(card, "ready").i == 7);
    CHECK(rt.getVariable(card, "built").i == 1);
    CHECK(rt.getVariable((HorizonCode::InstanceId)id, "pre").i == 1);
    CHECK(rt.getVariable((HorizonCode::InstanceId)id, "built").i == 1);
}

// The reason the phase exists: the page's Construct reads what the card set in
// its own PreConstruct. Get Child Widget(self, "Card") → Get Variable (Ref)
// "label" → the page's own "seen" — what a page filling itself from its parts
// does. Before PreConstruct the card would still be at its default here.
TEST_CASE("PreConstruct: the page's Construct sees what its embed set before it")
{
    TempDir dir;
    ContentManager cm(dir.path.string());

    HorizonCode::Graph cardG;
    {
        HorizonCode::Variable v; v.name = "label"; v.type = PinType::String;
        v.s = "default";
        cardG.variables.push_back(v);   // access 0 = public, readable by Ref
        const int ev = addEvent(cardG, "PreConstruct");
        HorizonCode::Node lit; lit.type = NodeType::ConstString; lit.s = "loaded";
        const int litId = cardG.addNode(lit);
        HorizonCode::Node set; set.type = NodeType::SetVariable; set.s = "label";
        set.propType = PinType::String;
        const int setId = cardG.addNode(set);
        REQUIRE(cardG.connect(ev, 0, setId, 0));
        REQUIRE(cardG.connect(litId, 0, setId, 2));
    }
    registerWidget(cm, plainCard(), &cardG, "mem://card.hasset");

    HorizonCode::Graph pageG;
    {
        HorizonCode::Variable v; v.name = "seen"; v.type = PinType::String;
        pageG.variables.push_back(v);
        const int ev = addEvent(pageG, "Construct");

        const HE::api::ApiFn* fn = HE::api::find("widget.childRef");
        REQUIRE(fn != nullptr);
        REQUIRE_FALSE(fn->isExec);   // pure: data pins only, params first
        HorizonCode::Node call; call.type = NodeType::EngineCall; call.s = fn->id;
        call.hasArg = fn->isExec;
        for (const auto& p : fn->params)  call.params.push_back({ p.name, p.type, p.isArray });
        for (const auto& r : fn->results) call.results.push_back({ r.name, r.type, r.isArray });
        call.pinDefaults[1] = HorizonCode::Value::ofString("Card");
        const int callId = pageG.addNode(std::move(call));
        HorizonCode::Node self; self.type = NodeType::GetSelf;
        const int selfId = pageG.addNode(self);
        REQUIRE(pageG.connect(selfId, 0, callId, 0));   // widget

        HorizonCode::Node get; get.type = NodeType::GetExternal; get.s = "label";
        get.propType = PinType::String;
        const int getId = pageG.addNode(get);
        REQUIRE(pageG.connect(callId, 2, getId, 0));    // [widget][element][child] → Target

        HorizonCode::Node set; set.type = NodeType::SetVariable; set.s = "seen";
        set.propType = PinType::String;
        const int setId = pageG.addNode(set);
        REQUIRE(pageG.connect(ev, 0, setId, 0));
        REQUIRE(pageG.connect(getId, 1, setId, 2));     // [Target][Value] → Value
    }
    registerWidget(cm, pageWithCard("mem://card.hasset"), &pageG, "mem://page.hasset");

    HorizonCode::Runtime rt;
    WidgetManager wm;
    // The engine-API row the app would dispatch to, against this manager. The
    // world hands the manager ITS runtime, so it is given ours first — the
    // order the app injects them in.
    HorizonWorld world;
    world.setScriptRuntime(&rt);
    world.setWidgetManager(&wm);
    HE::api::Ctx c;
    c.world = &world;
    c.content = &cm;
    {
        HorizonCode::Runtime::Services svc;
        svc.callApi = [&](HorizonCode::InstanceId, const std::string& apiId,
                          const std::vector<HorizonCode::Value>& args)
            -> std::vector<HorizonCode::Value>
        {
            if (apiId != "widget.childRef" || args.size() < 2) return {};
            return { HorizonCode::Value::ofRef(
                HE::api::widget::childRef(c, (int)args[0].ref, args[1].s)) };
        };
        rt.setServices(std::move(svc));
    }

    const int id = wm.createWidget(cm, "mem://page.hasset");
    REQUIRE(id != 0);
    const HorizonCode::InstanceId card = wm.childInstance(id, "Card");
    REQUIRE(card != 0);
    CHECK(rt.getVariable(card, "label").s == "loaded");
    // Read in the page's Construct, i.e. after the card's PreConstruct.
    CHECK(rt.getVariable((HorizonCode::InstanceId)id, "seen").s == "loaded");
}

TEST_CASE("PreConstruct: runs at creation, before the first tick and the first frame")
{
    TempDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 200.0f; t.canvasHeight = 100.0f;
    t.add(HE::UIWidgetType::Panel);
    HorizonCode::Graph g;
    addVar(g, "pre", PinType::Int);
    addVar(g, "ticked", PinType::Int);
    onEventSetInt(g, "PreConstruct", "pre", 1);
    {
        HorizonCode::Node tick; tick.type = NodeType::Event; tick.s = "Tick";
        tick.hasArg = true; tick.propType = PinType::Float;
        setIntAfter(g, g.addNode(tick), "ticked", 1);
    }
    registerWidget(cm, t, &g, "mem://w.hasset");

    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);
    const int id = wm.createWidget(cm, "mem://w.hasset");
    REQUIRE(id != 0);
    const auto inst = (HorizonCode::InstanceId)id;
    CHECK(rt.getVariable(inst, "pre").i == 1);
    CHECK(rt.getVariable(inst, "ticked").i == 0);

    // Created hidden: a tick does not reach it yet…
    wm.tick(0.016f);
    CHECK(rt.getVariable(inst, "ticked").i == 0);
    // …shown, it does — and PreConstruct is not run a second time by showing.
    rt.setVariable(inst, "pre", HorizonCode::Value::ofInt(0));
    wm.showWidget(id);
    wm.tick(0.016f);
    CHECK(rt.getVariable(inst, "ticked").i == 1);
    CHECK(rt.getVariable(inst, "pre").i == 0);
}

// A widget may destroy itself in PreConstruct (Destroy Widget(Get Self)). Its
// Construct then finds no instance and does nothing, and neither does the
// embed's, which went with it. Nothing touches freed memory on the way.
TEST_CASE("PreConstruct: a widget that destroys itself skips Construct")
{
    TempDir dir;
    ContentManager cm(dir.path.string());
    Trace trace;

    HorizonCode::Graph cardG;
    addVar(cardG, "x", PinType::Int);
    trace.label("mem://card.hasset", onEventSetInt(cardG, "PreConstruct", "x", 1), "P(embed)");
    trace.label("mem://card.hasset", onEventSetInt(cardG, "Construct", "x", 2), "C(embed)");
    registerWidget(cm, plainCard(), &cardG, "mem://card.hasset");

    HorizonCode::Graph pageG;
    addVar(pageG, "x", PinType::Int);
    {
        const int ev = addEvent(pageG, "PreConstruct");
        HorizonCode::Node self; self.type = NodeType::GetSelf;
        const int selfId = pageG.addNode(self);
        HorizonCode::Node kill; kill.type = NodeType::DestroyWidget;
        const int killId = pageG.addNode(kill);
        REQUIRE(pageG.connect(ev, 0, killId, 0));
        REQUIRE(pageG.connect(selfId, 0, killId, 2));
        trace.label("mem://page.hasset", killId, "P(host)");
    }
    trace.label("mem://page.hasset", onEventSetInt(pageG, "Construct", "x", 2), "C(host)");
    registerWidget(cm, pageWithCard("mem://card.hasset"), &pageG, "mem://page.hasset");

    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);
    {
        HorizonCode::Runtime::Services svc;
        svc.destroyWidget = [&wm](int w) { wm.destroyWidget(w); };
        rt.setServices(std::move(svc));
    }
    trace.hook(rt);

    const int id = wm.createWidget(cm, "mem://page.hasset");
    CHECK(id != 0);
    CHECK_FALSE(wm.isAlive(id));
    CHECK(wm.count() == 0);
    CHECK(trace.seen == "P(host)");
}

// A row grafted in at run time (Add Child, and every list row) gets both phases
// too, PreConstruct first.
TEST_CASE("PreConstruct: a row added at run time runs it before its Construct")
{
    TempDir dir;
    ContentManager cm(dir.path.string());
    Trace trace;

    HE::UIWidgetTree row;
    row.canvasWidth = 400.0f; row.canvasHeight = 40.0f;
    row.add(HE::UIWidgetType::Text);
    HorizonCode::Graph rowG;
    addVar(rowG, "x", PinType::Int);
    trace.label("mem://row.hasset", onEventSetInt(rowG, "PreConstruct", "x", 1), "P(row)");
    trace.label("mem://row.hasset", onEventSetInt(rowG, "Construct", "x", 2), "C(row)");
    registerWidget(cm, row, &rowG, "mem://row.hasset");

    HE::UIWidgetTree page;
    page.canvasWidth = 400.0f; page.canvasHeight = 600.0f;
    const int box = page.add(HE::UIWidgetType::VerticalBox);
    page.find(box)->name = "List";
    registerWidget(cm, page, nullptr, "mem://page.hasset");

    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);
    trace.hook(rt);
    const int id = wm.createWidget(cm, "mem://page.hasset");
    REQUIRE(id != 0);
    CHECK(trace.seen.empty());

    const HorizonCode::InstanceId child = wm.addChild(cm, id, "List", "mem://row.hasset");
    REQUIRE(child != 0);
    CHECK(trace.seen == "P(row) C(row)");
    CHECK(rt.getVariable(child, "x").i == 2);
}

// The example widget of the step: a status card that sets its own look and
// loads its own data in PreConstruct. The colour comes from a Set Property,
// the text from a variable that stands for loaded data (score → "Score: 42").
// Both must be in the FIRST picture the widget produces — nothing designed
// may flash first — and survive the theme/text pass that ran before.
TEST_CASE("PreConstruct: example widget, values set before the first frame")
{
    TempDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 200.0f; t.canvasHeight = 100.0f;
    t.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int panel = t.add(HE::UIWidgetType::Panel);
    {
        HE::UIElement& e = *t.find(panel);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 100.0f;
        e.setProp("Color", HE::UIPropValue::ofColor({ 0.5f, 0.5f, 0.5f, 1.0f }));   // designed grey
    }
    const int label = t.add(HE::UIWidgetType::Text);
    {
        HE::UIElement& e = *t.find(label);
        e.parentId = panel;
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 16.0f; e.posY = 30.0f; e.sizeX = 170.0f; e.sizeY = 40.0f;
        e.setProp("Text", HE::UIPropValue::ofString("designed"));
    }

    HorizonCode::Graph g;
    addVar(g, "score", PinType::Int, 42.0f);   // the "loaded" data
    {
        const int ev = addEvent(g, "PreConstruct");
        // Set Property Color = green on the panel.
        HorizonCode::Node col; col.type = NodeType::ConstColor;
        col.f[0] = 0.1f; col.f[1] = 0.8f; col.f[2] = 0.2f; col.f[3] = 1.0f;
        const int colId = g.addNode(col);
        HorizonCode::Node setC; setC.type = NodeType::SetProperty; setC.elem = panel;
        setC.s = "Color"; setC.propType = PinType::Color;
        const int setCId = g.addNode(setC);
        REQUIRE(g.connect(ev, 0, setCId, 0));
        REQUIRE(g.connect(colId, 0, setCId, 2));
        // Set Property Text = "Score: " + ToString(score).
        HorizonCode::Node gv; gv.type = NodeType::GetVariable; gv.s = "score";
        gv.propType = PinType::Int;
        const int gvId = g.addNode(gv);
        HorizonCode::Node ts; ts.type = NodeType::ToString;
        const int tsId = g.addNode(ts);
        HorizonCode::Node pre; pre.type = NodeType::ConstString; pre.s = "Score: ";
        const int preId = g.addNode(pre);
        HorizonCode::Node cat; cat.type = NodeType::Concat;
        const int catId = g.addNode(cat);
        REQUIRE(g.connect(gvId, 0, tsId, 0));
        REQUIRE(g.connect(preId, 0, catId, 0));
        REQUIRE(g.connect(tsId, 1, catId, 1));
        HorizonCode::Node setT; setT.type = NodeType::SetProperty; setT.elem = label;
        setT.s = "Text"; setT.propType = PinType::String;
        const int setTId = g.addNode(setT);
        REQUIRE(g.connect(setCId, 1, setTId, 0));
        REQUIRE(g.connect(catId, 2, setTId, 2));
    }
    registerWidget(cm, t, &g, "mem://status.hasset");

    WidgetManager wm;
    const int id = wm.createWidget(cm, "mem://status.hasset");
    REQUIRE(id != 0);
    // The model, before anything was drawn.
    CHECK(wm.tree(id)->find(label)->getProp("Text").s == "Score: 42");
    CHECK(wm.tree(id)->find(panel)->getProp("Color").col.g == doctest::Approx(0.8f));

    // The first frame.
    wm.showWidget(id);
    std::vector<UIRenderObject> out;
    wm.extract(200.0f, 100.0f, out);
    HE::sw::Image img;
    img.resize(200, 100);
    img.clear(0, 0, 0, 255);
    HE::sw::draw(img, out);
    int green = 0, grey = 0;
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 200; ++x)
        {
            std::uint8_t r, gg, b, a;
            img.pixel(x, y, r, gg, b, a);
            if (gg > 150 && r < 80 && b < 100) ++green;
            if (r > 100 && r < 160 && gg > 100 && gg < 160 && b > 100 && b < 160) ++grey;
        }
    INFO("green " << green << ", grey " << grey);
    CHECK(green > 10000);   // the 200x100 panel, minus the text on it
    CHECK(grey == 0);       // the designed colour never reached the screen
    dumpPpm(img, "pre_construct_status.ppm");
}
