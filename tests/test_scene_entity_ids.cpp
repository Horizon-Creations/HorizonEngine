#include "doctest.h"
#include "TestFsUtil.h"

#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneJsonParse.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/EntityIdComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <HorizonScene/Components/NameComponent.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// ─── Stable entity identity ──────────────────────────────────────────────────
// Entities used to be serialised by their entt handle — a dense allocator index.
// That made scene files merge-hostile: two people who each add an entity on their
// own branch both get the same next handle, the added JSON blocks do not overlap
// textually, so git merges them without a conflict and the result is one file
// where two entities claim one identity. Everything that referenced that number,
// including hierarchy links from entities neither person touched, then resolves
// to whichever was created last — in a file that parses and loads without error.
//
// These tests pin the properties that make that impossible.

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

fs::path uniqueScenePath(const char* stem)
{
	// Salted per process rather than by PID, which needs a different header on
	// each platform: two concurrent test binaries must not collide, and neither
	// must two calls within one run.
	static const std::uint64_t salt = HE::UUID::generate().lo;
	static int counter = 0;
	return fs::temp_directory_path() /
	       ("he_entity_ids_" + std::string(stem) + "_" + std::to_string(salt) + "_" +
	        std::to_string(counter++) + ".hescene");
}

Entity childNamed(const HorizonWorld& world, Entity parent, const std::string& name)
{
	auto& reg = const_cast<HorizonWorld&>(world).registry();
	const auto* h = reg.try_get<HierarchyComponent>(parent);
	if (!h) return entt::null;
	for (Entity c : h->children)
	{
		if (const auto* n = reg.try_get<NameComponent>(c); n && n->name == name)
			return c;
	}
	return entt::null;
}

} // namespace

TEST_CASE("Every entity is born with a distinct id")
{
	HorizonWorld world;

	const HE::UUID rootId = world.entityId(world.rootEntity());
	CHECK(rootId != HE::UUID{});

	std::set<std::pair<std::uint64_t, std::uint64_t>> seen;
	seen.insert({ rootId.hi, rootId.lo });

	for (int i = 0; i < 64; ++i)
	{
		const HE::UUID id = world.entityId(world.createEntity("E" + std::to_string(i)));
		CHECK(id != HE::UUID{});
		// The whole point: independently created entities never collide.
		CHECK(seen.insert({ id.hi, id.lo }).second);
	}
}

TEST_CASE("An entity keeps its id across a save/load round trip")
{
	const fs::path file = uniqueScenePath("roundtrip");

	HorizonWorld world;
	const Entity a = world.createEntity("Alpha");
	const Entity b = world.createEntity("Beta");
	world.reparentEntity(b, a);

	const HE::UUID rootId = world.entityId(world.rootEntity());
	const HE::UUID idA    = world.entityId(a);
	const HE::UUID idB    = world.entityId(b);

	SceneSerializer ser;
	REQUIRE(ser.save(world, file, SerializeFormat::JSON));

	HorizonWorld loaded;
	REQUIRE(ser.load(loaded, file, SerializeFormat::JSON));

	// Identity survives, including the root's — the root is the one entity the
	// loader maps onto an existing entity rather than creating.
	CHECK(loaded.entityId(loaded.rootEntity()) == rootId);

	const Entity la = loaded.findByEntityId(idA);
	const Entity lb = loaded.findByEntityId(idB);
	REQUIRE((la != entt::null));
	REQUIRE((lb != entt::null));

	auto& reg = loaded.registry();
	CHECK(reg.get<NameComponent>(la).name == "Alpha");
	CHECK(reg.get<NameComponent>(lb).name == "Beta");
	// And the hierarchy is restored through those ids, not through handles.
	CHECK(reg.get<HierarchyComponent>(lb).parent == la);

	he_test::removeQuiet(file);
}

