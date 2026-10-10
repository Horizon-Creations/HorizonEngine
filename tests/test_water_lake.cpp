#include "doctest.h"
#include "EditorUndo.h"
#include <HorizonScene/WaterLake.h>
#include <HorizonScene/WaterBrush.h>
#include <HorizonScene/WaterField.h>
#include <HorizonScene/WaterMesh.h>
#include <HorizonScene/WaterSurface.h>
#include <HorizonScene/TerrainSculpt.h>
#include <HorizonScene/TerrainMeshGenerator.h>
#include <HorizonScene/TerrainSystem.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SplineCurve.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/SplineComponent.h>
#include <HorizonScene/Components/WaterSurfaceComponent.h>
#include <ContentManager/ContentManager.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace water = HE::water;
namespace lake = HE::water::lake;
namespace brush = HE::water::brush;
namespace sculpt = TerrainSculpt;

// The lake (WaterLake.h): a closed spline's outline laid into the water model as
// one body, reshaped when the spline moves with the brush share kept on top, dug
// into the ground only on an explicit action, converted from brushed water, and
// through the editor's undo and the scene file. The panel and the viewport are the
// Spline tool's (test_spline_tool_ui.cpp).
//
// Terrain 64 × 64 m at 65 vertices (a vertex on every whole metre) and a 64-cell
// water grid (a cell is 1 m), so an outline on whole metres covers whole cells and
// the merge rule can be asserted cell by cell.

namespace
{
    TerrainComponent makeLand(float heightAt0 = 10.0f, float slopeX = 0.0f)
    {
        TerrainComponent tc;
        tc.sizeX = tc.sizeZ = 64.0f;
        tc.resolution = 65;
        tc.dirty = false;
        tc.water.res = 64;
        tc.sculptHeights.resize(65u * 65u);
        for (uint32_t z = 0; z < 65; ++z)
            for (uint32_t x = 0; x < 65; ++x)
                tc.sculptHeights[static_cast<size_t>(z) * 65 + x] =
                    heightAt0 + slopeX * (-32.0f + static_cast<float>(x));
        return tc;
    }

    std::vector<glm::vec2> rect(float x0, float z0, float x1, float z1)
    {
        return { { x0, z0 }, { x1, z0 }, { x1, z1 }, { x0, z1 } };
    }

    // Control points of a round lake. A spline through the four corners of a
    // rectangle (what `rect` gives the pure functions) is a rounded loop that
    // bulges past them; a ring of sixteen points is a circle within a percent.
    std::vector<glm::vec2> ring(float cx, float cz, float r, int n = 16)
    {
        std::vector<glm::vec2> p;
        for (int i = 0; i < n; ++i)
        {
            const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(n);
            p.emplace_back(cx + r * std::cos(a), cz + r * std::sin(a));
        }
        return p;
    }

    // The cell under a terrain-local point (a 64-cell grid over 64 m: cell k is
    // [k - 32, k - 31)).
    size_t cellOf(float x, float z) { return static_cast<size_t>(std::floor(z + 32.0f)) * 64 + static_cast<size_t>(std::floor(x + 32.0f)); }
    bool wetAt(const TerrainComponent& tc, float x, float z)
    {
        const size_t i = cellOf(x, z);
        return tc.water.coverage[i] >= water::kWet;
    }
    uint16_t ownerAt(const TerrainComponent& tc, float x, float z) { return tc.water.owner[cellOf(x, z)]; }
    uint8_t covAt(const TerrainComponent& tc, float x, float z) { return tc.water.coverage[cellOf(x, z)]; }

    bool sameBits(const std::vector<float>& a, const std::vector<float>& b)
    {
        return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
    }

    bool noWater(const TerrainComponent& tc) { return tc.water.bodies.empty() && tc.water.coverage.empty(); }

    // A stroke the way the Landscape tool makes it: begin, a dab at every step, end.
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

    brush::Params paint(float radius = 3.0f)
    {
        brush::Params p;
        p.radius = radius; p.falloff = 0.0f; p.amount = 1.0f;
        p.levelFromGround = true; p.levelOffset = 0.3f;
        return p;
    }
    brush::Params erase(float radius = 3.0f)
    {
        brush::Params p = paint(radius);
        p.erase = true;
        return p;
    }

    // The world the entity layer works on: a landscape (optionally moved), a spline
    // over a rectangle (a child of the world, at the landscape's position so local
    // points are terrain-local).
    struct Rig
    {
        HorizonWorld world;
        Entity terrain = entt::null;
        Entity spline  = entt::null;

        explicit Rig(glm::vec3 landAt = glm::vec3(0.0f), TerrainComponent land = makeLand())
        {
            terrain = world.createEntity("Landscape");
            TransformComponent t;
            t.position = landAt;
            t.dirty = true;
            world.registry().emplace<TransformComponent>(terrain, t);
            world.registry().emplace<TerrainComponent>(terrain, std::move(land));
        }
        TerrainComponent& tc() { return world.registry().get<TerrainComponent>(terrain); }

        Entity addSpline(const std::vector<glm::vec2>& xz, float y, bool closed = true)
        {
            const Entity e = world.createEntity("Spline");
            TransformComponent t;
            t.position = world.registry().get<TransformComponent>(terrain).position;
            t.dirty = true;
            world.registry().emplace<TransformComponent>(e, t);
            SplineComponent s;
            s.closed = closed;
            for (const glm::vec2& p : xz) s.controlPoints.emplace_back(p.x, y, p.y);
            world.registry().emplace<SplineComponent>(e, std::move(s));
            return e;
        }
        SplineComponent& points(Entity e) { return world.registry().get<SplineComponent>(e); }
    };

    lake::Params flat(float level, float depth = 0.0f, bool dig = false)
    {
        lake::Params p;
        p.levelFromGround = false;
        p.level = level;
        p.dig = dig;
        p.depth = depth;
        return p;
    }
}

// ── The pure lake ─────────────────────────────────────────────────────────────

