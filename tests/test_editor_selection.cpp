#include "doctest.h"
#include "EditorSelection.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <algorithm>

// The selection set the outliner, viewport and inspector share. Everything here
// is handles only — no ImGui, no world except where prune() needs a registry to
// ask.

TEST_CASE("EditorSelection: set replaces, null clears")
{
	EditorSelection sel;
	CHECK(sel.empty());
	CHECK((sel.primary() == entt::null));

	const Entity a = static_cast<Entity>(1);
	const Entity b = static_cast<Entity>(2);
	sel.set(a);
	CHECK(sel.size() == 1);
	CHECK(sel.primary() == a);
	CHECK(sel.anchor() == a);
	CHECK(sel.contains(a));

	sel.set(b);
	CHECK(sel.size() == 1);
	CHECK_FALSE(sel.contains(a));
	CHECK(sel.primary() == b);

	sel.set(entt::null);
	CHECK(sel.empty());
	CHECK((sel.primary() == entt::null));
	CHECK((sel.anchor() == entt::null));
}

TEST_CASE("EditorSelection: add keeps order, no duplicates, last is primary")
{
	EditorSelection sel;
	const Entity a = static_cast<Entity>(1);
	const Entity b = static_cast<Entity>(2);
	const Entity c = static_cast<Entity>(3);
	sel.add(a);
	sel.add(b);
	sel.add(c);
	REQUIRE(sel.size() == 3);
	CHECK(sel.entities()[0] == a);
	CHECK(sel.entities()[1] == b);
	CHECK(sel.entities()[2] == c);
	CHECK(sel.primary() == c);

	// Re-adding a member moves it to the back: the row just clicked is primary.
	sel.add(a);
	REQUIRE(sel.size() == 3);
	CHECK(sel.primary() == a);
	CHECK(sel.entities()[0] == b);

	// Null never enters the set.
	sel.add(entt::null);
	CHECK(sel.size() == 3);
}

TEST_CASE("EditorSelection: toggle and remove")
{
	EditorSelection sel;
	const Entity a = static_cast<Entity>(1);
	const Entity b = static_cast<Entity>(2);
	sel.toggle(a);
	sel.toggle(b);
	CHECK(sel.size() == 2);
	CHECK(sel.anchor() == b);

	sel.toggle(b); // Ctrl-click on a selected row deselects it
	CHECK(sel.size() == 1);
	CHECK(sel.primary() == a);
	// The anchor was b and b is gone — it falls back to the primary rather than
	// dangling on a handle that is no longer selected.
	CHECK(sel.anchor() == a);

	const std::uint64_t rev = sel.revision();
	sel.remove(b); // not a member: nothing changes, revision included
	CHECK(sel.revision() == rev);
	sel.remove(a);
	CHECK(sel.empty());
	CHECK(sel.revision() != rev);
}

TEST_CASE("EditorSelection: ranges leave the anchor alone")
{
	EditorSelection sel;
	const Entity a = static_cast<Entity>(1);
	const Entity b = static_cast<Entity>(2);
	const Entity c = static_cast<Entity>(3);
	const Entity d = static_cast<Entity>(4);
	sel.set(a);
	// Shift-click on c: the rows between the anchor a and c
	sel.setMany({ a, b, c });
	CHECK(sel.size() == 3);
	CHECK(sel.anchor() == a);
	CHECK(sel.primary() == c);

	// A second Shift-click on d ranges from the SAME anchor.
	sel.setMany({ a, b, c, d });
	CHECK(sel.size() == 4);
	CHECK(sel.anchor() == a);

	// addMany skips what is there and nulls, and appends the rest.
	sel.set(d);
	sel.addMany({ entt::null, d, c, b });
	REQUIRE(sel.size() == 3);
	CHECK(sel.entities()[0] == d);
	CHECK(sel.entities()[1] == c);
	CHECK(sel.entities()[2] == b);
	CHECK(sel.anchor() == d);
}

TEST_CASE("EditorSelection: prune drops what the world no longer has")
{
	HorizonWorld world;
	const Entity a = world.createEntity("A");
	const Entity b = world.createEntity("B");
	const Entity c = world.createEntity("C");

	EditorSelection sel;
	sel.add(a);
	sel.add(b);
	sel.add(c);
	sel.setAnchor(b);

	world.destroyEntity(b);
	sel.prune(world.registry());
	REQUIRE(sel.size() == 2);
	CHECK(sel.contains(a));
	CHECK(sel.contains(c));
	CHECK_FALSE(sel.contains(b));
	// The anchor pointed at the dead entity; it follows the primary.
	CHECK(sel.anchor() == c);

	// Nothing stale: no change, no revision bump.
	const std::uint64_t rev = sel.revision();
	sel.prune(world.registry());
	CHECK(sel.revision() == rev);

	sel.clear();
	CHECK(sel.empty());
}

