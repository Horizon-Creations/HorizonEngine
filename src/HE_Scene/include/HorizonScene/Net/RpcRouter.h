#pragma once

// ─── Layer 3b — "run this function over there" ───────────────────────────────
// GameReplication answers where net id 12 is, SpawnReplicator what it is,
// PropertyReplicator what it holds. This is the fourth question and the only
// one that is not about STATE at all: an ACTION, crossing the wire in the
// direction state never travels — from a client to the authority.
//
//   CallServer       client → host    "I pulled the lever"
//   CallClient       host   → owner   "your shot hit"
//   CallAllClients   host   → all     "the round is over"
//
// ── Three doors, one message ────────────────────────────────────────────────
// A HorizonCode function carries Run On at its header (Node::runOn), and a Call
// Function node on such a function is NOT executed by the caller — the Runner
// asks route() first (plan §7.2). Lua, Python and a C++ GameLogic have no
// function header the engine can read, so they say it explicitly through the
// net.call* rows. Both produce the same kMsgRpc, and the receiving side cannot
// tell which one wrote it. That is the point: a Lua script on a client may call
// a HorizonCode function on the host, and neither knows about the other.
//
// ── Fire and forget ─────────────────────────────────────────────────────────
// No return values, ever (plan §7.2). There is nobody to hand them back to
// without inventing a request/response protocol and a way to wait for it, and a
// graph that waits is a graph that stalls the frame. The editor refuses a
// Function Return on a Run On function rather than letting it be discovered
// three machines later.
//
// ── The side that IS the target runs it ─────────────────────────────────────
// route() returns TRUE for "it is on the wire, do not run it here" and FALSE
// for "run it here". False is the answer in the three cases that matter most:
//
//   • offline. No session at all, and then a Run On function behaves exactly as
//     if the mode were not set. The door in the docs has to work in single
//     player before anybody hosts anything, and net.isAuthority answers true
//     offline for the very same reason (plan §7.1).
//   • the host calling its own Server function. Mailing it to itself would be a
//     round trip to nowhere.
//   • AllClients on the host. It is sent to every joined client AND runs here,
//     which is what a multicast means: the host is a machine with a screen too.
//
// A CLIENT never mails another client. An OwningClient or AllClients call made
// on a client runs locally and goes nowhere — the alternative is a client that
// can make other people's machines run functions, which is the whole reason
// §7.3 has the host reject those on sight.
//
// ── An RPC is a claim, not a fact (plan §7.6) ───────────────────────────────
// Everything a client sends is checked on the host BEFORE delivery, in this
// order: the net id resolves; the caller owns the entity, or the function is
// marked Any Client; the connection is under the rate; the argument list
// matches the signature. Owner and format failures are HARD observations — no
// honest client produces them — and the rate is weighed like the input rate it
// is modelled on. `fromPlayer` is taken from the CONNECTION and never from the
// message, the same rule the beacon address follows.
//
// The Any Client flag lives on the HorizonCode function header, so a Lua or
// Python function on an unowned entity has no way to carry it. That is the
// honest boundary of this step and net.allowAnyClient is the door for it:
// the entity's own declaration, stored next to the replicated variables.
//
// ── Delivery is QUEUED ──────────────────────────────────────────────────────
// Like NetGameSession's events and PropertyReplicator's notifications, and for
// the same reason: running a graph from inside a message handler runs game code
// in the middle of draining a socket. The session drains this right after
// pump() — which is "before the simulation" on a host (its CallServers are
// inputs, plan §7.3) and "after pump, before the script tick" on a client, in
// one place rather than two that could drift.

#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Net/PlayerRoster.h"

#include <HorizonCode/HorizonCode.h>
#include <Net/NetSession.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class GameReplication;
namespace HorizonCode { class Runtime; }
namespace HE::AntiCheat { class AntiCheatService; }

// How many calls one connection may make per second before the host starts
// counting it against them (plan §7.6 point 3). Generous on purpose: a legit
// burst after a stall delivers a frame's worth at once, exactly like input.
// The DEFAULT: the project's Multiplayer page overrides it per session.
inline constexpr float kMaxRpcPerSecond = 60.0f;
// The window the rate is measured over. Seconds and not a frame, the lesson the
// dt budget and the input rate both already carry.
inline constexpr float kRpcRateWindowSec = 2.0f;

