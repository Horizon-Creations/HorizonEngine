#include "HorizonScene/Net/RpcRouter.h"

#include "HorizonScene/AntiCheat/AntiCheatService.h"
#include "HorizonScene/Components/NetworkComponent.h"
#include "HorizonScene/GameReplication.h"
#include "HorizonScene/Net/NetMessages.h"
#include "HorizonScene/Net/ValueWire.h"

#include <HorizonCode/HorizonCodeRuntime.h>

#include <Diagnostics/Log.h>

#include <algorithm>

using namespace HE::Net;
using namespace HE::Net::Game;
using HorizonCode::Value;

namespace
{
	// A function name is an identifier, not a document. Anything longer did not
	// come from our writer, and a receiver must not be made to allocate for it.
	constexpr std::size_t kMaxNameLength = 128;
	// Arguments on ONE call. `argc` is a byte, so 255 is the ceiling the format
	// can express anyway; the cap is here so a malformed count cannot make a
	// receiver reserve a stack of values before the stream has proved it has
	// them.
	constexpr std::uint8_t kMaxArgs = 32;

	// Is an argument of type `got` acceptable where `want` is declared?
	//
	// Exactly equal, OR both numeric. The second half is not a loophole, it is
	// what makes the text frontends usable: Lua and Python hand over untyped
	// numbers, so `takeDamage(40)` produces an Int for a parameter somebody
	// declared Float, and a strict comparison would refuse every honest call
	// from a script. The callee coerces to its declared types on the way in
	// anyway (Runner::callFunction), so nothing downstream sees the difference
	// — and what the check is for, a String where a number belongs or an array
	// where a scalar belongs, is still caught.
	bool typesCompatible(HorizonCode::PinType got, HorizonCode::PinType want)
	{
		if (got == want) return true;
		const auto numeric = [](HorizonCode::PinType t) {
			return t == HorizonCode::PinType::Int || t == HorizonCode::PinType::Float ||
			       t == HorizonCode::PinType::Enum;
		};
		return numeric(got) && numeric(want);
	}

	bool alreadyLogged(std::vector<std::string>& seen, const std::string& key)
	{
		if (std::find(seen.begin(), seen.end(), key) != seen.end()) return true;
		seen.push_back(key);
		return false;
	}
}

RpcRouter::RpcRouter(NetSession* net, NetRole role, GameReplication* replication)
	: m_net(net), m_role(role), m_rep(replication)
{
	if (!m_net) return;
	// BOTH sides listen, unlike every other replicator here: an RPC is the one
	// message that travels upstream. What each side ACCEPTS still differs, and
	// that is decided in handleRpc rather than by not registering — a client's
	// CallClient arriving at a host has to be seen in order to be refused.
	m_net->on(kMsgRpc, [this](ConnectionId conn, BitReader& r) { handleRpc(conn, r); });
}

bool RpcRouter::isAuthority() const
{
	return m_role == NetRole::Server || m_role == NetRole::Host;
}

void RpcRouter::setRuntime(HorizonCode::Runtime* rt, InstanceOfFn instanceOf)
{
	m_runtime    = rt;
	m_instanceOf = std::move(instanceOf);
}

void RpcRouter::clear()
{
	m_inbox.clear();
	m_sender = kNoPlayer;
	m_rate.clear();
	m_anyClient.clear();
	m_missingLogged.clear();
	m_now = 0.0f;
}

void RpcRouter::dropConnection(ConnectionId conn)
{
	m_rate.erase(conn);
}

void RpcRouter::update(float dt)
{
	m_now += dt > 0.0f ? dt : 0.0f;
	// Forget what has fallen out of the window. Done here and not only on
	// arrival, so a connection that stops calling stops carrying a history.
	const float cutoff = m_now - kRpcRateWindowSec;
	for (auto& [conn, times] : m_rate)
	{
		(void)conn;
		while (!times.empty() && times.front() < cutoff) times.pop_front();
	}
}

