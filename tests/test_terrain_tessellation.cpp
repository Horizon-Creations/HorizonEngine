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
    REQUIRE(lod.levels.size() == 5);
    CHECK(lod.levels.front().meshId == cc.tessMeshId);
    CHECK(lod.levels.front().maxDistance == doctest::Approx(30.0f));
    CHECK(reg.get<MeshComponent>(near).meshAssetId == cc.tessMeshId);
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

    // Far away: all four go, their LOD chains are the regular four again,
    // and the meshes are emptied — but stay registered, so coming back is a
    // replace rather than a new registration.
    tick(world, cm, glm::vec3(5000.0f, 5.0f, 0.0f));
    CHECK(countTess(world).active == 0);
    for (auto [e, cc, lod, mc] :
         world.registry().view<TerrainChunkComponent, LODComponent, MeshComponent>().each())
    {
        CHECK(lod.levels.size() == 4);
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

    // Same UUID, still in front of the chain, already at the new height —
    // before updateTessellation has run again.
    const auto& cc = reg.get<TerrainChunkComponent>(near);
    CHECK(cc.tessActive);
    CHECK(cc.tessMeshId == id);
    CHECK(reg.get<LODComponent>(near).levels.size() == 5);
    const StaticMeshAsset* m = cm.getStaticMesh(id);
    REQUIRE(m != nullptr);
    REQUIRE(m->vertices.size() > 1);
    CHECK(m->vertices[1] == doctest::Approx(42.0f));

    // Switching tessellation off on a rebuild gives it back.
    tc.tessellationFactor = 1;
    tc.dirty = true;
    tick(world, cm, cam);
    CHECK(countTess(world).active == 0);
    CHECK(reg.get<LODComponent>(near).levels.size() == 4);
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