// No HE_API — HorizonScene exports every symbol (see GameReplication.h).
class RpcRouter
{
public:
	// How the router reaches a HorizonCode instance on an entity, exactly as
	// PropertyReplicator takes it and for the same reason: EntityHost owns the
	// map and this header must not depend on it. Unbound is an ordinary state
	// and means "no HorizonCode classes in this project" — the format check
	// then has no signature to compare against, and says so rather than
	// guessing.
	using InstanceOfFn = std::function<HorizonCode::InstanceId(Entity)>;
	// Which connections count as IN the session (SpawnReplicator's filter).
	using JoinedFn = std::function<bool(HE::Net::ConnectionId)>;

	// `replication` must outlive this object: net ids are minted there.
	RpcRouter(HE::Net::NetSession* net, HE::Net::NetRole role,
	          GameReplication* replication);

	void setWorld(HorizonWorld* world) { m_world = world; }
	void setRuntime(HorizonCode::Runtime* rt, InstanceOfFn instanceOf);
	void setJoinedFilter(JoinedFn fn) { m_joined = std::move(fn); }
	// The roster, for player ↔ connection. Must outlive this object; the
	// session owns both.
	void setRoster(const HE::Net::Game::PlayerRoster* roster) { m_roster = roster; }
	// Which player this machine is. kHostPlayer on a host; what the Welcome
	// said on a client. Decides whether an OwningClient call is already home.
	void setLocalPlayer(HE::Net::Game::PlayerId p) { m_localPlayer = p; }
	// Where a refused claim is reported. Null = no anti-cheat in this session,
	// which is byte-for-byte the behaviour before the service existed: the call
	// is still refused, nobody is scored for it.
	void setAntiCheat(HE::AntiCheat::AntiCheatService* ac) { m_antiCheat = ac; }
	// The project's "Max remote calls per second" (plan §8.4). Anything at or
	// below zero is ignored rather than taken literally: a hand-edited 0 would
	// mean "no client may call anything", which is not a setting anybody wants
	// and not what the page says.
	void setMaxCallsPerSecond(float perSecond)
	{ if (perSecond > 0.0f) m_maxPerSecond = perSecond; }
	float maxCallsPerSecond() const { return m_maxPerSecond; }

	// ── Sending ──────────────────────────────────────────────────────────────
	// The three explicit doors (the net.call* rows, the scripting frontends).
	// False = it went nowhere, and the reason is in the log: no session, no net
	// id, no such player. A caller that wants "run it locally when offline"
	// asks route() instead — that is what the Run On path does.
	bool callServer(Entity entity, const std::string& fn,
	                const std::vector<HorizonCode::Value>& args);
	bool callClient(HE::Net::Game::PlayerId player, Entity entity, const std::string& fn,
	                const std::vector<HorizonCode::Value>& args);
	bool callAllClients(Entity entity, const std::string& fn,
	                    const std::vector<HorizonCode::Value>& args);

	// The Run On path (plan §7.2). TRUE = on the wire, do not run it locally.
	// See the header comment for every case that answers false.
	bool route(Entity entity, const std::string& fn,
	           const std::vector<HorizonCode::Value>& args,
	           std::uint8_t runOn, bool anyClient);

	// ── Any Client, for the frontends without a function header ──────────────
	// Lua, Python and a native module declare it on the ENTITY instead, which is
	// where their functions effectively live. Additive: a name listed here is
	// callable by any client, a name not listed follows the owner rule. A
	// HorizonCode function's own checkbox is consulted first and this second, so
	// the two can never contradict each other into a refusal.
	void allowAnyClient(Entity entity, const std::string& fn);
	bool anyClientAllowed(Entity entity, const std::string& fn) const;

	// ── Receiving ────────────────────────────────────────────────────────────
	struct Call
	{
		Entity                          entity = entt::null;
		std::uint32_t                   netId  = 0;
		std::string                     name;
		std::vector<HorizonCode::Value> args;
		// Who asked. The connection's player on a host, the host's player id on
		// a client. Never read off the message.
		HE::Net::Game::PlayerId         fromPlayer = HE::Net::Game::kNoPlayer;
	};
	// Oldest first, each delivered once. Returning false ALSO clears the sender
	// below, so a drain loop leaves nothing standing behind it.
	bool takeCall(Call& out);
	std::size_t pendingCallCount() const { return m_inbox.size(); }
	// Who made the call currently being delivered — net.rpcSender (plan §7.6
	// point 5). Valid between a takeCall that returned true and the next one;
	// kNoPlayer outside that, which is what a graph asking outside a handler
	// gets and the honest answer.
	HE::Net::Game::PlayerId rpcSender() const { return m_sender; }

