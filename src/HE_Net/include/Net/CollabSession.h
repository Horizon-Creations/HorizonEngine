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
// v2: every subject (locks, transforms, components, structure, selection) is
// derived from the entity's stable uuid instead of its entt handle — the two
// schemes resolve to different entities, so mixing them must be refused.
// v3: document deltas (per node / per UI element) and the authoritative lock
// query. A v2 peer would drop both silently and believe it was in sync while
// the other side edited a graph it never saw — worse than refusing to connect.
// v4: the join handshake carries a project key, and a rejection carries a
// detail string. A v3 peer omits both, so its join would read as a project
// mismatch — the version check runs first and says something truthful instead.
inline constexpr std::uint16_t kCollabProtocolVersion = 4;

enum class JoinRejectReason : std::uint8_t {
    None            = 0,
    VersionMismatch = 1,   // peer speaks a different collaboration protocol
    SessionFull     = 2,
    SnapshotFailed  = 3,   // host could not capture its own state
    // The two sides have DIFFERENT projects open. Everything a session sends —
    // scene entities, asset references, lock subjects — is addressed by uuids
    // that only mean anything inside one project, so a joiner would receive a
    // scene whose every asset reference dangles. It used to be admitted anyway.
    ProjectMismatch = 4,
};

struct Participant {
    ParticipantId id = kInvalidParticipant;
    std::string   name;
    bool          isHost = false;
};

// ─── Presence ────────────────────────────────────────────────────────────────
// Volatile per-participant state: where someone is looking and what they have
// selected. Unlike the scene itself this is disposable — a lost update is
// corrected by the next one, so it is rate-limited rather than reliable-ordered
// in spirit.
//
// Entity ids are carried as opaque 64-bit values. HorizonNet has no idea what
// they refer to, which is what keeps this layer independent of the scene.
struct PresenceState {
    float cameraPos[3] { 0.0f, 0.0f, 0.0f };
    float cameraRot[4] { 0.0f, 0.0f, 0.0f, 1.0f };   // quaternion (x, y, z, w)
    std::vector<std::uint64_t> selection;
    bool  valid = false;   // false until the participant has sent anything
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

// ─── Locks ───────────────────────────────────────────────────────────────────
// Who currently owns the right to edit a given thing. The host holds the only
// copy of this table and arbitrates every request, which is the whole point:
// unlike Git-LFS locks — which each client discovers by polling a server, so two
// people can believe they hold the same lock for a few seconds — a request here
// is answered by the one authority, in order, with no window at all.
//
// The subject is an opaque 64-bit id, exactly like presence selection: it may be
// an entity, an asset UUID hash, or anything else the editor decides. HorizonNet
// never interprets it.
struct LockInfo {
    std::uint64_t subject = 0;
    ParticipantId owner   = kInvalidParticipant;
    std::string   ownerName;   // carried so the UI can name a holder that is not
                               // in the roster yet (or already left)
};

enum class LockDenyReason : std::uint8_t {
    None       = 0,
    HeldByOther = 1,
    NotOwner    = 2,   // tried to release something someone else holds
    NotInSession = 3,
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

        // Presence is sent at most this often. A gizmo drag produces one change
        // per frame; forwarding all of them would swamp the link for state that
        // is obsolete a frame later.
        std::uint64_t presenceIntervalMs = 100;   // 10 Hz
        // Unchanged presence is still resent at this cadence, so late joiners
        // and lost relays converge without anyone having to move.
        std::uint64_t presenceKeepAliveMs = 2000;
        // Ignore sub-threshold camera jitter so a stationary editor emits nothing.
        float         presencePositionEpsilon = 0.001f;
        float         presenceRotationEpsilon = 0.001f;
        // Bounds the frame a peer can cause us to build.
        std::uint16_t maxSelectionIds = 1024;

