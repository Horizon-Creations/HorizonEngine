// Expose on Spawn for HorizonCode classes at Create Object (Thema 138,
// docs/hc-class-expose-on-spawn-design.md).
//
// A class ticks variables "Expose on Spawn"; every Create Object of it (and of
// every class deriving from it) grows an input per such variable, BEHIND
// Location and Rotation. A wired input, or one with a value on the node, is set
// on the new instance after its variables are seeded and BEFORE its Construct
// — and for an Entity class therefore before its BeginPlay as well.
//
// The object-class host below is the apps' object branch in miniature
// (resolve, addLevels, applySpawnValues, fireConstruct); the Entity-class one
// is the real EntityHost::spawn.
#include "doctest.h"
#include "TestFsUtil.h"
#include <HorizonScene/EntityHost.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/ScriptComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <HorizonCode/HcClassResolve.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace HorizonCode;

namespace
{
struct TempDir
{
    fs::path path;
    explicit TempDir(const char* name)
    {
        path = fs::temp_directory_path() / name;
        he_test::removeAllQuiet(path);
        fs::create_directories(path);
    }
    ~TempDir() { he_test::removeAllQuiet(path); }
};

std::string writeClass(ContentManager& cm, const char* name, const char* baseClass, const Graph& g)
{
    HorizonCodeClassAsset a;
    a.type      = HE::AssetType::HorizonCodeClass;
    a.name      = name;
    a.path      = std::string(name) + ".hasset";
    a.baseClass = baseClass;
    a.graphJson = toJson(g);
    REQUIRE(cm.saveAsset(a));
    return a.path;
}

Variable intVar(const char* name, int def, bool spawn, int access = 0)
{
    Variable v; v.name = name; v.type = PinType::Int; v.f[0] = (float)def;
    v.exposeOnSpawn = spawn; v.access = access;
    return v;
}
Variable stringVar(const char* name, const char* def, bool spawn)
{
    Variable v; v.name = name; v.type = PinType::String; v.s = def;
    v.exposeOnSpawn = spawn;
    return v;
}

// On `event`: Set `dst` (Int) = Get `src` (Int).
void copyIntOn(Graph& g, const char* event, const char* src, const char* dst)
{
    Node ev; ev.type = NodeType::Event; ev.s = event;
    const int evId = g.addNode(ev);
    Node get; get.type = NodeType::GetVariable; get.s = src; get.propType = PinType::Int;
    const int getId = g.addNode(get);
    Node set; set.type = NodeType::SetVariable; set.s = dst; set.propType = PinType::Int;
    const int setId = g.addNode(set);
    REQUIRE(g.connect(evId, 0, setId, 0));    // exec
    REQUIRE(g.connect(getId, 0, setId, 2));   // [exec in][exec out][Value]
}

// The class most cases create: `score` and `title` are offered to the creator,
// `secret` is private (ticked anyway), `hidden` public but not ticked.
// Construct and BeginPlay each record what `score` was when they ran.
Graph scoreClassGraph()
{
    Graph g;
    g.variables.push_back(intVar("score", 42, true));
    g.variables.push_back(stringVar("title", "Untitled", true));
    g.variables.push_back(intVar("secret", 0, true, /*access=*/1));
    g.variables.push_back(intVar("hidden", 3, false));
    g.variables.push_back(intVar("seenInConstruct", 0, false));
    g.variables.push_back(intVar("seenInBeginPlay", 0, false));
    copyIntOn(g, "Construct", "score", "seenInConstruct");
    copyIntOn(g, "BeginPlay", "score", "seenInBeginPlay");
    return g;
}

// Event "Go" → Create Object(cls), its spawn pins mirrored from `pins` the way
// the editor does it (inline values pre-filled with the class defaults).
int createObjectCaller(Graph& g, const std::string& cls, const std::vector<SpawnPin>& pins)
{
    Node ev; ev.type = NodeType::Event; ev.s = "Go";
    const int e = g.addNode(ev);
    Node co; co.type = NodeType::CreateObject; co.s = cls;
    const int c = g.addNode(co);
    REQUIRE(g.connect(e, 0, c, 0));
    if (!pins.empty()) REQUIRE(syncSpawnPins(g, c, pins, nullptr, /*dropWiresOnRetype=*/true));
    return c;
}

// The apps' object-class branch: resolve, create, Expose on Spawn, Construct.
// `afterConstruct` is the negative control — the same values, one step late.
struct ObjectSpawner
{
    ContentManager& cm;
    Runtime&        rt;
    bool            afterConstruct = false;
    int             calls = 0;
    bool            posNull = false, rotNull = false;
    SpawnValues     handed;

