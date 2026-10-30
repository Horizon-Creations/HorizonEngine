#include "doctest.h"
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/TerrainMeshGenerator.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/TerrainSystem.h>
#include <HorizonScene/TerrainPaint.h>
#include <HorizonScene/TerrainSculpt.h>
#include <HorizonScene/Components/TerrainChunkComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/PhysicsWorld.h>
#include <ContentManager/ContentManager.h>
#include <MaterialGraph/MaterialGraph.h>
#include <MaterialGraph/AutoLandscapeMaterial.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>

// ── Geometry correctness ───────────────────────────────────────────────────────

TEST_CASE("generateTerrainMesh vertex count matches resolution²")
{
    TerrainComponent tc;
    tc.resolution = 16;
    const StaticMeshAsset mesh = generateTerrainMesh(tc);

    const size_t expected = static_cast<size_t>(16) * 16;
    CHECK(mesh.vertices.size() == expected * 3); // x/y/z per vertex
    CHECK(mesh.normals .size() == expected * 3);
    CHECK(mesh.uvs     .size() == expected * 2);
}

TEST_CASE("generateTerrainMesh index count matches (res-1)² × 6")
{
    TerrainComponent tc;
    tc.resolution = 16;
    const StaticMeshAsset mesh = generateTerrainMesh(tc);

    const size_t cells = static_cast<size_t>(15) * 15;
    CHECK(mesh.indices.size() == cells * 6);
}

TEST_CASE("generateTerrainMesh resolution clamp: < 2 yields 2×2 grid")
{
    TerrainComponent tc;
    tc.resolution = 0;
    const StaticMeshAsset mesh = generateTerrainMesh(tc);
    CHECK(mesh.vertices.size() == 4 * 3);
    CHECK(mesh.indices .size() == 6);
}

TEST_CASE("generateTerrainMesh XZ extents match sizeX / sizeZ")
{
    TerrainComponent tc;
    tc.sizeX      = 80.0f;
    tc.sizeZ      = 50.0f;
    tc.resolution = 8;
    tc.heightScale= 0.0f; // flat — pure XZ geometry

    const StaticMeshAsset mesh = generateTerrainMesh(tc);

    float minX =  1e9f, maxX = -1e9f;
    float minZ =  1e9f, maxZ = -1e9f;
    for (size_t i = 0; i < mesh.vertices.size(); i += 3)
    {
        minX = std::min(minX, mesh.vertices[i]);
        maxX = std::max(maxX, mesh.vertices[i]);
        minZ = std::min(minZ, mesh.vertices[i + 2]);
        maxZ = std::max(maxZ, mesh.vertices[i + 2]);
    }
    CHECK(minX == doctest::Approx(-40.0f));
    CHECK(maxX == doctest::Approx( 40.0f));
    CHECK(minZ == doctest::Approx(-25.0f));
    CHECK(maxZ == doctest::Approx( 25.0f));
}

TEST_CASE("generateTerrainMesh height is in [0, heightScale]")
{
    TerrainComponent tc;
    tc.resolution  = 32;
    tc.heightScale = 15.0f;
    const StaticMeshAsset mesh = generateTerrainMesh(tc);

    for (size_t i = 1; i < mesh.vertices.size(); i += 3) // Y components
    {
        CHECK(mesh.vertices[i] >= -0.001f);
        CHECK(mesh.vertices[i] <= tc.heightScale + 0.001f);
    }
}

TEST_CASE("generateTerrainMesh normals are unit-length")
{
    TerrainComponent tc;
    tc.resolution = 8;
    const StaticMeshAsset mesh = generateTerrainMesh(tc);

    for (size_t i = 0; i < mesh.normals.size(); i += 3)
    {
        const float nx = mesh.normals[i];
        const float ny = mesh.normals[i + 1];
        const float nz = mesh.normals[i + 2];
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        CHECK(len == doctest::Approx(1.0f).epsilon(0.001f));
    }
}

TEST_CASE("generateTerrainMesh flat terrain normals point +Y")
{
    TerrainComponent tc;
    tc.resolution  = 4;
    tc.heightScale = 0.0f; // flat → all normals exactly (0, 1, 0)
    const StaticMeshAsset mesh = generateTerrainMesh(tc);

    for (size_t i = 0; i < mesh.normals.size(); i += 3)
    {
        CHECK(mesh.normals[i    ] == doctest::Approx(0.0f).epsilon(0.001f));
        CHECK(mesh.normals[i + 1] == doctest::Approx(1.0f).epsilon(0.001f));
        CHECK(mesh.normals[i + 2] == doctest::Approx(0.0f).epsilon(0.001f));
    }
}

// ── Determinism ────────────────────────────────────────────────────────────────

TEST_CASE("generateTerrainMesh same seed produces identical heights")
{
    TerrainComponent tc;
    tc.seed       = 999;
    tc.resolution = 16;

    const StaticMeshAsset a = generateTerrainMesh(tc);
    const StaticMeshAsset b = generateTerrainMesh(tc);

    REQUIRE(a.vertices.size() == b.vertices.size());
    for (size_t i = 0; i < a.vertices.size(); ++i)
        CHECK(a.vertices[i] == doctest::Approx(b.vertices[i]));
}

TEST_CASE("generateTerrainMesh different seeds produce different heights")
{
    TerrainComponent ta;
    ta.seed = 1; ta.resolution = 16; ta.heightScale = 10.0f;

    TerrainComponent tb;
    tb.seed = 2; tb.resolution = 16; tb.heightScale = 10.0f;

    const StaticMeshAsset a = generateTerrainMesh(ta);
    const StaticMeshAsset b = generateTerrainMesh(tb);

    // At least one Y value must differ
    bool anyDiff = false;
    for (size_t i = 1; i < a.vertices.size(); i += 3)
        if (std::abs(a.vertices[i] - b.vertices[i]) > 1e-4f) { anyDiff = true; break; }
    CHECK(anyDiff);
}

// ── Serialisation round-trip ───────────────────────────────────────────────────

TEST_CASE("TerrainComponent serialises and round-trips via SceneSerializer")
{
    HorizonWorld world;
    auto& registry = world.registry();

    // Attach a TerrainComponent with non-default values to a new entity
    Entity e = world.createEntity("myTerrain");
    TerrainComponent tc;
    tc.sizeX       = 200.0f;
    tc.sizeZ       = 150.0f;
    tc.resolution  = 64;
    tc.heightScale = 30.0f;
    tc.seed        = 7777;
    tc.octaves     = 6;
    tc.frequency   = 2.5f;
    tc.lacunarity  = 1.8f;
    tc.gain        = 0.4f;
    tc.dirty       = false; // intentionally false — must come back true after load
    registry.emplace<TerrainComponent>(e, tc);

    // Round-trip through memory
    SceneSerializer ser;
    std::vector<uint8_t> bytes;
    REQUIRE(ser.saveToMemory(world, bytes));

    HorizonWorld world2;
    REQUIRE(ser.loadFromMemory(world2, bytes));

    // Find the terrain entity in the restored world
    const TerrainComponent* loaded = nullptr;
    for (auto ent : world2.registry().view<TerrainComponent>())
    {
        if (const auto* n = world2.registry().try_get<NameComponent>(ent))
            if (n->name == "myTerrain")
                loaded = &world2.registry().get<TerrainComponent>(ent);
    }
    REQUIRE(loaded != nullptr);

    CHECK(loaded->sizeX       == doctest::Approx(200.0f));
    CHECK(loaded->sizeZ       == doctest::Approx(150.0f));
    CHECK(loaded->resolution  == 64u);
    CHECK(loaded->heightScale == doctest::Approx(30.0f));
    CHECK(loaded->seed        == 7777);
    CHECK(loaded->octaves     == 6);
    CHECK(loaded->frequency   == doctest::Approx(2.5f));
    CHECK(loaded->lacunarity  == doctest::Approx(1.8f));
    CHECK(loaded->gain        == doctest::Approx(0.4f));
    // dirty must always be true after a load so TerrainSystem regenerates
    CHECK(loaded->dirty == true);

    // MeshComponent must NOT have been serialised for a terrain entity
    // (no mesh key in the JSON means no MeshComponent was created on load)
    CHECK(world2.registry().try_get<MeshComponent>(
        world2.registry().view<TerrainComponent>().front()) == nullptr);
}

// ── Chunked / LOD terrain ──────────────────────────────────────────────────────

TEST_CASE("computeTerrainHeightField is res*res and flat for seed 0")
{
    TerrainComponent tc;
    tc.resolution = 32;
    tc.seed = 0;                       // flat
    const std::vector<float> h = computeTerrainHeightField(tc);
    CHECK(h.size() == static_cast<size_t>(32) * 32);
    bool allZero = true;
    for (float v : h) if (v != 0.0f) { allZero = false; break; }
    CHECK(allZero);
}

TEST_CASE("generateTerrainChunkMesh vertex/index counts include the skirt")
{
    const uint32_t res = 33;
    std::vector<float> field(static_cast<size_t>(res) * res, 0.0f);
    const uint32_t N = 5; // vertsPerSide
    StaticMeshAsset m = generateTerrainChunkMesh(field, res, 100.f, 100.f,
                                                 0.0f, 0.0f, 0.5f, 0.5f, N);
    const size_t ring   = static_cast<size_t>(4) * (N - 1);  // perimeter verts
    const size_t verts  = static_cast<size_t>(N) * N + ring; // grid + skirt
    CHECK(m.vertices.size() == verts * 3);
    CHECK(m.normals .size() == verts * 3);
    CHECK(m.uvs     .size() == verts * 2);
    const size_t gridIdx  = static_cast<size_t>(N - 1) * (N - 1) * 6;
    const size_t skirtIdx = ring * 6;                        // one-sided: 2 tris per segment
    CHECK(m.indices.size() == gridIdx + skirtIdx);
}

TEST_CASE("generateTerrainChunkMesh samples the height field (flat → grid at y=0)")
{
    const uint32_t res = 17;
    std::vector<float> field(static_cast<size_t>(res) * res, 0.0f);
    const uint32_t N = 4;
    StaticMeshAsset m = generateTerrainChunkMesh(field, res, 50.f, 50.f,
                                                 0.0f, 0.0f, 1.0f, 1.0f, N);
    // The N*N grid vertices (the first N*N) sit on the flat field → y == 0.
    for (uint32_t i = 0; i < N * N; ++i)
        CHECK(m.vertices[i * 3 + 1] == doctest::Approx(0.0f));
    // Skirt vertices (after the grid) are pushed below 0.
    bool skirtBelow = true;
    for (size_t i = static_cast<size_t>(N) * N; i < m.vertices.size() / 3; ++i)
        if (m.vertices[i * 3 + 1] >= 0.0f) { skirtBelow = false; break; }
    CHECK(skirtBelow);
}

