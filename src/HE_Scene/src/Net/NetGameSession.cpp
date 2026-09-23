#include "HorizonScene/Net/NetGameSession.h"

#include "HorizonScene/Components/NetworkComponent.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Net/NetMessages.h"

#include <Diagnostics/Log.h>
#include <Net/SecureTransport.h>
#include <Net/UdpTransport.h>
#include <Project/ProjectSettings.h>

#include <algorithm>
#include <chrono>
#include <utility>

using namespace HE::Net;
using namespace HE::Net::Game;

namespace
{
	// A display name is a name. Longer than this did not come from our own
	// dialog, and the roster must not be made to hold a novel per player.
	constexpr std::size_t kMaxNameLength      = 64;
	constexpr std::size_t kMaxProjectIdLength = 128;

	const char* reasonName(NetGameSession::RejectReason r)
	{
		switch (r)
		{
			case NetGameSession::RejectReason::VersionMismatch: return "protocol version";
			case NetGameSession::RejectReason::WrongProject:    return "different project";
			case NetGameSession::RejectReason::SessionFull:     return "session full";
			case NetGameSession::RejectReason::Banned:          return "banned label";
			case NetGameSession::RejectReason::Malformed:       return "malformed hello";
			default:                                            return "none";
		}
	}

	// Which OnDisconnected number a refusal turns into (plan §7.4). Version and
	// project get their own so the client can say WHAT to fix; everything else
	// is the generic "rejected".
	NetGameSession::DisconnectReason disconnectFor(NetGameSession::RejectReason r)
	{
		switch (r)
		{
			case NetGameSession::RejectReason::VersionMismatch:
				return NetGameSession::DisconnectReason::VersionMismatch;
			case NetGameSession::RejectReason::WrongProject:
				return NetGameSession::DisconnectReason::WrongProject;
			default:
				return NetGameSession::DisconnectReason::Rejected;
		}
	}
}

NetGameSession::NetGameSession()  = default;
NetGameSession::~NetGameSession() { leave(); }

// ── Lifecycle ────────────────────────────────────────────────────────────────

bool NetGameSession::host(const HostOptions& options)
{
	leave();

	auto listener = UdpTransport::listen(options.port);
	if (!listener)
	{
		m_error  = "Could not open a UDP port for the session.";
		m_status = Status::Failed;
		HE_LOG_ERROR(Replication, "%s", m_error.c_str());
		return false;
	}
	// Read before the transport disappears into the chain below: it is what the
	// announcement has to carry, and with port 0 it is the only way to learn
	// which port the OS actually gave us.
	const std::uint16_t boundPort = listener->boundPort();
	// Kept for pingMs alone: once wrapped, the chain is an ITransport and the
	// round-trip estimate lives one layer below that. Non-owning — the crypto
	// layer owns it from the next line on.
	UdpTransport* const udp = listener.get();

	// Machine-generated, never user-chosen: an observer who captured the
	// handshake could brute-force a weak passphrase offline.
	m_joinCode = SecureTransport::generateJoinSecret();

	SecureTransport::Config sec;
	sec.joinSecret        = m_joinCode;
	sec.role              = NetRole::Host;
	sec.requireEncryption = true;
	// Reorder over UDP is weather, not corruption — the one difference from the
	// collaboration stack, which runs this at 0 over TCP.
	sec.replayWindow      = SecureTransport::kReplayWindowFrames;

	auto secure = SecureTransport::wrap(std::move(listener), sec);
	if (!secure)
	{
		m_error  = "No crypto backend available — a session cannot be secured.";
		m_status = Status::Failed;
		HE_LOG_ERROR(Replication, "%s", m_error.c_str());
		return false;
	}

	if (!hostOn(std::move(secure), options)) return false;
	m_boundPort = boundPort;   // after hostOn: it resets the session's state
	m_udp       = udp;

	if (options.announceLan)
	{
		LanBeacon::Announcement a;
		a.protocol     = kGameProtocolVersion;
		a.instance     = m_instanceId;
		a.sessionId    = m_sessionId;
		a.port         = boundPort;
		a.hostName     = options.displayName;
		a.projectLabel = options.projectLabel;
		a.projectKey   = options.projectId;
		a.participants = static_cast<std::uint8_t>(std::min<std::size_t>(m_roster.size(), 255));
		// The whole point of the field: an editor browsing for collaborators
		// must not be offered this, and a lobby browsing for games must be.
		a.kind         = LanBeacon::Announcement::Kind::Game;
		// Not fatal. On macOS this is usually a missing Local Network
		// permission; the session runs, it just cannot be found this way.
		if (!m_announcer.start(a))
			HE_LOG_WARN(Replication, "%s",
			               "The session is up but cannot announce itself on the local network.");
	}
	return true;
}

