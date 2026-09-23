#include "doctest.h"

#include <Diagnostics/Log.h>
#include <Net/Socket.h>
#include <Net/UdpTransport.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace HE::Net;

// Every case here runs over real localhost UDP sockets, like test_net_tcp. What
// differs is TIME: both transports share one fake clock that the pump advances
// by a fixed step per round, so retransmission timeouts, keepalives and the
// five-second peer timeout run in milliseconds of wall time, deterministically.
// Loss is injected UNDER the reliability layer through the transport's own drop
// hook — the only way to test that the reliability layer repairs anything.
//
// Two things keep these cases from being flaky:
//   • Sends are interleaved with pumping (a few dozen per round). A thousand
//     datagrams pushed out in one go would overflow the kernel's receive buffer
//     on some platforms and produce loss where the test asserts there is none.
//   • Every wait is bounded in real time; a broken build fails, it never hangs.

namespace {

struct FakeClock {
    std::uint64_t ms = 1'000;
    UdpTransport::ClockFn fn() { return [this] { return ms; }; }
};

// Pump both ends until `done`, advancing the fake clock `stepMs` per round and
// yielding the CPU briefly so localhost datagrams cross the kernel.
template <typename Fn>
bool pumpUntil(UdpTransport& a, UdpTransport& b, FakeClock& clock, Fn done,
               std::uint32_t stepMs = 10,
               std::chrono::milliseconds realTimeout = std::chrono::seconds(20)) {
    const auto deadline = std::chrono::steady_clock::now() + realTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        a.update();
        b.update();
        if (done()) return true;
        clock.ms += stepMs;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    a.update();
    b.update();
    return done();
}

// A few quiet rounds — enough for stragglers to land — without a condition.
void pumpRounds(UdpTransport& a, UdpTransport& b, FakeClock& clock, int rounds,
                std::uint32_t stepMs = 10) {
    for (int i = 0; i < rounds; ++i) {
        a.update();
        b.update();
        clock.ms += stepMs;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

std::vector<NetEvent> drain(UdpTransport& t) {
    std::vector<NetEvent> out;
    NetEvent ev;
    while (t.poll(ev)) out.push_back(std::move(ev));
    return out;
}

struct Pair {
    FakeClock clock;
    std::unique_ptr<UdpTransport> host;
    std::unique_ptr<UdpTransport> client;
    ConnectionId hostConn   = kInvalidConnection;   // the client, as the host sees it
    ConnectionId clientConn = kInvalidConnection;   // the host, as the client sees it
};

// Host + client on localhost, handshake complete, both Connected events drained.
Pair connectedPair(UdpTransport::Config cfg = {}) {
    Pair p;
    p.host = UdpTransport::listen(0, cfg);
    REQUIRE(p.host != nullptr);
    p.host->setClock(p.clock.fn());
    p.client = UdpTransport::connect("127.0.0.1", p.host->boundPort(), cfg);
    REQUIRE(p.client != nullptr);
    p.client->setClock(p.clock.fn());

    REQUIRE(pumpUntil(*p.host, *p.client, p.clock, [&] {
        return p.host->connectionCount() == 1 && p.client->connectionCount() == 1;
    }));
    for (const NetEvent& ev : drain(*p.host)) {
        if (ev.type == NetEventType::Connected) p.hostConn = ev.conn;
    }
    for (const NetEvent& ev : drain(*p.client)) {
        if (ev.type == NetEventType::Connected) p.clientConn = ev.conn;
    }
    REQUIRE(p.hostConn != kInvalidConnection);
    REQUIRE(p.clientConn != kInvalidConnection);
    return p;
}

// Deterministic percentage drop for the transport's test hook.
UdpTransport::TestDatagramFn percentHook(int percent, std::uint32_t seed) {
    auto rng = std::make_shared<std::mt19937>(seed);
    return [rng, percent](const std::uint8_t*, std::size_t) {
        return static_cast<int>((*rng)() % 100) < percent;
    };
}

// Payload: a 4-byte big-endian index followed by filler, so the receiver can
// tell which message arrived and whether it arrived intact.
std::vector<std::uint8_t> indexed(std::uint32_t i, std::size_t size = 32) {
    std::vector<std::uint8_t> v(size, static_cast<std::uint8_t>(i & 0xFF));
    v[0] = static_cast<std::uint8_t>(i >> 24);
    v[1] = static_cast<std::uint8_t>(i >> 16);
    v[2] = static_cast<std::uint8_t>(i >> 8);
    v[3] = static_cast<std::uint8_t>(i);
    return v;
}
std::uint32_t indexOf(const std::vector<std::uint8_t>& v) {
    return (static_cast<std::uint32_t>(v[0]) << 24) | (static_cast<std::uint32_t>(v[1]) << 16) |
           (static_cast<std::uint32_t>(v[2]) << 8)  |  static_cast<std::uint32_t>(v[3]);
}

// Send `count` messages client → host, `perRound` per pump round, and collect
// what the host receives (in arrival order) until `count` arrived or the
// stream has been quiet for `quietRounds`.
// `quietRounds` must outlast the longest retransmission timeout (kRtoMaxMs =
// 2 s of fake time = 200 rounds), or a stream whose last packet is on its
// fourth attempt looks stalled.
std::vector<std::uint32_t> stream(Pair& p, SendMode mode, std::uint32_t count,
                                  std::uint32_t perRound = 40, int quietRounds = 400) {
    std::vector<std::uint32_t> got;
    std::uint32_t sent = 0;
    int quiet = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        for (std::uint32_t i = 0; i < perRound && sent < count; ++i, ++sent) {
            p.client->send(p.clientConn, indexed(sent), mode);
        }
        p.client->update();
        p.host->update();
        bool any = false;
        for (const NetEvent& ev : drain(*p.host)) {
            if (ev.type == NetEventType::Data) { got.push_back(indexOf(ev.data)); any = true; }
        }
        if (got.size() >= count) break;
        quiet = any ? 0 : quiet + 1;
        if (sent >= count && quiet >= quietRounds) break;
        p.clock.ms += 10;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return got;
}

// Captures every log record while alive with the Net category fully open.
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
    std::vector<std::string> lines() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_lines;
    }

private:
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

// A raw datagram in the transport's wire format, for poking the host directly.
std::vector<std::uint8_t> rawDatagram(std::uint8_t version, std::uint8_t flags,
                                      const std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> d;
    d.push_back(static_cast<std::uint8_t>(UdpTransport::kMagic >> 8));
    d.push_back(static_cast<std::uint8_t>(UdpTransport::kMagic & 0xFF));
    d.push_back(version);
    d.push_back(flags);
    d.push_back(UdpTransport::ChannelUnreliable);
    d.insert(d.end(), { 0, 0, 0, 0, 0, 0, 0, 0 });   // seq, ack, ackBits
    d.insert(d.end(), body.begin(), body.end());
    REQUIRE(d.size() == UdpTransport::kHeaderSize + body.size());
    return d;
}

// Pump the host while waiting for one reply on a raw socket. The host only
// answers from update(), and the datagram has to cross the kernel first, so
// the two are interleaved rather than ordered.
bool rawReceive(UdpTransport& host, SocketHandle s, std::vector<std::uint8_t>& out,
                int timeoutMs = 500) {
    for (int waited = 0; waited < timeoutMs; waited += 10) {
        host.update();
        if (!socketWaitReadable(s, 10)) continue;
        out.resize(2048);
        std::size_t got = 0;
        std::string from;
        std::uint16_t fromPort = 0;
        if (socketRecvFrom(s, out.data(), out.size(), got, from, fromPort) != SocketResult::Ok) {
            return false;
        }
        out.resize(got);
        return true;
    }
    return false;
}

} // namespace

// ─── 1. Connection and the cookie ────────────────────────────────────────────

TEST_CASE("UdpTransport: listen + connect come up Connected on both sides")
{
    Pair p = connectedPair();
    CHECK(p.host->isListening());
    CHECK(p.host->boundPort() != 0);
    CHECK(!p.client->isListening());
    CHECK(p.host->stats().cookiesRejected == 0);
}

TEST_CASE("UdpTransport: a Connect without a valid cookie allocates nothing on the host")
{
    Pair p = connectedPair();   // one genuine peer, so "nothing changed" is observable

    SocketHandle raw = socketCreateUdp();
    REQUIRE(raw != kInvalidSocket);
    REQUIRE(socketBindUdp(raw, 0));
    std::size_t sent = 0;

    SUBCASE("first contact is answered statelessly with a Challenge") {
        std::vector<std::uint8_t> nonce(8, 0xAB);
        const auto d = rawDatagram(UdpTransport::kProtocolVersion, UdpTransport::FlagConnect, nonce);
        REQUIRE(socketSendTo(raw, d.data(), d.size(), "127.0.0.1", p.host->boundPort(), sent)
                == SocketResult::Ok);
        p.host->update();
        std::vector<std::uint8_t> reply;
        REQUIRE(rawReceive(*p.host, raw, reply));
        REQUIRE(reply.size() == UdpTransport::kHeaderSize + 16);
        CHECK((reply[3] & UdpTransport::FlagChallenge) != 0);
        // The nonce is echoed, the cookie follows.
        CHECK(std::memcmp(reply.data() + UdpTransport::kHeaderSize, nonce.data(), 8) == 0);
        // …and no peer exists for it.
        CHECK(p.host->connectionCount() == 1);
        CHECK(p.host->stats().cookiesRejected == 0);
    }

    SUBCASE("a forged cookie is counted and ignored, no Reject sent") {
        std::vector<std::uint8_t> body(16, 0x5C);   // nonce + wrong cookie
        const auto d = rawDatagram(UdpTransport::kProtocolVersion, UdpTransport::FlagConnect, body);
        REQUIRE(socketSendTo(raw, d.data(), d.size(), "127.0.0.1", p.host->boundPort(), sent)
                == SocketResult::Ok);
        std::vector<std::uint8_t> reply;
        // Pumps the host for 100 ms: long enough for the datagram to land,
        // and the assertion is that nothing comes back.
        CHECK_FALSE(rawReceive(*p.host, raw, reply, 100));   // silence, not even a Reject
        CHECK(p.host->connectionCount() == 1);
        CHECK(p.host->stats().cookiesRejected == 1);
    }

    SUBCASE("a protocol version mismatch is answered with a Reject") {
        std::vector<std::uint8_t> nonce(8, 0x01);
        const auto d = rawDatagram(UdpTransport::kProtocolVersion + 1, UdpTransport::FlagConnect, nonce);
        REQUIRE(socketSendTo(raw, d.data(), d.size(), "127.0.0.1", p.host->boundPort(), sent)
                == SocketResult::Ok);
        p.host->update();
        std::vector<std::uint8_t> reply;
        REQUIRE(rawReceive(*p.host, raw, reply));
        CHECK((reply[3] & UdpTransport::FlagReject) != 0);
        CHECK(reply.back() == static_cast<std::uint8_t>(UdpTransport::RejectReason::VersionMismatch));
        CHECK(p.host->connectionCount() == 1);
    }

    SUBCASE("a stray datagram from an unknown address gets no answer at all") {
        const auto d = rawDatagram(UdpTransport::kProtocolVersion, 0, indexed(7));
        const std::uint64_t seenBefore = p.host->stats().datagramsReceived;
        REQUIRE(socketSendTo(raw, d.data(), d.size(), "127.0.0.1", p.host->boundPort(), sent)
                == SocketResult::Ok);
        std::vector<std::uint8_t> reply;
        CHECK_FALSE(rawReceive(*p.host, raw, reply, 100));
        CHECK(p.host->stats().datagramsReceived == seenBefore + 1);   // it did arrive
        CHECK(drain(*p.host).empty());
    }

    socketClose(raw);
}

TEST_CASE("UdpTransport: a host that never answers ends in Disconnected after the timeout")
{
    // Nothing listens on this port: the bound socket is closed right away.
    SocketHandle probe = socketCreateUdp();
    REQUIRE(socketBindUdp(probe, 0));
    const std::uint16_t deadPort = socketBoundPort(probe);
    socketClose(probe);

    FakeClock clock;
    UdpTransport::Config cfg;
    cfg.timeoutMs = 2000;
    auto client = UdpTransport::connect("127.0.0.1", deadPort, cfg);
    REQUIRE(client != nullptr);
    client->setClock(clock.fn());

    bool disconnected = false;
    for (int i = 0; i < 300 && !disconnected; ++i) {
        client->update();
        for (const NetEvent& ev : drain(*client)) {
            if (ev.type == NetEventType::Disconnected) disconnected = true;
        }
        clock.ms += 10;
    }
    CHECK(disconnected);
    CHECK(client->connectionCount() == 0);
}

// ─── 2. Unreliable ───────────────────────────────────────────────────────────

TEST_CASE("UdpTransport: unreliable datagrams are lost in proportion and never duplicated")
{
    Pair p = connectedPair();
    p.client->setTestDropFn(percentHook(20, 11));
    p.client->setTestDuplicateFn(percentHook(10, 12));

    const std::vector<std::uint32_t> got = stream(p, SendMode::Unreliable, 1000, 40, 20);

    // 20 % loss on 1000 datagrams: ~800, with a wide margin for the generator.
    CHECK(got.size() >= 700);
    CHECK(got.size() <= 900);

    // Duplicates were injected under the transport…
    CHECK(p.host->stats().duplicatesDropped > 0);
    // …and none of them reached poll(): every index at most once.
    std::set<std::uint32_t> unique(got.begin(), got.end());
    CHECK(unique.size() == got.size());
    // Unreliable never resends.
    CHECK(p.client->stats().resends == 0);
}

// ─── 3. ReliableOrdered ──────────────────────────────────────────────────────

TEST_CASE("UdpTransport: ReliableOrdered delivers everything, in order, through 30 % loss")
{
    Pair p = connectedPair();
    // Both directions lossy: data one way, acks the other.
    p.client->setTestDropFn(percentHook(30, 21));
    p.host->setTestDropFn(percentHook(30, 22));

    const std::vector<std::uint32_t> got = stream(p, SendMode::ReliableOrdered, 1000);

    REQUIRE(got.size() == 1000);
    for (std::uint32_t i = 0; i < 1000; ++i) {
        REQUIRE(got[i] == i);
    }
    CHECK(p.client->stats().resends > 0);
    CHECK(p.client->peerStats(p.clientConn).inFlight <= UdpTransport::kSendWindow);
}

TEST_CASE("UdpTransport: with 0 % loss ReliableOrdered needs no resend at all (negative control)")
{
    // Without this, the case above only proves that resends HAPPEN, not that
    // the loss made them necessary: a layer that resent everything would pass.
    Pair p = connectedPair();
    const std::vector<std::uint32_t> got = stream(p, SendMode::ReliableOrdered, 1000);

    REQUIRE(got.size() == 1000);
    for (std::uint32_t i = 0; i < 1000; ++i) {
        REQUIRE(got[i] == i);
    }
    CHECK(p.client->stats().resends == 0);
    CHECK(p.host->stats().duplicatesDropped == 0);
}

// ─── 4. Reliable, unordered ──────────────────────────────────────────────────

TEST_CASE("UdpTransport: Reliable delivers everything through 30 % loss, order not preserved")
{
    Pair p = connectedPair();
    p.client->setTestDropFn(percentHook(30, 31));
    p.host->setTestDropFn(percentHook(30, 32));

    const std::vector<std::uint32_t> got = stream(p, SendMode::Reliable, 1000);

    REQUIRE(got.size() == 1000);
    std::set<std::uint32_t> unique(got.begin(), got.end());
    CHECK(unique.size() == 1000);   // each exactly once

    // The mode difference must be visible: a resent message lands after
    // messages sent later. If no pair was ever swapped, this test could not
    // tell Reliable from ReliableOrdered.
    bool swapped = false;
    for (std::size_t i = 1; i < got.size(); ++i) {
        if (got[i] < got[i - 1]) { swapped = true; break; }
    }
    CHECK(swapped);
    CHECK(p.client->stats().resends > 0);
}

// ─── 5. Fragmentation ────────────────────────────────────────────────────────

TEST_CASE("UdpTransport: a 40 KiB reliable message arrives whole through 10 % loss")
{
    Pair p = connectedPair();
    p.client->setTestDropFn(percentHook(10, 41));
    p.host->setTestDropFn(percentHook(10, 42));

    std::vector<std::uint8_t> big(40 * 1024);
    std::mt19937 rng(5);
    for (auto& b : big) b = static_cast<std::uint8_t>(rng() & 0xFF);
    REQUIRE(big.size() <= p.client->maxReliableMessage());

    // Three copies: ~105 fragments in all, so "at least one was lost and
    // resent" is a near-certainty rather than a 97 % bet on one message.
    for (int i = 0; i < 3; ++i) p.client->send(p.clientConn, big, SendMode::ReliableOrdered);

    std::vector<std::vector<std::uint8_t>> received;
    REQUIRE(pumpUntil(*p.host, *p.client, p.clock, [&] {
        for (const NetEvent& ev : drain(*p.host)) {
            if (ev.type == NetEventType::Data) received.push_back(ev.data);
        }
        return received.size() >= 3;
    }));
    REQUIRE(received.size() == 3);
    for (const auto& r : received) CHECK(r == big);
    CHECK(p.host->stats().fragmentsReassembled == 3);
    CHECK(p.client->stats().resends > 0);
}

TEST_CASE("UdpTransport: a 40 KiB unreliable message is refused with a log, nothing arrives")
{
    Pair p = connectedPair();
    LogSpy spy;
    std::vector<std::uint8_t> big(40 * 1024, 0x42);
    REQUIRE(big.size() > p.client->maxUnreliableMessage());

    const std::uint64_t sentBefore = p.client->stats().datagramsSent;
    p.client->send(p.clientConn, big, SendMode::Unreliable);
    pumpRounds(*p.host, *p.client, p.clock, 5);

    CHECK(spy.mentions("unreliable send refused"));
    bool anyData = false;
    for (const NetEvent& ev : drain(*p.host)) anyData = anyData || ev.type == NetEventType::Data;
    CHECK_FALSE(anyData);
    // Not even a fragment left: only keepalive-sized traffic since.
    CHECK(p.client->stats().datagramsSent - sentBefore <= 5);
}

// ─── 6. Timeout ──────────────────────────────────────────────────────────────

TEST_CASE("UdpTransport: a silent client is dropped after exactly timeoutMs, not before")
{
    UdpTransport::Config cfg;
    cfg.timeoutMs = 3000;
    Pair p = connectedPair(cfg);

    // The client falls silent: everything it sends is dropped at its socket.
    p.client->setTestDropFn([](const std::uint8_t*, std::size_t) { return true; });
    pumpRounds(*p.host, *p.client, p.clock, 3);

    const std::uint64_t lastHeard = p.host->peerStats(p.hostConn).lastRecvMs;
    REQUIRE(lastHeard != 0);

    p.clock.ms = lastHeard + cfg.timeoutMs - 1;
    p.host->update();
    CHECK(p.host->connectionCount() == 1);
    CHECK(drain(*p.host).empty());

    p.clock.ms = lastHeard + cfg.timeoutMs;
    p.host->update();
    CHECK(p.host->connectionCount() == 0);
    const auto events = drain(*p.host);
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == NetEventType::Disconnected);
    CHECK(events[0].conn == p.hostConn);
}

