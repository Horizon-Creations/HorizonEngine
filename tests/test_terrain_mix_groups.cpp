#include "doctest.h"
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/TerrainPaint.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/HorizonWorld.h>
#include <MaterialGraph/AutoLandscapeMaterial.h>

// ── Mixed texture groups (Thema 233) ────────────────────────────────────────
// TerrainComponent::mixGroup[3] names up to three further groups whose share of a
// texel is a channel of TerrainComponent::groupWeights, a grid of its own: the paint
// layers (Puddles included) are not touched by a mix. The terrain's own group
// (MaterialComponent) is the remainder. A terrain with no mix group must behave
// EXACTLY as it did before this field; most of this file proves that, then exercises
// the group paths.

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

TEST_CASE("A terrain starts with no mix group and no group grid")
{
    TerrainComponent tc;
    for (int g : tc.mixGroup) CHECK(g == -1);
    CHECK(TerrainPaint::mixSlotMask(tc) == 0);
    CHECK(tc.groupWeights.empty());
}

TEST_CASE("Painting layers is byte-identical with and without a mix group")
{
    TerrainComponent a = smallTerrain(), b = smallTerrain();
    REQUIRE(TerrainPaint::setMixGroup(b, 0, static_cast<int>(G::Forest)));
    REQUIRE(TerrainPaint::setMixGroup(b, 2, static_cast<int>(G::Desert)));
    for (int layer : { 1, 3, 5, 6, 7, 2 })
    {
        REQUIRE(TerrainPaint::paint(a, 1.0f, 2.0f, layer, 5.0f, 2.0f, 0.7f));
        REQUIRE(TerrainPaint::paint(b, 1.0f, 2.0f, layer, 5.0f, 2.0f, 0.7f));
    }
    CHECK(a.layerWeights == b.layerWeights);
    CHECK(a.layerWeights2 == b.layerWeights2);
    // Puddles (5) and 6/7 stay ordinary paint layers whatever the mix.
    TerrainPaint::fillLayer(b, 5);
    uint8_t w[kTerrainMaxLayers];
    REQUIRE(TerrainPaint::texelWeights(b, 4, 4, w));
    for (int k = 0; k < kTerrainMaxLayers; ++k) CHECK(w[k] == (k == 5 ? 255 : 0));
}

TEST_CASE("setMixGroup reserves, refuses duplicates and clears a slot")
{
    TerrainComponent tc = smallTerrain();
    CHECK(TerrainPaint::setMixGroup(tc, 0, static_cast<int>(G::Forest)));
    CHECK(tc.mixGroup[0] == static_cast<int>(G::Forest));
    CHECK(TerrainPaint::mixSlotMask(tc) == 1);
    CHECK_FALSE(TerrainPaint::setMixGroup(tc, 1, static_cast<int>(G::Forest)));   // same group twice
    CHECK(TerrainPaint::setMixGroup(tc, 1, static_cast<int>(G::Desert)));
    CHECK(TerrainPaint::setMixGroup(tc, 2, static_cast<int>(G::Grassland)));
    CHECK(TerrainPaint::mixSlotMask(tc) == 7);
    CHECK_FALSE(TerrainPaint::setMixGroup(tc, 3, 0));                              // no fourth slot
    CHECK(TerrainPaint::setMixGroup(tc, 0, -1));
    CHECK(TerrainPaint::mixSlotMask(tc) == 6);
}

TEST_CASE("groupShares reads the terrain's own group as the remainder")
{
    TerrainComponent tc = smallTerrain();
    float s[4];
    REQUIRE(TerrainPaint::groupShares(tc, 3, 4, s));       // unpainted: own group alone
    CHECK(s[0] == doctest::Approx(1.0f));
    CHECK(s[1] + s[2] + s[3] == 0.0f);

    REQUIRE(TerrainPaint::setMixGroup(tc, 0, static_cast<int>(G::Grassland)));
    REQUIRE(TerrainPaint::paintGroup(tc, 0.0f, 0.0f, 1, 100.0f, 0.0f, 1.0f));   // whole map
    REQUIRE(TerrainPaint::groupShares(tc, 3, 4, s));
    CHECK(s[0] == doctest::Approx(0.0f));
    CHECK(s[1] == doctest::Approx(1.0f));
}

