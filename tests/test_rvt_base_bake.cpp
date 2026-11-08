// RVT 2.0 step 6 (Thema 335): the export's baked base of an RVT landscape (HorizonScene/RvtBaseBake.h). The pictures themselves
// are the renderer's business (the HE_DUMP_RVTBASE witness paints a landscape's pages from them and from the graphs and compares);
// what is pinned here is the gate: which key a landscape has, what moves it and what does not, and that the pictures are found
// and turned into view materials for exactly that key.
#include "doctest.h"

#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/RvtBaseBake.h>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <MaterialGraph/AutoLandscapeMaterial.h>
#include <MaterialGraph/MaterialGraph.h>

#include <set>
#include <string>
#include <vector>

namespace
{
    using namespace HE::landscape;

    const HE::UUID kTerrainId{ 0x0102030405060708ull, 0x1112131415161718ull };

    struct Rig
    {
        HorizonWorld world;
        ContentManager cm{ "." };
        entt::entity land = entt::null;
        HE::UUID material;

        explicit Rig(bool rvt = true)
        {
            MaterialAsset ma;
            ma.type = HE::AssetType::Material;
            ma.name = rvt ? "TestRvtLandscape" : "TestPlainLandscape";
            if (rvt)
                ma.nodeGraphJson = HE::materialGraphToJson(
                    HE::buildAutoLandscapeGraph(HE::AutoLandscapeView::Lit, HE::AutoLandscapeGroupId::Mountains, false).graph);
            else
                ma.nodeGraphJson = HE::materialGraphToJson(
                    HE::buildAutoLandscapeGraph(HE::AutoLandscapeView::BaseColor, HE::AutoLandscapeGroupId::Mountains, false).graph);
            material = cm.registerMaterial(std::move(ma));

            auto& reg = world.registry();
            land = world.createEntity("Landscape");
            reg.emplace<TransformComponent>(land);
            TerrainComponent tc;
            tc.resolution = 65;
            tc.sizeX = tc.sizeZ = 128.0f;
            tc.seed = 3;
            reg.emplace<TerrainComponent>(land, tc);
            reg.emplace<MaterialComponent>(land, MaterialComponent{ material });
            world.setEntityId(land, kTerrainId);
        }
        uint64_t key() { return rvtBaseKey(cm, world, land); }
    };

    HlodChunkImages picture(uint32_t side)
    {
        HlodChunkImages img;
        img.side = side;
        const size_t n = static_cast<size_t>(side) * side * 4;
        img.albedo.assign(n, 200); img.normal.assign(n, 128); img.surface.assign(n, 90);
        return img;
    }
}

TEST_CASE("RVT base: ids are deterministic and distinct per kind, key and landscape, and live in the reach-root folder")
{
    std::set<std::pair<uint64_t, uint64_t>> ids;
    std::set<std::string> paths;
    for (uint64_t key : { 7ull, 8ull })
        for (RvtBaseAsset w : { RvtBaseAsset::Albedo, RvtBaseAsset::Normal, RvtBaseAsset::Surface })
        {
            const HE::UUID a = rvtBaseAssetId(kTerrainId, key, w);
            CHECK(a == rvtBaseAssetId(kTerrainId, key, w));
            ids.insert({ a.hi, a.lo });
            paths.insert(rvtBaseAssetPath(kTerrainId, key, w));
            CHECK(rvtBaseAssetPath(kTerrainId, key, w).rfind(kHlodAssetFolder, 0) == 0);
        }
    CHECK(ids.size() == 6);
    CHECK(paths.size() == 6);
    const HE::UUID other{ kTerrainId.hi + 1, kTerrainId.lo };
    CHECK_FALSE(rvtBaseAssetId(other, 7, RvtBaseAsset::Albedo) == rvtBaseAssetId(kTerrainId, 7, RvtBaseAsset::Albedo));
    // None of them is a far-landscape chunk asset.
    CHECK_FALSE(rvtBaseAssetId(kTerrainId, 0, RvtBaseAsset::Albedo) == hlodAssetId(kTerrainId, 0, 0, HlodAsset::Albedo));
}

