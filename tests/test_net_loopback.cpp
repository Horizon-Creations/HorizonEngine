#include "doctest.h"

#include <Net/BitStream.h>
#include <Net/LoopbackTransport.h>
#include <Net/NetSession.h>

#include <string>
#include <vector>

using namespace HE::Net;

// ─── Raw transport behaviour ──────────────────────────────────────────────────

TEST_CASE("Loopback: pair comes up connected and reports a peer")
{
    auto [a, b] = LoopbackTransport::createPair();

    CHECK(a->connectionCount() == 1);
    CHECK(b->connectionCount() == 1);

    // Each side observes a Connected event first.
    NetEvent ev;
    REQUIRE(a->poll(ev));
    CHECK(ev.type == NetEventType::Connected);
    CHECK(ev.conn == LoopbackTransport::kPeer);
    CHECK_FALSE(a->poll(ev));   // nothing else queued
}

TEST_CASE("Loopback: datagram sent on one end arrives on the other")
{
    auto [a, b] = LoopbackTransport::createPair();

    // Drain the initial Connected events.
    NetEvent ev;
    while (a->poll(ev)) {}
    while (b->poll(ev)) {}

    const std::vector<std::uint8_t> payload{ 1, 2, 3, 4, 5 };
    a->send(LoopbackTransport::kPeer, payload, SendMode::ReliableOrdered);

    REQUIRE(b->poll(ev));
    CHECK(ev.type == NetEventType::Data);
    CHECK(ev.conn == LoopbackTransport::kPeer);
    CHECK(ev.data == payload);

    // The sender's own queue stays empty — no echo.
    CHECK_FALSE(a->poll(ev));
}

TEST_CASE("Loopback: disconnect notifies the peer, not the initiator")
{
    auto [a, b] = LoopbackTransport::createPair();
    NetEvent ev;
    while (a->poll(ev)) {}
    while (b->poll(ev)) {}

    a->disconnect(LoopbackTransport::kPeer);

    CHECK(a->connectionCount() == 0);
    CHECK_FALSE(a->poll(ev));            // initiator gets no local event

    REQUIRE(b->poll(ev));                // peer learns the link dropped
    CHECK(ev.type == NetEventType::Disconnected);
    CHECK(b->connectionCount() == 0);
}

TEST_CASE("Loopback: destroying one end tears the link down safely")
{
    auto pair = LoopbackTransport::createPair();
    auto a = std::move(pair.first);
    auto b = std::move(pair.second);

    NetEvent ev;
    while (a->poll(ev)) {}
    while (b->poll(ev)) {}

    a.reset();   // destroy one endpoint

    CHECK(b->connectionCount() == 0);
    REQUIRE(b->poll(ev));
    CHECK(ev.type == NetEventType::Disconnected);

    // Sending into the void is a no-op, not a crash.
    b->send(LoopbackTransport::kPeer, { 9, 9 }, SendMode::Reliable);
    CHECK_FALSE(b->poll(ev));
}

// ─── End-to-end through NetSession ────────────────────────────────────────────

namespace {
constexpr MessageId kChat     = kFirstUserMessage + 0;
constexpr MessageId kPingOnly = kFirstUserMessage + 1;
} // namespace

TEST_CASE("NetSession: typed message dispatches to the registered handler")
{
    auto [ta, tb] = LoopbackTransport::createPair();
    NetSession server(ta.get(), NetRole::Host);
    NetSession client(tb.get(), NetRole::Client);

    std::string  gotText;
    int          gotCount = 0;
    ConnectionId gotFrom  = kInvalidConnection;

    server.on(kChat, [&](ConnectionId from, BitReader& r) {
        std::string text;
        std::uint32_t count = 0;
        r.readString(text);
        r.readUInt32(count);
        gotText  = text;
        gotCount = static_cast<int>(count);
        gotFrom  = from;
    });

    // Pump both once so each session records its peer via the Connected event.
    server.pump();
    client.pump();
    REQUIRE(client.connections().size() == 1);

    BitWriter payload;
    payload.writeString("hello server");
    payload.writeUInt32(7);
    client.send(LoopbackTransport::kPeer, kChat, payload);

    server.pump();   // dispatch

    CHECK(gotText == "hello server");
    CHECK(gotCount == 7);
    CHECK(gotFrom == LoopbackTransport::kPeer);
}

TEST_CASE("NetSession: connect/disconnect callbacks and peer list track links")
{
    auto [ta, tb] = LoopbackTransport::createPair();
    NetSession server(ta.get(), NetRole::Host);

    int connects = 0, disconnects = 0;
    server.onConnect([&](ConnectionId) { ++connects; });
    server.onDisconnect([&](ConnectionId) { ++disconnects; });

    server.pump();
    CHECK(connects == 1);
    CHECK(server.connections().size() == 1);

    // Client drops.
    tb.reset();
    server.pump();
    CHECK(disconnects == 1);
    CHECK(server.connections().empty());
}

