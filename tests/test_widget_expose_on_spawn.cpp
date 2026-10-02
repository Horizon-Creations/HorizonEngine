// Expose on Spawn for Create Widget (Thema 119, docs/widget-pre-construct-design.md §6).
//
// A widget ticks variables "Expose on Spawn"; every Create Widget of it grows an
// input per such variable. A wired input, or one with a value on the node, is
// set on the new widget after its variables are seeded and BEFORE its
// PreConstruct — so the widget's own first code already sees the creator's
// value, as in UMG.
#include "doctest.h"
#include "HcGraphHost.h"
#include <UIWidget/UIElements.h>
#include <UIWidget/UIWidgetTree.h>
#include <UIWidget/WidgetManager.h>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using HorizonCode::NodeType;
using HorizonCode::PinType;
using HorizonCode::Value;

namespace
{
struct TempDir
{
    std::filesystem::path path;
    TempDir()
    {
        path = std::filesystem::temp_directory_path() / "he_test_widget_expose_on_spawn";
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
};

void registerWidget(ContentManager& cm, const HorizonCode::Graph& graph, const char* path)
{
    HE::UIWidgetTree tree;
    tree.canvasWidth = 200.0f; tree.canvasHeight = 100.0f;
    tree.add(HE::UIWidgetType::Panel);
    UIWidgetAsset a;
    a.treeJson  = HE::uiWidgetTreeToJson(tree);
    a.graphJson = HorizonCode::toJson(graph);
    a.path = path;
    cm.registerWidget(std::move(a));
}

HorizonCode::Variable intVar(const char* name, int def, bool spawn, int access = 0)
{
    HorizonCode::Variable v; v.name = name; v.type = PinType::Int; v.f[0] = (float)def;
    v.exposeOnSpawn = spawn; v.access = access;
    return v;
}
HorizonCode::Variable stringVar(const char* name, const char* def, bool spawn)
{
    HorizonCode::Variable v; v.name = name; v.type = PinType::String; v.s = def;
    v.exposeOnSpawn = spawn;
    return v;
}

// On `event`: Set `dst` (Int) = Get `src` (Int).
void copyIntOn(HorizonCode::Graph& g, const char* event, const char* src, const char* dst)
{
    HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = event;
    const int evId = g.addNode(ev);
    HorizonCode::Node get; get.type = NodeType::GetVariable; get.s = src; get.propType = PinType::Int;
    const int getId = g.addNode(get);
    HorizonCode::Node set; set.type = NodeType::SetVariable; set.s = dst; set.propType = PinType::Int;
    const int setId = g.addNode(set);
    REQUIRE(g.connect(evId, 0, setId, 0));    // exec
    REQUIRE(g.connect(getId, 0, setId, 2));   // [exec in][exec out][Value]
}

// The widget every case creates: `score` and `title` are offered to the
// creator, `secret` is private, `hidden` public but not ticked. PreConstruct
// and Construct each record what `score` was when they ran.
HorizonCode::Graph scoreWidgetGraph()
{
    HorizonCode::Graph g;
    g.variables.push_back(intVar("score", 42, true));
    g.variables.push_back(stringVar("title", "Untitled", true));
    g.variables.push_back(intVar("secret", 0, true, /*access=*/1));
    g.variables.push_back(intVar("hidden", 3, false));
    g.variables.push_back(intVar("seenInPre", 0, false));
    g.variables.push_back(intVar("seenInConstruct", 0, false));
    copyIntOn(g, "PreConstruct", "score", "seenInPre");
    copyIntOn(g, "Construct", "score", "seenInConstruct");
    return g;
}

const char* kScore = "mem://score.hasset";
} // namespace

TEST_CASE("Expose on Spawn: the creator's value is there before PreConstruct")
{
    TempDir dir;
    ContentManager cm(dir.path.string());
    registerWidget(cm, scoreWidgetGraph(), kScore);

    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);

    const HorizonCode::SpawnValues spawn = { { "score", Value::ofInt(99) } };
    const int id = wm.createWidget(cm, kScore, &spawn);
    REQUIRE(id != 0);
    const auto w = (HorizonCode::InstanceId)id;
    // The point of the order (§6.2): the widget's PreConstruct — the code that
    // writes "Score: " + score into the first picture — already sees 99. Set
    // between PreConstruct and Construct, this would read 42.
    CHECK(rt.getVariable(w, "seenInPre").i == 99);
    CHECK(rt.getVariable(w, "seenInConstruct").i == 99);
    CHECK(rt.getVariable(w, "score").i == 99);
    // What was not handed in keeps the widget's own default.
    CHECK(rt.getVariable(w, "title").s == "Untitled");

