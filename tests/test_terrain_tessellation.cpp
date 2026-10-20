#include "doctest.h"
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TerrainChunkComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/LODComponent.h>
#include <HorizonScene/TerrainMeshGenerator.h>
#include <HorizonScene/TerrainSystem.h>
#include <HorizonScene/LODSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <ContentManager/ContentManager.h>

#include <cmath>
#include <string>
#include <vector>

// Terrain tessellation: a level finer than LOD0 for the chunks near the camera
// (TerrainComponent::tessellationFactor). CPU-side, so everything here is
// plain meshes and components — no renderer.

namespace
{
    // A bumpy but deterministic field, res×res.
    std::vector<float> bumpyField(uint32_t res)
    {
        std::vector<float> h(static_cast<size_t>(res) * res);
        for (uint32_t z = 0; z < res; ++z)
            for (uint32_t x = 0; x < res; ++x)
                h[static_cast<size_t>(z) * res + x] =
                    3.0f * std::sin(0.7f * static_cast<float>(x)) +
                    2.0f * std::cos(0.45f * static_cast<float>(z)) +
                    0.1f * static_cast<float>(x * z % 7);
        return h;
    }

    size_t vertexCount(const StaticMeshAsset& m) { return m.vertices.size() / 3; }

    // A landscape at the origin: resolution 129 on 100 m gives a 2×2 chunk
    // grid (64 cells per chunk, 4 LODs), chunk centres at (±25, ±25).
    Entity makeLandscape(HorizonWorld& world, int factor, float distance)
    {
        auto& reg = world.registry();
        Entity te = world.createEntity("Landscape");
        reg.emplace<TransformComponent>(te);
        TerrainComponent tc;
        tc.resolution = 129;
        tc.sizeX = tc.sizeZ = 100.0f;
        tc.seed = 7;
        tc.heightScale = 10.0f;
        tc.tessellationFactor   = factor;
        tc.tessellationDistance = distance;
        tc.dirty = true;
        reg.emplace<TerrainComponent>(te, tc);
        return te;
    }

    struct TessCount { size_t active = 0, chunks = 0; };
    TessCount countTess(HorizonWorld& world)
    {
        TessCount c;
        for (auto [e, cc] : world.registry().view<TerrainChunkComponent>().each())
        {
            ++c.chunks;
            if (cc.tessActive) ++c.active;
        }
        return c;
    }

    entt::entity chunkAt(HorizonWorld& world, uint32_t cx, uint32_t cz)
    {
        for (auto [e, cc] : world.registry().view<TerrainChunkComponent>().each())
            if (cc.cx == cx && cc.cz == cz) return e;
        return entt::null;
    }

    void tick(HorizonWorld& world, ContentManager& cm, const glm::vec3& cam)
    {
        TerrainSystem::updateTerrains(world, cm);
        TerrainSystem::updateTessellation(world, cm, nullptr, cam);
        LODSystem::update(world, cam);
    }
}

// ── The smooth height ─────────────────────────────────────────────────────────

TEST_CASE("sampleTerrainHeightSmooth passes through every source sample")
{
    const uint32_t res = 17;
    const std::vector<float> h = bumpyField(res);
    for (uint32_t z = 0; z < res; ++z)
        for (uint32_t x = 0; x < res; ++x)
        {
            const float u = static_cast<float>(x) / static_cast<float>(res - 1);
            const float v = static_cast<float>(z) / static_cast<float>(res - 1);
            CHECK(sampleTerrainHeightSmooth(h, res, u, v)
                  == doctest::Approx(h[static_cast<size_t>(z) * res + x]).epsilon(1e-4));
        }
}

TEST_CASE("sampleTerrainHeightSmooth reproduces a linear slope between the samples")
{
    // Catmull-Rom is exact on linear data wherever it has real neighbours —
    // i.e. everywhere but the outermost cell, where the edge sample repeats.
    const uint32_t res = 9;
    std::vector<float> h(static_cast<size_t>(res) * res);
    for (uint32_t z = 0; z < res; ++z)
        for (uint32_t x = 0; x < res; ++x)
            h[static_cast<size_t>(z) * res + x] = 2.0f * x - 0.5f * z;
    for (float u = 0.13f; u < 0.87f; u += 0.0371f)
        for (float v = 0.13f; v < 0.87f; v += 0.0413f)
        {
            const float fx = u * (res - 1), fz = v * (res - 1);
            CHECK(sampleTerrainHeightSmooth(h, res, u, v)
                  == doctest::Approx(2.0f * fx - 0.5f * fz).epsilon(1e-4));
        }
}

// ── Displacement ──────────────────────────────────────────────────────────────

TEST_CASE("sampleTerrainDisplacement is centred on mid-grey and wraps")
{
    // 2×2: black, white / mid, mid.
    const std::vector<float> grey = { 0.0f, 1.0f, 0.5f, 0.5f };
    TerrainDisplacementMap d;
    d.grey = grey.data(); d.width = 2; d.height = 2;
    d.strength = 2.0f; d.tiling = 1.0f;

    // Texel centres: (0.25, 0.25) is the black texel, (0.75, 0.25) the white.
    CHECK(sampleTerrainDisplacement(d, 0.25f, 0.25f) == doctest::Approx(-1.0f));
    CHECK(sampleTerrainDisplacement(d, 0.75f, 0.25f) == doctest::Approx( 1.0f));
    CHECK(sampleTerrainDisplacement(d, 0.25f, 0.75f) == doctest::Approx( 0.0f));
    // One tile over is the same place.
    CHECK(sampleTerrainDisplacement(d, 1.25f, 0.25f) == doctest::Approx(-1.0f));
    // tiling 2 → the pattern repeats every half terrain.
    d.tiling = 2.0f;
    CHECK(sampleTerrainDisplacement(d, 0.125f, 0.125f)
          == doctest::Approx(sampleTerrainDisplacement(d, 0.625f, 0.625f)));
    // No strength, no map → nothing.
    d.strength = 0.0f;
    CHECK(sampleTerrainDisplacement(d, 0.25f, 0.25f) == 0.0f);
    CHECK(sampleTerrainDisplacement(TerrainDisplacementMap{}, 0.4f, 0.4f) == 0.0f);
}

// ── The refined chunk mesh ────────────────────────────────────────────────────

