#pragma once

// ─── Layer 3a — the object that owns a gameplay session ──────────────────────
// Until this existed, GameReplication was a fully tested library that nothing
// ever instantiated outside its own test: no play-mode multiplayer, no start
// path in a packaged game, no concept of a PLAYER at all. This is the missing
// middle — the counterpart of CollabController, but in HE_Scene so the editor
// and the packaged game use the SAME one rather than two that drift.
//
//     UdpTransport → SecureTransport → NetSession → GameReplication
//                                                 + SpawnReplicator
//                                                 + PlayerRoster
//                                                 + AntiCheatHost
//
// Everything is poll-driven: update() once per frame or nothing happens.
//
// WHY THE TRANSPORT CAN BE HANDED IN. host()/joinDirect() build the chain
// above, which needs sockets. hostOn()/joinOn() take an already-built
// ITransport instead, which is what lets a test run two complete sessions
// against each other through LossyTransport on a simulated clock — no ports, no
// timing, no flakiness — and what will let play-in-editor run both ends in one
// process over a loopback pair later (plan §5.6).
//
// WHAT THIS DELIBERATELY DOES NOT DO YET (plan step 5 and after): it binds no
// script frontend, spawns no player controller, and possesses no character.
// Events are QUEUED here (takeEvent) rather than dispatched, for the reason the
// HorizonCode debugger parks Continue/Step at the frame start: delivering to a
// script from inside a message handler runs game code in the middle of
// draining a socket.

#include "HorizonScene/AntiCheat/AntiCheatHost.h"
#include "HorizonScene/AntiCheat/AntiCheatService.h"
#include "HorizonScene/GameReplication.h"
#include "HorizonScene/Net/PlayerRoster.h"
#include "HorizonScene/Net/SpawnReplicator.h"

#include <Net/ITransport.h>
#include <Net/LanBeacon.h>
#include <Net/NetSession.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class HorizonWorld;
namespace HE { struct ProjectAntiCheatSettings; }
namespace HE::Net { class SecureTransport; class UdpTransport; }

// No HE_API — HorizonScene exports every symbol (see GameReplication.h).
class NetGameSession
{
public:
	enum class Status : std::uint8_t
	{
		Idle,        // nothing running
		Hosting,     // listening; peers may join
		Connecting,  // client: link up, join not yet complete
		Joined,      // client: baseline applied, fully in the session
		Failed,      // see lastError()
	};

	// The numbers OnDisconnected carries (plan §7.4). They are wire values the
	// moment a reject travels, so they are frozen.
	enum class DisconnectReason : std::uint8_t
	{
		Leave           = 0,
		Timeout         = 1,
		Kicked          = 2,
		Rejected        = 3,
		VersionMismatch = 4,
		WrongProject    = 5,
	};

	// Why a host refused a Hello. Travels in kMsgReject, so also frozen.
	enum class RejectReason : std::uint8_t
	{
		None            = 0,
		VersionMismatch = 1,
		WrongProject    = 2,
		SessionFull     = 3,
		Banned          = 4,
		Malformed       = 5,
	};

	struct HostOptions
	{
		std::uint16_t port        = 0;        // 0 = OS-assigned
		std::string   displayName = "Host";
		bool          announceLan = true;
		std::uint32_t maxPlayers  = 8;        // counting the host

		// Compared against every joiner's, exactly as CollabController does it:
		// everything a session sends is addressed by uuids that only mean
		// something inside ONE project. Empty on both sides skips the check,
		// which is what a test and a bare headless server want.
		std::string projectId;
		std::string projectLabel;             // sent back with a refusal, so the
		                                      // joiner learns WHICH project to open
		std::string scenePath;                // told to joiners; step 5 loads it

		// Anti-cheat, per session (plan §5.1). nullptr = OFF, which is
		// byte-for-byte the behaviour that existed before the service did.
		const HE::ProjectAntiCheatSettings* antiCheat = nullptr;
		// Play-in-editor: reports and events, but nobody is kicked from a
		// session that is the author's own window (anti-cheat plan §6.2.6).
		bool preview = false;

		GameReplication::Config replication;
	};

