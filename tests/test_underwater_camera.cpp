// Thema 185, step 1: "whether and how deep the render camera sits under a water
// surface" (RenderExtractor.h's HE::queryUnderwaterCamera), the value that step 1
// threads from the water-physics immersion detection (WaterQuery.h) into the
// post-processing stage (RenderWorld::underwaterCamera). The query itself is a pure
// function of (world, cameraPos) — no Jolt, no physics step, no renderer — so the
// cases below call it directly; the last test pins the one place it is actually
// wired in: RenderExtractor::extract().
#include "doctest.h"
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderWorld.h>
#include <HorizonScene/WaterQuery.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <limits>
#include <vector>

namespace water = HE::water;

namespace
{
    // Flat ground (no sculpt): the same minimal fixture test_water_query.cpp uses.
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

    Entity addLandscape(HorizonWorld& world)
    {
        Entity e = world.createEntity("Landscape");
        world.addComponent(e, TransformComponent{});
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
}

TEST_CASE("Underwater camera: a world without water is always dry")
{
    HorizonWorld world;
    const UnderwaterCameraData d = HE::queryUnderwaterCamera(world, glm::vec3(0.0f));
    CHECK_FALSE(d.wet);
    CHECK(d.signedDepth == 0.0f);
}

TEST_CASE("Underwater camera: above a lake is dry, inside its column is wet with the right signed depth")
{
    HorizonWorld world;
    TerrainComponent& tc = terrainOf(world, addLandscape(world));
    addLake(tc, rect(-10, -10, 10, 10), 2.0f);   // flat ground at y=0, surface at y=2

    CHECK_FALSE(HE::queryUnderwaterCamera(world, glm::vec3(0.0f, 10.0f, 0.0f)).wet);

    const UnderwaterCameraData d = HE::queryUnderwaterCamera(world, glm::vec3(0.0f, 1.0f, 0.0f));
    CHECK(d.wet);
    CHECK(d.surfaceY == doctest::Approx(2.0f));
    CHECK(d.signedDepth == doctest::Approx(1.0f));   // 2 - 1
}

TEST_CASE("Underwater camera: exactly on the waterline reads a continuous ~0, not NaN and not a special case")
{
    HorizonWorld world;
    TerrainComponent& tc = terrainOf(world, addLandscape(world));
    addLake(tc, rect(-10, -10, 10, 10), 2.0f);

    const UnderwaterCameraData onLine = HE::queryUnderwaterCamera(world, glm::vec3(0.0f, 2.0f, 0.0f));
    CHECK(onLine.wet);
    CHECK(std::isfinite(onLine.signedDepth));
    CHECK(onLine.signedDepth == doctest::Approx(0.0f));

    // A hair below and a hair above the line move the sign, nothing else — there is
    // no band near zero where the answer is ambiguous or undefined.
    const UnderwaterCameraData justUnder = HE::queryUnderwaterCamera(world, glm::vec3(0.0f, 1.999f, 0.0f));
    const UnderwaterCameraData justOver  = HE::queryUnderwaterCamera(world, glm::vec3(0.0f, 2.001f, 0.0f));
    CHECK(justUnder.signedDepth > 0.0f);
    CHECK(justOver.signedDepth  < 0.0f);
}

TEST_CASE("Underwater camera: the shore (outside the lake's footprint) is dry")
{
    HorizonWorld world;
    TerrainComponent& tc = terrainOf(world, addLandscape(world));
    addLake(tc, rect(-10, -10, 10, 10), 2.0f);

    // Well outside the lake polygon, even though the camera sits below the surface's Y.
    CHECK_FALSE(HE::queryUnderwaterCamera(world, glm::vec3(30.0f, 1.0f, 30.0f)).wet);
}

TEST_CASE("Underwater camera: below the lake's own bed is dry, not underwater")
{
    HorizonWorld world;
    TerrainComponent& tc = terrainOf(world, addLandscape(world));
    addLake(tc, rect(-10, -10, 10, 10), 2.0f);   // ground at y=0, column depth 2

    // Deep enough to be inside the solid ground under the lake, not swimming in it.
    CHECK_FALSE(HE::queryUnderwaterCamera(world, glm::vec3(0.0f, -5.0f, 0.0f)).wet);
}

TEST_CASE("Underwater camera: two overlapping lakes, the camera reads the higher surface")
{
    HorizonWorld world;
    const Entity a = addLandscape(world);
    const Entity b = addLandscape(world);
    addLake(terrainOf(world, a), rect(-10, -10, 10, 10), 1.0f);
    addLake(terrainOf(world, b), rect(-10, -10, 10, 10), 4.0f);

    // A camera sitting between the two levels gets the HIGHER one — WaterIndex's own
    // overlap rule (the same the water physics queries by), not the nearer surface.
    const UnderwaterCameraData d = HE::queryUnderwaterCamera(world, glm::vec3(0.0f, 2.0f, 0.0f));
    CHECK(d.wet);
    CHECK(d.surfaceY == doctest::Approx(4.0f));
    CHECK(d.signedDepth == doctest::Approx(2.0f));   // 4 - 2, not 1 - 2
}

TEST_CASE("Underwater camera: stateless — crossing the line in one step reads each side correctly")
{
    HorizonWorld world;
    TerrainComponent& tc = terrainOf(world, addLandscape(world));
    addLake(tc, rect(-10, -10, 10, 10), 2.0f);

    // No frame-to-frame state exists to carry a stale answer across a fast crossing:
    // calling the same query twice, far apart, is exactly as correct as calling it once.
    const UnderwaterCameraData above = HE::queryUnderwaterCamera(world, glm::vec3(0.0f, 7.0f, 0.0f));
    const UnderwaterCameraData below = HE::queryUnderwaterCamera(world, glm::vec3(0.0f, 1.0f, 0.0f));
    CHECK_FALSE(above.wet);
    CHECK(below.wet);
    CHECK(below.signedDepth == doctest::Approx(1.0f));
}

TEST_CASE("Underwater camera: a non-finite position is dry, never NaN")
{
    HorizonWorld world;
    TerrainComponent& tc = terrainOf(world, addLandscape(world));
    addLake(tc, rect(-10, -10, 10, 10), 2.0f);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(HE::queryUnderwaterCamera(world, glm::vec3(nan, 1.0f, 0.0f)).wet);
    CHECK_FALSE(HE::queryUnderwaterCamera(world, glm::vec3(0.0f, nan, 0.0f)).wet);
}

TEST_CASE("Underwater camera: RenderExtractor::extract wires it onto RenderWorld from the resolved camera")
{
    HorizonWorld world;
    TerrainComponent& tc = terrainOf(world, addLandscape(world));
    addLake(tc, rect(-10, -10, 10, 10), 2.0f);

    EditorCameraOverride cam;
    cam.active   = true;
    cam.position = glm::vec3(0.0f, 1.0f, 0.0f);   // 1 m under the 2 m surface
    cam.view     = glm::lookAt(cam.position, cam.position + glm::vec3(0, 0, 1), glm::vec3(0, 1, 0));

    RenderExtractor ex;
    RenderWorld     rw;
    ex.extract(world, rw, 16.0f / 9.0f, &cam);

    CHECK(rw.camera.position == cam.position);
    CHECK(rw.underwaterCamera.wet);
    CHECK(rw.underwaterCamera.surfaceY == doctest::Approx(2.0f));
    CHECK(rw.underwaterCamera.signedDepth == doctest::Approx(1.0f));

    // Move the camera above the surface and re-extract: a fresh walk, not a stale
    // cached one (the editor camera's position is part of RenderExtractor's reuse key).
    cam.position = glm::vec3(0.0f, 9.0f, 0.0f);
    ex.extract(world, rw, 16.0f / 9.0f, &cam);
    CHECK_FALSE(rw.underwaterCamera.wet);
}
