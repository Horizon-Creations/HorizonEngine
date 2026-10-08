#pragma once

// ─── Signing in to GitHub without typing a token: the OAuth Device Flow ──────
// The editor is registered on GitHub as an OAuth App. Signing in is three moves
// (RFC 8628, GitHub's flavour of it):
//
//   1. POST https://github.com/login/device/code with the app's client id and
//      the scopes → a short user code ("WDJB-MJHT"), the page to type it into,
//      how often we may ask, and how long the code lives.
//   2. The user opens that page in their browser, signs in there, types the
//      code and approves. The editor never sees a password.
//   3. Meanwhile the editor POSTs https://github.com/login/oauth/access_token
//      every `interval` seconds until GitHub answers with a token, a refusal,
//      or the code runs out.
//
// No client secret anywhere: the device flow does not use one, which is what
// makes it fit an application that is shipped to people. Only the public
// client id is compiled in.
//
// Token rules — the same ones as GitHubApi.h:
//   • never logged — and neither is the device code, which is a redeemable
//     credential until it expires. User code and verification page are fine.
//   • error texts are built from GitHub's response (error / error_description),
//     never from a request
//   • the token leaves this file once, through takeToken(), and the caller hands
//     it to the credential helper and wipes it
//
// What GitHub does that a reader would not guess: the token endpoint answers
// HTTP 200 for "not yet", "slower", "expired" and "denied" alike. The verdict is
// the `error` field of the body, not the status code.

#include "SourceControl/ScCommon.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace HE::Sc {

// What step 1 hands back. `deviceCode` stays inside this layer — it is what
// redeems the token, so the UI gets only DevicePrompt below.
struct DeviceCode
{
	std::string deviceCode;
	std::string userCode;          // "WDJB-MJHT" — what the user types on GitHub
	std::string verificationUri;   // "https://github.com/login/device"
	int         intervalSeconds  = 5;    // minimum gap between two polls
	int         expiresInSeconds = 900;  // the codes' lifetime
};

// What the sign-in dialog shows.
struct DevicePrompt
{
	std::string userCode;
	std::string verificationUri;
	int         expiresInSeconds = 0;
};

// One answer of the token endpoint, interpreted.
enum class DevicePollResult : std::uint8_t {
	Granted,    // access_token present
	Pending,    // authorization_pending — the user has not approved yet
	SlowDown,   // slow_down — we asked too fast; the interval grows
	Expired,    // expired_token — the code ran out, start over
	Denied,     // access_denied — the user clicked Cancel on GitHub
	Failed,     // anything else: transport, a disabled device flow, a bad client id
};

// How a whole polling run ended.
enum class DeviceFlowOutcome : std::uint8_t {
	Granted,
	Expired,
	Denied,
	Cancelled,  // the caller stopped it
	Failed,
};

// The HTTP response as this layer sees it. Own type rather than
// HE::Net::HttpsResponse: HorizonNet is a PRIVATE dependency of this module.
struct OAuthHttpResponse
{
	bool        ok         = false;   // transport-level success
	int         statusCode = 0;
	std::string body;
	std::string error;                // when ok == false
};

// A stop flag that can also be slept on: cancel() wakes a waitFor() at once, so
// stopping a sign-in never has to sit out a five-second poll interval.
class HE_SC_API OAuthCancel {
public:
	void cancel();
	bool cancelled() const { return m_cancelled.load(std::memory_order_acquire); }
	// Sleeps up to `ms`; false when cancelled before or during the wait.
	bool waitFor(int ms);
	// Arm it again for the next run.
	void reset();

private:
	std::atomic<bool>       m_cancelled{ false };
	std::mutex              m_mutex;
	std::condition_variable m_cv;
};

class HE_SC_API GitHubOAuth {
public:
	// The OAuth App's public client id (Settings ▸ Developer settings ▸ OAuth
	// Apps). Not a secret: it is visible in every device-code request anyway.
	static constexpr const char* kDefaultClientId = "Ov23lieb4i1Z6WKdnn8o";
	// Overrides the compiled-in id — for a fork's own app, or a test app.
	static constexpr const char* kClientIdEnvVar  = "HE_GITHUB_OAUTH_CLIENT_ID";
	// repo: create, list, clone, push. gist: the log upload of Report Issue.
	static constexpr const char* kScopes          = "repo gist";

	static constexpr const char* kDeviceCodeUrl   = "https://github.com/login/device/code";
	static constexpr const char* kAccessTokenUrl  = "https://github.com/login/oauth/access_token";

