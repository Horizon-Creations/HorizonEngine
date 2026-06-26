#include "doctest.h"
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/TerrainMeshGenerator.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/MeshComponent.h>

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
