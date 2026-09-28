#include "doctest.h"
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/TerrainGenerate.h>
#include <HorizonScene/TerrainSculpt.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// ── A mountain grown inside an area ───────────────────────────────────────────
// TerrainGenerate::mountain is a value in, a value out. What is worth asking:
// does the ground rise inside the area and come back down to nothing at its rim
// by the falloff it was given, is not one vertex outside the area touched, is
// the formation ADDED to what was there, and does the same seed give the same
// heights. Every terrain here is 128 m at resolution 129 — already 2ⁿ+1, so
// ensureHeights does not resample, and one grid step is exactly one metre: the
// profile is read off vertices, not off a bilinear sample between them.

namespace
{
    constexpr uint32_t kRes  = 129;
    constexpr float    kSize = 128.0f;

    TerrainComponent terrain(int seed = 0)
    {
        TerrainComponent tc;
        tc.sizeX = tc.sizeZ = kSize;
        tc.resolution  = kRes;
        tc.heightScale = 20.0f;
        tc.seed        = seed;
        // A fresh component starts dirty (nothing is built yet); clear it the way
        // TerrainSystem does after the first build, so a check that the
        // generator did not set it means something.
        tc.dirty       = false;
        return tc;
    }

    // Height of the vertex at terrain-local whole metres (x, z).
    float at(const TerrainComponent& tc, int x, int z)
    {
        const int xi = x + static_cast<int>(kSize / 2);
        const int zi = z + static_cast<int>(kSize / 2);
        return tc.sculptHeights[static_cast<size_t>(zi) * kRes + xi];
    }

    TerrainGenerate::Params smooth(float maxHeight, float falloff)
    {
        TerrainGenerate::Params p;
        p.maxHeight = maxHeight;
        p.falloff   = falloff;
        p.roughness = 0.0f;   // no noise: the profile is exactly the rim blend
        return p;
    }
}

TEST_CASE("TerrainGenerate::mountain rises inside the area and falls to zero across the falloff")
{
    TerrainComponent tc = terrain();
    const TerrainGenerate::Area area{ 0.0f, 0.0f, 20.0f, 20.0f };

    const TerrainGenerate::Result r = TerrainGenerate::mountain(tc, area, smooth(30.0f, 10.0f));
    REQUIRE(r.ok);
    CHECK(r.changed > 0);
    CHECK(r.peakAdded == doctest::Approx(30.0f));
    REQUIRE(tc.sculptHeights.size() == kRes * kRes);

    // Full height wherever the rim is at least `falloff` away…
    CHECK(at(tc, 0, 0) == doctest::Approx(30.0f));
    CHECK(at(tc, 10, 0) == doctest::Approx(30.0f));
    // …half of it halfway across the blend (smoothstep(0.5) = 0.5), the same way
    // round in every direction…
    CHECK(at(tc, 15, 0)  == doctest::Approx(15.0f));
    CHECK(at(tc, -15, 0) == doctest::Approx(15.0f));
    CHECK(at(tc, 0, 15)  == doctest::Approx(15.0f));
    CHECK(at(tc, 0, -15) == doctest::Approx(15.0f));
    // …next to nothing a metre inside the rim, and nothing on or past it.
    CHECK(at(tc, 19, 0) > 0.0f);
    CHECK(at(tc, 19, 0) < 0.05f * 30.0f);
    CHECK(at(tc, 20, 0) == 0.0f);
    CHECK(at(tc, 25, 0) == 0.0f);

    // Never climbing again on the way out.
    for (int x = 1; x <= 20; ++x)
        CHECK(at(tc, x, 0) <= at(tc, x - 1, 0));

    // Region-dirty over the area's box, not a full rebuild.
    CHECK_FALSE(tc.dirty);
    CHECK(tc.regionDirty);
    CHECK(tc.dirtyMinX == doctest::Approx(-20.0f));
    CHECK(tc.dirtyMaxX == doctest::Approx(20.0f));
    CHECK(tc.dirtyMinZ == doctest::Approx(-20.0f));
    CHECK(tc.dirtyMaxZ == doctest::Approx(20.0f));
}

TEST_CASE("TerrainGenerate::mountain with a falloff as wide as the area is one dome")
{
    TerrainComponent tc = terrain();
    const TerrainGenerate::Result r = TerrainGenerate::mountain(
        tc, { 0.0f, 0.0f, 20.0f, 20.0f }, smooth(30.0f, 40.0f));
    REQUIRE(r.ok);

    // The top is the centre, rescaled to maxHeight even though the blend never
    // reaches full weight there, and every step out is strictly lower.
    CHECK(at(tc, 0, 0) == doctest::Approx(30.0f));
    for (int x = 1; x <= 20; ++x)
        CHECK(at(tc, x, 0) < at(tc, x - 1, 0));
    CHECK(at(tc, 20, 0) == 0.0f);
}

