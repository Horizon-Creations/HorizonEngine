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

TEST_CASE("PrefabSync: a record the asset lost takes its entity with it, and the binding goes too")
{
    Template t;
    HorizonWorld scene;
    const Placed p = place(scene, t.capture(), HE::UUID::generate());
    auto& reg = scene.registry();

    t.world.destroyEntity(t.bulb);
    SceneSerializer ser;
    SceneSerializer::PrefabSyncReport rep;
    REQUIRE(ser.syncPrefabInstance(scene, p.root, t.capture(), &rep));
    CHECK(rep.entitiesRemoved == 1);
    CHECK(rep.unboundEntities == 0);
    CHECK_FALSE(reg.valid(p.bulb));
    CHECK(reg.get<HierarchyComponent>(p.root).children.empty());
    CHECK(reg.get<PrefabInstanceComponent>(p.root).bindings.size() == 1);
    // Nothing is left that a second pass could act on.
    SceneSerializer::PrefabSyncReport again;
    REQUIRE(ser.syncPrefabInstance(scene, p.root, t.capture(), &again));
    CHECK(again.entitiesRemoved == 0);
}

TEST_CASE("PrefabSync: a lost record whose entity was changed here stays, as a child added here")
{
    Template t;
    SceneSerializer ser;

    SUBCASE("an override on the record itself")
    {
        HorizonWorld scene;
        const Placed p = place(scene, t.capture(), HE::UUID::generate());
        auto& reg = scene.registry();
        reg.get<LightComponent>(p.bulb).intensity = 7.0f;
        REQUIRE(ser.recordPrefabOverrides(scene, p.root, p.bulb, t.capture()) == 1);

        t.world.destroyEntity(t.bulb);
        SceneSerializer::PrefabSyncReport rep;
        REQUIRE(ser.syncPrefabInstance(scene, p.root, t.capture(), &rep));
        CHECK(rep.entitiesRemoved == 0);
        CHECK(rep.unboundEntities == 1);
        REQUIRE(reg.valid(p.bulb));
        CHECK(reg.get<LightComponent>(p.bulb).intensity == doctest::Approx(7.0f));
        const auto& inst = reg.get<PrefabInstanceComponent>(p.root);
        CHECK(inst.bindings.size() == 1);   // the bulb's binding is gone with its record
        CHECK(inst.overrides.empty());      // and so is the override that named it
        CHECK(SceneSerializer::prefabAddedHereCount(scene, p.root) == 1);
    }
    SUBCASE("a child added here under it")
    {
        HorizonWorld scene;
        const Placed p = place(scene, t.capture(), HE::UUID::generate());
        auto& reg = scene.registry();
        const Entity mine = scene.createEntity("Mine");
        scene.reparentEntity(mine, p.bulb);

        t.world.destroyEntity(t.bulb);
        SceneSerializer::PrefabSyncReport rep;
        REQUIRE(ser.syncPrefabInstance(scene, p.root, t.capture(), &rep));
        CHECK(rep.entitiesRemoved == 0);
        CHECK(reg.valid(p.bulb));
        CHECK(reg.valid(mine));
    }
    SUBCASE("the entity is itself a nested placement with changes of its own")
    {
        // Outer asset: Lamp → Bulb → (nested) Lamp' → Bulb'. Capture it as the
        // asset, place it, change something in the nested one's own list,
        // then let the outer asset lose the Bulb.
        HorizonWorld outerTemplate;
        const Entity oLamp = outerTemplate.createEntity("Lamp");
        const Entity oBulb = outerTemplate.createEntity("Bulb");
        outerTemplate.reparentEntity(oBulb, oLamp);
        std::vector<PrefabInstanceComponent::Binding> nb;
        const Entity oNested = ser.instantiatePrefab(outerTemplate, t.capture(), oBulb, false, &nb);
        REQUIRE((oNested != entt::null));
        PrefabInstanceComponent nestedInst;
        nestedInst.asset    = HE::UUID::generate();
        nestedInst.bindings = nb;
        outerTemplate.registry().emplace_or_replace<PrefabInstanceComponent>(oNested, nestedInst);
        const auto outerBlob = ser.serializeSubtree(outerTemplate, oLamp);
        const HE::UUID tOBulb = idOf(outerTemplate.registry(), oBulb);

        HorizonWorld scene;
        std::vector<PrefabInstanceComponent::Binding> bindings;
        const Entity root = ser.instantiatePrefab(scene, outerBlob, entt::null, false, &bindings);
        REQUIRE((root != entt::null));
        PrefabInstanceComponent inst;
        inst.asset    = HE::UUID::generate();
        inst.bindings = bindings;
        scene.registry().emplace_or_replace<PrefabInstanceComponent>(root, inst);
        auto& reg = scene.registry();
        const Entity bulb   = childNamed(scene, root, "Bulb");
        const Entity nested = childNamed(scene, bulb, "Lamp");
        REQUIRE((nested != entt::null));
        REQUIRE(reg.all_of<PrefabInstanceComponent>(nested));
        reg.get<PrefabInstanceComponent>(nested).setOverride(t.tBulb, "light", "intensity");

        outerTemplate.destroyEntity(oBulb);
        SceneSerializer::PrefabSyncReport rep;
        REQUIRE(ser.syncPrefabInstance(scene, root, ser.serializeSubtree(outerTemplate, oLamp), &rep));
        CHECK(rep.entitiesRemoved == 0);
        CHECK(reg.valid(bulb));     // kept: the nested placement under it has changes
        CHECK(reg.valid(nested));
        CHECK(reg.get<PrefabInstanceComponent>(root).instanceOf(tOBulb) == HE::UUID{});

        // And the nested root as the lost record ITSELF, with its own changes:
        // outer asset loses the nested Lamp, keeps the Bulb.
        HorizonWorld scene2;
        std::vector<PrefabInstanceComponent::Binding> bindings2;
        const Entity root2 = ser.instantiatePrefab(scene2, outerBlob, entt::null, false, &bindings2);
        PrefabInstanceComponent inst2;
        inst2.asset    = HE::UUID::generate();
        inst2.bindings = bindings2;
        scene2.registry().emplace_or_replace<PrefabInstanceComponent>(root2, inst2);
        const Entity nested2 = childNamed(scene2, childNamed(scene2, root2, "Bulb"), "Lamp");
        REQUIRE((nested2 != entt::null));
        scene2.registry().get<PrefabInstanceComponent>(nested2).setOverride(t.tBulb, "light", "intensity");
        HorizonWorld outer2;   // the outer asset without the nested placement: Lamp → Bulb only
        const Entity l2 = outer2.createEntity("Lamp");
        const Entity b2 = outer2.createEntity("Bulb");
        outer2.reparentEntity(b2, l2);
        outer2.registry().get<EntityIdComponent>(l2).id = idOf(outerTemplate.registry(), oLamp);
        outer2.registry().get<EntityIdComponent>(b2).id = tOBulb;
        SceneSerializer::PrefabSyncReport rep2;
        REQUIRE(ser.syncPrefabInstance(scene2, root2, ser.serializeSubtree(outer2, l2), &rep2));
        CHECK(rep2.entitiesRemoved == 0);
        CHECK(rep2.unboundEntities == 1);
        CHECK(scene2.registry().valid(nested2));

        // Without changes of its own, a nested placement the outer asset lost
        // goes with everything under it — its own records included, which
        // are the nested placement's and go with their root.
        HorizonWorld scene3;
        std::vector<PrefabInstanceComponent::Binding> bindings3;
        const Entity root3 = ser.instantiatePrefab(scene3, outerBlob, entt::null, false, &bindings3);
        PrefabInstanceComponent inst3;
        inst3.asset    = HE::UUID::generate();
        inst3.bindings = bindings3;
        scene3.registry().emplace_or_replace<PrefabInstanceComponent>(root3, inst3);
        REQUIRE(scene3.registry().get<PrefabInstanceComponent>(root3).bindings.size() == 4);
        SceneSerializer::PrefabSyncReport rep3;
        REQUIRE(ser.syncPrefabInstance(scene3, root3, ser.serializeSubtree(outer2, l2), &rep3));
        CHECK(rep3.entitiesRemoved == 1);   // the nested Lamp' subtree
        CHECK(rep3.unboundEntities == 0);
        CHECK((childNamed(scene3, root3, "Bulb") != entt::null));
        CHECK((childNamed(scene3, childNamed(scene3, root3, "Bulb"), "Lamp") == entt::null));
        CHECK(scene3.registry().get<PrefabInstanceComponent>(root3).bindings.size() == 2);
    }
    SUBCASE("nothing under it: the whole subtree goes, bindings included")
    {
        // The asset grows a Shade under the Bulb, is placed, then loses the Bulb.
        const Entity shade = t.world.createEntity("Shade");
        t.world.reparentEntity(shade, t.bulb);
        HorizonWorld scene;
        const Placed p = place(scene, t.capture(), HE::UUID::generate());
        auto& reg = scene.registry();
        REQUIRE(reg.get<PrefabInstanceComponent>(p.root).bindings.size() == 3);

        t.world.destroyEntity(t.bulb);
        SceneSerializer::PrefabSyncReport rep;
        REQUIRE(ser.syncPrefabInstance(scene, p.root, t.capture(), &rep));
        CHECK(rep.entitiesRemoved == 1);   // one subtree
        CHECK_FALSE(reg.valid(p.bulb));
        CHECK(reg.get<PrefabInstanceComponent>(p.root).bindings.size() == 1);
    }
}

