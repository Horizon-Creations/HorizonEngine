#include "doctest.h"
#include <algorithm>
#include <HorizonScene/HorizonScene.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/AudioSourceComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/EntityIdComponent.h>
#include <HorizonScene/Components/PrefabInstanceComponent.h>
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

// ─── The identity a subtree blob carries ─────────────────────────────────────
// Collaboration reuses serializeSubtree as its "create" payload and instantiates
// it with preserveIds=true, because every later edit — a transform, a component
// change, a delete — travels addressed by the entity's UUID. The blob did not
// carry one: it wrote a sequential index and nothing else, so the receiver
// stamped {hi:0, lo:1}, {hi:0, lo:2} … onto the children of every replicated
// prefab. Those answered to different ids on the two machines, and two received
// prefabs gave one editor two entities both claiming id 1.

TEST_CASE("Prefab: a subtree blob carries the real entity UUIDs")
{
    HorizonWorld world;
    const Entity root  = world.createEntity("Robot");
    const Entity armL  = world.createEntity("ArmL");
    const Entity handL = world.createEntity("HandL");
    REQUIRE(world.reparentEntity(armL,  root));
    REQUIRE(world.reparentEntity(handL, armL));

    auto& reg = world.registry();
    const auto uuidOf = [&reg](Entity e) {
        const auto* c = reg.try_get<EntityIdComponent>(e);
        return c ? c->id : HE::UUID{};
    };

    SceneSerializer ser;
    const auto blob = ser.serializeSubtree(world, root);
    REQUIRE(!blob.empty());

    // Serialising mints the ids if they were missing, so read them afterwards.
    const HE::UUID rootId = uuidOf(root), armId = uuidOf(armL), handId = uuidOf(handL);
    REQUIRE(rootId != HE::UUID{});
    REQUIRE(armId  != HE::UUID{});
    REQUIRE(handId != HE::UUID{});
    // Distinct, or the whole exercise is pointless.
    REQUIRE(armId != handId);

    // The receiving side of a replicated create: a second world, preserving ids.
    HorizonWorld peer;
    const Entity peerRoot = ser.instantiatePrefab(peer, blob, entt::null,
                                                  /*preserveIds=*/true);
    REQUIRE((peerRoot != entt::null));

    auto& preg = peer.registry();
    const auto findByUuid = [&preg](const HE::UUID& id) {
        Entity found = entt::null;
        preg.view<EntityIdComponent>().each([&](auto e, const EntityIdComponent& c) {
            if (c.id == id) found = e;
        });
        return found;
    };

    // Every entity, root included. The root used to be skipped outright: its
    // sequential index was 0, which read back as the all-zero UUID and failed
    // the "is there an id here" guard.
    CHECK(findByUuid(rootId) == peerRoot);
    const Entity peerArm  = findByUuid(armId);
    const Entity peerHand = findByUuid(handId);
    REQUIRE((peerArm  != entt::null));
    REQUIRE((peerHand != entt::null));

    // And the hierarchy still stands. The links are UUID references now, so a
    // blob that kept the old ordinal refs would restore nothing at all — every
    // child would sit at the top level.
    const auto* armHier  = preg.try_get<HierarchyComponent>(peerArm);
    const auto* handHier = preg.try_get<HierarchyComponent>(peerHand);
    REQUIRE(armHier  != nullptr);
    REQUIRE(handHier != nullptr);
    CHECK(armHier->parent  == peerRoot);
    CHECK(handHier->parent == peerArm);

    const auto* nameArm = preg.try_get<NameComponent>(peerArm);
    REQUIRE(nameArm != nullptr);
    CHECK(nameArm->name == "ArmL");
}

TEST_CASE("Prefab: a template still mints fresh ids for every instance")
{
    // The other half of the same change. Carrying UUIDs in the blob must NOT
    // turn an ordinary prefab drop into two entities claiming one identity —
    // preserveIds is the sanctioned exception, and it defaults to off.
    HorizonWorld world;
    const Entity root  = world.createEntity("Crate");
    const Entity child = world.createEntity("Lid");
    REQUIRE(world.reparentEntity(child, root));

    SceneSerializer ser;
    const auto blob = ser.serializeSubtree(world, root);
    REQUIRE(!blob.empty());

    auto& reg = world.registry();
    const auto uuidOf = [&reg](Entity e) {
        const auto* c = reg.try_get<EntityIdComponent>(e);
        return c ? c->id : HE::UUID{};
    };
    const HE::UUID rootId = uuidOf(root), childId = uuidOf(child);

    const Entity a = ser.instantiatePrefab(world, blob);
    const Entity b = ser.instantiatePrefab(world, blob);
    REQUIRE((a != entt::null));
    REQUIRE((b != entt::null));
    CHECK(a != b);
    CHECK(uuidOf(a) != rootId);
    CHECK(uuidOf(b) != rootId);
    CHECK(uuidOf(a) != uuidOf(b));

    // Each instance got its own child, parented to its own root.
    const auto* ha = reg.try_get<HierarchyComponent>(a);
    const auto* hb = reg.try_get<HierarchyComponent>(b);
    REQUIRE(ha != nullptr);
    REQUIRE(hb != nullptr);
    REQUIRE(ha->children.size() == 1);
    REQUIRE(hb->children.size() == 1);
    CHECK(ha->children[0] != hb->children[0]);
    CHECK(uuidOf(ha->children[0]) != childId);
    CHECK(uuidOf(ha->children[0]) != uuidOf(hb->children[0]));
}

