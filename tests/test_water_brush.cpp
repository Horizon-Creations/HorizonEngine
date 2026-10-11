#include "doctest.h"
#include "EditorUndo.h"
#include <HorizonScene/WaterBrush.h>
#include <HorizonScene/WaterField.h>
#include <HorizonScene/WaterSurface.h>
#include <HorizonScene/TerrainSculpt.h>
#include <HorizonScene/TerrainMeshGenerator.h>
#include <HorizonScene/TerrainSystem.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/WaterSurfaceComponent.h>
#include <ContentManager/ContentManager.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace water = HE::water;
namespace brush = HE::water::brush;
namespace sculpt = TerrainSculpt;

// The water brush's strokes (WaterBrush.h): which body a stroke paints into, at
// which level, what an eraser does, what digging does to the ground, and that the
// whole stroke goes through the editor's undo and the scene file. The cursor, the
// pacing and the undo step belong to the Landscape tool; its own test is in
// test_terrain_tools_ui.cpp.
//
// Terrain 64 × 64 m at 65 vertices (a vertex on every whole metre) and a 64-cell
// water grid (a cell is 1 m), so a brush of radius 4 covers whole cells.

namespace
{
    TerrainComponent makeLand(float heightAt0 = 10.0f, float slopeX = 0.0f)
    {
        TerrainComponent tc;
        tc.sizeX = tc.sizeZ = 64.0f;
        tc.resolution = 65;
        tc.dirty = false;                 // a landscape that has been built
        tc.water.res = 64;
        tc.sculptHeights.resize(65u * 65u);
        for (uint32_t z = 0; z < 65; ++z)
            for (uint32_t x = 0; x < 65; ++x)
                tc.sculptHeights[static_cast<size_t>(z) * 65 + x] =
                    heightAt0 + slopeX * (-32.0f + static_cast<float>(x));
        return tc;
    }

    brush::Params params()
    {
        brush::Params p;
        p.radius = 4.0f; p.falloff = 2.0f; p.amount = 1.0f;
        p.levelFromGround = true; p.levelOffset = 0.3f;
        return p;
    }

    // A drag from `a` to `b` in `steps` dabs, the way the tool does it: begin at
    // the first point, then a dab there and one per step. Returns the stroke as it
    // stood before end() reset it (its body, level, dab count).
    brush::Stroke drag(TerrainComponent& tc, glm::vec2 a, glm::vec2 b, const brush::Params& p, int steps = 8)
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

    // No bodies and no grid: nothing the scene file would have to say. (pristine()
    // also wants the default resolution, which these tests do not use.)
    bool noWater(const TerrainComponent& tc) { return tc.water.bodies.empty() && tc.water.coverage.empty(); }

    bool sameBits(const std::vector<float>& a, const std::vector<float>& b)
    {
        return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
    }

    // A lake the way the lake tool will make it: a body with a source spline and a polygon.
    uint16_t addLake(TerrainComponent& tc, float level, float x0, float z0, float x1, float z1)
    {
        const uint16_t b = tc.water.createBody(level, HE::UUID::generate());
        REQUIRE(water::addPolygon(tc, b, { { x0, z0 }, { x1, z0 }, { x1, z1 }, { x0, z1 } }).ok);
        return b;
    }
}