TEST_CASE("Adjacent chunks share boundary height + normal (no seam) at same LOD")
{
    // A non-trivial field so normals vary.
    const uint32_t res = 33;
    std::vector<float> field(static_cast<size_t>(res) * res);
    for (uint32_t z = 0; z < res; ++z)
        for (uint32_t x = 0; x < res; ++x)
            field[z * res + x] = static_cast<float>(x) * 0.5f + static_cast<float>(z) * 0.25f;

    const uint32_t N = 5;
    // Left chunk covers u in [0,0.5], right chunk u in [0.5,1] — they meet at u=0.5.
    StaticMeshAsset L = generateTerrainChunkMesh(field, res, 80.f, 60.f, 0.0f, 0.0f, 0.5f, 1.0f, N);
    StaticMeshAsset R = generateTerrainChunkMesh(field, res, 80.f, 60.f, 0.5f, 0.0f, 1.0f, 1.0f, N);

    // L's right column (i=N-1) and R's left column (i=0), row by row: same world Y
    // and same normal (both sampled from the global field at the shared u=0.5).
    for (uint32_t j = 0; j < N; ++j)
    {
        const uint32_t li = j * N + (N - 1);
        const uint32_t ri = j * N + 0;
        CHECK(L.vertices[li * 3 + 1] == doctest::Approx(R.vertices[ri * 3 + 1])); // height
        CHECK(L.normals [li * 3 + 0] == doctest::Approx(R.normals [ri * 3 + 0])); // nx
        CHECK(L.normals [li * 3 + 1] == doctest::Approx(R.normals [ri * 3 + 1])); // ny
        CHECK(L.normals [li * 3 + 2] == doctest::Approx(R.normals [ri * 3 + 2])); // nz
    }
}

TEST_CASE("generateTerrainChunkMeshLOD's cached downsample is bit-identical to resampling the field")
{
    // A non-flat, non-linear field (not a seeded fBm — just varied enough that a bilinear
    // resample and a stride-2 downsample would disagree if the LOD cache's indexing were off
    // by even one cell) so a false-positive match is unlikely.
    const uint32_t res = 257;
    std::vector<float> field(static_cast<size_t>(res) * res);
    for (uint32_t z = 0; z < res; ++z)
        for (uint32_t x = 0; x < res; ++x)
        {
            const float fx = static_cast<float>(x), fz = static_cast<float>(z);
            field[static_cast<size_t>(z) * res + x] =
                3.0f * std::sin(fx * 0.37f) + 2.0f * std::cos(fz * 0.21f) + 0.05f * fx * fz / res;
        }

    const uint32_t lod0Cells = 64;   // verts = lod0Cells>>k + 1 → 65, 33, 17, 9
    const float sizeX = 200.0f, sizeZ = 150.0f;
    struct Chunk { float u0, v0, u1, v1; };
    // One interior chunk, one that hits the u1 = v1 = 1 clamp edge TerrainSystem::buildChunk's
    // last chunk in each row/column always lands on, and one with a span that is NOT a power of
    // two itself (1/3, 1/7 are not exactly representable in binary floating point) — a span of
    // 0.25 or 1.0 would make `(u1-u0)*i` land on a round number for almost any stride, which
    // would pass even if the power-of-two guard were accidentally relaxed to allow more strides.
    const Chunk chunks[] = {
        { 0.25f, 0.0f, 0.5f, 0.25f },
        { 0.75f, 0.75f, 1.0f, 1.0f },
        { 1.0f / 3.0f, 1.0f / 7.0f, 2.0f / 3.0f, 4.0f / 7.0f },
    };

    for (const Chunk& c : chunks)
    {
        TerrainChunkLODCache cache;
        for (uint32_t k = 0; k < 4; ++k)
        {
            const uint32_t verts = (lod0Cells >> k) + 1u;
            const StaticMeshAsset viaCache = generateTerrainChunkMeshLOD(
                field, res, sizeX, sizeZ, c.u0, c.v0, c.u1, c.v1, verts, 1.0f, cache);
            const StaticMeshAsset direct = generateTerrainChunkMesh(
                field, res, sizeX, sizeZ, c.u0, c.v0, c.u1, c.v1, verts, 1.0f);

            // Confirms the CACHED (downsample) path actually ran, not a silent fallback to the
            // resample path that would make this test pass for the wrong reason: the cache
            // stays at the finest level's size (65) across every coarser call, it is never
            // replaced by a smaller grid (see generateTerrainChunkMeshLOD).
            CHECK(cache.n == lod0Cells + 1);

            CHECK(viaCache.vertices == direct.vertices);
            CHECK(viaCache.normals  == direct.normals);
            CHECK(viaCache.uvs      == direct.uvs);
            CHECK(viaCache.indices  == direct.indices);
        }
    }
}

TEST_CASE("generateTerrainChunkMeshLOD falls back to resampling when the stride is not a power of two")
{
    const uint32_t res = 65;
    std::vector<float> field(static_cast<size_t>(res) * res);
    for (uint32_t z = 0; z < res; ++z)
        for (uint32_t x = 0; x < res; ++x)
            field[static_cast<size_t>(z) * res + x] =
                static_cast<float>(x) * 0.3f - static_cast<float>(z) * 0.2f;

    const float sizeX = 100.0f, sizeZ = 100.0f;
    // A span that is not an exact binary fraction (0.1, 0.7), so `(u1-u0)*i` does not land on a
    // round number for stride 3 either — a test with a span like 0.0..1.0 would not actually
    // exercise the rounding mismatch the power-of-two guard exists for.
    const float u0 = 0.1f, u1 = 0.7f, v0 = 0.1f, v1 = 0.7f;
    TerrainChunkLODCache cache;
    // Seed the cache with a 49-vertex grid — outside the usual 65/33/17/9 sequence.
    const StaticMeshAsset base = generateTerrainChunkMeshLOD(
        field, res, sizeX, sizeZ, u0, v0, u1, v1, 49, 1.0f, cache);
    REQUIRE(cache.n == 49);

    // (49-1)/(17-1) = 48/16 = 3 — an exact stride, but not a power of two, so this must fall
    // back to resampling the field directly instead of downsampling the (wrong-stride) cache.
    const StaticMeshAsset viaCache = generateTerrainChunkMeshLOD(
        field, res, sizeX, sizeZ, u0, v0, u1, v1, 17, 1.0f, cache);
    const StaticMeshAsset direct = generateTerrainChunkMesh(
        field, res, sizeX, sizeZ, u0, v0, u1, v1, 17, 1.0f);

    // Confirms the fallback actually ran (and refreshed the cache to the new, smaller grid) —
    // without this, the test above would also pass if strideOf were broken and the call had
    // silently (and in this case harmlessly, by luck) downsampled the 49-vertex cache instead.
    CHECK(cache.n == 17);

    CHECK(viaCache.vertices == direct.vertices);
    CHECK(viaCache.normals  == direct.normals);
    CHECK(viaCache.uvs      == direct.uvs);
    CHECK(viaCache.indices  == direct.indices);
    (void)base;
}

TEST_CASE("resampleHeightField preserves corners and linear fields")
{
    // A linear ramp on a 3×3 grid: value = x (columns 0,1,2).
    const uint32_t oldRes = 3;
    std::vector<float> src = { 0,1,2,  0,1,2,  0,1,2 };
    std::vector<float> dst = resampleHeightField(src, oldRes, 5);
    REQUIRE(dst.size() == 25);
    // Corners preserved exactly.
    CHECK(dst[0]            == doctest::Approx(0.0f)); // (0,0)
    CHECK(dst[4]            == doctest::Approx(2.0f)); // (4,0) → x=max → 2
    CHECK(dst[24]           == doctest::Approx(2.0f)); // (4,4)
    // Linear interior: column x=2 of 5 → u=0.5 → value 1.0.
    CHECK(dst[2]            == doctest::Approx(1.0f));
    // Same-resolution resample is a no-op.
    std::vector<float> same = resampleHeightField(src, oldRes, oldRes);
    CHECK(same == src);
}

TEST_CASE("TerrainComponent sculptHeights survive save/load (base64 round-trip)")
{
    HorizonWorld world;
    auto& reg = world.registry();
    Entity e = world.createEntity("sculptTerrain");
    TerrainComponent tc;
    tc.resolution = 16;
    // Distinct per-vertex values incl. negatives + fractions to catch any corruption.
    tc.sculptHeights.resize(16u * 16u);
    for (size_t i = 0; i < tc.sculptHeights.size(); ++i)
        tc.sculptHeights[i] = static_cast<float>(i) * 0.5f - 31.7f;
    reg.emplace<TerrainComponent>(e, tc);

    SceneSerializer ser;
    std::vector<uint8_t> bytes;                 // saveToMemory uses CBOR (the undo path)
    REQUIRE(ser.saveToMemory(world, bytes));
    HorizonWorld world2;
    REQUIRE(ser.loadFromMemory(world2, bytes));

    const TerrainComponent* loaded = nullptr;
    for (auto ent : world2.registry().view<TerrainComponent>())
        loaded = &world2.registry().get<TerrainComponent>(ent);
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->sculptHeights.size() == tc.sculptHeights.size());
    for (size_t i = 0; i < tc.sculptHeights.size(); ++i)
        CHECK(loaded->sculptHeights[i] == doctest::Approx(tc.sculptHeights[i]));
}

// ── Chunk material follows the Landscape entity ────────────────────────────────
// The Landscape entity itself has no mesh — its generated chunk children are what
// render. Chunks used to be pinned to the built-in terrain material at creation,
// so assigning (or recolouring) a material on the Landscape never reached screen.
TEST_CASE("TerrainSystem propagates the Landscape's material to its chunks")
{
    HorizonWorld world;
    ContentManager cm(".");
    auto& reg = world.registry();

    Entity te = world.createEntity("Landscape");
    reg.emplace<TransformComponent>(te);
    TerrainComponent tc;
    tc.resolution = 9;      // small: 2ⁿ+1 already, no resample
    tc.sizeX = tc.sizeZ = 16.0f;
    tc.dirty = true;
    reg.emplace<TerrainComponent>(te, tc);

    // First tick builds the chunk entities.
    TerrainSystem::updateTerrains(world, cm);
    size_t chunks = 0;
    for (auto ce : reg.view<TerrainChunkComponent>()) { (void)ce; ++chunks; }
    REQUIRE(chunks > 0);

    // Assign a different material to the Landscape (no heightfield change → the
    // terrain is NOT dirty, so the sync must not be gated on the rebuild path).
    const HE::UUID custom{ 0xABCDEF01ull, 0x1234ull };
    reg.get<MaterialComponent>(te).materialAssetId = custom;
    TerrainSystem::updateTerrains(world, cm);

    for (auto [ce, tcc, mc] : reg.view<TerrainChunkComponent, MaterialComponent>().each())
        CHECK(mc.materialAssetId == custom);

    // Per-entity param overrides ride along too.
    MaterialParamOverride ov; ov.name = "Tint"; ov.value[0] = 0.25f;
    reg.get<MaterialComponent>(te).paramOverrides = { ov };
    TerrainSystem::updateTerrains(world, cm);
    for (auto [ce, tcc, mc] : reg.view<TerrainChunkComponent, MaterialComponent>().each())
    {
        REQUIRE(mc.paramOverrides.size() == 1);
        CHECK(mc.paramOverrides[0].name == "Tint");
        CHECK(mc.paramOverrides[0].value[0] == doctest::Approx(0.25f));
    }
}

