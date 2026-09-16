#include "ViewportActions.h"
#include "EditorSelection.h"

#include <HorizonScene/EntityVisibility.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_set>

namespace ViewportActions
{
namespace
{
	// The whole scene, top-down from the world root — the same walk the
	// Outliner lists, so "everything" here is everything a user can see there.
	// The root itself is skipped: it draws nothing and is a built-in anyway.
	void forEachInScene(HorizonWorld& world, const std::function<void(Entity)>& fn)
	{
		auto& reg = world.registry();
		std::function<void(Entity)> walk = [&](Entity e)
		{
			if (!reg.valid(e)) return;
			if (e != world.rootEntity()) fn(e);
			if (const auto* h = reg.try_get<HierarchyComponent>(e))
				for (const Entity c : h->children) walk(c);
		};
		walk(world.rootEntity());
	}

	// Every entity in the subtree under (and including) `e`.
	void collectSubtree(const entt::registry& reg, Entity e, std::vector<Entity>& out)
	{
		if (!reg.valid(e)) return;
		out.push_back(e);
		if (const auto* h = reg.try_get<HierarchyComponent>(e))
			for (const Entity c : h->children) collectSubtree(reg, c, out);
	}

	Entity parentOf(const entt::registry& reg, Entity e)
	{
		const auto* h = reg.try_get<HierarchyComponent>(e);
		return h ? h->parent : entt::null;
	}

