#include "SourceControl/GitHubOAuth.h"

#include "ScLog.h"

#include <Net/HttpsClient.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace HE::Sc {

using nlohmann::json;

namespace {

// RFC 8628's grant type, spelled out by GitHub's docs.
constexpr const char* kDeviceGrantType = "urn:ietf:params:oauth:grant-type:device_code";

// A run of failed round trips in a row before the sign-in gives up. One dropped
// request on a flaky network must not cost the user a code they already typed.
constexpr int kMaxTransportFailuresInARow = 3;

void wipe(std::string& s)
{
	std::fill(s.begin(), s.end(), '\0');
	s.clear();
}

// A string field, or "" — json::value() throws when the key exists with
// another type.
std::string jsonString(const json& o, const char* key)
{
	const auto it = o.find(key);
	return (it != o.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

int jsonInt(const json& o, const char* key, int fallback)
{
	const auto it = o.find(key);
	return (it != o.end() && it->is_number_integer()) ? it->get<int>() : fallback;
}

// GitHub's own words for an OAuth error, when it sent any.
std::string oauthDetail(const json& j)
{
	if (!j.is_object()) return {};
	std::string d = jsonString(j, "error_description");
	return d.empty() ? jsonString(j, "error") : d;
}

// The error codes both endpoints share, in the user's language. Built only from
// the response — it holds no token, so quoting GitHub is safe.
std::string describeOAuthError(const std::string& code, const json& j)
{
	if (code == "device_flow_disabled")
		return "GitHub has the device flow switched off for this app — enable "
		       "\"Device Flow\" in the OAuth App's settings on GitHub.";
	if (code == "incorrect_client_credentials")
		return "GitHub does not know this app's client id — check " +
		       std::string(GitHubOAuth::kClientIdEnvVar) + " or the built-in id.";
	if (code == "incorrect_device_code")
		return "GitHub did not recognise the sign-in code — start the sign-in again.";
	if (code == "unsupported_grant_type")
		return "GitHub rejected the sign-in request as malformed (unsupported grant type).";
	if (code == "expired_token")
		return "The sign-in code expired before it was confirmed — start the sign-in again.";
	if (code == "access_denied")
		return "The sign-in was declined on GitHub.";

	const std::string detail = oauthDetail(j);
	return detail.empty() ? "GitHub refused the sign-in (" + code + ")."
	                      : "GitHub refused the sign-in: " + detail;
}

OAuthHttpResponse defaultPost(const std::string& url, const std::string& formBody)
{
	const std::vector<std::string> headers = {
		// Without it both endpoints answer form-encoded, not JSON.
		"Accept: application/json",
		"Content-Type: application/x-www-form-urlencoded",
		// GitHub rejects requests without a User-Agent outright, and none of the
		// three HTTPS backends sets a default one.
		"User-Agent: HorizonEngine-Editor",
	};
	HE::Net::HttpsResponse r = HE::Net::httpsRequest(url, "POST", headers, formBody, 15000);
	OAuthHttpResponse out;
	out.ok         = r.ok;
	out.statusCode = r.statusCode;
	out.body       = std::move(r.body);
	out.error      = std::move(r.error);
	return out;
}

OAuthHttpResponse post(const GitHubOAuth::Transport& t, const std::string& url,
                       const std::string& formBody)
{
	return t.post ? t.post(url, formBody) : defaultPost(url, formBody);
}

std::string transportError(const OAuthHttpResponse& r)
{
	return r.error.empty() ? "could not reach github.com"
	                       : "could not reach GitHub: " + r.error;
}

} // namespace

// ─── Cancellation ────────────────────────────────────────────────────────────

void OAuthCancel::cancel()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_cancelled.store(true, std::memory_order_release);
	}
	m_cv.notify_all();
}

bool OAuthCancel::waitFor(int ms)
{
	std::unique_lock<std::mutex> lock(m_mutex);
	const bool stopped = m_cv.wait_for(lock, std::chrono::milliseconds(std::max(0, ms)),
	                                   [this] { return cancelled(); });
	return !stopped;
}

void OAuthCancel::reset()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_cancelled.store(false, std::memory_order_release);
}

// ─── Client id and encoding ──────────────────────────────────────────────────

std::string GitHubOAuth::clientId()
{
	// Read from the process environment on Windows rather than through getenv,
	// which would see the CRT copy of whichever module the caller was linked
	// with (same reasoning as HE_NET_LOOPBACK_ONLY in HorizonNet).
#ifdef _WIN32
	char v[256] = {};
	const DWORD n = ::GetEnvironmentVariableA(kClientIdEnvVar, v, sizeof(v));
	if (n > 0 && n < sizeof(v)) return std::string(v, n);
#else
	if (const char* v = std::getenv(kClientIdEnvVar); v && *v) return v;
#endif
	return kDefaultClientId;
}