        // Opaque identity of the thing being edited together, compared on join;
        // a mismatch is refused with ProjectMismatch. HorizonNet does not
        // interpret it — the editor puts the project's uuid here — and an empty
        // key on BOTH sides matches, so a projectless session still works.
        std::string projectKey;
        // Human-readable name for the same thing, sent with a rejection so the
        // joiner can be told WHICH project to open rather than just "no".
        std::string projectLabel;

        // ── Document-delta bounds ──
        // One item's JSON has to fit a length-prefixed string, which BitStream
        // caps at 65535 and TRUNCATES silently — a truncated payload would land
        // as unparseable JSON and quietly drop the item on the peer. Refusing
        // well below that, and refusing an oversized batch, sends the caller to
        // the whole-file fallback instead. A node or UI element is a few hundred
        // bytes; anything near this cap is pathological.
        std::uint32_t maxDocItemBytes  = 48u * 1024u;
        std::uint32_t maxDocBatchBytes = 1024u * 1024u;
        std::uint16_t maxDocDeltas     = 4096;
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
    // `detail` carries whatever the host could say about the refusal — for a
    // project mismatch, the name of the project the session is editing. Empty
    // when the host had nothing to add.
    void onJoinRejected(std::function<void(JoinRejectReason, const std::string& detail)> fn) {
        m_onRejected = std::move(fn);
    }
    void onParticipantJoined(std::function<void(const Participant&)> fn) { m_onJoin = std::move(fn); }
    void onParticipantLeft(std::function<void(ParticipantId)> fn)  { m_onLeft = std::move(fn); }
    // Client: snapshot transfer progress, for a progress bar (received, total).
    void onSnapshotProgress(std::function<void(std::uint32_t, std::uint32_t)> fn) {
        m_onProgress = std::move(fn);
    }

    // Report where this user is looking and what they have selected. Cheap to
    // call every frame: the value is stored, and update() decides whether it is
    // worth sending.
    void setLocalPresence(const float cameraPos[3], const float cameraRot[4],
                          const std::vector<std::uint64_t>& selection);

    // Last known presence of a participant (including ourselves). `valid` is
    // false until they have reported any.
    const PresenceState* presenceOf(ParticipantId id) const;

    // Fires whenever a remote participant's presence changes — the editor
    // redraws their camera gizmo and selection highlight from this.
    void onPresenceChanged(std::function<void(ParticipantId, const PresenceState&)> fn) {
        m_onPresence = std::move(fn);
    }

    // ── Live scene deltas ──
    // A transform change on one entity, keyed by its handle. Handles match
    // across peers because everyone started from the same snapshot, so no id
    // remapping is needed — and a transform is by far the most common edit
    // during collaboration, which is why it gets its own compact message rather
    // than riding on a general component blob.
    //
    // The host only relays a change when the sender holds the lock on that
    // subject, so two people cannot move the same object at once.
    // Rotation is Euler degrees, matching TransformComponent exactly. A
    // quaternion would have to be converted on both ends, and quantizing it to
    // [-1,1] — as presence does — is only valid for a *unit* quaternion, not for
    // angles that legitimately run past 180°.
    struct TransformDelta {
        std::uint64_t subject = 0;
        float position[3] { 0.0f, 0.0f, 0.0f };
        float rotation[3] { 0.0f, 0.0f, 0.0f };   // Euler degrees
        float scale[3]    { 1.0f, 1.0f, 1.0f };
    };

    // Publish a local transform change. Ignored when we do not hold the lock.
    bool sendTransform(const TransformDelta& delta);

    // A remote participant moved something. The editor applies it to its world.
    void onTransform(std::function<void(ParticipantId, const TransformDelta&)> fn) {
        m_onTransform = std::move(fn);
    }

    // ── Component edits ──
    // Everything a transform delta does not cover: mesh and material
    // assignments, lights, cameras, colliders, scripts, names — any component
    // the scene format knows how to store. Carried as the entity's serialized
    // component state, so a component added to the scene format replicates
    // without touching this code at all.
    //
    // Sent as a whole blob rather than a field diff because the payload is small
    // (one entity's components) and a diff would need a shared schema on both
    // ends; the lock guarantees nobody else is editing it concurrently, so a
    // wholesale overwrite cannot lose someone's work.
    struct ComponentUpdate {
        std::uint64_t             netId = 0;
        std::vector<std::uint8_t> blob;
    };