	struct JoinOptions
	{
		std::string displayName = "Player";
		std::string joinCode;                 // the host's join secret
		std::string projectId;                // compared by the host
		GameReplication::Config replication;
	};

	// What happened, for the frontends step 5 will dispatch to. A queue and not
	// a callback: a handler firing from inside pump() would run game code in the
	// middle of draining a socket (the rule Memory `hc-breakpoints-latent-resume`
	// states for the debugger, and it is the same rule).
	struct Event
	{
		enum class Kind : std::uint8_t
		{
			SessionStarted,   // host, after host()
			SessionEnded,     // both
			PlayerJoined,     // host, after the joiner's baseline
			PlayerLeft,       // host
			Connected,        // client, after the baseline (Status::Joined)
			Disconnected,     // client; `reason` is a DisconnectReason
		};
		Kind                    kind   = Kind::SessionEnded;
		HE::Net::Game::PlayerId player = HE::Net::Game::kNoPlayer;
		int                     reason = 0;
		std::string             name;    // the player's display name, where there is one
	};

	NetGameSession();
	~NetGameSession();
	NetGameSession(const NetGameSession&)            = delete;
	NetGameSession& operator=(const NetGameSession&) = delete;

	// The world this session replicates. Must outlive the session.
	void setWorld(HorizonWorld* world) { m_world = world; }
	HorizonWorld* world() const { return m_world; }

	// ── Lifecycle ────────────────────────────────────────────────────────────
	// Sockets: open a port, secure it, start the session. The join code is
	// generated here and read back with joinCode().
	bool host(const HostOptions& options);
	// Sockets: connect, authenticate with the code, send the Hello.
	bool joinDirect(const std::string& address, std::uint16_t port, const JoinOptions& options);

	// The same two, on a transport somebody else built (tests, and the
	// in-process play-in-editor variant). Takes ownership.
	bool hostOn(std::unique_ptr<HE::Net::ITransport> transport, const HostOptions& options);
	bool joinOn(std::unique_ptr<HE::Net::ITransport> transport, const JoinOptions& options);

	// End the session. Sends a Bye as a client, drops every peer as a host,
	// tears the whole chain down. Idempotent.
	void leave();

	// Drain the transport, run the frame order (plan §5.2), collect events.
	void update(float dt);

	// ── State ────────────────────────────────────────────────────────────────
	Status             status() const { return m_status; }
	const std::string& lastError() const { return m_error; }
	const std::string& joinCode() const { return m_joinCode; }
	const std::string& sessionId() const { return m_sessionId; }
	const std::string& scenePath() const { return m_scenePath; }
	// The port host() actually opened. 0 with an injected transport (there is
	// no port) and 0 on a client. Worth having because HostOptions::port is
	// routinely 0 for "let the OS pick", and this is then the only way anyone —
	// a direct join, the announcement, a diagnostics line — learns the answer.
	std::uint16_t boundPort() const { return m_boundPort; }
	bool isAuthority() const { return m_role == HE::Net::NetRole::Host ||
	                                  m_role == HE::Net::NetRole::Server; }
	bool isClient() const { return m_role == HE::Net::NetRole::Client; }
	bool isActive() const { return m_status != Status::Idle && m_status != Status::Failed; }
	HE::Net::Game::PlayerId localPlayer() const { return m_localPlayer; }

	const HE::Net::Game::PlayerRoster& roster() const { return m_roster; }

	// How long a round trip to this player takes, in milliseconds, or 0 when
	// there is no sample: the host itself, a player who has not been measured
	// yet, and every session running on an injected transport (a test, the
	// in-process play-in-editor pair) — none of those have a socket to time.
	// Read off the UdpTransport under the crypto layer, which is why it is 0
	// rather than a guess when that is not what is down there.
	float pingMs(HE::Net::Game::PlayerId player) const;

