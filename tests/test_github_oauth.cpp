#include "doctest.h"

#include <SourceControl/GitHubOAuth.h>

#include <Diagnostics/Log.h>

#include <chrono>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ─── GitHub sign-in via the OAuth device flow ────────────────────────────────
// No network: a scripted GitHub answers from a queue and records what it was
// asked and how long the flow waited in between. The answers are GitHub's
// documented ones — including the part that matters most, that "not yet",
// "slower", "expired" and "denied" all arrive with HTTP 200.

using namespace HE::Sc;

namespace {

// Neither of these has a provider token prefix, so the log-scrubber would NOT
// catch them: if one shows up in the log, the code logged it.
constexpr const char* kFakeToken      = "plainsecret_TOKEN_7f3a9c";
constexpr const char* kFakeDeviceCode = "devicecode_SECRET_3584d8";

std::string deviceCodeBody(int interval = 5, int expiresIn = 900,
                           const char* uri = "https://github.com/login/device")
{
	return std::string(R"({"device_code":")") + kFakeDeviceCode +
	       R"(","user_code":"WDJB-MJHT","verification_uri":")" + uri +
	       R"(","expires_in":)" + std::to_string(expiresIn) +
	       R"(,"interval":)" + std::to_string(interval) + "}";
}

std::string grantedBody()
{
	return std::string(R"({"access_token":")") + kFakeToken +
	       R"(","token_type":"bearer","scope":"gist,repo"})";
}

const char* kPending  = R"({"error":"authorization_pending","error_description":"The authorization request is still pending."})";
const char* kExpired  = R"({"error":"expired_token","error_description":"The device_code has expired."})";
const char* kDenied   = R"({"error":"access_denied","error_description":"The user has denied your application access."})";

std::string slowDown(int interval)
{
	return std::string(R"({"error":"slow_down","error_description":"Too many requests.","interval":)") +
	       std::to_string(interval) + "}";
}

// A scripted GitHub. Every post pops one answer; an empty queue answers
// authorization_pending forever.
struct FakeGitHub
{
	struct Request { std::string url, body; };

	std::mutex                    mutex;
	std::deque<OAuthHttpResponse> answers;
	std::vector<Request>          requests;
	std::vector<std::string>      events;    // "wait:5000", "post:<url>" in order
	bool                          cancelOnNextWait = false;

	void answer(const std::string& body, int status = 200)
	{
		std::lock_guard<std::mutex> lock(mutex);
		OAuthHttpResponse r;
		r.ok = true; r.statusCode = status; r.body = body;
		answers.push_back(std::move(r));
	}
	void dropConnection()
	{
		std::lock_guard<std::mutex> lock(mutex);
		OAuthHttpResponse r;
		r.ok = false; r.error = "The network connection was lost.";
		answers.push_back(std::move(r));
	}

	std::vector<int> waits()
	{
		std::lock_guard<std::mutex> lock(mutex);
		std::vector<int> out;
		for (const std::string& e : events)
			if (e.rfind("wait:", 0) == 0) out.push_back(std::stoi(e.substr(5)));
		return out;
	}

	GitHubOAuth::Transport transport()
	{
		GitHubOAuth::Transport t;
		t.post = [this](const std::string& url, const std::string& body) {
			std::lock_guard<std::mutex> lock(mutex);
			requests.push_back({ url, body });
			events.push_back("post:" + url);
			if (answers.empty())
			{
				OAuthHttpResponse r;
				r.ok = true; r.statusCode = 200; r.body = kPending;
				return r;
			}
			OAuthHttpResponse r = answers.front();
			answers.pop_front();
			return r;
		};
		t.wait = [this](int ms) {
			std::lock_guard<std::mutex> lock(mutex);
			events.push_back("wait:" + std::to_string(ms));
			return !cancelOnNextWait;
		};
		return t;
	}
};

DeviceCode testCode(int interval = 5, int expiresIn = 900)
{
	DeviceCode c;
	c.deviceCode       = kFakeDeviceCode;
	c.userCode         = "WDJB-MJHT";
	c.verificationUri  = "https://github.com/login/device";
	c.intervalSeconds  = interval;
	c.expiresInSeconds = expiresIn;
	return c;
}