bool NetGameSession::joinDirect(const std::string& address, std::uint16_t port,
                                const JoinOptions& options)
{
	leave();

	auto link = UdpTransport::connect(address, port);
	if (!link)
	{
		m_error  = "Could not reach " + address + ":" + std::to_string(port) + ".";
		m_status = Status::Failed;
		HE_LOG_ERROR(Replication, "%s", m_error.c_str());
		return false;
	}
	UdpTransport* const udp = link.get();   // see host(): for pingMs alone

	SecureTransport::Config sec;
	sec.joinSecret        = options.joinCode;
	sec.role              = NetRole::Client;
	sec.requireEncryption = true;
	sec.replayWindow      = SecureTransport::kReplayWindowFrames;

	auto secure = SecureTransport::wrap(std::move(link), sec);
	if (!secure)
	{
		m_error  = "No crypto backend available — a session cannot be secured.";
		m_status = Status::Failed;
		HE_LOG_ERROR(Replication, "%s", m_error.c_str());
		return false;
	}

	if (!joinOn(std::move(secure), options)) return false;
	m_udp = udp;   // after joinOn: it resets the session's state
	return true;
}

bool NetGameSession::refuseClientSpawn(const std::string& classPath)
{
	if (!isActive() || !isClient()) return false;

	if (std::find(m_refusedSpawnClasses.begin(), m_refusedSpawnClasses.end(), classPath) ==
	    m_refusedSpawnClasses.end())
	{
		m_refusedSpawnClasses.push_back(classPath);
		HE_LOG_INFO(Replication,
		            "Create Object of '%s' does nothing here: only the host makes replicated "
		            "objects, and it sends you the one it made. Guard the spawn with "
		            "Is Authority to say so in the graph.",
		            classPath.c_str());
	}
	return true;
}

void NetGameSession::setSpawnFunction(SpawnReplicator::SpawnFn fn)
{
	m_spawnFn = std::move(fn);
	// Also to the live one, so binding mid-session is not a silent no-op.
	if (m_spawns) m_spawns->setSpawnFunction(m_spawnFn);
}

void NetGameSession::setDespawnFunction(SpawnReplicator::DespawnFn fn)
{
	m_despawnFn = std::move(fn);
	if (m_spawns) m_spawns->setDespawnFunction(m_despawnFn);
}

// ── Finding a session on the LAN (plan §5.3) ────────────────────────────────

bool NetGameSession::refreshLan()
{
	// Game announcements only. An editor collaboration session broadcasts on
	// the same port and the same wire format, and offering one as a game to
	// join would hand the player a refusal they cannot act on.
	m_browser.setKind(LanBeacon::Announcement::Kind::Game);
	m_browser.setSelfInstance(m_instanceId);
	if (m_browser.running()) { m_browser.stop(); }
	return m_browser.start();
}

void NetGameSession::stopLanBrowse() { m_browser.stop(); }

bool NetGameSession::joinLan(std::size_t index, const JoinOptions& options)
{
	const auto& found = m_browser.sessions();
	if (index >= found.size()) return false;
	const auto& s = found[index];
	// The address comes from the PACKET's source, never its payload
	// (LanBeacon::Browser::Session), which is the same rule the host follows for
	// a joiner's identity.
	return joinDirect(s.address, s.port, options);
}