TEST_CASE("A tessellated chunk agrees with LOD0 on every LOD0 vertex")
{
    const uint32_t res = 33;
    const std::vector<float> h = bumpyField(res);
    const float u0 = 0.25f, v0 = 0.5f, u1 = 0.5f, v1 = 0.75f;   // 8 source cells a side
    const uint32_t lod0Verts = 9, factor = 4, tessVerts = 8 * factor + 1;

    const StaticMeshAsset lod0 = generateTerrainChunkMesh(h, res, 40.0f, 40.0f,
                                                          u0, v0, u1, v1, lod0Verts, 3.0f);
    const StaticMeshAsset tess = generateTerrainChunkMeshTessellated(
        h, res, 40.0f, 40.0f, u0, v0, u1, v1, tessVerts, 3.0f, TerrainDisplacementMap{});

    // (factor × cells + 1)² grid vertices + one skirt vertex per ring vertex.
    const size_t grid = static_cast<size_t>(tessVerts) * tessVerts;
    CHECK(vertexCount(tess) == grid + 4 * (tessVerts - 1));
    CHECK(tess.indices.size() == static_cast<size_t>(tessVerts - 1) * (tessVerts - 1) * 6
                                 + 4 * (tessVerts - 1) * 6);

    for (uint32_t j = 0; j < lod0Verts; ++j)
        for (uint32_t i = 0; i < lod0Verts; ++i)
        {
            const size_t a = static_cast<size_t>(j) * lod0Verts + i;
            const size_t b = static_cast<size_t>(j * factor) * tessVerts + i * factor;
            for (int k = 0; k < 3; ++k)
            {
                CHECK(tess.vertices[b * 3 + k] == doctest::Approx(lod0.vertices[a * 3 + k]).epsilon(1e-4));
                // Same normals, so no lighting edge where a refined chunk
                // meets a LOD0 one.
                CHECK(tess.normals[b * 3 + k] == doctest::Approx(lod0.normals[a * 3 + k]).epsilon(1e-4));
            }
            CHECK(tess.uvs[b * 2 + 0] == doctest::Approx(lod0.uvs[a * 2 + 0]));
            CHECK(tess.uvs[b * 2 + 1] == doctest::Approx(lod0.uvs[a * 2 + 1]));
        }

    // Between the samples it is NOT bilinear: somewhere mid-cell the smooth
    // surface leaves the flat facet LOD0 draws.
    float maxDev = 0.0f;
    for (uint32_t j = 0; j + 1 < tessVerts; ++j)
        for (uint32_t i = 0; i + 1 < tessVerts; ++i)
        {
            if (i % factor == 0 && j % factor == 0) continue;
            const float u = u0 + (u1 - u0) * static_cast<float>(i) / (tessVerts - 1);
            const float v = v0 + (v1 - v0) * static_cast<float>(j) / (tessVerts - 1);
            // Bilinear reference, from the field directly.
            const float fx = u * (res - 1), fz = v * (res - 1);
            const int ix = static_cast<int>(fx), iz = static_cast<int>(fz);
            const float tx = fx - ix, tz = fz - iz;
            auto at = [&](int x, int z) { return h[static_cast<size_t>(z) * res + x]; };
            const float bil = (1 - tz) * ((1 - tx) * at(ix, iz) + tx * at(ix + 1, iz))
                            + tz * ((1 - tx) * at(ix, iz + 1) + tx * at(ix + 1, iz + 1));
            const size_t b = static_cast<size_t>(j) * tessVerts + i;
            maxDev = std::max(maxDev, std::fabs(tess.vertices[b * 3 + 1] - bil));
        }
    CHECK(maxDev > 0.05f);
}

TEST_CASE("Neighbouring tessellated chunks share their edge, displacement included")
{
    const uint32_t res = 17;
    const std::vector<float> h = bumpyField(res);
    std::vector<float> grey(16 * 16);
    for (size_t i = 0; i < grey.size(); ++i) grey[i] = static_cast<float>((i * 37) % 16) / 15.0f;
    TerrainDisplacementMap d;
    d.grey = grey.data(); d.width = 16; d.height = 16; d.strength = 0.4f; d.tiling = 3.0f;

    const uint32_t N = 8 * 2 + 1;   // 8 cells a side at factor 2
    const StaticMeshAsset L = generateTerrainChunkMeshTessellated(h, res, 30.0f, 30.0f,
                                  0.0f, 0.0f, 0.5f, 0.5f, N, 1.0f, d);
    const StaticMeshAsset R = generateTerrainChunkMeshTessellated(h, res, 30.0f, 30.0f,
                                  0.5f, 0.0f, 1.0f, 0.5f, N, 1.0f, d);
    // L's right column is R's left column. The vertices are chunk-local, so
    // compare heights (y) and normals, which are absolute.
    for (uint32_t j = 0; j < N; ++j)
    {
        const size_t l = static_cast<size_t>(j) * N + (N - 1);
        const size_t r = static_cast<size_t>(j) * N + 0;
        CHECK(L.vertices[l * 3 + 1] == doctest::Approx(R.vertices[r * 3 + 1]).epsilon(1e-4));
        for (int k = 0; k < 3; ++k)
            CHECK(L.normals[l * 3 + k] == doctest::Approx(R.normals[r * 3 + k]).epsilon(1e-3));
    }
}

TEST_CASE("Displacement lifts the tessellated surface and tilts its normals")
{
    const uint32_t res = 9;
    const std::vector<float> flat(static_cast<size_t>(res) * res, 0.0f);
    const uint32_t N = 8 * 4 + 1;

    // All white: the whole surface rises by half the strength, normals stay up.
    const std::vector<float> white(4, 1.0f);
    TerrainDisplacementMap d;
    d.grey = white.data(); d.width = 2; d.height = 2; d.strength = 0.6f;
    const StaticMeshAsset up = generateTerrainChunkMeshTessellated(flat, res, 16.0f, 16.0f,
                                   0.0f, 0.0f, 1.0f, 1.0f, N, 1.0f, d);
    for (size_t i = 0; i < static_cast<size_t>(N) * N; ++i)
    {
        CHECK(up.vertices[i * 3 + 1] == doctest::Approx(0.3f));
        CHECK(up.normals[i * 3 + 1] == doctest::Approx(1.0f));
    }

    // A ramp along X: the normals lean against it.
    const std::vector<float> ramp = { 0.0f, 0.25f, 0.5f, 0.75f };   // 4×1
    d.grey = ramp.data(); d.width = 4; d.height = 1; d.strength = 2.0f;
    const StaticMeshAsset tilted = generateTerrainChunkMeshTessellated(flat, res, 16.0f, 16.0f,
                                       0.0f, 0.0f, 1.0f, 1.0f, N, 1.0f, d);
    // Mid-row, inside the rising part of the ramp (texel centres 0.125…0.625).
    const size_t mid = static_cast<size_t>(N / 2) * N + N / 3;
    CHECK(tilted.normals[mid * 3 + 0] < -0.01f);   // rising along +X → normal leans -X
    CHECK(std::fabs(tilted.normals[mid * 3 + 2]) < 1e-3f);

    // The skirt reaches half the strength deeper than without displacement,
    // so a displaced edge still covers the crack to a LOD0 neighbour.
    const StaticMeshAsset none = generateTerrainChunkMeshTessellated(flat, res, 16.0f, 16.0f,
                                     0.0f, 0.0f, 1.0f, 1.0f, N, 1.0f, TerrainDisplacementMap{});
    float minNone = 1e30f, minUp = 1e30f;
    for (size_t i = 0; i < vertexCount(none); ++i) minNone = std::min(minNone, none.vertices[i * 3 + 1]);
    for (size_t i = 0; i < vertexCount(up);   ++i) minUp   = std::min(minUp,   up.vertices[i * 3 + 1]);
    // Skirt depth below its own surface: 0.3 (= strength / 2) more when displaced.
    CHECK(0.3f - minUp == doctest::Approx((0.0f - minNone) + 0.3f));
}