TEST_CASE("Water lake: create lays the outline into one body that remembers its spline and its outline")
{
    TerrainComponent tc = makeLand();
    const HE::UUID spline = HE::UUID::generate();
    const auto poly = rect(-10, -10, 10, 10);

    const uint16_t id = lake::create(tc, spline, poly, 10.5f);
    REQUIRE(id != water::kNoBody);
    REQUIRE(tc.water.bodies.size() == 1);
    const water::Body& b = tc.water.bodies[0];
    CHECK(b.id == id);
    CHECK(b.level == 10.5f);
    CHECK(b.sourceSpline == spline);
    CHECK(b.fromSpline());
    CHECK(b.polygon == poly);
    CHECK(tc.water.findBySource(spline) == &tc.water.bodies[0]);

    CHECK(tc.water.wetCells(id) == 400u);                      // 20 × 20 whole cells
    CHECK(wetAt(tc, 0.5f, 0.5f));
    CHECK(wetAt(tc, -9.5f, 9.5f));
    CHECK_FALSE(wetAt(tc, 10.5f, 0.5f));
    CHECK(ownerAt(tc, 0.5f, 0.5f) == id);
    CHECK(tc.water.dirty);                                     // the surface has something to build

    // The same spline again is the same body: a second create is a reshape.
    const uint16_t again = lake::create(tc, spline, rect(-10, -10, 12, 10), 11.0f);
    CHECK(again == id);
    CHECK(tc.water.bodies.size() == 1);
    CHECK(tc.water.bodies[0].level == 11.0f);
    CHECK(tc.water.wetCells(id) == 440u);
}

TEST_CASE("Water lake: an unusable outline creates nothing at all")
{
    TerrainComponent tc = makeLand();
    const HE::UUID spline = HE::UUID::generate();

    CHECK(lake::create(tc, spline, { { 0, 0 }, { 5, 0 } }, 1.0f) == water::kNoBody);                  // two points
    CHECK(lake::create(tc, spline, { { 0, 0 }, { 5, 0 }, { 10, 0 } }, 1.0f) == water::kNoBody);       // no area
    CHECK(lake::create(tc, spline, rect(100, 100, 120, 120), 1.0f) == water::kNoBody);                // off the terrain
    CHECK(lake::create(tc, HE::UUID{}, rect(-5, -5, 5, 5), 1.0f) == water::kNoBody);                  // no spline
    CHECK(lake::create(tc, spline, rect(-5, -5, 5, 5), std::nanf("")) == water::kNoBody);             // no level
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(lake::create(tc, spline, { { 0, 0 }, { inf, 0 }, { 5, 5 } }, 1.0f) == water::kNoBody);
    CHECK(noWater(tc));
}

TEST_CASE("Water lake: the default level is the lowest ground under the outline")
{
    TerrainComponent tc = makeLand(10.0f, 0.5f);               // 10 at x = 0, falling to the west: -6 at x = -32
    const auto poly = rect(-10, -10, 10, 10);
    CHECK(lake::lowestGround(tc, poly) == doctest::Approx(10.0f - 5.0f));

    // Never above the ground it is measured on.
    CHECK(lake::lowestGround(tc, rect(0, 0, 4, 4)) == doctest::Approx(10.0f));
}

TEST_CASE("Water lake dig: the ground goes down to level minus depth, never up, and the bank eases back")
{
    TerrainComponent tc = makeLand(10.0f);
    tc.sculptHeights[static_cast<size_t>(32) * 65 + 32] = 5.0f;   // a pit already deeper than the lake's bed
    const std::vector<float> before = tc.sculptHeights;
    const uint16_t id = lake::create(tc, HE::UUID::generate(), rect(-10, -10, 10, 10), 10.0f);
    REQUIRE(id != water::kNoBody);

    // create alone never touches the ground.
    CHECK(sameBits(tc.sculptHeights, before));

    const sculpt::Result r = lake::dig(tc, id, 3.0f, 4.0f);   // the bed is the level minus the depth: 10 − 3 = 7
    REQUIRE(r.ok);
    CHECK(r.changed > 0u);
    auto h = [&](int x, int z) { return tc.sculptHeights[static_cast<size_t>(z + 32) * 65 + (x + 32)]; };
    CHECK(h(0, 5) == doctest::Approx(7.0f));                  // inside: the floor, exactly
    CHECK(h(-9, -9) == doctest::Approx(7.0f));
    CHECK(h(0, 0) == doctest::Approx(5.0f));                  // the pit that was deeper stays deeper
    CHECK(h(13, 0) > 7.0f);                                    // in the bank: between floor and old ground
    CHECK(h(13, 0) < 10.0f);
    CHECK(h(25, 0) == doctest::Approx(10.0f));                 // beyond the bank: untouched
    CHECK(tc.regionDirty);                                     // chunks under it are rebuilt, not all of them
    CHECK_FALSE(tc.dirty);

    // A deeper pit than asked for stays where it is.
    tc.sculptHeights[static_cast<size_t>(32) * 65 + 34] = 2.0f;
    lake::dig(tc, id, 3.0f, 4.0f);
    CHECK(h(2, 0) == doctest::Approx(2.0f));

    // Not a lake, no dig.
    CHECK_FALSE(lake::dig(tc, 999, 3.0f, 4.0f).ok);
    CHECK_FALSE(lake::dig(tc, id, std::nanf(""), 4.0f).ok);
}

// ── The merge rule, one consequence at a time ───────────────────────────────

TEST_CASE("Water lake reshape 1: shrinking the outline dries the area only the old outline covered")
{
    TerrainComponent tc = makeLand();
    const uint16_t id = lake::create(tc, HE::UUID::generate(), rect(-10, -10, 10, 10), 10.5f);
    REQUIRE(wetAt(tc, 8.5f, 0.5f));

    const water::Result r = lake::reshape(tc, id, rect(-10, -10, 4, 10));
    REQUIRE(r.ok);
    CHECK(r.changed == 6u * 20u);                              // the six columns it gave up
    CHECK_FALSE(wetAt(tc, 8.5f, 0.5f));
    CHECK(ownerAt(tc, 8.5f, 0.5f) == water::kNoBody);
    CHECK(wetAt(tc, 3.5f, 0.5f));
    CHECK(tc.water.wetCells(id) == 14u * 20u);
    CHECK(tc.water.bodies[0].polygon == rect(-10, -10, 4, 10));
}