float NetGameSession::pingMs(PlayerId player) const
{
	if (!m_udp) return 0.0f;                         // injected transport: no socket to time
	const PlayerInfo* info = m_roster.find(player);
	if (!info || info->local) return 0.0f;           // ourselves: zero, and honestly so
	if (info->conn == kInvalidConnection) return 0.0f;
	const float srtt = m_udp->peerStats(info->conn).srttMs;
	return srtt > 0.0f ? srtt : 0.0f;                // <0 = no sample yet
}

bool NetGameSession::startCommon(std::unique_ptr<ITransport> transport, NetRole role,
                                 const GameReplication::Config& repCfg)
{
	if (!transport) return false;

	m_transport = std::move(transport);
	m_role      = role;
	m_net       = std::make_unique<NetSession>(m_transport.get(), role);

	m_replication = std::make_unique<GameReplication>(m_net.get(), role, repCfg);
	m_replication->setWorld(m_world);

	m_spawns = std::make_unique<SpawnReplicator>(m_net.get(), role, m_replication.get());
	m_spawns->setWorld(m_world);
	// Only welcomed peers hear about binds and spawns. The roster IS the answer
	// to "is this one in the session": a connection gets an entry in it at
	// exactly the moment the host accepts its Hello.
	m_spawns->setJoinedFilter([this](ConnectionId conn) {
		return m_roster.findByConnection(conn) != nullptr;
	});
	// The application's spawn services, bound once at startup and re-applied to
	// every replicator this session builds (see the setters).
	if (m_spawnFn)   m_spawns->setSpawnFunction(m_spawnFn);
	if (m_despawnFn) m_spawns->setDespawnFunction(m_despawnFn);

	// The host is always present, even before anything else is: a session with
	// nobody in it is a listening socket, not a session.
	m_acHost = std::make_unique<HE::AntiCheat::AntiCheatHost>();

	installHandlers();
	return true;
}

bool NetGameSession::hostOn(std::unique_ptr<ITransport> transport, const HostOptions& options)
{
	if (!transport) { m_error = "No transport."; m_status = Status::Failed; return false; }

	m_hostOptions = options;
	m_projectId   = options.projectId;
	m_scenePath   = options.scenePath;
	if (m_sessionId.empty())
		m_sessionId = SecureTransport::generateJoinSecret().substr(0, 8);
	// Identifies THIS run of the session on the LAN, so our own beacon coming
	// straight back off the segment is not listed as somebody else's.
	m_instanceId = std::chrono::steady_clock::now().time_since_epoch().count() ^
	               (static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(this)) << 16);

	if (!startCommon(std::move(transport), NetRole::Host, options.replication))
		return false;

	// Anti-cheat is per session and OFF unless the project asked for it. With no
	// service attached every reader answers its neutral default and every
	// response is a logged no-op — the behaviour that existed before the service
	// did (AntiCheatHost.h).
	if (options.antiCheat && options.antiCheat->enabled)
	{
		m_acService = std::make_unique<HE::AntiCheat::AntiCheatService>(
			HE::AntiCheat::AntiCheatHost::configFrom(*options.antiCheat));
		m_acHost->setPolicy(HE::AntiCheat::AntiCheatHost::policyFrom(*options.antiCheat));
		m_replication->setAntiCheat(m_acService.get());
	}
	m_acHost->setPreview(options.preview);
	m_acHost->attach(m_acService.get(), m_replication.get());

	// The host is player 1, always, and has no connection of its own.
	m_roster.clear();
	m_localPlayer = m_roster.add(kInvalidConnection, options.displayName, /*local*/ true);

	// The registry walk (plan §5.5). THIS is what the inspector switch buys:
	// nobody calls registerEntity by hand any more.
	const int bound = m_spawns->bindSceneEntities();

	m_status = Status::Hosting;
	m_error.clear();
	push(Event::Kind::SessionStarted, m_localPlayer, 0, options.displayName);

	HE_LOG_INFO(Replication,
	            "Hosting session '%s' as '%s': %d replicated entities, anti-cheat %s%s",
	            m_sessionId.c_str(), options.displayName.c_str(), bound,
	            m_acService ? "on" : "off", options.preview ? " (preview)" : "");
	return true;
}

