#include "doctest.h"
#include <HorizonScene/WaterBrush.h>
#include <HorizonScene/WaterFall.h>
#include <HorizonScene/WaterField.h>
#include <HorizonScene/WaterMesh.h>
#include <HorizonScene/WaterSurface.h>
#include <HorizonScene/TerrainSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TerrainChunkComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/WaterSurfaceComponent.h>
#include <ContentManager/ContentManager.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace water = HE::water;
namespace brush = HE::water::brush;

// The waterfall band between two terraces (Thema 232 Schritt 5, WaterFall.h): where two
// bodies touch and the level drops by at least kFallMinDrop there is a ribbon from the
// upper sheet's edge to the lower sheet with a foam apron; where the drop is smaller there
// is nothing. Terrain 64 x 64 m, 65 vertices, 64 water cells (a cell is 1 m).

namespace
{
    // `res` water cells per side: 64 is 1 m cells, 256 is 0.25 m (a stroke's steps then own several cells each,
    // enough for a sheet; at 1 m a step of a 0.5 m brush is a single cell and draws nothing).
    TerrainComponent makeLand(float heightAt0, float slopeX, uint32_t res = 64)
    {
        TerrainComponent tc;
        tc.sizeX = tc.sizeZ = 64.0f;
        tc.resolution = 65;
        tc.dirty = false;
        tc.water.res = res;
        tc.sculptHeights.resize(65u * 65u);
        for (uint32_t z = 0; z < 65; ++z)
            for (uint32_t x = 0; x < 65; ++x)
                tc.sculptHeights[static_cast<size_t>(z) * 65 + x] = heightAt0 + slopeX * (-32.0f + static_cast<float>(x));
        return tc;
    }

    brush::Params cascadeParams()
    {
        brush::Params p;
        p.radius = 0.5f; p.falloff = 0.5f; p.amount = 1.0f;
        p.levelFromGround = true; p.levelOffset = 0.3f;
        p.cascade = true;
        return p;
    }

    brush::Stroke drag(TerrainComponent& tc, glm::vec2 a, glm::vec2 b, const brush::Params& p, int steps)
    {
        brush::Stroke s;
        brush::begin(s, tc, a.x, a.y, p);
        for (int i = 0; i <= steps; ++i)
        {
            const glm::vec2 q = glm::mix(a, b, static_cast<float>(i) / static_cast<float>(steps));
            brush::dab(s, tc, q.x, q.y, p);
        }
        const brush::Stroke done = s;
        brush::end(s, tc);
        return done;
    }

    uint16_t addLake(TerrainComponent& tc, float level, float x0, float z0, float x1, float z1)
    {
        const uint16_t b = tc.water.createBody(level, HE::UUID::generate());
        REQUIRE(water::addPolygon(tc, b, { { x0, z0 }, { x1, z0 }, { x1, z1 }, { x0, z1 } }).ok);
        return b;
    }

    // The band of `upper` as the surface pass builds it.
    struct Band
    {
        water::SurfaceMesh s;
        glm::vec2 flow{ 0.0f };
        bool ok = false;
    };
    Band bandOf(const TerrainComponent& tc, uint16_t upper)
    {
        Band b;
        const water::Body* body = tc.water.findBody(upper);
        REQUIRE(body);
        water::SurfaceOptions opt;
        opt.clipToGround = tc.water.clipToGround;
        opt.shoreOvershoot = tc.water.shoreOvershoot;
        opt.shoreFade = true;
        const water::CellRect cells = water::bodyCells(tc, upper);
        REQUIRE(cells.valid());
        water::Lattice lat;
        REQUIRE(water::buildLattice(tc, upper, cells, lat, opt.clipFor(body->level)));
        b.ok = water::buildFalls(tc, upper, lat, cells, body->level, opt, b.s, b.flow);
        return b;
    }

    // Lowest and highest vertex of the mesh, as y relative to the entity.
    std::pair<float, float> yRange(const StaticMeshAsset& m)
    {
        float lo = 1e9f, hi = -1e9f;
        for (size_t i = 1; i < m.vertices.size(); i += 3) { lo = std::min(lo, m.vertices[i]); hi = std::max(hi, m.vertices[i]); }
        return { lo, hi };
    }
}