TEST_CASE("Water lake reshape 2: growing the outline absorbs the brush water it now covers and nothing doubles")
{
    TerrainComponent tc = makeLand();
    const uint16_t id = lake::create(tc, HE::UUID::generate(), rect(-10, -10, 10, 10), 10.5f);

    // Brush water on the east shore: pressed on the lake, dragged out of it.
    const brush::Stroke s = drag(tc, { 8.0f, 0.0f }, { 16.0f, 0.0f }, paint(2.0f), 8);
    REQUIRE(s.body == id);
    REQUIRE_FALSE(s.created);
    REQUIRE(wetAt(tc, 15.5f, 0.5f));
    const uint32_t before = tc.water.wetCells(id);
    REQUIRE(before > 400u);

    // The outline now reaches x = 14: the brush cells inside it become spline water.
    REQUIRE(lake::reshape(tc, id, rect(-10, -10, 14, 10)).ok);
    CHECK(tc.water.bodies.size() == 1);
    CHECK(wetAt(tc, 13.5f, 0.5f));
    CHECK(ownerAt(tc, 13.5f, 0.5f) == id);
    CHECK(wetAt(tc, 15.5f, 0.5f));                             // still brush water beyond it
    // 24 × 20 under the outline plus the brush cells beyond x = 14: more than the outline
    // alone, less than the old total plus a whole second rectangle.
    const uint32_t after = tc.water.wetCells(id);
    CHECK(after >= 24u * 20u);
    CHECK(after < 24u * 20u + 40u);
}

TEST_CASE("Water lake reshape 3: brush water outside both outlines stays where it was painted")
{
    TerrainComponent tc = makeLand();
    const uint16_t id = lake::create(tc, HE::UUID::generate(), rect(-10, -10, 10, 10), 10.5f);
    drag(tc, { 8.0f, 0.0f }, { 22.0f, 0.0f }, paint(2.0f), 10);    // a channel out of the east shore
    REQUIRE(wetAt(tc, 20.5f, 0.5f));
    REQUIRE(tc.water.bodies.size() == 1);                      // one body: the stroke continued the lake

    // Pull the east edge in to x = 0: the old-only part dries, the channel beyond x = 10 does not.
    REQUIRE(lake::reshape(tc, id, rect(-10, -10, 0, 10)).ok);
    CHECK_FALSE(wetAt(tc, 5.5f, 5.5f));                         // old outline only, beside the channel
    CHECK(wetAt(tc, 20.5f, 0.5f));
    CHECK(ownerAt(tc, 20.5f, 0.5f) == id);
    CHECK(wetAt(tc, 12.5f, 0.5f));
    CHECK(wetAt(tc, -5.5f, 0.5f));                              // the spline share, as asked
}

TEST_CASE("Water lake reshape 4: a notch the eraser cut inside the outline stays a notch")
{
    TerrainComponent tc = makeLand();
    const uint16_t id = lake::create(tc, HE::UUID::generate(), rect(-10, -10, 10, 10), 10.5f);
    drag(tc, { 0.0f, 0.0f }, { 0.0f, 0.0f }, erase(3.0f), 1);
    REQUIRE_FALSE(wetAt(tc, 0.5f, 0.5f));
    REQUIRE_FALSE(wetAt(tc, 2.5f, 0.5f));
    REQUIRE(wetAt(tc, 5.5f, 0.5f));
    REQUIRE(tc.water.bodies.size() == 1);                       // an erased lake keeps its body

    // Reshape to a bigger rectangle around it.
    REQUIRE(lake::reshape(tc, id, rect(-14, -14, 14, 14)).ok);
    CHECK_FALSE(wetAt(tc, 0.5f, 0.5f));                         // the notch is still dry,
    CHECK_FALSE(wetAt(tc, 2.5f, 0.5f));
    CHECK(ownerAt(tc, 0.5f, 0.5f) == water::kNoBody);
    CHECK(wetAt(tc, 5.5f, 0.5f));                               // its neighbours are lake,
    CHECK(wetAt(tc, 12.5f, 0.5f));                              // and the growth is lake.

    // A second reshape does not heal it either.
    REQUIRE(lake::reshape(tc, id, rect(-12, -12, 12, 12)).ok);
    CHECK_FALSE(wetAt(tc, 0.5f, 0.5f));
}

TEST_CASE("Water lake reshape 4b: a notch is remembered only while it is inside the outline")
{
    // The documented limit of storing an outline instead of a second raster.
    TerrainComponent tc = makeLand();
    const uint16_t id = lake::create(tc, HE::UUID::generate(), rect(-10, -10, 10, 10), 10.5f);
    drag(tc, { 0.0f, 0.0f }, { 0.0f, 0.0f }, erase(3.0f), 1);
    REQUIRE_FALSE(wetAt(tc, 0.5f, 0.5f));

    REQUIRE(lake::reshape(tc, id, rect(20, 20, 30, 30)).ok);    // away from it entirely
    CHECK_FALSE(wetAt(tc, 0.5f, 0.5f));
    REQUIRE(lake::reshape(tc, id, rect(-10, -10, 10, 10)).ok);  // and back
    CHECK(wetAt(tc, 0.5f, 0.5f));                               // the notch has healed
}

TEST_CASE("Water lake reshape 5: a cell the new outline covers properly is taken from another body")
{
    TerrainComponent tc = makeLand();
    const uint16_t id = lake::create(tc, HE::UUID::generate(), rect(-10, -10, 0, 10), 10.5f);
    const brush::Stroke pond = drag(tc, { 10.0f, 0.0f }, { 10.0f, 0.0f }, paint(3.0f), 1);    // its own body, to the east
    REQUIRE(pond.created);
    REQUIRE(pond.body != id);
    REQUIRE(ownerAt(tc, 10.5f, 0.5f) == pond.body);

    REQUIRE(lake::reshape(tc, id, rect(-10, -10, 12, 10)).ok);  // the outline grows over the pond
    CHECK(ownerAt(tc, 10.5f, 0.5f) == id);
    CHECK(ownerAt(tc, 5.5f, 0.5f) == id);
    CHECK(tc.water.findBody(pond.body) != nullptr);             // the pond's body is still there, with what lies beyond the new edge
    CHECK(ownerAt(tc, 11.5f, 0.5f) == id);                      // the lake's last column (x = 11..12)
    CHECK(ownerAt(tc, 12.5f, 0.5f) == pond.body);               // the pond's, just past it
    CHECK(tc.water.wetCells(pond.body) < 16u);                  // most of the pond now belongs to the lake
}

