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

// ─── Thema 185, step 2: the post pass's own tint/density, resolved in the same
// query from the body's UnderwaterFogParams (WaterField.h) — see RenderWorld.h's
// comment on UnderwaterCameraData for why this is a per-frame scalar (a function of
// the CAMERA's own depth) rather than something the shader derives per pixel.

TEST_CASE("Underwater camera: dry reads density 0 — the post pass's cue to skip entirely")
{
    HorizonWorld world;
    const UnderwaterCameraData dry = HE::queryUnderwaterCamera(world, glm::vec3(0.0f));
    CHECK_FALSE(dry.wet);
    CHECK(dry.density == 0.0f);
}

TEST_CASE("Underwater camera: right at the waterline, the tint is exactly shallowColor")
{
    HorizonWorld world;
    TerrainComponent& tc = terrainOf(world, addLandscape(world));
    const uint16_t id = addLake(tc, rect(-10, -10, 10, 10), 2.0f);

    water::UnderwaterFogParams fog;
    fog.shallowColor = { 0.1f, 0.3f, 0.4f };
    fog.deepColor    = { 0.0f, 0.0f, 0.05f };
    fog.density      = 1.0f;
    REQUIRE(water::setUnderwaterFog(tc, id, fog));

    // signedDepth == 0 here: the ramp (1 - exp(-density * 0)) is exactly 0, so the
    // blend lands on shallowColor bit for bit, whatever falloff is — no special case.
    const UnderwaterCameraData d = HE::queryUnderwaterCamera(world, glm::vec3(0.0f, 2.0f, 0.0f));
    CHECK(d.wet);
    CHECK(d.density == doctest::Approx(1.0f));
    CHECK(d.tintColor.x == doctest::Approx(fog.shallowColor.x));
    CHECK(d.tintColor.y == doctest::Approx(fog.shallowColor.y));
    CHECK(d.tintColor.z == doctest::Approx(fog.shallowColor.z));
}

TEST_CASE("Underwater camera: tint darkens monotonically with depth and saturates toward deepColor")
{
    HorizonWorld world;
    TerrainComponent& tc = terrainOf(world, addLandscape(world));
    const uint16_t id = addLake(tc, rect(-10, -10, 10, 10), 100.0f);   // deep column, ground at y=0

    water::UnderwaterFogParams fog;
    fog.shallowColor = { 1.0f, 1.0f, 1.0f };
    fog.deepColor    = { 0.0f, 0.0f, 0.0f };
    fog.density      = 0.5f;
    fog.falloff      = 1.0f;
    REQUIRE(water::setUnderwaterFog(tc, id, fog));

    float prevR = 2.0f;   // above white, so the first sample is guaranteed "lighter"
    for (float y : { 99.0f, 90.0f, 50.0f, 10.0f, 1.0f })   // signedDepth 1, 10, 50, 90, 99 (ground at y=0)
    {
        const UnderwaterCameraData d = HE::queryUnderwaterCamera(world, glm::vec3(0.0f, y, 0.0f));
        REQUIRE(d.wet);
        CHECK(d.tintColor.x <= prevR);      // never lighter than the previous, shallower sample
        CHECK(d.tintColor.x >= 0.0f);
        prevR = d.tintColor.x;
    }
    CHECK(prevR == doctest::Approx(0.0f).epsilon(0.01));   // deep enough to have all but reached deepColor
    // And the first, shallowest sample is strictly darker than the surface itself —
    // the ramp is actually moving, not flat at 0 or 1 everywhere.
    const float atOne = HE::queryUnderwaterCamera(world, glm::vec3(0.0f, 99.0f, 0.0f)).tintColor.x;
    CHECK(atOne < fog.shallowColor.x);
}

TEST_CASE("Underwater camera: falloff > 1 lingers near shallowColor longer than falloff < 1")
{
    HorizonWorld world;
    TerrainComponent& tc = terrainOf(world, addLandscape(world));
    const uint16_t id = addLake(tc, rect(-10, -10, 10, 10), 20.0f);

    water::UnderwaterFogParams fog;
    fog.shallowColor = { 1.0f, 0.0f, 0.0f };
    fog.deepColor    = { 0.0f, 0.0f, 0.0f };
    fog.density      = 0.3f;

    fog.falloff = 4.0f;
    REQUIRE(water::setUnderwaterFog(tc, id, fog));
    const float lingering = HE::queryUnderwaterCamera(world, glm::vec3(0.0f, 17.0f, 0.0f)).tintColor.x;

    fog.falloff = 0.25f;
    REQUIRE(water::setUnderwaterFog(tc, id, fog));
    const float eager = HE::queryUnderwaterCamera(world, glm::vec3(0.0f, 17.0f, 0.0f)).tintColor.x;

    // Same depth, same density — only the curve shape differs: a high falloff keeps
    // more of shallowColor (x closer to 1) than a low one (x closer to 0).
    CHECK(lingering > eager);
}