    // No values at all (Lua, the registry row, every caller from before): the
    // defaults, exactly as before this existed.
    const int plain = wm.createWidget(cm, kScore);
    REQUIRE(plain != 0);
    CHECK(rt.getVariable((HorizonCode::InstanceId)plain, "seenInPre").i == 42);
}

TEST_CASE("Expose on Spawn: a private or unknown name is skipped, the widget still comes up")
{
    TempDir dir;
    ContentManager cm(dir.path.string());
    registerWidget(cm, scoreWidgetGraph(), kScore);

    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);

    // The caller's pins are a mirror from when it was last opened: the widget
    // may have made one private or renamed it since. Set (Ref)'s rule decides.
    const HorizonCode::SpawnValues spawn = {
        { "secret", Value::ofInt(5) },
        { "renamedAway", Value::ofInt(1) },
        { "score", Value::ofInt(7) },
    };
    const int id = wm.createWidget(cm, kScore, &spawn);
    REQUIRE(id != 0);
    const auto w = (HorizonCode::InstanceId)id;
    CHECK(rt.getVariable(w, "secret").i == 0);
    CHECK(rt.variablesOf(w).count("renamedAway") == 0);
    CHECK(rt.getVariable(w, "score").i == 7);          // the good one still lands
    CHECK(rt.getVariable(w, "seenInPre").i == 7);

    // The same rule from the runtime directly, which Set (Ref) now shares.
    CHECK_FALSE(rt.setPublicVariable(w, "secret", Value::ofInt(1)));
    CHECK_FALSE(rt.setPublicVariable(w, "nope", Value::ofInt(1)));
    CHECK_FALSE(rt.setPublicVariable(0, "score", Value::ofInt(1)));
    CHECK(rt.setPublicVariable(w, "hidden", Value::ofInt(9)));   // public is enough
    CHECK(rt.getVariable(w, "hidden").i == 9);
}

TEST_CASE("Expose on Spawn: only ticked public instance variables become pins, in order")
{
    HorizonCode::Graph g = scoreWidgetGraph();
    HorizonCode::Variable local = intVar("local", 0, true);
    local.scope = 77;   // function-local: nobody outside reaches it
    g.variables.push_back(local);

    const std::vector<HorizonCode::SpawnPin> pins = HorizonCode::spawnPinsOf(g);
    REQUIRE(pins.size() == 2);
    CHECK(pins[0].param.name == "score");
    CHECK(pins[0].param.type == PinType::Int);
    CHECK(pins[0].def.i == 42);
    CHECK(pins[1].param.name == "title");
    CHECK(pins[1].param.type == PinType::String);
    CHECK(pins[1].def.s == "Untitled");

    // The flag rounds through JSON, and is dropped where it cannot apply.
    HorizonCode::Graph back;
    REQUIRE(HorizonCode::fromJson(HorizonCode::toJson(g), back));
    CHECK(back.findVariable("score")->exposeOnSpawn);
    CHECK_FALSE(back.findVariable("hidden")->exposeOnSpawn);
    CHECK_FALSE(back.findVariable("secret")->exposeOnSpawn);   // private
    CHECK_FALSE(back.findVariable("local")->exposeOnSpawn);    // local
    // Written only when set: a graph that ticks nothing saves as before.
    HorizonCode::Graph none;
    none.variables.push_back(intVar("x", 0, false));
    CHECK(HorizonCode::toJson(none).find("\"spawn\"") == std::string::npos);
}