TEST_CASE("Water lake reshape 6: the same polygon again changes nothing")
{
    TerrainComponent tc = makeLand();
    const auto poly = rect(-10, -10, 10, 10);
    const uint16_t id = lake::create(tc, HE::UUID::generate(), poly, 10.5f);
    drag(tc, { 8.0f, 0.0f }, { 18.0f, 4.0f }, paint(2.0f), 8);                 // a brush share
    drag(tc, { -4.0f, -4.0f }, { -4.0f, -4.0f }, erase(2.0f), 1);               // and a notch
    const water::Field before = tc.water;

    const water::Result r = lake::reshape(tc, id, poly);
    REQUIRE(r.ok);
    CHECK(r.changed == 0u);
    CHECK(water::sameContent(tc.water, before));
}

TEST_CASE("Water lake reshape: moving the outline never touches the ground")
{
    TerrainComponent tc = makeLand();
    const uint16_t id = lake::create(tc, HE::UUID::generate(), rect(-10, -10, 10, 10), 10.5f);
    REQUIRE(lake::dig(tc, id, 2.0f, 3.0f).ok);
    const std::vector<float> dug = tc.sculptHeights;
    tc.regionDirty = false;

    REQUIRE(lake::reshape(tc, id, rect(-6, -6, 14, 8)).ok);
    CHECK(sameBits(tc.sculptHeights, dug));
    CHECK_FALSE(tc.regionDirty);                                 // no chunk is rebuilt for the water
    CHECK_FALSE(tc.dirty);
    CHECK(tc.water.dirty);

    // An explicit dig is what digs under the new outline.
    CHECK(lake::dig(tc, id, 2.0f, 3.0f).changed > 0u);
    CHECK_FALSE(sameBits(tc.sculptHeights, dug));
}

TEST_CASE("Water lake reshape: refused input leaves the cells exactly as they were")
{
    TerrainComponent tc = makeLand();
    const uint16_t id = lake::create(tc, HE::UUID::generate(), rect(-10, -10, 10, 10), 10.5f);
    const water::Field before = tc.water;

    CHECK_FALSE(lake::reshape(tc, id, { { 0, 0 }, { 5, 0 } }).ok);
    CHECK_FALSE(lake::reshape(tc, id, rect(100, 100, 110, 110)).ok);
    CHECK_FALSE(lake::reshape(tc, 77, rect(-1, -1, 1, 1)).ok);
    CHECK(water::sameContent(tc.water, before));
}

TEST_CASE("Water lake: a lake whose water was erased entirely keeps its body and reshapes back to nothing new")
{
    TerrainComponent tc = makeLand();
    const uint16_t id = lake::create(tc, HE::UUID::generate(), rect(-10, -10, 10, 10), 10.5f);
    drag(tc, { 0.0f, 0.0f }, { 0.0f, 0.0f }, erase(20.0f), 1);   // wipes the lot
    REQUIRE(tc.water.wetCells() == 0u);
    REQUIRE(tc.water.findBody(id) != nullptr);                    // a lake keeps its body: its spline can bring it back

    REQUIRE(lake::reshape(tc, id, rect(-12, -12, 12, 12)).ok);
    // The erase covers what the old outline did, so that stays dry; the ring the
    // bigger outline adds around it is new spline water.
    CHECK(tc.water.wetCells(id) == 24u * 24u - 20u * 20u);
    CHECK_FALSE(wetAt(tc, 0.5f, 0.5f));
    CHECK(wetAt(tc, 11.5f, 0.5f));
}

TEST_CASE("Water lake: detach turns a lake into brush water with the cells untouched")
{
    TerrainComponent tc = makeLand();
    const uint16_t id = lake::create(tc, HE::UUID::generate(), rect(-10, -10, 10, 10), 10.5f);
    const uint32_t cells = tc.water.wetCells(id);
    REQUIRE(lake::detach(tc, id));
    CHECK_FALSE(tc.water.bodies[0].fromSpline());
    CHECK(tc.water.bodies[0].polygon.empty());
    CHECK(tc.water.wetCells(id) == cells);
    CHECK_FALSE(lake::detach(tc, 99));
}

// ── Brush water → lake ───────────────────────────────────────────────────────

namespace
{
    double polyArea(const std::vector<glm::vec2>& p)
    {
        double a = 0.0;
        for (size_t i = 0; i < p.size(); ++i)
        {
            const glm::vec2& u = p[i];
            const glm::vec2& v = p[(i + 1) % p.size()];
            a += static_cast<double>(u.x) * v.y - static_cast<double>(v.x) * u.y;
        }
        return a * 0.5;
    }
}

TEST_CASE("Water lake convert: the outline of a painted body is a short counter-clockwise ring around it")
{
    TerrainComponent tc = makeLand();
    const brush::Stroke s = drag(tc, { -10.0f, 0.0f }, { 10.0f, 0.0f }, paint(5.0f), 10);   // a 30 × 10 sausage
    REQUIRE(s.created);
    const uint32_t cells = tc.water.wetCells(s.body);
    REQUIRE(cells > 200u);

    const lake::Outline o = lake::extractOutline(tc, s.body);
    REQUIRE(o.ok);
    CHECK(o.pieces == 1u);
    CHECK(o.islands == 0u);
    CHECK(o.points.size() >= 3u);
    CHECK(o.points.size() <= 32u);                               // a spline, not a polyline of the shore
    CHECK(polyArea(o.points) > 0.0);                             // counter-clockwise
    // The ring encloses about the cells that were painted (a ragged shore simplified within a cell or so).
    CHECK(std::abs(polyArea(o.points) - static_cast<double>(cells)) < 0.12 * static_cast<double>(cells));

    // A tight cap makes a coarser ring, never a failed one.
    const lake::Outline coarse = lake::extractOutline(tc, s.body, 0.0f, 6);
    REQUIRE(coarse.ok);
    CHECK(coarse.points.size() <= 6u);
    CHECK(coarse.tolerance >= o.tolerance);

    CHECK_FALSE(lake::extractOutline(tc, 99).ok);               // no such body
    TerrainComponent dry = makeLand();
    CHECK_FALSE(lake::extractOutline(dry, 1).ok);
}