// ── The life cycle ────────────────────────────────────────────────────────────

TEST_CASE("Tessellation off builds nothing and leaves the LOD chain as it was")
{
    HorizonWorld world;
    ContentManager cm(".");
    makeLandscape(world, 1, 1000.0f);
    tick(world, cm, glm::vec3(-25.0f, 5.0f, -25.0f));
    const TessCount c = countTess(world);
    REQUIRE(c.chunks == 4);
    CHECK(c.active == 0);
    for (auto [e, cc, lod] : world.registry().view<TerrainChunkComponent, LODComponent>().each())
    {
        CHECK(lod.levels.size() == 4);
        CHECK(lod.refinedMeshId == HE::UUID{});
        CHECK(cc.tessMeshId == HE::UUID{});
    }
}

TEST_CASE("The chunk under the camera gets the refined level and LOD picks it")
{
    HorizonWorld world;
    ContentManager cm(".");
    makeLandscape(world, 4, 30.0f);
    const glm::vec3 cam(-25.0f, 5.0f, -25.0f);   // over chunk (0,0)
    tick(world, cm, cam);

    auto& reg = world.registry();
    const entt::entity near = chunkAt(world, 0, 0);
    REQUIRE((near != entt::null));
    const auto& cc  = reg.get<TerrainChunkComponent>(near);
    const auto& lod = reg.get<LODComponent>(near);
    REQUIRE(cc.tessActive);
    CHECK(lod.refinedMeshId == cc.tessMeshId);
    CHECK(lod.refinedMaxDistance == doctest::Approx(30.0f));
    CHECK(lod.current == LODComponent::kRefined);
    CHECK(reg.get<MeshComponent>(near).meshAssetId == cc.tessMeshId);
    // The regular chain is untouched: levels[0] is still LOD0, which is what
    // navigation, physics and the editor read as "the full-detail mesh".
    REQUIRE(lod.levels.size() == 4);
    CHECK(lod.levels.front().meshId != cc.tessMeshId);
    const StaticMeshAsset* lod0 = cm.getStaticMesh(lod.levels.front().meshId);
    REQUIRE(lod0 != nullptr);
    CHECK(vertexCount(*lod0) == 65u * 65u + 4u * 64u);
    const StaticMeshAsset* m = cm.getStaticMesh(cc.tessMeshId);
    REQUIRE(m != nullptr);
    CHECK(vertexCount(*m) == 257u * 257u + 4u * 256u);   // 64 cells × 4, plus skirt

    // The other three are 50 m and more away: over the 37.5 m build margin.
    CHECK(countTess(world).active == 1);
}

TEST_CASE("Refined levels are built two per tick and given back when the camera leaves")
{
    HorizonWorld world;
    ContentManager cm(".");
    makeLandscape(world, 2, 200.0f);
    const glm::vec3 cam(0.0f, 5.0f, 0.0f);
    tick(world, cm, cam);
    CHECK(countTess(world).active == 2);    // the budget, not the distance, stopped it
    tick(world, cm, cam);
    CHECK(countTess(world).active == 4);

    std::vector<HE::UUID> ids;
    for (auto [e, cc] : world.registry().view<TerrainChunkComponent>().each())
        ids.push_back(cc.tessMeshId);

    // Inside the release margin (1.5 × 200 m from every chunk centre) nothing goes.
    tick(world, cm, glm::vec3(0.0f, 250.0f, 0.0f));
    CHECK(countTess(world).active == 4);

    // Far away: all four go, unhooked from their LODComponents, and the
    // meshes are emptied — but stay registered, so coming back is a replace
    // rather than a new registration.
    tick(world, cm, glm::vec3(5000.0f, 5.0f, 0.0f));
    CHECK(countTess(world).active == 0);
    for (auto [e, cc, lod, mc] :
         world.registry().view<TerrainChunkComponent, LODComponent, MeshComponent>().each())
    {
        CHECK(lod.levels.size() == 4);
        CHECK(lod.refinedMeshId == HE::UUID{});
        CHECK(mc.meshAssetId != cc.tessMeshId);
        const StaticMeshAsset* m = cm.getStaticMesh(cc.tessMeshId);
        REQUIRE(m != nullptr);
        CHECK(m->vertices.empty());
    }

    tick(world, cm, cam);
    tick(world, cm, cam);
    CHECK(countTess(world).active == 4);
    size_t same = 0;
    for (auto [e, cc] : world.registry().view<TerrainChunkComponent>().each())
        for (const HE::UUID& id : ids) if (id == cc.tessMeshId) ++same;
    CHECK(same == 4);
}

TEST_CASE("At most sixteen chunks of one landscape are refined at once, nearest first")
{
    HorizonWorld world;
    ContentManager cm(".");
    auto& reg = world.registry();
    Entity te = world.createEntity("Landscape");
    reg.emplace<TransformComponent>(te);
    TerrainComponent tc;
    tc.resolution = 513;                 // 8×8 chunks of 64 cells, 12.5 m each
    tc.sizeX = tc.sizeZ = 100.0f;
    tc.tessellationFactor   = 2;
    tc.tessellationDistance = 1000.0f;   // every chunk is in range
    tc.dirty = true;
    reg.emplace<TerrainComponent>(te, tc);

    const glm::vec3 cam(-50.0f, 1.0f, -50.0f);   // the -X/-Z corner
    for (int i = 0; i < 12; ++i) tick(world, cm, cam);
    const TessCount c = countTess(world);
    REQUIRE(c.chunks == 64);
    CHECK(c.active == 16);
    // The corner chunk is among them; the opposite corner is not.
    CHECK(reg.get<TerrainChunkComponent>(chunkAt(world, 0, 0)).tessActive);
    CHECK_FALSE(reg.get<TerrainChunkComponent>(chunkAt(world, 7, 7)).tessActive);
}

TEST_CASE("Sculpting under the camera rebuilds the refined level in place")
{
    HorizonWorld world;
    ContentManager cm(".");
    Entity te = makeLandscape(world, 2, 30.0f);
    const glm::vec3 cam(-25.0f, 5.0f, -25.0f);
    tick(world, cm, cam);
    auto& reg = world.registry();
    const entt::entity near = chunkAt(world, 0, 0);
    const HE::UUID id = reg.get<TerrainChunkComponent>(near).tessMeshId;
    REQUIRE(reg.get<TerrainChunkComponent>(near).tessActive);

    // A plateau over the whole field, region-dirty over chunk (0,0).
    auto& tc = reg.get<TerrainComponent>(te);
    tc.sculptHeights.assign(static_cast<size_t>(tc.resolution) * tc.resolution, 42.0f);
    tc.regionDirty = true;
    tc.dirtyMinX = -40.0f; tc.dirtyMinZ = -40.0f; tc.dirtyMaxX = -10.0f; tc.dirtyMaxZ = -10.0f;
    TerrainSystem::updateTerrains(world, cm);

    // Same UUID, still hooked in AND still the drawn mesh (the editor's
    // direct updateTerrains calls have no LOD tick behind them, so dropping
    // to LOD0 here would pop for a frame per brush step), already at the new
    // height — before updateTessellation or LODSystem have run again.
    const auto& cc = reg.get<TerrainChunkComponent>(near);
    CHECK(cc.tessActive);
    CHECK(cc.tessMeshId == id);
    CHECK(reg.get<LODComponent>(near).refinedMeshId == id);
    CHECK(reg.get<LODComponent>(near).levels.size() == 4);
    CHECK(reg.get<MeshComponent>(near).meshAssetId == id);
    const StaticMeshAsset* m = cm.getStaticMesh(id);
    REQUIRE(m != nullptr);
    REQUIRE(m->vertices.size() > 1);
    CHECK(m->vertices[1] == doctest::Approx(42.0f));

    // Switching tessellation off on a rebuild gives it back.
    tc.tessellationFactor = 1;
    tc.dirty = true;
    tick(world, cm, cam);
    CHECK(countTess(world).active == 0);
    CHECK(reg.get<LODComponent>(near).refinedMeshId == HE::UUID{});
    CHECK(reg.get<MeshComponent>(near).meshAssetId != id);
}