bool NetGameSession::joinOn(std::unique_ptr<ITransport> transport, const JoinOptions& options)
{
	if (!transport) { m_error = "No transport."; m_status = Status::Failed; return false; }

	m_joinOptions = options;
	m_projectId   = options.projectId;
	m_joinCode    = options.joinCode;

	if (!startCommon(std::move(transport), NetRole::Client, options.replication))
		return false;

	// A client has no service — it makes no reports — but it DOES get a host,
	// so a notice from the host becomes a local ticket with the same readers
	// and fires the same OnCheatDetected (AntiCheatHost.h).
	m_acHost->attach(nullptr, m_replication.get());

	m_roster.clear();
	m_localPlayer = kNoPlayer;   // the host assigns it in the Welcome
	m_disconnectExplained = false;

	m_status = Status::Connecting;
	m_error.clear();
	HE_LOG_INFO(Replication, "Joining a session as '%s'", options.displayName.c_str());
	return true;
}

void NetGameSession::leave()
{
	if (!m_net)
	{
		// Even an idle session resets cleanly: a Failed status must not survive
		// into the next attempt.
		m_status = Status::Idle;
		m_role   = NetRole::None;
		return;
	}

	if (isClient() && m_status != Status::Failed)
	{
		// Say so rather than vanishing: the host would otherwise wait out the
		// transport timeout before anyone learns the seat is free.
		m_net->broadcast(kMsgBye, SendMode::ReliableOrdered);
		if (m_transport) m_transport->update();
	}
	else if (isAuthority())
	{
		for (const ConnectionId conn : m_net->connections())
			m_net->disconnect(conn);
	}

	push(Event::Kind::SessionEnded, m_localPlayer);

	// Sends the goodbye before closing, so browsers drop the row now instead of
	// waiting out the expiry.
	m_announcer.stop();

	if (m_acHost)   m_acHost->detach();
	if (m_spawns)   m_spawns->clear();

	// Reverse of construction: the session points at the transport, the
	// replicators point at the session.
	m_spawns.reset();
	m_replication.reset();
	m_acHost.reset();
	m_acService.reset();
	m_net.reset();
	m_transport.reset();
	m_udp = nullptr;   // it lived inside the chain that just went away

	m_roster.clear();
	m_pending.clear();
	m_localPlayer = kNoPlayer;
	m_localCharacter      = entt::null;
	m_pendingControlNetId = 0;
	m_refusedSpawnClasses.clear();
	m_role        = NetRole::None;
	m_status      = Status::Idle;
	m_sessionId.clear();
	m_joinCode.clear();
	m_boundPort = 0;

	HE_LOG_INFO(Replication, "Session ended");
}

// ── Handlers ─────────────────────────────────────────────────────────────────

void NetGameSession::installHandlers()
{
	if (isAuthority())
	{
		m_net->onConnect([this](ConnectionId conn) {
			// Through the crypto handshake, but it has not said who it is yet.
			// A peer without a Hello is NOT a player and must not be counted as
			// one — otherwise maxPlayers could be filled by connecting alone.
			m_pending.push_back(conn);
			HE_LOG_DEBUG(Replication, "Connection %u is up, awaiting its hello", conn);
		});
		m_net->onDisconnect([this](ConnectionId conn) {
			onPeerGone(conn, DisconnectReason::Timeout);
		});
		m_net->on(kMsgHello, [this](ConnectionId conn, BitReader& r) { handleHello(conn, r); });
		m_net->on(kMsgBye,   [this](ConnectionId conn, BitReader&)   { handleBye(conn); });
	}
	else
	{
		m_net->onConnect([this](ConnectionId) { sendHello(); });
		m_net->onDisconnect([this](ConnectionId) {
			if (m_disconnectExplained) return;
			m_disconnectExplained = true;
			// NOT pushed here. The notice that explains a kick arrives in the
			// same pump() as the disconnect that follows it, and the anti-cheat
			// host turns it into the local ticket that fires OnCheatDetected —
			// which the plan (§7.4) puts BEFORE OnDisconnected(2). Parking it
			// until after acHost->pump() is what makes that order hold instead
			// of depending on which message the socket handed over first.
			m_pendingDisconnect = true;
			m_status = Status::Failed;
		});
		m_net->on(kMsgWelcome,      [this](ConnectionId, BitReader& r) { handleWelcome(r); });
		m_net->on(kMsgReject,       [this](ConnectionId, BitReader& r) { handleReject(r); });
		m_net->on(kMsgJoinComplete, [this](ConnectionId, BitReader&)   { handleJoinComplete(); });
		m_net->on(kMsgControl,      [this](ConnectionId, BitReader& r) { handleControl(r); });
	}
}

