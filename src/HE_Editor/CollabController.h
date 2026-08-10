#pragma once

// ─── Editor ↔ HorizonNet bridge ──────────────────────────────────────────────
// Owns the whole networking stack for a collaboration session so neither
// EditorApplication nor the UI has to know how it fits together:
//
//     TcpTransport → SecureTransport → NetSession → CollabSession
//
// This is also the layer where the scene finally meets the network. HorizonNet
// deliberately knows nothing about HorizonScene (see CollabSession's
// ISessionStateProvider), so the implementation that serializes the world lives
// here — in the editor, the one place that already depends on both.
//
// Everything is poll-driven: update() must be called once per frame or nothing
// happens at all.

#include <Net/CollabSession.h>
#include <Net/PortMapper.h>
#include <Types/Enums.h>   // HE::AssetType — the .hasset header's real type

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class HorizonWorld;

namespace HE::Net {
class TcpTransport;
class SecureTransport;
class NetSession;
} // namespace HE::Net

class CollabController
{
public:
	enum class Status : std::uint8_t
	{
		Idle,        // nothing running
		Hosting,     // listening; peers may join
		Connecting,  // client: link up, join not yet complete
		Joined,      // client: snapshot applied, fully in the session
		Failed,      // see lastError()
	};

	CollabController();
	~CollabController();

	CollabController(const CollabController&)            = delete;
	CollabController& operator=(const CollabController&) = delete;

	// The world that is captured for joiners and replaced on join. Must outlive
	// any active session.
	void setWorld(HorizonWorld* world) { m_world = world; }

	// Which project this editor has open. Compared on join and refused when the
	// two sides differ, because everything a session sends — scene entities,
	// asset references, lock subjects — is addressed by uuids that only mean
	// something inside ONE project. Joining a session whose host had a different
	// project open used to succeed: the scene arrived and every asset reference
	// in it dangled, leaving the content browser showing nothing that resolved.
	//
	// `id` is the project manifest's stable uuid (ProjectData::id); `label` is
	// its display name, which the host sends back with a refusal so the joiner
	// can be told WHICH project to open. Set before hosting or joining.
	void setProjectIdentity(std::string id, std::string label)
	{
		m_projectId    = std::move(id);
		m_projectLabel = std::move(label);
	}

	// Fires after a received snapshot replaced the world. The editor must drop
	// its selection and undo history here — both refer to entity handles that no
	// longer exist, and acting on them would touch freed storage.
	void onWorldReplaced(std::function<void()> fn) { m_onWorldReplaced = std::move(fn); }

	// ── Lifecycle ──
	// Opens a session on `port` (0 = OS-assigned), generates a session id and a
	// join code, and publishes the endpoint to the session directory so peers can
	// find it without being told an address.
	bool startHosting(std::uint16_t port, const std::string& displayName);

	// The guest-facing path: a session id and a join code are all a peer needs.
	// The address is resolved through the directory, which is why this completes
	// asynchronously — watch status() and directoryBusy().
	bool joinBySessionId(const std::string& sessionId, const std::string& joinCode,
	                     const std::string& displayName);

	// Direct connect, bypassing the directory. Kept for LAN use and diagnostics
	// when the directory is unreachable.
	bool joinSession(const std::string& host, std::uint16_t port,
	                 const std::string& joinCode, const std::string& displayName);
	void leave();

	// Same cleanup as leave(), but it WAITS for it. Call once while the editor
	// is still a running process — from OnShutdown, not from a destructor that
	// runs on the way out.
	//
	// leave() detaches its router and directory calls because closing a session
	// has to feel instant. At process exit that is exactly wrong: a detached
	// thread has no guarantee of finishing before the process goes, so the port
	// forward, the IPv6 pinhole and the directory entry would all outlive the
	// editor. That is how one router accumulated 418 stale forwards.
	void shutdown();

	// Directory endpoint. Defaults to the public one; override with the
	// HE_COLLAB_DIRECTORY environment variable when testing against another.
	static std::string directoryEndpoint();

	// The colour a participant is drawn in: viewport marker, selection highlight,
	// lock badges, footer avatar. Prefers the colour the HOST assigned them —
	// that is the one everybody in the session agrees on — and falls back to a
	// value derived from the id.
	//
	// The fallback is not dead weight: lock badges name an owner who may already
	// have left the roster, and panels ask about ids that were never in it.
	void colorFor(HE::Net::ParticipantId id, float outRgb[3]) const;