TEST_CASE("Water brush: a stroke on dry ground starts a body at the ground under the first point plus the offset")
{
    TerrainComponent tc = makeLand(10.0f, 0.1f);          // rising to the east: 7.8 at x = -22, 12.2 at x = 22
    const float groundAtStart = terrainHeightAt(tc, -20.0f, 0.0f);

    const brush::Params p = params();
    const brush::Target t = brush::targetAt(tc, -20.0f, 0.0f, p);
    CHECK_FALSE(t.continues);
    CHECK(t.body == water::kNoBody);
    CHECK(t.level == doctest::Approx(groundAtStart + 0.3f));

    brush::Stroke s;
    brush::begin(s, tc, -20.0f, 0.0f, p);
    REQUIRE(s.active);
    CHECK(s.created);
    CHECK_FALSE(s.erase);
    CHECK(s.level == doctest::Approx(groundAtStart + 0.3f));
    REQUIRE(tc.water.bodies.size() == 1);
    CHECK(tc.water.bodies[0].id == s.body);
    CHECK_FALSE(tc.water.bodies[0].fromSpline());            // brush water has no source
    CHECK(tc.water.wetCells() == 0u);                        // begin() paints nothing

    // Dragging uphill: the level stays where the first point put it.
    for (int i = 0; i <= 10; ++i)
        CHECK(brush::dab(s, tc, -20.0f + 4.0f * static_cast<float>(i), 0.0f, p).ok);
    brush::end(s, tc);

    REQUIRE(tc.water.bodies.size() == 1);
    CHECK(tc.water.bodies[0].level == doctest::Approx(groundAtStart + 0.3f));
    CHECK(water::bodyAt(tc, -20.0f, 0.0f) == tc.water.bodies[0].id);
    CHECK(water::bodyAt(tc, 20.0f, 0.0f) == tc.water.bodies[0].id);
    CHECK(water::bodyAt(tc, 0.0f, 20.0f) == water::kNoBody);
    CHECK(water::bodyAt(tc, 30.0f, 30.0f) == water::kNoBody);
    CHECK(tc.water.dirty);
    CHECK(tc.water.revision > 0u);
    // The model's own flags: a water stroke must not make the terrain rebuild its chunks.
    CHECK_FALSE(tc.dirty);
    CHECK_FALSE(tc.regionDirty);
}

TEST_CASE("Water brush: without 'from ground' the level is the number in the panel")
{
    TerrainComponent tc = makeLand(10.0f, 0.1f);
    brush::Params p = params();
    p.levelFromGround = false;
    p.level = 7.25f;
    p.levelOffset = 99.0f;                                    // ignored

    const brush::Stroke s = drag(tc, { -10.0f, 0.0f }, { 10.0f, 0.0f }, p);
    REQUIRE(tc.water.bodies.size() == 1);
    CHECK(tc.water.bodies[0].level == 7.25f);
    CHECK(s.level == 7.25f);
    CHECK(s.dabs == 9u);
}

TEST_CASE("Water brush: pressed on water, the stroke continues that body at its own level")
{
    TerrainComponent tc = makeLand();
    const uint16_t lake = addLake(tc, 5.0f, -20.0f, -10.0f, 0.0f, 10.0f);
    const water::Body before = *tc.water.findBody(lake);
    REQUIRE(water::bodyAt(tc, -10.0f, 0.0f) == lake);

    brush::Params p = params();
    p.levelOffset = 3.0f;                                     // would be 13 for a new body
    brush::Stroke s;
    brush::begin(s, tc, -2.0f, 0.0f, p);                      // on the lake's east bank, still wet
    REQUIRE(s.active);
    CHECK_FALSE(s.created);
    CHECK(s.body == lake);
    CHECK(s.level == 5.0f);
    CHECK(brush::targetAt(tc, -2.0f, 0.0f, p).continues);
    for (int i = 0; i <= 6; ++i) brush::dab(s, tc, -2.0f + 4.0f * static_cast<float>(i), 0.0f, p);
    brush::end(s, tc);

    // One body, same id, level and source; the lake grew east by the stroke.
    REQUIRE(tc.water.bodies.size() == 1);
    CHECK(tc.water.bodies[0].id == lake);
    CHECK(tc.water.bodies[0].level == before.level);
    CHECK(tc.water.bodies[0].sourceSpline == before.sourceSpline);
    CHECK(water::bodyAt(tc, 14.0f, 0.0f) == lake);
    CHECK(water::bodyAt(tc, 14.0f, 20.0f) == water::kNoBody);
}

TEST_CASE("Water brush: a stroke on dry ground next to a lake is a body of its own and takes none of the lake")
{
    TerrainComponent tc = makeLand();
    const uint16_t lake = addLake(tc, 5.0f, -20.0f, -10.0f, 0.0f, 10.0f);
    const std::vector<uint16_t> ownerBefore = tc.water.owner;

    brush::Params p = params();
    brush::Stroke s;
    brush::begin(s, tc, 12.0f, 0.0f, p);
    REQUIRE(s.created);
    CHECK(s.body != lake);
    // Drag west, over the lake: its cells keep their owner, the stroke only adds
    // where the ground was dry.
    for (int i = 0; i <= 8; ++i) brush::dab(s, tc, 12.0f - 4.0f * static_cast<float>(i), 0.0f, p);
    const uint16_t pond = s.body;
    brush::end(s, tc);

    REQUIRE(tc.water.bodies.size() == 2);
    CHECK(water::bodyAt(tc, 12.0f, 0.0f) == pond);
    CHECK(water::bodyAt(tc, -10.0f, 0.0f) == lake);
    const size_t i = static_cast<size_t>(32 + 0) * 64 + (32 - 10);       // a lake cell, owned by it before and after
    CHECK(ownerBefore[i] == lake);
    CHECK(tc.water.owner[i] == lake);
}

