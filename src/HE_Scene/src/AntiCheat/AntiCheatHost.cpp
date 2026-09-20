#include "HorizonScene/AntiCheat/AntiCheatHost.h"

#include "HorizonScene/GameReplication.h"

#include <Diagnostics/Log.h>
#include <Events/EventBus.h>
#include <Net/NetSession.h>
#include <Project/ProjectSettings.h>

#include <algorithm>

using namespace HE::Net;

namespace HE::AntiCheat
{
	// One number from the settings file to here: the bits must agree, or a
	// policy saved as ["kick"] would flag instead.
	static_assert(Log       == ProjectAntiCheatSettings::Log,       "Response bits diverged");
	static_assert(Event     == ProjectAntiCheatSettings::Event,     "Response bits diverged");
	static_assert(Telemetry == ProjectAntiCheatSettings::Telemetry, "Response bits diverged");
	static_assert(Flag      == ProjectAntiCheatSettings::Flag,      "Response bits diverged");
	static_assert(Kick      == ProjectAntiCheatSettings::Kick,      "Response bits diverged");
	static_assert(Ban       == ProjectAntiCheatSettings::Ban,       "Response bits diverged");

	std::uint32_t Policy::forLevel(Level level) const
	{
		switch (level)
		{
		case Level::Suspect:   return suspect | Log;
		case Level::Confirmed: return confirmed | Log;
		case Level::Hard:      return hard | Log;
		case Level::Info:      break;
		}
		return Log;
	}

	namespace
	{
		// "Log+Event+Kick" for the log line, so a reader of the log sees what
		// the host decided without decoding a bit mask.
		std::string responseNames(std::uint32_t mask)
		{
			std::string out;
			for (int bit = 0; bit < kAntiCheatResponseCount; ++bit)
			{
				if (!(mask & (1u << bit))) continue;
				if (!out.empty()) out += "+";
				out += kAntiCheatResponseNames[bit];
			}
			return out.empty() ? std::string("none") : out;
		}
	} // namespace

	AntiCheatHost::AntiCheatHost()  = default;
	AntiCheatHost::~AntiCheatHost() = default;

	// ─── Wiring ──────────────────────────────────────────────────────────────

	void AntiCheatHost::attach(AntiCheatService* service, GameReplication* replication)
	{
		detach();
		m_service     = service;
		m_replication = replication;
	}

	void AntiCheatHost::detach()
	{
		m_service     = nullptr;
		m_replication = nullptr;
		m_pending.clear();
		m_kicking.clear();
		m_flagged.clear();
		m_banned.clear();
		m_local.clear();
		m_localReason.clear();
		m_telemetry.clear();
	}

	Config AntiCheatHost::configFrom(const ProjectAntiCheatSettings& s)
	{
		Config c;
		c.tolerance          = s.tolerance;
		c.windowSec          = s.windowSec;
		c.maxInputsPerSecond = static_cast<float>(s.maxInputsPerSecond);
		c.halfLifeSec        = s.scoreHalfLifeSec;
		c.suspectThreshold   = s.scoreSuspect;
		c.confirmedThreshold = s.scoreConfirmed;
		return c;
	}

	Policy AntiCheatHost::policyFrom(const ProjectAntiCheatSettings& s)
	{
		Policy p;
		p.suspect   = s.policySuspect;
		p.confirmed = s.policyConfirmed;
		p.hard      = s.policyHard;
		return p;
	}

	// ─── The frame ───────────────────────────────────────────────────────────

	int AntiCheatHost::pump()
	{
		int fired = 0;

		// Host: new reports since the last frame. Each is decided NOW (the
		// policy for its level), fired NOW (if the policy says so), executed
		// at flush. The decision is stored on the pending entry so a handler's
		// respond() has something to replace.
		if (m_service)
		{
			int id = 0;
			while (m_service->takeReport(id))
			{
				const Report* r = m_service->findReport(id);
				if (!r) continue;   // evicted between make and take; nothing to say
				++m_stats.reportsTaken;

				Pending p;
				p.id        = r->id;
				p.conn      = r->conn;
				p.level     = r->level;
				p.responses = m_policy.forLevel(r->level);
				p.rule      = r->rule;
				m_pending.push_back(p);

				if (p.responses & Event)
				{
					fire(r->id, *r, /*local=*/false);
					++fired;
				}
			}
		}

		// Client: notices from the host. A local ticket, then the same event.
		if (m_replication)
		{
			GameReplication::AntiCheatNotice n;
			while (m_replication->takeAntiCheatNotice(n))
			{
				++m_stats.noticesTaken;
				const int id = makeLocalReport(static_cast<Level>(std::clamp(n.level, 0, 3)),
				                               n.reasonCode, n.rule);
				HE_LOG_WARN(AntiCheat, "notice from host: %s, rule '%s', reason %d (ticket #%d)",
				            levelName(static_cast<Level>(std::clamp(n.level, 0, 3))),
				            n.rule.c_str(), n.reasonCode, id);
				if (const Report* r = findReport(id))
				{
					fire(id, *r, /*local=*/true);
					++fired;
				}
			}
		}
		return fired;
	}

