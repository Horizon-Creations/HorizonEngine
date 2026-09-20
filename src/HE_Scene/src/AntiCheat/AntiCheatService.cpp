#include "HorizonScene/AntiCheat/AntiCheatService.h"

#include <Diagnostics/Log.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

using namespace HE::Net;

namespace HE::AntiCheat
{
	const char* levelName(Level level)
	{
		switch (level)
		{
		case Level::Info:      return "Info";
		case Level::Suspect:   return "Suspect";
		case Level::Confirmed: return "Confirmed";
		case Level::Hard:      return "Hard";
		}
		return "?";
	}

	const char* kindName(Kind kind)
	{
		switch (kind)
		{
		case Kind::Malformed:     return "Malformed";
		case Kind::ForeignEntity: return "ForeignEntity";
		case Kind::InputRate:     return "InputRate";
		case Kind::DtBudget:      return "DtBudget";
		case Kind::Displacement:  return "Displacement";
		case Kind::IntegrityMismatch: return "IntegrityMismatch";
		case Kind::Custom:        return "Custom";
		}
		return "?";
	}

	bool isHard(Kind kind)
	{
		return kind == Kind::Malformed || kind == Kind::ForeignEntity;
	}

	namespace
	{
		std::string format(const char* fmt, ...)
		{
			char buf[256];
			va_list args;
			va_start(args, fmt);
			std::vsnprintf(buf, sizeof(buf), fmt, args);
			va_end(args);
			return buf;
		}

		// "DtBudget 3.50 (accepted 1.50 s in 1.00 s); Displacement 4.35 (…)".
		// One line, because that is what a log record and a script reader can
		// carry; the structured list travels alongside in Report::observations.
		std::string joinObservations(const std::vector<Observation>& list)
		{
			std::string out;
			for (const Observation& o : list)
			{
				if (!out.empty()) out += "; ";
				out += kindName(o.kind);
				if (!isHard(o.kind)) out += format(" %.2f", o.weight);
				if (!o.detail.empty())
				{
					out += " (";
					out += o.detail;
					out += ")";
				}
			}
			return out;
		}

		// " [label]" for the log, or nothing. The label is what the game chose to
		// call the player; the service itself knows nothing it would have to
		// redact — no secret, no key, no address ever reaches it.
		std::string labelSuffix(const std::string& label)
		{
			return label.empty() ? std::string() : " [" + label + "]";
		}
	} // namespace

	AntiCheatService::AntiCheatService(Config cfg)
		: m_cfg(cfg)
	{
		// Hold the knobs in ranges where the formulas still mean something. A
		// zero half-life would divide by zero; a window shorter than the
		// evaluation interval would judge on nothing.
		m_cfg.tolerance          = std::max(0.0f, m_cfg.tolerance);
		m_cfg.evalIntervalSec    = std::max(0.1f, m_cfg.evalIntervalSec);
		m_cfg.windowSec          = std::max(m_cfg.evalIntervalSec, m_cfg.windowSec);
		m_cfg.rateWindowSec      = std::max(0.5f, m_cfg.rateWindowSec);
		m_cfg.maxInputsPerSecond = std::max(1.0f, m_cfg.maxInputsPerSecond);
		m_cfg.halfLifeSec        = std::max(0.1f, m_cfg.halfLifeSec);
		m_cfg.suspectThreshold   = std::max(0.0f, m_cfg.suspectThreshold);
		m_cfg.confirmedThreshold = std::max(m_cfg.suspectThreshold, m_cfg.confirmedThreshold);
		m_cfg.reportHistorySec   = std::max(0.0f, m_cfg.reportHistorySec);
		m_cfg.reportTtlSec       = std::max(0.0f, m_cfg.reportTtlSec);
	}

	// ─── Per-connection state ────────────────────────────────────────────────

	AntiCheatService::ConnState& AntiCheatService::stateFor(ConnectionId conn)
	{
		auto it = m_conns.find(conn);
		if (it == m_conns.end())
		{
			it = m_conns.emplace(conn, ConnState{}).first;
			it->second.firstSeen = m_wall;
		}
		return it->second;
	}