    bool sendComponents(const ComponentUpdate& update);

    void onComponents(std::function<void(ParticipantId, const ComponentUpdate&)> fn) {
        m_onComponents = std::move(fn);
    }

    // ── Structural changes ──
    // Creating, destroying and reparenting entities. The subject is a *network
    // id*, not a raw ECS handle: peers assign handles independently, so a raw
    // handle would name different entities on different machines. Entities that
    // came from the shared snapshot use their handle as their id (identical
    // everywhere, since everyone loaded the same bytes); newly created ones use
    // an id scoped to the participant that made them, so two peers creating at
    // the same moment cannot collide.
    struct StructuralChange {
        enum class Kind : std::uint8_t { Created, Destroyed, Reparented };

        Kind          kind      = Kind::Created;
        std::uint64_t netId     = 0;
        std::uint64_t parentNet = 0;   // Created / Reparented
        // Created: the serialized subtree, as SceneSerializer produces it.
        std::vector<std::uint8_t> blob;
    };

    bool sendStructural(const StructuralChange& change);

    void onStructural(std::function<void(ParticipantId, const StructuralChange&)> fn) {
        m_onStructural = std::move(fn);
    }

    // ── Authored-asset sync ──
    // Every authored asset — HorizonCode graphs, materials, UI widgets, particle
    // and animator graphs, scenes — is an HAsset file, so one blob transfer
    // covers all of them without a single line of per-type code.
    //
    // Binary media (meshes, textures, audio) deliberately does NOT travel this
    // way: those are large, rarely edited in-session, and are source control's
    // job. See the scope boundary in the design document.
    //
    // The subject is the same opaque 64-bit id used for locks, so an asset and
    // an entity are arbitrated by exactly the same table.
    struct AssetUpdate {
        std::uint64_t             subject = 0;
        std::string               path;      // project-relative
        std::vector<std::uint8_t> bytes;
    };

    // Publish a saved asset. Ignored unless we hold the lock on `subject`.
    bool sendAsset(std::uint64_t subject, const std::string& path,
                   const std::vector<std::uint8_t>& bytes);

    // A remote peer saved an asset — the editor writes it and reloads.
    void onAssetUpdated(std::function<void(ParticipantId, const AssetUpdate&)> fn) {
        m_onAsset = std::move(fn);
    }

    // ── Document deltas ──────────────────────────────────────────────────────
    // AssetUpdate replicates a whole authored file; this replicates ONE ITEM
    // inside one — a graph node, a link, a UI element. That is what makes an
    // open editor live rather than periodically reloaded: the receiver patches
    // the document it is already showing instead of re-reading the file, so its
    // canvas, selection and undo history survive.
    //
    // Deliberately generic, exactly as AssetUpdate is one level up: HorizonCode,
    // material, particle and animator graphs and the UI element tree all
    // decompose into identified items with an item-level JSON form, so one
    // message type covers all five and a sixth costs no protocol work.
    // HorizonNet does not interpret `scope`, `kind` or `json` — see
    // CollabDocSync in the editor for what they mean there.
    struct DocDelta {
        std::uint8_t  scope  = 0;   // which document inside the asset
        std::uint8_t  kind   = 0;   // node / link / element / variable / …
        std::uint8_t  op     = 0;   // 0 = upsert, 1 = remove
        std::int64_t  itemId = 0;   // identifies the item within (scope, kind)
        std::string   json;         // upsert payload; empty for a remove
    };

