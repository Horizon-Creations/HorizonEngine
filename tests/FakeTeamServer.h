#pragma once

// ─── A team server in a test (topic 197) ─────────────────────────────────────
// Shared by test_net_team.cpp (the client and controller against it) and
// test_team_ui.cpp (the Team window drawn over a controller signed in to it).
// Frames are decoded and encoded by hand rather than with TeamClient's own
// helpers, so a wrong byte on either side shows up. Include once per test file:
// everything is in an anonymous namespace.

#include "doctest.h"

#include <Net/RelayTransport.h>
#include <Net/TcpTransport.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace HE::Net;

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
// A 24-bit BMP, bottom-up, as stb_image reads it: `w` x `h` pixels of one
// colour. A picture file for the profile tests (topic 202).
void writeBmp(const std::string& path, int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    const int rowBytes = (w * 3 + 3) & ~3;
    const std::uint32_t size = 54 + static_cast<std::uint32_t>(rowBytes * h);
    std::vector<std::uint8_t> f(size, 0);
    auto le32 = [&](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) f[at + i] = std::uint8_t(v >> (8 * i));
    };
    f[0] = 'B'; f[1] = 'M';
    le32(2, size);
    le32(10, 54);
    le32(14, 40);
    le32(18, static_cast<std::uint32_t>(w));
    le32(22, static_cast<std::uint32_t>(h));
    f[26] = 1;
    f[28] = 24;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            std::uint8_t* px = &f[54 + std::size_t(y) * rowBytes + std::size_t(x) * 3];
            px[0] = b; px[1] = g; px[2] = r;
        }
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(f.data()),
                                               static_cast<std::streamsize>(f.size()));
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
        std::string token;      // the session token it logged in with (op 3), else empty
    };
    struct Session {
        std::string id, host, peer;
        bool open = false;
        std::set<std::string> invited;          // members with an open invitation
        ConnectionId          hostConn = kInvalidConnection;   // announced by this connection
    };

    std::unique_ptr<TcpTransport> t = TcpTransport::listenLoopback(0);
    std::map<ConnectionId, Conn>  conns;
    std::vector<std::string>      roster = { "Anna", "Bob" };
    std::set<std::string>         admins = { "Anna" };
    std::vector<Session>          sessions;
    std::set<std::string>         alsoOnline;     // members "online" on other connections
    std::set<ConnectionId>        refused;        // closed after a refusal; ignore the rest
    std::vector<std::uint8_t>     ops;            // every request op, in arrival order
    // Session tokens (topic 201): token → member. Kept as given, the real
    // server keeps only a SHA-256.
    std::map<std::string, std::string> tokens;
    std::vector<std::uint8_t>     loginOps;       // the op of every TEAM_LOGIN
    bool noTokens    = false;   // a server from before tokens: op 13 is bad-request
    // Member profiles (topic 202), by lower-cased name. The picture id is a
    // counter here; the real server hashes the pixels.
    struct Profile {
        std::uint8_t r = 0, g = 0, b = 0;
        std::uint8_t size = 0;
        std::vector<std::uint8_t> rgba;
        std::uint32_t id = 0;
    };
    std::map<std::string, Profile> profiles;
    std::uint32_t nextAvatarId = 0x100;
    int  avatarFetches = 0;     // GET_AVATAR requests answered
    bool noProfiles  = false;   // a server from before profiles: ops 16-19 are bad-request
    // Source control (topic 199): the team's projects, and every SC_TICKET
    // handed out (16 raw bytes → member, project), for FakeScServer to check.
    std::set<std::string> scProjects = { "game" };
    struct ScTicket { std::string member, project; };
    std::map<std::string, ScTicket> scTickets;
    bool noSourceControl = false;   // a server from before source control: op 20 is bad-request
    int  issued      = 0;
    int  logins      = 0;
    int  pings       = 0;
    bool silent      = false;   // read requests, answer nothing
    // A connection whose first frame is not TEAM_LOGIN is ignored instead of
    // failing the test: the relay half of a team session (CollabController's
    // HELLO to the same address) when a UI test presses Host or Join.
    bool ignoreStrangers = false;
    // Sent right before the reply to the next LIST_MEMBERS: the client must
    // take it as an event, not as the reply it is waiting for.
    std::vector<std::vector<std::uint8_t>> eventsBeforeNextList;
    // Sync connections on the same port, as on the real server (FakeScServer.h
    // installs these): true from scFrame = the frame was a sync frame.
    std::function<bool(ConnectionId, const std::vector<std::uint8_t>&)> scFrame;
    std::function<void(ConnectionId)>                                   scGone;

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
    static std::vector<std::uint8_t> invitedEvent(const Session& s) {
        auto f = header(9);
        f.push_back(5);
        str8(f, s.id);
        str8(f, s.host);
        str8(f, s.peer);
        return f;
    }
    static std::string fold(std::string s) {
        for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    }
    static std::vector<std::uint8_t> profileEvent(const std::string& who, const Profile& p) {
        auto f = header(9);
        f.push_back(8);
        str8(f, who);
        f.push_back(p.r);
        f.push_back(p.g);
        f.push_back(p.b);
        u32(f, p.id);
        return f;
    }
    static std::vector<std::uint8_t> scHeadEvent(const std::string& project, std::uint64_t rev,
                                                 const std::string& member) {
        auto f = header(9);
        f.push_back(9);
        str8(f, project);
        u32(f, std::uint32_t(rev >> 32));
        u32(f, std::uint32_t(rev));
        str8(f, member);
        return f;
    }
    // To every logged-in connection (the real server: every team connection of the team).
    void sendToAll(const std::vector<std::uint8_t>& f) {
        for (auto& [c, conn] : conns)
            if (conn.loggedIn) send(c, f);
    }
    // Sets a member's profile from the test (as if set on another editor) and
    // tells every logged-in connection.
    void setProfile(const std::string& who, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                    std::uint8_t size = 0, std::uint8_t fill = 0) {
        Profile& p = profiles[fold(who)];
        p.r = r; p.g = g; p.b = b;
        p.size = size;
        p.rgba.assign(std::size_t(size) * size * 4, fill);
        p.id = size ? nextAvatarId++ : 0;
        for (auto& [c, conn] : conns)
            if (conn.loggedIn) send(c, profileEvent(who, p));
    }
    // To every logged-in connection of `member`.
    void sendToMember(const std::string& member, const std::vector<std::uint8_t>& f) {
        for (auto& [c, conn] : conns)
            if (conn.loggedIn && sameName(conn.member, member)) send(c, f);
    }
    void dropAll() {
        for (auto& [c, _] : conns) t->disconnect(c);
        conns.clear();
    }
    // What the server does to live token connections when their token goes
    // (remove member, SET_PASSWORD elsewhere, Sign Out Everywhere): bad-login
    // with the reason, then close. Forgets the tokens of `member`.
    void revokeTokensOf(const std::string& member, const std::string& why = "session tokens revoked") {
        for (auto it = tokens.begin(); it != tokens.end();)
            it = sameName(it->second, member) ? tokens.erase(it) : std::next(it);
        std::vector<ConnectionId> kick;
        for (auto& [c, conn] : conns)
            if (conn.loggedIn && !conn.token.empty() && sameName(conn.member, member)) kick.push_back(c);
        for (ConnectionId c : kick) {
            status(c, 23, why);
            t->update();
            t->disconnect(c);
            conns.erase(c);
            refused.insert(c);
        }
    }

    void pump() {
        t->update();
        NetEvent ev;
        while (t->poll(ev)) {
            if (ev.type == NetEventType::Connected) { conns[ev.conn]; continue; }
            if (ev.type == NetEventType::Disconnected) {
                conns.erase(ev.conn);
                if (scGone) scGone(ev.conn);
                continue;
            }
            if (refused.count(ev.conn)) continue;   // what the client sent behind its login
            if (scFrame && scFrame(ev.conn, ev.data)) continue;
            frame(ev.conn, ev.data);
        }
    }

    void frame(ConnectionId c, const std::vector<std::uint8_t>& f) {
        if (ignoreStrangers && !conns[c].loggedIn && (f.size() < 6 || f[5] != 6)) {
            refused.insert(c);
            return;
        }
        REQUIRE(f.size() >= 6);
        REQUIRE(std::string(f.begin(), f.begin() + 4) == "HERL");
        REQUIRE(f[4] == 1);
        Conn& conn = conns[c];
        std::size_t pos = 6;
        if (!conn.loggedIn) {
            REQUIRE(f[5] == 6);                       // TEAM_LOGIN first
            const std::uint8_t op = f[pos++];
            CHECK((op == 1 || op == 2 || (op == 3 && !noTokens)));
            const std::string team     = readStr(f, pos);
            const std::string member   = readStr(f, pos);
            const std::string password = readStr(f, pos);
            CHECK(pos == f.size());
            CHECK(team == "studio");
            ++logins;
            loginOps.push_back(op);
            const bool good = op == 3 ? (tokens.count(password) && sameName(tokens[password], member))
                                      : password == "correct-horse";
            if (!good) {
                status(c, 23, "bad login");
                t->update();
                t->disconnect(c);
                conns.erase(c);
                refused.insert(c);
                return;
            }
            conn.loggedIn = true;
            conn.member   = member;
            if (op == 3) conn.token = password;
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
                    body.push_back(std::uint8_t((admins.count(name) ? 1 : 0) |
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
                    body.push_back(std::uint8_t((s.open ? 1 : 0) |
                                                (s.invited.count(conn.member) ? 2 : 0) |
                                                (s.hostConn == c ? 4 : 0)));
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
                s.hostConn = c;
                sessions.push_back(s);
                reply(c, id, 0);
                return;
            }
            case 2: {   // ADD_MEMBER: into the roster (the client lists again)
                const std::string name = readStr(f, pos);
                readStr(f, pos);                          // password: not kept
                if (f.at(pos) == 1) admins.insert(name);
                roster.push_back(name);
                reply(c, id, 0);
                return;
            }
            case 3: {   // REMOVE_MEMBER
                const std::string name = readStr(f, pos);
                roster.erase(std::remove_if(roster.begin(), roster.end(),
                                            [&](const std::string& n) { return sameName(n, name); }),
                             roster.end());
                alsoOnline.erase(name);
                reply(c, id, 0);
                return;
            }
            case 10: {  // INVITE: kept, and pushed to the member's editors
                const std::string want   = readStr(f, pos);
                const std::string member = readStr(f, pos);
                for (Session& s : sessions)
                    if (s.id == want) {
                        s.invited.insert(member);
                        sendToMember(member, invitedEvent(s));
                    }
                reply(c, id, 0);
                return;
            }
            case 11: {  // DECLINE: the invitation is gone
                const std::string want = readStr(f, pos);
                for (Session& s : sessions)
                    if (s.id == want) s.invited.erase(conn.member);
                reply(c, id, 0);
                return;
            }
            case 12: {  // JOIN
                const std::string want = readStr(f, pos);
                for (const Session& s : sessions)
                    if (s.id == want) {
                        if (!s.open && !s.invited.count(conn.member)) { reply(c, id, 14); return; }
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
            case 4: {   // SET_PASSWORD: revokes the member's tokens, drops the other token logins
                const std::string oldPw = readStr(f, pos);
                readStr(f, pos);
                if (oldPw != "correct-horse") { reply(c, id, 6); return; }
                const std::string self = conn.member;
                conn.token.clear();   // proven the password: may issue now
                revokeTokensOf(self, "password changed");
                reply(c, id, 0);
                return;
            }
            case 13: {  // ISSUE_TOKEN: not on a token login
                if (noTokens) { reply(c, id, 1); return; }
                if (!conn.token.empty()) { reply(c, id, 14); return; }
                static const char kHex[] = "0123456789abcdef";
                std::string token;
                ++issued;
                for (int i = 0; i < 64; ++i)
                    token.push_back(kHex[(static_cast<unsigned>(i) * 7u + static_cast<unsigned>(issued) * 13u) & 15u]);
                tokens[token] = conn.member;
                std::vector<std::uint8_t> body;
                str8(body, token);
                reply(c, id, 0, body);
                return;
            }
            case 14: {  // REVOKE_TOKEN (own only)
                const std::string token = readStr(f, pos);
                auto it = tokens.find(token);
                if (it == tokens.end() || !sameName(it->second, conn.member)) { reply(c, id, 17); return; }
                tokens.erase(it);
                reply(c, id, 0);
                return;
            }
            case 15: {  // REVOKE_ALL_TOKENS: other token logins of ours are kicked
                const std::string self = conn.member;
                conn.token.clear();   // the connection that asks stays open
                revokeTokensOf(self);
                reply(c, id, 0);
                return;
            }
            case 16:    // SET_COLOR
            case 17: {  // SET_AVATAR: the others hear it, the caller does not
                if (noProfiles) { reply(c, id, 1); return; }
                Profile& p = profiles[fold(conn.member)];
                if (op == 16) {
                    REQUIRE(f.size() == pos + 3);
                    p.r = f[pos]; p.g = f[pos + 1]; p.b = f[pos + 2];
                } else {
                    const std::uint8_t size = f.at(pos++);
                    if (size > 64 || f.size() - pos != std::size_t(size) * size * 4) { reply(c, id, 1); return; }
                    p.size = size;
                    p.rgba.assign(f.begin() + static_cast<std::ptrdiff_t>(pos), f.end());
                    p.id = size ? nextAvatarId++ : 0;
                }
                reply(c, id, 0);
                for (auto& [other, oc] : conns)
                    if (other != c && oc.loggedIn) send(other, profileEvent(conn.member, p));
                return;
            }
            case 18: {  // LIST_PROFILES
                if (noProfiles) { reply(c, id, 1); return; }
                std::vector<std::uint8_t> body = { 0, std::uint8_t(roster.size()) };
                for (const std::string& name : roster) {
                    const Profile p = profiles.count(fold(name)) ? profiles[fold(name)] : Profile{};
                    str8(body, name);
                    body.push_back(p.r);
                    body.push_back(p.g);
                    body.push_back(p.b);
                    u32(body, p.id);
                }
                reply(c, id, 0, body);
                return;
            }
            case 19: {  // GET_AVATAR
                if (noProfiles) { reply(c, id, 1); return; }
                const std::string who = readStr(f, pos);
                ++avatarFetches;
                const Profile p = profiles.count(fold(who)) ? profiles[fold(who)] : Profile{};
                std::vector<std::uint8_t> body = { p.size };
                body.insert(body.end(), p.rgba.begin(), p.rgba.end());
                reply(c, id, 0, body);
                return;
            }
            case 20: {  // SC_TICKET: an existing project, or an admin about to create one
                if (noSourceControl) { reply(c, id, 1); return; }
                const std::string project = readStr(f, pos);
                if (!scProjects.count(project) && !admins.count(conn.member)) { reply(c, id, 18); return; }
                std::string ticket(16, '\0');
                const std::size_t n = scTickets.size() + 1;
                for (std::size_t i = 0; i < ticket.size(); ++i)
                    ticket[i] = static_cast<char>((i * 31u + n * 17u) & 0xFF);
                scTickets[ticket] = ScTicket{ conn.member, project };
                reply(c, id, 0, std::vector<std::uint8_t>(ticket.begin(), ticket.end()));
                return;
            }
            default:
                reply(c, id, 0);
                return;
        }
    }
};

} // namespace
