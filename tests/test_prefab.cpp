#include "doctest.h"
#include <HorizonScene/HorizonScene.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/AudioSourceComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <HorizonScene/Components/NameComponent.h>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>

// ─── Prefab: serializeSubtree / instantiatePrefab ────────────────────────────

TEST_CASE("Prefab: single entity round-trips name and transform")
{
    HorizonWorld world;
    Entity e = world.createEntity("Chair");
    TransformComponent t;
    t.scale = { 2.0f, 2.0f, 2.0f };
    world.addComponent(e, t);

    SceneSerializer ser;
    auto data = ser.serializeSubtree(world, e);
    CHECK(!data.empty());

    Entity inst = ser.instantiatePrefab(world, data);

    auto& reg = world.registry();
    REQUIRE(reg.valid(inst));
    CHECK(reg.get<NameComponent>(inst).name == "Chair");
    auto& tc = reg.get<TransformComponent>(inst);
    CHECK(tc.scale.x == doctest::Approx(2.0f));
    CHECK(tc.scale.y == doctest::Approx(2.0f));
    CHECK(tc.scale.z == doctest::Approx(2.0f));
}

TEST_CASE("Prefab: hierarchy is preserved across round-trip")
{
    HorizonWorld world;
    Entity house = world.createEntity("House");
    Entity wall  = world.createEntity("Wall");
    Entity roof  = world.createEntity("Roof");
    world.reparentEntity(wall, house);
    world.reparentEntity(roof, house);

    SceneSerializer ser;
    auto data = ser.serializeSubtree(world, house);
    CHECK(!data.empty());

    Entity instRoot = ser.instantiatePrefab(world, data);

    auto& reg = world.registry();
    REQUIRE(reg.valid(instRoot));
    CHECK(reg.get<NameComponent>(instRoot).name == "House");

    auto* hier = reg.try_get<HierarchyComponent>(instRoot);
    REQUIRE(hier != nullptr);
    CHECK(hier->children.size() == 2);
    for (Entity child : hier->children)
    {
        REQUIRE(reg.valid(child));
        auto* ch = reg.try_get<HierarchyComponent>(child);
        REQUIRE(ch != nullptr);
        CHECK(ch->parent == instRoot);
    }
}

TEST_CASE("Prefab: instantiate twice produces independent instances")
{
    HorizonWorld world;
    Entity e = world.createEntity("Barrel");
    world.addComponent(e, TransformComponent{});

    SceneSerializer ser;
    auto data = ser.serializeSubtree(world, e);

    Entity inst1 = ser.instantiatePrefab(world, data);
    Entity inst2 = ser.instantiatePrefab(world, data);

    auto& reg = world.registry();
    REQUIRE(reg.valid(inst1));
    REQUIRE(reg.valid(inst2));
    CHECK(inst1 != inst2);
}

TEST_CASE("Prefab: components survive round-trip")
{
    HorizonWorld world;
    Entity e = world.createEntity("Lamp");
    LightComponent light;
    light.intensity  = 3.5f;
    light.range      = 12.0f;
    light.castsShadow = true;
    world.addComponent(e, light);

    AudioSourceComponent audio;
    audio.volume = 0.75f;
    audio.loop   = true;
    world.addComponent(e, audio);

    SceneSerializer ser;
    auto data = ser.serializeSubtree(world, e);
    Entity inst = ser.instantiatePrefab(world, data);

    auto& reg = world.registry();
    REQUIRE(reg.valid(inst));
    auto* lc = reg.try_get<LightComponent>(inst);
    REQUIRE(lc != nullptr);
    CHECK(lc->intensity  == doctest::Approx(3.5f));
    CHECK(lc->range      == doctest::Approx(12.0f));
    CHECK(lc->castsShadow == true);

    auto* ac = reg.try_get<AudioSourceComponent>(inst);
    REQUIRE(ac != nullptr);
    CHECK(ac->volume == doctest::Approx(0.75f));
    CHECK(ac->loop   == true);
}

TEST_CASE("Prefab: data blob is non-empty for any entity")
{
    HorizonWorld world;
    Entity e = world.createEntity("Empty");
    SceneSerializer ser;
    auto data = ser.serializeSubtree(world, e);
    CHECK(data.size() > 4); // at minimum a CBOR-encoded JSON object
}

TEST_CASE("Prefab: corrupt data returns entt::null")
{
    HorizonWorld world;
    SceneSerializer ser;
    std::vector<uint8_t> garbage = { 0xFF, 0xAB, 0x00, 0x01, 0x02 };
    Entity result = ser.instantiatePrefab(world, garbage);
    CHECK(!world.registry().valid(result));
}

// ─── ContentManager: registerPrefab / getPrefab ──────────────────────────────

TEST_CASE("ContentManager: registerPrefab and getPrefab round-trip")
{
    HorizonWorld world;
    Entity e = world.createEntity("Crate");
    world.addComponent(e, TransformComponent{});

    SceneSerializer ser;
    auto data = ser.serializeSubtree(world, e);

    ContentManager cm;
    PrefabAsset pa;
    pa.name = "Crate";
    pa.data = data;
    HE::UUID id = cm.registerPrefab(std::move(pa));

    CHECK(id != HE::UUID{});
    CHECK(cm.isLoaded(id));
    CHECK(cm.assetType(id) == HE::AssetType::Prefab);

    const PrefabAsset* fetched = cm.getPrefab(id);
    REQUIRE(fetched != nullptr);
    CHECK(fetched->name == "Crate");
    CHECK(fetched->data == data);
}

TEST_CASE("ContentManager: getPrefab returns null for unknown UUID")
{
    ContentManager cm;
    HE::UUID id = HE::UUID::generate();
    CHECK(cm.getPrefab(id) == nullptr);
}

TEST_CASE("ContentManager: acquirePrefab pins the asset")
{
    ContentManager cm;
    PrefabAsset pa;
    pa.name = "PinnedPrefab";
    HE::UUID id = cm.registerPrefab(std::move(pa));

    auto ref = cm.acquirePrefab(id);
    CHECK(static_cast<bool>(ref));
    CHECK(cm.isPinned(id));
    // Unload should be refused while pinned
    CHECK(!cm.unloadAsset(id));
}
