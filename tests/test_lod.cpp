#include "doctest.h"
#include "TestFsUtil.h"
#include <HorizonScene/HorizonScene.h>
#include <HorizonScene/LODSystem.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/TransformHierarchy.h>
#include <glm/glm.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

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

// The refined level (terrain tessellation) sits outside `levels`, so levels[0]
// stays LOD0 for everything else that reads it, and is picked first in range.
TEST_CASE("LODSystem picks the refined level first within its distance, and only there")
{
    HorizonWorld world;
    auto& reg = world.registry();
    auto e = world.createEntity("Chunk");
    reg.emplace<TransformComponent>(e, TransformComponent{ .position = glm::vec3(0.f) });
    reg.emplace<MeshComponent>(e, MeshComponent{});

    LODComponent lod;
    lod.levels = { { makeId(1,0), 10.f }, { makeId(2,0), 1e9f } };
    lod.refinedMeshId      = makeId(9,0);
    lod.refinedMaxDistance = 4.f;
    reg.emplace<LODComponent>(e, lod);

    LODSystem::update(world, glm::vec3(3.f, 0.f, 0.f));   // inside 4 → refined
    CHECK(reg.get<MeshComponent>(e).meshAssetId == makeId(9,0));
    CHECK(reg.get<LODComponent>(e).current == LODComponent::kRefined);

    LODSystem::update(world, glm::vec3(6.f, 0.f, 0.f));   // past it → the chain again
    CHECK(reg.get<MeshComponent>(e).meshAssetId == makeId(1,0));
    CHECK(reg.get<LODComponent>(e).current == 0);

    // Unhooked: never chosen, however close.
    reg.get<LODComponent>(e).refinedMeshId = HE::UUID{};
    LODSystem::update(world, glm::vec3(0.f, 0.f, 0.f));
    CHECK(reg.get<MeshComponent>(e).meshAssetId == makeId(1,0));
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

// LODSystem composes a parent's world matrix once for a run of siblings instead
// of walking every chunk's whole chain (SCN-402). That has to be the SAME
// position, to the bit, that worldPositionOf gives: a level boundary is a <=
// on the distance, and a position an ulp off flips an entity standing on one.
// So every entity here gets two thresholds, exactly its distance and the next
// float below it, and must land on the first and miss the second. Only holds
// while the compiler fuses no multiply-add across statements: GCC with Jolt's
// -mfma did (-ffp-contract=fast), see the top-level CMakeLists.txt.
TEST_CASE("LODSystem's world position is worldPositionOf's to the bit, on a level boundary")
{
    HorizonWorld world;
    auto& reg = world.registry();
    uint32_t seed = 12345u;
    auto rnd = [&seed](float lo, float hi) {
        seed = seed * 1664525u + 1013904223u;
        return lo + (hi - lo) * static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    };
    auto randomTransform = [&] {
        TransformComponent t;
        t.position = { rnd(-300.f, 300.f), rnd(-50.f, 50.f), rnd(-300.f, 300.f) };
        t.rotation = { rnd(-180.f, 180.f), rnd(-180.f, 180.f), rnd(-180.f, 180.f) };
        t.scale    = { rnd(0.3f, 3.f), rnd(0.3f, 3.f), rnd(0.3f, 3.f) };
        return t;
    };

    // Parents up to three deep, then LOD entities under them: in runs of
    // siblings (the case the memo serves), alternating between parents (the
    // case it has to recompute), at the top level, and one without a transform.
    std::vector<Entity> parents;
    for (int i = 0; i < 12; ++i)
    {
        const Entity p = world.createEntity("Parent");
        reg.emplace_or_replace<TransformComponent>(p, randomTransform());
        if (i >= 4) world.reparentEntity(p, parents[static_cast<size_t>(i - 4)]);
        parents.push_back(p);
    }
    const glm::vec3 cam(17.f, 5.f, -23.f);
    std::vector<Entity> onEdge, belowEdge;
    for (int i = 0; i < 400; ++i)
    {
        Entity parent = entt::null;
        if (i < 240)      parent = parents[static_cast<size_t>(i / 20)];   // runs
        else if (i < 380) parent = parents[static_cast<size_t>(i % 12)];   // alternating
        for (int twin = 0; twin < 2; ++twin)
        {
            const Entity e = world.createEntity("Lod");
            if (i != 399) reg.emplace_or_replace<TransformComponent>(e, randomTransform());
            else          reg.remove<TransformComponent>(e);
            if (parent != entt::null) world.reparentEntity(e, parent);
            reg.emplace<MeshComponent>(e, MeshComponent{});
            (twin == 0 ? onEdge : belowEdge).push_back(e);
        }
        // The twin is an exact copy, so both stand at the same world position.
        if (const auto* t = reg.try_get<TransformComponent>(onEdge.back()))
            reg.emplace_or_replace<TransformComponent>(belowEdge.back(), *t);
    }
    // Half the world with fresh local caches, half without: cachedLocalMatrix
    // takes either branch, and both must agree with the walk.
    HE::propagateTransforms(world);
    for (size_t i = 0; i < onEdge.size(); i += 2)
        if (auto* t = reg.try_get<TransformComponent>(onEdge[i])) t->position.x += 1e-3f;
    for (size_t i = 0; i < onEdge.size(); ++i)
        if (const auto* t = reg.try_get<TransformComponent>(onEdge[i]))
            reg.emplace_or_replace<TransformComponent>(belowEdge[i], *t);

    for (size_t i = 0; i < onEdge.size(); ++i)
    {
        const float d = glm::distance(cam, HE::worldPositionOf(world, onEdge[i]));
        LODComponent on;
        on.levels = { { makeId(1, 0), d }, { makeId(2, 0), 1e30f } };
        reg.emplace<LODComponent>(onEdge[i], on);
        LODComponent below;
        below.levels = { { makeId(1, 0), std::nextafter(d, 0.0f) }, { makeId(2, 0), 1e30f } };
        reg.emplace<LODComponent>(belowEdge[i], below);
    }

    // Which side went wrong tells the cause apart: twins at different positions
    // (a fresh local matrix against propagateTransforms' cached one) put the
    // below threshold on the wrong twin; a wrong onEdge with equal twins means
    // LODSystem's position is not worldPositionOf's.
    auto bitsOf = [](const glm::vec3& v) {
        uint32_t b[3];
        std::memcpy(b, &v, sizeof(b));
        char s[40];
        std::snprintf(s, sizeof(s), "%08x %08x %08x", b[0], b[1], b[2]);
        return std::string(s);
    };
    // Strings, not literals out of a ?: : doctest under GCC streamed those as "1".
    auto kindOf = [](size_t i) {
        return std::string(i < 240 ? "run" : i < 380 ? "alternating" : "top") + (i % 2 == 0 ? ", moved" : ", cached");
    };
    size_t twinDiffers = 0;
    for (size_t i = 0; i < onEdge.size(); ++i)
    {
        const glm::vec3 a = HE::worldPositionOf(world, onEdge[i]);
        const glm::vec3 b = HE::worldPositionOf(world, belowEdge[i]);
        if (std::memcmp(&a, &b, sizeof(a)) == 0) continue;
        if (++twinDiffers <= 3)
            MESSAGE("twin " << i << " (" << kindOf(i) << "): " << bitsOf(a) << " vs " << bitsOf(b));
    }

    LODSystem::update(world, cam);
    size_t wrongOn = 0, wrongBelow = 0;
    for (size_t i = 0; i < onEdge.size(); ++i)
    {
        const bool on    = reg.get<LODComponent>(onEdge[i]).current != 0;
        const bool below = reg.get<LODComponent>(belowEdge[i]).current != 1;
        if ((on || below) && wrongOn + wrongBelow < 3)
            MESSAGE("wrong " << i << std::string(on ? " onEdge" : "") << std::string(below ? " belowEdge" : "")
                    << " (" << kindOf(i) << ")");
        wrongOn    += on;
        wrongBelow += below;
    }
    CHECK(twinDiffers == 0);
    CHECK(wrongOn == 0);
    CHECK(wrongBelow == 0);
}
