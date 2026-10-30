#include "doctest.h"
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/TerrainPaint.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/HorizonWorld.h>
#include <MaterialGraph/AutoLandscapeMaterial.h>

// ── Mixed texture groups (Thema 233) ────────────────────────────────────────
// TerrainComponent::groupMix turns weight layers 8..11 into the four groups' shares of a
// texel (by AutoLandscapeGroupId), outside the 255-sum of the paint layers. A terrain
// without it must behave EXACTLY as before; most of this file proves that, then
// exercises the group paths.

using G = HE::AutoLandscapeGroupId;

namespace
{
    TerrainComponent smallTerrain()
    {
        TerrainComponent tc;
        tc.sizeX = tc.sizeZ = 16.0f;
        tc.weightRes = 8;
        TerrainPaint::ensureWeightmap(tc);
        return tc;
    }

    const TerrainComponent* roundTrip(HorizonWorld& w2, const TerrainComponent& tc)
    {
        HorizonWorld world;
        Entity e = world.createEntity("Landscape");
        world.registry().emplace<TerrainComponent>(e, tc);
        SceneSerializer ser;
        std::vector<uint8_t> bytes;
        REQUIRE(ser.saveToMemory(world, bytes));
        REQUIRE(ser.loadFromMemory(w2, bytes));
        const TerrainComponent* l = nullptr;
        for (auto ent : w2.registry().view<TerrainComponent>())
            l = &w2.registry().get<TerrainComponent>(ent);
        REQUIRE(l != nullptr);
        return l;
    }
}

TEST_CASE("A terrain starts without a group mix and with no reserved layer")
{
    TerrainComponent tc;
    CHECK_FALSE(tc.groupMix);
    CHECK(TerrainPaint::reservedLayers(tc) == 0);
    for (int k = 0; k < kTerrainMaxLayers; ++k) CHECK_FALSE(TerrainPaint::isReservedLayer(tc, k));
}

TEST_CASE("Painting layers is byte-identical with the mix off, and layers 0..7 are unchanged with it on")
{
    TerrainComponent a = smallTerrain(), b = smallTerrain();
    REQUIRE(TerrainPaint::enableGroupMix(b, static_cast<int>(G::Forest)));
    for (int layer : { 1, 3, 5, 6, 7, 2 })
    {
        REQUIRE(TerrainPaint::paint(a, 1.0f, 2.0f, layer, 5.0f, 2.0f, 0.7f));
        REQUIRE(TerrainPaint::paint(b, 1.0f, 2.0f, layer, 5.0f, 2.0f, 0.7f));
    }
    CHECK(a.layerWeights == b.layerWeights);
    CHECK(a.layerWeights2 == b.layerWeights2);
    // Puddles (5) stays an ordinary paint layer; the share layers refuse a stroke.
    CHECK_FALSE(TerrainPaint::paint(b, 0.0f, 0.0f, 8, 4.0f, 0.0f, 1.0f));
    CHECK_FALSE(TerrainPaint::fillLayer(b, 11));
    TerrainPaint::fillLayer(b, 5);
    uint8_t w[kTerrainMaxLayers];
    REQUIRE(TerrainPaint::texelWeights(b, 4, 4, w));
    for (int k = 0; k < 8; ++k) CHECK(w[k] == (k == 5 ? 255 : 0));
    CHECK(w[static_cast<int>(G::Forest) + 8] == 255);   // fillLayer kept the shares
}

TEST_CASE("enableGroupMix starts every texel as the base group and folds old page-3 paint into layer 0")
{
    TerrainComponent tc = smallTerrain();
    REQUIRE(TerrainPaint::paint(tc, 0.0f, 0.0f, 9, 100.0f, 0.0f, 1.0f));   // a custom material's layer 9
    CHECK_FALSE(TerrainPaint::enableGroupMix(tc, 99) == false);              // base group is clamped
    CHECK(tc.groupMix);
    uint8_t w[kTerrainMaxLayers];
    REQUIRE(TerrainPaint::texelWeights(tc, 3, 3, w));
    CHECK(w[0] == 255);                                                      // folded into layer 0
    CHECK(w[9] == 0);
    CHECK(w[11] == 255);                                                     // base 99 -> clamped to group 3
    CHECK_FALSE(TerrainPaint::enableGroupMix(tc, 0));                        // already on
    CHECK(TerrainPaint::disableGroupMix(tc));
    CHECK_FALSE(tc.groupMix);
    REQUIRE(TerrainPaint::texelWeights(tc, 3, 3, w));
    for (int k = 8; k < 12; ++k) CHECK(w[k] == 0);
}

