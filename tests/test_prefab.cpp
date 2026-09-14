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

// ─── Propagation: SceneSerializer::syncPrefabInstance ────────────────────────
// A template world ("the asset") is captured, placed into a scene world with a
// stamped PrefabInstanceComponent, then CHANGED and captured again; the sync
// of the placement against the second blob is what these cases look at.

namespace
{
    // The asset: Lamp (root) → Bulb (light). Kept as a live world so a test can
    // change it and capture it again.
    struct Template
    {
        HorizonWorld world;
        Entity       lamp = entt::null;
        Entity       bulb = entt::null;
        HE::UUID     tLamp, tBulb;

        Template()
        {
            lamp = world.createEntity("Lamp");
            bulb = world.createEntity("Bulb");
            world.reparentEntity(bulb, lamp);
            world.addComponent(lamp, TransformComponent{});
            world.addComponent(bulb, TransformComponent{});
            LightComponent l;
            l.intensity = 1.0f;
            l.range     = 10.0f;
            world.addComponent(bulb, l);
            tLamp = idOf(world.registry(), lamp);
            tBulb = idOf(world.registry(), bulb);
        }
        std::vector<uint8_t> capture()
        {
            SceneSerializer ser;
            return ser.serializeSubtree(world, lamp);
        }
    };

    // The placement, the way the viewport drop makes one: fresh entities,
    // bindings from instantiatePrefab, the component stamped on the root.
    struct Placed
    {
        Entity root = entt::null;
        Entity bulb = entt::null;
    };
    Placed place(HorizonWorld& scene, const std::vector<uint8_t>& blob, const HE::UUID& asset)
    {
        SceneSerializer ser;
        std::vector<PrefabInstanceComponent::Binding> bindings;
        Placed p;
        p.root = ser.instantiatePrefab(scene, blob, entt::null, false, &bindings);
        REQUIRE((p.root != entt::null));
        PrefabInstanceComponent inst;
        inst.asset    = asset;
        inst.bindings = bindings;
        // _or_replace: a template captured from a placement carries the block
        // already, and the drop stamps over it exactly like this.
        scene.registry().emplace_or_replace<PrefabInstanceComponent>(p.root, inst);
        const auto& kids = scene.registry().get<HierarchyComponent>(p.root).children;
        REQUIRE(kids.size() == 1);
        p.bulb = kids.front();
        return p;
    }

    Entity childNamed(HorizonWorld& w, Entity parent, const char* name)
    {
        auto& reg = w.registry();
        for (Entity c : reg.get<HierarchyComponent>(parent).children)
            if (auto* n = reg.try_get<NameComponent>(c); n && n->name == name) return c;
        return entt::null;
    }
}

TEST_CASE("PrefabSync: a changed value reaches the bound child, the root's transform stays where it was placed")
{
    Template t;
    HorizonWorld scene;
    const Placed p = place(scene, t.capture(), HE::UUID::generate());
    auto& reg = scene.registry();
    reg.get<TransformComponent>(p.root).position = { 7.0f, 0.0f, 0.0f };

    // The asset changes: brighter bulb, and its root moved (which no instance follows).
    t.world.registry().get<LightComponent>(t.bulb).intensity = 5.0f;
    t.world.registry().get<TransformComponent>(t.lamp).position = { 0.0f, 99.0f, 0.0f };

    SceneSerializer ser;
    SceneSerializer::PrefabSyncReport rep;
    REQUIRE(ser.syncPrefabInstance(scene, p.root, t.capture(), &rep));
    CHECK(rep.instances == 1);
    CHECK(rep.componentsApplied == 1);   // the bulb's light — nothing else differed
    CHECK(reg.get<LightComponent>(p.bulb).intensity == doctest::Approx(5.0f));
    CHECK(reg.get<TransformComponent>(p.root).position.x == doctest::Approx(7.0f));
    CHECK(reg.get<TransformComponent>(p.root).position.y == doctest::Approx(0.0f));

    // A second sync against the same blob touches nothing.
    SceneSerializer::PrefabSyncReport again;
    REQUIRE(ser.syncPrefabInstance(scene, p.root, t.capture(), &again));
    CHECK(again.componentsApplied == 0);
    CHECK(again.componentsRemoved == 0);
    CHECK(again.entitiesCreated == 0);
}

