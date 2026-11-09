#include "doctest.h"
#include <HorizonScene/WaterBrush.h>
#include <HorizonScene/WaterField.h>
#include <HorizonScene/WaterMesh.h>
#include <HorizonScene/WaterQuery.h>
#include <HorizonScene/WaterSurface.h>
#include <HorizonScene/WaterSpray.h>
#include <HorizonScene/Components/ParticleSystemComponent.h>
#include <HorizonScene/ParticleSystem.h>
#include <HorizonScene/TerrainMeshGenerator.h>
#include <HorizonScene/TerrainSystem.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/WaterSurfaceComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/DefaultAssets.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <tuple>
#include <vector>

namespace water = HE::water;
namespace brush = HE::water::brush;

// A STREAM (Body::stream, WaterMesh.h): the sheet the water brush paints down a slope. It is the ground lifted
// by the body's depth, vertex by vertex, thinning to nothing at its edge; the mesh carries the ground's slope per
// vertex in its UV; the physics query reads its height and its current off the ground at the point.
//
// 64 x 64 m, a vertex on every metre, a 128-cell water grid (a cell is half a metre).

namespace
{
    TerrainComponent rampLand(float slopeX = 1.0f, float base = 10.0f)
    {
        TerrainComponent tc;
        tc.sizeX = tc.sizeZ = 64.0f;
        tc.resolution = 65;
        tc.dirty = false;
        tc.water.res = 128;
        tc.sculptHeights.resize(65u * 65u);
        for (uint32_t z = 0; z < 65; ++z)
            for (uint32_t x = 0; x < 65; ++x)
                tc.sculptHeights[static_cast<size_t>(z) * 65 + x] = base + slopeX * (-32.0f + static_cast<float>(x));
        return tc;
    }

    // A stream painted the way the tool does: a stroke down the ramp, Follow Slope on.
    uint16_t paintStream(TerrainComponent& tc, float depth = 0.3f)
    {
        brush::Params p;
        p.radius = 1.0f; p.falloff = 1.0f; p.amount = 1.0f;
        p.levelFromGround = true; p.levelOffset = depth;
        p.followSlope = true;
        brush::Stroke s;
        brush::begin(s, tc, 16.0f, 0.0f, p);
        for (int i = 0; i <= 60; ++i)
            brush::dab(s, tc, 16.0f - 32.0f * static_cast<float>(i) / 60.0f, 0.0f, p);
        const uint16_t body = s.body;
        brush::end(s, tc);
        return body;
    }

    water::SurfaceOptions optionsFor(float tiltScale = 1.0f)
    {
        water::SurfaceOptions opt;
        opt.uvMetersPerTile = 1.0f;
        opt.clipToGround = true;
        opt.shoreFade = true;
        opt.shoreTiltScale = tiltScale;
        return opt;
    }

    // Vertex i of the mesh in terrain-local (x, y, z): the mesh is relative to the entity at (center.x, level, center.y).
    glm::vec3 vertexOf(const water::SurfaceMesh& s, size_t i)
    {
        return { s.mesh.vertices[i * 3 + 0] + s.center.x, s.mesh.vertices[i * 3 + 1] + s.level, s.mesh.vertices[i * 3 + 2] + s.center.y };
    }
}

TEST_CASE("Water stream mesh: every vertex lies on the ground, lifted by at most the depth, and thin at the edge")
{
    TerrainComponent tc = rampLand();
    const uint16_t body = paintStream(tc, 0.3f);
    REQUIRE(tc.water.findBody(body)->stream);

    water::SurfaceMesh s;
    REQUIRE(water::buildSurface(tc, body, optionsFor(), s));
    REQUIRE_FALSE(s.empty());
    const size_t n = s.mesh.vertices.size() / 3;
    REQUIRE(n > 100);                                      // refined down to the terrain's grid, not a handful of corners

    float thinnest = 1e9f, thickest = -1e9f;
    for (size_t i = 0; i < n; ++i)
    {
        const glm::vec3 v = vertexOf(s, i);
        const float over = v.y - terrainHeightAt(tc, v.x, v.z);
        CAPTURE(i);
        CHECK(over >= water::kStreamLift - 1e-3f);          // never under the ground
        CHECK(over <= 0.3f + 0.075f + water::kStreamLift + 1e-3f);   // never more than the film or the filled channel (dug 0.375 - freeboard)
        thinnest = std::min(thinnest, over);
        thickest = std::max(thickest, over);
    }
    CHECK(thinnest == doctest::Approx(water::kStreamLift).epsilon(0.5));   // the edge has no depth
    CHECK(thickest > 0.25f);                                // the middle has it all

    // The sheet is not flat: it runs down the 45 degree ramp (about 30 m over the stroke), and its box says so.
    CHECK(s.mesh.boundsMax[1] - s.mesh.boundsMin[1] > 10.0f);
    for (size_t i = 0; i < n; ++i)
    {
        CHECK(s.mesh.vertices[i * 3 + 1] >= s.mesh.boundsMin[1] - 1e-4f);
        CHECK(s.mesh.vertices[i * 3 + 1] <= s.mesh.boundsMax[1] + 1e-4f);
    }
    CHECK(s.deepest >= 0.3f);                               // the film, or the filled channel when that is deeper
    CHECK(s.deepest <= 0.4f);
}

TEST_CASE("Water stream mesh: the UV carries the ground's slope in world axes, the normal stays +Y with the soft edge")
{
    TerrainComponent tc = rampLand(1.0f);                  // rising toward +X at 1 m/m
    const uint16_t body = paintStream(tc);
    water::SurfaceMesh s;
    REQUIRE(water::buildSurface(tc, body, optionsFor(), s));
    const size_t n = s.mesh.vertices.size() / 3;
    for (size_t i = 0; i < n; ++i)
    {
        CAPTURE(i);
        // The channel the stream digs adds a little cross slope at its banks; the hill's own slope dominates.
        CHECK(s.mesh.uvs[i * 2 + 0] > 0.5f);                  // uphill is +X, as the ground rises
        CHECK(std::abs(s.mesh.uvs[i * 2 + 1]) < 0.4f);
        CHECK(s.mesh.normals[i * 3 + 1] == doctest::Approx(1.0f));     // the material takes the wave normal off the tilt: +Y stays
        CHECK(s.mesh.normals[i * 3 + 2] == doctest::Approx(0.0f));
    }

    // A landscape turned 90 degrees about Y (local x -> world -z) and scaled to half height: the slope vector follows.
    water::SurfaceOptions turned = optionsFor(0.5f);
    turned.streamTurn = glm::vec4(0.0f, -1.0f, 1.0f, 0.0f);   // local x axis -> world (0, -1); local z axis -> world (1, 0)
    water::SurfaceMesh t;
    REQUIRE(water::buildSurface(tc, body, turned, t));
    CHECK(std::abs(t.mesh.uvs[0]) < 0.25f);
    CHECK(t.mesh.uvs[1] < -0.2f);
}

