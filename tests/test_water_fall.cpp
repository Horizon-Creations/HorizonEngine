#include "doctest.h"
#include <HorizonScene/WaterBrush.h>
#include <HorizonScene/WaterFall.h>
#include <HorizonScene/WaterField.h>
#include <HorizonScene/WaterMesh.h>
#include <HorizonScene/WaterSurface.h>
#include <HorizonScene/TerrainSystem.h>
#include <HorizonScene/TerrainMeshGenerator.h>
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
    // enough for a sheet; at 1 m a step of a 0.5 m brush is a single cell and draws nothing). `floor`: the
    // ground never goes below it, a flat foot where a stroke down the slope ends in a pool.
    TerrainComponent makeLand(float heightAt0, float slopeX, uint32_t res = 64, float floor = -1e9f)
    {
        TerrainComponent tc;
        tc.sizeX = tc.sizeZ = 64.0f;
        tc.resolution = 65;
        tc.dirty = false;
        tc.water.res = res;
        tc.sculptHeights.resize(65u * 65u);
        for (uint32_t z = 0; z < 65; ++z)
            for (uint32_t x = 0; x < 65; ++x)
                tc.sculptHeights[static_cast<size_t>(z) * 65 + x] =
                    std::max(floor, heightAt0 + slopeX * (-32.0f + static_cast<float>(x)));
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

    // The hill witness (HE_DUMP_WATERLAKE=hill, EditorApplication.cpp): a gaussian mound 24 m high with a
    // ripple on it, 128 m landscape, 0.5 m water cells.
    TerrainComponent hillLand()
    {
        TerrainComponent tc;
        tc.sizeX = tc.sizeZ = 128.0f;
        tc.resolution = 129;
        tc.dirty = false;
        tc.water.res = 256;
        tc.sculptHeights.resize(129u * 129u);
        for (uint32_t zi = 0; zi < 129; ++zi)
            for (uint32_t xi = 0; xi < 129; ++xi)
            {
                const float x = -64.0f + static_cast<float>(xi), z = -64.0f + static_cast<float>(zi);
                tc.sculptHeights[static_cast<size_t>(zi) * 129 + xi] = 24.0f * std::exp(-(x * x + z * z) / (2.0f * 18.0f * 18.0f))
                    + 0.6f * std::sin(0.35f * x + 0.5f * z) + 0.4f * std::sin(0.23f * z - 0.2f * x);
            }
        return tc;
    }
    brush::Params hillParams()
    {
        brush::Params p;
        p.radius = 2.5f; p.falloff = 1.0f; p.amount = 1.0f;
        p.levelFromGround = true; p.levelOffset = 0.3f;
        p.cascade = true; p.autoFlow = true;
        return p;
    }
    // The witness's stroke: from near the top down the +X flank with a meander in z, 201 dabs.
    float hillZ(float t) { return 5.0f * std::sin(t * 4.0f); }
    brush::Stroke hillStroke(TerrainComponent& tc, const brush::Params& p)
    {
        brush::Stroke st;
        brush::begin(st, tc, 3.0f, hillZ(0.0f), p);
        for (int i = 0; i <= 200; ++i)
        {
            const float t = static_cast<float>(i) / 200.0f;
            brush::dab(st, tc, 3.0f + 43.0f * t, hillZ(t), p);
        }
        const brush::Stroke s = st;
        brush::end(st, tc);
        return s;
    }
}