// ── Host ─────────────────────────────────────────────────────────────────────

void NetGameSession::handleHello(ConnectionId conn, BitReader& r)
{
	std::uint16_t version = 0;
	std::string   name;
	std::string   projectId;
	if (!r.readUInt16(version) || !r.readString(name) || !r.readString(projectId))
	{
		++m_stats.helloMalformed;
		reject(conn, RejectReason::Malformed);
		return;
	}
	if (name.size() > kMaxNameLength || projectId.size() > kMaxProjectIdLength)
	{
		++m_stats.helloMalformed;
		reject(conn, RejectReason::Malformed);
		return;
	}

	// A second Hello on the same connection is either a confused client or
	// somebody trying for a second seat. One per connection.
	if (m_roster.findByConnection(conn) != nullptr) return;

	if (version != kGameProtocolVersion) { reject(conn, RejectReason::VersionMismatch); return; }

	// Everything a session sends is addressed by uuids that only mean something
	// inside ONE project (the lesson CollabController learned the hard way:
	// the scene arrived and every asset reference in it dangled).
	if (!m_projectId.empty() && projectId != m_projectId)
	{
		reject(conn, RejectReason::WrongProject);
		return;
	}

	if (m_roster.size() >= m_hostOptions.maxPlayers) { reject(conn, RejectReason::SessionFull); return; }

	// A ban has no identity to hold on to but the label (anti-cheat plan §6.1).
	if (m_acHost && m_acHost->isBanned(name)) { reject(conn, RejectReason::Banned); return; }

	const PlayerId player = m_roster.add(conn, name);
	m_pending.erase(std::remove(m_pending.begin(), m_pending.end(), conn), m_pending.end());
	if (m_acHost) m_acHost->setPlayerLabel(conn, name);

	completeJoin(conn, player);
}

void NetGameSession::completeJoin(ConnectionId conn, PlayerId player)
{
	// Everything below is ReliableOrdered, which is what makes this a SEQUENCE:
	// Welcome, then what exists, then where it stands, then "you are in".
	BitWriter w;
	w.writeUInt32(player);
	w.writeString(m_scenePath);
	w.writeFloat(m_hostOptions.replication.tickHz);
	w.writeUInt32(m_hostOptions.maxPlayers);
	m_net->send(conn, kMsgWelcome, w, SendMode::ReliableOrdered);

	// Binds and spawns first — the baseline's samples need somewhere to land.
	m_spawns->sendWorldTo(conn);
	m_replication->sendBaseline(conn);

	m_net->send(conn, kMsgJoinComplete, SendMode::ReliableOrdered);

	++m_stats.joinsAccepted;
	const PlayerInfo* info = m_roster.find(player);
	push(Event::Kind::PlayerJoined, player, 0, info ? info->name : std::string{});

	HE_LOG_INFO(Replication, "Player %u ('%s') joined on connection %u (%zu in session)",
	            player, info ? info->name.c_str() : "?", conn, m_roster.size());
}

