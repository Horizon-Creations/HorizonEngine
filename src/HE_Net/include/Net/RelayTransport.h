#pragma once

// ─── HorizonNet Layer 1 — relay transport ────────────────────────────────────
// The same peer links TcpTransport makes, but every socket is an OUTGOING
// connection to a relay server (repository Horizon-Creations/CollabSession,
// protocol in docs/relay-server-protocol.md). For a host behind a NAT that
// cannot forward a port, this is the difference between "nobody gets in" and a
// working session; for the clients it means one address that is always the same.
//
// It is a separate class rather than a mode of TcpTransport on purpose: without
// a configured relay nothing here is constructed, and the direct peer-to-peer
// path is the same code it was before the relay existed.
//
// How a link comes up (protocol v1):
//
//   host   ── LISTEN(session, "host") ──────────────▶ relay   (one, kept open)
//   client ── CONNECT(session, clientId, "host") ───▶ relay
//   relay  ── INCOMING(clientId, token) ────────────▶ host's listener
//   host   ── ACCEPT(session, "host", clientId, token) ▶ relay (a NEW connection)
//   relay  ── STATUS paired ─────────────────────────▶ both
//
// From "paired" on, the relay forwards bytes 1:1 without reading them, and the
// bytes are TcpTransport's own framing — so SecureTransport and the session
// protocol run over a relay link exactly as they run over a direct one.
//
// What a caller sees is the ITransport contract it already knows:
//   • Connected only once a link is PAIRED. "Waiting for the host to accept"
//     is not connected — reporting it early would make the secure handshake
//     stall in silence when the host never answers.
//   • A refusal or a close before "paired" is a failed connect: Disconnected
//     for a link that never had Connected, as TcpTransport reports a refused
//     connect.
//   • The host's listener connection is never a peer. Each paired ACCEPT
//     connection is what accept() used to return, with its own ConnectionId.
//
// Session and peer ids travel in the HELLO, so the relay knows which session
// and which target a connection belongs to; after "paired" no further
// addressing is needed because the connection itself is the address.

#include "Net/ITransport.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace HE::Net {

class TcpTransport;

// Where the relay is. Parsed from what a person types into a settings field.
struct RelayAddress {
    std::string   host;
    std::uint16_t port = 0;
};

// "host:port", "[ipv6]:port", or a bare "host" (default port). Surrounding
// whitespace is ignored. Returns false for an empty text (meaning: no relay,
// connect peer-to-peer) and for anything malformed; `error`, if given, then
// says which, so a settings page can tell the two apart.
HE_NET_API bool parseRelayAddress(const std::string& text, RelayAddress& out,
                                  std::string* error = nullptr);

// The relay address that is actually in force: the HE_NET_RELAY environment
// variable if it is set and non-empty, otherwise `configured`. HE_NET_RELAY=off
// (or "none") forces peer-to-peer even with a configured relay — the switch a
// test or a support session needs without editing anybody's settings.
HE_NET_API std::string effectiveRelayAddress(const std::string& configured);

class HE_NET_API RelayTransport final : public ITransport {
public:
    // The relay's default port (collab_server's default).
    static constexpr std::uint16_t kDefaultPort = 7777;

    // The host's peer id inside a session. Peer ids only have to be unique per
    // session, and a session has exactly one host, so a fixed name does.
    static constexpr const char* kHostPeerId = "host";

    // Protocol constants, public so tests (and a fake relay) speak the same bytes.
    static constexpr std::uint8_t kProtocolVersion = 1;
    enum class Role : std::uint8_t { Listen = 1, Connect = 2, Accept = 3, Subscribe = 4 };
    enum class FrameType : std::uint8_t {
        Hello = 1, Status = 2, Incoming = 3, Gone = 4, Broadcast = 5
    };
    enum class StatusCode : std::uint8_t {
        Listening = 0, Waiting = 1, Paired = 2, Subscribed = 3,
        BadHello = 16, BadVersion = 17, IdTaken = 18, NoSuchPeer = 19,
        Timeout = 20, Full = 21, NotExpected = 22,
    };

    // 1..64 characters of [A-Za-z0-9_-] — the relay refuses anything else.
    static bool isValidId(const std::string& id);

    // A relay session id derived from a join code, for sessions whose joiners
    // are given only the code (gameplay: the Join dialog has no session id
    // field). HMAC-SHA256 under a fixed label, hex, so the id reveals nothing
    // about the code beyond what the relay already sees — it forwards the
    // handshake that is keyed by the same code. Same code, same id.
    static std::string sessionIdForJoinCode(const std::string& joinCode);

