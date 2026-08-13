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
// v5: the handshake also carries a client key and a profile picture, and the
// host can eject a participant (kMsgRemoved). A v4 peer sends neither, so the
// host could not tell two of its editors apart — which is exactly what a ban
// has to key on.
// v6: participants choose their own colour, and the host assigns the final one.
// A v5 peer neither states a preference nor reads the answer, so it would draw
// everyone in a colour nobody else agrees with — the one thing a colour is for.
// v7: an asset frame says whether it CREATES the asset or updates one that
// already exists, and the host can refuse a create or arbitrate a delete or a
// rename. The intent is one byte in front of the subject, so a v6 peer would
// read it as the subject's top byte and then address a lock nobody holds —
// silently, on every asset transfer. Hence a version rather than a tolerated
// difference.
// v8: delete and rename say whether they mean a FOLDER, and folder creation
// travels as an op of its own. A v7 peer stops reading one byte early, so every
// request would look malformed to it and be dropped — the requester would sit
// waiting for a verdict nobody is going to give.
// v9: asset frames and document deltas carry the holder's REVISION, and a
// receiver refuses anything it has already moved past. Without it the two
// channels for one document had no order between them: a whole file arriving
// after newer deltas overwrote them, so a peer's edit appeared for a moment and
// then snapped back. A v8 peer writes neither number and misreads both frames.
// v10: an asset-op request ends in the BATCH it belongs to, and Reimport joins
// the op enum. Selecting twenty assets and pressing Delete is ONE decision, and
// the host was shown twenty rows to click through; the batch id is what lets it
// be drawn and answered as one. A v10 host reading a v9 request runs out of
// bytes at the batch id and drops the frame — the requester then waits for a
// verdict that is never coming, which is precisely the failure a refused join
// prevents. Reimport travels the same channel because it is the one thing that
// changes an asset's bytes without those bytes ever going on the wire.
// v11: the join handshake carries "does this session also send the BIG assets"
// — a bool at the end of the request and one in the accept. Both directions
// misparse without the bump, and neither failure is quiet: a v10 host stops
// reading before the joiner's answer and admits somebody who will never publish
// a mesh, while a v11 client reading a v10 accept takes the roster count's first
// byte for the flag and shears every field behind it, so the roster it builds is
// nonsense. In the other direction a v10 request makes a v11 host log "malformed
// join request" instead of the version mismatch it actually is, which is the one
// verdict a user can act on.
// v12: the LAN announcement carries the same flag, so a session found on this
// network can say what it costs before anyone clicks it. The datagram itself
// tolerates the older shape — the field is appended and read only when it is
// there (LanBeacon.h) — so nothing on the wire forces this bump. What forces it
// is the ANSWER a missing field produces: a v11 host with the setting on would
// be listed as "does not transfer large assets" and still be joinable, which is
// the list advertising one thing while the handshake enforces another. Bumped,
// the two sets coincide: an announcement that cannot state the flag comes from a
// build this one refuses to join anyway, so the wrong answer is never one a user
// can act on.
// v13: a refusal travels BACK to whoever sent the file. The two ceilings are set
// on two machines by two people, and until now the lower one won in silence: the
// sender's own limit let the bytes go, so nothing on its side failed and nothing
// on its side ever said otherwise, while the only editor that knew the file had
// not been written was the one that refused it. The two machines then held
// different files with nobody on the sending side any the wiser — which is the
// very failure the refusal notice was added to end, solved for one of the two
// people it happens to. Nothing on the wire forces this bump: an id a peer does
// not know is throttle-logged and dropped (NetSession::pump), never fatal. What
// forces it is the ANSWER, exactly as in v12 — a v12 host drops a v13 client's
// report without a word, so the refusal it was carrying evaporates in the one
// hop that could have delivered it and the sender is told nothing. That is the
// same silence as before, now produced by a session that looks like it works.
inline constexpr std::uint16_t kCollabProtocolVersion = 13;

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
    // The host banned this client earlier in this session. The refusal is
    // automatic: the whole point of a ban is that the host is not asked again.
    Banned          = 5,
    // This session also carries the BIG assets — meshes, textures, audio — and
    // the joiner has not agreed to that. It is a rejection rather than a silent
    // demotion because the cost falls on the joiner's connection, not the
    // host's: somebody on a metered or slow link has to be able to say no, and
    // the only moment they can still say it is before the transfer starts.
    //
    // It is its OWN reason and not folded into a generic refusal precisely so
    // the client can tell it apart and ASK, rather than reporting a dead end to
    // a user who would happily have agreed. See the prompt in CollabPanel.
    LargeAssetsRequired = 6,
};