TEST_CASE("The same prefab instantiated twice yields two identities")
{
	// This is the trap in giving entities stable ids: a prefab is a *template*,
	// so restoring the stored id on instantiation would reintroduce exactly the
	// duplicate-identity bug the ids exist to prevent — one insertion per copy,
	// both claiming the same id.
	HorizonWorld world;
	const Entity src   = world.createEntity("Turret");
	const Entity barrel = world.createEntity("Barrel");
	world.reparentEntity(barrel, src);

	SceneSerializer ser;
	const std::vector<std::uint8_t> blob = ser.serializeSubtree(world, src);
	REQUIRE(!blob.empty());

	const Entity first  = ser.instantiatePrefab(world, blob, world.rootEntity());
	const Entity second = ser.instantiatePrefab(world, blob, world.rootEntity());
	REQUIRE((first != entt::null));
	REQUIRE((second != entt::null));

	const HE::UUID idFirst  = world.entityId(first);
	const HE::UUID idSecond = world.entityId(second);
	CHECK(idFirst  != HE::UUID{});
	CHECK(idSecond != HE::UUID{});
	CHECK(idFirst  != idSecond);
	// …and neither copy stole the source entity's identity.
	CHECK(idFirst  != world.entityId(src));
	CHECK(idSecond != world.entityId(src));

	// Children too — a prefab is a subtree, not a single entity.
	const Entity b1 = childNamed(world, first,  "Barrel");
	const Entity b2 = childNamed(world, second, "Barrel");
	REQUIRE((b1 != entt::null));
	REQUIRE((b2 != entt::null));
	CHECK(world.entityId(b1) != world.entityId(b2));
	CHECK(world.entityId(b1) != world.entityId(barrel));
}

TEST_CASE("Loading a scene additively twice yields two instances, not one")
{
	// Same reasoning as prefabs: an additive load grafts a copy into a world that
	// may already hold one, so restoring the stored ids would make the second
	// graft claim the first's identities.
	const fs::path file = uniqueScenePath("additive");

	HorizonWorld source;
	source.createEntity("Zone");

	SceneSerializer ser;
	REQUIRE(ser.save(source, file, SerializeFormat::JSON));

	HorizonWorld target;
	std::vector<Entity> firstBatch, secondBatch;
	REQUIRE(ser.loadAdditive(target, file, SerializeFormat::JSON, &firstBatch));
	REQUIRE(ser.loadAdditive(target, file, SerializeFormat::JSON, &secondBatch));
	REQUIRE(!firstBatch.empty());
	REQUIRE(!secondBatch.empty());

	std::set<std::pair<std::uint64_t, std::uint64_t>> seen;
	for (Entity e : firstBatch)
	{
		const HE::UUID id = target.entityId(e);
		CHECK(id != HE::UUID{});
		CHECK(seen.insert({ id.hi, id.lo }).second);
	}
	for (Entity e : secondBatch)
	{
		const HE::UUID id = target.entityId(e);
		CHECK(id != HE::UUID{});
		CHECK(seen.insert({ id.hi, id.lo }).second);
	}

	he_test::removeQuiet(file);
}

TEST_CASE("A scene written in the old handle-id format still loads")
{
	// Files written before stable ids address entities by uint32 handle, with
	// 0xFFFFFFFF for "no parent". Those must keep loading, hierarchy intact; the
	// entities simply keep the ids minted at creation and gain stable ones from
	// the next save onward.
	const fs::path file = uniqueScenePath("legacy");

	json scene;
	scene["version"] = "1.1";
	json entities = json::array();
	{
		json root;
		root["id"]       = 0u;
		root["name"]     = "World";
		root["parent"]   = 0xFFFFFFFFu;      // legacy null sentinel
		root["children"] = json::array({ 1u });
		entities.push_back(root);

		json child;
		child["id"]       = 1u;
		child["name"]     = "LegacyChild";
		child["parent"]   = 0u;
		child["children"] = json::array();
		entities.push_back(child);
	}
	scene["entities"] = entities;

	{
		std::ofstream out(file);
		REQUIRE(out.is_open());
		out << scene.dump(4);
	}

	HorizonWorld loaded;
	SceneSerializer ser;
	REQUIRE(ser.load(loaded, file, SerializeFormat::JSON));

	auto& reg = loaded.registry();
	CHECK(reg.get<NameComponent>(loaded.rootEntity()).name == "World");

	const Entity child = childNamed(loaded, loaded.rootEntity(), "LegacyChild");
	REQUIRE((child != entt::null));
	CHECK(reg.get<HierarchyComponent>(child).parent == loaded.rootEntity());
	// It got an id even though the file carried none.
	CHECK(loaded.entityId(child) != HE::UUID{});

	he_test::removeQuiet(file);
}

