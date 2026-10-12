// Thema 179, step 2: "where is the water and what is it doing here" (WaterQuery.h) and
// the settings the water carries for the physics (WaterField.h BodyPhysics, the
// rigid body's two fields). No Jolt: a landscape on the stack answers the questions.
// The bodies that float on it are test_water_buoyancy.cpp.
#include "doctest.h"
#include "TestFsUtil.h"
#include <HorizonScene/WaterQuery.h>
#include <HorizonScene/WaterField.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/PhysicsTuning.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <Physics/WaterWaves.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
namespace water = HE::water;
using json = nlohmann::json;

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    // 64 × 64 m at 64 cells, so a cell is a metre and a cell-aligned shape is 0 and 255.
    // Flat ground: no seed, no sculpted heights.
    TerrainComponent makeTerrain()
    {
        TerrainComponent tc;
        tc.sizeX = tc.sizeZ = 64.0f;
        tc.resolution = 9;
        tc.water.res = 64;
        return tc;
    }

    std::vector<glm::vec2> rect(float x0, float z0, float x1, float z1)
    {
        return { { x0, z0 }, { x1, z0 }, { x1, z1 }, { x0, z1 } };
    }

    Entity addLandscape(HorizonWorld& world, const glm::vec3& at = glm::vec3(0.0f),
                        const glm::vec3& turnDeg = glm::vec3(0.0f),
                        const glm::vec3& scale = glm::vec3(1.0f))
    {
        Entity e = world.createEntity("Landscape");
        TransformComponent t;
        t.position = at;
        t.rotation = turnDeg;
        t.scale    = scale;
        world.addComponent(e, t);
        world.addComponent(e, makeTerrain());
        return e;
    }

    TerrainComponent& terrainOf(HorizonWorld& world, Entity e)
    {
        return world.registry().get<TerrainComponent>(e);
    }

    uint16_t addLake(TerrainComponent& tc, const std::vector<glm::vec2>& poly, float level)
    {
        const uint16_t id = tc.water.createBody(level);
        REQUIRE(water::addPolygon(tc, id, poly).ok);
        return id;
    }

    float wavesAt(double x, double z, double t)
    {
        return water::sampleWaves(water::kPhysicsWaves, water::kPhysicsWaveCount, x, z, t).height;
    }

    float sumOfAmplitudes()
    {
        float a = 0.0f;
        for (int i = 0; i < water::kPhysicsWaveCount; ++i) a += water::waveAmplitude(water::kPhysicsWaves[i]);
        return a;
    }

    // A scene as JSON text. (saveToMemory is the binary form undo uses: its keys are not
    // quoted, so a search for `"physics"` in it could never fail.)
    std::string sceneText(HorizonWorld& world)
    {
        const fs::path file = fs::temp_directory_path() / "he_test_water_query_text.hescene";
        SceneSerializer ser;
        REQUIRE(ser.save(world, file, SerializeFormat::JSON));
        std::ifstream in(file);
        std::stringstream text;
        text << in.rdbuf();
        he_test::removeQuiet(file);
        return text.str();
    }

    json* findKey(json& j, const char* key)
    {
        if (j.is_object())
        {
            auto it = j.find(key);
            if (it != j.end() && it->is_object()) return &*it;
            for (auto& kv : j.items())
                if (auto* r = findKey(kv.value(), key)) return r;
        }
        else if (j.is_array())
            for (auto& v : j)
                if (auto* r = findKey(v, key)) return r;
        return nullptr;
    }

    // Save the world as a file, let `damage` edit the parsed scene, load it into `out`.
    template <class F>
    void reloadEdited(HorizonWorld& world, HorizonWorld& out, F damage)
    {
        const fs::path file = fs::temp_directory_path() / "he_test_water_query.hescene";
        SceneSerializer ser;
        REQUIRE(ser.save(world, file, SerializeFormat::JSON));
        json j;
        { std::ifstream in(file); in >> j; }
        damage(j);
        { std::ofstream o(file, std::ios::trunc); o << j.dump(1); }
        REQUIRE(ser.load(out, file, SerializeFormat::JSON));
        he_test::removeQuiet(file);
    }

    const TerrainComponent* firstTerrain(HorizonWorld& world)
    {
        for (auto [e, tc] : world.registry().view<TerrainComponent>().each()) { (void)e; return &tc; }
        return nullptr;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// The wave function: one definition
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Water waves: the amplitude is steepness times length over 2 pi, and a train travels")
{
    const water::WaveTrain a = water::kWaveA;
    CHECK(water::waveAmplitude(a) == doctest::Approx(0.07f * 14.0f / static_cast<float>(2.0 * kPi)));

    // Along its direction over one wavelength the train reaches both amplitudes.
    const double rad = static_cast<double>(a.directionDeg) * kPi / 180.0;
    float hi = -1.0e9f, lo = 1.0e9f;
    for (int i = 0; i < 4000; ++i)
    {
        const double d = static_cast<double>(a.length) * i / 4000.0;
        const float h = water::sampleWaves(&a, 1, d * std::cos(rad), d * std::sin(rad), 0.0).height;
        hi = std::max(hi, h);
        lo = std::min(lo, h);
    }
    CHECK(hi == doctest::Approx(water::waveAmplitude(a)).epsilon(1.0e-3));
    CHECK(lo == doctest::Approx(-water::waveAmplitude(a)).epsilon(1.0e-3));

    // Moving with the wave at its own speed keeps the height: the pattern at t is the
    // pattern at 0 carried along the direction by speed · t.
    for (const double t : { 0.7, 5.0, 61.3 })
    {
        const double x = 3.25, z = -8.5;
        const float h0 = water::sampleWaves(&a, 1, x, z, 0.0).height;
        const float h1 = water::sampleWaves(&a, 1, x + a.speed * t * std::cos(rad), z + a.speed * t * std::sin(rad), t).height;
        CHECK(h1 == doctest::Approx(h0).epsilon(1.0e-4).scale(0.1));
    }
}

TEST_CASE("Water waves: steepness 0 is flat, a wavelength that is not positive is ignored")
{
    water::WaveTrain flat = water::kWaveB;
    flat.steepness = 0.0f;
    const water::WaveSample s = water::sampleWaves(&flat, 1, 12.0, -3.0, 4.0);
    CHECK(s.height == 0.0f);
    CHECK(s.dhdx == 0.0f);
    CHECK(s.dhdz == 0.0f);

    water::WaveTrain broken = water::kWaveA;
    broken.length = 0.0f;
    const water::WaveSample b = water::sampleWaves(&broken, 1, 1.0, 2.0, 3.0);
    CHECK(std::isfinite(b.height));
    CHECK(b.height == 0.0f);
}

TEST_CASE("Water waves: the phase keeps its resolution far from the origin and late in a session")
{
    // The phase is computed in double. A float time of a million seconds would have lost
    // everything below a fraction of a second; the answer must still move smoothly.
    const water::WaveTrain a = water::kWaveA;
    const double t0 = 1.0e6, dt = 1.0 / 60.0;
    const float h0 = water::sampleWaves(&a, 1, 40000.25, -30000.5, t0).height;
    const float h1 = water::sampleWaves(&a, 1, 40000.25, -30000.5, t0 + dt).height;
    CHECK(std::isfinite(h0));
    CHECK(std::abs(h1 - h0) > 0.0f);                        // it moved…
    CHECK(std::abs(h1 - h0) < water::waveAmplitude(a) * 0.1f);   // …by a step, not a jump
}

// ═════════════════════════════════════════════════════════════════════════════
// The query
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Water query: a world without water has an empty index and answers dry")
{
    HorizonWorld world;
    const Entity e = addLandscape(world);
    water::WaterIndex idx = water::WaterIndex::build(world);
    CHECK(idx.empty());
    water::Surface s;
    s.wet = true;
    CHECK_FALSE(idx.surfaceAt(0.0f, 0.0f, 0.0, s));
    CHECK_FALSE(s.wet);
    CHECK(std::isnan(idx.heightAt(0.0f, 0.0f, 0.0)));

    // A body without any water in the field (no grid yet) is still no water.
    terrainOf(world, e).water.createBody(2.0f);
    CHECK(water::WaterIndex::build(world).empty());

    // Once the lake is drawn there is something to ask.
    addLake(terrainOf(world, e), rect(-10, -10, 10, 10), 2.0f);
    idx = water::WaterIndex::build(world);
    CHECK_FALSE(idx.empty());
    CHECK(idx.landscapes() == 1);
    CHECK_FALSE(std::isnan(idx.heightAt(0.0f, 0.0f, 0.0)));
}

TEST_CASE("Water query: the level is the field's, in world space, with the waves on top")
{
    HorizonWorld world;
    const Entity e = addLandscape(world, { 100.0f, 5.0f, -50.0f });
    const uint16_t lake = addLake(terrainOf(world, e), rect(-10, -10, 10, 10), 2.0f);
    const water::WaterIndex idx = water::WaterIndex::build(world);

    // The same point in the landscape's own space is what the field says (the existing
    // reader), so the query is that reader plus the landscape's place in the world.
    float local = 0.0f;
    REQUIRE(water::levelAt(terrainOf(world, e), 0.0f, 0.0f, local));
    CHECK(local == 2.0f);

    for (const double t : { 0.0, 3.7, 123.4 })
    {
        CAPTURE(t);
        water::Surface s;
        REQUIRE(idx.surfaceAt(100.0f, -50.0f, t, s));
        CHECK(s.wet);
        CHECK((s.terrain == e));
        CHECK(s.body == lake);
        CHECK(s.level == doctest::Approx(5.0f + local));
        CHECK(s.height == doctest::Approx(s.level + wavesAt(100.0, -50.0, t)));
        CHECK(s.depth == doctest::Approx(s.height - 5.0f));   // the ground stands at the landscape's y
        CHECK(std::abs(s.height - s.level) <= sumOfAmplitudes() + 1.0e-4f);
    }

    // Dry beyond the lake, beyond the landscape, and for a position that is not a number.
    water::Surface s;
    CHECK_FALSE(idx.surfaceAt(111.0f, -50.0f, 0.0, s));
    CHECK_FALSE(idx.surfaceAt(100.0f, -39.0f, 0.0, s));
    CHECK_FALSE(idx.surfaceAt(0.0f, 0.0f, 0.0, s));
    CHECK_FALSE(idx.surfaceAt(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0, s));
    // Right at the edge of the whole-metre lake: in at 109.5, out at 110.5.
    CHECK(idx.surfaceAt(109.5f, -50.0f, 0.0, s));
    CHECK_FALSE(idx.surfaceAt(110.5f, -50.0f, 0.0, s));
}

TEST_CASE("Water query: the normal is the slope of the height")
{
    HorizonWorld world;
    const Entity e = addLandscape(world);
    addLake(terrainOf(world, e), rect(-30, -30, 30, 30), 1.0f);
    const water::WaterIndex idx = water::WaterIndex::build(world);

    int checked = 0;
    for (const double t : { 0.0, 2.5, 9.1 })
        for (float x = -21.0f; x <= 21.0f; x += 7.0f)
            for (float z = -21.0f; z <= 21.0f; z += 7.0f)
            {
                water::Surface s;
                REQUIRE(idx.surfaceAt(x, z, t, s));
                CHECK(glm::length(s.normal) == doctest::Approx(1.0f));
                CHECK(s.normal.y > 0.0f);

                // Central differences of the height the same query reports.
                const float h = 0.01f;
                const float dhdx = (idx.heightAt(x + h, z, t) - idx.heightAt(x - h, z, t)) / (2.0f * h);
                const float dhdz = (idx.heightAt(x, z + h, t) - idx.heightAt(x, z - h, t)) / (2.0f * h);
                const glm::vec3 expect = glm::normalize(glm::vec3(-dhdx, 1.0f, -dhdz));
                CHECK(std::abs(s.normal.x - expect.x) < 3.0e-3f);
                CHECK(std::abs(s.normal.z - expect.z) < 3.0e-3f);
                ++checked;
            }
    CHECK(checked == 3 * 7 * 7);   // the loops ran; a lake that answered nothing would not have

    // And the waves are not flat: somewhere the surface really tilts.
    float steepest = 0.0f;
    for (float x = -20.0f; x <= 20.0f; x += 0.5f)
    {
        water::Surface s;
        REQUIRE(idx.surfaceAt(x, 3.0f, 1.0, s));
        steepest = std::max(steepest, 1.0f - s.normal.y);
    }
    CHECK(steepest > 1.0e-3f);
}

TEST_CASE("Water query: a landscape that is moved, turned about Y and scaled")
{
    HorizonWorld world;
    const Entity e = addLandscape(world, { 30.0f, 2.0f, -20.0f }, { 0.0f, 90.0f, 0.0f }, { 2.0f, 1.0f, 2.0f });
    addLake(terrainOf(world, e), rect(-20, -5, 0, 5), 3.0f);   // lopsided on purpose
    const glm::mat4 M = HE::worldMatrixOf(world, e);
    const auto world3 = [&](float lx, float lz) { return glm::vec3(M * glm::vec4(lx, 3.0f, lz, 1.0f)); };
    const water::WaterIndex idx = water::WaterIndex::build(world);

    water::Surface s;
    const glm::vec3 inside = world3(-10.0f, 0.0f);
    REQUIRE(idx.surfaceAt(inside.x, inside.z, 0.0, s));
    CHECK(s.level == doctest::Approx(inside.y));
    CHECK(inside.y == doctest::Approx(5.0f));                  // the landscape stands at 2, the level is 3 above it

    // The mirror image across the landscape's origin is outside the lake…
    const glm::vec3 mirror = world3(10.0f, 0.0f);
    CHECK_FALSE(idx.surfaceAt(mirror.x, mirror.z, 0.0, s));
    // …and so is where the same local spot would be if the turn were ignored.
    CHECK_FALSE(idx.surfaceAt(30.0f + 2.0f * -10.0f, -20.0f, 0.0, s));
    // The footprint ends where the scaled edge is (local x = 0), not at half a metre in world.
    const glm::vec3 justIn = world3(-0.5f, 0.0f), justOut = world3(0.5f, 0.0f);
    CHECK(idx.surfaceAt(justIn.x, justIn.z, 0.0, s));
    CHECK_FALSE(idx.surfaceAt(justOut.x, justOut.z, 0.0, s));
}

TEST_CASE("Water query: dry where the ground stands above the water, wet again with the clip off")
{
    HorizonWorld world;
    const Entity e = addLandscape(world);
    TerrainComponent& tc = terrainOf(world, e);
    tc.sculptHeights.assign(9 * 9, 0.0f);                       // 8 m between vertices
    for (int z = 3; z <= 5; ++z)
        for (int x = 3; x <= 5; ++x)
            tc.sculptHeights[static_cast<size_t>(z * 9 + x)] = 6.0f;   // a hill in the middle
    addLake(tc, rect(-20, -20, 20, 20), 2.0f);

    {
        const water::WaterIndex idx = water::WaterIndex::build(world);
        water::Surface s;
        CHECK_FALSE(idx.surfaceAt(0.0f, 0.0f, 0.0, s));          // the hilltop is above the level
        CHECK(idx.surfaceAt(15.0f, 15.0f, 0.0, s));              // the foot of it is not
    }
    water::setShoreClip(tc, false, 0.0f);
    {
        const water::WaterIndex idx = water::WaterIndex::build(world);
        water::Surface s;
        REQUIRE(idx.surfaceAt(0.0f, 0.0f, 0.0, s));              // the cells say water, nothing clips it
        CHECK(s.depth == 0.0f);                                  // and a depth is never negative
    }
}

TEST_CASE("Water query: two landscapes, the higher surface wins and a tie goes to the smaller id")
{
    HorizonWorld world;
    const Entity a = addLandscape(world);
    const Entity b = addLandscape(world);
    REQUIRE(static_cast<uint32_t>(a) < static_cast<uint32_t>(b));
    addLake(terrainOf(world, a), rect(-10, -10, 10, 10), 1.0f);
    const uint16_t upper = addLake(terrainOf(world, b), rect(-10, -10, 10, 10), 4.0f);

    water::Surface s;
    REQUIRE(water::WaterIndex::build(world).surfaceAt(0.0f, 0.0f, 0.0, s));
    CHECK((s.terrain == b));
    CHECK(s.level == doctest::Approx(4.0f));

    REQUIRE(water::setLevel(terrainOf(world, b), upper, 1.0f));
    REQUIRE(water::WaterIndex::build(world).surfaceAt(0.0f, 0.0f, 0.0, s));
    CHECK((s.terrain == a));                                    // equal: the smaller entity id, never a hash order
}

TEST_CASE("Water query: time is only an argument")
{
    HorizonWorld world;
    const Entity e = addLandscape(world);
    addLake(terrainOf(world, e), rect(-20, -20, 20, 20), 0.0f);
    const water::WaterIndex idx = water::WaterIndex::build(world);

    water::Surface a, b, c;
    REQUIRE(idx.surfaceAt(3.0f, 4.0f, 17.25, a));
    REQUIRE(idx.surfaceAt(3.0f, 4.0f, 17.25, b));
    CHECK(a.height == b.height);                                // same time, same answer, to the bit
    CHECK(a.normal == b.normal);
    REQUIRE(idx.surfaceAt(3.0f, 4.0f, 18.5, c));
    CHECK(a.height != c.height);                                // another time, another surface
    CHECK(a.level == c.level);                                  // …over the same level
}

TEST_CASE("Water query: the broad-phase box covers the wet rectangle and follows an edit")
{
    HorizonWorld world;
    const Entity e = addLandscape(world, { 100.0f, 5.0f, -50.0f });
    TerrainComponent& tc = terrainOf(world, e);
    water::WaterIndex idx;
    std::vector<water::Region> boxes;

    idx.refresh(world);
    idx.regions(boxes);
    CHECK(boxes.empty());                                       // no water, no box

    const uint16_t lake = addLake(tc, rect(-10, -10, 10, 10), 2.0f);
    idx.refresh(world);
    idx.regions(boxes);
    REQUIRE(boxes.size() == 1);
    CHECK((boxes[0].terrain == e));
    CHECK(boxes[0].min.x <= 90.0f);   CHECK(boxes[0].max.x >= 110.0f);
    CHECK(boxes[0].min.z <= -60.0f);  CHECK(boxes[0].max.z >= -40.0f);
    CHECK(boxes[0].max.y >= 7.0f + sumOfAmplitudes());          // up to the waves' crests
    CHECK(boxes[0].min.y < 7.0f - 100.0f);                      // and far below the level
    const float before = boxes[0].max.y;

    // The same index, after an edit: the rectangle is rescanned because the revision moved.
    REQUIRE(water::setLevel(tc, lake, 9.0f));
    idx.refresh(world);
    idx.regions(boxes);
    REQUIRE(boxes.size() == 1);
    CHECK(boxes[0].max.y > before + 6.0f);
    CHECK(idx.revisionOf(e) == tc.water.revision);
    CHECK(idx.revisionOf(entt::null) == 0);

    // Drying the lake out leaves nothing to box.
    REQUIRE(water::removeBody(tc, lake).ok);
    idx.refresh(world);
    idx.regions(boxes);
    CHECK(boxes.empty());
}

// ═════════════════════════════════════════════════════════════════════════════
// What the water carries for the physics: BodyPhysics
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Water physics settings: the defaults write nothing, a change writes only what changed")
{
    HorizonWorld world;
    const Entity e = addLandscape(world);
    TerrainComponent& tc = terrainOf(world, e);
    const uint16_t lake = addLake(tc, rect(-10, -10, 10, 10), 2.0f);

    SUBCASE("defaults leave no trace in the scene")
    {
        REQUIRE(tc.water.findBody(lake)->physics == water::BodyPhysics{});
        const std::string text = sceneText(world);
        CHECK(text.find("waterBodies") != std::string::npos);
        CHECK(text.find("\"physics\"") == std::string::npos);
        CHECK(text.find("linearDrag") == std::string::npos);
    }
    SUBCASE("a change writes only the keys that differ, and round-trips")
    {
        water::BodyPhysics p;
        p.enabled = false;
        p.density = 1025.0f;
        p.flow    = { 1.5f, -2.0f };
        REQUIRE(water::setPhysics(tc, lake, p));

        const std::string text = sceneText(world);
        json j = json::parse(text);
        json* terrain = findKey(j, "terrain");
        REQUIRE(terrain != nullptr);
        const json& ph = (*terrain)["waterBodies"][0]["physics"];
        CHECK(ph.size() == 3);
        CHECK(ph.contains("enabled"));
        CHECK(ph.contains("density"));
        CHECK(ph.contains("flow"));
        CHECK_FALSE(ph.contains("linearDrag"));
        CHECK_FALSE(ph.contains("angularDrag"));
        CHECK_FALSE(ph.contains("splash"));

        HorizonWorld loaded;
        reloadEdited(world, loaded, [](json&) {});         // the file, as written, loaded back
        const TerrainComponent* t = firstTerrain(loaded);
        REQUIRE(t != nullptr);
        REQUIRE(t->water.bodies.size() == 1);
        CHECK(t->water.bodies[0].physics == p);
        CHECK(water::sameContent(t->water, tc.water));
    }
    SUBCASE("every setting travels")
    {
        water::BodyPhysics p;
        p.enabled = false; p.density = 800.0f; p.linearDrag = 2.0f; p.angularDrag = 0.4f;
        p.flow = { -3.0f, 4.0f }; p.splash = 0.25f;
        REQUIRE(water::setPhysics(tc, lake, p));
        SceneSerializer ser;
        std::vector<uint8_t> blob;
        REQUIRE(ser.saveToMemory(world, blob));
        HorizonWorld loaded;
        REQUIRE(ser.loadFromMemory(loaded, blob));
        const TerrainComponent* t = firstTerrain(loaded);
        REQUIRE(t != nullptr);
        CHECK(t->water.bodies[0].physics == p);
    }
}

TEST_CASE("Water physics settings: a damaged file cannot leave a number the physics would trip on")
{
    HorizonWorld world;
    const Entity e = addLandscape(world);
    TerrainComponent& tc = terrainOf(world, e);
    const uint16_t lake = addLake(tc, rect(-10, -10, 10, 10), 2.0f);
    water::BodyPhysics p;
    p.density = 1100.0f;                                         // any change, so the key exists to damage
    REQUIRE(water::setPhysics(tc, lake, p));

    HorizonWorld loaded;
    reloadEdited(world, loaded, [](json& j)
    {
        json* terrain = findKey(j, "terrain");
        REQUIRE(terrain != nullptr);
        json& ph = (*terrain)["waterBodies"][0]["physics"];
        ph["density"]     = 1.0e9;      // far above anything water is
        ph["linearDrag"]  = -5.0;       // not a drag
        ph["angularDrag"] = 1.0e6;
        ph["splash"]      = 99.0;
        ph["flow"]        = json::array({ 1.0e9, -1.0e9 });
        ph["enabled"]     = "yes";      // not a bool: ignored, the default stands
    });
    const TerrainComponent* t = firstTerrain(loaded);
    REQUIRE(t != nullptr);
    REQUIRE(t->water.bodies.size() == 1);
    const water::BodyPhysics& q = t->water.bodies[0].physics;
    CHECK(q.density == water::kMaxDensity);
    CHECK(q.linearDrag == 0.0f);
    CHECK(q.angularDrag == water::kMaxDrag);
    CHECK(q.splash == water::kMaxSplash);
    CHECK(q.flow.x == water::kMaxFlow);
    CHECK(q.flow.y == -water::kMaxFlow);
    CHECK(q.enabled);

    // A shape that is not a pair of numbers is ignored whole.
    HorizonWorld loaded2;
    reloadEdited(world, loaded2, [](json& j)
    {
        json* terrain = findKey(j, "terrain");
        REQUIRE(terrain != nullptr);
        (*terrain)["waterBodies"][0]["physics"]["flow"] = json::array({ 1.0, "x", 3.0 });
    });
    const TerrainComponent* t2 = firstTerrain(loaded2);
    REQUIRE(t2 != nullptr);
    CHECK(t2->water.bodies[0].physics.flow == glm::vec2(0.0f));
    CHECK(t2->water.bodies[0].physics.density == 1100.0f);
}

TEST_CASE("Underwater fog settings: the defaults write nothing, a change writes only what changed and round-trips")
{
    HorizonWorld world;
    const Entity e = addLandscape(world);
    TerrainComponent& tc = terrainOf(world, e);
    const uint16_t lake = addLake(tc, rect(-10, -10, 10, 10), 2.0f);

    SUBCASE("defaults leave no trace in the scene")
    {
        REQUIRE(tc.water.findBody(lake)->underwaterFog == water::UnderwaterFogParams{});
        const std::string text = sceneText(world);
        CHECK(text.find("waterBodies") != std::string::npos);
        CHECK(text.find("underwaterFog") == std::string::npos);
    }
    SUBCASE("a change writes only the keys that differ, and round-trips")
    {
        water::UnderwaterFogParams f;
        f.density = 1.2f;
        f.deepColor = { 0.01f, 0.02f, 0.03f };
        REQUIRE(water::setUnderwaterFog(tc, lake, f));

        const std::string text = sceneText(world);
        json j = json::parse(text);
        json* terrain = findKey(j, "terrain");
        REQUIRE(terrain != nullptr);
        const json& fj = (*terrain)["waterBodies"][0]["underwaterFog"];
        CHECK(fj.size() == 2);
        CHECK(fj.contains("density"));
        CHECK(fj.contains("deepColor"));
        CHECK_FALSE(fj.contains("shallowColor"));
        CHECK_FALSE(fj.contains("falloff"));

        HorizonWorld loaded;
        reloadEdited(world, loaded, [](json&) {});
        const TerrainComponent* t = firstTerrain(loaded);
        REQUIRE(t != nullptr);
        REQUIRE(t->water.bodies.size() == 1);
        CHECK(t->water.bodies[0].underwaterFog == f);
    }
    SUBCASE("every setting travels")
    {
        water::UnderwaterFogParams f;
        f.shallowColor = { 0.3f, 0.4f, 0.5f }; f.deepColor = { 0.01f, 0.02f, 0.03f };
        f.density = 2.5f; f.falloff = 3.0f;
        REQUIRE(water::setUnderwaterFog(tc, lake, f));
        SceneSerializer ser;
        std::vector<uint8_t> blob;
        REQUIRE(ser.saveToMemory(world, blob));
        HorizonWorld loaded;
        REQUIRE(ser.loadFromMemory(loaded, blob));
        const TerrainComponent* t = firstTerrain(loaded);
        REQUIRE(t != nullptr);
        CHECK(t->water.bodies[0].underwaterFog == f);
    }
}

TEST_CASE("Underwater fog settings: a damaged file cannot leave a number the post pass would trip on")
{
    HorizonWorld world;
    const Entity e = addLandscape(world);
    TerrainComponent& tc = terrainOf(world, e);
    const uint16_t lake = addLake(tc, rect(-10, -10, 10, 10), 2.0f);
    water::UnderwaterFogParams f;
    f.density = 1.1f;   // any change, so the key exists to damage
    REQUIRE(water::setUnderwaterFog(tc, lake, f));

    HorizonWorld loaded;
    reloadEdited(world, loaded, [](json& j)
    {
        json* terrain = findKey(j, "terrain");
        REQUIRE(terrain != nullptr);
        json& fj = (*terrain)["waterBodies"][0]["underwaterFog"];
        fj["density"] = 1.0e9;                                 // far above anything water is
        fj["falloff"] = -5.0;                                  // not a valid exponent
        fj["shallowColor"] = json::array({ 1.0, "x", 3.0 });   // not a colour at all
    });
    const TerrainComponent* t = firstTerrain(loaded);
    REQUIRE(t != nullptr);
    REQUIRE(t->water.bodies.size() == 1);
    const water::UnderwaterFogParams& q = t->water.bodies[0].underwaterFog;
    CHECK(q.density == water::kMaxUnderwaterDensity);
    CHECK(q.falloff == water::kMinUnderwaterFalloff);
    CHECK(q.shallowColor == water::UnderwaterFogParams{}.shallowColor);   // ignored whole, the default stands
}

TEST_CASE("Water physics settings: clampPhysics replaces what is not a number and holds the rest to its bounds")
{
    water::BodyPhysics bad;
    const float notANumber = std::numeric_limits<float>::quiet_NaN();
    const float infinite = std::numeric_limits<float>::infinity();
    bad.density = notANumber; bad.linearDrag = infinite; bad.angularDrag = -infinite; bad.splash = notANumber; bad.flow = { notANumber, infinite };
    const water::BodyPhysics fixed = water::clampPhysics(bad);
    const water::BodyPhysics d;
    CHECK(fixed.density == d.density);
    CHECK(fixed.linearDrag == d.linearDrag);
    CHECK(fixed.angularDrag == d.angularDrag);
    CHECK(fixed.splash == d.splash);
    CHECK(fixed.flow == glm::vec2(0.0f));

    water::BodyPhysics wild;
    wild.density = 0.0f; wild.linearDrag = 50.0f; wild.angularDrag = -1.0f; wild.splash = -3.0f; wild.flow = { 80.0f, -80.0f };
    const water::BodyPhysics held = water::clampPhysics(wild);
    CHECK(held.density == water::kMinDensity);
    CHECK(held.linearDrag == water::kMaxDrag);
    CHECK(held.angularDrag == 0.0f);
    CHECK(held.splash == 0.0f);
    CHECK(held.flow == glm::vec2(water::kMaxFlow, -water::kMaxFlow));

    // In range is left alone, bit for bit.
    water::BodyPhysics fine;
    fine.density = 1025.0f; fine.linearDrag = 1.5f; fine.angularDrag = 0.2f; fine.splash = 2.0f; fine.flow = { 1.0f, -1.0f };
    CHECK(water::clampPhysics(fine) == fine);
}

TEST_CASE("Water physics settings: setPhysics touches the revision, never the surface")
{
    HorizonWorld world;
    const Entity e = addLandscape(world);
    TerrainComponent& tc = terrainOf(world, e);
    const uint16_t lake = addLake(tc, rect(-10, -10, 10, 10), 2.0f);
    tc.dirty       = false;                // as after the terrain has been built
    tc.water.dirty = false;                // …and the surface has caught up
    const uint64_t rev = tc.water.revision;

    CHECK_FALSE(water::setPhysics(tc, 999, water::BodyPhysics{}));      // an unknown body
    CHECK(tc.water.revision == rev);

    water::BodyPhysics p;
    p.density = 900.0f;
    REQUIRE(water::setPhysics(tc, lake, p));
    CHECK(tc.water.revision == rev + 1);                                // the physics notices
    CHECK_FALSE(tc.water.dirty);                                        // the sheet is not rebuilt
    CHECK_FALSE(tc.dirty);                                              // nor a single terrain chunk

    REQUIRE(water::setPhysics(tc, lake, p));                            // the same again: nothing
    CHECK(tc.water.revision == rev + 1);

    water::BodyPhysics wild;
    wild.density = 1.0e12f;
    REQUIRE(water::setPhysics(tc, lake, wild));
    CHECK(tc.water.findBody(lake)->physics.density == water::kMaxDensity);   // held to its bound on the way in
}

TEST_CASE("Water physics settings: they are content - equality, undo delta and its replay")
{
    TerrainComponent a = makeTerrain();
    const uint16_t lake = addLake(a, rect(-10, -10, 10, 10), 2.0f);
    TerrainComponent b = a;
    CHECK(water::sameContent(a.water, b.water));

    water::BodyPhysics p;
    p.flow = { 2.0f, 0.0f };
    REQUIRE(water::setPhysics(b, lake, p));
    CHECK_FALSE(water::sameContent(a.water, b.water));           // only the physics differs

    water::Delta d;
    REQUIRE(water::makeDelta(a.water, b.water, d));
    CHECK_FALSE(d.sameBodies());
    CHECK_FALSE(d.empty());
    CHECK(d.cells.empty());

    TerrainComponent c = a;
    c.water.dirty = false;
    const uint64_t rev = c.water.revision;
    REQUIRE(water::applyDelta(c, d));
    CHECK(c.water.findBody(lake)->physics == p);
    CHECK(c.water.revision > rev);                               // the physics is told…
    CHECK_FALSE(c.water.dirty);                                  // …no sheet is rebuilt for it
    REQUIRE(water::revertDelta(c, d));
    CHECK(water::sameContent(c.water, a.water));
}

// ═════════════════════════════════════════════════════════════════════════════
// What a body looks like from below its surface (Thema 185 Schritt 2)
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Underwater fog settings: clampUnderwaterFog holds density/falloff to their bounds, colour to its default")
{
    water::UnderwaterFogParams wild;
    wild.shallowColor = { std::numeric_limits<float>::quiet_NaN(), 2.0f, -1.0f };
    wild.deepColor    = { 0.5f, 0.5f, 0.5f };   // in range — left alone
    wild.density      = -3.0f;
    wild.falloff      = 50.0f;
    const water::UnderwaterFogParams held = water::clampUnderwaterFog(wild);
    CHECK(held.shallowColor == water::UnderwaterFogParams{}.shallowColor);   // one bad channel drops the whole colour
    CHECK(held.deepColor == wild.deepColor);
    CHECK(held.density == 0.0f);
    CHECK(held.falloff == water::kMaxUnderwaterFalloff);

    // In range is left alone, bit for bit.
    water::UnderwaterFogParams fine;
    fine.shallowColor = { 0.2f, 0.3f, 0.4f };
    fine.deepColor    = { 0.0f, 0.0f, 0.1f };
    fine.density      = 2.0f;
    fine.falloff      = 2.0f;
    CHECK(water::clampUnderwaterFog(fine) == fine);
}

TEST_CASE("Underwater fog settings: setUnderwaterFog touches the revision, never the surface")
{
    HorizonWorld world;
    const Entity e = addLandscape(world);
    TerrainComponent& tc = terrainOf(world, e);
    const uint16_t lake = addLake(tc, rect(-10, -10, 10, 10), 2.0f);
    tc.dirty       = false;
    tc.water.dirty = false;
    const uint64_t rev = tc.water.revision;

    CHECK_FALSE(water::setUnderwaterFog(tc, 999, water::UnderwaterFogParams{}));   // an unknown body
    CHECK(tc.water.revision == rev);

    water::UnderwaterFogParams f;
    f.density = 2.0f;
    REQUIRE(water::setUnderwaterFog(tc, lake, f));
    CHECK(tc.water.revision == rev + 1);
    CHECK_FALSE(tc.water.dirty);     // the sheet is not rebuilt
    CHECK_FALSE(tc.dirty);           // nor a single terrain chunk

    REQUIRE(water::setUnderwaterFog(tc, lake, f));   // the same again: nothing
    CHECK(tc.water.revision == rev + 1);

    water::UnderwaterFogParams wild;
    wild.density = 1.0e6f;
    REQUIRE(water::setUnderwaterFog(tc, lake, wild));
    CHECK(tc.water.findBody(lake)->underwaterFog.density == water::kMaxUnderwaterDensity);
}

TEST_CASE("Underwater fog settings: they are content - equality and undo delta notice them")
{
    TerrainComponent a = makeTerrain();
    const uint16_t lake = addLake(a, rect(-10, -10, 10, 10), 2.0f);
    TerrainComponent b = a;
    CHECK(water::sameContent(a.water, b.water));

    water::UnderwaterFogParams f;
    f.density = 1.5f;
    REQUIRE(water::setUnderwaterFog(b, lake, f));
    CHECK_FALSE(water::sameContent(a.water, b.water));   // only the fog settings differ

    water::Delta d;
    REQUIRE(water::makeDelta(a.water, b.water, d));
    CHECK_FALSE(d.sameBodies());
    CHECK_FALSE(d.empty());

    TerrainComponent c = a;
    REQUIRE(water::applyDelta(c, d));
    CHECK(c.water.findBody(lake)->underwaterFog == f);
    REQUIRE(water::revertDelta(c, d));
    CHECK(water::sameContent(c.water, a.water));
}

// ═════════════════════════════════════════════════════════════════════════════
// The rigid body's two fields
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Rigid body: affectedByWater and density are written only off their defaults")
{
    HorizonWorld world;
    Entity e = world.createEntity("Crate");
    world.addComponent(e, TransformComponent{});
    RigidBodyComponent rb;
    rb.type = RigidBodyType::Dynamic;
    world.addComponent(e, rb);

    SceneSerializer ser;
    SUBCASE("defaults: the scene is as it was before the fields existed")
    {
        const std::string text = sceneText(world);
        CHECK(text.find("\"rigidbody\"") != std::string::npos);
        CHECK(text.find("affectedByWater") == std::string::npos);
        CHECK(text.find("\"density\"") == std::string::npos);

        HorizonWorld loaded;
        reloadEdited(world, loaded, [](json&) {});
        int n = 0;
        for (auto [le, lrb] : loaded.registry().view<RigidBodyComponent>().each())
        {
            (void)le;
            CHECK(lrb.affectedByWater);
            CHECK(lrb.density == 0.0f);
            ++n;
        }
        CHECK(n == 1);
    }
    SUBCASE("changed: both travel")
    {
        world.registry().get<RigidBodyComponent>(e).affectedByWater = false;
        world.registry().get<RigidBodyComponent>(e).density = 640.0f;
        std::vector<uint8_t> blob;
        REQUIRE(ser.saveToMemory(world, blob));
        CHECK(sceneText(world).find("\"affectedByWater\"") != std::string::npos);
        HorizonWorld loaded;
        REQUIRE(ser.loadFromMemory(loaded, blob));
        int n = 0;
        for (auto [le, lrb] : loaded.registry().view<RigidBodyComponent>().each())
        {
            (void)le;
            CHECK_FALSE(lrb.affectedByWater);
            CHECK(lrb.density == 640.0f);
            ++n;
        }
        CHECK(n == 1);
    }
    SUBCASE("a density that is negative reads as 'from the mass'")
    {
        world.registry().get<RigidBodyComponent>(e).density = 5.0f;
        HorizonWorld loaded;
        reloadEdited(world, loaded, [](json& j)
        {
            json* rbj = findKey(j, "rigidbody");
            REQUIRE(rbj != nullptr);
            (*rbj)["density"] = -40.0;
        });
        for (auto [le, lrb] : loaded.registry().view<RigidBodyComponent>().each())
        {
            (void)le;
            CHECK(lrb.density == 0.0f);
        }
    }
}

TEST_CASE("Buoyancy factor: water density over body density, held to what the step can bear")
{
    using HE::PhysicsTuning::buoyancyFactorFor;
    CHECK(buoyancyFactorFor(1000.0f, 500.0f) == doctest::Approx(2.0f));      // wood floats half out
    CHECK(buoyancyFactorFor(1000.0f, 1000.0f) == doctest::Approx(1.0f));     // neutral
    CHECK(buoyancyFactorFor(1000.0f, 2500.0f) == doctest::Approx(0.4f));     // stone sinks
    CHECK(buoyancyFactorFor(1025.0f, 500.0f) > buoyancyFactorFor(1000.0f, 500.0f));   // salt water carries more
    CHECK(buoyancyFactorFor(1000.0f, 1.0f) == HE::PhysicsTuning::kMaxBuoyancyFactor);   // the 1 kg cubic metre
    CHECK(buoyancyFactorFor(1000.0f, 1.0e9f) == HE::PhysicsTuning::kMinBuoyancyFactor);
    // Nothing that is not a density is an exponent of the explicit step.
    CHECK(buoyancyFactorFor(0.0f, 500.0f) == 1.0f);
    CHECK(buoyancyFactorFor(1000.0f, 0.0f) == 1.0f);
    CHECK(buoyancyFactorFor(1000.0f, -3.0f) == 1.0f);
    CHECK(buoyancyFactorFor(std::numeric_limits<float>::quiet_NaN(), 500.0f) == 1.0f);
    CHECK(buoyancyFactorFor(1000.0f, std::numeric_limits<float>::infinity()) == 1.0f);
}

// ═════════════════════════════════════════════════════════════════════════════
// Thema 185 Schritt 3: the refracted-light (caustics) settings of a body
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Underwater caustics settings: the defaults write nothing, a change writes only what changed and round-trips")
{
    HorizonWorld world;
    const Entity e = addLandscape(world);
    TerrainComponent& tc = terrainOf(world, e);
    const uint16_t lake = addLake(tc, rect(-10, -10, 10, 10), 2.0f);

    SUBCASE("defaults leave no trace in the scene")
    {
        REQUIRE(tc.water.findBody(lake)->underwaterCaustics == water::UnderwaterCausticsParams{});
        const std::string text = sceneText(world);
        CHECK(text.find("waterBodies") != std::string::npos);
        CHECK(text.find("underwaterCaustics") == std::string::npos);
    }
    SUBCASE("a change writes only the keys that differ, and round-trips")
    {
        water::UnderwaterCausticsParams c;
        c.intensity = 1.75f;
        c.scale     = 2.5f;
        REQUIRE(water::setUnderwaterCaustics(tc, lake, c));

        json j = json::parse(sceneText(world));
        json* terrain = findKey(j, "terrain");
        REQUIRE(terrain != nullptr);
        const json& cj = (*terrain)["waterBodies"][0]["underwaterCaustics"];
        CHECK(cj.size() == 2);
        CHECK(cj.contains("intensity"));
        CHECK(cj.contains("scale"));
        CHECK_FALSE(cj.contains("speed"));
        CHECK_FALSE(cj.contains("depth"));

        HorizonWorld loaded;
        reloadEdited(world, loaded, [](json&) {});
        const TerrainComponent* t = firstTerrain(loaded);
        REQUIRE(t != nullptr);
        REQUIRE(t->water.bodies.size() == 1);
        CHECK(t->water.bodies[0].underwaterCaustics == c);
        // The fog next to it is untouched by the caustics key.
        CHECK(t->water.bodies[0].underwaterFog == water::UnderwaterFogParams{});
    }
    SUBCASE("every setting travels")
    {
        water::UnderwaterCausticsParams c;
        c.intensity = 3.0f; c.scale = 0.4f; c.speed = 2.0f; c.depth = 33.0f;
        REQUIRE(water::setUnderwaterCaustics(tc, lake, c));
        SceneSerializer ser;
        std::vector<uint8_t> blob;
        REQUIRE(ser.saveToMemory(world, blob));
        HorizonWorld loaded;
        REQUIRE(ser.loadFromMemory(loaded, blob));
        const TerrainComponent* t = firstTerrain(loaded);
        REQUIRE(t != nullptr);
        CHECK(t->water.bodies[0].underwaterCaustics == c);
    }
}

TEST_CASE("Underwater caustics settings: a damaged file cannot leave a number the post pass would trip on")
{
    HorizonWorld world;
    TerrainComponent& tc = terrainOf(world, addLandscape(world));
    const uint16_t lake = addLake(tc, rect(-10, -10, 10, 10), 2.0f);
    water::UnderwaterCausticsParams c;
    c.intensity = 2.0f;
    REQUIRE(water::setUnderwaterCaustics(tc, lake, c));

    HorizonWorld loaded;
    reloadEdited(world, loaded, [](json& j)
    {
        json* terrain = findKey(j, "terrain");
        REQUIRE(terrain != nullptr);
        json& cj = (*terrain)["waterBodies"][0]["underwaterCaustics"];
        cj["intensity"] = 1.0e9;     // far past the bound
        cj["scale"]     = -4.0;      // below the minimum
        cj["speed"]     = "fast";    // not a number: keeps the default
        cj["depth"]     = 0.0;       // below the minimum
    });
    const TerrainComponent* t = firstTerrain(loaded);
    REQUIRE(t != nullptr);
    const water::UnderwaterCausticsParams& q = t->water.bodies[0].underwaterCaustics;
    CHECK(q.intensity == water::kMaxCausticsIntensity);
    CHECK(q.scale     == water::kMinCausticsScale);
    CHECK(q.speed     == water::kDefaultCausticsSpeed);
    CHECK(q.depth     == water::kMinCausticsDepth);
}

TEST_CASE("Underwater caustics settings: clampUnderwaterCaustics holds each knob to its bounds, non-finite to its default")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    water::UnderwaterCausticsParams wild;
    wild.intensity = inf; wild.scale = nan; wild.speed = -1.0f; wild.depth = 1.0e9f;
    const water::UnderwaterCausticsParams c = water::clampUnderwaterCaustics(wild);
    CHECK(c.intensity == water::kDefaultCausticsIntensity);   // not a number: the default
    CHECK(c.scale     == water::kDefaultCausticsScale);
    CHECK(c.speed     == 0.0f);                               // negative: held at 0 (frozen)
    CHECK(c.depth     == water::kMaxCausticsDepth);

    water::UnderwaterCausticsParams ok;
    ok.intensity = 0.0f;   // 0 is a legal value: off
    CHECK(water::clampUnderwaterCaustics(ok).intensity == 0.0f);
}

