#include "Net/NetSession.h"

#include <algorithm>

namespace HE::Net {

NetSession::NetSession(ITransport* transport, NetRole role)
    : m_transport(transport), m_role(role) {}

void NetSession::on(MessageId id, MessageHandler handler) {
    m_handlers[id] = std::move(handler);
}

void NetSession::send(ConnectionId conn, MessageId id, const BitWriter& payload,
                      SendMode mode) {
    if (!m_transport) return;
    // Frame: 16-bit id (leaves the stream byte-aligned) followed by the payload
    // bytes verbatim. Any zero-pad in the payload's final byte is harmless — the
    // receiving handler reads exactly the typed fields it expects and stops.
    BitWriter framed;
    framed.writeUInt16(id);
    const std::vector<std::uint8_t> pd = payload.data();
    if (!pd.empty()) framed.writeBytes(pd.data(), pd.size());

    const std::vector<std::uint8_t> bytes = framed.data();
    m_transport->send(conn, bytes.data(), bytes.size(), mode);
}

void NetSession::broadcast(MessageId id, const BitWriter& payload, SendMode mode) {
    // Snapshot the peer list: a handler could mutate it, and send() must not be
    // iterating a container that shifts underneath it.
    const std::vector<ConnectionId> peers = m_connections;
    for (const ConnectionId conn : peers) {
        send(conn, id, payload, mode);
    }
}

void NetSession::pump() {
    if (!m_transport) return;

    NetEvent ev;
    while (m_transport->poll(ev)) {
        switch (ev.type) {
        case NetEventType::Connected: {
            if (std::find(m_connections.begin(), m_connections.end(), ev.conn)
                == m_connections.end()) {
                m_connections.push_back(ev.conn);
            }
            if (m_onConnect) m_onConnect(ev.conn);
            break;
        }
        case NetEventType::Disconnected: {
            m_connections.erase(
                std::remove(m_connections.begin(), m_connections.end(), ev.conn),
                m_connections.end());
            if (m_onDisconnect) m_onDisconnect(ev.conn);
            break;
        }
        case NetEventType::Data: {
            BitReader reader(ev.data);
            std::uint16_t id = 0;
            if (!reader.readUInt16(id)) break;   // truncated datagram → drop
            const auto it = m_handlers.find(static_cast<MessageId>(id));
            if (it != m_handlers.end() && it->second) {
                it->second(ev.conn, reader);
            }
            break;
        }
        }
    }
}

} // namespace HE::Net