    void bindTo(Runtime::Services& svc)
    {
        svc.createObject = [this](const std::string& path, const float* pos, const float* rot,
                                  const SpawnValues& spawn) -> uint32_t
        {
            ++calls;
            posNull = pos == nullptr; rotNull = rot == nullptr;
            handed  = spawn;
            ResolvedClass rc = resolveClassAsset(cm, path);
            if (rc.levels.empty()) return 0u;
            const InstanceId inst =
                rt.addLevels(std::move(rc.levels), {}, { path, rc.engineBase, rc.chain });
            if (!afterConstruct) rt.applySpawnValues(inst, spawn, path);
            rt.fireConstruct(inst);
            if (afterConstruct) rt.applySpawnValues(inst, spawn, path);
            return inst;
        };
    }
};

// The instance the caller's Create Object made: its cached Object output.
InstanceId createdBy(Runtime& rt, InstanceId callerId, const std::string& var = "made")
{
    return rt.getVariable(callerId, var).ref;
}

// Create Object's Object output → Set `made` (Ref), so the test can find the
// instance. Exec: Create Object → Set.
void keepCreated(Graph& g, int create)
{
    Variable made; made.name = "made"; made.type = PinType::Ref;
    g.variables.push_back(made);
    Node set; set.type = NodeType::SetVariable; set.s = "made"; set.propType = PinType::Ref;
    const int s = g.addNode(set);
    const Node* c = g.findNode(create);
    REQUIRE(c != nullptr);
    const int objectOut = 4 + (int)c->params.size();
    REQUIRE(g.connect(create, 1, s, 0));
    REQUIRE(g.connect(create, objectOut, s, 2));
}

std::vector<std::string> namesOf(const std::vector<SpawnPin>& pins)
{
    std::vector<std::string> out;
    for (const SpawnPin& p : pins) out.push_back(p.param.name);
    return out;
}

const Link* linkTo(const Graph& g, int dstNode, int dstPin)
{
    for (const Link& l : g.links)
        if (l.dstNode == dstNode && l.dstPin == dstPin) return &l;
    return nullptr;
}
} // namespace

// ── 1: order, object class ──────────────────────────────────────────────────
TEST_CASE("Create Object: the creator's value is there before the class's Construct")
{
    TempDir dir("he_test_class_expose_order");
    ContentManager cm(dir.path.string());
    const std::string cls = writeClass(cm, "Scorer", "Object", scoreClassGraph());
    const std::vector<SpawnPin> pins = spawnPinsOfClass(resolveClassAsset(cm, cls));
    REQUIRE(namesOf(pins) == std::vector<std::string>{ "score", "title" });

    for (const bool late : { false, true })
    {
        CAPTURE(late);
        Runtime rt;
        ObjectSpawner host{ cm, rt };
        host.afterConstruct = late;
        Runtime::Services svc;
        host.bindTo(svc);
        rt.setServices(svc);

        Graph caller;
        const int c = createObjectCaller(caller, cls, pins);
        caller.findNode(c)->pinDefaults[2] = Value::ofInt(99);   // spawn pin 0 = score
        keepCreated(caller, c);
        const InstanceId callerId = rt.add(std::move(caller));
        rt.fireEvent(callerId, "Go");

        const InstanceId inst = createdBy(rt, callerId);
        REQUIRE(inst != 0);
        CHECK(rt.getVariable(inst, "score").i == 99);
        // The point of the feature, and the negative control that proves the
        // test can see the order: set after Construct, Construct saw 42.
        CHECK(rt.getVariable(inst, "seenInConstruct").i == (late ? 42 : 99));
    }
}