// Every SourceControl record while alive, category opened to Trace.
class ScLogSpy {
public:
	ScLogSpy() : m_previous(HE::Log::verbosity(HE::Log::Cat::SourceControl)) {
		HE::Log::setVerbosity(HE::Log::Cat::SourceControl, HE::LogLevel::Trace);
		m_handle = HE::Log::addSink(&ScLogSpy::onRecord, this);
	}
	~ScLogSpy() {
		HE::Log::removeSink(m_handle);
		HE::Log::setVerbosity(HE::Log::Cat::SourceControl, m_previous);
	}
	ScLogSpy(const ScLogSpy&)            = delete;
	ScLogSpy& operator=(const ScLogSpy&) = delete;

	bool mentions(const std::string& needle) {
		HE::Log::flush();
		std::lock_guard<std::mutex> lk(m_mutex);
		for (const std::string& line : m_lines)
			if (line.find(needle) != std::string::npos) return true;
		return false;
	}
	std::size_t count() {
		HE::Log::flush();
		std::lock_guard<std::mutex> lk(m_mutex);
		return m_lines.size();
	}

private:
	static void onRecord(const HE::Log::Record& rec, void* user) {
		if (rec.category != HE::Log::Cat::SourceControl) return;
		auto* self = static_cast<ScLogSpy*>(user);
		std::lock_guard<std::mutex> lk(self->m_mutex);
		self->m_lines.emplace_back(rec.message ? rec.message : "");
	}

	HE::LogLevel             m_previous;
	int                      m_handle = 0;
	std::mutex               m_mutex;
	std::vector<std::string> m_lines;
};

template <class Pred>
bool waitUntil(Pred pred, int timeoutMs = 5000)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (pred()) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return pred();
}

} // namespace

TEST_CASE("Device-flow form values are percent-encoded")
{
	CHECK(GitHubOAuth::formEncode("repo gist") == "repo%20gist");
	CHECK(GitHubOAuth::formEncode("urn:ietf:params:oauth:grant-type:device_code") ==
	      "urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code");
	CHECK(GitHubOAuth::formEncode("Ov23lieb4i1Z6WKdnn8o") == "Ov23lieb4i1Z6WKdnn8o");
	CHECK(GitHubOAuth::formEncode("a&b=c~d") == "a%26b%3Dc~d");
}

#ifndef _WIN32
TEST_CASE("The client id comes from the build unless the environment overrides it")
{
	const char* prev = std::getenv(GitHubOAuth::kClientIdEnvVar);
	const std::string saved = prev ? prev : "";

	::unsetenv(GitHubOAuth::kClientIdEnvVar);
	CHECK(GitHubOAuth::clientId() == GitHubOAuth::kDefaultClientId);

	::setenv(GitHubOAuth::kClientIdEnvVar, "", 1);
	CHECK(GitHubOAuth::clientId() == GitHubOAuth::kDefaultClientId);

	::setenv(GitHubOAuth::kClientIdEnvVar, "Iv1.forkedapp", 1);
	CHECK(GitHubOAuth::clientId() == "Iv1.forkedapp");

	if (prev) ::setenv(GitHubOAuth::kClientIdEnvVar, saved.c_str(), 1);
	else      ::unsetenv(GitHubOAuth::kClientIdEnvVar);
}
#endif

TEST_CASE("Device-code answers parse, and a page outside github.com is refused")
{
	DeviceCode  code;
	std::string err;

	SUBCASE("success")
	{
		REQUIRE(GitHubOAuth::parseDeviceCodeResponse(200, deviceCodeBody(7, 600), code, &err));
		CHECK(code.deviceCode == kFakeDeviceCode);
		CHECK(code.userCode == "WDJB-MJHT");
		CHECK(code.verificationUri == "https://github.com/login/device");
		CHECK(code.intervalSeconds == 7);
		CHECK(code.expiresInSeconds == 600);
	}
	SUBCASE("device flow switched off on the app — answered with 200")
	{
		CHECK_FALSE(GitHubOAuth::parseDeviceCodeResponse(200,
			R"({"error":"device_flow_disabled","error_description":"Device Flow must be explicitly enabled for this App"})",
			code, &err));
		CHECK(err.find("Device Flow") != std::string::npos);
		CHECK(code.deviceCode.empty());
	}
	SUBCASE("unknown client id")
	{
		CHECK_FALSE(GitHubOAuth::parseDeviceCodeResponse(200,
			R"({"error":"incorrect_client_credentials"})", code, &err));
		CHECK(err.find("client id") != std::string::npos);
	}
	SUBCASE("a verification page that is not GitHub's")
	{
		CHECK_FALSE(GitHubOAuth::parseDeviceCodeResponse(200,
			deviceCodeBody(5, 900, "https://evil.example/login/device"), code, &err));
		CHECK(err.find("outside github.com") != std::string::npos);
		CHECK(code.deviceCode.empty());
		CHECK_FALSE(GitHubOAuth::parseDeviceCodeResponse(200,
			deviceCodeBody(5, 900, "http://github.com/login/device"), code, &err));
	}
	SUBCASE("missing fields, garbage, server errors")
	{
		CHECK_FALSE(GitHubOAuth::parseDeviceCodeResponse(200, R"({"user_code":"X"})", code, &err));
		CHECK_FALSE(GitHubOAuth::parseDeviceCodeResponse(200, "not json", code, &err));
		CHECK_FALSE(GitHubOAuth::parseDeviceCodeResponse(503, "", code, &err));
		CHECK(err.find("503") != std::string::npos);
	}
}