	// Advance the rate window. Once per frame, with real delta time.
	void update(float dt);

	// Forget a connection's rate history (it left, and an id the transport
	// reuses must inherit nothing — GameReplication::dropConnection's rule).
	void dropConnection(HE::Net::ConnectionId conn);
	// Drop everything; a second session in the same process inherits nothing.
	void clear();

	struct Stats
	{
		std::uint32_t sent            = 0;
		std::uint32_t received        = 0;
		std::uint32_t delivered       = 0;   // passed every check and was queued
		std::uint32_t unknownEntity   = 0;   // net id resolves to nothing here
		std::uint32_t notOwner        = 0;   // Hard: a stranger's CallServer
		std::uint32_t wrongDirection  = 0;   // Hard: a client sent CallClient
		std::uint32_t rateLimited     = 0;
		std::uint32_t formatMismatch  = 0;   // Hard: argc or types off
		std::uint32_t malformed       = 0;   // the stream ran out
		std::uint32_t refusedArgument = 0;   // a Ref among the arguments
		std::uint32_t bytesSent       = 0;
		std::uint32_t bytesReceived   = 0;
	};
	const Stats& stats() const { return m_stats; }
	void         resetStats() { m_stats = {}; }

private:
	bool isAuthority() const;
	std::vector<HE::Net::ConnectionId> joinedConnections() const;
	// The net id of an entity on this side, or 0 (no NetworkComponent, or one
	// that was never registered).
	std::uint32_t netIdOf(Entity entity) const;
	// Who owns an entity, straight off its NetworkComponent. 0 = nobody, which
	// is every authored prop in the scene.
	HE::Net::Game::PlayerId ownerOf(Entity entity) const;

	// Write one call and hand it to `conns`. False = it was refused before a
	// byte went out (a Ref argument, a name nobody could have written).
	bool emit(const std::vector<HE::Net::ConnectionId>& conns, std::uint32_t netId,
	          std::uint8_t target, const std::string& fn,
	          const std::vector<HorizonCode::Value>& args);
	void handleRpc(HE::Net::ConnectionId conn, HE::Net::BitReader& r);
	// The four checks of §7.6, in order. False = refused, and whatever had to be
	// observed has been.
	bool accept(HE::Net::ConnectionId conn, HE::Net::Game::PlayerId fromPlayer,
	            Entity entity, std::uint32_t netId, const std::string& fn,
	            const std::vector<HorizonCode::Value>& args);

	HE::Net::NetSession*  m_net   = nullptr;
	HE::Net::NetRole      m_role  = HE::Net::NetRole::None;
	GameReplication*      m_rep   = nullptr;
	HorizonWorld*         m_world = nullptr;
	HorizonCode::Runtime* m_runtime = nullptr;
	InstanceOfFn          m_instanceOf;
	JoinedFn              m_joined;
	const HE::Net::Game::PlayerRoster* m_roster = nullptr;
	HE::AntiCheat::AntiCheatService*   m_antiCheat = nullptr;
	HE::Net::Game::PlayerId            m_localPlayer = HE::Net::Game::kNoPlayer;
	float                              m_maxPerSecond = kMaxRpcPerSecond;

	std::deque<Call>        m_inbox;
	HE::Net::Game::PlayerId m_sender = HE::Net::Game::kNoPlayer;

	// Per connection: the timestamps of the calls inside the window, oldest
	// first. A deque and not a counter, because a rate is a shape over time and
	// a counter that resets on a boundary lets twice the rate through by
	// straddling it.
	std::unordered_map<HE::Net::ConnectionId, std::deque<float>> m_rate;
	float m_now = 0.0f;   // seconds since this router was built

	// Entity → the function names any client may call on it (allowAnyClient).
	std::unordered_map<std::uint32_t, std::vector<std::string>> m_anyClient;

	// Names already complained about, so "nobody has that function" is one line
	// per (entity, name) and not one per call.
	std::vector<std::string> m_missingLogged;

	Stats m_stats;
};
