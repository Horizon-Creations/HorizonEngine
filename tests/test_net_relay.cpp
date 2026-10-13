#include "doctest.h"

#include "../src/HE_Editor/CollabController.h"

#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Net/NetGameSession.h>

#include <Net/BitStream.h>
#include <Net/NetSession.h>
#include <Net/RelayTransport.h>
#include <Net/SecureTransport.h>
#include <Net/TcpTransport.h>

#include <chrono>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace HE::Net;

// The relay path of HE_Net (topic 193, step 3): every socket is an outgoing
// connection to a relay, which pairs a client with the host and then forwards
// bytes 1:1. The relay itself lives in another repository; the FakeRelay below
// speaks the same protocol v1 in-process, decoded here by hand rather than with
// RelayTransport's own encoder, so a wrong byte on either side shows up.
//
// One case at the bottom runs against the real collab_server when
// HE_TEST_RELAY=host:port names a running one.

namespace {

// ─── A relay in a test ───────────────────────────────────────────────────────
// Frame-level rather than byte-level: TcpTransport already reassembles frames,
// and re-sending a frame produces the same bytes the real relay forwards.
struct FakeRelay {
    struct Conn {
        int         role = 0;
        std::string session, self, target, token;
        ConnectionId partner = 0;
        bool        paired   = false;
    };

    std::unique_ptr<TcpTransport>  t = TcpTransport::listenLoopback(0);
    std::map<ConnectionId, Conn>   conns;
    std::map<std::string, ConnectionId> listeners;   // session + "/" + peer
    std::map<std::string, std::pair<ConnectionId, std::string>> waiting;   // session/client → conn, token
    int  hellos  = 0;
    int  forwarded = 0;
    bool withholdAccept = false;   // INCOMING never sent: the host "does not answer"

    RelayAddress address() const { return RelayAddress{ "127.0.0.1", t->boundPort() }; }

    void status(ConnectionId c, RelayTransport::StatusCode code, const std::string& text = {}) {
        t->send(c, RelayTransport::encodeStatus(code, text), SendMode::ReliableOrdered);
    }
    void refuse(ConnectionId c, RelayTransport::StatusCode code, const std::string& text) {
        status(c, code, text);
        t->update();          // flush before the close, as the relay does
        t->disconnect(c);
        conns.erase(c);
    }

    void pump() {
        t->update();
        NetEvent ev;
        while (t->poll(ev)) {
            if (ev.type == NetEventType::Connected) { conns[ev.conn]; continue; }
            if (ev.type == NetEventType::Disconnected) { closed(ev.conn); continue; }
            Conn& c = conns[ev.conn];
            if (c.paired) {
                t->send(c.partner, ev.data, SendMode::ReliableOrdered);
                ++forwarded;
                continue;
            }
            hello(ev.conn, ev.data);
        }
    }

    void closed(ConnectionId id) {
        auto it = conns.find(id);
        if (it == conns.end()) return;
        const ConnectionId partner = it->second.partner;
        conns.erase(it);
        for (auto l = listeners.begin(); l != listeners.end();)
            l = (l->second == id) ? listeners.erase(l) : std::next(l);
        if (partner != 0 && conns.count(partner)) {
            t->update();
            t->disconnect(partner);   // FIN travels through
            conns.erase(partner);
        }
    }

