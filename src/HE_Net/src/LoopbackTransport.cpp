#include "Net/LoopbackTransport.h"

namespace HE::Net {

std::pair<std::unique_ptr<LoopbackTransport>, std::unique_ptr<LoopbackTransport>>
LoopbackTransport::createPair() {
    // Not std::make_unique — the constructor is private (friendship isn't worth
    // it here); `new` inside the owning member function is fine.
    std::unique_ptr<LoopbackTransport> a(new LoopbackTransport());
    std::unique_ptr<LoopbackTransport> b(new LoopbackTransport());

    a->m_peer = b.get();
    b->m_peer = a.get();
    a->m_connected = true;
    b->m_connected = true;

    // Each side observes the link coming up on its first poll().
    a->m_inbound.push_back(NetEvent{ NetEventType::Connected, kPeer, {} });
    b->m_inbound.push_back(NetEvent{ NetEventType::Connected, kPeer, {} });

    return { std::move(a), std::move(b) };
}

LoopbackTransport::~LoopbackTransport() {
    // Tear the link down on the peer so it stops treating us as reachable and
    // never dereferences a dangling pointer.
    LoopbackTransport* peer = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        peer = m_peer;
        m_peer = nullptr;
        m_connected = false;
    }
    if (peer) peer->onPeerLinkDown();
}

void LoopbackTransport::update() {
    // Loopback delivers eagerly on send(); nothing to service per tick.
}

void LoopbackTransport::send(ConnectionId conn, const std::uint8_t* data,
                             std::size_t len, SendMode /*mode*/) {
    // Read link state under our lock, then release before touching the peer so
    // we never hold both endpoints' mutexes at once (would deadlock on a
    // simultaneous cross-send).
    LoopbackTransport* peer = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (conn != kPeer || !m_connected || !m_peer) return;
        peer = m_peer;
    }
    NetEvent ev{ NetEventType::Data, kPeer, {} };
    ev.data.assign(data, data + len);
    peer->enqueueInbound(std::move(ev));
}

bool LoopbackTransport::poll(NetEvent& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_inbound.empty()) return false;
    out = std::move(m_inbound.front());
    m_inbound.pop_front();
    return true;
}

void LoopbackTransport::disconnect(ConnectionId conn) {
    LoopbackTransport* peer = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (conn != kPeer || !m_connected) return;
        m_connected = false;
        peer = m_peer;
        // Drop our side of the link too, so a later destructor never dereferences
        // a peer that has already been freed (the link must sever symmetrically).
        m_peer = nullptr;
    }
    // Peer learns the link dropped; the initiator gets no local event.
    if (peer) peer->onPeerLinkDown();
}

std::size_t LoopbackTransport::connectionCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_connected ? 1u : 0u;
}

void LoopbackTransport::enqueueInbound(NetEvent ev) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_inbound.push_back(std::move(ev));
}

void LoopbackTransport::onPeerLinkDown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_connected && m_peer == nullptr) return;
    m_connected = false;
    m_peer = nullptr;
    m_inbound.push_back(NetEvent{ NetEventType::Disconnected, kPeer, {} });
}

} // namespace HE::Net
