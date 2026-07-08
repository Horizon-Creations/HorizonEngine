#include "doctest.h"
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonCode/HorizonCode.h>
#include <glm/glm.hpp>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

using HE::api::Ctx;
using HE::api::Value;
using P  = HorizonCode::PinType;
using NT = HorizonCode::NodeType;

namespace
{
    // Spawn an entity that carries a TransformComponent (createEntity alone does
    // not add one), so the transform api has something to read/write.
    HE::api::Entity spawnWithTransform(HorizonWorld& world, const glm::vec3& pos = glm::vec3(0.0f))
    {
        auto e = world.createEntity("ApiTest");
        TransformComponent tc;
        tc.position = pos;
        world.registry().emplace<TransformComponent>(e, tc);
        return static_cast<HE::api::Entity>(e);
    }
}

// ═══ Registry shape ═══════════════════════════════════════════════════════════

TEST_CASE("EngineApi: registry is populated and well-formed")
{
    const auto& reg = HE::api::registry();
    CHECK(reg.size() > 30);   // the promoted ScriptApi surface + the math library

    std::unordered_set<std::string> ids;
    for (const auto& fn : reg)
    {
        CHECK(fn.id != nullptr);
        CHECK(fn.category != nullptr);
        CHECK(fn.cppCall != nullptr);
        CHECK(fn.invoke != nullptr);
        CHECK(std::string(fn.id).length() > 0);
        // ids are unique
        CHECK(ids.insert(fn.id).second);
    }
}

TEST_CASE("EngineApi: find() resolves ids, rejects unknown")
{
    CHECK(HE::api::find("transform.setPosition") != nullptr);
    CHECK(HE::api::find("math.clamp") != nullptr);
    CHECK(HE::api::find("does.not.exist") == nullptr);
    CHECK(HE::api::find("") == nullptr);
}

TEST_CASE("EngineApi: side-effect classification is correct")
{
    // Getters / queries / pure math are data nodes; setters / actions are exec.
    CHECK(HE::api::find("transform.getPosition")->isExec == false);
    CHECK(HE::api::find("transform.setPosition")->isExec == true);
    CHECK(HE::api::find("physics.raycast")->isExec == false);
    CHECK(HE::api::find("math.clamp")->isExec == false);
    CHECK(HE::api::find("entity.spawn")->isExec == true);
    CHECK(HE::api::find("log")->isExec == true);

    // Signatures carry typed params + results.
    const auto* setPos = HE::api::find("transform.setPosition");
    REQUIRE(setPos->params.size() == 2);
    CHECK(setPos->params[0].type == P::Int);      // entity
    CHECK(setPos->params[1].type == P::Color);    // position (vec3 carrier)
    CHECK(setPos->results.empty());

    const auto* ray = HE::api::find("physics.raycast");
    REQUIRE(ray->results.size() == 5);
    CHECK(ray->results[0].type == P::Bool);       // hit
}

// ═══ Marshalling round-trips against a real world ═════════════════════════════

TEST_CASE("EngineApi: transform round-trips through the registry")
{
    HorizonWorld world;
    Ctx c{ &world, nullptr, nullptr };
    auto e = spawnWithTransform(world);

    // set via the registry thunk
    HE::api::find("transform.setPosition")->invoke(c,
        { Value::ofInt((int)e), Value::ofColor(glm::vec4(1.0f, 2.0f, 3.0f, 0.0f)) });

    // read back via the registry thunk
    auto out = HE::api::find("transform.getPosition")->invoke(c, { Value::ofInt((int)e) });
    REQUIRE(out.size() == 1);
    CHECK(out[0].col.x == doctest::Approx(1.0f));
    CHECK(out[0].col.y == doctest::Approx(2.0f));
    CHECK(out[0].col.z == doctest::Approx(3.0f));

    // and via the typed C++ api directly — same result
    const glm::vec3 p = HE::api::transform::getPosition(c, e);
    CHECK(p.x == doctest::Approx(1.0f));
    CHECK(p.z == doctest::Approx(3.0f));
}