	// The fallback on its own. Golden-ratio hue stepping off the id, so two
	// people who joined one after another never land on near-identical colours.
	// Static because the places that need a colour without a session — a lock
	// badge left behind by someone who is gone — have no controller to ask.
	static void participantColor(HE::Net::ParticipantId id, float outRgb[3]);

	// ── Local identity ───────────────────────────────────────────────────────
	// Who this editor is in a session: a display name, an optional profile
	// picture, and a stable key identifying this installation. All three are
	// persisted, so a user sets them once rather than on every join — and the
	// key HAS to persist, since a session ban is keyed on it and would otherwise
	// be shrugged off by restarting the editor.
	//
	// Read when a session STARTS. Editing them while one is running changes
	// nothing until the next join; the panel says so rather than pretending.
	struct Identity
	{
		std::string name = "Horizon User";
		// Square RGBA8, `avatarSize` pixels per side; empty when the user has
		// not picked a picture. Decoded here rather than on the wire so
		// HorizonNet never has to parse an image file — see HE::Net::Avatar.
		std::vector<std::uint8_t> avatarRgba;
		std::uint16_t             avatarSize = 0;
		std::string               clientKey;
		// The colour this user would like to be drawn in. Unset means "whatever
		// is free" — and so does a colour the host finds already taken, which is
		// why this is only ever a wish. What you actually get is in the roster.
		HE::Net::ParticipantColor color;
	};

	// Everyone's picture is stored and sent at this size. Small enough that it
	// rides along in the join handshake without chunking, large enough to stay
	// legible on a HiDPI footer row at 2x.
	static constexpr int kAvatarSize = 64;

	// The stored identity, loaded (and the client key minted) on first use.
	static const Identity& localIdentity();
	static void setLocalName(const std::string& name);
	// Replace the picture from an image file — png/jpg/bmp/tga, whatever
	// stb_image reads. It is decoded, centre-cropped square and scaled to
	// kAvatarSize here, so what is stored is already what goes on the wire.
	// Returns false with a reason the UI can show verbatim.
	static bool setLocalAvatarFromFile(const std::string& imagePath, std::string& error);
	static void clearLocalAvatar();
	// Pass an unset colour to go back to "let the host choose".
	static void setLocalColor(HE::Net::ParticipantColor color);

	// Centre-crop an RGBA8 image to a square and resample it to `size` pixels per
	// side, averaging each destination pixel's source box. Public because it is
	// the one part of the picture path with real edge cases — non-square sources,
	// upscales, single-pixel images — and the only one that can be checked
	// without touching the user's settings directory.
	static std::vector<std::uint8_t> resampleSquareRgba(const std::uint8_t* rgba,
	                                                    int width, int height, int size);

	// Pump transport → session → collaboration. Call once per frame.
	void update(std::uint64_t nowMs);

	// ── State for the UI ──
	Status             status() const { return m_status; }
	bool               active() const { return m_status == Status::Hosting ||
	                                            m_status == Status::Connecting ||
	                                            m_status == Status::Joined; }
	bool               isHost() const { return m_isHost; }
	// Fully in a session and able to see other participants. A host qualifies as
	// soon as it is listening; a client only once its snapshot has been applied,
	// since before that it does not share the scene the others are looking at.
	bool               inSession() const { return m_status == Status::Hosting ||
	                                              m_status == Status::Joined; }
	const std::string& joinCode() const { return m_joinCode; }
	const std::string& sessionId() const { return m_sessionId; }
	// The address of THIS machine on its own network (or, when joining, the
	// address we connected to).
	const std::string& localAddress() const { return m_localAddress; }
	// The address the session directory saw the registration arrive from. On any
	// NAT that is the *router's* public side, not this machine — which is why it
	// is kept apart from localAddress() instead of overwriting it: labelling a
	// WAN address "local" invites the reader to conclude this machine is
	// directly on the internet. Empty until a registration succeeds.
	const std::string& publicAddress() const { return m_publicAddress; }
	std::uint16_t      port() const { return m_port; }
	const std::string& lastError() const { return m_error; }

	// A directory call (register or lookup) is in flight. The UI shows a spinner
	// rather than an address the host does not have yet.
	bool               directoryBusy() const;
	// Human-readable directory outcome, e.g. why publishing failed.
	const std::string& directoryStatus() const { return m_directoryStatus; }
	// The directory managed to connect back to this host. False means peers on
	// other networks will not get through — port forwarding or a relay is needed.
	bool               reachable() const { return m_reachable; }
	bool               reachabilityKnown() const { return m_reachabilityKnown; }

