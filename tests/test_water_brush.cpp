#include "doctest.h"
#include "EditorUndo.h"
#include <HorizonScene/WaterBrush.h>
#include <HorizonScene/WaterField.h>
#include <HorizonScene/WaterMesh.h>
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
#include <limits>
#include <string>
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

TEST_CASE("Water brush dig: the ground under the brush goes down to the bed, the bank eases, a deeper pit stays "
          "(Thema 174 Schritt 13: capped at the water's own level below half weight, see TerrainSculpt::lowerToFloor "
          "wetLevel — a single dab's own paint does not turn its ground wet below half weight either)")
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
    // At 5 m (weight exactly 0.5, the circle's own coverage-wet boundary for a
    // single dab) the target IS the water's level (10.3) — above the untouched
    // ground (10) already, by the stroke's own levelOffset, so nothing is dug
    // here at all: digging further would sink it BELOW the level without this
    // one dab's own paint ever reaching it (the bug this schritt fixes).
    CHECK(h(5, 0) == 10.0f);                                  // bit equal: untouched, not bit equal to `before`'s coincidence
    CHECK(h(7, 0) == 10.0f);                                  // beyond radius + falloff
    CHECK(h(0, 7) == 10.0f);

    // Never raises: every vertex is at or below where it was.
    for (size_t i = 0; i < before.size(); ++i) CHECK(tc.sculptHeights[i] <= before[i]);

    // The terrain rebuilds only what moved: a rectangle around the brush, not
    // everything. Only out to the radius (4 m) now, one grid step wider: the 5 m
    // ring no longer moves at all (capped at the level, already there).
    CHECK(tc.regionDirty);
    CHECK_FALSE(tc.dirty);
    CHECK(tc.dirtyMinX <= -5.0f);
    CHECK(tc.dirtyMaxX >= 5.0f);
    CHECK(tc.dirtyMaxX < 12.0f);
}

TEST_CASE("lowerToFloor wetLevel: capped at the level below half weight, eased to the floor above it, "
          "NaN (the default, omitted here) reproduces the old, uncapped dig bit for bit")
{
    // radius 2, falloff 4: half weight (a single dab's own coverage-wet boundary,
    // WaterField::kWet at 255/2) lands exactly on the grid at 4 m, and 3 m (weight
    // 0.75) shows the eased zone between it and the radius, not just the two ends.
    TerrainComponent tc = makeLand(10.0f);
    REQUIRE(sculpt::lowerToFloor(tc, 0.0f, 0.0f, 2.0f, 4.0f, 6.0f, 1.0f, 10.0f).ok);
    auto h = [&](int x) { return tc.sculptHeights[32u * 65u + static_cast<size_t>(32 + x)]; };
    CHECK(h(0) == 6.0f);                                      // weight 1: exactly the floor
    CHECK(h(2) == 6.0f);                                      // weight 1, on the radius: exactly the floor
    // weight 0.75: the raw ceiling eases to 8 (halfway from the level to the
    // floor); the old blend alone would only reach 7 (further, since it blends
    // all the way toward the floor) — the ceiling wins (max, digging is
    // lowering, so the LESS-dug of the two is kept), landing on 8 exactly, not
    // blended a second time.
    CHECK(h(3) == 8.0f);
    CHECK(h(3) > 6.0f);
    CHECK(h(3) < 10.0f);
    CHECK(h(4) == 10.0f);                                     // weight exactly 0.5: target is the level (10), already there — untouched
    CHECK(h(5) == 10.0f);                                     // weight 0.25, below half: capped at the level, untouched
    CHECK(h(6) == 10.0f);                                     // beyond radius + falloff: untouched either way

    // wetLevel omitted (NaN): the old, uncapped dig, bit for bit.
    TerrainComponent old = makeLand(10.0f);
    REQUIRE(sculpt::lowerToFloor(old, 0.0f, 0.0f, 2.0f, 4.0f, 6.0f, 1.0f).ok);
    auto ho = [&](int x) { return old.sculptHeights[32u * 65u + static_cast<size_t>(32 + x)]; };
    CHECK(ho(4) == doctest::Approx(10.0f + 0.5f * (6.0f - 10.0f)).epsilon(1e-5));   // the old formula did dig here
    CHECK(ho(4) < h(4));
    CHECK(ho(5) < h(5));

    // An infinity (not NaN) is bad input, not "disabled": ok = false, nothing moves.
    TerrainComponent bad = makeLand(10.0f);
    const std::vector<float> badBefore = bad.sculptHeights;
    CHECK_FALSE(sculpt::lowerToFloor(bad, 0.0f, 0.0f, 2.0f, 4.0f, 6.0f, 1.0f,
                                     std::numeric_limits<float>::infinity()).ok);
    CHECK(bad.sculptHeights == badBefore);
}