// One pass over the chunks serves every landscape (SCN-802): each chunk must
// still follow its own.
TEST_CASE("TerrainSystem: chunks of two landscapes each follow their own landscape's material")
{
    HorizonWorld world;
    ContentManager cm(".");
    auto& reg = world.registry();

    Entity tes[2];
    for (int i = 0; i < 2; ++i)
    {
        tes[i] = world.createEntity("Landscape");
        reg.emplace<TransformComponent>(tes[i]);
        TerrainComponent tc;
        tc.resolution = 129;   // 2×2 chunks
        tc.sizeX = tc.sizeZ = 16.0f;
        tc.dirty = true;
        reg.emplace<TerrainComponent>(tes[i], tc);
    }
    TerrainSystem::updateTerrains(world, cm);

    const HE::UUID mats[2] = { HE::UUID{ 0xA1ull, 0x1ull }, HE::UUID{ 0xB2ull, 0x2ull } };
    for (int i = 0; i < 2; ++i)
    {
        auto& m = reg.get<MaterialComponent>(tes[i]);
        m.materialAssetId = mats[i];
        MaterialParamOverride ov; ov.name = i == 0 ? "Tint" : "Wetness"; ov.value[0] = 0.25f + i;
        m.paramOverrides = { ov };
    }
    // A chunk that lost its MaterialComponent gets the one of its own landscape.
    entt::entity bare = entt::null;
    for (auto [ce, cc] : reg.view<TerrainChunkComponent>().each())
        if (cc.terrain == tes[1]) { bare = ce; break; }
    REQUIRE((bare != entt::null));
    reg.remove<MaterialComponent>(bare);

    TerrainSystem::updateTerrains(world, cm);
    size_t seen[2] = { 0, 0 };
    for (auto [ce, cc] : reg.view<TerrainChunkComponent>().each())
    {
        const int i = cc.terrain == tes[0] ? 0 : 1;
        REQUIRE((cc.terrain == tes[i]));
        const auto* mc = reg.try_get<MaterialComponent>(ce);
        REQUIRE(mc != nullptr);
        CHECK(mc->materialAssetId == mats[i]);
        REQUIRE(mc->paramOverrides.size() == 1);
        CHECK(mc->paramOverrides[0].name == (i == 0 ? "Tint" : "Wetness"));
        CHECK(mc->paramOverrides[0].value[0] == doctest::Approx(0.25f + i));
        ++seen[i];
    }
    CHECK(seen[0] == 4);
    CHECK(seen[1] == 4);
}

// ── Texture tiling ────────────────────────────────────────────────────────────
// Terrain UVs run 0..1 across the WHOLE landscape, so a texture was stretched
// over the entire terrain instead of tiling.
TEST_CASE("Terrain UVs scale with uvTiling and stay continuous across chunks")
{
    TerrainComponent tc;
    tc.resolution = 9;
    tc.sizeX = tc.sizeZ = 64.0f;

    // Default is the historical 0..1 range (no behaviour change on old scenes).
    CHECK(tc.uvTiling == doctest::Approx(1.0f));
    {
        const StaticMeshAsset m = generateTerrainMesh(tc);
        float mx = 0.0f;
        for (size_t i = 0; i < m.uvs.size(); ++i) mx = std::max(mx, m.uvs[i]);
        CHECK(mx == doctest::Approx(1.0f));
    }
    tc.uvTiling = 16.0f;
    {
        const StaticMeshAsset m = generateTerrainMesh(tc);
        float mx = 0.0f;
        for (size_t i = 0; i < m.uvs.size(); ++i) mx = std::max(mx, m.uvs[i]);
        CHECK(mx == doctest::Approx(16.0f));
    }

    // Chunk meshes use GLOBAL uvs, so neighbouring chunks continue the pattern:
    // chunk (0,0) of a 2x2 grid ends exactly where chunk (1,0) begins.
    const std::vector<float> field = computeTerrainHeightField(tc);
    const StaticMeshAsset a = generateTerrainChunkMesh(field, tc.resolution, tc.sizeX, tc.sizeZ,
                                                       0.0f, 0.0f, 0.5f, 1.0f, 5, tc.uvTiling);
    const StaticMeshAsset b = generateTerrainChunkMesh(field, tc.resolution, tc.sizeX, tc.sizeZ,
                                                       0.5f, 0.0f, 1.0f, 1.0f, 5, tc.uvTiling);
    float aMaxU = 0.0f, bMinU = 1e30f;
    for (size_t i = 0; i < a.uvs.size(); i += 2) aMaxU = std::max(aMaxU, a.uvs[i]);
    for (size_t i = 0; i < b.uvs.size(); i += 2) bMinU = std::min(bMinU, b.uvs[i]);
    CHECK(aMaxU == doctest::Approx(8.0f));   // 0.5 * 16
    CHECK(bMinU == doctest::Approx(8.0f));   // seam matches → no visible repeat break
}

TEST_CASE("TerrainComponent uvTiling and lodDistanceScale round-trip")
{
    HorizonWorld world;
    Entity e = world.createEntity("t");
    TerrainComponent tc;
    tc.uvTiling = 12.5f;
    tc.lodDistanceScale = 3.25f;   // was editable but never persisted
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
    CHECK(l->uvTiling == doctest::Approx(12.5f));
    CHECK(l->lodDistanceScale == doctest::Approx(3.25f));
}

// ── Layer painting ────────────────────────────────────────────────────────────
TEST_CASE("TerrainPaint allocates a weightmap that is fully layer 0")
{
    TerrainComponent tc;
    tc.weightRes = 8;
    TerrainPaint::ensureWeightmap(tc);
    REQUIRE(tc.layerWeights.size() == 8u * 8u * 4u);
    for (size_t i = 0; i < tc.layerWeights.size(); i += 4)
    {
        CHECK(tc.layerWeights[i + 0] == 255); // layer 0 …
        CHECK(tc.layerWeights[i + 1] == 0);   // … and nothing else, which is what
        CHECK(tc.layerWeights[i + 2] == 0);   // an unpainted landscape already
        CHECK(tc.layerWeights[i + 3] == 0);   // renders as (1x1 default = 1,0,0,0)
    }
    CHECK(tc.weightsDirty);
    // Idempotent: a correctly sized map is left alone.
    tc.layerWeights[0] = 42;
    TerrainPaint::ensureWeightmap(tc);
    CHECK(tc.layerWeights[0] == 42);
}

TEST_CASE("TerrainPaint moves weight toward the painted layer and keeps the sum at 255")
{
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 64.0f;
    tc.weightRes = 32;
    TerrainPaint::ensureWeightmap(tc);

    // Paint layer 1 at the centre with a hard-edged brush.
    REQUIRE(TerrainPaint::paint(tc, 0.0f, 0.0f, /*layer=*/1,
                                /*radius=*/8.0f, /*falloff=*/0.0f, /*strength=*/0.5f));

    const uint32_t wr = tc.weightRes;
    auto texel = [&](int x, int z) { return &tc.layerWeights[(static_cast<size_t>(z) * wr + x) * 4]; };
    const int c = static_cast<int>(wr) / 2;

    // Centre moved halfway to layer 1 …
    const uint8_t* mid = texel(c, c);
    CHECK(mid[1] > 100);
    CHECK(mid[1] < 160);
    CHECK(mid[0] > 100);
    // … and every touched texel still sums to exactly 255 (no drift over strokes).
    for (size_t i = 0; i < tc.layerWeights.size(); i += 4)
    {
        const int sum = tc.layerWeights[i] + tc.layerWeights[i+1]
                      + tc.layerWeights[i+2] + tc.layerWeights[i+3];
        CHECK(sum == 255);
    }
    // A corner far outside the brush is untouched.
    const uint8_t* corner = texel(0, 0);
    CHECK(corner[0] == 255);
    CHECK(corner[1] == 0);

    // Repeated strokes converge on the layer instead of overshooting.
    for (int i = 0; i < 12; ++i)
        TerrainPaint::paint(tc, 0.0f, 0.0f, 1, 8.0f, 0.0f, 0.5f);
    CHECK(texel(c, c)[1] == 255);
    CHECK(texel(c, c)[0] == 0);

    // Painting a different layer over it takes the weight back — reversible.
    for (int i = 0; i < 12; ++i)
        TerrainPaint::paint(tc, 0.0f, 0.0f, 0, 8.0f, 0.0f, 0.5f);
    CHECK(texel(c, c)[0] == 255);
    CHECK(texel(c, c)[1] == 0);
}

TEST_CASE("TerrainPaint::fillLayer puts every texel on one layer, on either page")
{
    TerrainComponent tc;
    tc.sizeX = 40.0f; tc.sizeZ = 40.0f; tc.weightRes = 32;

    REQUIRE(TerrainPaint::fillLayer(tc, 2));
    REQUIRE(tc.layerWeights.size() == 32u * 32u * 4u);
    CHECK(tc.weightsDirty);
    uint8_t w[kTerrainMaxLayers];
    for (uint32_t z : { 0u, 17u, 31u })
        for (uint32_t x : { 0u, 9u, 31u })
        {
            REQUIRE(TerrainPaint::texelWeights(tc, x, z, w));
            for (int k = 0; k < kTerrainMaxLayers; ++k) CHECK(w[k] == (k == 2 ? 255 : 0));
        }
    CHECK_FALSE(TerrainPaint::usesSecondPage(tc));

    // A layer on the second page: page one goes to zero, the second page carries it.
    REQUIRE(TerrainPaint::fillLayer(tc, 5));
    REQUIRE(TerrainPaint::texelWeights(tc, 3, 4, w));
    for (int k = 0; k < kTerrainMaxLayers; ++k) CHECK(w[k] == (k == 5 ? 255 : 0));
    CHECK(TerrainPaint::usesSecondPage(tc));

    // And back: a first-page fill must not leave the second page voting.
    REQUIRE(TerrainPaint::fillLayer(tc, 0));
    REQUIRE(TerrainPaint::texelWeights(tc, 3, 4, w));
    for (int k = 0; k < kTerrainMaxLayers; ++k) CHECK(w[k] == (k == 0 ? 255 : 0));
    CHECK_FALSE(TerrainPaint::usesSecondPage(tc));

    CHECK_FALSE(TerrainPaint::fillLayer(tc, -1));
    CHECK_FALSE(TerrainPaint::fillLayer(tc, kTerrainMaxLayers));
}

TEST_CASE("TerrainPaint falloff fades with distance and rejects bad layers")
{
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 64.0f;
    tc.weightRes = 64;
    TerrainPaint::ensureWeightmap(tc);
    REQUIRE(TerrainPaint::paint(tc, 0.0f, 0.0f, 2, /*radius=*/4.0f,
                                /*falloff=*/12.0f, /*strength=*/1.0f));
    const uint32_t wr = tc.weightRes;
    auto at = [&](int x, int z) { return tc.layerWeights[(static_cast<size_t>(z) * wr + x) * 4 + 2]; };
    const int c = static_cast<int>(wr) / 2;
    // Inside the full-strength radius → saturated; further out → progressively less.
    CHECK(at(c, c) == 255);
    // 1 texel = 1 m here (64 m / 64 texels); the brush reaches radius+falloff = 16 m.
    const uint8_t near = at(c + 6,  c);  // ~6.5 m: inside the falloff band
    const uint8_t far  = at(c + 20, c);  // ~20.5 m: past radius+falloff
    CHECK(near > 0);
    CHECK(near < 255);
    CHECK(far == 0);

    CHECK_FALSE(TerrainPaint::paint(tc, 0.0f, 0.0f, kTerrainMaxLayers, 4.0f, 0.0f, 1.0f)); // only 12 layers
    CHECK_FALSE(TerrainPaint::paint(tc, 0.0f, 0.0f, -1, 4.0f, 0.0f, 1.0f));
}