	// Whether the router accepted an automatic port forward for this session.
	// Reported separately from reachability because the two can disagree: a
	// mapping can succeed and the host still be unreachable behind CGNAT, where
	// there is no forwardable port at any level.
	bool               portMapped() const { return m_portMapped; }
	const std::string& portMapStatus() const { return m_portMapStatus; }

	// What the user can DO about being unreachable, empty when there is nothing
	// to do. Deliberately separate from the two status strings above: those
	// report different *facts* (the router refused / the directory could not
	// reach back), but the advice for them is the same, and printing it under
	// each one made the panel say the same paragraph twice.
	const std::string& connectivityAdvice() const { return m_advice; }

	// 0..1 while a snapshot is arriving; 1 when complete.
	float snapshotProgress() const;
	bool  snapshotInProgress() const { return m_snapshotTotal > 0 &&
	                                          m_snapshotGot < m_snapshotTotal; }

	// The live roster, by reference: a Participant now carries a profile picture,
	// and the footer draws this every frame. Returning it by value copied every
	// portrait in the session sixty times a second.
	const std::vector<HE::Net::Participant>& participants() const;

	// ── Moderation (host only) ───────────────────────────────────────────────
	// Remove someone from the session. A ban also blacklists them for as long as
	// this session lives, so their next join is refused without the host being
	// asked again. Both are no-ops on a client and for our own id.
	bool kickParticipant(HE::Net::ParticipantId id);
	bool banParticipant(HE::Net::ParticipantId id);
	const std::vector<HE::Net::CollabSession::BanEntry>& bans() const;
	bool unban(const HE::Net::CollabSession::BanEntry& entry);

	// Set when the HOST removed us — the one thing a dropped link cannot tell
	// the user apart from a network failure. Empty when we left on our own or
	// were never in a session. Shown once, then cleared by the UI.
	const std::string& removalNotice() const { return m_removalNotice; }
	void clearRemovalNotice() { m_removalNotice.clear(); }

	// Short digest of the negotiated key, identical on both peers — lets two
	// users confirm out of band that they are in the same session.
	std::string sessionFingerprint() const;

	// ── Live scene deltas ──
	// Publish the transform of the entity we currently hold, when it actually
	// moved. Cheap to call every frame: unchanged values send nothing.
	void publishTransform(std::uint64_t subject,
	                      const float pos[3], const float rotEuler[3], const float scale[3],
	                      std::uint64_t nowMs);

	// Fires when a remote peer moved something; the editor writes it into the
	// world. Applying is deliberately NOT done here — CollabController must not
	// reach into the ECS.
	void onRemoteTransform(
		std::function<void(std::uint64_t, const float[3], const float[3], const float[3])> fn)
	{
		m_onRemoteTransform = std::move(fn);
	}

	// ── Component edits ──
	// Publish the full component state of the entity we hold, when it changed.
	// Cheap to call every frame: an unchanged entity sends nothing.
	void publishComponents(std::uint32_t entityHandle,
	                       const std::vector<std::uint8_t>& blob);

	// A peer edited an entity's components; the editor applies the blob.
	void onRemoteComponents(
		std::function<void(std::uint32_t, const std::vector<std::uint8_t>&)> fn)
	{
		m_onRemoteComponents = std::move(fn);
	}

	// ── Structural replication ──
	// Peers assign ECS handles independently, so a raw handle names different
	// entities on different machines. Everything network-facing therefore uses a
	// *network id*:
	//   • entities from the shared snapshot use their handle (identical on every
	//     peer, since everyone loaded the same bytes),
	//   • newly created ones use (participantId << 32 | counter), which cannot
	//     collide with a snapshot handle (those have a zero high word) nor with
	//     another participant's ids.
	std::uint64_t netIdFor(std::uint32_t entityHandle);
	std::uint32_t entityForNetId(std::uint64_t netId);   // 0 = unknown

	// The wire identity of an entity: derived from its EntityIdComponent uuid,
	// so every peer computes the same value from its own world. Mints the uuid
	// if the entity somehow lacks one. Public because lock lookups in the panels
	// must key on the SAME subject the lock was taken under.
	std::uint64_t subjectFor(std::uint32_t entityHandle) { return netIdFor(entityHandle); }
	void          forgetNetId(std::uint64_t netId);

	// Rebuild the mapping from the entities that exist right now. Called after a
	// snapshot replaced the world, where handle == network id by construction.
	void seedNetIds();

