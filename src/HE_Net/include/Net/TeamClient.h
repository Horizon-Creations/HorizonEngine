#pragma once

// ─── HorizonNet — team server client (topic 197) ─────────────────────────────
// One logged-in connection to the team server: the relay (CollabSession,
// docs/relay-server-protocol.md) grew teams, members with passwords, presence
// and session announcements, specified in docs/team-server-protocol-2026-10-13.md
// and the server README's "Team-Server" section.
//
// The team connection is NOT a session link. It is how a member says "I am
// here", sees who else is, announces the collab session it hosts and hears that
// it was invited to one. The session itself still runs over RelayTransport
// (LISTEN/CONNECT/ACCEPT on the same server) exactly as before; this class only
// hands out what that needs — the relay session id and the host's peer id.
//
// Same framing and header as the relay ([u32 BE length] "HERL" | 1 | type), on
// its own TCP connection whose first frame is TEAM_LOGIN instead of HELLO:
//
//   client ── TEAM_LOGIN(op, team, member, password) ──▶ server   (op 3: a session token)
//   server ── STATUS logged-in | refusal + close ──────▶ client
//   client ── TEAM_REQUEST(id, op, …) ─────────────────▶ server   (any number)
//   server ── TEAM_REPLY(id, result, text, …) ─────────▶ client   (one per request)
//   server ── TEAM_EVENT(kind, …) ─────────────────────▶ client   (any time, between replies)
//
// Events arrive between replies at any moment, so every frame is dispatched by
// its type byte and replies are matched by request id — never "the next frame
// is the answer". The server closes a connection that is silent for 60 s, so
// update() sends a PING every kPingIntervalMs.
//
// Session tokens (topic 201, docs/team-login-persist-2026-10-14.md): a
// connection that logged in with the password asks ISSUE_TOKEN for a long-lived
// token, and a later connect() with LoginOp::Token presents it instead of the
// password. A token that is wrong, revoked or expired is a plain bad-login,
// indistinguishable from a wrong password; the server also pushes bad-login to
// a live token connection when the token is revoked under it.
//
// Poll-driven like every transport here: update() once per frame, nothing
// blocks, no threads. Passwords and tokens never reach a log, and session ids only
// abbreviated — for a team session the id is the key to the session (see
// joinCodeForSession).

#include "Net/NetCommon.h"
#include "Net/RelayTransport.h"   // RelayAddress; the team server IS the relay
// The full type, not a forward declaration: on MSVC the dllexport of this class
// defines its inline default constructor in every TU of the DLL, and that needs
// ~unique_ptr<TcpTransport> to see a complete type (as in RelayTransport.h).
#include "Net/TcpTransport.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace HE::Net {

struct TeamMember {
    std::string name;            // the spelling the member was created with
    bool        admin  = false;
    bool        you    = false;  // the member this connection is logged in as
    bool        online = false;  // at least one team connection (any editor of theirs)
};

struct TeamSessionInfo {
    std::string session;         // relay session id
    std::string host;            // member hosting it
    std::string peer;            // the host's relay peer id (CONNECT target)
    bool        open    = false; // others may join without an invitation
    bool        invited = false; // this member has an open invitation to it
    bool        hosting = false; // announced by THIS connection
};

class HE_NET_API TeamClient {
public:
    // ── Wire constants (public so tests and a fake server speak the same bytes) ──
    enum class FrameType : std::uint8_t {
        Status = 2, TeamLogin = 6, TeamRequest = 7, TeamReply = 8, TeamEvent = 9,
    };
    // Token: the session token goes where the password would.
    enum class LoginOp : std::uint8_t { Login = 1, CreateTeam = 2, Token = 3 };
    enum class StatusCode : std::uint8_t {
        LoggedIn = 4, BadHello = 16, BadVersion = 17, Timeout = 20, Full = 21,
        BadLogin = 23, TeamExists = 24, Invalid = 25, ServerError = 26,
    };
    enum class Op : std::uint8_t {
        ListMembers = 1, AddMember = 2, RemoveMember = 3, SetPassword = 4, Ping = 5,
        OpenSession = 6, CloseSession = 7, SetSessionOpen = 8, ListSessions = 9,
        Invite = 10, Decline = 11, Join = 12,
        IssueToken = 13, RevokeToken = 14, RevokeAllTokens = 15,
    };
    enum class Result : std::uint8_t {
        Ok = 0, BadRequest = 1, InvalidName = 2, InvalidPassword = 3, MemberExists = 4,
        NoSuchMember = 5, WrongPassword = 6, NotAdmin = 7, LastAdmin = 8, Full = 9,
        ServerError = 10, NotOnline = 11, NoSuchSession = 12, NotHost = 13,
        NotAllowed = 14, SessionExists = 15, NotInvited = 16, NoSuchToken = 17,
        // Never on the wire: the connection went away before the reply came.
        ConnectionLost = 255,
    };
    enum class EventKind : std::uint8_t {
        Presence = 1, MembersChanged = 2, Session = 3, SessionClosed = 4,
        Invited = 5, Joining = 6, Declined = 7,
    };

