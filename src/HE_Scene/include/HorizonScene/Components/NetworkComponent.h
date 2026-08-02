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
};
