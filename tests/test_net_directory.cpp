#include "doctest.h"

#include <Net/HttpsClient.h>
#include <Net/SessionDirectory.h>

#include <set>
#include <string>

using namespace HE::Net;

// ─── Session ids ─────────────────────────────────────────────────────────────

TEST_CASE("SessionDirectory: session ids are long, URL-safe and unique")
{
    std::set<std::string> seen;
    for (int i = 0; i < 50; ++i) {
        const std::string id = SessionDirectory::newSessionId();

        // The server enforces a 16-character minimum; anything shorter would be
        // rejected, and a guessable id would expose the host's IP.
        CHECK(id.size() >= 16);
        for (const char c : id) {
            const bool urlSafe = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
            CHECK(urlSafe);
        }
        seen.insert(id);
    }
    CHECK(seen.size() == 50);   // no collisions, so not a fixed seed
}

// ─── URL construction ────────────────────────────────────────────────────────

TEST_CASE("SessionDirectory: action is appended with the right separator")
{
    CHECK(SessionDirectory::buildUrl("https://example.com/session-api.php", "register")
          == "https://example.com/session-api.php?action=register");

    // An endpoint that already carries a query must not get a second '?'.
    CHECK(SessionDirectory::buildUrl("https://example.com/api.php?v=2", "lookup")
          == "https://example.com/api.php?v=2&action=lookup");
}

// ─── HTTP status mapping ─────────────────────────────────────────────────────

TEST_CASE("SessionDirectory: HTTP statuses map to distinguishable outcomes")
{
    CHECK(SessionDirectory::statusFromHttp(200) == DirectoryStatus::Ok);
    CHECK(SessionDirectory::statusFromHttp(404) == DirectoryStatus::NotFound);
    CHECK(SessionDirectory::statusFromHttp(409) == DirectoryStatus::Rejected);
    CHECK(SessionDirectory::statusFromHttp(429) == DirectoryStatus::Rejected);
    CHECK(SessionDirectory::statusFromHttp(500) == DirectoryStatus::NetworkError);
}

// ─── Registration parsing ────────────────────────────────────────────────────

TEST_CASE("SessionDirectory: a successful registration is parsed")
{
    const std::string body = R"({
        "ok": true, "publicIp": "203.0.113.42", "port": 7777,
        "reachable": true, "token": "deadbeefcafe", "ttl": 150
    })";

    SessionRegistration reg;
    REQUIRE(SessionDirectory::parseRegistration(body, reg) == DirectoryStatus::Ok);
    CHECK(reg.publicIp == "203.0.113.42");
    CHECK(reg.port == 7777);
    CHECK(reg.reachable);
    CHECK(reg.token == "deadbeefcafe");
    CHECK(reg.ttlSeconds == 150);
}

TEST_CASE("SessionDirectory: an unreachable registration is still parsed but flagged")
{
    // The host must learn this: publishing an endpoint nobody can connect to
    // leaves peers in an unexplained timeout.
    const std::string body = R"({
        "ok": true, "publicIp": "100.64.1.2", "port": 7777,
        "reachable": false, "token": "tok", "ttl": 150
    })";

    SessionRegistration reg;
    REQUIRE(SessionDirectory::parseRegistration(body, reg) == DirectoryStatus::Ok);
    CHECK_FALSE(reg.reachable);
}

TEST_CASE("SessionDirectory: a registration without a token is rejected")
{
    // Without a token the caller could never heartbeat or clean up — an entry it
    // cannot manage is worse than a clear failure.
    const std::string body = R"({"ok": true, "publicIp": "1.2.3.4", "port": 7777})";

    SessionRegistration reg;
    CHECK(SessionDirectory::parseRegistration(body, reg)
          == DirectoryStatus::MalformedResponse);
}

TEST_CASE("SessionDirectory: server-side refusals and garbage are distinguished")
{
    SessionRegistration reg;
    CHECK(SessionDirectory::parseRegistration(R"({"ok": false, "error": "session_id_taken"})", reg)
          == DirectoryStatus::Rejected);
    CHECK(SessionDirectory::parseRegistration("not json at all", reg)
          == DirectoryStatus::MalformedResponse);
    CHECK(SessionDirectory::parseRegistration("[1,2,3]", reg)
          == DirectoryStatus::MalformedResponse);
    CHECK(SessionDirectory::parseRegistration("", reg)
          == DirectoryStatus::MalformedResponse);
}

// ─── Lookup parsing ──────────────────────────────────────────────────────────

TEST_CASE("SessionDirectory: a lookup yields a connect target")
{
    const std::string body = R"({
        "ok": true, "host": "203.0.113.42", "port": 7777,
        "name": "Annas Session", "engineVersion": "0.2.0",
        "protocolVersion": 1, "reachable": true
    })";

    SessionLookup look;
    REQUIRE(SessionDirectory::parseLookup(body, look) == DirectoryStatus::Ok);
    CHECK(look.host == "203.0.113.42");
    CHECK(look.port == 7777);
    CHECK(look.name == "Annas Session");
    CHECK(look.engineVersion == "0.2.0");
    CHECK(look.protocolVersion == 1);
}

TEST_CASE("SessionDirectory: an entry without a usable address is refused")
{
    SessionLookup look;
    // Handing back ""/0 as a connect target would surface as a confusing
    // connection failure much later.
    CHECK(SessionDirectory::parseLookup(R"({"ok":true,"host":"","port":7777})", look)
          == DirectoryStatus::MalformedResponse);
    CHECK(SessionDirectory::parseLookup(R"({"ok":true,"host":"1.2.3.4","port":0})", look)
          == DirectoryStatus::MalformedResponse);
    CHECK(SessionDirectory::parseLookup(R"({"ok":false,"error":"session_not_found"})", look)
          == DirectoryStatus::NotFound);
}

TEST_CASE("SessionDirectory: numeric fields sent as strings are accepted")
{
    // PHP readily emits numbers as JSON strings depending on how a value was
    // stored, so the client must not be brittle about it.
    const std::string body =
        R"({"ok":true,"host":"1.2.3.4","port":"7777","protocolVersion":"2"})";

    SessionLookup look;
    REQUIRE(SessionDirectory::parseLookup(body, look) == DirectoryStatus::Ok);
    CHECK(look.port == 7777);
    CHECK(look.protocolVersion == 2);
}

// ─── TLS backend ─────────────────────────────────────────────────────────────

TEST_CASE("HttpsClient: a TLS backend is compiled in")
{
    // On Linux without libcurl this is legitimately false — but then the
    // directory is unavailable and the UI has to say so.
    if (!httpsAvailable()) {
        WARN("no TLS backend in this build — session directory unavailable");
        return;
    }
    const std::string backend = httpsBackendName();
    CHECK(backend != "none");
}

TEST_CASE("SessionDirectory: calls fail cleanly when there is no TLS backend")
{
    if (httpsAvailable()) return;   // covered by the live probe instead

    SessionDirectory dir("https://example.com/session-api.php");
    SessionLookup look;
    CHECK(dir.lookup("abcdefghijklmnop", look) == DirectoryStatus::NoTlsBackend);
}