TEST_CASE("PrefabSync: a property override holds while the sibling property follows the asset")
{
    Template t;
    HorizonWorld scene;
    const Placed p = place(scene, t.capture(), HE::UUID::generate());
    auto& reg = scene.registry();
    reg.get<LightComponent>(p.bulb).intensity = 3.0f;               // authored here …
    reg.get<PrefabInstanceComponent>(p.root).setOverride(t.tBulb, "light", "intensity"); // … and marked

    t.world.registry().get<LightComponent>(t.bulb).intensity = 5.0f;
    t.world.registry().get<LightComponent>(t.bulb).range     = 20.0f;

    SceneSerializer ser;
    SceneSerializer::PrefabSyncReport rep;
    REQUIRE(ser.syncPrefabInstance(scene, p.root, t.capture(), &rep));
    CHECK(reg.get<LightComponent>(p.bulb).intensity == doctest::Approx(3.0f));
    CHECK(reg.get<LightComponent>(p.bulb).range     == doctest::Approx(20.0f));
    CHECK(rep.overridesKept == 1);
    // The override list is what it was: the sync reads it, never edits it.
    CHECK(reg.get<PrefabInstanceComponent>(p.root).overrides.size() == 1);
}

TEST_CASE("PrefabSync: a whole-component override keeps every property of that component")
{
    Template t;
    HorizonWorld scene;
    const Placed p = place(scene, t.capture(), HE::UUID::generate());
    auto& reg = scene.registry();
    reg.get<LightComponent>(p.bulb).range = 3.0f;
    reg.get<PrefabInstanceComponent>(p.root).setOverride(t.tBulb, "light");

    t.world.registry().get<LightComponent>(t.bulb).intensity = 5.0f;
    t.world.registry().get<LightComponent>(t.bulb).range     = 20.0f;

    SceneSerializer ser;
    REQUIRE(ser.syncPrefabInstance(scene, p.root, t.capture()));
    CHECK(reg.get<LightComponent>(p.bulb).intensity == doctest::Approx(1.0f));
    CHECK(reg.get<LightComponent>(p.bulb).range     == doctest::Approx(3.0f));
}

TEST_CASE("PrefabSync: a component the asset gained appears, one it lost goes — unless authored here")
{
    Template t;
    HorizonWorld scene;
    const Placed p = place(scene, t.capture(), HE::UUID::generate());
    auto& reg = scene.registry();

    SUBCASE("gained")
    {
        AudioSourceComponent a;
        a.volume = 0.5f;
        t.world.addComponent(t.bulb, a);
        SceneSerializer ser;
        SceneSerializer::PrefabSyncReport rep;
        REQUIRE(ser.syncPrefabInstance(scene, p.root, t.capture(), &rep));
        const auto* got = reg.try_get<AudioSourceComponent>(p.bulb);
        REQUIRE(got != nullptr);
        CHECK(got->volume == doctest::Approx(0.5f));
        CHECK(rep.componentsApplied == 1);
    }
    SUBCASE("lost")
    {
        t.world.registry().remove<LightComponent>(t.bulb);
        SceneSerializer ser;
        SceneSerializer::PrefabSyncReport rep;
        REQUIRE(ser.syncPrefabInstance(scene, p.root, t.capture(), &rep));
        CHECK(reg.try_get<LightComponent>(p.bulb) == nullptr);
        CHECK(rep.componentsRemoved == 1);
    }
    SUBCASE("lost, but a property of it was authored here")
    {
        reg.get<PrefabInstanceComponent>(p.root).setOverride(t.tBulb, "light", "intensity");
        t.world.registry().remove<LightComponent>(t.bulb);
        SceneSerializer ser;
        SceneSerializer::PrefabSyncReport rep;
        REQUIRE(ser.syncPrefabInstance(scene, p.root, t.capture(), &rep));
        CHECK(reg.try_get<LightComponent>(p.bulb) != nullptr);
        CHECK(rep.componentsRemoved == 0);
        CHECK(rep.overridesKept == 1);
    }
    SUBCASE("added here and marked stays")
    {
        AudioSourceComponent a;
        scene.addComponent(p.bulb, a);
        reg.get<PrefabInstanceComponent>(p.root).setOverride(t.tBulb, "audiosource");
        SceneSerializer ser;
        REQUIRE(ser.syncPrefabInstance(scene, p.root, t.capture()));
        CHECK(reg.try_get<AudioSourceComponent>(p.bulb) != nullptr);
    }
    SUBCASE("added here and NOT marked is what the asset says it is: gone")
    {
        AudioSourceComponent a;
        scene.addComponent(p.bulb, a);
        SceneSerializer ser;
        REQUIRE(ser.syncPrefabInstance(scene, p.root, t.capture()));
        CHECK(reg.try_get<AudioSourceComponent>(p.bulb) == nullptr);
    }
}

