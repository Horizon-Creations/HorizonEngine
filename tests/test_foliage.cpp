#include "doctest.h"
#include "TestFsUtil.h"
#include <HorizonScene/HorizonScene.h>
#include <HorizonScene/FoliageSystem.h>
#include <HorizonScene/FoliagePaint.h>
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
    // Fills a world in place: HorizonWorld is neither copyable nor movable, so
    // this cannot hand one back by value.
    auto scatterInto = [](HorizonWorld& w) -> std::vector<glm::mat4> {
        auto& reg = w.registry();
        auto e = w.createEntity("T");
        TerrainComponent tc; tc.sizeX = 20.f; tc.sizeZ = 20.f; tc.seed = 1;
        reg.emplace<TerrainComponent>(e, tc);
        FoliageComponent fol; fol.meshAssetId = HE::UUID{1,0}; fol.density = 0.5f; fol.seed = 7;
        reg.emplace<FoliageComponent>(e, fol);
        FoliageSystem::update(w);
        return reg.get<FoliageComponent>(e).cachedInstances;
    };

    HorizonWorld w1, w2;
    const std::vector<glm::mat4> i1 = scatterInto(w1);
    const std::vector<glm::mat4> i2 = scatterInto(w2);

    REQUIRE(!i1.empty());
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

    he_test::removeQuiet(tmp);
}

// ─── Painted density mask ─────────────────────────────────────────────────────
// The mask is the difference between "foliage everywhere" and a landscape with
// a yard, a road and a lake in it. Each check below is one thing the brush
// promises: an erased circle stays EMPTY (not merely thin), painting to full
// keeps every instance exactly where the uniform layer put it, a thinner target
// thins in proportion, and the mask survives a save.

namespace
{
    // A flat 100×100 terrain with a full-density layer of the given seed.
    Entity makeMaskedLayer(HorizonWorld& world, float density = 0.2f, int seed = 3)
    {
        auto& reg = world.registry();
        auto e = world.createEntity("Terrain");
        TerrainComponent tc;
        tc.sizeX = 100.f; tc.sizeZ = 100.f; tc.seed = 0;
        reg.emplace<TerrainComponent>(e, tc);
        FoliageComponent fol;
        fol.meshAssetId = HE::UUID{1, 0};
        fol.density     = density;
        fol.seed        = seed;
        reg.emplace<FoliageComponent>(e, fol);
        return e;
    }
}

TEST_CASE("FoliagePaint allocates a full mask that changes nothing")
{
    HorizonWorld world;
    auto& reg = world.registry();
    auto e = makeMaskedLayer(world);
    FoliageSystem::update(world);
    const std::vector<glm::mat4> uniform = reg.get<FoliageComponent>(e).cachedInstances;
    REQUIRE(uniform.size() == 2000u);   // 100*100*0.2

    auto& fol = reg.get<FoliageComponent>(e);
    FoliagePaint::ensureMask(fol);
    CHECK(fol.densityMask.size() == static_cast<size_t>(fol.maskRes) * fol.maskRes);
    CHECK(fol.dirty);
    for (uint8_t b : fol.densityMask) { if (b != 255) { CHECK(b == 255); break; } }
    CHECK(FoliagePaint::coverage(fol) == doctest::Approx(1.0f));

    // A full mask is the uniform layer: same count, same poses, same order.
    FoliageSystem::update(world);
    const auto& masked = reg.get<FoliageComponent>(e).cachedInstances;
    REQUIRE(masked.size() == uniform.size());
    for (size_t i = 0; i < masked.size(); ++i) CHECK(masked[i] == uniform[i]);

    // ensureMask on a correctly sized mask leaves it alone.
    fol.densityMask[7] = 3;
    FoliagePaint::ensureMask(fol);
    CHECK(fol.densityMask[7] == 3);
}

TEST_CASE("FoliagePaint erase leaves the circle empty and the rest untouched")
{
    HorizonWorld world;
    auto& reg = world.registry();
    auto e = makeMaskedLayer(world);
    FoliageSystem::update(world);
    const std::vector<glm::mat4> uniform = reg.get<FoliageComponent>(e).cachedInstances;

    auto& fol = reg.get<FoliageComponent>(e);
    const auto& tc = reg.get<TerrainComponent>(e);
    // A hard-edged full-strength erase at (20, -10), radius 15.
    REQUIRE(FoliagePaint::paint(fol, tc, 20.f, -10.f, /*radius=*/15.f, /*falloff=*/0.f,
                                /*strength=*/1.f, /*target=*/0.f));
    CHECK(fol.dirty);
    CHECK(FoliagePaint::sample(fol, tc, 20.f, -10.f) == doctest::Approx(0.0f));
    CHECK(FoliagePaint::sample(fol, tc, -40.f, 40.f) == doctest::Approx(1.0f));
    CHECK(FoliagePaint::coverage(fol) < 1.0f);

    FoliageSystem::update(world);
    const auto& masked = reg.get<FoliageComponent>(e).cachedInstances;
    CHECK(masked.size() < uniform.size());
    CHECK(!masked.empty());

    // Nothing inside the erased circle (a texel margin: the mask is 128 texels
    // over 100 m, so its edge is quantised to ~0.8 m).
    size_t insideUniform = 0;
    for (const auto& m : uniform)
    {
        const float dx = m[3].x - 20.f, dz = m[3].z + 10.f;
        if (dx * dx + dz * dz < 14.f * 14.f) ++insideUniform;
    }
    REQUIRE(insideUniform > 0);   // the uniform layer DID have instances there
    for (const auto& m : masked)
    {
        const float dx = m[3].x - 20.f, dz = m[3].z + 10.f;
        CHECK(dx * dx + dz * dz >= 14.f * 14.f);
    }

    // Every survivor is one of the uniform instances, pose and all: erasing
    // removes, it never reshuffles what stays.
    for (const auto& m : masked)
    {
        bool found = false;
        for (const auto& u : uniform) if (u == m) { found = true; break; }
        CHECK(found);
    }
}

