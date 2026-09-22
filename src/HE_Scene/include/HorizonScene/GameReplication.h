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

#include <Integrity/IntegrityProbe.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <glm/glm.hpp>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace HE::AntiCheat { class AntiCheatService; }

// No HE_API here. HorizonScene is built with WINDOWS_EXPORT_ALL_SYMBOLS, so the
// macro would expand to __declspec(dllimport) inside its own translation units
// and every definition would clash with its declaration (MSVC C4273,
// "inconsistent dll linkage"). HE_API belongs to HorizonCore, which exports
// explicitly; the scene layer never uses it.
class GameReplication
{
public:
	// The longest timestep a single command may represent. The server enforces
	// it so a modified client cannot claim a ten-second frame and teleport, and
	// the CLIENT must apply exactly the same bound when predicting — otherwise
	// the two run different simulations and every long frame mispredicts.
	// Public because the anti-cheat dt budget books the clamped value too.
	static constexpr float kMaxInputDeltaTime = 0.1f;

	// A manifest is the exe, a handful of libraries and a few paks. Anything
	// beyond this did not come from our writer. Public so the fragmentation
	// budget test can build the worst case the writer is allowed to produce.
	static constexpr std::uint16_t kMaxManifestEntries = 256;

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

		// Largest snapshot payload per datagram. Snapshots travel Unreliable,
		// and UdpTransport refuses (does not fragment) an unreliable message
		// over its 1200-byte MTU payload — one lost piece would lose the whole.
		// So a tick's entities are split across as many kMsgSnapshot datagrams
		// as this budget needs, all carrying the same tick number. 1024 leaves
		// room for the NetSession id (2), the SecureTransport counter and GCM
		// tag (24) and some margin under 1200. Below one entity it clamps to
		// one entity per datagram.
		std::size_t snapshotBudgetBytes = 1024;

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

		// Compare each joining client's integrity manifest (exe, engine
		// libraries, pak TOC hashes) against the host's own (plan §3.5). Only
		// acts with an anti-cheat service attached and a manifest on the host;
		// a dev build has neither and the check is simply off. The project
		// setting of the same name lands here.
		bool integrityCheck = true;
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

	// ── Anti-cheat (server) ──
	// Optional. With a service attached, handleInput runs its pre-apply check
	// (format, rate, dt budget) before the mover and its post-apply check
	// (displacement against maxSpeed·dt) after it, and reports every refusal it
	// used to make silently. nullptr — the default — is anti-cheat OFF, and the
	// behaviour is byte-for-byte what it was before the service existed. Not
	// owned; the caller keeps it alive for as long as it is attached.
	//
	// The host must assignControl a connection BEFORE it tells that client to
	// take control: input from a connection with no assignment is a Hard
	// observation (no legitimate client produces it), and a spawn message that
	// races ahead of the assignment would make an honest player look like one.
	void setAntiCheat(HE::AntiCheat::AntiCheatService* service) { m_antiCheat = service; }
	HE::AntiCheat::AntiCheatService* antiCheat() const { return m_antiCheat; }

	// ── Anti-cheat notice (plan §5.5) ──
	// What a client is told before the host drops it: the level, a reason code
	// and the NAME of the rule — and nothing else. No observation list, no
	// thresholds, no score: a prober that gets itself kicked learns which rule
	// fired and not how the heuristics behind it work, the same rule the bare
	// reject code in failPeer follows.
	struct AntiCheatNotice
	{
		int         level      = 0;   // HE::AntiCheat::Level as an int
		int         reasonCode = 0;   // 0 = the host's policy; anything else is the game's
		std::string rule;             // "" when the game kicked without naming one
	};
	// Host: send the notice, reliable. The disconnect is the caller's next step
	// (AntiCheatHost::flush), one frame later, so the frame has left the socket.
	void sendAntiCheatNotice(HE::Net::ConnectionId conn, const AntiCheatNotice& notice);
	// Client: notices that arrived since the last call, oldest first. The frame
	// loop turns each into a local report ticket and fires OnCheatDetected.
	bool takeAntiCheatNotice(AntiCheatNotice& out);

	// Host: forget everything about a connection this side dropped. NetSession
	// fires no onDisconnect for a link we severed ourselves, so the per-client
	// input tracking, the control assignment and the anti-cheat state would
	// otherwise outlive the peer — and a connection id the transport reuses
	// would inherit them.
	void dropConnection(HE::Net::ConnectionId conn);

	// The session this replication runs on, for the one caller that has to
	// speak to the transport directly (the kick). Null in a headless test.
	HE::Net::NetSession* session() const { return m_net; }
	// The entity a network id names on this side, or entt::null.
	Entity entityOf(std::uint32_t netId) const;