	// Publish a structural change. `blob` is the serialized subtree for a create.
	bool publishCreate(std::uint32_t entityHandle, std::uint32_t parentHandle,
	                   const std::vector<std::uint8_t>& blob);
	bool publishDestroy(std::uint32_t entityHandle);
	bool publishReparent(std::uint32_t entityHandle, std::uint32_t newParentHandle);

	// Remote structural changes, translated back into local handles. The editor
	// performs the actual ECS work — CollabController never touches the registry.
	//   onRemoteCreate(netId, parentHandle, blob) → returns the new local handle
	void onRemoteCreate(
		std::function<std::uint32_t(std::uint32_t, const std::vector<std::uint8_t>&)> fn)
	{
		m_onRemoteCreate = std::move(fn);
	}
	void onRemoteDestroy(std::function<void(std::uint32_t)> fn)
	{
		m_onRemoteDestroy = std::move(fn);
	}
	void onRemoteReparent(std::function<void(std::uint32_t, std::uint32_t)> fn)
	{
		m_onRemoteReparent = std::move(fn);
	}

	// ── Authored-asset sync ──
	// A stable subject id for an asset path, so an asset and an entity are
	// arbitrated by the same lock table.
	static std::uint64_t assetSubject(const std::string& relativePath);

	// True when this asset kind should travel over the session at all. Authored
	// data (graphs, materials, UI, scenes) yes; binary media (meshes, textures,
	// audio) no — those are large, rarely edited live, and source control's job.
	//
	// TWO gates, because the extension does not name the type: the engine writes
	// every authored asset as a `.hasset` container. isSyncableAsset() is the
	// path-only filter (it can run without opening the file, and is what keeps
	// source art like .fbx/.png out); isSyncableAssetType() is the real decision,
	// applied by the caller once it has sniffed the header. Deciding on the
	// extension alone is what silently disabled asset collaboration entirely —
	// see the comment on the list in the .cpp.
	static bool isSyncableAsset(const std::string& relativePath);
	static bool isSyncableAssetType(HE::AssetType type);

	// Publish a saved asset. Claims the lock first when nobody holds it, so
	// saving does not silently do nothing just because the user never selected
	// the asset in a way that took a lock.
	void publishAsset(const std::string& relativePath, const std::string& fullPath);
	// Announce a NEWLY created asset — same two arguments as publishAsset, and
	// called at the moment the naming dialog closes, which is when the path is
	// final. No-op outside a session, for an excluded type, or while a remote
	// create is being applied.
	void publishAssetCreate(const std::string& relativePath, const std::string& fullPath);

	// ── New assets that arrived from the session ──
	// Collected rather than announced one at a time: Save All is a single user
	// action that can produce a dozen, and a dozen notices for one keystroke is
	// what makes people stop reading them. The UI drains this.
	struct CreatedAssetNotice
	{
		std::string   path;        // project-relative
		std::string   byName;      // who made it, for the line of text
		std::uint64_t atMs = 0;
	};
	const std::vector<CreatedAssetNotice>& createdAssetNotices() const
	{ return m_createdNotices; }
	void clearCreatedAssetNotices() { m_createdNotices.clear(); }

	// ── The host's in-tray ──
	// Deleting or renaming an asset destroys or moves work that is not the
	// requester's, so it is asked for and the host answers. Collected PER ASSET
	// rather than per request: three people wanting the same file gone is one
	// decision, not three, and one Approve settles it for all of them.
	struct PendingAssetOp
	{
		HE::Net::CollabSession::AssetOp op =
			HE::Net::CollabSession::AssetOp::Delete;
		std::string path;        // project-relative, the asset in question
		std::string newPath;     // rename only
		// A folder takes everything under it with it, which is why the queue row
		// says so — approving one is a much bigger yes than approving a file.
		bool        folder = false;
		// Who asked, in the order they asked. The panel draws their profile
		// pictures — a name is a string, a face is the person you can go and
		// talk to about it.
		struct Requester { HE::Net::ParticipantId id = 0; std::uint32_t requestId = 0; };
		std::vector<Requester> requesters;
		std::uint64_t firstAskedMs = 0;   // for the "4m" age column
	};
	const std::vector<PendingAssetOp>& pendingAssetOps() const { return m_pendingOps; }
	// Answer one. Approving applies it everywhere, including here; denying tells
	// every requester and changes nothing. Both drop the row.
	void approveAssetOp(std::size_t index);
	void denyAssetOp(std::size_t index);
	// What a participant asked for, if anything — so a peer can be shown that
	// its own request is still waiting.
	bool hasPendingRequestOfOurs() const { return !m_ourPendingOps.empty(); }
	std::size_t pendingRequestsOfOurs() const { return m_ourPendingOps.size(); }
	// The clock the queue's timestamps are on. Exposed so the panel measures an
	// age against the same one that recorded it, rather than a second clock that
	// happens to be nearby.
	std::uint64_t nowMs() const { return m_lastUpdateMs; }