    void hello(ConnectionId id, const std::vector<std::uint8_t>& f) {
        ++hellos;
        // "HERL" | 1 | 1 | role | str8 session | str8 self | str8 target | str8 token
        std::size_t pos = 0;
        auto str8 = [&](std::string& out) {
            REQUIRE(pos < f.size());
            const std::size_t n = f[pos++];
            REQUIRE(f.size() - pos >= n);
            out.assign(reinterpret_cast<const char*>(f.data() + pos), n);
            pos += n;
        };
        REQUIRE(f.size() >= 7);
        REQUIRE(std::string(f.begin(), f.begin() + 4) == "HERL");
        REQUIRE(f[4] == 1);
        REQUIRE(f[5] == 1);
        Conn& c = conns[id];
        c.role = f[6];
        pos = 7;
        str8(c.session); str8(c.self); str8(c.target); str8(c.token);
        CHECK(pos == f.size());

        const std::string key = c.session + "/" + c.self;
        switch (c.role) {
            case 1:   // listen
                CHECK(c.target.empty());
                listeners[key] = id;
                status(id, RelayTransport::StatusCode::Listening);
                break;
            case 2: { // connect
                auto l = listeners.find(c.session + "/" + c.target);
                if (l == listeners.end()) {
                    refuse(id, RelayTransport::StatusCode::NoSuchPeer, "nobody listens as " + c.target);
                    return;
                }
                status(id, RelayTransport::StatusCode::Waiting);
                const std::string token = "0123456789abcdef";
                waiting[key] = { id, token };
                if (!withholdAccept)
                    t->send(l->second, RelayTransport::encodeIncoming(c.self, token),
                            SendMode::ReliableOrdered);
                break;
            }
            case 3: { // accept
                auto w = waiting.find(c.session + "/" + c.target);
                if (w == waiting.end() || w->second.second != c.token) {
                    refuse(id, RelayTransport::StatusCode::NotExpected, "wrong token");
                    return;
                }
                const ConnectionId client = w->second.first;
                waiting.erase(w);
                c.paired = true;
                c.partner = client;
                conns[client].paired = true;
                conns[client].partner = id;
                status(id, RelayTransport::StatusCode::Paired);
                status(client, RelayTransport::StatusCode::Paired);
                break;
            }
            default:
                refuse(id, RelayTransport::StatusCode::BadHello, "unknown role");
        }
    }
};

template <typename Fn>
bool pumpUntil(FakeRelay* relay, std::vector<ITransport*> ends, Fn done,
               std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (relay) relay->pump();
        for (ITransport* e : ends) e->update();
        if (done()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

std::vector<NetEvent> drain(ITransport& t) {
    std::vector<NetEvent> out;
    NetEvent ev;
    while (t.poll(ev)) out.push_back(std::move(ev));
    return out;
}

void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}

} // namespace

// ─── The settings field ──────────────────────────────────────────────────────

TEST_CASE("Relay: address parsing")
{
    RelayAddress a;
    std::string err;

    CHECK_FALSE(parseRelayAddress("", a, &err));
    CHECK(err == "no relay address");
    CHECK_FALSE(parseRelayAddress("   ", a, &err));

    REQUIRE(parseRelayAddress("relay.example.com:9000", a));
    CHECK(a.host == "relay.example.com");
    CHECK(a.port == 9000);

    REQUIRE(parseRelayAddress("  212.227.140.164  ", a));
    CHECK(a.host == "212.227.140.164");
    CHECK(a.port == RelayTransport::kDefaultPort);

    REQUIRE(parseRelayAddress("[2001:db8::1]:7777", a));
    CHECK(a.host == "2001:db8::1");
    CHECK(a.port == 7777);
    REQUIRE(parseRelayAddress("[::1]", a));
    CHECK(a.host == "::1");
    CHECK(a.port == RelayTransport::kDefaultPort);

    CHECK_FALSE(parseRelayAddress("2001:db8::1", a, &err));
    CHECK(err.find("brackets") != std::string::npos);
    CHECK_FALSE(parseRelayAddress("host:", a, &err));
    CHECK_FALSE(parseRelayAddress("host:0", a));
    CHECK_FALSE(parseRelayAddress("host:65536", a));
    CHECK_FALSE(parseRelayAddress("host:12ab", a));
    CHECK_FALSE(parseRelayAddress(":7777", a));
    CHECK_FALSE(parseRelayAddress("[::1", a));
    CHECK_FALSE(parseRelayAddress("http://relay:7777", a));
}

TEST_CASE("Relay: HE_NET_RELAY overrides the configured address, and 'off' forces peer-to-peer")
{
    const char* saved = std::getenv("HE_NET_RELAY");
    const std::string keep = saved ? saved : "";

    setEnv("HE_NET_RELAY", nullptr);
    CHECK(effectiveRelayAddress("relay:1") == "relay:1");
    CHECK(effectiveRelayAddress("").empty());

    setEnv("HE_NET_RELAY", "other:2");
    CHECK(effectiveRelayAddress("relay:1") == "other:2");
    CHECK(effectiveRelayAddress("") == "other:2");

    setEnv("HE_NET_RELAY", "off");
    CHECK(effectiveRelayAddress("relay:1").empty());

    setEnv("HE_NET_RELAY", saved ? keep.c_str() : nullptr);
}

TEST_CASE("Relay: ids follow the relay's alphabet")
{
    CHECK(RelayTransport::isValidId("host"));
    CHECK(RelayTransport::isValidId("c-AZaz09_"));
    CHECK(RelayTransport::isValidId(std::string(64, 'a')));
    CHECK_FALSE(RelayTransport::isValidId(""));
    CHECK_FALSE(RelayTransport::isValidId(std::string(65, 'a')));
    CHECK_FALSE(RelayTransport::isValidId("a b"));
    CHECK_FALSE(RelayTransport::isValidId("ä"));

    const RelayAddress addr{ "127.0.0.1", 1 };
    CHECK(RelayTransport::host(addr, "bad id") == nullptr);
    CHECK(RelayTransport::connect(addr, "session", "host", "no/slash") == nullptr);
}

TEST_CASE("Relay: HELLO bytes match protocol v1")
{
    const auto f = RelayTransport::encodeHello(RelayTransport::Role::Accept, "S1", "host", "c-7",
                                               "TOKEN");
    const std::vector<std::uint8_t> want = {
        'H', 'E', 'R', 'L', 1, 1, 3,
        2, 'S', '1',
        4, 'h', 'o', 's', 't',
        3, 'c', '-', '7',
        5, 'T', 'O', 'K', 'E', 'N',
    };
    CHECK(f == want);

    // LISTEN and CONNECT leave the unused fields as empty str8s, not absent.
    const auto l = RelayTransport::encodeHello(RelayTransport::Role::Listen, "S", "h", "", "");
    CHECK(l == std::vector<std::uint8_t>{ 'H', 'E', 'R', 'L', 1, 1, 1, 1, 'S', 1, 'h', 0, 0 });
}

// ─── Links through the relay ─────────────────────────────────────────────────

TEST_CASE("Relay: a client is Connected only once paired, then frames pass both ways")
{
    FakeRelay relay;
    REQUIRE(relay.t);

    auto host = RelayTransport::host(relay.address(), "sessionA");
    REQUIRE(host);
    CHECK(host->isHost());
    REQUIRE(pumpUntil(&relay, { host.get() }, [&] { return host->listening(); }));
    CHECK(host->connectionCount() == 0);   // the listener is not a peer
    CHECK(drain(*host).empty());

    // Hold the INCOMING back: the client must sit in "waiting", not connected.
    relay.withholdAccept = true;
    auto client = RelayTransport::connect(relay.address(), "sessionA");
    REQUIRE(client);
    CHECK_FALSE(client->isHost());
    CHECK(client->selfId().rfind("c-", 0) == 0);
    CHECK(pumpUntil(&relay, { host.get(), client.get() },
                    [&] { return relay.waiting.size() == 1; }));
    pumpUntil(&relay, { host.get(), client.get() }, [] { return false; },
              std::chrono::milliseconds(100));
    CHECK(client->connectionCount() == 0);
    CHECK(drain(*client).empty());

    // A send while waiting is held, not lost and not put on the wire early.
    const std::vector<std::uint8_t> early{ 'e', 'a', 'r', 'l', 'y' };
    client->send(1, early, SendMode::ReliableOrdered);

    // Now let the host hear about it.
    auto w = relay.waiting.begin();
    const std::string clientId = w->first.substr(w->first.find('/') + 1);
    CHECK(clientId == client->selfId());
    relay.t->send(relay.listeners["sessionA/host"],
                  RelayTransport::encodeIncoming(clientId, w->second.second),
                  SendMode::ReliableOrdered);

    std::vector<NetEvent> hostEv, clientEv;
    REQUIRE(pumpUntil(&relay, { host.get(), client.get() }, [&] {
        for (auto& e : drain(*host)) hostEv.push_back(std::move(e));
        for (auto& e : drain(*client)) clientEv.push_back(std::move(e));
        return hostEv.size() >= 2 && !clientEv.empty();   // host: Connected + "early"
    }));
    REQUIRE(clientEv[0].type == NetEventType::Connected);
    CHECK(clientEv[0].conn == 1);
    REQUIRE(hostEv[0].type == NetEventType::Connected);
    const ConnectionId hostSide = hostEv[0].conn;
    REQUIRE(hostEv[1].type == NetEventType::Data);
    CHECK(hostEv[1].conn == hostSide);
    CHECK(hostEv[1].data == early);
    CHECK(host->connectionCount() == 1);
    CHECK(client->connectionCount() == 1);

    // Every SendMode is carried (over one TCP stream, all reliable).
    const std::vector<std::uint8_t> ping{ 1, 2, 3 }, pong(70'000, 0x5A);
    host->send(hostSide, ping, SendMode::Unreliable);
    client->send(1, pong, SendMode::Reliable);
    std::vector<NetEvent> h2, c2;
    REQUIRE(pumpUntil(&relay, { host.get(), client.get() }, [&] {
        for (auto& e : drain(*host)) h2.push_back(std::move(e));
        for (auto& e : drain(*client)) c2.push_back(std::move(e));
        return !h2.empty() && !c2.empty();
    }));
    CHECK(c2[0].data == ping);
    CHECK(h2[0].data == pong);

    // The client leaves: the host sees exactly its peer go, listener stays up.
    client->disconnect(1);
    std::vector<NetEvent> h3;
    REQUIRE(pumpUntil(&relay, { host.get() }, [&] {
        for (auto& e : drain(*host)) h3.push_back(std::move(e));
        return !h3.empty();
    }));
    CHECK(h3[0].type == NetEventType::Disconnected);
    CHECK(h3[0].conn == hostSide);
    CHECK(host->connectionCount() == 0);
    CHECK(host->listening());
}

TEST_CASE("Relay: two clients get their own host connections and no crosstalk")
{
    FakeRelay relay;
    auto host = RelayTransport::host(relay.address(), "sessionB");
    REQUIRE(host);
    REQUIRE(pumpUntil(&relay, { host.get() }, [&] { return host->listening(); }));

    auto a = RelayTransport::connect(relay.address(), "sessionB");
    auto b = RelayTransport::connect(relay.address(), "sessionB");
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->selfId() != b->selfId());

    std::vector<ConnectionId> peers;
    REQUIRE(pumpUntil(&relay, { host.get(), a.get(), b.get() }, [&] {
        for (auto& e : drain(*host))
            if (e.type == NetEventType::Connected) peers.push_back(e.conn);
        return peers.size() == 2 && a->connectionCount() == 1 && b->connectionCount() == 1;
    }));
    CHECK(peers[0] != peers[1]);
    drain(*a);
    drain(*b);

    // Each client says who it is; the host answers on the connection it came in on.
    a->send(1, std::vector<std::uint8_t>{ 'A' }, SendMode::ReliableOrdered);
    b->send(1, std::vector<std::uint8_t>{ 'B' }, SendMode::ReliableOrdered);
    std::map<ConnectionId, std::uint8_t> said;
    REQUIRE(pumpUntil(&relay, { host.get(), a.get(), b.get() }, [&] {
        for (auto& e : drain(*host))
            if (e.type == NetEventType::Data) said[e.conn] = e.data.at(0);
        return said.size() == 2;
    }));
    for (const auto& [conn, who] : said)
        host->send(conn, std::vector<std::uint8_t>{ static_cast<std::uint8_t>(who + 32) },
                   SendMode::ReliableOrdered);
    std::vector<NetEvent> ea, eb;
    REQUIRE(pumpUntil(&relay, { host.get(), a.get(), b.get() }, [&] {
        for (auto& e : drain(*a)) ea.push_back(std::move(e));
        for (auto& e : drain(*b)) eb.push_back(std::move(e));
        return !ea.empty() && !eb.empty();
    }));
    CHECK(ea[0].data == std::vector<std::uint8_t>{ 'a' });
    CHECK(eb[0].data == std::vector<std::uint8_t>{ 'b' });
    CHECK(ea.size() == 1);
    CHECK(eb.size() == 1);
}

TEST_CASE("Relay: a refusal before 'paired' is a failed connect, with the relay's reason")
{
    FakeRelay relay;
    // Nobody hosts "nobody-here": the relay answers no-such-peer and closes.
    auto client = RelayTransport::connect(relay.address(), "nobody-here");
    REQUIRE(client);
    std::vector<NetEvent> ev;
    REQUIRE(pumpUntil(&relay, { client.get() }, [&] {
        for (auto& e : drain(*client)) ev.push_back(std::move(e));
        return !ev.empty();
    }));
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].type == NetEventType::Disconnected);   // never Connected
    CHECK(ev[0].conn == 1);
    CHECK(client->connectionCount() == 0);
    CHECK(client->lastError().find("no-such-peer") != std::string::npos);

    // Sending to the dead link is a no-op, not a crash.
    client->send(1, std::vector<std::uint8_t>{ 1 }, SendMode::ReliableOrdered);
}