TEST_CASE("Water fall: a stroke down a steep slope gets a band under every terrace but the last")
{
    TerrainComponent tc = makeLand(10.0f, 1.0f, 256);                 // 45 degrees, rising toward +X
    const brush::Stroke s = drag(tc, { 20.0f, 0.0f }, { 0.0f, 0.0f }, cascadeParams(), 20);
    REQUIRE(s.bodies.size() > 3u);

    const std::vector<uint16_t> uppers = water::fallUppers(tc);
    // Each step touches the next one down; the lowest has nothing under it.
    REQUIRE(uppers.size() == s.bodies.size() - 1u);
    for (size_t i = 0; i + 1 < s.bodies.size(); ++i)
        CHECK(std::find(uppers.begin(), uppers.end(), s.bodies[i]) != uppers.end());
    CHECK(std::find(uppers.begin(), uppers.end(), s.bodies.back()) == uppers.end());

    for (const uint16_t id : uppers)
    {
        const Band b = bandOf(tc, id);
        REQUIRE(b.ok);
        REQUIRE_FALSE(b.s.empty());
        const auto [lo, hi] = yRange(b.s.mesh);
        // The top is the upper sheet's level (the entity stands there), the foot a metre lower (one step)
        // and a hair above the lower sheet. Ribbons plus bounds' hair are not in the vertices.
        CHECK(hi == doctest::Approx(0.0f).epsilon(1e-4));
        CHECK(lo < -0.5f);
        CHECK(lo >= -(1.0f + 0.05f));
        CHECK(b.s.mesh.vertices.size() % 3 == 0u);
        CHECK(b.s.mesh.normals.size() == b.s.mesh.vertices.size());
        CHECK(b.s.mesh.uvs.size() / 2 == b.s.mesh.vertices.size() / 3);
        CHECK(b.s.triangles > 0u);
        CHECK(b.s.hash != 0u);
        // The water runs downhill: ground rises toward +X, so the current points to -X.
        CHECK(b.flow.x < -0.5f * water::kFallSpeed);
    }
}

TEST_CASE("Water fall: a drop under kFallMinDrop gets no band, at it gets one")
{
    TerrainComponent tc = makeLand(0.0f, 0.0f);
    const uint16_t a = addLake(tc, 5.0f, -10, -10, 0, 10);            // two ponds side by side, sharing the edge x = 0
    const uint16_t b = addLake(tc, 5.0f - 0.3f, 0, -10, 10, 10);
    REQUIRE(water::kFallMinDrop > 0.3f);
    CHECK(water::fallUppers(tc).empty());
    REQUIRE(water::setLevel(tc, b, 5.0f - water::kFallMinDrop - 0.01f));
    const std::vector<uint16_t> uppers = water::fallUppers(tc);
    REQUIRE(uppers.size() == 1u);
    CHECK(uppers[0] == a);                                             // the higher one hangs the band
    REQUIRE(water::setLevel(tc, b, 5.5f));                             // the other way round
    CHECK(water::fallUppers(tc) == std::vector<uint16_t>{ b });
    REQUIRE(water::setLevel(tc, b, 5.0f));                             // same level: ponds, not a fall
    CHECK(water::fallUppers(tc).empty());
}

TEST_CASE("Water fall: bodies that do not touch get no band however far apart in height")
{
    TerrainComponent tc = makeLand(0.0f, 0.0f);
    addLake(tc, 8.0f, -20, -10, -10, 10);
    addLake(tc, 2.0f, 10, -10, 20, 10);
    CHECK(water::fallUppers(tc).empty());
}

