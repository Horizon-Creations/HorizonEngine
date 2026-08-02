#include "doctest.h"

#include <Net/BitStream.h>
#include <Net/NetSession.h>
#include <Net/TcpTransport.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace HE::Net;

namespace {

// Pump both ends until `done` reports true or the deadline passes. Real sockets
// are asynchronous, so every assertion has to be driven rather than assumed —
// but a bounded wait keeps a broken build from hanging the suite.
template <typename Fn>
bool pumpUntil(TcpTransport& a, TcpTransport& b, Fn done,
               std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        a.update();
        b.update();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    a.update();
    b.update();
    return done();
}

// Drain a transport's event queue, collecting everything seen.
std::vector<NetEvent> drain(TcpTransport& t) {
    std::vector<NetEvent> out;
    NetEvent ev;
    while (t.poll(ev)) out.push_back(std::move(ev));
    return out;
}

} // namespace

// ─── Bind / connect lifecycle ────────────────────────────────────────────────

TEST_CASE("TcpTransport: listening on port 0 binds an ephemeral port")
{
    auto server = TcpTransport::listen(0);
    REQUIRE(server != nullptr);
    CHECK(server->isListening());
    CHECK(server->boundPort() != 0);
}

TEST_CASE("TcpTransport: client connects and both sides report Connected")
{
    auto server = TcpTransport::listen(0);
    REQUIRE(server != nullptr);

    auto client = TcpTransport::connect("127.0.0.1", server->boundPort());
    REQUIRE(client != nullptr);

    const bool up = pumpUntil(*server, *client, [&] {
        return server->connectionCount() == 1 && client->connectionCount() == 1;
    });
    REQUIRE(up);

    const auto serverEvents = drain(*server);
    const auto clientEvents = drain(*client);

    REQUIRE(serverEvents.size() >= 1);
    CHECK(serverEvents[0].type == NetEventType::Connected);
    REQUIRE(clientEvents.size() >= 1);
    CHECK(clientEvents[0].type == NetEventType::Connected);
}