TEST_CASE("Relay: a relay nobody runs is a failed connect, and a host keeps retrying")
{
    std::uint16_t deadPort = 0;
    {
        auto probe = TcpTransport::listenLoopback(0);
        REQUIRE(probe);
        deadPort = probe->boundPort();
    }
    const RelayAddress dead{ "127.0.0.1", deadPort };

    auto client = RelayTransport::connect(dead, "s");
    REQUIRE(client);
    std::vector<NetEvent> ev;
    REQUIRE(pumpUntil(nullptr, { client.get() }, [&] {
        for (auto& e : drain(*client)) ev.push_back(std::move(e));
        return !ev.empty();
    }));
    CHECK(ev[0].type == NetEventType::Disconnected);
    CHECK_FALSE(client->lastError().empty());

    auto host = RelayTransport::host(dead, "s");
    REQUIRE(host);   // not fatal: the relay may come up later
    pumpUntil(nullptr, { host.get() }, [] { return false; }, std::chrono::milliseconds(200));
    CHECK_FALSE(host->listening());
    CHECK(drain(*host).empty());   // the listener's failure is not a peer event
    CHECK_FALSE(host->lastError().empty());
}

TEST_CASE("Relay: a wrong accept token never surfaces a peer on the host")
{
    FakeRelay relay;
    auto host = RelayTransport::host(relay.address(), "sessionT");
    REQUIRE(host);
    REQUIRE(pumpUntil(&relay, { host.get() }, [&] { return host->listening(); }));

    // An INCOMING with a token the relay will not honour.
    relay.t->send(relay.listeners["sessionT/host"],
                  RelayTransport::encodeIncoming("c-ghost", "not-the-token!!!"),
                  SendMode::ReliableOrdered);
    pumpUntil(&relay, { host.get() }, [] { return false; }, std::chrono::milliseconds(300));
    CHECK(drain(*host).empty());
    CHECK(host->connectionCount() == 0);
    CHECK(host->listening());
    CHECK(host->lastError().find("not-expected") != std::string::npos);
}

