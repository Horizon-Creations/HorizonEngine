#include "doctest.h"

#include "../src/HE_Editor/CollabController.h"

#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Net/NetGameSession.h>

#include <Net/BitStream.h>
#include <Net/NetSession.h>
#include <Net/RelayTransport.h>
#include <Net/SecureTransport.h>
#include <Net/TcpTransport.h>

#include <Net/RelayFanout.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <initializer_list>
#include <iterator>
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
        bool        helloDone = false;
    };

    std::unique_ptr<TcpTransport>  t = TcpTransport::listenLoopback(0);
    std::map<ConnectionId, Conn>   conns;
    std::map<std::string, ConnectionId> listeners;   // session + "/" + peer
    std::map<std::string, std::pair<ConnectionId, std::string>> waiting;   // session/client → conn, token
    std::map<std::string, std::vector<ConnectionId>> subscribers;          // session/host → subscribe conns
    int  hellos  = 0;
    int  forwarded = 0;
    int  broadcastFrames = 0;      // BROADCAST frames read from listeners
    int  broadcastCopies = 0;      // payloads written to subscribers
    bool withholdAccept = false;   // INCOMING never sent: the host "does not answer"
    // Hold one stream to force an order between the two: the relay does not
    // order a client's pair against its broadcast stream, and neither does this.
    bool holdToClients  = false;   // pair bytes host → client
    bool holdBroadcasts = false;
    bool refuseSubscribe = false;  // every SUBSCRIBE refused: clients stay per pair
    std::vector<std::uint8_t> lastBroadcast;   // what the relay got to see
    std::vector<std::pair<ConnectionId, std::vector<std::uint8_t>>> heldPair, heldBroadcast;

    void release() {
        for (auto& [c, d] : heldPair)
            if (conns.count(c)) t->send(c, d, SendMode::ReliableOrdered);
        for (auto& [c, d] : heldBroadcast)
            if (conns.count(c)) t->send(c, d, SendMode::ReliableOrdered);
        heldPair.clear();
        heldBroadcast.clear();
    }

    // The relay ends every subscription (as for a stalled subscriber).
    void dropSubscribers() {
        t->update();
        for (auto& [key, subs] : subscribers)
            for (ConnectionId s : subs) { t->disconnect(s); conns.erase(s); }
        subscribers.clear();
    }

    void broadcast(ConnectionId listener, const std::vector<std::uint8_t>& f) {
        REQUIRE(f.size() >= 6);
        REQUIRE(std::string(f.begin(), f.begin() + 4) == "HERL");
        REQUIRE(f[5] == 5);
        ++broadcastFrames;
        const Conn& l = conns[listener];
        const std::vector<std::uint8_t> payload(f.begin() + 6, f.end());
        lastBroadcast = payload;
        for (ConnectionId s : subscribers[l.session + "/" + l.self]) {
            ++broadcastCopies;
            if (holdBroadcasts) heldBroadcast.emplace_back(s, payload);
            else                t->send(s, payload, SendMode::ReliableOrdered);
        }
    }

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
                if (holdToClients && conns[c.partner].role == 2)
                    heldPair.emplace_back(c.partner, ev.data);
                else
                    t->send(c.partner, ev.data, SendMode::ReliableOrdered);
                ++forwarded;
                continue;
            }
            if (c.helloDone && c.role == 1) { broadcast(ev.conn, ev.data); continue; }
            if (c.helloDone) continue;   // a subscriber has nothing to say
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
        for (auto& [key, subs] : subscribers)
            subs.erase(std::remove(subs.begin(), subs.end(), id), subs.end());
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
        c.helloDone = true;

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
            case 4: { // subscribe: only while `self` is paired with `target`
                bool pairedWithHost = false;
                for (const auto& [cid, o] : conns)
                    if (cid != id && o.role == 2 && o.paired && o.session == c.session &&
                        o.self == c.self && o.target == c.target)
                        pairedWithHost = true;
                if (!pairedWithHost || refuseSubscribe) {
                    refuse(id, RelayTransport::StatusCode::NotExpected, "not paired with " + c.target);
                    return;
                }
                CHECK(c.token.empty());
                subscribers[c.session + "/" + c.target].push_back(id);
                status(id, RelayTransport::StatusCode::Subscribed);
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

    // As TcpTransport::connect: an immediate refusal is nullptr, a pending one
    // ends in Disconnected. Either is a failed connect; a hang is not.
    if (auto client = RelayTransport::connect(dead, "s")) {
        std::vector<NetEvent> ev;
        REQUIRE(pumpUntil(nullptr, { client.get() }, [&] {
            for (auto& e : drain(*client)) ev.push_back(std::move(e));
            return !ev.empty();
        }));
        CHECK(ev[0].type == NetEventType::Disconnected);
        CHECK_FALSE(client->lastError().empty());
    }

    auto host = RelayTransport::host(dead, "s");
    REQUIRE(host);   // not fatal: the relay may come up later
    // Waited for, not a fixed pause: Linux and macOS refuse a dead loopback
    // port at once, Windows retries the SYN for about two seconds before
    // WSAECONNREFUSED, and only then does the listener know what is wrong.
    pumpUntil(nullptr, { host.get() }, [&] { return !host->lastError().empty(); },
              std::chrono::seconds(10));
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
bool pumpCollab(FakeRelay* relay, std::vector<CollabController*> ends, Fn done,
                std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::uint64_t now = 1;
    for (;;) {
        if (relay) relay->pump();
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
    REQUIRE(pumpCollab(&relay, { &host }, [&] {
        return host.directoryStatus().find("Hosting through the relay") != std::string::npos;
    }));
    REQUIRE(relay.listeners.count(host.sessionId() + "/host") == 1);

    HorizonWorld clientWorld;
    CollabController client;
    client.setWorld(&clientWorld);
    client.setRelayAddress(addressText(relay));
    REQUIRE(client.joinBySessionId(host.sessionId(), host.joinCode(), "Bob"));
    CHECK(client.viaRelay());
    REQUIRE(pumpCollab(&relay, { &host, &client }, [&] {
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
    REQUIRE(pumpCollab(&relay, { &client }, [&] {
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

    // A join before the host is registered is refused (no-such-peer), as on
    // the real relay; a person starts the host first, the test waits for it.
    const std::string relaySession = RelayTransport::sessionIdForJoinCode(host.joinCode());
    const auto registered = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!relay.listeners.count(relaySession + "/host") &&
           std::chrono::steady_clock::now() < registered) {
        relay.pump();
        host.update(1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(relay.listeners.count(relaySession + "/host") == 1);

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

    client.leave();
    host.leave();
    CHECK_FALSE(host.viaRelay());
}

TEST_CASE("Relay: a game join nobody hosts fails with the relay's reason instead of hanging")
{
    FakeRelay relay;
    NetGameSession client;
    HorizonWorld world;
    client.setWorld(&world);
    NetGameSession::JoinOptions joinOpts;
    joinOpts.joinCode     = "NOBODY-HOSTS-THIS-CODE";
    joinOpts.relayAddress = addressText(relay);
    REQUIRE(client.joinDirect("127.0.0.1", 1, joinOpts));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (client.status() == NetGameSession::Status::Connecting &&
           std::chrono::steady_clock::now() < deadline) {
        relay.pump();
        client.update(1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(client.status() == NetGameSession::Status::Failed);
    CHECK(client.lastError().find("no-such-peer") != std::string::npos);
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

    // The editor path, end to end through the real server.
    {
        HorizonWorld hostWorld, clientWorld;
        hostWorld.createEntity("Cube");
        CollabController host, client;
        host.setWorld(&hostWorld);
        client.setWorld(&clientWorld);
        host.setRelayAddress(env);
        client.setRelayAddress(env);
        REQUIRE(host.startHosting(0, "Anna"));
        REQUIRE(pumpCollab(nullptr, { &host }, [&] {
            return host.directoryStatus().find("Hosting through the relay") != std::string::npos;
        }));
        REQUIRE(client.joinBySessionId(host.sessionId(), host.joinCode(), "Bob"));
        CHECK(pumpCollab(nullptr, { &host, &client }, [&] {
            return client.status() == CollabController::Status::Joined;
        }));
        CHECK(entityCount(clientWorld) == entityCount(hostWorld));
        client.leave();
        host.leave();
    }

    // And the game path.
    {
        NetGameSession::HostOptions hostOpts;
        hostOpts.announceLan  = false;
        hostOpts.relayAddress = env;
        HorizonWorld   hostWorld, clientWorld;
        NetGameSession host, client;
        host.setWorld(&hostWorld);
        client.setWorld(&clientWorld);
        REQUIRE(host.host(hostOpts));
        // Give the listener a moment to register (no relay-side view here).
        const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (std::chrono::steady_clock::now() < settle) {
            host.update(1.0f / 60.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        NetGameSession::JoinOptions joinOpts;
        joinOpts.displayName  = "Anna";
        joinOpts.joinCode     = host.joinCode();
        joinOpts.relayAddress = env;
        REQUIRE(client.joinDirect("203.0.113.1", 9, joinOpts));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (client.status() == NetGameSession::Status::Connecting &&
               std::chrono::steady_clock::now() < deadline) {
            host.update(1.0f / 60.0f);
            client.update(1.0f / 60.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(client.status() == NetGameSession::Status::Joined);
        CHECK(host.roster().size() == 2u);
        client.leave();
        host.leave();
    }
}

// ─── Broadcast through the relay (step 4) ────────────────────────────────────
// The host hands a message for many clients to the relay once. RelayFanout
// carries the group key over each pair, and on the client merges the pair and
// the broadcast stream back into the order the host sent in.

namespace {
constexpr MessageId kFanMsg  = kFirstUserMessage + 42;
constexpr MessageId kFanName = kFirstUserMessage + 43;

BitWriter text(const std::string& s) {
    BitWriter w;
    w.writeString(s);
    return w;
}

struct FanEnd {
    RelayTransport*                     relay = nullptr;
    std::unique_ptr<RelayFanout>        t;
    std::unique_ptr<NetSession>         s;
    std::vector<std::string>            got;
    std::map<std::string, ConnectionId> names;   // host: who is which connection
};

std::unique_ptr<FanEnd> fanEnd(std::unique_ptr<RelayTransport> r, NetRole role,
                               const std::string& secret) {
    REQUIRE(r);
    auto e   = std::make_unique<FanEnd>();
    e->relay = r.get();
    auto sec = SecureTransport::wrap(std::move(r), SecureTransport::Config{ secret, role, true });
    REQUIRE(sec);
    RelayFanout::Config cfg;
    cfg.role = role;
    e->t = RelayFanout::wrap(std::move(sec), e->relay, cfg);
    REQUIRE(e->t);
    e->s = std::make_unique<NetSession>(e->t.get(), role);
    FanEnd* raw = e.get();
    e->s->on(kFanMsg, [raw](ConnectionId, BitReader& r2) {
        std::string v;
        r2.readString(v);
        raw->got.push_back(v);
    });
    e->s->on(kFanName, [raw](ConnectionId c, BitReader& r2) {
        std::string v;
        r2.readString(v);
        raw->names[v] = c;
    });
    return e;
}

template <typename Fn>
bool pumpFan(FakeRelay* relay, const std::vector<FanEnd*>& ends, Fn done,
             std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (relay) relay->pump();
        for (FanEnd* e : ends) {
            e->t->update();
            e->s->pump();
        }
        if (done()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// A host and `n` clients in `session`, every client on the relay (unless the
// relay refuses subscriptions) and introduced to the host by name c0, c1, ….
struct FanSession {
    std::string                          secret = SecureTransport::generateJoinSecret();
    std::unique_ptr<FanEnd>              host;
    std::vector<std::unique_ptr<FanEnd>> clients;
    std::vector<FanEnd*>                 all;

    FanSession(FakeRelay* relay, const RelayAddress& addr, const std::string& session,
               int n, bool expectOnRelay = true) {
        host = fanEnd(RelayTransport::host(addr, session), NetRole::Host, secret);
        all.push_back(host.get());
        if (relay) {
            REQUIRE(pumpFan(relay, all, [&] { return host->relay->listening(); }));
        } else {
            pumpFan(nullptr, all, [] { return false; }, std::chrono::milliseconds(300));
        }
        for (int i = 0; i < n; ++i) {
            clients.push_back(fanEnd(RelayTransport::connect(addr, session), NetRole::Client, secret));
            all.push_back(clients.back().get());
        }
        REQUIRE(pumpFan(relay, all, [&] {
            for (auto& c : clients) if (c->s->connections().empty()) return false;
            return true;
        }));
        for (int i = 0; i < n; ++i)
            clients[static_cast<std::size_t>(i)]->s->broadcast(kFanName, text("c" + std::to_string(i)));
        REQUIRE(pumpFan(relay, all, [&] {
            if (host->names.size() != static_cast<std::size_t>(n)) return false;
            return !expectOnRelay || host->t->relayedPeers() == static_cast<std::size_t>(n);
        }));
    }
    ConnectionId conn(int i) const { return host->names.at("c" + std::to_string(i)); }
};

std::vector<std::string> seq(std::initializer_list<const char*> l) {
    return std::vector<std::string>(l.begin(), l.end());
}

} // namespace

TEST_CASE("Relay fan-out: one frame to the relay reaches every client, in order, encrypted")
{
    FakeRelay relay;
    FanSession fs(&relay, relay.address(), "fanOne", 3);
    for (auto& c : fs.clients) CHECK(c->t->onRelay());

    const auto before       = fs.host->t->stats();
    const int  framesBefore = relay.broadcastFrames;
    for (int i = 0; i < 5; ++i) fs.host->s->broadcast(kFanMsg, text("tick " + std::to_string(i)));
    REQUIRE(pumpFan(&relay, fs.all, [&] {
        for (auto& c : fs.clients) if (c->got.size() < 5) return false;
        return true;
    }));
    for (auto& c : fs.clients)
        CHECK(c->got == seq({ "tick 0", "tick 1", "tick 2", "tick 3", "tick 4" }));
    // Once per message for the host, three times per message for the relay.
    CHECK(fs.host->t->stats().relayFrames - before.relayFrames == 5);
    CHECK(fs.host->t->stats().pairCopies == before.pairCopies);
    CHECK(relay.broadcastFrames - framesBefore == 5);
    // The relay never sees the plaintext.
    const std::string seen(relay.lastBroadcast.begin(), relay.lastBroadcast.end());
    CHECK(seen.find("tick") == std::string::npos);
}

TEST_CASE("Relay fan-out: a broadcast never overtakes an earlier message, nor the other way round")
{
    FakeRelay relay;
    // Two clients: a broadcast to a single peer is a plain send.
    FanSession fs(&relay, relay.address(), "fanOrder", 2);
    FanEnd& client = *fs.clients[0];
    const ConnectionId conn = fs.conn(0);

    // Unicast first, broadcast second — and the pair is the slow stream.
    relay.holdToClients = true;
    fs.host->s->send(conn, kFanMsg, text("unicast A"));
    fs.host->s->broadcast(kFanMsg, text("broadcast B"));
    pumpFan(&relay, fs.all, [] { return false; }, std::chrono::milliseconds(300));
    CHECK(relay.heldPair.size() == 1);
    CHECK(client.got.empty());   // B is there, and waits for A
    relay.holdToClients = false;
    relay.release();
    REQUIRE(pumpFan(&relay, fs.all, [&] { return client.got.size() == 2; }));
    CHECK(client.got == seq({ "unicast A", "broadcast B" }));

    // Broadcast first, unicast second — and the broadcast is the slow stream.
    relay.holdBroadcasts = true;
    fs.host->s->broadcast(kFanMsg, text("broadcast C"));
    fs.host->s->send(conn, kFanMsg, text("unicast D"));
    pumpFan(&relay, fs.all, [] { return false; }, std::chrono::milliseconds(300));
    CHECK(relay.heldBroadcast.size() == 2);   // one copy per subscriber
    CHECK(client.got.size() == 2);   // D is there, and waits for C
    relay.holdBroadcasts = false;
    relay.release();
    REQUIRE(pumpFan(&relay, fs.all, [&] { return client.got.size() == 4; }));
    CHECK(client.got == seq({ "unicast A", "broadcast B", "broadcast C", "unicast D" }));
}

TEST_CASE("Relay fan-out: everyone but the sender, with a client that is not on the relay")
{
    FakeRelay relay;
    FanSession fs(&relay, relay.address(), "fanExcept", 2);
    // A third client whose subscription the relay refuses: it stays per pair.
    relay.refuseSubscribe = true;
    fs.clients.push_back(fanEnd(RelayTransport::connect(relay.address(), "fanExcept"),
                                NetRole::Client, fs.secret));
    FanEnd& c2 = *fs.clients.back();
    fs.all.push_back(&c2);
    REQUIRE(pumpFan(&relay, fs.all, [&] { return !c2.s->connections().empty(); }));
    c2.s->broadcast(kFanName, text("c2"));
    REQUIRE(pumpFan(&relay, fs.all, [&] { return fs.host->names.size() == 3; }));
    pumpFan(&relay, fs.all, [] { return false; }, std::chrono::milliseconds(200));
    CHECK_FALSE(c2.t->onRelay());
    CHECK(fs.host->t->relayedPeers() == 2);

    FanEnd& c0 = *fs.clients[0];
    FanEnd& c1 = *fs.clients[1];
    const auto before = fs.host->t->stats();
    fs.host->s->broadcastExcept(fs.conn(0), kFanMsg, text("not for c0"));
    fs.host->s->send(fs.conn(0), kFanMsg, text("for c0"));
    fs.host->s->broadcastExcept(fs.host->names.at("c2"), kFanMsg, text("not for c2"));
    fs.host->s->send(fs.host->names.at("c2"), kFanMsg, text("for c2"));
    fs.host->s->sendTo({ fs.conn(1), fs.host->names.at("c2") }, kFanMsg, text("c1 and c2"));
    fs.host->s->sendTo({ fs.conn(1) }, kFanMsg, text("only c1"));   // one recipient: a plain send
    REQUIRE(pumpFan(&relay, fs.all, [&] {
        return c0.got.size() == 2 && c1.got.size() == 4 && c2.got.size() == 3;
    }));
    pumpFan(&relay, fs.all, [] { return false; }, std::chrono::milliseconds(100));
    CHECK(c0.got == seq({ "for c0", "not for c2" }));
    CHECK(c1.got == seq({ "not for c0", "not for c2", "c1 and c2", "only c1" }));
    CHECK(c2.got == seq({ "not for c0", "for c2", "c1 and c2" }));

    const auto after = fs.host->t->stats();
    CHECK(after.relayFrames - before.relayFrames == 3);   // c0 and c1 learn every index
    CHECK(after.pairCopies - before.pairCopies == 2);     // "not for c0", "c1 and c2" to c2
    CHECK(after.pairSkips - before.pairSkips == 1);       // "not for c2": c2's turn passed
    CHECK(after.unicasts - before.unicasts == 3);         // "for c0", "for c2", "only c1"
}

TEST_CASE("Relay fan-out: a client joining mid-stream gets each broadcast exactly once across the switch")
{
    FakeRelay relay;
    // One client already on the relay, so a broadcast has more than one
    // recipient (to a single peer it would be a plain send).
    FanSession fs(&relay, relay.address(), "fanJoin", 1);
    FanEnd* host = fs.host.get();
    // A message the host sends the moment it knows the client, before any broadcast.
    host->s->onConnect([&](ConnectionId c) { host->s->send(c, kFanMsg, text("welcome")); });

    auto client = fanEnd(RelayTransport::connect(relay.address(), "fanJoin"), NetRole::Client, fs.secret);
    std::vector<FanEnd*> all = fs.all;
    all.push_back(client.get());
    const auto before = host->t->stats();
    // The host broadcasts once per pump, all the way through the client's
    // join, KEY, subscription and switch to the relay.
    int n = 0;
    int afterSwitch = 0;
    REQUIRE(pumpFan(&relay, all, [&] {
        if (host->s->connections().size() == 2) host->s->broadcast(kFanMsg, text(std::to_string(n++)));
        if (host->t->relayedPeers() == 2) ++afterSwitch;
        return afterSwitch >= 30;
    }));
    REQUIRE(pumpFan(&relay, all, [&] { return client->got.size() >= static_cast<std::size_t>(n) + 1; }));
    pumpFan(&relay, all, [] { return false; }, std::chrono::milliseconds(100));

    REQUIRE(client->got.size() == static_cast<std::size_t>(n) + 1);
    CHECK(client->got[0] == "welcome");
    for (int i = 0; i < n; ++i) CHECK(client->got[static_cast<std::size_t>(i) + 1] == std::to_string(i));
    CHECK(host->t->stats().pairCopies > before.pairCopies);     // before the switch
    CHECK(host->t->stats().relayFrames >= before.relayFrames + 30);   // after it
    CHECK(client->t->onRelay());
}

TEST_CASE("Relay fan-out: a broadcast stream that ends takes the link down, with a reason")
{
    FakeRelay relay;
    FanSession fs(&relay, relay.address(), "fanLoss", 1);
    FanEnd& client = *fs.clients[0];
    relay.dropSubscribers();
    REQUIRE(pumpFan(&relay, fs.all, [&] {
        return client.s->connections().empty() && fs.host->s->connections().empty();
    }));
    CHECK(client.relay->lastError().find("broadcast stream ended") != std::string::npos);
}

TEST_CASE("Relay fan-out: a host whose relay listener is gone drops the clients that relied on it")
{
    FakeRelay relay;
    FanSession fs(&relay, relay.address(), "fanNoListener", 2);
    const std::string key = "fanNoListener/host";
    const ConnectionId l = relay.listeners.at(key);
    relay.t->disconnect(l);
    relay.conns.erase(l);
    relay.listeners.erase(key);
    REQUIRE(pumpFan(&relay, fs.all, [&] { return !fs.host->relay->listening(); }));

    fs.host->s->broadcast(kFanMsg, text("into the void"));
    CHECK(fs.host->t->lastError().find("could not be handed to the relay") != std::string::npos);
    fs.host->s->pump();
    CHECK(fs.host->s->connections().empty());
}

TEST_CASE("Relay fan-out: an editor session through the relay relays presence as broadcasts")
{
    FakeRelay relay;
    HorizonWorld hostWorld, worldA, worldB, worldC;
    hostWorld.createEntity("Cube");
    CollabController host, a, b, c;
    host.setWorld(&hostWorld);
    a.setWorld(&worldA);
    b.setWorld(&worldB);
    c.setWorld(&worldC);
    for (CollabController* e : { &host, &a, &b, &c }) e->setRelayAddress(addressText(relay));
    REQUIRE(host.startHosting(0, "Anna"));
    REQUIRE(pumpCollab(&relay, { &host }, [&] {
        return relay.listeners.count(host.sessionId() + "/host") == 1;
    }));
    REQUIRE(a.joinBySessionId(host.sessionId(), host.joinCode(), "Bob"));
    REQUIRE(b.joinBySessionId(host.sessionId(), host.joinCode(), "Cleo"));
    REQUIRE(c.joinBySessionId(host.sessionId(), host.joinCode(), "Dora"));
    REQUIRE(pumpCollab(&relay, { &host, &a, &b, &c }, [&] {
        for (CollabController* e : { &a, &b, &c })
            if (e->status() != CollabController::Status::Joined) return false;
        return relay.subscribers[host.sessionId() + "/host"].size() == 3;
    }));
    // Give the host time to read the SUBSCRIBED notices.
    pumpCollab(&relay, { &host, &a, &b, &c }, [] { return false; }, std::chrono::milliseconds(200));
    CHECK(entityCount(worldC) == entityCount(hostWorld));

    const int framesBefore = relay.broadcastFrames;
    const float hostPos[3] = { 1.0f, 2.0f, 3.0f };
    const float aPos[3]    = { 7.0f, 8.0f, 9.0f };
    const float rot[4]     = { 0.0f, 0.0f, 0.0f, 1.0f };
    host.setLocalPresence(hostPos, rot, {});
    a.setLocalPresence(aPos, rot, {});
    auto at = [](const HE::Net::PresenceState* p, const float* pos) {
        return p && p->valid && p->cameraPos[0] == pos[0] && p->cameraPos[2] == pos[2];
    };
    CHECK(pumpCollab(&relay, { &host, &a, &b, &c }, [&] {
        return at(a.presenceOf(host.localParticipant()), hostPos) &&
               at(c.presenceOf(host.localParticipant()), hostPos) &&
               at(b.presenceOf(a.localParticipant()), aPos) &&   // relayed by the host, except to a
               at(c.presenceOf(a.localParticipant()), aPos);
    }));
    // The host's own presence and its relay of a's: through the relay, once each.
    CHECK(relay.broadcastFrames - framesBefore >= 2);
    c.leave();
    b.leave();
    a.leave();
    host.leave();
}

TEST_CASE("Relay fan-out: without it, NetSession sends per peer exactly as before")
{
    struct Recorder final : ITransport {
        std::vector<std::pair<ConnectionId, std::vector<std::uint8_t>>> sent;
        std::vector<std::vector<ConnectionId>> many;
        std::deque<NetEvent> ev;
        bool fans = false;
        void update() override {}
        void send(ConnectionId c, const std::uint8_t* d, std::size_t n, SendMode) override {
            sent.emplace_back(c, std::vector<std::uint8_t>(d, d + n));
        }
        bool poll(NetEvent& out) override {
            if (ev.empty()) return false;
            out = ev.front();
            ev.pop_front();
            return true;
        }
        void disconnect(ConnectionId) override {}
        std::size_t connectionCount() const override { return 3; }
        bool fansOut() const override { return fans; }
        void sendToMany(const ConnectionId* to, std::size_t count, const std::uint8_t* d,
                        std::size_t n, SendMode) override {
            many.emplace_back(to, to + count);
            sent.emplace_back(0, std::vector<std::uint8_t>(d, d + n));
        }
    };
    auto connect3 = [](Recorder& r) {
        for (ConnectionId c : { 1u, 2u, 3u }) r.ev.push_back(NetEvent{ NetEventType::Connected, c, {} });
    };

    Recorder p2p;
    connect3(p2p);
    NetSession s(&p2p, NetRole::Host);
    s.pump();
    s.send(1, kFanMsg, text("x"));
    const std::vector<std::uint8_t> one = p2p.sent.at(0).second;
    p2p.sent.clear();
    s.broadcast(kFanMsg, text("x"));
    s.broadcastExcept(2, kFanMsg, text("x"));
    s.sendTo({ 3, 1 }, kFanMsg, text("x"));
    const std::vector<ConnectionId> order{ 1, 2, 3, 1, 3, 3, 1 };
    REQUIRE(p2p.sent.size() == order.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        CHECK(p2p.sent[i].first == order[i]);
        CHECK(p2p.sent[i].second == one);   // the same bytes a single send() writes
    }
    CHECK(p2p.many.empty());

    Recorder fan;
    fan.fans = true;
    connect3(fan);
    NetSession f(&fan, NetRole::Host);
    f.pump();
    f.broadcast(kFanMsg, text("x"));
    f.broadcastExcept(2, kFanMsg, text("x"));
    f.sendTo({ 3, 1 }, kFanMsg, text("x"));
    f.sendTo({ 3 }, kFanMsg, text("x"));   // one recipient: a plain send
    REQUIRE(fan.many.size() == 3);
    CHECK(fan.many[0] == std::vector<ConnectionId>{ 1, 2, 3 });
    CHECK(fan.many[1] == std::vector<ConnectionId>{ 1, 3 });
    CHECK(fan.many[2] == std::vector<ConnectionId>{ 3, 1 });
    REQUIRE(fan.sent.size() == 4);
    CHECK(fan.sent[3].first == 3);
    for (auto& [c, bytes] : fan.sent) CHECK(bytes == one);
}

TEST_CASE("Relay fan-out: against a real collab_server (HE_TEST_RELAY=host:port)")
{
    const char* env = std::getenv("HE_TEST_RELAY");
    if (!env || !*env) {
        MESSAGE("HE_TEST_RELAY not set — skipped (start collab_server and point it there)");
        return;
    }
    RelayAddress addr;
    REQUIRE(parseRelayAddress(env, addr));
    const std::string session = "he-fan-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count() % 1000000007);
    FanSession fs(nullptr, addr, session, 2);
    FanEnd& c0 = *fs.clients[0];
    FanEnd& c1 = *fs.clients[1];
    const auto before = fs.host->t->stats();
    fs.host->s->send(fs.conn(0), kFanMsg, text("first, per pair"));
    for (int i = 0; i < 20; ++i) fs.host->s->broadcast(kFanMsg, text(std::to_string(i)));
    fs.host->s->broadcastExcept(fs.conn(1), kFanMsg, text("not for c1"));
    fs.host->s->send(fs.conn(1), kFanMsg, text("last, per pair"));
    REQUIRE(pumpFan(nullptr, fs.all, [&] { return c0.got.size() == 22 && c1.got.size() == 21; }));
    CHECK(c0.got.front() == "first, per pair");
    CHECK(c0.got.back() == "not for c1");
    CHECK(c1.got.back() == "last, per pair");
    for (int i = 0; i < 20; ++i) {
        CHECK(c0.got[static_cast<std::size_t>(i) + 1] == std::to_string(i));
        CHECK(c1.got[static_cast<std::size_t>(i)] == std::to_string(i));
    }
    // 20 broadcasts; "not for c1" has one recipient left and is a plain send.
    CHECK(fs.host->t->stats().relayFrames - before.relayFrames == 20);
    CHECK(fs.host->t->stats().pairCopies == before.pairCopies);
}