std::vector<ConnectionId> RpcRouter::joinedConnections() const
{
	if (!m_net) return {};
	if (!m_joined) return m_net->connections();
	std::vector<ConnectionId> out;
	for (const ConnectionId conn : m_net->connections())
		if (m_joined(conn)) out.push_back(conn);
	return out;
}

std::uint32_t RpcRouter::netIdOf(Entity entity) const
{
	if (!m_world || entity == entt::null || !m_world->registry().valid(entity)) return 0;
	const auto* nc = m_world->registry().try_get<NetworkComponent>(entity);
	return nc ? nc->netId : 0u;
}

PlayerId RpcRouter::ownerOf(Entity entity) const
{
	if (!m_world || entity == entt::null || !m_world->registry().valid(entity)) return kNoPlayer;
	const auto* nc = m_world->registry().try_get<NetworkComponent>(entity);
	return nc ? static_cast<PlayerId>(nc->owner) : kNoPlayer;
}

// ── Any Client, declared on the entity ───────────────────────────────────────

void RpcRouter::allowAnyClient(Entity entity, const std::string& fn)
{
	const std::uint32_t netId = netIdOf(entity);
	if (netId == 0)
	{
		HE_LOG_WARN(Replication,
		            "net.allowAnyClient('%s'): this entity is not replicated, so no client "
		            "can call anything on it", fn.c_str());
		return;
	}
	auto& names = m_anyClient[netId];
	if (std::find(names.begin(), names.end(), fn) == names.end()) names.push_back(fn);
}

bool RpcRouter::anyClientAllowed(Entity entity, const std::string& fn) const
{
	const std::uint32_t netId = netIdOf(entity);
	if (netId == 0) return false;
	const auto it = m_anyClient.find(netId);
	if (it == m_anyClient.end()) return false;
	return std::find(it->second.begin(), it->second.end(), fn) != it->second.end();
}

// ── Sending ──────────────────────────────────────────────────────────────────

bool RpcRouter::emit(const std::vector<ConnectionId>& conns, std::uint32_t netId,
                     std::uint8_t target, const std::string& fn,
                     const std::vector<Value>& args)
{
	if (!m_net || conns.empty()) return false;
	if (fn.empty() || fn.size() > kMaxNameLength) return false;
	if (args.size() > kMaxArgs)
	{
		HE_LOG_ERROR(Replication,
		             "RPC '%s' has %zu arguments; at most %u travel", fn.c_str(),
		             args.size(), (unsigned)kMaxArgs);
		return false;
	}

	// A FRESH writer that is thrown away on refusal — writeValue may already
	// have put partial bits in it, which is the contract ValueWire documents.
	BitWriter w;
	w.writeUInt32(netId);
	w.writeByte(target);
	// Written so the format is the same in both directions and a client's own
	// log shows who it claimed to be. The HOST overwrites it from the
	// connection and never reads this field (plan §7.3).
	w.writeUInt32(static_cast<std::uint32_t>(m_localPlayer));
	w.writeString(fn);
	w.writeByte(static_cast<std::uint8_t>(args.size()));
	for (const Value& v : args)
		if (!writeValue(w, v))
		{
			++m_stats.refusedArgument;
			HE_LOG_ERROR(Replication,
			             "RPC '%s': an argument is an object reference, which names nothing "
			             "on another machine; the call was not sent", fn.c_str());
			return false;
		}

	for (const ConnectionId to : conns)
	{
		m_net->send(to, kMsgRpc, w, SendMode::ReliableOrdered);
		++m_stats.sent;
	}
	m_stats.bytesSent += static_cast<std::uint32_t>((w.bitCount() + 7) / 8) *
	                     static_cast<std::uint32_t>(conns.size());
	return true;
}

bool RpcRouter::callServer(Entity entity, const std::string& fn, const std::vector<Value>& args)
{
	const std::uint32_t netId = netIdOf(entity);
	if (netId == 0)
	{
		HE_LOG_WARN(Replication,
		            "CallServer '%s': the entity is not replicated (no net id), so the host "
		            "has nothing to address", fn.c_str());
		return false;
	}
	if (isAuthority()) return false;   // the caller runs it; see route()
	const auto conns = m_net ? m_net->connections() : std::vector<ConnectionId>{};
	if (conns.empty()) return false;
	// A client has exactly one connection, and it is the host.
	return emit({ conns.front() }, netId, static_cast<std::uint8_t>(HorizonCode::RunOn::Server),
	            fn, args);
}