TEST_CASE("Underwater camera: resolved tint/density are non-finite-proof like the rest of the query")
{
    HorizonWorld world;
    TerrainComponent& tc = terrainOf(world, addLandscape(world));
    addLake(tc, rect(-10, -10, 10, 10), 2.0f);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const UnderwaterCameraData d = HE::queryUnderwaterCamera(world, glm::vec3(nan, 1.0f, 0.0f));
    CHECK_FALSE(d.wet);
    CHECK(d.density == 0.0f);
}

// ─── Thema 185, step 3: the caustic net's waterline fade-in and its resolved knobs ─
// causticsIntensity is the body's intensity times a smoothstep of the camera's depth
// below the surface: exactly 0 ON the line, so the net can never pop on at the
// waterline, the body's full value from kCausticsFadeInDepth down.

TEST_CASE("Underwater caustics: the fade-in is 0 on and above the waterline, 1 from half a metre down, continuous between")
{
    CHECK(HE::causticsFadeIn(0.0f) == 0.0f);
    CHECK(HE::causticsFadeIn(-3.0f) == 0.0f);
    CHECK(HE::causticsFadeIn(HE::kCausticsFadeInDepth) == doctest::Approx(1.0f));
    CHECK(HE::causticsFadeIn(40.0f) == doctest::Approx(1.0f));
    // Monotonic and with no jump bigger than the step could explain (smoothstep
    // slope is at most 1.5 per unit of t, so a 1 mm step moves it at most 0.003).
    float prev = 0.0f;
    for (float d = 0.0f; d <= 0.6f; d += 0.001f)
    {
        const float f = HE::causticsFadeIn(d);
        CHECK(f >= prev - 1e-6f);
        CHECK(f - prev <= 0.0031f);
        prev = f;
    }
    // Non-finite input never escapes as a non-finite factor.
    CHECK(std::isfinite(HE::causticsFadeIn(std::numeric_limits<float>::quiet_NaN())));
}

TEST_CASE("Underwater caustics: the camera resolves the body's knobs, faded in by depth, and 0 whenever the pass should skip")
{
    HorizonWorld world;
    TerrainComponent& tc = terrainOf(world, addLandscape(world));
    const uint16_t id = addLake(tc, rect(-10, -10, 10, 10), 2.0f);

    water::UnderwaterCausticsParams c;
    c.intensity = 1.5f; c.scale = 2.0f; c.speed = 0.5f; c.depth = 9.0f;
    REQUIRE(water::setUnderwaterCaustics(tc, id, c));

    // Dry (above the surface): the pass is skipped.
    CHECK(HE::queryUnderwaterCamera(world, glm::vec3(0.0f, 10.0f, 0.0f)).causticsIntensity == 0.0f);
    // Exactly on the waterline: wet, yet zero — the fade starts from nothing.
    const UnderwaterCameraData onLine = HE::queryUnderwaterCamera(world, glm::vec3(0.0f, 2.0f, 0.0f));
    CHECK(onLine.wet);
    CHECK(onLine.causticsIntensity == 0.0f);
    // A hair below it: a hair of net, not the whole thing.
    const UnderwaterCameraData hair = HE::queryUnderwaterCamera(world, glm::vec3(0.0f, 1.999f, 0.0f));
    CHECK(hair.causticsIntensity > 0.0f);
    CHECK(hair.causticsIntensity < 0.01f);
    // Well below: the body's own numbers, unchanged.
    const UnderwaterCameraData deep = HE::queryUnderwaterCamera(world, glm::vec3(0.0f, 0.5f, 0.0f));
    CHECK(deep.causticsIntensity == doctest::Approx(1.5f));
    CHECK(deep.causticsScale == doctest::Approx(2.0f));
    CHECK(deep.causticsSpeed == doctest::Approx(0.5f));
    CHECK(deep.causticsDepth == doctest::Approx(9.0f));

    // A body that turned the net off stays at 0 however deep the camera is.
    c.intensity = 0.0f;
    REQUIRE(water::setUnderwaterCaustics(tc, id, c));
    CHECK(HE::queryUnderwaterCamera(world, glm::vec3(0.0f, 0.5f, 0.0f)).causticsIntensity == 0.0f);
}
