#pragma once

// ─── Layer 3a — replicated VARIABLES, and the OnRep that follows one ─────────
// GameReplication answers "where is net id 12" thirty times a second and
// nothing else. SpawnReplicator answers "what IS net id 12". This answers the
// third question — "what does net id 12 currently hold" — for values that are
// not a pose: health, ammo, a door's open flag, a team's score, a struct of
// loadout, a map of votes.
//
// TWO SOURCES, ONE MODEL (plan §6.1). A HorizonCode class declares its
// variables in its graph and ticks a checkbox; the engine reads them through
// the Runtime. Lua, Python and a C++ GameLogic have no variables the engine can
// see, so they declare explicitly (net.declareVar) into a
// ReplicatedVarsComponent. Both end up as an ordered list of (name, Value) per
// entity, and everything below this line treats them identically.
//
// ── Why the table carries the initial values, and why everything is ordered ──
// The plan (§6.2) wrote kMsgProperties as `Reliable`, i.e. delivered but in no
// particular order across entities. Read against the transport that actually
// shipped in step 2, that is a hole rather than an optimisation: SendMode
// documents Reliable as "guaranteed delivery, unspecified order", so a delta
// may overtake the ReliableOrdered kMsgPropertyTable that names the property it
// addresses. The receiver's own rule then applies — an unknown index is dropped
// with a log line (below) — and a dropped delta is never resent, because the
// transport did deliver it. The result is a door that is open on the host and
// shut on one client, which is the exact failure the plan chose reliable
// delivery to avoid, arriving only under reordering and therefore only
// sometimes.
//
// So in this step BOTH messages are ReliableOrdered, and the table carries the
// current values rather than only the names. That costs the per-entity ordering
// the plan wanted to avoid paying for (a door delta now waits behind an
// unrelated one) and buys a property model that is correct under reorder, which
// is the trade worth making first. Per-entity channels are the way to take the
// ordering back later; they are a transport feature, and the transport has one
// channel. The table carrying values also means a joining client needs no
// separate property baseline — the table IS it — and kMsgSpawn's wire format is
// untouched.
//
// ── Dirty tracking ──
// Poll and compare, at the frame's end, against what was last SENT (plan §6.2)
// — not a dirty bit set inside setVariable, because a graph may write the same
// variable ten times in a frame and only the last value is worth a datagram.
// The comparison is ValueWire::valuesEqual, recursive through containers and
// struct fields.
//
// ── Interest management ──
// The plan says it applies here too. It does not yet: GameReplication computes
// relevance privately inside sendSnapshots and exposes no per-(client, entity)
// query, and culling a property without an "entered the radius" baseline would
// leave a client permanently holding a stale value — strictly worse than
// sending it. So a delta goes to every joined client, and the honest place to
// fix it is when relevance becomes something anyone can ask about.
//
// ── OnRep ──
// Queued here, drained by the application after update() and before the script
// tick, exactly as NetGameSession queues its lifecycle events and for the same
// reason: running a graph from inside a message handler runs game code in the
// middle of draining a socket. Never on the host (plan §6.4) — the host set the
// value and knows it.

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

// No HE_API — HorizonScene exports every symbol (see GameReplication.h).
class PropertyReplicator
{
public:
	// How the replicator reaches a HorizonCode instance on an entity. Set by
	// whoever owns both (the application, through NetGameSession): EntityHost
	// holds the entity↔instance map and this header must not depend on it —
	// the same reason SpawnReplicator takes a SpawnFn rather than calling
	// Ctx::createObject. Unbound is an ordinary state and means "this project
	// has no HorizonCode classes", which is true of every Lua/Python project
	// and of every test that does not ask for one.
	using InstanceOfFn = std::function<HorizonCode::InstanceId(Entity)>;

	// `replication` must outlive this object: net ids are minted there.
	PropertyReplicator(HE::Net::NetSession* net, HE::Net::NetRole role,
	                   GameReplication* replication);

	void setWorld(HorizonWorld* world) { m_world = world; }
	// The interpreter whose instances hold HorizonCode variables, and the map
	// from entity to instance. Both or neither.
	void setRuntime(HorizonCode::Runtime* rt, InstanceOfFn instanceOf);
	// Which connections count as IN the session (SpawnReplicator's filter, and
	// for its reason). Unset = every connection.
	using JoinedFn = std::function<bool(HE::Net::ConnectionId)>;
	void setJoinedFilter(JoinedFn fn) { m_joined = std::move(fn); }

	// ── Host ─────────────────────────────────────────────────────────────────
	// Send `conn` the table (names, types, notify flags and CURRENT values) of
	// every replicated entity that has one. Called from completeJoin, after the
	// binds and spawns — the table addresses a net id, which the client must
	// already know.
	void sendTablesTo(HE::Net::ConnectionId conn);
	// The same for one entity, for a runtime spawn: the client has just been
	// told the entity exists, and this is what it holds.
	void sendTableFor(std::uint32_t netId, HE::Net::ConnectionId conn);
	void broadcastTableFor(std::uint32_t netId);

	// Frame end: compare every replicated entity's declared variables against
	// what was last sent and send the differences. No-op on a client.
	void update();

	// Forget an entity's tracking (it despawned, or left the session).
	void forget(std::uint32_t netId);
	// Drop everything; a second session in the same process inherits nothing.
	void clear();