TEST_CASE("Four groups on one terrain: three shares, normalised, painted per group")
{
    TerrainComponent tc = smallTerrain();
    REQUIRE(TerrainPaint::setMixGroup(tc, 0, static_cast<int>(G::Forest)));
    REQUIRE(TerrainPaint::setMixGroup(tc, 1, static_cast<int>(G::Grassland)));
    REQUIRE(TerrainPaint::setMixGroup(tc, 2, static_cast<int>(G::Desert)));
    CHECK_FALSE(TerrainPaint::paintGroup(tc, 0, 0, 4, 8.0f, 0.0f, 1.0f));

    const uint32_t c = tc.weightRes / 2;
    float s[4];
    for (int slot = 1; slot <= 3; ++slot)
    {
        REQUIRE(TerrainPaint::paintGroup(tc, 0.0f, 0.0f, slot, 100.0f, 0.0f, 1.0f));
        REQUIRE(TerrainPaint::groupShares(tc, c, c, s));
        for (int i = 0; i < 4; ++i) CHECK(s[i] == doctest::Approx(i == slot ? 1.0f : 0.0f));
    }
    // A half stroke of another group leaves the shares summing to one.
    REQUIRE(TerrainPaint::paintGroup(tc, 0.0f, 0.0f, 1, 100.0f, 0.0f, 0.5f));
    REQUIRE(TerrainPaint::groupShares(tc, c, c, s));
    CHECK(s[0] + s[1] + s[2] + s[3] == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(s[1] > 0.4f);
    CHECK(s[3] > 0.4f);

    REQUIRE(TerrainPaint::fillGroup(tc, 0));
    REQUIRE(TerrainPaint::groupShares(tc, c, c, s));
    CHECK(s[0] == doctest::Approx(1.0f));
    // Clearing a slot drops its share.
    REQUIRE(TerrainPaint::fillGroup(tc, 3));
    REQUIRE(TerrainPaint::setMixGroup(tc, 2, -1));
    REQUIRE(TerrainPaint::groupShares(tc, c, c, s));
    CHECK(s[0] == doctest::Approx(1.0f));
}

TEST_CASE("paintInGroup paints the texture and the active group's share together")
{
    TerrainComponent tc = smallTerrain();
    REQUIRE(TerrainPaint::setMixGroup(tc, 0, static_cast<int>(G::Forest)));
    TerrainComponent plain = smallTerrain();
    CHECK(TerrainPaint::paintInGroup(plain, 0.0f, 0.0f, 2, -1, 8.0f, 0.0f, 1.0f));
    CHECK(plain.groupWeights.empty());

    REQUIRE(TerrainPaint::paintInGroup(tc, 0.0f, 0.0f, 2, 1, 8.0f, 0.0f, 1.0f));
    const uint32_t c = tc.weightRes / 2;
    uint8_t w[kTerrainMaxLayers];
    REQUIRE(TerrainPaint::texelWeights(tc, c, c, w));
    CHECK(w[2] == 255);
    float s[4];
    REQUIRE(TerrainPaint::groupShares(tc, c, c, s));
    CHECK(s[1] == doctest::Approx(1.0f));
}

TEST_CASE("The weight texture carries the group page only when asked and used")
{
    TerrainComponent tc = smallTerrain();
    REQUIRE(TerrainPaint::setMixGroup(tc, 0, static_cast<int>(G::Forest)));
    uint32_t w = 0, h = 0;
    CHECK(TerrainPaint::buildWeightTexture(tc, w, h, true).size() == tc.layerWeights.size());
    REQUIRE(TerrainPaint::paintGroup(tc, 0.0f, 0.0f, 1, 100.0f, 0.0f, 1.0f));
    CHECK(TerrainPaint::usesGroupPage(tc));
    // A material that does not read it keeps the 1:1 / 2:1 layouts.
    CHECK(TerrainPaint::buildWeightTexture(tc, w, h, false).size() == tc.layerWeights.size());
    CHECK(w == 8);
    const std::vector<uint8_t> t = TerrainPaint::buildWeightTexture(tc, w, h, true);
    CHECK(w == 24);
    CHECK(h == 8);
    REQUIRE(t.size() == 24u * 8u * 4u);
    CHECK(t[0] == tc.layerWeights[0]);
    CHECK(t[2 * 8 * 4] == 255);   // row 0, third block, texel 0: slot 0's share
}

TEST_CASE("Three mix groups and their shares round-trip through the scene file")
{
    TerrainComponent tc = smallTerrain();
    REQUIRE(TerrainPaint::setMixGroup(tc, 0, static_cast<int>(G::Forest)));
    REQUIRE(TerrainPaint::setMixGroup(tc, 1, static_cast<int>(G::Grassland)));
    REQUIRE(TerrainPaint::setMixGroup(tc, 2, static_cast<int>(G::Desert)));
    REQUIRE(TerrainPaint::paintGroup(tc, 0.0f, 0.0f, 3, 6.0f, 0.0f, 1.0f));
    HorizonWorld w2;
    const TerrainComponent* l = roundTrip(w2, tc);
    CHECK(l->mixGroup[0] == static_cast<int>(G::Forest));
    CHECK(l->mixGroup[1] == static_cast<int>(G::Grassland));
    CHECK(l->mixGroup[2] == static_cast<int>(G::Desert));
    CHECK(l->groupWeights == tc.groupWeights);
}

TEST_CASE("A terrain saved with no mix group loads as one group (legacy)")
{
    TerrainComponent tc = smallTerrain();
    TerrainPaint::paint(tc, 0, 0, 2, 4.0f, 0.0f, 1.0f);
    HorizonWorld w2;
    const TerrainComponent* l = roundTrip(w2, tc);
    for (int g : l->mixGroup) CHECK(g == -1);
    CHECK(l->groupWeights.empty());
    CHECK(l->layerWeights == tc.layerWeights);
}