TEST_CASE("Water stream mesh: the hash sees the ground, so sculpting under the stream rebuilds it")
{
    TerrainComponent tc = rampLand();
    const uint16_t body = paintStream(tc);
    water::SurfaceMesh a, b;
    REQUIRE(water::buildSurface(tc, body, optionsFor(), a));
    CHECK(a.hash != 0u);
    water::SurfaceMesh again;
    REQUIRE(water::buildSurface(tc, body, optionsFor(), again));
    CHECK(again.hash == a.hash);                           // deterministic
    for (uint32_t z = 28; z < 37; ++z)
        for (uint32_t x = 20; x < 45; ++x) tc.sculptHeights[static_cast<size_t>(z) * 65 + x] -= 0.4f;
    REQUIRE(water::buildSurface(tc, body, optionsFor(), b));
    CHECK(b.hash != a.hash);
    // A flat body is a flat sheet: its box is a hair, whatever the ground does.
    TerrainComponent flat = rampLand();
    const uint16_t pond = flat.water.createBody(25.0f);
    REQUIRE(water::addCircle(flat, pond, 0.0f, 0.0f, 3.0f, 1.0f).ok);
    water::SurfaceMesh p;
    REQUIRE(water::buildSurface(flat, pond, optionsFor(), p));
    CHECK(p.mesh.boundsMax[1] - p.mesh.boundsMin[1] < 0.2f);
}

TEST_CASE("Water stream: the physics query reads the height and the current off the ground at the point")
{
    HorizonWorld world;
    Entity e = world.createEntity("Landscape");
    world.addComponent(e, TransformComponent{});
    TerrainComponent tc = rampLand();
    const uint16_t body = paintStream(tc, 0.3f);
    world.addComponent(e, tc);

    water::WaterIndex idx = water::WaterIndex::build(world);
    water::Surface s;
    REQUIRE(idx.surfaceAt(0.0f, 0.0f, 0.0, s));
    CHECK(s.body == body);
    const TerrainComponent& live = world.registry().get<TerrainComponent>(e);
    CHECK(s.level == doctest::Approx(terrainHeightAt(live, 0.0f, 0.0f) + 0.325f).epsilon(0.03));
    CHECK(s.height == doctest::Approx(s.level));           // a film carries no swell
    CHECK(s.flow.x < -1.0f);                               // it runs down the hill (toward -X)
    CHECK(std::abs(s.flow.z) < 1e-2f);
    CHECK(s.depth >= 0.29f);
    CHECK(s.depth <= 0.4f);

    // Two points of one stream, two heights: not the one plane of an ordinary body.
    water::Surface s2;
    REQUIRE(idx.surfaceAt(8.0f, 0.0f, 0.0, s2));
    CHECK(s2.level - s.level == doctest::Approx(8.0f).epsilon(0.02));

    // Beside the stream there is none: no plane that clips.
    water::Surface dry;
    CHECK_FALSE(idx.surfaceAt(0.0f, 20.0f, 0.0, dry));

    // The query's regions follow the ground's range under the stream, not the one reference level.
    std::vector<water::Region> regions;
    idx.regions(regions);
    REQUIRE(regions.size() == 1);
    CHECK(regions[0].max.y - regions[0].min.y > 10.0f);
}

TEST_CASE("Water stream: the surface pass draws it with a mesh that follows the ground and the stream's Flow knob")
{
    HorizonWorld world;
    ContentManager cm{ "." };
    Entity e = world.createEntity("Landscape");
    world.addComponent(e, TransformComponent{});
    TerrainComponent tc = rampLand();
    tc.dirty = true;
    const uint16_t stream = paintStream(tc, 0.3f);
    const uint16_t pond = tc.water.createBody(10.4f);
    REQUIRE(water::addCircle(tc, pond, -26.0f, 20.0f, 2.0f, 1.0f).ok);
    world.addComponent(e, tc);
    TerrainSystem::updateTerrains(world, cm);

    Entity streamSurface = entt::null, pondSurface = entt::null;
    for (auto [en, ws] : world.registry().view<WaterSurfaceComponent>().each())
    {
        if (ws.body == stream) streamSurface = en;
        if (ws.body == pond) pondSurface = en;
    }
    REQUIRE(streamSurface != entt::entity{ entt::null });
    REQUIRE(pondSurface != entt::entity{ entt::null });
    const auto& ws = world.registry().get<WaterSurfaceComponent>(streamSurface);
    CHECK(ws.deepest >= 0.3f);
    CHECK(ws.deepest <= 0.4f);
    // The stream knob exists whatever the material could be made of: Flow.z = the depth, Flow.w = known.
    const auto* mat = world.registry().try_get<MaterialComponent>(streamSurface);
    REQUIRE(mat != nullptr);
    bool hasFlow = false;
    for (const MaterialParamOverride& o : mat->paramOverrides)
        if (o.name == "Flow") { hasFlow = true; CHECK(o.value[2] >= 0.3f); CHECK(o.value[3] == doctest::Approx(1.0f)); }
    CHECK(hasFlow);
    // The pond next to it is the ordinary water with no flow knob.
    const auto* pm = world.registry().try_get<MaterialComponent>(pondSurface);
    REQUIRE(pm != nullptr);
    for (const MaterialParamOverride& o : pm->paramOverrides) CHECK_FALSE(o.name == "Flow");
    // No waterfall band entities any more: one surface per body.
    CHECK(world.registry().view<WaterSurfaceComponent>().size() == 2u);
}

TEST_CASE("Water stream: it digs its own channel, so the water stays at or under the old ground (no hovering)")
{
    TerrainComponent orig = rampLand();
    TerrainComponent tc = orig;
    const uint16_t body = paintStream(tc, 0.3f);              // Dig Bed is OFF in paintStream
    // The ground moved: a channel, deeper than the water.
    float deepest = 0.0f;
    for (size_t i = 0; i < tc.sculptHeights.size(); ++i) deepest = std::max(deepest, orig.sculptHeights[i] - tc.sculptHeights[i]);
    CHECK(deepest >= 0.3f);
    CHECK(deepest <= brush::kStreamChannelFactor * 0.3f + 0.05f);

    water::SurfaceMesh s;
    REQUIRE(water::buildSurface(tc, body, optionsFor(), s));
    const size_t n = s.mesh.vertices.size() / 3;
    REQUIRE(n > 100);
    for (size_t i = 0; i < n; ++i)
    {
        const glm::vec3 v = vertexOf(s, i);
        CAPTURE(i);
        CHECK(v.y <= terrainHeightAt(orig, v.x, v.z) + 0.05f);   // never over the ground the stream was painted on
        CHECK(v.y >= terrainHeightAt(tc, v.x, v.z) + water::kStreamLift - 1e-3f);   // and never under the dug one
    }
}

