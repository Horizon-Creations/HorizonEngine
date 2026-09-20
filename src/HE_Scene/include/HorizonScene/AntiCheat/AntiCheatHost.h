#pragma once

// ─── Anti-Cheat — the event/response side (plan §5) ──────────────────────────
// AntiCheatService MAKES reports; this is what happens to them. Three calls
// frame the host's tick:
//
//   pump()   after GameReplication::update: take every new report, decide its
//            responses from the policy, fire OnCheatDetected through every
//            frontend the application bound, and keep the report PENDING.
//   respond(reportId, responses)   any time in between: a handler REPLACES the
//            pending report's responses — the whole set, Log always included.
//   flush()  at the frame end: execute what is pending — Flag, Notice + Kick,
//            Ban, the telemetry queue — and only then.
//
// The one rule this object exists to enforce is the last one: a kick happens at
// the FRAME END and never where the event fires, because the event is the
// handler's window to overrule the policy (plan §5.3). It is the same rule the
// HorizonCode debugger follows for Continue/Step: never inside the pass that
// asked. The notice goes out on that flush, the disconnect on the NEXT one, so
// the notice has a frame to leave the socket before the link closes.
//
// The same object serves the CLIENT. A notice from the host (plan §5.5) becomes
// a LOCAL ticket with the same readers — level, rule, "player" 0 for "you" —
// and fires the same OnCheatDetected, so one graph shows "removed from the
// session: Damage" on both sides. The notice carries the rule's name and
// nothing else; a client never learns the score, the thresholds or the
// observations that led there.
//
// Anti-cheat OFF is a host with no service attached: isEnabled says false,
// every reader answers its neutral default, every exec row is a logged no-op.
// That is what the packaged game has until a session with a service exists,
// and what the editor has for a project that never ticked the box.

#include "HorizonScene/AntiCheat/AntiCheatService.h"

#include <Net/NetCommon.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class GameReplication;
class EventBus;
namespace HE { struct ProjectAntiCheatSettings; }

// No HE_API: HorizonScene exports every symbol (see GameReplication.h).
namespace HE::AntiCheat
{
	// A response set, as bits. The SAME bits as ProjectAntiCheatSettings::
	// Response — asserted in the .cpp — so a policy travels from the settings
	// file to here as one number, and `anticheat.respond(id, 0)` means "log
	// only" in every frontend without a translation table anywhere.
	enum Response : std::uint32_t
	{
		Log       = 1u << 0,   // a line in Cat::AntiCheat; always on, cannot be taken out
		Event     = 1u << 1,   // OnCheatDetected in every frontend + the EventBus
		Telemetry = 1u << 2,   // into the upload queue (step 7 drains it)
		Flag      = 1u << 3,   // marked for review; readable, no game effect
		Kick      = 1u << 4,   // notice, then NetSession::disconnect
		Ban       = 1u << 5,   // kick + the label refused for the rest of this session
		AllResponses = Log | Event | Telemetry | Flag | Kick | Ban,
	};

	// What the host does on its own per level. The defaults are the plan's
	// (§5.3): no kick on the two score levels, whose thresholds are unmeasured
	// starting values, a kick on Hard, which no hitch or burst can produce.
	struct Policy
	{
		std::uint32_t suspect   = Log | Event | Telemetry;
		std::uint32_t confirmed = Log | Event | Telemetry;
		std::uint32_t hard      = Log | Event | Telemetry | Kick;
		std::uint32_t forLevel(Level level) const;
	};

	// What native C++ in the host process gets through the EventBus (plan
	// §4.3): the same fields the script readers answer, in one struct.
	struct CheatDetected
	{
		int                   reportId = 0;
		Level                 level    = Level::Info;
		HE::Net::ConnectionId player   = HE::Net::kInvalidConnection;   // 0 on the client: "you"
		std::uint32_t         entity   = 0;      // network id, when the report names one
		float                 score    = 0.0f;
		std::string           rule;
		std::string           detail;
		std::string           label;
		bool                  local    = false;  // a notice about ourselves, not a report we made
	};