TEST_CASE("Two entities added on separate branches both survive a merge")
{
	// The scenario the whole change exists for, reproduced without git: build the
	// file that a textual three-way merge of two independent additions produces,
	// and require that both entities load with the right parent.
	//
	// Before stable ids this file could not even be constructed correctly — both
	// additions would carry the same handle, and one would silently win.
	const fs::path base   = uniqueScenePath("merge_base");
	const fs::path merged = uniqueScenePath("merge_result");

	SceneSerializer ser;

	// A shared starting point.
	HorizonWorld baseWorld;
	baseWorld.createEntity("Existing");
	REQUIRE(ser.save(baseWorld, base, SerializeFormat::JSON));

	// Two people load it and each add one entity.
	auto branchAddition = [&](const char* name) {
		HorizonWorld w;
		REQUIRE(ser.load(w, base, SerializeFormat::JSON));
		w.createEntity(name);
		const fs::path p = uniqueScenePath(name);
		REQUIRE(ser.save(w, p, SerializeFormat::JSON));
		std::ifstream in(p);
		json j = json::parse(in, nullptr, false);
		he_test::removeQuiet(p);
		REQUIRE(!j.is_discarded());
		return j;
	};

	const json branchA = branchAddition("FromAlice");
	const json branchB = branchAddition("FromBob");

	// Splice: take branch A whole, then append Bob's new entity and add it to the
	// root's children — which is what a clean textual merge of the two produces.
	json mergedScene = branchA;
	auto findByName = [](const json& scene, const char* name) -> json {
		for (const auto& e : scene["entities"])
			if (e.value("name", "") == name) return e;
		return {};
	};
	const json bobEntity = findByName(branchB, "FromBob");
	REQUIRE(!bobEntity.is_null());
	mergedScene["entities"].push_back(bobEntity);

	for (auto& e : mergedScene["entities"])
	{
		if (e.value("name", "") != "World") continue;
		e["children"].push_back(bobEntity["uuid"]);
	}

	{
		std::ofstream out(merged);
		REQUIRE(out.is_open());
		out << mergedScene.dump(4);
	}

	HorizonWorld loaded;
	REQUIRE(ser.load(loaded, merged, SerializeFormat::JSON));

	// All three entities are present, each under the root — nothing was silently
	// dropped or re-parented.
	const Entity existing = childNamed(loaded, loaded.rootEntity(), "Existing");
	const Entity alice    = childNamed(loaded, loaded.rootEntity(), "FromAlice");
	const Entity bob      = childNamed(loaded, loaded.rootEntity(), "FromBob");
	CHECK((existing != entt::null));
	CHECK((alice != entt::null));
	CHECK((bob != entt::null));

	CHECK(loaded.entityId(alice) != loaded.entityId(bob));

	he_test::removeQuiet(base);
	he_test::removeQuiet(merged);
}

// ─── The id index (Thema 164, step 2a) ────────────────────────────────────────
// findByEntityId used to scan every EntityIdComponent. Cells make that matter:
// references by id are resolved per frame (rope, camera rig, IK, joints,
// sequencer), the base holds the whole world's worth of entities, and a cell being
// loaded asks "is this id taken" once per entity. The index behind it is fed by
// the registry's own signals, not by the callers, because the id is written at
// more places than one API can cover (createEntity, setEntityId, a scene load, a
// collab peer's blob, a test that emplaces the component). These tests walk every
// way an id can appear, change or go.