TEST_CASE("Water fall: on flat ground the band is a vertical drop that ends a hair above the lower sheet")
{
    TerrainComponent tc = makeLand(0.0f, 0.0f);
    const uint16_t a = addLake(tc, 3.0f, -10, -10, 0, 10);
    addLake(tc, 1.0f, 0, -10, 10, 10);
    const Band b = bandOf(tc, a);
    REQUIRE(b.ok);
    REQUIRE_FALSE(b.s.empty());
    const auto [lo, hi] = yRange(b.s.mesh);
    CHECK(hi == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(lo == doctest::Approx(-2.0f + water::kFallLift).epsilon(1e-3));   // 3 -> 1, lifted
    // A vertical drop: the ribbon's top and its foot are at the same XZ (the apron then runs on).
    CHECK(b.flow.x != 0.0f);                                                // outward normal: toward +X
    CHECK(b.flow.x > 0.0f);
    // The band of the lower pond: it has nothing under it.
    CHECK(bandOf(tc, tc.water.bodies[1].id).s.empty());
}

TEST_CASE("Water fall: the band stays on the edge between the two bodies")
{
    TerrainComponent tc = makeLand(0.0f, 0.0f);
    const uint16_t a = addLake(tc, 3.0f, -10, -10, 0, 10);
    addLake(tc, 1.0f, 0, -10, 10, 10);
    const Band b = bandOf(tc, a);
    REQUIRE_FALSE(b.s.empty());
    // Vertices are terrain-local relative to the centre; the ribbon runs along x = 0, z in -10..10, and
    // the apron reaches kFallApron further toward +X. Nothing lies on the other side.
    float xlo = 1e9f, xhi = -1e9f, zlo = 1e9f, zhi = -1e9f;
    for (size_t i = 0; i + 2 < b.s.mesh.vertices.size(); i += 3)
    {
        const float x = b.s.mesh.vertices[i] + b.s.center.x, z = b.s.mesh.vertices[i + 2] + b.s.center.y;
        xlo = std::min(xlo, x); xhi = std::max(xhi, x); zlo = std::min(zlo, z); zhi = std::max(zhi, z);
    }
    CHECK(xlo >= -0.6f);
    CHECK(xlo <= 0.6f);
    CHECK(xhi == doctest::Approx(water::kFallApron).epsilon(0.5));
    CHECK(zlo >= -10.6f);
    CHECK(zhi <= 10.6f);
    CHECK(zhi - zlo > 15.0f);                                       // along the whole shared edge
}

TEST_CASE("Water fall: the surface pass makes one band entity per upper body and takes it away with the drop")
{
    HorizonWorld world;
    ContentManager cm{ "." };
    const Entity e = world.createEntity("Landscape");
    world.registry().emplace<TransformComponent>(e);
    TerrainComponent land = makeLand(10.0f, 1.0f, 256);
    land.dirty = true;
    world.registry().emplace<TerrainComponent>(e, land);
    TerrainSystem::updateTerrains(world, cm);
    auto& tc = world.registry().get<TerrainComponent>(e);

    const brush::Stroke s = drag(tc, { 20.0f, 0.0f }, { 0.0f, 0.0f }, cascadeParams(), 20);
    REQUIRE(s.bodies.size() > 3u);
    TerrainSystem::updateTerrains(world, cm);

    auto count = [&](bool fall) {
        size_t n = 0;
        for (auto [we, ws] : world.registry().view<WaterSurfaceComponent>().each()) if (ws.fall == fall) ++n;
        return n;
    };
    CHECK(count(false) == s.bodies.size());
    REQUIRE(count(true) == s.bodies.size() - 1u);
    CHECK(WaterSurface::lastStats().falls == s.bodies.size() - 1u);
    for (auto [we, ws, mat, tf] : world.registry().view<WaterSurfaceComponent, MaterialComponent, TransformComponent>().each())
    {
        if (!ws.fall) continue;
        CHECK(HE::isTerrainGenerated(world.registry(), we));                  // never saved, never listed
        const water::Body* upper = tc.water.findBody(ws.body);
        REQUIRE(upper);
        CHECK(tf.position.y == doctest::Approx(upper->level));
        CHECK(ws.level == doctest::Approx(upper->level));
        CHECK(cm.getStaticMesh(ws.meshId) != nullptr);
        // A band runs: the Flow knob is set (downhill = -X here). Which variant carries it needs the water
        // graph, which a bare ContentManager has not (waterMaterialFor then falls back to the plain water).
        bool knob = false;
        for (const auto& o : mat.paramOverrides) if (o.name == "Flow") { knob = true; CHECK(o.value[0] < 0.0f); }
        CHECK(knob);
    }

    // Nothing changed: a second pass rebuilds nothing.
    TerrainSystem::updateTerrains(world, cm);
    CHECK(WaterSurface::lastStats().falls == 0u);

    // Lift the lowest step to within the threshold of its upper neighbour: that band goes, the others stay.
    const uint16_t lowest = s.bodies.back();
    const uint16_t above = s.bodies[s.bodies.size() - 2];
    const float aboveLevel = tc.water.findBody(above)->level;
    REQUIRE(water::setLevel(tc, lowest, aboveLevel - 0.1f));
    TerrainSystem::updateTerrains(world, cm);
    CHECK(count(true) == s.bodies.size() - 2u);
    for (auto [we, ws] : world.registry().view<WaterSurfaceComponent>().each())
        if (ws.fall) CHECK(ws.body != above);

    // The water goes: so do the bands and their meshes.
    for (const uint16_t id : s.bodies) water::removeBody(tc, id);
    TerrainSystem::updateTerrains(world, cm);
    CHECK(count(true) == 0u);
    CHECK(count(false) == 0u);
}

TEST_CASE("Water fall: a landscape without terraces has no band entity at all")
{
    HorizonWorld world;
    ContentManager cm{ "." };
    const Entity e = world.createEntity("Landscape");
    world.registry().emplace<TransformComponent>(e);
    TerrainComponent land = makeLand(0.0f, 0.0f);
    land.dirty = true;
    world.registry().emplace<TerrainComponent>(e, land);
    TerrainSystem::updateTerrains(world, cm);
    auto& tc = world.registry().get<TerrainComponent>(e);
    addLake(tc, 1.0f, -10, -10, 0, 10);
    addLake(tc, 1.2f, 0, -10, 10, 10);                                  // touching, 0.2 m apart: one pond's worth of step
    TerrainSystem::updateTerrains(world, cm);
    size_t sheets = 0, falls = 0;
    for (auto [we, ws] : world.registry().view<WaterSurfaceComponent>().each()) (ws.fall ? falls : sheets)++;
    CHECK(sheets == 2u);
    CHECK(falls == 0u);
}