TEST_CASE("EngineApi: entity spawn + distance via the api")
{
    HorizonWorld world;
    Ctx c{ &world, nullptr, nullptr };

    auto a = spawnWithTransform(world, glm::vec3(0.0f));
    auto b = spawnWithTransform(world, glm::vec3(3.0f, 4.0f, 0.0f));
    CHECK(HE::api::entity::distance(c, a, b) == doctest::Approx(5.0f));

    // spawn through the registry, then it should have a valid name
    auto out = HE::api::find("entity.spawn")->invoke(c, { Value::ofInt(0), Value::ofString("Fresh") });
    REQUIRE(out.size() == 1);
    const auto spawned = (HE::api::Entity)out[0].i;
    CHECK(HE::api::entity::getName(c, spawned) == "Fresh");
}

// ═══ Math library ═════════════════════════════════════════════════════════════

TEST_CASE("EngineApi: pure math thunks compute correctly")
{
    Ctx c{};   // math needs no world

    auto call1 = [&](const char* id, float x) {
        return HE::api::find(id)->invoke(c, { Value::ofFloat(x) })[0].f; };
    auto call2 = [&](const char* id, float a, float b) {
        return HE::api::find(id)->invoke(c, { Value::ofFloat(a), Value::ofFloat(b) })[0].f; };

    CHECK(call1("math.sqrt", 16.0f) == doctest::Approx(4.0f));
    CHECK(call1("math.abs", -3.0f) == doctest::Approx(3.0f));
    CHECK(call1("math.floor", 2.9f) == doctest::Approx(2.0f));
    CHECK(call1("math.sign", -7.0f) == doctest::Approx(-1.0f));
    CHECK(call2("math.pow", 2.0f, 10.0f) == doctest::Approx(1024.0f));
    CHECK(call2("math.max", 3.0f, 8.0f) == doctest::Approx(8.0f));
    CHECK(call2("math.mod", 7.0f, 0.0f) == doctest::Approx(0.0f));   // div-by-zero guard

    auto clamp = HE::api::find("math.clamp")->invoke(c,
        { Value::ofFloat(5.0f), Value::ofFloat(0.0f), Value::ofFloat(3.0f) });
    CHECK(clamp[0].f == doctest::Approx(3.0f));

    auto lerp = HE::api::find("math.lerp")->invoke(c,
        { Value::ofFloat(0.0f), Value::ofFloat(10.0f), Value::ofFloat(0.5f) });
    CHECK(lerp[0].f == doctest::Approx(5.0f));

    auto dist = HE::api::find("math.distance")->invoke(c,
        { Value::ofVec2(glm::vec2(0.0f)), Value::ofVec2(glm::vec2(3.0f, 4.0f)) });
    CHECK(dist[0].f == doctest::Approx(5.0f));
}

// ═══ Null-Ctx tolerance ═══════════════════════════════════════════════════════

TEST_CASE("EngineApi: null handles return neutral defaults, never crash")
{
    Ctx c{};   // no world / physics / content

    CHECK(HE::api::entity::getName(c, 1) == "");
    CHECK(HE::api::transform::getPosition(c, 1) == glm::vec3(0.0f));
    CHECK(HE::api::transform::getScale(c, 1) == glm::vec3(1.0f));   // scale default (1,1,1)
    CHECK(HE::api::widget::create(c, "x") == 0);
    CHECK(HE::api::physics::isGrounded(c, 1) == false);

    // through the registry: a setter on a null world is a no-op, a getter defaults
    CHECK_NOTHROW(HE::api::find("transform.setPosition")->invoke(c,
        { Value::ofInt(1), Value::ofColor(glm::vec4(9.0f)) }));
    auto name = HE::api::find("entity.getName")->invoke(c, { Value::ofInt(1) });
    REQUIRE(name.size() == 1);
    CHECK(name[0].s == "");

    // log with a null ctx must be safe too
    CHECK_NOTHROW(HE::api::find("log")->invoke(c, { Value::ofString("hello from a null ctx") }));
}

