#include "doctest.h"
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/WaterField.h>
#include <HorizonScene/WaterLake.h>
#include <HorizonScene/TerrainMeshGenerator.h>
#include <HorizonScene/Components/SplineComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <cmath>
#include <vector>

using HE::api::Ctx;
using HE::api::Value;

// The water group of HE::api (EngineApi.h): the same lakes and brush the editor's
// panels use, reached the way a HorizonCode graph, a Lua or Python script or an
// MCP client reaches them. The model underneath is tested in test_water_lake.cpp;
// what these say is that the rows are there, take WORLD coordinates, land on the
// right entity and agree with each other.

namespace
{
    // A 64 m landscape standing at (100, 20, -50), ground at local 10 (world 30),
    // 1 m water cells, and a round closed spline over its middle.
    struct World
    {
        HorizonWorld world;
        Entity terrain = entt::null;
        Entity spline  = entt::null;

        World()
        {
            terrain = world.createEntity("Landscape");
            TransformComponent t;
            t.position = glm::vec3(100.0f, 20.0f, -50.0f);
            t.dirty = true;
            world.registry().emplace<TransformComponent>(terrain, t);
            TerrainComponent tc;
            tc.sizeX = tc.sizeZ = 64.0f;
            tc.resolution = 65;
            tc.dirty = false;
            tc.water.res = 64;
            tc.sculptHeights.assign(65u * 65u, 10.0f);
            world.registry().emplace<TerrainComponent>(terrain, tc);

            spline = world.createEntity("Shore");
            TransformComponent st;
            st.position = t.position;
            st.dirty = true;
            world.registry().emplace<TransformComponent>(spline, st);
            SplineComponent sc;
            sc.closed = true;
            for (int i = 0; i < 16; ++i)
            {
                const float a = 6.2831853f * static_cast<float>(i) / 16.0f;
                sc.controlPoints.emplace_back(10.0f * std::cos(a), 10.0f, 10.0f * std::sin(a));
            }
            world.registry().emplace<SplineComponent>(spline, std::move(sc));
        }

        TerrainComponent& tc() { return world.registry().get<TerrainComponent>(terrain); }
        Value land() const { return Value::ofInt(static_cast<int>(terrain)); }
        Value line() const { return Value::ofInt(static_cast<int>(spline)); }
    };

    Value F(float v) { return Value::ofFloat(v); }
    Value I(int v)   { return Value::ofInt(v); }
    Value B(bool v)  { return Value::ofBool(v); }

    std::vector<Value> call(Ctx& c, const char* id, std::vector<Value> args)
    {
        const HE::api::ApiFn* fn = HE::api::find(id);
        REQUIRE_MESSAGE(fn != nullptr, id);
        return fn->invoke(c, args);
    }
}

TEST_CASE("Water api: the group exists, reaches the text languages, and reads are pure while edits are not")
{
    for (const char* id : { "water.createLake", "water.createLakeAtGround", "water.reshapeLake", "water.digLake",
                            "water.setLakeLevel", "water.removeLake", "water.convertToLake", "water.paint",
                            "water.erase", "water.bodyAt", "water.levelAt", "water.lakeBody", "water.lakeSpline",
                            "water.wetCells" })
    {
        INFO("row: " << id);
        const HE::api::ApiFn* fn = HE::api::find(id);
        REQUIRE(fn != nullptr);
        CHECK(std::string(fn->category) == "Water");
    }
    CHECK(HE::api::isScriptGroup("water"));

    // Edits write the world, so they are exec rows and MCP will not call them
    // (it has terrain_lake, which goes through the undo gateway); the questions
    // are pure and MCP calls them as they are.
    for (const char* id : { "water.createLake", "water.createLakeAtGround", "water.reshapeLake", "water.digLake",
                            "water.setLakeLevel", "water.removeLake", "water.convertToLake", "water.paint",
                            "water.erase" })
        CHECK(HE::api::find(id)->isExec);
    for (const char* id : { "water.bodyAt", "water.levelAt", "water.lakeBody", "water.lakeSpline", "water.wetCells" })
        CHECK_FALSE(HE::api::find(id)->isExec);

    REQUIRE(HE::api::find("water.levelAt")->results.size() == 2);       // wet, level
    CHECK(HE::api::find("water.createLake")->params.size() == 6);
}

