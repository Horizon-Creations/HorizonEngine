#include "doctest.h"
#include "EditorUndo.h"
#include <HorizonScene/TerrainSculpt.h>
#include <HorizonScene/TerrainMeshGenerator.h>
#include <HorizonScene/TerrainSystem.h>
#include <HorizonScene/WaterField.h>
#include <HorizonScene/WaterSurface.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TerrainChunkComponent.h>
#include <HorizonScene/Components/WaterSurfaceComponent.h>
#include <HorizonScene/Components/LODComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/ContentManager.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <utility>
#include <vector>

namespace sculpt = TerrainSculpt;
namespace water  = HE::water;

// Excavation under a polygon (TerrainSculpt::excavatePolygon): the ground inside
// goes to a floor, a soft bank eases back to the old ground, nothing beyond it is
// touched, the chunks under it are rebuilt, and the editor's one undo step gives
// the old ground back bit for bit.
//
// Terrain 64 × 64 m at 65 vertices unless a test says otherwise, so a vertex sits
// on every whole metre (x = -32 + column) and a polygon with whole-metre corners
// has its edges exactly on vertices.

namespace
{
    // Ground that is never flat and never below 4: a slope plus a ripple, so a
    // vertex that moved cannot be mistaken for one that was already there.
    float baseHeight(float x, float z)
    {
        return 10.0f + 0.1f * x + 0.05f * z + std::sin(x * 0.7f) * std::cos(z * 0.5f);
    }

    TerrainComponent makeLand(float sizeX = 64.0f, float sizeZ = 64.0f, uint32_t res = 65)
    {
        TerrainComponent tc;
        tc.sizeX = sizeX; tc.sizeZ = sizeZ;
        tc.resolution = res;
        tc.dirty = false;       // a landscape that has been built; a default one wants a full rebuild
        tc.sculptHeights.resize(static_cast<size_t>(res) * res);
        for (uint32_t z = 0; z < res; ++z)
            for (uint32_t x = 0; x < res; ++x)
                tc.sculptHeights[static_cast<size_t>(z) * res + x] =
                    baseHeight(-sizeX * 0.5f + sizeX * x / static_cast<float>(res - 1),
                               -sizeZ * 0.5f + sizeZ * z / static_cast<float>(res - 1));
        return tc;
    }

    std::vector<glm::vec2> rect(float x0, float z0, float x1, float z1)
    {
        return { { x0, z0 }, { x1, z0 }, { x1, z1 }, { x0, z1 } };
    }

    // A U: 40 × 40 with a notch 20 wide cut from the top (+z) down to z = -10.
    std::vector<glm::vec2> uShape()
    {
        return { { -20, -20 }, { 20, -20 }, { 20, 20 }, { 10, 20 },
                 { 10, -10 }, { -10, -10 }, { -10, 20 }, { -20, 20 } };
    }

    // Where vertex (xi, zi) is, terrain-local.
    glm::vec2 posOf(const TerrainComponent& tc, uint32_t xi, uint32_t zi)
    {
        const uint32_t res = tc.resolution;
        return { -tc.sizeX * 0.5f + tc.sizeX * static_cast<float>(xi) / static_cast<float>(res - 1),
                 -tc.sizeZ * 0.5f + tc.sizeZ * static_cast<float>(zi) / static_cast<float>(res - 1) };
    }

    // The reference, written the dumb way and in double: crossing-number inside
    // test, distance to every edge, no grid tricks.
    struct Ref
    {
        const std::vector<glm::vec2>& poly;

        bool inside(double x, double z) const
        {
            bool in = false;
            for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
            {
                const double ax = poly[i].x, az = poly[i].y, bx = poly[j].x, bz = poly[j].y;
                if ((az > z) != (bz > z) && x < (bx - ax) * (z - az) / (bz - az) + ax) in = !in;
            }
            return in;
        }

        double dist(double x, double z) const
        {
            double best = 1e300;
            for (size_t i = 0; i < poly.size(); ++i)
            {
                const glm::dvec2 a(poly[i]), b(poly[(i + 1) % poly.size()]);
                const glm::dvec2 ab = b - a;
                const double len2 = ab.x * ab.x + ab.y * ab.y;
                double t = len2 > 0 ? ((x - a.x) * ab.x + (z - a.y) * ab.y) / len2 : 0.0;
                t = std::clamp(t, 0.0, 1.0);
                best = std::min(best, std::hypot(x - (a.x + t * ab.x), z - (a.y + t * ab.y)));
            }
            return best;
        }

        // The weight the header promises: 1 on and inside, smoothstep to 0 at `falloff`.
        double weight(double x, double z, double falloff) const
        {
            if (inside(x, z)) return 1.0;
            const double d = dist(x, z);
            if (d <= 1e-9) return 1.0;
            if (d >= falloff) return 0.0;
            const double t = 1.0 - d / falloff;
            return t * t * (3.0 - 2.0 * t);
        }
    };