TEST_CASE("PrefabSync: a record new in the asset is created once, bound, and parented under its counterpart")
{
    Template t;
    HorizonWorld scene;
    const Placed p = place(scene, t.capture(), HE::UUID::generate());
    auto& reg = scene.registry();

    // The asset grows a Shade under the Bulb.
    const Entity shade = t.world.createEntity("Shade");
    t.world.reparentEntity(shade, t.bulb);
    TransformComponent st;
    st.position = { 0.0f, 2.0f, 0.0f };
    t.world.addComponent(shade, st);
    const HE::UUID tShade = idOf(t.world.registry(), shade);

    SceneSerializer ser;
    SceneSerializer::PrefabSyncReport rep;
    const auto blob = t.capture();
    REQUIRE(ser.syncPrefabInstance(scene, p.root, blob, &rep));
    CHECK(rep.entitiesCreated == 1);
    const Entity got = childNamed(scene, p.bulb, "Shade");
    REQUIRE((got != entt::null));
    CHECK(reg.get<TransformComponent>(got).position.y == doctest::Approx(2.0f));
    const auto& inst = reg.get<PrefabInstanceComponent>(p.root);
    CHECK(inst.instanceOf(tShade) == idOf(reg, got));
    CHECK(inst.bindings.size() == 3);

    // Once. The binding written above is what stops a second pass from
    // creating another one.
    SceneSerializer::PrefabSyncReport again;
    REQUIRE(ser.syncPrefabInstance(scene, p.root, blob, &again));
    CHECK(again.entitiesCreated == 0);
    CHECK(reg.get<HierarchyComponent>(p.bulb).children.size() == 1);
}

TEST_CASE("PrefabSync: a child deleted from the placement is not brought back, and its own children are not either")
{
    Template t;
    HorizonWorld scene;
    const Placed p = place(scene, t.capture(), HE::UUID::generate());
    auto& reg = scene.registry();
    scene.destroyEntity(p.bulb);
    REQUIRE(reg.get<HierarchyComponent>(p.root).children.empty());

    // The asset gains a Shade under the (here deleted) Bulb.
    const Entity shade = t.world.createEntity("Shade");
    t.world.reparentEntity(shade, t.bulb);

    SceneSerializer ser;
    SceneSerializer::PrefabSyncReport rep;
    REQUIRE(ser.syncPrefabInstance(scene, p.root, t.capture(), &rep));
    CHECK(rep.entitiesCreated == 0);
    CHECK(reg.get<HierarchyComponent>(p.root).children.empty());
    // The deletion's binding is still on record.
    CHECK(reg.get<PrefabInstanceComponent>(p.root).bindings.size() == 2);
}

TEST_CASE("PrefabSync: the display name follows the asset unless authored here")
{
    Template t;
    HorizonWorld scene;
    const Placed p = place(scene, t.capture(), HE::UUID::generate());
    auto& reg = scene.registry();
    reg.get<NameComponent>(p.root).name = "Lamp_01";
    reg.get<PrefabInstanceComponent>(p.root).setOverride(t.tLamp, "__name");

    t.world.registry().get<NameComponent>(t.bulb).name = "Filament";

    SceneSerializer ser;
    REQUIRE(ser.syncPrefabInstance(scene, p.root, t.capture()));
    CHECK(reg.get<NameComponent>(p.bulb).name == "Filament");
    CHECK(reg.get<NameComponent>(p.root).name == "Lamp_01");
}

TEST_CASE("PrefabSync: the instance's own link is never overwritten by the template's prefab block")
{
    // A template captured from a placement carries a "prefab" block on its
    // root. Applying it would replace the instance's asset id and bindings.
    Template t;
    PrefabInstanceComponent foreign;
    foreign.asset    = HE::UUID::generate();
    foreign.bindings = { { HE::UUID::generate(), HE::UUID::generate() } };
    t.world.registry().emplace<PrefabInstanceComponent>(t.lamp, foreign);

    HorizonWorld scene;
    const HE::UUID mine = HE::UUID::generate();
    const Placed p = place(scene, t.capture(), mine);
    auto& reg = scene.registry();
    // place() stamped over the copied block; the bindings are ours.
    const auto before = reg.get<PrefabInstanceComponent>(p.root);
    REQUIRE(before.asset == mine);

    SceneSerializer ser;
    REQUIRE(ser.syncPrefabInstance(scene, p.root, t.capture()));
    const auto& after = reg.get<PrefabInstanceComponent>(p.root);
    CHECK(after.asset    == mine);
    CHECK(after.bindings == before.bindings);
}