TEST_CASE("Relay: a non-relay frame before 'paired' closes the link")
{
    FakeRelay relay;
    auto client = RelayTransport::connect(relay.address(), "sessionG");
    REQUIRE(client);
    // Answer the HELLO with garbage instead of a STATUS.
    bool sent = false;
    std::vector<NetEvent> ev;
    REQUIRE(pumpUntil(nullptr, { client.get() }, [&] {
        relay.t->update();
        NetEvent e;
        while (relay.t->poll(e)) {
            if (e.type == NetEventType::Data && !sent) {
                relay.t->send(e.conn, std::vector<std::uint8_t>{ 'n', 'o', 'p', 'e', 0, 0 },
                              SendMode::ReliableOrdered);
                sent = true;
            }
        }
        for (auto& x : drain(*client)) ev.push_back(std::move(x));
        return !ev.empty();
    }));
    CHECK(ev[0].type == NetEventType::Disconnected);
    CHECK(client->lastError().find("protocol error") != std::string::npos);
}

// ─── The real stack over the relay ───────────────────────────────────────────

namespace {
constexpr MessageId kRelayHello = kFirstUserMessage + 41;

// Host + client, both wrapped in SecureTransport and a NetSession, over `relay`.
// Returns what the host received from the client.
std::string secureRoundTrip(FakeRelay* fake, const RelayAddress& addr, const std::string& session) {
    const std::string secret = SecureTransport::generateJoinSecret();
    auto hostRelay = RelayTransport::host(addr, session);
    REQUIRE(hostRelay);
    RelayTransport* hostRaw = hostRelay.get();
    auto host = SecureTransport::wrap(std::move(hostRelay),
                                      SecureTransport::Config{ secret, NetRole::Host, true });
    REQUIRE(host);
    REQUIRE(pumpUntil(fake, { host.get() }, [&] { return hostRaw->listening(); }));

    auto clientRelay = RelayTransport::connect(addr, session);
    REQUIRE(clientRelay);
    auto client = SecureTransport::wrap(std::move(clientRelay),
                                        SecureTransport::Config{ secret, NetRole::Client, true });
    REQUIRE(client);

    NetSession hostSession(host.get(), NetRole::Host);
    NetSession clientSession(client.get(), NetRole::Client);
    std::string got;
    hostSession.on(kRelayHello, [&](ConnectionId, BitReader& r) { r.readString(got); });

    REQUIRE(pumpUntil(fake, { host.get(), client.get() }, [&] {
        hostSession.pump();
        clientSession.pump();
        return !hostSession.connections().empty() && !clientSession.connections().empty();
    }, std::chrono::seconds(10)));

    BitWriter payload;
    payload.writeString("ueber den relay");
    clientSession.broadcast(kRelayHello, payload);
    pumpUntil(fake, { host.get(), client.get() }, [&] {
        hostSession.pump();
        clientSession.pump();
        return !got.empty();
    }, std::chrono::seconds(10));
    return got;
}
} // namespace