TEST_CASE("The refined mesh of a chunk that is gone is unloaded")
{
    HorizonWorld world;
    ContentManager cm(".");
    Entity te = makeLandscape(world, 2, 30.0f);
    const glm::vec3 cam(-25.0f, 5.0f, -25.0f);
    tick(world, cm, cam);
    const HE::UUID id = world.registry().get<TerrainChunkComponent>(chunkAt(world, 0, 0)).tessMeshId;
    REQUIRE(cm.getStaticMesh(id) != nullptr);

    // A resolution change destroys the whole chunk grid; the next tick builds
    // a new one and gives the old refined mesh back.
    world.registry().get<TerrainComponent>(te).resolution = 257;
    world.registry().get<TerrainComponent>(te).dirty = true;
    tick(world, cm, cam);
    CHECK(cm.getStaticMesh(id) == nullptr);
}

// updateTessellation measures camera distances only for a landscape that
// refines (SCN-801). Everything else it does per chunk has to happen anyway.

TEST_CASE("With tessellation off, a refined level the chunk does not own is unhooked, not emptied")
{
    HorizonWorld world;
    ContentManager cm(".");
    makeLandscape(world, 1, 30.0f);
    const glm::vec3 cam(-25.0f, 5.0f, -25.0f);
    tick(world, cm, cam);

    // What a copied world leaves on a chunk: the refined mesh of the world it
    // was copied from, hooked in, and not in this world's bookkeeping.
    auto& reg = world.registry();
    const entt::entity ce = chunkAt(world, 0, 0);
    REQUIRE((ce != entt::null));
    StaticMeshAsset foreign;
    foreign.type = HE::AssetType::StaticMesh;
    foreign.vertices = { 1.0f, 2.0f, 3.0f };
    const HE::UUID id = cm.registerStaticMesh(std::move(foreign));
    auto& cc  = reg.get<TerrainChunkComponent>(ce);
    auto& lod = reg.get<LODComponent>(ce);
    cc.tessMeshId = id;
    cc.tessActive = true;
    lod.refinedMeshId      = id;
    lod.refinedMaxDistance = 30.0f;

    TerrainSystem::updateTessellation(world, cm, nullptr, cam);
    CHECK(cc.tessMeshId == HE::UUID{});
    CHECK_FALSE(cc.tessActive);
    CHECK(lod.refinedMeshId == HE::UUID{});
    // The other world's mesh is not this chunk's to empty.
    const StaticMeshAsset* m = cm.getStaticMesh(id);
    REQUIRE(m != nullptr);
    CHECK(m->vertices.size() == 3);
}

TEST_CASE("Switching tessellation off without a rebuild gives the refined levels back")
{
    HorizonWorld world;
    ContentManager cm(".");
    Entity te = makeLandscape(world, 2, 30.0f);
    const glm::vec3 cam(-25.0f, 5.0f, -25.0f);
    tick(world, cm, cam);
    auto& reg = world.registry();
    const entt::entity near = chunkAt(world, 0, 0);
    const HE::UUID id = reg.get<TerrainChunkComponent>(near).tessMeshId;
    REQUIRE(reg.get<TerrainChunkComponent>(near).tessActive);

    // The Inspector's factor field: the height field is unchanged, nothing dirty.
    reg.get<TerrainComponent>(te).tessellationFactor = 1;
    TerrainSystem::updateTessellation(world, cm, nullptr, cam);
    CHECK(countTess(world).active == 0);
    CHECK(reg.get<LODComponent>(near).refinedMeshId == HE::UUID{});
    CHECK(reg.get<MeshComponent>(near).meshAssetId != id);
    const StaticMeshAsset* m = cm.getStaticMesh(id);
    REQUIRE(m != nullptr);
    CHECK(m->vertices.empty());
}

TEST_CASE("Each landscape is handled by its own state: refining, off, rebuilding or gone")
{
    HorizonWorld world;
    ContentManager cm(".");
    auto& reg = world.registry();
    // Four landscapes on top of each other, all refining at first.
    Entity tes[4];
    for (int i = 0; i < 4; ++i) tes[i] = makeLandscape(world, 2, 30.0f);
    TerrainSystem::updateTerrains(world, cm);
    auto nearChunk = [&](int i)
    {
        for (auto [e, cc] : reg.view<TerrainChunkComponent>().each())
            if (cc.terrain == tes[i] && cc.cx == 0 && cc.cz == 0) return e;
        return entt::entity{ entt::null };
    };
    // One chunk each under the camera: two ticks of two builds.
    const glm::vec3 cam(-25.0f, 5.0f, -25.0f);
    TerrainSystem::updateTessellation(world, cm, nullptr, cam);
    TerrainSystem::updateTessellation(world, cm, nullptr, cam);
    for (int i = 0; i < 4; ++i)
    {
        REQUIRE((nearChunk(i) != entt::null));
        REQUIRE(reg.get<TerrainChunkComponent>(nearChunk(i)).tessActive);
    }

    // 0 keeps refining, 1 is switched off, 2 waits for a rebuild that has not
    // run yet, 3 has lost its TerrainComponent. The camera is far from all.
    reg.get<TerrainComponent>(tes[1]).tessellationFactor = 1;
    reg.get<TerrainComponent>(tes[2]).regionDirty = true;
    reg.remove<TerrainComponent>(tes[3]);
    TerrainSystem::updateTessellation(world, cm, nullptr, glm::vec3(0.0f, 5000.0f, 0.0f));

    CHECK_FALSE(reg.get<TerrainChunkComponent>(nearChunk(0)).tessActive);   // out of range
    CHECK_FALSE(reg.get<TerrainChunkComponent>(nearChunk(1)).tessActive);   // off
    CHECK(reg.get<TerrainChunkComponent>(nearChunk(2)).tessActive);         // updateTerrains' tick
    CHECK(reg.get<TerrainChunkComponent>(nearChunk(3)).tessActive);         // left as it is

    // Back: only the refining landscape builds again.
    TerrainSystem::updateTessellation(world, cm, nullptr, cam);
    CHECK(reg.get<TerrainChunkComponent>(nearChunk(0)).tessActive);
    CHECK_FALSE(reg.get<TerrainChunkComponent>(nearChunk(1)).tessActive);
}