// ═══ EngineCall node: interpreter routes through the registry ═════════════════

namespace
{
    namespace HC = HorizonCode;

    // A Context whose callApi dispatches to the HE::api registry against `api`,
    // with a local variable store for Get/SetVariable.
    HC::Context makeApiContext(HE::api::Ctx& api, std::unordered_map<std::string, Value>& vars)
    {
        HC::Context ctx;
        ctx.getVariable = [&vars](const std::string& n){ auto it = vars.find(n); return it != vars.end() ? it->second : Value{}; };
        ctx.setVariable = [&vars](const std::string& n, const Value& v){ vars[n] = v; };
        ctx.callApi = [&api](const std::string& id, const std::vector<Value>& args) -> std::vector<Value> {
            const HE::api::ApiFn* fn = HE::api::find(id);
            return fn ? fn->invoke(api, args) : std::vector<Value>{};
        };
        return ctx;
    }
}

TEST_CASE("EngineCall: an exec engine call runs and feeds its result downstream")
{
    HorizonWorld world;
    HE::api::Ctx api{ &world, nullptr, nullptr };

    HC::Graph g;
    // Event "Run" (exec out at pin 0)
    HC::Node ev; ev.type = NT::Event; ev.s = "Run";
    const int evId = g.addNode(ev);
    // ConstInt 0 (parent) + ConstString "Made" (name)
    HC::Node ci; ci.type = NT::ConstInt; ci.f[0] = 0.0f;   const int ciId = g.addNode(ci);
    HC::Node cs; cs.type = NT::ConstString; cs.s = "Made"; const int csId = g.addNode(cs);
    // EngineCall entity.spawn (exec): pins [execIn 0][execOut 1][parent 2][name 3][entity out 4]
    HC::Node ec; ec.type = NT::EngineCall; ec.s = "entity.spawn"; ec.hasArg = true;
    ec.params  = { { "parent", P::Int }, { "name", P::String } };
    ec.results = { { "entity", P::Int } };
    const int ecId = g.addNode(ec);
    // SetVariable "spawned" (Int): pins [execIn 0][execOut 1][Value 2][Value out 3]
    HC::Node sv; sv.type = NT::SetVariable; sv.s = "spawned"; sv.propType = P::Int;
    const int svId = g.addNode(sv);

    REQUIRE(g.connect(evId, 0, ecId, 0));   // event exec → spawn exec-in
    REQUIRE(g.connect(ciId, 0, ecId, 2));   // 0 → parent
    REQUIRE(g.connect(csId, 0, ecId, 3));   // "Made" → name
    REQUIRE(g.connect(ecId, 1, svId, 0));   // spawn exec-out → setVar exec-in
    REQUIRE(g.connect(ecId, 4, svId, 2));   // spawned entity → setVar value

    std::unordered_map<std::string, Value> vars;
    HC::Context ctx = makeApiContext(api, vars);
    HC::Runner runner(g, ctx);
    runner.fireEvent("Run", 0);

    // The variable holds the spawned entity id, and the world knows that entity.
    REQUIRE(vars.count("spawned") == 1);
    const auto spawned = (HE::api::Entity)vars["spawned"].i;
    CHECK(spawned != 0);
    CHECK(HE::api::entity::getName(api, spawned) == "Made");
}