// Why a participant stopped being in the session, as told to the participant
// itself. A kick is a door held open; a ban locks it for the session's lifetime.
enum class RemovalReason : std::uint8_t {
    Kicked = 0,
    Banned = 1,
};

// A profile picture. Raw RGBA8, square, `size` pixels per side — deliberately
// NOT an encoded PNG: HorizonNet would then need an image decoder, and a peer
// could hand us a decompression bomb dressed as a portrait. Raw pixels have
// exactly one possible size (size*size*4), which the receiver checks before
// allocating anything.
struct Avatar {
    std::uint16_t             size = 0;   // 0 = this participant has no picture
    std::vector<std::uint8_t> rgba;       // size*size*4 bytes

    bool empty() const { return size == 0 || rgba.empty(); }
};

// ─── Participant colours ─────────────────────────────────────────────────────
// The colour someone's viewport marker, selection highlight and lock badges are
// drawn in. It is the fastest way to tell two collaborators apart, so it is worth
// getting right: users pick their own, and the HOST has the last word, because
// only it can see everybody and refuse a colour that is already taken.
struct ParticipantColor {
    std::uint8_t r = 0, g = 0, b = 0;

    // All-zero is the "no preference" marker rather than the colour black: black
    // is unusable as a marker on any background the editor actually has, so
    // nothing is lost by spending the value on a sentinel.
    bool unset() const { return r == 0 && g == 0 && b == 0; }
};

// The colours the editor offers as presets and the host draws from when it has
// to assign one itself. Well separated on the wheel and all legible against a
// dark viewport — a palette whose neighbours are a nudge apart tells nobody
// anything. Their mutual separation is asserted in the tests, so adding one
// carelessly fails there rather than in someone's session.
inline constexpr ParticipantColor kParticipantPalette[] = {
    { 232,  74,  74 },   // red
    { 240, 150,  50 },   // orange
    { 226, 206,  64 },   // yellow
    { 110, 205,  70 },   // green
    {  60, 200, 175 },   // teal
    {  70, 150, 235 },   // blue
    { 150, 110, 240 },   // violet
    { 230,  95, 190 },   // pink
};

// How close two colours may be before the host treats them as the same one.
// Straight Euclidean distance in RGB — crude as colour science, but the job here
// is only to catch "these two are indistinguishable at gizmo size", and every
// palette entry clears it comfortably.
inline constexpr int kColorCollisionDistance = 48;

struct Participant {
    ParticipantId id = kInvalidParticipant;
    std::string   name;
    bool          isHost = false;
    Avatar        avatar;
    // ASSIGNED BY THE HOST. A joiner says what it would like; if that clashes
    // with somebody already here, the host hands back a free one instead. Every
    // peer therefore agrees on who is which colour, which a locally derived
    // colour could never guarantee once people pick their own.
    ParticipantColor color;

    // Stable identity of the editor install behind this participant, minted once
    // per installation and sent in the join handshake. ONLY THE HOST EVER SEES
    // IT: it is never relayed to the other clients, because the one thing it is
    // for — keying a ban so the same person is refused on reconnect — is the
    // host's job alone, and handing every peer a durable id for everyone else
    // would be a tracking token nobody asked for.
    //
    // Empty on clients (including for their own entry) and for the host's own
    // participant, since a host cannot ban itself.
    std::string clientKey;
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

        // This user's profile picture, shown next to their name everywhere the
        // roster appears. Optional — an empty avatar simply falls back to the
        // participant's colour and initial.
        Avatar        avatar;
        // Upper bound on an ACCEPTED avatar, in pixels per side. A peer that
        // announces more is refused its picture (not its join): a portrait is
        // decoration, and losing it must never cost someone their session.
        // 64² RGBA is 16 KiB, which rides along in the join handshake without
        // being worth chunking.
        std::uint16_t maxAvatarSize = 64;