    // The server closes a connection after 60 s without a frame; three pings
    // per window survive one being late.
    static constexpr std::uint64_t kPingIntervalMs  = 20'000;
    // Connect + login, and any one reply. A server that does not answer a ping
    // within this is gone for practical purposes (lid closed on the far side of
    // a NAT, Wi-Fi dropped without a FIN), and saying so beats a roster that
    // keeps showing everybody online.
    static constexpr std::uint64_t kAnswerTimeoutMs = 30'000;

    enum class State : std::uint8_t {
        Connecting,   // TCP not up yet
        LoggingIn,    // TEAM_LOGIN sent, no STATUS yet
        LoggedIn,     // requests and events flow
        Closed,       // refused, lost or close()d — see closeCode()/lastError()
    };

    struct Reply {
        std::uint32_t request = 0;
        Op            op      = Op::Ping;
        Result        result  = Result::Ok;
        std::string   text;                 // the server's reason, may be empty
        // JOIN only: what the relay CONNECT needs.
        std::string   session, host, peer;
        // ISSUE_TOKEN only: the new session token (64 hex characters). A
        // secret: the receiver keeps it out of logs and wipes its copies.
        std::string   token;
        bool ok() const { return result == Result::Ok; }
    };
    using ReplyFn = std::function<void(const Reply&)>;

    // What the owner gets to see happen. Roster and session changes are folded
    // into members()/sessions() and bump revision(); these are the ones that
    // ask somebody to act.
    struct Event {
        EventKind   kind = EventKind::Presence;
        std::string session, host, peer, member;
        bool        flag = false;           // presence: online; session: open
    };

    // Starts connecting and logs in (or creates the team, whose first admin
    // `member` then is; or, with LoginOp::Token, presents the session token
    // passed as `password`) as soon as the TCP link is up. Null only when the
    // names cannot be valid or the OS refuses the connect on the spot; a
    // server that is down shows up as State::Closed from update().
    static std::unique_ptr<TeamClient> connect(const RelayAddress& server,
                                               const std::string& team,
                                               const std::string& member,
                                               const std::string& password,
                                               LoginOp op = LoginOp::Login);
    ~TeamClient();

    TeamClient(const TeamClient&)            = delete;
    TeamClient& operator=(const TeamClient&) = delete;

    // Pump the connection: read frames, run reply callbacks, apply events,
    // send the heartbeat. `nowMs` is a test seam; the no-argument form uses
    // the steady clock.
    void update();
    void update(std::uint64_t nowMs);
    void close();

    State              state() const { return m_state; }
    bool               loggedIn() const { return m_state == State::LoggedIn; }
    // The STATUS code the server closed us with (0: none — the link was lost
    // or close() was called) and a sentence for the UI.
    std::uint8_t       closeCode() const { return m_closeCode; }
    const std::string& lastError() const { return m_lastError; }
    // The server's own text with that STATUS ("session token revoked",
    // "password changed", …), which lastError() leaves out for bad-login.
    const std::string& closeText() const { return m_closeText; }
    // True when logging in again with the same name and password is pointless
    // (bad-login, invalid, team-exists): a reconnect loop must stop there.
    bool               loginRefused() const;

    const RelayAddress& server() const { return m_server; }
    const std::string&  team() const { return m_team; }
    // Our own name, in the server's spelling once the roster has arrived.
    const std::string&  member() const { return m_member; }
    bool                isAdmin() const;

    const std::vector<TeamMember>&      members() const { return m_members; }
    const std::vector<TeamSessionInfo>& sessions() const { return m_sessions; }
    const TeamMember*      findMember(const std::string& name) const;
    const TeamSessionInfo* findSession(const std::string& session) const;
    // Bumped on every change to members()/sessions(), so a panel can tell
    // "nothing happened" without comparing lists.
    std::uint64_t revision() const { return m_revision; }

    bool pollEvent(Event& out);