TEST_CASE("Painted layer weights round-trip through the scene file")
{
    HorizonWorld world;
    Entity e = world.createEntity("Landscape");
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 32.0f;
    tc.weightRes = 16;
    TerrainPaint::ensureWeightmap(tc);
    TerrainPaint::paint(tc, 4.0f, -3.0f, 3, 6.0f, 2.0f, 0.7f);
    const std::vector<uint8_t> expected = tc.layerWeights;
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
    CHECK(l->weightRes == 16);
    REQUIRE(l->layerWeights.size() == expected.size());
    CHECK(l->layerWeights == expected);
    CHECK(l->weightsDirty);   // needs an upload on the first tick after load
}

// ─── More than four layers (Thema 158, Schritt 2) ─────────────────────────────
// Layers 4..7 live on a SECOND weight page (TerrainComponent::layerWeights2),
// uploaded as the right half of the same texture. What has to hold: a landscape
// that never touches layer 4+ is byte-for-byte what it was with four layers —
// in memory, in the scene file and on the GPU — and painting crosses the page
// boundary without breaking the "weights sum to 255" invariant.

static_assert(kTerrainMaxLayers == HE::kMatMaxLandscapeLayers,
              "the paint tool and the material blend must agree on the layer count");
static_assert(kTerrainWeightPages == 3 && kTerrainMaxLayers == 12,
              "three RGBA8 weight pages = twelve paintable layers (Thema 329)");
static_assert(kTerrainMaxLayers >= 5,
              "the auto landscape material needs grass, dirt, rock, snow, puddle");

namespace {
int texelSum(const TerrainComponent& tc, uint32_t x, uint32_t z)
{
    uint8_t w[kTerrainMaxLayers] = {};
    REQUIRE(TerrainPaint::texelWeights(tc, x, z, w));
    int s = 0;
    for (int k = 0; k < kTerrainMaxLayers; ++k) s += w[k];
    return s;
}
int texelLayer(const TerrainComponent& tc, uint32_t x, uint32_t z, int layer)
{
    uint8_t w[kTerrainMaxLayers] = {};
    REQUIRE(TerrainPaint::texelWeights(tc, x, z, w));
    return w[layer];
}
} // namespace

TEST_CASE("TerrainPaint paints layers 3, 4 and 5 across the weight-page boundary")
{
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 64.0f;
    tc.weightRes = 64;                 // 1 texel = 1 m
    TerrainPaint::ensureWeightmap(tc);
    const uint32_t c = tc.weightRes / 2;

    // Layer 3 (index 2) and layer 4 (index 3) are first-page channels: no
    // second page appears, painting is exactly the four-layer behaviour.
    REQUIRE(TerrainPaint::paint(tc, -20.0f, 0.0f, 2, 5.0f, 0.0f, 1.0f));
    REQUIRE(TerrainPaint::paint(tc,   0.0f, -20.0f, 3, 5.0f, 0.0f, 1.0f));
    CHECK(tc.layerWeights2.empty());
    CHECK_FALSE(TerrainPaint::usesSecondPage(tc));

    // Layer 5 (index 4) is the first channel of the second page: the stroke
    // allocates it, and the dab centre is now fully on layer index 4.
    tc.weightsDirty = false;
    REQUIRE(TerrainPaint::paint(tc, 0.0f, 0.0f, 4, 6.0f, 4.0f, 1.0f));
    REQUIRE(tc.layerWeights2.size() == tc.layerWeights.size());
    CHECK(TerrainPaint::usesSecondPage(tc));
    CHECK(tc.weightsDirty);
    CHECK(texelLayer(tc, c, c, 4) == 255);
    CHECK(texelLayer(tc, c, c, 0) == 0);
    // In the falloff band layer 0 and layer index 4 share the texel.
    CHECK(texelLayer(tc, c + 8, c, 4) > 0);
    CHECK(texelLayer(tc, c + 8, c, 4) < 255);
    CHECK(texelLayer(tc, c + 8, c, 0) > 0);
    // The earlier first-page strokes are untouched outside this dab …
    CHECK(texelLayer(tc, c - 20, c, 2) == 255);
    CHECK(texelLayer(tc, c, c - 20, 3) == 255);

    // … and the indices 5..7 are reachable as well.
    REQUIRE(TerrainPaint::paint(tc, 20.0f, 20.0f, 5, 3.0f, 0.0f, 1.0f));
    REQUIRE(TerrainPaint::paint(tc, -20.0f, 20.0f, 7, 3.0f, 0.0f, 1.0f));
    CHECK(texelLayer(tc, c + 20, c + 20, 5) == 255);
    CHECK(texelLayer(tc, c - 20, c + 20, 7) == 255);

    // The invariant spans BOTH pages: every texel sums to exactly 255.
    for (uint32_t z = 0; z < tc.weightRes; ++z)
        for (uint32_t x = 0; x < tc.weightRes; ++x)
            REQUIRE(texelSum(tc, x, z) == 255);

    // Partial strokes converge across the boundary too, and painting a first-
    // page layer back over a second-page one takes all of its weight.
    for (int i = 0; i < 40; ++i)
        TerrainPaint::paint(tc, 0.0f, 0.0f, 1, 6.0f, 0.0f, 0.5f);
    CHECK(texelLayer(tc, c, c, 1) == 255);
    CHECK(texelLayer(tc, c, c, 4) == 0);
    CHECK(texelSum(tc, c, c) == 255);
}

TEST_CASE("An empty second page paints the first page exactly like four layers did")
{
    // The eight-channel path with layers 4..7 all zero must be arithmetic-
    // identical to the four-channel path: the same strokes on 0..3 give the
    // same bytes whether or not a second page has been allocated.
    TerrainComponent a;
    a.sizeX = a.sizeZ = 48.0f;
    a.weightRes = 48;
    TerrainPaint::ensureWeightmap(a);
    TerrainComponent b = a;
    b.layerWeights2.assign(b.layerWeights.size(), 0);

    const struct { float x, z; int layer; float r, f, s; } strokes[] = {
        {  0.0f,  0.0f, 1, 9.0f, 5.0f, 0.35f }, {  4.0f, -3.0f, 3, 6.0f, 2.0f, 0.7f },
        { -9.0f,  6.0f, 2, 4.0f, 8.0f, 0.15f }, {  0.0f,  0.0f, 0, 3.0f, 3.0f, 0.5f },
        {  2.0f,  2.0f, 3, 7.0f, 0.0f, 1.0f  }, { -1.0f,  5.0f, 1, 5.0f, 9.0f, 0.05f },
    };
    for (const auto& st : strokes)
    {
        TerrainPaint::paint(a, st.x, st.z, st.layer, st.r, st.f, st.s);
        TerrainPaint::paint(b, st.x, st.z, st.layer, st.r, st.f, st.s);
    }
    CHECK(a.layerWeights2.empty());               // never allocated by 0..3
    CHECK(a.layerWeights == b.layerWeights);
    CHECK(std::all_of(b.layerWeights2.begin(), b.layerWeights2.end(),
                      [](uint8_t v) { return v == 0; }));
    CHECK_FALSE(TerrainPaint::usesSecondPage(b));
}

TEST_CASE("buildWeightTexture keeps a four-layer map as it was and puts layers 4..7 beside it")
{
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 16.0f;
    tc.weightRes = 8;
    uint32_t w = 99, h = 99;
    CHECK(TerrainPaint::buildWeightTexture(tc, w, h).empty());   // no weightmap
    CHECK(w == 0);
    CHECK(h == 0);

    TerrainPaint::ensureWeightmap(tc);
    TerrainPaint::paint(tc, 3.0f, -2.0f, 2, 3.0f, 2.0f, 0.6f);
    // First page only → the upload IS layerWeights, square, as before.
    CHECK(TerrainPaint::buildWeightTexture(tc, w, h) == tc.layerWeights);
    CHECK(w == 8);
    CHECK(h == 8);
    // An allocated but all-zero second page changes nothing either.
    tc.layerWeights2.assign(tc.layerWeights.size(), 0);
    CHECK(TerrainPaint::buildWeightTexture(tc, w, h) == tc.layerWeights);
    CHECK(w == 8);

    // A used second page → 2:1, each row = page-0 row then page-1 row.
    REQUIRE(TerrainPaint::paint(tc, -4.0f, 4.0f, 6, 2.0f, 1.0f, 1.0f));
    const std::vector<uint8_t> tex = TerrainPaint::buildWeightTexture(tc, w, h);
    REQUIRE(w == 16);
    REQUIRE(h == 8);
    REQUIRE(tex.size() == size_t(16) * 8 * 4);
    for (uint32_t z = 0; z < 8; ++z)
        for (uint32_t x = 0; x < 8; ++x)
            for (int k = 0; k < 4; ++k)
            {
                const size_t src = (size_t(z) * 8 + x) * 4 + k;
                REQUIRE(tex[(size_t(z) * 16 + x) * 4 + k]     == tc.layerWeights[src]);
                REQUIRE(tex[(size_t(z) * 16 + 8 + x) * 4 + k] == tc.layerWeights2[src]);
            }

    // A second page that does not match the first is never uploaded …
    tc.layerWeights2.resize(tc.layerWeights2.size() - 4);
    CHECK_FALSE(TerrainPaint::usesSecondPage(tc));
    CHECK(TerrainPaint::buildWeightTexture(tc, w, h) == tc.layerWeights);
    // … and ensureWeightmap drops it rather than carry a half-sized page.
    TerrainPaint::ensureWeightmap(tc);
    CHECK(tc.layerWeights2.empty());
}

namespace {
nlohmann::json* terrainJsonOf(nlohmann::json& scene)
{
    for (auto& e : scene["entities"])
        if (e.contains("components") && e["components"].contains("terrain"))
            return &e["components"]["terrain"];
    return nullptr;
}
const TerrainComponent* onlyTerrain(HorizonWorld& w)
{
    const TerrainComponent* t = nullptr;
    for (auto ent : w.registry().view<TerrainComponent>())
        t = &w.registry().get<TerrainComponent>(ent);
    return t;
}
} // namespace

TEST_CASE("A four-layer landscape writes the old scene format and an old scene loads unchanged")
{
    HorizonWorld world;
    Entity e = world.createEntity("Landscape");
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 32.0f;
    tc.weightRes = 16;
    TerrainPaint::ensureWeightmap(tc);
    TerrainPaint::paint(tc, 4.0f, -3.0f, 3, 6.0f, 2.0f, 0.7f);
    TerrainPaint::paint(tc, -6.0f, 5.0f, 1, 4.0f, 4.0f, 0.4f);
    world.registry().emplace<TerrainComponent>(e, tc);

    SceneSerializer ser;
    std::vector<uint8_t> bytes;
    REQUIRE(ser.saveToMemory(world, bytes));
    nlohmann::json scene = nlohmann::json::from_cbor(bytes);
    nlohmann::json* tj = terrainJsonOf(scene);
    REQUIRE(tj != nullptr);
    // Nothing new is written for a landscape that does not use layers 4..7:
    // the file is what a four-layer build wrote and still reads.
    CHECK(tj->contains("layerWeightsB64"));
    CHECK_FALSE(tj->contains("layerWeights2B64"));
    CHECK_FALSE(tj->contains("layerWeights3B64"));

    // Loading that (= every scene saved before eight layers) gives no second
    // page, and the texture the renderer gets is the first page, square.
    HorizonWorld w2;
    REQUIRE(ser.loadFromMemory(w2, nlohmann::json::to_cbor(scene)));
    const TerrainComponent* l = onlyTerrain(w2);
    REQUIRE(l != nullptr);
    CHECK(l->layerWeights == tc.layerWeights);
    CHECK(l->layerWeights2.empty());
    uint32_t w = 0, h = 0;
    CHECK(TerrainPaint::buildWeightTexture(*l, w, h) == tc.layerWeights);
    CHECK(w == 16);
    CHECK(h == 16);
}