	const AntiCheatService::ConnState* AntiCheatService::findState(ConnectionId conn) const
	{
		const auto it = m_conns.find(conn);
		return it == m_conns.end() ? nullptr : &it->second;
	}

	Level AntiCheatService::scoreLevel(double score) const
	{
		if (score >= m_cfg.confirmedThreshold) return Level::Confirmed;
		if (score >= m_cfg.suspectThreshold)   return Level::Suspect;
		return Level::Info;
	}

	void AntiCheatService::trimRateWindow(ConnState& st)
	{
		const double oldest = m_wall - m_cfg.rateWindowSec;
		while (!st.inputs.empty() && st.inputs.front().wall < oldest)
		{
			if (st.inputs.front().accepted && st.acceptedInRateWindow > 0)
				--st.acceptedInRateWindow;
			st.inputs.pop_front();
		}
	}

	// ─── Hooks ───────────────────────────────────────────────────────────────

	bool AntiCheatService::preApply(ConnectionId conn, const GameReplication::InputCommand& cmd)
	{
		ConnState& st = stateFor(conn);

		// Format. The frame parsed, but a NaN in it would land in a position and
		// take the entity out of the simulation for everyone. std::clamp does
		// not catch it — every comparison with NaN is false — so this runs first.
		const bool finite = std::isfinite(cmd.deltaTime) && std::isfinite(cmd.yaw) &&
		                    std::isfinite(cmd.move.x) && std::isfinite(cmd.move.y) &&
		                    std::isfinite(cmd.move.z);
		if (!finite)
		{
			st.inputs.push_back({ m_wall, false });
			++m_stats.rejectedInputs;
			addObservation(conn, st,
			               Observation{ Kind::Malformed, 0.0f, kindName(Kind::Malformed),
			                            format("non-finite value in input #%u", cmd.sequence),
			                            m_wall },
			               0);
			return false;
		}

		// Rate. A hard cap on what gets APPLIED inside the window: a flood must
		// not run the mover ten thousand times a second. The observation for it
		// is made once per evaluation (evaluateWindows), not once per dropped
		// command, so a burst is weighed once rather than thirty times.
		trimRateWindow(st);
		const auto cap = static_cast<std::size_t>(
			std::ceil(m_cfg.maxInputsPerSecond * m_cfg.rateWindowSec));
		if (st.acceptedInRateWindow >= cap)
		{
			st.inputs.push_back({ m_wall, false });
			++m_stats.rejectedInputs;
			return false;
		}
		st.inputs.push_back({ m_wall, true });
		++st.acceptedInRateWindow;

		// dt budget: book the CLAMPED value, because that is what the mover will
		// run with. Booking the raw claim would make a single dt=10 command look
		// like a ten-second speedhack, when the clamp already reduced it to a
		// tenth of a second of movement.
		st.pending.accepted += std::clamp(cmd.deltaTime, 0.0f, GameReplication::kMaxInputDeltaTime);
		return true;
	}

