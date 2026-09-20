#include "HorizonScene/AntiCheat/AntiCheatTelemetry.h"

#include <Diagnostics/Log.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

namespace HE::AntiCheat
{
	namespace
	{
		std::int64_t unixMillisNow()
		{
			using namespace std::chrono;
			return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
		}
	} // namespace

	AntiCheatTelemetry::AntiCheatTelemetry()
	    : m_post([](const std::string& url, const std::string& body) {
	          return HE::Net::httpsPostJson(url, body, kTimeoutMs);
	      })
	    , m_lastUpload(Clock::now())
	{
	}

	// The future's destructor joins the worker; that is what kTimeoutMs bounds.
	AntiCheatTelemetry::~AntiCheatTelemetry() = default;

	// ─── Redaction ───────────────────────────────────────────────────────────

	std::string AntiCheatTelemetry::shortSessionId(const std::string& id)
	{
		if (id.size() <= 6) return id;
		return id.substr(0, 6) + "…";
	}

	std::string AntiCheatTelemetry::loggableUrl(const std::string& url)
	{
		const std::size_t q = url.find_first_of("?#");
		return q == std::string::npos ? url : url.substr(0, q);
	}

	std::string AntiCheatTelemetry::redact(const Report& r, const std::string& sessionShort)
	{
		// Only what the Report carries. `label` is the game's choice
		// (setPlayerLabel) and goes out only when the game made it; `conn` is
		// the session-local connection number, not an address.
		nlohmann::json j;
		j["id"]      = r.id;
		j["level"]   = levelName(r.level);
		j["score"]   = r.score;
		j["trigger"] = kindName(r.trigger);
		j["rule"]    = r.rule;
		j["conn"]    = r.conn;
		if (r.netId != 0)     j["entity"]  = r.netId;
		if (!r.label.empty()) j["label"]   = r.label;
		if (!sessionShort.empty()) j["session"] = sessionShort;
		j["detail"]  = r.detail;
		j["atWall"]  = r.atWall;
		nlohmann::json obs = nlohmann::json::array();
		for (const Observation& o : r.observations)
		{
			nlohmann::json e;
			e["kind"]   = kindName(o.kind);
			e["rule"]   = o.rule;
			e["weight"] = o.weight;
			e["detail"] = o.detail;
			e["atWall"] = o.atWall;
			obs.push_back(std::move(e));
		}
		j["observations"] = std::move(obs);
		j["unixMillis"]   = unixMillisNow();
		return j.dump();
	}

	// ─── Configuration ───────────────────────────────────────────────────────

	void AntiCheatTelemetry::configure(const std::string& url, const std::string& sessionId,
	                                   const std::string& project)
	{
		m_session = shortSessionId(sessionId);
		m_project = project;
		if (url.empty())
		{
			if (!m_url.empty()) HE_LOG_INFO(AntiCheat, "%s", "telemetry off (no URL)");
			m_url.clear();
			return;
		}
		if (!HE::Net::httpsAvailable())
		{
			// Said once here rather than as a failing future every interval.
			HE_LOG_WARN(AntiCheat, "telemetry URL set but this build has no HTTPS backend (%s); "
			                       "reports stay local", HE::Net::httpsBackendName());
			m_url.clear();
			return;
		}
		m_url = url;
		HE_LOG_INFO(AntiCheat, "telemetry: POST to %s every %.0f s, at once from Confirmed",
		            loggableUrl(url).c_str(), m_intervalSec);
	}

	// ─── The frame ───────────────────────────────────────────────────────────

