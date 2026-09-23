#include "doctest.h"

#include <Diagnostics/Log.h>
#include <Net/NetSession.h>
#include <Net/SecureTransport.h>
#include <Net/TcpTransport.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
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

// Captures every log record emitted while it is alive, and turns the Net
// category all the way up so nothing is filtered out before it is seen. Both
// halves matter: at the default Info verbosity a Trace-level leak would slip
// past the test unnoticed.
class LogSpy {
public:
    LogSpy() : m_previous(HE::Log::verbosity(HE::Log::Cat::Net)) {
        HE::Log::setVerbosity(HE::Log::Cat::Net, HE::LogLevel::Trace);
        m_handle = HE::Log::addSink(&LogSpy::onRecord, this);
    }
    ~LogSpy() {
        HE::Log::removeSink(m_handle);
        HE::Log::setVerbosity(HE::Log::Cat::Net, m_previous);
    }
    LogSpy(const LogSpy&)            = delete;
    LogSpy& operator=(const LogSpy&) = delete;

    bool mentions(const std::string& needle) const {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (const std::string& line : m_lines) {
            if (line.find(needle) != std::string::npos) return true;
        }
        return false;
    }
    std::size_t count() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_lines.size();
    }

private:
    // Sinks run on the logging thread with the log mutex held, so this only
    // copies the text and never logs itself.
    static void onRecord(const HE::Log::Record& rec, void* user) {
        auto* self = static_cast<LogSpy*>(user);
        std::lock_guard<std::mutex> lk(self->m_mutex);
        self->m_lines.emplace_back(rec.message ? rec.message : "");
    }

    mutable std::mutex       m_mutex;
    std::vector<std::string> m_lines;
    int                      m_handle = 0;
    HE::LogLevel             m_previous;
};

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

// ─── Logging must never leak the material it handles ─────────────────────────

TEST_CASE("SecureTransport: the join secret never appears in the log")
{
    // Deliberately distinctive so a substring search cannot match by accident,
    // and long enough that no formatting would ever produce it incidentally.
    const std::string secret = "ZZTOPSECRET7NEVERLOGME9XYZQ";

    LogSpy spy;
    SpyPair p = makeSecurePair(secret, secret);
    REQUIRE(p.host);
    REQUIRE(p.client);
    pumpRounds(*p.host, *p.client);

    // Sanity: the run has to have logged *something*, or this test would pass
    // simply because logging is switched off.
    CHECK(spy.count() > 0);
    CHECK_FALSE(spy.mentions(secret));

    // The shape may be reported — that is the deliberate redaction — but only
    // as a length, never as any part of the value itself.
    CHECK(spy.mentions("<27 chars>"));
}

TEST_CASE("SecureTransport: a failed handshake logs the reason without the secret")
{
    const std::string hostSecret   = "AAAHOSTSECRET1234567890XYZ";
    const std::string clientSecret = "BBBCLIENTSECRET098765432ZY";

    LogSpy spy;
    SpyPair p = makeSecurePair(hostSecret, clientSecret);
    REQUIRE(p.host);
    REQUIRE(p.client);
    pumpRounds(*p.host, *p.client);

    // Neither side may end up authenticated…
    CHECK(p.host->stateOf(1) != HandshakeState::Established);
    // …and the log must say *why* in words a user can act on. Before the reason
    // was threaded through failPeer, every rejection produced an identical
    // message and a wrong join code was indistinguishable from a version skew.
    CHECK(spy.mentions("join code"));

    CHECK_FALSE(spy.mentions(hostSecret));
    CHECK_FALSE(spy.mentions(clientSecret));
}

// ─── Anti-replay window (the UDP mode) ───────────────────────────────────────
// Over UdpTransport frames arrive out of order, and "counter must strictly
// increase" would drop the connection on every swapped pair. With
// Config::replayWindow set, the receiver keeps a window instead: a reorder
// inside it is accepted, a replay is dropped and the connection lives. The
// exact-distance cases use the spy endpoint (its inbound queue can be
// rearranged by hand); the statistical case runs over LossyTransport.

#include <Net/LoopbackTransport.h>
#include <Net/LossyTransport.h>

