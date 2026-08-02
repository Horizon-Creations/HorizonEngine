#pragma once

// ─── HorizonNet Layer 1 — TCP transport ──────────────────────────────────────
// The real-network ITransport: a host listens, clients connect, and both sides
// exchange datagrams over TCP. This is the transport editor collaboration runs
// on — collab needs reliable, ordered, lossless delivery, which is exactly what
// TCP provides, without the weight of a UDP reliability layer.
//
// (GameNetworkingSockets stays reserved for gameplay replication, where
// unreliable low-latency channels actually matter.)
//
// TCP is a byte stream, but ITransport is datagram-oriented, so every message is
// length-prefixed on the wire:
//
//     [uint32 big-endian payload length][payload bytes]
//
// Partial reads and partial writes are buffered per connection: update() drains
// whatever the kernel has, emits every *complete* frame, and flushes as much
// queued output as the kernel accepts. Nothing blocks — the editor pumps this
// from its frame loop.

#include "Net/ITransport.h"
#include "Net/Socket.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace HE::Net {

class HE_NET_API TcpTransport final : public ITransport {
public:
    // Upper bound on a single frame. Late-join scene snapshots are the largest
    // legitimate payload; anything beyond this is treated as a corrupt or
    // hostile length prefix and the connection is dropped rather than allocating
    // whatever the peer claims.
    static constexpr std::uint32_t kMaxFrameSize = 64u * 1024u * 1024u;

    // Host side: bind + listen. Pass port 0 to let the OS choose, then read the
    // actual port back with boundPort(). Returns nullptr on failure.
    static std::unique_ptr<TcpTransport> listen(std::uint16_t port);

    // Client side: begin connecting. Returns non-null as soon as the attempt
    // starts — completion is asynchronous, surfacing as a Connected event from
    // poll() (or a Disconnected event if it fails).
    static std::unique_ptr<TcpTransport> connect(const std::string& host,
                                                 std::uint16_t port);

    ~TcpTransport() override;

    TcpTransport(const TcpTransport&)            = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    // Bring the base-class vector overload into scope (overriding the pointer
    // form would otherwise hide it).
    using ITransport::send;

    void        update() override;
    void        send(ConnectionId conn, const std::uint8_t* data,
                     std::size_t len, SendMode mode) override;
    bool        poll(NetEvent& out) override;
    void        disconnect(ConnectionId conn) override;
    std::size_t connectionCount() const override;

    // Port actually bound in listen mode (0 otherwise).
    std::uint16_t boundPort() const { return m_boundPort; }
    bool          isListening() const { return m_listener != kInvalidSocket; }

private:
    TcpTransport() = default;

    struct Conn {
        SocketHandle              sock = kInvalidSocket;
        std::vector<std::uint8_t> inBuf;    // bytes received, not yet framed
        std::vector<std::uint8_t> outBuf;   // bytes queued, not yet accepted
        bool connecting = false;            // client-side connect still pending
        bool alive      = true;
    };

    void acceptPending();
    void serviceConnection(ConnectionId id, Conn& c);
    void extractFrames(ConnectionId id, Conn& c);
    void flushOutput(Conn& c);
    void dropConnection(ConnectionId id, Conn& c, bool notify);

    SocketHandle                             m_listener  = kInvalidSocket;
    std::uint16_t                            m_boundPort = 0;
    ConnectionId                             m_nextId    = 1;
    std::unordered_map<ConnectionId, Conn>   m_conns;
    std::deque<NetEvent>                     m_events;
};

} // namespace HE::Net