bool RpcRouter::callClient(PlayerId player, Entity entity, const std::string& fn,
                           const std::vector<Value>& args)
{
	if (!isAuthority())
	{
		HE_LOG_WARN(Replication,
		            "CallClient '%s' was made on a CLIENT, which may not run functions on "
		            "other machines; nothing was sent", fn.c_str());
		return false;
	}
	const std::uint32_t netId = netIdOf(entity);
	if (netId == 0) return false;
	if (!m_roster) return false;
	const PlayerInfo* info = m_roster->find(player);
	if (!info)
	{
		HE_LOG_WARN(Replication, "CallClient '%s': no player %u in this session",
		            fn.c_str(), (unsigned)player);
		return false;
	}
	// The host's own player has no connection to send down — it is already
	// here. Running it locally is the caller's business (route does exactly
	// that); from the explicit row it is a no-op rather than a lie.
	if (info->local) return false;
	return emit({ info->conn }, netId,
	            static_cast<std::uint8_t>(HorizonCode::RunOn::OwningClient), fn, args);
}

bool RpcRouter::callAllClients(Entity entity, const std::string& fn,
                               const std::vector<Value>& args)
{
	if (!isAuthority())
	{
		HE_LOG_WARN(Replication,
		            "CallAllClients '%s' was made on a CLIENT, which may not run functions "
		            "on other machines; nothing was sent", fn.c_str());
		return false;
	}
	const std::uint32_t netId = netIdOf(entity);
	if (netId == 0) return false;
	const auto conns = joinedConnections();
	if (conns.empty()) return false;
	return emit(conns, netId, static_cast<std::uint8_t>(HorizonCode::RunOn::AllClients),
	            fn, args);
}

bool RpcRouter::route(Entity entity, const std::string& fn, const std::vector<Value>& args,
                      std::uint8_t runOn, bool anyClient)
{
	(void)anyClient;   // the SENDER never checks it; the host does, on arrival
	const auto mode = static_cast<HorizonCode::RunOn>(runOn);
	if (mode == HorizonCode::RunOn::Local) return false;

	// No session: every Run On function is an ordinary function. This is the
	// single-player answer the whole feature rests on — see the header.
	if (!m_net || m_net->connections().empty()) return false;

	switch (mode)
	{
	case HorizonCode::RunOn::Server:
		// The host IS the server. Running it here is not a shortcut, it is the
		// definition.
		if (isAuthority()) return false;
		return callServer(entity, fn, args);

	case HorizonCode::RunOn::OwningClient:
	{
		if (!isAuthority()) return false;   // a client never mails another client
		const PlayerId owner = ownerOf(entity);
		// Unowned, or owned by the host's own player: this machine is already
		// the owning client.
		if (owner == kNoPlayer || owner == m_localPlayer) return false;
		return callClient(owner, entity, fn, args);
	}

	case HorizonCode::RunOn::AllClients:
		if (!isAuthority()) return false;
		// Sent AND run here: a multicast reaches every machine, and the host is
		// one. The send's success does not change that — a host with no clients
		// yet still plays its own effect.
		callAllClients(entity, fn, args);
		return false;

	case HorizonCode::RunOn::Local:
		break;
	}
	return false;
}

// ── Receiving ────────────────────────────────────────────────────────────────