TEST_CASE("Relay: SecureTransport and NetSession run unchanged over a relay link")
{
    FakeRelay relay;
    CHECK(secureRoundTrip(&relay, relay.address(), "sessionSecure") == "ueber den relay");
    // The relay forwarded only ciphertext frames; it never saw the plaintext.
    CHECK(relay.forwarded > 0);
}

TEST_CASE("Relay: against a real collab_server (HE_TEST_RELAY=host:port)")
{
    const char* env = std::getenv("HE_TEST_RELAY");
    if (!env || !*env) {
        MESSAGE("HE_TEST_RELAY not set — skipped (start collab_server and point it there)");
        return;
    }
    RelayAddress addr;
    REQUIRE(parseRelayAddress(env, addr));
    // A fresh session per run: a relay that still holds a previous run's
    // listener would otherwise answer id-taken.
    const std::string session = "he-test-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count() % 1000000007);
    CHECK(secureRoundTrip(nullptr, addr, session) == "ueber den relay");

    // And a refusal from the real thing: nobody listens in this session.
    auto lost = RelayTransport::connect(addr, session + "-empty");
    REQUIRE(lost);
    std::vector<NetEvent> ev;
    REQUIRE(pumpUntil(nullptr, { lost.get() }, [&] {
        for (auto& e : drain(*lost)) ev.push_back(std::move(e));
        return !ev.empty();
    }));
    CHECK(ev[0].type == NetEventType::Disconnected);
    CHECK(lost->lastError().find("no-such-peer") != std::string::npos);
}