TEST_CASE("Token-endpoint answers map to one verdict each, all with HTTP 200")
{
	std::string token, scopes, err;
	int interval = -1;

	CHECK(GitHubOAuth::parseTokenResponse(200, grantedBody(), token, &scopes, &interval, &err) ==
	      DevicePollResult::Granted);
	CHECK(token == kFakeToken);
	CHECK(scopes == "gist,repo");

	CHECK(GitHubOAuth::parseTokenResponse(200, kPending, token, &scopes, &interval, &err) ==
	      DevicePollResult::Pending);
	CHECK(token.empty());   // a previous token is not left lying in the out-parameter

	CHECK(GitHubOAuth::parseTokenResponse(200, slowDown(10), token, &scopes, &interval, &err) ==
	      DevicePollResult::SlowDown);
	CHECK(interval == 10);
	CHECK(GitHubOAuth::parseTokenResponse(200, R"({"error":"slow_down"})", token, &scopes,
	                                      &interval, &err) == DevicePollResult::SlowDown);
	CHECK(interval == 0);

	err.clear();
	CHECK(GitHubOAuth::parseTokenResponse(200, kExpired, token, &scopes, &interval, &err) ==
	      DevicePollResult::Expired);
	CHECK(err.find("expired") != std::string::npos);

	err.clear();
	CHECK(GitHubOAuth::parseTokenResponse(200, kDenied, token, &scopes, &interval, &err) ==
	      DevicePollResult::Denied);
	CHECK(err.find("declined") != std::string::npos);

	err.clear();
	CHECK(GitHubOAuth::parseTokenResponse(200,
		R"({"error":"something_new","error_description":"A brand new reason."})",
		token, &scopes, &interval, &err) == DevicePollResult::Failed);
	CHECK(err.find("A brand new reason.") != std::string::npos);

	CHECK(GitHubOAuth::parseTokenResponse(200, "{}", token, &scopes, &interval, &err) ==
	      DevicePollResult::Failed);
	CHECK(GitHubOAuth::parseTokenResponse(502, "<html>", token, &scopes, &interval, &err) ==
	      DevicePollResult::Failed);
	CHECK(token.empty());
}

TEST_CASE("Requesting a device code sends the client id and both scopes")
{
	FakeGitHub gh;
	gh.answer(deviceCodeBody());

	DeviceCode  code;
	std::string err;
	REQUIRE(GitHubOAuth::requestDeviceCode(code, &err, gh.transport(), "Ov23testclient"));
	CHECK(code.userCode == "WDJB-MJHT");

	REQUIRE(gh.requests.size() == 1);
	CHECK(gh.requests[0].url == GitHubOAuth::kDeviceCodeUrl);
	CHECK(gh.requests[0].body == "client_id=Ov23testclient&scope=repo%20gist");

	SUBCASE("a dropped connection is reported, not retried")
	{
		FakeGitHub down;
		down.dropConnection();
		CHECK_FALSE(GitHubOAuth::requestDeviceCode(code, &err, down.transport(), "x"));
		CHECK(err.find("network connection was lost") != std::string::npos);
	}
}