TEST_CASE("Layers 4..7 round-trip through the scene file, a broken second page is dropped")
{
    HorizonWorld world;
    Entity e = world.createEntity("Landscape");
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 32.0f;
    tc.weightRes = 16;
    TerrainPaint::ensureWeightmap(tc);
    TerrainPaint::paint(tc, 4.0f, -3.0f, 2, 6.0f, 2.0f, 0.7f);
    TerrainPaint::paint(tc, -5.0f, 6.0f, 4, 5.0f, 3.0f, 0.9f);
    TerrainPaint::paint(tc, 7.0f, 7.0f, 7, 3.0f, 1.0f, 1.0f);
    REQUIRE(TerrainPaint::usesSecondPage(tc));
    world.registry().emplace<TerrainComponent>(e, tc);

    SceneSerializer ser;
    std::vector<uint8_t> bytes;
    REQUIRE(ser.saveToMemory(world, bytes));
    {
        HorizonWorld w2;
        REQUIRE(ser.loadFromMemory(w2, bytes));
        const TerrainComponent* l = onlyTerrain(w2);
        REQUIRE(l != nullptr);
        CHECK(l->layerWeights  == tc.layerWeights);
        CHECK(l->layerWeights2 == tc.layerWeights2);
        CHECK(l->weightsDirty);
    }

    nlohmann::json scene = nlohmann::json::from_cbor(bytes);
    nlohmann::json* tj = terrainJsonOf(scene);
    REQUIRE(tj != nullptr);
    REQUIRE(tj->contains("layerWeights2B64"));
    // A two-page landscape writes exactly what an eight-layer build wrote: no third key.
    CHECK_FALSE(tj->contains("layerWeights3B64"));
    {
        // Truncated second page → dropped, the first page survives.
        nlohmann::json bad = scene;
        (*terrainJsonOf(bad))["layerWeights2B64"] =
            SceneSerializer::encodeBase64(tc.layerWeights2.data(), tc.layerWeights2.size() - 4);
        HorizonWorld w3;
        REQUIRE(ser.loadFromMemory(w3, nlohmann::json::to_cbor(bad)));
        const TerrainComponent* l = onlyTerrain(w3);
        REQUIRE(l != nullptr);
        CHECK(l->layerWeights == tc.layerWeights);
        CHECK(l->layerWeights2.empty());
    }
    {
        // A second page without a first page has nothing to sit beside.
        nlohmann::json bad = scene;
        terrainJsonOf(bad)->erase("layerWeightsB64");
        HorizonWorld w4;
        REQUIRE(ser.loadFromMemory(w4, nlohmann::json::to_cbor(bad)));
        const TerrainComponent* l = onlyTerrain(w4);
        REQUIRE(l != nullptr);
        CHECK(l->layerWeights.empty());
        CHECK(l->layerWeights2.empty());
    }
}

// ─── Twelve layers (Thema 329, Schritt 1) ─────────────────────────────────────
// Layers 8..11 live on a THIRD weight page (TerrainComponent::layerWeights3), the
// last third of a 3:1 texture. Same contract as the second page: a landscape that
// never touches layer 8+ is byte-for-byte what it was with eight layers — in
// memory, in the scene file and on the GPU.

TEST_CASE("TerrainPaint paints layers 8..11 on a third page, normalised across all three")
{
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 64.0f;
    tc.weightRes = 64;                 // 1 texel = 1 m
    TerrainPaint::ensureWeightmap(tc);
    const uint32_t c = tc.weightRes / 2;

    // Layer 5 is on the second page: it allocates that one, and not a third.
    REQUIRE(TerrainPaint::paint(tc, -20.0f, 0.0f, 5, 5.0f, 0.0f, 1.0f));
    CHECK(tc.layerWeights3.empty());
    CHECK_FALSE(TerrainPaint::usesThirdPage(tc));
    CHECK(TerrainPaint::weightPageCount(tc) == 2);

    // Layer 9 is the second channel of the third page: the stroke allocates it
    // (all zero) beside the second, and the dab centre is fully on layer 9.
    tc.weightsDirty = false;
    REQUIRE(TerrainPaint::paint(tc, 0.0f, 0.0f, 9, 6.0f, 4.0f, 1.0f));
    REQUIRE(tc.layerWeights2.size() == tc.layerWeights.size());
    REQUIRE(tc.layerWeights3.size() == tc.layerWeights.size());
    CHECK(TerrainPaint::usesThirdPage(tc));
    CHECK(TerrainPaint::weightPageCount(tc) == 3);
    CHECK(tc.weightsDirty);
    CHECK(texelLayer(tc, c, c, 9) == 255);
    CHECK(texelLayer(tc, c, c, 0) == 0);
    // In the falloff band layer 0 and layer 9 share the texel.
    CHECK(texelLayer(tc, c + 8, c, 9) > 0);
    CHECK(texelLayer(tc, c + 8, c, 9) < 255);
    CHECK(texelLayer(tc, c + 8, c, 0) > 0);
    // The earlier second-page stroke is untouched outside this dab.
    CHECK(texelLayer(tc, c - 20, c, 5) == 255);

    // The other channels of the third page are reachable as well.
    REQUIRE(TerrainPaint::paint(tc, 20.0f, 20.0f, 8, 3.0f, 0.0f, 1.0f));
    REQUIRE(TerrainPaint::paint(tc, -20.0f, 20.0f, 11, 3.0f, 0.0f, 1.0f));
    REQUIRE(TerrainPaint::paint(tc, 20.0f, -20.0f, 10, 3.0f, 0.0f, 1.0f));
    CHECK(texelLayer(tc, c + 20, c + 20, 8) == 255);
    CHECK(texelLayer(tc, c - 20, c + 20, 11) == 255);
    CHECK(texelLayer(tc, c + 20, c - 20, 10) == 255);

    // The invariant spans ALL pages: every texel sums to exactly 255.
    for (uint32_t z = 0; z < tc.weightRes; ++z)
        for (uint32_t x = 0; x < tc.weightRes; ++x)
            REQUIRE(texelSum(tc, x, z) == 255);

    // Partial strokes converge, and a lower-page layer painted back over a
    // third-page one takes all of its weight.
    for (int i = 0; i < 40; ++i)
        TerrainPaint::paint(tc, 0.0f, 0.0f, 1, 6.0f, 0.0f, 0.5f);
    CHECK(texelLayer(tc, c, c, 1) == 255);
    CHECK(texelLayer(tc, c, c, 9) == 0);
    CHECK(texelSum(tc, c, c) == 255);

    // And a layer past the last one is still refused.
    CHECK_FALSE(TerrainPaint::paint(tc, 0.0f, 0.0f, 12, 4.0f, 0.0f, 1.0f));
}

TEST_CASE("An empty third page paints the first two pages exactly like eight layers did")
{
    // The twelve-channel path with layers 8..11 all zero must be arithmetic-
    // identical to the eight-channel path: the same strokes on 0..7 give the same
    // bytes whether or not a third page has been allocated.
    TerrainComponent a;
    a.sizeX = a.sizeZ = 48.0f;
    a.weightRes = 48;
    TerrainPaint::ensureWeightmap(a);
    TerrainPaint::paint(a, 6.0f, 6.0f, 5, 8.0f, 4.0f, 0.8f);   // both start with a second page
    REQUIRE(a.layerWeights2.size() == a.layerWeights.size());
    TerrainComponent b = a;
    b.layerWeights3.assign(b.layerWeights.size(), 0);

    const struct { float x, z; int layer; float r, f, s; } strokes[] = {
        {  0.0f,  0.0f, 1, 9.0f, 5.0f, 0.35f }, {  4.0f, -3.0f, 6, 6.0f, 2.0f, 0.7f },
        { -9.0f,  6.0f, 2, 4.0f, 8.0f, 0.15f }, {  0.0f,  0.0f, 7, 3.0f, 3.0f, 0.5f },
        {  2.0f,  2.0f, 3, 7.0f, 0.0f, 1.0f  }, { -1.0f,  5.0f, 4, 5.0f, 9.0f, 0.05f },
    };
    for (const auto& st : strokes)
    {
        TerrainPaint::paint(a, st.x, st.z, st.layer, st.r, st.f, st.s);
        TerrainPaint::paint(b, st.x, st.z, st.layer, st.r, st.f, st.s);
    }
    CHECK(a.layerWeights3.empty());               // never allocated by 0..7
    CHECK(a.layerWeights  == b.layerWeights);
    CHECK(a.layerWeights2 == b.layerWeights2);
    CHECK(std::all_of(b.layerWeights3.begin(), b.layerWeights3.end(),
                      [](uint8_t v) { return v == 0; }));
    CHECK_FALSE(TerrainPaint::usesThirdPage(b));
    CHECK(TerrainPaint::weightPageCount(a) == TerrainPaint::weightPageCount(b));
}

TEST_CASE("TerrainPaint::fillLayer on the third page, and back down again")
{
    TerrainComponent tc;
    tc.sizeX = 40.0f; tc.sizeZ = 40.0f; tc.weightRes = 32;
    uint8_t w[kTerrainMaxLayers];

    REQUIRE(TerrainPaint::fillLayer(tc, 10));
    REQUIRE(TerrainPaint::texelWeights(tc, 3, 4, w));
    for (int k = 0; k < kTerrainMaxLayers; ++k) CHECK(w[k] == (k == 10 ? 255 : 0));
    CHECK(TerrainPaint::usesThirdPage(tc));
    CHECK(TerrainPaint::weightPageCount(tc) == 3);

    // A second-page fill leaves no third page voting, and drops its bytes.
    REQUIRE(TerrainPaint::fillLayer(tc, 5));
    REQUIRE(TerrainPaint::texelWeights(tc, 3, 4, w));
    for (int k = 0; k < kTerrainMaxLayers; ++k) CHECK(w[k] == (k == 5 ? 255 : 0));
    CHECK(tc.layerWeights3.empty());
    CHECK(TerrainPaint::weightPageCount(tc) == 2);

    // A first-page fill clears both.
    REQUIRE(TerrainPaint::fillLayer(tc, 11));
    REQUIRE(TerrainPaint::fillLayer(tc, 0));
    REQUIRE(TerrainPaint::texelWeights(tc, 3, 4, w));
    for (int k = 0; k < kTerrainMaxLayers; ++k) CHECK(w[k] == (k == 0 ? 255 : 0));
    CHECK(tc.layerWeights2.empty());
    CHECK(tc.layerWeights3.empty());
    CHECK(TerrainPaint::weightPageCount(tc) == 1);

    CHECK_FALSE(TerrainPaint::fillLayer(tc, kTerrainMaxLayers));
}