TEST_CASE("EngineCall: a pure engine call evaluates on demand (no exec pin)")
{
    HE::api::Ctx api{};   // math needs no world

    HC::Graph g;
    HC::Node ev; ev.type = NT::Event; ev.s = "Run"; const int evId = g.addNode(ev);
    // Three constants feeding clamp(5, 0, 3)
    HC::Node x;  x.type  = NT::ConstFloat; x.f[0]  = 5.0f; const int xId  = g.addNode(x);
    HC::Node lo; lo.type = NT::ConstFloat; lo.f[0] = 0.0f; const int loId = g.addNode(lo);
    HC::Node hi; hi.type = NT::ConstFloat; hi.f[0] = 3.0f; const int hiId = g.addNode(hi);
    // Pure EngineCall math.clamp: no exec pins → dataIns [x 0][lo 1][hi 2], dataOut [result 3]
    HC::Node ec; ec.type = NT::EngineCall; ec.s = "math.clamp"; ec.hasArg = false;
    ec.params  = { { "x", P::Float }, { "lo", P::Float }, { "hi", P::Float } };
    ec.results = { { "result", P::Float } };
    const int ecId = g.addNode(ec);
    // SetVariable "r": pins [execIn 0][execOut 1][Value 2][Value out 3]
    HC::Node sv; sv.type = NT::SetVariable; sv.s = "r"; sv.propType = P::Float;
    const int svId = g.addNode(sv);

    REQUIRE(g.connect(evId, 0, svId, 0));   // event exec → setVar exec-in
    REQUIRE(g.connect(xId,  0, ecId, 0));   // 5 → x
    REQUIRE(g.connect(loId, 0, ecId, 1));   // 0 → lo
    REQUIRE(g.connect(hiId, 0, ecId, 2));   // 3 → hi
    REQUIRE(g.connect(ecId, 3, svId, 2));   // clamp result → setVar value

    std::unordered_map<std::string, Value> vars;
    HC::Context ctx = makeApiContext(api, vars);
    HC::Runner runner(g, ctx);
    runner.fireEvent("Run", 0);

    REQUIRE(vars.count("r") == 1);
    CHECK(vars["r"].f == doctest::Approx(3.0f));
}

TEST_CASE("EngineCall: signatureOf reflects isExec + mirrored params/results")
{
    HC::Node exec; exec.type = NT::EngineCall; exec.hasArg = true;
    exec.params = { { "entity", P::Int }, { "position", P::Color } };
    const auto es = HC::signatureOf(exec);
    CHECK(es.execIns.size() == 1);          // exec node has flow pins
    CHECK(es.execOuts.size() == 1);
    CHECK(es.dataIns.size() == 2);

    HC::Node pure; pure.type = NT::EngineCall; pure.hasArg = false;
    pure.params  = { { "x", P::Float } };
    pure.results = { { "result", P::Float } };
    const auto ps = HC::signatureOf(pure);
    CHECK(ps.execIns.empty());              // pure node is a compact data chip
    CHECK(ps.execOuts.empty());
    CHECK(ps.dataIns.size() == 1);
    CHECK(ps.dataOuts.size() == 1);
}

TEST_CASE("EngineCall: round-trips through JSON (type by name + mirrored signature)")
{
    HC::Graph g;
    HC::Node ec; ec.type = NT::EngineCall; ec.s = "transform.setPosition"; ec.hasArg = true;
    ec.params = { { "entity", P::Int }, { "position", P::Color } };
    g.addNode(ec);

    HC::Graph loaded;
    REQUIRE(HC::fromJson(HC::toJson(g), loaded));
    REQUIRE(loaded.nodes.size() == 1);
    CHECK(loaded.nodes[0].type == NT::EngineCall);
    CHECK(loaded.nodes[0].s == "transform.setPosition");
    CHECK(loaded.nodes[0].hasArg == true);            // isExec survives
    REQUIRE(loaded.nodes[0].params.size() == 2);
    CHECK(loaded.nodes[0].params[1].type == P::Color);
}

// ═══ Random library (seeded, bounded, reproducible) ══════════════════════════