	// ── Both sides: reading and writing a declared variable ──────────────────
	// The one door net.setVar*/getVar* go through, so the two storage kinds
	// (HorizonCode instance, ReplicatedVarsComponent) are resolved in exactly
	// one place. `declare` creates the component entry; on an entity whose
	// HorizonCode class already declares the name, it is a no-op that logs once
	// — a variable declared twice in two storages would replicate whichever the
	// lookup order happened to prefer.
	bool declareVar(Entity entity, const std::string& name,
	                const HorizonCode::Value& initial, bool notify);
	bool setVar(Entity entity, const std::string& name, const HorizonCode::Value& v);
	bool getVar(Entity entity, const std::string& name, HorizonCode::Value& out) const;
	bool hasVar(Entity entity, const std::string& name) const;

	// ── Client: what changed, for the frontends ──────────────────────────────
	struct Notification
	{
		Entity             entity = entt::null;
		std::uint32_t      netId  = 0;
		std::string        name;
		HorizonCode::Value oldValue;   // the plan's OnRep argument
	};
	// Oldest first, each delivered once. Ordered per entity in property order,
	// which is the order the plan asks for.
	bool takeNotification(Notification& out);
	std::size_t pendingNotificationCount() const { return m_notifications.size(); }

	struct Stats
	{
		std::uint32_t tablesSent        = 0;
		std::uint32_t tablesReceived    = 0;
		std::uint32_t deltasSent        = 0;
		std::uint32_t deltasReceived    = 0;
		std::uint32_t propertiesSent    = 0;   // individual values, not datagrams
		std::uint32_t propertiesApplied = 0;
		std::uint32_t unknownIndex      = 0;   // delta for a property we have no name for
		std::uint32_t typeMismatch      = 0;   // delta whose value is not the declared type
		std::uint32_t malformed         = 0;   // the stream ran out
		std::uint32_t predictedMatches  = 0;   // delta equalled the local value → no OnRep
		std::uint32_t clientWrites      = 0;   // setVar on a client (local prediction)
		std::uint32_t bytesSent         = 0;
		std::uint32_t bytesReceived     = 0;
	};
	const Stats& stats() const { return m_stats; }
	void         resetStats() { m_stats = {}; }

	// The three costliest properties by bytes since the last reset, for the
	// diagnostics overlay (plan §8.5) — the place "my variable belongs in the
	// snapshot" becomes visible instead of being folklore.
	struct PropertyCost { std::string name; std::uint32_t bytes = 0; std::uint32_t sends = 0; };
	std::vector<PropertyCost> costliestProperties(std::size_t howMany = 3) const;

	// How many entities this side tracks a table for.
	std::size_t trackedCount() const { return m_tables.size(); }
	// The declared names of a tracked entity, in index order (tests, overlay).
	const std::vector<std::string>* namesOf(std::uint32_t netId) const;

private:
	// What one entity's properties look like on this side. Host: the names it
	// declared and the values it last sent. Client: the names the table gave it
	// and the values it last applied.
	struct Table
	{
		Entity                          entity = entt::null;
		std::vector<std::string>        names;
		std::vector<bool>               notify;
		std::vector<HorizonCode::Value> values;   // host: last SENT; client: last applied
		// Client only: this table's own names were never resolved against the
		// local declaration, because a client's declaration may legitimately be
		// absent (a Lua script that declares in OnInit which has not run yet).
		bool fromWire = false;
	};

	bool isAuthority() const;
	std::vector<HE::Net::ConnectionId> joinedConnections() const;

	// Collect an entity's declared (name, value, notify) triples, in the order
	// the wire uses: HorizonCode variables in declaration order (base classes
	// first), then ReplicatedVarsComponent entries in insertion order.
	void collect(Entity entity, std::vector<std::string>& names,
	             std::vector<bool>& notify, std::vector<HorizonCode::Value>& values) const;

	void writeTable(HE::Net::BitWriter& w, std::uint32_t netId, const Table& t) const;
	void handleTable(HE::Net::BitReader& r);
	void handleDelta(HE::Net::BitReader& r);

	// Apply one value locally, whichever storage the name lives in. Client side.
	void applyLocally(Entity entity, const std::string& name, const HorizonCode::Value& v);

	void noteCost(const std::string& name, std::uint32_t bytes);

	HE::Net::NetSession*  m_net  = nullptr;
	HE::Net::NetRole      m_role = HE::Net::NetRole::None;
	GameReplication*      m_rep  = nullptr;
	HorizonWorld*         m_world = nullptr;
	HorizonCode::Runtime* m_runtime = nullptr;
	InstanceOfFn          m_instanceOf;
	JoinedFn              m_joined;

	std::unordered_map<std::uint32_t, Table> m_tables;
	std::deque<Notification>                 m_notifications;
	std::unordered_map<std::string, PropertyCost> m_costs;

	// Client: names this side has already complained about writing locally, so
	// the "why does my value snap back" line is one per variable and not one
	// per frame (plan §6.3).
	std::vector<std::string> m_localWriteLogged;
	// Entities whose double declaration has been reported. Same rule.
	std::vector<std::string> m_doubleDeclareLogged;

	Stats m_stats;
};