TEST_CASE("findByEntityId follows every way an id can be set, replaced or removed")
{
	HorizonWorld world;
	auto& reg = world.registry();

	// Born with an id, the root included; the zero id is "no identity", never a hit.
	const Entity a     = world.createEntity("A");
	const HE::UUID idA = world.entityId(a);
	CHECK((world.findByEntityId(idA) == a));
	CHECK((world.findByEntityId(world.entityId(world.rootEntity())) == world.rootEntity()));
	CHECK((world.findByEntityId(HE::UUID{}) == entt::null));
	CHECK((world.findByEntityId(HE::UUID::generate()) == entt::null));

	// setEntityId: the old id stops resolving the moment the new one does.
	const HE::UUID idA2 = HE::UUID::generate();
	world.setEntityId(a, idA2);
	CHECK((world.findByEntityId(idA) == entt::null));
	CHECK((world.findByEntityId(idA2) == a));

	// A writer that never heard of setEntityId: emplace_or_replace on a component
	// the entity has (what applyPrefabJson does for a collab peer's subtree) ...
	const HE::UUID idA3 = HE::UUID::generate();
	reg.emplace_or_replace<EntityIdComponent>(a, EntityIdComponent{ idA3 });
	CHECK((world.findByEntityId(idA2) == entt::null));
	CHECK((world.findByEntityId(idA3) == a));
	// ... patch, which is how a caller changes one field of a component in place ...
	const HE::UUID idA4 = HE::UUID::generate();
	reg.patch<EntityIdComponent>(a, [&](EntityIdComponent& c) { c.id = idA4; });
	CHECK((world.findByEntityId(idA3) == entt::null));
	CHECK((world.findByEntityId(idA4) == a));
	// ... and an entity that did not have the component at all.
	const Entity raw       = reg.create();
	const HE::UUID idRaw   = HE::UUID::generate();
	reg.emplace<EntityIdComponent>(raw, EntityIdComponent{ idRaw });
	CHECK((world.findByEntityId(idRaw) == raw));

	// Setting the zero id takes an entity out of the index rather than indexing "none".
	world.setEntityId(a, HE::UUID{});
	CHECK((world.findByEntityId(idA4) == entt::null));
	CHECK((world.findByEntityId(HE::UUID{}) == entt::null));
	world.setEntityId(a, idA4);
	CHECK((world.findByEntityId(idA4) == a));

	// Taking the component away.
	reg.remove<EntityIdComponent>(raw);
	CHECK((world.findByEntityId(idRaw) == entt::null));
	reg.destroy(raw);

	// Destroying the entity, and a handle that is reused afterwards: the new
	// occupant of the slot has its own id and the dead one's answers nothing.
	const Entity b     = world.createEntity("B");
	const HE::UUID idB = world.entityId(b);
	world.destroyEntity(b);
	CHECK((world.findByEntityId(idB) == entt::null));
	const Entity c = world.createEntity("C");
	CHECK((world.findByEntityId(idB) == entt::null));
	CHECK((world.findByEntityId(world.entityId(c)) == c));

	// A subtree goes with its root.
	const Entity parent = world.createEntity("Parent");
	const Entity child  = world.createEntity("Child");
	world.reparentEntity(child, parent);
	const HE::UUID idParent = world.entityId(parent), idChild = world.entityId(child);
	world.destroyEntity(parent);
	CHECK((world.findByEntityId(idParent) == entt::null));
	CHECK((world.findByEntityId(idChild) == entt::null));

	// clear() drops everything but the root.
	const HE::UUID idRoot = world.entityId(world.rootEntity());
	const HE::UUID idC    = world.entityId(c);
	world.clear();
	CHECK((world.findByEntityId(idA4) == entt::null));
	CHECK((world.findByEntityId(idC) == entt::null));
	CHECK((world.findByEntityId(idRoot) == world.rootEntity()));
}

TEST_CASE("findByEntityId: two holders of one id, and the id survives the first one dying")
{
	// A damaged merge or a pasted blob can leave two entities with one id. Which of
	// them answers is not defined, but it must be one of them, never a stale handle,
	// and the other must still answer once the first is gone — the scan this index
	// replaced found whichever was left.
	HorizonWorld world;
	const Entity a = world.createEntity("A");
	const Entity b = world.createEntity("B");
	const HE::UUID shared = HE::UUID::generate();
	world.setEntityId(a, shared);
	world.setEntityId(b, shared);

	const Entity first = world.findByEntityId(shared);
	CHECK((first == a || first == b));

	world.destroyEntity(first);
	const Entity other = first == a ? b : a;
	CHECK((world.findByEntityId(shared) == other));

	// And when the survivor takes another id, the shared one is free again.
	world.setEntityId(other, HE::UUID::generate());
	CHECK((world.findByEntityId(shared) == entt::null));
}