TEST_CASE("PrefabSync: an instance placed before bindings existed is adopted without a visible change")
{
    Template t;
    HorizonWorld scene;
    auto& reg = scene.registry();
    SceneSerializer ser;
    // The old kind of placement: a copy plus the asset id, no table.
    const Entity root = ser.instantiatePrefab(scene, t.capture());
    REQUIRE((root != entt::null));
    PrefabInstanceComponent link;
    link.asset = HE::UUID::generate();
    reg.emplace<PrefabInstanceComponent>(root, link);
    const Entity bulb = childNamed(scene, root, "Bulb");
    REQUIRE((bulb != entt::null));

    // Edits made since, unmarked because nothing could mark them.
    reg.get<LightComponent>(bulb).intensity = 3.0f;
    reg.get<NameComponent>(root).name = "Lamp_01";
    AudioSourceComponent added;
    scene.addComponent(root, added);

    // Meanwhile the asset changed too.
    t.world.registry().get<LightComponent>(t.bulb).range = 20.0f;

    SceneSerializer::PrefabSyncReport rep;
    REQUIRE(ser.syncPrefabInstance(scene, root, t.capture(), &rep));

    // Bound by structure: root to root, Bulb to Bulb by its unique name.
    const auto& inst = reg.get<PrefabInstanceComponent>(root);
    CHECK(inst.instanceOf(t.tLamp) == idOf(reg, root));
    CHECK(inst.instanceOf(t.tBulb) == idOf(reg, bulb));
    // Every difference became an override rather than a revert: the bulb's
    // whole light (so the asset's new range does not land either), the root's
    // name, the audio source added here.
    CHECK(rep.overridesAdopted == 3);
    CHECK(inst.hasOverride(t.tBulb, "light"));
    CHECK(inst.hasOverride(t.tLamp, "__name"));
    CHECK(inst.hasOverride(t.tLamp, "audiosource"));
    CHECK(reg.get<LightComponent>(bulb).intensity == doctest::Approx(3.0f));
    CHECK(reg.get<LightComponent>(bulb).range     == doctest::Approx(10.0f));
    CHECK(reg.get<NameComponent>(root).name == "Lamp_01");
    CHECK(reg.try_get<AudioSourceComponent>(root) != nullptr);
    CHECK(rep.componentsApplied == 0);
    CHECK(rep.componentsRemoved == 0);
    CHECK(rep.entitiesCreated   == 0);

    // From here on it is an ordinary instance: the next asset change reaches
    // the parts that were not adopted as overrides.
    TransformComponent bt;
    bt.position = { 0.0f, 0.5f, 0.0f };
    t.world.registry().get<TransformComponent>(t.bulb) = bt;
    REQUIRE(ser.syncPrefabInstance(scene, root, t.capture()));
    CHECK(reg.get<TransformComponent>(bulb).position.y == doctest::Approx(0.5f));
}