        // What this user would like to be drawn in. Left unset, the host picks
        // one — which is also what happens when the wish is already taken, so
        // this is a preference and never a guarantee.
        ParticipantColor preferredColor;

        // Stable identity of this editor install, sent to the host on join. See
        // Participant::clientKey — it exists so a ban survives a reconnect, and
        // so it must be the same string every time this editor joins. Empty is
        // allowed (bans then fall back to matching the display name, which is
        // weaker but better than nothing).
        std::string   clientKey;
        // Upper bound on an accepted snapshot. Guards against a hostile or buggy
        // host announcing a size that would exhaust memory.
        std::uint32_t maxSnapshotBytes = 64u * 1024u * 1024u;
        // Upper bound on ONE asset transfer, which used to be the same number.
        // They are separate because they are bounded by different things: a
        // snapshot is the scene, it happens once per join, and its size is not
        // anybody's choice. An asset transfer is a file a user picked, it happens
        // on every save, and in a session that carries meshes and textures it is
        // the number that decides whether their work travels at all. Tying the
        // two meant raising the ceiling for a 200 MB mesh also raised what a peer
        // could make us hold for a scene we never asked for.
        //
        // Same default, so a session that nobody has configured behaves exactly
        // as it did before this was split out.
        std::uint32_t maxAssetBytes    = 64u * 1024u * 1024u;
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

        // Does this session also carry the assets that are normally left out —
        // meshes, textures, audio, fonts? See isCollabSyncableAssetType for the
        // set and why it is drawn where it is.
        //
        // The meaning depends on the role, and deliberately so. On a HOST this
        // IS the session's rule, because the host is the one thing every peer
        // shares. On a CLIENT it is only a statement of consent: "I am willing
        // to receive them." A client that says no to a host that says yes is
        // refused (LargeAssetsRequired) so it can be asked; a client that says
        // yes to a host that says no simply follows the session and sends
        // nothing large, because the host decides and there is nobody for the
        // client's preference to bind.
        bool syncLargeAssets = false;

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

        // A removed peer is told first and cut loose a moment later. Closing the
        // link in the same breath would be simpler and wrong: a transport
        // discards whatever is still queued when the local side closes (see
        // TcpTransport::disconnect), so the kick message — the only thing that
        // distinguishes being thrown out from the host's network dying — is
        // exactly what would be lost.
        std::uint64_t removalGraceMs = 500;
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
    // Binary media (meshes, textures, audio) does not normally travel this way:
    // it is large, rarely edited in-session, and is source control's job. See
    // the scope boundary in the design document. A HOST may decide otherwise for
    // its session (Config::syncLargeAssets), and then the same blob transfer
    // carries those too — the message never cared which kind it was, and the
    // decision has always lived a layer up in the editor.
    //
    // The subject is the same opaque 64-bit id used for locks, so an asset and
    // an entity are arbitrated by exactly the same table.
    // What an asset frame is doing. Carried on the wire rather than inferred
    // from `fs::exists` at the receiver: the two answers differ during a race,
    // and the sender is the only side that knows which it meant.
    enum class AssetIntent : std::uint8_t {
        Update = 0,   // the asset exists on both sides; these are its new bytes
        Create = 1,   // it does not exist yet — the host arbitrates the name
    };

    // What a participant wants done to an asset that already exists. Unlike a
    // create, neither can be settled by a rule: they destroy or move someone
    // else's work, so the host is ASKED and a human answers.
    enum class AssetOp : std::uint8_t {
        Delete = 0,
        Rename = 1,
        // Creating a FOLDER. Assets are created through the AssetUpdate path,
        // which carries their bytes; a folder has none, so it travels here.
        // Permissive like any other create — it destroys nothing, so there is
        // nothing for the host to weigh, and it is broadcast without asking.
        Create = 2,
        // "May I edit this?" — the only op whose answer is not the host's. It
        // interrupts the HOLDER's work, so the holder decides; the host merely
        // knows who that is and forwards it.
        Edit   = 3,
        // "The bytes behind this asset changed and they are NOT coming down this
        // wire." A reimport rewrites an asset in place from its source file, and
        // for the kinds reimport actually touches — meshes, textures, audio —
        // the bytes usually do not travel (see the scope note above). Peers used
        // to keep the old asset for ever with nothing to tell them, so this
        // carries the one thing that is left to say: go and pull it. Permissive
        // like Create — it asks for nothing and destroys nothing, so it is
        // relayed rather than queued in front of a human.
        //
        // In a session that DOES carry the big assets (Config::syncLargeAssets)
        // this op simply stops being reached for those kinds: the new bytes go
        // out through the ordinary AssetUpdate path instead, which is the whole
        // difference the setting makes. It still exists for that session — a
        // reimport whose file is too large, or whose lock is held elsewhere,
        // lands here exactly as before.
        Reimport = 4,
    };