TEST_CASE("Group shares: four groups, normalised, painted per group")
{
    TerrainComponent tc = smallTerrain();
    CHECK_FALSE(TerrainPaint::paintGroup(tc, 0, 0, 1, 8.0f, 0.0f, 1.0f));   // no mix yet
    REQUIRE(TerrainPaint::enableGroupMix(tc, static_cast<int>(G::Mountains)));
    CHECK_FALSE(TerrainPaint::paintGroup(tc, 0, 0, 4, 8.0f, 0.0f, 1.0f));   // not a group

    const uint32_t c = tc.weightRes / 2;
    float s[4];
    REQUIRE(TerrainPaint::groupShares(tc, c, c, s));
    CHECK(s[0] == doctest::Approx(1.0f));
    for (int g = 1; g <= 3; ++g)
    {
        REQUIRE(TerrainPaint::paintGroup(tc, 0.0f, 0.0f, g, 100.0f, 0.0f, 1.0f));
        REQUIRE(TerrainPaint::groupShares(tc, c, c, s));
        for (int i = 0; i < 4; ++i) CHECK(s[i] == doctest::Approx(i == g ? 1.0f : 0.0f));
    }
    // A half stroke of another group leaves the shares summing to one, bytes to 255.
    REQUIRE(TerrainPaint::paintGroup(tc, 0.0f, 0.0f, 1, 100.0f, 0.0f, 0.5f));
    REQUIRE(TerrainPaint::groupShares(tc, c, c, s));
    CHECK(s[0] + s[1] + s[2] + s[3] == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(s[1] > 0.4f);
    CHECK(s[3] > 0.4f);
    uint8_t w[kTerrainMaxLayers];
    REQUIRE(TerrainPaint::texelWeights(tc, c, c, w));
    CHECK(w[8] + w[9] + w[10] + w[11] == 255);

    REQUIRE(TerrainPaint::fillGroup(tc, 2));
    REQUIRE(TerrainPaint::groupShares(tc, c, c, s));
    CHECK(s[2] == doctest::Approx(1.0f));
}

TEST_CASE("paintInGroup paints the texture and the group's share together")
{
    TerrainComponent tc = smallTerrain();
    TerrainComponent plain = smallTerrain();
    CHECK(TerrainPaint::paintInGroup(plain, 0.0f, 0.0f, 2, 1, 8.0f, 0.0f, 1.0f));   // mix off: plain paint
    uint8_t w[kTerrainMaxLayers];
    const uint32_t c = plain.weightRes / 2;
    REQUIRE(TerrainPaint::texelWeights(plain, c, c, w));
    CHECK(w[2] == 255);
    CHECK_FALSE(plain.groupMix);

    REQUIRE(TerrainPaint::enableGroupMix(tc, 0));
    REQUIRE(TerrainPaint::paintInGroup(tc, 0.0f, 0.0f, 2, 1, 8.0f, 0.0f, 1.0f));
    REQUIRE(TerrainPaint::texelWeights(tc, c, c, w));
    CHECK(w[2] == 255);
    CHECK(w[9] == 255);   // group 1's share
}

TEST_CASE("The group mix round-trips through the scene file; a terrain without it stays legacy")
{
    TerrainComponent tc = smallTerrain();
    TerrainPaint::paint(tc, 0, 0, 2, 4.0f, 0.0f, 1.0f);
    HorizonWorld wl;
    const TerrainComponent* legacy = roundTrip(wl, tc);
    CHECK_FALSE(legacy->groupMix);
    CHECK(legacy->layerWeights == tc.layerWeights);

    REQUIRE(TerrainPaint::enableGroupMix(tc, static_cast<int>(G::Forest)));
    REQUIRE(TerrainPaint::paintGroup(tc, 0.0f, 0.0f, 3, 6.0f, 0.0f, 1.0f));
    HorizonWorld w2;
    const TerrainComponent* l = roundTrip(w2, tc);
    CHECK(l->groupMix);
    CHECK(l->layerWeights3 == tc.layerWeights3);
    CHECK(TerrainPaint::isReservedLayer(*l, 10));
}

TEST_CASE("The weight texture is 3:1 while the mix is on, and 1:1 again when it is off")
{
    TerrainComponent tc = smallTerrain();
    uint32_t w = 0, h = 0;
    TerrainPaint::buildWeightTexture(tc, w, h);
    CHECK(w == tc.weightRes);
    REQUIRE(TerrainPaint::enableGroupMix(tc, static_cast<int>(G::Desert)));
    TerrainPaint::buildWeightTexture(tc, w, h);
    CHECK(w == 3 * tc.weightRes);
    TerrainPaint::disableGroupMix(tc);
    TerrainPaint::fillLayer(tc, 0);
    TerrainPaint::buildWeightTexture(tc, w, h);
    CHECK(w == tc.weightRes);
}