TEST_CASE("Expose on Spawn: Create Widget pins are mirrored, wires and values follow by name")
{
    HorizonCode::Graph g;
    HorizonCode::Node cw; cw.type = NodeType::CreateWidget; cw.s = kScore;
    const int create = g.addNode(cw);
    HorizonCode::Node set; set.type = NodeType::SetVariable; set.s = "w"; set.propType = PinType::Ref;
    const int setId = g.addNode(set);

    // A node from before the pins: Widget is data-out pin 2, and an empty
    // mirror changes nothing (no migration needed, unlike Create Object).
    REQUIRE(g.connect(create, 2, setId, 2));
    CHECK_FALSE(HorizonCode::syncSpawnPins(g, create, {}));
    CHECK(g.links.back().srcPin == 2);

    HorizonCode::SpawnPin a; a.param.name = "a"; a.param.type = PinType::Int;    a.def = Value::ofInt(1);
    HorizonCode::SpawnPin b; b.param.name = "b"; b.param.type = PinType::String; b.def = Value::ofString("x");
    HorizonCode::SpawnPin c; c.param.name = "c"; c.param.type = PinType::Color;
    c.def = Value::ofColor({ 1, 0, 0, 1 });

    // Three pins appear, the output moves behind them and its wire with it.
    REQUIRE(HorizonCode::syncSpawnPins(g, create, { a, b, c }));
    const HorizonCode::Node* n = g.findNode(create);
    REQUIRE(n->params.size() == 3);
    const auto linkFrom = [&](int node, int pin) {
        for (const auto& l : g.links) if (l.srcNode == node && l.srcPin == pin) return true;
        return false;
    };
    const auto linkTo = [&](int node, int pin) {
        for (const auto& l : g.links) if (l.dstNode == node && l.dstPin == pin) return true;
        return false;
    };
    CHECK(linkFrom(create, 5));          // [exec in][exec out][a][b][c][Widget]
    // Pre-filled with the widget's defaults where a pin has an inline field;
    // the Color has none and stays empty (it only ever acts wired).
    CHECK(n->pinDefaults.at(0).i == 1);
    CHECK(n->pinDefaults.at(1).s == "x");
    CHECK(n->pinDefaults.count(2) == 0);
    // Mirroring again with the same answer is not a change.
    CHECK_FALSE(HorizonCode::syncSpawnPins(g, create, { a, b, c }));

    // Wire `a`, type a value on `b`.
    HorizonCode::Node lit; lit.type = NodeType::ConstInt; lit.f[0] = 5.0f;
    const int litId = g.addNode(lit);
    REQUIRE(g.connect(litId, 0, create, 2));
    g.findNode(create)->pinDefaults[1] = Value::ofString("typed");

    // The widget reorders them: the wire and the typed value follow the NAME.
    REQUIRE(HorizonCode::syncSpawnPins(g, create, { b, a, c }));
    n = g.findNode(create);
    CHECK(n->params[0].name == "b");
    CHECK(linkTo(create, 3));            // a is data-in 1 now
    CHECK_FALSE(linkTo(create, 2));
    CHECK(n->pinDefaults.at(0).s == "typed");
    CHECK(linkFrom(create, 5));

    // `a` is gone: its wire goes with it (visibly, not onto a neighbour).
    REQUIRE(HorizonCode::syncSpawnPins(g, create, { b, c }));
    n = g.findNode(create);
    CHECK_FALSE(linkTo(create, 2));
    CHECK_FALSE(linkTo(create, 3));
    CHECK(n->pinDefaults.at(0).s == "typed");
    CHECK(linkFrom(create, 4));          // [b][c][Widget]

    // A changed default: a pin still at the OLD default follows it, the one
    // somebody typed into does not.
    HorizonCode::SpawnPin d; d.param.name = "d"; d.param.type = PinType::Int; d.def = Value::ofInt(10);
    REQUIRE(HorizonCode::syncSpawnPins(g, create, { b, c, d }));
    CHECK(g.findNode(create)->pinDefaults.at(2).i == 10);
    HorizonCode::SpawnPin b2 = b; b2.def = Value::ofString("y");
    HorizonCode::SpawnPin d2 = d; d2.def = Value::ofInt(20);
    const std::vector<HorizonCode::SpawnPin> before = { b, c, d };
    REQUIRE(HorizonCode::syncSpawnPins(g, create, { b2, c, d2 }, &before));
    n = g.findNode(create);
    CHECK(n->pinDefaults.at(0).s == "typed");   // chosen here — kept
    CHECK(n->pinDefaults.at(2).i == 20);        // never touched — follows

    // A retyped variable starts over at its new default.
    HorizonCode::SpawnPin d3 = d2; d3.param.type = PinType::String; d3.def = Value::ofString("s");
    REQUIRE(HorizonCode::syncSpawnPins(g, create, { b2, c, d3 }));
    CHECK(g.findNode(create)->pinDefaults.at(2).s == "s");

    // The node (pins and values) survives a save.
    HorizonCode::Graph back;
    REQUIRE(HorizonCode::fromJson(HorizonCode::toJson(g), back));
    const HorizonCode::Node* bn = back.findNode(create);
    REQUIRE(bn != nullptr);
    REQUIRE(bn->params.size() == 3);
    CHECK(bn->params[2].name == "d");
    CHECK(bn->pinDefaults.at(0).s == "typed");
}

