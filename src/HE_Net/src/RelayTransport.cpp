#include "Net/RelayTransport.h"

#include "Net/SessionDirectory.h"
#include "Net/TcpTransport.h"

#include "NetLog.h"

#include <Hpak/KeyDerivation.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace HE::Net {
namespace {

constexpr char        kMagic[4]       = { 'H', 'E', 'R', 'L' };
constexpr std::size_t kHeaderSize     = 6;   // magic + version + type
constexpr std::uint32_t kMaxBackoffMs = 30'000;

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

void putHeader(std::vector<std::uint8_t>& out, RelayTransport::FrameType type) {
    out.insert(out.end(), kMagic, kMagic + 4);
    out.push_back(RelayTransport::kProtocolVersion);
    out.push_back(static_cast<std::uint8_t>(type));
}

void putStr8(std::vector<std::uint8_t>& out, const std::string& s) {
    // Every field this side writes is an id (≤ 64), a 16-byte token or a short
    // reason, so the clamp never cuts anything real; it only keeps a caller's
    // mistake from producing a length byte that disagrees with the bytes.
    const std::size_t n = std::min<std::size_t>(s.size(), 255);
    out.push_back(static_cast<std::uint8_t>(n));
    out.insert(out.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}

// Reads one str8 at `pos`; false if the frame is too short for it.
bool getStr8(const std::vector<std::uint8_t>& in, std::size_t& pos, std::string& out) {
    if (pos >= in.size()) return false;
    const std::size_t n = in[pos++];
    if (in.size() - pos < n) return false;
    out.assign(reinterpret_cast<const char*>(in.data() + pos), n);
    pos += n;
    return true;
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool parsePort(const std::string& digits, std::uint16_t& out) {
    if (digits.empty() || digits.size() > 5) return false;
    unsigned long v = 0;
    for (const char ch : digits) {
        if (ch < '0' || ch > '9') return false;
        v = v * 10 + static_cast<unsigned long>(ch - '0');
    }
    if (v == 0 || v > 65535) return false;
    out = static_cast<std::uint16_t>(v);
    return true;
}

} // namespace

// ─── Address parsing ─────────────────────────────────────────────────────────

bool parseRelayAddress(const std::string& text, RelayAddress& out, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    const std::string t = trim(text);
    if (t.empty()) return fail("no relay address");

    std::string host;
    std::string port;
    if (t.front() == '[') {
        // "[2001:db8::1]:7777" — the brackets are what make an IPv6 port readable.
        const std::size_t close = t.find(']');
        if (close == std::string::npos || close == 1) return fail("unterminated [IPv6] address");
        host = t.substr(1, close - 1);
        const std::string rest = t.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') return fail("expected :port after the ]");
            port = rest.substr(1);
            if (port.empty()) return fail("missing port after the colon");
        }
    } else {
        const std::size_t first = t.find(':');
        if (first != t.rfind(':'))
            return fail("an IPv6 address needs brackets, e.g. [2001:db8::1]:7777");
        if (first == std::string::npos) {
            host = t;
        } else {
            host = t.substr(0, first);
            port = t.substr(first + 1);
            if (port.empty()) return fail("missing port after the colon");
        }
    }
    if (host.empty()) return fail("missing host name");
    for (const char ch : host) {
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == '/')
            return fail("host name contains a space or a slash");
    }

    std::uint16_t p = RelayTransport::kDefaultPort;
    if (!port.empty() && !parsePort(port, p)) return fail("port must be 1-65535");

    out.host = std::move(host);
    out.port = p;
    if (error) error->clear();
    return true;
}

std::string effectiveRelayAddress(const std::string& configured) {
    if (const char* env = std::getenv("HE_NET_RELAY"); env && *env) {
        const std::string v = trim(env);
        if (v == "off" || v == "none") return {};
        return v;
    }
    return trim(configured);
}

// ─── Wire helpers ────────────────────────────────────────────────────────────

bool RelayTransport::isValidId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

std::string RelayTransport::sessionIdForJoinCode(const std::string& joinCode) {
    static constexpr char kLabel[] = "HorizonEngine relay session v1";
    std::uint8_t mac[32];
    KeyDerivation::hmac(reinterpret_cast<const std::uint8_t*>(joinCode.data()), joinCode.size(),
                        reinterpret_cast<const std::uint8_t*>(kLabel), sizeof(kLabel) - 1, mac);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out = "g-";
    // 128 bits: as unguessable as a minted session id, and well under 64 chars.
    for (int i = 0; i < 16; ++i) {
        out.push_back(kHex[mac[i] >> 4]);
        out.push_back(kHex[mac[i] & 0xF]);
    }
    return out;
}