	bool AntiCheatService::postApply(ConnectionId conn, const MoveCheck& c)
	{
		ConnState& st = stateFor(conn);

		const glm::vec3 d          = c.after - c.before;
		const float     horizontal = std::sqrt(d.x * d.x + d.z * d.z);
		const float     vertical   = std::abs(d.y);
		const float     dist       = glm::length(d);

		// A mover that produced NaN is a game bug, not a cheat: undo it so the
		// entity survives, say so, but weigh nothing against the client.
		if (!std::isfinite(dist))
		{
			++m_stats.rolledBack;
			HE_LOG_WARN(AntiCheat, "conn %u: mover produced a non-finite position for entity %u, restored",
			            conn, c.netId);
			return false;
		}

		// The game announced this move (respawn, portal, dash). One shot: the
		// allowance is spent here whether or not it covered what happened.
		if (const auto it = m_expected.find(c.netId); it != m_expected.end())
		{
			const float announced = it->second;
			m_expected.erase(it);
			if (dist <= announced + c.quantStep) return true;
			// Announced, but it went further than announced — judged like any
			// other move below.
		}

		// allowed = maxSpeed · dt · (1 + tol) + quantStep. The quantisation step
		// is added, not multiplied in: a perfectly legal move at the bound would
		// otherwise fail by the width of the wire's rounding.
		const float scale = c.deltaTime * (1.0f + m_cfg.tolerance);
		float       worst = 0.0f;   // distance / allowance, over the checked axes
		std::string detail;
		if (c.limits.maxSpeed > 0.0f)
		{
			const float allowed = c.limits.maxSpeed * scale + c.quantStep;
			if (horizontal > allowed)
			{
				worst  = std::max(worst, horizontal / allowed);
				detail = format("moved %.3f horizontally, allowed %.3f at %.2f u/s over %.4f s",
				                horizontal, allowed, c.limits.maxSpeed, c.deltaTime);
			}
		}
		if (c.limits.maxVerticalSpeed > 0.0f)
		{
			const float allowed = c.limits.maxVerticalSpeed * scale + c.quantStep;
			if (vertical > allowed)
			{
				worst = std::max(worst, vertical / allowed);
				if (!detail.empty()) detail += "; ";
				detail += format("moved %.3f vertically, allowed %.3f at %.2f u/s over %.4f s",
				                 vertical, allowed, c.limits.maxVerticalSpeed, c.deltaTime);
			}
		}
		if (worst <= 0.0f) return true;

		++m_stats.rolledBack;
		addObservation(conn, st,
		               Observation{ Kind::Displacement, worst, kindName(Kind::Displacement),
		                            detail, m_wall },
		               c.netId);
		return false;
	}

	void AntiCheatService::observe(ConnectionId conn, Kind kind, float weight, std::string detail)
	{
		ConnState& st = stateFor(conn);
		addObservation(conn, st, Observation{ kind, weight, kindName(kind), std::move(detail), m_wall }, 0);
	}

	void AntiCheatService::integrityMismatch(ConnectionId conn, std::string detail)
	{
		ConnState& st = stateFor(conn);
		addObservation(conn, st,
		               Observation{ Kind::IntegrityMismatch, m_cfg.integrityWeight,
		                            kindName(Kind::IntegrityMismatch), std::move(detail), m_wall },
		               0);
	}

	bool AntiCheatService::treatAsHard(Kind kind) const
	{
		return isHard(kind) || (kind == Kind::IntegrityMismatch && m_cfg.integrityHard);
	}

	void AntiCheatService::update(float dt)
	{
		if (!(dt > 0.0f) || !std::isfinite(dt)) return;
		m_wall += dt;

		// score *= 0.5^(dt / halfLife): a true halving every halfLife seconds,
		// independent of frame rate.
		const double decay = std::pow(0.5, static_cast<double>(dt) / m_cfg.halfLifeSec);

		for (auto& [conn, st] : m_conns)
		{
			st.score *= decay;

			// Close the frame: what arrived since the last update, against the
			// host time that passed. Then trim the window from the front, keeping
			// it at least windowSec wide — never shorter, or a burst right at the
			// edge would be judged against too little wall time.
			st.pending.wall = dt;
			st.frames.push_back(st.pending);
			st.windowWall     += st.pending.wall;
			st.windowAccepted += st.pending.accepted;
			st.pending = {};
			while (st.frames.size() > 1 &&
			       st.windowWall - st.frames.front().wall >= m_cfg.windowSec)
			{
				st.windowWall     -= st.frames.front().wall;
				st.windowAccepted -= st.frames.front().accepted;
				st.frames.pop_front();
			}

			// The cadence. fmod rather than a subtraction: one long hitch must
			// not queue up several evaluations of the same window.
			st.evalAccum += dt;
			if (st.evalAccum >= m_cfg.evalIntervalSec)
			{
				st.evalAccum = std::fmod(st.evalAccum, m_cfg.evalIntervalSec);
				evaluateWindows(conn, st);
			}

			// The level may only FALL here. Rising is handled where the
			// observation lands, so a report follows its cause immediately.
			const Level now = scoreLevel(st.score);
			if (now < st.level) st.level = now;
		}

		expireReports();
	}