TEST_CASE("RVT base: a landscape without an RVT Write / Read pair has no key")
{
    Rig plain(/*rvt=*/false);
    CHECK(plain.key() == 0);
    Rig rvt;
    CHECK(rvt.key() != 0);
}

TEST_CASE("RVT base: the key follows what the pages read, not what sits after the Read node")
{
    Rig rig;
    const uint64_t base = rig.key();
    REQUIRE(base != 0);
    CHECK(rig.key() == base);                                   // stable

    auto& reg = rig.world.registry();
    auto withOverride = [&](const char* name, float v) {
        reg.get<MaterialComponent>(rig.land).paramOverrides.clear();
        MaterialParamOverride ov;
        ov.name = name;
        ov.value[0] = ov.value[1] = ov.value[2] = ov.value[3] = v;
        reg.get<MaterialComponent>(rig.land).paramOverrides.push_back(ov);
        return rig.key();
    };
    // After the Read: snow, puddles, the look. A script moving them at run time must not lose the baked base.
    CHECK(withOverride(HE::kAutoLandscapeParamSnowHeight, 0.9f) == base);
    CHECK(withOverride(HE::kAutoLandscapeParamPuddleAmount, 0.9f) == base);
    CHECK(withOverride(HE::kAutoLandscapeParamSandLook, 0.9f) == base);
    // Before the Write: the layers. Another picture, so another key and the pictures of the old one are not found.
    CHECK(withOverride(HE::kAutoLandscapeParamRockSlope, 0.77f) != base);
    CHECK(withOverride(HE::kAutoLandscapeParamGroundTile, 0.77f) != base);
    reg.get<MaterialComponent>(rig.land).paramOverrides.clear();
    CHECK(rig.key() == base);
}

TEST_CASE("RVT base: the key follows the ground and the landscape's place")
{
    Rig rig;
    auto& reg = rig.world.registry();
    const uint64_t base = rig.key();

    TerrainComponent& tc = reg.get<TerrainComponent>(rig.land);
    tc.sculptHeights.assign(static_cast<size_t>(tc.resolution) * tc.resolution, 0.5f);
    const uint64_t sculpted = rig.key();
    CHECK(sculpted != base);
    tc.sculptHeights[100] = 0.75f;                              // one vertex
    CHECK(rig.key() != sculpted);
    tc.sculptHeights[100] = 0.5f;
    CHECK(rig.key() == sculpted);

    const uint64_t unpainted = rig.key();
    tc.layerWeights.assign(static_cast<size_t>(tc.weightRes) * tc.weightRes * 4, 0);
    const uint64_t painted = rig.key();
    CHECK(painted != unpainted);
    tc.layerWeights[12345] = 9;                                 // one stroke texel
    CHECK(rig.key() != painted);

    reg.get<TransformComponent>(rig.land).position.x += 1.0f;
    CHECK(rig.key() != painted);
}