TEST_CASE("Water stream: where it runs into a lake it reaches into the lake's first cell, so no gap is left")
{
    TerrainComponent tc = rampLand();
    // A lake at the foot of the ramp: its level over the ground there, its east shore at x = -16.5.
    const uint16_t lake = tc.water.createBody(-9.0f);
    REQUIRE(water::addPolygon(tc, lake, { { -32.0f, -12.0f }, { -16.5f, -12.0f }, { -16.5f, 12.0f }, { -32.0f, 12.0f } }).ok);
    const uint16_t body = paintStream(tc, 0.3f);              // ends at x = -16, next to the lake
    REQUIRE(tc.water.findBody(body)->stream);
    REQUIRE(body != lake);

    water::SurfaceMesh s;
    REQUIRE(water::buildSurface(tc, body, optionsFor(), s));
    float minX = 1e9f;
    for (size_t i = 0; i < s.mesh.vertices.size() / 3; ++i) minX = std::min(minX, vertexOf(s, i).x);
    // The stream's own cells end at the lake's border (x = -16.5); its sheet goes on over the lake's first cell.
    {
        water::Lattice L;
        const water::CellRect cr = water::bodyCells(tc, body);
        water::ShoreClip clip; clip.streamDepth = 0.3f;
        const bool okL = water::buildLattice(tc, body, cr, L, clip);
        size_t wet = 0; for (float v : L.v) if (v >= 127.5f) ++wet;
        MESSAGE("lattice ok " << okL << " " << L.w << "x" << L.h << " wet corners " << wet << " contours " << water::extractContours(L).size()
                << " box " << cr.x0 << ".." << cr.x1 << "," << cr.z0 << ".." << cr.z1);
    }
    {
        // Where the stream's own cells end, for the message.
        float ownMin = 1e9f;
        const float cell = 64.0f / 128.0f;
        for (int z = 0; z < 128; ++z)
            for (int x = 0; x < 128; ++x)
            {
                const size_t i = static_cast<size_t>(z) * 128 + x;
                if (tc.water.owner[i] == body && tc.water.coverage[i] >= water::kWet) ownMin = std::min(ownMin, -32.0f + x * cell);
            }
        MESSAGE("stream's own cells start at x = " << ownMin << ", mesh minX = " << minX << ", contours " << s.contours
                << ", polygons " << s.polygons << ", triangles " << s.triangles << ", cells x " << s.cells.x0 << ".." << s.cells.x1);
    }
    CHECK(minX < -16.5f - 0.25f);
    CHECK(minX > -16.5f - 2.0f);                              // one cell, not a second lake
}

TEST_CASE("Water joining: two flat bodies of one level reach into each other's first cell, bodies of other levels do not")
{
    auto twoBodies = [](float levelB)
    {
        TerrainComponent tc = rampLand(0.0f, 0.0f);         // flat ground at y = 0
        const uint16_t a = tc.water.createBody(0.5f);
        const uint16_t b = tc.water.createBody(levelB);
        REQUIRE(water::addPolygon(tc, a, { { -20.0f, -10.0f }, { 0.0f, -10.0f }, { 0.0f, 10.0f }, { -20.0f, 10.0f } }).ok);
        REQUIRE(water::addPolygon(tc, b, { { 0.0f, -10.0f }, { 20.0f, -10.0f }, { 20.0f, 10.0f }, { 0.0f, 10.0f } }).ok);
        return std::make_tuple(tc, a, b);
    };
    auto maxX = [](const water::SurfaceMesh& s)
    {
        float m = -1e9f;
        for (size_t i = 0; i < s.mesh.vertices.size() / 3; ++i) m = std::max(m, vertexOf(s, i).x);
        return m;
    };
    {
        auto [tc, a, b] = twoBodies(0.5f);                  // the same level: one water
        water::SurfaceOptions opt = optionsFor();
        water::SurfaceMesh sa, sb;
        REQUIRE(water::buildSurface(tc, a, opt, sa));
        REQUIRE(water::buildSurface(tc, b, opt, sb));
        CHECK(maxX(sa) > 0.25f);                            // the lower id reaches one cell (0.5 m) into the other's water
        CHECK(maxX(sa) < 1.0f);
        float minXb = 1e9f;
        for (size_t i = 0; i < sb.mesh.vertices.size() / 3; ++i) minXb = std::min(minXb, vertexOf(sb, i).x);
        CHECK(minXb > -0.1f);                               // the higher one does not reach back: the strip is one sheet
    }
    {
        auto [tc, a, b] = twoBodies(1.3f);                  // 0.8 m apart: two waters, side by side
        water::SurfaceOptions opt = optionsFor();
        water::SurfaceMesh sa;
        REQUIRE(water::buildSurface(tc, a, opt, sa));
        CHECK(maxX(sa) < 0.1f);
        (void)b;
    }
}

TEST_CASE("Water joining: the surface pass draws same-level flat neighbours as ONE sheet, other levels as two")
{
    auto surfaces = [](float levelB, size_t& triangles)
    {
        HorizonWorld world;
        ContentManager cm{ "." };
        Entity e = world.createEntity("Landscape");
        world.addComponent(e, TransformComponent{});
        TerrainComponent tc = rampLand(0.0f, 0.0f);
        tc.dirty = true;
        const uint16_t a = tc.water.createBody(0.5f);
        const uint16_t b = tc.water.createBody(levelB);
        REQUIRE(water::addPolygon(tc, a, { { -20.0f, -10.0f }, { 0.0f, -10.0f }, { 0.0f, 10.0f }, { -20.0f, 10.0f } }).ok);
        REQUIRE(water::addPolygon(tc, b, { { 0.0f, -10.0f }, { 20.0f, -10.0f }, { 20.0f, 10.0f }, { 0.0f, 10.0f } }).ok);
        world.addComponent(e, tc);
        TerrainSystem::updateTerrains(world, cm);
        triangles = 0;
        size_t n = 0;
        for (auto [en, ws] : world.registry().view<WaterSurfaceComponent>().each()) { ++n; triangles += ws.triangles; (void)en; }
        return n;
    };
    size_t trisOne = 0, trisTwo = 0;
    CHECK(surfaces(0.5f, trisOne) == 1u);          // one water: one sheet, no seam to leave
    CHECK(surfaces(1.5f, trisTwo) == 2u);          // a metre apart: two waters
    CHECK(trisOne > 0u);
    CHECK(trisTwo > 0u);
}