std::vector<std::uint8_t> RelayTransport::encodeHello(Role role, const std::string& session,
                                                      const std::string& self,
                                                      const std::string& target,
                                                      const std::string& token) {
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + 1 + 4 + session.size() + self.size() + target.size() + token.size());
    putHeader(out, FrameType::Hello);
    out.push_back(static_cast<std::uint8_t>(role));
    putStr8(out, session);
    putStr8(out, self);
    putStr8(out, target);
    putStr8(out, token);
    return out;
}

std::vector<std::uint8_t> RelayTransport::encodeStatus(StatusCode code, const std::string& text) {
    std::vector<std::uint8_t> out;
    putHeader(out, FrameType::Status);
    out.push_back(static_cast<std::uint8_t>(code));
    putStr8(out, text);
    return out;
}

std::vector<std::uint8_t> RelayTransport::encodeIncoming(const std::string& peer,
                                                         const std::string& token) {
    std::vector<std::uint8_t> out;
    putHeader(out, FrameType::Incoming);
    putStr8(out, peer);
    putStr8(out, token);
    return out;
}

const char* RelayTransport::statusName(std::uint8_t code) {
    switch (static_cast<StatusCode>(code)) {
        case StatusCode::Listening:   return "listening";
        case StatusCode::Waiting:     return "waiting";
        case StatusCode::Paired:      return "paired";
        case StatusCode::Subscribed:  return "subscribed";
        case StatusCode::BadHello:    return "bad-hello";
        case StatusCode::BadVersion:  return "bad-version";
        case StatusCode::IdTaken:     return "id-taken";
        case StatusCode::NoSuchPeer:  return "no-such-peer";
        case StatusCode::Timeout:     return "timeout";
        case StatusCode::Full:        return "full";
        case StatusCode::NotExpected: return "not-expected";
    }
    return "unknown";
}

// ─── Construction ────────────────────────────────────────────────────────────

RelayTransport::~RelayTransport() = default;   // links close their sockets

std::unique_ptr<RelayTransport> RelayTransport::host(const RelayAddress& relay,
                                                     const std::string& sessionId,
                                                     const std::string& selfId) {
    if (!isValidId(sessionId) || !isValidId(selfId)) {
        HE_LOG_ERROR(Net, "Relay host refused: session or peer id is not 1-64 chars of "
                          "[A-Za-z0-9_-] (session %s, peer '%s')",
                     detail::logSessionId(sessionId).c_str(), selfId.c_str());
        return nullptr;
    }
    std::unique_ptr<RelayTransport> t(new RelayTransport());
    t->m_relay     = relay;
    t->m_sessionId = sessionId;
    t->m_selfId    = selfId;
    t->m_isHost    = true;
    HE_LOG_INFO(Net, "Relay host: registering session %s as '%s' at %s:%u",
                detail::logSessionId(sessionId).c_str(), selfId.c_str(),
                relay.host.c_str(), static_cast<unsigned>(relay.port));
    t->m_listener = t->openLink(Role::Listen, {}, {});
    if (!t->m_listener) {
        // Refused on the spot (nothing listens there right now) or not
        // resolvable — the socket layer does not tell the two apart. Either way
        // the host keeps trying: a relay that restarts must not end a session,
        // and lastError() says what is wrong in the meantime.
        HE_LOG_WARN(Net, "Relay host: cannot reach %s:%u yet — retrying",
                    relay.host.c_str(), static_cast<unsigned>(relay.port));
        t->scheduleListenerRetry(nowMs());
    }
    return t;
}