    // Publish a batch — everything one edit produced, so a paste of thirty nodes
    // arrives as one atomic frame rather than thirty that can interleave with
    // another peer's. Ignored unless we hold the lock on `subject`.
    //
    // Returns false when the batch cannot be sent AS DELTAS — no session, no
    // lock, or it exceeds the size bounds below. The caller must then fall back
    // to the whole-file AssetUpdate path: a coarser update is fine, a truncated
    // one is not (BitWriter::writeString silently cuts at 65535 bytes, which
    // would land as corrupt JSON on the peer).
    bool sendDocDeltas(std::uint64_t subject, const std::string& path,
                       const std::vector<DocDelta>& batch);

    void onDocDeltas(std::function<void(ParticipantId, std::uint64_t subject,
                                        const std::string& path,
                                        const std::vector<DocDelta>&)> fn) {
        m_onDocDeltas = std::move(fn);
    }

    // ── Locks ──
    // Ask to own `subject`. On the host this is answered immediately; on a
    // client it is a request, and the answer arrives via onLockChanged /
    // onLockDenied. Returns false only when there is no session to ask.
    bool requestLock(std::uint64_t subject);
    // Give up a lock. Silently ignored if we do not hold it.
    void releaseLock(std::uint64_t subject);

    // Current holder of `subject`, or nullptr when it is free.
    const LockInfo* lockFor(std::uint64_t subject) const;
    bool            ownsLock(std::uint64_t subject) const;
    const std::vector<LockInfo>& locks() const { return m_locks; }

    // A lock was taken or released — the editor repaints the affected item and,
    // if it lost one, stops allowing edits.
    void onLockChanged(std::function<void(const LockInfo&, bool acquired)> fn) {
        m_onLockChanged = std::move(fn);
    }
    // Our own request was refused.
    void onLockDenied(std::function<void(std::uint64_t, LockDenyReason)> fn) {
        m_onLockDenied = std::move(fn);
    }

    // Ask the HOST who holds `subject` right now. lockFor() reads the local
    // replica, which is correct almost always but is up to one round trip stale
    // — and that is exactly the moment an editor tab opens and has to decide
    // whether it is editable. This asks the authority instead. On the host it is
    // answered inline (it IS the table) so both roles have one code path.
    // Returns false when there is nobody to ask.
    bool queryLock(std::uint64_t subject);

    // The answer. `owner`/`ownerName` are meaningless when `held` is false.
    void onLockQueryResult(std::function<void(std::uint64_t subject, bool held,
                                              ParticipantId owner,
                                              const std::string& ownerName)> fn) {
        m_onLockQuery = std::move(fn);
    }

    // Drive the session. Pump the transport first, then this.
    void update();

    // Explicit-time overload. Presence throttling depends on the clock, and a
    // wall-clock read would make it untestable and unreproducible; passing the
    // time in also suits a fixed-step host loop.
    void update(std::uint64_t nowMs);

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

    // Presence
    void handlePresenceUpdate(ConnectionId conn, BitReader& r);   // host: from a client
    void handlePresenceRelay(BitReader& r);                       // client: from the host
    bool readPresenceBody(BitReader& r, PresenceState& out) const;
    void writePresenceBody(BitWriter& w, const PresenceState& p) const;
    void sendLocalPresence();
    bool presenceDiffersFromSent() const;
    void applyPresence(ParticipantId id, PresenceState state);

    // Locks
    void handleLockRequest(ConnectionId conn, BitReader& r);   // host
    void handleLockRelease(ConnectionId conn, BitReader& r);   // host
    void handleLockUpdate(BitReader& r);                       // client
    void handleLockDenied(BitReader& r);                       // client
    void handleLockTable(BitReader& r);                        // client, on join
    void writeLockTable(BitWriter& w) const;
    void grantLock(std::uint64_t subject, ParticipantId owner, const std::string& name);
    void dropLock(std::uint64_t subject);
    void broadcastLock(std::uint64_t subject, bool acquired, ConnectionId except);
    void releaseLocksOf(ParticipantId owner);

    // Transforms
    void handleTransformUpdate(ConnectionId conn, BitReader& r);   // host
    void handleTransformRelay(BitReader& r);                       // client