TEST_CASE("Water api: createLake takes a WORLD level, digs the bed, and the reads agree with it")
{
    World w;
    Ctx c{ &w.world, nullptr, nullptr };

    // Ground at world 30. A lake with its surface at world 31 and a 3 m bed.
    const int body = call(c, "water.createLake", { w.land(), w.line(), F(31.0f), B(true), F(3.0f), F(3.0f) })[0].i;
    REQUIRE(body > 0);
    REQUIRE(w.tc().water.bodies.size() == 1);
    CHECK(w.tc().water.bodies[0].level == doctest::Approx(11.0f));        // stored terrain-local: 31 − 20
    CHECK(w.tc().water.bodies[0].fromSpline());
    // The bed lies depth under the surface, in the landscape's own heights: 11 − 3.
    CHECK(terrainHeightAt(w.tc(), 0.0f, 0.0f) == doctest::Approx(8.0f));

    // Reads, in WORLD coordinates: the middle of the lake is at (100, −50).
    CHECK(call(c, "water.bodyAt", { w.land(), F(100.0f), F(-50.0f) })[0].i == body);
    CHECK(call(c, "water.bodyAt", { w.land(), F(100.0f), F(-5.0f) })[0].i == 0);          // 45 m north: dry
    const auto level = call(c, "water.levelAt", { w.land(), F(100.0f), F(-50.0f) });
    REQUIRE(level.size() == 2);
    CHECK(level[0].b);
    CHECK(level[1].f == doctest::Approx(31.0f));
    CHECK_FALSE(call(c, "water.levelAt", { w.land(), F(100.0f), F(-5.0f) })[0].b);
    CHECK(call(c, "water.lakeBody", { w.line() })[0].i == body);
    CHECK(call(c, "water.lakeSpline", { w.land(), I(body) })[0].i == static_cast<int>(w.spline));
    CHECK(call(c, "water.wetCells", { w.land(), I(body) })[0].i > 250);
    CHECK(call(c, "water.wetCells", { w.land(), I(0) })[0].i == call(c, "water.wetCells", { w.land(), I(body) })[0].i);

    // A second create on the same spline is refused with a 0, not a second lake.
    CHECK(call(c, "water.createLake", { w.land(), w.line(), F(32.0f), B(false), F(0.0f), F(0.0f) })[0].i == 0);
    CHECK(w.tc().water.bodies.size() == 1);
}

TEST_CASE("Water api: createLakeAtGround reads the ground before it digs")
{
    World w;
    Ctx c{ &w.world, nullptr, nullptr };
    const int body = call(c, "water.createLakeAtGround", { w.land(), w.line(), F(0.5f), B(true), F(2.0f), F(3.0f) })[0].i;
    REQUIRE(body > 0);
    // Ground 10 local, plus 0.5 above it: the level is 10.5 whatever the bed does to the ground after.
    CHECK(w.tc().water.bodies[0].level == doctest::Approx(10.5f));
    CHECK(terrainHeightAt(w.tc(), 0.0f, 0.0f) == doctest::Approx(8.5f));
}

TEST_CASE("Water api: reshape, dig, level and remove act on the lake of a spline")
{
    World w;
    Ctx c{ &w.world, nullptr, nullptr };
    const int body = call(c, "water.createLake", { w.land(), w.line(), F(32.0f), B(false), F(0.0f), F(0.0f) })[0].i;
    REQUIRE(body > 0);
    const std::vector<float> ground0 = w.tc().sculptHeights;

    // Move the spline 12 m east and ask for the reshape by hand.
    w.world.registry().get<TransformComponent>(w.spline).position.x += 12.0f;
    const int changed = call(c, "water.reshapeLake", { w.line() })[0].i;
    CHECK(changed > 100);
    CHECK(call(c, "water.bodyAt", { w.land(), F(112.0f), F(-50.0f) })[0].i == body);
    CHECK(call(c, "water.reshapeLake", { w.line() })[0].i == 0);               // nothing left to do
    CHECK(w.tc().sculptHeights == ground0);                                    // reshaping never digs

    // The ground is dug by digLake and by nothing else.
    const int lowered = call(c, "water.digLake", { w.line(), F(3.0f), F(3.0f) })[0].i;
    CHECK(lowered > 100);
    CHECK(terrainHeightAt(w.tc(), 12.0f, 0.0f) == doctest::Approx(9.0f));       // surface local 12, bed 3 under it

    // The level is WORLD: raise it a metre.
    CHECK(call(c, "water.setLakeLevel", { w.line(), F(33.0f) })[0].b);
    CHECK(call(c, "water.levelAt", { w.land(), F(112.0f), F(-50.0f) })[1].f == doctest::Approx(33.0f));

    CHECK(call(c, "water.removeLake", { w.line() })[0].b);
    CHECK(call(c, "water.lakeBody", { w.line() })[0].i == 0);
    CHECK(w.tc().water.wetCells() == 0u);
    CHECK_FALSE(call(c, "water.removeLake", { w.line() })[0].b);

    // Not a lake: -1 and false, not a crash.
    CHECK(call(c, "water.reshapeLake", { w.line() })[0].i == -1);
    CHECK(call(c, "water.digLake", { w.line(), F(1.0f), F(1.0f) })[0].i == -1);
    CHECK_FALSE(call(c, "water.setLakeLevel", { w.line(), F(30.0f) })[0].b);
}