// ── 2: order, Entity class through EntityHost ───────────────────────────────
TEST_CASE("Create Object of an Entity class: the value is there in Construct AND BeginPlay")
{
    TempDir dir("he_test_class_expose_entity");
    ContentManager cm(dir.path.string());
    const std::string cls = writeClass(cm, "ScoreActor", "Entity", scoreClassGraph());

    HorizonWorld world;
    // A placed entity of the same class: begin() binds it without a creator,
    // so it keeps the class's own defaults.
    const Entity placed = world.createEntity("Placed");
    world.addComponent(placed, TransformComponent{});
    {
        ScriptComponent sc;
        sc.scriptAssetId = cm.loadAsset(cls);
        world.addComponent(placed, sc);
    }

    Runtime    rt;
    EntityHost host;
    Runtime::Services svc;
    svc.createObject = [&](const std::string& p, const float* pos, const float* rot,
                           const SpawnValues& spawn) -> uint32_t
    { return host.spawn(p, entt::null, pos, rot, &spawn).instance; };
    rt.setServices(svc);
    host.begin(rt, world, cm);

    const InstanceId placedInst = host.instanceOf(placed);
    REQUIRE(placedInst != 0);
    CHECK(rt.getVariable(placedInst, "seenInConstruct").i == 42);
    CHECK(rt.getVariable(placedInst, "seenInBeginPlay").i == 42);

    Graph caller;
    const int c = createObjectCaller(caller, cls, spawnPinsOfClass(resolveClassAsset(cm, cls)));
    caller.findNode(c)->pinDefaults[2] = Value::ofInt(7);
    keepCreated(caller, c);
    const InstanceId callerId = rt.add(std::move(caller));
    rt.fireEvent(callerId, "Go");

    const InstanceId inst = createdBy(rt, callerId);
    REQUIRE(inst != 0);
    CHECK((host.entityOf(inst) != entt::null));
    CHECK(rt.getVariable(inst, "seenInConstruct").i == 7);
    CHECK(rt.getVariable(inst, "seenInBeginPlay").i == 7);

    // And a direct spawn without values (Lua, Python, Spawn Class) is as before.
    const EntityHost::Spawned bare = host.spawn(cls);
    REQUIRE(bare.instance != 0);
    CHECK(rt.getVariable(bare.instance, "seenInBeginPlay").i == 42);
}

