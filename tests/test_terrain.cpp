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