TEST_CASE("Water stream: the current never points uphill, not even next to the lip of a drop")
{
    // A plateau at y = 20 that ends in a 45 degree face at x = 0: ground = 20 for x < 0, falling toward +X.
    TerrainComponent tc = rampLand(0.0f, 20.0f);
    for (uint32_t z = 0; z < 65; ++z)
        for (uint32_t x = 0; x < 65; ++x)
        {
            const float wx = -32.0f + static_cast<float>(x);
            tc.sculptHeights[static_cast<size_t>(z) * 65 + x] = 20.0f - std::max(0.0f, wx);
        }
    // On the face, and on both sides of the lip (where a wide central difference would point back up the edge).
    for (float x : { -1.5f, -0.5f, 0.5f, 1.5f, 6.0f })
    {
        CAPTURE(x);
        const glm::vec2 f = water::streamFlowAt(tc, x, 0.0f);
        CHECK(f.x >= -1e-3f);                                // along +X (down the face), never toward the plateau
        const glm::vec2 s = water::streamSlopeAt(tc, x, 0.0f);
        CHECK(s.x <= 1e-3f);                                 // the slope vector points uphill (toward -X)
    }
    CHECK(water::streamFlowAt(tc, 6.0f, 0.0f).x > 1.0f);
}

TEST_CASE("Water brush dig: two touching flat bodies of one level are dug as ONE water, no ridge at their border")
{
    TerrainComponent tc = rampLand(0.0f, 1.0f);              // flat ground at y = 1, the level 1.3 above it
    const uint16_t a = tc.water.createBody(1.3f);
    const uint16_t b = tc.water.createBody(1.3f);
    REQUIRE(water::addPolygon(tc, a, { { -20.0f, -8.0f }, { 0.0f, -8.0f }, { 0.0f, 8.0f }, { -20.0f, 8.0f } }).ok);
    REQUIRE(water::addPolygon(tc, b, { { 0.0f, -8.0f }, { 20.0f, -8.0f }, { 20.0f, 8.0f }, { 0.0f, 8.0f } }).ok);
    // Dig under a dab in the middle of A: the bed must not rise at the border x = 0 as if it were a shore.
    brush::Params p;
    p.radius = 6.0f; p.falloff = 2.0f; p.amount = 1.0f;
    p.levelFromGround = false; p.level = 1.3f;
    p.dig = true; p.digDepth = 2.0f; p.bedSlope = 0.5f;
    brush::Stroke s;
    brush::begin(s, tc, -6.0f, 0.0f, p);                      // continues A
    for (int i = 0; i < 6; ++i) brush::dab(s, tc, -6.0f + 1.5f * static_cast<float>(i), 0.0f, p);
    brush::end(s, tc);
    // Near the border (x = -1 and x = +1) the ground has been dug at LEAST as deep as it is 3 m from it: no shore, no ridge.
    const float atBorderA = terrainHeightAt(tc, -1.0f, 0.0f);
    const float atBorderB = terrainHeightAt(tc, 1.0f, 0.0f);
    CHECK(atBorderA <= 1.0f);
    CHECK(atBorderA == doctest::Approx(atBorderB).epsilon(0.2));
}

namespace
{
    // A plateau at y = top for x < 0, a 45 degree face down to y = top - fall, then level ground: a waterfall's profile.
    TerrainComponent fallLand(float fall)
    {
        TerrainComponent tc = rampLand(0.0f, 20.0f);
        for (uint32_t z = 0; z < 65; ++z)
            for (uint32_t x = 0; x < 65; ++x)
            {
                const float wx = -32.0f + static_cast<float>(x);
                tc.sculptHeights[static_cast<size_t>(z) * 65 + x] = 20.0f - std::clamp(wx, 0.0f, fall);
            }
        return tc;
    }

    // The same with a 63 degree face (2 m/m): steep enough that the water falls (WaterSpray.h kFallSlope).
    TerrainComponent cliffLand(float fall)
    {
        TerrainComponent tc = rampLand(0.0f, 20.0f);
        for (uint32_t z = 0; z < 65; ++z)
            for (uint32_t x = 0; x < 65; ++x)
            {
                const float wx = -32.0f + static_cast<float>(x);
                tc.sculptHeights[static_cast<size_t>(z) * 65 + x] = 20.0f - std::clamp(2.0f * wx, 0.0f, fall);
            }
        return tc;
    }

    // A rounded crest (the brush on a steep valley wall, not a cut cliff): 14 m over a tanh profile, at most 2 m/m, its
    // steepest line bending back at the sides (x = -2 + 0.025 z^2), a pool at the foot. `shelf`: for z < 1 the face is cut
    // flat from x = -0.5 to 2.5 and drops ~3 m back to the profile there.
    float crestProfile(float x, float z)
    {
        return 14.0f * (0.5f - 0.5f * std::tanh((x - (-2.0f + 0.025f * z * z)) / 3.5f));
    }
    TerrainComponent crestLand(bool shelf)
    {
        TerrainComponent tc = rampLand(0.0f, 0.0f);
        for (uint32_t z = 0; z < 65; ++z)
            for (uint32_t x = 0; x < 65; ++x)
            {
                const float wx = -32.0f + static_cast<float>(x), wz = -32.0f + static_cast<float>(z);
                float h = crestProfile(wx, wz);
                if (shelf && wx > -0.5f && wx < 2.5f)
                    h += (crestProfile(-0.5f, wz) - h) * (0.5f - 0.5f * std::tanh((wz - 1.0f) / 0.5f));
                h -= 1.6f * std::exp(-((wx - 14.0f) * (wx - 14.0f) + wz * wz) / 40.0f);
                tc.sculptHeights[static_cast<size_t>(z) * 65 + x] = 10.0f + h;
            }
        return tc;
    }

    // A broad stroke over it, the way the tool paints: radius 5, Follow Slope, down the wall into the pool.
    void paintBroad(TerrainComponent& tc)
    {
        brush::Params p;
        p.radius = 5.0f; p.falloff = 1.0f; p.amount = 1.0f;
        p.levelFromGround = true; p.levelOffset = 0.3f;
        p.followSlope = true;
        brush::Stroke s;
        brush::begin(s, tc, -16.0f, 0.0f, p);
        for (int i = 0; i <= 120; ++i) brush::dab(s, tc, -16.0f + 42.0f * static_cast<float>(i) / 120.0f, 0.0f, p);
        brush::end(s, tc);
    }

