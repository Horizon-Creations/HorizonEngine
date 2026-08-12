#include "doctest.h"
#include <algorithm>
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


TEST_CASE("HorizonWorld: un-parent a child back to the World root")
{
    HorizonWorld world;
    Entity parent = world.createEntity("Parent");
    Entity child  = world.createEntity("Child");
    REQUIRE(world.reparentEntity(child, parent));
    auto& reg = world.registry();
    CHECK(reg.get<HierarchyComponent>(child).parent == parent);

    // Dragging an entity onto the outliner background reparents it to the root. The
    // root is a built-in, but reparenting TO it (detaching to the top level) is allowed.
    REQUIRE(world.reparentEntity(child, world.rootEntity()));
    CHECK(reg.get<HierarchyComponent>(child).parent == world.rootEntity());
    auto& pch = reg.get<HierarchyComponent>(parent).children;
    CHECK(std::find(pch.begin(), pch.end(), child) == pch.end());       // gone from old parent
    auto& rch = reg.get<HierarchyComponent>(world.rootEntity()).children;
    CHECK(std::find(rch.begin(), rch.end(), child) != rch.end());        // now under the root
}

// ─── Prefab: nothing is left double-parented to the World root ───────────────
// createEntity() hangs every new entity off the World root, and prefab
// instantiation builds the authored links on top of that. It used to leave the
// root's own child list alone, so every prefab CHILD stayed listed under the root
// as well as under its real parent: it appeared twice in the Outliner, rendered
// through a transform chain nobody authored, and the double link went straight
// into the .hescene on the next save. A full scene load never had the problem —
// there the root is one of the serialised records, so its child list is cleared
// and restored like any other parent's.

namespace
{
    // How OFTEN an entity is listed under the World root. A plain "is it there"
    // check cannot see the bug this guards: the stale entry sits next to the
    // legitimate one, and for a prefab child the count has to be zero, not one.
    size_t rootChildCount(HorizonWorld& world, Entity e)
    {
        const auto& rc =
            world.registry().get<HierarchyComponent>(world.rootEntity()).children;
        return static_cast<size_t>(std::count(rc.begin(), rc.end(), e));
    }

    // A "Turret" with a "Barrel" and a "Base" under it — the smallest prefab that
    // has both a root and non-root entities.
    std::vector<uint8_t> makeTurretPrefab(HorizonWorld& world, SceneSerializer& ser)
    {
        Entity turret = world.createEntity("Turret");
        Entity barrel = world.createEntity("Barrel");
        Entity base   = world.createEntity("Base");
        REQUIRE(world.reparentEntity(barrel, turret));
        REQUIRE(world.reparentEntity(base,   turret));
        return ser.serializeSubtree(world, turret);
    }
}

TEST_CASE("Prefab: instantiation attaches only the prefab root to the World root")
{
    HorizonWorld world;
    SceneSerializer ser;
    auto data = makeTurretPrefab(world, ser);

    Entity instRoot = ser.instantiatePrefab(world, data);

    auto& reg = world.registry();
    REQUIRE(reg.valid(instRoot));
    CHECK(reg.get<NameComponent>(instRoot).name == "Turret");

    // The instance root is the one and only thing the drop adds at the top level…
    CHECK(reg.get<HierarchyComponent>(instRoot).parent == world.rootEntity());
    CHECK(rootChildCount(world, instRoot) == 1);

    // …and the children hang off it exclusively, in the authored order.
    auto& kids = reg.get<HierarchyComponent>(instRoot).children;
    REQUIRE(kids.size() == 2);
    CHECK(reg.get<NameComponent>(kids[0]).name == "Barrel");
    CHECK(reg.get<NameComponent>(kids[1]).name == "Base");
    for (Entity child : kids)
    {
        CHECK(reg.get<HierarchyComponent>(child).parent == instRoot);
        CHECK(rootChildCount(world, child) == 0);
    }
}

TEST_CASE("Prefab: dropping onto a parent leaves nothing under the World root")
{
    // The viewport drop handler passes the entity under the cursor as the parent;
    // this is the path where a leftover root link is most visible, because the
    // whole instance is supposed to be somewhere else entirely.
    HorizonWorld world;
    SceneSerializer ser;
    auto data = makeTurretPrefab(world, ser);

    Entity mount = world.createEntity("Mount");
    Entity instRoot = ser.instantiatePrefab(world, data, mount);

    auto& reg = world.registry();
    REQUIRE(reg.valid(instRoot));
    CHECK(reg.get<HierarchyComponent>(instRoot).parent == mount);
    CHECK(rootChildCount(world, instRoot) == 0);

    auto& mountKids = reg.get<HierarchyComponent>(mount).children;
    CHECK(std::count(mountKids.begin(), mountKids.end(), instRoot) == 1);

    for (Entity child : reg.get<HierarchyComponent>(instRoot).children)
        CHECK(rootChildCount(world, child) == 0);
}

TEST_CASE("Prefab: instantiating twice produces two independent subtrees")
{
    HorizonWorld world;
    SceneSerializer ser;
    auto data = makeTurretPrefab(world, ser);

    // Explicitly parented to the root — that is how the Content Browser drop and
    // the collaboration replay call it, and it takes a different branch inside
    // reparentEntity than the default (the entity is already there).
    Entity first  = ser.instantiatePrefab(world, data, world.rootEntity());
    Entity second = ser.instantiatePrefab(world, data, world.rootEntity());

    auto& reg = world.registry();
    REQUIRE(reg.valid(first));
    REQUIRE(reg.valid(second));
    CHECK(first != second);

    CHECK(rootChildCount(world, first)  == 1);
    CHECK(rootChildCount(world, second) == 1);

    auto& firstKids  = reg.get<HierarchyComponent>(first).children;
    auto& secondKids = reg.get<HierarchyComponent>(second).children;
    REQUIRE(firstKids.size()  == 2);
    REQUIRE(secondKids.size() == 2);

    // Two subtrees, not one subtree shared by two roots.
    for (Entity child : firstKids)
    {
        CHECK(reg.get<HierarchyComponent>(child).parent == first);
        CHECK(rootChildCount(world, child) == 0);
        CHECK(std::find(secondKids.begin(), secondKids.end(), child) == secondKids.end());
    }
    for (Entity child : secondKids)
    {
        CHECK(reg.get<HierarchyComponent>(child).parent == second);
        CHECK(rootChildCount(world, child) == 0);
    }
}