TEST_CASE("Water brush: the eraser takes water out of every body under it and creates none")
{
    TerrainComponent tc = makeLand();
    const uint16_t lake = addLake(tc, 5.0f, -20.0f, -10.0f, 0.0f, 10.0f);
    brush::Params paint = params();
    const brush::Stroke made = drag(tc, { 6.0f, 0.0f }, { 14.0f, 0.0f }, paint);
    REQUIRE(tc.water.bodies.size() == 2);
    const uint16_t pond = made.body;
    const uint32_t lakeCells = tc.water.wetCells(lake);

    brush::Params erase = params();
    erase.erase = true;
    erase.radius = 10.0f; erase.falloff = 4.0f;
    brush::Stroke s;
    brush::begin(s, tc, -4.0f, 0.0f, erase);
    REQUIRE(s.active);
    CHECK(s.erase);
    CHECK(s.body == water::kNoBody);
    CHECK_FALSE(s.created);
    CHECK(tc.water.bodies.size() == 2);                       // begin() made nothing
    for (int i = 0; i <= 20; ++i) brush::dab(s, tc, -4.0f + static_cast<float>(i), 0.0f, erase);
    brush::end(s, tc);

    // A path along z = 0 from x = -4 to 16 (radius 10 + falloff 4) dries the pond
    // completely and cuts a notch in the lake's east end.
    CHECK(water::bodyAt(tc, 10.0f, 0.0f) == water::kNoBody);
    CHECK(tc.water.findBody(pond) == nullptr);                // a brush body left empty is forgotten
    REQUIRE(tc.water.findBody(lake) != nullptr);              // a lake keeps its body: its spline can bring it back
    CHECK(tc.water.wetCells(lake) < lakeCells);
    CHECK(tc.water.wetCells(lake) > 0u);
    CHECK(water::bodyAt(tc, -19.0f, 0.0f) == lake);           // the far end, past the brush, is untouched
    CHECK(tc.water.bodies.size() == 1);
}

TEST_CASE("Water brush: erasing where there is no water leaves the landscape exactly as it was")
{
    TerrainComponent tc = makeLand();
    REQUIRE(noWater(tc));
    brush::Params erase = params();
    erase.erase = true;
    drag(tc, { -10.0f, 0.0f }, { 10.0f, 0.0f }, erase);
    CHECK(noWater(tc));                                       // no all-dry grid allocated, nothing to save
    CHECK_FALSE(tc.water.dirty);

    // Paint, then erase it all: the grid is handed back too.
    brush::Params paint = params();
    drag(tc, { -4.0f, 0.0f }, { 4.0f, 0.0f }, paint);
    REQUIRE(tc.water.wetCells() > 0u);
    erase.radius = 40.0f;
    drag(tc, { 0.0f, 0.0f }, { 0.0f, 0.0f }, erase, 1);
    CHECK(tc.water.wetCells() == 0u);
    CHECK(noWater(tc));
}

TEST_CASE("Water brush: a press that lands nowhere leaves nothing behind")
{
    TerrainComponent tc = makeLand();
    brush::Params p = params();
    brush::Stroke s;
    brush::begin(s, tc, 500.0f, 500.0f, p);                    // far off the 64 m landscape
    REQUIRE(s.active);
    const brush::Dab d = brush::dab(s, tc, 500.0f, 500.0f, p);
    CHECK(d.cells == 0u);
    brush::end(s, tc);
    CHECK_FALSE(s.active);
    CHECK(noWater(tc));
}

