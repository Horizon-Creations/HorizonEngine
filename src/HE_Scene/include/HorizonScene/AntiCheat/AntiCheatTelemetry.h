#pragma once

// ─── Anti-Cheat — the telemetry sink (plan §3.7) ─────────────────────────────
// AntiCheatHost queues the reports whose responses said Telemetry; this is
// where they leave the machine. Reports are turned into JSON on the frame
// thread, buffered, and POSTed in one batch per upload to the project's
// `anticheat.telemetryUrl` — every kIntervalSec, or at once when a batch holds
// a report at Confirmed or above. Default URL empty = this object does nothing
// and the host's queue stays where it was, for whoever else wants to drain it.
//
// Two rules shape everything below:
//
//  1. ASYNCHRONOUS. An HTTPS call blocks for as long as the network takes; on
//     the frame thread that is a host frozen for seconds, and every client
//     seeing a "hitch" the anti-cheat then has to forgive. The upload runs in a
//     std::async worker like the directory calls in CollabController, one at a
//     time, and the frame collects the result with wait_for(0s). The worker
//     captures VALUES only — the URL, the body, the post function — never the
//     host, never a Report pointer (the service evicts those).
//
//  2. REDACTED BY CONSTRUCTION, the same rules as HorizonNet's NetLog.h. The
//     payload is built from the Report's own fields and nothing else: no
//     NetSession, no SecureTransport, no address is ever read. The join secret
//     and the key material cannot reach it because nothing here can name them;
//     the player's name only travels if the game set it (setPlayerLabel), and
//     the session id is shortened exactly as logSessionId shortens it for the
//     log — enough to correlate, not enough to be an invitation. The URL is
//     logged without its query string, because a `?token=` in it is the
//     directory's management token rule (NetLog.h, rule 3).
//
// The post function is a std::function so a test can stand in for the network
// and read what would have been sent; the default is HE::Net::httpsPostJson.

#include "HorizonScene/AntiCheat/AntiCheatService.h"

#include <Net/HttpsClient.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <string>
#include <vector>

// No HE_API: HorizonScene exports every symbol (see GameReplication.h).
namespace HE::AntiCheat
{
	class AntiCheatTelemetry
	{
	public:
		using Clock = std::chrono::steady_clock;
		// What the worker calls. `body` is the JSON batch.
		using Post = std::function<HE::Net::HttpsResponse(const std::string& url,
		                                                   const std::string& body)>;

		static constexpr double kIntervalSec = 30.0;
		static constexpr int    kTimeoutMs   = 5000;   // short: a future's destructor waits for it
		static constexpr std::size_t kMaxBuffered = 256;   // JSON reports held between uploads

		AntiCheatTelemetry();
		// Waits for an upload still in flight (at most kTimeoutMs).
		~AntiCheatTelemetry();
		AntiCheatTelemetry(const AntiCheatTelemetry&)            = delete;
		AntiCheatTelemetry& operator=(const AntiCheatTelemetry&) = delete;

		// Empty url = off. `sessionId` is shortened before it is stored; the
		// full one is never kept here. `project` is a label for the endpoint
		// (the game's title), so one collector can serve several games.
		void configure(const std::string& url, const std::string& sessionId,
		               const std::string& project);
		bool enabled() const { return !m_url.empty(); }
		// Tests: stand in for the network, and shorten the cadence. A post
		// function of its own does not need the platform's HTTPS backend.
		void setPost(Post post) { m_post = std::move(post); m_customPost = true; }
		void setIntervalSec(double sec) { m_intervalSec = sec; }

		// The frame. Takes every report out of `queue` and buffers it (when
		// enabled; otherwise the queue is left alone), collects a finished
		// upload, and starts the next one when it is due. Call it BEFORE any
		// early return in the frame-end function — a result that is never
		// collected is a thread that is never joined until the destructor.
		void pump(std::deque<Report>& queue);
		// Session end: send what is buffered if nothing is in flight; otherwise
		// it is dropped with a log line. The upload in flight is NOT waited
		// for — the next pump, or the destructor, collects it.
		void flushOnDetach();

		bool uploadInFlight() const { return m_inFlight.valid(); }

		// One report as one JSON object, redacted. Public so a test can check
		// the shape without a network, and so the host's own log can carry the
		// same fields. `sessionShort` is the already-shortened id.
		static std::string redact(const Report& r, const std::string& sessionShort);
		// The NetLog.h rule, restated here because that header is private to
		// HorizonNet: six characters and an ellipsis, short ids passed through.
		static std::string shortSessionId(const std::string& id);
		// Scheme, host and path only — never a query string.
		static std::string loggableUrl(const std::string& url);

		struct Stats
		{
			std::uint32_t buffered  = 0;   // reports taken from the host's queue
			std::uint32_t dropped   = 0;   // buffer overflow or detach with an upload in flight
			std::uint32_t uploads   = 0;   // POSTs started
			std::uint32_t succeeded = 0;   // HTTP 2xx
			std::uint32_t failed    = 0;   // transport error or non-2xx
			std::uint32_t reportsSent = 0; // reports inside succeeded uploads
		};
		const Stats& stats() const { return m_stats; }

	private:
		struct Outcome
		{
			HE::Net::HttpsResponse response;
			std::uint32_t          reports = 0;
		};

		void collect();
		void start();
		std::string batchBody() const;

		std::string m_url;
		std::string m_session;    // shortened
		std::string m_project;
		Post        m_post;
		bool        m_customPost  = false;
		double      m_intervalSec = kIntervalSec;

		std::vector<std::string> m_buffer;      // redacted JSON objects
		bool                     m_urgent = false;   // a >= Confirmed report is buffered
		Clock::time_point        m_lastUpload;
		std::future<Outcome>     m_inFlight;
		Stats                    m_stats;
	};
} // namespace HE::AntiCheat