TEST_CASE("FoliagePaint half-density target thins the scatter in proportion")
{
    HorizonWorld world;
    auto& reg = world.registry();
    auto e = makeMaskedLayer(world, /*density=*/0.5f);
    auto& fol = reg.get<FoliageComponent>(e);
    FoliagePaint::fillMask(fol, 0.5f);
    CHECK(FoliagePaint::coverage(fol) == doctest::Approx(0.5f).epsilon(0.01));

    FoliageSystem::update(world);
    const size_t n = reg.get<FoliageComponent>(e).cachedInstances.size();
    // 5000 candidates kept with probability 0.5 → 2500 ± a generous band.
    CHECK(n > 2200u);
    CHECK(n < 2800u);

    // Fill at 0 → nothing at all, dirty again.
    FoliagePaint::fillMask(fol, 0.0f);
    CHECK(fol.dirty);
    FoliageSystem::update(world);
    CHECK(reg.get<FoliageComponent>(e).cachedInstances.empty());

    // Clearing the mask is back to the uniform layer.
    FoliagePaint::clearMask(fol);
    CHECK(fol.densityMask.empty());
    CHECK(fol.dirty);
    FoliageSystem::update(world);
    CHECK(reg.get<FoliageComponent>(e).cachedInstances.size() == 5000u);
}

TEST_CASE("FoliagePaint falloff fades and repeated strokes converge to the target")
{
    HorizonWorld world;
    auto& reg = world.registry();
    auto e = makeMaskedLayer(world);
    auto& fol = reg.get<FoliageComponent>(e);
    const auto& tc = reg.get<TerrainComponent>(e);

    REQUIRE(FoliagePaint::paint(fol, tc, 0.f, 0.f, /*radius=*/5.f, /*falloff=*/20.f,
                                /*strength=*/1.f, /*target=*/0.f));
    const float centre = FoliagePaint::sample(fol, tc, 0.f, 0.f);
    const float mid    = FoliagePaint::sample(fol, tc, 15.f, 0.f);
    const float edge   = FoliagePaint::sample(fol, tc, 30.f, 0.f);
    CHECK(centre == doctest::Approx(0.0f));
    CHECK(mid > centre);
    CHECK(mid < edge);
    CHECK(edge == doctest::Approx(1.0f));

    // Soft strokes: 0.3 per call toward 0 reaches EXACTLY 0 rather than
    // parking at 1/255 (the residue snap), so an erased yard is truly empty.
    FoliagePaint::fillMask(fol, 1.0f);
    for (int i = 0; i < 40; ++i)
        FoliagePaint::paint(fol, tc, 0.f, 0.f, 5.f, 0.f, 0.3f, 0.f);
    CHECK(FoliagePaint::sample(fol, tc, 0.f, 0.f) == doctest::Approx(0.0f));

    // Out of range or degenerate → false, nothing touched.
    FoliagePaint::fillMask(fol, 1.0f);
    fol.dirty = false;
    CHECK_FALSE(FoliagePaint::paint(fol, tc, 500.f, 500.f, 5.f, 0.f, 1.f, 0.f));
    CHECK_FALSE(FoliagePaint::paint(fol, tc, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f));
    CHECK_FALSE(FoliagePaint::paint(fol, tc, 0.f, 0.f, 5.f, 0.f, 0.f, 0.f));
    CHECK_FALSE(fol.dirty);
}

TEST_CASE("FoliageComponent density mask survives a save and drops a bad blob")
{
    HorizonWorld world;
    auto& reg = world.registry();
    auto e = makeMaskedLayer(world);
    auto& fol = reg.get<FoliageComponent>(e);
    const auto& tc = reg.get<TerrainComponent>(e);
    fol.maskRes = 64;
    REQUIRE(FoliagePaint::paint(fol, tc, 10.f, 10.f, 10.f, 0.f, 1.f, 0.f));
    const std::vector<uint8_t> painted = fol.densityMask;

    auto tmp = std::filesystem::temp_directory_path() / "he_test_foliage_mask.hescene";
    SceneSerializer ser;
    REQUIRE(ser.save(world, tmp, HE::SerializeFormat::JSON));

    HorizonWorld world2;
    REQUIRE(ser.load(world2, tmp, HE::SerializeFormat::JSON));
    bool found = false;
    world2.registry().view<FoliageComponent>().each([&](auto, const FoliageComponent& f) {
        found = true;
        CHECK(f.maskRes == 64u);
        CHECK(f.densityMask == painted);
        CHECK(f.dirty);
    });
    CHECK(found);

    // A mask whose size disagrees with maskRes is dropped on load, not carried.
    fol.maskRes = 32;   // densityMask still 64² bytes
    REQUIRE(ser.save(world, tmp, HE::SerializeFormat::JSON));
    HorizonWorld world3;
    REQUIRE(ser.load(world3, tmp, HE::SerializeFormat::JSON));
    world3.registry().view<FoliageComponent>().each([&](auto, const FoliageComponent& f) {
        CHECK(f.maskRes == 32u);
        CHECK(f.densityMask.empty());
    });

    he_test::removeQuiet(tmp);
}
