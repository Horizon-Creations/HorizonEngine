#include "doctest.h"

#include "ViewportActions.h"
#include "EditorSelection.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/EntityVisibility.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <HorizonScene/Components/EnvironmentLightComponent.h>
#include <glm/glm.hpp>
#include <cmath>

// ── The viewport's right-click verbs, without the viewport ───────────────────
// Hide, Isolate, Show All, Group and Ungroup are the scene edits behind the
// context menu. Each has a rule that only shows up later if it is wrong: an
// isolate that took the sun with it (a black scene), a group whose members
// jumped (reparentEntity keeps LOCAL transforms), an ungroup that left the
// group behind. This file states those rules.

namespace
{
Entity meshAt(HorizonWorld& world, const char* name, glm::vec3 pos, Entity parent = entt::null)
{
	const Entity e = world.createEntity(name);
	TransformComponent tc;
	tc.position = pos;
	world.addComponent(e, tc);
	world.addComponent(e, MeshComponent{});
	if (parent != entt::null) world.reparentEntity(e, parent);
	return e;
}

bool close3(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f)
{
	return std::abs(a.x - b.x) < eps && std::abs(a.y - b.y) < eps && std::abs(a.z - b.z) < eps;
}

Entity parentOf(HorizonWorld& world, Entity e)
{
	return world.registry().get<HierarchyComponent>(e).parent;
}
} // namespace

TEST_CASE("ViewportActions: Hide takes the subtree, Show All brings everything back")
{
	HorizonWorld world;
	auto& reg = world.registry();
	const Entity a     = meshAt(world, "A", { 0, 0, 0 });
	const Entity aKid  = meshAt(world, "A.kid", { 1, 0, 0 }, a);
	const Entity b     = meshAt(world, "B", { 5, 0, 0 });

	EditorSelection sel;
	sel.set(a);
	const auto touched = ViewportActions::hideSelected(world, sel);

	CHECK(touched.size() == 2);   // A and its child, not B
	CHECK(HE::entityVisibility(reg, a)    == HE::Visibility::Hidden);
	CHECK(HE::entityVisibility(reg, aKid) == HE::Visibility::Hidden);
	CHECK(HE::entityVisibility(reg, b)    == HE::Visibility::Visible);
	CHECK(ViewportActions::anyHidden(world));

	const auto shown = ViewportActions::showAll(world);
	CHECK(shown.size() == 2);
	CHECK(HE::entityVisibility(reg, a)    == HE::Visibility::Visible);
	CHECK(HE::entityVisibility(reg, aKid) == HE::Visibility::Visible);
	CHECK_FALSE(ViewportActions::anyHidden(world));
}

TEST_CASE("ViewportActions: Isolate hides the rest, keeps the selection's subtree and the sun")
{
	HorizonWorld world;
	auto& reg = world.registry();
	world.addSky();   // brings the built-in sun + moon lights
	const Entity a    = meshAt(world, "A", { 0, 0, 0 });
	const Entity aKid = meshAt(world, "A.kid", { 1, 0, 0 }, a);
	const Entity b    = meshAt(world, "B", { 5, 0, 0 });
	const Entity c    = meshAt(world, "C", { 9, 0, 0 });

	EditorSelection sel;
	sel.set(a);
	const auto hidden = ViewportActions::isolateSelected(world, sel);

	CHECK(hidden.size() == 2);   // B and C
	CHECK(HE::entityVisibility(reg, a)    == HE::Visibility::Visible);
	CHECK(HE::entityVisibility(reg, aKid) == HE::Visibility::Visible);
	CHECK(HE::entityVisibility(reg, b)    == HE::Visibility::Hidden);
	CHECK(HE::entityVisibility(reg, c)    == HE::Visibility::Hidden);

	// The built-in lights are never touched: a hidden sun is a black scene.
	int builtinLights = 0;
	for (auto [e, light, env] : reg.view<LightComponent, EnvironmentLightComponent>().each())
	{
		++builtinLights;
		CHECK(world.isBuiltin(e));
		CHECK(light.visible);
	}
	CHECK(builtinLights == 2);

	// Show All is the exact inverse.
	const auto shown = ViewportActions::showAll(world);
	CHECK(shown.size() == 2);
	CHECK(HE::entityVisibility(reg, b) == HE::Visibility::Visible);
	CHECK(HE::entityVisibility(reg, c) == HE::Visibility::Visible);
}