TEST_CASE("findByEntityId agrees with a scan of the registry after thousands of random edits")
{
	HorizonWorld world;
	std::mt19937 rng(0x164u);
	std::vector<HE::UUID> everUsed;
	std::vector<Entity>   live;

	const auto freshId = [&]
	{
		const HE::UUID id = HE::UUID::generate();
		everUsed.push_back(id);
		return id;
	};
	const auto pick = [&](size_t n) { return static_cast<size_t>(rng() % n); };

	const auto verify = [&]
	{
		// One pass over the registry says who holds what ...
		std::unordered_map<HE::UUID, int> held;
		for (auto [e, c] : world.registry().view<EntityIdComponent>().each())
			if (c.id != HE::UUID{}) ++held[c.id];
		// ... every id somebody holds resolves to a live entity that really holds it ...
		for (const auto& [id, count] : held)
		{
			(void)count;
			const Entity found = world.findByEntityId(id);
			REQUIRE((found != entt::null));
			REQUIRE(world.registry().valid(found));
			REQUIRE(world.entityId(found) == id);
		}
		// ... and every id that ever existed resolves exactly when somebody holds it.
		for (const HE::UUID& id : everUsed)
			CHECK((world.findByEntityId(id) != entt::null) == (held.count(id) != 0));
	};

	for (int step = 0; step < 4000; ++step)
	{
		const unsigned op = rng() % 100;
		if (op < 35 || live.empty())
		{
			const Entity e = world.createEntity("E");
			everUsed.push_back(world.entityId(e));
			if (!live.empty() && rng() % 2 == 0) world.reparentEntity(e, live[pick(live.size())]);
			live.push_back(e);
		}
		else if (op < 55)
		{
			world.destroyEntity(live[pick(live.size())]);   // takes a subtree with it
			live.erase(std::remove_if(live.begin(), live.end(),
			           [&](Entity e) { return !world.registry().valid(e); }), live.end());
		}
		else if (op < 75)
		{
			world.setEntityId(live[pick(live.size())], freshId());
		}
		else if (op < 85)
		{
			// Copy another entity's id: a deliberate duplicate.
			const Entity from = live[pick(live.size())];
			const Entity to   = live[pick(live.size())];
			world.setEntityId(to, world.entityId(from));
		}
		else if (op < 92)
		{
			const Entity e = live[pick(live.size())];
			world.registry().emplace_or_replace<EntityIdComponent>(e, EntityIdComponent{ freshId() });
		}
		else if (op < 96)
		{
			const Entity e = live[pick(live.size())];
			world.registry().patch<EntityIdComponent>(e, [&](EntityIdComponent& c) { c.id = freshId(); });
		}
		else
		{
			world.setEntityId(live[pick(live.size())], HE::UUID{});
		}
		if (step % 97 == 0) verify();
	}
	verify();
}

// ─── Additive load that keeps the stored ids (Thema 164, step 2a) ─────────────
// The default stays what the test above pins: a graft mints fresh ids, so a scene
// grafted twice is two instances. A streamed cell is the exception — it is in the
// world at most once, and what refers to its entities by id (a prefab placement's
// bindings, a joint, a rig's target) has to find them again after an unload.

namespace {

// A small scene's JSON, and the ids its named entities have in the world it came from.
json villageScene(HorizonWorld& source, std::map<std::string, HE::UUID>& ids)
{
	const Entity hall = source.createEntity("Hall");
	const Entity door = source.createEntity("Door");
	source.reparentEntity(door, hall);
	const Entity well = source.createEntity("Well");
	ids["Hall"] = source.entityId(hall);
	ids["Door"] = source.entityId(door);
	ids["Well"] = source.entityId(well);
	SceneSerializer ser;
	std::vector<std::uint8_t> bytes;
	REQUIRE(ser.saveToMemory(source, bytes));
	return HE::parseSceneCbor(bytes);
}

} // namespace

