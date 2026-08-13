#pragma once
#include <entt/entt.hpp>
#include <HorizonScene/Components/HierarchyComponent.h>

#include <functional>
#include <unordered_set>
#include <vector>

// ── Which new entities are worth a create message, and what one covers ───────
// Pulled out of EditorApplication::syncStructuralChanges so the rule can be
// tested: the editor application itself is not in the test binary, and this is
// the half that got it wrong.
//
// The rule it encodes: a create carries the SUBTREE below the entity it names,
// so a new entity whose parent is also new in the same pass is already inside
// that parent's message and must not get one of its own. The loop this replaced
// published one per new entity, so dropping a three-entity prefab sent the root
// (all three) and then each child again — and a receiver that instantiates with
// preserved uuids duplicated every child. Recording only the published root was
// the same bug by a slower route: the descendants looked new on the next frame
// and went out again.
//
// Deliberately independent of iteration order. The caller's "everything that
// exists" is an unordered set, so a child is routinely reached before its
// parent, and any rule that depended on seeing parents first would work in
// testing and fail on a rehash.

namespace HE::Ed
{

using StructEntity   = entt::entity;
using StructEntitySet = std::unordered_set<StructEntity>;
using LocalOnlyFn    = std::function<bool(StructEntity)>;

// The parent of `e`, or entt::null when it has none the registry still knows.
inline StructEntity structParentOf(const entt::registry& reg, StructEntity e)
{
	const auto* h = reg.try_get<HierarchyComponent>(e);
	return (h && reg.valid(h->parent)) ? h->parent : entt::null;
}

// The tops of the newly created subtrees: in `current`, not in `known`, and
// without a parent that is itself in that state.
inline std::vector<StructEntity> newSubtreeRoots(const entt::registry& reg,
                                                 const StructEntitySet& current,
                                                 const StructEntitySet& known)
{
	const auto isNew = [&](StructEntity e) {
		return e != entt::null && current.count(e) && !known.count(e);
	};
	std::vector<StructEntity> roots;
	for (const StructEntity e : current)
		if (!known.count(e) && !isNew(structParentOf(reg, e))) roots.push_back(e);
	return roots;
}

// Record `e` and everything under it. Entities the predicate calls local-only
// are skipped rather than recorded: the caller compares this set against a
// "everything that exists" set that never contains them, so an entry for one
// would read as destroyed on the next pass and publish a destroy for an entity
// no peer ever had.
inline void markSubtreeKnown(const entt::registry& reg, StructEntity e,
                             const LocalOnlyFn& isLocalOnly, StructEntitySet& known)
{
	if (e == entt::null || !reg.valid(e)) return;
	if (isLocalOnly && isLocalOnly(e)) return;
	if (!known.insert(e).second) return;   // already walked — also the cycle guard
	if (const auto* h = reg.try_get<HierarchyComponent>(e))
		for (const StructEntity c : h->children)
			markSubtreeKnown(reg, c, isLocalOnly, known);
}

} // namespace HE::Ed