// ── 3: which pins act ───────────────────────────────────────────────────────
TEST_CASE("Create Object: a pin acts when wired or carrying a value; placement stays wire-only")
{
    TempDir dir("he_test_class_expose_rule");
    ContentManager cm(dir.path.string());
    const std::string cls = writeClass(cm, "Scorer", "Object", scoreClassGraph());
    const std::vector<SpawnPin> pins = spawnPinsOfClass(resolveClassAsset(cm, cls));

    Runtime rt;
    ObjectSpawner host{ cm, rt };
    Runtime::Services svc;
    host.bindTo(svc);
    rt.setServices(svc);

    SUBCASE("untouched pin leaves the class default, a value on the node sets it")
    {
        Graph caller;
        const int c = createObjectCaller(caller, cls, pins);
        Node* n = caller.findNode(c);
        n->pinDefaults.erase(2);                            // score: no value, no wire
        n->pinDefaults[3] = Value::ofString("Champion");    // title: a value
        keepCreated(caller, c);
        const InstanceId callerId = rt.add(std::move(caller));
        rt.fireEvent(callerId, "Go");

        const InstanceId inst = createdBy(rt, callerId);
        REQUIRE(inst != 0);
        CHECK(rt.getVariable(inst, "score").i == 42);
        CHECK(rt.getVariable(inst, "title").s == "Champion");
        REQUIRE(host.handed.size() == 1);
        CHECK(host.handed[0].name == "title");
        // Spawn pins acting does not make the placement act: still "as authored".
        CHECK(host.posNull);
        CHECK(host.rotNull);
    }
    SUBCASE("a wired pin hands its wire's value, in the creator's context")
    {
        Graph caller;
        const int c = createObjectCaller(caller, cls, pins);
        caller.findNode(c)->pinDefaults.erase(2);
        Node k; k.type = NodeType::ConstInt; k.f[0] = 1234.0f;
        const int kId = caller.addNode(k);
        REQUIRE(caller.connect(kId, 0, c, 4));              // → score (data-in 2)
        keepCreated(caller, c);
        const InstanceId callerId = rt.add(std::move(caller));
        rt.fireEvent(callerId, "Go");

        const InstanceId inst = createdBy(rt, callerId);
        REQUIRE(inst != 0);
        CHECK(rt.getVariable(inst, "score").i == 1234);
        CHECK(rt.getVariable(inst, "seenInConstruct").i == 1234);
    }
    SUBCASE("a node without spawn pins hands nothing — the call is as before")
    {
        Graph caller;
        const int c = createObjectCaller(caller, cls, {});
        keepCreated(caller, c);
        const InstanceId callerId = rt.add(std::move(caller));
        rt.fireEvent(callerId, "Go");

        CHECK(host.calls == 1);
        CHECK(host.handed.empty());
        const InstanceId inst = createdBy(rt, callerId);
        REQUIRE(inst != 0);
        CHECK(rt.getVariable(inst, "score").i == 42);
    }
}

// ── 4: inheritance ──────────────────────────────────────────────────────────
TEST_CASE("Create Object offers the ancestors' ticked variables, root first")
{
    TempDir dir("he_test_class_expose_inherit");
    ContentManager cm(dir.path.string());

    Graph base;
    base.variables.push_back(intVar("hp", 10, true));
    base.variables.push_back(intVar("Location", 0, true));      // name taken by the fixed input
    base.variables.push_back(intVar("shadow", 1, true));
    base.variables.push_back(intVar("baseSecret", 0, true, 1)); // private
    base.variables.push_back(intVar("seenHp", 0, false));
    copyIntOn(base, "Construct", "hp", "seenHp");
    const std::string baseCls = writeClass(cm, "Unit", "Object", base);

    Graph derived;
    derived.variables.push_back(intVar("speed", 5, true));
    // Redeclared without the tick: the nearest declaration decides, and it is
    // the one setPublicVariable resolves, so no pin.
    derived.variables.push_back(intVar("shadow", 1, false));
    derived.variables.push_back(intVar("Rotation", 0, true));
    const std::string derivedCls = writeClass(cm, "Runner", baseCls.c_str(), derived);

    const std::vector<SpawnPin> pins = spawnPinsOfClass(resolveClassAsset(cm, derivedCls));
    CHECK(namesOf(pins) == std::vector<std::string>{ "hp", "speed" });
    // The base on its own offers its own, minus the reserved name.
    CHECK(namesOf(spawnPinsOfClass(resolveClassAsset(cm, baseCls)))
          == std::vector<std::string>{ "hp", "shadow" });

    Runtime rt;
    ObjectSpawner host{ cm, rt };
    Runtime::Services svc;
    host.bindTo(svc);
    rt.setServices(svc);

    Graph caller;
    const int c = createObjectCaller(caller, derivedCls, pins);
    caller.findNode(c)->pinDefaults[2] = Value::ofInt(77);   // hp, the BASE's variable
    caller.findNode(c)->pinDefaults[3] = Value::ofInt(9);    // speed
    keepCreated(caller, c);
    const InstanceId callerId = rt.add(std::move(caller));
    rt.fireEvent(callerId, "Go");

    const InstanceId inst = createdBy(rt, callerId);
    REQUIRE(inst != 0);
    CHECK(rt.getVariable(inst, "hp").i == 77);
    CHECK(rt.getVariable(inst, "speed").i == 9);
    CHECK(rt.getVariable(inst, "seenHp").i == 77);   // before the BASE's Construct too
}

