#include "doctest.h"

#include "../src/HE_Editor/CollabController.h"
#include "../src/HE_Editor/TeamController.h"

#include <HorizonScene/HorizonWorld.h>

#include <Net/RelayTransport.h>
#include <Net/TcpTransport.h>
#include <Net/TeamClient.h>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace HE::Net;

// The team server client (topic 197, step 10): HE::Net::TeamClient speaks the
// team protocol of the relay server (docs/team-server-protocol-2026-10-13.md),
// and the editor's TeamController turns team actions into collab sessions.
//
// FakeTeamServer below decodes and encodes the frames by hand rather than with
// TeamClient's own helpers, so a wrong byte on either side shows up. One case
// at the bottom runs two editors against the real collab_server when
// HE_TEST_TEAM_SERVER=host:port names a running one.

namespace {

// ─── Wire helpers, written out independently of TeamClient ───────────────────
std::vector<std::uint8_t> header(std::uint8_t type) {
    return { 'H', 'E', 'R', 'L', 1, type };
}
void str8(std::vector<std::uint8_t>& f, const std::string& s) {
    f.push_back(static_cast<std::uint8_t>(s.size()));
    f.insert(f.end(), s.begin(), s.end());
}
void u32(std::vector<std::uint8_t>& f, std::uint32_t v) {
    f.push_back(std::uint8_t(v >> 24));
    f.push_back(std::uint8_t(v >> 16));
    f.push_back(std::uint8_t(v >> 8));
    f.push_back(std::uint8_t(v));
}
bool sameName(std::string a, std::string b) {
    for (char& ch : a) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (char& ch : b) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return a == b;
}
std::string readStr(const std::vector<std::uint8_t>& f, std::size_t& pos) {
    REQUIRE(pos < f.size());
    const std::size_t n = f[pos++];
    REQUIRE(pos + n <= f.size());
    std::string s(f.begin() + static_cast<std::ptrdiff_t>(pos),
                  f.begin() + static_cast<std::ptrdiff_t>(pos + n));
    pos += n;
    return s;
}

// ─── A team server in a test ─────────────────────────────────────────────────
struct FakeTeamServer {
    struct Conn {
        bool        loggedIn = false;
        std::string member;
    };
    struct Session { std::string id, host, peer; bool open = false; };

    std::unique_ptr<TcpTransport> t = TcpTransport::listenLoopback(0);
    std::map<ConnectionId, Conn>  conns;
    std::vector<std::string>      roster = { "Anna", "Bob" };
    std::vector<Session>          sessions;
    std::set<std::string>         alsoOnline;     // members "online" on other connections
    std::set<ConnectionId>        refused;        // closed after a refusal; ignore the rest
    std::vector<std::uint8_t>     ops;            // every request op, in arrival order
    int  logins      = 0;
    int  pings       = 0;
    bool silent      = false;   // read requests, answer nothing
    // Sent right before the reply to the next LIST_MEMBERS: the client must
    // take it as an event, not as the reply it is waiting for.
    std::vector<std::vector<std::uint8_t>> eventsBeforeNextList;

    RelayAddress address() const { return RelayAddress{ "127.0.0.1", t->boundPort() }; }

    void send(ConnectionId c, const std::vector<std::uint8_t>& f) {
        t->send(c, f, SendMode::ReliableOrdered);
    }
    void status(ConnectionId c, std::uint8_t code, const std::string& text = {}) {
        auto f = header(2);
        f.push_back(code);
        str8(f, text);
        send(c, f);
    }
    void reply(ConnectionId c, std::uint32_t id, std::uint8_t result,
               const std::vector<std::uint8_t>& body = {}) {
        auto f = header(8);
        u32(f, id);
        f.push_back(result);
        str8(f, result ? "refused by the fake" : "");
        f.insert(f.end(), body.begin(), body.end());
        send(c, f);
    }
    static std::vector<std::uint8_t> presence(const std::string& who, bool online) {
        auto f = header(9);
        f.push_back(1);
        str8(f, who);
        f.push_back(online ? 1 : 0);
        return f;
    }
    static std::vector<std::uint8_t> sessionEvent(const Session& s) {
        auto f = header(9);
        f.push_back(3);
        str8(f, s.id);
        str8(f, s.host);
        str8(f, s.peer);
        f.push_back(s.open ? 1 : 0);
        return f;
    }
    void dropAll() {
        for (auto& [c, _] : conns) t->disconnect(c);
        conns.clear();
    }