std::unique_ptr<RelayTransport> RelayTransport::connect(const RelayAddress& relay,
                                                        const std::string& sessionId,
                                                        const std::string& hostId,
                                                        const std::string& selfId) {
    // Random, because two clients that chose the same name would refuse each
    // other with id-taken. Prefixed so a relay log line says which side it is.
    const std::string self = selfId.empty()
        ? "c-" + SessionDirectory::newSessionId().substr(0, 12)
        : selfId;
    if (!isValidId(sessionId) || !isValidId(hostId) || !isValidId(self)) {
        HE_LOG_ERROR(Net, "Relay connect refused: session or peer id is not 1-64 chars of "
                          "[A-Za-z0-9_-] (session %s, host '%s', self '%s')",
                     detail::logSessionId(sessionId).c_str(), hostId.c_str(), self.c_str());
        return nullptr;
    }
    std::unique_ptr<RelayTransport> t(new RelayTransport());
    t->m_relay     = relay;
    t->m_sessionId = sessionId;
    t->m_selfId    = self;
    auto link = t->openLink(Role::Connect, hostId, {});
    if (!link) {
        HE_LOG_ERROR(Net, "Relay connect: cannot start a connection to %s:%u",
                     relay.host.c_str(), static_cast<unsigned>(relay.port));
        return nullptr;
    }
    // The id exists from the start, as TcpTransport::connect's does, so a
    // failed attempt can be reported as Disconnected for it.
    link->outer = t->m_nextId++;
    HE_LOG_INFO(Net, "Relay connect: session %s as '%s' to '%s' via %s:%u (conn %llu)",
                detail::logSessionId(sessionId).c_str(), self.c_str(), hostId.c_str(),
                relay.host.c_str(), static_cast<unsigned>(relay.port),
                static_cast<unsigned long long>(link->outer));
    t->m_links.push_back(std::move(link));
    return t;
}

std::unique_ptr<RelayTransport::Link> RelayTransport::openLink(Role role, const std::string& peer,
                                                               const std::string& token) {
    auto tcp = TcpTransport::connect(m_relay.host, m_relay.port);
    if (!tcp) {
        m_lastError = "cannot reach the relay at " + m_relay.host + ":" +
                      std::to_string(m_relay.port);
        return nullptr;
    }
    auto l   = std::make_unique<Link>();
    l->tcp   = std::move(tcp);
    l->role  = role;
    l->peer  = peer;
    l->token = token;
    return l;
}

// ─── Pump ────────────────────────────────────────────────────────────────────

void RelayTransport::update() {
    if (m_isHost) {
        if (m_listener) {
            serviceLink(*m_listener);
            if (m_listener->dead) m_listener.reset();
        } else if (nowMs() >= m_listenerRetryAtMs) {
            m_listener = openLink(Role::Listen, {}, {});
            if (m_listener) {
                HE_LOG_INFO(Net, "Relay host: reconnecting the listener to %s:%u",
                            m_relay.host.c_str(), static_cast<unsigned>(m_relay.port));
            } else {
                scheduleListenerRetry(nowMs());
            }
        }
    }

    // Index loop: serviceLink on the listener above may have appended ACCEPT
    // links, and nothing below appends while iterating.
    for (std::size_t i = 0; i < m_links.size(); ++i) {
        if (!m_links[i]->dead) serviceLink(*m_links[i]);
    }
    m_links.erase(std::remove_if(m_links.begin(), m_links.end(),
                                 [](const std::unique_ptr<Link>& l) { return l->dead; }),
                  m_links.end());
}

void RelayTransport::serviceLink(Link& l) {
    l.tcp->update();
    NetEvent ev;
    while (!l.dead && l.tcp->poll(ev)) {
        l.inner = ev.conn;
        switch (ev.type) {
            case NetEventType::Connected: {
                if (l.stage != Stage::Connecting) break;
                const std::string& target = l.role == Role::Listen ? std::string() : l.peer;
                l.tcp->send(l.inner, encodeHello(l.role, m_sessionId, m_selfId, target, l.token),
                            SendMode::ReliableOrdered);
                l.stage = Stage::Hello;
                break;
            }
            case NetEventType::Data:
                if (l.stage == Stage::Paired) {
                    // The peer's frame, passed through untouched: the relay
                    // forwards TcpTransport framing 1:1, so this is the frame
                    // the far end's send() produced.
                    m_events.push_back(NetEvent{ NetEventType::Data, l.outer, std::move(ev.data) });
                } else if (l.stage == Stage::Subscribed) {
                    // One broadcast payload, header already stripped by the relay.
                    m_broadcasts.push_back(Broadcast{ l.forConn, std::move(ev.data) });
                } else {
                    onControlFrame(l, ev.data);
                }
                break;
            case NetEventType::Disconnected:
                onLinkClosed(l, l.stage == Stage::Connecting
                                    ? "the relay could not be reached"
                                    : "the relay closed the connection");
                break;
        }
    }
}