	// ── Integrity (plan §3.5) ──
	// This side's manifest. Without a call, update() adopts the process-wide
	// IntegrityProbe once it is Ready, or "no manifest" when it was never
	// started (dev build). Explicit nullopt says the same. A client sends its
	// manifest to every connection once, as soon as it knows what it has; the
	// host compares each client's list against its own and reports every file
	// that differs as an IntegrityMismatch observation with the file's name.
	void setLocalManifest(std::optional<HE::Integrity::Manifest> manifest);
	bool hasLocalManifest() const { return m_localManifest == LocalManifest::Have; }

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
		std::uint32_t snapshotsSent     = 0;   // datagrams: a split tick counts each part
		std::uint32_t entitiesSent      = 0;   // summed over snapshots
		std::uint32_t entitiesCulled    = 0;   // skipped by interest management
		std::uint32_t snapshotsReceived = 0;
		std::uint32_t snapshotsSplit    = 0;   // extra datagrams beyond the first, per client and tick
		std::uint32_t snapshotsStale    = 0;   // client side: dropped, older than the newest tick seen
		std::uint32_t samplesDuplicate  = 0;   // client side: entity already had this tick's sample
		std::uint32_t bytesSent         = 0;
		std::uint32_t inputsSent        = 0;
		std::uint32_t inputsProcessed   = 0;   // server side
		std::uint32_t reconciliations   = 0;   // corrections that moved us
		std::uint32_t hardSnaps         = 0;   // corrections too large to ease
		std::uint32_t manifestsSent     = 0;   // client side
		std::uint32_t manifestsChecked  = 0;   // host side: compared against our own
		std::uint32_t integrityMismatches = 0; // host side: files that differed
		std::uint32_t noticesSent       = 0;   // host side: anti-cheat notices before a kick
		std::uint32_t noticesReceived   = 0;   // client side
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
		// Tick of `current`. A second sample for the same tick is a duplicated
		// datagram (or a re-sent part) and must not shift the buffer again —
		// that would set previous = current and freeze the entity until the
		// next tick.
		std::uint32_t tick = 0;
		bool          hasTick = false;
	};

	void sendSnapshots();
	void applySnapshot(HE::Net::BitReader& r);
	void advanceInterpolation(float dt);
	void handleInput(HE::Net::ConnectionId conn, HE::Net::BitReader& r);

	// Integrity: the local manifest is resolved lazily (the probe hashes on a
	// worker at startup and may still be running when the first connection
	// appears), the client sends once per connection, the host queues what
	// arrives until its own manifest is known and then compares.
	enum class LocalManifest : std::uint8_t { Unknown, None, Have };
	struct GuestManifest
	{
		bool                    present = false;   // false: the client said it has none
		HE::Integrity::Manifest manifest;
	};
	void resolveLocalManifest();
	void sendManifest(HE::Net::ConnectionId conn);
	void handleIntegrity(HE::Net::ConnectionId conn, HE::Net::BitReader& r);
	void checkGuestManifests();
	void checkGuestManifest(HE::Net::ConnectionId conn, const GuestManifest& guest);
	void handleAntiCheatNotice(HE::Net::ConnectionId conn, HE::Net::BitReader& r);
	void reconcile(const Sample& authoritative, std::uint32_t ackedSequence, std::uint32_t tick);
	void applySmoothing(float dt);

	void writeSample(HE::Net::BitWriter& w, const Sample& s) const;
	bool readSample(HE::Net::BitReader& r, Sample& s) const;
	// One step of the position quantisation on the wire. Both the reconcile dead
	// zone and the anti-cheat displacement allowance are widened by it, because
	// a perfect move still arrives rounded by up to this much.
	float quantStep() const;

	HE::Net::NetSession* m_net  = nullptr;
	HE::Net::NetRole     m_role = HE::Net::NetRole::None;
	Config               m_cfg;
	HorizonWorld*        m_world = nullptr;
	HE::AntiCheat::AntiCheatService* m_antiCheat = nullptr;

	std::unordered_map<std::uint32_t, Entity> m_byNetId;
	std::uint32_t m_nextNetId = 1;

	std::unordered_map<HE::Net::ConnectionId, glm::vec3> m_viewpoints;
	std::unordered_map<std::uint32_t, InterpState>       m_interp;

	float  m_tickAccumulator = 0.0f;
	Stats  m_stats;

	// ── Snapshot ordering ──
	// Server: the number of the tick being sent; every datagram of one tick
	// carries it, so a client can tell "another part of the same tick" from
	// "an older tick that arrived late". Starts at 1 and skips 0 on wrap, so
	// a client that has seen nothing yet (m_newestTick = 0) never mistakes
	// the first snapshot for a duplicate.
	std::uint32_t m_snapshotTick = 1;
	// Client: newest tick applied; anything older is dropped whole. Compared
	// as a signed 32-bit difference, so the counter may wrap.
	std::uint32_t m_newestTick = 0;
	// Client: tick of the last reconciliation, so a duplicated datagram does
	// not run the correction twice and a stale one cannot rewind the ack.
	std::uint32_t m_lastReconcileTick = 0;

	// ── Prediction state (client) ──
	MoveFn        m_move;
	Entity        m_controlled      = entt::null;
	std::uint32_t m_controlledNetId = 0;
	std::uint32_t m_inputSequence   = 0;
	std::vector<InputCommand> m_pendingInputs;
	// Residual error after a correction, eased out over a few frames so a small
	// misprediction does not read as a visible jolt.
	glm::vec3     m_positionError { 0.0f };

	// ── Integrity ──
	LocalManifest           m_localManifest = LocalManifest::Unknown;
	HE::Integrity::Manifest m_manifest;
	std::unordered_set<HE::Net::ConnectionId>                 m_manifestSentTo;      // client
	std::unordered_set<HE::Net::ConnectionId>                 m_manifestReceived;    // host: one per connection
	std::unordered_map<HE::Net::ConnectionId, GuestManifest>  m_pendingGuestManifests;
	bool                    m_integrityOffLogged = false;

	// ── Anti-cheat notices (client) ──
	std::deque<AntiCheatNotice> m_notices;

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