    // Why the host would not take a create. Spelled out because the creator has
    // the file on their disk either way and has to be told which it was — a
    // refusal that only says "no" leaves them with a local asset nobody else
    // will ever see.
    enum class AssetRejectReason : std::uint8_t {
        None        = 0,
        NameTaken   = 1,   // something already lives at that path
        NotSyncable = 2,   // an asset kind that does not travel (mesh, audio, …)
        TooLarge    = 3,
        BadPath     = 4,   // leaves the project — see HE::isRelativePathContained
        RateLimited = 5,
        NotPermitted = 6,
    };

    struct AssetUpdate {
        std::uint64_t             subject = 0;
        std::string               path;      // project-relative
        std::vector<std::uint8_t> bytes;
        AssetIntent               intent = AssetIntent::Update;
        // The holder's revision, the same counter the document deltas carry.
        // These are two channels for one document with no ordering between
        // them: a whole file that arrived after newer deltas used to overwrite
        // them, and the peer's edit appeared for a moment and then snapped back.
        // The receiver refuses anything at or below what it has already applied.
        std::uint32_t             revision = 0;
    };

    // Publish a saved asset. Ignored unless we hold the lock on `subject`.
    bool sendAsset(std::uint64_t subject, const std::string& path,
                   const std::vector<std::uint8_t>& bytes);
    // The same, for a frame that is not an ordinary save — the caller fills in
    // the intent. The three-argument form above is this one with Update.
    bool sendAsset(const AssetUpdate& a);

    // A remote peer saved an asset — the editor writes it and reloads.
    void onAssetUpdated(std::function<void(ParticipantId, const AssetUpdate&)> fn) {
        m_onAsset = std::move(fn);
    }

    // An incoming asset THIS side would not take: it announced more bytes than
    // maxAssetBytes, so not one of them was read and the file will never appear
    // here. (path, announced size.)
    //
    // It exists because the two ceilings are set independently and the lower one
    // wins without either user being party to the decision. The sender's own
    // ceiling let this through, so from here the refusal is always ours — and on
    // their machine the save simply looked as though it worked. Somebody has to
    // be told, and this side is the only one that knows; a log line would leave
    // both of them believing the file travelled.
    void onAssetRefused(std::function<void(ParticipantId, const std::string& path,
                                           std::uint32_t bytes)> fn) {
        m_onAssetRefused = std::move(fn);
    }

    // The other half of the same event, on the machine that SENT the file: a peer
    // would not take what we published, because their ceiling is lower than ours.
    // (who refused it, path, the size we announced, THEIR ceiling in bytes, and
    // what the frame was doing.)
    //
    // The ceiling is theirs and so the number has to come off the wire: this
    // process has no way to know it, and quoting our own would tell the reader the
    // limit that let the file through. The intent is here for the same reason —
    // "they still have the older file" is true of a save and false of a create,
    // where they have no asset at all, and only the frame knows which it was.
    //
    // Nothing on this side failed, which is exactly why this exists: without it a
    // save that was refused two hops away is indistinguishable from one that
    // worked, and the two machines part company over a file neither user has been
    // given any reason to look at.
    void onAssetRefusedByPeer(
        std::function<void(ParticipantId refusedBy, const std::string& path,
                           std::uint32_t bytes, std::uint32_t theirLimit,
                           AssetIntent intent)> fn) {
        m_onAssetRefusedByPeer = std::move(fn);
    }