TEST_CASE("A displacement texture that is missing or compressed just means no displacement")
{
    HorizonWorld world;
    ContentManager cm(".");
    Entity te = makeLandscape(world, 2, 30.0f);
    auto& tc = world.registry().get<TerrainComponent>(te);
    tc.displacementTexture  = HE::UUID{ 0x1234ull, 0x5678ull };   // not an asset
    tc.displacementStrength = 1.0f;
    const glm::vec3 cam(-25.0f, 5.0f, -25.0f);
    tick(world, cm, cam);
    REQUIRE(countTess(world).active == 1);

    // Same landscape without the texture: identical heights.
    HorizonWorld ref;
    ContentManager cm2(".");
    makeLandscape(ref, 2, 30.0f);
    tick(ref, cm2, cam);
    const StaticMeshAsset* a = cm.getStaticMesh(
        world.registry().get<TerrainChunkComponent>(chunkAt(world, 0, 0)).tessMeshId);
    const StaticMeshAsset* b = cm2.getStaticMesh(
        ref.registry().get<TerrainChunkComponent>(chunkAt(ref, 0, 0)).tessMeshId);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a->vertices == b->vertices);

    // A registered texture does displace: all white lifts by strength / 2.
    TextureAsset white;
    white.type = HE::AssetType::Texture;
    white.width = white.height = 2; white.channels = 4;
    white.data.assign(16, 255);
    const HE::UUID texId = cm.registerTexture(std::move(white));
    tc.displacementTexture = texId;
    tc.dirty = true;
    tick(world, cm, cam);
    a = cm.getStaticMesh(
        world.registry().get<TerrainChunkComponent>(chunkAt(world, 0, 0)).tessMeshId);
    b = cm2.getStaticMesh(
        ref.registry().get<TerrainChunkComponent>(chunkAt(ref, 0, 0)).tessMeshId);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a->vertices.size() > 1);
    REQUIRE(b->vertices.size() > 1);
    CHECK(a->vertices[1] == doctest::Approx(b->vertices[1] + 0.5f));
}

TEST_CASE("Tessellation settings round-trip, and an unused block is not written")
{
    SceneSerializer ser;
    {
        HorizonWorld world;
        Entity e = world.createEntity("t");
        world.registry().emplace<TerrainComponent>(e, TerrainComponent{});
        std::vector<uint8_t> bytes;
        REQUIRE(ser.saveToMemory(world, bytes));
        const std::string text(bytes.begin(), bytes.end());
        CHECK(text.find("tessellationFactor") == std::string::npos);
        CHECK(text.find("displacement") == std::string::npos);
    }

    HorizonWorld world;
    Entity e = world.createEntity("t");
    TerrainComponent tc;
    tc.tessellationFactor   = 4;
    tc.tessellationDistance = 75.0f;
    tc.displacementTexture  = HE::UUID{ 0xAAull, 0xBBull };
    tc.displacementStrength = 0.35f;
    tc.displacementTiling   = 24.0f;
    world.registry().emplace<TerrainComponent>(e, tc);
    std::vector<uint8_t> bytes;
    REQUIRE(ser.saveToMemory(world, bytes));
    HorizonWorld w2;
    REQUIRE(ser.loadFromMemory(w2, bytes));
    const TerrainComponent* l = nullptr;
    for (auto ent : w2.registry().view<TerrainComponent>())
        l = &w2.registry().get<TerrainComponent>(ent);
    REQUIRE(l != nullptr);
    CHECK(l->tessellationFactor == 4);
    CHECK(l->tessellationDistance == doctest::Approx(75.0f));
    CHECK(l->displacementTexture == HE::UUID{ 0xAAull, 0xBBull });
    CHECK(l->displacementStrength == doctest::Approx(0.35f));
    CHECK(l->displacementTiling == doctest::Approx(24.0f));
}

// ── Seam stitching (Thema 190) ────────────────────────────────────────────────
// Where two chunks of different detail meet, the finer edge has to lie on the
// coarser one; any step there casts a shadow the GI sun ray turns into a dashed
// black line. The witness is the largest height gap between the two edge
// polylines, both ways round, in world space.

namespace
{
    struct EdgePt { float s, y; };   // position along the edge, height

    // The grid edge `e` (TerrainChunkStitch order: v0, u1, v1, u0) of a chunk
    // mesh with n vertices a side, in world space (the chunk centre added).
    std::vector<EdgePt> edgeLine(const StaticMeshAsset& m, size_t n, int e,
                                 float centreX, float centreZ)
    {
        std::vector<EdgePt> pts;
        for (size_t p = 0; p < n; ++p)
        {
            const size_t idx = e == 0 ? p : e == 1 ? p * n + (n - 1)
                             : e == 2 ? (n - 1) * n + p : p * n;
            const bool alongX = (e == 0 || e == 2);
            pts.push_back({ alongX ? m.vertices[idx * 3 + 0] + centreX
                                   : m.vertices[idx * 3 + 2] + centreZ,
                            m.vertices[idx * 3 + 1] });
        }
        return pts;
    }

    float heightOn(const std::vector<EdgePt>& line, float s)
    {
        for (size_t k = 0; k + 1 < line.size(); ++k)
            if (s >= line[k].s - 1e-4f && s <= line[k + 1].s + 1e-4f)
            {
                const float t = std::clamp((s - line[k].s) / (line[k + 1].s - line[k].s), 0.0f, 1.0f);
                return line[k].y + (line[k + 1].y - line[k].y) * t;
            }
        return line.back().y;
    }

    float edgeGap(const std::vector<EdgePt>& a, const std::vector<EdgePt>& b)
    {
        float gap = 0.0f;
        for (const EdgePt& p : a) gap = std::max(gap, std::fabs(p.y - heightOn(b, p.s)));
        for (const EdgePt& p : b) gap = std::max(gap, std::fabs(p.y - heightOn(a, p.s)));
        return gap;
    }

    // Vertices a side of a chunk mesh: n² grid + 4(n-1) skirt vertices.
    size_t sideOf(const StaticMeshAsset& m)
    {
        const size_t v = vertexCount(m);
        size_t n = 2;
        while (n * n + 4 * (n - 1) < v) ++n;
        return n;
    }

    // The widest step along any seam of the landscape, between the meshes the
    // chunks DRAW. `mixed` counts the seams whose two sides differ in detail.
    float worstSeam(HorizonWorld& world, ContentManager& cm, int* mixed = nullptr)
    {
        auto& reg = world.registry();
        float worst = 0.0f;
        if (mixed) *mixed = 0;
        for (auto [a, ca, ma, ta] : reg.view<TerrainChunkComponent, MeshComponent, TransformComponent>().each())
            for (int e = 1; e <= 2; ++e)   // right and top neighbours: each seam once
            {
                const entt::entity b = chunkAt(world, ca.cx + (e == 1 ? 1 : 0), ca.cz + (e == 2 ? 1 : 0));
                if (b == entt::null) continue;
                const StaticMeshAsset* A = cm.getStaticMesh(ma.meshAssetId);
                const StaticMeshAsset* B = cm.getStaticMesh(reg.get<MeshComponent>(b).meshAssetId);
                REQUIRE(A != nullptr);
                REQUIRE(B != nullptr);
                const auto& tb = reg.get<TransformComponent>(b);
                const size_t na = sideOf(*A), nb = sideOf(*B);
                if (mixed && na != nb) ++*mixed;
                worst = std::max(worst, edgeGap(
                    edgeLine(*A, na, e,     ta.position.x, ta.position.z),
                    edgeLine(*B, nb, (e + 2) % 4, tb.position.x, tb.position.z)));
            }
        return worst;
    }
}