TEST_CASE("Random: seeded → reproducible; ranges bounded; chance extremes")
{
    Ctx c{};   // no engine state needed
    auto call = [&](const char* id, std::vector<Value> a){ return HE::api::find(id)->invoke(c, a); };

    // Same seed → same sequence.
    call("random.seed", { Value::ofInt(1234) });
    const float a0 = call("random.value", {})[0].f;
    const float a1 = call("random.value", {})[0].f;
    call("random.seed", { Value::ofInt(1234) });
    CHECK(call("random.value", {})[0].f == doctest::Approx(a0));
    CHECK(call("random.value", {})[0].f == doctest::Approx(a1));

    // value() ∈ [0, 1).
    for (int i = 0; i < 200; ++i)
    { const float v = call("random.value", {})[0].f; CHECK(v >= 0.0f); CHECK(v < 1.0f); }

    // Degenerate ranges collapse to the endpoint.
    CHECK(call("random.range",    { Value::ofFloat(5.0f), Value::ofFloat(5.0f) })[0].f == doctest::Approx(5.0f));
    CHECK(call("random.rangeInt", { Value::ofInt(7),      Value::ofInt(7) })[0].i == 7);

    // rangeInt is inclusive on both ends; swaps reversed bounds.
    for (int i = 0; i < 200; ++i)
    { const int v = call("random.rangeInt", { Value::ofInt(6), Value::ofInt(1) })[0].i; CHECK(v >= 1); CHECK(v <= 6); }

    // chance(1) always true, chance(0) always false.
    CHECK(call("random.chance", { Value::ofFloat(1.0f) })[0].b == true);
    CHECK(call("random.chance", { Value::ofFloat(0.0f) })[0].b == false);
}

// ═══ Time / frame clock ═══════════════════════════════════════════════════════

TEST_CASE("Time: advancing the clock reflects through the registry")
{
    Ctx c{};
    auto call = [&](const char* id){ return HE::api::find(id)->invoke(c, {}); };

    HE::api::time::reset();
    CHECK(call("time.frameCount")[0].i == 0);
    CHECK(call("time.elapsed")[0].f == doctest::Approx(0.0f));

    HE::api::time::advance(0.5f);
    HE::api::time::advance(0.25f);
    CHECK(call("time.deltaTime")[0].f  == doctest::Approx(0.25f)); // last dt
    CHECK(call("time.elapsed")[0].f    == doctest::Approx(0.75f)); // accumulated
    CHECK(call("time.frameCount")[0].i == 2);

    HE::api::time::reset();
    CHECK(call("time.elapsed")[0].f    == doctest::Approx(0.0f));
    CHECK(call("time.frameCount")[0].i == 0);
}

// ═══ Input snapshot ═══════════════════════════════════════════════════════════

TEST_CASE("Input: the pushed snapshot reflects through the registry")
{
    Ctx c{};
    auto call = [&](const char* id, std::vector<Value> a){ return HE::api::find(id)->invoke(c, a); };

    HE::api::input::clear();
    CHECK(call("input.keyDown", { Value::ofString("W") })[0].b == false);

    HE::api::input::setKeysDown({ "W", "Space" });
    HE::api::input::setMouse({ 12.0f, 34.0f }, { 1.0f, -2.0f }, (1u << 0) | (1u << 2), 3.0f);

    CHECK(call("input.keyDown", { Value::ofString("W") })[0].b     == true);
    CHECK(call("input.keyDown", { Value::ofString("Space") })[0].b == true);
    CHECK(call("input.keyDown", { Value::ofString("A") })[0].b     == false);
    CHECK(call("input.mouseButton", { Value::ofInt(0) })[0].b == true);   // left
    CHECK(call("input.mouseButton", { Value::ofInt(1) })[0].b == false);  // right
    CHECK(call("input.mouseButton", { Value::ofInt(2) })[0].b == true);   // middle

    const Value pos = call("input.mousePosition", {})[0];
    CHECK(pos.v2.x == doctest::Approx(12.0f));
    CHECK(pos.v2.y == doctest::Approx(34.0f));
    const Value d = call("input.mouseDelta", {})[0];
    CHECK(d.v2.x == doctest::Approx(1.0f));
    CHECK(d.v2.y == doctest::Approx(-2.0f));
    CHECK(call("input.scrollDelta", {})[0].f == doctest::Approx(3.0f));

    HE::api::input::clear();
}

