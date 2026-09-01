#include "doctest.h"
#include "TestFsUtil.h"
#include <HorizonScene/HorizonScene.h>
#include <HorizonScene/LODSystem.h>
#include <HorizonScene/SceneSerializer.h>
#include <glm/glm.hpp>
#include <filesystem>

static HE::UUID makeId(uint64_t hi, uint64_t lo) { return {hi, lo}; }

// ─── LOD selection ────────────────────────────────────────────────────────────

TEST_CASE("LODSystem selects nearest level when camera is close")
{
    HorizonWorld world;
    auto& reg = world.registry();
    auto e = world.createEntity("Mesh");
    reg.emplace<TransformComponent>(e, TransformComponent{ .position = glm::vec3(0.f) });
    reg.emplace<MeshComponent>(e, MeshComponent{});

    LODComponent lod;
    lod.levels = {
        { makeId(1,0), 10.f  }, // LOD0: use when dist <= 10
        { makeId(2,0), 50.f  }, // LOD1: use when dist <= 50
        { makeId(3,0), 1e9f  }, // LOD2: fallback
    };
    reg.emplace<LODComponent>(e, lod);

    LODSystem::update(world, glm::vec3(5.f, 0.f, 0.f)); // dist=5 → LOD0
    CHECK(reg.get<MeshComponent>(e).meshAssetId == makeId(1,0));
    CHECK(reg.get<LODComponent>(e).current == 0);
}

TEST_CASE("LODSystem switches to lower LOD at greater distance")
{
    HorizonWorld world;
    auto& reg = world.registry();
    auto e = world.createEntity("Mesh");
    reg.emplace<TransformComponent>(e, TransformComponent{ .position = glm::vec3(0.f) });
    reg.emplace<MeshComponent>(e, MeshComponent{});

    LODComponent lod;
    lod.levels = {
        { makeId(10,0), 10.f },
        { makeId(20,0), 50.f },
        { makeId(30,0), 1e9f },
    };
    reg.emplace<LODComponent>(e, lod);

    LODSystem::update(world, glm::vec3(25.f, 0.f, 0.f)); // dist=25 → LOD1
    CHECK(reg.get<MeshComponent>(e).meshAssetId == makeId(20,0));
    CHECK(reg.get<LODComponent>(e).current == 1);

    LODSystem::update(world, glm::vec3(100.f, 0.f, 0.f)); // dist=100 → LOD2
    CHECK(reg.get<MeshComponent>(e).meshAssetId == makeId(30,0));
    CHECK(reg.get<LODComponent>(e).current == 2);
}

TEST_CASE("LODSystem marks mesh dirty on LOD change")
{
    HorizonWorld world;
    auto& reg = world.registry();
    auto e = world.createEntity("Mesh");
    reg.emplace<TransformComponent>(e, TransformComponent{ .position = glm::vec3(0.f) });
    MeshComponent mc{}; mc.dirty = false;
    reg.emplace<MeshComponent>(e, mc);

    LODComponent lod;
    lod.levels = {
        { makeId(1,0), 5.f  },
        { makeId(2,0), 1e9f },
    };
    reg.emplace<LODComponent>(e, lod);

    LODSystem::update(world, glm::vec3(1.f, 0.f, 0.f)); // dist=1 → LOD0
    CHECK(reg.get<MeshComponent>(e).dirty == true);
}

TEST_CASE("LODSystem does not change mesh if same LOD level")
{
    HorizonWorld world;
    auto& reg = world.registry();
    auto e = world.createEntity("Mesh");
    reg.emplace<TransformComponent>(e, TransformComponent{ .position = glm::vec3(0.f) });
    reg.emplace<MeshComponent>(e, MeshComponent{});

    LODComponent lod;
    lod.current = 0;
    lod.levels = { { makeId(1,0), 100.f } };
    reg.emplace<LODComponent>(e, lod);

    LODSystem::update(world, glm::vec3(5.f, 0.f, 0.f));
    LODSystem::update(world, glm::vec3(10.f, 0.f, 0.f));
    CHECK(reg.get<MeshComponent>(e).meshAssetId == makeId(1,0));
    CHECK(reg.get<LODComponent>(e).current == 0);
}

TEST_CASE("LODSystem ignores entities without LODComponent")
{
    HorizonWorld world;
    auto& reg = world.registry();
    auto e = world.createEntity("Plain Mesh");
    const HE::UUID origMesh = makeId(99,99);
    reg.emplace<MeshComponent>(e, MeshComponent{ .meshAssetId = origMesh });

    LODSystem::update(world, glm::vec3(0.f));
    CHECK(reg.get<MeshComponent>(e).meshAssetId == origMesh);
}

TEST_CASE("LODSystem handles LODComponent with empty levels gracefully")
{
    HorizonWorld world;
    auto& reg = world.registry();
    auto e = world.createEntity("Empty LOD");
    reg.emplace<MeshComponent>(e, MeshComponent{});
    reg.emplace<LODComponent>(e, LODComponent{});

    LODSystem::update(world, glm::vec3(0.f)); // must not crash
}