namespace {

SpyPair makeWindowedPair(const std::string& secret, std::uint8_t window) {
    auto a = std::make_unique<SpyEndpoint>();
    auto b = std::make_unique<SpyEndpoint>();
    SpyEndpoint* aRaw = a.get();
    SpyEndpoint* bRaw = b.get();
    aRaw->peer = bRaw;
    bRaw->peer = aRaw;
    aRaw->inbound.push_back(NetEvent{ NetEventType::Connected, 1, {} });
    bRaw->inbound.push_back(NetEvent{ NetEventType::Connected, 1, {} });

    SecureTransport::Config hostCfg;
    hostCfg.joinSecret        = secret;
    hostCfg.role              = NetRole::Host;
    hostCfg.requireEncryption = false;
    hostCfg.replayWindow      = window;
    SecureTransport::Config clientCfg = hostCfg;
    clientCfg.role = NetRole::Client;

    SpyPair out;
    out.hostRaw   = aRaw;
    out.clientRaw = bRaw;
    out.host   = SecureTransport::wrap(std::move(a), hostCfg);
    out.client = SecureTransport::wrap(std::move(b), clientCfg);
    REQUIRE(out.host);
    REQUIRE(out.client);
    pumpRounds(*out.host, *out.client);
    drainSecure(*out.host);
    drainSecure(*out.client);
    REQUIRE(out.host->stateOf(1) == HandshakeState::Established);
    REQUIRE(out.client->stateOf(1) == HandshakeState::Established);
    return out;
}

std::vector<std::uint8_t> numbered(std::uint32_t i) {
    return { static_cast<std::uint8_t>(i >> 24), static_cast<std::uint8_t>(i >> 16),
             static_cast<std::uint8_t>(i >> 8),  static_cast<std::uint8_t>(i) };
}
std::uint32_t numberOf(const std::vector<std::uint8_t>& v) {
    return (static_cast<std::uint32_t>(v[0]) << 24) | (static_cast<std::uint32_t>(v[1]) << 16) |
           (static_cast<std::uint32_t>(v[2]) << 8)  |  static_cast<std::uint32_t>(v[3]);
}

// Host sends `count` frames; they queue in the client's raw inbound deque
// WITHOUT being processed, so the test can rearrange them before the client
// pumps.
void queueFrames(SpyPair& p, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i) {
        p.host->send(1, numbered(i), SendMode::ReliableOrdered);
    }
    REQUIRE(p.clientRaw->inbound.size() == count);
}

std::vector<std::uint32_t> receivedNumbers(SecureTransport& t) {
    std::vector<std::uint32_t> out;
    for (const NetEvent& ev : drainSecure(t)) {
        if (ev.type == NetEventType::Data) out.push_back(numberOf(ev.data));
    }
    return out;
}

} // namespace

TEST_CASE("SecureTransport: with a replay window a reorder inside the window is accepted")
{
    SpyPair p = makeWindowedPair("WINDOWSECRET123456789ABCDE", SecureTransport::kReplayWindowFrames);
    if (!p.host->encryptionActive()) return;

    // 40 frames; the first one is moved to the back: it arrives 39 later.
    queueFrames(p, 40);
    NetEvent first = std::move(p.clientRaw->inbound.front());
    p.clientRaw->inbound.pop_front();
    p.clientRaw->inbound.push_back(std::move(first));

    p.client->update();
    const auto got = receivedNumbers(*p.client);
    REQUIRE(got.size() == 40);
    CHECK(got.back() == 0);                                 // late, but delivered
    CHECK(p.client->connectionCount() == 1);
    CHECK(p.client->statsOf(1).framesAccepted == 40);
    CHECK(p.client->statsOf(1).replaysDropped == 0);
    CHECK(p.client->statsOf(1).staleDropped == 0);
}

TEST_CASE("SecureTransport: with a replay window a replayed frame is dropped and the link lives")
{
    SpyPair p = makeWindowedPair("WINDOWSECRET123456789ABCDE", SecureTransport::kReplayWindowFrames);
    if (!p.host->encryptionActive()) return;

    queueFrames(p, 10);
    // Frame 3 again, verbatim, behind the rest.
    p.clientRaw->inbound.push_back(p.clientRaw->inbound[3]);

    p.client->update();
    const auto got = receivedNumbers(*p.client);
    REQUIRE(got.size() == 10);                              // exactly once each
    for (std::uint32_t i = 0; i < 10; ++i) CHECK(got[i] == i);
    CHECK(p.client->statsOf(1).replaysDropped == 1);
    CHECK(p.client->connectionCount() == 1);

    // Still usable afterwards: the next frame goes through.
    p.host->send(1, numbered(99), SendMode::ReliableOrdered);
    p.client->update();
    const auto after = receivedNumbers(*p.client);
    REQUIRE(after.size() == 1);
    CHECK(after[0] == 99);
}

TEST_CASE("SecureTransport: a frame older than the window is dropped, the link lives")
{
    SpyPair p = makeWindowedPair("WINDOWSECRET123456789ABCDE", SecureTransport::kReplayWindowFrames);
    if (!p.host->encryptionActive()) return;

    // Frame 0 held back behind 100 others: distance 100 > window 64.
    queueFrames(p, 101);
    NetEvent first = std::move(p.clientRaw->inbound.front());
    p.clientRaw->inbound.pop_front();
    p.clientRaw->inbound.push_back(std::move(first));

    p.client->update();
    const auto got = receivedNumbers(*p.client);
    REQUIRE(got.size() == 100);
    for (const std::uint32_t n : got) CHECK(n != 0);
    CHECK(p.client->statsOf(1).staleDropped == 1);
    CHECK(p.client->statsOf(1).replaysDropped == 0);
    CHECK(p.client->connectionCount() == 1);
}