// The whole path a graph takes: a creator widget's Construct runs Create
// Widget with the pins mirrored by the editor helper, through the runtime's
// services, into WidgetManager::createWidget.
TEST_CASE("Expose on Spawn: a Create Widget node hands wired and typed pins to the new widget")
{
    TempDir dir;
    ContentManager cm(dir.path.string());
    HorizonCode::Graph scoreG = scoreWidgetGraph();
    {
        HorizonCode::Variable tint; tint.name = "tint"; tint.type = PinType::Color;
        tint.exposeOnSpawn = true;
        tint.f[0] = 0.25f; tint.f[3] = 1.0f;
        scoreG.variables.push_back(tint);
    }
    registerWidget(cm, scoreG, kScore);

    HorizonCode::Graph creator;
    {
        HorizonCode::Variable child; child.name = "child"; child.type = PinType::Ref;
        creator.variables.push_back(child);
        HorizonCode::Node ev; ev.type = NodeType::Event; ev.s = "Construct";
        const int evId = creator.addNode(ev);
        HorizonCode::Node cw; cw.type = NodeType::CreateWidget; cw.s = kScore;
        const int create = creator.addNode(cw);
        // What picking the asset in the details does: mirror its pins now.
        REQUIRE(HcGraphHost::syncCreateWidgetPins(&cm, creator, /*force=*/true));
        const HorizonCode::Node* n = creator.findNode(create);
        REQUIRE(n->params.size() == 3);   // score, title, tint — not secret/hidden
        CHECK(n->params[2].name == "tint");

        HorizonCode::Node lit; lit.type = NodeType::ConstInt; lit.f[0] = 99.0f;
        const int litId = creator.addNode(lit);
        REQUIRE(creator.connect(evId, 0, create, 0));
        REQUIRE(creator.connect(litId, 0, create, 2));     // score, wired
        creator.findNode(create)->pinDefaults[1] = Value::ofString("Level 3");   // title, typed
        // tint: no inline field, unwired — must not be handed over.
        HorizonCode::Node set; set.type = NodeType::SetVariable; set.s = "child";
        set.propType = PinType::Ref;
        const int setId = creator.addNode(set);
        REQUIRE(creator.connect(create, 1, setId, 0));
        REQUIRE(creator.connect(create, 5, setId, 2));     // [..][score][title][tint][Widget]
    }
    registerWidget(cm, creator, "mem://creator.hasset");

    HorizonCode::Runtime rt;
    WidgetManager wm;
    wm.setRuntime(&rt);
    std::vector<std::string> handed;
    {
        HorizonCode::Runtime::Services svc;
        svc.createWidget = [&](const std::string& p, const HorizonCode::SpawnValues& spawn)
        {
            for (const HorizonCode::SpawnValue& v : spawn) handed.push_back(v.name);
            return wm.createWidget(cm, p, &spawn);
        };
        rt.setServices(svc);
    }

    const int creatorId = wm.createWidget(cm, "mem://creator.hasset");
    REQUIRE(creatorId != 0);
    const uint32_t child = rt.getVariable((HorizonCode::InstanceId)creatorId, "child").ref;
    REQUIRE(child != 0);
    REQUIRE(handed.size() == 2);
    CHECK(handed[0] == "score");
    CHECK(handed[1] == "title");
    CHECK(rt.getVariable(child, "seenInPre").i == 99);
    CHECK(rt.getVariable(child, "title").s == "Level 3");
    CHECK(rt.getVariable(child, "tint").col.x == doctest::Approx(0.25f));   // its own default
}

TEST_CASE("Expose on Spawn: the editor's mirror follows the live widget asset")
{
    TempDir dir;
    ContentManager cm(dir.path.string());
    // A path of its own: the mirror caches per widget path for the session.
    const char* path = "mem://follow.hasset";
    HorizonCode::Graph wg;
    wg.variables.push_back(intVar("score", 42, true));
    registerWidget(cm, wg, path);

    HorizonCode::Graph g;
    HorizonCode::Node cw; cw.type = NodeType::CreateWidget; cw.s = path;
    const int create = g.addNode(cw);
    REQUIRE(HcGraphHost::syncCreateWidgetPins(&cm, g, true));
    CHECK(g.findNode(create)->pinDefaults.at(0).i == 42);
    CHECK_FALSE(HcGraphHost::syncCreateWidgetPins(&cm, g, true));   // nothing new

    // The designer applies an edit to the live asset: a new default and a
    // second ticked variable. The untouched pin follows, the new one appears.
    wg.variables[0].f[0] = 50.0f;
    wg.variables.push_back(stringVar("title", "T", true));
    UIWidgetAsset* live = cm.getWidgetMutable(cm.loadAsset(path));
    REQUIRE(live != nullptr);
    live->graphJson = HorizonCode::toJson(wg);
    REQUIRE(HcGraphHost::syncCreateWidgetPins(&cm, g, true));
    const HorizonCode::Node* n = g.findNode(create);
    REQUIRE(n->params.size() == 2);
    CHECK(n->pinDefaults.at(0).i == 50);
    CHECK(n->pinDefaults.at(1).s == "T");

    // A widget that cannot be found leaves the node as it is.
    g.findNode(create)->s = "mem://missing.hasset";
    CHECK_FALSE(HcGraphHost::syncCreateWidgetPins(&cm, g, true));
    CHECK(g.findNode(create)->params.size() == 2);
}