TEST_CASE("LODSystem uses entity world position for distance")
{
    HorizonWorld world;
    auto& reg = world.registry();

    // Entity at world (100, 0, 0), camera at origin → distance 100. LODSystem
    // composes the WORLD position from the parent chain; under the world root that
    // chain is the entity's own local matrix, so worldMatrix is deliberately left
    // untouched here — it is derived state and nothing has propagated it.
    auto e = world.createEntity("Distant Mesh");
    reg.emplace<TransformComponent>(e, TransformComponent{ .position = glm::vec3(100.f, 0.f, 0.f) });
    reg.emplace<MeshComponent>(e, MeshComponent{});

    LODComponent lod;
    lod.levels = {
        { makeId(1,0), 10.f   }, // LOD0: dist <= 10
        { makeId(2,0), 1000.f }, // LOD1: dist <= 1000
    };
    reg.emplace<LODComponent>(e, lod);

    LODSystem::update(world, glm::vec3(0.f));
    CHECK(reg.get<LODComponent>(e).current == 1); // 100 > 10 → LOD1
}

TEST_CASE("LODSystem measures an entity spawned THIS frame where it actually is")
{
    // The one-frame trap. worldMatrix is derived state, written by
    // propagateTransforms — and nothing in SceneSystems::tickWorld runs one, so an
    // entity created during this frame still carries the identity there. Reading it
    // would place every fresh entity at the world origin for one frame.
    //
    // No propagate anywhere in this test, on purpose: this IS the state of a scene
    // the frame it spawns something.
    HorizonWorld world;
    auto& reg = world.registry();

    auto e = world.createEntity("Freshly Spawned");
    reg.emplace<TransformComponent>(e, TransformComponent{ .position = glm::vec3(500.f, 0.f, 0.f) });
    reg.emplace<MeshComponent>(e, MeshComponent{});
    REQUIRE(reg.get<TransformComponent>(e).worldMatrix == glm::mat4(1.f)); // nothing propagated it

    LODComponent lod;
    lod.levels = {
        { makeId(1,0), 10.f }, // LOD0: dist <= 10
        { makeId(2,0), 1e9f }, // LOD1: fallback
    };
    reg.emplace<LODComponent>(e, lod);

    // Camera sits ON the entity → distance 0 → the finest level. Reading the stale
    // worldMatrix would measure 500 instead and drop straight to the fallback.
    LODSystem::update(world, glm::vec3(500.f, 0.f, 0.f));
    CHECK(reg.get<LODComponent>(e).current == 0);
    CHECK(reg.get<MeshComponent>(e).meshAssetId == makeId(1,0));
}

TEST_CASE("LODSystem composes the parent chain for a chunk that has not propagated")
{
    // The terrain case, which is the one that actually happens in the engine:
    // TerrainSystem creates the chunk entities at the top of tickWorld and LOD
    // picks their level at the bottom of the SAME tick. A chunk is a CHILD, so its
    // local position is only the per-chunk offset — the world position needs the
    // parent chain, and worldMatrix has not been written on either of them yet.
    HorizonWorld world;
    auto& reg = world.registry();

    auto terrain = world.createEntity("Terrain");
    reg.emplace<TransformComponent>(terrain, TransformComponent{ .position = glm::vec3(0.f, 0.f, 300.f) });

    auto chunk = world.createEntity("TerrainChunk");
    reg.emplace<TransformComponent>(chunk, TransformComponent{ .position = glm::vec3(0.f, 0.f, 20.f) });
    reg.emplace<MeshComponent>(chunk, MeshComponent{});
    REQUIRE(world.reparentEntity(chunk, terrain));

    LODComponent lod;
    lod.levels = {
        { makeId(7,0), 50.f }, // LOD0: dist <= 50
        { makeId(8,0), 1e9f }, // LOD1: fallback
    };
    reg.emplace<LODComponent>(chunk, lod);

    // Chunk world position is (0,0,320), camera 30 away → inside LOD0's 50. The
    // local position alone (20) or a stale identity (0) both put it out of range.
    LODSystem::update(world, glm::vec3(0.f, 0.f, 350.f));
    CHECK(reg.get<LODComponent>(chunk).current == 0);
    CHECK(reg.get<MeshComponent>(chunk).meshAssetId == makeId(7,0));
}

// ─── Serialisation round-trip ─────────────────────────────────────────────────

TEST_CASE("LODComponent serializes and deserializes correctly")
{
    HorizonWorld world;
    auto& reg = world.registry();
    auto e = world.createEntity("LOD Entity");

    LODComponent lod;
    lod.levels = {
        { makeId(0xDEAD,0xBEEF), 15.0f  },
        { makeId(0x1111,0x2222), 80.0f  },
        { makeId(0x3333,0x4444), 500.0f },
    };
    reg.emplace<LODComponent>(e, lod);

    auto tmp = std::filesystem::temp_directory_path() / "he_test_lod.hescene";
    SceneSerializer ser;
    REQUIRE(ser.save(world, tmp, HE::SerializeFormat::JSON));

    HorizonWorld world2;
    REQUIRE(ser.load(world2, tmp, HE::SerializeFormat::JSON));

    bool found = false;
    world2.registry().view<LODComponent>().each([&](auto, const LODComponent& l) {
        found = true;
        REQUIRE(l.levels.size() == 3);
        CHECK(l.levels[0].meshId == makeId(0xDEAD,0xBEEF));
        CHECK(l.levels[0].maxDistance == doctest::Approx(15.0f));
        CHECK(l.levels[2].meshId == makeId(0x3333,0x4444));
    });
    CHECK(found);

    he_test::removeQuiet(tmp);
}