TEST_CASE("RVT base: the pictures are packed as three single-level textures and found by key, only by key")
{
    Rig rig;
    const uint64_t key = rig.key();
    REQUIRE(key != 0);

    // Nothing in the pak: no baked views (a miss says nothing).
    HE::UUID views[3];
    CHECK_FALSE(rvtBaseViewMaterials(rig.cm, rig.world, rig.land, key, views));

    std::vector<HlodAssetBlob> blobs;
    std::string why;
    REQUIRE(buildRvtBaseAssets(picture(16), kTerrainId, key, blobs, &why));
    INFO(why);
    REQUIRE(blobs.size() == 3);
    for (const HlodAssetBlob& b : blobs) { CHECK_FALSE(b.bytes.empty()); CHECK(b.relPath.rfind(kHlodAssetFolder, 0) == 0); }
    REQUIRE(loadHlodAssets(rig.cm, blobs) == 3);

    for (RvtBaseAsset w : { RvtBaseAsset::Albedo, RvtBaseAsset::Normal, RvtBaseAsset::Surface })
    {
        const TextureAsset* t = rig.cm.getTexture(rvtBaseAssetId(kTerrainId, key, w));
        REQUIRE(t != nullptr);
        CHECK(t->width == 16); CHECK(t->height == 16);
        CHECK(t->mipLevels == 1);                               // the packer builds the chain and compresses (BC7 / ASTC)
        CHECK(t->srgb == (w == RvtBaseAsset::Albedo));
        CHECK(t->data[0] == (w == RvtBaseAsset::Albedo ? 200 : w == RvtBaseAsset::Normal ? 128 : 90));
    }

    // Found for the key they were made for ...
    REQUIRE(rvtBaseViewMaterials(rig.cm, rig.world, rig.land, key, views));
    for (int i = 0; i < 3; ++i)
    {
        const MaterialAsset* m = rig.cm.getMaterial(views[i]);
        REQUIRE(m != nullptr);
        CHECK_FALSE(m->customShaderFragGlsl.empty());
        // ... one texture each, the picture of its layer: the page painter never builds the material's own page graphs.
        REQUIRE(m->graphTexturePaths.size() == 1);
        CHECK(m->graphTexturePaths[0] == rvtBaseAssetPath(kTerrainId, key, static_cast<RvtBaseAsset>(i)));
    }
    CHECK_FALSE(views[0] == views[1]);
    CHECK_FALSE(views[1] == views[2]);

    // ... and not for another one: a parameter before the Write node moved, the pictures are of the old material.
    {
        MaterialParamOverride ov;
        ov.name = HE::kAutoLandscapeParamRockSlope;
        ov.value[0] = ov.value[1] = ov.value[2] = ov.value[3] = 0.77f;
        rig.world.registry().get<MaterialComponent>(rig.land).paramOverrides.push_back(ov);
    }
    const uint64_t moved = rig.key();
    REQUIRE(moved != key);
    HE::UUID v2[3];
    CHECK_FALSE(rvtBaseViewMaterials(rig.cm, rig.world, rig.land, moved, v2));
    // ... and not for another landscape.
    rig.world.setEntityId(rig.land, HE::UUID{ 9, 9 });
    HE::UUID v3[3];
    CHECK_FALSE(rvtBaseViewMaterials(rig.cm, rig.world, rig.land, key, v3));
}

TEST_CASE("RVT base: buildRvtBaseAssets refuses pictures that do not match their side")
{
    HlodChunkImages bad = picture(8);
    bad.normal.resize(10);
    std::vector<HlodAssetBlob> blobs;
    std::string why;
    CHECK_FALSE(buildRvtBaseAssets(bad, kTerrainId, 5, blobs, &why));
    CHECK_FALSE(why.empty());
    CHECK(blobs.empty());
}

TEST_CASE("RVT base: the picture side is the live page resolution, capped, and halved until it fits the budget")
{
    RvtBaseOptions o;
    o.budgetBytes = 0;                                          // no limit
    bool down = true;
    CHECK(rvtBaseSide(128.0f, o, 0, &down) == 512);             // 128 m at 0.25 m a texel
    CHECK_FALSE(down);
    CHECK(rvtBaseSide(512.0f, o, 0, &down) == 2048);
    CHECK_FALSE(down);
    CHECK(rvtBaseSide(4096.0f, o, 0, &down) == 4096);           // 16384 wanted, capped at maxTexels
    CHECK(down);
    CHECK(rvtBaseSide(300.0f, o, 0) == 2048);                   // 1200 wanted -> the next power of two

    o.budgetBytes = 64ull << 20;                                // 4096 px costs 64 MiB cooked: exactly fits
    CHECK(rvtBaseSide(4096.0f, o, 64ull << 20) == 4096);
    CHECK(rvtBaseSide(4096.0f, o, 32ull << 20, &down) == 2048);  // 2048 px = 16 MiB; 4096 did not fit
    CHECK(down);
    CHECK(rvtBaseSide(4096.0f, o, 1, &down) == static_cast<uint32_t>(o.minTexels));   // never below the floor
    CHECK(rvtBaseCookedBytes(1024) == 1024ull * 1024ull * 4ull);
}