	// The reason code a notice carries. 0 is the host's policy; a game's
	// anticheat.kick passes its own, and the client reads it back off the local
	// ticket (reportReason). An int on purpose — the engine attaches no meaning
	// to any value but zero, so a game's codes never collide with the engine's.
	inline constexpr int kReasonPolicy = 0;

	class AntiCheatHost
	{
	public:
		AntiCheatHost();
		~AntiCheatHost();
		AntiCheatHost(const AntiCheatHost&)            = delete;
		AntiCheatHost& operator=(const AntiCheatHost&) = delete;

		// ── Wiring (the application, per session) ────────────────────────────
		// Either may be null. No service = anti-cheat OFF (see the file
		// comment). No replication = no wire: a kick is bookkept and logged but
		// nobody can be dropped — the headless case, and the client's until the
		// notice handler needs it. Neither is owned.
		void attach(AntiCheatService* service, GameReplication* replication);
		// End of the session. Pending responses and kicks in flight are dropped
		// with it — they were about connections that no longer exist — and so
		// are the client's local tickets.
		void detach();
		AntiCheatService* service() const { return m_service; }
		GameReplication*  replication() const { return m_replication; }

		void          setPolicy(const Policy& policy) { m_policy = policy; }
		const Policy& policy() const { return m_policy; }
		// The project's settings, translated: the service's knobs and the policy.
		static Config configFrom(const ProjectAntiCheatSettings& s);
		static Policy policyFrom(const ProjectAntiCheatSettings& s);

		// Preview (plan §6.2.6, the editor's play mode): everything except Kick,
		// Ban and Telemetry. Reports go to the log and the events, and nobody is
		// dropped from a session that is the author's own window.
		void setPreview(bool preview) { m_preview = preview; }
		bool preview() const { return m_preview; }

		// ── Event delivery ───────────────────────────────────────────────────
		// How a report reaches the script frontends. The application binds ONE
		// sink that fires the HorizonCode Game Instance, the level script, the
		// entity's class, the Lua/Python instances and the C++ GameLogic hook —
		// the order the plan gives (§5.4) — because HE_Scene can name none of
		// the application's objects. Called from pump(), inside the frame, and
		// only for reports whose responses include Event.
		using Sink = std::function<void(int reportId, const Report& report)>;
		void setEventSink(Sink sink) { m_sink = std::move(sink); }
		// Native C++ subscribers (CheatDetected). Optional; not owned.
		void setEventBus(EventBus* bus) { m_bus = bus; }

		// ── The frame ────────────────────────────────────────────────────────
		// Frame start. Takes new reports from the service and new notices from
		// the replication (client), fires the events, holds the reports pending.
		// Returns how many events were fired — the event-driven host uses that
		// to ask for the frame that draws the reaction.
		int  pump();
		// Frame end. Executes the pending responses, then the disconnects whose
		// notice went out on the previous flush.
		void flush();

		// ── The rows (plan §4.3) ─────────────────────────────────────────────
		// Rule check. Step 5 brings the rule table; until then every rule passes,
		// which is the contract for an unknown rule anyway: a check the engine
		// cannot make must not block the game.
		bool check(const std::string& rule, float value, HE::Net::ConnectionId player);
		void expectDisplacement(std::uint32_t entity, float maxDistance);
		void report(HE::Net::ConnectionId player, const std::string& rule, float weight,
		            const std::string& detail);
		void setPlayerLabel(HE::Net::ConnectionId player, const std::string& label);
		// Replace the pending report's responses. False when the ticket is not
		// pending — unknown, already flushed, or a client's local one — and the
		// call is then a logged no-op: a handler that answers a frame late has
		// missed its window, and pretending otherwise would kick nobody.
		bool respond(int reportId, std::uint32_t responses);
		// Explicit kick, independent of the score. Notice at this frame's flush,
		// disconnect at the next. `reasonCode` is the game's own; the client
		// reads it back through the ticket. No-op without a session.
		void kick(HE::Net::ConnectionId player, int reasonCode);