TEST_CASE("EditorSelection: roots leaves out a member whose ancestor is selected")
{
	// The group gizmo moves a parent's children through the parent, so a child
	// that is selected alongside its parent (or grandparent) must not be moved
	// a second time on its own.
	HorizonWorld world;
	const Entity parent     = world.createEntity("Parent");
	const Entity child      = world.createEntity("Child");
	const Entity grandchild = world.createEntity("Grandchild");
	const Entity loner      = world.createEntity("Loner");
	const Entity orphan     = world.createEntity("Orphan"); // a child whose parent is NOT selected
	const Entity unselectedParent = world.createEntity("UnselectedParent");
	world.reparentEntity(child, parent);
	world.reparentEntity(grandchild, child);
	world.reparentEntity(orphan, unselectedParent);

	EditorSelection sel;
	sel.add(grandchild);
	sel.add(loner);
	sel.add(parent);
	sel.add(orphan);
	sel.add(child);

	const std::vector<Entity> roots = sel.roots(world.registry());
	REQUIRE(roots.size() == 3);
	// Selection order survives: the primary (last added) stays last.
	CHECK(roots[0] == loner);
	CHECK(roots[1] == parent);
	CHECK(roots[2] == orphan);

	// A destroyed member is skipped rather than reported.
	world.destroyEntity(loner);
	const std::vector<Entity> after = sel.roots(world.registry());
	REQUIRE(after.size() == 2);
	CHECK(after[0] == parent);
	CHECK(after[1] == orphan);
}

TEST_CASE("EditorSelection: roots over a Select All of thousands, chains and a cycle")
{
	// Select All hands the whole scene over and the viewport asks roots() every
	// frame. 1250 chains of four: only the chain heads are roots, in order.
	HorizonWorld world;
	std::vector<Entity> all;
	std::vector<Entity> heads;
	for (int c = 0; c < 1250; ++c)
	{
		Entity parent = world.createEntity("Head");
		heads.push_back(parent);
		all.push_back(parent);
		for (int d = 0; d < 3; ++d)
		{
			const Entity child = world.createEntity("Link");
			world.reparentEntity(child, parent);
			all.push_back(child);
			parent = child;
		}
	}

	// A hand-edited file can carry a parent cycle; the step cap ends the walk.
	// The member below it is a root: nothing selected sits on its chain.
	const Entity cycA   = world.createEntity("CycleA");
	const Entity cycB   = world.createEntity("CycleB");
	const Entity member = world.createEntity("BelowTheCycle");
	world.registry().get<HierarchyComponent>(cycA).parent   = cycB;
	world.registry().get<HierarchyComponent>(cycB).parent   = cycA;
	world.registry().get<HierarchyComponent>(member).parent = cycA;

	EditorSelection sel;
	sel.setMany(all);
	sel.add(member);
	REQUIRE(sel.size() == all.size() + 1);

	const std::vector<Entity> roots = sel.roots(world.registry());
	REQUIRE(roots.size() == heads.size() + 1);
	for (std::size_t i = 0; i < heads.size(); ++i) CHECK(roots[i] == heads[i]);
	CHECK(roots.back() == member);
}

TEST_CASE("EditorSelection: contains agrees with entities after every change")
{
	// contains() answers from a set kept beside the ordered list; every mutator
	// has to keep the two in step.
	HorizonWorld world;
	std::vector<Entity> pool;
	for (int i = 0; i < 8; ++i) pool.push_back(world.createEntity("E"));

	EditorSelection sel;
	const auto agrees = [&] {
		const auto& list = sel.entities();
		for (const Entity e : pool)
		{
			const bool listed = std::find(list.begin(), list.end(), e) != list.end();
			if (sel.contains(e) != listed) return false;
		}
		return !sel.contains(entt::null);
	};

	sel.set(pool[0]);                               CHECK(agrees());
	sel.add(pool[1]); sel.add(pool[2]);             CHECK(agrees());
	sel.add(pool[0]);                               CHECK(agrees()); // moved to the back
	CHECK(sel.primary() == pool[0]);
	sel.remove(pool[1]);                            CHECK(agrees());
	sel.remove(pool[1]);                            CHECK(agrees()); // absent: no-op
	sel.toggle(pool[3]); sel.toggle(pool[2]);       CHECK(agrees());
	sel.addMany({ pool[4], pool[4], entt::null, pool[0], pool[5] });
	CHECK(agrees());
	CHECK(sel.size() == 4);                          // 0, 3, 4, 5
	sel.setMany({ pool[6], pool[7], pool[6] });     CHECK(agrees());
	CHECK(sel.size() == 2);
	world.destroyEntity(pool[6]);
	sel.prune(world.registry());                    CHECK(agrees());
	CHECK_FALSE(sel.contains(pool[6]));
	sel.set(entt::null);                            CHECK(agrees());
	sel.add(pool[1]); sel.clear();                  CHECK(agrees());
	CHECK(sel.empty());
}
