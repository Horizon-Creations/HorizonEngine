#pragma once

// ─── HorizonNet Layer 1 — transport abstraction ──────────────────────────────
// The transport is the *only* thing that touches the wire. Everything above it
// (message dispatch, session, presence/lock collaboration, gameplay replication)
// is transport-agnostic. Concrete backings:
//   • LoopbackTransport      — in-process, no sockets (tests + local editor)
//   • TcpTransport           — editor collaboration and the MCP bridge (reliable stream)
//   • UdpTransport           — gameplay replication: own reliability layer, all three SendModes
// Decorators over any of them:
//   • SecureTransport        — authenticated, encrypted channel
//   • LossyTransport         — deterministic loss / reorder / duplication for tests
//
// Event model is poll-based: the owner pumps update() once per tick, then drains
// poll() until it returns false. This keeps threading policy in the transport and
// gives consumers a single-threaded, deterministic drain — which is exactly what
// the ECS tick and the editor frame want.

#include "Net/NetCommon.h"

#include <cstdint>
#include <vector>

namespace HE::Net {

struct NetEvent {
    NetEventType              type = NetEventType::Data;
    ConnectionId              conn = kInvalidConnection;
    std::vector<std::uint8_t> data;   // populated for NetEventType::Data
};

class HE_NET_API ITransport {
public:
    virtual ~ITransport() = default;

    // Advance internal state (drive reliability, service sockets). Call once/tick
    // before draining poll().
    virtual void update() = 0;

    // Send a datagram to a peer. A no-op (not a crash) for unknown/dead conns.
    virtual void send(ConnectionId conn,
                      const std::uint8_t* data,
                      std::size_t len,
                      SendMode mode) = 0;

    // Pop the next pending event. Returns false when the queue is empty.
    virtual bool poll(NetEvent& out) = 0;

    // Gracefully drop a peer link (emits no local Disconnected for the initiator).
    virtual void disconnect(ConnectionId conn) = 0;

    // Number of currently live peer links.
    virtual std::size_t connectionCount() const = 0;

    // ── One message to several peers ─────────────────────────────────────────
    // A transport that can deliver one message to many peers cheaper than one
    // send() each (RelayFanout: one frame to the relay, which makes the copies)
    // says so here. NetSession asks once, at construction; for every other
    // transport it keeps its own send() loop, so their path does not change.
    virtual bool fansOut() const { return false; }

    // `to` lists the recipients; every live peer not in it must not get the
    // message. Only called when fansOut() is true. The default is the loop.
    virtual void sendToMany(const ConnectionId* to, std::size_t count,
                            const std::uint8_t* data, std::size_t len, SendMode mode) {
        for (std::size_t i = 0; i < count; ++i) send(to[i], data, len, mode);
    }

    // Convenience overload for the common vector payload.
    void send(ConnectionId conn, const std::vector<std::uint8_t>& bytes, SendMode mode) {
        send(conn, bytes.data(), bytes.size(), mode);
    }
};

} // namespace HE::Net