bool NetGameSession::assignControl(PlayerId player, Entity character)
{
	if (!isAuthority() || !m_replication || !m_world) return false;
	if (character == entt::null || !m_world->registry().valid(character)) return false;

	PlayerInfo* info = m_roster.find(player);
	if (!info) return false;

	const auto* nc = m_world->registry().try_get<NetworkComponent>(character);
	if (!nc || nc->netId == 0)
	{
		HE_LOG_WARN(Replication,
		            "assignControl: entity is not replicated, so player %u cannot be given it. "
		            "Turn Replicates on, or spawn it through the session.",
		            player);
		return false;
	}
	const std::uint32_t netId = nc->netId;

	// The order the whole function exists for (GameReplication::setAntiCheat):
	// own it, accept its input, THEN say so. A kMsgControl that overtook the
	// assignment would make the owner's first input look like input for an
	// entity they do not drive, which is a Hard observation.
	m_replication->registerEntity(character, player);
	info->characterNetId = netId;

	if (info->local)
	{
		// The host's own player. Nothing travels; it simply is ours.
		m_localCharacter = character;
		if (m_control) m_control(character, netId);
	}
	else
	{
		m_replication->assignControl(info->conn, netId);
		BitWriter w;
		w.writeUInt32(netId);
		m_net->send(info->conn, kMsgControl, w, SendMode::ReliableOrdered);
	}

	HE_LOG_INFO(Replication, "Player %u drives net id %u", player, netId);
	return true;
}

void NetGameSession::handleControl(BitReader& r)
{
	std::uint32_t netId = 0;
	if (!r.readUInt32(netId) || netId == 0) return;
	m_pendingControlNetId = netId;
	tryTakeControl();
}

void NetGameSession::tryTakeControl()
{
	if (m_pendingControlNetId == 0 || !m_replication) return;

	const Entity e = m_replication->entityOf(m_pendingControlNetId);
	if (e == entt::null) return;   // the spawn has not landed yet; update() retries

	const std::uint32_t netId = m_pendingControlNetId;
	m_pendingControlNetId = 0;
	m_localCharacter = e;

	// The network half first: from here the entity is driven by local input and
	// corrected against the host, rather than interpolated towards it.
	m_replication->setLocallyControlled(e, netId);
	if (m_control) m_control(e, netId);

	HE_LOG_INFO(Replication, "We drive net id %u", netId);
}

void NetGameSession::reject(ConnectionId conn, RejectReason reason)
{
	BitWriter w;
	w.writeByte(static_cast<std::uint8_t>(reason));
	w.writeString(m_hostOptions.projectLabel);
	m_net->send(conn, kMsgReject, w, SendMode::ReliableOrdered);

	++m_stats.joinsRejected;
	HE_LOG_WARN(Replication, "Refused connection %u: %s", conn, reasonName(reason));

	// The link is dropped on the NEXT update, not here: the refusal has to leave
	// the socket first, and a disconnect in the middle of draining it would take
	// the explanation with it. Same rule the anti-cheat kick follows.
	m_pending.erase(std::remove(m_pending.begin(), m_pending.end(), conn), m_pending.end());
	m_rejected.push_back(conn);
}

void NetGameSession::handleBye(ConnectionId conn)
{
	onPeerGone(conn, DisconnectReason::Leave);
	// They said goodbye; nothing more will come. Dropping the link now frees
	// the slot without waiting out the transport's timeout.
	m_net->disconnect(conn);
}

void NetGameSession::onPeerGone(ConnectionId conn, DisconnectReason reason)
{
	const PlayerInfo* info = m_roster.findByConnection(conn);
	const std::string name = info ? info->name : std::string{};
	const std::uint32_t character = info ? info->characterNetId : 0u;

	const PlayerId left = m_roster.removeByConnection(conn);
	m_pending.erase(std::remove(m_pending.begin(), m_pending.end(), conn), m_pending.end());

	// Their character goes with them. Keeping it for a rejoin is a game rule,
	// not an engine one, and is outside v1 (plan §5.4).
	if (character != 0 && m_spawns) m_spawns->notifyDespawnedById(character);

	// NetSession fires no onDisconnect for a link WE severed, so the per-client
	// input tracking, the control assignment and the anti-cheat state would
	// otherwise outlive the peer — and a connection id the transport reuses
	// would inherit them.
	if (m_replication) m_replication->dropConnection(conn);

	if (left == kNoPlayer) return;   // never got past the hello

	++m_stats.playersLeft;
	push(Event::Kind::PlayerLeft, left, static_cast<int>(reason), name);
	HE_LOG_INFO(Replication, "Player %u ('%s') left (%d)", left, name.c_str(),
	            static_cast<int>(reason));
}