TEST_CASE("lowerToFloor wetLevel: on ground ABOVE the level, the edge of a single dab's own wet reach "
          "still lands exactly ON the level — blending the ceiling a second time (like a first attempt "
          "at this fix did) would leave it short instead, 'water stops too early' on a slope")
{
    // Ground one metre above the level (10): a gentle upward slope under the
    // stroke, the ground the dab's own falloff ring digs INTO, not out of.
    // radius 2, falloff 4, floor 8 (depth 2 under the level): at 4 m (weight
    // exactly 0.5, the circle's own coverage-wet boundary) blending the capped
    // ceiling a SECOND time (h = was + f*(wetLevel - was), the shape a first
    // attempt at this fix used) gives 11 + 0.5*(10 - 11) = 10.5 — ABOVE the
    // level, the water the SAME dab just painted there would not reach the
    // ground. Taking the raw ceiling instead (not re-blended), maxed against the
    // plain old blend toward the floor (11 + 0.5*(8 - 11) = 9.5, already at or
    // past the level here) and never raised past the original height: min(11,
    // max(9.5, 10)) = 10 — the level, exactly, in one dab.
    TerrainComponent tc = makeLand(11.0f);
    REQUIRE(sculpt::lowerToFloor(tc, 0.0f, 0.0f, 2.0f, 4.0f, 8.0f, 1.0f, 10.0f).ok);
    auto h = [&](int x) { return tc.sculptHeights[32u * 65u + static_cast<size_t>(32 + x)]; };
    CHECK(h(4) == 10.0f);
    CHECK(h(0) == 8.0f);        // weight 1: exactly the floor regardless of the old height
}

TEST_CASE("Water brush dig: after a stroke, nothing is dug below the water's level where the water itself "
          "is not wet there (Thema 174 Schritt 13: the dry-ring half of the bug WaterLake.cpp's outward bank "
          "had, fixed the same way — capped at the level outside a single dab's own wet reach)")
{
    TerrainComponent tc = makeLand(10.0f);
    const std::vector<float> before = tc.sculptHeights;
    brush::Params p = params();           // radius 4, falloff 2, levelOffset 0.3
    p.dig = true;
    p.digDepth = 2.0f;
    const brush::Stroke s = drag(tc, { -6.0f, 0.0f }, { 6.0f, 0.0f }, p, 10);
    REQUIRE(s.body != water::kNoBody);
    REQUIRE(tc.water.bodies.size() == 1);

    // Flat ground with a positive levelOffset sits below the water's own level
    // EVERYWHERE, dug or not (that offset is the whole point — see WaterBrush.h),
    // so "below the level" is not the test; "actually dug" is. Every vertex this
    // stroke lowered at all must be wet, the same invariant the lake fix checks.
    //
    // "Wet" is what the water MESH itself would say: the lattice corner at this
    // position (the bilinear average of its four cells, water::coverageAt) at or
    // past water::kContourIso / 255 — the exact threshold WaterMesh.cpp's marching
    // squares uses, NOT water::bodyAt (the coarser "which single cell contains
    // this point" query, off by up to half a cell from the nearest lattice
    // corner, which a point right at a dab's edge can cross).
    const uint32_t res = tc.resolution;
    uint32_t dug = 0, dugButDry = 0;
    for (uint32_t zi = 0; zi < res; ++zi)
        for (uint32_t xi = 0; xi < res; ++xi)
        {
            const size_t i = static_cast<size_t>(zi) * res + xi;
            if (tc.sculptHeights[i] == before[i]) continue;    // untouched: nothing to check here
            ++dug;
            const float wx = -32.0f + static_cast<float>(xi), wz = -32.0f + static_cast<float>(zi);
            if (water::coverageAt(tc, wx, wz) < 0.5f)
            {
                ++dugButDry;
                MESSAGE("dry but dug: (" << wx << "," << wz << ") lowered by "
                        << (before[i] - tc.sculptHeights[i]) << " to " << tc.sculptHeights[i]);
            }
        }
    CHECK(dug > 0u);              // the stroke actually dug something
    CHECK(dugButDry == 0u);       // and none of it is dry
}