TEST_CASE("fillLayer keeps a mixed terrain's group shares and refuses a share layer")
{
    // The group shares sit in layers 8..11 (third page); a fill starts the PAINT over
    // and must neither wipe them nor refuse a paint layer.
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 32.0f;
    tc.weightRes = 16;
    TerrainPaint::ensureWeightmap(tc);
    REQUIRE(TerrainPaint::enableGroupMix(tc, 0));
    REQUIRE(TerrainPaint::paintGroup(tc, 0.0f, 0.0f, 1, 40.0f, 0.0f, 1.0f));
    float s[4];
    REQUIRE(TerrainPaint::groupShares(tc, 8, 8, s));
    REQUIRE(s[1] == doctest::Approx(1.0f));

    CHECK_FALSE(TerrainPaint::fillLayer(tc, 9));   // a share layer is no paint layer
    REQUIRE(TerrainPaint::fillLayer(tc, 6));
    uint8_t w[kTerrainMaxLayers];
    REQUIRE(TerrainPaint::texelWeights(tc, 8, 8, w));
    CHECK(w[6] == 255);
    CHECK(w[9] == 255);                           // the share byte survived
    for (int k : { 0, 1, 2, 3, 4, 5, 7, 8, 10, 11 }) CHECK(w[k] == 0);
    // A fill back on the first page keeps the shares too.
    REQUIRE(TerrainPaint::fillLayer(tc, 2));
    REQUIRE(TerrainPaint::groupShares(tc, 8, 8, s));
    CHECK(s[1] == doctest::Approx(1.0f));
}

TEST_CASE("buildWeightTexture lays a used third page out as the last third of a 3:1 map")
{
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 16.0f;
    tc.weightRes = 8;
    uint32_t w = 0, h = 0;
    TerrainPaint::ensureWeightmap(tc);
    TerrainPaint::paint(tc, 3.0f, -2.0f, 2, 3.0f, 2.0f, 0.6f);

    // An allocated but all-zero third page changes nothing — here beside no second
    // page at all, so the upload is still the first page, square.
    tc.layerWeights3.assign(tc.layerWeights.size(), 0);
    CHECK(TerrainPaint::buildWeightTexture(tc, w, h) == tc.layerWeights);
    CHECK(w == 8);

    // With a used second page the map is the 2:1 one, exactly as it was before there
    // was a third page: each row the page-0 row then the page-1 row — and the empty
    // third page makes no difference to a byte of it.
    REQUIRE(TerrainPaint::paint(tc, -4.0f, 4.0f, 6, 2.0f, 1.0f, 1.0f));
    CHECK(TerrainPaint::weightPageCount(tc) == 2);
    const std::vector<uint8_t> two = TerrainPaint::buildWeightTexture(tc, w, h);
    REQUIRE(w == 16);
    REQUIRE(h == 8);
    REQUIRE(two.size() == size_t(16) * 8 * 4);
    for (uint32_t z = 0; z < 8; ++z)
        for (uint32_t x = 0; x < 8; ++x)
            for (int k = 0; k < 4; ++k)
            {
                const size_t src = (size_t(z) * 8 + x) * 4 + k;
                REQUIRE(two[(size_t(z) * 16 + x) * 4 + k]     == tc.layerWeights[src]);
                REQUIRE(two[(size_t(z) * 16 + 8 + x) * 4 + k] == tc.layerWeights2[src]);
            }
    {
        TerrainComponent noThird = tc;
        noThird.layerWeights3.clear();
        uint32_t w0 = 0, h0 = 0;
        CHECK(TerrainPaint::buildWeightTexture(noThird, w0, h0) == two);
        CHECK(w0 == 16);
    }

    // A used third page → 3:1, each row = page 0, page 1, page 2.
    REQUIRE(TerrainPaint::paint(tc, 5.0f, 5.0f, 10, 2.0f, 1.0f, 1.0f));
    CHECK(TerrainPaint::usesThirdPage(tc));
    const std::vector<uint8_t> tex = TerrainPaint::buildWeightTexture(tc, w, h);
    REQUIRE(w == 24);
    REQUIRE(h == 8);
    REQUIRE(tex.size() == size_t(24) * 8 * 4);
    for (uint32_t z = 0; z < 8; ++z)
        for (uint32_t x = 0; x < 8; ++x)
            for (int k = 0; k < 4; ++k)
            {
                const size_t src = (size_t(z) * 8 + x) * 4 + k;
                REQUIRE(tex[(size_t(z) * 24 + x) * 4 + k]      == tc.layerWeights[src]);
                REQUIRE(tex[(size_t(z) * 24 + 8 + x) * 4 + k]  == tc.layerWeights2[src]);
                REQUIRE(tex[(size_t(z) * 24 + 16 + x) * 4 + k] == tc.layerWeights3[src]);
            }

    // A third page without a second beside it is uploaded with zeros in the middle
    // (the paint API never makes that state; a scene file could).
    {
        TerrainComponent odd = tc;
        odd.layerWeights2.clear();
        const std::vector<uint8_t> t3 = TerrainPaint::buildWeightTexture(odd, w, h);
        REQUIRE(w == 24);
        for (uint32_t z = 0; z < 8; ++z)
            for (uint32_t x = 0; x < 8; ++x)
                for (int k = 0; k < 4; ++k)
                {
                    const size_t src = (size_t(z) * 8 + x) * 4 + k;
                    REQUIRE(t3[(size_t(z) * 24 + x) * 4 + k]      == odd.layerWeights[src]);
                    REQUIRE(t3[(size_t(z) * 24 + 8 + x) * 4 + k]  == 0);
                    REQUIRE(t3[(size_t(z) * 24 + 16 + x) * 4 + k] == odd.layerWeights3[src]);
                }
    }

    // A third page that does not match the first is never uploaded, the map stays 2:1 …
    tc.layerWeights3.resize(tc.layerWeights3.size() - 4);
    CHECK_FALSE(TerrainPaint::usesThirdPage(tc));
    {
        TerrainComponent noThird = tc;           // the pages did move with the layer-10 stroke
        noThird.layerWeights3.clear();
        uint32_t w0 = 0, h0 = 0;
        const std::vector<uint8_t> want = TerrainPaint::buildWeightTexture(noThird, w0, h0);
        CHECK(TerrainPaint::buildWeightTexture(tc, w, h) == want);
        CHECK(w == 16);
        CHECK(w0 == 16);
    }
    // … and ensureWeightmap drops it rather than carry a half-sized page.
    TerrainPaint::ensureWeightmap(tc);
    CHECK(tc.layerWeights3.empty());
    CHECK_FALSE(tc.layerWeights2.empty());     // the good second page stays
    // A third page whose second page is gone is dropped too (it only sits beside one).
    tc.layerWeights3.assign(tc.layerWeights.size(), 7);
    tc.layerWeights2.clear();
    TerrainPaint::ensureWeightmap(tc);
    CHECK(tc.layerWeights3.empty());
}

TEST_CASE("Layers 8..11 round-trip through the scene file, an eight-layer scene loads without them")
{
    HorizonWorld world;
    Entity e = world.createEntity("Landscape");
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 32.0f;
    tc.weightRes = 16;
    TerrainPaint::ensureWeightmap(tc);
    TerrainPaint::paint(tc, 4.0f, -3.0f, 2, 6.0f, 2.0f, 0.7f);
    TerrainPaint::paint(tc, -5.0f, 6.0f, 5, 5.0f, 3.0f, 0.9f);
    TerrainPaint::paint(tc, 7.0f, 7.0f, 11, 3.0f, 1.0f, 1.0f);
    TerrainPaint::paint(tc, -7.0f, -7.0f, 8, 4.0f, 1.0f, 0.8f);
    REQUIRE(TerrainPaint::usesThirdPage(tc));
    world.registry().emplace<TerrainComponent>(e, tc);

    SceneSerializer ser;
    std::vector<uint8_t> bytes;
    REQUIRE(ser.saveToMemory(world, bytes));
    {
        HorizonWorld w2;
        REQUIRE(ser.loadFromMemory(w2, bytes));
        const TerrainComponent* l = onlyTerrain(w2);
        REQUIRE(l != nullptr);
        CHECK(l->layerWeights  == tc.layerWeights);
        CHECK(l->layerWeights2 == tc.layerWeights2);
        CHECK(l->layerWeights3 == tc.layerWeights3);
        CHECK(l->weightsDirty);
        uint32_t w = 0, h = 0, w0 = 0, h0 = 0;
        CHECK(TerrainPaint::buildWeightTexture(*l, w, h) == TerrainPaint::buildWeightTexture(tc, w0, h0));
        CHECK(w == 3 * 16);
    }

    nlohmann::json scene = nlohmann::json::from_cbor(bytes);
    nlohmann::json* tj = terrainJsonOf(scene);
    REQUIRE(tj != nullptr);
    REQUIRE(tj->contains("layerWeights3B64"));
    {
        // A scene saved before there were twelve layers has no third key: it loads as
        // two pages, and uploads the 2:1 map those pages always made.
        nlohmann::json old = scene;
        terrainJsonOf(old)->erase("layerWeights3B64");
        HorizonWorld w3;
        REQUIRE(ser.loadFromMemory(w3, nlohmann::json::to_cbor(old)));
        const TerrainComponent* l = onlyTerrain(w3);
        REQUIRE(l != nullptr);
        CHECK(l->layerWeights  == tc.layerWeights);
        CHECK(l->layerWeights2 == tc.layerWeights2);
        CHECK(l->layerWeights3.empty());
        uint32_t w = 0, h = 0;
        const std::vector<uint8_t> tex = TerrainPaint::buildWeightTexture(*l, w, h);
        REQUIRE(w == 32);
        REQUIRE(h == 16);
        std::vector<uint8_t> want;                     // [page 0 row | page 1 row], row by row
        for (size_t z = 0; z < 16; ++z)
        {
            want.insert(want.end(), tc.layerWeights.begin()  + z * 64, tc.layerWeights.begin()  + (z + 1) * 64);
            want.insert(want.end(), tc.layerWeights2.begin() + z * 64, tc.layerWeights2.begin() + (z + 1) * 64);
        }
        CHECK(tex == want);
    }
    {
        // Truncated third page → dropped, the first two survive.
        nlohmann::json bad = scene;
        (*terrainJsonOf(bad))["layerWeights3B64"] =
            SceneSerializer::encodeBase64(tc.layerWeights3.data(), tc.layerWeights3.size() - 4);
        HorizonWorld w4;
        REQUIRE(ser.loadFromMemory(w4, nlohmann::json::to_cbor(bad)));
        const TerrainComponent* l = onlyTerrain(w4);
        REQUIRE(l != nullptr);
        CHECK(l->layerWeights  == tc.layerWeights);
        CHECK(l->layerWeights2 == tc.layerWeights2);
        CHECK(l->layerWeights3.empty());
    }
    {
        // A third page without a second page has nothing to sit beside.
        nlohmann::json bad = scene;
        terrainJsonOf(bad)->erase("layerWeights2B64");
        HorizonWorld w5;
        REQUIRE(ser.loadFromMemory(w5, nlohmann::json::to_cbor(bad)));
        const TerrainComponent* l = onlyTerrain(w5);
        REQUIRE(l != nullptr);
        CHECK(l->layerWeights == tc.layerWeights);
        CHECK(l->layerWeights2.empty());
        CHECK(l->layerWeights3.empty());
    }
}