    void pump() {
        t->update();
        NetEvent ev;
        while (t->poll(ev)) {
            if (ev.type == NetEventType::Connected) { conns[ev.conn]; continue; }
            if (ev.type == NetEventType::Disconnected) { conns.erase(ev.conn); continue; }
            if (refused.count(ev.conn)) continue;   // what the client sent behind its login
            frame(ev.conn, ev.data);
        }
    }

    void frame(ConnectionId c, const std::vector<std::uint8_t>& f) {
        REQUIRE(f.size() >= 6);
        REQUIRE(std::string(f.begin(), f.begin() + 4) == "HERL");
        REQUIRE(f[4] == 1);
        Conn& conn = conns[c];
        std::size_t pos = 6;
        if (!conn.loggedIn) {
            REQUIRE(f[5] == 6);                       // TEAM_LOGIN first
            const std::uint8_t op = f[pos++];
            CHECK((op == 1 || op == 2));
            const std::string team     = readStr(f, pos);
            const std::string member   = readStr(f, pos);
            const std::string password = readStr(f, pos);
            CHECK(pos == f.size());
            CHECK(team == "studio");
            ++logins;
            if (password != "correct-horse") {
                status(c, 23, "bad login");
                t->update();
                t->disconnect(c);
                conns.erase(c);
                refused.insert(c);
                return;
            }
            conn.loggedIn = true;
            conn.member   = member;
            status(c, 4);
            return;
        }
        REQUIRE(f[5] == 7);                           // TEAM_REQUEST
        REQUIRE(f.size() >= 11);
        const std::uint32_t id = (std::uint32_t(f[6]) << 24) | (std::uint32_t(f[7]) << 16) |
                                 (std::uint32_t(f[8]) << 8) | f[9];
        const std::uint8_t op = f[10];
        pos = 11;
        ops.push_back(op);
        if (op == 5) ++pings;
        if (silent) return;
        switch (op) {
            case 1: {   // LIST_MEMBERS
                for (auto& e : eventsBeforeNextList) send(c, e);
                eventsBeforeNextList.clear();
                std::vector<std::uint8_t> body = { 0, std::uint8_t(roster.size()) };
                for (const std::string& name : roster) {
                    str8(body, name);
                    bool online = alsoOnline.count(name) > 0;
                    for (auto& [_, other] : conns) online |= other.loggedIn && sameName(other.member, name);
                    body.push_back(std::uint8_t((name == "Anna" ? 1 : 0) |
                                                (sameName(name, conn.member) ? 2 : 0) | (online ? 4 : 0)));
                }
                reply(c, id, 0, body);
                return;
            }
            case 9: {   // LIST_SESSIONS
                std::vector<std::uint8_t> body = { 0, std::uint8_t(sessions.size()) };
                for (const Session& s : sessions) {
                    str8(body, s.id);
                    str8(body, s.host);
                    str8(body, s.peer);
                    body.push_back(s.open ? 1 : 0);
                }
                reply(c, id, 0, body);
                return;
            }
            case 6: {   // OPEN_SESSION
                Session s;
                s.id   = readStr(f, pos);
                s.peer = readStr(f, pos);
                s.open = f.at(pos) == 1;
                s.host = conn.member;
                sessions.push_back(s);
                reply(c, id, 0);
                return;
            }
            case 12: {  // JOIN
                const std::string want = readStr(f, pos);
                for (const Session& s : sessions)
                    if (s.id == want) {
                        if (!s.open) { reply(c, id, 14); return; }
                        std::vector<std::uint8_t> body;
                        str8(body, s.id);
                        str8(body, s.host);
                        str8(body, s.peer);
                        reply(c, id, 0, body);
                        return;
                    }
                reply(c, id, 12);
                return;
            }
            default:
                reply(c, id, 0);
                return;
        }
    }
};

// Pumps the fake server and every client on one injected clock; returns once
// `done` holds. The wall-clock deadline only bounds a broken test.
template <typename Fn>
bool pumpTeam(FakeTeamServer& server, std::vector<TeamClient*> clients, std::uint64_t& now,
              Fn done, std::uint64_t stepMs = 16) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        server.pump();
        for (TeamClient* c : clients) c->update(now);
        if (done()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        now += stepMs;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace

TEST_CASE("Team: login and request bytes match the protocol")
{
    std::vector<std::uint8_t> login = header(6);
    login.push_back(2);
    str8(login, "studio");
    str8(login, "Anna");
    str8(login, "correct-horse");
    CHECK(TeamClient::encodeLogin(TeamClient::LoginOp::CreateTeam, "studio", "Anna",
                                  "correct-horse") == login);

    std::vector<std::uint8_t> fields;
    str8(fields, "s-1");
    str8(fields, "Bob");
    std::vector<std::uint8_t> invite = header(7);
    u32(invite, 0x01020304);
    invite.push_back(10);
    invite.insert(invite.end(), fields.begin(), fields.end());
    CHECK(TeamClient::encodeRequest(0x01020304, TeamClient::Op::Invite, fields) == invite);
}

TEST_CASE("Team: the join code of a team session is derived from its id")
{
    const std::string a = TeamClient::joinCodeForSession("abc123");
    CHECK(a.size() == 32);
    CHECK(a == TeamClient::joinCodeForSession("abc123"));   // both ends compute the same
    CHECK(a != TeamClient::joinCodeForSession("abc124"));
    CHECK(a.find_first_not_of("0123456789abcdef") == std::string::npos);
}

TEST_CASE("Team: names that cannot be valid are refused before connecting")
{
    const RelayAddress addr{ "127.0.0.1", 7777 };
    CHECK_FALSE(TeamClient::connect(addr, "my team", "Anna", "correct-horse"));
    CHECK_FALSE(TeamClient::connect(addr, "studio", "", "correct-horse"));
    CHECK_FALSE(TeamClient::connect(addr, "studio", "Anna", ""));
    CHECK_FALSE(TeamClient::connect(addr, "studio", "Anna", std::string(256, 'x')));
}

TEST_CASE("Team: login, roster and sessions; events between replies are taken by type")
{
    FakeTeamServer server;
    REQUIRE(server.t);
    server.sessions.push_back({ "s-open", "Bob", "host", true });

    auto c = TeamClient::connect(server.address(), "studio", "anna", "correct-horse");
    REQUIRE(c);
    // Asked before the link is even up: it goes out right behind the login.
    bool early = false;
    c->listSessions([&](const TeamClient::Reply& r) { early = r.ok(); });

    // Bob comes online and a session gets announced while our member list is
    // in flight: both land before its reply.
    server.alsoOnline.insert("Bob");
    server.sessions.push_back({ "s-new", "Bob", "host", false });
    server.eventsBeforeNextList.push_back(FakeTeamServer::presence("Bob", true));
    server.eventsBeforeNextList.push_back(FakeTeamServer::sessionEvent(server.sessions.back()));

    std::uint64_t now = 1;
    REQUIRE(pumpTeam(server, { c.get() }, now, [&] {
        return c->loggedIn() && c->members().size() == 2 && early &&
               c->findSession("s-new") != nullptr;
    }));
    CHECK(server.ops.front() == 9);              // the early request was first
    CHECK(c->member() == "Anna");                // the server's spelling, from the "you" flag
    CHECK(c->isAdmin());
    REQUIRE(c->findMember("bob"));               // names compare without case
    CHECK(c->findMember("bob")->online);
    REQUIRE(c->findSession("s-open"));
    CHECK(c->findSession("s-open")->open);

    TeamClient::Event ev;
    REQUIRE(c->pollEvent(ev));
    CHECK(ev.kind == TeamClient::EventKind::Presence);
    CHECK(ev.member == "Bob");
    CHECK(ev.flag);
    REQUIRE(c->pollEvent(ev));
    CHECK(ev.kind == TeamClient::EventKind::Session);
    CHECK(ev.session == "s-new");

    // A refusal keeps the connection; the reason reaches the callback.
    TeamClient::Reply refused;
    c->join("s-new", [&](const TeamClient::Reply& r) { refused = r; });
    REQUIRE(pumpTeam(server, { c.get() }, now, [&] { return refused.request != 0; }));
    CHECK(refused.result == TeamClient::Result::NotAllowed);
    CHECK(c->loggedIn());

    TeamClient::Reply joined;
    c->join("s-open", [&](const TeamClient::Reply& r) { joined = r; });
    REQUIRE(pumpTeam(server, { c.get() }, now, [&] { return joined.request != 0; }));
    CHECK(joined.ok());
    CHECK(joined.session == "s-open");
    CHECK(joined.host == "Bob");
    CHECK(joined.peer == "host");
}

TEST_CASE("Team: a refused login closes with the reason and is not retried")
{
    FakeTeamServer server;
    REQUIRE(server.t);

    auto c = TeamClient::connect(server.address(), "studio", "Anna", "wrong-password");
    REQUIRE(c);
    bool lost = false;
    c->listMembers([&](const TeamClient::Reply& r) {
        lost = r.result == TeamClient::Result::ConnectionLost;
    });
    std::uint64_t now = 1;
    REQUIRE(pumpTeam(server, { c.get() }, now, [&] {
        return c->state() == TeamClient::State::Closed;
    }));
    CHECK(c->closeCode() == 23);
    CHECK(c->loginRefused());
    CHECK(c->lastError().find("Wrong team, member name or password") != std::string::npos);
    CHECK(lost);   // the held request learns it will never be answered

    // The controller turns that into Failed and leaves it there.
    CollabController collab;
    TeamController team(collab);
    REQUIRE(team.signIn("127.0.0.1:" + std::to_string(server.t->boundPort()), "studio", "Anna",
                        "wrong-password"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::uint64_t t = 1;
    while (team.status() != TeamController::Status::Failed &&
           std::chrono::steady_clock::now() < deadline) {
        server.pump();
        team.update(t += 16);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(team.status() == TeamController::Status::Failed);
    const int logins = server.logins;
    for (int i = 0; i < 50; ++i) {
        server.pump();
        team.update(t += 1000);   // far past every backoff
    }
    CHECK(server.logins == logins);
    CHECK(team.lastError().find("Wrong team") != std::string::npos);
}

TEST_CASE("Team: a heartbeat every 20 s, and a server that stops answering is noticed")
{
    FakeTeamServer server;
    REQUIRE(server.t);
    auto c = TeamClient::connect(server.address(), "studio", "Anna", "correct-horse");
    REQUIRE(c);
    std::uint64_t now = 1;
    REQUIRE(pumpTeam(server, { c.get() }, now, [&] {
        return c->loggedIn() && c->members().size() == 2;
    }));
    CHECK(server.pings == 0);

    // A minute in 1-second steps: three pings, none earlier than due.
    const std::uint64_t start = now;
    REQUIRE(pumpTeam(server, { c.get() }, now, [&] { return now - start >= 60'000; }, 1000));
    // The third one left on the last update; give loopback a moment to deliver
    // it without moving the client's clock (which could send a fourth).
    const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (server.pings < 3 && std::chrono::steady_clock::now() < settle) {
        server.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(server.pings == 3);
    CHECK(c->loggedIn());

    // Now the server reads but never answers (lid shut on its side).
    server.silent = true;
    REQUIRE(pumpTeam(server, { c.get() }, now, [&] {
        return c->state() == TeamClient::State::Closed;
    }, 1000));
    CHECK(c->lastError().find("stopped answering") != std::string::npos);
    CHECK(c->closeCode() == 0);
    CHECK_FALSE(c->loginRefused());
    for (const TeamMember& m : c->members()) CHECK_FALSE(m.online);
}

TEST_CASE("Team: the controller reconnects after a lost link")
{
    FakeTeamServer server;
    REQUIRE(server.t);
    CollabController collab;
    TeamController team(collab);
    REQUIRE(team.signIn("127.0.0.1:" + std::to_string(server.t->boundPort()), "studio", "Anna",
                        "correct-horse"));

    std::uint64_t t = 1;
    auto pumpUntil = [&](auto done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            server.pump();
            team.update(t += 100);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    };
    REQUIRE(pumpUntil([&] { return team.signedIn() && team.members().size() == 2; }));
    CHECK(server.logins == 1);

    server.dropAll();
    REQUIRE(pumpUntil([&] { return team.status() == TeamController::Status::Reconnecting; }));
    TeamController::Notice n;
    REQUIRE(team.pollNotice(n));
    CHECK(n.error);
    REQUIRE(pumpUntil([&] { return team.signedIn(); }));
    CHECK(server.logins == 2);

    // Not signed in = nothing to ask; the reason says so instead of a crash.
    team.signOut();
    CHECK_FALSE(team.hostSession(true));
    CHECK(team.lastError().find("Sign in") != std::string::npos);
}

// ─── Two editors, the real server ────────────────────────────────────────────

namespace {

std::uint64_t wallMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::size_t entityCount(HorizonWorld& world) {
    std::size_t n = 0;
    world.registry().view<entt::entity>().each([&](auto) { ++n; });
    return n;
}

struct Editor {
    HorizonWorld     world;
    CollabController collab;
    TeamController   team{ collab };
    std::vector<TeamController::Notice> notices;
    Editor() { collab.setWorld(&world); }
    void update(std::uint64_t now) {
        collab.update(now);
        team.update(now);
        TeamController::Notice n;
        while (team.pollNotice(n)) notices.push_back(n);
    }
    int heardCount(const std::string& part) const {
        int n = 0;
        for (const auto& note : notices)
            if (note.text.find(part) != std::string::npos) ++n;
        return n;
    }
    bool heard(const std::string& part) const { return heardCount(part) > 0; }
    bool sees(const std::string& member, bool online) const {
        for (const auto& m : team.members())
            if (m.name == member) return m.online == online;
        return false;
    }
    const TeamSessionInfo* session(const std::string& id) const {
        for (const auto& s : team.sessions())
            if (s.session == id) return &s;
        return nullptr;
    }
};

template <typename Fn>
bool pumpEditors(std::vector<Editor*> eds, Fn done, std::chrono::seconds timeout = std::chrono::seconds(15)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const std::uint64_t now = wallMs();
        for (Editor* e : eds) e->update(now);
        if (done()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

} // namespace

TEST_CASE("Team: two editors through a real collab_server (HE_TEST_TEAM_SERVER=host:port)")
{
    const char* env = std::getenv("HE_TEST_TEAM_SERVER");
    if (!env || !*env) {
        MESSAGE("HE_TEST_TEAM_SERVER not set — skipped (start collab_server and point it there)");
        return;
    }
    // A fresh team per run: teams persist on the server.
    const std::string teamName = "he-test-" + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count() % 1000000007);

    Editor anna, bob;
    anna.world.createEntity("Cube");
    anna.world.createEntity("Sun");

    // Anna creates the team and takes Bob in.
    REQUIRE(anna.team.createTeam(env, teamName, "Anna", "anna-password"));
    REQUIRE(pumpEditors({ &anna }, [&] { return anna.team.signedIn() && anna.sees("Anna", true); }));
    CHECK(anna.team.isAdmin());
    REQUIRE(anna.team.addMember("Bob", "bob-password", false));
    REQUIRE(pumpEditors({ &anna }, [&] { return anna.team.members().size() == 2; }));
    CHECK(anna.sees("Bob", false));

    // Bob signs in from the other editor: each sees the other.
    REQUIRE(bob.team.signIn(env, teamName, "bob", "bob-password"));
    REQUIRE(pumpEditors({ &anna, &bob }, [&] {
        return bob.team.signedIn() && bob.sees("Anna", true) && anna.sees("Bob", true);
    }));
    CHECK(bob.team.member() == "Bob");
    CHECK_FALSE(bob.team.isAdmin());

    // Anna hosts, invitation-only. Bob sees the session but may not walk in.
    REQUIRE(anna.team.hostSession(false));
    const std::string sid = anna.team.hostedSession();
    REQUIRE_FALSE(sid.empty());
    REQUIRE(pumpEditors({ &anna, &bob }, [&] {
        return anna.collab.directoryStatus().find("Hosting through the team server") != std::string::npos &&
               bob.session(sid) != nullptr;
    }));
    CHECK_FALSE(bob.session(sid)->open);
    REQUIRE(bob.team.join(sid));
    REQUIRE(pumpEditors({ &anna, &bob }, [&] { return bob.heard("invitation-only"); }));
    CHECK_FALSE(bob.collab.active());

    // Invited, Bob first declines…
    REQUIRE(anna.team.invite("Bob"));
    REQUIRE(pumpEditors({ &anna, &bob }, [&] {
        return bob.heard("Anna invited you") && bob.session(sid) && bob.session(sid)->invited;
    }));
    REQUIRE(bob.team.decline(sid));
    REQUIRE(pumpEditors({ &anna, &bob }, [&] { return anna.heard("Bob declined"); }));

    // …then accepts the next invitation and lands in Anna's scene. Waiting
    // for the second "invited" itself: the flag in the list may still be the
    // first one's until the list Bob asked for after declining comes back.
    REQUIRE(anna.team.invite("Bob"));
    REQUIRE(pumpEditors({ &anna, &bob }, [&] { return bob.heardCount("Anna invited you") == 2; }));
    REQUIRE(bob.team.join(sid));
    REQUIRE(pumpEditors({ &anna, &bob }, [&] {
        return bob.collab.status() == CollabController::Status::Joined;
    }, std::chrono::seconds(30)));
    CHECK(bob.collab.teamSession());
    CHECK(entityCount(bob.world) == entityCount(anna.world));
    CHECK(pumpEditors({ &anna, &bob }, [&] { return anna.heard("Bob is joining"); }));

    // Opened up, Cleo — never invited (the server keeps Bob's invitation after
    // his join, so he would not prove it) — walks in on her own.
    bob.collab.leave();
    REQUIRE(anna.team.addMember("Cleo", "cleo-password", false));
    REQUIRE(anna.team.setSessionOpen(true));
    Editor cleo;
    REQUIRE(cleo.team.signIn(env, teamName, "Cleo", "cleo-password"));
    REQUIRE(pumpEditors({ &anna, &bob, &cleo }, [&] {
        return cleo.team.signedIn() && cleo.session(sid) && cleo.session(sid)->open &&
               !cleo.session(sid)->invited;
    }));
    REQUIRE(cleo.team.join(sid));
    REQUIRE(pumpEditors({ &anna, &bob, &cleo }, [&] {
        return cleo.collab.status() == CollabController::Status::Joined;
    }, std::chrono::seconds(30)));
    CHECK(entityCount(cleo.world) == entityCount(anna.world));
    CHECK(pumpEditors({ &anna, &bob, &cleo }, [&] { return anna.heard("Cleo is joining"); }));

    // Anna ends the session: the team stops seeing it.
    anna.collab.leave();
    REQUIRE(pumpEditors({ &anna, &bob, &cleo }, [&] {
        return bob.session(sid) == nullptr && cleo.session(sid) == nullptr &&
               anna.team.hostedSession().empty();
    }));
    bob.collab.leave();
    cleo.collab.leave();
    cleo.team.signOut();

    // Bob signs out: Anna sees him go.
    bob.team.signOut();
    REQUIRE(pumpEditors({ &anna }, [&] { return anna.sees("Bob", false); }));

    // Clean up the server's team list as far as the protocol allows.
    REQUIRE(anna.team.removeMember("Bob"));
    REQUIRE(anna.team.removeMember("Cleo"));
    REQUIRE(pumpEditors({ &anna }, [&] { return anna.team.members().size() == 1; }));
    anna.team.signOut();
}