// ── 5: names the class no longer offers ─────────────────────────────────────
TEST_CASE("Create Object skips a stale or private spawn value and still creates the instance")
{
    TempDir dir("he_test_class_expose_stale");
    ContentManager cm(dir.path.string());
    const std::string cls = writeClass(cm, "Scorer", "Object", scoreClassGraph());

    Runtime rt;
    ObjectSpawner host{ cm, rt };
    Runtime::Services svc;
    host.bindTo(svc);
    rt.setServices(svc);

    // The node's mirror from an older state of the class: `gone` was renamed
    // away since, `secret` made private.
    SpawnPin gone;   gone.param.name   = "gone";   gone.param.type   = PinType::Int; gone.def   = Value::ofInt(0);
    SpawnPin secret; secret.param.name = "secret"; secret.param.type = PinType::Int; secret.def = Value::ofInt(0);
    SpawnPin score;  score.param.name  = "score";  score.param.type  = PinType::Int; score.def  = Value::ofInt(42);
    Graph caller;
    const int c = createObjectCaller(caller, cls, { gone, secret, score });
    caller.findNode(c)->pinDefaults[2] = Value::ofInt(5);
    caller.findNode(c)->pinDefaults[3] = Value::ofInt(6);
    caller.findNode(c)->pinDefaults[4] = Value::ofInt(7);
    keepCreated(caller, c);
    const InstanceId callerId = rt.add(std::move(caller));
    rt.fireEvent(callerId, "Go");

    REQUIRE(host.handed.size() == 3);
    const InstanceId inst = createdBy(rt, callerId);
    REQUIRE(inst != 0);
    CHECK(rt.getVariable(inst, "score").i == 7);        // the live one still lands
    CHECK(rt.getVariable(inst, "secret").i == 0);       // private stays untouched
    CHECK(rt.variablesOf(inst).count("gone") == 0);     // and nothing is invented
}

