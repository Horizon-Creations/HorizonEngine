#include "doctest.h"
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/TerrainPaint.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/HorizonWorld.h>
#include <MaterialGraph/AutoLandscapeMaterial.h>

// ── Mixed texture groups (Thema 233, step 7) ────────────────────────────────
// TerrainComponent::mixGroup[3] names up to three further groups whose share of a texel
// lives in weight layer 6/7/5; the terrain's own group (MaterialComponent) is the
// remainder. The whole point of reserving layers rather than adding a grid is that
// a terrain with no mix group must behave EXACTLY as it did before this field:
// most of this file proves that, then exercises the new paths once a slot is set.

TEST_CASE("TerrainComponent starts with no mix group, and reservedLayers() is 0")
{
    TerrainComponent tc;
    CHECK(tc.mixGroup[0] == -1);
    CHECK(tc.mixGroup[1] == -1);
    CHECK(TerrainPaint::reservedLayers(tc) == 0);
    CHECK_FALSE(TerrainPaint::isReservedLayer(tc, 6));
    CHECK_FALSE(TerrainPaint::isReservedLayer(tc, 7));
}

TEST_CASE("paint() and fillLayer() are unchanged when no mix group is set")
{
    // Same stroke as the plain TerrainPaint tests in test_terrain.cpp, byte for
    // byte: paintInGroup/paintGroup only ever touch layerWeights2[*+2]/[*+3], and
    // paint()/fillLayer() only skip a channel when isReservedLayer() is true —
    // with mixGroup untouched that is never, so the old code path runs unchanged.
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 64.0f;
    tc.weightRes = 32;
    TerrainPaint::ensureWeightmap(tc);
    REQUIRE(TerrainPaint::paint(tc, 0.0f, 0.0f, 1, 8.0f, 0.0f, 0.5f));
    const uint32_t wr = tc.weightRes;
    const int c = static_cast<int>(wr) / 2;
    const uint8_t* mid = &tc.layerWeights[(static_cast<size_t>(c) * wr + c) * 4];
    CHECK(mid[1] > 100);
    CHECK(mid[1] < 160);
    for (int i = 0; i < 12; ++i) TerrainPaint::paint(tc, 0.0f, 0.0f, 1, 8.0f, 0.0f, 0.5f);
    CHECK(mid[1] == 255);

    TerrainComponent tc2;
    tc2.sizeX = tc2.sizeZ = 40.0f; tc2.weightRes = 32;
    REQUIRE(TerrainPaint::fillLayer(tc2, 5));
    CHECK(TerrainPaint::usesSecondPage(tc2));
    uint8_t w[kTerrainMaxLayers];
    REQUIRE(TerrainPaint::texelWeights(tc2, 3, 4, w));
    for (int k = 0; k < kTerrainMaxLayers; ++k) CHECK(w[k] == (k == 5 ? 255 : 0));
}

TEST_CASE("setMixGroup reserves a layer and refuses painting it as a texture")
{
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 32.0f;
    tc.weightRes = 16;
    TerrainPaint::ensureWeightmap(tc);

    CHECK(TerrainPaint::setMixGroup(tc, 0, static_cast<int>(HE::AutoLandscapeGroupId::Forest)));
    CHECK(tc.mixGroup[0] == static_cast<int>(HE::AutoLandscapeGroupId::Forest));
    CHECK(TerrainPaint::reservedLayers(tc) == 1);
    CHECK(TerrainPaint::isReservedLayer(tc, 6));
    CHECK_FALSE(TerrainPaint::isReservedLayer(tc, 7));

    // A reserved layer is a group share, not a paint layer: paint()/fillLayer()
    // on it are refused outright rather than silently doing nothing useful.
    CHECK_FALSE(TerrainPaint::paint(tc, 0.0f, 0.0f, 6, 8.0f, 0.0f, 1.0f));
    CHECK_FALSE(TerrainPaint::fillLayer(tc, 6));
    // Layer 7 (no group there) is still an ordinary paint layer.
    CHECK(TerrainPaint::paint(tc, 0.0f, 0.0f, 7, 8.0f, 0.0f, 1.0f));

    // Slot 2 cannot take the same group as slot 1.
    CHECK_FALSE(TerrainPaint::setMixGroup(tc, 1, static_cast<int>(HE::AutoLandscapeGroupId::Forest)));
    CHECK(TerrainPaint::setMixGroup(tc, 1, static_cast<int>(HE::AutoLandscapeGroupId::Desert)));
    CHECK(TerrainPaint::reservedLayers(tc) == 3);

    // Clearing a slot drops its share and frees the layer for plain paint again.
    CHECK(TerrainPaint::setMixGroup(tc, 0, -1));
    CHECK(TerrainPaint::reservedLayers(tc) == 2);
    CHECK(TerrainPaint::paint(tc, 0.0f, 0.0f, 6, 8.0f, 0.0f, 1.0f));
}

