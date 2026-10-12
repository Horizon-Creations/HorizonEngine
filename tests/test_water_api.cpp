// Thema 179, step 5: what an author and a script can do with the water physics.
//   * the rows of the water group that ask the water (surfaceAt, depthAt, flowAt, submerged,
//     buoyancy), the one that waits for the news (pollImmersion) and the two that act
//     (disturb, setFlow), through the registry as HorizonCode reaches them, through the
//     C++ functions behind them, and from Lua (Python has its case in test_python_scripting);
//   * the event queue under pollImmersion (PhysicsWorld::pollWaterEvents), on real Jolt bodies;
//   * the editor's side of the same settings: the calls the Water tab and the Rigid Body rows
//     make, saved, loaded, undone, and still what the physics does afterwards.
// The model underneath is tested in test_water_query.cpp (the query, the settings in the scene
// format) and test_water_buoyancy.cpp (the forces); what these say is that the rows are there,
// answer in WORLD space, agree with the physics, and that nothing an author sets is lost on the
// way to the bodies.
//
// The lake is the one of test_water_buoyancy.cpp: 40 x 40 m, surface at y = 3 over flat ground at
// y = 0, inside a 64 x 64 m landscape, with the material's waves (+/-0.24 m).
#include "doctest.h"
#include "TestFsUtil.h"
#include "EditorUndo.h"
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/ScriptContext.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/WaterField.h>
#include <HorizonScene/WaterQuery.h>
#include <HorizonScene/WaterRipples.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace water = HE::water;
using HE::api::Ctx;
using HE::api::Value;

namespace
{
    constexpr float kDt    = 1.0f / 60.0f;
    constexpr float kLevel = 3.0f;      // the lake's surface; the ground is at 0

    Value F(float v) { return Value::ofFloat(v); }
    Value I(int v)   { return Value::ofInt(v); }
    int   asInt(Entity e) { return static_cast<int>(static_cast<uint32_t>(e)); }

    std::vector<Value> call(Ctx& c, const char* id, std::vector<Value> args = {})
    {
        const HE::api::ApiFn* fn = HE::api::find(id);
        REQUIRE_MESSAGE(fn != nullptr, id);
        return fn->invoke(c, args);
    }

    std::vector<glm::vec2> rect(float x0, float z0, float x1, float z1)
    {
        return { { x0, z0 }, { x1, z0 }, { x1, z1 }, { x0, z1 } };
    }

    struct BodySpec
    {
        glm::vec3 at{ 0.0f, 3.0f, 0.0f };
        glm::vec3 halfExtents{ 0.5f, 0.5f, 0.5f };
        float     mass    = 500.0f;
        float     density = 500.0f;     // half the water's: it floats
        bool      sphere  = false;
        bool      affected = true;
    };

    // A landscape (optionally moved and turned about Y) with a lake, the bodies in it, and the
    // physics that steps them.
    struct Pond
    {
        HorizonWorld world;
        Entity       terrain = entt::null;
        uint16_t     lake    = water::kNoBody;
        PhysicsWorld phys;

        explicit Pond(const glm::vec3& at = glm::vec3(0.0f), const glm::vec3& turnDeg = glm::vec3(0.0f))
        {
            terrain = world.createEntity("Landscape");
            TransformComponent t;
            t.position = at;
            t.rotation = turnDeg;
            world.addComponent(terrain, t);
            TerrainComponent tc;
            tc.sizeX = tc.sizeZ = 64.0f;
            tc.resolution = 9;             // flat ground: no seed, no sculpted heights
            tc.water.res  = 64;            // a cell is a metre
            world.addComponent(terrain, tc);
            lake = tcRef().water.createBody(kLevel);
            REQUIRE(water::addPolygon(tcRef(), lake, rect(-20, -20, 20, 20)).ok);
        }

        TerrainComponent& tcRef() { return world.registry().get<TerrainComponent>(terrain); }

        Entity add(const BodySpec& s)
        {
            Entity e = world.createEntity("Body");
            TransformComponent t;
            t.position = s.at;
            world.addComponent(e, t);
            RigidBodyComponent rb;
            rb.type            = RigidBodyType::Dynamic;
            rb.mass            = s.mass;
            rb.density         = s.density;
            rb.affectedByWater = s.affected;
            world.addComponent(e, rb);
            ColliderComponent col;
            col.shape       = s.sphere ? ColliderShape::Sphere : ColliderShape::Box;
            col.halfExtents = s.halfExtents;
            col.radius      = s.halfExtents.x;
            world.addComponent(e, col);
            return e;
        }

        void start(bool waterOn = true)
        {
            phys.initialize(world);
            phys.setThreadCount(1);
            phys.setWaterPhysics(waterOn);
        }
        void step(int n = 1) { for (int i = 0; i < n; ++i) phys.step(world, kDt); }
        Ctx  ctx() { return Ctx{ &world, &phys, nullptr }; }
        static uint32_t id(Entity e) { return static_cast<uint32_t>(e); }
    };