// ── 7: the pin mirror on Create Object ──────────────────────────────────────
TEST_CASE("syncSpawnPins on Create Object: behind Location/Rotation, wires follow by name")
{
    Graph g;
    Node ev; ev.type = NodeType::Event; ev.s = "Go";
    const int e = g.addNode(ev);
    Node co; co.type = NodeType::CreateObject; co.s = "X.hasset";
    const int c = g.addNode(co);
    Node mv; mv.type = NodeType::MakeVector3;
    const int loc = g.addNode(mv);
    Node de; de.type = NodeType::DestroyObject;
    const int d = g.addNode(de);
    REQUIRE(g.connect(e, 0, c, 0));
    REQUIRE(g.connect(loc, 3, c, 2));      // → Location
    REQUIRE(g.connect(c, 4, d, 2));        // Object → Destroy Object
    // A value on a fixed input is not the mirror's to touch.
    g.findNode(c)->pinDefaults[0] = Value::ofVec3({ 1.0f, 2.0f, 3.0f });

    SpawnPin a; a.param.name = "a"; a.param.type = PinType::Int;    a.def = Value::ofInt(1);
    SpawnPin b; b.param.name = "b"; b.param.type = PinType::String; b.def = Value::ofString("x");
    SpawnPin v; v.param.name = "v"; v.param.type = PinType::Vec3;   // wired-only type

    // Old node, nothing ticked: no change, Object stays at pin 4.
    CHECK_FALSE(syncSpawnPins(g, c, {}, nullptr, true));

    REQUIRE(syncSpawnPins(g, c, { a, b, v }, nullptr, true));
    {
        const Node* n = g.findNode(c);
        REQUIRE(n->params.size() == 3);
        const NodeSig s = signatureOf(*n);
        REQUIRE(s.dataIns.size() == 5);
        CHECK(std::string(s.dataIns[0].name) == "Location");
        CHECK(std::string(s.dataIns[1].name) == "Rotation");
        CHECK(std::string(s.dataIns[2].name) == "a");
        CHECK(std::string(s.dataIns[4].name) == "v");
        // Inline values keyed by data-in index, behind the fixed inputs.
        CHECK(n->pinDefaults.count(0) == 1);                 // Location's, kept
        CHECK(n->pinDefaults.at(2).i == 1);                  // a
        CHECK(n->pinDefaults.at(3).s == "x");                // b
        CHECK(n->pinDefaults.count(4) == 0);                 // v: wired only
        // Location's wire stayed, the Object wire moved to the new output.
        CHECK(linkTo(g, c, 2) != nullptr);
        const Link* out = linkTo(g, d, 2);
        REQUIRE(out != nullptr);
        CHECK(out->srcPin == 7);
    }
    // Idempotent.
    CHECK_FALSE(syncSpawnPins(g, c, { a, b, v }, nullptr, true));

    // Wire `a`, then reorder: the wire and the typed value follow the name.
    Node k; k.type = NodeType::ConstInt; k.f[0] = 5.0f;
    const int kId = g.addNode(k);
    REQUIRE(g.connect(kId, 0, c, 4));                         // → a (data-in 2)
    g.findNode(c)->pinDefaults[3] = Value::ofString("typed");
    REQUIRE(syncSpawnPins(g, c, { b, a, v }, nullptr, true));
    {
        const Node* n = g.findNode(c);
        CHECK(n->params[0].name == "b");
        CHECK(n->pinDefaults.at(2).s == "typed");             // b's value went with b
        const Link* aw = linkTo(g, c, 5);                     // a is data-in 3 now
        REQUIRE(aw != nullptr);
        CHECK(aw->srcNode == kId);
        CHECK(linkTo(g, c, 2) != nullptr);                    // Location untouched
        CHECK(linkTo(g, d, 2)->srcPin == 7);
    }

    // Retype `a` (Int → Float): the wire of the old type is dropped, the value
    // starts at the new default.
    SpawnPin a2 = a; a2.param.type = PinType::Float; a2.def = Value::ofFloat(2.5f);
    REQUIRE(syncSpawnPins(g, c, { b, a2, v }, nullptr, true));
    CHECK(linkTo(g, c, 5) == nullptr);
    CHECK(g.findNode(c)->pinDefaults.at(3).f == doctest::Approx(2.5f));

    // Delete `b`: its pin and value go, the others close up.
    REQUIRE(syncSpawnPins(g, c, { a2, v }, nullptr, true));
    {
        const Node* n = g.findNode(c);
        REQUIRE(n->params.size() == 2);
        CHECK(n->params[0].name == "a");
        CHECK(n->pinDefaults.at(2).f == doctest::Approx(2.5f));
        CHECK(n->pinDefaults.count(3) == 0);
        CHECK(n->pinDefaults.count(0) == 1);                  // Location's, still kept
        CHECK(linkTo(g, d, 2)->srcPin == 6);
    }

    // Everything unticked: back to the old layout, Object at pin 4 again.
    REQUIRE(syncSpawnPins(g, c, {}, nullptr, true));
    CHECK(g.findNode(c)->params.empty());
    CHECK(linkTo(g, d, 2)->srcPin == 4);
    CHECK(linkTo(g, c, 2) != nullptr);
}