namespace
{
    // Thema 188: the pit a digging stroke leaves against the sheet the water mesh
    // actually builds from it (water::buildSurface with the field's own shore
    // clipping, the mesh WaterSurface draws). A terrain vertex the stroke lowered
    // that no triangle of the sheet covers is ground dug away with no water over it
    // — the dry crater at the rim.
    struct PitVsSheet
    {
        uint32_t dug = 0;            // vertices the stroke lowered
        uint32_t dugDry = 0;         // ... that the sheet does not cover
        float    dryDepth = 0.0f;    // the deepest of those, metres below the old ground
        uint32_t triangles = 0;
        // The other way round, reported, not required: water over ground the
        // stroke left where it was (shallow water at the rim, the level offset).
        uint32_t wetUndug = 0;
        float    widestShelf = 0.0f; // metres from such a vertex to the nearest lowered one, at most
    };

    // The sheet the field's own settings build for `body`, the mesh WaterSurface draws.
    water::Surface sheetOf(const TerrainComponent& tc, uint16_t body)
    {
        water::SurfaceOptions opt;
        opt.clipToGround   = tc.water.clipToGround;
        opt.shoreOvershoot = tc.water.shoreOvershoot;
        water::Surface sheet;
        REQUIRE(water::buildSurface(tc, body, opt, sheet));
        return sheet;
    }

    // Whether a triangle of the sheet covers terrain-local (x, z), the outline
    // counting as covered within `tol` metres.
    bool sheetCovers(const water::Surface& sheet, float x, float z, float tol)
    {
        const std::vector<float>&    v  = sheet.mesh.vertices;
        const std::vector<uint32_t>& ix = sheet.mesh.indices;
        const float px = x - sheet.center.x, pz = z - sheet.center.y;
        // Signed distance of the point from an edge's line.
        auto side = [&](float x0, float z0, float x1, float z1)
        {
            const float len = std::sqrt((x1 - x0) * (x1 - x0) + (z1 - z0) * (z1 - z0));
            return len > 0.0f ? ((x1 - x0) * (pz - z0) - (z1 - z0) * (px - x0)) / len : 0.0f;
        };
        for (size_t t = 0; t + 2 < ix.size(); t += 3)
        {
            const float ax = v[ix[t] * 3], az = v[ix[t] * 3 + 2];
            const float bx = v[ix[t + 1] * 3], bz = v[ix[t + 1] * 3 + 2];
            const float cx = v[ix[t + 2] * 3], cz = v[ix[t + 2] * 3 + 2];
            const float d1 = side(ax, az, bx, bz), d2 = side(bx, bz, cx, cz), d3 = side(cx, cz, ax, az);
            if ((d1 >= -tol && d2 >= -tol && d3 >= -tol) || (d1 <= tol && d2 <= tol && d3 <= tol)) return true;
        }
        return false;
    }

    PitVsSheet pitVsSheet(const TerrainComponent& tc, const std::vector<float>& before, uint16_t body)
    {
        PitVsSheet r;
        const water::Surface sheet = sheetOf(tc, body);
        r.triangles = sheet.triangles;
        // On the outline counts as under it, within what the mesh's own
        // simplification moves it (a thousandth of a cell, 4 mm here).
        auto covered = [&](float x, float z) { return sheetCovers(sheet, x, z, 0.005f); };
        const uint32_t res = tc.resolution;
        const float stepX = tc.sizeX / static_cast<float>(res - 1), stepZ = tc.sizeZ / static_cast<float>(res - 1);
        std::vector<glm::vec2> dugAt, wetUndugAt;
        for (uint32_t zi = 0; zi < res; ++zi)
            for (uint32_t xi = 0; xi < res; ++xi)
            {
                const size_t i = static_cast<size_t>(zi) * res + xi;
                const float x = -tc.sizeX * 0.5f + static_cast<float>(xi) * stepX;
                const float z = -tc.sizeZ * 0.5f + static_cast<float>(zi) * stepZ;
                if (!(tc.sculptHeights[i] < before[i]))
                {
                    if (sheetCovers(sheet, x, z, 0.0f)) wetUndugAt.push_back({ x, z });
                    continue;
                }
                ++r.dug;
                dugAt.push_back({ x, z });
                if (covered(x, z)) continue;
                ++r.dugDry;
                r.dryDepth = std::max(r.dryDepth, before[i] - tc.sculptHeights[i]);
            }
        r.wetUndug = static_cast<uint32_t>(wetUndugAt.size());
        if (!dugAt.empty())
            for (const glm::vec2& w : wetUndugAt)
            {
                float best = 1e30f;
                for (const glm::vec2& d : dugAt) best = std::min(best, glm::dot(w - d, w - d));
                r.widestShelf = std::max(r.widestShelf, std::sqrt(best));
            }
        return r;
    }