TEST_CASE("A stitched LOD0 edge lies on its LOD2 neighbour's edge")
{
    const uint32_t res = 33;   // 32 source cells; two chunks of 16 side by side
    const std::vector<float> h = bumpyField(res);
    const float size = 40.0f;
    const StaticMeshAsset fine   = generateTerrainChunkMesh(h, res, size, size, 0.0f, 0.0f, 0.5f, 0.5f, 17);
    const StaticMeshAsset coarse = generateTerrainChunkMesh(h, res, size, size, 0.5f, 0.0f, 1.0f, 0.5f, 5);
    const float fineX = -10.0f, coarseX = 10.0f, z = -10.0f;   // chunk centres

    const float before = edgeGap(edgeLine(fine, 17, 1, fineX, z), edgeLine(coarse, 5, 3, coarseX, z));
    CHECK(before > 0.05f);   // the lens-shaped step the GI sun ray hits

    TerrainChunkStitch st;
    st.edgeCells[1] = 4;
    const StaticMeshAsset s = stitchTerrainChunkEdges(fine, 16, fine, 16, st);
    REQUIRE(vertexCount(s) == vertexCount(fine));
    CHECK(edgeGap(edgeLine(s, 17, 1, fineX, z), edgeLine(coarse, 5, 3, coarseX, z)) < 1e-4f);

    // Only that edge moved; x/z, UVs and indices are the level's.
    for (int e : { 0, 2, 3 })
    {
        const auto a = edgeLine(s, 17, e, fineX, z), b = edgeLine(fine, 17, e, fineX, z);
        for (size_t p = 0; p < a.size(); ++p) CHECK(a[p].y == b[p].y);
    }
    for (size_t j = 1; j + 1 < 17; ++j)
        for (size_t i = 1; i + 1 < 17; ++i)
            CHECK(s.vertices[(j * 17 + i) * 3 + 1] == fine.vertices[(j * 17 + i) * 3 + 1]);
    for (size_t k = 0; k < vertexCount(s); ++k)
    {
        CHECK(s.vertices[k * 3 + 0] == fine.vertices[k * 3 + 0]);
        CHECK(s.vertices[k * 3 + 2] == fine.vertices[k * 3 + 2]);
    }
    CHECK(s.uvs == fine.uvs);
    CHECK(s.indices == fine.indices);

    // The skirt keeps its depth below the moved ring: right edge = ring 16..32.
    const size_t skirtBase = 17 * 17;
    for (size_t j = 0; j < 17; ++j)
    {
        const size_t ringK = 16 + j, grid = j * 17 + 16;
        const float depthBefore = fine.vertices[grid * 3 + 1] - fine.vertices[(skirtBase + ringK) * 3 + 1];
        const float depthAfter  = s.vertices[grid * 3 + 1]    - s.vertices[(skirtBase + ringK) * 3 + 1];
        CHECK(depthAfter == doctest::Approx(depthBefore).epsilon(1e-4));
    }

    // A count that does not fit, or a mesh of the wrong layout, changes nothing / gives nothing.
    TerrainChunkStitch odd; odd.edgeCells[1] = 3;
    CHECK(stitchTerrainChunkEdges(fine, 16, fine, 16, odd).vertices == fine.vertices);
    CHECK(stitchTerrainChunkEdges(fine, 8, fine, 16, st).vertices.empty());
}

TEST_CASE("A displaced refined edge is stitched onto its LOD0 neighbour, corners included")
{
    const uint32_t res = 17;
    const std::vector<float> h = bumpyField(res);
    std::vector<float> grey(16 * 16);
    for (size_t i = 0; i < grey.size(); ++i) grey[i] = static_cast<float>((i * 37) % 16) / 15.0f;
    TerrainDisplacementMap d;
    d.grey = grey.data(); d.width = 16; d.height = 16; d.strength = 0.4f; d.tiling = 3.0f;

    const float size = 30.0f;
    const uint32_t cells = 8, factor = 2, N = cells * factor + 1;
    const StaticMeshAsset lod0  = generateTerrainChunkMesh(h, res, size, size, 0.0f, 0.0f, 0.5f, 0.5f, cells + 1);
    const StaticMeshAsset tess  = generateTerrainChunkMeshTessellated(h, res, size, size,
                                      0.0f, 0.0f, 0.5f, 0.5f, N, 1.0f, d);
    const StaticMeshAsset right = generateTerrainChunkMesh(h, res, size, size, 0.5f, 0.0f, 1.0f, 0.5f, cells + 1);
    const float x = -7.5f, rx = 7.5f, z = -7.5f;

    CHECK(edgeGap(edgeLine(tess, N, 1, x, z), edgeLine(right, cells + 1, 3, rx, z)) > 0.05f);

    TerrainChunkStitch st;
    st.edgeCells[1] = cells;
    st.corners = 0b1001;   // the two corners on the u0 edge, which itself stays displaced
    const StaticMeshAsset s = stitchTerrainChunkEdges(tess, cells * factor, lod0, cells, st);
    REQUIRE(vertexCount(s) == vertexCount(tess));
    CHECK(edgeGap(edgeLine(s, N, 1, x, z), edgeLine(right, cells + 1, 3, rx, z)) < 1e-4f);
    // Pinned corners carry LOD0's height and normal.
    const size_t corner[2][2] = { { 0, 0 }, { (N - 1) * N, (cells) * (cells + 1) } };
    for (const auto& c : corner)
    {
        CHECK(s.vertices[c[0] * 3 + 1] == lod0.vertices[c[1] * 3 + 1]);
        for (int k = 0; k < 3; ++k) CHECK(s.normals[c[0] * 3 + k] == lod0.normals[c[1] * 3 + k]);
    }
    // Between them the u0 edge keeps its displacement.
    CHECK(edgeGap(edgeLine(s, N, 3, x, z), edgeLine(tess, N, 3, x, z)) > 0.0f);
    CHECK(s.vertices[(N / 2) * N * 3 + 1] == tess.vertices[(N / 2) * N * 3 + 1]);
}