    // ── Requests ──
    // Each returns its request id (0 if the connection is closed — `done`
    // then never runs). Allowed from the moment connect() returns: requests
    // made before "logged-in" wait and go out right behind it. `done` runs
    // from update(), also with Result::ConnectionLost if the link dies first.
    // The cached lists are refreshed on their own after every change this
    // connection makes; `done` is for reporting the outcome.
    std::uint32_t listMembers(ReplyFn done = {});
    std::uint32_t listSessions(ReplyFn done = {});
    std::uint32_t addMember(const std::string& name, const std::string& password,
                            bool admin, ReplyFn done = {});
    std::uint32_t removeMember(const std::string& name, ReplyFn done = {});
    std::uint32_t setPassword(const std::string& oldPassword, const std::string& newPassword,
                              ReplyFn done = {});
    std::uint32_t openSession(const std::string& session, const std::string& peer,
                              bool open, ReplyFn done = {});
    std::uint32_t closeSession(const std::string& session, ReplyFn done = {});
    std::uint32_t setSessionOpen(const std::string& session, bool open, ReplyFn done = {});
    std::uint32_t invite(const std::string& session, const std::string& member,
                         ReplyFn done = {});
    std::uint32_t decline(const std::string& session, ReplyFn done = {});
    std::uint32_t join(const std::string& session, ReplyFn done = {});
    // Session tokens. issueToken() is refused (NotAllowed) on a connection that
    // logged in with a token, unless it has changed the password since.
    std::uint32_t issueToken(ReplyFn done = {});
    std::uint32_t revokeToken(const std::string& token, ReplyFn done = {});
    std::uint32_t revokeAllTokens(ReplyFn done = {});
    // Which credential this connection presented.
    LoginOp       loginOp() const { return m_loginOp; }

    // ── Helpers ──
    // The SecureTransport join secret of a team session, derived from its
    // relay session id (HMAC-SHA256 under a fixed label). The team server's
    // JOIN reply carries only session, host and peer, so both ends compute
    // the secret instead of sending it: whoever knows a team session's id can
    // enter it — the team's members and the server, which the team trusts
    // with its passwords already.
    static std::string joinCodeForSession(const std::string& sessionId);

    static const char* statusName(std::uint8_t code);
    static const char* resultName(std::uint8_t result);
    // A sentence for the UI: "wrong team, name or password", …
    static std::string describeStatus(std::uint8_t code, const std::string& serverText);
    static std::string describeResult(Result result, const std::string& serverText);

    // Frame payloads without TcpTransport's length prefix.
    static std::vector<std::uint8_t> encodeLogin(LoginOp op, const std::string& team,
                                                 const std::string& member,
                                                 const std::string& password);
    // `fields` are the op's fields, already encoded (str8/u8).
    static std::vector<std::uint8_t> encodeRequest(std::uint32_t request, Op op,
                                                   const std::vector<std::uint8_t>& fields);
    static void putStr8(std::vector<std::uint8_t>& out, const std::string& s);

private:
    TeamClient() = default;

    struct Pending {
        Op            op = Op::Ping;
        ReplyFn       done;
        std::uint64_t sentMs = 0;
        std::vector<std::uint8_t> held;   // made before the link was up; sent behind the login
    };

    std::uint32_t request(Op op, std::vector<std::uint8_t> fields, ReplyFn done);
    void          sendFrame(const std::vector<std::uint8_t>& payload);
    void          onFrame(const std::vector<std::uint8_t>& frame);
    void          onStatus(std::uint8_t code, const std::string& text);
    void          onReply(const std::uint8_t* p, std::size_t n);
    void          onEvent(const std::uint8_t* p, std::size_t n);
    void          applyReply(Reply& reply, const std::uint8_t* body, std::size_t n);
    void          fail(std::uint8_t code, std::string why);
    void          changed() { ++m_revision; }

    std::unique_ptr<TcpTransport> m_tcp;
    ConnectionId                  m_conn = kInvalidConnection;
    RelayAddress                  m_server;
    std::string                   m_team, m_member;
    std::vector<std::uint8_t>     m_loginFrame;   // holds the password until sent
    LoginOp                       m_loginOp = LoginOp::Login;

    State         m_state     = State::Connecting;
    std::uint8_t  m_closeCode = 0;
    std::string   m_lastError;
    std::string   m_closeText;
    std::uint64_t m_nowMs     = 0;
    std::uint64_t m_startMs   = 0;   // connect began (login deadline)
    std::uint64_t m_lastSentMs = 0;

    std::uint32_t                     m_nextRequest = 1;
    std::map<std::uint32_t, Pending>  m_pending;

    std::vector<TeamMember>      m_members;
    std::vector<TeamSessionInfo> m_sessions;
    std::uint64_t                m_revision = 0;
    std::deque<Event>            m_events;
    // Callbacks are collected and run after the frame is applied, so a callback
    // that issues a request (or closes us) never re-enters the frame loop.
    std::vector<std::pair<ReplyFn, Reply>> m_ready;
};

} // namespace HE::Net
