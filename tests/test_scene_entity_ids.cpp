#include "doctest.h"
#include "TestFsUtil.h"

#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/EntityIdComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <HorizonScene/Components/NameComponent.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

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
