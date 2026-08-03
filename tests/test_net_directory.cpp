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

// ─── Publishing both address families ────────────────────────────────────────
// A host reaches the directory over exactly ONE address family, so only that one
// used to be recorded — and a guest that had only the other family could not
// connect at all, with nothing in the UI to explain why.

TEST_CASE("A lookup reply with several addresses yields all of them, best first")
{
    HE::Net::SessionLookup out;
    const std::string body = R"({
        "ok": true,
        "host": "203.0.113.7",
        "hosts": ["203.0.113.7", "2a02:3100:82c3:ae00::1"],
        "port": 7777,
        "name": "Studio",
        "engineVersion": "HorizonEngine",
        "protocolVersion": 1,
        "reachable": true
    })";
    REQUIRE(HE::Net::SessionDirectory::parseLookup(body, out) == HE::Net::DirectoryStatus::Ok);
    REQUIRE(out.hosts.size() == 2);
    CHECK(out.hosts[0] == "203.0.113.7");
    CHECK(out.hosts[1] == "2a02:3100:82c3:ae00::1");
    // host stays the first entry, so callers that want one address need not
    // handle an empty vector everywhere.
    CHECK(out.host == "203.0.113.7");
    CHECK(out.port == 7777);
}

TEST_CASE("A reply that predates the address list still works")
{
    // The single-valued form must keep working: a client updated before the
    // server would otherwise find no address at all and report the session as
    // broken.
    HE::Net::SessionLookup out;
    const std::string body =
        R"({"ok":true,"host":"198.51.100.9","port":7777,"name":"Old","reachable":true})";
    REQUIRE(HE::Net::SessionDirectory::parseLookup(body, out) == HE::Net::DirectoryStatus::Ok);
    REQUIRE(out.hosts.size() == 1);
    CHECK(out.hosts[0] == "198.51.100.9");
    CHECK(out.host == "198.51.100.9");
}

TEST_CASE("An entry with no usable address is refused rather than half-accepted")
{
    // Handing back a connect target of ""/0 would send the joiner into an
    // unexplained timeout instead of a clear failure.
    HE::Net::SessionLookup out;
    CHECK(HE::Net::SessionDirectory::parseLookup(
              R"({"ok":true,"hosts":[],"port":7777})", out) !=
          HE::Net::DirectoryStatus::Ok);
    CHECK(HE::Net::SessionDirectory::parseLookup(
              R"({"ok":true,"host":"203.0.113.7","port":0})", out) !=
          HE::Net::DirectoryStatus::Ok);
}

TEST_CASE("A verified second address comes back from registration, an unverified one does not")
{
    HE::Net::SessionRegistration out;
    const std::string verified = R"({
        "ok": true, "token": "abc", "publicIp": "203.0.113.7", "port": 7777,
        "reachable": true, "ttl": 150, "altAddress": "2a02:3100:82c3:ae00::1"
    })";
    REQUIRE(HE::Net::SessionDirectory::parseRegistration(verified, out) ==
            HE::Net::DirectoryStatus::Ok);
    CHECK(out.altAddress == "2a02:3100:82c3:ae00::1");

    // The server drops a claim it could not connect back to, so its absence is
    // the signal — the client must not assume the address it offered was taken.
    HE::Net::SessionRegistration plain;
    const std::string unverified =
        R"({"ok":true,"token":"abc","publicIp":"203.0.113.7","port":7777,"reachable":true,"ttl":150})";
    REQUIRE(HE::Net::SessionDirectory::parseRegistration(unverified, plain) ==
            HE::Net::DirectoryStatus::Ok);
    CHECK(plain.altAddress.empty());
}