		// Readers. A ticket answers as long as the service keeps it (reportTtlSec)
		// or, for a local one, until detach. Unknown = neutral defaults.
		int         reportLevel(int reportId) const;    // Level as an int; 0 unknown
		std::string reportRule(int reportId) const;
		int         reportPlayer(int reportId) const;   // ConnectionId; 0 = unknown, or "you"
		int         reportEntity(int reportId) const;   // network id; 0 = none
		float       reportScore(int reportId) const;
		std::string reportDetail(int reportId) const;   // the observation list; on the client the rule
		int         reportReason(int reportId) const;   // the notice's reason code; 0 for a host report
		float       playerScore(HE::Net::ConnectionId player) const;
		bool        isEnabled() const { return m_service != nullptr; }
		bool        isFlagged(HE::Net::ConnectionId player) const;
		// Ban without identity (plan §6.1): the LABEL is all a session can hold
		// on to. The application asks this when a player joins with a label.
		bool        isBanned(const std::string& label) const;
		// The ticket as one struct, or nullptr. What the sink gets, and what the
		// application's EventBus subscribers get as CheatDetected.
		const Report* findReport(int reportId) const;

		struct Stats
		{
			std::uint32_t reportsTaken  = 0;
			std::uint32_t eventsFired   = 0;
			std::uint32_t responded     = 0;   // respond() calls that hit a pending report
			std::uint32_t flagged       = 0;
			std::uint32_t noticesSent   = 0;
			std::uint32_t kicked        = 0;   // disconnects executed
			std::uint32_t banned        = 0;
			std::uint32_t telemetryQueued = 0;
			std::uint32_t noticesTaken  = 0;   // client side
		};
		const Stats& stats() const { return m_stats; }

		// ── Telemetry hand-off (step 7) ──────────────────────────────────────
		// Reports whose responses included Telemetry, in order. The sink of a
		// later step takes them out; until one exists the queue is bounded and
		// the oldest falls off, so a long session cannot grow through it.
		bool takeTelemetry(Report& out);
		static constexpr std::size_t kMaxTelemetryQueue = 128;

	private:
		struct Pending
		{
			int                   id    = 0;
			HE::Net::ConnectionId conn  = HE::Net::kInvalidConnection;
			Level                 level = Level::Info;
			std::uint32_t         responses = 0;
			std::string           rule;
			int                   reason = kReasonPolicy;
		};
		struct KickInFlight
		{
			HE::Net::ConnectionId conn = HE::Net::kInvalidConnection;
			bool                  ban  = false;
			std::string           label;
		};

		void fire(int reportId, const Report& r, bool local);
		void execute(const Pending& p, const Report* r);
		void sendNotice(HE::Net::ConnectionId conn, Level level, int reason,
		                const std::string& rule);
		int  makeLocalReport(Level level, int reason, const std::string& rule);

		AntiCheatService* m_service     = nullptr;
		GameReplication*  m_replication = nullptr;
		Policy            m_policy;
		bool              m_preview     = false;
		Sink              m_sink;
		EventBus*         m_bus         = nullptr;

		std::vector<Pending>      m_pending;      // this frame's reports, until flush
		std::vector<KickInFlight> m_kicking;      // notice sent, disconnect on the next flush
		std::unordered_set<HE::Net::ConnectionId> m_flagged;
		std::unordered_set<std::string>           m_banned;

		// The client's tickets. Their own counter: a client has no service, so
		// nothing else mints ids on this side, and detach() clears them before
		// this process could ever host a session of its own.
		std::deque<Report>            m_local;
		std::unordered_map<int, int>  m_localReason;   // ticket → reason code
		int                           m_nextLocalId = 0;
		static constexpr std::size_t  kMaxLocalReports = 16;

		std::deque<Report> m_telemetry;
		Stats              m_stats;
	};
} // namespace HE::AntiCheat
