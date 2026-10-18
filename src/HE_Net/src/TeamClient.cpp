#include "Net/TeamClient.h"

#include "Net/TcpTransport.h"

#include "NetLog.h"

#include <Hpak/KeyDerivation.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <utility>

namespace HE::Net {
namespace {

constexpr char        kMagic[4]   = { 'H', 'E', 'R', 'L' };
constexpr std::uint8_t kVersion   = 1;
constexpr std::size_t kHeaderSize = 6;   // magic + version + type
// Team frames are small (the largest is a member list); a megabyte is far past
// anything legitimate and keeps a broken length prefix from allocating more.
constexpr std::uint32_t kMaxTeamFrame = 1u << 20;

std::uint64_t steadyMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

void putHeader(std::vector<std::uint8_t>& out, TeamClient::FrameType type) {
    out.insert(out.end(), kMagic, kMagic + 4);
    out.push_back(kVersion);
    out.push_back(static_cast<std::uint8_t>(type));
}

void putU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

// Sequential reader over one frame; every getter fails (and stays failed) once
// the frame is too short, so a parse is one chain of && and one check.
struct Reader {
    const std::uint8_t* p;
    std::size_t         n;
    std::size_t         pos = 0;
    bool u8(std::uint8_t& v) {
        if (pos + 1 > n) return false;
        v = p[pos++];
        return true;
    }
    bool u16(std::uint16_t& v) {
        if (pos + 2 > n) return false;
        v = static_cast<std::uint16_t>((p[pos] << 8) | p[pos + 1]);
        pos += 2;
        return true;
    }
    bool u32(std::uint32_t& v) {
        if (pos + 4 > n) return false;
        v = (std::uint32_t(p[pos]) << 24) | (std::uint32_t(p[pos + 1]) << 16) |
            (std::uint32_t(p[pos + 2]) << 8) | std::uint32_t(p[pos + 3]);
        pos += 4;
        return true;
    }
    bool u64(std::uint64_t& v) {
        std::uint32_t hi = 0, lo = 0;
        if (!u32(hi) || !u32(lo)) return false;
        v = (std::uint64_t(hi) << 32) | lo;
        return true;
    }
    bool str(std::string& s) {
        std::uint8_t len = 0;
        if (!u8(len) || pos + len > n) return false;
        s.assign(reinterpret_cast<const char*>(p + pos), len);
        pos += len;
        return true;
    }
    bool atEnd() const { return pos == n; }
};

// Names are compared without case, as the server does ("Connor" = "connor").
bool sameName(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

// The key a member's profile is kept under (the server folds the same way).
std::string foldName(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string withReason(std::string sentence, const std::string& serverText) {
    if (!serverText.empty()) sentence += " (" + serverText + ")";
    return sentence;
}

} // namespace

// ─── Construction ────────────────────────────────────────────────────────────

std::unique_ptr<TeamClient> TeamClient::connect(const RelayAddress& server,
                                                const std::string& team,
                                                const std::string& member,
                                                const std::string& password,
                                                LoginOp op) {
    // Names share the relay's id alphabet. A password longer than a str8 cannot
    // be sent at all; shorter-than-allowed ones are the server's to refuse, with
    // its own wording of the rule.
    if (!RelayTransport::isValidId(team) || !RelayTransport::isValidId(member) ||
        password.empty() || password.size() > 255 || server.host.empty() || server.port == 0)
        return nullptr;

    auto tcp = TcpTransport::connect(server.host, server.port);
    if (!tcp) return nullptr;
    tcp->setMaxFrameSize(kMaxTeamFrame);

    std::unique_ptr<TeamClient> c(new TeamClient());
    c->m_tcp        = std::move(tcp);
    c->m_server     = server;
    c->m_team       = team;
    c->m_member     = member;
    c->m_loginFrame = encodeLogin(op, team, member, password);
    c->m_loginOp    = op;
    HE_LOG_INFO(Net, "Team: %s team '%s' as '%s' at %s:%u%s",
                op == LoginOp::CreateTeam ? "creating" : "logging in to", team.c_str(),
                member.c_str(), server.host.c_str(), static_cast<unsigned>(server.port),
                op == LoginOp::Token ? " with a session token" : "");
    return c;
}

TeamClient::~TeamClient() {
    // The password sits in the login frame until it is sent; do not leave it
    // behind in freed memory longer than needed.
    std::fill(m_loginFrame.begin(), m_loginFrame.end(), std::uint8_t(0));
}

void TeamClient::close() {
    if (m_state == State::Closed) return;
    fail(0, "Signed out of the team server.");
}

// ─── Pump ────────────────────────────────────────────────────────────────────

void TeamClient::update() { update(steadyMs()); }

void TeamClient::update(std::uint64_t nowMs) {
    m_nowMs = nowMs;
    if (m_startMs == 0) m_startMs = nowMs ? nowMs : 1;

    if (m_state != State::Closed) {
        m_tcp->update();
        NetEvent ev;
        while (m_state != State::Closed && m_tcp->poll(ev)) {
            switch (ev.type) {
                case NetEventType::Connected: {
                    m_conn  = ev.conn;
                    m_state = State::LoggingIn;
                    sendFrame(m_loginFrame);
                    std::fill(m_loginFrame.begin(), m_loginFrame.end(), std::uint8_t(0));
                    m_loginFrame.clear();
                    // The server takes requests in the same write as the login,
                    // so whatever was asked for while connecting goes out now.
                    for (auto& [id, p] : m_pending) {
                        (void)id;
                        if (p.held.empty()) continue;
                        sendFrame(p.held);
                        p.held.clear();
                        p.sentMs = m_nowMs;
                    }
                    break;
                }
                case NetEventType::Data:
                    onFrame(ev.data);
                    break;
                case NetEventType::Disconnected:
                    if (m_state == State::Connecting)
                        fail(0, "Could not reach the team server at " + m_server.host + ":" +
                                    std::to_string(m_server.port) + ".");
                    else
                        fail(0, "The team server closed the connection.");
                    break;
            }
        }
    }

    if (m_state == State::Connecting || m_state == State::LoggingIn) {
        if (m_nowMs - m_startMs > kAnswerTimeoutMs)
            fail(0, m_state == State::Connecting
                        ? "Could not reach the team server at " + m_server.host + ":" +
                              std::to_string(m_server.port) + " (no answer)."
                        : std::string("The team server did not answer the login."));
    } else if (m_state == State::LoggedIn) {
        // Replies come in request order, so the oldest outstanding one is the
        // one that has waited longest.
        if (!m_pending.empty() && m_nowMs - m_pending.begin()->second.sentMs > kAnswerTimeoutMs)
            fail(0, "The team server stopped answering.");
        else if (m_nowMs - m_lastSentMs >= kPingIntervalMs)
            request(Op::Ping, {}, {});
    }

    // Callbacks last, from a local list: one that sends a request or closes
    // this client must not run inside the frame loop above.
    auto ready = std::move(m_ready);
    m_ready.clear();
    for (auto& [fn, reply] : ready)
        if (fn) fn(reply);
}

void TeamClient::sendFrame(const std::vector<std::uint8_t>& payload) {
    m_tcp->send(m_conn, payload, SendMode::ReliableOrdered);
    m_lastSentMs = m_nowMs;
}

void TeamClient::fail(std::uint8_t code, std::string why) {
    if (m_state == State::Closed) return;
    const bool wasLoggedIn = m_state == State::LoggedIn;
    m_state     = State::Closed;
    m_closeCode = code;
    m_lastError = std::move(why);
    if (m_conn != kInvalidConnection) m_tcp->disconnect(m_conn);
    std::fill(m_loginFrame.begin(), m_loginFrame.end(), std::uint8_t(0));
    m_loginFrame.clear();

    for (auto& [id, p] : m_pending) {
        Reply r;
        r.request = id;
        r.op      = p.op;
        r.result  = Result::ConnectionLost;
        r.text    = m_lastError;
        if (p.done) m_ready.emplace_back(std::move(p.done), std::move(r));
    }
    m_pending.clear();
    m_avatarQueue.clear();
    m_avatarFetches = 0;
    for (auto& [key, p] : m_profiles) {
        (void)key;
        p.queued = false;
    }

    // Without the connection nothing in the lists is current: our sessions
    // ended with it, and presence is unknown. Names (and pictures) stay for display.
    for (TeamMember& m : m_members) m.online = false;
    m_sessions.clear();
    changed();

    if (code != 0)
        HE_LOG_WARN(Net, "Team: closed by the server — %s", statusName(code));
    else if (wasLoggedIn)
        HE_LOG_INFO(Net, "Team: connection closed — %s", m_lastError.c_str());
    else
        HE_LOG_WARN(Net, "Team: %s", m_lastError.c_str());
}

bool TeamClient::loginRefused() const {
    return m_state == State::Closed &&
           (m_closeCode == std::uint8_t(StatusCode::BadLogin) ||
            m_closeCode == std::uint8_t(StatusCode::Invalid) ||
            m_closeCode == std::uint8_t(StatusCode::TeamExists));
}

// ─── Incoming frames ─────────────────────────────────────────────────────────

void TeamClient::onFrame(const std::vector<std::uint8_t>& f) {
    if (f.size() < kHeaderSize || std::memcmp(f.data(), kMagic, 4) != 0 || f[4] != kVersion) {
        HE_LOG_WARN(Net, "Team: frame is not team protocol v1 (%s)", detail::logBytes(f.size()).c_str());
        fail(0, "The server at " + m_server.host + " does not speak the team protocol.");
        return;
    }
    const std::uint8_t* body = f.data() + kHeaderSize;
    const std::size_t   n    = f.size() - kHeaderSize;

    // Dispatched by type, always: events interleave with replies.
    switch (static_cast<FrameType>(f[5])) {
        case FrameType::Status: {
            Reader r{ body, n };
            std::uint8_t code = 0;
            std::string  text;
            if (!r.u8(code) || !r.str(text)) {
                fail(0, "The team server sent a broken status frame.");
                return;
            }
            onStatus(code, text);
            return;
        }
        case FrameType::TeamReply: onReply(body, n); return;
        case FrameType::TeamEvent: onEvent(body, n); return;
        default:
            HE_LOG_WARN(Net, "Team: unexpected frame type %u", static_cast<unsigned>(f[5]));
            fail(0, "The team server sent a frame this editor does not know.");
            return;
    }
}

void TeamClient::onStatus(std::uint8_t code, const std::string& text) {
    if (code == std::uint8_t(StatusCode::LoggedIn)) {
        if (m_state != State::LoggingIn) return;
        m_state = State::LoggedIn;
        m_lastError.clear();
        HE_LOG_INFO(Net, "Team: logged in to '%s' as '%s'", m_team.c_str(), m_member.c_str());
        listMembers();
        listSessions();
        listProfiles();
        changed();
        return;
    }
    // Every other status ends the connection (the server closes after it).
    m_closeText = text;
    fail(code, describeStatus(code, text));
}

void TeamClient::onReply(const std::uint8_t* p, std::size_t n) {
    Reader r{ p, n };
    std::uint32_t id = 0;
    std::uint8_t  result = 0;
    std::string   text;
    if (!r.u32(id) || !r.u8(result) || !r.str(text)) {
        fail(0, "The team server sent a broken reply.");
        return;
    }
    auto it = m_pending.find(id);
    if (it == m_pending.end()) {
        HE_LOG_WARN(Net, "Team: reply to unknown request %u ignored", static_cast<unsigned>(id));
        return;
    }
    Reply reply;
    reply.request = id;
    reply.op      = it->second.op;
    reply.result  = static_cast<Result>(result);
    reply.text    = std::move(text);
    Pending pending = std::move(it->second);
    ReplyFn done    = std::move(pending.done);
    m_pending.erase(it);

    applyReply(reply, p + r.pos, n - r.pos, pending);
    if (m_state == State::Closed) return;   // a malformed body
    if (!reply.ok() && reply.op != Op::Ping)
        HE_LOG_INFO(Net, "Team: request %u refused — %s", static_cast<unsigned>(id),
                    resultName(result));
    if (done) m_ready.emplace_back(std::move(done), std::move(reply));
}

void TeamClient::applyReply(Reply& reply, const std::uint8_t* body, std::size_t n,
                            const Pending& pending) {
    if (reply.op == Op::GetAvatar) {
        // Whatever the answer, this fetch is over: make room for the next.
        if (m_avatarFetches > 0) --m_avatarFetches;
        auto pit = m_profiles.find(pending.member);
        if (pit != m_profiles.end()) pit->second.queued = false;
        if (reply.ok()) {
            Reader r{ body, n };
            std::uint8_t size = 0;
            const bool ok = r.u8(size) && size <= kMaxAvatarSize &&
                            n - r.pos == std::size_t(size) * size * 4;
            if (!ok) {
                fail(0, "The team server sent a reply this editor cannot read.");
                return;
            }
            if (pit != m_profiles.end()) {
                Profile& prof = pit->second;
                if (pending.avatarId == prof.avatarId) {
                    prof.avatarSize = size;
                    prof.avatarRgba.assign(body + r.pos, body + n);
                    prof.pixelsId   = prof.avatarId;
                    mergeProfiles();
                    changed();
                } else if (prof.avatarId != 0 && !prof.queued) {
                    // The picture changed while this one was on its way.
                    prof.queued = true;
                    m_avatarQueue.push_back(pending.member);
                }
            }
        }
        fetchAvatars();
        return;
    }
    if (!reply.ok()) {
        // A server from before profiles does not know the op. Not an error:
        // members simply have no colour or picture there.
        if (reply.op == Op::ListProfiles && reply.result == Result::BadRequest) {
            if (m_profilesSupported)
                HE_LOG_INFO(Net, "Team: the server has no member profiles (colour, picture)");
            m_profilesSupported = false;
            changed();
        }
        return;
    }
    Reader r{ body, n };
    switch (reply.op) {
        case Op::ListMembers: {
            std::uint16_t count = 0;
            if (!r.u16(count)) break;
            std::vector<TeamMember> list;
            list.reserve(count);
            for (std::uint16_t i = 0; i < count; ++i) {
                TeamMember m;
                std::uint8_t flags = 0;
                if (!r.str(m.name) || !r.u8(flags)) break;
                m.admin  = (flags & 1) != 0;
                m.you    = (flags & 2) != 0;
                m.online = (flags & 4) != 0;
                if (m.you) m_member = m.name;   // the server's spelling
                list.push_back(std::move(m));
            }
            if (list.size() != count) break;
            m_members = std::move(list);
            mergeProfiles();
            changed();
            return;
        }
        case Op::ListProfiles: {
            std::uint16_t count = 0;
            if (!r.u16(count)) break;
            std::uint16_t i = 0;
            for (; i < count; ++i) {
                std::string   name;
                TeamColor     color;
                std::uint32_t avatarId = 0;
                if (!r.str(name) || !r.u8(color.r) || !r.u8(color.g) || !r.u8(color.b) ||
                    !r.u32(avatarId))
                    break;
                setProfile(name, color, avatarId);
            }
            if (i != count) break;
            m_profilesSupported = true;
            mergeProfiles();
            fetchAvatars();
            changed();
            return;
        }
        case Op::SetColor:
        case Op::SetAvatar:
            // Our own change comes back as no event; the list is the server's view.
            listProfiles();
            return;
        case Op::ListSessions: {
            std::uint16_t count = 0;
            if (!r.u16(count)) break;
            std::vector<TeamSessionInfo> list;
            list.reserve(count);
            for (std::uint16_t i = 0; i < count; ++i) {
                TeamSessionInfo s;
                std::uint8_t flags = 0;
                if (!r.str(s.session) || !r.str(s.host) || !r.str(s.peer) || !r.u8(flags)) break;
                s.open    = (flags & 1) != 0;
                s.invited = (flags & 2) != 0;
                s.hosting = (flags & 4) != 0;
                list.push_back(std::move(s));
            }
            if (list.size() != count) break;
            m_sessions = std::move(list);
            changed();
            return;
        }
        case Op::Join:
            if (!r.str(reply.session) || !r.str(reply.host) || !r.str(reply.peer)) break;
            listSessions();   // the invitation, if there was one, is used up
            return;
        case Op::IssueToken:
            if (!r.str(reply.token) || reply.token.empty()) break;
            return;
        case Op::ScTicket:
            if (n != kScTicketSize) break;
            reply.ticket.assign(reinterpret_cast<const char*>(body), n);
            return;
        case Op::AddMember:
        case Op::RemoveMember:
            listMembers();
            return;
        case Op::OpenSession:
        case Op::CloseSession:
        case Op::SetSessionOpen:
        case Op::Decline:
            // The server does not echo our own changes as events; ask for the
            // list instead of patching it, so it is the server's view.
            listSessions();
            return;
        default:
            return;
    }
    fail(0, "The team server sent a reply this editor cannot read.");
}

void TeamClient::onEvent(const std::uint8_t* p, std::size_t n) {
    Reader r{ p, n };
    std::uint8_t kind = 0;
    if (!r.u8(kind)) {
        fail(0, "The team server sent a broken event.");
        return;
    }
    Event ev;
    ev.kind = static_cast<EventKind>(kind);
    std::uint8_t flag = 0;
    bool ok = true;
    switch (ev.kind) {
        case EventKind::Presence:
            ok = r.str(ev.member) && r.u8(flag);
            if (!ok) break;
            ev.flag = flag != 0;
            if (TeamMember* m = const_cast<TeamMember*>(findMember(ev.member))) {
                m->online = ev.flag;
                changed();
            } else {
                listMembers();   // somebody we have not listed yet
            }
            break;
        case EventKind::MembersChanged:
            listMembers();
            break;
        case EventKind::Session: {
            ok = r.str(ev.session) && r.str(ev.host) && r.str(ev.peer) && r.u8(flag);
            if (!ok) break;
            ev.flag = flag != 0;
            auto* s = const_cast<TeamSessionInfo*>(findSession(ev.session));
            if (!s) {
                m_sessions.emplace_back();
                s = &m_sessions.back();
                s->session = ev.session;
            }
            s->host = ev.host;
            s->peer = ev.peer;
            s->open = ev.flag;
            changed();
            break;
        }
        case EventKind::SessionClosed:
            ok = r.str(ev.session);
            if (!ok) break;
            m_sessions.erase(std::remove_if(m_sessions.begin(), m_sessions.end(),
                                            [&](const TeamSessionInfo& s) {
                                                return s.session == ev.session;
                                            }),
                             m_sessions.end());
            changed();
            break;
        case EventKind::Invited: {
            ok = r.str(ev.session) && r.str(ev.host) && r.str(ev.peer);
            if (!ok) break;
            auto* s = const_cast<TeamSessionInfo*>(findSession(ev.session));
            if (!s) {
                m_sessions.emplace_back();
                s = &m_sessions.back();
                s->session = ev.session;
            }
            s->host    = ev.host;
            s->peer    = ev.peer;
            s->invited = true;
            changed();
            break;
        }
        case EventKind::Joining:
        case EventKind::Declined:
            ok = r.str(ev.session) && r.str(ev.member);
            break;
        case EventKind::Profile:
            ok = r.str(ev.member) && r.u8(ev.color.r) && r.u8(ev.color.g) && r.u8(ev.color.b) &&
                 r.u32(ev.avatarId);
            if (!ok) break;
            setProfile(ev.member, ev.color, ev.avatarId);
            mergeProfiles();
            fetchAvatars();
            changed();
            break;
        case EventKind::ScHead:
            ok = r.str(ev.project) && r.u64(ev.rev) && r.str(ev.member);
            break;
        default:
            // A newer server's event: skip it rather than drop the connection.
            HE_LOG_DEBUG(Net, "Team: unknown event kind %u ignored", static_cast<unsigned>(kind));
            return;
    }
    if (!ok) {
        fail(0, "The team server sent a broken event.");
        return;
    }
    m_events.push_back(std::move(ev));
}

// ─── Profiles ────────────────────────────────────────────────────────────────

void TeamClient::setProfile(const std::string& name, TeamColor color, std::uint32_t avatarId) {
    const std::string key = foldName(name);
    Profile& p = m_profiles[key];
    p.color    = color;
    p.avatarId = avatarId;
    if (avatarId == 0) {
        p.avatarRgba.clear();
        p.avatarSize = 0;
        p.pixelsId   = 0;
    } else if (avatarId != p.pixelsId && !p.queued) {
        p.queued = true;
        m_avatarQueue.push_back(key);
    }
}

void TeamClient::mergeProfiles() {
    for (TeamMember& m : m_members) {
        auto it = m_profiles.find(foldName(m.name));
        if (it == m_profiles.end()) continue;
        const Profile& p = it->second;
        m.color    = p.color;
        m.avatarId = p.avatarId;
        // Until a new picture arrives the old one stays up, rather than the
        // initial flashing in between.
        if (m.avatarSize != p.avatarSize || m.avatarRgba != p.avatarRgba) {
            m.avatarSize = p.avatarSize;
            m.avatarRgba = p.avatarRgba;
        }
    }
}

void TeamClient::fetchAvatars() {
    while (m_state == State::LoggedIn && m_avatarFetches < kAvatarFetchesInFlight &&
           !m_avatarQueue.empty()) {
        const std::string key = std::move(m_avatarQueue.front());
        m_avatarQueue.pop_front();
        auto it = m_profiles.find(key);
        if (it == m_profiles.end()) continue;
        Profile& p = it->second;
        if (p.avatarId == 0 || p.avatarId == p.pixelsId) {
            p.queued = false;
            continue;
        }
        std::vector<std::uint8_t> fields;
        putStr8(fields, key);
        const std::uint32_t id = request(Op::GetAvatar, std::move(fields), {});
        if (id == 0) return;
        Pending& pending = m_pending[id];
        pending.member   = key;
        pending.avatarId = p.avatarId;
        ++m_avatarFetches;
    }
}

std::uint32_t TeamClient::setColor(TeamColor color, ReplyFn done) {
    return request(Op::SetColor, { color.r, color.g, color.b }, std::move(done));
}

std::uint32_t TeamClient::setAvatar(std::uint16_t size, const std::vector<std::uint8_t>& rgba,
                                    ReplyFn done) {
    if (size > kMaxAvatarSize || rgba.size() != std::size_t(size) * size * 4) return 0;
    std::vector<std::uint8_t> fields;
    fields.reserve(1 + rgba.size());
    fields.push_back(static_cast<std::uint8_t>(size));
    fields.insert(fields.end(), rgba.begin(), rgba.end());
    return request(Op::SetAvatar, std::move(fields), std::move(done));
}

std::uint32_t TeamClient::listProfiles(ReplyFn done) {
    return request(Op::ListProfiles, {}, std::move(done));
}

bool TeamClient::pollEvent(Event& out) {
    if (m_events.empty()) return false;
    out = std::move(m_events.front());
    m_events.pop_front();
    return true;
}

// ─── Lookups ─────────────────────────────────────────────────────────────────

const TeamMember* TeamClient::findMember(const std::string& name) const {
    for (const TeamMember& m : m_members)
        if (sameName(m.name, name)) return &m;
    return nullptr;
}

const TeamSessionInfo* TeamClient::findSession(const std::string& session) const {
    for (const TeamSessionInfo& s : m_sessions)
        if (s.session == session) return &s;
    return nullptr;
}

bool TeamClient::isAdmin() const {
    for (const TeamMember& m : m_members)
        if (m.you) return m.admin;
    return false;
}

// ─── Requests ────────────────────────────────────────────────────────────────

std::uint32_t TeamClient::request(Op op, std::vector<std::uint8_t> fields, ReplyFn done) {
    if (m_state == State::Closed) return 0;
    std::uint32_t id = m_nextRequest++;
    if (id == 0) id = m_nextRequest++;
    Pending p;
    p.op   = op;
    p.done = std::move(done);
    std::vector<std::uint8_t> frame = encodeRequest(id, op, fields);
    if (m_state == State::Connecting) {
        p.held = std::move(frame);
    } else {
        sendFrame(frame);
        p.sentMs = m_nowMs;
    }
    m_pending.emplace(id, std::move(p));
    return id;
}

std::uint32_t TeamClient::listMembers(ReplyFn done) { return request(Op::ListMembers, {}, std::move(done)); }
std::uint32_t TeamClient::listSessions(ReplyFn done) { return request(Op::ListSessions, {}, std::move(done)); }

std::uint32_t TeamClient::addMember(const std::string& name, const std::string& password,
                                    bool admin, ReplyFn done) {
    std::vector<std::uint8_t> f;
    putStr8(f, name);
    putStr8(f, password);
    f.push_back(admin ? 1 : 0);
    return request(Op::AddMember, std::move(f), std::move(done));
}

std::uint32_t TeamClient::removeMember(const std::string& name, ReplyFn done) {
    std::vector<std::uint8_t> f;
    putStr8(f, name);
    return request(Op::RemoveMember, std::move(f), std::move(done));
}

std::uint32_t TeamClient::setPassword(const std::string& oldPassword,
                                      const std::string& newPassword, ReplyFn done) {
    std::vector<std::uint8_t> f;
    putStr8(f, oldPassword);
    putStr8(f, newPassword);
    return request(Op::SetPassword, std::move(f), std::move(done));
}

std::uint32_t TeamClient::openSession(const std::string& session, const std::string& peer,
                                      bool open, ReplyFn done) {
    std::vector<std::uint8_t> f;
    putStr8(f, session);
    putStr8(f, peer);
    f.push_back(open ? 1 : 0);
    return request(Op::OpenSession, std::move(f), std::move(done));
}

std::uint32_t TeamClient::closeSession(const std::string& session, ReplyFn done) {
    std::vector<std::uint8_t> f;
    putStr8(f, session);
    return request(Op::CloseSession, std::move(f), std::move(done));
}

std::uint32_t TeamClient::setSessionOpen(const std::string& session, bool open, ReplyFn done) {
    std::vector<std::uint8_t> f;
    putStr8(f, session);
    f.push_back(open ? 1 : 0);
    return request(Op::SetSessionOpen, std::move(f), std::move(done));
}

std::uint32_t TeamClient::invite(const std::string& session, const std::string& member,
                                 ReplyFn done) {
    std::vector<std::uint8_t> f;
    putStr8(f, session);
    putStr8(f, member);
    return request(Op::Invite, std::move(f), std::move(done));
}

std::uint32_t TeamClient::decline(const std::string& session, ReplyFn done) {
    std::vector<std::uint8_t> f;
    putStr8(f, session);
    return request(Op::Decline, std::move(f), std::move(done));
}

std::uint32_t TeamClient::join(const std::string& session, ReplyFn done) {
    std::vector<std::uint8_t> f;
    putStr8(f, session);
    return request(Op::Join, std::move(f), std::move(done));
}

std::uint32_t TeamClient::issueToken(ReplyFn done) {
    return request(Op::IssueToken, {}, std::move(done));
}

std::uint32_t TeamClient::revokeToken(const std::string& token, ReplyFn done) {
    std::vector<std::uint8_t> f;
    putStr8(f, token);
    const std::uint32_t id = request(Op::RevokeToken, f, std::move(done));
    std::fill(f.begin(), f.end(), std::uint8_t(0));
    return id;
}

std::uint32_t TeamClient::revokeAllTokens(ReplyFn done) {
    return request(Op::RevokeAllTokens, {}, std::move(done));
}

std::uint32_t TeamClient::scTicket(const std::string& project, ReplyFn done) {
    std::vector<std::uint8_t> f;
    putStr8(f, project);
    return request(Op::ScTicket, std::move(f), std::move(done));
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

std::string TeamClient::joinCodeForSession(const std::string& sessionId) {
    static constexpr char kLabel[] = "HorizonEngine team session v1";
    std::uint8_t mac[32];
    KeyDerivation::hmac(reinterpret_cast<const std::uint8_t*>(sessionId.data()), sessionId.size(),
                        reinterpret_cast<const std::uint8_t*>(kLabel), sizeof(kLabel) - 1, mac);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    // 128 bits, the strength of a generated join secret.
    for (int i = 0; i < 16; ++i) {
        out.push_back(kHex[mac[i] >> 4]);
        out.push_back(kHex[mac[i] & 0xF]);
    }
    return out;
}

void TeamClient::putStr8(std::vector<std::uint8_t>& out, const std::string& s) {
    const std::size_t n = std::min<std::size_t>(s.size(), 255);
    out.push_back(static_cast<std::uint8_t>(n));
    out.insert(out.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}

std::vector<std::uint8_t> TeamClient::encodeLogin(LoginOp op, const std::string& team,
                                                  const std::string& member,
                                                  const std::string& password) {
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + 4 + team.size() + member.size() + password.size());
    putHeader(out, FrameType::TeamLogin);
    out.push_back(static_cast<std::uint8_t>(op));
    putStr8(out, team);
    putStr8(out, member);
    putStr8(out, password);
    return out;
}

std::vector<std::uint8_t> TeamClient::encodeRequest(std::uint32_t request, Op op,
                                                    const std::vector<std::uint8_t>& fields) {
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + 5 + fields.size());
    putHeader(out, FrameType::TeamRequest);
    putU32(out, request);
    out.push_back(static_cast<std::uint8_t>(op));
    out.insert(out.end(), fields.begin(), fields.end());
    return out;
}

const char* TeamClient::statusName(std::uint8_t code) {
    switch (code) {
        case 4:  return "logged-in";
        case 16: return "bad-hello";
        case 17: return "bad-version";
        case 20: return "timeout";
        case 21: return "full";
        case 23: return "bad-login";
        case 24: return "team-exists";
        case 25: return "invalid";
        case 26: return "server-error";
        default: return RelayTransport::statusName(code);
    }
}

const char* TeamClient::resultName(std::uint8_t result) {
    static constexpr const char* kNames[] = {
        "ok", "bad-request", "invalid-name", "invalid-password", "member-exists",
        "no-such-member", "wrong-password", "not-admin", "last-admin", "full",
        "server-error", "not-online", "no-such-session", "not-host", "not-allowed",
        "session-exists", "not-invited", "no-such-token", "no-such-project",
    };
    if (result < sizeof(kNames) / sizeof(kNames[0])) return kNames[result];
    if (result == std::uint8_t(Result::ConnectionLost)) return "connection-lost";
    return "unknown";
}

std::string TeamClient::describeStatus(std::uint8_t code, const std::string& text) {
    switch (static_cast<StatusCode>(code)) {
        case StatusCode::BadLogin:
            // The server deliberately does not say which of the three was wrong.
            return "Wrong team, member name or password — or this member was removed from the team.";
        case StatusCode::TeamExists:  return "A team with that name already exists.";
        case StatusCode::Invalid:     return withReason("The team server refused the name or password.", text);
        case StatusCode::Full:        return withReason("The team server is full.", text);
        case StatusCode::ServerError: return withReason("The team server could not save the change.", text);
        case StatusCode::Timeout:     return "The team server dropped the connection after a minute without a heartbeat.";
        case StatusCode::BadVersion:  return "The team server speaks a different protocol version than this editor.";
        case StatusCode::BadHello:    return withReason("The team server did not understand this editor.", text);
        default:
            return withReason(std::string("The team server closed the connection: ") +
                                  statusName(code) + ".", text);
    }
}

std::string TeamClient::describeResult(Result result, const std::string& text) {
    switch (result) {
        case Result::Ok:              return {};
        case Result::InvalidName:     return withReason("That name is not allowed (1-64 letters, digits, - or _).", text);
        case Result::InvalidPassword: return withReason("That password is not allowed (8-255 bytes).", text);
        case Result::MemberExists:    return "The team already has a member with that name.";
        case Result::NoSuchMember:    return "The team has no member with that name.";
        case Result::WrongPassword:   return "The current password is wrong.";
        case Result::NotAdmin:        return "Only a team admin can do that.";
        case Result::LastAdmin:       return "A team needs at least one admin.";
        case Result::Full:            return withReason("The team is full.", text);
        case Result::NotOnline:       return "That member is not online.";
        case Result::NoSuchSession:   return "That session has ended.";
        case Result::NotHost:         return "Only the editor hosting the session can do that.";
        case Result::NotAllowed:      return "That session is invitation-only.";
        case Result::SessionExists:   return "That session id is already in use.";
        case Result::NotInvited:      return "There is no invitation to decline.";
        case Result::NoSuchToken:     return "That sign-in was already revoked.";
        case Result::NoSuchProject:   return "The team has no such project on its server (a team admin creates it).";
        case Result::ConnectionLost:  return text.empty() ? std::string("The connection to the team server was lost.") : text;
        default:
            return withReason(std::string("The team server refused: ") +
                                  resultName(static_cast<std::uint8_t>(result)) + ".", text);
    }
}

} // namespace HE::Net
