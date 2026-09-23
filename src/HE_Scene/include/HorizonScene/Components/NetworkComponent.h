#pragma once
#include <cstdint>

// Marks an entity as replicated in a networked game session (Layer 3a).
//
// This is the GAMEPLAY path, deliberately separate from editor collaboration:
// collab replicates authored edits reliably and rarely, while this replicates
// simulation state ~30 times a second and tolerates loss, because a dropped
// snapshot is corrected by the next one. They share the transport underneath and
// nothing above it.
struct NetworkComponent
{
	// THE switch (plan §8.1). One tick in the inspector's "Replication" category
	// is the whole opt-in: the entity is registered when a session starts, its
	// transform rides the snapshot, and — once steps 6 and 7 land — its
	// replicated variables sync and its Run-On functions travel.
	//
	// Turning it OFF deliberately KEEPS the component, so the radius and the
	// speed limits below survive and a second click restores them. That is also
	// why this defaults to TRUE: a component somebody added by hand, and every
	// scene saved before this field existed, means "replicate this".
	bool replicates = true;

	// Identity across peers, assigned by the server. Zero means "not registered
	// yet" — an entity with no id is simply not replicated, which is how purely
	// local effects (muzzle flashes, debris) stay off the wire.
	std::uint32_t netId = 0;

	// Participant allowed to drive this entity. 0 = the server owns it. Used to
	// reject a client trying to move something that is not theirs.
	std::uint32_t owner = 0;

	// Interest management: clients further away than this never receive updates
	// for it. The single most effective bandwidth lever in a large world — far
	// more than any per-field compression.
	float relevanceRadius = 150.0f;

	// Entities that never move (level geometry, static props) waste a slot in
	// every snapshot. Clearing this keeps them out entirely after the initial
	// state is known.
	bool replicateTransform = true;

	// ── Anti-cheat: what the host may believe about this entity's movement ──
	// (docs/anti-cheat-plan.md §4.4). The host checks a client's claimed
	// displacement against maxSpeed·dt; anything beyond is a report, not a
	// correction (the snapshot already corrects). Authored here rather than in
	// a project-wide setting because a dash-capable player and a slow turret
	// are different entities with different truths.
	//
	// Horizontal, in m/s. 0 means "derive from MovementComponent::maxSpeed if
	// the entity has one, otherwise do not check" — so a character stays
	// checked by the number that already drives it, and a replicated prop
	// with no mover is left alone.
	float maxSpeed = 0.0f;

	// Vertical, in m/s. 0 means "do not check": there is no movement field to
	// derive a jump or fall speed from, so a game that wants the vertical
	// axis checked says so explicitly.
	float maxVerticalSpeed = 0.0f;
};