void RpcRouter::handleRpc(ConnectionId conn, BitReader& r)
{
	++m_stats.received;
	// Measured against what is left rather than read off a counter the reader
	// does not keep: every refusal below consumes the whole message anyway, so
	// this is the message's size either way.
	const std::size_t bitsAtEntry = r.bitsRemaining();

	std::uint32_t netId = 0;
	std::uint8_t  target = 0;
	std::uint32_t claimedPlayer = 0;
	std::string   fn;
	std::uint8_t  argc = 0;
	if (!r.readUInt32(netId) || !r.readByte(target) || !r.readUInt32(claimedPlayer) ||
	    !r.readString(fn) || !r.readByte(argc))
	{
		++m_stats.malformed;
		if (m_antiCheat && isAuthority())
			m_antiCheat->observe(conn, HE::AntiCheat::Kind::Malformed, 0.0f,
			                     "truncated remote call");
		return;
	}
	if (fn.empty() || fn.size() > kMaxNameLength || argc > kMaxArgs)
	{
		++m_stats.malformed;
		if (m_antiCheat && isAuthority())
			m_antiCheat->observe(conn, HE::AntiCheat::Kind::Malformed, 0.0f,
			                     "remote call with an impossible name or argument count");
		return;
	}

	// The arguments are read BEFORE any refusal below returns, and deliberately
	// so: this is a shared reliable stream, and leaving a half-read message in
	// it would derail the next one. A refused call is still a fully consumed
	// one.
	std::vector<Value> args(argc);
	bool parsed = true;
	for (std::uint8_t i = 0; i < argc && parsed; ++i) parsed = readValue(r, args[i]);
	if (!parsed)
	{
		++m_stats.malformed;
		if (m_antiCheat && isAuthority())
			m_antiCheat->observe(conn, HE::AntiCheat::Kind::Malformed, 0.0f,
			                     "remote call whose arguments did not parse");
		return;
	}

	// ── Who sent it ──
	// On the host: out of the CONNECTION, never out of the message. On a
	// client: whatever arrives is from the host, and the host's player id is
	// the one thing in the message a client may believe, because the host is
	// the authority on the roster.
	PlayerId fromPlayer = kNoPlayer;
	if (isAuthority())
	{
		if (m_roster)
			for (const PlayerInfo& p : m_roster->players())
				if (p.conn == conn && !p.local) { fromPlayer = p.id; break; }
	}
	else
	{
		fromPlayer = static_cast<PlayerId>(claimedPlayer);
	}

	const Entity entity = m_rep ? m_rep->entityOf(netId) : entt::null;
	const auto   mode   = static_cast<HorizonCode::RunOn>(target);

	if (isAuthority())
	{
		// A client may ask the host to run a Server function and nothing else.
		// Telling the host to run a CLIENT function is not a mistake any
		// honest build makes — it is a message our own writer never produces.
		if (mode != HorizonCode::RunOn::Server)
		{
			++m_stats.wrongDirection;
			if (m_antiCheat)
				m_antiCheat->observe(conn, HE::AntiCheat::Kind::ForeignEntity, 0.0f,
				                     "client asked the host to run a client-side call");
			return;
		}
		if (!accept(conn, fromPlayer, entity, netId, fn, args)) return;
	}
	else
	{
		// From the host, which is the authority: no ownership to check and no
		// rate to enforce — a client that does not trust its host has already
		// lost. The wrong DIRECTION is still worth dropping, because it means
		// the two sides disagree about the protocol.
		if (mode == HorizonCode::RunOn::Server || mode == HorizonCode::RunOn::Local)
		{
			++m_stats.wrongDirection;
			return;
		}
		if (entity == entt::null)
		{
			++m_stats.unknownEntity;
			HE_LOG_WARN(Replication,
			            "Remote call '%s' for net id %u, which this client does not know",
			            fn.c_str(), (unsigned)netId);
			return;
		}
	}

	Call call;
	call.entity     = entity;
	call.netId      = netId;
	call.name       = std::move(fn);
	call.args       = std::move(args);
	call.fromPlayer = fromPlayer;
	m_inbox.push_back(std::move(call));
	++m_stats.delivered;
	m_stats.bytesReceived +=
	    static_cast<std::uint32_t>((bitsAtEntry - r.bitsRemaining() + 7) / 8);
}

