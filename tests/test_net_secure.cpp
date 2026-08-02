#include "doctest.h"

#include <Net/NetSession.h>
#include <Net/SecureTransport.h>
#include <Net/TcpTransport.h>

#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace HE::Net;

namespace {

// A minimal in-process ITransport that also *records* every frame handed to it,
// so tests can inspect what actually crosses the wire (and corrupt it). The
// SecureTransport takes ownership of these, so tests keep raw pointers taken
// before the move.
class SpyEndpoint final : public ITransport {
public:
    SpyEndpoint*                       peer       = nullptr;
    std::vector<std::vector<std::uint8_t>> sentFrames;
    bool                               corruptNext = false;
    // Flip a byte at a chosen offset instead of the middle — lets a test target
    // a specific field (e.g. the ephemeral public key) rather than whatever
    // happens to sit at the halfway point.
    int                                corruptAtIndex = -1;
    bool                               connected   = true;

    using ITransport::send;

    void update() override {}

    void send(ConnectionId conn, const std::uint8_t* data, std::size_t len,
              SendMode) override {
        if (conn != 1 || !connected) return;
        std::vector<std::uint8_t> frame(data, data + len);
        sentFrames.push_back(frame);
        if (corruptNext && !frame.empty()) {
            corruptNext = false;
            const std::size_t at = (corruptAtIndex >= 0 &&
                                    static_cast<std::size_t>(corruptAtIndex) < frame.size())
                                       ? static_cast<std::size_t>(corruptAtIndex)
                                       : frame.size() / 2;
            frame[at] ^= 0xFF;   // flip a byte in flight
        }
        if (peer) peer->inbound.push_back(NetEvent{ NetEventType::Data, 1, std::move(frame) });
    }

    bool poll(NetEvent& out) override {
        if (inbound.empty()) return false;
        out = std::move(inbound.front());
        inbound.pop_front();
        return true;
    }

    void disconnect(ConnectionId) override { connected = false; }
    std::size_t connectionCount() const override { return connected ? 1u : 0u; }

    std::deque<NetEvent> inbound;
};

struct SpyPair {
    SpyEndpoint*                     hostRaw   = nullptr;
    SpyEndpoint*                     clientRaw = nullptr;
    std::unique_ptr<SecureTransport> host;
    std::unique_ptr<SecureTransport> client;
};

// Wire up two spy endpoints behind SecureTransport, seeded with the initial
// Connected event each real transport would deliver.
SpyPair makeSecurePair(const std::string& hostSecret, const std::string& clientSecret) {
    auto a = std::make_unique<SpyEndpoint>();
    auto b = std::make_unique<SpyEndpoint>();
    SpyEndpoint* aRaw = a.get();
    SpyEndpoint* bRaw = b.get();
    aRaw->peer = bRaw;
    bRaw->peer = aRaw;
    aRaw->inbound.push_back(NetEvent{ NetEventType::Connected, 1, {} });
    bRaw->inbound.push_back(NetEvent{ NetEventType::Connected, 1, {} });

    SpyPair out;
    out.hostRaw   = aRaw;
    out.clientRaw = bRaw;
    out.host   = SecureTransport::wrap(std::move(a),
                     SecureTransport::Config{ hostSecret,   NetRole::Host,   false });
    out.client = SecureTransport::wrap(std::move(b),
                     SecureTransport::Config{ clientSecret, NetRole::Client, false });
    return out;
}

// The handshake needs a few pump rounds to complete (challenge → response →
// accept), so drive both ends until they settle.
void pumpRounds(SecureTransport& a, SecureTransport& b, int rounds = 8) {
    for (int i = 0; i < rounds; ++i) { a.update(); b.update(); }
}

std::vector<NetEvent> drainSecure(SecureTransport& t) {
    std::vector<NetEvent> out;
    NetEvent ev;
    while (t.poll(ev)) out.push_back(std::move(ev));
    return out;
}

} // namespace

// ─── Join secrets ────────────────────────────────────────────────────────────

TEST_CASE("SecureTransport: generated join secrets are long and distinct")
{
    const std::string a = SecureTransport::generateJoinSecret();
    const std::string b = SecureTransport::generateJoinSecret();

    CHECK(a.size() == 26);          // ~128 bits of entropy
    CHECK(b.size() == 26);
    CHECK(a != b);                  // must not be deterministic

    // Alphabet excludes I/L/O/U so codes stay unambiguous when read aloud.
    for (const char c : a) {
        CHECK(c != 'I'); CHECK(c != 'L'); CHECK(c != 'O'); CHECK(c != 'U');
    }
}