TEST_CASE("ViewportActions: Group puts the roots under one parent at their centre, nothing moves")
{
	HorizonWorld world;
	auto& reg = world.registry();
	const Entity a = meshAt(world, "A", { 0, 0, 0 });
	const Entity b = meshAt(world, "B", { 4, 2, 0 });
	// A child of A, selected too: it is not a root and must stay under A.
	const Entity aKid = meshAt(world, "A.kid", { 1, 0, 0 }, a);

	EditorSelection sel;
	sel.setMany({ a, b, aKid });
	const Entity group = ViewportActions::groupSelected(world, sel);

	REQUIRE((group != entt::null));
	CHECK(reg.get<NameComponent>(group).name == "Group");
	CHECK(parentOf(world, group) == world.rootEntity());
	CHECK(parentOf(world, a) == group);
	CHECK(parentOf(world, b) == group);
	CHECK(parentOf(world, aKid) == a);
	// The group stands at the centre of the roots…
	CHECK(close3(HE::worldPositionOf(world, group), { 2, 1, 0 }));
	// …and the roots stand exactly where they stood.
	CHECK(close3(HE::worldPositionOf(world, a),    { 0, 0, 0 }));
	CHECK(close3(HE::worldPositionOf(world, b),    { 4, 2, 0 }));
	CHECK(close3(HE::worldPositionOf(world, aKid), { 1, 0, 0 }));
	CHECK(close3(reg.get<TransformComponent>(a).position, { -2, -1, 0 }));
	// The group is what is selected afterwards.
	CHECK(sel.entities().size() == 1);
	CHECK(sel.primary() == group);
}

TEST_CASE("ViewportActions: Group under a rotated parent keeps the world pose")
{
	HorizonWorld world;
	auto& reg = world.registry();
	const Entity rig = world.createEntity("Rig");
	{
		TransformComponent tc;
		tc.position = { 10, 0, 0 };
		tc.rotation = { 0, 90, 0 };
		tc.scale    = { 2, 2, 2 };
		world.addComponent(rig, tc);
	}
	const Entity a = meshAt(world, "A", { 1, 0, 0 }, rig);
	const Entity b = meshAt(world, "B", { 3, 0, 0 }, rig);
	const glm::vec3 aBefore = HE::worldPositionOf(world, a);
	const glm::vec3 bBefore = HE::worldPositionOf(world, b);

	EditorSelection sel;
	sel.setMany({ a, b });
	const Entity group = ViewportActions::groupSelected(world, sel);

	REQUIRE((group != entt::null));
	// Shared parent → the group goes under it, not under the world root.
	CHECK(parentOf(world, group) == rig);
	CHECK(close3(HE::worldPositionOf(world, a), aBefore, 1e-3f));
	CHECK(close3(HE::worldPositionOf(world, b), bBefore, 1e-3f));
	// And the whole world matrix, not just the translation.
	const glm::mat4 aWorld = HE::worldMatrixOf(world, a);
	CHECK(close3(glm::vec3(aWorld[0]), glm::vec3(0, 0, -2), 1e-3f));  // rig's 90° yaw × scale 2
}

TEST_CASE("ViewportActions: Group skips the built-ins and refuses an empty selection")
{
	HorizonWorld world;
	world.addSky();
	EditorSelection sel;
	CHECK((ViewportActions::groupSelected(world, sel) == entt::null));
	// Only the sun selected: nothing to group.
	for (auto [e, env] : world.registry().view<EnvironmentLightComponent>().each())
	{
		sel.set(e);
		break;
	}
	CHECK((ViewportActions::groupSelected(world, sel) == entt::null));
}

TEST_CASE("ViewportActions: Ungroup frees the children where they stand and drops the parent")
{
	HorizonWorld world;
	auto& reg = world.registry();
	const Entity a = meshAt(world, "A", { 0, 0, 0 });
	const Entity b = meshAt(world, "B", { 4, 2, 0 });
	EditorSelection sel;
	sel.setMany({ a, b });
	const Entity group = ViewportActions::groupSelected(world, sel);
	REQUIRE((group != entt::null));
	CHECK(ViewportActions::canUngroup(world, sel));

	const auto freed = ViewportActions::ungroupSelected(world, sel);
	CHECK(freed.size() == 2);
	CHECK_FALSE(reg.valid(group));
	CHECK(parentOf(world, a) == world.rootEntity());
	CHECK(parentOf(world, b) == world.rootEntity());
	CHECK(close3(HE::worldPositionOf(world, a), { 0, 0, 0 }));
	CHECK(close3(HE::worldPositionOf(world, b), { 4, 2, 0 }));
	CHECK(sel.entities().size() == 2);
	CHECK(sel.contains(a));
	CHECK(sel.contains(b));
	CHECK_FALSE(ViewportActions::canUngroup(world, sel));   // leaves have no children
}
