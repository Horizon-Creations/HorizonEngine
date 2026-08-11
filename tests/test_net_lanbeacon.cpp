#include "doctest.h"

#include <Net/LanBeacon.h>
#include <Net/CollabSession.h>
#include "../src/HE_Editor/CollabController.h"
#include <string>
#include <vector>

using namespace HE::Net;
using namespace HE::Net::LanBeacon;

namespace {

Announcement sample() {
    Announcement a;
    a.protocol     = kCollabProtocolVersion;
    a.instance     = 0xABCDEF0123456789ull;
    a.sessionId    = "sess-1234";
    a.port         = 7777;
    a.hostName     = "Anna";
    a.projectLabel = "Catania";
    a.projectKey   = "proj-key-1";
    a.participants = 3;
    return a;
}

// Hand the browser a datagram exactly as the socket loop would.
void feed(Browser& b, const std::string& from, const Announcement& a,
          std::uint64_t nowMs) {
    const std::vector<std::uint8_t> bytes = encode(a);
    b.ingest(from, bytes.data(), bytes.size(), nowMs);
}

} // namespace

TEST_CASE("LanBeacon: an announcement survives the round trip")
{
    const Announcement in = sample();
    const std::vector<std::uint8_t> bytes = encode(in);

    Announcement out;
    REQUIRE(decode(bytes.data(), bytes.size(), out));
    CHECK(out.protocol     == in.protocol);
    CHECK(out.instance     == in.instance);
    CHECK(out.sessionId    == in.sessionId);
    CHECK(out.port         == in.port);
    CHECK(out.hostName     == in.hostName);
    CHECK(out.projectLabel == in.projectLabel);
    CHECK(out.projectKey   == in.projectKey);
    CHECK(out.participants == in.participants);
    CHECK_FALSE(out.closing);
}

TEST_CASE("LanBeacon: the join code is not in the announcement")
{
    // The one property this whole design rests on. An Announcement has no field
    // to put it in — this test exists so that adding one is a failing test
    // rather than a quiet leak of the thing that guards every session.
    Announcement a = sample();
    a.sessionId = "SESSIONID";
    a.hostName  = "Anna";
    const std::vector<std::uint8_t> bytes = encode(a);

    const std::string blob(bytes.begin(), bytes.end());
    // A join secret is generated, never chosen; if one ever reached the wire it
    // would appear verbatim, as these do.
    CHECK(blob.find("SESSIONID") != std::string::npos);   // the id DOES travel
    for (const char* secretish : { "join", "code", "secret" })
        CHECK(blob.find(secretish) == std::string::npos);
}