    // Compare every vertex of `after` with what the reference says it should be.
    // Returns the number of vertices that differ by more than `tol`; vertices within
    // `skip` of the outline are left out (a slanted edge can put one on either side
    // by rounding, and the weight there is ~1 either way).
    int mismatches(const TerrainComponent& tc, const std::vector<float>& before,
                   const std::vector<glm::vec2>& poly, sculpt::ExcavateMode mode,
                   float amount, float falloff, double tol = 1e-4, double skip = 1e-3)
    {
        const Ref ref{ poly };
        int bad = 0;
        const uint32_t res = tc.resolution;
        for (uint32_t zi = 0; zi < res; ++zi)
            for (uint32_t xi = 0; xi < res; ++xi)
            {
                const glm::vec2 p = posOf(tc, xi, zi);
                if (ref.dist(p.x, p.y) < skip) continue;
                const double w = ref.weight(p.x, p.y, falloff);
                const double was = before[static_cast<size_t>(zi) * res + xi];
                double want = was;
                if (mode == sculpt::ExcavateMode::Floor)
                    want = was > amount ? was + w * (amount - was) : was;
                else
                    want = was - w * std::max(0.0f, amount);
                if (std::fabs(tc.sculptHeights[static_cast<size_t>(zi) * res + xi] - want) > tol)
                {
                    ++bad;
                    MESSAGE("vertex " << xi << "," << zi << " at " << p.x << "," << p.y << " got "
                            << tc.sculptHeights[static_cast<size_t>(zi) * res + xi] << " want " << want
                            << " was " << was << " w " << w << " d " << ref.dist(p.x, p.y)
                            << " inside " << ref.inside(p.x, p.y));
                }
            }
        return bad;
    }

    bool sameBits(const std::vector<float>& a, const std::vector<float>& b)
    {
        return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
    }

    sculpt::ExcavateParams floorAt(float height, float falloff)
    {
        sculpt::ExcavateParams p;
        p.mode = sculpt::ExcavateMode::Floor; p.amount = height; p.falloff = falloff;
        return p;
    }