std::string GitHubOAuth::formEncode(const std::string& value)
{
	static const char* kHex = "0123456789ABCDEF";
	std::string out;
	out.reserve(value.size());
	for (const unsigned char c : value)
	{
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		    c == '-' || c == '.' || c == '_' || c == '~')
		{
			out.push_back(static_cast<char>(c));
		}
		else
		{
			out.push_back('%');
			out.push_back(kHex[c >> 4]);
			out.push_back(kHex[c & 0x0F]);
		}
	}
	return out;
}

// ─── Step 1: the device code ─────────────────────────────────────────────────

bool GitHubOAuth::parseDeviceCodeResponse(int statusCode, const std::string& body,
                                          DeviceCode& out, std::string* err)
{
	out = DeviceCode{};
	const json j = json::parse(body, nullptr, /*allow_exceptions=*/false);

	// An OAuth error object wins over the status code: GitHub sends some of
	// them with 200.
	if (j.is_object() && j.contains("error"))
	{
		if (err) *err = describeOAuthError(jsonString(j, "error"), j);
		return false;
	}
	if (statusCode != 200)
	{
		if (err)
		{
			const std::string detail = oauthDetail(j);
			*err = "GitHub answered HTTP " + std::to_string(statusCode) +
			       (detail.empty() ? "" : ": " + detail);
		}
		return false;
	}
	if (!j.is_object())
	{
		if (err) *err = "GitHub answered the sign-in request, but not with JSON";
		return false;
	}

	out.deviceCode       = jsonString(j, "device_code");
	out.userCode         = jsonString(j, "user_code");
	out.verificationUri  = jsonString(j, "verification_uri");
	out.intervalSeconds  = std::max(1, jsonInt(j, "interval", 5));
	out.expiresInSeconds = std::max(1, jsonInt(j, "expires_in", 900));

	if (out.deviceCode.empty() || out.userCode.empty() || out.verificationUri.empty())
	{
		if (err) *err = "GitHub answered the sign-in request without a code or a page to enter it";
		wipe(out.deviceCode);
		out = DeviceCode{};
		return false;
	}
	// The editor opens this in the user's browser. Anything but GitHub's own
	// page is refused rather than opened.
	if (out.verificationUri.rfind("https://github.com/", 0) != 0)
	{
		if (err) *err = "GitHub named a sign-in page outside github.com (" +
		                out.verificationUri + ") — refusing to open it";
		wipe(out.deviceCode);
		out = DeviceCode{};
		return false;
	}
	return true;
}

bool GitHubOAuth::requestDeviceCode(DeviceCode& out, std::string* err,
                                    const Transport& transport,
                                    const std::string& clientIdOverride)
{
	out = DeviceCode{};
	const std::string id = clientIdOverride.empty() ? clientId() : clientIdOverride;
	if (id.empty())
	{
		if (err) *err = "no GitHub OAuth client id is configured";
		return false;
	}

	const std::string form = "client_id=" + formEncode(id) + "&scope=" + formEncode(kScopes);

	HE_SC_INFO("Requesting a GitHub sign-in code (scopes: %s)", kScopes);
	OAuthHttpResponse resp = post(transport, kDeviceCodeUrl, form);
	if (!resp.ok)
	{
		if (err) *err = transportError(resp);
		return false;
	}
	const bool ok = parseDeviceCodeResponse(resp.statusCode, resp.body, out, err);
	wipe(resp.body);   // it holds the device code
	if (ok)
	{
		// The user code and the page are what the user sees anyway; the device
		// code is never logged.
		HE_SC_INFO("GitHub sign-in code %s — confirm at %s (valid %d s)",
		           out.userCode.c_str(), out.verificationUri.c_str(), out.expiresInSeconds);
	}
	return ok;
}

// ─── Step 3: waiting for the user ────────────────────────────────────────────