TEST_CASE("TerrainSystem uploads a 2:1 weightmap only once layers 4..7 are painted")
{
    HorizonWorld world;
    ContentManager cm(".");
    auto& reg = world.registry();
    Entity te = world.createEntity("Landscape");
    reg.emplace<TransformComponent>(te);
    TerrainComponent tc;
    tc.resolution = 9;
    tc.sizeX = tc.sizeZ = 16.0f;
    tc.weightRes = 8;
    tc.dirty = true;
    TerrainPaint::ensureWeightmap(tc);
    TerrainPaint::paint(tc, 0.0f, 0.0f, 1, 3.0f, 0.0f, 1.0f);
    reg.emplace<TerrainComponent>(te, tc);

    TerrainSystem::updateTerrains(world, cm);
    auto& live = reg.get<TerrainComponent>(te);
    const HE::UUID texId = live.weightmapTextureId;
    REQUIRE(texId != HE::UUID{});
    const TextureAsset* t = cm.getTexture(texId);
    REQUIRE(t != nullptr);
    CHECK(t->width == 8);           // four layers: the square map, as before
    CHECK(t->height == 8);
    CHECK(t->data == live.layerWeights);
    CHECK(live.avgLayerWeights[4] == 0.0f);

    // A stroke on layer index 5 that covers the whole map → the same texture
    // id, now 2:1.
    REQUIRE(TerrainPaint::paint(live, 0.0f, 0.0f, 5, 16.0f, 0.0f, 1.0f));
    TerrainSystem::updateTerrains(world, cm);
    CHECK(live.weightmapTextureId == texId);
    t = cm.getTexture(texId);
    REQUIRE(t != nullptr);
    CHECK(t->width == 16);
    CHECK(t->height == 8);
    // The terrain-wide mean sees the second page: all weight on layer 5.
    CHECK(live.avgLayerWeights[5] == doctest::Approx(1.0f));
    CHECK(live.avgLayerWeights[0] == doctest::Approx(0.0f));
}

TEST_CASE("TerrainSystem uploads a 3:1 weightmap only once layers 8..11 are painted")
{
    HorizonWorld world;
    ContentManager cm(".");
    auto& reg = world.registry();
    Entity te = world.createEntity("Landscape");
    reg.emplace<TransformComponent>(te);
    TerrainComponent tc;
    tc.resolution = 9;
    tc.sizeX = tc.sizeZ = 16.0f;
    tc.weightRes = 8;
    tc.dirty = true;
    TerrainPaint::ensureWeightmap(tc);
    TerrainPaint::paint(tc, 0.0f, 0.0f, 5, 3.0f, 0.0f, 1.0f);   // a two-page landscape
    reg.emplace<TerrainComponent>(te, tc);

    TerrainSystem::updateTerrains(world, cm);
    auto& live = reg.get<TerrainComponent>(te);
    const HE::UUID texId = live.weightmapTextureId;
    REQUIRE(texId != HE::UUID{});
    const TextureAsset* t = cm.getTexture(texId);
    REQUIRE(t != nullptr);
    CHECK(t->width == 16);          // two pages: the 2:1 map, as before the third
    CHECK(t->height == 8);
    {
        uint32_t w = 0, h = 0;
        CHECK(t->data == TerrainPaint::buildWeightTexture(live, w, h));
    }
    CHECK(live.avgLayerWeights[8] == 0.0f);

    // A stroke on layer 10 that covers the whole map → the same texture id, now 3:1.
    REQUIRE(TerrainPaint::paint(live, 0.0f, 0.0f, 10, 16.0f, 0.0f, 1.0f));
    TerrainSystem::updateTerrains(world, cm);
    CHECK(live.weightmapTextureId == texId);
    t = cm.getTexture(texId);
    REQUIRE(t != nullptr);
    CHECK(t->width == 24);
    CHECK(t->height == 8);
    CHECK(t->data.size() == size_t(24) * 8 * 4);
    // The terrain-wide mean sees the third page: all weight on layer 10.
    CHECK(live.avgLayerWeights[10] == doctest::Approx(1.0f));
    CHECK(live.avgLayerWeights[5]  == doctest::Approx(0.0f));
    CHECK(live.avgLayerWeights[0]  == doctest::Approx(0.0f));
}

// ─── Landscape collision (B2: the player used to fall through the ground) ─────
//
// ColliderShape had exactly Box, Sphere and Capsule, and NOTHING ever gave a
// terrain entity a RigidBodyComponent — not the Create Landscape tool, not the
// scene loader, not the chunk generator. So an outdoor level had no ground at
// all, while NavigationSystem happily baked the same terrain chunks into the
// navmesh: the AI walked on a floor the player dropped through. Navigation and
// physics were looking at two different worlds.
//
// The collider is ONE static height field on the Landscape entity, not one per
// chunk. That is deliberate — the chunks carry distance-LOD, and a collider made
// from whatever LOD is on screen would change shape as the camera moved.
//
// A note on resolution: 33 is 2ⁿ+1, so TerrainSystem's snap is a no-op and the
// physics height field is sample-for-sample the array everything else reads.

namespace
{
    // A landscape with real relief and NO RigidBodyComponent — the implicit
    // terrain body is the fallback for exactly that case, which is the one the
    // engine actually produces.
    Entity makeLandscape(HorizonWorld& world, TerrainComponent& outTc)
    {
        TerrainComponent tc;
        tc.sizeX = tc.sizeZ = 64.0f;
        tc.resolution  = 33;          // 2⁵+1: no resample on the way to Jolt
        tc.heightScale = 20.0f;
        tc.seed        = 1234;        // non-zero → fBm relief, not a flat plane
        outTc = tc;

        Entity e = world.createEntity("Landscape");
        TransformComponent t;
        t.position = { 0.0f, 0.0f, 0.0f };
        t.scale    = { 1.0f, 1.0f, 1.0f };
        world.addComponent(e, t);
        world.registry().emplace<TerrainComponent>(e, tc);
        return e;
    }
}

TEST_CASE("Landscape has collision and its height matches what navigation bakes")
{
    HorizonWorld     world;
    TerrainComponent tc;
    Entity           land = makeLandscape(world, tc);

    PhysicsWorld phys;
    phys.initialize(world);

    // BEFORE THE CHANGE: no terrain entity ever had a physics representation, so
    // this was false and every raycast below missed. That is the whole blocker
    // in one assertion.
    REQUIRE(phys.hasPhysics(static_cast<uint32_t>(land)));

    // The array NavigationSystem's input is built from: the chunk meshes sample
    // this field, and LOD0 vertices land exactly on its grid points, so a value
    // here IS the height the navmesh has at that point.
    const std::vector<float> field = computeTerrainHeightField(tc);
    REQUIRE(field.size() == static_cast<size_t>(tc.resolution) * tc.resolution);

    const float step = tc.sizeX / static_cast<float>(tc.resolution - 1);

    // Sample at EXACT grid points. Between samples Jolt's triangulation and the
    // bilinear lookup can differ by a little; on a sample they must agree, and
    // that is the comparison worth pinning.
    int checked = 0;
    for (uint32_t iz = 4; iz < tc.resolution - 4; iz += 7)
        for (uint32_t ix = 4; ix < tc.resolution - 4; ix += 7)
        {
            const float x = -tc.sizeX * 0.5f + static_cast<float>(ix) * step;
            const float z = -tc.sizeZ * 0.5f + static_cast<float>(iz) * step;

            const auto hit = phys.raycast({ x, 200.0f, z }, { 0.0f, -1.0f, 0.0f }, 400.0f);
            REQUIRE(hit.hit);
            CHECK(hit.entityId == static_cast<uint32_t>(land));

            // Three descriptions of the same ground, and they have to agree:
            // what physics hit, what FoliageSystem places on, and what the
            // navmesh was baked from.
            const float navHeight = field[static_cast<size_t>(iz) * tc.resolution + ix];
            CHECK(hit.point.y == doctest::Approx(navHeight).epsilon(0.01));
            CHECK(hit.point.y == doctest::Approx(terrainHeightAt(tc, x, z)).epsilon(0.01));
            ++checked;
        }
    REQUIRE(checked > 4);   // the loop actually ran

    // The relief is real, not a flat plane that would make the above trivial.
    const auto lo = *std::min_element(field.begin(), field.end());
    const auto hi = *std::max_element(field.begin(), field.end());
    CHECK(hi - lo > 1.0f);
}

TEST_CASE("A body dropped on a landscape comes to rest on it instead of falling through")
{
    HorizonWorld     world;
    TerrainComponent tc;
    Entity           land = makeLandscape(world, tc);

    // A sphere so the resting height is exactly centre = ground + radius, with
    // no box corner to catch on a slope.
    const float radius = 0.5f;
    const float dropX = 6.0f, dropZ = -10.0f;
    const float ground = terrainHeightAt(tc, dropX, dropZ);

    Entity ball = world.createEntity("Ball");
    {
        TransformComponent t;
        t.position = { dropX, ground + 15.0f, dropZ };
        t.scale    = { 1.0f, 1.0f, 1.0f };
        world.addComponent(ball, t);
        RigidBodyComponent rb; rb.type = RigidBodyType::Dynamic; rb.mass = 1.0f;
        world.addComponent(ball, rb);
        ColliderComponent col; col.shape = ColliderShape::Sphere; col.radius = radius;
        world.addComponent(ball, col);
    }

    PhysicsWorld phys;
    phys.initialize(world);
    REQUIRE(phys.hasPhysics(static_cast<uint32_t>(land)));

    // BEFORE THE CHANGE: with no terrain collider the ball fell for four seconds
    // and was ~78 m below the ground it should have landed on. This is the
    // player-falls-through-the-landscape bug, measured.
    for (int i = 0; i < 240; ++i)
        phys.step(world, 1.0f / 60.0f);

    const auto& tr = world.registry().get<TransformComponent>(ball);
    // A slope lets the ball roll a little before it settles, so the resting
    // height is checked against the ground UNDER WHERE IT ENDED UP, not under
    // where it was dropped.
    const float groundBelow = terrainHeightAt(tc, tr.position.x, tr.position.z);
    CHECK(tr.position.y > groundBelow - 0.1f);
    CHECK(tr.position.y == doctest::Approx(groundBelow + radius).epsilon(0.15));
    CHECK(tr.position.y > ground - 2.0f);   // it never sank through
}