	// Ask for an asset to go, or — as host — make it go. FALSE means there is no
	// session and the caller should do it locally, exactly as it always did.
	// That return value is the whole interface: no caller needs to know whether
	// it is hosting, joining, or working alone.
	bool requestAssetDelete(const std::string& relPath, bool folder = false)
	{
		return requestOrPerformAssetOp(
			HE::Net::CollabSession::AssetOp::Delete, relPath, {}, folder);
	}
	bool requestAssetRename(const std::string& relPath, const std::string& newRelPath,
	                        bool folder = false)
	{
		return requestOrPerformAssetOp(
			HE::Net::CollabSession::AssetOp::Rename, relPath, newRelPath, folder);
	}
	// Creating a folder needs nobody's permission — it destroys nothing — so it
	// goes straight out. Returns false when there is no session.
	bool publishFolderCreate(const std::string& relPath)
	{
		return requestOrPerformAssetOp(
			HE::Net::CollabSession::AssetOp::Create, relPath, {}, true);
	}

	// Fires when a peer saved an asset: (relativePath, bytes). The editor writes
	// the file and reloads — CollabController does not touch the ContentManager.
	void onRemoteAsset(
		std::function<void(const std::string&, const std::vector<std::uint8_t>&)> fn)
	{
		m_onRemoteAsset = std::move(fn);
	}

	// An approved delete or rename, to be carried out here. isDelete decides
	// which; on a rename `newPath` is where it goes. Fires on every peer
	// including the host, so there is one implementation rather than two.
	void onRemoteAssetOp(
		std::function<void(HE::Net::CollabSession::AssetOp, const std::string& path,
		                   const std::string& newPath, bool folder)> fn)
	{
		m_onRemoteAssetOp = std::move(fn);
	}

	// Session key → local absolute path, or empty if it resolves to nothing that
	// belongs to this project. Installed by the editor, which owns the content
	// root and the Source tree; arbitrating a create needs to ask the disk
	// whether a name is free, and this controller has no other way to look.
	void setLocalPathResolver(std::function<std::string(const std::string&)> fn)
	{
		m_localPathForKey = std::move(fn);
	}

	// One line about the last thing that happened to an asset of OURS — a create
	// the host refused, or a name it changed. Read and cleared by the UI, same
	// as lockNotice.
	const std::string& assetNotice() const { return m_assetNotice; }
	void clearAssetNotice() { m_assetNotice.clear(); }

	// ── Locks ──
	bool requestLock(std::uint64_t subject);
	void releaseLock(std::uint64_t subject);
	const HE::Net::LockInfo* lockFor(std::uint64_t subject) const;
	bool ownsLock(std::uint64_t subject) const;

	// Keep exactly one entity locked: the one currently selected. Called every
	// frame with the selection; releases the previous subject and claims the new
	// one only when it actually changed, so this stays free to call.
	void followSelection(std::uint64_t subject);

	// ── Asset-level locking (lazy) ───────────────────────────────────────────
	// Assets lock on FIRST EDIT, not on open: reading a graph together is fine,
	// only writing needs an owner. The lock table is the same one entities use
	// (assetSubject() sets the top bit, so the two spaces stay disjoint).
	//
	// The grant is asynchronous — host round trip — so the flow is optimistic:
	// the editor keeps editing while the request is in flight, and the deny
	// callback (someone else won the race) flips the tab read-only. The race
	// window is one RTT; the replicated lock table makes the common case (held
	// long before you click) synchronous via assetLockedByOther().
	bool assetLockedByOther(const std::string& relativePath);
	bool ownsAssetLock(const std::string& relativePath);
	void requestAssetLock(const std::string& relativePath);
	void releaseAssetLock(const std::string& relativePath);

