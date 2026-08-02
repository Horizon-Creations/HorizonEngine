#include "Net/TcpTransport.h"

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
    if (s == kInvalidSocket) return nullptr;

    std::unique_ptr<TcpTransport> t(new TcpTransport());
    t->m_listener  = s;
    t->m_boundPort = socketBoundPort(s);
    return t;
}

std::unique_ptr<TcpTransport> TcpTransport::connect(const std::string& host,
                                                    std::uint16_t port) {
    // Resolves the destination first and then creates a socket of the matching
    // family, so IPv6 peers are reachable too.
    SocketHandle s = kInvalidSocket;
    const SocketResult rc = socketCreateTcpConnecting(host, port, s);
    if (rc == SocketResult::Error || s == kInvalidSocket) {
        if (s != kInvalidSocket) socketClose(s);
        return nullptr;
    }

    std::unique_ptr<TcpTransport> t(new TcpTransport());
    const ConnectionId id = t->m_nextId++;

    Conn c;
    c.sock = s;
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
    }
}

void TcpTransport::serviceConnection(ConnectionId id, Conn& c) {
    if (!c.alive) return;

    // Resolve a still-pending client connect before doing any I/O on it.
    if (c.connecting) {
        const SocketResult rc = socketConnectPoll(c.sock);
        if (rc == SocketResult::WouldBlock) return;      // still in flight
        if (rc != SocketResult::Ok) {
            dropConnection(id, c, /*notify=*/true);      // connect failed
            return;
        }
        c.connecting = false;
        m_events.push_back(NetEvent{ NetEventType::Connected, id, {} });
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
            c.inBuf.clear();
            dropConnection(id, c, /*notify=*/true);
            return;
        }
        if (c.inBuf.size() - offset - kFrameHeaderSize < len) break;   // incomplete

        NetEvent ev{ NetEventType::Data, id, {} };
        const std::uint8_t* payload = c.inBuf.data() + offset + kFrameHeaderSize;
        ev.data.assign(payload, payload + len);
        m_events.push_back(std::move(ev));

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
        if (rc == SocketResult::WouldBlock) break;   // kernel buffer full; retry next tick
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
    if (it == m_conns.end() || !it->second.alive) return;
    if (len > kMaxFrameSize) return;   // refuse to emit what a peer must reject

    Conn& c = it->second;
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