    // The events of one poll as plain data, from the four parallel arrays of the row.
    struct Seen
    {
        int       entity  = 0;
        bool      entered = false;
        float     speed   = 0.0f;
        glm::vec3 where{ 0.0f };
    };

    std::vector<Seen> poll(Ctx& c)
    {
        const auto out = call(c, "water.pollImmersion");
        REQUIRE(out.size() == 4);
        REQUIRE(out[1].items.size() == out[0].items.size());
        REQUIRE(out[2].items.size() == out[0].items.size());
        REQUIRE(out[3].items.size() == out[0].items.size());
        std::vector<Seen> events;
        for (size_t i = 0; i < out[0].items.size(); ++i)
            events.push_back({ out[0].items[i].i, out[1].items[i].b, out[2].items[i].f, out[3].items[i].v3 });
        return events;
    }

    std::vector<Seen> forEntity(const std::vector<Seen>& all, Entity e)
    {
        std::vector<Seen> mine;
        for (const Seen& s : all)
            if (s.entity == asInt(e)) mine.push_back(s);
        return mine;
    }

    // The first terrain in a world (entity handles are remapped by undo and by a reload).
    TerrainComponent* firstTerrain(HorizonWorld& world)
    {
        for (auto [e, tc] : world.registry().view<TerrainComponent>().each()) { (void)e; return &tc; }
        return nullptr;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// The rows
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Water api (physics): the new rows are there, with the shapes a graph and the text languages rely on")
{
    using P = HE::api::PinType;
    struct Row { const char* id; bool exec; size_t params; size_t results; };
    for (const Row& r : { Row{ "water.surfaceAt", false, 2, 2 }, Row{ "water.depthAt", false, 2, 1 },
                          Row{ "water.flowAt", false, 2, 1 },    Row{ "water.submerged", false, 1, 1 },
                          Row{ "water.buoyancy", false, 1, 1 },  Row{ "water.pollImmersion", true, 0, 4 },
                          Row{ "water.disturb", true, 4, 0 },    Row{ "water.setFlow", true, 4, 1 } })
    {
        INFO("row: " << r.id);
        const HE::api::ApiFn* fn = HE::api::find(r.id);
        REQUIRE(fn != nullptr);
        CHECK(std::string(fn->category) == "Water");
        CHECK(fn->isExec == r.exec);
        CHECK(fn->params.size() == r.params);
        CHECK(fn->results.size() == r.results);
        CHECK(std::string(fn->cppCall).rfind("HE::api::water::", 0) == 0);
    }
    CHECK(HE::api::isScriptGroup("water"));

    const HE::api::ApiFn* surface = HE::api::find("water.surfaceAt");
    CHECK(surface->results[0].type == P::Bool);       // wet, then the height: read Wet first
    CHECK(surface->results[1].type == P::Float);
    CHECK(HE::api::find("water.flowAt")->results[0].type == P::Vec3);

    // Index i of each list is one event; all four must be arrays or the pairing is lost.
    const HE::api::ApiFn* events = HE::api::find("water.pollImmersion");
    CHECK(events->results[0].type == P::Int);
    CHECK(events->results[1].type == P::Bool);
    CHECK(events->results[2].type == P::Float);
    CHECK(events->results[3].type == P::Vec3);
    for (const auto& res : events->results)
    {
        INFO("result: " << res.name);
        CHECK(res.isArray);
    }
}

TEST_CASE("Water api (physics): surface, depth and flow answer in WORLD space, from the physics' own water")
{
    // The landscape stands at (30, 2, -20) and is turned a quarter about Y: the lake's centre is
    // (30, -20) in the world and its surface at 2 + 3 = 5.
    Pond pond({ 30.0f, 2.0f, -20.0f }, { 0.0f, 90.0f, 0.0f });
    Ctx noPhysics{ &pond.world, nullptr, nullptr };
    const glm::mat4 M = HE::worldMatrixOf(pond.world, pond.terrain);
    const auto wavesAt = [](float x, float z, double t)
    { return water::sampleWaves(water::kPhysicsWaves, water::kPhysicsWaveCount, x, z, t).height; };

    SUBCASE("without a physics world the clock stands at 0")
    {
        const auto at = call(noPhysics, "water.surfaceAt", { F(30.0f), F(-20.0f) });
        REQUIRE(at.size() == 2);
        CHECK(at[0].b);
        CHECK(at[1].f == doctest::Approx(5.0f + wavesAt(30.0f, -20.0f, 0.0)).epsilon(1.0e-4));

        // The depth is the surface over the ground, which is flat at the landscape's own y.
        CHECK(call(noPhysics, "water.depthAt", { F(30.0f), F(-20.0f) })[0].f == doctest::Approx(at[1].f - 2.0f));

        // Dry on the bank (the lake ends 20 m from its middle, the landscape 32 m) and off the
        // landscape altogether.
        const auto dry = call(noPhysics, "water.surfaceAt", { F(55.0f), F(-20.0f) });
        CHECK_FALSE(dry[0].b);
        CHECK(call(noPhysics, "water.depthAt", { F(55.0f), F(-20.0f) })[0].f == 0.0f);
        CHECK_FALSE(call(noPhysics, "water.surfaceAt", { F(500.0f), F(500.0f) })[0].b);

        // The C++ function behind the row says the same thing.
        float h = -1.0f;
        CHECK(HE::api::water::surfaceAt(noPhysics, 30.0f, -20.0f, h));
        CHECK(h == doctest::Approx(at[1].f));
        h = 123.0f;
        CHECK_FALSE(HE::api::water::surfaceAt(noPhysics, 55.0f, -20.0f, h));
        CHECK(h == 123.0f);                              // untouched when dry
    }
    SUBCASE("with one, the waves move with the physics clock")
    {
        pond.start();
        Ctx c = pond.ctx();
        const float at0 = call(c, "water.surfaceAt", { F(30.0f), F(-20.0f) })[1].f;
        pond.step(90);                                  // 1.5 s of simulation
        REQUIRE(pond.phys.simTime() == doctest::Approx(1.5).epsilon(1.0e-6));
        const float at15 = call(c, "water.surfaceAt", { F(30.0f), F(-20.0f) })[1].f;
        CHECK(at15 == doctest::Approx(5.0f + wavesAt(30.0f, -20.0f, pond.phys.simTime())).epsilon(1.0e-4));
        REQUIRE(std::abs(wavesAt(30.0f, -20.0f, 1.5) - wavesAt(30.0f, -20.0f, 0.0)) > 1.0e-3f);
        CHECK(std::abs(at15 - at0) > 1.0e-3f);
    }
    SUBCASE("the current is the landscape's own, carried into the world")
    {
        water::BodyPhysics p;
        p.flow = { 2.0f, 0.0f };                         // 2 m/s along the landscape's +x
        REQUIRE(water::setPhysics(pond.tcRef(), pond.lake, p));
        const glm::vec3 expected = glm::vec3(M * glm::vec4(2.0f, 0.0f, 0.0f, 0.0f));   // a direction: no translation
        const auto flow = call(noPhysics, "water.flowAt", { F(30.0f), F(-20.0f) })[0].v3;
        CHECK(flow.x == doctest::Approx(expected.x).epsilon(1.0e-4));
        CHECK(flow.y == 0.0f);
        CHECK(flow.z == doctest::Approx(expected.z).epsilon(1.0e-4));
        CHECK(glm::length(flow) == doctest::Approx(2.0f).epsilon(1.0e-4));   // turning does not change the speed
        // Still where it is dry.
        const glm::vec3 none = call(noPhysics, "water.flowAt", { F(55.0f), F(-20.0f) })[0].v3;
        CHECK(glm::length(none) == 0.0f);
    }
}

TEST_CASE("Water api (physics): setFlow takes WORLD axes and flowAt gives the same back")
{
    Pond pond({ 30.0f, 2.0f, -20.0f }, { 0.0f, 90.0f, 0.0f });
    Ctx c{ &pond.world, nullptr, nullptr };
    TerrainComponent& tc = pond.tcRef();
    const glm::mat4 M = HE::worldMatrixOf(pond.world, pond.terrain);
    const Value land = I(asInt(pond.terrain));
    const bool  dirtyBefore = tc.dirty, waterDirtyBefore = tc.water.dirty;
    const uint64_t revisionBefore = tc.water.revision;

    // 3 m/s along the WORLD +z, wherever the landscape happens to be turned.
    const auto ok = call(c, "water.setFlow", { land, I(pond.lake), F(0.0f), F(3.0f) });
    REQUIRE(ok.size() == 1);
    CHECK(ok[0].b);
    const glm::vec3 back = call(c, "water.flowAt", { F(30.0f), F(-20.0f) })[0].v3;
    CHECK(back.x == doctest::Approx(0.0f).epsilon(1.0e-4));
    CHECK(back.z == doctest::Approx(3.0f).epsilon(1.0e-4));

    // Stored in the landscape's own axes, which is what the Water tab shows and the scene keeps.
    const glm::vec4 local = glm::inverse(M) * glm::vec4(0.0f, 0.0f, 3.0f, 0.0f);
    const water::Body* body = tc.water.findBody(pond.lake);
    REQUIRE(body != nullptr);
    CHECK(body->physics.flow.x == doctest::Approx(local.x).epsilon(1.0e-4));
    CHECK(body->physics.flow.y == doctest::Approx(local.z).epsilon(1.0e-4));
    CHECK(body->physics.density == water::kDefaultDensity);          // nothing else was touched

    // Like setPhysics: the physics is told, the surface is not rebuilt and no chunk is.
    CHECK(tc.water.revision > revisionBefore);
    CHECK(tc.water.dirty == waterDirtyBefore);
    CHECK(tc.dirty == dirtyBefore);

    // Held to the bounds a stored value is held to.
    CHECK(call(c, "water.setFlow", { land, I(pond.lake), F(1000.0f), F(-1000.0f) })[0].b);
    const water::BodyPhysics& held = tc.water.findBody(pond.lake)->physics;
    CHECK(std::abs(held.flow.x) <= water::kMaxFlow);
    CHECK(std::abs(held.flow.y) <= water::kMaxFlow);

    // Refused, and nothing changes: no such body, body 0, not a landscape, not a number.
    const water::BodyPhysics before = tc.water.findBody(pond.lake)->physics;
    CHECK_FALSE(call(c, "water.setFlow", { land, I(99), F(1.0f), F(0.0f) })[0].b);
    CHECK_FALSE(call(c, "water.setFlow", { land, I(0), F(1.0f), F(0.0f) })[0].b);
    CHECK_FALSE(call(c, "water.setFlow", { I(asInt(pond.world.createEntity("Not land"))), I(pond.lake), F(1.0f), F(0.0f) })[0].b);
    CHECK_FALSE(call(c, "water.setFlow", { land, I(pond.lake), F(std::numeric_limits<float>::quiet_NaN()), F(0.0f) })[0].b);
    CHECK(tc.water.findBody(pond.lake)->physics == before);
}

TEST_CASE("Water api (physics): submerged and buoyancy follow a body in the water")
{
    Pond pond;
    BodySpec boxSpec;                                   // half the water's density
    const Entity box = pond.add(boxSpec);
    BodySpec stoneSpec;
    stoneSpec.at = { 8.0f, 3.5f, 0.0f };
    stoneSpec.mass = 2600.0f; stoneSpec.density = 2600.0f; stoneSpec.sphere = true;
    const Entity stone = pond.add(stoneSpec);
    BodySpec shoreSpec;                                 // on the dry bank, 28 m from the middle
    shoreSpec.at = { 28.0f, 1.0f, 0.0f };
    const Entity onLand = pond.add(shoreSpec);
    pond.start();
    Ctx c = pond.ctx();

    // Before anything has been served nothing is known: 0, not a guess.
    CHECK(call(c, "water.submerged", { I(asInt(box)) })[0].f == 0.0f);
    CHECK(call(c, "water.buoyancy", { I(asInt(box)) })[0].f == 0.0f);

    pond.step(360);                                     // the box finds the surface, the stone the bed
    double share = 0.0, lift = 0.0;
    int n = 0;
    for (int i = 0; i < 600; ++i, ++n)
    {
        pond.step();
        share += call(c, "water.submerged", { I(asInt(box)) })[0].f;
        lift  += call(c, "water.buoyancy", { I(asInt(box)) })[0].f;
    }
    // A box of half the water's density rides half under, and the water carries exactly its weight
    // (the lift is the buoyancy factor 2 times the share): the mean of both over the swell.
    CHECK(std::abs(share / n - 0.5) < 0.1);
    CHECK(std::abs(lift / n - 1.0) < 0.2);

    // The stone lies fully under, and is carried by 1000 / 2600 of its weight however deep. It sleeps
    // on the bed and keeps the numbers it came to rest with.
    CHECK(call(c, "water.submerged", { I(asInt(stone)) })[0].f == doctest::Approx(1.0f).epsilon(1.0e-3));
    CHECK(call(c, "water.buoyancy", { I(asInt(stone)) })[0].f == doctest::Approx(1000.0f / 2600.0f).epsilon(0.01));
    pond.step(120);
    CHECK(call(c, "water.submerged", { I(asInt(stone)) })[0].f == doctest::Approx(1.0f).epsilon(1.0e-3));
    CHECK(call(c, "water.buoyancy", { I(asInt(stone)) })[0].f == doctest::Approx(1000.0f / 2600.0f).epsilon(0.01));

    // Not in water, not a body, not a thing: zero, and the C++ function says the same.
    CHECK(call(c, "water.submerged", { I(asInt(onLand)) })[0].f == 0.0f);
    CHECK(call(c, "water.buoyancy", { I(asInt(onLand)) })[0].f == 0.0f);
    CHECK(call(c, "water.submerged", { I(asInt(pond.terrain)) })[0].f == 0.0f);
    CHECK(call(c, "water.submerged", { I(123456) })[0].f == 0.0f);
    CHECK(HE::api::water::submerged(c, static_cast<HE::api::Entity>(stone)) == doctest::Approx(1.0f).epsilon(1.0e-3));
    CHECK(HE::api::water::buoyancy(c, static_cast<HE::api::Entity>(stone)) ==
          doctest::Approx(1000.0f / 2600.0f).epsilon(0.01));

    // Without a physics world there is nothing to ask.
    Ctx noPhysics{ &pond.world, nullptr, nullptr };
    CHECK(call(noPhysics, "water.submerged", { I(asInt(stone)) })[0].f == 0.0f);
    CHECK(call(noPhysics, "water.buoyancy", { I(asInt(stone)) })[0].f == 0.0f);
}

// ═════════════════════════════════════════════════════════════════════════════
// The event
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Water api (physics): a body falling into the lake enters once, with the speed it hit at")
{
    Pond pond;
    BodySpec s;
    s.at = { 0.0f, 5.0f, 0.0f };                        // its bottom is 1.5 m over the surface (the box is 1 m)
    const Entity box = pond.add(s);
    pond.start();
    Ctx c = pond.ctx();

    CHECK(poll(c).empty());                             // nothing has happened yet
    pond.step(20);
    CHECK(poll(c).empty());                             // a third of a second of falling is still air

    pond.step(150);                                     // it lands, plunges, comes up, bobs
    const std::vector<Seen> events = forEntity(poll(c), box);
    REQUIRE_FALSE(events.empty());
    const Seen& first = events.front();
    CHECK(first.entered);
    // 5 m is 1.5 m of fall to the surface with the box's bottom: about 5.4 m/s.
    CHECK(first.speed > 3.5f);
    CHECK(first.speed < 7.5f);
    CHECK(first.where.x == doctest::Approx(0.0f).epsilon(0.05));
    CHECK(first.where.z == doctest::Approx(0.0f).epsilon(0.05));
    CHECK(first.where.y > kLevel - 0.3f);
    CHECK(first.where.y < kLevel + 1.3f);               // the centre of mass, at the moment its bottom met the water

    // However it bobs afterwards, entering and leaving alternate, and it ends in the water.
    int net = 0;
    bool expectEnter = true;
    for (const Seen& e : events)
    {
        CHECK(e.entered == expectEnter);
        expectEnter = !expectEnter;
        net += e.entered ? 1 : -1;
    }
    CHECK(net == 1);

    CHECK(poll(c).empty());                             // drained: read once

    pond.step(300);
    const std::vector<Seen> later = forEntity(poll(c), box);
    int after = net;
    for (const Seen& e : later) after += e.entered ? 1 : -1;
    CHECK(after == 1);                                  // five seconds on: it floats there, in the water
    CHECK(pond.phys.submergedFraction(Pond::id(box)) > 0.2f);
}

TEST_CASE("Water api (physics): a body that starts in the lake enters at the first step, slowly")
{
    Pond pond;
    const Entity box = pond.add({});                    // at the surface
    pond.start();
    Ctx c = pond.ctx();
    pond.step(1);
    const std::vector<Seen> events = forEntity(poll(c), box);
    REQUIRE(events.size() == 1);
    CHECK(events[0].entered);
    CHECK(events[0].speed < 1.0f);                      // it did not fall in: it was put there
}

TEST_CASE("Water api (physics): a body thrown out of the water leaves, and comes back in")
{
    Pond pond;
    BodySpec s;
    s.sphere = true;                                    // a ball floats the same however it lands
    const Entity ball = pond.add(s);
    pond.start();
    Ctx c = pond.ctx();
    pond.step(120);
    poll(c);                                            // the start-of-scene entry, not what is tested here

    pond.phys.setVelocity(Pond::id(ball), { 0.0f, 6.0f, 0.0f });
    pond.step(180);                                     // out, up, down, in again
    const std::vector<Seen> events = forEntity(poll(c), ball);
    REQUIRE(events.size() >= 2);
    CHECK_FALSE(events[0].entered);                     // it left the water through the surface…
    CHECK(events[0].speed > 1.0f);                      // …fast (the water has slowed the 6 m/s it was thrown at)
    CHECK(events[0].speed < 7.0f);
    CHECK(events[1].entered);                           // and came back
    CHECK(events[1].speed > 0.8f);                      // a fall of its own: well over the 'put there' case
}

TEST_CASE("Water api (physics): water switched off under a body, or a body taken out of it, is an exit without a speed")
{
    Pond pond;
    std::vector<Entity> boxes;
    for (int i = 0; i < 3; ++i)
    {
        BodySpec s;
        s.at = { -6.0f + 6.0f * static_cast<float>(i), 3.0f, 0.0f };
        boxes.push_back(pond.add(s));
    }
    pond.start();
    Ctx c = pond.ctx();
    pond.step(120);
    CHECK(poll(c).size() >= 3);                         // they were all there from the start

    SUBCASE("the lake stops doing anything")
    {
        water::BodyPhysics off;
        off.enabled = false;
        REQUIRE(water::setPhysics(pond.tcRef(), pond.lake, off));
        pond.step(1);
        const std::vector<Seen> events = poll(c);
        REQUIRE(events.size() == 3);
        for (size_t i = 0; i < events.size(); ++i)
        {
            CHECK_FALSE(events[i].entered);
            CHECK(events[i].speed == 0.0f);             // it did not pass the surface, the surface stopped counting
            CHECK(events[i].where.y > 1.0f);            // where the box was, not the origin
            if (i > 0) CHECK(events[i].entity > events[i - 1].entity);   // in entity order, not a hash's
        }
        pond.step(1);
        CHECK(poll(c).empty());                         // once, not every step
    }
    SUBCASE("one body opts out")
    {
        pond.world.registry().get<RigidBodyComponent>(boxes[1]).affectedByWater = false;
        pond.step(1);
        const std::vector<Seen> events = poll(c);
        REQUIRE(events.size() == 1);
        CHECK(events[0].entity == asInt(boxes[1]));
        CHECK_FALSE(events[0].entered);
    }
    SUBCASE("a destroyed entity is not an exit")
    {
        pond.world.destroyEntity(boxes[0]);             // gone while it is in the water
        pond.step(2);
        for (const Seen& e : poll(c))
            CHECK(e.entity != asInt(boxes[0]));
    }
    SUBCASE("an entity taken away takes its unread events with it")
    {
        water::BodyPhysics off;
        off.enabled = false;
        REQUIRE(water::setPhysics(pond.tcRef(), pond.lake, off));
        pond.step(1);                                   // three exits are waiting, nobody has read them
        pond.phys.removeEntity(Pond::id(boxes[2]));     // how the engine takes a body away
        const std::vector<Seen> events = poll(c);
        REQUIRE(events.size() == 2);
        CHECK(events[0].entity == asInt(boxes[0]));
        CHECK(events[1].entity == asInt(boxes[1]));
    }
}

TEST_CASE("Water api (physics): the queue is bounded, the oldest news goes, and it is the newest that stays")
{
    Pond pond;
    std::vector<Entity> boxes;
    constexpr int kBoxes = 20;
    for (int i = 0; i < kBoxes; ++i)
    {
        BodySpec s;
        s.at = { -9.0f + 2.0f * static_cast<float>(i % 10), 3.0f, -3.0f + 6.0f * static_cast<float>(i / 10) };
        s.halfExtents = { 0.4f, 0.4f, 0.4f };
        s.mass = 100.0f; s.density = 500.0f;
        boxes.push_back(pond.add(s));
    }
    pond.start();
    Ctx c = pond.ctx();
    pond.step(60);
    poll(c);

    // Off and on again, seven times: each round is twenty exits and twenty entries, 280 events with
    // nobody reading, against a bound of PhysicsWorld::kMaxWaterEvents.
    for (int round = 0; round < 7; ++round)
    {
        water::BodyPhysics p;
        p.enabled = false;
        REQUIRE(water::setPhysics(pond.tcRef(), pond.lake, p));
        pond.step(1);
        p.enabled = true;
        REQUIRE(water::setPhysics(pond.tcRef(), pond.lake, p));
        pond.step(1);
    }
    const std::vector<Seen> events = poll(c);
    REQUIRE(events.size() == PhysicsWorld::kMaxWaterEvents);
    // The last step put them all back in, in entity order: the tail of the queue is that.
    CHECK(events.back().entered);
    CHECK(events.back().entity == asInt(boxes.back()));
    for (size_t i = events.size() - kBoxes; i < events.size(); ++i)
        CHECK(events[i].entered);
    CHECK(poll(c).empty());
}

TEST_CASE("Water api (physics): no water physics, no news")
{
    Pond pond;
    pond.add({});
    pond.start(false);                                  // switched off, whatever the environment says
    Ctx c = pond.ctx();
    pond.step(60);
    CHECK(poll(c).empty());
    CHECK(call(c, "water.submerged", { I(0) })[0].f == 0.0f);

    // And a world with no physics world at all answers with empty lists, not with a crash.
    Ctx none{ &pond.world, nullptr, nullptr };
    CHECK(poll(none).empty());
}

// ═════════════════════════════════════════════════════════════════════════════
// disturb
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Water api (physics): disturb raises a ring for the display, and only that")
{
    Pond pond;
    const Entity box = pond.add({});
    pond.start();
    Ctx c = pond.ctx();
    pond.step(60);
    pond.phys.waterRipples().clear();
    const glm::vec3 before = pond.world.registry().get<TransformComponent>(box).position;

    call(c, "water.disturb", { F(3.0f), F(-4.0f), F(1.0f), F(0.8f) });
    REQUIRE(pond.phys.waterRipples().count() == 1);
    const water::Ripple& r = pond.phys.waterRipples().at(0);
    CHECK(r.pos.x == 3.0f);
    CHECK(r.pos.y == -4.0f);
    CHECK(r.kind == WaterRippleScript);
    CHECK(r.radius == doctest::Approx(1.0f));
    CHECK(r.strength == doctest::Approx(0.8f));
    CHECK(r.age == 0.0f);
    CHECK(glm::length(r.drift) == 0.0f);                // still water

    // Moving water carries it.
    REQUIRE(call(c, "water.setFlow", { I(asInt(pond.terrain)), I(pond.lake), F(1.0f), F(0.0f) })[0].b);
    call(c, "water.disturb", { F(-5.0f), F(5.0f), F(0.5f), F(0.5f) });
    REQUIRE(pond.phys.waterRipples().count() == 2);
    CHECK(pond.phys.waterRipples().at(1).drift.x == doctest::Approx(1.0f).epsilon(1.0e-4));

    // Held to what a ring can be (RippleField::add: 0.05 to 6 m, strength 0 to 1), and what is not
    // a number or has no strength is not a ring.
    call(c, "water.disturb", { F(0.0f), F(0.0f), F(500.0f), F(7.0f) });
    REQUIRE(pond.phys.waterRipples().count() == 3);
    CHECK(pond.phys.waterRipples().at(2).radius == 6.0f);
    CHECK(pond.phys.waterRipples().at(2).strength == 1.0f);
    call(c, "water.disturb", { F(std::numeric_limits<float>::quiet_NaN()), F(0.0f), F(1.0f), F(0.5f) });
    call(c, "water.disturb", { F(1.0f), F(1.0f), F(1.0f), F(0.0f) });
    CHECK(pond.phys.waterRipples().count() == 3);

    // Without a physics world there is no list to hold it: it does nothing, quietly.
    Ctx none{ &pond.world, nullptr, nullptr };
    call(none, "water.disturb", { F(0.0f), F(0.0f), F(1.0f), F(1.0f) });
    CHECK(pond.phys.waterRipples().count() == 3);

    // A ring moves no body: the box floats on as it did. (The current set above does drag it, and a
    // body standing in moving water leaves a wake of its own, so only the rings of the SCRIPT are
    // counted from here on.)
    pond.step(2);
    CHECK(pond.world.registry().get<TransformComponent>(box).position.x == doctest::Approx(before.x).epsilon(0.05));
    int scripted = 0;
    for (int i = 0; i < pond.phys.waterRipples().count(); ++i)
        if (pond.phys.waterRipples().at(i).kind == WaterRippleScript) ++scripted;
    CHECK(scripted == 3);
}

// ═════════════════════════════════════════════════════════════════════════════
// Lua
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Water api (physics): the rows reach a Lua script as horizon.water.*")
{
    Pond pond;
    const Entity box = pond.add({});
    pond.start();
    pond.step(120);

    ScriptContext ctx(pond.world);
    ctx.setPhysicsWorld(&pond.phys);
    const std::string script =
        "_G._land = " + std::to_string(asInt(pond.terrain)) + "\n"
        "_G._box  = " + std::to_string(asInt(box)) + "\n"
        "local wet, y = horizon.water.surfaceAt(0, 0)\n"
        "_G._wet = wet and 1 or 0\n"
        "_G._y = y\n"
        "_G._depth = horizon.water.depthAt(0, 0)\n"
        "_G._dryDepth = horizon.water.depthAt(50, 50)\n"
        "_G._ok = horizon.water.setFlow(_G._land, " + std::to_string(static_cast<int>(pond.lake)) + ", 0, 3) and 1 or 0\n"
        "local fx, fy, fz = horizon.water.flowAt(0, 0)\n"
        "_G._fz = fz\n"
        "_G._share = horizon.water.submerged(_G._box)\n"
        "_G._lift = horizon.water.buoyancy(_G._box)\n"
        "horizon.water.disturb(2, 3, 1, 0.5)\n"
        // A list result is ONE table (the generic dispatcher used to hand back the number 0 for it),
        // and a vec3 inside a list is {x, y, z}.
        "local who, entered, speed, where = horizon.water.pollImmersion()\n"
        "_G._events = #who\n"
        "_G._first = who[1]\n"
        "_G._enteredFirst = entered[1] and 1 or 0\n"
        "_G._speedType = type(speed[1]) == 'number' and 1 or 0\n"
        "_G._whereY = where[1][2]\n"
        "local ja, jb = horizon.physics.pollJointBroken()\n"
        "_G._jointLists = (type(ja) == 'table' and type(jb) == 'table' and #ja == 0 and #jb == 0) and 1 or 0\n";
    const bool ran = ctx.engine().exec(script.c_str());
    INFO("lua said: " << ctx.engine().lastError());
    REQUIRE(ran);

    CHECK(ctx.engine().getGlobalNumber("_wet") == 1.0);
    CHECK(ctx.engine().getGlobalNumber("_y") == doctest::Approx(kLevel).epsilon(0.1));          // the surface, waves and all
    CHECK(ctx.engine().getGlobalNumber("_depth") == doctest::Approx(kLevel).epsilon(0.1));      // over flat ground at 0
    CHECK(ctx.engine().getGlobalNumber("_dryDepth") == 0.0);
    CHECK(ctx.engine().getGlobalNumber("_ok") == 1.0);
    CHECK(ctx.engine().getGlobalNumber("_fz") == doctest::Approx(3.0).epsilon(1.0e-4));
    CHECK(ctx.engine().getGlobalNumber("_share") > 0.2);                                        // floating, so partly under
    CHECK(ctx.engine().getGlobalNumber("_share") < 0.9);
    CHECK(ctx.engine().getGlobalNumber("_lift") > 0.3);
    CHECK(static_cast<int>(ctx.engine().getGlobalNumber("_events")) >= 1);                      // the box came in at the start
    CHECK(static_cast<int>(ctx.engine().getGlobalNumber("_first")) == asInt(box));
    CHECK(ctx.engine().getGlobalNumber("_enteredFirst") == 1.0);
    CHECK(ctx.engine().getGlobalNumber("_speedType") == 1.0);
    CHECK(ctx.engine().getGlobalNumber("_whereY") == doctest::Approx(kLevel).epsilon(0.2));   // where the box floats
    CHECK(ctx.engine().getGlobalNumber("_jointLists") == 1.0);       // the same fix reaches every list result

    // The ring from disturb(2, 3, 1, 0.5) is on the water.
    int scripted = 0;
    for (int i = 0; i < pond.phys.waterRipples().count(); ++i)
    {
        const water::Ripple& r = pond.phys.waterRipples().at(i);
        if (r.kind == WaterRippleScript && r.pos.x == 2.0f && r.pos.y == 3.0f) ++scripted;
    }
    CHECK(scripted == 1);
}

// ═════════════════════════════════════════════════════════════════════════════
// What an author sets in the editor
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Water settings: the Water tab's edit is one undo step, and undo and redo bring the numbers back")
{
    Pond pond;
    EditorUndo undo;
    undo.setWorld(&pond.world);

    water::BodyPhysics edited;
    edited.density     = 1250.0f;
    edited.linearDrag  = 1.5f;
    edited.angularDrag = 0.2f;
    edited.flow        = { 0.75f, -0.25f };
    edited.splash      = 2.0f;
    REQUIRE(edited != water::BodyPhysics{});

    // The gesture the Water tab makes when a control is grabbed, written, and let go.
    undo.capturePre();
    undo.stashPre("Water Physics");
    REQUIRE(water::setPhysics(pond.tcRef(), pond.lake, edited));
    undo.commitPending();
    REQUIRE(undo.canUndo());
    CHECK(undo.undoLabel() == "Water Physics");
    CHECK(undo.undoDepth() == 1);

    REQUIRE(undo.undo());
    TerrainComponent* back = firstTerrain(pond.world);                  // undo remaps entity handles
    REQUIRE(back != nullptr);
    REQUIRE(back->water.bodies.size() == 1);
    CHECK(back->water.bodies[0].physics == water::BodyPhysics{});       // all the way back, every field
    CHECK(back->water.wetCells() > 1000);                               // and the lake itself is intact

    REQUIRE(undo.redo());
    back = firstTerrain(pond.world);
    REQUIRE(back != nullptr);
    REQUIRE(back->water.bodies.size() == 1);
    CHECK(back->water.bodies[0].physics == edited);
}

TEST_CASE("Water settings: what is typed in the editor is saved, loaded, and is what the bodies feel afterwards")
{
    // The Water tab's numbers and the Rigid Body rows: heavy brine, a box of 500 that would float
    // half out in fresh water, and a bullet that opts out of the water.
    const fs::path file = fs::temp_directory_path() / "he_test_water_api.hescene";
    water::BodyPhysics typed;
    typed.density = 2000.0f;                            // the box (500) rides a quarter under it
    typed.flow    = { 0.0f, 0.5f };                     // 0.5 m/s toward +z: a few metres in the time below
    typed.splash  = 0.5f;
    {
        Pond pond;
        REQUIRE(water::setPhysics(pond.tcRef(), pond.lake, typed));
        BodySpec box;
        box.at = { -8.0f, 3.0f, 0.0f };
        pond.add(box);
        BodySpec bullet;
        bullet.at = { 8.0f, 3.0f, 0.0f };
        bullet.density = 7800.0f; bullet.affected = false;
        pond.add(bullet);
        SceneSerializer ser;
        REQUIRE(ser.save(pond.world, file, SerializeFormat::JSON));
    }

    Pond reloaded;
    reloaded.world.clear();
    SceneSerializer ser;
    REQUIRE(ser.load(reloaded.world, file, SerializeFormat::JSON));
    he_test::removeQuiet(file);

    TerrainComponent* tc = firstTerrain(reloaded.world);
    REQUIRE(tc != nullptr);
    REQUIRE(tc->water.bodies.size() == 1);
    CHECK(tc->water.bodies[0].physics == typed);        // every number, bit for bit

    Entity box = entt::null, bullet = entt::null;
    for (auto [e, rb] : reloaded.world.registry().view<RigidBodyComponent>().each())
    {
        if (rb.affectedByWater) { box = e; CHECK(rb.density == 500.0f); }
        else                    { bullet = e; CHECK(rb.density == 7800.0f); }
    }
    REQUIRE((box != entt::null));
    REQUIRE((bullet != entt::null));

    // …and the physics of the reloaded scene does what the numbers say: the box rides a quarter
    // under (500 over 2000), the bullet falls through to the bed, and the brine carries a current.
    reloaded.terrain = entt::null;
    for (auto [e, t] : reloaded.world.registry().view<TerrainComponent>().each()) { (void)t; reloaded.terrain = e; }
    reloaded.start();
    reloaded.step(360);
    double share = 0.0;
    int n = 0;
    for (int i = 0; i < 600; ++i, ++n)
    {
        reloaded.step();
        share += reloaded.phys.submergedFraction(Pond::id(box));
    }
    CHECK(std::abs(share / n - 0.25) < 0.1);            // in fresh water the same box rides at 0.5
    CHECK(reloaded.world.registry().get<TransformComponent>(bullet).position.y < 1.0f);   // on the bed
    CHECK(reloaded.phys.submergedFraction(Pond::id(bullet)) == 0.0f);                     // it never felt the water
    const glm::vec3 drifted = reloaded.world.registry().get<TransformComponent>(box).position;
    CHECK(drifted.z > 0.5f);                            // the current pulls toward +z; the box started at z = 0
    CHECK(drifted.z < 18.0f);                           // and it is still in the lake
}
