// The baked far-landscape assets of a chunk (HorizonScene/LandscapeHlod.h): their ids, the engine assets built from
// three pictures, and the terrain system picking the material up for the far LOD levels. The pictures themselves are
// the renderer's business (the HE_DUMP_HLODFAR witness compares the frame with and without them).
#include "doctest.h"

#include <HorizonScene/Components/LODComponent.h>
#include <HorizonScene/Components/TerrainChunkComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LandscapeHlod.h>
#include <HorizonScene/TerrainSystem.h>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/TextureArrayBuild.h>

#include <set>
#include <string>
#include <vector>

namespace
{
    HE::landscape::HlodChunkImages syntheticImages(uint32_t side)
    {
        HE::landscape::HlodChunkImages img;
        img.side = side;
        const size_t n = static_cast<size_t>(side) * side * 4;
        img.albedo.assign(n, 200);
        img.normal.assign(n, 128);
        img.surface.assign(n, 90);
        return img;
    }

    const HE::UUID kTerrainId{ 0x1111222233334444ull, 0x5555666677778888ull };
}

TEST_CASE("Baked far-landscape ids are deterministic and distinct per chunk and asset")
{
    using HE::landscape::HlodAsset;
    std::set<std::pair<uint64_t, uint64_t>> seen;
    for (uint32_t cz = 0; cz < 4; ++cz)
        for (uint32_t cx = 0; cx < 4; ++cx)
            for (HlodAsset w : { HlodAsset::Material, HlodAsset::Albedo, HlodAsset::Normal, HlodAsset::Surface })
            {
                const HE::UUID id = HE::landscape::hlodAssetId(kTerrainId, cx, cz, w);
                CHECK(id == HE::landscape::hlodAssetId(kTerrainId, cx, cz, w));
                CHECK(seen.insert({ id.hi, id.lo }).second);
            }
    CHECK(HE::landscape::hlodAssetId(kTerrainId, 1, 2, HlodAsset::Material)
          != HE::landscape::hlodAssetId(HE::UUID{ 1, 2 }, 1, 2, HlodAsset::Material));
    // Every path is under the folder the export keeps as a reach root.
    CHECK(HE::landscape::hlodAssetPath(kTerrainId, 1, 2, HlodAsset::Albedo).rfind(HE::landscape::kHlodAssetFolder, 0) == 0);
}

TEST_CASE("buildHlodAssets makes three single-level textures and a material that names them")
{
    HE::landscape::HlodChunkRect rect;
    rect.min = glm::vec2(-32.0f, 0.0f);
    rect.size = glm::vec2(32.0f, 32.0f);
    std::vector<HE::landscape::HlodAssetBlob> blobs;
    std::string why;
    REQUIRE(HE::landscape::buildHlodAssets(syntheticImages(16), rect, kTerrainId, 3, 5, blobs, &why));
    INFO(why);
    REQUIRE(blobs.size() == 4);
    for (const auto& b : blobs) CHECK_FALSE(b.bytes.empty());

    ContentManager cm(".");
    REQUIRE(HE::landscape::loadHlodAssets(cm, blobs) == 4);

    const HE::UUID matId = HE::landscape::hlodAssetId(kTerrainId, 3, 5, HE::landscape::HlodAsset::Material);
    const MaterialAsset* mat = cm.getMaterial(matId);
    REQUIRE(mat != nullptr);
    // The three pictures, in the order the graph samples them.
    CHECK(mat->graphTexturePaths.size() == 3);
    for (const std::string& p : mat->graphTexturePaths)
        CHECK(p.rfind(HE::landscape::kHlodAssetFolder, 0) == 0);
    CHECK_FALSE(mat->customShaderFragGlsl.empty());

    using HE::landscape::HlodAsset;
    for (HlodAsset w : { HlodAsset::Albedo, HlodAsset::Normal, HlodAsset::Surface })
    {
        const TextureAsset* t = cm.getTexture(HE::landscape::hlodAssetId(kTerrainId, 3, 5, w));
        REQUIRE(t != nullptr);
        CHECK(t->width == 16);
        CHECK(t->height == 16);
        // Single level on purpose: the pack builds the mip chain and block-compresses it, and does neither for a
        // texture that arrives with mips already baked (the first real export grew 270 MB that way).
        CHECK(t->mipLevels == 1);
        CHECK(t->layers == 1);
        CHECK(t->srgb == (w == HlodAsset::Albedo)); // colour is sRGB, the normal and the surface are data
        CHECK(t->data.size() == 16u * 16u * 4u);
        CHECK(t->data[0] == (w == HlodAsset::Albedo ? 200 : w == HlodAsset::Normal ? 128 : 90));
    }
}

