#include "doctest.h"
#include "EditorUndo.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/EnvironmentLightComponent.h>

namespace
{
	// Counts the root + authored scene entities, excluding the two built-in
	// environment sun/moon lights (which every world carries automatically).
	int entityCount(HorizonWorld& w)
	{
		auto& reg = w.registry();
		int n = 0;
		for (auto e : reg.view<entt::entity>())
			if (!reg.all_of<EnvironmentLightComponent>(e)) ++n;
		return n;
	}

	Entity findByName(HorizonWorld& w, const std::string& name)
	{
		for (auto [e, n] : w.registry().view<NameComponent>().each())
			if (n.name == name) return e;
		return entt::null;
	}
}

TEST_CASE("EditorUndo reverts structural changes")
{
	HorizonWorld world;
	EditorUndo undo;
	undo.setWorld(&world);
	CHECK_FALSE(undo.canUndo());

	undo.snapshotNow();
	world.createEntity("Added");
	REQUIRE(entityCount(world) == 2);
	REQUIRE(undo.canUndo());

	CHECK(undo.undo());
	CHECK(entityCount(world) == 1);
	CHECK((findByName(world, "Added") == entt::null));
	CHECK(undo.canRedo());

	CHECK(undo.redo());
	CHECK(entityCount(world) == 2);
	CHECK((findByName(world, "Added") != entt::null));
}

TEST_CASE("EditorUndo edit-session pattern reverts component values")
{
	HorizonWorld world;
	Entity e = world.createEntity("Thing");
	TransformComponent t;
	t.position = { 1, 2, 3 };
	world.addComponent(e, t);

	EditorUndo undo;
	undo.setWorld(&world);

	// Simulates: capturePre each frame, stash on activation, mutate, commit
	undo.capturePre();
	undo.stashPre();
	world.registry().get<TransformComponent>(e).position = { 9, 9, 9 };
	undo.commitPending();

	REQUIRE(undo.canUndo());
	REQUIRE(undo.undo());

	Entity loaded = findByName(world, "Thing");
	REQUIRE((loaded != entt::null));
	CHECK(world.registry().get<TransformComponent>(loaded).position.x == doctest::Approx(1.0f));

	REQUIRE(undo.redo());
	loaded = findByName(world, "Thing");
	CHECK(world.registry().get<TransformComponent>(loaded).position.x == doctest::Approx(9.0f));
}

TEST_CASE("EditorUndo new edit clears the redo stack")
{
	HorizonWorld world;
	EditorUndo undo;
	undo.setWorld(&world);

	undo.snapshotNow();
	world.createEntity("A");
	undo.undo();
	REQUIRE(undo.canRedo());

	undo.snapshotNow();   // diverging edit
	world.createEntity("B");
	CHECK_FALSE(undo.canRedo());
}

TEST_CASE("EditorUndo multi-level undo and redo")
{
	HorizonWorld world;
	EditorUndo undo;
	undo.setWorld(&world);

	// Build 3 undo-able edits
	undo.snapshotNow(); world.createEntity("A");
	undo.snapshotNow(); world.createEntity("B");
	undo.snapshotNow(); world.createEntity("C");
	CHECK(entityCount(world) == 4); // root + A + B + C

	CHECK(undo.undo()); CHECK(entityCount(world) == 3);
	CHECK((findByName(world, "C") == entt::null));
	CHECK(undo.undo()); CHECK(entityCount(world) == 2);
	CHECK((findByName(world, "B") == entt::null));
	CHECK(undo.undo()); CHECK(entityCount(world) == 1);
	CHECK_FALSE(undo.canUndo());

	// All three redo steps restore entities in order
	CHECK(undo.redo()); CHECK(entityCount(world) == 2);
	CHECK(undo.redo()); CHECK(entityCount(world) == 3);
	CHECK(undo.redo()); CHECK(entityCount(world) == 4);
	CHECK_FALSE(undo.canRedo());
}

TEST_CASE("EditorUndo revision counter increments on every mutation")
{
	HorizonWorld world;
	EditorUndo undo;
	undo.setWorld(&world);
	const uint64_t base = undo.revision();

	undo.snapshotNow();
	world.createEntity("X");
	CHECK(undo.revision() == base + 1); // push increments

	undo.undo();
	CHECK(undo.revision() == base + 2); // undo increments

	undo.redo();
	CHECK(undo.revision() == base + 3); // redo increments
}

TEST_CASE("EditorUndo clearHistory wipes both stacks without changing the world")
{
	HorizonWorld world;
	EditorUndo undo;
	undo.setWorld(&world);

	undo.snapshotNow(); world.createEntity("A");
	undo.snapshotNow(); world.createEntity("B");
	REQUIRE(undo.canUndo());

	undo.undo(); // restores to pre-B state; canRedo is now true
	REQUIRE(undo.canRedo());

	undo.clearHistory();
	CHECK_FALSE(undo.canUndo());
	CHECK_FALSE(undo.canRedo());
	// World state is unchanged — only history is gone
	CHECK(entityCount(world) == 2); // root + A (undo already reverted B)
}