TEST_CASE("Water brush: the pacing is the amount, painted cells start dry and cross the shoreline over several dabs")
{
    TerrainComponent tc = makeLand();
    brush::Params p = params();
    p.amount = 0.3f;
    brush::Stroke s;
    brush::begin(s, tc, 0.0f, 0.0f, p);
    brush::dab(s, tc, 0.0f, 0.0f, p);
    const size_t centre = 32u * 64u + 32u;
    CHECK(tc.water.coverage[centre] == static_cast<uint8_t>(std::lround(255.0f * 0.3f)));   // 77: below kWet
    CHECK(water::bodyAt(tc, 0.5f, 0.5f) == water::kNoBody);
    brush::dab(s, tc, 0.0f, 0.0f, p);
    brush::dab(s, tc, 0.0f, 0.0f, p);
    CHECK(tc.water.coverage[centre] >= water::kWet);
    CHECK(water::bodyAt(tc, 0.5f, 0.5f) == s.body);
    brush::end(s, tc);
    CHECK_FALSE(s.active);
    CHECK(tc.water.bodies.size() == 1);
}

// ── Digging ──────────────────────────────────────────────────────────────────

TEST_CASE("Water brush dig: the ground under the brush goes down to the bed, the bank eases, a deeper pit stays")
{
    TerrainComponent tc = makeLand(10.0f);                    // flat at 10
    const size_t pitIdx = 32u * 65u + 33u;                    // (x = 1, z = 0), inside the radius
    tc.sculptHeights[pitIdx] = 4.0f;                          // already deeper than the bed
    const std::vector<float> before = tc.sculptHeights;

    brush::Params p = params();
    p.dig = true;
    p.digDepth = 2.0f;
    brush::Stroke s;
    brush::begin(s, tc, 0.0f, 0.0f, p);
    REQUIRE(s.active);
    CHECK(s.level == doctest::Approx(10.3f));
    const brush::Dab d = brush::dab(s, tc, 0.0f, 0.0f, p);
    brush::end(s, tc);

    CHECK(d.ground > 0u);
    REQUIRE(tc.water.bodies.size() == 1);
    const float floorY = tc.water.bodies[0].level - 2.0f;     // the bed: 2 m under the stroke's level (10.3)
    CHECK(tc.water.bodies[0].level == doctest::Approx(10.3f));
    auto h = [&](int ix, int iz) { return tc.sculptHeights[static_cast<size_t>(32 + iz) * 65 + (32 + ix)]; };
    CHECK(h(0, 0) == floorY);                                 // bit equal: assigned, not blended
    CHECK(h(3, 0) == floorY);                                 // inside the radius of 4
    CHECK(h(1, 0) == 4.0f);                                   // the existing pit stays
    CHECK(h(5, 0) < 10.0f);                                   // on the bank (weight 0.5 at 5 m)
    CHECK(h(5, 0) > floorY);
    CHECK(h(5, 0) == doctest::Approx(10.0f + 0.5f * (floorY - 10.0f)));
    CHECK(h(7, 0) == 10.0f);                                  // beyond radius + falloff
    CHECK(h(0, 7) == 10.0f);

    // Never raises: every vertex is at or below where it was.
    for (size_t i = 0; i < before.size(); ++i) CHECK(tc.sculptHeights[i] <= before[i]);

    // The terrain rebuilds only what moved: a rectangle around the brush, not everything.
    CHECK(tc.regionDirty);
    CHECK_FALSE(tc.dirty);
    CHECK(tc.dirtyMinX <= -6.0f);
    CHECK(tc.dirtyMaxX >= 6.0f);
    CHECK(tc.dirtyMaxX < 12.0f);
}

TEST_CASE("Water brush dig: a dab repeated at one spot converges on the bed instead of digging without end")
{
    TerrainComponent tc = makeLand(10.0f);
    brush::Params p = params();
    p.dig = true;
    p.digDepth = 1.5f;
    p.amount = 0.4f;
    brush::Stroke s;
    brush::begin(s, tc, 0.0f, 0.0f, p);
    float last = 1.0e9f;
    for (int i = 0; i < 40; ++i)
    {
        brush::dab(s, tc, 0.0f, 0.0f, p);
        const float c = tc.sculptHeights[32u * 65u + 32u];
        CHECK(c <= last);
        CHECK(c >= 10.3f - 1.5f - 1e-4f);
        last = c;
    }
    CHECK(last == doctest::Approx(10.3f - 1.5f).epsilon(1e-4));
    brush::end(s, tc);
}