TEST_CASE("syncSpawnPins keeps Create Widget's wire on a retype unless asked (widget behaviour)")
{
    Graph g;
    Node cw; cw.type = NodeType::CreateWidget; cw.s = "w.hasset";
    const int c = g.addNode(cw);
    SpawnPin a; a.param.name = "a"; a.param.type = PinType::Int; a.def = Value::ofInt(1);
    REQUIRE(syncSpawnPins(g, c, { a }));
    Node k; k.type = NodeType::ConstInt; k.f[0] = 5.0f;
    const int kId = g.addNode(k);
    REQUIRE(g.connect(kId, 0, c, 2));                         // Create Widget: data-in 0 = pin 2
    SpawnPin a2 = a; a2.param.type = PinType::Float; a2.def = Value::ofFloat(0.5f);
    REQUIRE(syncSpawnPins(g, c, { a2 }));
    CHECK(linkTo(g, c, 2) != nullptr);
    CHECK(g.findNode(c)->pinDefaults.at(0).f == doctest::Approx(0.5f));
    // Other node types are not Create nodes.
    Node other; other.type = NodeType::DestroyObject;
    CHECK_FALSE(syncSpawnPins(g, g.addNode(other), { a }));
}

// ── 8: what is stored ───────────────────────────────────────────────────────
TEST_CASE("Create Object spawn pins and the class flag survive a save, old graphs load as before")
{
    SUBCASE("a class variable's tick round-trips, and only on a public instance variable")
    {
        Graph g = scoreClassGraph();
        Graph back;
        REQUIRE(fromJson(toJson(g), back));
        CHECK(back.findVariable("score")->exposeOnSpawn);
        CHECK(back.findVariable("title")->exposeOnSpawn);
        CHECK_FALSE(back.findVariable("secret")->exposeOnSpawn);
        CHECK_FALSE(back.findVariable("hidden")->exposeOnSpawn);
        // Old asset: no "spawn" key at all.
        CHECK(toJson(g).find("\"spawn\"") != std::string::npos);
        Graph plain; plain.variables.push_back(intVar("hp", 1, false));
        CHECK(toJson(plain).find("\"spawn\"") == std::string::npos);
    }
    SUBCASE("a node with spawn pins keeps its layout and its Object wire")
    {
        Graph g;
        Node co; co.type = NodeType::CreateObject; co.s = "X.hasset";
        const int c = g.addNode(co);
        Node de; de.type = NodeType::DestroyObject;
        const int d = g.addNode(de);
        SpawnPin a; a.param.name = "a"; a.param.type = PinType::Int; a.def = Value::ofInt(3);
        SpawnPin b; b.param.name = "b"; b.param.type = PinType::Bool; b.def = Value::ofBool(true);
        REQUIRE(syncSpawnPins(g, c, { a, b }, nullptr, true));
        REQUIRE(g.connect(c, 6, d, 2));                       // Object = 4 + 2

        Graph back;
        REQUIRE(fromJson(toJson(g), back));
        const Node* n = back.findNode(c);
        REQUIRE(n != nullptr);
        REQUIRE(n->params.size() == 2);
        CHECK(n->params[1].name == "b");
        CHECK(n->pinDefaults.at(2).i == 3);
        CHECK(n->pinDefaults.at(3).b == true);
        const Link* out = linkTo(back, d, 2);
        REQUIRE(out != nullptr);
        CHECK(out->srcPin == 6);                              // NOT touched by the 2 → 4 migration
    }
    SUBCASE("a graph from before the placement pins still migrates its Object wire")
    {
        const std::string oldJson = R"({
            "nodes": [
                { "id": 1, "type": "Event",          "s": "Go" },
                { "id": 2, "type": "Create Object",  "s": "MyClass" },
                { "id": 3, "type": "Destroy Object" }
            ],
            "links": [ [1, 0, 2, 0], [2, 1, 3, 0], [2, 2, 3, 2] ],
            "variables": []
        })";
        Graph g;
        REQUIRE(fromJson(oldJson, g));
        CHECK(g.findNode(2)->params.empty());
        const Link* ref = linkTo(g, 3, 2);
        REQUIRE(ref != nullptr);
        CHECK(ref->srcPin == 4);
    }
}