TEST_CASE("TerrainGenerate::mountain keeps the falloff in metres on both axes of an ellipse")
{
    TerrainComponent tc = terrain();
    // Off-centre on purpose: a formation at the origin hides a missing offset.
    const TerrainGenerate::Area area{ 20.0f, -10.0f, 30.0f, 10.0f };
    const TerrainGenerate::Result r = TerrainGenerate::mountain(tc, area, smooth(12.0f, 6.0f));
    REQUIRE(r.ok);

    CHECK(at(tc, 20, -10) == doctest::Approx(12.0f));
    // Three metres inside the rim is halfway across a six-metre blend, on the
    // long axis and on the short one alike.
    CHECK(at(tc, 20 + 27, -10) == doctest::Approx(6.0f));
    CHECK(at(tc, 20 - 27, -10) == doctest::Approx(6.0f));
    CHECK(at(tc, 20, -10 + 7)  == doctest::Approx(6.0f));
    CHECK(at(tc, 20, -10 - 7)  == doctest::Approx(6.0f));

    // Past the rim, and in the corner of the bounding box the ellipse does not
    // cover, the ground is where it was.
    CHECK(at(tc, 20 + 31, -10) == 0.0f);
    CHECK(at(tc, 20, -10 + 11) == 0.0f);
    CHECK(at(tc, 20 + 25, -10 + 9) == 0.0f);

    CHECK(tc.dirtyMinX == doctest::Approx(-10.0f));
    CHECK(tc.dirtyMaxX == doctest::Approx(50.0f));
    CHECK(tc.dirtyMinZ == doctest::Approx(-20.0f));
    CHECK(tc.dirtyMaxZ == doctest::Approx(0.0f));
}

TEST_CASE("TerrainGenerate::mountain has no pit at the centre of a long ellipse with a wide falloff")
{
    // A rectangle dragged long and thin, and a falloff wider than its short
    // radius: the distance to the rim along the ray jumps with direction right at
    // the centre (short radius one way, long radius the other), which once left
    // the centre vertex at half height inside a full-height ring.
    TerrainComponent tc = terrain();
    const TerrainGenerate::Result r = TerrainGenerate::mountain(
        tc, { 0.0f, 0.0f, 30.0f, 10.0f }, smooth(20.0f, 20.0f));
    REQUIRE(r.ok);

    CHECK(at(tc, 0, 0) == doctest::Approx(20.0f));
    CHECK(at(tc, 1, 0) == doctest::Approx(at(tc, 0, 0)));
    CHECK(at(tc, 0, 0) >= at(tc, 0, 1));
    CHECK(at(tc, 0, 0) >= at(tc, 1, 1));
    // Down the short axis it only falls.
    for (int z = 1; z <= 10; ++z)
        CHECK(at(tc, 0, z) <= at(tc, 0, z - 1));
}

TEST_CASE("TerrainGenerate::mountain adds to the landscape underneath instead of replacing it")
{
    TerrainComponent tc = terrain(7);   // a seeded fBm landscape, not flat ground
    TerrainSculpt::ensureHeights(tc);
    const std::vector<float> before = tc.sculptHeights;
    REQUIRE(*std::max_element(before.begin(), before.end()) > 0.0f);

    const TerrainGenerate::Area area{ 10.0f, 10.0f, 25.0f, 25.0f };
    TerrainGenerate::Params p;
    p.maxHeight = 40.0f; p.falloff = 15.0f; p.roughness = 0.6f; p.seed = 42;
    const TerrainGenerate::Result r = TerrainGenerate::mountain(tc, area, p);
    REQUIRE(r.ok);
    CHECK(r.peakAdded == doctest::Approx(40.0f));

    float maxAdded = 0.0f;
    bool outsideUntouched = true, coreRaised = true, neverLowered = true;
    for (int z = -64; z <= 64; ++z)
        for (int x = -64; x <= 64; ++x)
        {
            const size_t i = static_cast<size_t>(z + 64) * kRes + (x + 64);
            const float added = tc.sculptHeights[i] - before[i];
            const float dx = static_cast<float>(x) - area.centerX;
            const float dz = static_cast<float>(z) - area.centerZ;
            const float r2 = dx * dx + dz * dz;
            if (r2 >= 25.0f * 25.0f)
                outsideUntouched = outsideUntouched && tc.sculptHeights[i] == before[i];
            else if (r2 <= 10.0f * 10.0f)
                coreRaised = coreRaised && added > 0.0f;
            neverLowered = neverLowered && added >= 0.0f;
            maxAdded = std::max(maxAdded, added);
        }
    CHECK(outsideUntouched);
    CHECK(coreRaised);
    CHECK(neverLowered);
    CHECK(maxAdded == doctest::Approx(40.0f).epsilon(0.001));
}