// ─── 7. Keepalive ────────────────────────────────────────────────────────────

TEST_CASE("UdpTransport: keepalives hold an idle link open for 30 seconds")
{
    Pair p = connectedPair();
    const std::uint64_t hostSentBefore   = p.host->stats().datagramsSent;
    const std::uint64_t clientSentBefore = p.client->stats().datagramsSent;

    // Steps well under keepAliveMs (250), so keepalives actually go out
    // instead of both sides jumping straight past the timeout.
    for (int i = 0; i < 300; ++i) {
        pumpRounds(*p.host, *p.client, p.clock, 1, 100);
    }
    CHECK(p.host->connectionCount() == 1);
    CHECK(p.client->connectionCount() == 1);
    CHECK(drain(*p.host).empty());
    CHECK(drain(*p.client).empty());
    // ~100 keepalives per side in 30 s at 250 ms with a 100 ms step (one
    // every third round); the exact count depends on phase, so a floor is
    // asserted, and a ceiling that rules out "one per round".
    CHECK(p.host->stats().datagramsSent - hostSentBefore >= 90);
    CHECK(p.host->stats().datagramsSent - hostSentBefore <= 160);
    CHECK(p.client->stats().datagramsSent - clientSentBefore >= 90);
    CHECK(p.client->stats().datagramsSent - clientSentBefore <= 160);
}

