#pragma once

// ─── Anti-Cheat — server-side observations, score and reports ────────────────
// The host is authoritative: a client only ever sends INPUT, and the one place
// that input enters the simulation is GameReplication::handleInput. This
// service hangs off that function with a hook before the mover runs and one
// after it, and turns every refusal — which used to be a silent `return` — into
// an OBSERVATION with a weight.
//
// Observations are never a verdict on their own. A host hitch, a client under
// load and a TCP burst after a stall all produce the same shapes as a clumsy
// cheat, so each connection carries a SCORE that observations add to and that
// decays with a half-life. Crossing a threshold is a LEVEL change, and every
// level change upwards is a REPORT: connection, level, the observations of the
// last few seconds, the score. Reports are what the log carries today and what
// events, kick and telemetry will carry in later steps (docs/anti-cheat-plan.md
// §5). Going down again — the score decaying — produces nothing; it only
// closes the escalation.
//
// `Hard` is deliberately its own level and not "Confirmed with a big weight":
// it is reserved for observations no legitimate client can produce — a frame
// that does not parse, input from a connection that controls nothing. No hitch,
// burst or clock skew can cause those, so they need no calibration and stick to
// the connection for the rest of the session.
//
// Anti-cheat OFF is the absence of this object. GameReplication holds an
// optional pointer; with nullptr its behaviour is byte-for-byte what it was
// before this service existed. There is no "enabled" branch inside the checks.
//
// Scope of this file (plan §7.1 step 2): the server path only. No policy
// execution (kick, notice), no script events, no settings page, no telemetry —
// those read the reports this produces (takeReport) and come in later steps.

#include "HorizonScene/GameReplication.h"

#include <Net/NetCommon.h>

#include <cstdint>
#include <deque>
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>

// No HE_API: HorizonScene exports every symbol (see GameReplication.h).
namespace HE::AntiCheat
{
	// Ordered so that a handler can write `level >= Confirmed`.
	enum class Level : std::uint8_t
	{
		Info      = 0,   // score below the suspect threshold; no report
		Suspect   = 1,   // score >= suspectThreshold
		Confirmed = 2,   // score >= confirmedThreshold
		Hard      = 3,   // one observation no legitimate client produces
	};
	const char* levelName(Level level);

	enum class Kind : std::uint8_t
	{
		Malformed,       // frame did not parse, or carried NaN/Inf     → Hard
		ForeignEntity,   // input from a connection assigned no entity  → Hard
		InputRate,       // more inputs per second than any client sends
		DtBudget,        // accepted simulated time outruns wall time (speedhack)
		Displacement,    // the mover carried the entity further than maxSpeed·dt
		IntegrityMismatch, // the client's exe/dylib/pak hashes differ from the host's (plan §3.5)
		Custom,          // reported by the game (anticheat.report, step 5)
	};
	const char* kindName(Kind kind);
	// Malformed and ForeignEntity. The rest are weighed, these are facts.
	// IntegrityMismatch is weighed by default and Hard only when
	// Config::integrityHard says so; that is the service's decision, not the
	// kind's.
	bool        isHard(Kind kind);

	struct Observation
	{
		Kind        kind   = Kind::Custom;
		float       weight = 0.0f;    // what it added to the score (0 for Hard)
		std::string detail;           // human-readable, redaction-safe
		double      atWall = 0.0;     // service wall clock when it was made
	};

	struct Config
	{
		// ── dt budget (plan §3.3 b) ──
		// Accepted simulated time may exceed wall time by this fraction before it
		// counts. Client and host clocks drift, and the per-command clamp only
		// ever pushes the ratio DOWN; only a stretched timer pushes it up.
		float tolerance = 0.15f;
		// Sliding window the ratio is measured over. Has to be seconds, not a
		// frame: after a 500 ms TCP stall thirty commands arrive at once and
		// their dt sums to exactly the 500 ms the host waited.
		float windowSec = 3.0f;
		// The ratio is judged ONCE per wall second per connection, not per frame.
		// Per frame, a 1.5× speedhack would reach Confirmed in a tenth of a
		// second; per second it takes the handful of seconds a human would also
		// need to notice, and leaves a handler time to see Suspect first.
		float evalIntervalSec = 1.0f;