TEST_CASE("SecureTransport: wrap rejects an empty secret or a null transport")
{
    auto ep = std::make_unique<SpyEndpoint>();
    CHECK(SecureTransport::wrap(std::move(ep),
              SecureTransport::Config{ "", NetRole::Host, false }) == nullptr);
    CHECK(SecureTransport::wrap(nullptr,
              SecureTransport::Config{ "secret", NetRole::Host, false }) == nullptr);
}

// ─── Handshake ───────────────────────────────────────────────────────────────

TEST_CASE("SecureTransport: matching secrets establish an authenticated session")
{
    auto pair = makeSecurePair("JOINSECRET123", "JOINSECRET123");
    REQUIRE(pair.host != nullptr);
    REQUIRE(pair.client != nullptr);

    // Nothing is visible upward before authentication completes.
    CHECK(pair.host->connectionCount() == 0);
    CHECK(pair.client->connectionCount() == 0);

    pumpRounds(*pair.host, *pair.client);

    CHECK(pair.host->connectionCount() == 1);
    CHECK(pair.client->connectionCount() == 1);
    CHECK(pair.host->stateOf(1) == HandshakeState::Established);
    CHECK(pair.client->stateOf(1) == HandshakeState::Established);

    // Each side surfaces exactly one Connected — and only after the handshake.
    const auto hostEvents   = drainSecure(*pair.host);
    const auto clientEvents = drainSecure(*pair.client);
    REQUIRE(hostEvents.size() == 1);
    CHECK(hostEvents[0].type == NetEventType::Connected);
    REQUIRE(clientEvents.size() == 1);
    CHECK(clientEvents[0].type == NetEventType::Connected);
}

TEST_CASE("SecureTransport: a wrong join secret never authenticates")
{
    auto pair = makeSecurePair("CORRECT-SECRET", "WRONG-SECRET");
    REQUIRE(pair.host != nullptr);

    pumpRounds(*pair.host, *pair.client);

    CHECK(pair.host->connectionCount() == 0);
    CHECK(pair.client->connectionCount() == 0);

    // Critically: no Connected is ever surfaced to the application.
    CHECK(drainSecure(*pair.host).empty());
    CHECK(drainSecure(*pair.client).empty());
}

TEST_CASE("SecureTransport: the join secret never appears on the wire")
{
    const std::string secret = "SUPERSECRETJOINCODE";
    auto pair = makeSecurePair(secret, secret);
    pumpRounds(*pair.host, *pair.client);

    const std::vector<std::uint8_t> needle(secret.begin(), secret.end());
    for (const auto* ep : { pair.hostRaw, pair.clientRaw }) {
        for (const auto& frame : ep->sentFrames) {
            const bool contains = std::search(frame.begin(), frame.end(),
                                              needle.begin(), needle.end()) != frame.end();
            CHECK_FALSE(contains);
        }
    }
}

// ─── Forward secrecy (ephemeral X25519, handshake v2) ────────────────────────

TEST_CASE("SecureTransport: both ends of a session agree on the fingerprint")
{
    auto pair = makeSecurePair("JOINSECRET123", "JOINSECRET123");
    pumpRounds(*pair.host, *pair.client);
    REQUIRE(pair.host->connectionCount() == 1);

    const std::string hostFp   = pair.host->sessionFingerprint(1);
    const std::string clientFp = pair.client->sessionFingerprint(1);

    CHECK_FALSE(hostFp.empty());
    // Both sides must derive the identical session key, or nothing they send
    // could be decrypted by the other.
    CHECK(hostFp == clientFp);
}

TEST_CASE("SecureTransport: the same join secret yields a different key each session")
{
    // Necessary but NOT sufficient for forward secrecy: the old v1 handshake
    // also produced per-session keys, because its nonces were random too. What
    // this rules out is key reuse across sessions sharing a secret. The
    // distinguishing evidence is in the next test.
    auto first = makeSecurePair("SAME-SECRET-BOTH", "SAME-SECRET-BOTH");
    pumpRounds(*first.host, *first.client);
    REQUIRE(first.host->connectionCount() == 1);
    const std::string firstFp = first.host->sessionFingerprint(1);

    auto second = makeSecurePair("SAME-SECRET-BOTH", "SAME-SECRET-BOTH");
    pumpRounds(*second.host, *second.client);
    REQUIRE(second.host->connectionCount() == 1);
    const std::string secondFp = second.host->sessionFingerprint(1);

    CHECK_FALSE(firstFp.empty());
    CHECK(firstFp != secondFp);
}

