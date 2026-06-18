#include "doctest.h"
#include <HorizonScene/HorizonScene.h>
#include <HorizonScene/FoliageSystem.h>
#include <HorizonScene/SceneSerializer.h>
#include <glm/glm.hpp>
#include <filesystem>

// ─── FoliageSystem instance generation ────────────────────────────────────────

TEST_CASE("FoliageSystem generates expected number of instances")
{
    HorizonWorld world;
    auto& reg = world.registry();
    auto e = world.createEntity("Terrain");

    TerrainComponent tc;
    tc.sizeX = 50.f; tc.sizeZ = 50.f; tc.seed = 0; // flat terrain
    reg.emplace<TerrainComponent>(e, tc);

    FoliageComponent fol;
    fol.meshAssetId = HE::UUID{1, 0};
    fol.density     = 0.1f;  // 0.1 per unit area → 50*50*0.1 = 250 instances
    fol.seed        = 42;
    fol.dirty       = true;
    reg.emplace<FoliageComponent>(e, fol);

    FoliageSystem::update(world);

    const auto& f = reg.get<FoliageComponent>(e);
    CHECK(!f.dirty);
    CHECK(f.cachedInstances.size() == 250u);
}

TEST_CASE("FoliageSystem clears dirty flag after update")
{
    HorizonWorld world;
    auto& reg = world.registry();
    auto e = world.createEntity("T");
    reg.emplace<TerrainComponent>(e, TerrainComponent{});

    FoliageComponent fol;
    fol.meshAssetId = HE::UUID{1,0};
    fol.density = 0.01f;
    fol.dirty = true;
    reg.emplace<FoliageComponent>(e, fol);

    FoliageSystem::update(world);
    CHECK(!reg.get<FoliageComponent>(e).dirty);

    // Second update with dirty=false should not regenerate
    const size_t before = reg.get<FoliageComponent>(e).cachedInstances.size();
    FoliageSystem::update(world); // dirty=false → no-op
    CHECK(reg.get<FoliageComponent>(e).cachedInstances.size() == before);
}

TEST_CASE("FoliageSystem instances are deterministic across calls")
{
    auto makeWorld = []() {
        HorizonWorld w;
        auto& reg = w.registry();
        auto e = w.createEntity("T");
        TerrainComponent tc; tc.sizeX = 20.f; tc.sizeZ = 20.f; tc.seed = 1;
        reg.emplace<TerrainComponent>(e, tc);
        FoliageComponent fol; fol.meshAssetId = HE::UUID{1,0}; fol.density = 0.5f; fol.seed = 7;
        reg.emplace<FoliageComponent>(e, fol);
        FoliageSystem::update(w);
        return w;
    };

    auto w1 = makeWorld();
    auto w2 = makeWorld();

    const auto& i1 = w1.registry().view<FoliageComponent>().begin()
                   == w1.registry().view<FoliageComponent>().end()
                   ? std::vector<glm::mat4>{}
                   : w1.registry().get<FoliageComponent>(*w1.registry().view<FoliageComponent>().begin()).cachedInstances;
    const auto& i2 = w2.registry().view<FoliageComponent>().begin()
                   == w2.registry().view<FoliageComponent>().end()
                   ? std::vector<glm::mat4>{}
                   : w2.registry().get<FoliageComponent>(*w2.registry().view<FoliageComponent>().begin()).cachedInstances;

    REQUIRE(i1.size() == i2.size());
    for (size_t k = 0; k < i1.size(); ++k)
        CHECK(i1[k] == i2[k]);
}

TEST_CASE("FoliageSystem skips entities without TerrainComponent")
{
    HorizonWorld world;
    auto& reg = world.registry();
    auto e = world.createEntity("NoTerrain");

    FoliageComponent fol;
    fol.meshAssetId = HE::UUID{1,0};
    fol.density = 1.f;
    fol.dirty = true;
    reg.emplace<FoliageComponent>(e, fol);

    FoliageSystem::update(world); // should not crash
    // Without TerrainComponent the view won't match this entity; dirty is still true
    CHECK(reg.get<FoliageComponent>(e).dirty);
}

TEST_CASE("FoliageSystem skips FoliageComponent with null mesh")
{
    HorizonWorld world;
    auto& reg = world.registry();
    auto e = world.createEntity("T");
    reg.emplace<TerrainComponent>(e, TerrainComponent{});

    FoliageComponent fol;
    // meshAssetId left as null UUID
    fol.density = 1.f;
    fol.dirty = true;
    reg.emplace<FoliageComponent>(e, fol);

    FoliageSystem::update(world);
    // dirty=false but cachedInstances should be empty (skipped due to null mesh)
    CHECK(!reg.get<FoliageComponent>(e).dirty);
    CHECK(reg.get<FoliageComponent>(e).cachedInstances.empty());
}

TEST_CASE("FoliageSystem places instances at terrain height")
{
    HorizonWorld world;
    auto& reg = world.registry();
    auto e = world.createEntity("T");

    TerrainComponent tc;
    tc.sizeX = 10.f; tc.sizeZ = 10.f;
    tc.heightScale = 5.f; tc.seed = 123;
    reg.emplace<TerrainComponent>(e, tc);

    FoliageComponent fol;
    fol.meshAssetId = HE::UUID{1,0};
    fol.density = 1.f;
    fol.dirty = true;
    reg.emplace<FoliageComponent>(e, fol);

    FoliageSystem::update(world);

    const auto& instances = reg.get<FoliageComponent>(e).cachedInstances;
    REQUIRE(!instances.empty());

    // All Y positions should be >= 0 (terrain height is >= 0 for seed!=0 fbm)
    for (const auto& m : instances)
    {
        const float y = m[3].y;
        CHECK(y >= 0.0f);
        CHECK(y <= 5.5f); // <= heightScale + small margin
    }
}

// ─── Serialisation ────────────────────────────────────────────────────────────

TEST_CASE("FoliageComponent serializes and deserializes correctly")
{
    HorizonWorld world;
    auto& reg = world.registry();
    auto e = world.createEntity("FolEnt");

    FoliageComponent fol;
    fol.meshAssetId     = HE::UUID{0xDEAD,0xBEEF};
    fol.materialAssetId = HE::UUID{0x1234,0x5678};
    fol.density         = 0.25f;
    fol.seed            = 99;
    fol.minScale        = 0.5f;
    fol.maxScale        = 2.0f;
    fol.drawDistance    = 120.f;
    reg.emplace<FoliageComponent>(e, fol);

    auto tmp = std::filesystem::temp_directory_path() / "he_test_foliage.hescene";
    SceneSerializer ser;
    REQUIRE(ser.save(world, tmp, HE::SerializeFormat::JSON));

    HorizonWorld world2;
    REQUIRE(ser.load(world2, tmp, HE::SerializeFormat::JSON));

    bool found = false;
    world2.registry().view<FoliageComponent>().each([&](auto, const FoliageComponent& f) {
        found = true;
        CHECK(f.meshAssetId     == HE::UUID{0xDEAD,0xBEEF});
        CHECK(f.materialAssetId == HE::UUID{0x1234,0x5678});
        CHECK(f.density         == doctest::Approx(0.25f));
        CHECK(f.seed            == 99);
        CHECK(f.minScale        == doctest::Approx(0.5f));
        CHECK(f.maxScale        == doctest::Approx(2.0f));
        CHECK(f.drawDistance    == doctest::Approx(120.f));
        CHECK(f.dirty           == true); // always dirty after load
    });
    CHECK(found);

    std::filesystem::remove(tmp);
}