	// slow_down without a new interval in the body: RFC 8628 §3.5 says +5 s.
	static constexpr int kSlowDownStepSeconds = 5;

	// The env override when set and non-empty, the compiled-in id otherwise.
	static std::string clientId();

	// Everything that touches the outside world, swappable for tests. An empty
	// `post` means HTTPS through HorizonNet; an empty `wait` means
	// OAuthCancel::waitFor. `wait` returns false to mean "cancelled".
	struct Transport
	{
		std::function<OAuthHttpResponse(const std::string& url,
		                                 const std::string& formBody)> post;
		std::function<bool(int ms)>                                  wait;
	};

	// Step 1. Blocking, one HTTPS round trip — worker thread only.
	static bool requestDeviceCode(DeviceCode& out, std::string* err = nullptr,
	                              const Transport& transport = {},
	                              const std::string& clientIdOverride = {});

	// Step 3. Blocking until the user decides, the code expires, or `cancel`
	// fires — worker thread only. Waits one interval BEFORE the first poll
	// (GitHub counts an early first poll as too fast). `token` receives the
	// access token on Granted and is empty otherwise; `scopes` what GitHub
	// actually granted.
	static DeviceFlowOutcome pollForToken(const DeviceCode& code, OAuthCancel& cancel,
	                                      std::string& token, std::string* scopes = nullptr,
	                                      std::string* err = nullptr,
	                                      const Transport& transport = {},
	                                      const std::string& clientIdOverride = {});

	// Pure response interpretation, exposed so it is testable without a network.
	// A device code is only accepted with a verification page under
	// https://github.com/ — step 2 opens it in the user's browser.
	static bool parseDeviceCodeResponse(int statusCode, const std::string& body,
	                                    DeviceCode& out, std::string* err = nullptr);
	// `newIntervalSeconds` receives the interval a slow_down answer names, or
	// 0 when it names none. `token`/`scopes` are only filled on Granted.
	static DevicePollResult parseTokenResponse(int statusCode, const std::string& body,
	                                           std::string& token, std::string* scopes,
	                                           int* newIntervalSeconds,
	                                           std::string* err = nullptr);

	// application/x-www-form-urlencoded value encoding (RFC 3986 unreserved
	// characters pass, everything else is %XX; a space is %20).
	static std::string formEncode(const std::string& value);
};

// The whole sign-in off the UI thread. The dialog calls start(), reads state()
// and prompt() every frame, and on Granted takes the token once.
//
// Threading: one worker per run. Everything the UI reads is copied under a
// mutex; nothing here touches ImGui or the editor. The destructor cancels and
// joins — immediate while the worker waits between polls, bounded by the HTTPS
// timeout (15 s) if a request is in flight.
class HE_SC_API GitHubDeviceLogin {
public:
	enum class State : std::uint8_t {
		Idle,
		RequestingCode,   // step 1 in flight
		WaitingForUser,   // prompt() is valid; polling
		Granted,          // takeToken() has the token
		Denied,
		Expired,
		Cancelled,
		Failed,           // error() says why
	};

	explicit GitHubDeviceLogin(GitHubOAuth::Transport transport = {});
	~GitHubDeviceLogin();

	GitHubDeviceLogin(const GitHubDeviceLogin&)            = delete;
	GitHubDeviceLogin& operator=(const GitHubDeviceLogin&) = delete;

	// Begin a sign-in. Ignored while one is running; after a finished one it
	// starts over with a fresh code (and drops a token nobody took).
	void start();
	// Stop polling. The state becomes Cancelled once the worker notices.
	void cancel();

	State        state() const;
	bool         running() const;
	DevicePrompt prompt() const;          // empty before WaitingForUser
	std::string  error() const;           // set for Failed, Expired, Denied
	std::string  grantedScopes() const;

	// The token, exactly once: the internal copy is wiped. Empty unless Granted.
	std::string takeToken();

private:
	void run();
	void joinWorker();

	GitHubOAuth::Transport m_transport;
	OAuthCancel            m_cancel;
	std::thread            m_worker;

	mutable std::mutex m_mutex;
	State              m_state = State::Idle;
	DevicePrompt       m_prompt;
	std::string        m_error;
	std::string        m_scopes;
	std::string        m_token;
};

HE_SC_API const char* deviceLoginStateName(GitHubDeviceLogin::State state);

} // namespace HE::Sc