DevicePollResult GitHubOAuth::parseTokenResponse(int statusCode, const std::string& body,
                                                 std::string& token, std::string* scopes,
                                                 int* newIntervalSeconds, std::string* err)
{
	wipe(token);
	if (scopes) scopes->clear();
	if (newIntervalSeconds) *newIntervalSeconds = 0;

	const json j = json::parse(body, nullptr, /*allow_exceptions=*/false);
	if (!j.is_object())
	{
		if (err) *err = "GitHub answered HTTP " + std::to_string(statusCode) +
		                " without a readable sign-in answer";
		return DevicePollResult::Failed;
	}

	// The verdict is the body, not the status: pending, slow_down, expired and
	// denied all come with 200.
	if (const std::string code = jsonString(j, "error"); !code.empty())
	{
		if (code == "authorization_pending") return DevicePollResult::Pending;
		if (code == "slow_down")
		{
			if (newIntervalSeconds) *newIntervalSeconds = std::max(0, jsonInt(j, "interval", 0));
			return DevicePollResult::SlowDown;
		}
		if (err) *err = describeOAuthError(code, j);
		if (code == "expired_token") return DevicePollResult::Expired;
		if (code == "access_denied") return DevicePollResult::Denied;
		return DevicePollResult::Failed;
	}

	if (statusCode == 200)
	{
		std::string t = jsonString(j, "access_token");
		if (!t.empty())
		{
			token = std::move(t);
			if (scopes) *scopes = jsonString(j, "scope");
			return DevicePollResult::Granted;
		}
		if (err) *err = "GitHub answered the sign-in without a token or a reason";
		return DevicePollResult::Failed;
	}

	if (err) *err = "GitHub answered HTTP " + std::to_string(statusCode) +
	                " to the sign-in poll";
	return DevicePollResult::Failed;
}

DeviceFlowOutcome GitHubOAuth::pollForToken(const DeviceCode& code, OAuthCancel& cancel,
                                            std::string& token, std::string* scopes,
                                            std::string* err, const Transport& transport,
                                            const std::string& clientIdOverride)
{
	wipe(token);
	if (scopes) scopes->clear();
	if (code.deviceCode.empty())
	{
		if (err) *err = "no sign-in code to wait for";
		return DeviceFlowOutcome::Failed;
	}
	const std::string id = clientIdOverride.empty() ? clientId() : clientIdOverride;

	std::string form = "client_id=" + formEncode(id) +
	                   "&device_code=" + formEncode(code.deviceCode) +
	                   "&grant_type=" + formEncode(kDeviceGrantType);

	// Elapsed time is counted in waited intervals, not read off a clock: the
	// round trips themselves only make the local estimate early, and GitHub
	// says expired_token on its own anyway. It also keeps tests deterministic.
	const long long budgetMs  = static_cast<long long>(std::max(1, code.expiresInSeconds)) * 1000;
	long long       waitedMs  = 0;
	int             interval  = std::max(1, code.intervalSeconds);
	int             failsInARow = 0;

	DeviceFlowOutcome outcome = DeviceFlowOutcome::Failed;
	for (;;)
	{
		if (waitedMs >= budgetMs)
		{
			if (err) *err = describeOAuthError("expired_token", json());
			outcome = DeviceFlowOutcome::Expired;
			break;
		}

		// Wait first: a poll before one interval has passed counts as too fast.
		const int  waitMs = interval * 1000;
		const bool waited = transport.wait ? transport.wait(waitMs) : cancel.waitFor(waitMs);
		if (!waited || cancel.cancelled())
		{
			outcome = DeviceFlowOutcome::Cancelled;
			break;
		}
		waitedMs += waitMs;

		OAuthHttpResponse resp = post(transport, kAccessTokenUrl, form);
		if (cancel.cancelled())
		{
			wipe(resp.body);
			outcome = DeviceFlowOutcome::Cancelled;
			break;
		}
		if (!resp.ok)
		{
			if (++failsInARow >= kMaxTransportFailuresInARow)
			{
				if (err) *err = transportError(resp);
				outcome = DeviceFlowOutcome::Failed;
				break;
			}
			HE_SC_WARN("GitHub sign-in poll failed (%s) — retrying", transportError(resp).c_str());
			continue;
		}
		failsInARow = 0;

		int newInterval = 0;
		const DevicePollResult r =
			parseTokenResponse(resp.statusCode, resp.body, token, scopes, &newInterval, err);
		wipe(resp.body);   // it holds the token on success

		if (r == DevicePollResult::Pending) continue;
		if (r == DevicePollResult::SlowDown)
		{
			interval = std::max(newInterval, interval + kSlowDownStepSeconds);
			HE_SC_DEBUG("GitHub asked to poll slower — every %d s now", interval);
			continue;
		}
		outcome = r == DevicePollResult::Granted ? DeviceFlowOutcome::Granted
		        : r == DevicePollResult::Expired ? DeviceFlowOutcome::Expired
		        : r == DevicePollResult::Denied  ? DeviceFlowOutcome::Denied
		                                         : DeviceFlowOutcome::Failed;
		break;
	}
	wipe(form);   // carries the device code

	switch (outcome)
	{
	case DeviceFlowOutcome::Granted:
		HE_SC_INFO("GitHub sign-in granted (scopes: %s)",
		           scopes && !scopes->empty() ? scopes->c_str() : "?");
		break;
	case DeviceFlowOutcome::Cancelled:
		HE_SC_INFO("GitHub sign-in cancelled");
		break;
	default:
		HE_SC_WARN("GitHub sign-in ended without a token: %s",
		           err && !err->empty() ? err->c_str() : "no reason given");
		break;
	}
	return outcome;
}