TEST_CASE("LanBeacon: rubbish on the port is ignored")
{
    Announcement out;
    const std::uint8_t junk[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    CHECK_FALSE(decode(junk, sizeof(junk), out));
    CHECK_FALSE(decode(nullptr, 0, out));

    // Ours, but truncated mid-way: everything after the magic must fail rather
    // than yield half an entry.
    std::vector<std::uint8_t> bytes = encode(sample());
    for (std::size_t cut : { std::size_t(4), std::size_t(8), bytes.size() / 2 }) {
        Announcement partial;
        CHECK_FALSE(decode(bytes.data(), cut, partial));
    }

    // A datagram larger than anything we send is refused before it is parsed.
    std::vector<std::uint8_t> huge(kMaxDatagram + 1, 0);
    CHECK_FALSE(decode(huge.data(), huge.size(), out));
}

TEST_CASE("LanBeacon: a port of zero is not a session")
{
    Announcement a = sample();
    a.port = 0;
    const std::vector<std::uint8_t> bytes = encode(a);
    Announcement out;
    // Well-formed in every other respect, and still useless: there is nothing
    // to connect to.
    CHECK_FALSE(decode(bytes.data(), bytes.size(), out));
}

TEST_CASE("LanBeacon: the address comes from the packet, not from its contents")
{
    Browser b;
    feed(b, "192.168.1.50", sample(), 1000);
    REQUIRE(b.sessions().size() == 1);
    // An announcer that could name its own address could point joins at a third
    // machine. Only the port is taken from the payload.
    CHECK(b.sessions()[0].address == "192.168.1.50");
    CHECK(b.sessions()[0].port    == 7777);
}

TEST_CASE("LanBeacon: the same host heard twice is one entry")
{
    Browser b;
    const Announcement a = sample();
    // Every beacon goes out multicast AND broadcast, so both arrive within
    // milliseconds of each other. One session, not two.
    feed(b, "192.168.1.50", a, 1000);
    feed(b, "192.168.1.50", a, 1001);
    CHECK(b.sessions().size() == 1);

    // A different editor run is a different session, even from one machine.
    Announcement other = a;
    other.instance  = 42;
    other.sessionId = "sess-9999";
    feed(b, "192.168.1.50", other, 1002);
    CHECK(b.sessions().size() == 2);
}

TEST_CASE("LanBeacon: a session that stops talking disappears")
{
    Browser b;
    feed(b, "192.168.1.50", sample(), 1000);
    REQUIRE(b.sessions().size() == 1);

    b.update(1000 + kExpiryMs - 1);
    CHECK(b.sessions().size() == 1);   // still within earshot

    // A host that crashed or walked out of range never says goodbye, so silence
    // has to be enough on its own.
    b.update(1000 + kExpiryMs + 1);
    CHECK(b.sessions().empty());
}

TEST_CASE("LanBeacon: a goodbye removes it at once")
{
    Browser b;
    Announcement a = sample();
    feed(b, "192.168.1.50", a, 1000);
    REQUIRE(b.sessions().size() == 1);

    a.closing = true;
    feed(b, "192.168.1.50", a, 1100);
    // Not left to expire: someone clicking a session that closed two seconds ago
    // meets a connection error as their first impression of the feature.
    CHECK(b.sessions().empty());
}

TEST_CASE("LanBeacon: a flood of forged announcements is bounded")
{
    Browser b;
    for (std::uint64_t i = 0; i < kMaxSessions * 4; ++i) {
        Announcement a = sample();
        a.instance  = 1000 + i;
        a.sessionId = "sess-" + std::to_string(i);
        feed(b, "10.0.0.9", a, 1000);
    }
    // Unauthenticated input from anyone who can reach the port — it must not be
    // able to grow this without limit.
    CHECK(b.sessions().size() == kMaxSessions);
}

TEST_CASE("LanBeacon: an absurd name still fits in a datagram")
{
    Announcement a = sample();
    a.hostName     = std::string(4000, 'x');
    a.projectLabel = std::string(4000, 'y');
    a.sessionId    = std::string(4000, 'z');
    const std::vector<std::uint8_t> bytes = encode(a);

    // Capped on the way OUT, which is what keeps this under the size every
    // receiver enforces. Without it, a long project name would produce a
    // datagram everyone discards — the session would never appear for anyone,
    // with nothing on screen to suggest why.
    CHECK(bytes.size() <= kMaxDatagram);

    Announcement out;
    REQUIRE(decode(bytes.data(), bytes.size(), out));
    CHECK(out.hostName.size()     <= kMaxStringLen);
    CHECK(out.projectLabel.size() <= kMaxStringLen);
}

TEST_CASE("LanBeacon: a session heard here is reached by its local address")
{
    // The rule that makes joining by ID work between two people in the same
    // room: the directory would answer with the host's PUBLIC address, and most
    // routers refuse to let a machine reach its own network that way. If we can
    // hear the session locally, the local address wins.
    Browser b;
    Announcement a = sample();
    a.sessionId = "sess-here";
    a.port      = 7777;
    feed(b, "192.168.1.50", a, 1000);

    const auto* found =
        CollabController::lanEndpointFor(b.sessions(), "sess-here");
    REQUIRE(found != nullptr);
    CHECK(found->address == "192.168.1.50");
    CHECK(found->port    == 7777);

    // A session we cannot hear falls through to the directory, as before.
    CHECK(CollabController::lanEndpointFor(b.sessions(), "sess-elsewhere") == nullptr);
    CHECK(CollabController::lanEndpointFor(b.sessions(), "") == nullptr);
}

TEST_CASE("LanBeacon: an entry with nowhere to connect never wins")
{
    // It would take precedence over the directory and then fail — worse than
    // not having heard the session at all.
    std::vector<Browser::Session> sessions(1);
    sessions[0].sessionId = "sess-1";
    sessions[0].address   = "";
    sessions[0].port      = 7777;
    CHECK(CollabController::lanEndpointFor(sessions, "sess-1") == nullptr);

    sessions[0].address = "192.168.1.50";
    sessions[0].port    = 0;
    CHECK(CollabController::lanEndpointFor(sessions, "sess-1") == nullptr);

    sessions[0].port = 7777;
    CHECK(CollabController::lanEndpointFor(sessions, "sess-1") != nullptr);
}

TEST_CASE("LanBeacon: a peer on another protocol is kept, not hidden")
{
    Browser b;
    Announcement old = sample();
    old.protocol = kCollabProtocolVersion - 1;
    feed(b, "192.168.1.77", old, 1000);

    // Deliberately NOT dropped: "my friend's session does not show up at all" is
    // a support thread; a row that says which build is needed explains itself.
    REQUIRE(b.sessions().size() == 1);
    CHECK(b.sessions()[0].protocol == kCollabProtocolVersion - 1);
}