		// ── input rate (plan §3.3 a) ──
		// 4× the 60 Hz rate, because bursts after a stall deliver everything
		// at once. Inputs beyond this in the window are dropped, not applied.
		float maxInputsPerSecond = 240.0f;
		float rateWindowSec      = 2.0f;

		// ── score (plan §3.6) ──
		float halfLifeSec        = 30.0f;   // score *= 0.5 every halfLife
		float suspectThreshold   = 5.0f;
		float confirmedThreshold = 20.0f;
		// How far back a report lists observations.
		float reportHistorySec   = 10.0f;
		// How long a report ticket stays readable after it was made. Long enough
		// for a widget that opens later to still read it.
		float reportTtlSec       = 60.0f;

		// ── integrity (plan §3.5) ──
		// One observation per file that differs between the client's manifest
		// and the host's. The default weight sits just above the suspect
		// threshold: a single edited pak is Suspect at once and STAYS so for a
		// handler that reads the level a few frames later (a weight equal to
		// the threshold would decay below it on the very next update), and
		// four differing files reach Confirmed. A game that ships nothing
		// moddable can make it Hard instead.
		float integrityWeight = 6.0f;
		bool  integrityHard   = false;
	};

	// One weight can never exceed this. A 100-unit teleport against an allowance
	// of a few centimetres would otherwise weigh several hundred points and keep
	// the score above Confirmed for minutes after a single event.
	inline constexpr float kMaxObservationWeight = 100.0f;

	// Speed bounds for the post-apply check, per axis group. 0 = not checked.
	// Resolved by the caller (GameReplication) from the entity's components —
	// NetworkComponent::maxSpeed once step 3 adds it, MovementComponent::maxSpeed
	// as the fallback — so this service never touches a registry.
	struct MoveLimits
	{
		float maxSpeed         = 0.0f;   // horizontal (xz), units per second
		float maxVerticalSpeed = 0.0f;   // y, units per second
	};

	// Everything the post-apply check needs about one applied command.
	struct MoveCheck
	{
		std::uint32_t netId     = 0;
		glm::vec3     before { 0.0f };
		glm::vec3     after  { 0.0f };
		float         deltaTime = 0.0f;   // the CLAMPED dt the mover ran with
		MoveLimits    limits;
		// One quantisation step of the replication channel. Added to every
		// allowance, because a perfectly legal move otherwise fails by the width
		// of the wire's rounding — the same lesson as the reconcile dead zone.
		float         quantStep = 0.0f;
	};

	struct Report
	{
		int                      id     = 0;
		HE::Net::ConnectionId    conn   = HE::Net::kInvalidConnection;
		Level                    level  = Level::Info;
		float                    score  = 0.0f;
		Kind                     trigger = Kind::Custom;   // the observation that tipped it
		std::uint32_t            netId   = 0;              // entity involved, when known
		std::string              label;                    // player label, "" unless set
		std::string              detail;                   // observation list as one line
		std::vector<Observation> observations;             // the last reportHistorySec
		double                   atWall  = 0.0;
	};

	class AntiCheatService
	{
	public:
		explicit AntiCheatService(Config cfg);
		AntiCheatService() : AntiCheatService(Config{}) {}

		const Config& config() const { return m_cfg; }

		// ── Hooks (called by GameReplication on the host) ──
		// Before the mover: format (finite numbers), input rate, and the dt
		// accounting for the speedhack check. False = drop the command; the next
		// snapshot corrects the client, exactly like any misprediction.
		bool preApply(HE::Net::ConnectionId conn, const GameReplication::InputCommand& cmd);
		// After the mover: was the displacement possible at the entity's speed?
		// False = the caller restores `before`. A pending expectDisplacement for
		// the entity is consumed here, whether or not it covered the move.
		bool postApply(HE::Net::ConnectionId conn, const MoveCheck& check);
		// A refusal the caller made itself (parse failure, no assignment).
		void observe(HE::Net::ConnectionId conn, Kind kind, float weight, std::string detail);
		// One file of the client's manifest that does not match the host's
		// (GameReplication compares at join). Weight and hardness come from the
		// config; `detail` names the file (Integrity::describe).
		void integrityMismatch(HE::Net::ConnectionId conn, std::string detail);
		// Wall clock. GameReplication::update(dt) drives this on the server;
		// do not call it a second time from the frame loop while attached.
		void update(float dt);