bool RpcRouter::accept(ConnectionId conn, PlayerId fromPlayer, Entity entity,
                       std::uint32_t netId, const std::string& fn,
                       const std::vector<Value>& args)
{
	// 1. The net id resolves. Silent-ish: an entity destroyed while a call was
	// in flight is SERVER state, and the client did nothing wrong.
	if (entity == entt::null)
	{
		++m_stats.unknownEntity;
		HE_LOG_WARN(Replication,
		            "CallServer '%s' for net id %u, which names no entity here",
		            fn.c_str(), (unsigned)netId);
		return false;
	}

	// The signature, when a HorizonCode class on the entity has one. It answers
	// both the Any Client question and the format check; a Lua/Python/native
	// target has neither, and then both are skipped rather than guessed.
	HorizonCode::Runtime::FunctionSignature sig;
	if (m_runtime && m_instanceOf)
		if (const HorizonCode::InstanceId inst = m_instanceOf(entity))
			sig = m_runtime->functionSignatureOf(inst, fn);

	// 2. Owner. Either the caller owns the entity, or the function said out
	// loud that anybody may ask for it.
	const PlayerId owner = ownerOf(entity);
	const bool open = (sig.found && sig.anyClient) || anyClientAllowed(entity, fn);
	if (!open && owner != fromPlayer)
	{
		++m_stats.notOwner;
		if (m_antiCheat)
			m_antiCheat->observe(conn, HE::AntiCheat::Kind::ForeignEntity, 0.0f,
			                     "CallServer for an entity this player does not own");
		return false;
	}

	// 3. Rate. Weighed exactly like the input rate it is modelled on, so one
	// scale covers both (AntiCheatService::preApply).
	auto& times = m_rate[conn];
	const float cutoff = m_now - kRpcRateWindowSec;
	while (!times.empty() && times.front() < cutoff) times.pop_front();
	times.push_back(m_now);
	const float rate = static_cast<float>(times.size()) / kRpcRateWindowSec;
	if (rate > kMaxRpcPerSecond)
	{
		++m_stats.rateLimited;
		if (m_antiCheat)
		{
			const float weight = (rate / kMaxRpcPerSecond - 1.0f) * 10.0f;
			m_antiCheat->observe(conn, HE::AntiCheat::Kind::InputRate, weight,
			                     "remote calls above the rate limit");
		}
		// Dropped, not applied — the same answer an over-rate input gets.
		return false;
	}

	// 4. Format, when there is a signature to compare against. An argument list
	// that does not match the function is not something a legitimate client
	// produces: both sides generated it from the same graph.
	if (sig.found && sig.hasParams)
	{
		bool ok = args.size() == sig.params.size();
		for (std::size_t i = 0; ok && i < args.size(); ++i)
			// Container shape is part of the type: an Array of Int where an Int
			// is declared is exactly the shape a hand-built message has.
			ok = typesCompatible(args[i].type, sig.params[i]) &&
			     args[i].kind() == HorizonCode::ContainerKind::None;
		if (!ok)
		{
			++m_stats.formatMismatch;
			if (m_antiCheat)
				m_antiCheat->observe(conn, HE::AntiCheat::Kind::Malformed, 0.0f,
				                     "CallServer whose arguments do not match the function");
			return false;
		}
	}
	else if (!sig.found)
	{
		// Not an error: the target may be a Lua, Python or native handler, and
		// this side cannot see those. Said once per (entity, name) so a missing
		// handler is findable without drowning the log.
		const std::string key = std::to_string(netId) + ":" + fn;
		if (!alreadyLogged(m_missingLogged, key))
			HE_LOG_INFO(Replication,
			            "CallServer '%s' on net id %u: no HorizonCode signature to check it "
			            "against; delivering to whatever frontend has it", fn.c_str(),
			            (unsigned)netId);
	}
	return true;
}

bool RpcRouter::takeCall(Call& out)
{
	if (m_inbox.empty())
	{
		// The drain loop's exit is also where the sender stops being valid —
		// net.rpcSender outside a handler must not answer whoever went last.
		m_sender = kNoPlayer;
		return false;
	}
	out = std::move(m_inbox.front());
	m_inbox.pop_front();
	m_sender = out.fromPlayer;
	return true;
}
