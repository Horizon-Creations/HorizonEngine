#pragma once

// ─── HorizonNet Layer 3b — collaboration session ─────────────────────────────
// The first real consumer of the transport stack: a host opens a session, peers
// join, everyone sees who is present, and a joiner receives the current scene so
// it starts from the same state as everyone else.
//
// Host-authoritative by design. The host owns the participant registry, assigns
// ids, and is the single source of truth for state — which is also what later
// makes the lock table (N5) instantly authoritative instead of poll-based.
//
// Layering: this deliberately does NOT depend on HorizonScene. HorizonNet sits
// *below* the scene layer, so reaching up into it would invert the dependency
// and drag the ECS into every networking test. Snapshot capture/apply is
// therefore abstracted behind ISessionStateProvider, which the editor wires to
// SceneSerializer::saveToMemory / loadFromMemory. Tests supply a trivial
// in-memory provider instead.
//
// Join flow:
//   client → host   JoinRequest      protocol version, display name
//   host  → client  JoinAccepted     assigned id + current participant list
//                   (or JoinRejected with a reason)
//   host  → client  SnapshotBegin / SnapshotChunk… / SnapshotEnd
//   host  → others  ParticipantJoined
//   …on disconnect  ParticipantLeft
//
// The snapshot is chunked rather than sent as one frame: a scene can be many
// megabytes, and a single blob would give no progress feedback and force the
// receiver to accept an arbitrary allocation up front.

#include "Net/NetCommon.h"
#include "Net/NetSession.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace HE::Net {

using ParticipantId = std::uint32_t;
inline constexpr ParticipantId kInvalidParticipant = 0;

// Bumped whenever the collaboration protocol changes shape. Peers that disagree
// are rejected rather than allowed to misinterpret each other's messages.
inline constexpr std::uint16_t kCollabProtocolVersion = 1;

enum class JoinRejectReason : std::uint8_t {
    None            = 0,
    VersionMismatch = 1,   // peer speaks a different collaboration protocol
    SessionFull     = 2,
    SnapshotFailed  = 3,   // host could not capture its own state
};

struct Participant {
    ParticipantId id = kInvalidParticipant;
    std::string   name;
    bool          isHost = false;
};

// Supplies and applies the session's shared state. Implemented by the editor on
// top of SceneSerializer; implemented trivially by tests.
class HE_NET_API ISessionStateProvider {
public:
    virtual ~ISessionStateProvider() = default;

    // Serialize the current state for a joining peer. Return false if the state
    // cannot be captured — the join is then rejected rather than completed with
    // a peer that silently holds a different scene.
    virtual bool captureSnapshot(std::vector<std::uint8_t>& out) = 0;

    // Replace local state with a received snapshot.
    virtual bool applySnapshot(const std::vector<std::uint8_t>& data) = 0;
};

class HE_NET_API CollabSession {
public:
    struct Config {
        std::string   displayName   = "Horizon User";
        std::size_t   maxParticipants = 8;
        // Upper bound on an accepted snapshot. Guards against a hostile or buggy
        // host announcing a size that would exhaust memory.
        std::uint32_t maxSnapshotBytes = 64u * 1024u * 1024u;
        std::uint32_t chunkBytes       = 256u * 1024u;
    };

    CollabSession(NetSession* net, NetRole role, Config cfg);

    // Delegating overload rather than a `Config cfg = {}` default argument: the
    // default would be parsed while Config is still incomplete, since its own
    // members carry initializers. Inline bodies are compiled once the class is
    // complete, so this form is well-formed.
    CollabSession(NetSession* net, NetRole role)
        : CollabSession(net, role, Config{}) {}

    void setStateProvider(ISessionStateProvider* provider) { m_state = provider; }

    // ── Callbacks ──
    // Client: the join completed and the snapshot has been applied.
    void onJoined(std::function<void(ParticipantId)> fn)          { m_onJoined = std::move(fn); }
    void onJoinRejected(std::function<void(JoinRejectReason)> fn) { m_onRejected = std::move(fn); }
    void onParticipantJoined(std::function<void(const Participant&)> fn) { m_onJoin = std::move(fn); }
    void onParticipantLeft(std::function<void(ParticipantId)> fn)  { m_onLeft = std::move(fn); }
    // Client: snapshot transfer progress, for a progress bar (received, total).
    void onSnapshotProgress(std::function<void(std::uint32_t, std::uint32_t)> fn) {
        m_onProgress = std::move(fn);
    }

    // Drive the session. Pump the transport first, then this.
    void update();

    const std::vector<Participant>& participants() const { return m_participants; }
    ParticipantId localId() const { return m_localId; }
    bool          isJoined() const { return m_joined; }
    NetRole       role() const { return m_role; }

    // Sequence number of the state every participant currently shares. The
    // snapshot carries it, and live deltas (N6) will continue from it — defined
    // now so adding them later is not a protocol break.
    std::uint64_t stateSequence() const { return m_sequence; }

private:
    void installHandlers();

    // Host side
    void handleJoinRequest(ConnectionId conn, BitReader& r);
    void sendSnapshotTo(ConnectionId conn);
    void onPeerConnected(ConnectionId conn);
    void onPeerDisconnected(ConnectionId conn);

    // Client side
    void handleJoinAccepted(BitReader& r);
    void handleJoinRejected(BitReader& r);
    void handleSnapshotBegin(BitReader& r);
    void handleSnapshotChunk(BitReader& r);
    void handleSnapshotEnd();

    Participant* findParticipant(ParticipantId id);

    NetSession*           m_net   = nullptr;
    NetRole               m_role  = NetRole::None;
    Config                m_cfg;
    ISessionStateProvider* m_state = nullptr;

    std::vector<Participant> m_participants;
    ParticipantId            m_localId   = kInvalidParticipant;
    ParticipantId            m_nextId    = 1;      // host-side allocator
    bool                     m_joined    = false;
    bool                     m_joinSent  = false;  // client: request already sent
    std::uint64_t            m_sequence  = 0;

    // Host: which connection belongs to which participant.
    std::vector<std::pair<ConnectionId, ParticipantId>> m_connToParticipant;

    // Client: snapshot assembly in progress.
    std::vector<std::uint8_t> m_snapshotBuf;
    std::uint32_t             m_snapshotExpected = 0;
    std::uint32_t             m_snapshotReceived = 0;
    bool                      m_snapshotActive   = false;

    std::function<void(ParticipantId)>                m_onJoined;
    std::function<void(JoinRejectReason)>             m_onRejected;
    std::function<void(const Participant&)>           m_onJoin;
    std::function<void(ParticipantId)>                m_onLeft;
    std::function<void(std::uint32_t, std::uint32_t)> m_onProgress;
};

} // namespace HE::Net