TEST_CASE("PrefabSync: adoption binds an ambiguous child to nothing rather than guessing, and never duplicates it")
{
    Template t;
    // Two records called Bulb under the root.
    const Entity bulb2 = t.world.createEntity("Bulb");
    t.world.reparentEntity(bulb2, t.lamp);

    HorizonWorld scene;
    auto& reg = scene.registry();
    SceneSerializer ser;
    const Entity root = ser.instantiatePrefab(scene, t.capture());
    REQUIRE((root != entt::null));
    PrefabInstanceComponent link;
    link.asset = HE::UUID::generate();
    reg.emplace<PrefabInstanceComponent>(root, link);
    REQUIRE(reg.get<HierarchyComponent>(root).children.size() == 2);

    SceneSerializer::PrefabSyncReport rep;
    REQUIRE(ser.syncPrefabInstance(scene, root, t.capture(), &rep));
    const auto& inst = reg.get<PrefabInstanceComponent>(root);
    CHECK(inst.bindings.size() == 3);
    CHECK(inst.instanceOf(t.tLamp) == idOf(reg, root));
    // Both Bulb records are on record as "no counterpart here" …
    CHECK(inst.instanceOf(t.tBulb) == HE::UUID{});
    CHECK(inst.instanceOf(idOf(t.world.registry(), bulb2)) == HE::UUID{});
    // … which is why nothing was created, now or on the next pass.
    CHECK(rep.entitiesCreated == 0);
    REQUIRE(ser.syncPrefabInstance(scene, root, t.capture(), &rep));
    CHECK(rep.entitiesCreated == 0);
    CHECK(reg.get<HierarchyComponent>(root).children.size() == 2);

    // The "no counterpart" bindings have to survive a save, or the next open
    // would read both records as new in the asset and create them after all.
    std::vector<uint8_t> saved;
    REQUIRE(ser.saveToMemory(scene, saved));
    HorizonWorld reopened;
    REQUIRE(ser.loadFromMemory(reopened, saved));
    Entity again = entt::null;
    for (auto [e, n] : reopened.registry().view<NameComponent>().each())
        if (n.name == "Lamp") again = e;
    REQUIRE((again != entt::null));
    const auto& reloaded = reopened.registry().get<PrefabInstanceComponent>(again);
    CHECK(reloaded.bindings.size() == 3);
    CHECK(reloaded.bindings == inst.bindings);
    SceneSerializer::PrefabSyncReport afterReload;
    REQUIRE(ser.syncPrefabInstance(reopened, again, t.capture(), &afterReload));
    CHECK(afterReload.entitiesCreated == 0);
    CHECK(reopened.registry().get<HierarchyComponent>(again).children.size() == 2);
}

TEST_CASE("PrefabSync: a binding whose record the asset lost is counted, its entity left standing")
{
    Template t;
    HorizonWorld scene;
    const Placed p = place(scene, t.capture(), HE::UUID::generate());
    auto& reg = scene.registry();

    t.world.destroyEntity(t.bulb);
    SceneSerializer ser;
    SceneSerializer::PrefabSyncReport rep;
    REQUIRE(ser.syncPrefabInstance(scene, p.root, t.capture(), &rep));
    CHECK(rep.unboundEntities == 1);
    CHECK(reg.valid(p.bulb));
    CHECK(reg.get<PrefabInstanceComponent>(p.root).bindings.size() == 2);
}

TEST_CASE("PrefabSync: a blob that is no subtree, or a root that is no instance, is refused")
{
    Template t;
    HorizonWorld scene;
    const Placed p = place(scene, t.capture(), HE::UUID::generate());
    SceneSerializer ser;
    CHECK_FALSE(ser.syncPrefabInstance(scene, p.root, { 0xFF, 0x00, 0x13 }));
    CHECK_FALSE(ser.syncPrefabInstance(scene, p.bulb, t.capture()));
    CHECK_FALSE(ser.syncPrefabInstance(scene, entt::null, t.capture()));
}

TEST_CASE("PrefabSync: the whole-world pass reads each asset from the content manager once and reports what it could not")
{
    Template t;
    ContentManager cm;
    PrefabAsset pa;
    pa.name = "Lamp";
    pa.data = t.capture();
    const HE::UUID asset = cm.registerPrefab(std::move(pa));

    HorizonWorld scene;
    auto& reg = scene.registry();
    const Placed a = place(scene, t.capture(), asset);
    const Placed b = place(scene, t.capture(), asset);
    const Placed orphan = place(scene, t.capture(), HE::UUID::generate()); // asset nobody has

    // The asset changes after the placements: re-register the new bytes.
    t.world.registry().get<LightComponent>(t.bulb).intensity = 5.0f;
    PrefabAsset changed;
    changed.id   = asset;
    changed.name = "Lamp";
    changed.data = t.capture();
    cm.registerPrefab(std::move(changed));

    SceneSerializer ser;
    SceneSerializer::PrefabSyncReport rep;
    CHECK(ser.syncPrefabInstances(scene, cm, &rep) == 2);
    CHECK(rep.unresolvedAssets == 1);
    CHECK(reg.get<LightComponent>(a.bulb).intensity == doctest::Approx(5.0f));
    CHECK(reg.get<LightComponent>(b.bulb).intensity == doctest::Approx(5.0f));
    CHECK(reg.get<LightComponent>(orphan.bulb).intensity == doctest::Approx(1.0f));
}

// ─── Override recording, revert, push-to-prefab ──────────────────────────────
// What puts entries on the override list the sync reads, what takes them off
// again, and the way back into the asset.

