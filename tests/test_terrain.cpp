#include "doctest.h"
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/TerrainMeshGenerator.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/TerrainSystem.h>
#include <HorizonScene/TerrainPaint.h>
#include <HorizonScene/Components/TerrainChunkComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/PhysicsWorld.h>
#include <ContentManager/ContentManager.h>

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

    CHECK_FALSE(TerrainPaint::paint(tc, 0.0f, 0.0f, 4, 4.0f, 0.0f, 1.0f)); // only 4 layers
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