TEST_CASE("SecureTransport: without a window a single swapped pair still fails the peer (negative control)")
{
    // replayWindow = 0 is the default every existing consumer runs with, and
    // the cases above would prove nothing if this one did not fail.
    SpyPair p = makeWindowedPair("WINDOWSECRET123456789ABCDE", 0);
    if (!p.host->encryptionActive()) return;

    LogSpy spy;
    queueFrames(p, 2);
    std::swap(p.clientRaw->inbound[0], p.clientRaw->inbound[1]);

    p.client->update();
    const auto got = receivedNumbers(*p.client);
    CHECK(got.size() == 1);                                 // the first (out-of-order) one
    CHECK(p.client->connectionCount() == 0);                // then the link is gone
    CHECK(spy.mentions("did not increase"));
}

TEST_CASE("SecureTransport: 1000 frames through LossyTransport reorder and duplication with a window")
{
    auto [la, lb] = LoopbackTransport::createPair();
    // The handshake runs over a CLEAN link and the mistreatment starts after
    // it: there is no reliability under this decorator, so a lost or
    // duplicated handshake frame is a failed handshake — which over the real
    // UdpTransport cannot happen, because that one deduplicates and resends
    // underneath. What is under test is the data path.
    auto lossyHost   = LossyTransport::wrap(std::move(la), {});
    auto lossyClient = LossyTransport::wrap(std::move(lb), {});
    LossyTransport* hostRaw   = lossyHost.get();
    LossyTransport* clientRaw = lossyClient.get();

    SecureTransport::Config hostCfg;
    hostCfg.joinSecret        = "LOSSYSECRET123456789ABCDEF";
    hostCfg.role              = NetRole::Host;
    hostCfg.requireEncryption = false;
    hostCfg.replayWindow      = SecureTransport::kReplayWindowFrames;
    SecureTransport::Config clientCfg = hostCfg;
    clientCfg.role = NetRole::Client;

    auto host   = SecureTransport::wrap(std::move(lossyHost), hostCfg);
    auto client = SecureTransport::wrap(std::move(lossyClient), clientCfg);
    REQUIRE(host);
    REQUIRE(client);

    // Handshake: the reorder delay means a few simulated ms have to pass.
    for (int i = 0; i < 200 && (host->stateOf(1) != HandshakeState::Established ||
                                client->stateOf(1) != HandshakeState::Established); ++i) {
        host->update();
        client->update();
        hostRaw->advance(1);
        clientRaw->advance(1);
    }
    REQUIRE(host->stateOf(1) == HandshakeState::Established);
    REQUIRE(client->stateOf(1) == HandshakeState::Established);
    drainSecure(*host);
    drainSecure(*client);
    if (!host->encryptionActive()) return;

    // Reorder up to 30 sends back, 5 % duplicates, no loss.
    LossyTransport::Config bad;
    bad.reorderPercent   = 30.f;
    bad.reorderDelayMs   = 30;
    bad.duplicatePercent = 5.f;
    bad.seed             = 99;
    hostRaw->setConfig(bad);

    // One frame per simulated millisecond, so reorder distance ≤ 30 frames.
    // Unreliable, because that is the mode the decorator mistreats, and the
    // realistic case for the window: snapshots over SecureTransport over UDP.
    std::vector<std::uint32_t> got;
    for (std::uint32_t i = 0; i < 1000; ++i) {
        host->send(1, numbered(i), SendMode::Unreliable);
        hostRaw->advance(1);
        host->update();
        client->update();
        for (const std::uint32_t n : receivedNumbers(*client)) got.push_back(n);
    }
    hostRaw->flush();
    client->update();
    for (const std::uint32_t n : receivedNumbers(*client)) got.push_back(n);

    // Every frame exactly once, connection intact, duplicates caught by the window.
    REQUIRE(got.size() == 1000);
    std::vector<std::uint32_t> sorted = got;
    std::sort(sorted.begin(), sorted.end());
    for (std::uint32_t i = 0; i < 1000; ++i) REQUIRE(sorted[i] == i);
    CHECK(got != sorted);                                   // reorder really happened
    CHECK(client->connectionCount() == 1);
    CHECK(client->statsOf(1).framesAccepted == 1000);
    CHECK(client->statsOf(1).replaysDropped == hostRaw->stats().duplicated);
    CHECK(client->statsOf(1).replaysDropped > 0);
    CHECK(client->statsOf(1).staleDropped == 0);
}