TEST_CASE("Water brush dig: the eraser never digs, and a brush with dig off leaves the ground alone")
{
    TerrainComponent tc = makeLand(10.0f);
    const std::vector<float> before = tc.sculptHeights;

    brush::Params off = params();
    off.dig = false;
    drag(tc, { -8.0f, 0.0f }, { 8.0f, 0.0f }, off);
    CHECK(sameBits(tc.sculptHeights, before));
    CHECK_FALSE(tc.regionDirty);

    brush::Params erase = params();
    erase.erase = true;
    erase.dig = true;                                         // a stale checkbox must not dig under an eraser
    erase.digDepth = 3.0f;
    drag(tc, { -8.0f, 0.0f }, { 8.0f, 0.0f }, erase);
    CHECK(sameBits(tc.sculptHeights, before));
}

TEST_CASE("Water brush dig: a landscape that never had baked heights is baked, not flattened")
{
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 64.0f;
    tc.resolution = 65;
    tc.heightScale = 6.0f;
    tc.seed = 11;
    tc.dirty = false;
    tc.water.res = 64;
    REQUIRE(tc.sculptHeights.empty());
    const float far0 = terrainHeightAt(tc, 24.0f, 24.0f);

    brush::Params p = params();
    p.dig = true;
    p.digDepth = 1.0f;
    drag(tc, { 0.0f, 0.0f }, { 0.0f, 0.0f }, p, 1);
    CHECK(tc.sculptHeights.size() == 65u * 65u);
    CHECK(terrainHeightAt(tc, 24.0f, 24.0f) == doctest::Approx(far0).epsilon(1e-4));      // out of reach: the noise shape stays
}

// ── The sheet ────────────────────────────────────────────────────────────────

TEST_CASE("Water brush: a stroke shows up as a water sheet over the painted spot, without rebuilding the landscape")
{
    HorizonWorld world;
    ContentManager cm{ "." };
    const Entity e = world.createEntity("Landscape");
    world.registry().emplace<TransformComponent>(e);
    TerrainComponent land = makeLand(0.0f);
    land.dirty = true;                                        // the first update builds the chunks
    world.registry().emplace<TerrainComponent>(e, land);
    TerrainSystem::updateTerrains(world, cm);
    auto& tc = world.registry().get<TerrainComponent>(e);
    REQUIRE_FALSE(tc.dirty);
    CHECK(world.registry().view<WaterSurfaceComponent>().size() == 0u);

    brush::Params p = params();
    p.levelOffset = 1.0f;                                     // one metre under water at the press
    // Ground is flat at 0 and the level 1.0: the whole painted disc is under the sheet.
    const brush::Stroke s = drag(tc, { 0.0f, 0.0f }, { 4.0f, 0.0f }, p, 4);
    TerrainSystem::updateTerrains(world, cm);

    CHECK_FALSE(tc.dirty);                                    // chunks untouched
    auto view = world.registry().view<WaterSurfaceComponent, TransformComponent>();
    int sheets = 0;
    for (auto [we, ws, wt] : view.each())
    {
        ++sheets;
        CHECK(ws.body == s.body);
        CHECK(wt.position.y == doctest::Approx(1.0f));
        const StaticMeshAsset* mesh = cm.getStaticMesh(ws.meshId);
        REQUIRE(mesh);
        float lo = 1e9f, hi = -1e9f;
        for (size_t i = 0; i + 2 < mesh->vertices.size(); i += 3)
        {
            const float x = mesh->vertices[i] + wt.position.x;
            lo = std::min(lo, x); hi = std::max(hi, x);
        }
        // Brush centres 0..4, radius 4 + falloff 2: the sheet spans about -5 to 9 in x.
        CHECK(lo >= -6.5f);
        CHECK(lo <= -3.0f);
        CHECK(hi >= 7.0f);
        CHECK(hi <= 10.5f);
    }
    CHECK(sheets == 1);
}

// ── Undo and the file ────────────────────────────────────────────────────────

namespace
{
    struct UndoRig
    {
        HorizonWorld world;
        EditorUndo   undo;
        UndoRig() { undo.setWorld(&world); }

        void makeLandscape(TerrainComponent tc)
        {
            const Entity e = world.createEntity("Landscape");
            world.registry().emplace<TransformComponent>(e);
            world.registry().emplace<TerrainComponent>(e, std::move(tc));
        }
        TerrainComponent& landscape()
        {
            auto view = world.registry().view<TerrainComponent>();
            REQUIRE(view.size() == 1u);
            return world.registry().get<TerrainComponent>(*view.begin());
        }
    };
}