// ─── 8. Window and backlog ───────────────────────────────────────────────────

TEST_CASE("UdpTransport: a peer that acknowledges nothing is dropped once the backlog is full")
{
    Pair p = connectedPair();
    // Total silence from the client's socket: no ack ever returns, so the
    // window fills and the queue behind it grows. The clock stands still so
    // the peer timeout cannot be what ends this.
    p.client->setTestDropFn([](const std::uint8_t*, std::size_t) { return true; });

    const std::size_t msgSize = 1000;
    bool disconnected = false;
    std::size_t sentMessages = 0;
    for (; sentMessages < 1000 && !disconnected; ++sentMessages) {
        p.client->send(p.clientConn, indexed(static_cast<std::uint32_t>(sentMessages), msgSize),
                       SendMode::Reliable);
        for (const NetEvent& ev : drain(*p.client)) {
            if (ev.type == NetEventType::Disconnected) disconnected = true;
        }
    }
    REQUIRE(disconnected);
    CHECK(p.client->connectionCount() == 0);
    // The window (256 packets) plus the 64 KiB allowed behind it.
    const std::size_t limitMessages = UdpTransport::kSendWindow + (64 * 1024) / msgSize;
    CHECK(sentMessages >= limitMessages);
    CHECK(sentMessages <= limitMessages + 2);

    // After the drop, send is a no-op: nothing leaves and nothing queues.
    const std::uint64_t sentBefore = p.client->stats().datagramsSent;
    p.client->send(p.clientConn, indexed(1), SendMode::Reliable);
    p.client->update();
    CHECK(p.client->stats().datagramsSent == sentBefore);
    CHECK(p.client->peerStats(p.clientConn).queuedBytes == 0);
}

