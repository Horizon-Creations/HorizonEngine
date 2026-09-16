#pragma once
#include <entt/entt.hpp>

// ── "Is this entity switched on?" ────────────────────────────────────────────
// The one reading of InactiveComponent (see its header for what off means).
// Two questions, because the systems need one and the Details panel the other:
//
//  * isEntityActiveSelf — the entity's own box. What the Details panel shows
//    and toggles; a child under an inactive parent still has its own box
//    ticked, the way the Outliner's eye shows the entity's own flag.
//  * isEntityActive — the box AND every ancestor's. What the extractor, the
//    script starters, the physics build and the audio start ask, because a
//    switched-off room takes its furniture with it.
//
// anyEntityInactive is the fast path for the loops that run every frame over
// thousands of entities: when no entity anywhere carries the tag (the common
// case — most scenes never switch anything off), no walk up the hierarchy is
// needed for any of them. It reads the pool HorizonWorld reserves up front, so
// asking never creates one.
namespace HE
{
	bool isEntityActiveSelf(const entt::registry& reg, entt::entity e);
	bool isEntityActive(const entt::registry& reg, entt::entity e);
	bool anyEntityInactive(const entt::registry& reg);

	// Tick or untick the entity's own box. No undo, no hierarchy: the caller
	// (Details panel, script API) decides what one toggle means.
	void setEntityActive(entt::registry& reg, entt::entity e, bool active);

	// The per-frame shape of the question, for a loop over a view: the fast
	// path decided once, the hierarchy walked only when something is off.
	//
	//     const HE::ActiveFilter active(reg);
	//     for (auto [e, t, mesh] : view.each())
	//         if (active.off(e)) continue;
	struct ActiveFilter
	{
		explicit ActiveFilter(const entt::registry& r) : reg(r), anyOff(anyEntityInactive(r)) {}
		bool off(entt::entity e) const { return anyOff && !isEntityActive(reg, e); }
		const entt::registry& reg;
		const bool            anyOff;
	};
}
