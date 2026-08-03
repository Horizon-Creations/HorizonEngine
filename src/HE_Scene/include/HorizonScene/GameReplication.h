#pragma once

// ─── Layer 3a — gameplay replication ─────────────────────────────────────────
// Server-authoritative snapshot replication of entities carrying a
// NetworkComponent. The server simulates, samples the world at a fixed tick and
// sends each client the entities relevant to it; clients apply what arrives.
//
// Why this is a separate consumer from editor collaboration, despite sharing the
// transport: the two have opposite requirements. Collab replicates authored
// edits — rare, reliable, must never be lost. Gameplay replicates simulation
// state — ~30 Hz, loss-tolerant, because a dropped snapshot is corrected by the
// next one a few milliseconds later. Forcing gameplay through the collab path
// would make every position update a reliable message, and forcing collab
// through this one would silently drop somebody's edit.
//
// What this does NOT do yet: client-side prediction and server reconciliation.
// Without them a controlled character feels laggy by exactly the round-trip
// time, so this is the honest foundation rather than a finished netcode. What it
// does do is interpolate between the last two snapshots, which is what keeps
// *other* entities from visibly stepping at the tick rate.

#include "HorizonScene/HorizonWorld.h"

#include <Net/NetSession.h>

#include <cstdint>
#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

// No HE_API here. HorizonScene is built with WINDOWS_EXPORT_ALL_SYMBOLS, so the
// macro would expand to __declspec(dllimport) inside its own translation units
// and every definition would clash with its declaration (MSVC C4273,
// "inconsistent dll linkage"). HE_API belongs to HorizonCore, which exports
// explicitly; the scene layer never uses it.
class GameReplication
{
public:
	struct Config
	{
		// Snapshot rate. Higher costs bandwidth linearly; lower makes
		// interpolation lag more visible.
		float  tickHz = 30.0f;

		// Position quantization bound, in world units from the origin. Positions
		// outside it clamp, so it must comfortably contain the playable area.
		float  worldExtent = 4096.0f;
		// 24 bits over ±4096 is sub-millimetre — far finer than anything a player
		// can see, and still only 9 bytes for a position.
		int    positionBits = 24;
		// Euler degrees over [-180, 180]; 16 bits is ~0.005°.
		int    rotationBits = 16;

		// Snapshots older than this are dropped from the interpolation buffer.
		float  interpolationDelaySec = 0.1f;
	};

	GameReplication(HE::Net::NetSession* net, HE::Net::NetRole role, Config cfg);

	// Delegating overload rather than `Config cfg = {}`: the default argument
	// would be parsed while Config is still incomplete, since its members carry
	// initializers. Inline bodies compile once the class is complete.
	GameReplication(HE::Net::NetSession* net, HE::Net::NetRole role)
		: GameReplication(net, role, Config{}) {}

	void setWorld(HorizonWorld* world) { m_world = world; }

	// ── Server ──
	// Give an entity a network identity. Until then it is not replicated, which
	// is how purely local effects stay off the wire.
	std::uint32_t registerEntity(Entity entity, std::uint32_t owner = 0);
	void          unregisterEntity(Entity entity);

	// ── Client ──
	// Adopt an entity under the id the SERVER assigned. A client must never mint
	// its own ids — the two peers would then disagree about which entity a
	// snapshot refers to. This is what a spawn message hands to the client.
	bool adoptEntity(Entity entity, std::uint32_t netId, std::uint32_t owner = 0);

	// Where a client is looking from, for interest management. Without this a
	// client is treated as interested in everything, which is correct but
	// wasteful.
	void setViewpoint(HE::Net::ConnectionId conn, const glm::vec3& position);

	// ── Both ──
	// Server: emits a snapshot when the tick is due. Client: advances
	// interpolation. Call once per frame with real delta time.
	void update(float dt);

	// ── Diagnostics ──
	struct Stats
	{
		std::uint32_t snapshotsSent     = 0;
		std::uint32_t entitiesSent      = 0;   // summed over snapshots
		std::uint32_t entitiesCulled    = 0;   // skipped by interest management
		std::uint32_t snapshotsReceived = 0;
		std::uint32_t bytesSent         = 0;
	};
	const Stats& stats() const { return m_stats; }
	void         resetStats() { m_stats = {}; }

	std::size_t replicatedCount() const { return m_byNetId.size(); }

private:
	struct Sample
	{
		glm::vec3 position { 0.0f };
		glm::vec3 rotation { 0.0f };   // Euler degrees, matching TransformComponent
	};

	// Two most recent samples per entity, so the client can interpolate rather
	// than snapping at the tick rate.
	struct InterpState
	{
		Sample previous;
		Sample current;
		float  elapsed = 0.0f;   // seconds since `current` arrived
		bool   hasPrevious = false;
	};

	void sendSnapshots();
	void applySnapshot(HE::Net::BitReader& r);
	void advanceInterpolation(float dt);

	void writeSample(HE::Net::BitWriter& w, const Sample& s) const;
	bool readSample(HE::Net::BitReader& r, Sample& s) const;

	HE::Net::NetSession* m_net  = nullptr;
	HE::Net::NetRole     m_role = HE::Net::NetRole::None;
	Config               m_cfg;
	HorizonWorld*        m_world = nullptr;

	std::unordered_map<std::uint32_t, Entity> m_byNetId;
	std::uint32_t m_nextNetId = 1;

	std::unordered_map<HE::Net::ConnectionId, glm::vec3> m_viewpoints;
	std::unordered_map<std::uint32_t, InterpState>       m_interp;

	float  m_tickAccumulator = 0.0f;
	Stats  m_stats;
};