TEST_CASE("paint() normalises layers 0..5 only once a layer is reserved")
{
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 32.0f;
    tc.weightRes = 8;
    TerrainPaint::ensureWeightmap(tc);
    REQUIRE(TerrainPaint::setMixGroup(tc, 0, static_cast<int>(HE::AutoLandscapeGroupId::Forest)));
    // Give the centre texel a share first, THEN paint a texture layer there —
    // the share must survive untouched and the paint layers must still sum to
    // 255 among themselves (not 255 - share, which would be a silent fade).
    const uint32_t wr = tc.weightRes;
    const int c = static_cast<int>(wr) / 2;
    REQUIRE(TerrainPaint::paintGroup(tc, 0.0f, 0.0f, 1, 8.0f, 0.0f, 1.0f));
    const size_t at = (static_cast<size_t>(c) * wr + c) * 4;
    const uint8_t shareBefore = tc.layerWeights2[at + 2];
    CHECK(shareBefore == 255);

    REQUIRE(TerrainPaint::paint(tc, 0.0f, 0.0f, 2, 8.0f, 0.0f, 1.0f));
    CHECK(tc.layerWeights2[at + 2] == shareBefore);    // the share is untouched
    const int sum05 = tc.layerWeights[at] + tc.layerWeights[at + 1]
                    + tc.layerWeights[at + 2] + tc.layerWeights[at + 3]
                    + tc.layerWeights2[at]   + tc.layerWeights2[at + 1];
    CHECK(sum05 == 255);                               // layers 0..5 alone sum to 255
}

TEST_CASE("fillLayer keeps the group shares when a reserved layer is set")
{
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 16.0f;
    tc.weightRes = 8;
    TerrainPaint::ensureWeightmap(tc);
    REQUIRE(TerrainPaint::setMixGroup(tc, 0, static_cast<int>(HE::AutoLandscapeGroupId::Forest)));
    REQUIRE(TerrainPaint::fillGroup(tc, 1));            // the whole landscape shares slot 1
    uint8_t w[kTerrainMaxLayers];
    REQUIRE(TerrainPaint::texelWeights(tc, 3, 4, w));
    CHECK(w[6] == 255);

    REQUIRE(TerrainPaint::fillLayer(tc, 2));            // start the paint layers over
    REQUIRE(TerrainPaint::texelWeights(tc, 3, 4, w));
    CHECK(w[2] == 255);
    CHECK(w[6] == 255);                                 // the share rode through the fill
}