	// ── Host-confirmed edit state ────────────────────────────────────────────
	// What a panel is allowed to do with an asset RIGHT NOW.
	//
	// The replicated lock table (assetLockedByOther) answers instantly and is
	// right almost always, but it is up to one round trip stale — and that is
	// exactly the moment a tab opens and has to decide whether it is editable.
	// Opening therefore asks the HOST (CollabSession::queryLock) and the tab
	// stays Unknown, drawn but not editable, for the one frame or two that takes.
	// Without that step two people opening the same graph in the same second
	// both believe they may edit, and one of them loses their work to the deny
	// path a second later.
	enum class AssetEditState : std::uint8_t
	{
		Editable,    // no session, or the answer is in and nobody else holds it
		Unknown,     // asked the host, waiting — read-only until it answers
		HeldByOther, // someone else has it; read-only with the banner
		Owned,       // we hold the lock
	};

	// Ask the host about this asset. Idempotent per path: a second call while an
	// answer is outstanding does nothing. Called when a tab is rendered for an
	// asset we have not asked about yet.
	void           beginAssetEditSession(const std::string& relativePath);
	AssetEditState assetEditState(const std::string& relativePath);
	// Claim before mutating. False = do not apply the edit (still Unknown, or
	// someone else holds it). Safe to call every frame once owned.
	bool           beginAssetEdit(const std::string& relativePath);
	// Drop what we know about a path (its tab closed), so reopening asks again.
	void           forgetAssetEditSession(const std::string& relativePath);

	// Publish a document delta batch. False = it could not go as deltas (no
	// lock, or over the wire's size bounds) and the caller must fall back to
	// publishAsset — a coarser update, never a corrupt one.
	bool publishDocDeltas(const std::string& relativePath,
	                      const std::vector<HE::Net::CollabSession::DocDelta>& batch);

	// A peer edited a document: (relativePath, batch). The editor routes it to
	// whichever panel holds that asset — CollabController does not know panels.
	void onRemoteDocDeltas(
		std::function<void(const std::string&,
		                   const std::vector<HE::Net::CollabSession::DocDelta>&)> fn)
	{
		m_onRemoteDocDeltas = std::move(fn);
	}
	// Who holds it (nullptr = nobody) — for the read-only banner.
	const HE::Net::LockInfo* assetLockInfo(const std::string& relativePath);
	// Every asset path we hold or have requested a lock for — the caller
	// releases the ones whose editor tab has gone away.
	const std::vector<std::string>& heldAssetLocks() const { return m_heldAssetLocks; }

	// Fired when an optimistic asset-lock request lost the race — the editor
	// reloads that tab from disk, discarding the one-RTT sliver of local edits.
	void onAssetLockDenied(std::function<void(const std::string&)> fn)
	{
		m_onAssetLockDenied = std::move(fn);
	}

	// abs → project-relative, with forward slashes: the form assetSubject()
	// and the sync pipeline key on. Empty when the path is outside the project.
	static std::string projectRelativeAssetPath(const std::string& absolutePath,
	                                            const std::string& contentRoot);

	// Set when a lock request was refused, for a transient UI notice.
	const std::string& lockNotice() const { return m_lockNotice; }
	void clearLockNotice() { m_lockNotice.clear(); }

	// ── Presence ──
	void setLocalPresence(const float cameraPos[3], const float cameraRot[4],
	                      const std::vector<std::uint64_t>& selection);
	const HE::Net::PresenceState* presenceOf(HE::Net::ParticipantId id) const;
	HE::Net::ParticipantId        localParticipant() const;

private:
	// Host side of a create: the questions HorizonNet cannot answer — is the
	// name free, does this kind of asset travel, does the path stay inside the
	// project. False refuses, and fills `reason` (plus a free name when there
	// is one, so the creator can retry instead of losing the asset).
	bool acceptRemoteCreate(const HE::Net::CollabSession::AssetUpdate& a,
	                        HE::Net::CollabSession::AssetRejectReason& reason,
	                        std::string& suggestedPath);
	// Creator side: the host's answer to something we made.
	void onOwnCreateAnswered(const std::string& path, bool accepted,
	                         HE::Net::CollabSession::AssetRejectReason reason,
	                         const std::string& suggestedPath);
	void noteAssetCreated(HE::Net::ParticipantId who, const std::string& path);

	std::vector<CreatedAssetNotice> m_createdNotices;
	std::vector<PendingAssetOp>     m_pendingOps;     // host only
	// Requests WE are waiting on, so the footer can say "1 request pending" and
	// the verdict can be matched back to what it answers.
	struct OurOp { std::uint32_t requestId = 0;
	               HE::Net::CollabSession::AssetOp op =
	                   HE::Net::CollabSession::AssetOp::Delete;
	               std::string path; };
	std::vector<OurOp>              m_ourPendingOps;