    // ── Host: may this create go ahead? ──
    // HorizonNet cannot answer this itself — whether a name is free, whether the
    // kind of asset travels at all, whether the path stays inside the project
    // are all questions about the editor's filesystem and asset types, and this
    // layer knows none of them. So it asks, once, before applying or relaying.
    //
    // Return false to refuse: fill `reason`, and `suggestedPath` when there is a
    // sensible alternative (a free name next to the taken one). The creator is
    // told either way — they have the file locally regardless, and a silent
    // refusal would leave them with an asset nobody else will ever see.
    //
    // Not installed = everything is accepted, which is what a host that does not
    // care should get rather than a session where creates vanish.
    using CreatePolicy = std::function<bool(const AssetUpdate&,
                                            AssetRejectReason& reason,
                                            std::string& suggestedPath)>;
    void setCreatePolicy(CreatePolicy fn) { m_createPolicy = std::move(fn); }

    // The creator's side of the same exchange. `accepted` false means the asset
    // exists on this machine and nowhere else.
    void onAssetCreateResult(
        std::function<void(const std::string& path, bool accepted,
                           AssetRejectReason, const std::string& suggestedPath)> fn) {
        m_onCreateResult = std::move(fn);
    }

    // ── Delete and rename: asked for, not done ──
    // Unlike everything else here, these are not replicated as they happen —
    // they destroy or move work that is not the requester's, so the host is
    // asked and a person answers. This layer only carries the exchange: the
    // QUEUE lives in the editor, because answering it needs a surface, and a
    // network session is the wrong place to keep something a human must read.
    //
    // Client → host. Returns the request id, or 0 outside a session. The host
    // calling this gets 0 too: it does not ask itself.
    // `folder` is not folded into the op because deleting a folder IS a delete —
    // same decision, same row in the host's queue, same wording. It differs only
    // in what gets removed, which is exactly what a flag is for.
    // `subject` is the same opaque id the locks use — the editor derives it
    // from the path and this layer never could. Only Edit needs it (to find the
    // holder), but it rides along on every op rather than being a special case
    // the caller has to remember for one of four.
    //
    // `batch` names the USER ACTION this request is part of, 0 meaning "on its
    // own". Selecting twenty assets and pressing Delete is one decision, and it
    // used to arrive as twenty unrelated rows the host had to click through one
    // by one. The id is minted by the requester and is only ever unique within
    // it, so the host keys a bundle on (requester, batch) — two clients that
    // both happen to call their batch 1 are still two bundles.
    //
    // Deliberately NOT a count: the host cannot be told up front how many are
    // coming, because the requests arrive over several frames and a header that
    // promised twenty would be wrong for as long as nineteen had landed. The
    // bundle is simply the rows that share the key, whatever they are right now.
    std::uint32_t requestAssetOp(AssetOp op, const std::string& path,
                                 const std::string& newPath = {},
                                 bool folder = false,
                                 std::uint64_t subject = 0,
                                 std::uint32_t batch = 0);

    // Host: somebody wants something done. Queue it and answer later.
    void onAssetOpRequested(
        std::function<void(ParticipantId, std::uint32_t requestId, AssetOp,
                           const std::string& path, const std::string& newPath,
                           bool folder, std::uint32_t batch)> fn) {
        m_onOpRequested = std::move(fn);
    }

    // Host: the answer, to the one who asked. Denying is not a failure state —
    // it is the feature — so it travels the same way as approval.
    void sendAssetOpVerdict(ParticipantId to, std::uint32_t requestId, bool approved);

    // Host: it happened; everyone applies it. Sent after the verdict, and only
    // on approval.
    void broadcastAssetOpApply(AssetOp op, const std::string& path,
                               const std::string& newPath, ParticipantId by,
                               bool folder = false);

    // Client: the answer to something we asked for.
    void onAssetOpVerdict(
        std::function<void(std::uint32_t requestId, bool approved)> fn) {
        m_onOpVerdict = std::move(fn);
    }

    // Everyone: it was approved — do it locally. Also fires on the host, so one
    // code path applies it everywhere.
    void onAssetOpApply(
        std::function<void(ParticipantId by, AssetOp, const std::string& path,
                           const std::string& newPath, bool folder)> fn) {
        m_onOpApply = std::move(fn);
    }

