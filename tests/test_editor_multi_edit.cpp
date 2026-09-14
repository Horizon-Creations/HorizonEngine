#include "doctest.h"
#include "EditorMultiEdit.h"
#include "EditorUndo.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <string>

// The rule behind "three selected, drag Position X, all three move": what the
// Details panel works out from the active entity's before/after component
// state, and what it then writes into the other members. The questions are the
// user's: does only the axis I dragged travel, does a member keep its own other
// values, does an entity without the component stay out of it, and does ONE
// undo put all of them back.

using EditorMultiEdit::json;

namespace
{
	Entity findByName(HorizonWorld& w, const std::string& name)
	{
		for (auto [e, n] : w.registry().view<NameComponent>().each())
			if (n.name == name) return e;
		return entt::null;
	}

	Entity makeLit(HorizonWorld& w, const char* name, glm::vec3 pos, float intensity)
	{
		const Entity e = w.createEntity(name);
		TransformComponent t;
		t.position = pos;
		w.addComponent(e, t);
		LightComponent l;
		l.intensity = intensity;
		w.addComponent(e, l);
		return e;
	}
}

TEST_CASE("EditorMultiEdit::diff reports leaves, not whole vectors")
{
	const json before = { { "transform", { { "position", { 1.0, 2.0, 3.0 } },
	                                       { "scale",    { 1.0, 1.0, 1.0 } } } },
	                      { "light",     { { "intensity", 1.0 }, { "visible", true } } } };

	SUBCASE("identical states yield nothing")
	{
		CHECK(EditorMultiEdit::diff(before, before).empty());
	}
	SUBCASE("one axis of a vec3 is one change with that element's path")
	{
		json after = before;
		after["transform"]["position"][0] = 7.0;
		const auto changes = EditorMultiEdit::diff(before, after);
		REQUIRE(changes.size() == 1);
		CHECK(changes[0].component == "transform");
		CHECK(changes[0].path.to_string() == "/position/0");
		CHECK(changes[0].value == json(7.0));
	}
	SUBCASE("changes in two components are two changes")
	{
		json after = before;
		after["transform"]["scale"][2] = 2.0;
		after["light"]["visible"]      = false;
		const auto changes = EditorMultiEdit::diff(before, after);
		REQUIRE(changes.size() == 2);
		CHECK(changes[0].component != changes[1].component);
	}
	SUBCASE("an array that changed length travels whole")
	{
		json b = { { "script", { { "vars", { 1, 2 } } } } };
		json a = { { "script", { { "vars", { 1, 2, 3 } } } } };
		const auto changes = EditorMultiEdit::diff(b, a);
		REQUIRE(changes.size() == 1);
		CHECK(changes[0].path.to_string() == "/vars");
		CHECK(changes[0].value == json({ 1, 2, 3 }));
	}
	SUBCASE("a component that appeared is not a change")
	{
		json after = before;
		after["mesh"] = { { "visible", true } };
		CHECK(EditorMultiEdit::diff(before, after).empty());
	}
	SUBCASE("a component that vanished is not a change either")
	{
		json after = before;
		after.erase("light");
		CHECK(EditorMultiEdit::diff(before, after).empty());
	}
}

TEST_CASE("EditorMultiEdit::state carries components and never the name")
{
	HorizonWorld world;
	const Entity e = makeLit(world, "Lamp", { 1, 2, 3 }, 5.0f);
	const json s = EditorMultiEdit::state(world, e);
	CHECK(s.contains("transform"));
	CHECK(s.contains("light"));
	CHECK_FALSE(s.contains("__name"));
	CHECK(s["transform"]["position"][2] == json(3.0));
	CHECK(EditorMultiEdit::state(world, entt::null).empty());
}

