#pragma once
#include <Types/UUID.h>
#include <entt/entt.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace HE::api { struct Ctx; }

namespace HE
{

// ── What a cell was like when it went (Thema 164, step 3b; plan 6.3) ─────────
// A cell is destroyed on an unload and built again from its file on the next
// load. For a static thing that is all there is to it; for a thing that changed
// while the cell was there it is not: the crate a script destroyed comes back,
// the door somebody opened is shut again. CellState is what the runtime keeps of
// an unloaded cell, for the session (in memory, not in a savegame):
//
//   tombstones  entities the load brought in that were no longer under the cell's
//               root when it went — destroyed by anything (a script's
//               entity.destroy, the physics reap, a graph), or taken out of the
//               cell. Neither is built again: a destroyed crate stays destroyed,
//               and one a script hung under the base would otherwise come a second
//               time, as a copy with a fresh id.
//   states      for every entity with an enabled SaveStateComponent, what
//               entity.saveState would write (EngineApi.h, stateOf): transform,
//               visibility, the Save Game variables of its HorizonCode class.
//
// The savegame: an entity the session has not seen go yet takes its state from
// the ACTIVE save instead (in play mode, with an active save, and only if the save
// holds an entry for it) — what entity.applySavedState would have given it at the
// load of the game, had its cell been there. Once the session kept a state of its
// own, that one wins: it is newer. CellState does not write into the save; an
// entity whose cell is out when the game saves keeps the entry it had (the save's
// entity.saveState cannot reach it either).
//
// Keyed by the entity's UUID, so it only works for a cell whose ids are the same
// on every load (a version-2 cell, CellStreamer::keepsIds); a cell that mints
// fresh ids has nothing to find its entities by and keeps nothing (logged once).
//
// A tombstone is not destroyed after it was built but never built:
// CellStreamer::Hooks::skip (isTombstoned) takes its record out of the slice. A
// thing taken out of the cell is still in the world under its id, so building it
// again would not even give it that id (the load would mint a fresh one) and
// nothing after the build could tell it from any other.
//
// The world half of a state (transform, visibility) goes on as each slice is
// built, before the slice's bodies are made, so a moved thing is built where it
// was and its body with it. The script half goes on once the whole cell is built,
// after the host's cellLoaded, because a variable needs its class instance.
class CellState
{
public:
	// A slice of a cell was built. For a cell with stable ids: the world half of the
	// stored states applied, and what the slice created remembered as this load's.
	// Without: nothing.
	void sliceBuilt(api::Ctx& ctx, entt::entity root, const std::vector<entt::entity>& created, bool stableIds);
	// The whole cell is built and the host has had it: the script half.
	void cellBuilt(api::Ctx& ctx, entt::entity root);
	// The cell is about to be destroyed, built in full or in part: what this load
	// brought in and is still under the root writes its state; what is not, becomes
	// a tombstone. What the load never got to (the slices of a cell abandoned half
	// built) keeps what an earlier unload left.
	void cellUnloading(api::Ctx& ctx, entt::entity root);
	// Forgets what this load brought in without writing anything: for a cell that
	// goes with the whole session (Play stops, the runtime is cleared).
	void forgetCell(entt::entity root);
	// Everything, for a new session.
	void reset();

	size_t             tombstones() const { return m_tombstones.size(); }
	size_t             states() const { return m_states.size(); }
	bool               isTombstoned(const HE::UUID& id) const { return m_tombstones.count(id) != 0; }
	// The stored state (JSON text, the saveState form), or null.
	const std::string* stateOf(const HE::UUID& id) const;

private:
	// What to restore an entity from: this session's state, else the active save's.
	std::string restoreText(const HE::UUID& id) const;

	std::unordered_map<uint32_t, std::vector<HE::UUID>> m_built;   // live cell root → ids its load brought in
	std::unordered_set<HE::UUID>                        m_tombstones;
	std::unordered_map<HE::UUID, std::string>           m_states;
	bool                                                m_warnedFreshIds = false;
};

} // namespace HE