    // Every fall's foot is where its own curtain lands: run metres on along its current from the middle of its edge, on
    // the water below the face, exactly as wide as the edge.
    void checkFeetUnderTheirCurtains(const std::vector<water::Fall>& falls)
    {
        for (const water::Fall& f : falls)
        {
            const glm::vec2 d = f.foot.pos - f.line.center;
            CHECK(glm::dot(d, f.line.dir) == doctest::Approx(f.line.run).epsilon(0.01));
            CHECK(std::abs(glm::dot(d, f.line.across)) < 0.01f);
            CHECK(f.line.run > 1.0f);
            CHECK(f.foot.halfWidth == doctest::Approx(f.line.halfWidth));
            CHECK(f.foot.surfaceY < f.line.surfaceY - 1.0f);
            CHECK(f.foot.strength > 0.0f);
        }
    }

    uint16_t paintAcross(TerrainComponent& tc, float x0, float x1)
    {
        brush::Params p;
        p.radius = 1.0f; p.falloff = 1.0f; p.amount = 1.0f;
        p.levelFromGround = true; p.levelOffset = 0.3f;
        p.followSlope = true;
        brush::Stroke s;
        brush::begin(s, tc, x0, 0.0f, p);
        for (int i = 0; i <= 80; ++i) brush::dab(s, tc, x0 + (x1 - x0) * static_cast<float>(i) / 80.0f, 0.0f, p);
        const uint16_t body = s.body;
        brush::end(s, tc);
        return body;
    }
}

TEST_CASE("Water spray: fast water meeting calm water at the foot of a fall throws spray, the more the higher the fall")
{
    auto feetFor = [](float fall)
    {
        TerrainComponent tc = fallLand(fall);
        paintAcross(tc, -10.0f, 22.0f);
        return water::findSprayFeet(tc);
    };
    const auto low = feetFor(4.0f);
    const auto high = feetFor(8.0f);
    REQUIRE_FALSE(low.empty());
    REQUIRE_FALSE(high.empty());
    // The foot lies at the bottom of the face (x = fall .. fall + a few m), where the ground turns level.
    CHECK(high.front().pos.x > 4.0f);                       // on the lower face or just past it, where the water lands
    CHECK(high.front().pos.x < 14.0f);
    CHECK(high.front().dir.x > 0.9f);                       // the water arrives in +X, down the face
    CHECK(high.front().drop == doctest::Approx(8.0f).epsilon(0.25));
    CHECK(low.front().drop == doctest::Approx(4.0f).epsilon(0.3));
    CHECK(high.front().strength > low.front().strength);    // a higher fall throws more
    CHECK(high.front().strength <= 1.0f);

    // A gentle slope has no foot: the water never gets fast enough, nor meets calm water.
    {
        TerrainComponent tc = rampLand(0.12f, 10.0f);
        paintAcross(tc, 18.0f, -18.0f);
        CHECK(water::findSprayFeet(tc).empty());
    }
    // And no stream, no feet.
    {
        TerrainComponent tc = fallLand(8.0f);
        CHECK(water::findSprayFeet(tc).empty());
    }
    CHECK(water::sprayStrength(0.5f, 3.0f) == 0.0f);       // a ripple
    CHECK(water::sprayStrength(8.0f, 0.5f) == 0.0f);       // too slow
}