// ─── 9. RTO ──────────────────────────────────────────────────────────────────

TEST_CASE("UdpTransport: the RTT estimator follows samples and the RTO backs off after an outlier")
{
    Pair p = connectedPair();

    // One reliable message, acknowledged after exactly `rttMs` of fake time.
    // Real time only moves the datagrams across the kernel; the clock is what
    // the estimator reads, and it moves exactly once, by rttMs.
    auto settle = [&](UdpTransport& t, auto pred) {
        bool ok = pred();
        for (int i = 0; i < 200 && !ok; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            t.update();
            ok = pred();
        }
        REQUIRE(ok);
    };
    auto sample = [&](std::uint32_t rttMs) {
        p.client->send(p.clientConn, indexed(1), SendMode::Reliable);
        settle(*p.host, [&] { return !drain(*p.host).empty(); });   // received; ack pending
        p.clock.ms += rttMs;
        p.host->update();                       // ack-only goes out (kAckDelayMs passed)
        settle(*p.client, [&] { return p.client->peerStats(p.clientConn).inFlight == 0; });
    };

    sample(100);
    sample(100);
    sample(100);
    const UdpTransport::PeerStats steady = p.client->peerStats(p.clientConn);
    CHECK(steady.srttMs == doctest::Approx(100.f).epsilon(0.01));
    // rttvar decays from 50 towards 0: 50 → 37.5 → 28.1; rto = srtt + 4·rttvar.
    CHECK(steady.rtoMs > 200.f);
    CHECK(steady.rtoMs < 230.f);
    CHECK(p.client->stats().srttMs == doctest::Approx(100.f).epsilon(0.01));

    sample(400);
    const UdpTransport::PeerStats spiked = p.client->peerStats(p.clientConn);
    CHECK(spiked.srttMs > 130.f);
    CHECK(spiked.srttMs < 145.f);
    CHECK(spiked.rtoMs > steady.rtoMs + 200.f);   // the outlier widened it

    sample(100);
    sample(100);
    const UdpTransport::PeerStats settling = p.client->peerStats(p.clientConn);
    CHECK(settling.rtoMs < spiked.rtoMs);          // …and it comes back down
    CHECK(p.client->stats().resends == 0);         // no retransmission was ever needed
}