TEST_CASE("Water lake convert: adopt makes the body that spline's lake, keeps its level and other pieces, and reshapes cleanly")
{
    TerrainComponent tc = makeLand();
    // One body, two pieces: a sausage, and a blob far away that the same stroke reached.
    brush::Stroke s;
    brush::begin(s, tc, -10.0f, 0.0f, paint(4.0f));
    for (int i = 0; i <= 10; ++i) brush::dab(s, tc, -10.0f + 2.0f * i, 0.0f, paint(4.0f));
    for (int i = 0; i < 3; ++i) brush::dab(s, tc, 22.0f, 22.0f, paint(3.0f));
    const uint16_t body = s.body;                                // end() resets the stroke
    brush::end(s, tc);
    REQUIRE(wetAt(tc, 22.5f, 22.5f));
    const float level = tc.water.bodies[0].level;

    const lake::Outline o = lake::extractOutline(tc, body);
    REQUIRE(o.ok);
    CHECK(o.pieces == 2u);
    CHECK(polyArea(o.points) > 100.0);                           // the sausage, not the blob

    const HE::UUID spline = HE::UUID::generate();
    const water::Result r = lake::adopt(tc, body, spline, o.points);
    REQUIRE(r.ok);
    REQUIRE(tc.water.bodies.size() == 1);
    CHECK(tc.water.bodies[0].fromSpline());
    CHECK(tc.water.bodies[0].sourceSpline == spline);
    CHECK(tc.water.bodies[0].polygon == o.points);
    CHECK(tc.water.bodies[0].level == level);                    // converting does not move the surface
    CHECK(wetAt(tc, 0.5f, 0.5f));
    CHECK(wetAt(tc, 22.5f, 22.5f));                              // the far piece is kept, as brush water
    CHECK(ownerAt(tc, 22.5f, 22.5f) == body);

    // From here on it is a lake like any other: reshape follows, the far piece stays.
    std::vector<glm::vec2> moved = o.points;
    for (glm::vec2& p : moved) p.y += 10.0f;
    REQUIRE(lake::reshape(tc, body, moved).ok);
    CHECK_FALSE(wetAt(tc, 0.5f, -3.5f));                          // the old position's edge is dry now: no ghost of the painted shore
    CHECK(wetAt(tc, 0.5f, 10.5f));
    CHECK(wetAt(tc, 22.5f, 22.5f));

    // And it cannot be adopted twice.
    CHECK_FALSE(lake::adopt(tc, body, HE::UUID::generate(), o.points).ok);
}

TEST_CASE("Water lake convert: an island is reported and filled, and an erase brings it back that survives reshapes")
{
    TerrainComponent tc = makeLand();
    const brush::Stroke s = drag(tc, { 0.0f, 0.0f }, { 0.0f, 0.0f }, paint(10.0f), 1);
    drag(tc, { 0.0f, 0.0f }, { 0.0f, 0.0f }, erase(3.0f), 1);   // an island in the middle
    REQUIRE_FALSE(wetAt(tc, 0.5f, 0.5f));

    const lake::Outline o = lake::extractOutline(tc, s.body);
    REQUIRE(o.ok);
    CHECK(o.islands == 1u);

    REQUIRE(lake::adopt(tc, s.body, HE::UUID::generate(), o.points).ok);
    CHECK(wetAt(tc, 0.5f, 0.5f));                                // the island is flooded: the lake is its outline

    drag(tc, { 0.0f, 0.0f }, { 0.0f, 0.0f }, erase(3.0f), 1);    // carved back by hand
    REQUIRE_FALSE(wetAt(tc, 0.5f, 0.5f));
    std::vector<glm::vec2> bigger = o.points;
    for (glm::vec2& p : bigger) p *= 1.2f;
    REQUIRE(lake::reshape(tc, s.body, bigger).ok);
    CHECK_FALSE(wetAt(tc, 0.5f, 0.5f));
}

// ── The entity layer ─────────────────────────────────────────────────────────

TEST_CASE("Water lake entity: polygonOf puts the spline in the landscape's space, wherever both stand")
{
    Rig rig(glm::vec3(100.0f, 5.0f, -50.0f));
    const Entity sp = rig.addSpline(ring(0.0f, 0.0f, 10.0f), 3.0f);
    std::vector<glm::vec2> poly;
    REQUIRE(lake::polygonOf(rig.world, sp, rig.terrain, poly));
    REQUIRE(poly.size() >= 16u);
    // The spline stands at the landscape's position, so its points are terrain-local as written.
    float lo = 1e9f, hi = -1e9f;
    for (const glm::vec2& p : poly) { lo = std::min(lo, p.x); hi = std::max(hi, p.x); }
    CHECK(lo == doctest::Approx(-10.0f).epsilon(0.05));
    CHECK(hi == doctest::Approx(10.0f).epsilon(0.05));

    // Move the spline 7 m east in the world: the polygon moves 7 m east in the landscape.
    rig.world.registry().get<TransformComponent>(sp).position.x += 7.0f;
    REQUIRE(lake::polygonOf(rig.world, sp, rig.terrain, poly));
    lo = 1e9f; hi = -1e9f;
    for (const glm::vec2& p : poly) { lo = std::min(lo, p.x); hi = std::max(hi, p.x); }
    CHECK(lo == doctest::Approx(-3.0f).epsilon(0.05));
    CHECK(hi == doctest::Approx(17.0f).epsilon(0.05));

    // Open, or two points: no polygon.
    const Entity open = rig.addSpline(rect(-10, -10, 10, 10), 3.0f, false);
    CHECK_FALSE(lake::polygonOf(rig.world, open, rig.terrain, poly));
    const Entity two = rig.addSpline({ { 0, 0 }, { 5, 5 } }, 3.0f, true);
    CHECK_FALSE(lake::polygonOf(rig.world, two, rig.terrain, poly));
}

TEST_CASE("Water lake entity: create digs and lays the water in one go, from the ground as it was")
{
    Rig rig(glm::vec3(0.0f), makeLand(10.0f, 0.2f));              // 10 m at x = 0, 3.6 at x = -32, lower to the west
    const Entity sp = rig.addSpline(ring(0.0f, 0.0f, 10.0f), 10.0f);
    const std::vector<float> groundBefore = rig.tc().sculptHeights;

    lake::Params p;                                                // level from the ground, dig on
    p.levelFromGround = true; p.levelOffset = 0.5f; p.depth = 2.0f; p.bank = 3.0f;
    const lake::Created c = lake::create(rig.world, rig.terrain, sp, p);
    REQUIRE(c.ok);
    CHECK(c.error.empty());
    // The lowest ground under the outline is its western edge (10 − 0.2·10 = 8), plus the offset.
    CHECK(c.level == doctest::Approx(8.5f).epsilon(0.02));
    CHECK(c.cells > 300u);
    CHECK(c.ground > 0u);
    CHECK_FALSE(sameBits(rig.tc().sculptHeights, groundBefore));
    CHECK(terrainHeightAt(rig.tc(), 0.0f, 0.0f) == doctest::Approx(c.level - 2.0f).epsilon(0.02));

    const lake::Link l = lake::linkOf(rig.world, sp);
    CHECK(l.terrain == rig.terrain);
    CHECK(l.body == c.body);

    // Twice is refused, and says why.
    const lake::Created again = lake::create(rig.world, rig.terrain, sp, p);
    CHECK_FALSE(again.ok);
    CHECK_FALSE(again.error.empty());
}