TEST_CASE("Mixed-LOD seams of a landscape are closed by stitchSeams, same-LOD ones untouched")
{
    // lodDistanceScale 0.1 on 50 m chunks: LOD0 up to 30 m, LOD1 to 60, LOD2
    // to 120. From the middle of chunk (0,0) the four chunks draw LOD0, LOD1,
    // LOD1 and LOD2 — every seam is a mixed one.
    const glm::vec3 cam(-25.0f, 0.0f, -25.0f);
    auto build = [&](HorizonWorld& world, ContentManager& cm)
    {
        Entity te = makeLandscape(world, 1, 0.0f);
        world.registry().get<TerrainComponent>(te).lodDistanceScale = 0.1f;
        tick(world, cm, cam);
    };

    // Negative control: without the stitch the steps are there.
    {
        HorizonWorld world;
        ContentManager cm(".");
        build(world, cm);
        int mixed = 0;
        CHECK(worstSeam(world, cm, &mixed) > 0.01f);
        CHECK(mixed == 4);
    }

    HorizonWorld world;
    ContentManager cm(".");
    build(world, cm);
    TerrainSystem::stitchSeams(world, cm, nullptr);
    int mixed = 0;
    CHECK(worstSeam(world, cm, &mixed) < 1e-4f);
    CHECK(mixed == 4);

    auto& reg = world.registry();
    const entt::entity c00 = chunkAt(world, 0, 0), c11 = chunkAt(world, 1, 1);
    REQUIRE((c00 != entt::null));
    REQUIRE((c11 != entt::null));
    // The finest chunk draws its stitched copy, the coarsest its plain level.
    const auto& lod00 = reg.get<LODComponent>(c00);
    CHECK(lod00.current == 0);
    CHECK(lod00.stitchedMeshId != HE::UUID{});
    CHECK(reg.get<MeshComponent>(c00).meshAssetId == lod00.stitchedMeshId);
    CHECK(reg.get<LODComponent>(c11).current == 2);
    CHECK(reg.get<LODComponent>(c11).stitchedMeshId == HE::UUID{});

    // LODSystem keeps the copy; a second pass with nothing changed rebuilds nothing.
    const uint64_t key = reg.get<TerrainChunkComponent>(c00).stitchKey;
    const StaticMeshAsset* copy = cm.getStaticMesh(lod00.stitchedMeshId);
    LODSystem::update(world, cam);
    CHECK(reg.get<MeshComponent>(c00).meshAssetId == lod00.stitchedMeshId);
    TerrainSystem::stitchSeams(world, cm, nullptr);
    CHECK(reg.get<TerrainChunkComponent>(c00).stitchKey == key);
    CHECK(cm.getStaticMesh(lod00.stitchedMeshId) == copy);

    // Far away every chunk is LOD3: no mixed seam, back to the plain levels.
    const glm::vec3 far(5000.0f, 0.0f, 0.0f);
    tick(world, cm, far);
    TerrainSystem::stitchSeams(world, cm, nullptr);
    CHECK(worstSeam(world, cm, &mixed) < 1e-4f);
    CHECK(mixed == 0);
    for (auto [e, cc, lod, mc] : reg.view<TerrainChunkComponent, LODComponent, MeshComponent>().each())
    {
        CHECK(lod.stitchedMeshId == HE::UUID{});
        CHECK(mc.meshAssetId == lod.levels.back().meshId);
    }
}

TEST_CASE("A refined chunk next to LOD0 chunks is stitched; its mesh goes when the chunk does")
{
    HorizonWorld world;
    ContentManager cm(".");
    Entity te = makeLandscape(world, 2, 30.0f);
    const glm::vec3 cam(-25.0f, 5.0f, -25.0f);
    tick(world, cm, cam);
    tick(world, cm, cam);
    const entt::entity c00 = chunkAt(world, 0, 0);
    REQUIRE((c00 != entt::null));
    auto& reg = world.registry();
    REQUIRE(reg.get<LODComponent>(c00).current == LODComponent::kRefined);

    // Catmull-Rom against LOD0's straight segments: a (small) step unstitched.
    int mixed = 0;
    CHECK(worstSeam(world, cm, &mixed) > 1e-3f);
    CHECK(mixed == 2);

    TerrainSystem::stitchSeams(world, cm, nullptr);
    CHECK(worstSeam(world, cm) < 1e-4f);
    const HE::UUID stitched = reg.get<TerrainChunkComponent>(c00).stitchMeshId;
    REQUIRE(stitched != HE::UUID{});
    CHECK(reg.get<LODComponent>(c00).stitchedLevel == LODComponent::kRefined);
    CHECK(reg.get<MeshComponent>(c00).meshAssetId == stitched);

    // Sculpting rebuilds the chunk: it goes on drawing its refined level (as
    // the plain mesh until the next stitch), and the next pass stitches again.
    reg.get<TerrainComponent>(te).dirty = true;
    TerrainSystem::updateTerrains(world, cm);
    CHECK(reg.get<LODComponent>(c00).current == LODComponent::kRefined);
    CHECK(reg.get<TerrainChunkComponent>(c00).stitchKey == 0);
    TerrainSystem::stitchSeams(world, cm, nullptr);
    CHECK(reg.get<MeshComponent>(c00).meshAssetId == stitched);
    CHECK(worstSeam(world, cm) < 1e-4f);

    // A resolution change destroys the chunk grid: the stitched mesh is given back.
    reg.get<TerrainComponent>(te).resolution = 257;
    reg.get<TerrainComponent>(te).dirty = true;
    tick(world, cm, cam);
    TerrainSystem::stitchSeams(world, cm, nullptr);
    CHECK(cm.getStaticMesh(stitched) == nullptr);
}

// ── Seams under LOD changes and streaming (Thema 190, step 6) ─────────────────
// The cases above look at one camera. Here a camera flies over the terrain and
// every single tick is checked, so a frame in which LOD has switched a chunk but
// its neighbour's stitch is not rebuilt yet (or a refined level still waits for
// its build budget, or a sculpt just rebuilt the grid) shows up as a step.
// Terrain is Resident in cell streaming, so "while streaming" for a landscape
// means exactly these: levels arriving over several ticks, and rebuilds.

namespace
{
    // The terrain part of SceneSystems::tickWorld, in its order:
    // TerrainTess → LOD → TerrainSeams.
    void tickFull(HorizonWorld& world, ContentManager& cm, const glm::vec3& cam, bool stitch)
    {
        tick(world, cm, cam);
        if (stitch) TerrainSystem::stitchSeams(world, cm, nullptr);
    }

    struct SweepTerrain
    {
        const char* name;
        uint32_t    res;
        float       size, heightScale;
        int         factor;     // tessellation factor (1 = off)
        glm::vec3   pos;
    };

    Entity makeSweepLandscape(HorizonWorld& world, const SweepTerrain& t)
    {
        auto& reg = world.registry();
        Entity te = world.createEntity("Landscape");
        TransformComponent tr;
        tr.position = t.pos;
        reg.emplace<TransformComponent>(te, tr);
        TerrainComponent tc;
        tc.resolution  = t.res;
        tc.sizeX = tc.sizeZ = t.size;
        tc.seed        = 11;
        tc.heightScale = t.heightScale;
        tc.lodDistanceScale     = 0.1f;   // every LOD within a few chunk widths
        tc.tessellationFactor   = t.factor;
        tc.tessellationDistance = t.factor > 1 ? t.size * 0.3f : 0.0f;
        tc.dirty = true;
        reg.emplace<TerrainComponent>(te, tc);
        return te;
    }

