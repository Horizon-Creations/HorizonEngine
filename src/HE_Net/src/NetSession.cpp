#include "Net/NetSession.h"

#include "NetLog.h"

#include <algorithm>

namespace HE::Net {

NetSession::NetSession(ITransport* transport, NetRole role)
    : m_transport(transport), m_role(role) {}

void NetSession::on(MessageId id, MessageHandler handler) {
    m_handlers[id] = std::move(handler);
}

void NetSession::send(ConnectionId conn, MessageId id, const BitWriter& payload,
                      SendMode mode) {
    if (!m_transport) {
        HE_LOG_WARN(Net, "Send of message %u dropped: session has no transport",
                    static_cast<unsigned>(id));
        return;
    }
    // Frame: 16-bit id (leaves the stream byte-aligned) followed by the payload
    // bytes verbatim. Any zero-pad in the payload's final byte is harmless — the
    // receiving handler reads exactly the typed fields it expects and stops.
    BitWriter framed;
    framed.writeUInt16(id);
    const std::vector<std::uint8_t> pd = payload.data();
    if (!pd.empty()) framed.writeBytes(pd.data(), pd.size());

    const std::vector<std::uint8_t> bytes = framed.data();
    HE_LOG_TRACE(Net, "msg %u out → conn %llu (%s)", static_cast<unsigned>(id),
                 static_cast<unsigned long long>(conn),
                 detail::logBytes(bytes.size()).c_str());
    m_transport->send(conn, bytes.data(), bytes.size(), mode);
}

void NetSession::broadcast(MessageId id, const BitWriter& payload, SendMode mode) {
    // Snapshot the peer list: a handler could mutate it, and send() must not be
    // iterating a container that shifts underneath it.
    const std::vector<ConnectionId> peers = m_connections;
    HE_LOG_TRACE(Net, "msg %u broadcast to %zu peer(s)", static_cast<unsigned>(id),
                 peers.size());
    for (const ConnectionId conn : peers) {
        send(conn, id, payload, mode);
    }
}

void NetSession::disconnect(ConnectionId conn) {
    if (!m_transport) return;
    HE_LOG_INFO(Net, "Session: dropping conn %llu from this side",
                static_cast<unsigned long long>(conn));
    m_transport->disconnect(conn);
    // The transport gives the initiator no Disconnected event, so the peer list
    // has to be pruned here or broadcast() would keep sending into the void.
    m_connections.erase(
        std::remove(m_connections.begin(), m_connections.end(), conn),
        m_connections.end());
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
            HE_LOG_DEBUG(Net, "Session: conn %llu joined the peer list (%zu total)",
                         static_cast<unsigned long long>(ev.conn), m_connections.size());
            if (m_onConnect) m_onConnect(ev.conn);
            break;
        }
        case NetEventType::Disconnected: {
            m_connections.erase(
                std::remove(m_connections.begin(), m_connections.end(), ev.conn),
                m_connections.end());
            HE_LOG_DEBUG(Net, "Session: conn %llu left the peer list (%zu remain)",
                         static_cast<unsigned long long>(ev.conn), m_connections.size());
            if (m_onDisconnect) m_onDisconnect(ev.conn);
            break;
        }
        case NetEventType::Data: {
            BitReader reader(ev.data);
            std::uint16_t id = 0;
            if (!reader.readUInt16(id)) {
                HE_LOG_WARN(Net, "Dropping a %s datagram from conn %llu: too short to "
                                 "even hold a message id",
                            detail::logBytes(ev.data.size()).c_str(),
                            static_cast<unsigned long long>(ev.conn));
                break;   // truncated datagram → drop
            }
            const auto it = m_handlers.find(static_cast<MessageId>(id));
            if (it != m_handlers.end() && it->second) {
                HE_LOG_TRACE(Net, "msg %u in ← conn %llu (%s)", static_cast<unsigned>(id),
                             static_cast<unsigned long long>(ev.conn),
                             detail::logBytes(ev.data.size()).c_str());
                it->second(ev.conn, reader);
            } else {
                // Almost always version skew: the peer speaks a message this
                // build has no handler for. Silently ignoring it is correct
                // behaviour but a terrible debugging experience.
                //
                // Throttled rather than HE_LOG_ONCE: a stream of unknown ids
                // arrives at frame rate, but "once per call site, ever" would
                // report the first id and then hide every *different* one
                // behind it — which is precisely the case worth seeing.
                HE_LOG_THROTTLE(Net, Warning, 5.0,
                                "No handler for message id %u (from conn %llu) — the peer is "
                                "probably running a different engine build",
                                static_cast<unsigned>(id),
                                static_cast<unsigned long long>(ev.conn));
            }
            break;
        }
        }
    }
}

} // namespace HE::Net