// ═══ Transform value type ═════════════════════════════════════════════════════

TEST_CASE("Transform: ConstTransform flows through Set as a Transform value")
{
    HC::Graph g;
    HC::Node ev; ev.type = NT::Event; ev.s = "Run"; const int evId = g.addNode(ev);
    HC::Node ct; ct.type = NT::ConstTransform;
    ct.tpos = { 1.0f, 2.0f, 3.0f }; ct.trot = { 10.0f, 20.0f, 30.0f }; ct.tscl = { 4.0f, 5.0f, 6.0f };
    const int ctId = g.addNode(ct);
    HC::Node sv; sv.type = NT::SetVariable; sv.s = "t"; sv.propType = P::Transform;
    const int svId = g.addNode(sv);
    REQUIRE(g.connect(evId, 0, svId, 0));   // exec
    REQUIRE(g.connect(ctId, 0, svId, 2));   // ConstTransform out → SetVariable value

    std::unordered_map<std::string, Value> vars;
    HC::Context ctx;
    ctx.setVariable = [&vars](const std::string& n, const Value& v){ vars[n] = v; };
    HC::Runner runner(g, ctx);
    runner.fireEvent("Run", 0);

    REQUIRE(vars.count("t") == 1);
    CHECK(vars["t"].type == P::Transform);
    CHECK(vars["t"].tpos.x == doctest::Approx(1.0f));
    CHECK(vars["t"].trot.y == doctest::Approx(20.0f));
    CHECK(vars["t"].tscl.z == doctest::Approx(6.0f));
}

TEST_CASE("Transform: ConstTransform round-trips through JSON")
{
    HC::Graph g;
    HC::Node ct; ct.type = NT::ConstTransform;
    ct.tpos = { 1.0f, 2.0f, 3.0f }; ct.trot = { 4.0f, 5.0f, 6.0f }; ct.tscl = { 7.0f, 8.0f, 9.0f };
    g.addNode(ct);

    HC::Graph loaded;
    REQUIRE(HC::fromJson(HC::toJson(g), loaded));
    REQUIRE(loaded.nodes.size() == 1);
    CHECK(loaded.nodes[0].type == NT::ConstTransform);
    CHECK(loaded.nodes[0].tpos.y == doctest::Approx(2.0f));
    CHECK(loaded.nodes[0].trot.z == doctest::Approx(6.0f));
    CHECK(loaded.nodes[0].tscl.x == doctest::Approx(7.0f));
}

TEST_CASE("Transform: a Transform variable defaults to identity")
{
    HC::Variable v; v.name = "xf"; v.type = P::Transform;
    const Value d = HC::variableDefaultValue(v);
    CHECK(d.type == P::Transform);
    CHECK(d.tpos.x == doctest::Approx(0.0f));
    CHECK(d.trot.x == doctest::Approx(0.0f));
    CHECK(d.tscl.x == doctest::Approx(1.0f));
    CHECK(d.tscl.z == doctest::Approx(1.0f));
}

// ═══ Array variables + array-op nodes ═════════════════════════════════════════

TEST_CASE("Array: an array variable defaults to empty; round-trips through JSON")
{
    HC::Graph g;
    HC::Variable v; v.name = "nums"; v.type = P::Int; v.isArray = true;
    g.variables.push_back(v);

    const Value d = HC::variableDefaultValue(v);
    CHECK(d.isArray == true);
    CHECK(d.type == P::Int);
    CHECK(d.items.empty());

    HC::Graph loaded;
    REQUIRE(HC::fromJson(HC::toJson(g), loaded));
    REQUIRE(loaded.variables.size() == 1);
    CHECK(loaded.variables[0].isArray == true);
    CHECK(loaded.variables[0].type == P::Int);
}