	void AntiCheatService::evaluateWindows(ConnectionId conn, ConnState& st)
	{
		// dt budget (plan §3.3 b). Judged only once the window holds a second:
		// on a fresh connection a single frame with three queued commands would
		// otherwise read as a 3× speedhack.
		if (st.windowWall >= 1.0)
		{
			const double ratio  = st.windowAccepted / st.windowWall;
			const double excess = ratio - 1.0 - m_cfg.tolerance;
			if (excess > 0.0)
			{
				// 10 % over the tolerance = 1 point, 100 % over = 10.
				const auto weight = static_cast<float>(excess * 10.0);
				addObservation(conn, st,
				               Observation{ Kind::DtBudget, weight, kindName(Kind::DtBudget),
				                            format("accepted %.2f s of input in %.2f s wall (ratio %.2f)",
				                                   st.windowAccepted, st.windowWall, ratio),
				                            m_wall },
				               0);
			}
		}

		// Input rate (plan §3.3 a): everything OFFERED, dropped commands
		// included — the flood is the observation, not what survived the cap.
		trimRateWindow(st);
		const double span = std::min<double>(m_cfg.rateWindowSec, m_wall - st.firstSeen);
		if (span >= 1.0 && !st.inputs.empty())
		{
			const double rate = static_cast<double>(st.inputs.size()) / span;
			if (rate > m_cfg.maxInputsPerSecond)
			{
				const auto weight = static_cast<float>((rate / m_cfg.maxInputsPerSecond - 1.0) * 10.0);
				addObservation(conn, st,
				               Observation{ Kind::InputRate, weight, kindName(Kind::InputRate),
				                            format("%.0f inputs/s offered, limit %.0f",
				                                   rate, m_cfg.maxInputsPerSecond),
				                            m_wall },
				               0);
			}
		}
	}

	// ─── Score, levels, reports ──────────────────────────────────────────────

	void AntiCheatService::addObservation(ConnectionId conn, ConnState& st, Observation obs,
	                                      std::uint32_t netId)
	{
		obs.weight = std::clamp(obs.weight, 0.0f, kMaxObservationWeight);
		// Hard is a level, not a number: it does not need the score and must
		// not be something the score could ever have reached on its own.
		const bool hard = treatAsHard(obs.kind);
		if (hard) obs.weight = 0.0f;

		++m_stats.observations;
		st.history.push_back(obs);
		const double oldest = m_wall - m_cfg.reportHistorySec;
		while (st.history.size() > 1 && st.history.front().atWall < oldest)
			st.history.pop_front();

		if (hard)
		{
			HE_LOG_INFO(AntiCheat, "conn %u%s: %s — %s", conn, labelSuffix(st.label).c_str(),
			            kindName(obs.kind), obs.detail.c_str());
			st.hard = true;
			makeReport(conn, st, Level::Hard, obs, netId);
			return;
		}

		st.score += obs.weight;

		// Every observation is visible at Debug; once a connection is Suspect,
		// each further one is worth a line at the default verbosity (plan §3.7).
		if (st.level >= Level::Suspect)
		{
			HE_LOG_INFO(AntiCheat, "conn %u%s: %s w=%.2f score=%.2f — %s", conn,
			            labelSuffix(st.label).c_str(), kindName(obs.kind), obs.weight,
			            st.score, obs.detail.c_str());
		}
		else
		{
			HE_LOG_DEBUG(AntiCheat, "conn %u%s: %s w=%.2f score=%.2f — %s", conn,
			             labelSuffix(st.label).c_str(), kindName(obs.kind), obs.weight,
			             st.score, obs.detail.c_str());
		}

		const Level now = scoreLevel(st.score);
		if (now > st.level)
		{
			st.level = now;
			makeReport(conn, st, now, obs, netId);
		}
	}