TEST_CASE("Underwater caustics settings: setUnderwaterCaustics touches the revision, never the surface; they are content")
{
    TerrainComponent a = makeTerrain();
    const uint16_t lake = addLake(a, rect(-10, -10, 10, 10), 2.0f);
    CHECK_FALSE(water::setUnderwaterCaustics(a, 999, water::UnderwaterCausticsParams{}));   // an unknown body

    TerrainComponent b = a;
    CHECK(water::sameContent(a.water, b.water));
    b.dirty       = false;   // addLake (addPolygon) left both set; the question is what the setter does
    b.water.dirty = false;
    const uint64_t rev = b.water.revision;
    water::UnderwaterCausticsParams c;
    c.intensity = 1.5f;
    REQUIRE(water::setUnderwaterCaustics(b, lake, c));
    CHECK(b.water.revision == rev + 1);
    CHECK_FALSE(b.water.dirty);                              // the sheet is not rebuilt
    CHECK_FALSE(b.dirty);                                    // nor a single terrain chunk
    REQUIRE(water::setUnderwaterCaustics(b, lake, c));       // the same again: nothing
    CHECK(b.water.revision == rev + 1);
    CHECK_FALSE(water::sameContent(a.water, b.water));       // only the caustics differ

    water::Delta d;
    REQUIRE(water::makeDelta(a.water, b.water, d));
    CHECK_FALSE(d.empty());
    TerrainComponent undone = a;
    REQUIRE(water::applyDelta(undone, d));
    CHECK(undone.water.findBody(lake)->underwaterCaustics == c);
    REQUIRE(water::revertDelta(undone, d));
    CHECK(water::sameContent(undone.water, a.water));
}