void NetGameSession::reapSeveredLinks()
{
	if (!m_net) return;

	// One mechanism for all three ways a link ends — a kick at the anti-cheat's
	// frame end, a refusal, our own leave — because none of them fire
	// onDisconnect and all three leave the same debris behind.
	const auto& live = m_net->connections();
	const auto isLive = [&live](ConnectionId c) {
		return std::find(live.begin(), live.end(), c) != live.end();
	};

	std::vector<ConnectionId> gone;
	for (const auto& p : m_roster.players())
		if (p.conn != kInvalidConnection && !isLive(p.conn)) gone.push_back(p.conn);
	for (const ConnectionId conn : gone)
		onPeerGone(conn, DisconnectReason::Kicked);

	m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
	                               [&](ConnectionId c) { return !isLive(c); }),
	                m_pending.end());
}

bool NetGameSession::kick(PlayerId player)
{
	if (!isAuthority() || !m_net) return false;
	const PlayerInfo* info = m_roster.find(player);
	if (!info || info->conn == kInvalidConnection) return false;

	// Through the anti-cheat host when there is one, so the player gets the
	// notice their client turns into a local ticket, and the disconnect lands a
	// frame later — the rule that gives the notice time to leave the socket.
	if (m_acHost && m_acHost->isEnabled())
	{
		m_acHost->kick(info->conn, HE::AntiCheat::kReasonPolicy);
		return true;
	}

	const ConnectionId conn = info->conn;
	onPeerGone(conn, DisconnectReason::Kicked);
	m_net->disconnect(conn);
	return true;
}

// ── Client ───────────────────────────────────────────────────────────────────

void NetGameSession::sendHello()
{
	if (!m_net || m_net->connections().empty()) return;
	BitWriter w;
	w.writeUInt16(kGameProtocolVersion);
	w.writeString(m_joinOptions.displayName);
	w.writeString(m_joinOptions.projectId);
	m_net->send(m_net->connections().front(), kMsgHello, w, SendMode::ReliableOrdered);
	HE_LOG_DEBUG(Replication, "Hello sent (protocol %u)", kGameProtocolVersion);
}

void NetGameSession::handleWelcome(BitReader& r)
{
	std::uint32_t player = 0;
	std::string   scene;
	float         tickHz = 0.0f;
	std::uint32_t maxPlayers = 0;
	if (!r.readUInt32(player) || !r.readString(scene) || !r.readFloat(tickHz) ||
	    !r.readUInt32(maxPlayers))
		return;

	m_localPlayer = player;
	m_scenePath   = scene;
	// The roster on a client holds ITSELF ALONE, and under the id the HOST
	// minted — not one of its own, which is what add() would give it (1, the
	// host's own number). That mismatch made net.playerName(net.localPlayer())
	// answer empty on every client.
	//
	// The other players are still missing: nothing sends a client the list, and
	// no message id is reserved for one. Until there is (the natural place is
	// the late-join baseline), net.playerCount and net.playerAt see one player
	// on a client, and the docs for those rows say so.
	m_roster.clear();
	m_roster.addWithId(m_localPlayer, kInvalidConnection, m_joinOptions.displayName,
	                   /*local*/ true);

	HE_LOG_INFO(Replication, "Welcome: player %u, scene '%s', %.1f Hz",
	            player, scene.c_str(), static_cast<double>(tickHz));
}

void NetGameSession::handleReject(BitReader& r)
{
	std::uint8_t reason = 0;
	std::string  projectLabel;
	if (!r.readByte(reason)) return;
	(void)r.readString(projectLabel);

	const auto rr = static_cast<RejectReason>(reason);
	switch (rr)
	{
		case RejectReason::VersionMismatch:
			m_error = "The host runs a different version of the game."; break;
		case RejectReason::WrongProject:
			m_error = projectLabel.empty()
				? std::string("The host has a different project open.")
				: "The host has '" + projectLabel + "' open.";
			break;
		case RejectReason::SessionFull: m_error = "The session is full."; break;
		case RejectReason::Banned:      m_error = "This name is banned from the session."; break;
		default:                        m_error = "The host refused the join."; break;
	}

	m_status              = Status::Failed;
	m_disconnectExplained = true;
	push(Event::Kind::Disconnected, m_localPlayer, static_cast<int>(disconnectFor(rr)));
	HE_LOG_WARN(Replication, "Join refused: %s", m_error.c_str());
}