	void AntiCheatHost::fire(int reportId, const Report& r, bool local)
	{
		++m_stats.eventsFired;
		if (m_sink) m_sink(reportId, r);
		if (m_bus)
		{
			CheatDetected ev;
			ev.reportId = reportId;
			ev.level    = r.level;
			ev.player   = r.conn;
			ev.entity   = r.netId;
			ev.score    = r.score;
			ev.rule     = r.rule;
			ev.detail   = r.detail;
			ev.label    = r.label;
			ev.local    = local;
			m_bus->publish(ev);
		}
	}

	void AntiCheatHost::flush()
	{
		// Disconnects whose notice went out on the previous flush. First, so a
		// kick decided this frame does not also close its link this frame.
		if (!m_kicking.empty())
		{
			std::vector<KickInFlight> kicks;
			kicks.swap(m_kicking);
			for (const KickInFlight& k : kicks)
			{
				if (m_replication)
				{
					if (NetSession* net = m_replication->session()) net->disconnect(k.conn);
					m_replication->dropConnection(k.conn);
				}
				else if (m_service)
				{
					m_service->forgetConnection(k.conn);
				}
				++m_stats.kicked;
				HE_LOG_WARN(AntiCheat, "conn %u %s: dropped", k.conn, k.ban ? "banned" : "kicked");
			}
		}

		if (m_pending.empty()) return;
		std::vector<Pending> pending;
		pending.swap(m_pending);
		for (const Pending& p : pending)
			execute(p, m_service ? m_service->findReport(p.id) : nullptr);
	}

	void AntiCheatHost::execute(const Pending& p, const Report* r)
	{
		std::uint32_t responses = p.responses | Log;
		// Preview: the author's own window. Reports are seen, nobody is dropped,
		// nothing leaves the machine.
		if (m_preview) responses &= ~(Kick | Ban | Telemetry);

		if (p.id != 0)
			HE_LOG_INFO(AntiCheat, "report #%d: conn %u %s → %s", p.id, p.conn,
			            levelName(p.level), responseNames(responses).c_str());
		else
			HE_LOG_INFO(AntiCheat, "explicit kick: conn %u (reason %d) → %s", p.conn, p.reason,
			            responseNames(responses).c_str());

		if (responses & Flag)
		{
			if (m_flagged.insert(p.conn).second) ++m_stats.flagged;
		}
		if ((responses & Telemetry) && r)
		{
			m_telemetry.push_back(*r);
			++m_stats.telemetryQueued;
			while (m_telemetry.size() > kMaxTelemetryQueue) m_telemetry.pop_front();
		}
		if (responses & (Kick | Ban))
		{
			const bool ban = (responses & Ban) != 0;
			// One notice per connection per frame: a Hard report and a Confirmed
			// one on the same frame must not drop the link twice.
			const bool already = std::any_of(m_kicking.begin(), m_kicking.end(),
			                                 [&](const KickInFlight& k) { return k.conn == p.conn; });
			if (!already)
			{
				sendNotice(p.conn, p.level, p.reason, p.rule);
				KickInFlight k;
				k.conn  = p.conn;
				k.ban   = ban;
				k.label = r ? r->label : std::string();
				m_kicking.push_back(std::move(k));
			}
			if (ban && r && !r->label.empty())
			{
				if (m_banned.insert(r->label).second) ++m_stats.banned;
			}
			else if (ban)
			{
				// Without a label there is nothing to refuse later: the ban is a
				// kick, and the log says so rather than implying more.
				HE_LOG_WARN(AntiCheat, "report #%d: ban requested for conn %u without a label — "
				                       "a kick is all this session can do", p.id, p.conn);
			}
		}
	}

	void AntiCheatHost::sendNotice(ConnectionId conn, Level level, int reason,
	                               const std::string& rule)
	{
		if (!m_replication) return;
		GameReplication::AntiCheatNotice n;
		n.level      = static_cast<int>(level);
		n.reasonCode = reason;
		n.rule       = rule;
		m_replication->sendAntiCheatNotice(conn, n);
		++m_stats.noticesSent;
	}

	// ─── The rows ────────────────────────────────────────────────────────────

	bool AntiCheatHost::check(const std::string& rule, float value, ConnectionId player)
	{
		// Step 5 brings the rule table. Until then — and for a rule the table
		// does not know, after — the answer is "passes": a check the engine
		// cannot make must never block the game, which is the opposite default
		// from every other row that lacks its service.
		(void)value; (void)player;
		if (!m_service) return true;
		HE_LOG_DEBUG(AntiCheat, "check '%s': no rule table in this build, passes", rule.c_str());
		return true;
	}

