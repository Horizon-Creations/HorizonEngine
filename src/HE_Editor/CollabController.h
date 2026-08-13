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

#include "NotificationStore.h"

#include <Net/CollabSession.h>
#include <Net/LanBeacon.h>
#include <Net/PortMapper.h>
#include <Types/Enums.h>   // HE::AssetType — the .hasset header's real type

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
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

	// Where "something happened that you did not do" goes — a peer that could
	// not apply a delete, a request the host refused, a reimport somebody else's
	// lock blocked. Owned by EditorApplication and set once at startup; null in
	// tests and headless runs, which every post checks for.
	//
	// A session is exactly the kind of thing this store exists for: almost
	// everything in here is caused by SOMEBODY ELSE, and until this pointer
	// existed the only places to say so were a footer line destroyed by the next
	// click and a log file nobody has open.
	void setNotifications(HE::Ed::NotificationStore* store) { m_notes = store; }

	// ── How much of that store one PEER may fill ─────────────────────────────
	// Notices caused by another machine are rate-limited per participant (see
	// postRemoteNote). Public, and for the same reason kMinAssetMB is: the test
	// that proves the limit holds has to assert against the number the limit
	// actually uses, or the two drift and the test starts proving nothing.
	//
	// Ten per ten seconds, per peer. The number is picked against what an
	// ordinary busy session produces, which is far less than people expect:
	// saves, deltas, transforms and lock changes post NOTHING at all — the
	// remote-triggered notices are the exceptional events (a peer reimported
	// something whose bytes do not travel, a file one side refused as too large,
	// a lock we were denied, an approved delete that did not take here), and a
	// busy hour of three people editing produces a handful each. The largest
	// legitimate burst I can construct is somebody reimporting a folder of
	// meshes one after another, and ten of those still get through with the rest
	// named in the summary row. A peer looping on a bad file, meanwhile, is
	// clipped from thousands to ten — and told about, which is the half that
	// makes a limiter honest rather than a silencer.
	static constexpr std::uint64_t kRemoteNoteWindowMs = 10'000;
	static constexpr int           kRemoteNoteBudget   = 10;

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
	// What a discovered session is compared against, so the join panel can say
	// "a different project" before someone tries and is refused.
	const std::string& projectId() const { return m_projectId; }

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

	// ── Sessions on this network ─────────────────────────────────────────────
	// The directory route needs the host to be reachable FROM THE INTERNET, and
	// behind carrier NAT or a router that will not forward, it is not — two
	// people on one Wi-Fi were told to use a relay that does not exist. On one
	// network none of that is needed: hosts announce themselves, guests listen,
	// and joining is a click plus the code (which is never announced — see
	// LanBeacon.h).
	//
	// Both halves follow ONE switch, because "find sessions near me" is one
	// feature to a user even though it is two sockets to us.
	void setLanDiscoveryEnabled(bool on);
	bool lanDiscoveryEnabled() const { return m_lanEnabled; }

	// ── Bigger assets ────────────────────────────────────────────────────────
	// Whether this editor is willing to put meshes, textures, audio and fonts
	// through a session instead of leaving them to source control. The pair
	// below is what the Preferences checkbox drives; EditorConfig owns the
	// persisted value and pushes it down here.
	//
	// It is REFUSED while a session is up, and silently — the caller is the
	// editor pushing its config down every frame, so returning a failure would
	// only be logged sixty times a second. Half a session running one rule and
	// half the other is a set of peers that quietly hold different files, which
	// is the failure the lock is for. The panel greys the box out and says why;
	// this is the backstop that makes that true rather than merely displayed.
	void setSyncLargeAssets(bool on);
	// What the user has agreed to, independent of any session. This is the value
	// the checkbox reflects.
	bool syncLargeAssetsSetting() const { return m_syncLargeAssets; }
	// Why the checkbox is greyed out. Deliberately active() and not inSession():
	// a join that is still connecting has ALREADY put the answer on the wire in
	// its join request, so the window in which the setting is settled closes
	// when the connect starts, not when the snapshot lands.
	bool largeAssetSyncLocked() const { return active(); }

	// What THIS SESSION carries — the host's rule, which is the only one that
	// decides anything. Outside a session it falls back to the local setting, so
	// a caller asking "would this asset travel" gets a truthful answer either
	// way rather than a false "no" that depends on when it asked.
	bool sessionSyncsLargeAssets() const;

	// ── How big a file may travel ────────────────────────────────────────────
	// The ceiling on ONE asset transfer, in megabytes, and it is per MACHINE
	// rather than per session: each side refuses what it will not hold, so two
	// peers may disagree and the lower of the two is what actually gets through.
	// That is also why this may be changed while a session is up when the
	// large-asset switch above may not — see CollabSession::setMaxAssetBytes.
	//
	// The bounds are here, and public, so the panel's slider and this clamp
	// cannot drift apart: a UI that offered 2 GB while this quietly capped at 512
	// would leave a user certain they had raised a limit that had not moved.
	//
	// The maximum is not a round number picked for looks. The ceiling is what a
	// peer can make this process allocate — the receiving side reserves the
	// ANNOUNCED size before a byte arrives — and during one send the same file is
	// resident about three times over: the sender's copy, the frames queued in
	// the transport's out-buffer, and the receiver's assembly. 512 MiB is already
	// a gigabyte and a half of that, which is as far as this is willing to go on
	// somebody else's say-so. The minimum is 1 MiB because below it ordinary
	// authored assets — a scene, a widget with embedded art — stop travelling,
	// and a session where saves silently do not arrive is worse than no ceiling
	// control at all.
	static constexpr int kMinAssetMB = 1;
	static constexpr int kMaxAssetMB = 512;
	void setMaxAssetMB(int mb);
	int  maxAssetMB() const { return m_maxAssetMB; }

	// The one answer to "does an asset of this kind travel". Everything that
	// used to ask isSyncableAssetType asks this instead: the static predicate is
	// still the type rule, and this is that rule OR the session's decision to
	// carry everything. Combined here rather than at each call site because
	// there were four of them and they must not be able to disagree.
	bool assetTypeTravels(HE::AssetType type) const;

	// ── The joiner was refused for not having agreed ─────────────────────────
	// Set when a host turned us away with JoinRejectReason::LargeAssetsRequired.
	// It is not an error to report and forget: the user can say yes, and the
	// whole reason that reason exists on the wire is so they get asked. The
	// panel raises a dialog on this and calls one of the two below.
	bool largeAssetsPrompt() const { return m_largeAssetsPrompt; }
	void clearLargeAssetsPrompt() { m_largeAssetsPrompt = false; }
	// Agree, and dial the same host again. The setting must be turned on (and
	// persisted) by the caller first — this controller does not own the config
	// file. False when there is nothing remembered to retry.
	bool retryJoinWithLargeAssets();
	// What the browser currently hears. Empty while disabled, and empty for the
	// first couple of seconds after enabling — hosts speak on a timer.
	const std::vector<HE::Net::LanBeacon::Browser::Session>& lanSessions() const;
	// True when the local network is unavailable to this process at all (on
	// macOS: the Local Network permission). Discovery cannot work then, and an
	// empty list would otherwise be indistinguishable from "nobody is hosting".
	bool lanBlocked() const { return m_lanBlocked; }

	// Enough to tell the two sides of "it does not work" apart WITHOUT a packet
	// capture: announcements we got out, announcements the system refused, and
	// beacons we heard. Nothing sent = this machine is muted (permission,
	// firewall); sent but nothing heard = we speak and they do not reach us, or
	// nobody is there. Guessing between those cost a whole evening once.
	struct LanStats { std::uint32_t sent = 0, failed = 0, heard = 0; };
	LanStats lanStats() const
	{
		return { m_lanAnnouncer.sentCount(), m_lanAnnouncer.failedCount(),
		         m_lanBrowser.heardCount() };
	}

	// Is this session id one we can hear announcing itself here? Returns its
	// announced endpoint, or null.
	//
	// A free-standing rule rather than a few lines inside joinBySessionId,
	// because it decides something worth pinning down: a session we can hear
	// on this network is reached by its LOCAL address, never by the public one
	// the directory would hand back. Most routers refuse to let a machine
	// reach its own network through its public address, so the directory route
	// can fail between two people in the same room — and blame the port
	// forward while doing it.
	static const HE::Net::LanBeacon::Browser::Session* lanEndpointFor(
		const std::vector<HE::Net::LanBeacon::Browser::Session>& sessions,
		const std::string& sessionId);
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

	// ── Test seams ──
	// Deliver a refusal as though it had arrived from `refuser`, and ask whether
	// a create is still outstanding. Both exist so the "a peer's refusal is not a
	// verdict" rule can be tested for what it GUARDS rather than only for the
	// happy path — the alternative is a third editor in the test just to produce
	// one frame.
	void debugInjectAssetRefused(HE::Net::ParticipantId refuser, const std::string& relPath,
	                             std::uint32_t bytes, std::uint32_t theirLimit, bool wasCreate)
	{ applyAssetRefusedByPeer(refuser, relPath, bytes, theirLimit, wasCreate); }
	bool debugHasPendingCreate(const std::string& relPath) const
	{ return m_pendingCreates.find(relPath) != m_pendingCreates.end(); }
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
	//
	// isSyncableAssetType is the TYPE rule and nothing else, which is why it can
	// stay static. It is no longer the whole answer: a host may open a session
	// that carries the big assets too, and that is a property of the session,
	// not of the type. Call assetTypeTravels() unless you specifically want the
	// type rule on its own.
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

	// ── Writing a new asset through ContentManager::saveAsset ────────────────
	// Hold one of these across the save when the file being written is NEW and
	// you are going to call publishAssetCreate for it afterwards. The save hook
	// would otherwise publish the same bytes as an ordinary update first — see
	// publishAsset for what that costs. Callers that write the file themselves
	// (the content browser uses HAsset::Writer directly) never go through the
	// hook and do not need this.
	class CreatingAsset
	{
	public:
		CreatingAsset(CollabController* c, std::string relativePath)
			: m_c(c)
		{
			if (m_c) m_c->m_creatingAsset = std::move(relativePath);
		}
		~CreatingAsset() { if (m_c) m_c->m_creatingAsset.clear(); }

		CreatingAsset(const CreatingAsset&)            = delete;
		CreatingAsset& operator=(const CreatingAsset&) = delete;

	private:
		CollabController* m_c = nullptr;
	};

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
		// The user action this row came out of — see requestAssetOp's `batch`.
		// Twenty rows sharing a key are ONE thing the host decides, drawn as one
		// collapsible row instead of twenty to click through.
		//
		// Set by whichever request CREATED the row and never overwritten
		// afterwards, except that a batched ask adopts a row that has no batch
		// yet: the alternative is a file that silently drops out of the bundle
		// it was selected in, and then sits in the tray on its own after the
		// bundle has been answered. Two batches wanting the same file stay one
		// row (that is the per-asset coalescing) and it belongs to the first.
		HE::Net::ParticipantId batchOwner = 0;
		std::uint32_t          batchId    = 0;   // 0 = asked for on its own
		bool inBatch() const { return batchId != 0; }
	};
	const std::vector<PendingAssetOp>& pendingAssetOps() const { return m_pendingOps; }
	// Answer one. Approving applies it everywhere, including here; denying tells
	// every requester and changes nothing. Both drop the row.
	void approveAssetOp(std::size_t index);
	void denyAssetOp(std::size_t index);
	// Answer a whole bundle in one pass. Deliberately keyed on (owner, batch)
	// rather than taking a list of indices: approving a row erases it, so every
	// index behind it shifts, and a caller looping over indices would answer the
	// wrong rows from the second one on. Everything matching is copied out and
	// removed FIRST, then answered — so nothing being answered can be moving.
	// Returns how many rows it settled.
	std::size_t approveAssetOpBatch(HE::Net::ParticipantId owner, std::uint32_t batch);
	std::size_t denyAssetOpBatch(HE::Net::ParticipantId owner, std::uint32_t batch);
	// What a participant asked for, if anything — so a peer can be shown that
	// its own request is still waiting.
	bool hasPendingRequestOfOurs() const { return !m_ourPendingOps.empty(); }
	std::size_t pendingRequestsOfOurs() const { return m_ourPendingOps.size(); }
	// The clock the queue's timestamps are on. Exposed so the panel measures an
	// age against the same one that recorded it, rather than a second clock that
	// happens to be nearby.
	std::uint64_t nowMs() const { return m_lastUpdateMs; }

	// ── "May I edit this?" ───────────────────────────────────────────────────
	// The one request the HOST does not answer. Deleting and renaming destroy
	// work, and the host decides those; asking to edit interrupts work, and the
	// person being interrupted decides that. The host only knows who is holding
	// it and passes the question along.
	//
	// Ask whoever holds `relPath` to hand it over. False = no session, we hold
	// it already, or we have an unanswered ask for it out. Nobody holding it is
	// NOT a failure: the lock is claimed on the spot and this returns true.
	bool requestAssetEdit(const std::string& relPath);
	// True while our own ask for this asset is unanswered, so the banner can say
	// "asked" rather than offering the button again.
	bool hasAskedToEdit(const std::string& relPath) const;

	// The other side: somebody wants what we are holding. Same shape as the
	// host's in-tray and shown the same way — a row that waits, not a dialog
	// that interrupts. Unlike that queue this one reaches ANYONE: holding a lock
	// is not a role.
	struct EditRequest
	{
		HE::Net::ParticipantId id = 0;         // who is asking
		std::uint32_t          requestId = 0;
		std::string            path;           // project-relative
		std::uint64_t          askedMs = 0;
	};
	const std::vector<EditRequest>& pendingEditRequests() const
	{ return m_editRequests; }
	// Yes hands the lock over in one step (the net layer releases and grants
	// together, so there is no window for a third peer to take it); no leaves
	// everything as it is. Both drop the row.
	void answerEditRequest(std::size_t index, bool allowed);

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

	// ── One user action, one decision ────────────────────────────────────────
	// Wrap a LOOP of the calls above and everything it asks for arrives at the
	// host as one bundle: one row saying "Delete 20 assets", openable into the
	// files, with Approve all / Deny all as well as per-file buttons. Without it
	// a multi-select delete of twenty put twenty unrelated rows in front of the
	// host — the same decision, twenty clicks, and no way to see that they came
	// from one keystroke.
	//
	//     collab.beginAssetOpBatch();
	//     for (const std::string& rel : selection) collab.requestAssetDelete(rel);
	//     collab.endAssetOpBatch();
	//
	// Safe to call with no session (it does nothing), safe to leave a batch open
	// across frames, and every existing single-asset call site keeps working
	// unchanged — outside a batch the id is 0, which means "on its own".
	// Batches do not nest: a second begin starts a new one.
	std::uint32_t beginAssetOpBatch();
	void          endAssetOpBatch() { m_openBatch = 0; }
	// The batch currently open, or 0. For a caller that wants to know whether it
	// is already inside one before opening its own.
	std::uint32_t openAssetOpBatch() const { return m_openBatch; }

	// ── Rewriting an asset in place (reimport) ───────────────────────────────
	// Reimport re-reads an asset from the source file it remembers and writes it
	// back over itself, keeping its uuid. Two things follow from that in a
	// session, and both used to be missing.
	//
	// May we write to this asset RIGHT NOW? A non-interactive answer for a
	// background operation — no dialog, no waiting for a round trip, no lock
	// claimed. True when there is no session, when the asset is free, or when we
	// already hold it. This is a QUERY: it posts nothing, so a menu item can ask
	// every frame to decide whether to grey itself out.
	bool assetWritableNow(const std::string& relPath);

	// ── The claim a background write holds while it writes ───────────────────
	// What beginBackgroundWrite hands back. Asking whether an asset is free and
	// then writing it are two moments, and between them a peer can take the lock
	// — the time-of-check/time-of-use gap the gate existed to close and did not,
	// because it only ever asked. This object is the claim itself: it is taken
	// before the write and given back when the object dies, so the release
	// cannot be forgotten by an early return, an exception, or a menu handler
	// that grew a branch six months from now.
	//
	// A default-constructed lease permits NOTHING. That is the safe direction to
	// be wrong in: the only way to obtain a lease that permits a write is to ask
	// a controller for one, and a controller never permits a path somebody else
	// is holding. (The "there is no session at all" case still has to succeed —
	// see the two-argument beginBackgroundWrite below, which is the null-safe
	// door into this and the one the content browser uses.)
	//
	// Keep it alive for as long as the write lasts, and not one moment longer
	// than the controller: it holds a bare pointer back, which is safe for the
	// stack local inside a menu handler it is meant to be and nothing else. A
	// lease that dies at the end of the expression that created it —
	// `if (!collab->beginBackgroundWrite(p))` — releases immediately and leaves
	// exactly the gap described above; it is still permitted to compile so the
	// callers can be migrated one at a time, but it is not the shape to write.
	class AssetWriteLease
	{
	public:
		AssetWriteLease() = default;
		~AssetWriteLease() { release(); }

		// Movable so it can be returned and stored; NOT copyable, because two
		// copies would each release on destruction and the second release would
		// drop a lock that by then may belong to a tab the user has open.
		AssetWriteLease(AssetWriteLease&& other) noexcept { *this = std::move(other); }
		AssetWriteLease& operator=(AssetWriteLease&& other) noexcept;
		AssetWriteLease(const AssetWriteLease&)            = delete;
		AssetWriteLease& operator=(const AssetWriteLease&) = delete;

		// May the write go ahead? The whole answer for the caller.
		bool allowed() const { return m_allowed; }
		explicit operator bool() const { return m_allowed; }

		// Did THIS lease claim the lock — i.e. will it give something back? False
		// for the two cases where there is nothing to give back: no session, and
		// "we were already holding it before this write started". The second is
		// the one that matters: the asset is open in a tab here, the user still
		// wants it when the reimport is over, and releasing it because a
		// background write finished would take it out from under them.
		bool holdsLock() const { return m_owner != nullptr; }

		// True on a CLIENT that has just asked for the lock: the grant is a host
		// round trip away and cannot arrive while this call stack is running, so
		// the write proceeds on a claim that is in flight rather than settled.
		// See beginBackgroundWrite's implementation for why waiting for it is not
		// an option, and what this does buy.
		bool pending() const { return m_pending; }

		// Give the claim back early. Idempotent, and a spent lease permits
		// nothing — releasing and then writing is the misuse this exists to
		// prevent, so it must not still answer "allowed" afterwards.
		void release();

	private:
		friend class CollabController;
		CollabController* m_owner   = nullptr;   // set only when we claimed
		std::string       m_path;
		bool              m_allowed = false;
		bool              m_pending = false;
	};

	// The gate the reimport itself goes through, BEFORE the local write. A lease
	// that answers false means somebody else is holding the asset and the write
	// must not happen: rewriting it here would fork the file locally and the
	// session would never see the divergence, because the bytes of an imported
	// mesh do not travel. The user is told who is holding it (a Problem in the
	// notification store) — the caller only has to obey the answer.
	//
	// When it answers true the lock is CLAIMED, and stays claimed until the
	// returned lease goes out of scope.
	[[nodiscard]] AssetWriteLease beginBackgroundWrite(const std::string& relPath);

	// The same gate for a caller that may not have a controller or a session key
	// at all — the content browser draws in contexts with no editor behind it
	// (tests, tooling), and there `ctx.collab` is null and the key is empty.
	// Both mean "nothing to arbitrate", which must PERMIT the write: a lease
	// built by hand for that case would default to refusing and silently kill
	// reimport everywhere outside a session. One door, so that cannot be got
	// wrong at the call site.
	[[nodiscard]] static AssetWriteLease beginBackgroundWrite(CollabController* collab,
	                                                          const std::string& relPath);

	// After the reimport wrote the file. What this does depends on the asset:
	//  · a kind that TRAVELS (see isSyncableAssetType) is re-transmitted whole,
	//    exactly like a save — peers get the new bytes and are done;
	//  · a kind that does not (mesh, texture, audio, font — which is most of
	//    what reimport touches) is NOT widened into the sync set just because it
	//    changed. Those are large, and source control carries them by design.
	//    Peers are told it happened instead, so they know to pull it, rather
	//    than keeping the old bytes for ever with nothing to say so.
	void publishReimport(const std::string& relPath, const std::string& fullPath);

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

	// A lock request that was refused used to be written to a `lockNotice`
	// string here that NOTHING in the editor ever read — a channel with a writer
	// and no reader, which is the same as silence. It goes to the notification
	// store now, where the rest of "somebody else did something to you" lives.

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
	// A file that did not travel because of the size ceiling, from whichever end
	// noticed. It is THIS machine's ceiling either way — when we send, ours
	// refused our own file; when we receive, ours refused a file whose sender was
	// evidently willing to send it, which means their limit is the higher of the
	// two. What changes with `sending` is only which of those two sentences is
	// true, and both of them have to name the file, its size, whose limit stopped
	// it and where that limit is changed. Anything less leaves the reader with a
	// file that simply did not arrive.
	void noteAssetTooLarge(const std::string& relPath, std::size_t bytes,
	                       bool sending, const std::string& who = {},
	                       HE::Net::ParticipantId from = HE::Net::kInvalidParticipant);
	// The third case, and the only one where the machine being told is not the
	// machine that can fix it: THEY refused OUR file, because their ceiling is
	// lower than ours. Kept apart from the two above rather than folded into the
	// `sending` bool, because the advice is the opposite one — raising the limit
	// here would change nothing, the number to quote is theirs and comes off the
	// wire, and the reader has to be told the file is not on their machine so they
	// go and put it through source control instead of assuming it landed.
	//
	// `wasCreate` decides one clause and it is not cosmetic: after a refused save
	// they still have the older file, and after a refused create they have no such
	// asset at all — "they still have the old one" would be a plain untruth.
	// What a refusal carried back from the far end changes here — the pending
	// create it may settle, and the notice. Split out of the callback so a test
	// can deliver one without standing up a third machine.
	void applyAssetRefusedByPeer(HE::Net::ParticipantId who, const std::string& relPath,
	                             std::uint32_t bytes, std::uint32_t theirLimit,
	                             bool wasCreate);
	void noteAssetRefusedByPeer(HE::Net::ParticipantId who, const std::string& relPath,
	                            std::size_t bytes, std::size_t theirLimitBytes,
	                            bool wasCreate);

	// One post, with the null check in ONE place: the store is absent in tests
	// and in any headless run, and forty call sites each remembering to ask
	// would be forty chances to forget.
	void postNote(HE::Ed::NoteLevel level, std::string text,
	              std::string detail = {}, const std::string& relPath = {});

	// ── A notice ANOTHER MACHINE caused ──────────────────────────────────────
	// Same post, through the rate limit below. Everything that reaches the store
	// because of something a peer did goes through here instead of postNote, and
	// nothing else does: a peer — hostile, or merely looping on a file it cannot
	// read — otherwise decides how much of this editor's notification list is
	// theirs. The store already collapses CONSECUTIVE identical rows, which
	// bounds a repeated message and nothing else: two messages posted
	// alternately never collapse at all, and 200 rows of them push everything
	// the user was actually looking at out of the list.
	//
	// The limit lives here rather than in the store because the store also
	// carries notices the user caused themselves — a reimport they blocked, a
	// save of theirs that was too large — and throttling those would swallow the
	// answer somebody is standing there waiting for. `from` is what separates
	// the two: our own id (or none) posts unthrottled.
	void postRemoteNote(HE::Net::ParticipantId from, HE::Ed::NoteLevel level,
	                    std::string text, std::string detail = {},
	                    const std::string& relPath = {});
	// Has `from` any budget left in the current window? Consumes one when it
	// has. Thread-safe on its own account — see m_noteBudgetMutex.
	bool allowRemoteNote(HE::Net::ParticipantId rawFrom);
	// The bucket an id is charged against. Anything the roster does not know is
	// folded into one shared bucket, because two of the feeding paths take the
	// actor from the payload rather than from the connection — see the definition.
	HE::Net::ParticipantId noteBudgetKey(HE::Net::ParticipantId from) const;
	// True only for the first drop of a peer's window, so the summary below is
	// posted once per burst rather than once per dropped message.
	bool claimRemoteNoteSummary(HE::Net::ParticipantId rawFrom);
	// The one thing a limiter must never do is go quiet. This is the row that
	// says a peer is producing more than can be shown — at most one per peer per
	// window, because the store's collapse only reaches the row directly behind
	// it and two peers at once would otherwise alternate forever.
	void noteRemoteNotesDropped(HE::Net::ParticipantId from);
	// Settle one row, the row itself rather than an index into a vector that the
	// settling mutates. Shared by the single-row and the whole-bundle paths so
	// the two cannot answer differently — approving a rename onto a name that is
	// taken on the host's disk, in particular, turns into a denial in here and
	// must do so wherever it was approved from.
	void approveOne(const PendingAssetOp& op);
	void denyOne(const PendingAssetOp& op);
	// After an approved op was carried out here: is the disk in the state the
	// session just agreed on? Posts a Problem when it is not. See the call site
	// for why the disk is asked rather than the apply handler's error codes.
	// `by` is who the session says performed it, so the notice this may post is
	// attributed to the machine that caused it and rate-limited with the rest of
	// their traffic — an apply broadcast in a loop would otherwise be a way to
	// fill the list one failed verification at a time.
	void verifyAppliedAssetOp(HE::Net::CollabSession::AssetOp op,
	                          const std::string& relPath,
	                          const std::string& newRelPath,
	                          HE::Net::ParticipantId by);
	// Somebody else reimported an asset whose bytes do not travel.
	void noteRemoteReimport(HE::Net::ParticipantId by, const std::string& relPath);

	HE::Ed::NotificationStore*      m_notes = nullptr;
	std::vector<CreatedAssetNotice> m_createdNotices;
	std::vector<PendingAssetOp>     m_pendingOps;     // host only
	// The batch ids WE mint, and the one currently open. Per-controller, so two
	// clients hand out the same numbers — which is why the host keys a bundle on
	// (requester, batch) and never on the id alone.
	std::uint32_t                   m_nextBatchId = 1;
	std::uint32_t                   m_openBatch   = 0;
	// Requests WE are waiting on, so the footer can say "1 request pending" and
	// the verdict can be matched back to what it answers.
	struct OurOp { std::uint32_t requestId = 0;
	               HE::Net::CollabSession::AssetOp op =
	                   HE::Net::CollabSession::AssetOp::Delete;
	               std::string path; };
	std::vector<OurOp>              m_ourPendingOps;
	// Asks pointed at us because we hold the asset. Anyone can accumulate these,
	// host or not.
	std::vector<EditRequest>        m_editRequests;

	// Ask for it, or — when we are the host — simply do it. Returns false when
	// there is no session, in which case the caller acts locally as before.
	bool requestOrPerformAssetOp(HE::Net::CollabSession::AssetOp op,
	                             const std::string& relPath,
	                             const std::string& newRelPath,
	                             bool folder);
	// Creates awaiting a verdict, BY KEY. Not one slot: more than one can be in
	// flight — a C++ class publishes its header and its source in the same frame
	// — and the answers arrive one at a time. A single slot meant the first
	// verdict was applied to the last file published, which renamed and
	// republished the wrong one.
	//
	// `retried` is per create, not global: one retry each. A name taken twice
	// running means somebody is creating in a loop, and another attempt would
	// join them.
	struct PendingCreate { std::string fullPath; bool retried = false; };

	// A peer's create for a path we are ALSO creating, held back until our own
	// create has been answered.
	//
	// Two people naming a new asset the same thing in the same second: the host
	// picks one, and the loser is told "NameTaken" and moves their file to a
	// free name. But the winner's bytes are broadcast independently of that
	// answer, and applying them writes THAT path — truncating the loser's file
	// before it has been moved out of the way. The loser then renamed the
	// winner's bytes to the suggested name and their own work was simply gone.
	struct DeferredCreate
	{
		std::string               path;
		std::vector<std::uint8_t> bytes;
		HE::Net::ParticipantId    who = 0;
	};
	std::vector<DeferredCreate> m_deferredCreates;
	// Apply (and clear) anything held back for this path. Called once our own
	// create for it has been settled, whichever way it went.
	void flushDeferredCreate(const std::string& path);
	std::unordered_map<std::string, PendingCreate> m_pendingCreates;

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
	// A machine on our own segment either answers or is being blocked; waiting
	// the full twenty seconds for that verdict only delays a message the user
	// needs. The address came from a beacon we heard seconds ago, so "it might
	// still be routing" is not a live possibility here.
	static constexpr std::uint64_t kLanConnectTimeoutMs = 6'000;
	std::uint64_t m_connectDeadlineMs = 0;   // 0 = not connecting
	std::string   m_connectTarget;           // address being dialled, for the message
	// Whether that address came from a LAN announcement. It changes what a
	// silent failure MEANS: for a directory address, an unforwarded port is the
	// likely cause; for a machine that is announcing itself two metres away,
	// port forwarding is irrelevant and the host's own firewall is the answer.
	// Naming the wrong cause sends people to the wrong settings page.
	bool          m_connectIsLan = false;

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

	// ── Bigger assets ──
	// The persisted user setting, pushed down by the editor. Copied into the
	// session Config at startHosting/beginLink and not read again for the rest
	// of the session: from then on the SESSION's answer is what counts, and on a
	// client the two deliberately differ (the host decides).
	bool          m_syncLargeAssets = false;
	// The transfer ceiling in megabytes, clamped on the way in. Unlike the flag
	// above this IS read again mid-session — setMaxAssetMB pushes it straight
	// into the live session, because it binds nothing but this machine.
	int           m_maxAssetMB = 64;
	// A host turned us away because we had not agreed. Held until the panel has
	// asked the user, because it is a question and not a failure.
	bool          m_largeAssetsPrompt = false;
	// Enough to dial the same host again after the user says yes. Kept as its
	// own copy rather than reused from m_localAddress/m_joinCode: teardown()
	// runs at the top of every join and clears those, so a retry that read them
	// afterwards would connect to nothing.
	struct LastJoin {
		std::string   host;
		std::uint16_t port = 0;
		std::string   joinCode;
		std::string   displayName;
		bool          valid = false;
	};
	LastJoin      m_lastJoin;

	// ── LAN discovery ──
	// The announcer only exists while hosting; the browser runs whenever
	// discovery is on and we are not in a session — that is the only time its
	// list is of any use, and a socket nobody reads is one nobody has to think
	// about.
	HE::Net::LanBeacon::Announcer m_lanAnnouncer;
	HE::Net::LanBeacon::Browser   m_lanBrowser;
	bool          m_lanEnabled = true;    // user setting; persisted by the editor
	bool          m_lanBlocked = false;   // the local network refused us outright
	// This editor run, so the two copies of one beacon (multicast + broadcast)
	// collapse to one row and a restarted session is a new one.
	std::uint64_t m_lanInstance = 0;
	void updateLanDiscovery(std::uint64_t nowMs);

	std::uint32_t m_snapshotGot   = 0;
	std::uint32_t m_snapshotTotal = 0;

	std::uint64_t m_heldSubject = 0;   // the one entity we currently hold
	std::string   m_assetNotice;
	// One counter per document, used from both sides. As holder we increment it
	// on every publish — deltas and whole files alike, since they describe the
	// same document and have to be ordered against each other. As receiver we
	// remember the highest we have applied and refuse anything at or below it.
	//
	// One map, not two: when a lock changes hands the new holder has been
	// RECEIVING all along, so its number is already at least the old holder's
	// and its first publish continues the sequence instead of restarting it —
	// which would make every peer discard the new holder's work as stale.
	std::unordered_map<std::string, std::uint32_t> m_docRevision;
	// True when this frame is newer than everything applied for that document,
	// and records it. False means drop it — which is the ordinary fate of a
	// whole file that arrived behind newer deltas.
	bool acceptRevision(const std::string& relativePath, std::uint32_t revision);
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
	// Asset locks we asked for and no longer want, whose grant has not arrived
	// yet. Only a client can be in this state, and only because releasing a lock
	// it does not hold yet sends nothing: the request is already at the host, so
	// the grant comes back regardless and lands on a machine that has forgotten
	// it ever asked. Left alone that is a lock held for the rest of the session
	// by nobody — the asset goes read-only for every other peer and no tab
	// exists here to release it. Whatever is in here is released the moment it
	// is granted.
	//
	// Background writes are what made this reachable: a reimport claims, writes
	// and gives the lock back inside one frame, which is always faster than the
	// round trip. A tab does it too, if it is closed quickly enough.
	std::vector<std::string> m_pendingLockReleases;
	// Give back the claim a lease took, from the lease's destructor.
	void endBackgroundWrite(const std::string& relPath);
	// Our background write's claim lost the race after the file had already been
	// rewritten here. The one honest thing left to say — see the call site.
	void noteBackgroundWriteRaced(const std::string& relPath, std::uint64_t subject);
	std::function<void(const std::string&)> m_onAssetLockDenied;

	// ── The per-peer notification budget ─────────────────────────────────────
	// One window per participant, so a peer flooding the list cannot spend
	// anybody else's budget — the failure that would make this worse than no
	// limit at all, because the one machine with something urgent to say would
	// be the one silenced by the noisy one.
	struct RemoteNoteBudget
	{
		std::uint64_t windowStartMs = 0;
		int           posted        = 0;   // shown so far in this window
		// Whether the "…is sending more messages than this list can show" row has
		// already gone out for THIS window.
		// One summary per peer per window, not one per dropped message: the store
		// collapses only against its LAST row, so two peers over budget at once
		// produce alternating texts that never collapse and the summary rebuilds
		// the exact flood it is supposed to replace — with rows that carry no
		// information at all. Bounded here instead, at the source.
		bool          summarised    = false;
	};
	std::unordered_map<HE::Net::ParticipantId, RemoteNoteBudget> m_noteBudgets;
	// The one path whose save hook must not publish an update — see CreatingAsset.
	// A single path rather than a set: this is held across one synchronous
	// saveAsset call on the frame thread, never nested.
	std::string m_creatingAsset;
	// Today every one of these callbacks runs on the frame thread: HorizonNet is
	// poll-driven end to end (CollabController::update → SecureTransport::update
	// → NetSession::pump → CollabSession::update), and there is not a single
	// thread in the whole stack. The lock is here anyway because the thing this
	// sits in front of — NotificationStore::post — promises to be callable from
	// ANY thread, and the editor already posts to it from workers. A limiter
	// that quietly narrowed that promise would turn the first background poster
	// into a data race on this map rather than an obvious compile-time change.
	std::mutex m_noteBudgetMutex;

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