void NetGameSession::handleJoinComplete()
{
	// Everything before this was ReliableOrdered, so "the baseline is over" is
	// a fact and not a guess.
	m_status = Status::Joined;
	push(Event::Kind::Connected, m_localPlayer);
	HE_LOG_INFO(Replication, "Joined as player %u", m_localPlayer);
}

// ── Frame ────────────────────────────────────────────────────────────────────

void NetGameSession::update(float dt)
{
	// BEFORE the guard below, not after: browsing is what a main menu does while
	// there is no session at all, and behind the guard it would only ever run
	// once somebody had already joined one.
	if (m_browser.running())
		m_browser.update(static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count()));

	if (!m_net || !m_transport) return;

	m_transport->update();

	// BEFORE pump, not after: a refusal decided during THIS pump has not been
	// written to the socket yet, and cutting the link in the same frame takes
	// the explanation with it — the joiner then sees a dead connection instead
	// of "you have the wrong project open". The transport update above is what
	// has just flushed the previous frame's refusals, so by here they are gone.
	if (!m_rejected.empty())
	{
		for (const ConnectionId conn : m_rejected)
		{
			m_net->disconnect(conn);
			if (m_replication) m_replication->dropConnection(conn);
		}
		m_rejected.clear();
	}

	m_net->pump();          // handlers: hello, welcome, bind, spawn, snapshot, input

	// A kMsgControl that arrived before the entity it names existed (an unbound
	// authored entity, plan §5.5). Retried every frame until it resolves; free
	// when there is nothing pending, which is every frame but one per session.
	if (m_pendingControlNetId != 0) tryTakeControl();

	m_replication->update(dt);

	// Frame end, in the order AntiCheatHost documents: pump fires the events
	// while a handler can still overrule the policy, flush executes what is
	// left — the kick among it, which is why it is here and not where the
	// observation was made.
	if (m_acHost)
	{
		m_acHost->pump();
		m_acHost->flush();
	}

	// After the flush: a kick just severed a link, and nothing fires
	// onDisconnect for a link this side severed.
	if (isAuthority()) reapSeveredLinks();

	if (m_pendingDisconnect)
	{
		m_pendingDisconnect = false;
		// The host does not say "you were kicked" on the wire — it sends the
		// anti-cheat notice first and drops the link a frame later (plan §5.5).
		// A notice received is therefore what distinguishes a kick from a link
		// that simply died, and by now the notice has become a ticket.
		const bool kicked = m_replication && m_replication->stats().noticesReceived > 0;
		if (m_error.empty())
			m_error = kicked ? "Removed from the session." : "Lost the connection to the host.";
		push(Event::Kind::Disconnected, m_localPlayer,
		     static_cast<int>(kicked ? DisconnectReason::Kicked : DisconnectReason::Timeout));
	}

	if (m_announcer.running())
	{
		m_announcer.setParticipants(
			static_cast<std::uint8_t>(std::min<std::size_t>(m_roster.size(), 255)));
		m_announcer.update(static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count()));
	}
}

// ── Events ───────────────────────────────────────────────────────────────────

void NetGameSession::push(Event::Kind kind, PlayerId player, int reason, std::string name)
{
	Event e;
	e.kind   = kind;
	e.player = player;
	e.reason = reason;
	e.name   = std::move(name);
	m_events.push_back(std::move(e));

	// A queue nobody drains is a leak. Step 5 drains it every frame; until then
	// a headless host running for an hour must not grow through it.
	constexpr std::size_t kMaxEvents = 256;
	while (m_events.size() > kMaxEvents) m_events.pop_front();
}

bool NetGameSession::takeEvent(Event& out)
{
	if (m_events.empty()) return false;
	out = std::move(m_events.front());
	m_events.pop_front();
	return true;
}