TEST_CASE("An authored rigid body on a terrain entity wins over the implicit one")
{
    HorizonWorld     world;
    TerrainComponent tc;
    Entity           land = makeLandscape(world, tc);

    // The implicit height field is a FALLBACK for the landscape nothing authors
    // physics for. Once someone gives the terrain entity a rigid body of its
    // own, that is the body — one entity must never end up with two, which would
    // be an invisible second collider nothing can address or remove.
    {
        RigidBodyComponent rb; rb.type = RigidBodyType::Static;
        world.addComponent(land, rb);
        ColliderComponent col;
        col.shape = ColliderShape::Box;
        col.halfExtents = { 2.0f, 2.0f, 2.0f };   // deliberately not terrain-shaped
        world.addComponent(land, col);
    }

    PhysicsWorld phys;
    phys.initialize(world);
    REQUIRE(phys.hasPhysics(static_cast<uint32_t>(land)));

    // The authored 4 m box sits at the origin; the terrain would have covered
    // the whole 64 m square. A ray far out in the corner therefore hits nothing
    // — which is only true if exactly ONE body was built, and it was the
    // authored one.
    const float far_ = tc.sizeX * 0.5f - 4.0f;
    CHECK_FALSE(phys.raycast({ far_, 200.0f, far_ }, { 0.0f, -1.0f, 0.0f }, 400.0f).hit);

    // And the authored box is really there.
    CHECK(phys.raycast({ 0.0f, 200.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 400.0f).hit);
}

TEST_CASE("Sculpting a landscape moves its collider with it")
{
    HorizonWorld     world;
    TerrainComponent tc;
    Entity           land = makeLandscape(world, tc);

    PhysicsWorld phys;
    phys.initialize(world);
    REQUIRE(phys.hasPhysics(static_cast<uint32_t>(land)));

    const glm::vec3 from{ 0.0f, 200.0f, 0.0f };
    const glm::vec3 down{ 0.0f, -1.0f, 0.0f };
    const auto before = phys.raycast(from, down, 400.0f);
    REQUIRE(before.hit);

    // Sculpt the whole landscape up to a flat plateau the way a stroke would:
    // write sculptHeights on the component. These are ABSOLUTE heights that
    // override the noise, not an offset added to it — so the plateau is put well
    // above the terrain's own 0..heightScale range to make the move unambiguous.
    const float plateau = tc.heightScale + 40.0f;
    auto& live = world.registry().get<TerrainComponent>(land);
    live.sculptHeights.assign(static_cast<size_t>(live.resolution) * live.resolution, plateau);
    REQUIRE(before.point.y < plateau - 1.0f);

    // The collider does NOT follow on its own — nothing polls the component.
    CHECK(phys.raycast(from, down, 400.0f).point.y == doctest::Approx(before.point.y));

    // addEntity is the refresh, and it REPLACES rather than adds: if it stacked a
    // second height field, the ray would still stop on the old surface below.
    CHECK(phys.addEntity(world, static_cast<uint32_t>(land)));
    const auto after = phys.raycast(from, down, 400.0f);
    REQUIRE(after.hit);
    CHECK(after.entityId == static_cast<uint32_t>(land));
    CHECK(after.point.y == doctest::Approx(plateau).epsilon(0.01));
    CHECK(after.point.y > before.point.y + 1.0f);
}

// Red under two independent source mutations, both applied, built and observed:
//   - disabling the `if (physics) g_pendingTerrainColliders[...] = true` queueing
//     in TerrainSystem::updateTerrains: the collider never catches up at all.
//   - disabling the latch's `if (it->second)` skip so the flush rebuilds in the
//     same tick: the TICK ONE assertion below fails, which is what keeps that
//     assertion from being decoration.
TEST_CASE("The terrain tick pulls the collider after a sculpt, without anyone calling addEntity")
{
    // The ANSCHLUSS for terrain. The test above proves addEntity refreshes a
    // height field when something calls it; this one proves the SYSTEM calls it,
    // which is the only version of the claim a player is affected by. Nothing
    // here touches PhysicsWorld except to read it back.
    HorizonWorld     world;
    ContentManager   cm(".");
    TerrainComponent tc;
    Entity           land = makeLandscape(world, tc);

    PhysicsWorld phys;
    phys.initialize(world);
    REQUIRE(phys.hasPhysics(static_cast<uint32_t>(land)));

    const glm::vec3 from{ 0.0f, 200.0f, 0.0f };
    const glm::vec3 down{ 0.0f, -1.0f, 0.0f };
    const auto before = phys.raycast(from, down, 400.0f);
    REQUIRE(before.hit);

    // A stroke, in the shape the brush leaves behind: absolute heights on the
    // component plus the dirty flag that tells the tick to regenerate.
    const float plateau = tc.heightScale + 40.0f;
    {
        auto& live = world.registry().get<TerrainComponent>(land);
        live.sculptHeights.assign(static_cast<size_t>(live.resolution) * live.resolution, plateau);
        live.dirty = true;
    }
    REQUIRE(before.point.y < plateau - 1.0f);

    // TICK ONE — the brush frame. The meshes regenerate, and the collider is
    // deliberately left alone: a full height field per brush frame is what makes
    // a stroke stutter, so the rebuild is queued instead. Stale collision while
    // the brush is down is the documented trade, and pinning it here means a
    // future change to that policy has to come past this assertion.
    TerrainSystem::updateTerrains(world, cm, nullptr, &phys);
    CHECK(phys.raycast(from, down, 400.0f).point.y == doctest::Approx(before.point.y));

    // TICK TWO — the brush has lifted and nothing dirtied the terrain again, so
    // the queued rebuild flushes. BEFORE THE CHANGE there was no physics-aware
    // overload at all and the collision surface stayed on the pre-stroke shape
    // for the rest of the session.
    TerrainSystem::updateTerrains(world, cm, nullptr, &phys);
    const auto after = phys.raycast(from, down, 400.0f);
    REQUIRE(after.hit);
    CHECK(after.entityId == static_cast<uint32_t>(land));
    CHECK(after.point.y == doctest::Approx(plateau).epsilon(0.01));
    CHECK(after.point.y > before.point.y + 1.0f);

    // A third tick with nothing pending must not undo it — the queue is drained,
    // not re-armed.
    TerrainSystem::updateTerrains(world, cm, nullptr, &phys);
    CHECK(phys.raycast(from, down, 400.0f).point.y == doctest::Approx(plateau).epsilon(0.01));
}

// ── The top of the 2ⁿ+1 ladder ─────────────────────────────────────────────────
// A resolution of 513..1024 snaps UP to 1025. The clamp every consumer used was
// 1024, so the 1025² height field was read as a 1024² grid, never matched, and the
// landscape came out flat — while the snap ran again every frame and rebuilt and
// re-uploaded all 256 chunks each time.
TEST_CASE("terrain: a resolution that snaps to 1025 keeps its heights and settles")
{
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 100.0f;
    tc.resolution = 1024;
    tc.seed = 0;
    TerrainSculpt::ensureHeights(tc);
    CHECK(tc.resolution == 1025);
    REQUIRE(tc.sculptHeights.size() == 1025u * 1025u);

    tc.sculptHeights[5 * 1025 + 7] = 12.0f;
    const std::vector<float> field = computeTerrainHeightField(tc);
    REQUIRE(field.size() == 1025u * 1025u);
    CHECK(field[5 * 1025 + 7] == doctest::Approx(12.0f));

    // Settled: snapping again changes nothing and asks for no rebuild.
    tc.dirty = false;
    TerrainSculpt::ensureHeights(tc);
    CHECK(tc.resolution == 1025);
    CHECK_FALSE(tc.dirty);
}

TEST_CASE("terrain: updateTerrains on a 1024 landscape builds the sculpted heights and then rests")
{
    HorizonWorld world;
    ContentManager cm(".");
    const auto e = world.createEntity("Landscape");
    world.registry().emplace<TransformComponent>(e);
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 100.0f;
    tc.resolution = 1024;
    tc.seed = 0;
    TerrainSculpt::ensureHeights(tc);
    tc.sculptHeights[512 * 1025 + 512] = 25.0f;
    tc.dirty = true;
    world.registry().emplace<TerrainComponent>(e, tc);

    TerrainSystem::updateTerrains(world, cm);
    const auto& after = world.registry().get<TerrainComponent>(e);
    CHECK(after.resolution == 1025);
    // Rested: nothing is pending, so the next tick does not rebuild.
    CHECK_FALSE(after.dirty);
    CHECK_FALSE(after.regionDirty);
    TerrainSystem::updateTerrains(world, cm);
    CHECK_FALSE(world.registry().get<TerrainComponent>(e).dirty);
}

// Texture groups (Thema 233 step 3): the group is the terrain's MATERIAL, the weightmap is the same
// one in every group (channel k = role k). A scene saved before groups names Mountains' id and reads
// back as Mountains; switching the id to Forest's changes no byte of the terrain component.
TEST_CASE("Texture groups: the weightmap does not depend on the group, a pre-group scene reads as Mountains")
{
    HorizonWorld world;
    Entity e = world.createEntity("Landscape");
    TerrainComponent tc;
    tc.sizeX = tc.sizeZ = 32.0f;
    tc.weightRes = 16;
    TerrainPaint::ensureWeightmap(tc);
    TerrainPaint::paint(tc, 4.0f, -3.0f, 2, 6.0f, 2.0f, 0.7f);
    TerrainPaint::paint(tc, -5.0f, 6.0f, 4, 5.0f, 3.0f, 0.9f);   // layer 4: the second page
    world.registry().emplace<TerrainComponent>(e, tc);
    MaterialComponent mc;
    mc.materialAssetId = HE::kAutoLandscapeMaterialId;           // what every earlier scene stored
    world.registry().emplace<MaterialComponent>(e, mc);

    SceneSerializer ser;
    std::vector<uint8_t> before;
    REQUIRE(ser.saveToMemory(world, before));

    HorizonWorld w2;
    REQUIRE(ser.loadFromMemory(w2, before));
    const TerrainComponent* l = onlyTerrain(w2);
    REQUIRE(l != nullptr);
    CHECK(l->layerWeights  == tc.layerWeights);
    CHECK(l->layerWeights2 == tc.layerWeights2);
    for (auto ent : w2.registry().view<TerrainComponent, MaterialComponent>())
    {
        const HE::UUID id = w2.registry().get<MaterialComponent>(ent).materialAssetId;
        CHECK(id == HE::kAutoLandscapeMaterialId);
        CHECK(HE::autoLandscapeGroupOfMaterial(id) == static_cast<int>(HE::AutoLandscapeGroupId::Mountains));
    }

    // Bind the terrain to Forest: the terrain component's JSON is the same, byte for byte.
    nlohmann::json sceneMountains = nlohmann::json::from_cbor(before);
    world.registry().get<MaterialComponent>(e).materialAssetId =
        HE::autoLandscapeGroup(HE::AutoLandscapeGroupId::Forest).materialId;
    std::vector<uint8_t> after;
    REQUIRE(ser.saveToMemory(world, after));
    nlohmann::json sceneForest = nlohmann::json::from_cbor(after);
    REQUIRE(terrainJsonOf(sceneMountains) != nullptr);
    REQUIRE(terrainJsonOf(sceneForest) != nullptr);
    CHECK(*terrainJsonOf(sceneForest) == *terrainJsonOf(sceneMountains));

    HorizonWorld w3;
    REQUIRE(ser.loadFromMemory(w3, after));
    const TerrainComponent* f = onlyTerrain(w3);
    REQUIRE(f != nullptr);
    CHECK(f->layerWeights  == tc.layerWeights);
    CHECK(f->layerWeights2 == tc.layerWeights2);
    for (auto ent : w3.registry().view<TerrainComponent, MaterialComponent>())
        CHECK(HE::autoLandscapeGroupOfMaterial(w3.registry().get<MaterialComponent>(ent).materialAssetId)
              == static_cast<int>(HE::AutoLandscapeGroupId::Forest));
}