TEST_CASE("Water brush undo: a stroke is one step, undo dries the landscape, redo paints it back exactly")
{
    UndoRig rig;
    rig.makeLandscape(makeLand(10.0f, 0.05f));
    const water::Field untouched = rig.landscape().water;

    brush::Params p = params();
    rig.undo.snapshotNow("Paint Water");                       // the tool's protocol: one snapshot, then the stroke
    drag(rig.landscape(), { -12.0f, -6.0f }, { 12.0f, 8.0f }, p, 12);
    const water::Field painted = rig.landscape().water;
    REQUIRE(painted.wetCells() > 0u);
    REQUIRE_FALSE(water::sameContent(painted, untouched));
    CHECK(rig.undo.undoDepth() == 1u);

    REQUIRE(rig.undo.undo());
    CHECK(water::sameContent(rig.landscape().water, untouched));
    CHECK(noWater(rig.landscape()));
    CHECK(rig.undo.redoDepth() == 1u);

    REQUIRE(rig.undo.redo());
    CHECK(water::sameContent(rig.landscape().water, painted));
    CHECK(rig.landscape().water.bodies[0].level == painted.bodies[0].level);
}

TEST_CASE("Water brush undo: a digging stroke gives back the water AND the ground, bit for bit")
{
    UndoRig rig;
    rig.makeLandscape(makeLand(10.0f, 0.05f));
    const std::vector<float> groundBefore = rig.landscape().sculptHeights;

    brush::Params p = params();
    p.dig = true;
    p.digDepth = 2.0f;
    rig.undo.snapshotNow("Paint Water");
    drag(rig.landscape(), { -10.0f, 0.0f }, { 10.0f, 0.0f }, p);
    const std::vector<float> groundDug = rig.landscape().sculptHeights;
    const water::Field painted = rig.landscape().water;
    REQUIRE_FALSE(sameBits(groundDug, groundBefore));

    REQUIRE(rig.undo.undo());
    CHECK(sameBits(rig.landscape().sculptHeights, groundBefore));
    CHECK(noWater(rig.landscape()));
    REQUIRE(rig.undo.redo());
    CHECK(sameBits(rig.landscape().sculptHeights, groundDug));
    CHECK(water::sameContent(rig.landscape().water, painted));
}

TEST_CASE("Water brush: a painted landscape saves and loads with its water, and a stroke can go on after the load")
{
    HorizonWorld world;
    const Entity e = world.createEntity("Landscape");
    world.registry().emplace<TransformComponent>(e);
    world.registry().emplace<TerrainComponent>(e, makeLand(10.0f, 0.05f));
    auto& tc = world.registry().get<TerrainComponent>(e);
    brush::Params p = params();
    drag(tc, { -12.0f, -6.0f }, { 12.0f, 8.0f }, p, 12);
    const water::Field painted = tc.water;
    const float level = painted.bodies[0].level;
    REQUIRE(painted.wetCells() > 0u);

    SceneSerializer ser;
    std::vector<uint8_t> blob;
    REQUIRE(ser.saveToMemory(world, blob));
    HorizonWorld loaded;
    REQUIRE(ser.loadFromMemory(loaded, blob));
    auto view = loaded.registry().view<TerrainComponent>();
    REQUIRE(view.size() == 1u);
    TerrainComponent& lt = loaded.registry().get<TerrainComponent>(*view.begin());
    CHECK(water::sameContent(lt.water, painted));
    CHECK(lt.water.bodies[0].level == level);
    CHECK_FALSE(lt.water.bodies[0].fromSpline());
    CHECK(lt.water.dirty);                                     // the sheet is rebuilt from the cells, never saved

    // Pressed on the loaded water, the brush continues the body that came back.
    const brush::Target t = brush::targetAt(lt, 0.0f, 1.0f, p);
    CHECK(t.continues);
    CHECK(t.body == painted.bodies[0].id);
    CHECK(t.level == level);
    brush::Stroke s;
    brush::begin(s, lt, 0.0f, 1.0f, p);
    CHECK_FALSE(s.created);
    brush::end(s, lt);
}