TEST_CASE("TerrainGenerate::mountain is reproducible from its seed")
{
    const TerrainGenerate::Area area{ -5.0f, 12.0f, 30.0f, 22.0f };
    TerrainGenerate::Params p;
    p.maxHeight = 25.0f; p.falloff = 12.0f; p.roughness = 0.8f; p.octaves = 6; p.seed = 1234;

    TerrainComponent a = terrain(3), b = terrain(3);
    REQUIRE(TerrainGenerate::mountain(a, area, p).ok);
    REQUIRE(TerrainGenerate::mountain(b, area, p).ok);
    CHECK(a.sculptHeights == b.sculptHeights);

    // …and the seed actually reaches the noise: another one grows another
    // mountain, and so does no noise at all.
    TerrainComponent c = terrain(3);
    TerrainGenerate::Params other = p;
    other.seed = 1235;
    REQUIRE(TerrainGenerate::mountain(c, area, other).ok);
    CHECK(c.sculptHeights != a.sculptHeights);

    TerrainComponent d = terrain(3);
    TerrainGenerate::Params plain = p;
    plain.roughness = 0.0f;
    REQUIRE(TerrainGenerate::mountain(d, area, plain).ok);
    CHECK(d.sculptHeights != a.sculptHeights);
}

TEST_CASE("TerrainGenerate::mountain with a negative height digs the same shape down")
{
    TerrainComponent tc = terrain();
    const TerrainGenerate::Result r = TerrainGenerate::mountain(
        tc, { 0.0f, 0.0f, 20.0f, 20.0f }, smooth(-8.0f, 10.0f));
    REQUIRE(r.ok);
    CHECK(r.peakAdded == doctest::Approx(-8.0f));
    CHECK(at(tc, 0, 0)  == doctest::Approx(-8.0f));
    CHECK(at(tc, 15, 0) == doctest::Approx(-4.0f));
    CHECK(at(tc, 20, 0) == 0.0f);
}

TEST_CASE("TerrainGenerate::mountain is honest about what it cannot or did not do")
{
    TerrainComponent degenerate;
    degenerate.sizeX = 0.0f;
    CHECK_FALSE(TerrainGenerate::mountain(degenerate, { 0, 0, 10, 10 }, {}).ok);

    TerrainComponent flatArea = terrain();
    CHECK_FALSE(TerrainGenerate::mountain(flatArea, { 0, 0, 0, 10 }, {}).ok);
    CHECK_FALSE(TerrainGenerate::mountain(flatArea, { 0, 0, 10, -1 }, {}).ok);

    // Legal requests that land nowhere: ok, nothing changed, nothing to rebuild.
    TerrainComponent off = terrain();
    const TerrainGenerate::Result r = TerrainGenerate::mountain(off, { 900, 900, 20, 20 }, {});
    CHECK(r.ok);
    CHECK(r.changed == 0);
    CHECK_FALSE(off.regionDirty);

    TerrainComponent zero = terrain();
    const TerrainGenerate::Result z = TerrainGenerate::mountain(zero, { 0, 0, 20, 20 }, smooth(0.0f, 5.0f));
    CHECK(z.ok);
    CHECK(z.changed == 0);
    CHECK_FALSE(zero.regionDirty);
}

TEST_CASE("TerrainGenerate::mountain over the terrain's edge still reaches maxHeight on it")
{
    TerrainComponent tc = terrain();
    // Centre on the last column: the eastern half of the area is off the terrain.
    const TerrainGenerate::Result r = TerrainGenerate::mountain(
        tc, { 64.0f, 0.0f, 20.0f, 20.0f }, smooth(10.0f, 8.0f));
    REQUIRE(r.ok);
    CHECK(r.changed > 0);
    CHECK(r.peakAdded == doctest::Approx(10.0f));
    CHECK(at(tc, 64, 0) == doctest::Approx(10.0f));
    CHECK(at(tc, 40, 0) == 0.0f);
}