	// Ask for it, or — when we are the host — simply do it. Returns false when
	// there is no session, in which case the caller acts locally as before.
	bool requestOrPerformAssetOp(HE::Net::CollabSession::AssetOp op,
	                             const std::string& relPath,
	                             const std::string& newRelPath,
	                             bool folder);
	// The absolute path of the create we are waiting on an answer for, and
	// whether we already retried it once. One retry only: a name that is taken
	// twice running means somebody is creating in a loop, and answering that
	// with another attempt would join them.
	std::string m_pendingCreateFull;
	bool        m_createRetried = false;

	void teardown();
	// Give the router and the directory back what this session took: the port
	// forward, the IPv6 pinhole, the directory entry. `blocking` decides whether
	// the calls are waited for (process exit) or detached (the user closed a
	// session and wants the UI back now). Safe to call when nothing was taken.
	void releaseNetworkResources(bool blocking);
	void wireCallbacks();
	// Copy the persisted name/picture/client key into a session config. Called
	// on every host and join, so the identity the user just edited is the one
	// that goes on the wire.
	static void applyLocalIdentity(HE::Net::CollabSession::Config& cfg);
	void pumpDirectory(std::uint64_t nowMs);
	bool beginLink(const std::string& host, std::uint16_t port,
	               const std::string& joinCode, const std::string& displayName);

	class SceneStateProvider;

	// Directory results are produced off-thread — an HTTPS round trip on the
	// frame thread would stall the editor for seconds.
	struct RegisterResult
	{
		bool        ok = false;
		std::string publicIp;
		// Only set when the directory VERIFIED it by connecting back, and it is
		// what guests are given — so it, not publicIp, is the host's address.
		std::string altAddress;
		bool        reachable = false;
		std::string token;
		std::string error;
		// Port-mapping outcome, carried back from the same worker so the UI
		// learns about both in one step.
		HE::Net::PortMapper::MappingHandle mapping;
		bool        portMapped = false;
		std::string mapStatus;
		// IPv6 firewall pinhole, opened alongside the IPv4 mapping — the two
		// serve different guests (v6-capable vs v4-only), so both are wanted.
		HE::Net::PortMapper::PinholeHandle pinhole;
		bool        pinholeOpen = false;
		// Advice the worker already knows applies (CGNAT needs a narrower one
		// than a plain mapping failure). Empty means "decide from reachability".
		std::string advice;
	};
	struct LookupResult
	{
		bool          ok = false;
		// Every address the directory offers, best first. A dual-stack host is
		// listed under both families, and only the joiner can tell which of them
		// it actually has a route for.
		std::vector<std::string> hosts;
		std::string   host;
		std::uint16_t port = 0;
		std::string   error;
	};

	std::unique_ptr<SceneStateProvider>        m_provider;
	std::unique_ptr<HE::Net::SecureTransport>  m_secure;   // owns the TcpTransport
	std::unique_ptr<HE::Net::NetSession>       m_net;
	std::unique_ptr<HE::Net::CollabSession>    m_collab;

	HorizonWorld* m_world  = nullptr;
	std::string   m_projectId;      // manifest uuid — compared on join
	std::string   m_projectLabel;   // display name — shown to a refused joiner
	Status        m_status = Status::Idle;
	bool          m_isHost = false;

	std::string   m_joinCode;
	std::string   m_sessionId;
	std::string   m_localAddress;
	// The address a GUEST is handed, which is the only one worth showing: the
	// directory publishes a verified alt address when it has one, and guests dial
	// that. Showing what the directory merely OBSERVED instead sends people
	// chasing an address nobody connects to — on a Mac the registration leaves
	// from the temporary privacy address while the pinhole sits on the stable
	// one, so the two differ by design and only one of them accepts connections.
	std::string   m_publicAddress;
	std::uint16_t m_port = 0;
	std::string   m_error;

	// Connecting has no natural end: the socket is non-blocking, so a SYN into a
	// black hole (no route to the host's address family, a firewall that drops
	// rather than refuses) leaves the join pending with nothing to report it.
	// Without a deadline the UI says "Connecting…" until the user gives up.
	static constexpr std::uint64_t kConnectTimeoutMs = 20'000;
	std::uint64_t m_connectDeadlineMs = 0;   // 0 = not connecting
	std::string   m_connectTarget;           // address being dialled, for the message