TEST_CASE("Polling waits an interval first, then until the user approves")
{
	FakeGitHub gh;
	gh.answer(kPending);
	gh.answer(kPending);
	gh.answer(grantedBody());

	OAuthCancel cancel;
	std::string token, scopes, err;
	CHECK(GitHubOAuth::pollForToken(testCode(), cancel, token, &scopes, &err, gh.transport(),
	                                "Ov23testclient") == DeviceFlowOutcome::Granted);
	CHECK(token == kFakeToken);
	CHECK(scopes == "gist,repo");

	// The very first thing is a wait — an early first poll counts as too fast.
	REQUIRE(gh.events.size() == 6);
	CHECK(gh.events[0] == "wait:5000");
	CHECK(gh.events[1] == std::string("post:") + GitHubOAuth::kAccessTokenUrl);
	CHECK(gh.waits() == std::vector<int>{ 5000, 5000, 5000 });

	REQUIRE(gh.requests.size() == 3);
	CHECK(gh.requests[0].body ==
	      std::string("client_id=Ov23testclient&device_code=") + kFakeDeviceCode +
	      "&grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code");
}

TEST_CASE("slow_down stretches the interval: to GitHub's number, or by five seconds")
{
	FakeGitHub gh;
	gh.answer(slowDown(12));             // GitHub names a new interval
	gh.answer(R"({"error":"slow_down"})"); // ...and once it does not: +5
	gh.answer(slowDown(3));              // a smaller one never shortens the gap
	gh.answer(grantedBody());

	OAuthCancel cancel;
	std::string token, err;
	CHECK(GitHubOAuth::pollForToken(testCode(5), cancel, token, nullptr, &err, gh.transport(),
	                                "id") == DeviceFlowOutcome::Granted);
	CHECK(gh.waits() == std::vector<int>{ 5000, 12000, 17000, 22000 });
}

TEST_CASE("An expired code, a refusal and an unknown error each end the polling")
{
	OAuthCancel cancel;
	std::string token, err;

	SUBCASE("GitHub says the code expired")
	{
		FakeGitHub gh;
		gh.answer(kPending);
		gh.answer(kExpired);
		CHECK(GitHubOAuth::pollForToken(testCode(), cancel, token, nullptr, &err,
		                                gh.transport(), "id") == DeviceFlowOutcome::Expired);
		CHECK(err.find("expired") != std::string::npos);
		CHECK(gh.requests.size() == 2);
	}
	SUBCASE("the code's lifetime runs out locally while GitHub keeps saying pending")
	{
		FakeGitHub gh;   // empty queue: pending forever
		CHECK(GitHubOAuth::pollForToken(testCode(5, 12), cancel, token, nullptr, &err,
		                                gh.transport(), "id") == DeviceFlowOutcome::Expired);
		// 5 + 5 + 5 = 15 s ≥ 12 s: three polls, then it stops asking.
		CHECK(gh.requests.size() == 3);
	}
	SUBCASE("the user declined on GitHub")
	{
		FakeGitHub gh;
		gh.answer(kDenied);
		CHECK(GitHubOAuth::pollForToken(testCode(), cancel, token, nullptr, &err,
		                                gh.transport(), "id") == DeviceFlowOutcome::Denied);
		CHECK(err.find("declined") != std::string::npos);
	}
	SUBCASE("the app's device flow is switched off")
	{
		FakeGitHub gh;
		gh.answer(R"({"error":"device_flow_disabled"})");
		CHECK(GitHubOAuth::pollForToken(testCode(), cancel, token, nullptr, &err,
		                                gh.transport(), "id") == DeviceFlowOutcome::Failed);
		CHECK(err.find("Device Flow") != std::string::npos);
	}
	CHECK(token.empty());
}

TEST_CASE("A dropped poll is retried; three in a row give up")
{
	OAuthCancel cancel;
	std::string token, err;

	SUBCASE("two drops, then the token")
	{
		FakeGitHub gh;
		gh.dropConnection();
		gh.dropConnection();
		gh.answer(grantedBody());
		CHECK(GitHubOAuth::pollForToken(testCode(), cancel, token, nullptr, &err,
		                                gh.transport(), "id") == DeviceFlowOutcome::Granted);
		CHECK(token == kFakeToken);
	}
	SUBCASE("three drops")
	{
		FakeGitHub gh;
		gh.dropConnection();
		gh.dropConnection();
		gh.dropConnection();
		CHECK(GitHubOAuth::pollForToken(testCode(), cancel, token, nullptr, &err,
		                                gh.transport(), "id") == DeviceFlowOutcome::Failed);
		CHECK(err.find("network connection was lost") != std::string::npos);
	}
}