TEST_CASE("PrefabRecord: an untouched placement records nothing, an edited value records exactly itself")
{
    Template t;
    HorizonWorld scene;
    const Placed p = place(scene, t.capture(), HE::UUID::generate());
    auto& reg = scene.registry();
    reg.get<TransformComponent>(p.root).position = { 7.0f, 0.0f, 0.0f }; // the placement, never an override

    SceneSerializer ser;
    const auto blob = t.capture();
    CHECK(ser.recordPrefabOverrides(scene, p.root, p.root, blob) == 0);
    CHECK(ser.recordPrefabOverrides(scene, p.root, p.bulb, blob) == 0);
    CHECK(reg.get<PrefabInstanceComponent>(p.root).overrides.empty());

    reg.get<LightComponent>(p.bulb).intensity = 7.0f;
    CHECK(ser.recordPrefabOverrides(scene, p.root, p.bulb, blob) == 1);
    const auto& inst = reg.get<PrefabInstanceComponent>(p.root);
    REQUIRE(inst.overrides.size() == 1);
    CHECK(inst.overrides[0].templateEntity == t.tBulb);
    CHECK(inst.overrides[0].component == "light");
    CHECK(inst.overrides[0].property == "intensity");
    // Recording again adds nothing — the entry is there.
    CHECK(ser.recordPrefabOverrides(scene, p.root, p.bulb, blob) == 0);
    CHECK(inst.overrides.size() == 1);

    // And the entry does what it is for: the value survives a sync.
    t.world.registry().get<LightComponent>(t.bulb).intensity = 5.0f;
    REQUIRE(ser.syncPrefabInstance(scene, p.root, t.capture()));
    CHECK(reg.get<LightComponent>(p.bulb).intensity == doctest::Approx(7.0f));

    // A value put back to the template's leaves nothing behind.
    HorizonWorld scene2;
    const Placed q = place(scene2, t.capture(), HE::UUID::generate());
    scene2.registry().get<LightComponent>(q.bulb).range = 99.0f;
    scene2.registry().get<LightComponent>(q.bulb).range = 10.0f;
    CHECK(ser.recordPrefabOverrides(scene2, q.root, q.bulb, t.capture()) == 0);
}

TEST_CASE("PrefabRecord: a component added or removed here, and a rename, become whole entries")
{
    Template t;
    HorizonWorld scene;
    const Placed p = place(scene, t.capture(), HE::UUID::generate());
    auto& reg = scene.registry();
    const auto blob = t.capture();
    SceneSerializer ser;

    SUBCASE("removed here")
    {
        reg.remove<LightComponent>(p.bulb);
        CHECK(ser.recordPrefabOverrides(scene, p.root, p.bulb, blob) == 1);
        const auto& inst = reg.get<PrefabInstanceComponent>(p.root);
        CHECK(inst.hasOverride(t.tBulb, "light"));
        CHECK(inst.overrides[0].property.empty());
        // The sync respects it: the light does not come back.
        REQUIRE(ser.syncPrefabInstance(scene, p.root, blob));
        CHECK_FALSE(reg.all_of<LightComponent>(p.bulb));
    }
    SUBCASE("added here")
    {
        scene.addComponent(p.bulb, AudioSourceComponent{});
        CHECK(ser.recordPrefabOverrides(scene, p.root, p.bulb, blob) == 1);
        CHECK(reg.get<PrefabInstanceComponent>(p.root).hasOverride(t.tBulb, "audiosource"));
        REQUIRE(ser.syncPrefabInstance(scene, p.root, blob));
        CHECK(reg.all_of<AudioSourceComponent>(p.bulb));
    }
    SUBCASE("renamed here")
    {
        scene.renameEntity(p.bulb, "Filament");
        CHECK(ser.recordPrefabOverrides(scene, p.root, p.bulb, blob) == 1);
        CHECK(reg.get<PrefabInstanceComponent>(p.root).hasOverride(t.tBulb, "__name"));
        REQUIRE(ser.syncPrefabInstance(scene, p.root, blob));
        CHECK(reg.get<NameComponent>(p.bulb).name == "Filament");
    }
    SUBCASE("a child added here is nobody's counterpart and records nothing")
    {
        const Entity shade = scene.createEntity("Shade");
        scene.reparentEntity(shade, p.root);
        scene.addComponent(shade, TransformComponent{});
        CHECK(ser.recordPrefabOverrides(scene, p.root, shade, blob) == 0);
        CHECK(ser.prefabInstancesBinding(scene, shade).empty());
    }
}