	void AntiCheatService::makeReport(ConnectionId conn, ConnState& st, Level level,
	                                  const Observation& trigger, std::uint32_t netId)
	{
		Report r;
		r.id      = ++m_nextReportId;
		r.conn    = conn;
		r.level   = level;
		r.score   = static_cast<float>(st.score);
		r.trigger = trigger.kind;
		r.rule    = trigger.rule;
		r.netId   = netId;
		r.label   = st.label;
		r.atWall  = m_wall;
		const double oldest = m_wall - m_cfg.reportHistorySec;
		for (const Observation& o : st.history)
		{
			if (o.atWall >= oldest) r.observations.push_back(o);
		}
		r.detail = joinObservations(r.observations);

		HE_LOG_WARN(AntiCheat, "report #%d: conn %u%s is %s (score %.1f, trigger %s): %s", r.id,
		            conn, labelSuffix(st.label).c_str(), levelName(level), r.score,
		            kindName(trigger.kind), r.detail.c_str());

		m_pendingReports.push_back(r.id);
		m_reports.push_back(std::move(r));
		++m_stats.reports;

		// Bounded: a long session with a persistent offender must not grow
		// through this table. Evicted tickets read as unknown, and are taken out
		// of the pending queue so the host never receives an id it cannot read.
		while (m_reports.size() > kMaxReports)
		{
			const int gone = m_reports.front().id;
			m_reports.pop_front();
			m_pendingReports.erase(
				std::remove(m_pendingReports.begin(), m_pendingReports.end(), gone),
				m_pendingReports.end());
		}
	}

	void AntiCheatService::expireReports()
	{
		while (!m_reports.empty() && m_wall - m_reports.front().atWall > m_cfg.reportTtlSec)
		{
			const int gone = m_reports.front().id;
			m_reports.pop_front();
			m_pendingReports.erase(
				std::remove(m_pendingReports.begin(), m_pendingReports.end(), gone),
				m_pendingReports.end());
		}
	}

	// ─── Game-facing ─────────────────────────────────────────────────────────

	void AntiCheatService::expectDisplacement(std::uint32_t netId, float maxDistance)
	{
		if (netId == 0 || !(maxDistance >= 0.0f)) return;
		m_expected[netId] = maxDistance;
	}

	void AntiCheatService::report(ConnectionId conn, const std::string& rule, float weight,
	                              const std::string& detail)
	{
		ConnState& st = stateFor(conn);
		std::string text = rule;
		if (!detail.empty())
		{
			text += ": ";
			text += detail;
		}
		addObservation(conn, st, Observation{ Kind::Custom, weight, rule, std::move(text), m_wall }, 0);
	}

	void AntiCheatService::setPlayerLabel(ConnectionId conn, std::string label)
	{
		stateFor(conn).label = std::move(label);
	}

	void AntiCheatService::forgetConnection(ConnectionId conn)
	{
		m_conns.erase(conn);
	}

	// ─── Readers ─────────────────────────────────────────────────────────────

	float AntiCheatService::score(ConnectionId conn) const
	{
		const ConnState* st = findState(conn);
		return st ? static_cast<float>(st->score) : 0.0f;
	}

	Level AntiCheatService::level(ConnectionId conn) const
	{
		const ConnState* st = findState(conn);
		if (!st) return Level::Info;
		return st->hard ? Level::Hard : st->level;
	}

	std::size_t AntiCheatService::observationCount(ConnectionId conn) const
	{
		const ConnState* st = findState(conn);
		return st ? st->history.size() : 0;
	}

	std::size_t AntiCheatService::observationCount(ConnectionId conn, Kind kind) const
	{
		const ConnState* st = findState(conn);
		if (!st) return 0;
		return static_cast<std::size_t>(std::count_if(
			st->history.begin(), st->history.end(),
			[kind](const Observation& o) { return o.kind == kind; }));
	}

	bool AntiCheatService::takeReport(int& id)
	{
		if (m_pendingReports.empty()) return false;
		id = m_pendingReports.front();
		m_pendingReports.pop_front();
		return true;
	}

	const Report* AntiCheatService::findReport(int id) const
	{
		for (const Report& r : m_reports)
		{
			if (r.id == id) return &r;
		}
		return nullptr;
	}
} // namespace HE::AntiCheat
