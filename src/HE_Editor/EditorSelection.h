#pragma once
#include <HorizonScene/HorizonWorld.h> // Entity
#include <cstddef>
#include <cstdint>
#include <vector>

// The editor's selection: an ORDERED set of entities, not a single handle.
//
// The outliner, the viewport pick, the inspector and every "act on the
// selection" gesture (delete, duplicate, focus, gizmo, collab lock) used to
// share one `Entity m_selectedEntity`. This is the same thing as a set:
//
//   - `entities()` is the whole selection in the order it was built, without
//     duplicates. A Ctrl-click appends, a Shift-range replaces the set with the
//     rows between anchor and click (Explorer-style); nothing here sorts, so
//     "the order the user clicked in" survives.
//   - `primary()` is the entity a single-entity consumer works with: the LAST
//     one added — the row just clicked, the object just picked, the copy just
//     made. Every place that could only ever handle one entity (the transform
//     gizmo, the collab lock, the Focus key) reads this and keeps behaving the
//     way it did with a single handle. `entt::null` when nothing is selected.
//   - `anchor()` is where a Shift-click range starts: set by a plain click or a
//     Ctrl-click, left alone by a Shift-click, so click A, Shift-click B,
//     Shift-click C selects A..C and not B..C.
//
// Deliberately ImGui-free and world-free: it stores handles and nothing else,
// so the unit test can drive it without an editor. Handles go stale when the
// world is replaced (undo, scene open, play stop) — those paths `clear()`, and
// the per-frame `prune()` drops anything a peer or a script destroyed in
// between, so panels can trust every member to be valid.
class EditorSelection
{
public:
	// Replace the whole selection with one entity. `entt::null` clears, which
	// is what the old `m_selectedEntity = entt::null` meant.
	void set(Entity e);
	// Add without disturbing what is there; a member already present moves to
	// the back so it becomes the primary. Null is ignored.
	void add(Entity e);
	// Drop one member (the anchor with it, if it was the anchor). No-op when
	// absent.
	void remove(Entity e);
	// Ctrl-click: in → out, out → in (and primary).
	void toggle(Entity e);
	void clear();

	// Replace the selection with `range`, in the given order, skipping
	// duplicates and nulls. The anchor is left where it is — this is what a
	// Shift-click range does with the rows between the anchor and the click.
	void setMany(const std::vector<Entity>& range);
	// Shift-click: keep everything selected so far and append `range`.
	void addMany(const std::vector<Entity>& range);

	// Remove every member the registry no longer knows. Cheap for a handful of
	// entities and the only thing that keeps a peer's delete from leaving a
	// dead handle in the inspector's hands.
	void prune(const entt::registry& registry);

	// The members no OTHER member is an ancestor of, in selection order. This
	// is what a gesture over "the whole selection" has to act on when the
	// action propagates down the hierarchy anyway: the group gizmo moves a
	// parent's children through the parent, so a child that is also selected
	// must not be moved a second time on its own. Members the registry no
	// longer knows are skipped, not reported.
	std::vector<Entity> roots(const entt::registry& registry) const;

	bool        contains(Entity e) const;
	bool        empty() const { return m_entities.empty(); }
	std::size_t size()  const { return m_entities.size(); }
	Entity      primary() const { return m_entities.empty() ? entt::null : m_entities.back(); }
	Entity      anchor()  const { return m_anchor; }
	void        setAnchor(Entity e) { m_anchor = e; }
	const std::vector<Entity>& entities() const { return m_entities; }

	// Bumped on every change. A panel that caches something per selection
	// (the inspector's common-component list) compares this instead of the
	// whole vector.
	std::uint64_t revision() const { return m_revision; }

private:
	std::vector<Entity> m_entities;
	Entity              m_anchor   = entt::null;
	std::uint64_t       m_revision = 0;
};
