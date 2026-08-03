#include "Net/TcpTransport.h"

#include "NetLog.h"

#include <algorithm>
#include <utility>

namespace HE::Net {
namespace {

constexpr std::size_t kFrameHeaderSize = 4;   // uint32 big-endian length
constexpr std::size_t kRecvChunk       = 16 * 1024;

void writeLengthPrefix(std::vector<std::uint8_t>& out, std::uint32_t len) {
    out.push_back(static_cast<std::uint8_t>((len >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((len >>  8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>( len        & 0xFF));
}

std::uint32_t readLengthPrefix(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) <<  8) |
            static_cast<std::uint32_t>(p[3]);
}

} // namespace

// ─── Construction ────────────────────────────────────────────────────────────

std::unique_ptr<TcpTransport> TcpTransport::listen(std::uint16_t port) {
    // Dual-stack: a host published in the session directory is reached at
    // whichever address the directory observed, which is frequently IPv6.
    // An IPv4-only listener would be unreachable for those peers.
    SocketHandle s = socketCreateListenerDualStack(port);
    if (s == kInvalidSocket) {
        HE_LOG_ERROR(Net, "TCP listen on port %u failed", static_cast<unsigned>(port));
        return nullptr;
    }

    std::unique_ptr<TcpTransport> t(new TcpTransport());
    t->m_listener  = s;
    t->m_boundPort = socketBoundPort(s);
    // The requested port is logged alongside the bound one because port 0 means
    // "pick one", and knowing which one the OS picked is the whole point.
    HE_LOG_INFO(Net, "TCP listening on port %u (requested %u)",
                static_cast<unsigned>(t->m_boundPort), static_cast<unsigned>(port));
    return t;
}

std::unique_ptr<TcpTransport> TcpTransport::connect(const std::string& host,
                                                    std::uint16_t port) {
    // Resolves the destination first and then creates a socket of the matching
    // family, so IPv6 peers are reachable too.
    SocketHandle s = kInvalidSocket;
    const SocketResult rc = socketCreateTcpConnecting(host, port, s);
    if (rc == SocketResult::Error || s == kInvalidSocket) {
        HE_LOG_ERROR(Net, "TCP connect to %s:%u failed to start (resolve or socket error)",
                     host.c_str(), static_cast<unsigned>(port));
        if (s != kInvalidSocket) socketClose(s);
        return nullptr;
    }

    std::unique_ptr<TcpTransport> t(new TcpTransport());
    const ConnectionId id = t->m_nextId++;

    Conn c;
    c.sock = s;
    HE_LOG_INFO(Net, "TCP connecting to %s:%u (conn %llu, %s)",
                host.c_str(), static_cast<unsigned>(port),
                static_cast<unsigned long long>(id),
                rc == SocketResult::Ok ? "established immediately" : "in progress");
    // Ok means the connect already completed (common on loopback); WouldBlock
    // means it is still in flight and update() will resolve it.
    c.connecting = (rc == SocketResult::WouldBlock);
    t->m_conns.emplace(id, std::move(c));

    if (rc == SocketResult::Ok) {
        t->m_events.push_back(NetEvent{ NetEventType::Connected, id, {} });
    }
    return t;
}

TcpTransport::~TcpTransport() {
    for (auto& [id, c] : m_conns) {
        if (c.sock != kInvalidSocket) socketClose(c.sock);
    }
    m_conns.clear();
    if (m_listener != kInvalidSocket) {
        socketClose(m_listener);
        m_listener = kInvalidSocket;
    }
}

// ─── Pump ────────────────────────────────────────────────────────────────────

void TcpTransport::update() {
    acceptPending();

    // Collect dead connections rather than erasing mid-iteration.
    std::vector<ConnectionId> dead;
    for (auto& [id, c] : m_conns) {
        serviceConnection(id, c);
        if (!c.alive) dead.push_back(id);
    }
    for (const ConnectionId id : dead) {
        auto it = m_conns.find(id);
        if (it != m_conns.end()) {
            if (it->second.sock != kInvalidSocket) socketClose(it->second.sock);
            m_conns.erase(it);
        }
    }
}

void TcpTransport::acceptPending() {
    if (m_listener == kInvalidSocket) return;

    // Drain the backlog: several clients may be queued from one tick.
    for (;;) {
        SocketHandle accepted = kInvalidSocket;
        const SocketResult rc = socketAccept(m_listener, accepted);
        if (rc != SocketResult::Ok) break;   // WouldBlock (empty) or Error

        const ConnectionId id = m_nextId++;
        Conn c;
        c.sock = accepted;
        m_conns.emplace(id, std::move(c));
        m_events.push_back(NetEvent{ NetEventType::Connected, id, {} });
        // Info, not Debug: an inbound connection is the moment that proves port
        // forwarding and reachability actually worked, which is exactly what
        // people are trying to establish when they turn this log on.
        HE_LOG_INFO(Net, "TCP accepted inbound connection (conn %llu, %zu total)",
                    static_cast<unsigned long long>(id), m_conns.size());
    }
}

void TcpTransport::serviceConnection(ConnectionId id, Conn& c) {
    if (!c.alive) return;

    // Resolve a still-pending client connect before doing any I/O on it.
    if (c.connecting) {
        const SocketResult rc = socketConnectPoll(c.sock);
        if (rc == SocketResult::WouldBlock) return;      // still in flight
        if (rc != SocketResult::Ok) {
            // The common real-world failure: nothing is listening, or a
            // firewall dropped it. Distinguishing those needs the OS error,
            // which socketConnectPoll already logs at Layer 0.
            HE_LOG_WARN(Net, "TCP connect failed (conn %llu) — no listener, or blocked en route",
                        static_cast<unsigned long long>(id));
            dropConnection(id, c, /*notify=*/true);      // connect failed
            return;
        }
        c.connecting = false;
        m_events.push_back(NetEvent{ NetEventType::Connected, id, {} });
        HE_LOG_INFO(Net, "TCP connect established (conn %llu)",
                    static_cast<unsigned long long>(id));
    }

    // Read whatever is available.
    std::uint8_t chunk[kRecvChunk];
    for (;;) {
        std::size_t got = 0;
        const SocketResult rc = socketRecv(c.sock, chunk, sizeof(chunk), got);
        if (rc == SocketResult::Ok) {
            c.inBuf.insert(c.inBuf.end(), chunk, chunk + got);
            // A short read means the kernel buffer is drained; avoid spinning.
            if (got < sizeof(chunk)) break;
            continue;
        }
        if (rc == SocketResult::WouldBlock) break;
        // Closed or Error — surface the drop, but emit any complete frames
        // already buffered first so no delivered data is lost.
        if (rc == SocketResult::Closed) {
            HE_LOG_INFO(Net, "TCP peer closed the connection (conn %llu)",
                        static_cast<unsigned long long>(id));
        } else {
            HE_LOG_WARN(Net, "TCP receive error, dropping connection %llu",
                        static_cast<unsigned long long>(id));
        }
        // A non-empty inBuf here means the peer vanished mid-frame; the partial
        // bytes are discarded and worth saying so, because the symptom upstairs
        // is a message that was sent but never arrived.
        if (!c.inBuf.empty()) {
            HE_LOG_DEBUG(Net, "  discarding %s of a partially received frame (conn %llu)",
                         detail::logBytes(c.inBuf.size()).c_str(),
                         static_cast<unsigned long long>(id));
        }
        extractFrames(id, c);
        dropConnection(id, c, /*notify=*/true);
        return;
    }

    extractFrames(id, c);
    if (!c.alive) return;
    flushOutput(c);
}

void TcpTransport::extractFrames(ConnectionId id, Conn& c) {
    std::size_t offset = 0;
    while (c.alive && c.inBuf.size() - offset >= kFrameHeaderSize) {
        const std::uint32_t len = readLengthPrefix(c.inBuf.data() + offset);

        if (len > kMaxFrameSize) {
            // Corrupt or hostile prefix — never allocate what the peer claims.
            // Warning rather than Error: refusing it is the system working as
            // designed, but it is never normal, so it must be visible.
            HE_LOG_WARN(Net,
                        "TCP frame prefix claims %s (limit %s) on conn %llu — corrupt "
                        "stream or hostile peer; dropping connection",
                        detail::logBytes(len).c_str(),
                        detail::logBytes(kMaxFrameSize).c_str(),
                        static_cast<unsigned long long>(id));
            c.inBuf.clear();
            dropConnection(id, c, /*notify=*/true);
            return;
        }
        if (c.inBuf.size() - offset - kFrameHeaderSize < len) break;   // incomplete

        NetEvent ev{ NetEventType::Data, id, {} };
        const std::uint8_t* payload = c.inBuf.data() + offset + kFrameHeaderSize;
        ev.data.assign(payload, payload + len);
        m_events.push_back(std::move(ev));

        // Size only, never contents — frames carry scene and asset data.
        HE_LOG_TRACE(Net, "TCP frame in: %s (conn %llu)",
                     detail::logBytes(len).c_str(), static_cast<unsigned long long>(id));

        offset += kFrameHeaderSize + len;
    }

    // Drop consumed bytes in one move rather than per frame.
    if (offset > 0) {
        c.inBuf.erase(c.inBuf.begin(),
                      c.inBuf.begin() + static_cast<std::ptrdiff_t>(offset));
    }
}

void TcpTransport::flushOutput(Conn& c) {
    std::size_t sentTotal = 0;
    while (sentTotal < c.outBuf.size()) {
        std::size_t sent = 0;
        const SocketResult rc = socketSend(c.sock, c.outBuf.data() + sentTotal,
                                           c.outBuf.size() - sentTotal, sent);
        if (rc == SocketResult::Ok) {
            sentTotal += sent;
            continue;
        }
        if (rc == SocketResult::WouldBlock) {
            // Backpressure. Normal in bursts (a snapshot going out), but a
            // queue that keeps growing across ticks is the signature of a peer
            // that cannot keep up — so the remaining depth is logged, not just
            // the fact that it blocked.
            HE_LOG_TRACE(Net, "TCP send would block, %s still queued",
                         detail::logBytes(c.outBuf.size() - sentTotal).c_str());
            break;
        }
        HE_LOG_WARN(Net, "TCP send failed, marking connection dead (%s unsent)",
                    detail::logBytes(c.outBuf.size() - sentTotal).c_str());
        c.alive = false;                             // dead socket
        break;
    }
    if (sentTotal > 0) {
        c.outBuf.erase(c.outBuf.begin(),
                       c.outBuf.begin() + static_cast<std::ptrdiff_t>(sentTotal));
    }
}

void TcpTransport::dropConnection(ConnectionId id, Conn& c, bool notify) {
    if (!c.alive) return;
    c.alive = false;
    if (notify) {
        m_events.push_back(NetEvent{ NetEventType::Disconnected, id, {} });
    }
}

// ─── ITransport ──────────────────────────────────────────────────────────────

void TcpTransport::send(ConnectionId conn, const std::uint8_t* data,
                        std::size_t len, SendMode /*mode*/) {
    // TCP is always reliable+ordered, so SendMode carries no extra meaning here.
    auto it = m_conns.find(conn);
    if (it == m_conns.end() || !it->second.alive) {
        // Silently dropping a send is the kind of thing that surfaces much
        // later as "the other side never got it", so it is recorded here.
        HE_LOG_DEBUG(Net, "TCP send dropped: connection %llu is %s (%s)",
                     static_cast<unsigned long long>(conn),
                     it == m_conns.end() ? "unknown" : "no longer alive",
                     detail::logBytes(len).c_str());
        return;
    }
    if (len > kMaxFrameSize) {
        HE_LOG_ERROR(Net, "TCP send refused: %s exceeds the %s frame limit (conn %llu)",
                     detail::logBytes(len).c_str(),
                     detail::logBytes(kMaxFrameSize).c_str(),
                     static_cast<unsigned long long>(conn));
        return;   // refuse to emit what a peer must reject
    }

    Conn& c = it->second;
    HE_LOG_TRACE(Net, "TCP frame out: %s (conn %llu)",
                 detail::logBytes(len).c_str(), static_cast<unsigned long long>(conn));
    c.outBuf.reserve(c.outBuf.size() + kFrameHeaderSize + len);
    writeLengthPrefix(c.outBuf, static_cast<std::uint32_t>(len));
    c.outBuf.insert(c.outBuf.end(), data, data + len);

    // Try to push it out immediately; anything left waits for update(). Skip
    // while the connect is still pending — the socket isn't usable yet.
    if (!c.connecting) flushOutput(c);
}

bool TcpTransport::poll(NetEvent& out) {
    if (m_events.empty()) return false;
    out = std::move(m_events.front());
    m_events.pop_front();
    return true;
}

void TcpTransport::disconnect(ConnectionId conn) {
    auto it = m_conns.find(conn);
    if (it == m_conns.end()) return;
    HE_LOG_INFO(Net, "TCP closing connection %llu locally",
                static_cast<unsigned long long>(conn));
    if (!it->second.outBuf.empty()) {
        HE_LOG_WARN(Net, "  %s was still queued and is discarded",
                    detail::logBytes(it->second.outBuf.size()).c_str());
    }
    // Local close: no Disconnected event for the initiator, matching
    // LoopbackTransport's semantics.
    if (it->second.sock != kInvalidSocket) socketClose(it->second.sock);
    m_conns.erase(it);
}

std::size_t TcpTransport::connectionCount() const {
    std::size_t n = 0;
    for (const auto& [id, c] : m_conns) {
        if (c.alive && !c.connecting) ++n;
    }
    return n;
}

} // namespace HE::Net