	// Rewrite `e`'s local transform so that it keeps standing at `childWorld`
	// now that its parent is `parent`. Position is exact; rotation and scale
	// come out of a decompose, which is exact for the rigid + uniform cases and
	// the best a TRS component can do for the rest. Done by the caller AFTER the
	// reparent, with the world matrix it measured BEFORE — the parent chain has
	// changed in between and worldMatrixOf(e) would already answer with the
	// jump this is here to prevent.
	void keepWorldPose(HorizonWorld& world, Entity e, Entity parent, const glm::mat4& childWorld)
	{
		auto& reg = world.registry();
		auto* tc = reg.try_get<TransformComponent>(e);
		if (!tc) return;
		const glm::mat4 parentWorld = (parent == entt::null) ? glm::mat4(1.0f)
		                                                     : HE::worldMatrixOf(world, parent);
		const glm::mat4 local = glm::inverse(parentWorld) * childWorld;

		tc->position = glm::vec3(local[3]);
		glm::vec3 axis[3] = { glm::vec3(local[0]), glm::vec3(local[1]), glm::vec3(local[2]) };
		glm::vec3 scale(glm::length(axis[0]), glm::length(axis[1]), glm::length(axis[2]));
		for (int i = 0; i < 3; ++i) axis[i] /= std::max(scale[i], 1.0e-6f);
		// A mirror shows only in the determinant (three positive column
		// lengths either way); the sign goes on X, as PhysicsWorld does it.
		if (glm::determinant(glm::mat3(local)) < 0.0f) { scale.x = -scale.x; axis[0] = -axis[0]; }
		tc->scale    = scale;
		tc->rotation = glm::degrees(glm::eulerAngles(
			glm::normalize(glm::quat_cast(glm::mat3(axis[0], axis[1], axis[2])))));
		tc->dirty = true;
	}
} // namespace

std::vector<Entity> hideSelected(HorizonWorld& world, const EditorSelection& selection)
{
	auto& reg = world.registry();
	std::vector<Entity> touched;
	for (const Entity root : selection.roots(reg))
	{
		if (root == world.rootEntity()) continue;
		HE::setSubtreeVisible(reg, root, false);
		collectSubtree(reg, root, touched);
	}
	return touched;
}

std::vector<Entity> isolateSelected(HorizonWorld& world, const EditorSelection& selection)
{
	auto& reg = world.registry();
	std::unordered_set<Entity> keep;
	{
		std::vector<Entity> subtree;
		for (const Entity root : selection.roots(reg))
			collectSubtree(reg, root, subtree);
		keep.insert(subtree.begin(), subtree.end());
	}
	std::vector<Entity> hidden;
	forEachInScene(world, [&](Entity e)
	{
		if (keep.count(e) || world.isBuiltin(e)) return;
		if (HE::entityVisibility(reg, e) != HE::Visibility::Visible) return;
		HE::setEntityVisible(reg, e, false);
		hidden.push_back(e);
	});
	return hidden;
}

std::vector<Entity> showAll(HorizonWorld& world)
{
	auto& reg = world.registry();
	std::vector<Entity> shown;
	forEachInScene(world, [&](Entity e)
	{
		// Hidden means "has renderables and all are off" — an entity with one
		// of two lights off is Visible and is left as the author set it.
		if (HE::entityVisibility(reg, e) != HE::Visibility::Hidden) return;
		HE::setEntityVisible(reg, e, true);
		shown.push_back(e);
	});
	return shown;
}

bool anyHidden(HorizonWorld& world)
{
	auto& reg = world.registry();
	bool any = false;
	forEachInScene(world, [&](Entity e)
	{
		if (!any && HE::entityVisibility(reg, e) == HE::Visibility::Hidden) any = true;
	});
	return any;
}

Entity groupSelected(HorizonWorld& world, EditorSelection& selection)
{
	auto& reg = world.registry();
	std::vector<Entity> roots = selection.roots(reg);
	roots.erase(std::remove_if(roots.begin(), roots.end(),
	                           [&](Entity e) { return world.isBuiltin(e); }),
	            roots.end());
	if (roots.empty()) return entt::null;

	// Where the group goes: the parent every root shares, else the world root.
	Entity parent = parentOf(reg, roots.front());
	for (const Entity r : roots)
		if (parentOf(reg, r) != parent) { parent = world.rootEntity(); break; }
	if (parent == entt::null) parent = world.rootEntity();

	// Where it stands: the centre of the roots, measured in WORLD space through
	// the parent chain (TransformComponent::position is local, and the stored
	// worldMatrix is a frame old — see TransformHierarchy.h).
	std::vector<glm::mat4> worlds;
	glm::vec3 centre(0.0f);
	for (const Entity r : roots)
	{
		worlds.push_back(HE::worldMatrixOf(world, r));
		centre += glm::vec3(worlds.back()[3]);
	}
	centre /= static_cast<float>(roots.size());

	const Entity group = world.createEntity("Group");
	world.addComponent(group, TransformComponent{});
	if (parent != world.rootEntity())
		world.reparentEntity(group, parent);
	{
		auto& gt = reg.get<TransformComponent>(group);
		gt.position = HE::localPositionForWorld(world, group, centre);
		gt.dirty    = true;
	}

	// The reparent keeps each root's LOCAL transform, which was relative to a
	// different parent a moment ago: put it back where it was on screen.
	for (std::size_t i = 0; i < roots.size(); ++i)
	{
		if (!world.reparentEntity(roots[i], group)) continue;
		keepWorldPose(world, roots[i], group, worlds[i]);
	}
	world.markHierarchyDirty();
	selection.set(group);
	return group;
}

bool canUngroup(HorizonWorld& world, const EditorSelection& selection)
{
	auto& reg = world.registry();
	for (const Entity e : selection.entities())
	{
		if (!reg.valid(e) || world.isBuiltin(e)) continue;
		if (const auto* h = reg.try_get<HierarchyComponent>(e); h && !h->children.empty())
			return true;
	}
	return false;
}

std::vector<Entity> ungroupSelected(HorizonWorld& world, EditorSelection& selection)
{
	auto& reg = world.registry();
	std::vector<Entity> freed;
	// Roots only: ungrouping a parent and a child of it in one go would have
	// the child's move undone by the parent's destroy.
	for (const Entity g : selection.roots(reg))
	{
		if (!reg.valid(g) || world.isBuiltin(g)) continue;
		const auto* h = reg.try_get<HierarchyComponent>(g);
		if (!h || h->children.empty()) continue;
		Entity parent = h->parent;
		if (parent == entt::null) parent = world.rootEntity();

		// Copy: reparentEntity edits the list being walked.
		const std::vector<Entity> children = h->children;
		for (const Entity c : children)
		{
			const glm::mat4 before = HE::worldMatrixOf(world, c);
			if (!world.reparentEntity(c, parent)) continue;
			keepWorldPose(world, c, parent, before);
			freed.push_back(c);
		}
		world.destroyEntity(g);
	}
	world.markHierarchyDirty();
	selection.prune(reg);
	if (!freed.empty()) selection.setMany(freed);
	return freed;
}

std::vector<Entity> snapToGround(HorizonWorld& world, const EditorSelection& selection,
                                 const SurfaceProbe& probe, const SubtreeBounds& bounds)
{
	std::vector<Entity> moved;
	if (!probe) return moved;
	auto& reg = world.registry();
	// Roots only: a child whose parent is selected too moves through the
	// parent, and dropping both would land the child on the floor twice.
	for (const Entity root : selection.roots(reg))
	{
		if (!reg.valid(root) || root == world.rootEntity() || world.isBuiltin(root)) continue;
		auto* t = reg.try_get<TransformComponent>(root);
		if (!t) continue;

		// Composed on the spot, not read off worldMatrix: a root the user just
		// dragged has not been propagated yet this frame.
		const glm::vec3 pivot = HE::worldPositionOf(world, root);

		HE::AABB box;
		if (!bounds || !bounds(root, box) || !box.isValid())
		{
			box = HE::AABB{};
			box.expand(pivot);
		}
		// The box may not include the pivot (a mesh offset from its origin);
		// the object's "bottom" is the lower of the two either way.
		const float bottom = std::min(box.min.y, pivot.y);
		const float top    = std::max(box.max.y, pivot.y);

		std::vector<Entity> subtree;
		collectSubtree(reg, root, subtree);
		std::unordered_set<uint32_t> exclude;
		exclude.reserve(subtree.size());
		for (const Entity e : subtree) exclude.insert(static_cast<uint32_t>(e));

		// From just above the top, straight down, through the object itself.
		const glm::vec3 origin(pivot.x, top + 0.01f, pivot.z);
		glm::vec3 hitPoint;
		if (!probe(origin, glm::vec3(0.0f, -1.0f, 0.0f), exclude, hitPoint)) continue;

		const float newY = hitPoint.y + (pivot.y - bottom);
		if (std::abs(newY - pivot.y) < 1e-5f) continue;   // already standing on it

		const glm::vec3 target(pivot.x, newY, pivot.z);
		t->position = HE::localPositionForWorld(world, root, target);
		t->dirty    = true;
		moved.push_back(root);
	}
	if (!moved.empty()) world.markHierarchyDirty();
	return moved;
}

} // namespace ViewportActions
