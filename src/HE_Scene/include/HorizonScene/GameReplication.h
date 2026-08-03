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
// Two mechanisms, for two different problems:
//
//   *Other* entities are INTERPOLATED between the last two snapshots, so they do
//   not visibly step at the tick rate on a higher-refresh display.
//
//   The player's OWN entity is PREDICTED: input is applied locally the instant
//   it happens, then replayed on top of whatever the server later confirms.
//   Without this, moving would only respond after a full round trip — which
//   feels wrong at any ping above ~50 ms, and no amount of interpolation hides
//   it, because interpolation is about smoothness, not latency.

#include "HorizonScene/HorizonWorld.h"

#include <Net/NetSession.h>

#include "HorizonScene/Components/TransformComponent.h"

#include <cstdint>
#include <functional>
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

		// A predicted position further than this from the server's answer is
		// snapped rather than eased, because easing a large error looks like the
		// character sliding on ice.
		float  reconcileSnapDistance = 2.0f;
		// How quickly a small correction is eased away (fraction per second).
		float  reconcileSmoothing = 12.0f;

		// Cap on unacknowledged inputs kept for replay. At 60 Hz this is a
		// second of round trip; beyond that the connection is unusable anyway and
		// an unbounded buffer would be the real problem.
		std::size_t maxPendingInputs = 64;
	};

	// One player command. The engine does not interpret it — the game supplies a
	// mover callback — so this stays a movement delta plus whatever the game
	// needs to reproduce the step deterministically.
	struct InputCommand
	{
		std::uint32_t sequence = 0;
		float         deltaTime = 0.0f;
		glm::vec3     move { 0.0f };    // desired movement, game-defined units
		float         yaw = 0.0f;       // facing, degrees
	};

	// Applies one command to a transform. MUST be deterministic: the client
	// replays the same commands the server already ran, and any divergence
	// between the two shows up as a correction the player can feel.
	using MoveFn = std::function<void(TransformComponent&, const InputCommand&)>;

	GameReplication(HE::Net::NetSession* net, HE::Net::NetRole role, Config cfg);

	// Delegating overload rather than `Config cfg = {}`: the default argument
	// would be parsed while Config is still incomplete, since its members carry
	// initializers. Inline bodies compile once the class is complete.
	GameReplication(HE::Net::NetSession* net, HE::Net::NetRole role)
		: GameReplication(net, role, Config{}) {}

	void setWorld(HorizonWorld* world) { m_world = world; }

	// The simulation step, shared by client prediction and server execution.
	// Without one shared function the two would drift by construction.
	void setMoveFunction(MoveFn fn) { m_move = std::move(fn); }

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

	// ── Prediction (client) ──
	// The entity this client controls. Its transform is driven by local input
	// immediately and corrected against the server, rather than interpolated.
	void setLocallyControlled(Entity entity, std::uint32_t netId);

	// Apply input now, remember it, and send it to the server. Returns the
	// sequence number assigned, which is what the server later acknowledges.
	std::uint32_t pushInput(const glm::vec3& move, float yaw, float dt);

	// How many of our inputs the server has not confirmed yet — effectively the
	// round trip expressed in commands. Useful as a diagnostic overlay.
	std::size_t pendingInputCount() const { return m_pendingInputs.size(); }

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
		std::uint32_t inputsSent        = 0;
		std::uint32_t inputsProcessed   = 0;   // server side
		std::uint32_t reconciliations   = 0;   // corrections that moved us
		std::uint32_t hardSnaps         = 0;   // corrections too large to ease
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
	void handleInput(HE::Net::ConnectionId conn, HE::Net::BitReader& r);
	void reconcile(const Sample& authoritative, std::uint32_t ackedSequence);
	void applySmoothing(float dt);

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

	// ── Prediction state (client) ──
	MoveFn        m_move;
	Entity        m_controlled      = entt::null;
	std::uint32_t m_controlledNetId = 0;
	std::uint32_t m_inputSequence   = 0;
	std::vector<InputCommand> m_pendingInputs;
	// Residual error after a correction, eased out over a few frames so a small
	// misprediction does not read as a visible jolt.
	glm::vec3     m_positionError { 0.0f };

	// ── Per-client input tracking (server) ──
	std::unordered_map<HE::Net::ConnectionId, std::uint32_t> m_lastProcessedInput;
	// Which entity each connection is allowed to drive. A client sending input
	// for something it does not own must not move it.
	std::unordered_map<HE::Net::ConnectionId, std::uint32_t> m_controlledByConn;

public:
	// Server: bind a connection to the entity it controls, so its input is
	// accepted for that entity and refused for every other.
	void assignControl(HE::Net::ConnectionId conn, std::uint32_t netId);
};
