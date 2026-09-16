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

// ── Labels and multi-step jumps (the Undo History window) ────────────────────

TEST_CASE("EditorUndo labels: explicit, then the context scope, then a plain Edit")
{
	HorizonWorld world;
	EditorUndo undo;
	undo.setWorld(&world);

	undo.snapshotNow("Create Entity"); world.createEntity("A");
	{
		EditorUndo::Context scope(&undo, "Light");
		undo.snapshotNow(); world.createEntity("B");         // inherits "Light"
		undo.capturePre();
		undo.stashPre();                                      // label decided NOW
		{
			EditorUndo::Context inner(&undo, "Rigid Body");
			undo.commitPending();                             // …not at commit
		}
		world.createEntity("C");
	}
	undo.snapshotNow(); world.createEntity("D");             // scope unwound → "Edit"

	REQUIRE(undo.undoDepth() == 4);
	CHECK(undo.undoLabelAt(0) == "Create Entity");
	CHECK(undo.undoLabelAt(1) == "Light");
	CHECK(undo.undoLabelAt(2) == "Light");
	CHECK(undo.undoLabelAt(3) == "Edit");
	CHECK(undo.undoLabel() == "Edit");
	CHECK(undo.redoLabel().empty());
	CHECK(undo.context().empty());
}

TEST_CASE("EditorUndo undo/redo carry the label of the operation they take back / bring back")
{
	HorizonWorld world;
	EditorUndo undo;
	undo.setWorld(&world);

	undo.snapshotNow("A"); world.createEntity("A");
	undo.snapshotNow("B"); world.createEntity("B");
	undo.snapshotNow("C"); world.createEntity("C");

	CHECK(undo.undo());                       // takes back C
	CHECK(undo.undoLabel() == "B");
	CHECK(undo.redoLabel() == "C");           // NOT "B": what redo brings back is C
	CHECK(undo.undo());                       // takes back B
	CHECK(undo.undoLabel() == "A");
	REQUIRE(undo.redoDepth() == 2);
	CHECK(undo.redoLabelAt(1) == "B");        // top of the redo stack: next Ctrl+Y
	CHECK(undo.redoLabelAt(0) == "C");
	CHECK(undo.redo());                       // brings back B
	CHECK(undo.undoLabel() == "B");
	CHECK(undo.redoLabel() == "C");
	CHECK(entityCount(world) == 3);           // root + A + B
}

TEST_CASE("EditorUndo undoSteps/redoSteps jump several entries in one restore, labels intact")
{
	HorizonWorld world;
	EditorUndo undo;
	undo.setWorld(&world);

	undo.snapshotNow("A"); world.createEntity("A");
	undo.snapshotNow("B"); world.createEntity("B");
	undo.snapshotNow("C"); world.createEntity("C");
	undo.snapshotNow("D"); world.createEntity("D");
	REQUIRE(entityCount(world) == 5);

	// Three back at once: the world is as it was before B (root + A).
	const uint64_t rev = undo.revision();
	CHECK(undo.undoSteps(3));
	CHECK(undo.revision() == rev + 1);       // one mutation, not three
	CHECK(entityCount(world) == 2);
	CHECK((findByName(world, "A") != entt::null));
	CHECK((findByName(world, "B") == entt::null));
	REQUIRE(undo.undoDepth() == 1);
	REQUIRE(undo.redoDepth() == 3);
	CHECK(undo.undoLabelAt(0) == "A");
	// Redo stack top-down = the order Ctrl+Y would bring them back: B, C, D.
	CHECK(undo.redoLabelAt(2) == "B");
	CHECK(undo.redoLabelAt(1) == "C");
	CHECK(undo.redoLabelAt(0) == "D");

	// Two forward at once: after C, before D.
	CHECK(undo.redoSteps(2));
	CHECK(entityCount(world) == 4);
	CHECK((findByName(world, "C") != entt::null));
	CHECK((findByName(world, "D") == entt::null));
	REQUIRE(undo.undoDepth() == 3);
	CHECK(undo.undoLabelAt(0) == "A");
	CHECK(undo.undoLabelAt(1) == "B");
	CHECK(undo.undoLabelAt(2) == "C");
	CHECK(undo.redoLabel() == "D");

	// Clamped: asking for more than there is takes everything, no crash.
	CHECK(undo.redoSteps(10));
	CHECK(entityCount(world) == 5);
	CHECK_FALSE(undo.canRedo());
	CHECK(undo.undoSteps(99));
	CHECK(entityCount(world) == 1);
	CHECK_FALSE(undo.canUndo());
	CHECK(undo.redoDepth() == 4);
	CHECK_FALSE(undo.undoSteps(1));           // nothing left: false, no mutation
	CHECK_FALSE(undo.redoSteps(0));
}