TEST_CASE("Array: connect rejects an array pin joined to a scalar pin")
{
    HC::Graph g;
    HC::Node mk; mk.type = NT::ArrayMake; mk.propType = P::Int; const int mkId = g.addNode(mk);
    HC::Node sv; sv.type = NT::SetVariable; sv.s = "n"; sv.propType = P::Int; /* isArray=false */
    const int svId = g.addNode(sv);
    // mk out (Int[]) → SetVariable value (Int scalar) must be rejected.
    CHECK(g.connect(mkId, 0, svId, 2) == false);
    // But an Int[] SetVariable accepts it.
    HC::Node sa; sa.type = NT::SetVariable; sa.s = "a"; sa.propType = P::Int; sa.isArray = true;
    const int saId = g.addNode(sa);
    CHECK(g.connect(mkId, 0, saId, 2) == true);
}

TEST_CASE("Array: Make → Add chain → Length + Get evaluate correctly")
{
    HC::Graph g;
    HC::Node ev; ev.type = NT::Event; ev.s = "Run"; const int evId = g.addNode(ev);
    auto konst = [&](int val){ HC::Node c; c.type = NT::ConstInt; c.f[0] = (float)val; return g.addNode(c); };
    const int c10 = konst(10), c20 = konst(20), c30 = konst(30), ci1 = konst(1);
    auto arrNode = [&](NT t){ HC::Node n; n.type = t; n.propType = P::Int; return g.addNode(n); };
    const int mk  = arrNode(NT::ArrayMake);
    const int a1  = arrNode(NT::ArrayAdd);
    const int a2  = arrNode(NT::ArrayAdd);
    const int a3  = arrNode(NT::ArrayAdd);
    const int len = arrNode(NT::ArrayLength);
    const int get = arrNode(NT::ArrayGet);
    HC::Node sL; sL.type = NT::SetVariable; sL.s = "len"; sL.propType = P::Int; const int sLId = g.addNode(sL);
    HC::Node sE; sE.type = NT::SetVariable; sE.s = "el";  sE.propType = P::Int; const int sEId = g.addNode(sE);

    // Build the array: []→[10]→[10,20]→[10,20,30]. Add pins: array-in 0, value-in 1, array-out 2.
    REQUIRE(g.connect(mk, 0, a1, 0)); REQUIRE(g.connect(c10, 0, a1, 1));
    REQUIRE(g.connect(a1, 2, a2, 0)); REQUIRE(g.connect(c20, 0, a2, 1));
    REQUIRE(g.connect(a2, 2, a3, 0)); REQUIRE(g.connect(c30, 0, a3, 1));
    // Length (out pin 1) + Get index 1 (array-in 0, index-in 1, out pin 2).
    REQUIRE(g.connect(a3, 2, len, 0));
    REQUIRE(g.connect(a3, 2, get, 0)); REQUIRE(g.connect(ci1, 0, get, 1));
    // Drive via exec: Run → set len → set el.
    REQUIRE(g.connect(evId, 0, sLId, 0)); REQUIRE(g.connect(len, 1, sLId, 2));
    REQUIRE(g.connect(sLId, 1, sEId, 0)); REQUIRE(g.connect(get, 2, sEId, 2));

    std::unordered_map<std::string, Value> vars;
    HC::Context ctx;
    ctx.setVariable = [&vars](const std::string& n, const Value& v){ vars[n] = v; };
    HC::Runner runner(g, ctx);
    runner.fireEvent("Run", 0);

    REQUIRE(vars.count("len") == 1);
    REQUIRE(vars.count("el") == 1);
    CHECK(vars["len"].i == 3);   // three elements
    CHECK(vars["el"].i == 20);   // element at index 1
}