    // A landscape at the proportions of the editor's default (a vertex every
    // metre, a water cell four vertices wide: 1 km at 1025 and 256), small enough
    // for a test.
    TerrainComponent makeWideLand(float heightAt0, float slopeX)
    {
        TerrainComponent tc;
        tc.sizeX = tc.sizeZ = 128.0f;
        tc.resolution = 129;
        tc.dirty = false;
        tc.water.res = 32;
        tc.sculptHeights.resize(129u * 129u);
        for (uint32_t z = 0; z < 129; ++z)
            for (uint32_t x = 0; x < 129; ++x)
                tc.sculptHeights[static_cast<size_t>(z) * 129 + x] =
                    heightAt0 + slopeX * (-64.0f + static_cast<float>(x));
        return tc;
    }
}

TEST_CASE("Water brush dig (Thema 188): the pit is never wider than the sheet — every lowered vertex is under "
          "the water the mesh builds, at the editor's own pacing, on flat and rising ground, fast and slow")
{
    // bed: the stroke dwells long enough on its middle line to reach the floor there.
    struct Case { const char* name; float slopeX; float amount; float step; bool bed; };
    // amount = Brush Strength · dt, the Landscape tool's pacing (TerrainTools.cpp):
    // the default strength 5 at 60 and 144 fps, a gentle 0.5 at 60 fps, and one
    // full-strength dab per step (the witness's and the older tests' pacing).
    const Case cases[] = {
        { "flat, strength 5 @ 60 fps, slow drag",   0.0f,  5.0f / 60.0f,  0.25f, true  },
        { "flat, strength 5 @ 60 fps, fast drag",   0.0f,  5.0f / 60.0f,  1.5f,  false },
        { "flat, strength 5 @ 144 fps",             0.0f,  5.0f / 144.0f, 0.5f,  false },
        { "flat, strength 0.5 @ 60 fps",            0.0f,  0.5f / 60.0f,  0.25f, false },
        { "uphill, strength 5 @ 60 fps",            0.08f, 5.0f / 60.0f,  0.5f,  true  },
        { "uphill, strength 0.5 @ 60 fps",          0.08f, 0.5f / 60.0f,  0.25f, false },
        { "flat, full-strength dabs",               0.0f,  1.0f,          4.0f,  true  },
        { "uphill, full-strength dabs",             0.08f, 1.0f,          4.0f,  true  },
    };
    for (const Case& c : cases)
    {
        CAPTURE(std::string(c.name));
        TerrainComponent tc = makeWideLand(10.0f, c.slopeX);
        const std::vector<float> before = tc.sculptHeights;
        brush::Params p;
        p.radius = 10.0f; p.falloff = 5.0f;              // the editor's defaults
        p.amount = c.amount;
        p.levelFromGround = true; p.levelOffset = 0.3f;
        p.dig = true; p.digDepth = 1.0f;
        const int steps = static_cast<int>(std::lround(48.0f / c.step));
        const brush::Stroke s = drag(tc, { -30.0f, 0.0f }, { 18.0f, 0.0f }, p, steps);
        REQUIRE(s.body != water::kNoBody);
        const PitVsSheet r = pitVsSheet(tc, before, s.body);

        // Not by digging less: the stroke digs, reaches its bed where it dwelt,
        // and on rising ground still cuts what stood above the water's level.
        const float floorY = s.level - p.digDepth;
        float deepest = 1e9f;
        uint32_t cutAboveLevel = 0;
        for (size_t i = 0; i < before.size(); ++i)
        {
            if (!(tc.sculptHeights[i] < before[i])) continue;
            deepest = std::min(deepest, tc.sculptHeights[i]);
            if (before[i] > s.level) ++cutAboveLevel;
        }
        MESSAGE(std::string(c.name) << ": dug " << r.dug << " vertices, " << r.dugDry << " of them dry (deepest "
                << r.dryDepth << " m), bed " << (deepest - floorY) << " m above the floor, "
                << cutAboveLevel << " cut down from above the level, sheet " << r.triangles << " triangles; "
                << r.wetUndug << " vertices under water but not dug, at most " << r.widestShelf
                << " m from the pit");
        CHECK(r.dugDry == 0u);
        CHECK(r.dug > 100u);   // a gentle brush paints (and digs) a narrower stroke, not none
        if (c.bed) CHECK(deepest <= floorY + 0.05f);   // paced: within a few cm of it, not on it
        if (c.slopeX > 0.0f) CHECK(cutAboveLevel > 50u);
    }

    // A caller that hands the brush a negative falloff: the dig clamps it to 0 and
    // walks the radius, and so must the check after it.
    TerrainComponent tc = makeWideLand(10.0f, 0.08f);
    const std::vector<float> before = tc.sculptHeights;
    brush::Params p;
    p.radius = 10.0f; p.falloff = -5.0f; p.amount = 1.0f;
    p.levelFromGround = true; p.levelOffset = 0.3f;
    p.dig = true; p.digDepth = 1.0f;
    const brush::Stroke s = drag(tc, { -30.0f, 0.0f }, { 18.0f, 0.0f }, p, 12);
    REQUIRE(s.body != water::kNoBody);
    const PitVsSheet r = pitVsSheet(tc, before, s.body);
    CHECK(r.dug > 100u);
    CHECK(r.dugDry == 0u);
}