	// ── Finding a session on the LAN (plan §5.3) ─────────────────────────────
	// Browsing is the LOBBY's business, not the session's — which is why step 4
	// put the Announcer here and left the Browser out. The net.lanSession* rows
	// are that lobby, and a row has nowhere else to look, so it lives here after
	// all, filtered to Game announcements so an editor collaboration session on
	// the same segment is never offered as a game to join.
	//
	// Running it costs one bound UDP port and nothing else; it is independent of
	// status(), so a main menu may browse before anything is hosted or joined.
	bool         refreshLan();
	void         stopLanBrowse();
	bool         browsingLan() const { return m_browser.running(); }
	const std::vector<HE::Net::LanBeacon::Browser::Session>& lanSessions() const
	{ return m_browser.sessions(); }
	// Join the index'th row of lanSessions(). False = no such row.
	bool joinLan(std::size_t index, const JoinOptions& options);

	// ── Possession (plan §5.4 point 4) ───────────────────────────────────────
	// Host: make `character` this player's. One call and not three at a call
	// site, because the ORDER is the point and getting it wrong is invisible:
	// ownership and the input assignment have to be in place BEFORE the message
	// that announces them, or the owner's first input arrives at a host that
	// does not yet believe the entity is theirs — a Hard anti-cheat observation
	// against an honest player (the warning on GameReplication::setAntiCheat).
	//
	// The entity must already be replicated (through the scene walk or a spawn).
	// False = no such player, or no net id.
	bool assignControl(HE::Net::Game::PlayerId player, Entity character);

	// Client: what to do when the host says an entity is ours. The session does
	// the network half itself (setLocallyControlled, so prediction takes over);
	// this callback is the APPLICATION half — possess it with the local player
	// controller, point the camera at it — which HE_Scene cannot do from here
	// for SpawnReplicator::SpawnFn's reason. Unset is an ordinary state.
	using ControlFn = std::function<void(Entity character, std::uint32_t netId)>;
	void setControlFunction(ControlFn fn) { m_control = std::move(fn); }

	// Client: how the application makes and unmakes an object of a class, for
	// the spawns the host sends (SpawnReplicator::SpawnFn). Set on the SESSION
	// and not on the replicator, because the replicator is built per session and
	// the application binds its services once, at startup — there is no moment
	// in OnInit at which a replicator exists to be told.
	void setSpawnFunction(SpawnReplicator::SpawnFn fn);
	void setDespawnFunction(SpawnReplicator::DespawnFn fn);
	// The entity this side drives, or entt::null. On the host that is whatever
	// was last assigned to player 1; on a client, what kMsgControl named.
	Entity localCharacter() const { return m_localCharacter; }

	// ── Parts ────────────────────────────────────────────────────────────────
	// Null outside a session. Callers hold them for one frame at most: leave()
	// destroys them.
	GameReplication*  replication() { return m_replication.get(); }
	SpawnReplicator*  spawns()      { return m_spawns.get(); }
	HE::Net::NetSession* session()  { return m_net.get(); }
	HE::AntiCheat::AntiCheatHost* antiCheat() { return m_acHost.get(); }
	// The service is only built when the project asked for it; null = OFF.
	HE::AntiCheat::AntiCheatService* antiCheatService() { return m_acService.get(); }

	// ── Events ───────────────────────────────────────────────────────────────
	// Oldest first; each is delivered exactly once. Step 5 drains this into
	// NetEvents::dispatch.
	bool takeEvent(Event& out);
	std::size_t pendingEventCount() const { return m_events.size(); }

	// ── Host: dropping somebody ──────────────────────────────────────────────
	// By player id, so callers never handle a connection id that the transport
	// may reuse. False = no such player.
	bool kick(HE::Net::Game::PlayerId player);

	struct Stats
	{
		std::uint32_t joinsAccepted = 0;
		std::uint32_t joinsRejected = 0;
		std::uint32_t playersLeft   = 0;
		std::uint32_t helloMalformed = 0;
	};
	const Stats& stats() const { return m_stats; }

private:
	bool   startCommon(std::unique_ptr<HE::Net::ITransport> transport,
	                   HE::Net::NetRole role, const GameReplication::Config& repCfg);
	void   installHandlers();

	// Host
	void   handleHello(HE::Net::ConnectionId conn, HE::Net::BitReader& r);
	void   handleBye(HE::Net::ConnectionId conn);
	void   reject(HE::Net::ConnectionId conn, RejectReason reason);
	void   completeJoin(HE::Net::ConnectionId conn, HE::Net::Game::PlayerId player);
	void   onPeerGone(HE::Net::ConnectionId conn, DisconnectReason reason);
	void   reapSeveredLinks();