// ─── The worker the dialog drives ────────────────────────────────────────────

const char* deviceLoginStateName(GitHubDeviceLogin::State state)
{
	using S = GitHubDeviceLogin::State;
	switch (state)
	{
	case S::Idle:           return "Idle";
	case S::RequestingCode: return "RequestingCode";
	case S::WaitingForUser: return "WaitingForUser";
	case S::Granted:        return "Granted";
	case S::Denied:         return "Denied";
	case S::Expired:        return "Expired";
	case S::Cancelled:      return "Cancelled";
	case S::Failed:         return "Failed";
	}
	return "?";
}

GitHubDeviceLogin::GitHubDeviceLogin(GitHubOAuth::Transport transport)
	: m_transport(std::move(transport))
{
}

GitHubDeviceLogin::~GitHubDeviceLogin()
{
	m_cancel.cancel();
	joinWorker();
	std::lock_guard<std::mutex> lock(m_mutex);
	wipe(m_token);
}

void GitHubDeviceLogin::joinWorker()
{
	if (m_worker.joinable()) m_worker.join();
}

void GitHubDeviceLogin::start()
{
	if (running()) return;
	// A finished run's thread has set its final state and only has to return,
	// so this join does not wait.
	joinWorker();
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		wipe(m_token);
		m_prompt = DevicePrompt{};
		m_error.clear();
		m_scopes.clear();
		m_state = State::RequestingCode;
	}
	m_cancel.reset();
	m_worker = std::thread([this] { run(); });
}

void GitHubDeviceLogin::cancel()
{
	m_cancel.cancel();
}

void GitHubDeviceLogin::run()
{
	DeviceCode  code;
	std::string err;
	if (!GitHubOAuth::requestDeviceCode(code, &err, m_transport))
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_state = m_cancel.cancelled() ? State::Cancelled : State::Failed;
		if (m_state == State::Failed) m_error = err;
		return;
	}
	if (m_cancel.cancelled())
	{
		wipe(code.deviceCode);
		std::lock_guard<std::mutex> lock(m_mutex);
		m_state = State::Cancelled;
		return;
	}
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_prompt.userCode         = code.userCode;
		m_prompt.verificationUri  = code.verificationUri;
		m_prompt.expiresInSeconds = code.expiresInSeconds;
		m_state = State::WaitingForUser;
	}

	std::string token, scopes;
	const DeviceFlowOutcome outcome =
		GitHubOAuth::pollForToken(code, m_cancel, token, &scopes, &err, m_transport);
	wipe(code.deviceCode);

	std::lock_guard<std::mutex> lock(m_mutex);
	switch (outcome)
	{
	case DeviceFlowOutcome::Granted:
		m_token  = std::move(token);
		m_scopes = std::move(scopes);
		m_state  = State::Granted;
		break;
	case DeviceFlowOutcome::Denied:    m_state = State::Denied;    m_error = err; break;
	case DeviceFlowOutcome::Expired:   m_state = State::Expired;   m_error = err; break;
	case DeviceFlowOutcome::Cancelled: m_state = State::Cancelled; break;
	case DeviceFlowOutcome::Failed:    m_state = State::Failed;    m_error = err; break;
	}
	wipe(token);
}

GitHubDeviceLogin::State GitHubDeviceLogin::state() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_state;
}

bool GitHubDeviceLogin::running() const
{
	const State s = state();
	return s == State::RequestingCode || s == State::WaitingForUser;
}

DevicePrompt GitHubDeviceLogin::prompt() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_prompt;
}

std::string GitHubDeviceLogin::error() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_error;
}

std::string GitHubDeviceLogin::grantedScopes() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_scopes;
}

std::string GitHubDeviceLogin::takeToken()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	std::string t = std::move(m_token);
	wipe(m_token);
	return t;
}

} // namespace HE::Sc