TEST_CASE("Water lake entity: create without digging leaves the ground bit for bit alone")
{
    Rig rig;
    const Entity sp = rig.addSpline(rect(-10, -10, 10, 10), 10.0f);
    const std::vector<float> groundBefore = rig.tc().sculptHeights;
    const lake::Created c = lake::create(rig.world, rig.terrain, sp, flat(10.5f));
    REQUIRE(c.ok);
    CHECK(c.ground == 0u);
    CHECK(sameBits(rig.tc().sculptHeights, groundBefore));
    CHECK(c.level == 10.5f);
}

TEST_CASE("Water lake entity: create refuses what it cannot make and changes nothing")
{
    Rig rig;
    const std::vector<float> groundBefore = rig.tc().sculptHeights;

    const Entity open = rig.addSpline(rect(-10, -10, 10, 10), 10.0f, false);
    const lake::Created a = lake::create(rig.world, rig.terrain, open, flat(10.5f, 2.0f, true));
    CHECK_FALSE(a.ok);
    CHECK_FALSE(a.error.empty());

    const Entity off = rig.addSpline(rect(200, 200, 220, 220), 10.0f);
    const lake::Created b = lake::create(rig.world, rig.terrain, off, flat(10.5f, 2.0f, true));
    CHECK_FALSE(b.ok);

    const Entity notLand = rig.world.createEntity("Not a landscape");
    const lake::Created c = lake::create(rig.world, notLand, open, flat(10.5f));
    CHECK_FALSE(c.ok);

    CHECK(sameBits(rig.tc().sculptHeights, groundBefore));       // the refused dig did not dig
    CHECK(noWater(rig.tc()));
}

TEST_CASE("Water lake sync: moving a spline point reshapes the water, never the ground, and only once")
{
    Rig rig;
    const Entity sp = rig.addSpline(rect(-10, -10, 10, 10), 10.0f);
    REQUIRE(lake::create(rig.world, rig.terrain, sp, flat(10.5f, 2.0f, true)).ok);
    const std::vector<float> dug = rig.tc().sculptHeights;
    rig.tc().regionDirty = false;
    const uint16_t id = lake::linkOf(rig.world, sp).body;

    CHECK(lake::syncSplines(rig.world) == 0u);                   // nothing moved since create
    CHECK(lake::syncSplines(rig.world) == 0u);

    // Drag the east edge out by 4 m.
    rig.points(sp).controlPoints[1].x = 14.0f;
    rig.points(sp).controlPoints[2].x = 14.0f;
    CHECK(lake::syncSplines(rig.world) == 1u);
    CHECK(wetAt(rig.tc(), 12.5f, 0.5f));
    CHECK(rig.tc().water.wetCells(id) > 450u);
    CHECK(sameBits(rig.tc().sculptHeights, dug));
    CHECK_FALSE(rig.tc().regionDirty);
    CHECK(lake::syncSplines(rig.world) == 0u);                   // same fingerprint: nothing to do

    // Move the whole spline entity: the water follows.
    rig.world.registry().get<TransformComponent>(sp).position.z += 8.0f;
    CHECK(lake::syncSplines(rig.world) == 1u);
    CHECK(wetAt(rig.tc(), 0.5f, 16.5f));
    CHECK_FALSE(wetAt(rig.tc(), 0.5f, -9.5f));

    // Dig Again is what digs under the new place.
    CHECK(lake::dig(rig.world, sp, 2.0f, 3.0f).changed > 0u);
    CHECK_FALSE(sameBits(rig.tc().sculptHeights, dug));
}

TEST_CASE("Water lake sync: the landscape moving under a still spline reshapes the water")
{
    Rig rig;
    const Entity sp = rig.addSpline(rect(-10, -10, 10, 10), 10.0f);
    REQUIRE(lake::create(rig.world, rig.terrain, sp, flat(10.5f)).ok);
    REQUIRE(lake::syncSplines(rig.world) == 0u);

    rig.world.registry().get<TransformComponent>(rig.terrain).position.x -= 6.0f;   // the landscape slides west: the spline is 6 m east of where it was
    CHECK(lake::syncSplines(rig.world) == 1u);
    CHECK(wetAt(rig.tc(), 12.5f, 0.5f));
    CHECK_FALSE(wetAt(rig.tc(), -8.5f, 0.5f));
}

TEST_CASE("Water lake sync: a spline that is not a closed shape for a moment leaves the water alone")
{
    Rig rig;
    const Entity sp = rig.addSpline(rect(-10, -10, 10, 10), 10.0f);
    REQUIRE(lake::create(rig.world, rig.terrain, sp, flat(10.5f)).ok);
    const water::Field before = rig.tc().water;

    rig.points(sp).closed = false;                               // the user opened the loop
    CHECK(lake::syncSplines(rig.world) == 0u);
    CHECK(water::sameContent(rig.tc().water, before));
    rig.points(sp).controlPoints.erase(rig.points(sp).controlPoints.begin());   // and deleted a point
    rig.points(sp).closed = true;
    CHECK(lake::syncSplines(rig.world) == 1u);                   // three points, closed again: a lake of a triangle
    CHECK(rig.tc().water.wetCells() < before.wetCells());
}

TEST_CASE("Water lake sync: a lake whose spline entity is gone keeps its water")
{
    Rig rig;
    const Entity sp = rig.addSpline(rect(-10, -10, 10, 10), 10.5f);
    const lake::Created c = lake::create(rig.world, rig.terrain, sp, flat(10.5f));
    REQUIRE(c.ok);
    const uint32_t cells = rig.tc().water.wetCells();
    rig.world.destroyEntity(sp);

    CHECK(lake::syncSplines(rig.world) == 0u);
    CHECK(rig.tc().water.wetCells() == cells);
    CHECK(lake::linkOf(rig.world, rig.world.rootEntity()).body == water::kNoBody);
    CHECK(rig.tc().water.bodies[0].fromSpline());                // until it is detached
    REQUIRE(lake::detach(rig.tc(), c.body));
    CHECK_FALSE(rig.tc().water.bodies[0].fromSpline());
}