// ─── 10. Disconnect ──────────────────────────────────────────────────────────

TEST_CASE("UdpTransport: disconnect() tells the peer and emits nothing locally")
{
    Pair p = connectedPair();
    p.client->disconnect(p.clientConn);
    CHECK(p.client->connectionCount() == 0);
    CHECK(drain(*p.client).empty());   // the initiator gets no Disconnected

    REQUIRE(pumpUntil(*p.host, *p.client, p.clock, [&] {
        return p.host->connectionCount() == 0;
    }));
    const auto events = drain(*p.host);
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == NetEventType::Disconnected);
    CHECK(events[0].conn == p.hostConn);
}

TEST_CASE("UdpTransport: send to an unknown connection is a no-op")
{
    Pair p = connectedPair();
    const std::uint64_t before = p.client->stats().datagramsSent;
    p.client->send(999, indexed(1), SendMode::ReliableOrdered);
    p.client->send(999, indexed(1), SendMode::Unreliable);
    CHECK(p.client->stats().datagramsSent == before);
}

// ─── 11. Log hygiene ─────────────────────────────────────────────────────────

TEST_CASE("UdpTransport: the log carries no cookie material and no nonce")
{
    LogSpy spy;
    Pair p = connectedPair();
    p.client->send(p.clientConn, indexed(1), SendMode::ReliableOrdered);
    p.client->send(p.clientConn, indexed(2), SendMode::Unreliable);
    pumpRounds(*p.host, *p.client, p.clock, 10);

    CHECK(spy.count() > 0);   // the check below would be vacuous otherwise
    CHECK(spy.mentions("UDP accepted"));
    CHECK_FALSE(spy.mentions("nonce"));
    CHECK_FALSE(spy.mentions("secret"));
    // "cookie" may appear in prose ("cookie returned"), never with a value: a
    // 64-bit cookie or nonce renders as at least 16 hex or ~19 decimal digits,
    // and no legitimate line about it carries a run of ten.
    for (const std::string& line : spy.lines()) {
        if (line.find("cookie") == std::string::npos) continue;
        std::size_t run = 0, longest = 0;
        for (const char c : line) {
            const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                             (c >= 'A' && c <= 'F');
            run = hex ? run + 1 : 0;
            longest = std::max(longest, run);
        }
        CHECK_MESSAGE(longest < 10, "log line carries a cookie value: " << line);
    }
}