	void AntiCheatTelemetry::pump(std::deque<Report>& queue)
	{
		// Collect first, and even when off: a URL removed while an upload was
		// out must still have its result joined.
		collect();
		if (!enabled()) return;

		while (!queue.empty())
		{
			const Report& r = queue.front();
			if (r.level >= Level::Confirmed) m_urgent = true;
			m_buffer.push_back(redact(r, m_session));
			++m_stats.buffered;
			queue.pop_front();
		}
		if (m_buffer.size() > kMaxBuffered)
		{
			// The oldest go, as in the host's own queue: a long session against
			// a dead endpoint must not grow through this buffer.
			const std::size_t excess = m_buffer.size() - kMaxBuffered;
			m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(excess));
			m_stats.dropped += static_cast<std::uint32_t>(excess);
		}

		if (m_buffer.empty() || m_inFlight.valid()) return;
		const double since =
			std::chrono::duration<double>(Clock::now() - m_lastUpload).count();
		if (m_urgent || since >= m_intervalSec) start();
	}

	void AntiCheatTelemetry::flushOnDetach()
	{
		collect();
		if (m_buffer.empty()) return;
		if (m_inFlight.valid())
		{
			HE_LOG_INFO(AntiCheat, "telemetry: %zu report(s) dropped at session end, an upload is "
			                       "still out", m_buffer.size());
			m_stats.dropped += static_cast<std::uint32_t>(m_buffer.size());
			m_buffer.clear();
			m_urgent = false;
			return;
		}
		start();
	}

	std::string AntiCheatTelemetry::batchBody() const
	{
		// Assembled from already-serialised objects, so the batch never holds
		// a Report and the redaction happened once, at enqueue time.
		std::string body = "{\"schema\":1";
		if (!m_project.empty()) body += ",\"project\":" + nlohmann::json(m_project).dump();
		if (!m_session.empty()) body += ",\"session\":" + nlohmann::json(m_session).dump();
		body += ",\"sentAt\":" + std::to_string(unixMillisNow());
		body += ",\"reports\":[";
		for (std::size_t i = 0; i < m_buffer.size(); ++i)
		{
			if (i) body += ',';
			body += m_buffer[i];
		}
		body += "]}";
		return body;
	}

	void AntiCheatTelemetry::start()
	{
		const std::string   url   = m_url;
		const std::string   body  = batchBody();
		const std::uint32_t count = static_cast<std::uint32_t>(m_buffer.size());
		Post                post  = m_post;
		m_buffer.clear();
		m_urgent     = false;
		m_lastUpload = Clock::now();
		++m_stats.uploads;
		HE_LOG_DEBUG(AntiCheat, "telemetry: uploading %u report(s), %zu bytes", count, body.size());

		// Values only: the worker must outlive nothing but itself.
		m_inFlight = std::async(std::launch::async, [url, body, count, post]() {
			Outcome o;
			o.reports  = count;
			o.response = post ? post(url, body) : HE::Net::HttpsResponse{};
			return o;
		});
	}

	void AntiCheatTelemetry::collect()
	{
		using namespace std::chrono_literals;
		if (!m_inFlight.valid() || m_inFlight.wait_for(0s) != std::future_status::ready) return;
		const Outcome o = m_inFlight.get();
		const bool ok = o.response.ok && o.response.statusCode >= 200 && o.response.statusCode < 300;
		if (ok)
		{
			++m_stats.succeeded;
			m_stats.reportsSent += o.reports;
			HE_LOG_DEBUG(AntiCheat, "telemetry: %u report(s) accepted (HTTP %d)", o.reports,
			             o.response.statusCode);
		}
		else
		{
			// The batch is gone: telemetry is best effort, and a retry queue
			// against a dead endpoint is how memory grows. The error text is the
			// transport's, never the response body.
			++m_stats.failed;
			if (o.response.ok)
				HE_LOG_WARN(AntiCheat, "telemetry: endpoint answered HTTP %d, %u report(s) lost",
				            o.response.statusCode, o.reports);
			else
				HE_LOG_WARN(AntiCheat, "telemetry: upload failed (%s), %u report(s) lost",
				            o.response.error.c_str(), o.reports);
		}
	}
} // namespace HE::AntiCheat