    // ── Asking the holder for an asset ──
    // Somebody wants what WE are holding. The host routed it here because we
    // have the lock; answering yes releases it and hands it to them.
    void onAssetEditRequested(
        std::function<void(ParticipantId from, std::uint32_t requestId,
                           const std::string& path)> fn) {
        m_onEditRequested = std::move(fn);
    }
    // The holder's answer, back to the host, which grants the lock on a yes.
    void sendAssetEditAnswer(ParticipantId requester, std::uint32_t requestId,
                             const std::string& path, bool allowed,
                             std::uint64_t subject);

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
    // `revision` is the SENDER's count for this document — it holds the lock, so
    // it is the single source of truth and its numbering is the only one. See
    // onDocDeltas for what the receiver does with it.
    bool sendDocDeltas(std::uint64_t subject, const std::string& path,
                       const std::vector<DocDelta>& batch, std::uint32_t revision);

    // The revision comes through so the receiver can refuse anything it has
    // already moved past. Deltas and whole files are two channels carrying one
    // document with no ordering between them: without this a file that arrived
    // late overwrote newer deltas, and the edit appeared for a moment and then
    // snapped back.
    void onDocDeltas(std::function<void(ParticipantId, std::uint64_t subject,
                                        const std::string& path,
                                        const std::vector<DocDelta>&,
                                        std::uint32_t revision)> fn) {
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

    // ── Moderation (host only) ───────────────────────────────────────────────
    // The host owns the participant registry, so it is also the only side that
    // can take someone out of it. Both calls are no-ops on a client and for the
    // host's own id — there is nobody above the host to appeal to, and equally
    // nobody for it to eject itself towards.
    //
    // Kicking ends the session for that peer, who may turn around and join
    // again; banning also records them, so the next join request is refused
    // without the host being asked. The ban lasts as long as THIS session —
    // closing the session forgets it, which is the scope a host can reason about
    // without maintaining a list they will never see again.
    bool kickParticipant(ParticipantId id);
    bool banParticipant(ParticipantId id);

    struct BanEntry {
        // What the ban is keyed on. Matching prefers the client key; when the
        // peer sent none (an editor with no identity file yet), the display name
        // is the fallback — weaker, but the alternative is a ban that does
        // nothing at all.
        std::string clientKey;
        std::string name;
    };
    const std::vector<BanEntry>& bans() const { return m_bans; }
    // Let someone back in. Matches the same way a join request is checked.
    bool unban(const std::string& clientKey, const std::string& name);

    // We were removed from the session by the host. Fires on the client only, and
    // before the link goes down, so the editor can say which of the two happened
    // instead of reporting a generic lost connection.
    void onRemoved(std::function<void(RemovalReason)> fn) { m_onRemoved = std::move(fn); }

    // A participant was ejected by us. Host-side counterpart of onParticipantLeft,
    // kept apart from it so the UI can distinguish "they left" from "we removed
    // them".
    void onParticipantRemoved(std::function<void(const Participant&, RemovalReason)> fn) {
        m_onRemovedLocal = std::move(fn);
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

    // Does THIS SESSION carry the big assets? The host's answer, which is the
    // only one that means anything: a client learns it in the accept and
    // publishes by it for the rest of the session, whatever its own Config said.
    //
    // A client answers false until it has been accepted. That is not a guess
    // standing in for the real value — before the accept there is no session to
    // publish into, so nothing can consult this and be wrong.
    bool sessionSyncsLargeAssets() const { return m_sessionLargeAssets; }

    // The ceiling on ONE asset transfer, in bytes. sendAsset refuses anything
    // over it and the receiving side refuses it again — nothing is truncated,
    // the frame is simply never emitted. Exposed because the editor has to be
    // able to TELL the user which of the two things happened when a save does
    // not travel, and repeating the number there would let the two drift apart.
    std::uint32_t maxAssetBytes() const { return m_cfg.maxAssetBytes; }

    // The user moved the setting. Allowed WHILE A SESSION IS UP, unlike the
    // large-asset rule next to it in the same panel, and the difference is not an
    // oversight: that one is an agreement between peers, and half a session
    // running each way is a group that silently holds different files. This one
    // binds nothing but this machine's own memory and its own link. Two peers are
    // expected to disagree about it — the lower of the two is simply what gets
    // through — so there is no shared state for a mid-session change to break,
    // and the refusal notice can tell someone to raise it and mean "now",
    // instead of "leave the session first".
    void setMaxAssetBytes(std::uint32_t bytes) { m_cfg.maxAssetBytes = bytes; }

private:
    void installHandlers();

    // Host side
    void handleJoinRequest(ConnectionId conn, BitReader& r);
    void sendSnapshotTo(ConnectionId conn);
    void onPeerConnected(ConnectionId conn);
    void onPeerDisconnected(ConnectionId conn);

    // Moderation
    bool removeParticipant(ParticipantId id, RemovalReason reason);
    // Everything that has to happen when a participant stops being in the
    // session, whichever way it happened: their locks freed, their presence
    // dropped, the roster pruned, everyone else told. Shared by the disconnect
    // path and the kick path so the two cannot drift apart.
    void retireParticipant(ParticipantId id, ConnectionId conn, ConnectionId except);
    bool isBanned(const std::string& clientKey, const std::string& name) const;
    void handleRemoved(BitReader& r);   // client: the host ejected us

    // Host: settle on a colour for a joiner. Honours `wish` when it is set and
    // far enough from everyone already here; otherwise takes a free palette
    // entry, starting from an offset derived from `seed` so consecutive joiners
    // do not all land on red. Deterministic given the same inputs — which is
    // what makes it testable without making it predictable to a user.
    ParticipantColor assignColor(ParticipantColor wish, std::uint32_t seed) const;

    // Avatars ride along in the join handshake and in the roster announcements.
    static void writeAvatar(BitWriter& w, const Avatar& a);
    // Refuses an avatar larger than `maxSize` rather than allocating what the
    // peer claims. A refused picture leaves `out` empty and still returns true —
    // losing a portrait must not cost anyone their session.
    static bool readAvatar(BitReader& r, Avatar& out, std::uint16_t maxSize);

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
                           const std::vector<DocDelta>& batch,
                           std::uint32_t revision) const;
    bool readDocDeltaBody(BitReader& r, std::uint64_t& subject, std::string& path,
                          std::vector<DocDelta>& out, std::uint32_t& revision) const;

    // Lock query
    void handleLockQuery(ConnectionId conn, BitReader& r);         // host
    void handleLockQueryResult(BitReader& r);                      // client

    // Assets
    void handleAssetUpdate(ConnectionId conn, BitReader& r);       // host
    void handleAssetRelay(BitReader& r);                           // client
    void handleAssetCreateResult(BitReader& r);                    // client
    void handleAssetOpRequest(ConnectionId conn, BitReader& r);    // host
    void handleAssetOpVerdict(BitReader& r);                       // client
    void handleAssetOpApply(BitReader& r);                         // client
    void handleAssetEditRequest(BitReader& r);                     // the holder
    void handleAssetEditAnswer(ConnectionId conn, BitReader& r);   // host
    // A refusal on its way home. Two hops, because a client never talks to
    // another client: the refuser tells the host, and the host is the only side
    // that can reach the peer whose file it was.
    void handleAssetRefusedReport(ConnectionId conn, BitReader& r);  // host
    void handleAssetRefused(BitReader& r);                           // the sender
    // Host: hand a refusal to `author`, wherever they are — over the wire, or
    // straight to the callback when the author is the host itself. `refuser` is
    // always stamped by the host from the connection it heard the refusal on, so
    // no peer can put a refusal in somebody else's mouth.
    void sendAssetRefusalTo(ParticipantId author, ParticipantId refuser,
                            AssetIntent intent, const std::string& path,
                            std::uint32_t bytes, std::uint32_t limit);
    // Client: "the file you relayed to me from `author` was over my ceiling."
    void reportAssetRefusal(ParticipantId author, AssetIntent intent,
                            const std::string& path, std::uint32_t bytes);
    // Host: route an edit request to whoever holds the lock. True when it went
    // somewhere; false means nobody holds it and the caller can just grant it.
    bool routeEditRequest(ParticipantId from, std::uint32_t requestId,
                          const std::string& path, std::uint64_t subject);
    // Release and grant as ONE step — see the body for why the gap matters.
    void handOverLock(std::uint64_t subject, ParticipantId to);
    // Host: run the policy and answer the creator. True = go ahead and relay.
    bool arbitrateCreate(ConnectionId conn, const AssetUpdate& a);
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
    // The session's rule about big assets, as opposed to Config::syncLargeAssets
    // which is only this peer's wish. Set from the config on a host (it IS the
    // authority) and from the accept on a client, and cleared when a client's
    // link drops — a stale "yes" would outlive the session that granted it.
    bool                     m_sessionLargeAssets = false;

    // Host: which connection belongs to which participant.
    std::vector<std::pair<ConnectionId, ParticipantId>> m_connToParticipant;

    // Host: peers told they are out, whose link is severed once the grace period
    // is up (see Config::removalGraceMs). Until then they are already gone from
    // the roster, and anything further they send is ignored — they are no longer
    // a joined participant, which every host handler already checks.
    struct PendingRemoval {
        ConnectionId  conn  = kInvalidConnection;
        std::uint64_t dueMs = 0;
        bool          armed = false;   // dueMs is only meaningful once stamped
    };
    std::vector<PendingRemoval> m_pendingRemovals;
    std::vector<BanEntry>       m_bans;

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
        // Carried from the Begin frame to the End frame: the receiver has to
        // know what it was told when the last chunk lands, not re-guess then.
        AssetIntent               intent = AssetIntent::Update;
        std::uint32_t             revision = 0;
    };
    std::vector<AssetAssembly> m_assetAssembly;