void RelayTransport::onControlFrame(Link& l, const std::vector<std::uint8_t>& f) {
    auto protocolError = [&](const char* what) {
        HE_LOG_WARN(Net, "Relay: %s (%s) — closing that relay connection", what,
                    detail::logBytes(f.size()).c_str());
        m_lastError = std::string("relay protocol error: ") + what;
        l.tcp->disconnect(l.inner);
        onLinkClosed(l, "protocol error");
    };

    if (f.size() < kHeaderSize || std::memcmp(f.data(), kMagic, 4) != 0)
        return protocolError("expected a relay control frame before 'paired'");
    if (f[4] != kProtocolVersion)
        return protocolError("the relay speaks a different protocol version");

    std::size_t pos = kHeaderSize;
    switch (static_cast<FrameType>(f[5])) {
        case FrameType::Status: {
            if (pos >= f.size()) return protocolError("short STATUS");
            const std::uint8_t code = f[pos++];
            std::string text;
            if (!getStr8(f, pos, text)) return protocolError("short STATUS");

            if (code >= static_cast<std::uint8_t>(StatusCode::BadHello)) {
                // A refusal. The relay closes after it; closing first is the same
                // outcome without waiting for the FIN.
                m_lastError = std::string(statusName(code)) + (text.empty() ? "" : ": " + text);
                HE_LOG_WARN(Net, "Relay refused the %s for session %s: %s",
                            l.role == Role::Listen ? "listener"
                            : l.role == Role::Accept ? "accept"
                            : l.role == Role::Subscribe ? "subscription" : "connect",
                            detail::logSessionId(m_sessionId).c_str(), m_lastError.c_str());
                l.tcp->disconnect(l.inner);
                onLinkClosed(l, "refused");
                return;
            }
            const auto sc = static_cast<StatusCode>(code);
            if (sc == StatusCode::Listening && l.role == Role::Listen && l.stage == Stage::Hello) {
                l.stage             = Stage::Listening;
                m_listening         = true;
                m_listenerBackoffMs = 1000;
                m_lastError.clear();
                HE_LOG_INFO(Net, "Relay host: listening as '%s' in session %s",
                            m_selfId.c_str(), detail::logSessionId(m_sessionId).c_str());
                return;
            }
            if (sc == StatusCode::Waiting && l.role == Role::Connect && l.stage == Stage::Hello) {
                l.stage = Stage::Waiting;
                HE_LOG_INFO(Net, "Relay: waiting for '%s' to accept (conn %llu)",
                            l.peer.c_str(), static_cast<unsigned long long>(l.outer));
                return;
            }
            if (sc == StatusCode::Paired && l.role != Role::Listen &&
                (l.stage == Stage::Hello || l.stage == Stage::Waiting)) {
                l.stage = Stage::Paired;
                if (l.outer == 0) l.outer = m_nextId++;   // a host's new peer
                l.token.clear();
                m_events.push_back(NetEvent{ NetEventType::Connected, l.outer, {} });
                HE_LOG_INFO(Net, "Relay: paired with '%s' (conn %llu)",
                            l.peer.c_str(), static_cast<unsigned long long>(l.outer));
                for (auto& msg : l.held) l.tcp->send(l.inner, msg, SendMode::ReliableOrdered);
                l.held.clear();
                return;
            }
            if (sc == StatusCode::Subscribed && l.role == Role::Subscribe && l.stage == Stage::Hello) {
                l.stage = Stage::Subscribed;
                HE_LOG_INFO(Net, "Relay: subscribed to '%s' broadcasts (conn %llu)",
                            l.peer.c_str(), static_cast<unsigned long long>(l.forConn));
                return;
            }
            return protocolError("unexpected STATUS for this connection");
        }
        case FrameType::Incoming: {
            if (l.role != Role::Listen || l.stage != Stage::Listening)
                return protocolError("INCOMING on a connection that is not a listener");
            std::string peer, token;
            if (!getStr8(f, pos, peer) || !getStr8(f, pos, token))
                return protocolError("short INCOMING");
            if (!isValidId(peer)) return protocolError("INCOMING with an invalid peer id");
            HE_LOG_INFO(Net, "Relay host: '%s' wants to join — accepting", peer.c_str());
            auto link = openLink(Role::Accept, peer, token);
            if (!link) {
                HE_LOG_WARN(Net, "Relay host: could not open the accept connection for '%s'",
                            peer.c_str());
                return;
            }
            m_links.push_back(std::move(link));
            return;
        }
        case FrameType::Gone: {
            std::string peer;
            if (l.role != Role::Listen || !getStr8(f, pos, peer))
                return protocolError("unexpected GONE");
            // Nothing to undo: an ACCEPT already on its way is refused with
            // not-expected and its link closes without ever being a peer.
            HE_LOG_INFO(Net, "Relay host: '%s' gave up before being accepted", peer.c_str());
            return;
        }
        default:
            return protocolError("unknown control frame type");
    }
}