TEST_CASE("Water api: the brush and the lake share one model — a dab at the shore widens the lake")
{
    World w;
    Ctx c{ &w.world, nullptr, nullptr };
    const int body = call(c, "water.createLake", { w.land(), w.line(), F(32.0f), B(false), F(0.0f), F(0.0f) })[0].i;
    REQUIRE(body > 0);
    const int before = call(c, "water.wetCells", { w.land(), I(body) })[0].i;

    // A dab pressed on the lake's edge: water at x = 100 + 8, which is inside the lake.
    const int cells = call(c, "water.paint", { w.land(), F(108.0f), F(-50.0f), F(5.0f), F(0.0f) })[0].i;
    CHECK(cells > 0);
    CHECK(w.tc().water.bodies.size() == 1);                                    // it continued the lake, did not start a pond
    CHECK(call(c, "water.wetCells", { w.land(), I(body) })[0].i > before);
    CHECK(call(c, "water.bodyAt", { w.land(), F(112.0f), F(-50.0f) })[0].i == body);

    // On dry ground it starts a pond of its own, 0.3 m over the ground (local 10.3, world 30.3).
    CHECK(call(c, "water.paint", { w.land(), F(125.0f), F(-30.0f), F(4.0f), F(0.0f) })[0].i > 0);
    REQUIRE(w.tc().water.bodies.size() == 2);
    const int pond = call(c, "water.bodyAt", { w.land(), F(125.0f), F(-30.0f) })[0].i;
    CHECK(pond != body);
    CHECK(call(c, "water.lakeSpline", { w.land(), I(pond) })[0].i == 0);       // a pond has no spline
    CHECK(call(c, "water.levelAt", { w.land(), F(125.0f), F(-30.0f) })[1].f == doctest::Approx(30.3f));

    // The eraser takes it away again.
    CHECK(call(c, "water.erase", { w.land(), F(125.0f), F(-30.0f), F(6.0f), F(0.0f) })[0].i > 0);
    CHECK(call(c, "water.bodyAt", { w.land(), F(125.0f), F(-30.0f) })[0].i == 0);
}

TEST_CASE("Water api: convertToLake gives a painted pond a spline that reshapes it")
{
    World w;
    Ctx c{ &w.world, nullptr, nullptr };
    for (int i = 0; i <= 6; ++i)
        call(c, "water.paint", { w.land(), F(100.0f - 12.0f + 4.0f * static_cast<float>(i)), F(-50.0f), F(5.0f), F(0.0f) });
    REQUIRE(w.tc().water.bodies.size() == 1);
    const int pond = static_cast<int>(w.tc().water.bodies[0].id);
    const int cellsBefore = call(c, "water.wetCells", { w.land(), I(pond) })[0].i;
    REQUIRE(cellsBefore > 100);

    const int spline = call(c, "water.convertToLake", { w.land(), I(pond) })[0].i;
    REQUIRE(spline != 0);
    CHECK(call(c, "water.lakeBody", { I(spline) })[0].i == pond);
    CHECK(call(c, "water.lakeSpline", { w.land(), I(pond) })[0].i == spline);
    const int cellsAfter = call(c, "water.wetCells", { w.land(), I(pond) })[0].i;
    CHECK(std::abs(cellsAfter - cellsBefore) < cellsBefore / 4);               // smoothed, not a different pond

    // Not twice, and not a body that is not there.
    CHECK(call(c, "water.convertToLake", { w.land(), I(pond) })[0].i == 0);
    CHECK(call(c, "water.convertToLake", { w.land(), I(99) })[0].i == 0);
}

TEST_CASE("Water api: without a world or a landscape every row is neutral")
{
    Ctx none{};
    CHECK(call(none, "water.createLake", { I(1), I(2), F(0), B(true), F(1), F(1) })[0].i == 0);
    CHECK(call(none, "water.bodyAt", { I(1), F(0), F(0) })[0].i == 0);
    CHECK_FALSE(call(none, "water.levelAt", { I(1), F(0), F(0) })[0].b);
    CHECK(call(none, "water.reshapeLake", { I(1) })[0].i == -1);
    CHECK(call(none, "water.paint", { I(1), F(0), F(0), F(1), F(0) })[0].i == 0);

    World w;
    Ctx c{ &w.world, nullptr, nullptr };
    const Value notLand = Value::ofInt(static_cast<int>(w.spline));           // a spline is no landscape
    CHECK(call(c, "water.bodyAt", { notLand, F(0), F(0) })[0].i == 0);
    CHECK(call(c, "water.createLake", { notLand, w.line(), F(0), B(true), F(1), F(1) })[0].i == 0);
    const Value notSpline = Value::ofInt(static_cast<int>(w.terrain));        // a landscape is no spline
    CHECK(call(c, "water.createLake", { w.land(), notSpline, F(0), B(true), F(1), F(1) })[0].i == 0);
    CHECK(w.tc().water.bodies.empty());
}