// ─── PrefabInstanceComponent: bindings and overrides ─────────────────────────
// The component is a record on the ROOT of a placement: which asset, which
// entity is which template record, which properties were edited here. Nothing
// in the serializer applies any of it; what it promises is that the record
// survives a save/load, and that instantiating a blob that carries one leaves
// the bindings pointing at the entities that call was made of.

namespace
{
    HE::UUID idOf(entt::registry& reg, Entity e)
    {
        const auto* c = reg.try_get<EntityIdComponent>(e);
        return c ? c->id : HE::UUID{};
    }
}

TEST_CASE("PrefabInstance: bindings and overrides survive a memory round-trip")
{
    HorizonWorld world;
    const Entity root  = world.createEntity("Lamp");
    const Entity child = world.createEntity("Bulb");
    world.reparentEntity(child, root);
    auto& reg = world.registry();

    PrefabInstanceComponent inst;
    inst.asset = HE::UUID::generate();
    const HE::UUID tRoot = HE::UUID::generate(), tBulb = HE::UUID::generate();
    inst.bindings = { { tRoot, idOf(reg, root) }, { tBulb, idOf(reg, child) } };
    inst.setOverride(tBulb, "light", "intensity");
    inst.setOverride(tBulb, "transform", "position");
    inst.setOverride(tRoot, "mesh");   // whole component
    reg.emplace<PrefabInstanceComponent>(root, inst);

    SceneSerializer ser;
    std::vector<uint8_t> bytes;
    REQUIRE(ser.saveToMemory(world, bytes));
    HorizonWorld loaded;
    REQUIRE(ser.loadFromMemory(loaded, bytes));

    auto& lreg = loaded.registry();
    const PrefabInstanceComponent* got = nullptr;
    for (auto e : lreg.view<PrefabInstanceComponent>()) got = &lreg.get<PrefabInstanceComponent>(e);
    REQUIRE(got != nullptr);
    CHECK(got->asset == inst.asset);
    // Same entries, same order — the order is what the Inspector lists.
    CHECK(got->bindings  == inst.bindings);
    CHECK(got->overrides == inst.overrides);
    CHECK(got->instanceOf(tBulb) == idOf(reg, child));
    CHECK(got->templateOf(idOf(reg, root)) == tRoot);
}

TEST_CASE("PrefabInstance: a scene written before bindings existed loads as a plain link")
{
    // Exactly what the previous format wrote: "prefab": { "asset": [hi, lo] }
    // and nothing else. It must load as an instance with empty lists, not be
    // refused and not be dropped.
    HorizonWorld world;
    const Entity root = world.createEntity("Lamp");
    PrefabInstanceComponent link;
    link.asset = HE::UUID::generate();
    world.registry().emplace<PrefabInstanceComponent>(root, link);

    SceneSerializer ser;
    std::vector<uint8_t> bytes;
    REQUIRE(ser.saveToMemory(world, bytes));
    // A link with no lists writes no list keys — that IS the old format, so
    // the round-trip here is the compatibility check, not a tautology.
    HorizonWorld loaded;
    REQUIRE(ser.loadFromMemory(loaded, bytes));
    const PrefabInstanceComponent* got = nullptr;
    for (auto e : loaded.registry().view<PrefabInstanceComponent>())
        got = &loaded.registry().get<PrefabInstanceComponent>(e);
    REQUIRE(got != nullptr);
    CHECK(got->asset == link.asset);
    CHECK(got->bindings.empty());
    CHECK(got->overrides.empty());
}

TEST_CASE("PrefabInstance: instantiatePrefab reports which record became which entity")
{
    HorizonWorld world;
    const Entity root  = world.createEntity("Lamp");
    const Entity child = world.createEntity("Bulb");
    world.reparentEntity(child, root);
    auto& reg = world.registry();
    const HE::UUID tRoot = idOf(reg, root), tBulb = idOf(reg, child);

    SceneSerializer ser;
    const auto blob = ser.serializeSubtree(world, root);

    std::vector<PrefabInstanceComponent::Binding> bindings;
    const Entity placed = ser.instantiatePrefab(world, blob, entt::null, false, &bindings);
    REQUIRE((placed != entt::null));
    // Nothing stamped by the call itself: it also serves paste and duplicate.
    CHECK(reg.try_get<PrefabInstanceComponent>(placed) == nullptr);

    // One per record, template side = the record's uuid (the source entity's
    // id, which is what the asset file will carry), instance side = the new one.
    REQUIRE(bindings.size() == 2);
    PrefabInstanceComponent inst;
    inst.bindings = bindings;
    const HE::UUID newRoot = inst.instanceOf(tRoot), newBulb = inst.instanceOf(tBulb);
    CHECK(newRoot == idOf(reg, placed));
    CHECK(newBulb == idOf(reg, reg.get<HierarchyComponent>(placed).children.at(0)));
    CHECK(newRoot != tRoot);
    CHECK(newBulb != tBulb);
}

