#pragma once
#include <entt/entt.hpp>
#include <HorizonScene/Components/HierarchyComponent.h>

#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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

// ── Re-parenting, detected the same way ──────────────────────────────────────
// The outliner's drag, Group, Create Child, the MCP Reparent tool and a session
// undo all move entities in the hierarchy, and none of them published it: the
// wire message existed (CollabController::publishReparent) with no caller. Same
// cure as for creates — not a hook per call site, but a diff: remember the
// parent every known entity had when we last looked, and report the ones whose
// parent is a different entity now.
using StructParentMap = std::unordered_map<StructEntity, StructEntity>;

struct StructReparent
{
	StructEntity entity = entt::null;
	StructEntity from   = entt::null;   // as recorded
	StructEntity to     = entt::null;   // as it is now
};

// Record the parent of every known entity that has no record yet. A record is
// made, never reported: a fresh entity went out inside its create (which
// carries its parent), and one that arrived from a peer is where they put it.
inline void recordMissingParents(const entt::registry& reg, const StructEntitySet& known,
                                 StructParentMap& parents)
{
	for (const StructEntity e : known)
		if (reg.valid(e) && !parents.count(e)) parents.emplace(e, structParentOf(reg, e));
}

// Known entities whose parent is no longer the recorded one. Pure: the caller
// decides per entry whether it went out, and only then updates the record —
// a reparent that could not be sent yet must come up again next pass.
inline std::vector<StructReparent> reparentedSince(const entt::registry& reg,
                                                   const StructEntitySet& known,
                                                   const StructParentMap& parents)
{
	std::vector<StructReparent> moved;
	for (const StructEntity e : known)
	{
		if (!reg.valid(e)) continue;
		const auto it = parents.find(e);
		if (it == parents.end()) continue;
		const StructEntity now = structParentOf(reg, e);
		if (now != it->second) moved.push_back({ e, it->second, now });
	}
	return moved;
}

// ── A new subtree that adopted entities the peers already have ───────────────
// Group makes a fresh "Group" entity and re-parents the selected roots under it
// in the same frame. The create for the group carries its whole subtree, so the
// adopted entities would travel inside it, and a receiver that instantiates with
// preserved uuids would build a SECOND copy of each of them next to the
// original. They are not new — they moved. While the create is serialised they
// are taken out of their new parent's child list (and put back exactly as they
// were afterwards), so the blob holds only what is really new; the re-parent
// pass then moves the originals under the group on every peer.
class DetachKnownDescendants
{
public:
	DetachKnownDescendants(entt::registry& reg, StructEntity root, const StructEntitySet& known)
		: m_reg(reg)
	{
		walk(root, known);
	}
	~DetachKnownDescendants()
	{
		// Reverse order: a parent emptied twice is restored to its first copy.
		for (auto it = m_saved.rbegin(); it != m_saved.rend(); ++it)
			if (auto* h = m_reg.try_get<HierarchyComponent>(it->first))
				h->children = std::move(it->second);
	}
	DetachKnownDescendants(const DetachKnownDescendants&)            = delete;
	DetachKnownDescendants& operator=(const DetachKnownDescendants&) = delete;

	// The adopted entities that were taken out (tops only: what is under one of
	// them is known too, and went with it).
	const std::vector<StructEntity>& detached() const { return m_detached; }

private:
	void walk(StructEntity e, const StructEntitySet& known)
	{
		auto* h = m_reg.try_get<HierarchyComponent>(e);
		if (!h) return;
		std::vector<StructEntity> keep;
		bool any = false;
		for (const StructEntity c : h->children)
		{
			if (known.count(c)) { m_detached.push_back(c); any = true; }
			else                keep.push_back(c);
		}
		if (any)
		{
			m_saved.emplace_back(e, h->children);
			h->children = keep;
		}
		for (const StructEntity c : keep) walk(c, known);
	}

	entt::registry& m_reg;
	std::vector<std::pair<StructEntity, std::vector<StructEntity>>> m_saved;
	std::vector<StructEntity> m_detached;
};

} // namespace HE::Ed