void RelayTransport::onLinkClosed(Link& l, const char* why) {
    if (l.dead) return;
    l.dead = true;

    if (l.role == Role::Listen) {
        const bool wasListening = m_listening;
        m_listening = false;
        if (m_lastError.empty() || wasListening) m_lastError = why;
        HE_LOG_WARN(Net, "Relay host: listener connection lost (%s) — retrying in %u ms; "
                         "peers already paired keep running",
                    why, static_cast<unsigned>(m_listenerBackoffMs));
        scheduleListenerRetry(nowMs());
        return;
    }

    if (l.role == Role::Subscribe) {
        if (l.stage != Stage::Subscribed) {
            // Refused or never reached: the pair is untouched, and the caller
            // simply never sees subscribed() — it stays on per-pair delivery.
            HE_LOG_WARN(Net, "Relay: subscription to '%s' not granted — %s (conn %llu)",
                        l.peer.c_str(), why, static_cast<unsigned long long>(l.forConn));
            return;
        }
        // Granted and now gone (relay dropped a stalled subscriber, or the
        // host's listener went away). What it would have carried is lost, so
        // the pair goes too: a session that silently misses broadcasts is
        // worse than one that says it lost the connection.
        Link* pair = findLink(l.forConn, Role::Connect);
        m_lastError = std::string("the relay's broadcast stream ended (") + why + ")";
        HE_LOG_WARN(Net, "Relay: broadcast stream from '%s' ended — %s; closing the link "
                         "(conn %llu), broadcasts after this point would be missing",
                    l.peer.c_str(), why, static_cast<unsigned long long>(l.forConn));
        if (pair && !pair->dead) {
            pair->tcp->disconnect(pair->inner);
            pair->dead = true;
            m_events.push_back(NetEvent{ NetEventType::Disconnected, pair->outer, {} });
        }
        return;
    }

    if (l.role == Role::Connect) {
        // Before "paired" this is a failed connect, after it a dropped link;
        // either way the caller's one connection is gone.
        if (l.stage != Stage::Paired && m_lastError.empty()) m_lastError = why;
        HE_LOG_INFO(Net, "Relay: link to '%s' closed — %s (conn %llu)", l.peer.c_str(), why,
                    static_cast<unsigned long long>(l.outer));
        m_events.push_back(NetEvent{ NetEventType::Disconnected, l.outer, {} });
        // Its subscription ends with it (the relay ends it too).
        if (Link* sub = findLink(l.outer, Role::Subscribe); sub && !sub->dead) {
            sub->tcp->disconnect(sub->inner);
            sub->dead = true;
        }
        return;
    }

    // Accept: only a paired one was ever a peer the caller knows about.
    if (l.stage == Stage::Paired) {
        HE_LOG_INFO(Net, "Relay host: '%s' left — %s (conn %llu)", l.peer.c_str(), why,
                    static_cast<unsigned long long>(l.outer));
        m_events.push_back(NetEvent{ NetEventType::Disconnected, l.outer, {} });
    } else {
        HE_LOG_INFO(Net, "Relay host: accepting '%s' failed — %s", l.peer.c_str(), why);
    }
}

void RelayTransport::scheduleListenerRetry(std::uint64_t now) {
    m_listenerRetryAtMs = now + m_listenerBackoffMs;
    m_listenerBackoffMs = std::min(m_listenerBackoffMs * 2, kMaxBackoffMs);
}

RelayTransport::Link* RelayTransport::findLink(ConnectionId conn, Role role) {
    for (auto& l : m_links) {
        if (l->role != role) continue;
        if ((role == Role::Subscribe ? l->forConn : l->outer) == conn) return l.get();
    }
    return nullptr;
}

// ─── Broadcast ───────────────────────────────────────────────────────────────