TEST_CASE("Water fall: a stroke down a modelled hill is shallow pools and one-step falls, nothing hangs in the air")
{
    // The hill witness (HE_DUMP_WATERLAKE=hill, EditorApplication.cpp): a gaussian mound 24 m high with a
    // ripple on it, 128 m landscape, 0.5 m water cells, the same stroke from near the top down the +X flank.
    // Before Thema 232 Schritt 11 it made 15 bodies hanging up to 7 m over the ground, a 4.7 m wall of
    // water in the air between two of them (the turquoise slab of the close-up) and 0.7 m curtains between
    // the rest: a staircase of plates.
    TerrainComponent tc = hillLand();
    const brush::Params p = hillParams();
    const brush::Stroke s = hillStroke(tc, p);
    REQUIRE(s.bodies.size() >= 6u);
    REQUIRE(tc.water.bodies.size() == s.bodies.size());

    // How far each step's water stands over the ground, over the cells it holds wet.
    const auto deepest = [&tc](uint16_t id) {
        const water::Body* b = tc.water.findBody(id);
        REQUIRE(b != nullptr);
        const int res = static_cast<int>(tc.water.res);
        float d = -1e9f;
        for (int cz = 0; cz < res; ++cz)
            for (int cx = 0; cx < res; ++cx)
            {
                const size_t i = static_cast<size_t>(cz) * res + cx;
                if (tc.water.owner[i] != id || tc.water.coverage[i] < water::kWet) continue;
                const glm::vec2 c = water::cellCenter(tc, cx, cz);
                d = std::max(d, b->level - terrainHeightAt(tc, c.x, c.y));
            }
        return d;
    };
    const float lip = brush::lipDepth(p);
    for (size_t k = 0; k < s.bodies.size(); ++k)
    {
        CAPTURE(k);
        const float d = deepest(s.bodies[k]);
        CHECK(d > 0.0f);
        // The first step began on the gentle top, the last ends on the gentle foot (both under kTerraceSlope:
        // a pond there stays one body); every step on the flank between is a pool no deeper than its lip.
        if (k == 0 || k + 1 == s.bodies.size()) CHECK(d <= 2.0f);       // was 4.2 and 9 m
        else                                     CHECK(d <= lip + 1e-3f);
    }

    // Every step hangs one band, one step tall, into the next; the lowest none.
    const std::vector<uint16_t> uppers = water::fallUppers(tc);
    CHECK(uppers.size() == s.bodies.size() - 1u);
    for (const uint16_t id : uppers)
    {
        CAPTURE(id);
        const Band b = bandOf(tc, id);
        REQUIRE(b.ok);
        CHECK_FALSE(b.s.empty());
        const auto [lo, hi] = yRange(b.s.mesh);
        CHECK(hi == doctest::Approx(0.0f).epsilon(1e-4));
        CHECK(lo >= -(brush::kTerraceDrop + 0.05f));
    }
}

