#pragma once

// ─── HorizonNet Layer 1½ — session glue ──────────────────────────────────────
// NetSession sits between the raw transport and the application. It:
//   • frames application messages as [MessageId:16][payload bytes] datagrams,
//   • drains the transport once per pump() and dispatches Data to typed handlers,
//   • surfaces connect/disconnect lifecycle as callbacks and a live peer list.
//
// It is transport-agnostic (holds a non-owning ITransport*), so the very same
// session drives loopback in tests, GameNetworkingSockets for gameplay, or a
// WebSocket link for editor collaboration. This is the seam both Layer 3a
// (replication) and Layer 3b (presence/locks) build on.

#include "Net/BitStream.h"
#include "Net/ITransport.h"
#include "Net/NetCommon.h"

#include <functional>
#include <unordered_map>
#include <vector>

namespace HE::Net {

class HE_NET_API NetSession {
public:
    using MessageHandler    = std::function<void(ConnectionId, BitReader&)>;
    using ConnectionHandler = std::function<void(ConnectionId)>;

    explicit NetSession(ITransport* transport, NetRole role = NetRole::None);

    NetRole role() const { return m_role; }
    void    setRole(NetRole role) { m_role = role; }

    // Register a handler for a message id. Re-registering replaces the previous
    // handler. The BitReader is positioned just past the message id.
    void on(MessageId id, MessageHandler handler);

    void onConnect(ConnectionHandler handler)    { m_onConnect = std::move(handler); }
    void onDisconnect(ConnectionHandler handler) { m_onDisconnect = std::move(handler); }

    // Send a framed message to one peer.
    void send(ConnectionId conn, MessageId id, const BitWriter& payload,
              SendMode mode = SendMode::ReliableOrdered);

    // Send to every live peer.
    void broadcast(MessageId id, const BitWriter& payload,
                   SendMode mode = SendMode::ReliableOrdered);

    // Overloads for id-only messages (no payload).
    void send(ConnectionId conn, MessageId id, SendMode mode = SendMode::ReliableOrdered) {
        send(conn, id, BitWriter{}, mode);
    }
    void broadcast(MessageId id, SendMode mode = SendMode::ReliableOrdered) {
        broadcast(id, BitWriter{}, mode);
    }

    // Drain the transport: fire lifecycle callbacks and dispatch data messages.
    // Call once per tick after transport->update().
    void pump();

    const std::vector<ConnectionId>& connections() const { return m_connections; }

private:
    ITransport*                                    m_transport = nullptr;
    NetRole                                        m_role      = NetRole::None;
    std::unordered_map<MessageId, MessageHandler>  m_handlers;
    ConnectionHandler                              m_onConnect;
    ConnectionHandler                              m_onDisconnect;
    std::vector<ConnectionId>                      m_connections;
};

} // namespace HE::Net