// ─── The editor and the game, through the relay ──────────────────────────────
// What the settings fields switch on: CollabController and NetGameSession with
// a relay address, end to end over the fake relay. Without an address both run
// the direct path their own test files cover — those tests are the proof that
// the peer-to-peer path is unchanged.

namespace {

std::string addressText(const FakeRelay& r) {
    return "127.0.0.1:" + std::to_string(r.t->boundPort());
}

template <typename Fn>
bool pumpCollab(FakeRelay& relay, std::vector<CollabController*> ends, Fn done,
                std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::uint64_t now = 1;
    for (;;) {
        relay.pump();
        for (CollabController* c : ends) c->update(now);
        if (done()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        now += 16;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

std::size_t entityCount(HorizonWorld& world) {
    std::size_t n = 0;
    world.registry().view<entt::entity>().each([&](auto) { ++n; });
    return n;
}

} // namespace

TEST_CASE("Relay: two editors share a session through the relay, joined by session id")
{
    FakeRelay relay;
    REQUIRE(relay.t);

    HorizonWorld hostWorld;
    hostWorld.createEntity("Cube");
    hostWorld.createEntity("Sun");
    CollabController host;
    host.setWorld(&hostWorld);
    host.setRelayAddress(addressText(relay));
    REQUIRE(host.startHosting(0, "Anna"));
    CHECK(host.status() == CollabController::Status::Hosting);
    CHECK(host.viaRelay());
    CHECK(host.port() == 0);              // no port of our own was opened
    REQUIRE_FALSE(host.sessionId().empty());
    REQUIRE(pumpCollab(relay, { &host }, [&] {
        return host.directoryStatus().find("Hosting through the relay") != std::string::npos;
    }));
    REQUIRE(relay.listeners.count(host.sessionId() + "/host") == 1);

    HorizonWorld clientWorld;
    CollabController client;
    client.setWorld(&clientWorld);
    client.setRelayAddress(addressText(relay));
    REQUIRE(client.joinBySessionId(host.sessionId(), host.joinCode(), "Bob"));
    CHECK(client.viaRelay());
    REQUIRE(pumpCollab(relay, { &host, &client }, [&] {
        return client.status() == CollabController::Status::Joined;
    }));
    CHECK(entityCount(clientWorld) == entityCount(hostWorld));
    CHECK(relay.forwarded > 0);           // the scene went through the relay

    // A join by address is refused while a relay is set, rather than dialled.
    CollabController direct;
    HorizonWorld directWorld;
    direct.setWorld(&directWorld);
    direct.setRelayAddress(addressText(relay));
    CHECK_FALSE(direct.joinSession("127.0.0.1", 47000, host.joinCode(), "Cleo"));
    CHECK(direct.status() == CollabController::Status::Failed);
    CHECK(direct.lastError().find("relay server is set") != std::string::npos);

    client.leave();
    host.leave();
    CHECK_FALSE(host.viaRelay());
}

TEST_CASE("Relay: an editor joining a session nobody hosts fails at once with the relay's reason")
{
    FakeRelay relay;
    CollabController client;
    HorizonWorld world;
    client.setWorld(&world);
    client.setRelayAddress(addressText(relay));
    REQUIRE(client.joinBySessionId("nosuchsession", "SOMECODE", "Bob"));
    // Well inside the 20 s connect deadline: the refusal is the answer.
    REQUIRE(pumpCollab(relay, { &client }, [&] {
        return client.status() == CollabController::Status::Failed;
    }, std::chrono::seconds(5)));
    CHECK(client.lastError().find("no-such-peer") != std::string::npos);
}

TEST_CASE("Relay: a malformed relay address refuses to host instead of going direct")
{
    CollabController host;
    HorizonWorld world;
    host.setWorld(&world);
    host.setRelayAddress("2001:db8::1");   // IPv6 without brackets
    CHECK_FALSE(host.startHosting(0, "Anna"));
    CHECK(host.status() == CollabController::Status::Failed);
    CHECK(host.lastError().find("cannot be used") != std::string::npos);
    CHECK(host.port() == 0);
}

TEST_CASE("Relay: the relay session of a game is derived from the join code")
{
    const std::string a = RelayTransport::sessionIdForJoinCode("CODE-ONE");
    CHECK(a == RelayTransport::sessionIdForJoinCode("CODE-ONE"));
    CHECK(a != RelayTransport::sessionIdForJoinCode("CODE-TWO"));
    CHECK(RelayTransport::isValidId(a));
    CHECK(a.find("CODE") == std::string::npos);
}

TEST_CASE("Relay: a game session hosts and joins through the relay with only the join code")
{
    FakeRelay relay;

    NetGameSession::HostOptions hostOpts;
    hostOpts.announceLan  = false;
    hostOpts.relayAddress = addressText(relay);
    HorizonWorld   hostWorld, clientWorld;
    NetGameSession host, client;
    host.setWorld(&hostWorld);
    client.setWorld(&clientWorld);
    REQUIRE(host.host(hostOpts));
    CHECK(host.viaRelay());
    CHECK(host.boundPort() == 0);          // no UDP port was opened

    NetGameSession::JoinOptions joinOpts;
    joinOpts.displayName  = "Anna";
    joinOpts.joinCode     = host.joinCode();
    joinOpts.relayAddress = addressText(relay);
    // Address and port are not dialled through a relay: deliberately nonsense.
    REQUIRE(client.joinDirect("203.0.113.1", 9, joinOpts));
    CHECK(client.viaRelay());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (client.status() != NetGameSession::Status::Joined &&
           std::chrono::steady_clock::now() < deadline) {
        relay.pump();
        host.update(1.0f / 60.0f);
        client.update(1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(client.status() == NetGameSession::Status::Joined);
    CHECK(host.roster().size() == 2u);
    CHECK(relay.listeners.count(RelayTransport::sessionIdForJoinCode(host.joinCode()) + "/host") == 1);

    client.leave();
    host.leave();
    CHECK_FALSE(host.viaRelay());
}