TEST_CASE("PrefabInstance: a duplicate of an instance binds to its own children")
{
    HorizonWorld world;
    const Entity root  = world.createEntity("Lamp");
    const Entity child = world.createEntity("Bulb");
    world.reparentEntity(child, root);
    auto& reg = world.registry();

    // The original placement, bound and with an override on the bulb.
    const HE::UUID tRoot = HE::UUID::generate(), tBulb = HE::UUID::generate();
    PrefabInstanceComponent inst;
    inst.asset    = HE::UUID::generate();
    inst.bindings = { { tRoot, idOf(reg, root) }, { tBulb, idOf(reg, child) } };
    inst.setOverride(tBulb, "light", "intensity");
    reg.emplace<PrefabInstanceComponent>(root, inst);

    // Duplicate = serialise the instance, instantiate the blob with fresh ids.
    SceneSerializer ser;
    const auto blob = ser.serializeSubtree(world, root);
    const Entity copy = ser.instantiatePrefab(world, blob);
    REQUIRE((copy != entt::null));
    const Entity copyBulb = reg.get<HierarchyComponent>(copy).children.at(0);

    const auto* dup = reg.try_get<PrefabInstanceComponent>(copy);
    REQUIRE(dup != nullptr);
    CHECK(dup->asset == inst.asset);
    REQUIRE(dup->bindings.size() == 2);
    // Template side untouched, instance side re-pointed at the copy's entities.
    CHECK(dup->instanceOf(tRoot) == idOf(reg, copy));
    CHECK(dup->instanceOf(tBulb) == idOf(reg, copyBulb));
    CHECK(dup->instanceOf(tBulb) != idOf(reg, child));
    // Overrides are keyed by template entity: they travel without translation.
    CHECK(dup->overrides == inst.overrides);
    CHECK(dup->hasOverride(tBulb, "light", "intensity"));

    // The original is what it was.
    const auto* orig = reg.try_get<PrefabInstanceComponent>(root);
    REQUIRE(orig != nullptr);
    CHECK(orig->bindings == inst.bindings);
}

TEST_CASE("PrefabInstance: preserveIds keeps bindings as they are, a deleted child's binding survives")
{
    HorizonWorld world;
    const Entity root  = world.createEntity("Lamp");
    const Entity child = world.createEntity("Bulb");
    world.reparentEntity(child, root);
    auto& reg = world.registry();
    const HE::UUID rootId = idOf(reg, root), childId = idOf(reg, child);

    const HE::UUID tRoot = HE::UUID::generate(), tBulb = HE::UUID::generate(),
                   tGone = HE::UUID::generate();
    PrefabInstanceComponent inst;
    inst.asset    = HE::UUID::generate();
    // A third binding to an entity that is not in the subtree any more: the
    // human deleted that child. That is an override, not a broken entry.
    inst.bindings = { { tRoot, rootId }, { tBulb, childId }, { tGone, HE::UUID::generate() } };
    reg.emplace<PrefabInstanceComponent>(root, inst);

    SceneSerializer ser;
    const auto blob = ser.serializeSubtree(world, root);
    HorizonWorld peer;
    const Entity mirrored = ser.instantiatePrefab(peer, blob, entt::null, /*preserveIds=*/true);
    REQUIRE((mirrored != entt::null));
    const auto* got = peer.registry().try_get<PrefabInstanceComponent>(mirrored);
    REQUIRE(got != nullptr);
    CHECK(got->bindings == inst.bindings);
}

TEST_CASE("PrefabInstance: the override set is idempotent and a whole-component entry covers its properties")
{
    PrefabInstanceComponent inst;
    const HE::UUID t = HE::UUID::generate();
    CHECK(inst.setOverride(t, "light", "intensity"));
    CHECK_FALSE(inst.setOverride(t, "light", "intensity"));
    CHECK(inst.overrides.size() == 1);
    CHECK(inst.hasOverride(t, "light", "intensity"));
    CHECK_FALSE(inst.hasOverride(t, "light", "color"));
    CHECK_FALSE(inst.hasOverride(t, "light"));   // whole component was not claimed

    CHECK(inst.setOverride(t, "light"));         // now it is
    CHECK(inst.hasOverride(t, "light", "color"));
    CHECK(inst.hasOverride(t, "light"));

    CHECK(inst.clearOverride(t, "light", "intensity"));
    CHECK(inst.hasOverride(t, "light", "intensity"));   // still covered by the whole-component entry
    CHECK(inst.clearOverride(t, "light"));              // empty property clears every entry of the component
    CHECK(inst.overrides.empty());
    CHECK_FALSE(inst.clearOverride(t, "light"));
}