TEST_CASE("Cancelling stops the polling without another request")
{
	FakeGitHub gh;
	gh.cancelOnNextWait = true;

	OAuthCancel cancel;
	std::string token, err;
	CHECK(GitHubOAuth::pollForToken(testCode(), cancel, token, nullptr, &err, gh.transport(),
	                                "id") == DeviceFlowOutcome::Cancelled);
	CHECK(gh.requests.empty());

	SUBCASE("the real wait wakes up at once on cancel")
	{
		OAuthCancel c;
		std::thread t([&c] {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			c.cancel();
		});
		const auto t0 = std::chrono::steady_clock::now();
		CHECK_FALSE(c.waitFor(30000));
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - t0).count();
		t.join();
		CHECK(ms < 5000);
		c.reset();
		CHECK(c.waitFor(1));
	}
}

TEST_CASE("The sign-in worker runs off the calling thread and hands the token over once")
{
	FakeGitHub gh;
	gh.answer(deviceCodeBody());
	gh.answer(kPending);
	gh.answer(grantedBody());

	ScLogSpy spy;
	{
		GitHubDeviceLogin login(gh.transport());
		CHECK(login.state() == GitHubDeviceLogin::State::Idle);
		login.start();
		REQUIRE(waitUntil([&] { return !login.running(); }));
		REQUIRE(login.state() == GitHubDeviceLogin::State::Granted);

		const DevicePrompt p = login.prompt();
		CHECK(p.userCode == "WDJB-MJHT");
		CHECK(p.verificationUri == "https://github.com/login/device");
		CHECK(p.expiresInSeconds == 900);
		CHECK(login.grantedScopes() == "gist,repo");

		CHECK(login.takeToken() == kFakeToken);
		CHECK(login.takeToken().empty());   // exactly once
	}

	// Something was logged — and neither the token nor the device code.
	CHECK(spy.count() > 0);
	CHECK(spy.mentions("WDJB-MJHT"));
	CHECK_FALSE(spy.mentions(kFakeToken));
	CHECK_FALSE(spy.mentions(kFakeDeviceCode));
}

TEST_CASE("The sign-in worker reports refusals and stops promptly on cancel")
{
	SUBCASE("denied")
	{
		FakeGitHub gh;
		gh.answer(deviceCodeBody());
		gh.answer(kDenied);
		GitHubDeviceLogin login(gh.transport());
		login.start();
		REQUIRE(waitUntil([&] { return !login.running(); }));
		CHECK(login.state() == GitHubDeviceLogin::State::Denied);
		CHECK_FALSE(login.error().empty());
		CHECK(login.takeToken().empty());
	}
	SUBCASE("device code request fails")
	{
		FakeGitHub gh;
		gh.answer(R"({"error":"device_flow_disabled"})");
		GitHubDeviceLogin login(gh.transport());
		login.start();
		REQUIRE(waitUntil([&] { return !login.running(); }));
		CHECK(login.state() == GitHubDeviceLogin::State::Failed);
		CHECK(login.error().find("Device Flow") != std::string::npos);
	}
	SUBCASE("cancel during a real five-second wait")
	{
		FakeGitHub gh;
		gh.answer(deviceCodeBody());
		GitHubOAuth::Transport t = gh.transport();
		t.wait = nullptr;   // the real, cancellable wait
		GitHubDeviceLogin login(t);
		login.start();
		REQUIRE(waitUntil([&] {
			return login.state() == GitHubDeviceLogin::State::WaitingForUser;
		}));
		const auto t0 = std::chrono::steady_clock::now();
		login.cancel();
		REQUIRE(waitUntil([&] { return !login.running(); }));
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - t0).count();
		CHECK(login.state() == GitHubDeviceLogin::State::Cancelled);
		CHECK(ms < 2000);
		// Only the device-code request went out; the cancelled wait never polled.
		CHECK(gh.requests.size() == 1);

		SUBCASE("and it can start over")
		{
			gh.answer(deviceCodeBody());
			gh.answer(grantedBody());
			// Back to the instant fake wait would need a new worker; the real
			// wait just takes one interval here, so only check the restart.
			login.start();
			CHECK(waitUntil([&] {
				return login.state() == GitHubDeviceLogin::State::WaitingForUser;
			}));
			login.cancel();
		}
	}
	SUBCASE("destroying a running sign-in does not hang")
	{
		FakeGitHub gh;
		gh.answer(deviceCodeBody());
		GitHubOAuth::Transport t = gh.transport();
		t.wait = nullptr;
		const auto t0 = std::chrono::steady_clock::now();
		{
			GitHubDeviceLogin login(t);
			login.start();
			waitUntil([&] { return login.state() == GitHubDeviceLogin::State::WaitingForUser; });
		}
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - t0).count();
		CHECK(ms < 3000);
	}
}