	// Client
	void   sendHello();
	void   handleWelcome(HE::Net::BitReader& r);
	void   handleReject(HE::Net::BitReader& r);
	void   handleJoinComplete();
	void   handleControl(HE::Net::BitReader& r);
	// Resolve a pending kMsgControl once the entity behind its net id exists.
	void   tryTakeControl();

	void   push(Event::Kind kind, HE::Net::Game::PlayerId player = HE::Net::Game::kNoPlayer,
	            int reason = 0, std::string name = {});

	HorizonWorld*    m_world = nullptr;
	HE::Net::NetRole m_role  = HE::Net::NetRole::None;
	Status           m_status = Status::Idle;
	std::string      m_error;

	// The chain, outermost first. Destroyed in reverse by leave().
	std::unique_ptr<HE::Net::ITransport>  m_transport;   // owns everything under it
	std::unique_ptr<HE::Net::NetSession>  m_net;
	std::unique_ptr<GameReplication>      m_replication;
	std::unique_ptr<SpawnReplicator>      m_spawns;
	std::unique_ptr<HE::AntiCheat::AntiCheatService> m_acService;
	std::unique_ptr<HE::AntiCheat::AntiCheatHost>    m_acHost;

	HostOptions m_hostOptions;
	JoinOptions m_joinOptions;

	HE::Net::Game::PlayerRoster m_roster;
	HE::Net::Game::PlayerId     m_localPlayer = HE::Net::Game::kNoPlayer;

	ControlFn     m_control;
	// Held here and handed to each SpawnReplicator as it is built; see the
	// setters.
	SpawnReplicator::SpawnFn   m_spawnFn;
	SpawnReplicator::DespawnFn m_despawnFn;
	Entity        m_localCharacter = entt::null;
	// Client: a kMsgControl whose net id has no entity YET. It cannot normally
	// happen (the spawn is ReliableOrdered and goes first), but an authored
	// entity whose bind found nothing leaves exactly this hole, and dropping the
	// message would leave the player watching a character nobody drives.
	std::uint32_t m_pendingControlNetId = 0;

	std::string m_joinCode;
	std::string m_sessionId;
	std::string m_scenePath;
	std::string m_projectId;
	std::uint16_t m_boundPort = 0;

	// Host: connections whose Hello has not been accepted yet. A peer that is
	// through the crypto handshake but has not identified itself is NOT a player
	// and must not be counted as one.
	std::vector<HE::Net::ConnectionId> m_pending;
	// Refused, notice already written to the socket, link to be dropped on the
	// NEXT update — the same one-frame delay the anti-cheat kick uses, and for
	// the same reason: cutting the link here would take the explanation with it.
	std::vector<HE::Net::ConnectionId> m_rejected;

	// LAN discovery (plan §5.3). Announcing is the host's own business, unlike
	// browsing, which belongs to whatever UI is looking for a session — so the
	// Announcer lives here and the Browser does not.
	HE::Net::LanBeacon::Announcer m_announcer;
	// The lobby's side of the same beacon. Survives leave(): a menu that browses
	// between two sessions should not have to start over because one ended.
	HE::Net::LanBeacon::Browser   m_browser;
	std::uint64_t                 m_instanceId = 0;   // dedupes our own beacon
	// The UDP transport under the crypto layer, for the one thing only it can
	// answer (pingMs). Non-owning and null with an injected transport; cleared
	// by leave() together with the chain it points into.
	HE::Net::UdpTransport*        m_udp = nullptr;

	std::deque<Event> m_events;
	Stats             m_stats;
	// Set by the reject handler so leave() knows a disconnect was already
	// explained and does not follow it with a second, wrong reason.
	bool m_disconnectExplained = false;
	// Client: the link went down and the reason has not been decided yet. Held
	// until after the anti-cheat host has turned any notice into a ticket, so
	// OnCheatDetected precedes OnDisconnected as the plan states.
	bool m_pendingDisconnect = false;
};