	void AntiCheatHost::expectDisplacement(std::uint32_t entity, float maxDistance)
	{
		if (m_service) m_service->expectDisplacement(entity, maxDistance);
	}

	void AntiCheatHost::report(ConnectionId player, const std::string& rule, float weight,
	                           const std::string& detail)
	{
		if (!m_service || player == kInvalidConnection || rule.empty()) return;
		m_service->report(player, rule, weight, detail);
	}

	void AntiCheatHost::setPlayerLabel(ConnectionId player, const std::string& label)
	{
		if (m_service && player != kInvalidConnection) m_service->setPlayerLabel(player, label);
	}

	bool AntiCheatHost::respond(int reportId, std::uint32_t responses)
	{
		for (Pending& p : m_pending)
		{
			if (p.id != reportId) continue;
			p.responses = (responses & AllResponses) | Log;
			++m_stats.responded;
			HE_LOG_INFO(AntiCheat, "report #%d: handler set responses to %s", reportId,
			            responseNames(p.responses).c_str());
			return true;
		}
		HE_LOG_WARN(AntiCheat, "respond(#%d): no such pending report — a response only counts in "
		                       "the frame the event fired", reportId);
		return false;
	}

	void AntiCheatHost::kick(ConnectionId player, int reasonCode)
	{
		if (!m_service || player == kInvalidConnection) return;
		if (m_preview)
		{
			HE_LOG_INFO(AntiCheat, "kick(conn %u, reason %d) ignored in preview", player, reasonCode);
			return;
		}
		// Same path as a policy kick, with the game's reason and no rule: a
		// pending entry the flush executes, so an explicit kick from inside a
		// handler is also a frame-end kick.
		Pending p;
		p.conn      = player;
		p.level     = m_service->level(player);
		p.responses = Log | Kick;
		p.reason    = reasonCode;
		m_pending.push_back(std::move(p));
	}

	// ─── Readers ─────────────────────────────────────────────────────────────

	const Report* AntiCheatHost::findReport(int reportId) const
	{
		if (m_service)
			if (const Report* r = m_service->findReport(reportId)) return r;
		for (const Report& r : m_local)
			if (r.id == reportId) return &r;
		return nullptr;
	}

	int AntiCheatHost::reportLevel(int reportId) const
	{
		const Report* r = findReport(reportId);
		return r ? static_cast<int>(r->level) : 0;
	}

	std::string AntiCheatHost::reportRule(int reportId) const
	{
		const Report* r = findReport(reportId);
		return r ? r->rule : std::string();
	}

	int AntiCheatHost::reportPlayer(int reportId) const
	{
		const Report* r = findReport(reportId);
		return r ? static_cast<int>(r->conn) : 0;
	}

	int AntiCheatHost::reportEntity(int reportId) const
	{
		const Report* r = findReport(reportId);
		return r ? static_cast<int>(r->netId) : 0;
	}

	float AntiCheatHost::reportScore(int reportId) const
	{
		const Report* r = findReport(reportId);
		return r ? r->score : 0.0f;
	}

	std::string AntiCheatHost::reportDetail(int reportId) const
	{
		const Report* r = findReport(reportId);
		return r ? r->detail : std::string();
	}

	int AntiCheatHost::reportReason(int reportId) const
	{
		const auto it = m_localReason.find(reportId);
		return it == m_localReason.end() ? kReasonPolicy : it->second;
	}

	float AntiCheatHost::playerScore(ConnectionId player) const
	{
		return m_service ? m_service->score(player) : 0.0f;
	}

	bool AntiCheatHost::isFlagged(ConnectionId player) const
	{
		return m_flagged.count(player) != 0;
	}

	bool AntiCheatHost::isBanned(const std::string& label) const
	{
		return !label.empty() && m_banned.count(label) != 0;
	}

	// ─── Client tickets ──────────────────────────────────────────────────────

	int AntiCheatHost::makeLocalReport(Level level, int reason, const std::string& rule)
	{
		Report r;
		r.id     = ++m_nextLocalId;
		r.conn   = kInvalidConnection;   // "you"
		r.level  = level;
		r.score  = 0.0f;                 // the client is told no score
		r.rule   = rule;
		r.detail = rule;                 // and no observation list
		m_local.push_back(std::move(r));
		m_localReason[m_nextLocalId] = reason;
		while (m_local.size() > kMaxLocalReports)
		{
			m_localReason.erase(m_local.front().id);
			m_local.pop_front();
		}
		return m_nextLocalId;
	}

	// ─── Telemetry hand-off ──────────────────────────────────────────────────

	bool AntiCheatHost::takeTelemetry(Report& out)
	{
		if (m_telemetry.empty()) return false;
		out = std::move(m_telemetry.front());
		m_telemetry.pop_front();
		return true;
	}
} // namespace HE::AntiCheat