TEST_CASE("PrefabSync: an asset rebuilt under a new root id keeps the placement's root")
{
    Template t;
    HorizonWorld scene;
    const Placed p = place(scene, t.capture(), HE::UUID::generate());
    auto& reg = scene.registry();

    // A different template altogether — new ids for everything.
    Template other;
    SceneSerializer ser;
    SceneSerializer::PrefabSyncReport rep;
    REQUIRE(ser.syncPrefabInstance(scene, p.root, other.capture(), &rep));
    REQUIRE(reg.valid(p.root));
    CHECK(rep.entitiesRemoved == 1);   // the old bulb
    CHECK(rep.entitiesCreated == 1);   // the new one
    const auto& inst = reg.get<PrefabInstanceComponent>(p.root);
    CHECK(inst.instanceOf(other.tLamp) == idOf(reg, p.root));
    CHECK(inst.instanceOf(t.tLamp) == HE::UUID{});
    CHECK(inst.bindings.size() == 2);
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

// ─── Structure: children removed here, children added here ───────────────────
// Neither is in the override list — a dead binding is the deletion, an unbound
// entity under the root is the addition — and both can be taken back.

TEST_CASE("PrefabStructure: a child deleted here is listed by its record's name, one added here by entity")
{
    Template t;
    const Entity shade = t.world.createEntity("Shade");
    t.world.reparentEntity(shade, t.bulb);
    HorizonWorld scene;
    const auto blob = t.capture();
    const Placed p = place(scene, blob, HE::UUID::generate());
    auto& reg = scene.registry();

    const SceneSerializer::PrefabRecordIndex index = SceneSerializer::indexPrefabRecords(blob);
    REQUIRE(index.valid);
    CHECK(index.records.size() == 3);
    REQUIRE(index.find(t.tBulb) != nullptr);
    CHECK(index.find(t.tBulb)->name == "Bulb");
    CHECK(index.find(t.tBulb)->parent == t.tLamp);
    CHECK(index.find(HE::UUID::generate()) == nullptr);
    CHECK_FALSE(SceneSerializer::indexPrefabRecords({ 1, 2, 3 }).valid);

    // Untouched: nothing.
    auto st = SceneSerializer::prefabStructureOf(scene, p.root, index);
    CHECK_FALSE(st.any());

    // The whole Bulb subtree deleted here: ONE entry, the topmost record.
    scene.destroyEntity(p.bulb);
    st = SceneSerializer::prefabStructureOf(scene, p.root, index);
    REQUIRE(st.removedHere.size() == 1);
    CHECK(st.removedHere[0].templateEntity == t.tBulb);
    CHECK(st.removedHere[0].name == "Bulb");
    CHECK(st.addedHere.empty());
    CHECK(SceneSerializer::prefabRemovedHereCount(scene, p.root) == 2);   // both bindings are dead

    // Something added here, with a child of its own: ONE entry, the topmost.
    const Entity mine = scene.createEntity("Mine");
    scene.reparentEntity(mine, p.root);
    const Entity mineChild = scene.createEntity("MineChild");
    scene.reparentEntity(mineChild, mine);
    st = SceneSerializer::prefabStructureOf(scene, p.root, index);
    REQUIRE(st.addedHere.size() == 1);
    CHECK(st.addedHere[0] == mine);
    CHECK(SceneSerializer::prefabAddedHereCount(scene, p.root) == 1);

    // A plain entity is no instance and has no structure.
    CHECK_FALSE(SceneSerializer::prefabStructureOf(scene, mine, index).any());
    CHECK(SceneSerializer::prefabRemovedHereCount(scene, mine) == 0);
    CHECK(SceneSerializer::prefabAddedHereCount(scene, mine) == 0);
    (void)reg;
}

TEST_CASE("PrefabStructure: reverting a deletion brings the subtree back from the asset, bound again")
{
    Template t;
    const Entity shade = t.world.createEntity("Shade");
    t.world.reparentEntity(shade, t.bulb);
    const HE::UUID tShade = idOf(t.world.registry(), shade);
    HorizonWorld scene;
    const auto blob = t.capture();
    const Placed p = place(scene, blob, HE::UUID::generate());
    auto& reg = scene.registry();
    SceneSerializer ser;

    scene.destroyEntity(p.bulb);
    REQUIRE(reg.get<HierarchyComponent>(p.root).children.empty());
    REQUIRE(ser.revertPrefabRemoval(scene, p.root, blob, t.tBulb));

    const Entity bulb = childNamed(scene, p.root, "Bulb");
    REQUIRE((bulb != entt::null));
    CHECK(reg.get<LightComponent>(bulb).range == doctest::Approx(10.0f));
    const Entity back = childNamed(scene, bulb, "Shade");
    CHECK((back != entt::null));
    const auto& inst = reg.get<PrefabInstanceComponent>(p.root);
    CHECK(inst.bindings.size() == 3);
    CHECK(inst.instanceOf(t.tBulb)  == idOf(reg, bulb));
    CHECK(inst.instanceOf(tShade)   == idOf(reg, back));
    CHECK(SceneSerializer::prefabRemovedHereCount(scene, p.root) == 0);

    // Nothing to revert: alive, unknown, or not bound.
    CHECK_FALSE(ser.revertPrefabRemoval(scene, p.root, blob, t.tBulb));
    CHECK_FALSE(ser.revertPrefabRemoval(scene, p.root, blob, HE::UUID::generate()));
    CHECK(reg.get<PrefabInstanceComponent>(p.root).bindings.size() == 3);
}

TEST_CASE("PrefabStructure: reverting a deletion of a child alone leaves its living parent as it is")
{
    Template t;
    const Entity shade = t.world.createEntity("Shade");
    t.world.reparentEntity(shade, t.bulb);
    const HE::UUID tShade = idOf(t.world.registry(), shade);
    HorizonWorld scene;
    const auto blob = t.capture();
    const Placed p = place(scene, blob, HE::UUID::generate());
    auto& reg = scene.registry();
    SceneSerializer ser;

    reg.get<LightComponent>(p.bulb).intensity = 3.0f;
    REQUIRE(ser.recordPrefabOverrides(scene, p.root, p.bulb, blob) == 1);
    scene.destroyEntity(childNamed(scene, p.bulb, "Shade"));
    const SceneSerializer::PrefabRecordIndex index = SceneSerializer::indexPrefabRecords(blob);
    auto st = SceneSerializer::prefabStructureOf(scene, p.root, index);
    REQUIRE(st.removedHere.size() == 1);
    CHECK(st.removedHere[0].templateEntity == tShade);

    REQUIRE(ser.revertPrefabRemoval(scene, p.root, blob, tShade));
    CHECK((childNamed(scene, p.bulb, "Shade") != entt::null));
    CHECK(reg.get<LightComponent>(p.bulb).intensity == doctest::Approx(3.0f));   // still mine
    CHECK(reg.get<PrefabInstanceComponent>(p.root).overrides.size() == 1);
}

TEST_CASE("PrefabStructure: reverting an addition deletes it, and only what was added here")
{
    Template t;
    HorizonWorld scene;
    const Placed p = place(scene, t.capture(), HE::UUID::generate());
    auto& reg = scene.registry();
    SceneSerializer ser;

    const Entity mine = scene.createEntity("Mine");
    scene.reparentEntity(mine, p.bulb);
    const Entity elsewhere = scene.createEntity("Elsewhere");

    CHECK_FALSE(ser.revertPrefabAddition(scene, p.root, p.bulb));      // the asset's child
    CHECK_FALSE(ser.revertPrefabAddition(scene, p.root, p.root));      // the root itself
    CHECK_FALSE(ser.revertPrefabAddition(scene, p.root, elsewhere));   // not under the root
    CHECK_FALSE(ser.revertPrefabAddition(scene, elsewhere, mine));     // no instance
    CHECK(reg.valid(p.bulb));
    CHECK(reg.valid(mine));

    REQUIRE(ser.revertPrefabAddition(scene, p.root, mine));
    CHECK_FALSE(reg.valid(mine));
    CHECK(reg.valid(p.bulb));
    CHECK(SceneSerializer::prefabAddedHereCount(scene, p.root) == 0);
}

TEST_CASE("PrefabStructure: a child of a nested placement is the nested one's, not added to the outer")
{
    // Outer: Lamp → Bulb. Nested under Bulb: a second placement of the same asset.
    Template t;
    HorizonWorld scene;
    const auto blob = t.capture();
    const Placed outer = place(scene, blob, HE::UUID::generate());
    SceneSerializer ser;
    std::vector<PrefabInstanceComponent::Binding> bindings;
    const Entity nestedRoot = ser.instantiatePrefab(scene, blob, outer.bulb, false, &bindings);
    REQUIRE((nestedRoot != entt::null));
    PrefabInstanceComponent inst;
    inst.asset    = HE::UUID::generate();
    inst.bindings = bindings;
    scene.registry().emplace_or_replace<PrefabInstanceComponent>(nestedRoot, inst);

    // The nested root sits under the outer's bulb, and the outer's table does
    // not bind it — but a placement does (its own), so it is not "added here"
    // for the outer one, and neither is its bulb.
    CHECK(SceneSerializer::prefabAddedHereCount(scene, outer.root) == 0);
    CHECK(SceneSerializer::prefabAddedHereCount(scene, nestedRoot) == 0);

    // A stranger under the nested bulb is added here for BOTH — it is in both
    // subtrees and nobody's counterpart.
    const Entity stranger = scene.createEntity("Stranger");
    scene.reparentEntity(stranger, childNamed(scene, nestedRoot, "Bulb"));
    CHECK(SceneSerializer::prefabAddedHereCount(scene, outer.root) == 1);
    CHECK(SceneSerializer::prefabAddedHereCount(scene, nestedRoot) == 1);
}

TEST_CASE("PrefabPush: a child deleted here and pushed leaves the OTHER placements too")
{
    Template t;
    HorizonWorld scene;
    auto& reg = scene.registry();
    const HE::UUID asset = HE::UUID::generate();
    const Placed a = place(scene, t.capture(), asset);
    const Placed b = place(scene, t.capture(), asset);
    const Placed c = place(scene, t.capture(), asset);
    SceneSerializer ser;

    // c changed its bulb for itself; a deletes its bulb and pushes.
    reg.get<LightComponent>(c.bulb).intensity = 9.0f;
    REQUIRE(ser.recordPrefabOverrides(scene, c.root, c.bulb, t.capture()) == 1);
    scene.destroyEntity(a.bulb);
    std::vector<uint8_t> pushed;
    REQUIRE(ser.pushPrefabInstance(scene, a.root, t.capture(), pushed));

    SceneSerializer::PrefabSyncReport rep;
    REQUIRE(ser.syncPrefabInstance(scene, b.root, pushed, &rep));
    CHECK(rep.entitiesRemoved == 1);
    CHECK_FALSE(reg.valid(b.bulb));
    CHECK(reg.get<PrefabInstanceComponent>(b.root).bindings.size() == 1);

    SceneSerializer::PrefabSyncReport repC;
    REQUIRE(ser.syncPrefabInstance(scene, c.root, pushed, &repC));
    CHECK(repC.entitiesRemoved == 0);
    CHECK(repC.unboundEntities == 1);
    REQUIRE(reg.valid(c.bulb));
    CHECK(reg.get<LightComponent>(c.bulb).intensity == doctest::Approx(9.0f));
    CHECK(SceneSerializer::prefabAddedHereCount(scene, c.root) == 1);
}

// ─── Undo ────────────────────────────────────────────────────────────────────
// The editor's undo is a world snapshot (EditorUndo), and the instance's table
// and override list are part of the world: whatever an edit, a revert or a
// sync did to them goes back with the rest. What these cases pin down is that
// the bindings still resolve after a restore (uuids survive a memory
// round-trip, handles do not) and that the sync draws the right conclusion
// from a restored world.

#include "EditorUndo.h"

TEST_CASE("PrefabUndo: a deletion undone is no deletion — the binding resolves again and the sync creates nothing")
{
    Template t;
    HorizonWorld scene;
    const auto blob = t.capture();
    const Placed p = place(scene, blob, HE::UUID::generate());
    auto& reg = scene.registry();
    EditorUndo undo;
    undo.setWorld(&scene);
    SceneSerializer ser;

    undo.snapshotNow();
    scene.destroyEntity(p.bulb);
    CHECK(SceneSerializer::prefabRemovedHereCount(scene, p.root) == 1);

    REQUIRE(undo.undo());
    // Handles are re-minted; find the placement by its component.
    Entity newRoot = entt::null;
    for (auto e : reg.view<PrefabInstanceComponent>()) newRoot = e;
    REQUIRE((newRoot != entt::null));
    const Entity bulb = childNamed(scene, newRoot, "Bulb");
    REQUIRE((bulb != entt::null));
    CHECK(reg.get<PrefabInstanceComponent>(newRoot).instanceOf(t.tBulb) == idOf(reg, bulb));
    CHECK(SceneSerializer::prefabRemovedHereCount(scene, newRoot) == 0);

    SceneSerializer::PrefabSyncReport rep;
    REQUIRE(ser.syncPrefabInstance(scene, newRoot, blob, &rep));
    CHECK(rep.entitiesCreated == 0);
    CHECK(reg.get<HierarchyComponent>(newRoot).children.size() == 1);

    // And redo makes it a deletion again.
    REQUIRE(undo.redo());
    for (auto e : reg.view<PrefabInstanceComponent>()) newRoot = e;
    CHECK(SceneSerializer::prefabRemovedHereCount(scene, newRoot) == 1);
    CHECK((childNamed(scene, newRoot, "Bulb") == entt::null));
}

TEST_CASE("PrefabUndo: a reverted deletion undone is a deletion again")
{
    Template t;
    HorizonWorld scene;
    const auto blob = t.capture();
    const Placed p = place(scene, blob, HE::UUID::generate());
    auto& reg = scene.registry();
    EditorUndo undo;
    undo.setWorld(&scene);
    SceneSerializer ser;

    scene.destroyEntity(p.bulb);
    undo.snapshotNow();   // what the editor does before a revert
    REQUIRE(ser.revertPrefabRemoval(scene, p.root, blob, t.tBulb));
    CHECK((childNamed(scene, p.root, "Bulb") != entt::null));

    REQUIRE(undo.undo());
    Entity root = entt::null;
    for (auto e : reg.view<PrefabInstanceComponent>()) root = e;
    REQUIRE((root != entt::null));
    CHECK((childNamed(scene, root, "Bulb") == entt::null));
    CHECK(SceneSerializer::prefabRemovedHereCount(scene, root) == 1);
    // The sync still respects the (restored) dead binding.
    SceneSerializer::PrefabSyncReport rep;
    REQUIRE(ser.syncPrefabInstance(scene, root, blob, &rep));
    CHECK(rep.entitiesCreated == 0);
}

TEST_CASE("PrefabUndo: a child the sync created is undone with its binding, and the next sync creates it once")
{
    Template t;
    HorizonWorld scene;
    const Placed p = place(scene, t.capture(), HE::UUID::generate());
    auto& reg = scene.registry();
    EditorUndo undo;
    undo.setWorld(&scene);
    SceneSerializer ser;

    const Entity shade = t.world.createEntity("Shade");
    t.world.reparentEntity(shade, t.bulb);
    const auto blob = t.capture();

    undo.snapshotNow();
    SceneSerializer::PrefabSyncReport rep;
    REQUIRE(ser.syncPrefabInstance(scene, p.root, blob, &rep));
    CHECK(rep.entitiesCreated == 1);
    CHECK(reg.get<PrefabInstanceComponent>(p.root).bindings.size() == 3);

    REQUIRE(undo.undo());
    Entity root = entt::null;
    for (auto e : reg.view<PrefabInstanceComponent>()) root = e;
    REQUIRE((root != entt::null));
    CHECK(reg.get<PrefabInstanceComponent>(root).bindings.size() == 2);
    const Entity bulb = childNamed(scene, root, "Bulb");
    REQUIRE((bulb != entt::null));
    CHECK((childNamed(scene, bulb, "Shade") == entt::null));

    SceneSerializer::PrefabSyncReport again;
    REQUIRE(ser.syncPrefabInstance(scene, root, blob, &again));
    CHECK(again.entitiesCreated == 1);
    CHECK(reg.get<HierarchyComponent>(bulb).children.size() == 1);
}

TEST_CASE("PrefabUndo: an edit's override entry goes with the undo and comes back with the redo")
{
    Template t;
    HorizonWorld scene;
    const auto blob = t.capture();
    const Placed p = place(scene, blob, HE::UUID::generate());
    auto& reg = scene.registry();
    EditorUndo undo;
    undo.setWorld(&scene);
    SceneSerializer ser;

    // The editor's order: snapshot, the edit, then the recorder (next frame).
    undo.snapshotNow();
    reg.get<LightComponent>(p.bulb).intensity = 7.0f;
    REQUIRE(ser.recordPrefabOverrides(scene, p.root, p.bulb, blob) == 1);

    REQUIRE(undo.undo());
    Entity root = entt::null;
    for (auto e : reg.view<PrefabInstanceComponent>()) root = e;
    REQUIRE((root != entt::null));
    CHECK(reg.get<PrefabInstanceComponent>(root).overrides.empty());
    Entity bulb = childNamed(scene, root, "Bulb");
    CHECK(reg.get<LightComponent>(bulb).intensity == doctest::Approx(1.0f));
    // The recorder, run over the restored entity (the editor does, with last
    // frame's selection), finds nothing to record: it equals the template.
    CHECK(ser.recordPrefabOverrides(scene, root, bulb, blob) == 0);

    REQUIRE(undo.redo());
    for (auto e : reg.view<PrefabInstanceComponent>()) root = e;
    bulb = childNamed(scene, root, "Bulb");
    CHECK(reg.get<LightComponent>(bulb).intensity == doctest::Approx(7.0f));
    CHECK(reg.get<PrefabInstanceComponent>(root).hasOverride(t.tBulb, "light", "intensity"));
    // And the sync keeps it, as it did before the undo.
    REQUIRE(ser.syncPrefabInstance(scene, root, blob));
    CHECK(reg.get<LightComponent>(bulb).intensity == doctest::Approx(7.0f));
}

TEST_CASE("PrefabUndo: a lost-record removal undone comes back, and the next sync removes it again")
{
    Template t;
    HorizonWorld scene;
    const Placed p = place(scene, t.capture(), HE::UUID::generate());
    auto& reg = scene.registry();
    EditorUndo undo;
    undo.setWorld(&scene);
    SceneSerializer ser;

    t.world.destroyEntity(t.bulb);
    const auto blob = t.capture();
    undo.snapshotNow();
    SceneSerializer::PrefabSyncReport rep;
    REQUIRE(ser.syncPrefabInstance(scene, p.root, blob, &rep));
    CHECK(rep.entitiesRemoved == 1);

    REQUIRE(undo.undo());
    Entity root = entt::null;
    for (auto e : reg.view<PrefabInstanceComponent>()) root = e;
    REQUIRE((root != entt::null));
    CHECK((childNamed(scene, root, "Bulb") != entt::null));
    CHECK(reg.get<PrefabInstanceComponent>(root).bindings.size() == 2);

    SceneSerializer::PrefabSyncReport again;
    REQUIRE(ser.syncPrefabInstance(scene, root, blob, &again));
    CHECK(again.entitiesRemoved == 1);
    CHECK((childNamed(scene, root, "Bulb") == entt::null));
}