TEST_CASE("PrefabRecord: which placements bind an entity")
{
    Template t;
    HorizonWorld scene;
    const Placed a = place(scene, t.capture(), HE::UUID::generate());
    const Placed b = place(scene, t.capture(), HE::UUID::generate());
    const Entity loose = scene.createEntity("Loose");

    SceneSerializer ser;
    auto ofA = ser.prefabInstancesBinding(scene, a.bulb);
    REQUIRE(ofA.size() == 1);
    CHECK((ofA[0] == a.root));
    auto ofRootB = ser.prefabInstancesBinding(scene, b.root);
    REQUIRE(ofRootB.size() == 1);
    CHECK((ofRootB[0] == b.root));
    CHECK(ser.prefabInstancesBinding(scene, loose).empty());
}

TEST_CASE("PrefabRevert: dropping the entry brings the template's value back, one entry at a time")
{
    Template t;
    HorizonWorld scene;
    const Placed p = place(scene, t.capture(), HE::UUID::generate());
    auto& reg = scene.registry();
    SceneSerializer ser;

    reg.get<LightComponent>(p.bulb).intensity = 7.0f;
    reg.get<LightComponent>(p.bulb).range     = 70.0f;
    REQUIRE(ser.recordPrefabOverrides(scene, p.root, p.bulb, t.capture()) == 2);

    // Meanwhile the asset moved on; both overrides hold through a sync.
    t.world.registry().get<LightComponent>(t.bulb).intensity = 5.0f;
    const auto blob = t.capture();
    REQUIRE(ser.syncPrefabInstance(scene, p.root, blob));
    CHECK(reg.get<LightComponent>(p.bulb).intensity == doctest::Approx(7.0f));

    const PrefabInstanceComponent::Override intensity{ t.tBulb, "light", "intensity" };
    REQUIRE(ser.revertPrefabOverride(scene, p.root, blob, intensity));
    CHECK(reg.get<LightComponent>(p.bulb).intensity == doctest::Approx(5.0f));   // the asset's
    CHECK(reg.get<LightComponent>(p.bulb).range     == doctest::Approx(70.0f));  // still mine
    CHECK(reg.get<PrefabInstanceComponent>(p.root).overrides.size() == 1);

    // An entry that is not there is refused, and nothing else changes.
    CHECK_FALSE(ser.revertPrefabOverride(scene, p.root, blob, intensity));
    CHECK(reg.get<LightComponent>(p.bulb).range == doctest::Approx(70.0f));

    // A whole-component entry reverts the component: added here, gone again.
    scene.addComponent(p.bulb, AudioSourceComponent{});
    REQUIRE(ser.recordPrefabOverrides(scene, p.root, p.bulb, blob) == 1);
    REQUIRE(ser.revertPrefabOverride(scene, p.root, blob, { t.tBulb, "audiosource", "" }));
    CHECK_FALSE(reg.all_of<AudioSourceComponent>(p.bulb));
}