TEST_CASE("groupShares reads the terrain's own group as the remainder")
{
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 16.0f;
    tc.weightRes = 8;
    TerrainPaint::ensureWeightmap(tc);
    float shares[4];
    // Unpainted, no mix group: entirely the own group.
    REQUIRE(TerrainPaint::groupShares(tc, 3, 4, shares));
    CHECK(shares[0] == doctest::Approx(1.0f));
    CHECK(shares[1] == 0.0f);
    CHECK(shares[2] == 0.0f);
    CHECK(shares[3] == 0.0f);

    REQUIRE(TerrainPaint::setMixGroup(tc, 0, static_cast<int>(HE::AutoLandscapeGroupId::Grassland)));
    REQUIRE(TerrainPaint::paintGroup(tc, 0.0f, 0.0f, 1, 100.0f, 0.0f, 1.0f));  // whole map
    REQUIRE(TerrainPaint::groupShares(tc, 3, 4, shares));
    CHECK(shares[0] == doctest::Approx(0.0f));
    CHECK(shares[1] == doctest::Approx(1.0f));
    CHECK(shares[2] == 0.0f);
}

TEST_CASE("TerrainComponent::mixGroup round-trips through the scene file")
{
    HorizonWorld world;
    Entity e = world.createEntity("Landscape");
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 16.0f;
    tc.weightRes = 8;
    TerrainPaint::ensureWeightmap(tc);
    REQUIRE(TerrainPaint::setMixGroup(tc, 0, static_cast<int>(HE::AutoLandscapeGroupId::Forest)));
    REQUIRE(TerrainPaint::setMixGroup(tc, 1, static_cast<int>(HE::AutoLandscapeGroupId::Desert)));
    world.registry().emplace<TerrainComponent>(e, tc);

    SceneSerializer ser;
    std::vector<uint8_t> bytes;
    REQUIRE(ser.saveToMemory(world, bytes));
    HorizonWorld w2;
    REQUIRE(ser.loadFromMemory(w2, bytes));
    const TerrainComponent* l = nullptr;
    for (auto ent : w2.registry().view<TerrainComponent>())
        l = &w2.registry().get<TerrainComponent>(ent);
    REQUIRE(l != nullptr);
    CHECK(l->mixGroup[0] == static_cast<int>(HE::AutoLandscapeGroupId::Forest));
    CHECK(l->mixGroup[1] == static_cast<int>(HE::AutoLandscapeGroupId::Desert));
}

TEST_CASE("A terrain saved with no mix group loads as one group (legacy)")
{
    // Every scene saved before this field, and every terrain that never touches
    // it, writes no "mixGroups" key at all (SceneSerializer only writes it when
    // one slot is >= 0) — the read side must default both slots to -1 rather
    // than leave them at whatever the freshly constructed component already had,
    // which happens to be the same default but must not be relied on silently.
    HorizonWorld world;
    Entity e = world.createEntity("Landscape");
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 16.0f;
    world.registry().emplace<TerrainComponent>(e, tc);

    SceneSerializer ser;
    std::vector<uint8_t> bytes;
    REQUIRE(ser.saveToMemory(world, bytes));
    HorizonWorld w2;
    REQUIRE(ser.loadFromMemory(w2, bytes));
    const TerrainComponent* l = nullptr;
    for (auto ent : w2.registry().view<TerrainComponent>())
        l = &w2.registry().get<TerrainComponent>(ent);
    REQUIRE(l != nullptr);
    CHECK(l->mixGroup[0] == -1);
    CHECK(l->mixGroup[1] == -1);
    CHECK(TerrainPaint::reservedLayers(*l) == 0);
}

TEST_CASE("paintInGroup paints the texture and the active group's share together")
{
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 16.0f;
    tc.weightRes = 8;
    TerrainPaint::ensureWeightmap(tc);
    REQUIRE(TerrainPaint::setMixGroup(tc, 0, static_cast<int>(HE::AutoLandscapeGroupId::Forest)));

    // Without a mix group, paintInGroup with slot < 0 is a plain paint() — the
    // call Setup's brush falls back to on a terrain that never set one up.
    TerrainComponent plain;
    plain.sizeX = plain.sizeZ = 16.0f; plain.weightRes = 8;
    TerrainPaint::ensureWeightmap(plain);
    CHECK(TerrainPaint::paintInGroup(plain, 0.0f, 0.0f, 2, -1, 8.0f, 0.0f, 1.0f));
    CHECK_FALSE(TerrainPaint::reservedLayers(plain));

    // With a mix group and slot 1 active, a stroke moves the texture layer AND
    // the group share at once.
    REQUIRE(TerrainPaint::paintInGroup(tc, 0.0f, 0.0f, 2, 1, 8.0f, 0.0f, 1.0f));
    uint8_t w[kTerrainMaxLayers];
    const uint32_t wr = tc.weightRes;
    const int c = static_cast<int>(wr) / 2;
    REQUIRE(TerrainPaint::texelWeights(tc, c, c, w));
    CHECK(w[2] == 255);   // the role texture, as plain paint() would leave it
    CHECK(w[6] == 255);   // slot 1's share, as paintGroup() would leave it
}