TEST_CASE("SecureTransport: the handshake carries fresh ephemeral keys each session")
{
    // This is what actually separates v2 from v1: the transcript contains
    // ephemeral public keys whose private halves are never transmitted and are
    // wiped after use. Forward secrecy follows from that — an eavesdropper who
    // later learns the join secret still lacks the private scalars, and they no
    // longer exist anywhere. A unit test cannot prove the absence of a
    // derivation, but it can prove the mechanism is present and per-session.
    auto first  = makeSecurePair("SAME-SECRET-BOTH", "SAME-SECRET-BOTH");
    pumpRounds(*first.host, *first.client);
    REQUIRE(first.host->connectionCount() == 1);

    auto second = makeSecurePair("SAME-SECRET-BOTH", "SAME-SECRET-BOTH");
    pumpRounds(*second.host, *second.client);
    REQUIRE(second.host->connectionCount() == 1);

    // Challenge layout: [msg][version][32 nonce][32 ephemeral pubkey].
    REQUIRE_FALSE(first.hostRaw->sentFrames.empty());
    REQUIRE_FALSE(second.hostRaw->sentFrames.empty());
    const auto& c1 = first.hostRaw->sentFrames[0];
    const auto& c2 = second.hostRaw->sentFrames[0];

    REQUIRE(c1.size() == 2 + 32 + 32);   // v1 challenges were 32 bytes shorter
    REQUIRE(c2.size() == 2 + 32 + 32);
    CHECK(c1[1] == 2);                   // protocol version 2

    const std::vector<std::uint8_t> pub1(c1.begin() + 34, c1.end());
    const std::vector<std::uint8_t> pub2(c2.begin() + 34, c2.end());
    CHECK(pub1.size() == 32);
    CHECK(pub1 != pub2);                 // a fresh keypair per session

    // And the client contributes its own, so neither side alone fixes the key.
    REQUIRE_FALSE(first.clientRaw->sentFrames.empty());
    const auto& r1 = first.clientRaw->sentFrames[0];
    REQUIRE(r1.size() == 1 + 32 + 32 + 32);
    const std::vector<std::uint8_t> clientPub(r1.begin() + 33, r1.begin() + 65);
    CHECK(clientPub != pub1);
}

TEST_CASE("SecureTransport: substituting the ephemeral public key breaks the handshake")
{
    auto pair = makeSecurePair("JOINSECRET123", "JOINSECRET123");

    // Corrupt a byte inside the host's ephemeral public key in the Challenge:
    // layout is [msg][version][32 nonce][32 pubkey], so offset 34 is the first
    // key byte. This is what a man in the middle would attempt — swapping in a
    // key it controls to read the traffic.
    pair.hostRaw->corruptNext    = true;
    pair.hostRaw->corruptAtIndex = 2 + 32 + 4;

    pumpRounds(*pair.host, *pair.client);

    // The mac covers the transcript including both public keys, so the host sees
    // a mac over a key it never sent and refuses. No session is established, and
    // nothing is surfaced to the application.
    CHECK(pair.host->connectionCount() == 0);
    CHECK(pair.client->connectionCount() == 0);
    CHECK(drainSecure(*pair.host).empty());
}

TEST_CASE("SecureTransport: no fingerprint before the handshake completes")
{
    auto pair = makeSecurePair("JOINSECRET123", "JOINSECRET123");
    CHECK(pair.host->sessionFingerprint(1).empty());
    CHECK(pair.host->sessionFingerprint(999).empty());   // unknown peer
}

// ─── Encrypted data ──────────────────────────────────────────────────────────

TEST_CASE("SecureTransport: payloads round-trip and are not sent in the clear")
{
    auto pair = makeSecurePair("JOINSECRET123", "JOINSECRET123");
    pumpRounds(*pair.host, *pair.client);
    drainSecure(*pair.host);
    drainSecure(*pair.client);

    if (!pair.host->encryptionActive()) return;   // no crypto backend in this build

    const std::string plaintext = "geheime szenendaten";
    const std::vector<std::uint8_t> payload(plaintext.begin(), plaintext.end());

    const std::size_t framesBefore = pair.clientRaw->sentFrames.size();
    pair.client->send(1, payload, SendMode::ReliableOrdered);
    pumpRounds(*pair.host, *pair.client, 2);

    // The application sees the original bytes...
    const auto hostEvents = drainSecure(*pair.host);
    REQUIRE(hostEvents.size() == 1);
    CHECK(hostEvents[0].type == NetEventType::Data);
    CHECK(hostEvents[0].data == payload);

    // ...but the frame that actually crossed the wire does not contain them.
    REQUIRE(pair.clientRaw->sentFrames.size() > framesBefore);
    const auto& wire = pair.clientRaw->sentFrames[framesBefore];
    const bool leaked = std::search(wire.begin(), wire.end(),
                                    payload.begin(), payload.end()) != wire.end();
    CHECK_FALSE(leaked);
    CHECK(wire.size() > payload.size());   // counter + GCM tag overhead
}