TEST_CASE("TcpTransport: connecting to a closed port surfaces a drop, not a hang")
{
    // Bind then immediately release, so the port is almost certainly unused.
    std::uint16_t deadPort = 0;
    {
        auto probe = TcpTransport::listen(0);
        REQUIRE(probe != nullptr);
        deadPort = probe->boundPort();
    }

    auto client = TcpTransport::connect("127.0.0.1", deadPort);
    if (!client) return;   // immediate refusal is an acceptable outcome too

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool sawDisconnect = false;
    while (std::chrono::steady_clock::now() < deadline && !sawDisconnect) {
        client->update();
        NetEvent ev;
        while (client->poll(ev)) {
            if (ev.type == NetEventType::Disconnected) sawDisconnect = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(sawDisconnect);
    CHECK(client->connectionCount() == 0);
}

// ─── Data transfer ───────────────────────────────────────────────────────────

TEST_CASE("TcpTransport: datagrams round-trip in both directions")
{
    auto server = TcpTransport::listen(0);
    REQUIRE(server != nullptr);
    auto client = TcpTransport::connect("127.0.0.1", server->boundPort());
    REQUIRE(client != nullptr);

    REQUIRE(pumpUntil(*server, *client, [&] {
        return server->connectionCount() == 1 && client->connectionCount() == 1;
    }));

    ConnectionId serverSideConn = kInvalidConnection;
    for (const auto& ev : drain(*server)) {
        if (ev.type == NetEventType::Connected) serverSideConn = ev.conn;
    }
    REQUIRE(serverSideConn != kInvalidConnection);
    drain(*client);

    // Client → server.
    const std::vector<std::uint8_t> up{ 1, 2, 3, 4, 5 };
    client->send(1, up, SendMode::ReliableOrdered);

    std::vector<NetEvent> serverGot;
    REQUIRE(pumpUntil(*server, *client, [&] {
        for (auto& ev : drain(*server)) serverGot.push_back(std::move(ev));
        return !serverGot.empty();
    }));
    REQUIRE(serverGot.size() == 1);
    CHECK(serverGot[0].type == NetEventType::Data);
    CHECK(serverGot[0].data == up);

    // Server → client.
    const std::vector<std::uint8_t> down{ 9, 8, 7 };
    server->send(serverSideConn, down, SendMode::ReliableOrdered);

    std::vector<NetEvent> clientGot;
    REQUIRE(pumpUntil(*server, *client, [&] {
        for (auto& ev : drain(*client)) clientGot.push_back(std::move(ev));
        return !clientGot.empty();
    }));
    REQUIRE(clientGot.size() == 1);
    CHECK(clientGot[0].data == down);
}

TEST_CASE("TcpTransport: message boundaries survive stream fragmentation")
{
    auto server = TcpTransport::listen(0);
    REQUIRE(server != nullptr);
    auto client = TcpTransport::connect("127.0.0.1", server->boundPort());
    REQUIRE(client != nullptr);

    REQUIRE(pumpUntil(*server, *client, [&] {
        return server->connectionCount() == 1 && client->connectionCount() == 1;
    }));
    drain(*server);
    drain(*client);

    // A payload far larger than one recv chunk plus small ones behind it: TCP
    // will split and coalesce these arbitrarily, so this is the real test that
    // length-prefix framing reassembles correctly.
    std::vector<std::uint8_t> big(200 * 1024);
    for (std::size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<std::uint8_t>(i * 31u);
    }
    const std::vector<std::uint8_t> small1{ 0xAA };
    const std::vector<std::uint8_t> small2{ 0xBB, 0xCC };

    client->send(1, big,    SendMode::ReliableOrdered);
    client->send(1, small1, SendMode::ReliableOrdered);
    client->send(1, small2, SendMode::ReliableOrdered);

    std::vector<NetEvent> got;
    REQUIRE(pumpUntil(*server, *client, [&] {
        for (auto& ev : drain(*server)) got.push_back(std::move(ev));
        return got.size() >= 3;
    }, std::chrono::seconds(10)));

    REQUIRE(got.size() == 3);
    CHECK(got[0].data == big);      // exact size and content preserved
    CHECK(got[1].data == small1);   // and the boundaries after it
    CHECK(got[2].data == small2);
}

// ─── Disconnect ──────────────────────────────────────────────────────────────

TEST_CASE("TcpTransport: destroying the client notifies the server")
{
    auto server = TcpTransport::listen(0);
    REQUIRE(server != nullptr);
    auto client = TcpTransport::connect("127.0.0.1", server->boundPort());
    REQUIRE(client != nullptr);

    REQUIRE(pumpUntil(*server, *client, [&] {
        return server->connectionCount() == 1 && client->connectionCount() == 1;
    }));
    drain(*server);

    client.reset();   // peer goes away

    bool sawDisconnect = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !sawDisconnect) {
        server->update();
        for (const auto& ev : drain(*server)) {
            if (ev.type == NetEventType::Disconnected) sawDisconnect = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(sawDisconnect);
    CHECK(server->connectionCount() == 0);
}

// ─── Multiple clients (the session host case) ────────────────────────────────

TEST_CASE("TcpTransport: host accepts several clients with distinct ids")
{
    auto server = TcpTransport::listen(0);
    REQUIRE(server != nullptr);
    const std::uint16_t port = server->boundPort();

    auto c1 = TcpTransport::connect("127.0.0.1", port);
    auto c2 = TcpTransport::connect("127.0.0.1", port);
    REQUIRE(c1 != nullptr);
    REQUIRE(c2 != nullptr);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && server->connectionCount() < 2) {
        server->update();
        c1->update();
        c2->update();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(server->connectionCount() == 2);

    std::vector<ConnectionId> ids;
    for (const auto& ev : drain(*server)) {
        if (ev.type == NetEventType::Connected) ids.push_back(ev.conn);
    }
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] != ids[1]);
}

// ─── NetSession over real TCP ────────────────────────────────────────────────

namespace {
constexpr MessageId kGreeting = kFirstUserMessage + 20;
} // namespace

TEST_CASE("NetSession: typed messages dispatch over a real TCP link")
{
    auto serverT = TcpTransport::listen(0);
    REQUIRE(serverT != nullptr);
    auto clientT = TcpTransport::connect("127.0.0.1", serverT->boundPort());
    REQUIRE(clientT != nullptr);

    NetSession server(serverT.get(), NetRole::Host);
    NetSession client(clientT.get(), NetRole::Client);

    std::string received;
    std::uint32_t receivedNum = 0;
    server.on(kGreeting, [&](ConnectionId, BitReader& r) {
        r.readString(received);
        r.readUInt32(receivedNum);
    });

    REQUIRE(pumpUntil(*serverT, *clientT, [&] {
        server.pump();
        client.pump();
        return !server.connections().empty() && !client.connections().empty();
    }));

    BitWriter payload;
    payload.writeString("hallo host");
    payload.writeUInt32(1234);
    client.broadcast(kGreeting, payload);

    REQUIRE(pumpUntil(*serverT, *clientT, [&] {
        server.pump();
        client.pump();
        return !received.empty();
    }));

    CHECK(received == "hallo host");
    CHECK(receivedNum == 1234);
}