TEST_CASE("Four groups on one terrain: three shares, normalised, painted per group")
{
    using G = HE::AutoLandscapeGroupId;
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 16.0f;
    tc.weightRes = 8;
    TerrainPaint::ensureWeightmap(tc);
    REQUIRE(TerrainPaint::setMixGroup(tc, 0, static_cast<int>(G::Forest)));
    REQUIRE(TerrainPaint::setMixGroup(tc, 1, static_cast<int>(G::Grassland)));
    REQUIRE(TerrainPaint::setMixGroup(tc, 2, static_cast<int>(G::Desert)));
    CHECK(TerrainPaint::reservedLayers(tc) == 7);
    CHECK(TerrainPaint::shareLayer(2) == 5);
    CHECK(TerrainPaint::isReservedLayer(tc, 5));
    CHECK_FALSE(TerrainPaint::paint(tc, 0.0f, 0.0f, 5, 8.0f, 0.0f, 1.0f));   // Puddles became a share
    CHECK(TerrainPaint::paint(tc, 0.0f, 0.0f, 3, 8.0f, 0.0f, 1.0f));

    // A group may not be mixed in twice.
    CHECK_FALSE(TerrainPaint::setMixGroup(tc, 2, static_cast<int>(G::Forest)));

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

    // Fill, then clear the third slot: its share goes, the layer is paintable again.
    REQUIRE(TerrainPaint::fillGroup(tc, 0));
    REQUIRE(TerrainPaint::groupShares(tc, c, c, s));
    CHECK(s[0] == doctest::Approx(1.0f));
    REQUIRE(TerrainPaint::setMixGroup(tc, 2, -1));
    CHECK(TerrainPaint::paint(tc, 0.0f, 0.0f, 5, 8.0f, 0.0f, 1.0f));
}

TEST_CASE("Three mix groups round-trip through the scene file")
{
    using G = HE::AutoLandscapeGroupId;
    HorizonWorld world;
    Entity e = world.createEntity("Landscape");
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 16.0f;
    tc.weightRes = 8;
    TerrainPaint::ensureWeightmap(tc);
    REQUIRE(TerrainPaint::setMixGroup(tc, 0, static_cast<int>(G::Forest)));
    REQUIRE(TerrainPaint::setMixGroup(tc, 1, static_cast<int>(G::Grassland)));
    REQUIRE(TerrainPaint::setMixGroup(tc, 2, static_cast<int>(G::Desert)));
    world.registry().emplace<TerrainComponent>(e, tc);

    SceneSerializer ser;
    std::vector<uint8_t> bytes;
    REQUIRE(ser.saveToMemory(world, bytes));
    HorizonWorld w2;
    REQUIRE(ser.loadFromMemory(w2, bytes));
    const TerrainComponent* l = nullptr;
    for (auto ent : w2.registry().view<TerrainComponent>())
        l = &w2.registry().get<TerrainComponent>(ent);
    REQUIRE(l != nullptr);
    CHECK(l->mixGroup[0] == static_cast<int>(G::Forest));
    CHECK(l->mixGroup[1] == static_cast<int>(G::Grassland));
    CHECK(l->mixGroup[2] == static_cast<int>(G::Desert));
}