TEST_CASE("Additive load with preserveIds restores the stored ids, and a second graft takes none of them")
{
	HorizonWorld source;
	std::map<std::string, HE::UUID> ids;
	const json scene = villageScene(source, ids);
	const size_t records = scene["entities"].size();   // the source's root, Hall, Door, Well

	SceneSerializer ser;
	HorizonWorld target;
	size_t collisions = 0;
	SceneSerializer::AdditiveOptions keep;
	keep.preserveIds  = true;
	keep.idCollisions = &collisions;

	std::vector<Entity> first;
	REQUIRE(ser.loadAdditiveFromJson(target, scene, &first, keep));
	CHECK(collisions == 0);
	CHECK(first.size() == records);
	// Every named record is back under the id it had, in the hierarchy it was saved in.
	for (const auto& [name, id] : ids)
	{
		const Entity e = target.findByEntityId(id);
		REQUIRE((e != entt::null));
		CHECK(target.registry().get<NameComponent>(e).name == name);
	}
	// The source's own root came along as a child of the target's, with its id too.
	CHECK((target.findByEntityId(source.entityId(source.rootEntity())) != entt::null));
	const Entity hall = target.findByEntityId(ids.at("Hall"));
	CHECK((childNamed(target, hall, "Door") == target.findByEntityId(ids.at("Door"))));

	// The same file again: every record collides, so every entity keeps the fresh id
	// it was created with, nothing is taken from the first graft, and the second
	// graft's hierarchy is its own (the records are still addressed by the stored
	// ids while the links are rebuilt).
	std::vector<Entity> second;
	REQUIRE(ser.loadAdditiveFromJson(target, scene, &second, keep));
	CHECK(collisions == records);
	CHECK(second.size() == records);
	for (const auto& [name, id] : ids)
		CHECK(target.registry().get<NameComponent>(target.findByEntityId(id)).name == name);
	std::set<std::pair<std::uint64_t, std::uint64_t>> seen;
	for (Entity e : first)  CHECK(seen.insert({ target.entityId(e).hi, target.entityId(e).lo }).second);
	for (Entity e : second) CHECK(seen.insert({ target.entityId(e).hi, target.entityId(e).lo }).second);
	Entity secondHall = entt::null;
	for (Entity e : second)
		if (target.registry().get<NameComponent>(e).name == "Hall") secondHall = e;
	REQUIRE((secondHall != entt::null));
	CHECK((secondHall != hall));
	const Entity secondDoor = childNamed(target, secondHall, "Door");
	REQUIRE((secondDoor != entt::null));
	CHECK((secondDoor != target.findByEntityId(ids.at("Door"))));

	// Without the option nothing changes: fresh ids, no collisions counted.
	HorizonWorld plain;
	size_t none = 0;
	SceneSerializer::AdditiveOptions defaults;
	defaults.idCollisions = &none;
	std::vector<Entity> grafted;
	REQUIRE(ser.loadAdditiveFromJson(plain, scene, &grafted, defaults));
	CHECK(none == 0);
	for (const auto& [name, id] : ids)
		CHECK((plain.findByEntityId(id) == entt::null));
}

TEST_CASE("Additive load with preserveIds: records without a stored id keep the one minted for them")
{
	// A scene from before stable ids addresses entities by handle and carries no
	// uuid; there is nothing to restore, and it must still load.
	json scene;
	scene["version"] = "1.1";
	json root;
	root["id"]       = 0u;
	root["name"]     = "World";
	root["parent"]   = 0xFFFFFFFFu;
	root["children"] = json::array({ 1u });
	json child;
	child["id"]       = 1u;
	child["name"]     = "LegacyChild";
	child["parent"]   = 0u;
	child["children"] = json::array();
	scene["entities"] = json::array({ root, child });

	HorizonWorld target;
	SceneSerializer ser;
	SceneSerializer::AdditiveOptions keep;
	keep.preserveIds = true;
	std::vector<Entity> created;
	REQUIRE(ser.loadAdditiveFromJson(target, scene, &created, keep));
	REQUIRE(created.size() == 2);
	for (Entity e : created) CHECK(target.entityId(e) != HE::UUID{});
	CHECK(target.entityId(created[0]) != target.entityId(created[1]));
}