		// ── Game-facing (plan §4.3, the parts step 2 needs) ──
		// One-shot allowance for the NEXT post-apply check of this entity: a
		// respawn, portal or dash is legitimate if the game says so beforehand.
		// Without it the same move weighs kMaxObservationWeight.
		void expectDisplacement(std::uint32_t netId, float maxDistance);
		// Game-defined observation (line of sight violated, impossible pickup…).
		void report(HE::Net::ConnectionId conn, const std::string& rule, float weight,
		            const std::string& detail = {});
		// Name for log and reports. Without it a connection is only its id.
		void setPlayerLabel(HE::Net::ConnectionId conn, std::string label);
		// Drop everything about a connection. The host calls this on disconnect;
		// nothing here observes the transport.
		void forgetConnection(HE::Net::ConnectionId conn);

		// ── Readers ──
		float score(HE::Net::ConnectionId conn) const;
		// Hard once any Hard observation was made, otherwise the score level.
		Level level(HE::Net::ConnectionId conn) const;
		// Observations still inside the report history (reportHistorySec), not a
		// lifetime total — the score is the lifetime summary.
		std::size_t observationCount(HE::Net::ConnectionId conn) const;
		std::size_t observationCount(HE::Net::ConnectionId conn, Kind kind) const;

		// Reports are handed out as integer tickets, like OnHttpResponse: an event
		// carries one value, a report carries a dozen. The host takes new ids
		// out in order and reads the fields through findReport. A ticket answers
		// nullptr once it expired (reportTtlSec) or was evicted (kMaxReports).
		bool          takeReport(int& id);
		const Report* findReport(int id) const;
		std::size_t   pendingReportCount() const { return m_pendingReports.size(); }
		static constexpr std::size_t kMaxReports = 64;

		struct Stats
		{
			std::uint32_t observations   = 0;
			std::uint32_t rejectedInputs = 0;   // preApply said no
			std::uint32_t rolledBack     = 0;   // postApply said no
			std::uint32_t reports        = 0;
		};
		const Stats& stats() const { return m_stats; }

	private:
		struct InputStamp
		{
			double wall;
			bool   accepted;
		};
		struct Frame
		{
			float wall     = 0.0f;   // host time this frame covered
			float accepted = 0.0f;   // simulated time the accepted commands claimed
		};
		struct ConnState
		{
			double score   = 0.0;
			Level  level   = Level::Info;   // score level, without Hard
			bool   hard    = false;
			std::string label;

			// dt budget: one Frame per update(), summed over the window.
			std::deque<Frame> frames;
			Frame  pending;                  // the frame being accumulated
			double windowWall     = 0.0;
			double windowAccepted = 0.0;
			float  evalAccum      = 0.0f;
			double firstSeen      = 0.0;

			// input rate: every parsed input, newest last.
			std::deque<InputStamp> inputs;
			std::size_t            acceptedInRateWindow = 0;

			std::deque<Observation> history;
		};

		ConnState&       stateFor(HE::Net::ConnectionId conn);
		bool             treatAsHard(Kind kind) const;
		const ConnState* findState(HE::Net::ConnectionId conn) const;
		Level            scoreLevel(double score) const;
		void             evaluateWindows(HE::Net::ConnectionId conn, ConnState& st);
		void             trimRateWindow(ConnState& st);
		void             addObservation(HE::Net::ConnectionId conn, ConnState& st,
		                                Observation obs, std::uint32_t netId);
		void             makeReport(HE::Net::ConnectionId conn, ConnState& st, Level level,
		                            const Observation& trigger, std::uint32_t netId);
		void             expireReports();

		Config m_cfg;
		double m_wall = 0.0;
		std::unordered_map<HE::Net::ConnectionId, ConnState> m_conns;
		std::unordered_map<std::uint32_t, float>             m_expected;   // netId → maxDistance

		std::deque<Report> m_reports;
		std::deque<int>    m_pendingReports;
		int                m_nextReportId = 0;
		Stats              m_stats;
	};
} // namespace HE::AntiCheat