    // Host side: keep a listener connection to the relay for `sessionId` and
    // turn every incoming client into a peer link. Returns nullptr only for
    // an invalid id; a relay that is down or unreachable is retried in the
    // background (1 s doubling to 30 s), listening() says when it has taken
    // us and lastError() why it has not.
    static std::unique_ptr<RelayTransport> host(const RelayAddress& relay,
                                                const std::string& sessionId,
                                                const std::string& selfId = kHostPeerId);

    // Client side: one link to `hostId` in `sessionId` through the relay.
    // Non-null as soon as the attempt starts; the outcome arrives as Connected
    // (paired) or Disconnected (refused, timed out, relay unreachable) from
    // poll(), exactly like TcpTransport::connect — including its nullptr for a
    // connect the OS refuses on the spot. An empty `selfId` picks a
    // random one, which is what callers want: it only has to be unique in the
    // session.
    static std::unique_ptr<RelayTransport> connect(const RelayAddress& relay,
                                                   const std::string& sessionId,
                                                   const std::string& hostId = kHostPeerId,
                                                   const std::string& selfId = {});

    ~RelayTransport() override;

    RelayTransport(const RelayTransport&)            = delete;
    RelayTransport& operator=(const RelayTransport&) = delete;

    using ITransport::send;

    void        update() override;
    void        send(ConnectionId conn, const std::uint8_t* data,
                     std::size_t len, SendMode mode) override;
    bool        poll(NetEvent& out) override;
    void        disconnect(ConnectionId conn) override;
    std::size_t connectionCount() const override;

    bool               isHost() const { return m_isHost; }
    // Host: the relay has confirmed the listener (STATUS listening).
    bool               listening() const { return m_listening; }
    const std::string& sessionId() const { return m_sessionId; }
    const std::string& selfId() const { return m_selfId; }
    const RelayAddress& relay() const { return m_relay; }
    // The relay's reason for the most recent refusal ("id-taken: …"), or why
    // the relay could not be reached. Empty while nothing has gone wrong.
    const std::string& lastError() const { return m_lastError; }

    // ── Wire helpers (also used by tests) ────────────────────────────────────
    // A HELLO payload, without TcpTransport's length prefix.
    static std::vector<std::uint8_t> encodeHello(Role role, const std::string& session,
                                                 const std::string& self,
                                                 const std::string& target,
                                                 const std::string& token);
    // A STATUS / INCOMING payload, as the relay sends them.
    static std::vector<std::uint8_t> encodeStatus(StatusCode code, const std::string& text);
    static std::vector<std::uint8_t> encodeIncoming(const std::string& peer,
                                                    const std::string& token);
    static const char* statusName(std::uint8_t code);

private:
    RelayTransport() = default;

    enum class Stage : std::uint8_t {
        Connecting,   // TCP to the relay not up yet
        Hello,        // HELLO sent, waiting for the relay's answer
        Waiting,      // client: relay accepted the CONNECT, host not yet
        Listening,    // host listener: registered
        Paired,       // raw bytes from here on
    };

    struct Link {
        std::unique_ptr<TcpTransport> tcp;
        ConnectionId inner  = 0;              // the TcpTransport's own id (always its first)
        ConnectionId outer  = 0;              // our id; 0 until it is a peer (listener: never)
        Stage        stage  = Stage::Connecting;
        Role         role   = Role::Connect;
        std::string  peer;                    // accept: the client id; connect: the host id
        std::string  token;                   // accept only; never logged
        std::vector<std::vector<std::uint8_t>> held;   // sends before "paired"
        bool         dead   = false;
    };

    std::unique_ptr<Link> openLink(Role role, const std::string& peer,
                                   const std::string& token);
    void serviceLink(Link& l);
    void onControlFrame(Link& l, const std::vector<std::uint8_t>& frame);
    void onLinkClosed(Link& l, const char* why);
    void scheduleListenerRetry(std::uint64_t nowMs);

    RelayAddress m_relay;
    std::string  m_sessionId;
    std::string  m_selfId;
    std::string  m_lastError;
    bool         m_isHost    = false;
    bool         m_listening = false;

    // Host: the listener link and when to try again if it is gone.
    std::unique_ptr<Link> m_listener;
    std::uint64_t         m_listenerRetryAtMs = 0;
    std::uint32_t         m_listenerBackoffMs = 1000;

    // Links that carry (or will carry) a peer: client CONNECTs and host ACCEPTs.
    std::vector<std::unique_ptr<Link>> m_links;
    ConnectionId                       m_nextId = 1;
    std::deque<NetEvent>               m_events;
};

} // namespace HE::Net