TEST_CASE("insideSheet (Thema 188): the point test the dig asks says what the mesh covers, saddles and shore "
          "clipping included, up to the outline")
{
    // Hills and hollows under a fixed level, so the shore clipping cuts the sheet
    // on every side, and paced strokes, so the coverage is ragged at the rim.
    TerrainComponent tc = makeLand(10.0f);
    for (uint32_t z = 0; z < 65; ++z)
        for (uint32_t x = 0; x < 65; ++x)
            tc.sculptHeights[static_cast<size_t>(z) * 65 + x] =
                10.0f + 1.5f * std::sin(static_cast<float>(x) / 4.0f) * std::cos(static_cast<float>(z) / 3.0f);
    brush::Params p = params();
    p.levelFromGround = false; p.level = 10.4f;
    p.amount = 0.15f; p.radius = 5.0f; p.falloff = 4.0f;
    brush::Stroke s;
    brush::begin(s, tc, -20.0f, -14.0f, p);
    const uint16_t body = s.body;
    for (int i = 0; i <= 120; ++i)
    {
        const float t = static_cast<float>(i) / 120.0f;
        brush::dab(s, tc, -20.0f + 40.0f * t, -14.0f + 24.0f * t + 6.0f * std::sin(t * 9.0f), p);
    }
    brush::end(s, tc);
    REQUIRE(tc.water.findBody(body));

    const water::Surface sheet = sheetOf(tc, body);
    water::Lattice lattice;
    REQUIRE(water::buildLattice(tc, body, water::bodyCells(tc, body), lattice,
                                { tc.water.clipToGround, p.level, tc.water.shoreOvershoot }));
    std::vector<water::Contour> outline = water::extractContours(lattice);
    auto nearOutline = [&](float x, float z)
    {
        const glm::dvec2 q(x, z);
        for (const water::Contour& c : outline)
            for (size_t i = 0, j = c.points.size() - 1; i < c.points.size(); j = i++)
            {
                const glm::dvec2 a = c.points[j], ab = c.points[i] - a;
                const double t = std::clamp(glm::dot(q - a, ab) / std::max(1e-12, glm::dot(ab, ab)), 0.0, 1.0);
                if (glm::length(q - (a + ab * t)) <= 0.01) return true;
            }
        return false;
    };

    uint32_t in = 0, out = 0, disagree = 0;
    for (float z = -31.9f; z < 32.0f; z += 0.37f)
        for (float x = -31.9f; x < 32.0f; x += 0.37f)
        {
            const bool asked = water::insideSheet(lattice, x, z);
            const bool drawn = sheetCovers(sheet, x, z, 0.0f);
            (asked ? in : out)++;
            if (asked != drawn && !nearOutline(x, z))
            {
                ++disagree;
                MESSAGE("insideSheet " << asked << " but the mesh " << drawn << " at (" << x << ", " << z << ")");
            }
        }
    CHECK(disagree == 0u);
    CHECK(in > 500u);                       // a sheet with some area, and land around it
    CHECK(out > 500u);
    CHECK(outline.size() >= 2u);            // the hills cut it into more than one shore
    CHECK_FALSE(water::insideSheet(lattice, 1.0e6, 0.0));
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