bool RelayTransport::sendBroadcast(const std::uint8_t* data, std::size_t len) {
    if (!m_isHost || !m_listener || m_listener->dead || m_listener->stage != Stage::Listening)
        return false;
    std::vector<std::uint8_t> frame;
    frame.reserve(kHeaderSize + len);
    putHeader(frame, FrameType::Broadcast);
    frame.insert(frame.end(), data, data + len);
    m_listener->tcp->send(m_listener->inner, frame, SendMode::ReliableOrdered);
    return true;
}

bool RelayTransport::subscribe(ConnectionId conn) {
    Link* pair = findLink(conn, Role::Connect);
    if (m_isHost || !pair || pair->dead || pair->stage != Stage::Paired) return false;
    if (Link* sub = findLink(conn, Role::Subscribe); sub && !sub->dead) return false;
    auto link = openLink(Role::Subscribe, pair->peer, {});
    if (!link) {
        HE_LOG_WARN(Net, "Relay: cannot open the subscription to '%s' (conn %llu) — staying "
                         "on per-link delivery",
                    pair->peer.c_str(), static_cast<unsigned long long>(conn));
        return false;
    }
    link->forConn = conn;
    HE_LOG_INFO(Net, "Relay: subscribing to '%s' broadcasts (conn %llu)", pair->peer.c_str(),
                static_cast<unsigned long long>(conn));
    m_links.push_back(std::move(link));
    return true;
}

bool RelayTransport::subscribed(ConnectionId conn) const {
    for (const auto& l : m_links) {
        if (l->role == Role::Subscribe && l->forConn == conn && !l->dead)
            return l->stage == Stage::Subscribed;
    }
    return false;
}

bool RelayTransport::pollBroadcast(ConnectionId& conn, std::vector<std::uint8_t>& out) {
    if (m_broadcasts.empty()) return false;
    conn = m_broadcasts.front().conn;
    out  = std::move(m_broadcasts.front().data);
    m_broadcasts.pop_front();
    return true;
}

// ─── ITransport ──────────────────────────────────────────────────────────────

void RelayTransport::send(ConnectionId conn, const std::uint8_t* data,
                          std::size_t len, SendMode /*mode*/) {
    // Over the relay every mode is reliable and ordered: it is one TCP stream.
    for (auto& l : m_links) {
        if (l->outer != conn || l->dead) continue;
        if (l->stage == Stage::Paired) {
            l->tcp->send(l->inner, data, len, SendMode::ReliableOrdered);
        } else {
            // Not ours to put on the wire before "paired": the relay would
            // take it, but a refusal would then have eaten it silently.
            l->held.emplace_back(data, data + len);
        }
        return;
    }
    HE_LOG_DEBUG(Net, "Relay send dropped: connection %llu is unknown (%s)",
                 static_cast<unsigned long long>(conn), detail::logBytes(len).c_str());
}

bool RelayTransport::poll(NetEvent& out) {
    if (m_events.empty()) return false;
    out = std::move(m_events.front());
    m_events.pop_front();
    return true;
}

void RelayTransport::disconnect(ConnectionId conn) {
    // Local close: no Disconnected event for the initiator (ITransport contract).
    // The link's subscription, if any, goes with it.
    if (Link* sub = findLink(conn, Role::Subscribe)) {
        sub->tcp->disconnect(sub->inner);
        sub->dead = true;
    }
    for (auto it = m_links.begin(); it != m_links.end(); ++it) {
        if ((*it)->outer != conn || (*it)->role == Role::Subscribe) continue;
        HE_LOG_INFO(Net, "Relay: closing link to '%s' locally (conn %llu)",
                    (*it)->peer.c_str(), static_cast<unsigned long long>(conn));
        (*it)->tcp->disconnect((*it)->inner);
        m_links.erase(it);
        break;
    }
    m_links.erase(std::remove_if(m_links.begin(), m_links.end(),
                                 [](const std::unique_ptr<Link>& l) {
                                     return l->dead && l->role == Role::Subscribe;
                                 }),
                  m_links.end());
}

std::size_t RelayTransport::connectionCount() const {
    return static_cast<std::size_t>(std::count_if(
        m_links.begin(), m_links.end(),
        [](const std::unique_ptr<Link>& l) { return !l->dead && l->stage == Stage::Paired; }));
}

} // namespace HE::Net