TEST_CASE("SecureTransport: a tampered frame drops the link instead of being delivered")
{
    auto pair = makeSecurePair("JOINSECRET123", "JOINSECRET123");
    pumpRounds(*pair.host, *pair.client);
    drainSecure(*pair.host);
    drainSecure(*pair.client);

    if (!pair.host->encryptionActive()) return;

    const std::vector<std::uint8_t> payload{ 1, 2, 3, 4, 5, 6, 7, 8 };
    pair.clientRaw->corruptNext = true;      // flip a byte in flight
    pair.client->send(1, payload, SendMode::ReliableOrdered);
    pumpRounds(*pair.host, *pair.client, 2);

    // The GCM auth tag fails: no Data is handed up, and the peer is dropped.
    for (const auto& ev : drainSecure(*pair.host)) {
        CHECK(ev.type != NetEventType::Data);
    }
    CHECK(pair.host->connectionCount() == 0);
}

TEST_CASE("SecureTransport: sending before the handshake completes emits nothing")
{
    auto pair = makeSecurePair("JOINSECRET123", "JOINSECRET123");
    const std::size_t before = pair.clientRaw->sentFrames.size();

    const std::vector<std::uint8_t> payload{ 42 };
    pair.client->send(1, payload, SendMode::ReliableOrdered);

    CHECK(pair.clientRaw->sentFrames.size() == before);   // refused, not queued blindly
}

// ─── End-to-end over real TCP ────────────────────────────────────────────────

namespace {
constexpr MessageId kSecureHello = kFirstUserMessage + 30;
} // namespace

TEST_CASE("SecureTransport: authenticated session over a real TCP link")
{
    auto listener = TcpTransport::listen(0);
    REQUIRE(listener != nullptr);
    const std::uint16_t port = listener->boundPort();

    const std::string secret = SecureTransport::generateJoinSecret();

    auto host = SecureTransport::wrap(std::move(listener),
                    SecureTransport::Config{ secret, NetRole::Host, false });
    REQUIRE(host != nullptr);

    auto clientTcp = TcpTransport::connect("127.0.0.1", port);
    REQUIRE(clientTcp != nullptr);
    auto client = SecureTransport::wrap(std::move(clientTcp),
                      SecureTransport::Config{ secret, NetRole::Client, false });
    REQUIRE(client != nullptr);

    NetSession hostSession(host.get(), NetRole::Host);
    NetSession clientSession(client.get(), NetRole::Client);

    std::string got;
    hostSession.on(kSecureHello, [&](ConnectionId, BitReader& r) { r.readString(got); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool established = false;
    while (std::chrono::steady_clock::now() < deadline && !established) {
        host->update();
        client->update();
        hostSession.pump();
        clientSession.pump();
        established = !hostSession.connections().empty() &&
                      !clientSession.connections().empty();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(established);

    BitWriter payload;
    payload.writeString("authentifiziert");
    clientSession.broadcast(kSecureHello, payload);

    const auto deadline2 = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline2 && got.empty()) {
        host->update();
        client->update();
        hostSession.pump();
        clientSession.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(got == "authentifiziert");
}

TEST_CASE("SecureTransport: a client with the wrong secret cannot join over TCP")
{
    auto listener = TcpTransport::listen(0);
    REQUIRE(listener != nullptr);
    const std::uint16_t port = listener->boundPort();

    auto host = SecureTransport::wrap(std::move(listener),
                    SecureTransport::Config{ "HOST-SECRET", NetRole::Host, false });
    REQUIRE(host != nullptr);

    auto clientTcp = TcpTransport::connect("127.0.0.1", port);
    REQUIRE(clientTcp != nullptr);
    auto client = SecureTransport::wrap(std::move(clientTcp),
                      SecureTransport::Config{ "INTRUDER", NetRole::Client, false });
    REQUIRE(client != nullptr);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        host->update();
        client->update();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    CHECK(host->connectionCount() == 0);
    CHECK(client->connectionCount() == 0);
}