TEST_CASE("PrefabPush: the placement becomes the asset under the template's ids, and the others follow")
{
    Template t;
    t.world.registry().get<TransformComponent>(t.lamp).position = { 0.0f, 99.0f, 0.0f };
    HorizonWorld scene;
    auto& reg = scene.registry();
    const HE::UUID asset = HE::UUID::generate();
    const Placed a = place(scene, t.capture(), asset);
    const Placed b = place(scene, t.capture(), asset);
    reg.get<TransformComponent>(a.root).position = { 7.0f, 0.0f, 0.0f };
    SceneSerializer ser;

    // Authored on A: a brighter bulb (marked), and a new child.
    reg.get<LightComponent>(a.bulb).intensity = 7.0f;
    REQUIRE(ser.recordPrefabOverrides(scene, a.root, a.bulb, t.capture()) == 1);
    const Entity shade = scene.createEntity("Shade");
    scene.reparentEntity(shade, a.root);
    scene.addComponent(shade, TransformComponent{});
    const HE::UUID shadeId = idOf(reg, shade);

    std::vector<uint8_t> pushed;
    REQUIRE(ser.pushPrefabInstance(scene, a.root, t.capture(), pushed));
    CHECK(!pushed.empty());

    // A is now the asset: no overrides, bindings identity over the records.
    const auto& instA = reg.get<PrefabInstanceComponent>(a.root);
    CHECK(instA.overrides.empty());
    CHECK(instA.bindings.size() == 3);
    CHECK(instA.instanceOf(t.tLamp) == idOf(reg, a.root));
    CHECK(instA.instanceOf(t.tBulb) == idOf(reg, a.bulb));
    CHECK(instA.instanceOf(shadeId) == shadeId);   // a new record is called by the entity's own id

    // The blob speaks the template's ids: a fresh placement of it binds under
    // the same keys B's table already has, and B's sync reaches B's bulb.
    HorizonWorld probe;
    std::vector<PrefabInstanceComponent::Binding> bindings;
    const Entity probeRoot = ser.instantiatePrefab(probe, pushed, entt::null, false, &bindings);
    REQUIRE((probeRoot != entt::null));
    bool sawLamp = false, sawBulb = false, sawShade = false;
    for (const auto& bd : bindings)
    {
        sawLamp  |= bd.templateEntity == t.tLamp;
        sawBulb  |= bd.templateEntity == t.tBulb;
        sawShade |= bd.templateEntity == shadeId;
    }
    CHECK(sawLamp); CHECK(sawBulb); CHECK(sawShade);
    // The asset's root stays where the asset had it, not where A stands, and
    // carries no prefab block of its own.
    CHECK(probe.registry().get<TransformComponent>(probeRoot).position.y == doctest::Approx(99.0f));
    CHECK_FALSE(probe.registry().all_of<PrefabInstanceComponent>(probeRoot));

    SceneSerializer::PrefabSyncReport rep;
    REQUIRE(ser.syncPrefabInstance(scene, b.root, pushed, &rep));
    CHECK(reg.get<LightComponent>(b.bulb).intensity == doctest::Approx(7.0f));
    CHECK(rep.entitiesCreated == 1);
    CHECK((childNamed(scene, b.root, "Shade") != entt::null));
    // And syncing A against what it just pushed changes nothing.
    SceneSerializer::PrefabSyncReport repA;
    REQUIRE(ser.syncPrefabInstance(scene, a.root, pushed, &repA));
    CHECK(repA.componentsApplied == 0);
    CHECK(repA.entitiesCreated == 0);
    CHECK(reg.get<TransformComponent>(a.root).position.x == doctest::Approx(7.0f));
}

TEST_CASE("PrefabPush: a child deleted here leaves the asset, its binding goes with it")
{
    Template t;
    HorizonWorld scene;
    auto& reg = scene.registry();
    const Placed a = place(scene, t.capture(), HE::UUID::generate());
    scene.destroyEntity(a.bulb);

    SceneSerializer ser;
    std::vector<uint8_t> pushed;
    REQUIRE(ser.pushPrefabInstance(scene, a.root, t.capture(), pushed));
    const auto& inst = reg.get<PrefabInstanceComponent>(a.root);
    CHECK(inst.bindings.size() == 1);
    CHECK(inst.instanceOf(t.tBulb) == HE::UUID{});

    HorizonWorld probe;
    const Entity probeRoot = ser.instantiatePrefab(probe, pushed);
    REQUIRE((probeRoot != entt::null));
    auto* h = probe.registry().try_get<HierarchyComponent>(probeRoot);
    CHECK((!h || h->children.empty()));
}

TEST_CASE("PrefabPush: a root that is no instance is refused, and the content manager swaps the payload in place")
{
    HorizonWorld scene;
    const Entity plain = scene.createEntity("Plain");
    SceneSerializer ser;
    std::vector<uint8_t> out;
    CHECK_FALSE(ser.pushPrefabInstance(scene, plain, {}, out));
    CHECK(out.empty());

    Template t;
    ContentManager cm;
    PrefabAsset pa;
    pa.name = "Lamp";
    pa.path = "Prefabs/Lamp.hasset";
    pa.data = t.capture();
    const HE::UUID id = cm.registerPrefab(std::move(pa));

    t.world.registry().get<LightComponent>(t.bulb).intensity = 5.0f;
    PrefabAsset next;
    next.data = t.capture();
    REQUIRE(cm.replacePrefab(id, std::move(next)));
    const PrefabAsset* got = cm.getPrefab(id);
    REQUIRE(got != nullptr);
    CHECK(got->path == "Prefabs/Lamp.hasset");
    CHECK(got->name == "Lamp");
    CHECK(got->id == id);
    HorizonWorld probe;
    const Entity r = ser.instantiatePrefab(probe, got->data);
    REQUIRE((r != entt::null));
    CHECK(probe.registry().get<LightComponent>(childNamed(probe, r, "Bulb")).intensity == doctest::Approx(5.0f));
    CHECK_FALSE(cm.replacePrefab(HE::UUID::generate(), PrefabAsset{}));
}