	// Directory state
	std::future<RegisterResult> m_registerFuture;
	// The router we mapped through, kept so the mapping can be removed again on
	// leave. Leaving it behind would hold a hole open in the user's firewall
	// long after the session ended.
	HE::Net::PortMapper::MappingHandle m_mapping;
	HE::Net::PortMapper::PinholeHandle m_pinhole;
	bool m_pinholeOpen = false;
	bool               m_portMapped = false;
	std::string        m_portMapStatus;
	std::string        m_advice;   // shown once, below both status lines
	std::future<LookupResult>   m_lookupFuture;
	std::string   m_directoryToken;      // needed to heartbeat / unregister
	std::string   m_directoryStatus;
	bool          m_reachable          = false;
	bool          m_reachabilityKnown  = false;
	std::uint64_t m_lastHeartbeatMs    = 0;
	// The frame clock, kept because a request arrives on the network and has no
	// clock of its own — the queue's age column reads from this.
	std::uint64_t m_lastUpdateMs       = 0;
	// Held while a lookup is in flight, so the connect can be made once the
	// address arrives.
	std::string   m_pendingJoinCode;
	std::string   m_pendingDisplayName;

	std::uint32_t m_snapshotGot   = 0;
	std::uint32_t m_snapshotTotal = 0;

	std::uint64_t m_heldSubject = 0;   // the one entity we currently hold
	std::string   m_lockNotice;
	std::string   m_assetNotice;
	std::function<std::string(const std::string&)> m_localPathForKey;
	std::function<void(HE::Net::CollabSession::AssetOp, const std::string&,
	                   const std::string&, bool)> m_onRemoteAssetOp;
	std::string   m_removalNotice;   // set when the host kicked or banned us

	// Last transform we published, so an unmoved object sends nothing. Same
	// reasoning as presence: a gizmo drag changes this every frame.
	std::uint64_t m_lastTransformSubject = 0;
	float         m_lastTransform[9] {};   // pos3 + rotEuler3 + scale3
	bool          m_hasLastTransform = false;
	std::uint64_t m_lastTransformSendMs = 0;

	std::function<void(std::uint64_t, const float[3], const float[3], const float[3])>
		m_onRemoteTransform;
	std::function<void(const std::string&, const std::vector<std::uint8_t>&)>
		m_onRemoteAsset;

	// Network id ↔ local ECS handle. The only source of truth about identity
	// across peers; local-only entities (terrain chunks, environment lights) are
	// simply absent from it.
	std::vector<std::pair<std::uint64_t, std::uint32_t>> m_netIds;
	std::vector<std::string> m_heldAssetLocks;
	std::function<void(const std::string&)> m_onAssetLockDenied;

	// What the host last told us about an asset, keyed by project-relative path.
	// Absent = never asked. This is the open-time answer, kept separate from the
	// replicated table on purpose: the table is a live view that changes under
	// us, this is "what were we told when this tab opened".
	struct AssetAnswer
	{
		bool                   pending = true;  // asked, no reply yet
		bool                   held    = false; // by someone else
		HE::Net::ParticipantId owner   = 0;
		std::string            ownerName;
	};
	std::unordered_map<std::string, AssetAnswer> m_assetAnswers;
	// subject → path, so the reply (which carries only the subject) finds its way
	// back to the path the editor keys on.
	std::unordered_map<std::uint64_t, std::string> m_assetSubjectPaths;

	std::function<void(const std::string&,
	                   const std::vector<HE::Net::CollabSession::DocDelta>&)>
		m_onRemoteDocDeltas;

	std::function<std::uint32_t(std::uint32_t, const std::vector<std::uint8_t>&)>
		m_onRemoteCreate;
	std::function<void(std::uint32_t, const std::vector<std::uint8_t>&)>
		m_onRemoteComponents;
	// Hash of the last component blob we published, so an untouched entity costs
	// nothing per frame.
	// Last blob hash sent per entity, so switching selection back and forth
	// does not read as an edit.
	std::unordered_map<std::uint32_t, std::uint64_t> m_lastComponentHashes;
	std::function<void(std::uint32_t)>                m_onRemoteDestroy;
	std::function<void(std::uint32_t, std::uint32_t)> m_onRemoteReparent;
	// Set while applying a received asset, so writing it to disk does not bounce
	// straight back out as our own change.
	bool m_applyingRemoteAsset = false;

	std::function<void()> m_onWorldReplaced;
};