TEST_CASE("Water lake entity: converting a painted body makes a closed spline child of the landscape and a lake of the body")
{
    Rig rig(glm::vec3(30.0f, 4.0f, -20.0f));
    const brush::Stroke s = drag(rig.tc(), { -10.0f, 0.0f }, { 10.0f, 0.0f }, paint(5.0f), 10);
    const float level = rig.tc().water.bodies[0].level;

    const lake::Converted cv = lake::convertBody(rig.world, rig.terrain, s.body, "Pond");
    REQUIRE((cv.spline != entt::null));       // doubled parentheses: doctest cannot decompose entt::null
    CHECK(cv.error.empty());
    CHECK(cv.pieces == 1u);
    CHECK(cv.points >= 3u);
    CHECK(cv.points <= 32u);

    auto& reg = rig.world.registry();
    REQUIRE(reg.all_of<SplineComponent>(cv.spline));
    const SplineComponent& sc = reg.get<SplineComponent>(cv.spline);
    CHECK(sc.closed);
    CHECK(sc.controlPoints.size() == cv.points);
    for (const glm::vec3& p : sc.controlPoints) CHECK(p.y == level);   // drawn at the water surface
    CHECK(reg.get<HierarchyComponent>(cv.spline).parent == rig.terrain);
    CHECK(reg.get<NameComponent>(cv.spline).name == "Pond");

    const lake::Link l = lake::linkOf(rig.world, cv.spline);
    CHECK(l.terrain == rig.terrain);
    CHECK(l.body == s.body);
    CHECK(rig.tc().water.findBody(s.body)->level == level);
    std::vector<glm::vec2> poly;
    REQUIRE(lake::polygonOf(rig.world, cv.spline, rig.terrain, poly));
    CHECK(poly == rig.tc().water.findBody(s.body)->polygon);

    CHECK(lake::syncSplines(rig.world) == 0u);                    // the conversion left it in sync
    // Move a point: the new lake reshapes like any other.
    rig.points(cv.spline).controlPoints[0].z += 6.0f;
    CHECK(lake::syncSplines(rig.world) == 1u);

    // Not twice, not without water, not a stranger.
    CHECK_FALSE(lake::convertBody(rig.world, rig.terrain, s.body).error.empty());
    CHECK_FALSE(lake::convertBody(rig.world, rig.terrain, 99).error.empty());
    CHECK_FALSE(lake::convertBody(rig.world, rig.world.rootEntity(), s.body).error.empty());
}

TEST_CASE("Water lake entity: remove takes the water and the link, and leaves the spline and the ground")
{
    Rig rig;
    const Entity sp = rig.addSpline(rect(-10, -10, 10, 10), 10.0f);
    REQUIRE(lake::create(rig.world, rig.terrain, sp, flat(10.5f, 2.0f, true)).ok);
    const std::vector<float> dug = rig.tc().sculptHeights;
    CHECK(lake::setLevel(rig.world, sp, 11.0f));
    CHECK(rig.tc().water.bodies[0].level == 11.0f);

    CHECK(lake::remove(rig.world, sp));
    CHECK(noWater(rig.tc()) == false);                            // the grid stays allocated, but dry
    CHECK(rig.tc().water.wetCells() == 0u);
    CHECK(rig.tc().water.bodies.empty());
    CHECK(rig.world.registry().all_of<SplineComponent>(sp));
    CHECK(sameBits(rig.tc().sculptHeights, dug));
    CHECK(lake::linkOf(rig.world, sp).body == water::kNoBody);
    CHECK_FALSE(lake::remove(rig.world, sp));
}

// ── The world tick: the sheet follows in the same call ──────────────────────

TEST_CASE("Water lake in the world: a moved spline point has its new sheet by the end of the same updateTerrains")
{
    Rig rig;
    ContentManager cm{ "." };
    const Entity sp = rig.addSpline(rect(-10, -10, 10, 10), 10.0f);
    rig.tc().dirty = true;
    REQUIRE(lake::create(rig.world, rig.terrain, sp, flat(10.5f)).ok);
    TerrainSystem::updateTerrains(rig.world, cm);
    REQUIRE_FALSE(rig.tc().dirty);

    auto sheetSpan = [&](float& lo, float& hi)
    {
        int sheets = 0;
        lo = 1e9f; hi = -1e9f;
        for (auto [we, ws, wt] : rig.world.registry().view<WaterSurfaceComponent, TransformComponent>().each())
        {
            ++sheets;
            const StaticMeshAsset* mesh = cm.getStaticMesh(ws.meshId);
            REQUIRE(mesh);
            for (size_t i = 0; i + 2 < mesh->vertices.size(); i += 3)
            {
                const float x = mesh->vertices[i] + wt.position.x;
                lo = std::min(lo, x); hi = std::max(hi, x);
            }
        }
        return sheets;
    };
    float lo = 0.0f, hi = 0.0f;
    REQUIRE(sheetSpan(lo, hi) == 1);
    const float hiBefore = hi;
    CHECK(hiBefore > 9.0f);                                       // the loop through the corners bulges a little past them
    CHECK(hiBefore < 14.0f);

    rig.points(sp).controlPoints[1].x = 18.0f;
    rig.points(sp).controlPoints[2].x = 18.0f;
    TerrainSystem::updateTerrains(rig.world, cm);                // the sync and the surface pass, one call
    CHECK_FALSE(rig.tc().dirty);                                  // no chunk was rebuilt for it
    REQUIRE(sheetSpan(lo, hi) == 1);
    CHECK(hi > hiBefore + 5.0f);                                  // the sheet reaches the dragged edge
}

// ── Undo and the file ────────────────────────────────────────────────────────

namespace
{
    struct UndoRig : Rig
    {
        EditorUndo undo;
        UndoRig() { undo.setWorld(&world); }
        Entity theSpline()
        {
            auto view = world.registry().view<SplineComponent>();
            REQUIRE(view.size() == 1u);
            return *view.begin();
        }
        Entity theTerrain()
        {
            auto view = world.registry().view<TerrainComponent>();
            REQUIRE(view.size() == 1u);
            return *view.begin();
        }
        TerrainComponent& land() { return world.registry().get<TerrainComponent>(theTerrain()); }
    };
}