    sculpt::ExcavateParams digBy(float depth, float falloff)
    {
        sculpt::ExcavateParams p;
        p.mode = sculpt::ExcavateMode::Dig; p.amount = depth; p.falloff = falloff;
        return p;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// The shape of the pit
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Excavate: inside the polygon the ground is exactly the floor, beyond the bank it is untouched")
{
    TerrainComponent tc = makeLand();
    const std::vector<float> before = tc.sculptHeights;
    const float floorY = 2.0f, falloff = 4.0f;

    const auto r = sculpt::excavatePolygon(tc, rect(-10, -10, 10, 10), floorAt(floorY, falloff));
    REQUIRE(r.ok);
    CHECK(r.changed > 0);

    int inside = 0, beyond = 0, band = 0;
    for (uint32_t zi = 0; zi < 65; ++zi)
        for (uint32_t xi = 0; xi < 65; ++xi)
        {
            const float x = -32.0f + xi, z = -32.0f + zi;
            const float h = tc.sculptHeights[zi * 65 + xi], was = before[zi * 65 + xi];
            const float dx = std::max(std::fabs(x) - 10.0f, 0.0f), dz = std::max(std::fabs(z) - 10.0f, 0.0f);
            const float d = std::sqrt(dx * dx + dz * dz);
            if (d == 0.0f)         { CHECK(h == floorY); ++inside; }   // == on purpose: a floor, not nearly one
            else if (d >= falloff) { CHECK(h == was);    ++beyond; }   // bit-equal: not written at all
            else
            {
                // In the bank: between the floor and the old ground, strictly.
                CHECK(h > floorY);
                CHECK(h < was);
                ++band;
            }
        }
    CHECK(inside == 21 * 21);
    CHECK(beyond > 0);
    CHECK(band > 0);
}

TEST_CASE("Excavate: the bank is a smoothstep of the distance from the outline")
{
    TerrainComponent tc = makeLand();
    const std::vector<float> before = tc.sculptHeights;
    REQUIRE(sculpt::excavatePolygon(tc, rect(-10, -10, 10, 10), floorAt(2.0f, 4.0f)).ok);

    // Along z = 0 east of the edge at x = 10: distances 1, 2, 3.
    float prev = 2.0f;
    for (int d = 1; d <= 3; ++d)
    {
        const uint32_t xi = 32 + 10 + d, zi = 32;
        const float was = before[zi * 65 + xi];
        const float t = 1.0f - d / 4.0f, w = t * t * (3.0f - 2.0f * t);
        CHECK(tc.sculptHeights[zi * 65 + xi] == doctest::Approx(was + w * (2.0f - was)).epsilon(1e-5));
        CHECK(tc.sculptHeights[zi * 65 + xi] > prev);   // the bank rises away from the pit
        prev = tc.sculptHeights[zi * 65 + xi];
    }
    // Round the corner the distance is Euclidean, not the larger of two axes.
    // (12, 12) is 2.83 m from the corner (10, 10).
    const float was = before[(32 + 12) * 65 + (32 + 12)];
    const float dc = std::sqrt(8.0f), t = 1.0f - dc / 4.0f, w = t * t * (3.0f - 2.0f * t);
    CHECK(tc.sculptHeights[(32 + 12) * 65 + (32 + 12)] == doctest::Approx(was + w * (2.0f - was)).epsilon(1e-5));
}

TEST_CASE("Excavate: a concave polygon leaves the notch alone beyond the bank")
{
    TerrainComponent tc = makeLand();
    const std::vector<float> before = tc.sculptHeights;
    const auto u = uShape();
    REQUIRE(sculpt::excavatePolygon(tc, u, floorAt(2.0f, 3.0f)).ok);

    auto h = [&](float x, float z) { return tc.sculptHeights[static_cast<size_t>(z + 32) * 65 + static_cast<size_t>(x + 32)]; };
    auto was = [&](float x, float z) { return before[static_cast<size_t>(z + 32) * 65 + static_cast<size_t>(x + 32)]; };

    CHECK(h(-15, 0) == 2.0f);                // left arm
    CHECK(h(15, 0) == 2.0f);                 // right arm
    CHECK(h(0, -15) == 2.0f);                // the bottom of the U
    CHECK(h(0, 10) == was(0, 10));           // the middle of the notch: 10 m from both arms
    CHECK(h(0, 0) == was(0, 0));             // 10 m from the bottom edge
    CHECK(h(-8, 10) < was(-8, 10));          // 2 m from the left arm: in its bank
    CHECK(h(8, 10) < was(8, 10));            // and the right
    CHECK(h(0, -8) < was(0, -8));            // 2 m above the bottom edge
    CHECK(h(-9, 10) < h(-8, 10));            // closer to the arm, deeper

    CHECK(mismatches(tc, before, u, sculpt::ExcavateMode::Floor, 2.0f, 3.0f) == 0);
}

TEST_CASE("Excavate: every vertex agrees with a brute-force reference, on odd shapes and a non-square terrain")
{
    std::mt19937 rng(0xC0FFEE);
    auto uni = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };

    for (int round = 0; round < 24; ++round)
    {
        // 80 × 48 m, 33 vertices a side: a different step in x (2.5) and z (1.5).
        TerrainComponent tc = makeLand(80.0f, 48.0f, 33);
        if (round % 3 == 2) tc = makeLand(64.0f, 64.0f, 65);
        const std::vector<float> before = tc.sculptHeights;

        // A star-shaped polygon (simple by construction), anywhere — often partly off the terrain.
        const int n = 5 + static_cast<int>(rng() % 10);
        const glm::vec2 c(uni(-30.0f, 30.0f), uni(-20.0f, 20.0f));
        std::vector<glm::vec2> poly;
        for (int i = 0; i < n; ++i)
        {
            const float a = (static_cast<float>(i) + uni(0.05f, 0.95f)) * 6.2831853f / static_cast<float>(n);
            const float rad = uni(3.0f, 18.0f);
            poly.emplace_back(c.x + rad * std::cos(a), c.y + rad * std::sin(a));
        }
        const float falloff = (round % 4 == 0) ? 0.0f : uni(0.5f, 7.0f);
        const bool dig = (round % 2) == 1;
        const float amount = dig ? uni(0.5f, 6.0f) : uni(1.0f, 9.0f);

        const auto r = sculpt::excavatePolygon(tc, poly, dig ? digBy(amount, falloff) : floorAt(amount, falloff));
        REQUIRE(r.ok);
        CHECK(mismatches(tc, before, poly, dig ? sculpt::ExcavateMode::Dig : sculpt::ExcavateMode::Floor,
                         amount, falloff) == 0);
    }
}

TEST_CASE("Excavate: winding and starting corner do not matter, bit for bit")
{
    const auto u = uShape();
    TerrainComponent a = makeLand();
    REQUIRE(sculpt::excavatePolygon(a, u, floorAt(2.0f, 3.0f)).ok);

    std::vector<glm::vec2> reversed(u.rbegin(), u.rend());
    TerrainComponent b = makeLand();
    REQUIRE(sculpt::excavatePolygon(b, reversed, floorAt(2.0f, 3.0f)).ok);
    CHECK(sameBits(a.sculptHeights, b.sculptHeights));

    std::vector<glm::vec2> rotated(u.begin() + 3, u.end());
    rotated.insert(rotated.end(), u.begin(), u.begin() + 3);
    TerrainComponent c = makeLand();
    REQUIRE(sculpt::excavatePolygon(c, rotated, floorAt(2.0f, 3.0f)).ok);
    CHECK(sameBits(a.sculptHeights, c.sculptHeights));
}

// ═════════════════════════════════════════════════════════════════════════════
// Floor and Dig
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Excavate Floor never raises: ground already below the floor stays where it is")
{
    TerrainComponent tc = makeLand();
    // A pit that is already deeper than the floor asked for, straddling the edge.
    for (int z = -4; z <= 4; ++z)
        for (int x = 7; x <= 14; ++x)
            tc.sculptHeights[static_cast<size_t>(z + 32) * 65 + static_cast<size_t>(x + 32)] = -5.0f;
    const std::vector<float> before = tc.sculptHeights;

    REQUIRE(sculpt::excavatePolygon(tc, rect(-10, -10, 10, 10), floorAt(2.0f, 4.0f)).ok);
    for (int z = -4; z <= 4; ++z)
        for (int x = 7; x <= 14; ++x)
        {
            const size_t i = static_cast<size_t>(z + 32) * 65 + static_cast<size_t>(x + 32);
            CHECK(tc.sculptHeights[i] == -5.0f);
        }
    CHECK(tc.sculptHeights[32 * 65 + 32] == 2.0f);   // the rest of the inside still got its floor
}

TEST_CASE("Excavate Dig lowers every vertex by the depth and keeps the relief")
{
    TerrainComponent tc = makeLand();
    const std::vector<float> before = tc.sculptHeights;
    REQUIRE(sculpt::excavatePolygon(tc, rect(-10, -10, 10, 10), digBy(3.0f, 2.0f)).ok);

    for (int z = -10; z <= 10; ++z)
        for (int x = -10; x <= 10; ++x)
        {
            const size_t i = static_cast<size_t>(z + 32) * 65 + static_cast<size_t>(x + 32);
            CHECK(tc.sculptHeights[i] == before[i] - 3.0f);        // exactly the old height minus the depth
        }
    // The ground two metres out is untouched; one metre out is part-way.
    const size_t far1 = 32 * 65 + (32 + 12), near1 = 32 * 65 + (32 + 11);
    CHECK(tc.sculptHeights[far1] == before[far1]);
    const float t = 0.5f, w = t * t * (3.0f - 2.0f * t);
    CHECK(tc.sculptHeights[near1] == doctest::Approx(before[near1] - w * 3.0f).epsilon(1e-5));

    // A negative depth is no depth: nothing is raised.
    TerrainComponent up = makeLand();
    const auto r = sculpt::excavatePolygon(up, rect(-10, -10, 10, 10), digBy(-3.0f, 2.0f));
    CHECK(r.ok);
    CHECK(r.changed == 0u);
    CHECK(sameBits(up.sculptHeights, before));
}

TEST_CASE("Excavate: no bank is a hard edge")
{
    TerrainComponent tc = makeLand();
    const std::vector<float> before = tc.sculptHeights;
    REQUIRE(sculpt::excavatePolygon(tc, rect(-10, -10, 10, 10), floorAt(2.0f, 0.0f)).ok);

    for (uint32_t zi = 0; zi < 65; ++zi)
        for (uint32_t xi = 0; xi < 65; ++xi)
        {
            const bool in = std::abs(static_cast<int>(xi) - 32) <= 10 && std::abs(static_cast<int>(zi) - 32) <= 10;
            const size_t i = zi * 65 + xi;
            if (in) CHECK(tc.sculptHeights[i] == 2.0f);            // the outline itself counts as inside
            else    CHECK(tc.sculptHeights[i] == before[i]);       // one metre out: not touched
        }
}

TEST_CASE("Excavate: the result says what moved")
{
    TerrainComponent tc = makeLand();
    const auto r = sculpt::excavatePolygon(tc, rect(-10, -10, 10, 10), floorAt(2.0f, 4.0f));
    REQUIRE(r.ok);
    // Over the vertices the polygon and its bank reach (weight > 0: less than 4 m out), after.
    float lo = 1e30f, hi = -1e30f;
    for (uint32_t zi = 0; zi < 65; ++zi)
        for (uint32_t xi = 0; xi < 65; ++xi)
        {
            const float dx = std::max(std::fabs(-32.0f + xi) - 10.0f, 0.0f), dz = std::max(std::fabs(-32.0f + zi) - 10.0f, 0.0f);
            if (std::sqrt(dx * dx + dz * dz) >= 4.0f) continue;
            lo = std::min(lo, tc.sculptHeights[zi * 65 + xi]);
            hi = std::max(hi, tc.sculptHeights[zi * 65 + xi]);
        }
    CHECK(r.minHeight == 2.0f);
    CHECK(r.minHeight == lo);
    CHECK(r.maxHeight == hi);
    CHECK(r.maxHeight > 6.0f);                                     // the outer bank is most of the way back up
    CHECK(r.centerHeight == 2.0f);                                 // the middle of the box, in the pit

    // Floor above the whole terrain: legal, and nothing moves.
    TerrainComponent flat = makeLand();
    const std::vector<float> before = flat.sculptHeights;
    const auto none = sculpt::excavatePolygon(flat, rect(-10, -10, 10, 10), floorAt(100.0f, 4.0f));
    CHECK(none.ok);
    CHECK(none.changed == 0u);
    CHECK(sameBits(flat.sculptHeights, before));
    CHECK_FALSE(flat.regionDirty);
}

// ═════════════════════════════════════════════════════════════════════════════
// Bad input, and the edge of the terrain
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Excavate: input that cannot be applied changes nothing, not even by baking the heights")
{
    const float notANumber = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();

    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 64.0f; tc.resolution = 65; tc.seed = 5;          // noise only, no sculpt heights
    tc.dirty = false;
    REQUIRE(tc.sculptHeights.empty());

    CHECK_FALSE(sculpt::excavatePolygon(tc, { { 0, 0 }, { 5, 0 } }, floorAt(2.0f, 2.0f)).ok);          // two points
    CHECK_FALSE(sculpt::excavatePolygon(tc, { { 0, 0 }, { 5, 0 }, { 10, 0 } }, floorAt(2.0f, 2.0f)).ok); // a line
    CHECK_FALSE(sculpt::excavatePolygon(tc, { { 0, 0 }, { 5, 0 }, { 0, notANumber } }, floorAt(2.0f, 2.0f)).ok);
    CHECK_FALSE(sculpt::excavatePolygon(tc, { { 0, 0 }, { 5, 0 }, { infinity, 5 } }, floorAt(2.0f, 2.0f)).ok);
    CHECK_FALSE(sculpt::excavatePolygon(tc, { { 0, 0 }, { 5, 0 }, { 1e30f, 5 } }, floorAt(2.0f, 2.0f)).ok);
    CHECK_FALSE(sculpt::excavatePolygon(tc, rect(-5, -5, 5, 5), floorAt(notANumber, 2.0f)).ok);
    CHECK_FALSE(sculpt::excavatePolygon(tc, rect(-5, -5, 5, 5), floorAt(2.0f, notANumber)).ok);
    CHECK_FALSE(sculpt::excavatePolygon(tc, rect(-5, -5, 5, 5), floorAt(2.0f, infinity)).ok);

    TerrainComponent empty;
    empty.sizeX = 0.0f;
    CHECK_FALSE(sculpt::excavatePolygon(empty, rect(-5, -5, 5, 5), floorAt(2.0f, 2.0f)).ok);

    CHECK(tc.sculptHeights.empty());                    // the refusal came before ensureHeights
    CHECK_FALSE(tc.regionDirty);
    CHECK_FALSE(tc.dirty);
}

TEST_CASE("Excavate: a polygon off the terrain is legal and moves nothing; one half on it is clipped")
{
    TerrainComponent tc = makeLand();
    const std::vector<float> before = tc.sculptHeights;

    // Entirely off, bank included: 50 m east of a terrain that ends at 32, bank 4.
    const auto off = sculpt::excavatePolygon(tc, rect(50, -5, 60, 5), floorAt(2.0f, 4.0f));
    CHECK(off.ok);
    CHECK(off.changed == 0u);
    CHECK(sameBits(tc.sculptHeights, before));
    CHECK_FALSE(tc.regionDirty);

    // Only the bank reaches the terrain: 33 → 37, the terrain ends at 32 (bank 4 → 33 - 4 = 29).
    const auto edge = sculpt::excavatePolygon(tc, rect(33, -5, 40, 5), floorAt(2.0f, 4.0f));
    CHECK(edge.ok);
    CHECK(edge.changed > 0u);
    CHECK(tc.sculptHeights[32 * 65 + 64] < before[32 * 65 + 64]);        // x = 32, one metre from the outline

    // Half on, half off: the corner of the terrain is a pit, the rest of it is as it was.
    TerrainComponent corner = makeLand();
    const std::vector<float> b2 = corner.sculptHeights;
    REQUIRE(sculpt::excavatePolygon(corner, rect(20, 20, 100, 100), floorAt(2.0f, 3.0f)).ok);
    for (int z = 20; z <= 32; ++z)
        for (int x = 20; x <= 32; ++x)
            CHECK(corner.sculptHeights[static_cast<size_t>(z + 32) * 65 + static_cast<size_t>(x + 32)] == 2.0f);
    CHECK(corner.sculptHeights[0] == b2[0]);
    CHECK(corner.sculptHeights[32 * 65 + 32] == b2[32 * 65 + 32]);

    // A polygon far bigger than the terrain, around all of it: the whole terrain is the floor.
    TerrainComponent all = makeLand();
    REQUIRE(sculpt::excavatePolygon(all, rect(-1000, -1000, 1000, 1000), floorAt(2.0f, 1.0e6f)).ok);
    for (float h : all.sculptHeights) CHECK(h == 2.0f);
}

TEST_CASE("Excavate: a terrain that was never sculpted is baked first, and the noise is what gets cut")
{
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 64.0f; tc.resolution = 65; tc.seed = 7; tc.heightScale = 20.0f;
    REQUIRE(tc.sculptHeights.empty());
    const std::vector<float> noise = computeTerrainHeightField(tc);

    REQUIRE(sculpt::excavatePolygon(tc, rect(-10, -10, 10, 10), floorAt(3.0f, 4.0f)).ok);
    REQUIRE(tc.sculptHeights.size() == 65u * 65u);
    CHECK(mismatches(tc, noise, rect(-10, -10, 10, 10), sculpt::ExcavateMode::Floor, 3.0f, 4.0f) == 0);
    // The terrain outside the bank keeps its fBm shape rather than going flat.
    CHECK(tc.sculptHeights[0] == noise[0]);
    CHECK(tc.sculptHeights[64 * 65 + 64] == noise[64 * 65 + 64]);
}

TEST_CASE("Excavate: a resolution that is not 2^n+1 is snapped first, so the rebuild is a full one")
{
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 64.0f; tc.resolution = 100; tc.seed = 3;
    REQUIRE(sculpt::excavatePolygon(tc, rect(-10, -10, 10, 10), floorAt(3.0f, 2.0f)).ok);
    CHECK(tc.resolution == 129u);
    CHECK(tc.sculptHeights.size() == 129u * 129u);
    CHECK(tc.dirty);
    // The centre vertex (64, 64) is inside the pit at the snapped grid's step.
    CHECK(tc.sculptHeights[64 * 129 + 64] <= 3.0f);
}

TEST_CASE("Excavate: a big lake with a long outline on a big landscape takes a moment, not a minute")
{
    // 1 km at 1025 vertices, a 256-point outline 600 m across and a 20 m bank: about
    // 280 000 vertices inside. The work is the vertices plus the outline's length
    // times the bank, not the vertices times the outline.
    TerrainComponent tc = makeLand(1000.0f, 1000.0f, 1025);
    const std::vector<float> before = tc.sculptHeights;
    std::vector<glm::vec2> poly;
    for (int i = 0; i < 256; ++i)
    {
        const float a = static_cast<float>(i) * 6.2831853f / 256.0f;
        const float rad = 300.0f + 40.0f * std::sin(a * 5.0f);
        poly.emplace_back(rad * std::cos(a), rad * std::sin(a));
    }
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = sculpt::excavatePolygon(tc, poly, floorAt(0.0f, 20.0f));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("excavatePolygon, 1025² vertices, 256 points, bank 20 m: " << ms << " ms");
    REQUIRE(r.ok);
    CHECK(r.changed > 150000u);   // the ground west of x = -100 is below the floor already
    CHECK(ms < 30000.0);        // a Debug build on a loaded runner: this guards the order of growth, not the speed
    CHECK(mismatches(tc, before, poly, sculpt::ExcavateMode::Floor, 0.0f, 20.0f) == 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// Which chunks are rebuilt
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Excavate: the dirty rectangle is the vertices that moved plus one step, joined to what was there")
{
    TerrainComponent tc = makeLand();
    REQUIRE(sculpt::excavatePolygon(tc, rect(-10, -10, 10, 10), floorAt(2.0f, 4.0f)).ok);

    // Vertices move out to 3 m past the outline (13); 4 m is the end of the bank (weight 0).
    REQUIRE(tc.regionDirty);
    CHECK(tc.dirtyMinX == -14.0f);
    CHECK(tc.dirtyMaxX == 14.0f);
    CHECK(tc.dirtyMinZ == -14.0f);
    CHECK(tc.dirtyMaxZ == 14.0f);
    CHECK_FALSE(tc.dirty);                          // a region update, not a regeneration

    // A second excavation joins the first's rectangle instead of replacing it.
    REQUIRE(sculpt::excavatePolygon(tc, rect(20, 20, 24, 24), floorAt(2.0f, 1.0f)).ok);
    CHECK(tc.dirtyMinX == -14.0f);
    CHECK(tc.dirtyMaxX == 25.0f);
    CHECK(tc.dirtyMaxZ == 25.0f);
    CHECK_FALSE(tc.dirty);
}

TEST_CASE("Excavate: only the chunks under the pit are rebuilt, every LOD level of them")
{
    HorizonWorld world;
    ContentManager cm(".");
    auto& reg = world.registry();

    // 64 m at 257 vertices: 4 × 4 chunks of 16 m, four LOD levels each.
    const Entity te = world.createEntity("Landscape");
    reg.emplace<TransformComponent>(te);
    TerrainComponent tc = makeLand(64.0f, 64.0f, 257);
    tc.dirty = true;
    reg.emplace<TerrainComponent>(te, tc);
    TerrainSystem::updateTerrains(world, cm);

    struct Built { uint32_t cx, cz; std::vector<HE::UUID> ids; std::vector<std::vector<float>> verts; };
    auto snapshot = [&]()
    {
        std::vector<Built> out;
        for (auto [ce, cc, lod] : reg.view<TerrainChunkComponent, LODComponent>().each())
        {
            Built b{ cc.cx, cc.cz, {}, {} };
            for (const auto& lv : lod.levels)
            {
                const StaticMeshAsset* m = cm.getStaticMesh(lv.meshId);
                REQUIRE(m);
                b.ids.push_back(lv.meshId);
                b.verts.push_back(m->vertices);
            }
            out.push_back(std::move(b));
        }
        return out;
    };
    auto find = [](std::vector<Built>& v, uint32_t cx, uint32_t cz) -> Built&
    {
        for (auto& b : v) if (b.cx == cx && b.cz == cz) return b;
        FAIL("no such chunk");
        return v.front();
    };

    const auto first = snapshot();
    REQUIRE(first.size() == 16u);
    REQUIRE(first.front().ids.size() == 4u);

    // A pit that fills chunk (2, 2), x and z in [0, 16], and ends inside it.
    auto& live = reg.get<TerrainComponent>(te);
    REQUIRE(sculpt::excavatePolygon(live, rect(2, 2, 14, 14), floorAt(1.0f, 1.0f)).ok);
    CHECK_FALSE(live.dirty);
    TerrainSystem::updateTerrains(world, cm);

    // `rebuilt` chunks have new vertices at every LOD level (same UUIDs: the renderer
    // re-uploads, it does not re-register); every other chunk is byte-for-byte as it was.
    auto expectRebuilt = [&](std::vector<Built> was, std::vector<Built> now,
                             std::vector<std::pair<uint32_t, uint32_t>> rebuilt, float floorY)
    {
        REQUIRE(now.size() == 16u);
        for (uint32_t cz = 0; cz < 4; ++cz)
            for (uint32_t cx = 0; cx < 4; ++cx)
            {
                Built& w = find(was, cx, cz);
                Built& n = find(now, cx, cz);
                REQUIRE(w.verts.size() == n.verts.size());
                const bool hit = std::find(rebuilt.begin(), rebuilt.end(), std::make_pair(cx, cz)) != rebuilt.end();
                for (size_t k = 0; k < w.verts.size(); ++k)
                {
                    CHECK(w.ids[k] == n.ids[k]);
                    if (hit) CHECK(w.verts[k] != n.verts[k]);
                    else     CHECK(w.verts[k] == n.verts[k]);   // not touched, not rebuilt
                }
                if (cx == 2 && cz == 2)
                    for (size_t k = 0; k < n.verts.size(); ++k)
                    {
                        bool onFloor = false;                   // the floor shows in the mesh
                        for (size_t i = 1; i < n.verts[k].size(); i += 3)
                            onFloor = onFloor || std::fabs(n.verts[k][i] - floorY) < 1e-4f;
                        CHECK(onFloor);
                    }
            }
    };
    const auto second = snapshot();
    expectRebuilt(first, second, { { 2, 2 } }, 1.0f);
    CHECK_FALSE(live.regionDirty);                         // TerrainSystem consumed the rectangle

    // A pit whose edge sits ON the border between chunks (2, 2) and (3, 2), x = 16:
    // the vertex there belongs to both, and so both are rebuilt (the seam stays shut).
    REQUIRE(sculpt::excavatePolygon(live, rect(10, 2, 16, 14), floorAt(0.5f, 0.0f)).ok);
    TerrainSystem::updateTerrains(world, cm);
    expectRebuilt(second, snapshot(), { { 2, 2 }, { 3, 2 } }, 0.5f);
}

TEST_CASE("Excavate: a pit under a sheet of water that the plateau had hidden brings the sheet back")
{
    HorizonWorld world;
    ContentManager cm(".");
    auto& reg = world.registry();
    const Entity te = world.createEntity("Landscape");
    reg.emplace<TransformComponent>(te);
    TerrainComponent tc = makeLand(64.0f, 64.0f, 65);
    std::fill(tc.sculptHeights.begin(), tc.sculptHeights.end(), 5.0f);       // a flat plateau
    tc.water.res = 64;
    tc.dirty = true;
    const uint16_t lake = tc.water.createBody(1.0f);                         // 4 m under the ground
    REQUIRE(water::addPolygon(tc, lake, rect(-20, -20, 20, 20)).ok);
    reg.emplace<TerrainComponent>(te, tc);

    auto sheets = [&] { size_t n = 0; for (auto e : reg.view<WaterSurfaceComponent>()) { (void)e; ++n; } return n; };
    TerrainSystem::updateTerrains(world, cm);
    CHECK(sheets() == 0u);                                  // the ground is above the water everywhere

    auto& live = reg.get<TerrainComponent>(te);
    REQUIRE(sculpt::excavatePolygon(live, rect(-8, -8, 8, 8), floorAt(-2.0f, 3.0f)).ok);
    TerrainSystem::updateTerrains(world, cm);
    REQUIRE(sheets() == 1u);                                // the pit holds water now
    CHECK_FALSE(live.water.dirty);
    CHECK(live.water.wetCells(lake) == 1600u);              // the field never changed, only the ground
}

// ═════════════════════════════════════════════════════════════════════════════
// Undo
// ═════════════════════════════════════════════════════════════════════════════

namespace
{
    struct UndoRig
    {
        HorizonWorld world;
        EditorUndo   undo;

        UndoRig() { undo.setWorld(&world); }

        Entity makeLandscape(TerrainComponent tc)
        {
            const Entity e = world.createEntity("Landscape");
            world.registry().emplace<TransformComponent>(e);
            world.registry().emplace<TerrainComponent>(e, std::move(tc));
            return e;
        }

        // Undo mints new handles; the one landscape in the world is found again.
        TerrainComponent& landscape()
        {
            auto view = world.registry().view<TerrainComponent>();
            REQUIRE(view.size() == 1u);
            return world.registry().get<TerrainComponent>(*view.begin());
        }
    };
}

TEST_CASE("Excavate undo: one step gives the old ground back bit for bit, redo digs it again")
{
    UndoRig rig;
    rig.makeLandscape(makeLand());
    const std::vector<float> before = rig.landscape().sculptHeights;

    // The editor's protocol: one snapshot, one excavation.
    rig.undo.snapshotNow("Excavate");
    REQUIRE(sculpt::excavatePolygon(rig.landscape(), uShape(), floorAt(2.0f, 3.0f)).ok);
    const std::vector<float> dug = rig.landscape().sculptHeights;
    REQUIRE_FALSE(sameBits(dug, before));
    CHECK(rig.undo.undoDepth() == 1u);

    REQUIRE(rig.undo.undo());
    CHECK(sameBits(rig.landscape().sculptHeights, before));      // memcmp, not Approx
    CHECK(rig.undo.undoDepth() == 0u);
    CHECK(rig.undo.redoDepth() == 1u);

    REQUIRE(rig.undo.redo());
    CHECK(sameBits(rig.landscape().sculptHeights, dug));
}

TEST_CASE("Excavate undo: a landscape that was never sculpted goes back to having no sculpt heights")
{
    UndoRig rig;
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 64.0f; tc.resolution = 100; tc.seed = 9;     // noise, and a resolution to snap
    rig.makeLandscape(tc);
    REQUIRE(rig.landscape().sculptHeights.empty());

    rig.undo.snapshotNow("Excavate");
    REQUIRE(sculpt::excavatePolygon(rig.landscape(), rect(-10, -10, 10, 10), floorAt(3.0f, 2.0f)).ok);
    REQUIRE(rig.landscape().sculptHeights.size() == 129u * 129u);      // baked, resampled grid
    REQUIRE(rig.landscape().resolution == 129u);

    REQUIRE(rig.undo.undo());
    CHECK(rig.landscape().sculptHeights.empty());                      // not 129² of baked noise
    CHECK(rig.landscape().resolution == 100u);
    CHECK(rig.undo.undoDepth() == 0u);
}

TEST_CASE("Excavate undo: two excavations are two steps, taken back newest first")
{
    UndoRig rig;
    rig.makeLandscape(makeLand());
    const std::vector<float> original = rig.landscape().sculptHeights;

    rig.undo.snapshotNow("Excavate");
    REQUIRE(sculpt::excavatePolygon(rig.landscape(), rect(-20, -20, -10, -10), floorAt(2.0f, 2.0f)).ok);
    const std::vector<float> one = rig.landscape().sculptHeights;

    rig.undo.snapshotNow("Excavate");
    REQUIRE(sculpt::excavatePolygon(rig.landscape(), rect(5, 5, 15, 15), digBy(4.0f, 2.0f)).ok);
    CHECK(rig.undo.undoDepth() == 2u);

    REQUIRE(rig.undo.undo());
    CHECK(sameBits(rig.landscape().sculptHeights, one));
    REQUIRE(rig.undo.undo());
    CHECK(sameBits(rig.landscape().sculptHeights, original));
}