TEST_CASE("NetSession: id-only message (no payload) dispatches")
{
    auto [ta, tb] = LoopbackTransport::createPair();
    NetSession server(ta.get(), NetRole::Host);
    NetSession client(tb.get(), NetRole::Client);

    bool pinged = false;
    server.on(kPingOnly, [&](ConnectionId, BitReader&) { pinged = true; });

    server.pump();
    client.pump();

    client.send(LoopbackTransport::kPeer, kPingOnly);
    server.pump();

    CHECK(pinged);
}

TEST_CASE("NetSession: unregistered message id is dropped without error")
{
    auto [ta, tb] = LoopbackTransport::createPair();
    NetSession server(ta.get(), NetRole::Host);
    NetSession client(tb.get(), NetRole::Client);

    server.pump();
    client.pump();

    // No handler registered for this id.
    client.send(LoopbackTransport::kPeer, kFirstUserMessage + 99);
    server.pump();   // must not crash

    CHECK(server.connections().size() == 1);
}

// ─── One frame per message, not per peer (NET-102) ───────────────────────────

namespace {

// Records what NetSession hands down; three peers are "connected" on the first
// poll so the session's peer list has someone to broadcast to.
class CaptureTransport final : public ITransport {
public:
    struct Sent { ConnectionId conn; std::vector<std::uint8_t> bytes; };
    std::vector<Sent>     sent;
    std::vector<NetEvent> pending{ { NetEventType::Connected, 1, {} },
                                   { NetEventType::Connected, 2, {} },
                                   { NetEventType::Connected, 3, {} } };

    void update() override {}
    void send(ConnectionId conn, const std::uint8_t* data, std::size_t len,
              SendMode) override {
        sent.push_back({ conn, std::vector<std::uint8_t>(data, data + len) });
    }
    bool poll(NetEvent& out) override {
        if (pending.empty()) return false;
        out = std::move(pending.front());
        pending.erase(pending.begin());
        return true;
    }
    void disconnect(ConnectionId) override {}
    std::size_t connectionCount() const override { return 3; }
};

} // namespace

TEST_CASE("NetSession: a frame is [id low][id high][payload], the same for every peer")
{
    CaptureTransport t;
    NetSession s(&t, NetRole::Host);
    s.pump();
    REQUIRE(s.connections().size() == 3);

    // Pinned by hand: id 0x1234 goes out low byte first; the payload's three
    // bits 0b101 sit zero-padded in one byte behind it.
    BitWriter partial;
    partial.writeBits(0b101u, 3);
    s.send(2, 0x1234, partial);
    REQUIRE(t.sent.size() == 1);
    CHECK(t.sent[0].conn == 2);
    CHECK(t.sent[0].bytes == std::vector<std::uint8_t>{ 0x34, 0x12, 0x05 });

    // No payload: just the id.
    t.sent.clear();
    s.send(1, 0xBEEF);
    REQUIRE(t.sent.size() == 1);
    CHECK(t.sent[0].bytes == std::vector<std::uint8_t>{ 0xEF, 0xBE });

    // Broadcast, broadcast-except and sendTo hand every peer the identical frame.
    BitWriter big;
    big.writeBool(true);   // unaligned bulk write behind it
    std::vector<std::uint8_t> blob(1000);
    for (std::size_t i = 0; i < blob.size(); ++i) blob[i] = static_cast<std::uint8_t>(i * 7u);
    big.writeBytes(blob.data(), blob.size());
    std::vector<std::uint8_t> want{ 0x01, 0x02 };
    big.appendTo(want);

    t.sent.clear();
    s.broadcast(0x0201, big);
    REQUIRE(t.sent.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(t.sent[i].conn == static_cast<ConnectionId>(i + 1));
        CHECK(t.sent[i].bytes == want);
    }

    t.sent.clear();
    s.broadcastExcept(2, 0x0201, big);
    REQUIRE(t.sent.size() == 2);
    CHECK(t.sent[0].conn == 1);
    CHECK(t.sent[1].conn == 3);
    CHECK(t.sent[0].bytes == want);
    CHECK(t.sent[1].bytes == want);

    t.sent.clear();
    s.sendTo({ 3, 1 }, 0x0201, big);
    REQUIRE(t.sent.size() == 2);
    CHECK(t.sent[0].conn == 3);
    CHECK(t.sent[1].conn == 1);
    CHECK(t.sent[1].bytes == want);

    // Nobody to send to is not an error.
    t.sent.clear();
    s.sendTo({}, 0x0201, big);
    s.broadcastExcept(1, 0x0201, big);   // still two others
    CHECK(t.sent.size() == 2);
}