TEST_CASE("Every chunk's far material generates the same shader, so a landscape compiles one pipeline, not one per chunk")
{
    // The first measurement on a 256-chunk landscape: the chunk rectangle was a constant in the shader, 256 different
    // sources, 256 background compiles, and most chunks missing from the frame for seconds.
    HE::landscape::HlodChunkRect a, b;
    a.min = glm::vec2(-1000.0f, -1000.0f); a.size = glm::vec2(125.0f);
    b.min = glm::vec2(375.0f, 250.0f);     b.size = glm::vec2(125.0f);
    std::vector<HE::landscape::HlodAssetBlob> blobsA, blobsB;
    REQUIRE(HE::landscape::buildHlodAssets(syntheticImages(16), a, kTerrainId, 0, 0, blobsA));
    REQUIRE(HE::landscape::buildHlodAssets(syntheticImages(32), b, kTerrainId, 9, 11, blobsB));

    ContentManager cm(".");
    REQUIRE(HE::landscape::loadHlodAssets(cm, blobsA) == 4);
    REQUIRE(HE::landscape::loadHlodAssets(cm, blobsB) == 4);
    const MaterialAsset* ma = cm.getMaterial(HE::landscape::hlodAssetId(kTerrainId, 0, 0, HE::landscape::HlodAsset::Material));
    const MaterialAsset* mb = cm.getMaterial(HE::landscape::hlodAssetId(kTerrainId, 9, 11, HE::landscape::HlodAsset::Material));
    REQUIRE(ma != nullptr);
    REQUIRE(mb != nullptr);
    CHECK(ma->customShaderFragGlsl == mb->customShaderFragGlsl);
    CHECK(ma->customShaderGBufGlsl == mb->customShaderGBufGlsl);
    CHECK_FALSE(ma->customShaderFragGlsl.empty());
    // What differs is the data: the map parameter (min.x, min.y, uv scale, uv offset) and the textures.
    REQUIRE(ma->graphParamNames.size() == 1);
    CHECK(ma->graphParamNames[0] == "FarMap");
    REQUIRE(ma->shaderParamData.size() >= 4);
    REQUIRE(mb->shaderParamData.size() >= 4);
    CHECK(ma->shaderParamData[0] == doctest::Approx(-1000.0f));
    CHECK(ma->shaderParamData[1] == doctest::Approx(-1000.0f));
    CHECK(mb->shaderParamData[0] == doctest::Approx(375.0f));
    CHECK(mb->shaderParamData[1] == doctest::Approx(250.0f));
    CHECK(ma->shaderParamData[2] == doctest::Approx((1.0f - 1.0f / 16.0f) / 125.0f));
    CHECK(mb->shaderParamData[3] == doctest::Approx(0.5f / 32.0f));
    CHECK(ma->graphTexturePaths != mb->graphTexturePaths);
}

TEST_CASE("buildHlodAssets refuses pictures that do not match their side")
{
    HE::landscape::HlodChunkImages img = syntheticImages(16);
    img.normal.pop_back();
    std::vector<HE::landscape::HlodAssetBlob> blobs;
    std::string why;
    CHECK_FALSE(HE::landscape::buildHlodAssets(img, {}, kTerrainId, 0, 0, blobs, &why));
    CHECK_FALSE(why.empty());
    CHECK(blobs.empty());
}

TEST_CASE("A chunk with a baked material gives its far LOD levels that material, the near level and other chunks none")
{
    HorizonWorld world;
    ContentManager cm(".");
    auto& reg = world.registry();
    const entt::entity te = world.createEntity("Landscape");
    reg.emplace<TransformComponent>(te);
    TerrainComponent tc;
    tc.resolution = 129;
    tc.sizeX = tc.sizeZ = 100.0f;          // 2 x 2 chunks, 4 LODs
    tc.heightScale = 10.0f;
    tc.seed = 7;
    tc.dirty = true;
    reg.emplace<TerrainComponent>(te, tc);
    world.setEntityId(te, kTerrainId);

    // Without baked assets: no level carries a material.
    TerrainSystem::updateTerrains(world, cm);
    for (auto [e, cc, lod] : reg.view<TerrainChunkComponent, LODComponent>().each())
    {
        REQUIRE(lod.levels.size() == 4);
        for (const LODLevel& lv : lod.levels) CHECK(lv.materialId == HE::UUID{});
    }

    // Bake chunk (0, 1) only.
    std::vector<HE::landscape::HlodAssetBlob> blobs;
    HE::landscape::HlodChunkRect rect;
    rect.min = glm::vec2(-50.0f, 0.0f); rect.size = glm::vec2(50.0f, 50.0f);
    REQUIRE(HE::landscape::buildHlodAssets(syntheticImages(8), rect, kTerrainId, 0, 1, blobs));
    REQUIRE(HE::landscape::loadHlodAssets(cm, blobs) == 4);
    reg.get<TerrainComponent>(te).dirty = true;
    TerrainSystem::updateTerrains(world, cm);

    const HE::UUID matId = HE::landscape::hlodAssetId(kTerrainId, 0, 1, HE::landscape::HlodAsset::Material);
    int baked = 0;
    for (auto [e, cc, lod] : reg.view<TerrainChunkComponent, LODComponent>().each())
    {
        REQUIRE(lod.levels.size() == 4);
        const bool mine = cc.cx == 0 && cc.cz == 1;
        CHECK(lod.levels[0].materialId == HE::UUID{});           // the near level keeps the landscape's graph
        for (size_t k = 1; k < lod.levels.size(); ++k)
        {
            CHECK(lod.levels[k].materialId == (mine ? matId : HE::UUID{}));
            if (mine && lod.levels[k].materialId == matId) ++baked;
        }
    }
    CHECK(baked == 3);
}