    std::function<void(ParticipantId, std::uint64_t, const std::string&,
                       const std::vector<DocDelta>&, std::uint32_t)> m_onDocDeltas;
    std::function<void(std::uint64_t, bool, ParticipantId, const std::string&)>
                                                                m_onLockQuery;
    std::function<void(ParticipantId, const ComponentUpdate&)>  m_onComponents;
    std::function<void(ParticipantId, const StructuralChange&)> m_onStructural;
    std::function<void(ParticipantId, const AssetUpdate&)>     m_onAsset;
    std::function<void(ParticipantId, const std::string&, std::uint32_t)>
                                                               m_onAssetRefused;
    std::function<void(ParticipantId, const std::string&, std::uint32_t,
                       std::uint32_t, AssetIntent)>            m_onAssetRefusedByPeer;
    CreatePolicy                                               m_createPolicy;
    std::function<void(const std::string&, bool, AssetRejectReason, const std::string&)>
                                                               m_onCreateResult;
    std::function<void(ParticipantId, std::uint32_t, AssetOp,
                       const std::string&, const std::string&, bool,
                       std::uint32_t)>                          m_onOpRequested;
    std::function<void(std::uint32_t, bool)>                    m_onOpVerdict;
    std::function<void(ParticipantId, AssetOp, const std::string&,
                       const std::string&, bool)>               m_onOpApply;
    std::function<void(ParticipantId, std::uint32_t, const std::string&)>
                                                                m_onEditRequested;
    // Ids are per-CLIENT and only ever paired with that client's own requests,
    // so a plain counter is enough — two peers may both hold request 3 and the
    // host never confuses them, because it also knows which connection asked.
    std::uint32_t                                               m_nextOpRequestId = 1;
    std::function<void(ParticipantId, const TransformDelta&)> m_onTransform;
    std::function<void(const LockInfo&, bool)>         m_onLockChanged;
    std::function<void(std::uint64_t, LockDenyReason)> m_onLockDenied;
    std::function<void(ParticipantId, const PresenceState&)> m_onPresence;
    std::function<void(ParticipantId)>                m_onJoined;
    std::function<void(JoinRejectReason, const std::string&)> m_onRejected;
    std::function<void(const Participant&)>           m_onJoin;
    std::function<void(ParticipantId)>                m_onLeft;
    std::function<void(RemovalReason)>                m_onRemoved;
    std::function<void(const Participant&, RemovalReason)> m_onRemovedLocal;
    std::function<void(std::uint32_t, std::uint32_t)> m_onProgress;
};

} // namespace HE::Net