TEST_CASE("EditorMultiEdit::propagate writes the changed leaf and leaves the rest alone")
{
	HorizonWorld world;
	const Entity a = makeLit(world, "A", { 0, 0, 0 }, 1.0f);   // the active one
	const Entity b = makeLit(world, "B", { 0, 5, 0 }, 2.0f);   // its own Y, its own light
	const Entity c = world.createEntity("C");                    // transform only, no light
	{
		TransformComponent t;
		t.position = { 0, 0, 9 };
		world.addComponent(c, t);
	}
	auto& reg = world.registry();
	// As the extractor leaves it after a frame: clean. The write-back has to
	// set it again or the renderer keeps drawing B where it was.
	reg.get<TransformComponent>(b).dirty = false;

	// What the panel does: state before the widgets, the widgets edit the
	// active entity, state after, diff, propagate.
	const json before = EditorMultiEdit::state(world, a);
	reg.get<TransformComponent>(a).position.x = 4.0f;
	reg.get<LightComponent>(a).intensity      = 3.0f;
	const json after = EditorMultiEdit::state(world, a);
	const auto changes = EditorMultiEdit::diff(before, after);
	REQUIRE(changes.size() == 2);

	const int written = EditorMultiEdit::propagate(world, changes, { a, b, c }, a);
	CHECK(written == 2);

	// B: X followed, Y is still B's own, the light followed too.
	const auto& tb = reg.get<TransformComponent>(b);
	CHECK(tb.position.x == doctest::Approx(4.0f));
	CHECK(tb.position.y == doctest::Approx(5.0f));
	CHECK(tb.dirty);
	CHECK(reg.get<LightComponent>(b).intensity == doctest::Approx(3.0f));

	// C: X followed, Z is still C's own, and it did not grow a light.
	const auto& tcmp = reg.get<TransformComponent>(c);
	CHECK(tcmp.position.x == doctest::Approx(4.0f));
	CHECK(tcmp.position.z == doctest::Approx(9.0f));
	CHECK_FALSE(reg.all_of<LightComponent>(c));

	// The primary itself is not touched again (its value is the source).
	CHECK(reg.get<TransformComponent>(a).position.x == doctest::Approx(4.0f));
}

TEST_CASE("EditorMultiEdit::propagate skips vetoed members and does not re-emplace untouched components")
{
	HorizonWorld world;
	const Entity a = makeLit(world, "A", { 0, 0, 0 }, 1.0f);
	const Entity b = makeLit(world, "B", { 0, 0, 0 }, 1.0f);
	const Entity c = makeLit(world, "C", { 0, 0, 0 }, 1.0f);
	auto& reg = world.registry();
	// B's transform carries runtime state the serializer does not write (the
	// computed world matrix, the clean dirty flag). If the transform were
	// re-applied along with the light edit, a fresh component would replace it
	// and both would be gone.
	{
		auto& tb = reg.get<TransformComponent>(b);
		tb.dirty       = false;
		tb.worldMatrix = glm::mat4(2.0f);
	}

	const json before = EditorMultiEdit::state(world, a);
	reg.get<LightComponent>(a).range = 42.0f;
	const json after = EditorMultiEdit::state(world, a);
	const auto changes = EditorMultiEdit::diff(before, after);
	REQUIRE(changes.size() == 1);

	const int written = EditorMultiEdit::propagate(world, changes, { a, b, c }, a, /*skip=*/{ c });
	CHECK(written == 1);
	CHECK(reg.get<LightComponent>(b).range == doctest::Approx(42.0f));
	CHECK(reg.get<LightComponent>(c).range == doctest::Approx(10.0f)); // vetoed: untouched
	CHECK_FALSE(reg.get<TransformComponent>(b).dirty);                    // transform untouched…
	CHECK(reg.get<TransformComponent>(b).worldMatrix[0][0] == doctest::Approx(2.0f)); // …not re-emplaced
}

TEST_CASE("EditorMultiEdit: one undo entry covers the active entity and every member")
{
	HorizonWorld world;
	const Entity a = makeLit(world, "A", { 1, 0, 0 }, 1.0f);
	const Entity b = makeLit(world, "B", { 2, 0, 0 }, 1.0f);
	const Entity c = makeLit(world, "C", { 3, 0, 0 }, 1.0f);
	auto& reg = world.registry();

	EditorUndo undo;
	undo.setWorld(&world);

	// The Details panel's edit session: the pre-state is captured on the
	// mouse press, stashed when the widget activates, the widget edits the
	// active entity, the panel propagates in the same frame, and the entry is
	// committed when the widget deactivates. One entry, whatever the count.
	undo.capturePre();
	undo.stashPre();
	const json before = EditorMultiEdit::state(world, a);
	reg.get<TransformComponent>(a).position.y = 8.0f;
	const json after = EditorMultiEdit::state(world, a);
	EditorMultiEdit::propagate(world, EditorMultiEdit::diff(before, after), { a, b, c }, a);
	undo.commitPending();

	REQUIRE(reg.get<TransformComponent>(b).position.y == doctest::Approx(8.0f));
	REQUIRE(reg.get<TransformComponent>(c).position.y == doctest::Approx(8.0f));

	REQUIRE(undo.undo());
	CHECK_FALSE(undo.canUndo()); // exactly one entry was on the stack
	for (const char* name : { "A", "B", "C" })
	{
		const Entity e = findByName(world, name);
		REQUIRE((e != entt::null));
		CHECK(reg.get<TransformComponent>(e).position.y == doctest::Approx(0.0f));
	}
	// And the X values — one per entity — came back as their own, not as one.
	CHECK(reg.get<TransformComponent>(findByName(world, "B")).position.x == doctest::Approx(2.0f));
	CHECK(reg.get<TransformComponent>(findByName(world, "C")).position.x == doctest::Approx(3.0f));
}