TEST_CASE("Water lake undo: create lake is one step that gives back the water AND the ground, bit for bit")
{
    UndoRig rig;
    rig.addSpline(rect(-10, -10, 10, 10), 10.0f);
    const std::vector<float> groundBefore = rig.land().sculptHeights;
    const water::Field dryField = rig.land().water;

    lake::Params p;
    p.levelFromGround = true; p.depth = 2.0f; p.bank = 3.0f;
    rig.undo.snapshotNow("Create Lake");                          // the panel's protocol: one snapshot, then the action
    REQUIRE(lake::create(rig.world, rig.theTerrain(), rig.theSpline(), p).ok);
    const std::vector<float> groundDug = rig.land().sculptHeights;
    const water::Field made = rig.land().water;
    REQUIRE_FALSE(sameBits(groundDug, groundBefore));
    CHECK(rig.undo.undoDepth() == 1u);

    REQUIRE(rig.undo.undo());
    CHECK(sameBits(rig.land().sculptHeights, groundBefore));
    CHECK(water::sameContent(rig.land().water, dryField));
    CHECK(noWater(rig.land()));
    CHECK(lake::linkOf(rig.world, rig.theSpline()).body == water::kNoBody);

    REQUIRE(rig.undo.redo());
    CHECK(sameBits(rig.land().sculptHeights, groundDug));
    CHECK(water::sameContent(rig.land().water, made));           // outline included
    CHECK(rig.land().water.bodies[0].polygon == made.bodies[0].polygon);
    CHECK(lake::linkOf(rig.world, rig.theSpline()).body == made.bodies[0].id);   // and the link came back with the entities
}

TEST_CASE("Water lake undo: undoing a reshape puts the water back and the sync does not undo the undo")
{
    UndoRig rig;
    rig.addSpline(rect(-10, -10, 10, 10), 10.0f);
    REQUIRE(lake::create(rig.world, rig.theTerrain(), rig.theSpline(), flat(10.5f)).ok);
    const water::Field original = rig.land().water;

    rig.undo.snapshotNow("Move Spline Point");
    rig.world.registry().get<SplineComponent>(rig.theSpline()).controlPoints[1].x = 20.0f;
    rig.world.registry().get<SplineComponent>(rig.theSpline()).controlPoints[2].x = 20.0f;
    REQUIRE(lake::syncSplines(rig.world) == 1u);
    REQUIRE_FALSE(water::sameContent(rig.land().water, original));

    REQUIRE(rig.undo.undo());
    CHECK(water::sameContent(rig.land().water, original));
    CHECK(rig.world.registry().get<SplineComponent>(rig.theSpline()).controlPoints[1].x == doctest::Approx(10.0f));
    CHECK(lake::syncSplines(rig.world) == 0u);                    // consistent already: a fresh body, first look only records
    CHECK(water::sameContent(rig.land().water, original));
}

TEST_CASE("Water lake file: a lake saves and loads with its outline, its link and its brush share, and still reshapes")
{
    Rig rig;
    const Entity sp = rig.addSpline(rect(-10, -10, 10, 10), 10.0f);
    const lake::Created c = lake::create(rig.world, rig.terrain, sp, flat(10.5f));
    REQUIRE(c.ok);
    drag(rig.tc(), { 8.0f, 0.0f }, { 20.0f, 0.0f }, paint(2.0f), 10);          // a channel
    drag(rig.tc(), { -4.0f, 0.0f }, { -4.0f, 0.0f }, erase(2.0f), 1);          // and a notch
    const water::Field saved = rig.tc().water;
    const HE::UUID splineId = rig.world.entityId(sp);

    SceneSerializer ser;
    std::vector<uint8_t> blob;
    REQUIRE(ser.saveToMemory(rig.world, blob));
    HorizonWorld loaded;
    REQUIRE(ser.loadFromMemory(loaded, blob));

    auto tview = loaded.registry().view<TerrainComponent>();
    REQUIRE(tview.size() == 1u);
    const Entity lt = *tview.begin();
    TerrainComponent& ltc = loaded.registry().get<TerrainComponent>(lt);
    CHECK(water::sameContent(ltc.water, saved));                  // cells, bodies, source and outline
    REQUIRE(ltc.water.bodies.size() == 1);
    CHECK(ltc.water.bodies[0].sourceSpline == splineId);
    CHECK(ltc.water.bodies[0].polygon == saved.bodies[0].polygon);
    CHECK(ltc.water.bodies[0].level == 10.5f);
    CHECK(ltc.water.dirty);

    auto sview = loaded.registry().view<SplineComponent>();
    REQUIRE(sview.size() == 1u);
    const Entity ls = *sview.begin();
    CHECK(loaded.entityId(ls) == splineId);
    CHECK(lake::linkOf(loaded, ls).body == c.body);

    // A load only looks: nothing is reshaped by being loaded.
    CHECK(lake::syncSplines(loaded) == 0u);
    CHECK(water::sameContent(ltc.water, saved));

    // Reshaping after the load still keeps the brush share the file carried.
    loaded.registry().get<SplineComponent>(ls).controlPoints[0].x = -14.0f;
    loaded.registry().get<SplineComponent>(ls).controlPoints[3].x = -14.0f;
    CHECK(lake::syncSplines(loaded) == 1u);
    CHECK(wetAt(ltc, -12.5f, 0.5f));                              // the outline grew west,
    CHECK(wetAt(ltc, 18.5f, 0.5f));                               // the channel is still there,
    CHECK_FALSE(wetAt(ltc, -3.5f, 0.5f));                         // and so is the notch.
}

TEST_CASE("Water lake file: a damaged outline in a scene is dropped, not trusted")
{
    TerrainComponent tc = makeLand();
    const uint16_t id = lake::create(tc, HE::UUID::generate(), rect(-10, -10, 10, 10), 10.5f);
    water::Body* b = tc.water.findBody(id);
    REQUIRE(b);
    b->polygon[1].x = std::nanf("");
    water::sanitize(tc);
    CHECK(tc.water.findBody(id)->polygon.empty());                // a plain replace on the next reshape
    CHECK(lake::reshape(tc, id, rect(-5, -5, 5, 5)).ok);

    // An outline on brush water makes no sense either.
    TerrainComponent tc2 = makeLand();
    const uint16_t brushBody = tc2.water.createBody(1.0f);
    tc2.water.findBody(brushBody)->polygon = rect(0, 0, 1, 1);
    water::sanitize(tc2);
    CHECK(tc2.water.findBody(brushBody)->polygon.empty());
}