    // From far outside in along one diagonal, low over the terrain, then back
    // over the other one — every chunk goes through every level both ways.
    glm::vec3 sweepCam(const SweepTerrain& t, int i, int steps)
    {
        const int   half = steps / 2;
        const float k    = static_cast<float>(i % half) / static_cast<float>(half - 1);
        const float a    = -2.0f * t.size + 4.0f * t.size * k;
        const float y    = t.heightScale + 2.0f;
        return t.pos + (i < half ? glm::vec3(a, y, a) : glm::vec3(-a, y, a));
    }

    struct SweepResult { float worst = 0.0f; int ticksMixed = 0, maxMixed = 0, ticksRefined = 0; };

    SweepResult runSweep(const SweepTerrain& t, int steps, bool stitch, float tol)
    {
        HorizonWorld world;
        ContentManager cm(".");
        Entity te = makeSweepLandscape(world, t);
        SweepResult r;
        for (int i = 0; i < steps; ++i)
        {
            // A third of the way in the terrain is sculpted: the whole chunk
            // grid is rebuilt in the middle of the flight.
            if (i == steps / 3) world.registry().get<TerrainComponent>(te).dirty = true;
            tickFull(world, cm, sweepCam(t, i, steps), stitch);
            int mixed = 0;
            const float w = worstSeam(world, cm, &mixed);
            r.worst = std::max(r.worst, w);
            if (mixed > 0) ++r.ticksMixed;
            r.maxMixed = std::max(r.maxMixed, mixed);
            for (auto [e, lod] : world.registry().view<LODComponent>().each())
                if (lod.current == LODComponent::kRefined) { ++r.ticksRefined; break; }
            if (stitch)
            {
                CAPTURE(i);
                CHECK(w <= tol);
            }
        }
        return r;
    }

    const SweepTerrain kSweepTerrains[] = {
        { "100 m, 2x2 chunks",              129,  100.0f, 10.0f, 1, glm::vec3(0.0f) },
        { "8 m, 2x2 chunks",                129,    8.0f,  0.8f, 1, glm::vec3(0.0f) },
        { "400 m, 4x4 chunks, off origin",  257,  400.0f, 40.0f, 1, glm::vec3(1000.0f, 50.0f, -300.0f) },
        { "1 km, 8x8 chunks",               513, 1000.0f, 80.0f, 1, glm::vec3(0.0f) },
        { "100 m, 2x2 chunks, tess x2",     129,  100.0f, 10.0f, 2, glm::vec3(0.0f) },
    };
}

TEST_CASE("Every tick of a camera flight keeps every terrain seam closed")
{
    const int steps = 160;
    for (const SweepTerrain& t : kSweepTerrains)
    {
        CAPTURE(std::string(t.name));
        // Relative to the relief: a small terrain's steps are far below any
        // absolute threshold that suits a big one.
        const float tol = 1e-5f * t.heightScale;

        const SweepResult on = runSweep(t, steps, true, tol);
        // The flight did what it is for: mixed seams in most ticks, and the
        // refined level where there is one.
        CHECK(on.ticksMixed > steps / 4);
        CHECK(on.maxMixed > 0);
        if (t.factor > 1) CHECK(on.ticksRefined > 0);
        MESSAGE(std::string(t.name) << ": worst stitched step " << on.worst << " m, "
                << on.ticksMixed << "/" << steps << " ticks with mixed seams (max "
                << on.maxMixed << ")");

        // Negative control: the same flight without stitchSeams has the steps.
        const SweepResult off = runSweep(t, steps, false, tol);
        CHECK(off.worst > 10.0f * tol);
        MESSAGE(std::string(t.name) << ": worst unstitched step " << off.worst << " m");
    }
}

TEST_CASE("A camera jittering across a LOD distance keeps the seams closed and reuses the stitched meshes")
{
    // LOD0 ends at 30 m on these 50 m chunks (lodDistanceScale 0.1). The camera
    // stands 30 m from chunk (1,1)'s centre and jitters by ±0.5 m across that
    // line, so the chunk flips LOD0 ↔ LOD1 every tick and its neighbours' seams
    // change from mixed to plain and back.
    HorizonWorld world;
    ContentManager cm(".");
    const SweepTerrain t = kSweepTerrains[0];
    makeSweepLandscape(world, t);
    auto& reg = world.registry();
    const glm::vec3 centre11(25.0f, 0.0f, 25.0f);
    const glm::vec3 dir = glm::normalize(glm::vec3(-1.0f, 0.0f, -0.2f));
    auto camAt = [&](float d) { return centre11 + dir * d; };

    tickFull(world, cm, camAt(29.5f), true);
    tickFull(world, cm, camAt(30.5f), true);
    std::vector<std::pair<entt::entity, HE::UUID>> ids;
    for (auto [e, cc] : reg.view<TerrainChunkComponent>().each())
        if (cc.stitchMeshId != HE::UUID{}) ids.push_back({ e, cc.stitchMeshId });

    const entt::entity c11 = chunkAt(world, 1, 1);
    REQUIRE((c11 != entt::null));
    int toggles = 0, rebuilds = 0;
    const int ticks = 40;
    for (int i = 0; i < ticks; ++i)
    {
        std::vector<uint64_t> before;
        for (auto [e, cc] : reg.view<TerrainChunkComponent>().each()) before.push_back(cc.stitchKey);
        const uint8_t was = reg.get<LODComponent>(c11).current;
        tickFull(world, cm, camAt(i % 2 ? 30.5f : 29.5f), true);
        if (reg.get<LODComponent>(c11).current != was) ++toggles;
        size_t k = 0;
        for (auto [e, cc] : reg.view<TerrainChunkComponent>().each())
            if (cc.stitchKey != before[k++] && cc.stitchKey != 0) ++rebuilds;
        CAPTURE(i);
        CHECK(worstSeam(world, cm) <= 1e-5f * t.heightScale);
    }
    CHECK(toggles == ticks);
    // A rebuild refills the chunk's one stitched mesh in place: no new asset
    // per flip, nothing piles up in the ContentManager.
    for (const auto& [e, id] : ids)
        CHECK(reg.get<TerrainChunkComponent>(e).stitchMeshId == id);
    // Known cost (see the fix doc): every flip re-stitches the chunks whose
    // neighbour changed — at most the flipping chunk and its four neighbours.
    CHECK(rebuilds <= 5 * toggles);
    MESSAGE("jitter: " << toggles << " LOD flips in " << ticks << " ticks, "
            << rebuilds << " stitched-mesh rebuilds");

    // Standing still (where the last jitter tick left the camera) costs nothing.
    int still = 0;
    for (int i = 0; i < 10; ++i)
    {
        std::vector<uint64_t> before;
        for (auto [e, cc] : reg.view<TerrainChunkComponent>().each()) before.push_back(cc.stitchKey);
        tickFull(world, cm, camAt(ticks % 2 ? 29.5f : 30.5f), true);
        size_t k = 0;
        for (auto [e, cc] : reg.view<TerrainChunkComponent>().each())
            if (cc.stitchKey != before[k++]) ++still;
    }
    CHECK(still == 0);
}