TEST_CASE("Water spray: each fall gets the particle graphs its landscape names, tuned per fall, and loses them again")
{
    HorizonWorld world;
    ContentManager cm{ "." };
#ifdef HE_EDITOR_DEPS_DIR
    // The engine's own spray graphs (fx_gen) and their material, found by UUID like in a session.
    cm.setEngineContentRoot((std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent").string());
    cm.scanContentDirectory();
#endif
    Entity e = world.createEntity("Landscape");
    world.addComponent(e, TransformComponent{});
    TerrainComponent tc = cliffLand(8.0f);
    tc.dirty = true;
    paintAcross(tc, -10.0f, 22.0f);
    world.addComponent(e, tc);
    TerrainSystem::updateTerrains(world, cm);

    std::vector<entt::entity> emitters;
    for (auto [en, ws] : world.registry().view<WaterSurfaceComponent>().each())
        if (ws.spray) emitters.push_back(en);
    REQUIRE_FALSE(emitters.empty());
    // Three kinds, by the asset each plays: the drops at the foot, the mist over it, the curtain from the lip. The
    // surface pass only names the asset and tunes it (ParticleEmitterModifiers); the look is the asset's.
    const water::Field& field = world.registry().get<TerrainComponent>(e).water;
    ParticleSystemComponent* foot = nullptr;
    ParticleSystemComponent* mist = nullptr;
    ParticleSystemComponent* lip = nullptr;
    for (const entt::entity en : emitters)
    {
        auto& c = world.registry().get<ParticleSystemComponent>(en);
        if (c.particleAssetId == field.fallDrops)   foot = &c;
        if (c.particleAssetId == field.fallMist)    mist = &c;
        if (c.particleAssetId == field.fallCurtain) lip  = &c;
        CHECK(c.configDirty);                           // resolved from the asset on the next particle tick
    }
    REQUIRE(foot != nullptr);
    REQUIRE(mist != nullptr);
    REQUIRE(lip != nullptr);                            // a fall of 8 m has a lip
    CHECK(field.fallCurtain == HE::kEngineWaterCurtainParticlesId);
    // Tuned per fall: the curtain leaves along the current (+X), falls for about sqrt(2 * 8 / 9.8) s, is as wide as the edge.
    CHECK(lip->modifiers.heading.x > 0.9f);
    CHECK(lip->modifiers.lifetimeScale == doctest::Approx(std::sqrt(2.0f * 8.0f / 9.8f)).epsilon(0.25));
    CHECK(lip->modifiers.spawnScale.x >= 1.0f);
    CHECK(foot->modifiers.rateScale > 1.0f);            // a fall of 8 m throws more than a ripple
    CHECK(foot->modifiers.heading.x > 0.0f);

#ifdef HE_EDITOR_DEPS_DIR
    // Resolved as the particle tick does: the graph's look with the fall's tuning on top.
    for (const entt::entity en : emitters) ParticleSystem::resolveConfig(world.registry().get<ParticleSystemComponent>(en), cm);
    for (const ParticleSystemComponent* c : { foot, mist, lip })
    {
        // Every one is drawn with the spray material the ASSET names, as a particle band (not the GL/Metal sprite).
        CHECK(c->resolvedConfig.materialAssetId == HE::kEngineWaterSprayMaterialId);
        CHECK(c->resolvedConfig.billboard != static_cast<uint8_t>(HE::ParticleBillboard::Sprite));
    }
    CHECK(mist->resolvedConfig.billboard == static_cast<uint8_t>(HE::ParticleBillboard::Puff));
    CHECK(foot->resolvedConfig.initialVelocity[1] > 1.0f);   // thrown UP
    CHECK(foot->playing);
    CHECK(foot->resolvedConfig.emitRate > 30.0f);
    CHECK(foot->resolvedConfig.initialVelocity[0] > 0.0f);       // and on, the way the water arrives (+X)
    CHECK(foot->resolvedConfig.gravity[1] < 0.0f);
    CHECK(foot->resolvedConfig.endAlpha < foot->resolvedConfig.startAlpha);   // drops fade out
    REQUIRE(mist != nullptr);
    CHECK(mist->resolvedConfig.endAlpha < mist->resolvedConfig.startAlpha);
    CHECK(mist->resolvedConfig.endSize > mist->resolvedConfig.startSize);   // a puff grows
    CHECK(mist->resolvedConfig.lifetimeMax > foot->resolvedConfig.lifetimeMax);
    REQUIRE(lip != nullptr);                                      // a fall of 8 m has a lip
    CHECK(lip->resolvedConfig.initialVelocity[0] > 0.5f);
    CHECK(lip->resolvedConfig.lifetimeMax > 0.8f);                // it falls for about sqrt(2 * 8 / 9.8) s
    CHECK(lip->resolvedConfig.billboard == static_cast<uint8_t>(HE::ParticleBillboard::Streak));
    CHECK(lip->resolvedConfig.fadeIn > 0.0f);     // the top is a soft, even edge, not a row of fresh blobs
    CHECK(glm::length(glm::vec3(lip->resolvedConfig.spawnHalfB[0], lip->resolvedConfig.spawnHalfB[1],
                                lip->resolvedConfig.spawnHalfB[2])) == 0.0f);   // born ON the edge line
    // The curtain is a sheet, not a jet: born across the whole lip (lips stand 1.5 m apart, so the halves overlap),
    // across the current (+X here), and dense.
    const glm::vec3 across(lip->resolvedConfig.spawnHalfA[0], lip->resolvedConfig.spawnHalfA[1], lip->resolvedConfig.spawnHalfA[2]);
    CHECK(glm::length(across) >= 0.75f);
    CHECK(std::abs(across.x) < 0.25f * glm::length(across));   // the current at the lip is +X within a few degrees
    CHECK(lip->resolvedConfig.emitRate >= 140.0f);
    CHECK(lip->resolvedConfig.maxParticles >= static_cast<int>(lip->resolvedConfig.emitRate * lip->resolvedConfig.lifetimeMax * 0.9f));
#endif

    // Another particle graph named by the landscape is played instead; none named, none placed.
    {
        TerrainComponent& live = world.registry().get<TerrainComponent>(e);
        const HE::UUID mine{ 0xAB, 0xCD };
        live.water.fallMist = mine;
        live.water.fallDrops = HE::UUID{};
        ++live.water.revision;
        TerrainSystem::updateTerrains(world, cm);
        int mists = 0, dropsLeft = 0;
        for (auto [en, ws, ps] : world.registry().view<WaterSurfaceComponent, ParticleSystemComponent>().each())
        {
            if (!ws.spray) continue;
            mists     += ps.particleAssetId == mine ? 1 : 0;
            dropsLeft += ps.particleAssetId == HE::kEngineWaterDropsParticlesId ? 1 : 0;
        }
        CHECK(mists > 0);
        CHECK(dropsLeft == 0);
    }

    // The stream is erased: the emitters go with it.
    TerrainComponent& live = world.registry().get<TerrainComponent>(e);
    brush::Params er;
    er.erase = true; er.radius = 40.0f; er.falloff = 1.0f; er.amount = 1.0f;
    brush::Stroke s;
    brush::begin(s, live, 5.0f, 0.0f, er);
    brush::dab(s, live, 5.0f, 0.0f, er);
    brush::end(s, live);
    TerrainSystem::updateTerrains(world, cm);
    size_t left = 0;
    for (auto [en, ws] : world.registry().view<WaterSurfaceComponent>().each()) if (ws.spray) ++left;
    CHECK(left == 0u);
}

TEST_CASE("Water stream: a basin the stroke dug is FILLED up to just under the old ground, with a waterline at its rim")
{
    // Flat ground at y = 10; Follow Slope with Dig Bed 2 m deep: a basin 2 m deep with a stream of water in it.
    TerrainComponent orig = rampLand(0.0f, 10.0f);
    TerrainComponent tc = orig;
    brush::Params p;
    p.radius = 3.0f; p.falloff = 1.0f; p.amount = 1.0f;
    p.levelFromGround = true; p.levelOffset = 0.3f;
    p.followSlope = true;
    p.dig = true; p.digDepth = 2.0f; p.bedSlope = 0.5f;
    brush::Stroke s;
    brush::begin(s, tc, -8.0f, 0.0f, p);
    for (int i = 0; i <= 40; ++i) brush::dab(s, tc, -8.0f + 16.0f * static_cast<float>(i) / 40.0f, 0.0f, p);
    const uint16_t body = s.body;
    brush::end(s, tc);
    const water::Body* b = tc.water.findBody(body);
    REQUIRE(b != nullptr);
    REQUIRE(b->stream);

    // In the middle of the basin the ground is down by about the dig depth, the water up to just under the old ground.
    const float groundMid = terrainHeightAt(tc, 0.0f, 0.0f);
    CHECK(groundMid < 10.0f - 1.0f);                       // dug
    const float surfaceMid = water::streamSurfaceAt(tc, *b, 0.0f, 0.0f);
    CHECK(surfaceMid == doctest::Approx(10.0f - water::kStreamFreeboard).epsilon(0.02));   // flush with the rim, not a film on the floor
    CHECK(water::streamFillAt(tc, 0.0f, 0.0f) > 1.0f);

    water::SurfaceMesh m;
    REQUIRE(water::buildSurface(tc, body, optionsFor(), m));
    CHECK(m.deepest > 1.0f);                               // a camera can really be in this water
    // No vertex above the old ground, and the water's top is level-ish with it (the waterline at the rim).
    float highest = -1e9f;
    for (size_t i = 0; i < m.mesh.vertices.size() / 3; ++i)
    {
        const glm::vec3 v = vertexOf(m, i);
        CHECK(v.y <= terrainHeightAt(orig, v.x, v.z) + water::kStreamLift);   // the lift at the thin edge, nothing more
        highest = std::max(highest, v.y);
    }
    CHECK(highest > 10.0f - 0.2f);
    // The physics agree: a point in the basin is wet and the water is deep there.
    HorizonWorld world;
    Entity e = world.createEntity("Landscape");
    world.addComponent(e, TransformComponent{});
    world.addComponent(e, tc);
    water::WaterIndex idx = water::WaterIndex::build(world);
    water::Surface surf;
    REQUIRE(idx.surfaceAt(0.0f, 0.0f, 0.0, surf));
    CHECK(surf.depth > 1.0f);
    CHECK(surf.level == doctest::Approx(surfaceMid).epsilon(0.02));
}

TEST_CASE("Water falls: the top of a steep run throws a curtain, the taller the run the stronger, a gentle slope none")
{
    auto lipsFor = [](float fall)
    {
        TerrainComponent tc = cliffLand(fall);
        paintAcross(tc, -10.0f, 22.0f);
        return water::findFallLips(tc);
    };
    const auto low = lipsFor(3.0f);
    const auto high = lipsFor(10.0f);
    REQUIRE_FALSE(low.empty());
    REQUIRE_FALSE(high.empty());
    CHECK(high.front().pos.x > -2.0f);
    CHECK(high.front().pos.x < 3.0f);                      // at the top of the face (x = 0)
    CHECK(high.front().height == doctest::Approx(10.0f).epsilon(0.25));
    CHECK(high.front().strength > low.front().strength);
    {
        TerrainComponent tc = rampLand(0.3f, 10.0f);       // 17 degrees: no fall
        paintAcross(tc, 18.0f, -18.0f);
        CHECK(water::findFallLips(tc).empty());
    }
    CHECK(water::fallStrength(0.5f) == 0.0f);
    CHECK(water::fallStrength(30.0f) == 1.0f);
}

TEST_CASE("Water falls: a wide fall is ONE line along its edge, every lip on the edge itself, so the curtain starts flush")
{
    TerrainComponent tc = cliffLand(10.0f);
    brush::Params p;
    p.radius = 4.0f; p.falloff = 1.0f; p.amount = 1.0f;
    p.levelFromGround = true; p.levelOffset = 0.3f;
    p.followSlope = true;
    brush::Stroke s;
    brush::begin(s, tc, -10.0f, 0.0f, p);
    for (int i = 0; i <= 80; ++i) brush::dab(s, tc, -10.0f + 32.0f * static_cast<float>(i) / 80.0f, 0.0f, p);
    brush::end(s, tc);

    const auto lips = water::findFallLips(tc);
    REQUIRE(lips.size() >= 3u);
    // The edge of the face is x = 0 (cliffLand): every lip sits on it, not a grid stride down the face, and so at one
    // height (a lip 1 m down a 2 m/m face would be 2 m lower).
    float xLo = 1e9f, xHi = -1e9f, yLo = 1e9f, yHi = -1e9f;
    for (const auto& l : lips)
    {
        if (l.height < 8.0f) continue;       // the tall run only
        xLo = std::min(xLo, l.pos.x); xHi = std::max(xHi, l.pos.x);
        yLo = std::min(yLo, l.surfaceY); yHi = std::max(yHi, l.surfaceY);
    }
    MESSAGE("lips " << lips.size() << ", x " << xLo << ".." << xHi << ", surface " << yLo << ".." << yHi);
    CHECK(xHi - xLo < 0.6f);
    CHECK(yHi - yLo < 0.6f);
    CHECK(xLo > -1.0f);
    CHECK(xHi < 1.0f);

    const auto lines = water::findFallLines(tc);
    REQUIRE(lines.size() == 1u);
    const water::FallLine& f = lines.front();
    CHECK(f.lips >= 3u);
    CHECK(f.dir.x > 0.9f);                       // the water leaves along +X
    CHECK(std::abs(f.across.x) < 0.2f);          // the line runs across it (Z)
    CHECK(f.halfWidth >= 1.5f);                  // as wide as the stream
    CHECK(std::abs(f.center.x) < 1.0f);          // on the edge
    CHECK(f.height == doctest::Approx(10.0f).epsilon(0.25));
}

TEST_CASE("Water falls: a broad stroke down a rounded wall is ONE fall along its whole top edge, its spray where the curtain lands")
{
    // What the brush makes on a steep valley wall: the water goes over the rounded top and falls the face. The curtain
    // has to start along the top edge across the whole stream (not at a few places part-way down the face), and the
    // spray has to rise where that curtain comes down — nowhere else.
    TerrainComponent tc = crestLand(false);
    paintBroad(tc);
    const auto falls = water::findFalls(tc);
    REQUIRE_FALSE(falls.empty());

    // Every lip on the top edge: upstream of the face's steepest line, none part-way down the face.
    const auto lips = water::findFallLips(tc);
    REQUIRE_FALSE(lips.empty());
    for (const auto& l : lips) CHECK(l.pos.x < -2.0f + 0.025f * l.pos.y * l.pos.y);

    // The pieces lie side by side along the edge (across z), touching, and together as wide as the stream there.
    std::vector<const water::FallLine*> lines;
    for (const auto& f : falls) lines.push_back(&f.line);
    std::sort(lines.begin(), lines.end(), [](const auto* a, const auto* b) { return a->center.y < b->center.y; });
    float covered = 0.0f, worstGap = 0.0f;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        covered += 2.0f * lines[i]->halfWidth;
        CHECK(lines[i]->dir.x > 0.8f);
        CHECK(lines[i]->height > 5.0f);
        if (i > 0)
            worstGap = std::max(worstGap, (lines[i]->center.y - lines[i]->halfWidth) - (lines[i - 1]->center.y + lines[i - 1]->halfWidth));
    }
    MESSAGE(falls.size() << " piece(s) along the edge, " << covered << " m of it, widest gap " << worstGap << " m");
    CHECK(covered >= 6.0f);
    CHECK(worstGap <= 0.5f);

    // One foot per piece, where its curtain lands, and no other spray anywhere.
    checkFeetUnderTheirCurtains(falls);
    CHECK(water::findSprayFeet(tc).size() == falls.size());
}

TEST_CASE("Water falls: a shelf in the face makes a cascade, the upper curtain landing on the shelf, the next from its edge")
{
    TerrainComponent tc = crestLand(true);
    paintBroad(tc);
    const auto falls = water::findFalls(tc);
    REQUIRE_FALSE(falls.empty());
    bool landsOnShelf = false, fromShelfEdge = false, otherSide = false;
    for (const auto& f : falls)
    {
        MESSAGE("fall at (" << f.line.center.x << ", " << f.line.center.y << ") half width " << f.line.halfWidth
                << ", height " << f.line.height << ", lands at x " << f.line.landing.x);
        if (f.line.center.y > 2.0f) otherSide = true;
        if (f.line.center.y > -0.5f) continue;                               // the shelf side (z < 1)
        if (f.line.center.x < -1.0f && f.line.landing.x > -1.0f && f.line.landing.x < 2.5f) landsOnShelf = true;
        if (f.line.center.x > 1.5f && f.line.center.x < 3.5f) fromShelfEdge = true;
    }
    CHECK(landsOnShelf);
    CHECK(fromShelfEdge);
    CHECK(otherSide);                     // the side without the shelf falls too
    checkFeetUnderTheirCurtains(falls);
}

TEST_CASE("Water falls: the curtain's water comes down on its own foot")
{
#ifdef HE_EDITOR_DEPS_DIR
    HorizonWorld world;
    ContentManager cm{ "." };
    cm.setEngineContentRoot((std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent").string());
    cm.scanContentDirectory();
    Entity e = world.createEntity("Landscape");
    world.addComponent(e, TransformComponent{});
    TerrainComponent tc = crestLand(false);
    tc.dirty = true;
    paintBroad(tc);
    world.addComponent(e, tc);
    TerrainSystem::updateTerrains(world, cm);
    HE::propagateTransforms(world);
    const water::Field& field = world.registry().get<TerrainComponent>(e).water;
    const auto falls = water::findFalls(world.registry().get<TerrainComponent>(e));
    REQUIRE_FALSE(falls.empty());
    for (int i = 0; i < 240; ++i) ParticleSystem::update(world, cm, 1.0f / 60.0f, nullptr);

    // The drops of each curtain at the end of their fall (the fall's time old) are over that fall's foot.
    size_t checked = 0, onFoot = 0;
    for (auto [en, ws, ps] : world.registry().view<WaterSurfaceComponent, ParticleSystemComponent>().each())
    {
        if (!ws.spray || ps.particleAssetId != field.fallCurtain) continue;
        const glm::vec3 at = world.registry().get<TransformComponent>(en).position;
        const water::Fall* fall = nullptr;
        for (const auto& f : falls)
            if (!fall || glm::length(f.line.center - glm::vec2(at.x, at.z)) < glm::length(fall->line.center - glm::vec2(at.x, at.z)))
                fall = &f;
        REQUIRE(fall != nullptr);
        const float fallTime = std::sqrt(2.0f * std::max(fall->line.height, 0.5f) / 9.8f);
        for (const Particle& p : ps.particles)
        {
            const float age = p.maxLifetime - p.lifetime;
            if (age < 0.95f * fallTime || age > 1.05f * fallTime) continue;
            ++checked;
            const glm::vec2 d = glm::vec2(p.position.x, p.position.z) - fall->line.center;
            const float along = glm::dot(d, fall->line.dir), across = std::abs(glm::dot(d, fall->line.across));
            if (std::abs(along - fall->line.run) <= 1.5f && across <= fall->foot.halfWidth + 1.0f) ++onFoot;
        }
    }
    MESSAGE(onFoot << " of " << checked << " drops at the end of their fall are over their foot");
    REQUIRE(checked > 20u);
    CHECK(static_cast<double>(onFoot) >= 0.9 * static_cast<double>(checked));
#endif
}

TEST_CASE("Water stream pools: a hollow inside the water fills up to its spill point as a level, still surface")
{
    // A bowl: 10 m at the middle, rising 0.02 x r^2 (12 m at r = 10).
    TerrainComponent tc = rampLand(0.0f, 10.0f);
    for (uint32_t z = 0; z < 65; ++z)
        for (uint32_t x = 0; x < 65; ++x)
        {
            const float wx = -32.0f + static_cast<float>(x), wz = -32.0f + static_cast<float>(z);
            tc.sculptHeights[static_cast<size_t>(z) * 65 + x] = 10.0f + 0.02f * (wx * wx + wz * wz);
        }
    brush::Params p;
    p.radius = 10.0f; p.falloff = 0.5f; p.amount = 1.0f;
    p.levelFromGround = true; p.levelOffset = 0.3f;
    p.followSlope = true;
    brush::Stroke st;
    brush::begin(st, tc, 0.0f, 0.0f, p);
    brush::dab(st, tc, 0.0f, 0.0f, p);
    const uint16_t body = st.body;
    brush::end(st, tc);
    const water::Body* b = tc.water.findBody(body);
    REQUIRE(b != nullptr);
    REQUIRE(b->stream);

    // Level across the bowl, well above the bottom: the hollow is full (to the lowest point of its rim within the water).
    const float mid = water::streamSurfaceAt(tc, *b, 0.0f, 0.0f);
    CHECK(mid > 10.0f + 1.0f);
    CHECK(water::streamSurfaceAt(tc, *b, 4.0f, 0.0f) == doctest::Approx(mid).epsilon(0.01));
    CHECK(water::streamSurfaceAt(tc, *b, 0.0f, -5.0f) == doctest::Approx(mid).epsilon(0.01));
    const auto pool = water::streamPoolOf(tc, body);
    REQUIRE(pool);
    CHECK(water::streamPoolAt(*pool, tc, 0.0f, 0.0f) > 1.0f);
    // Still water: the physics current is zero in it.
    HorizonWorld world;
    Entity e = world.createEntity("Landscape");
    world.addComponent(e, TransformComponent{});
    world.addComponent(e, tc);
    water::WaterIndex idx = water::WaterIndex::build(world);
    water::Surface surf;
    REQUIRE(idx.surfaceAt(2.0f, 1.0f, 0.0, surf));
    CHECK(glm::length(surf.flow) == doctest::Approx(0.0f));
    CHECK(surf.depth > 1.0f);
}

TEST_CASE("Water stream: over the edge of a plateau the water goes down with the ground, it does not carry on level")
{
    // A plateau at 10 m (x < 0), a 45 degree drop to 2 m (x = 0 .. 8), a plain after.
    TerrainComponent tc = rampLand(0.0f, 10.0f);
    for (uint32_t z = 0; z < 65; ++z)
        for (uint32_t x = 0; x < 65; ++x)
        {
            const float wx = -32.0f + static_cast<float>(x);
            tc.sculptHeights[static_cast<size_t>(z) * 65 + x] = 10.0f - std::clamp(wx, 0.0f, 8.0f);
        }
    const uint16_t body = paintAcross(tc, -8.0f, 16.0f);
    const water::Body* b = tc.water.findBody(body);
    REQUIRE(b != nullptr);
    REQUIRE(b->stream);
    for (float x : { -4.0f, 2.0f, 4.0f, 6.0f, 12.0f })
    {
        CAPTURE(x);
        const float over = water::streamSurfaceAt(tc, *b, x, 0.0f) - terrainHeightAt(tc, x, 0.0f);
        CHECK(over > 0.0f);
        CHECK(over < 0.6f);                                 // on the ground, wherever it is: no level sheet hanging past the edge
    }
    water::SurfaceMesh m;
    REQUIRE(water::buildSurface(tc, body, optionsFor(), m));
    for (size_t i = 0; i < m.mesh.vertices.size() / 3; ++i)
    {
        const glm::vec3 v = vertexOf(m, i);
        CHECK(v.y - terrainHeightAt(tc, v.x, v.z) < 0.6f);
    }
}