    // Components
    void handleComponentsUpdate(ConnectionId conn, BitReader& r);  // host
    void handleComponentsRelay(BitReader& r);                      // client

    // Structural
    void handleStructuralUpdate(ConnectionId conn, BitReader& r);  // host
    void handleStructuralRelay(BitReader& r);                      // client
    static void writeStructuralBody(BitWriter& w, const StructuralChange& c);
    static bool readStructuralBody(BitReader& r, StructuralChange& out,
                                   std::uint32_t maxBlob);

    // Document deltas
    void handleDocDeltasUpdate(ConnectionId conn, BitReader& r);   // host
    void handleDocDeltasRelay(BitReader& r);                       // client
    void writeDocDeltaBody(BitWriter& w, std::uint64_t subject, const std::string& path,
                           const std::vector<DocDelta>& batch) const;
    bool readDocDeltaBody(BitReader& r, std::uint64_t& subject, std::string& path,
                          std::vector<DocDelta>& out) const;

    // Lock query
    void handleLockQuery(ConnectionId conn, BitReader& r);         // host
    void handleLockQueryResult(BitReader& r);                      // client

    // Assets
    void handleAssetUpdate(ConnectionId conn, BitReader& r);       // host
    void handleAssetRelay(BitReader& r);                           // client
    void sendAssetChunks(ConnectionId conn, ParticipantId from, const AssetUpdate& a);
    void installAssetChunkHandlers();
    bool readAssetHeader(BitReader& r, AssetUpdate& out, std::uint32_t& outTotal,
                         std::uint32_t& outChunks) const;
    static void writeTransformBody(BitWriter& w, const TransformDelta& d);
    static bool readTransformBody(BitReader& r, TransformDelta& out);

    Participant* findParticipant(ParticipantId id);
    ParticipantId participantForConnection(ConnectionId conn) const;

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

    // Presence, keyed by participant. Cleared when they leave.
    std::vector<std::pair<ParticipantId, PresenceState>> m_presence;
    PresenceState m_localPresence;
    PresenceState m_lastSentPresence;
    bool          m_localPresenceSet  = false;
    bool          m_everSentPresence  = false;
    std::uint64_t m_lastPresenceSendMs = 0;

    // The authoritative table on the host; a replica on clients.
    std::vector<LockInfo> m_locks;

    // Inbound asset transfers in progress, keyed by the sender's connection so
    // two peers saving at once cannot interleave into one buffer.
    struct AssetAssembly {
        ConnectionId              from = kInvalidConnection;
        ParticipantId             sender = kInvalidParticipant;
        std::uint64_t             subject = 0;
        std::string               path;
        std::vector<std::uint8_t> bytes;
        std::uint32_t             expected = 0;
    };
    std::vector<AssetAssembly> m_assetAssembly;

    std::function<void(ParticipantId, std::uint64_t, const std::string&,
                       const std::vector<DocDelta>&)>          m_onDocDeltas;
    std::function<void(std::uint64_t, bool, ParticipantId, const std::string&)>
                                                                m_onLockQuery;
    std::function<void(ParticipantId, const ComponentUpdate&)>  m_onComponents;
    std::function<void(ParticipantId, const StructuralChange&)> m_onStructural;
    std::function<void(ParticipantId, const AssetUpdate&)>     m_onAsset;
    std::function<void(ParticipantId, const TransformDelta&)> m_onTransform;
    std::function<void(const LockInfo&, bool)>         m_onLockChanged;
    std::function<void(std::uint64_t, LockDenyReason)> m_onLockDenied;
    std::function<void(ParticipantId, const PresenceState&)> m_onPresence;
    std::function<void(ParticipantId)>                m_onJoined;
    std::function<void(JoinRejectReason, const std::string&)> m_onRejected;
    std::function<void(const Participant&)>           m_onJoin;
    std::function<void(ParticipantId)>                m_onLeft;
    std::function<void(std::uint32_t, std::uint32_t)> m_onProgress;
};

} // namespace HE::Net