TEST_CASE("Water fall: a stroke down a steep slope gets a band under every terrace but the last")
{
    // 45 degrees, rising toward +X, flat at 10 m below x = 0: the stroke ends in a pool on the flat. (One
    // that ends on the slope ends at a step's lip: the step under it holds only ground the slope hides,
    // and a ribbon to it would land past the water, so it is not hung.)
    TerrainComponent tc = makeLand(10.0f, 1.0f, 256, 10.0f);
    const brush::Stroke s = drag(tc, { 20.0f, 0.0f }, { -6.0f, 0.0f }, cascadeParams(), 26);
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
        // The top is the upper sheet's level (the entity stands there), the foot one step (kTerraceDrop)
        // lower and a hair above the lower sheet. Ribbons plus bounds' hair are not in the vertices.
        CHECK(hi == doctest::Approx(0.0f).epsilon(1e-4));
        CHECK(lo == doctest::Approx(-brush::kTerraceDrop + water::kFallLift).epsilon(1e-3));
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

TEST_CASE("Water fall: the apron fades into the lower sheet's own shore value, never into open water")
{
    // Flat ground at 0 and the lower pond 1 m deep: its own shore value is 1 - 1/kShoreFadeDepth all over.
    // The apron's far edge used to be 0 (open water), which the material tints as deep water: a dark line
    // across every shallow pool under a fall (Thema 232 Schritt 11).
    TerrainComponent tc = makeLand(0.0f, 0.0f);
    const uint16_t a = addLake(tc, 3.0f, -10, -10, 0, 10);
    addLake(tc, 1.0f, 0, -10, 10, 10);
    const Band b = bandOf(tc, a);
    REQUIRE_FALSE(b.s.empty());
    const StaticMeshAsset& m = b.s.mesh;
    const float lowY = -2.0f + water::kFallLift;
    const float eLow = 1.0f - 1.0f / HE::water::kShoreFadeDepth;
    size_t n = 0;
    for (size_t v = 0; v < m.vertices.size() / 3; ++v)
    {
        if (std::abs(m.vertices[v * 3 + 1] - lowY) > 1e-3f) continue;      // the foot and the apron's far edge
        CHECK(m.normals[v * 3] / HE::water::kShoreNormalGain >= eLow - 1e-3f);
        ++n;
    }
    CHECK(n > 0u);
}

TEST_CASE("Water fall: a ribbon that would run off the water onto dry ground is not hung")
{
    // Ground rises toward +X at 45 degrees (ground = 10 + x). The upper pond holds x = 9..11 at 20.3, the
    // lower body is 2 m lower: its waterline is at x = 8.3, where the ribbon from the upper edge lands.
    SUBCASE("the lower body's water is where the ribbon lands: a band")
    {
        TerrainComponent tc = makeLand(10.0f, 1.0f, 256);
        const uint16_t up = addLake(tc, 20.3f, 9, -3, 11, 3);
        addLake(tc, 18.3f, 6, -3, 9, 3);
        const Band b = bandOf(tc, up);
        CHECK_FALSE(b.s.empty());
        CHECK(yRange(b.s.mesh).first == doctest::Approx(-2.0f + water::kFallLift).epsilon(1e-3));
    }
    SUBCASE("the lower body only holds ground above its level there (the side of a hilltop): no band")
    {
        TerrainComponent tc = makeLand(10.0f, 1.0f, 256);
        const uint16_t up = addLake(tc, 20.3f, 9, -3, 11, 3);
        addLake(tc, 18.3f, 8.6f, -3, 9, 3);                                 // ground 18.6..19, all dry for it
        REQUIRE(water::fallUppers(tc) == std::vector<uint16_t>{ up });      // they touch, and the drop is there
        CHECK(bandOf(tc, up).s.empty());
    }
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
    TerrainComponent land = makeLand(10.0f, 1.0f, 256, 10.0f);       // the slope with a flat foot, as above
    land.dirty = true;
    world.registry().emplace<TerrainComponent>(e, land);
    TerrainSystem::updateTerrains(world, cm);
    auto& tc = world.registry().get<TerrainComponent>(e);

    const brush::Stroke s = drag(tc, { 20.0f, 0.0f }, { -6.0f, 0.0f }, cascadeParams(), 26);
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

TEST_CASE("Water fall: the hill stroke with Dig Bed lies in one channel down the flank, and every step still hangs "
          "its band (Thema 232 Schritt 12)")
{
    TerrainComponent tc = hillLand();
    const TerrainComponent untouched = tc;
    brush::Params p = hillParams();
    p.dig = true; p.digDepth = 1.0f;                 // the panel's defaults otherwise: Slope 0.25, no roughness
    const brush::Stroke s = hillStroke(tc, p);
    REQUIRE(s.bodies.size() >= 6u);
    REQUIRE(tc.water.bodies.size() == s.bodies.size());

    // The channel: along the stroke the ground is down by most of Max Depth (the stream's outline is about
    // 3 m off the axis, 0.75 m of depth at Slope 0.25), on the flank as at its foot; 8 m to the side of the
    // stroke nothing moved; nothing anywhere rose.
    for (const float t : { 0.2f, 0.4f, 0.6f, 0.8f })
    {
        CAPTURE(t);
        const float x = 3.0f + 43.0f * t, z = hillZ(t);
        CHECK(terrainHeightAt(untouched, x, z) - terrainHeightAt(tc, x, z) > 0.5f);
        CHECK(terrainHeightAt(tc, x, z + 8.0f) == terrainHeightAt(untouched, x, z + 8.0f));
    }
    for (size_t i = 0; i < tc.sculptHeights.size(); ++i) CHECK(tc.sculptHeights[i] <= untouched.sculptHeights[i]);

    // The steps on the flank still hold their cells at most the lip deep (the ladder settled on the dug
    // ground). The first is a pond on the gentle top, dug like any pond; the last three are at the foot,
    // where the brush is under Min Slope: the dig there still lowers the ground within its reach, but a
    // dab off the slope settles nothing, so those pools end up to Max Depth deeper (ponds, by the rule).
    const float lip = brush::lipDepth(p);
    const int res = static_cast<int>(tc.water.res);
    for (size_t k = 1; k + 3 < s.bodies.size(); ++k)
    {
        CAPTURE(k);
        const water::Body* b = tc.water.findBody(s.bodies[k]);
        REQUIRE(b != nullptr);
        float d = -1e9f;
        for (int cz = 0; cz < res; ++cz)
            for (int cx = 0; cx < res; ++cx)
            {
                const size_t i = static_cast<size_t>(cz) * res + cx;
                if (tc.water.owner[i] != b->id || tc.water.coverage[i] < water::kWet) continue;
                const glm::vec2 c = water::cellCenter(tc, cx, cz);
                d = std::max(d, b->level - terrainHeightAt(tc, c.x, c.y));
            }
        CHECK(d > 0.0f);
        CHECK(d <= lip + 1e-3f);
    }

    // Every step hangs one band into the next, the lowest none.
    const std::vector<uint16_t> uppers = water::fallUppers(tc);
    CHECK(uppers.size() == s.bodies.size() - 1u);
    for (const uint16_t id : uppers)
    {
        CAPTURE(id);
        const Band b = bandOf(tc, id);
        REQUIRE(b.ok);
        CHECK_FALSE(b.s.empty());
    }
}
