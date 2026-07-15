#pragma once

// ─── HorizonNet Layer 1 — in-process loopback transport ──────────────────────
// A fully in-memory ITransport: two cross-wired endpoints exchange datagrams
// with no sockets involved. Two jobs:
//   1. Unit-testing the entire stack (message dispatch, session, collaboration)
//      deterministically, with zero network flakiness.
//   2. Single-process editor collaboration / local play-in-editor, where "host"
//      and "client" live in the same process.
//
// SendMode is ignored — loopback is always lossless and ordered. Delivery is
// queued (not synchronous): a datagram sent this tick is observed by the peer's
// poll() after it has drained its queue, mirroring real transports where data
// crosses a tick boundary rather than arriving mid-call.

#include "Net/ITransport.h"

#include <deque>
#include <memory>
#include <mutex>
#include <utility>

namespace HE::Net {

class HE_NET_API LoopbackTransport final : public ITransport {
public:
    // Build two cross-wired endpoints. Each observes the other as connection id
    // 1 and receives a Connected event on its first poll().
    static std::pair<std::unique_ptr<LoopbackTransport>,
                     std::unique_ptr<LoopbackTransport>>
    createPair();

    ~LoopbackTransport() override;

    LoopbackTransport(const LoopbackTransport&)            = delete;
    LoopbackTransport& operator=(const LoopbackTransport&) = delete;

    // Bring the base-class vector convenience overload into scope — overriding
    // the pointer form otherwise hides it (name hiding).
    using ITransport::send;

    void        update() override;
    void        send(ConnectionId conn, const std::uint8_t* data,
                     std::size_t len, SendMode mode) override;
    bool        poll(NetEvent& out) override;
    void        disconnect(ConnectionId conn) override;
    std::size_t connectionCount() const override;

    // The single peer connection id, exposed so tests / session code can address
    // "the other end" without magic numbers.
    static constexpr ConnectionId kPeer = 1;

private:
    LoopbackTransport() = default;

    // Called by the peer endpoint to hand us an inbound event.
    void enqueueInbound(NetEvent ev);
    // Called by the peer's destructor / disconnect to tear the link down locally.
    void onPeerLinkDown();

    LoopbackTransport*     m_peer      = nullptr;
    bool                   m_connected = false;
    std::deque<NetEvent>   m_inbound;
    mutable std::mutex     m_mutex;
};

} // namespace HE::Net
