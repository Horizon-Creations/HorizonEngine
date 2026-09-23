#include "HorizonScene/Net/PropertyReplicator.h"

#include "HorizonScene/Components/NetworkComponent.h"
#include "HorizonScene/Components/ReplicatedVarsComponent.h"
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
	// A property name is an identifier, not a document. Anything longer did not
	// come from our writer, and a client must not be made to allocate for it.
	constexpr std::size_t kMaxNameLength = 128;
	// Properties on ONE entity. A class with more than this many replicated
	// variables is a class whose state belongs in a struct; the cap is here so a
	// malformed count cannot make a receiver reserve a million strings.
	// 255 and not 256, because a delta's `count` and its indices are both ONE
	// BYTE: with 256 properties the last index would not be representable, and
	// a full set of changes would go out as count 0 — a datagram that says
	// "nothing changed" while carrying everything.
	constexpr std::uint16_t kMaxProperties = 255;

	bool alreadyLogged(std::vector<std::string>& seen, const std::string& key)
	{
		if (std::find(seen.begin(), seen.end(), key) != seen.end()) return true;
		seen.push_back(key);
		return false;
	}
}

PropertyReplicator::PropertyReplicator(NetSession* net, NetRole role, GameReplication* replication)
	: m_net(net), m_role(role), m_rep(replication)
{
	if (!m_net) return;

	// Only a client is told what things hold. A host that accepted a property
	// message would be letting a peer write the authoritative world — E4, the
	// decision the whole topology rests on (plan §6.3).
	if (isAuthority()) return;

	m_net->on(kMsgPropertyTable, [this](ConnectionId, BitReader& r) { handleTable(r); });
	m_net->on(kMsgProperties,    [this](ConnectionId, BitReader& r) { handleDelta(r); });
}

bool PropertyReplicator::isAuthority() const
{
	return m_role == NetRole::Server || m_role == NetRole::Host;
}

void PropertyReplicator::setRuntime(HorizonCode::Runtime* rt, InstanceOfFn instanceOf)
{
	m_runtime    = rt;
	m_instanceOf = std::move(instanceOf);
}

void PropertyReplicator::clear()
{
	m_tables.clear();
	m_notifications.clear();
	m_costs.clear();
	m_localWriteLogged.clear();
	m_doubleDeclareLogged.clear();
}

void PropertyReplicator::forget(std::uint32_t netId)
{
	m_tables.erase(netId);
}

std::vector<ConnectionId> PropertyReplicator::joinedConnections() const
{
	if (!m_net) return {};
	if (!m_joined) return m_net->connections();
	std::vector<ConnectionId> out;
	for (const ConnectionId conn : m_net->connections())
		if (m_joined(conn)) out.push_back(conn);
	return out;
}

// ── Where a variable lives ───────────────────────────────────────────────────

void PropertyReplicator::collect(Entity entity, std::vector<std::string>& names,
                                 std::vector<bool>& notify, std::vector<Value>& values) const
{
	names.clear(); notify.clear(); values.clear();
	if (!m_world || entity == entt::null || !m_world->registry().valid(entity)) return;

	// HorizonCode first, in the runtime's wire order (base classes first). The
	// Runtime answers for both backends, so nothing here has to know whether the
	// class is interpreted or generated.
	if (m_runtime && m_instanceOf)
	{
		if (const HorizonCode::InstanceId inst = m_instanceOf(entity))
			for (const auto& rv : m_runtime->replicatedVariablesOf(inst))
			{
				names.push_back(rv.name);
				notify.push_back(rv.notify);
				values.push_back(m_runtime->getVariable(inst, rv.name));
			}
	}

	// Then the explicitly declared ones, in insertion order. A name that the
	// HorizonCode class already declares is skipped: declareVar refused it and
	// said so, and letting it through here would put the same property on the
	// wire twice under one index.
	if (const auto* rvc = m_world->registry().try_get<ReplicatedVarsComponent>(entity))
		for (const auto& e : rvc->entries)
		{
			if (std::find(names.begin(), names.end(), e.name) != names.end()) continue;
			names.push_back(e.name);
			notify.push_back(e.notify);
			values.push_back(e.value);
		}
}

bool PropertyReplicator::hasVar(Entity entity, const std::string& name) const
{
	Value ignored;
	return getVar(entity, name, ignored);
}

bool PropertyReplicator::getVar(Entity entity, const std::string& name, Value& out) const
{
	if (!m_world || entity == entt::null || !m_world->registry().valid(entity)) return false;

	if (m_runtime && m_instanceOf)
		if (const HorizonCode::InstanceId inst = m_instanceOf(entity))
			for (const auto& rv : m_runtime->replicatedVariablesOf(inst))
				if (rv.name == name) { out = m_runtime->getVariable(inst, name); return true; }

	if (const auto* rvc = m_world->registry().try_get<ReplicatedVarsComponent>(entity))
		if (const Value* v = rvc->find(name)) { out = *v; return true; }

	return false;
}

bool PropertyReplicator::declareVar(Entity entity, const std::string& name,
                                    const Value& initial, bool notify)
{
	if (!m_world || entity == entt::null || !m_world->registry().valid(entity)) return false;
	if (name.empty() || name.size() > kMaxNameLength) return false;
	if (!isReplicableType(initial.type))
	{
		HE_LOG_WARN(Replication, "declareVar('%s'): %s",
		            name.c_str(), replicationRefusalReason(initial.type));
		return false;
	}

	// A HorizonCode class that already declares this name owns it. Declaring it
	// a second time in the component would give the entity two storages for one
	// property, and which one replicated would depend on the lookup order.
	if (m_runtime && m_instanceOf)
		if (const HorizonCode::InstanceId inst = m_instanceOf(entity))
			for (const auto& rv : m_runtime->replicatedVariablesOf(inst))
				if (rv.name == name)
				{
					if (!alreadyLogged(m_doubleDeclareLogged, name))
						HE_LOG_WARN(Replication,
						            "declareVar('%s'): this entity's HorizonCode class already "
						            "replicates a variable of that name — the class's own one is "
						            "used and this declaration is ignored",
						            name.c_str());
					return false;
				}

	auto& rvc = m_world->registry().get_or_emplace<ReplicatedVarsComponent>(entity);
	rvc.declare(name, initial, notify);
	return true;
}

bool PropertyReplicator::setVar(Entity entity, const std::string& name, const Value& v)
{
	if (!m_world || entity == entt::null || !m_world->registry().valid(entity)) return false;
	if (!isReplicableType(v.type)) return false;

	// E4, and it is deliberately not a refusal: a client MAY write a replicated
	// variable, it just does not own it. The write takes effect locally (a graph
	// predicting `ammo -= 1` is the case this exists for) and the host's next
	// delta overwrites it. One log line per variable per session, so "why does
	// my value snap back" has an answer somewhere (plan §6.3).
	if (!isAuthority() && m_net)
	{
		++m_stats.clientWrites;
		if (!alreadyLogged(m_localWriteLogged, name))
			HE_LOG_DEBUG(Replication,
			             "Wrote the replicated variable '%s' on a client. It takes effect here "
			             "and the host's next update replaces it — send a CallServer if the host "
			             "should agree.", name.c_str());
	}

	if (m_runtime && m_instanceOf)
		if (const HorizonCode::InstanceId inst = m_instanceOf(entity))
			for (const auto& rv : m_runtime->replicatedVariablesOf(inst))
				if (rv.name == name) { m_runtime->setVariable(inst, name, v); return true; }

	if (auto* rvc = m_world->registry().try_get<ReplicatedVarsComponent>(entity))
		if (Value* slot = rvc->find(name)) { *slot = v; return true; }

	return false;   // never declared
}

void PropertyReplicator::applyLocally(Entity entity, const std::string& name, const Value& v)
{
	if (m_runtime && m_instanceOf)
		if (const HorizonCode::InstanceId inst = m_instanceOf(entity))
			for (const auto& rv : m_runtime->replicatedVariablesOf(inst))
				if (rv.name == name) { m_runtime->setVariable(inst, name, v); return; }

	if (!m_world || entity == entt::null || !m_world->registry().valid(entity)) return;
	// The component is CREATED when a value arrives for a name this side has not
	// declared. That is the normal case for a Lua script whose OnInit has not
	// run yet, and it is what makes net.getVar answer correctly the moment the
	// script does declare: the declaration keeps the value that is already there
	// (ReplicatedVarsComponent::declare).
	auto& rvc = m_world->registry().get_or_emplace<ReplicatedVarsComponent>(entity);
	if (Value* slot = rvc.find(name)) *slot = v;
	else                              rvc.declare(name, v, /*notify*/ false);
}

// ── Host: the table ──────────────────────────────────────────────────────────

void PropertyReplicator::writeTable(BitWriter& w, std::uint32_t netId, const Table& t) const
{
	w.writeUInt32(netId);
	w.writeUInt16(static_cast<std::uint16_t>(t.names.size()));
	for (std::size_t i = 0; i < t.names.size(); ++i)
	{
		w.writeString(t.names[i]);
		w.writeBool(t.notify[i]);
		// The value rides along, so the table IS the property baseline (see the
		// header). A value that refuses to encode is skipped by the caller, not
		// here — by this point the list has already been filtered.
		writeValue(w, t.values[i]);
	}
}

void PropertyReplicator::sendTableFor(std::uint32_t netId, ConnectionId conn)
{
	if (!m_net || !isAuthority() || !m_rep) return;
	const Entity entity = m_rep->entityOf(netId);
	if (entity == entt::null) return;

	Table t;
	t.entity = entity;
	collect(entity, t.names, t.notify, t.values);

	// A value that cannot travel (a Ref slipped into a hand-edited graph, a
	// container of them) is dropped from the TABLE, not merely from the delta —
	// otherwise the indices the two sides count with would disagree by one and
	// every property after it would land on its neighbour.
	for (std::size_t i = 0; i < t.names.size();)
	{
		BitWriter probe;
		if (writeValue(probe, t.values[i])) { ++i; continue; }
		HE_LOG_WARN(Replication,
		            "Variable '%s' cannot be replicated (%s) — it is left out of the session",
		            t.names[i].c_str(), replicationRefusalReason(t.values[i].type));
		t.names.erase(t.names.begin() + static_cast<long>(i));
		t.notify.erase(t.notify.begin() + static_cast<long>(i));
		t.values.erase(t.values.begin() + static_cast<long>(i));
	}
	if (t.names.size() > kMaxProperties) return;

	// WHAT GETS RECORDED, and this distinction is the whole of a bug that only
	// shows when somebody joins at the wrong moment. `m_tables` is the host's
	// record of what it last SENT — that is what update() compares against.
	// The table this connection is handed carries the CURRENT values, because
	// a joiner wants the truth and not the history.
	//
	// Overwriting the record with those current values would erase a change
	// that has not gone out yet: the next update() would compare the value
	// against itself, find nothing, and the players who were already in the
	// session would sit on the old value forever, with nothing to resend it.
	// That is exactly the permanent divergence ordering the messages was meant
	// to rule out, arriving through the one door that looked harmless.
	//
	// So an existing record keeps its values and only refreshes the entity. The
	// joiner then receives the same value once more as a delta on the next
	// frame; that costs a few bytes and is swallowed silently, because it
	// equals what the table already put there (handleDelta's prediction path).
	const auto existing = m_tables.find(netId);
	// Did the NAME LIST change (a script declared another variable mid-session)?
	// An index is a position in that list, so every peer has to be counting with
	// the same one — a new list has to reach ALL of them, not just the caller's
	// connection, or two clients would read the same delta as two properties.
	const bool listChanged = existing != m_tables.end() && existing->second.names != t.names;

	if (existing == m_tables.end() || listChanged)
	{
		// Recorded even when empty: an entity whose table was never built would
		// send its whole state as a "change" the first time anything else did.
		// On a changed list the values go with it — an index into the old list
		// means nothing in the new one.
		m_tables[netId] = t;
	}
	else
	{
		existing->second.entity = entity;
		existing->second.notify = t.notify;
	}
	if (t.names.empty()) return;

	BitWriter w;
	writeTable(w, netId, t);
	if (listChanged)
	{
		for (const ConnectionId to : joinedConnections())
		{
			m_net->send(to, kMsgPropertyTable, w, SendMode::ReliableOrdered);
			++m_stats.tablesSent;
		}
		// The caller's own connection may not be in the roster yet (a table
		// built for a peer mid-handshake), so it is addressed on its own if the
		// loop above missed it.
		const auto joined = joinedConnections();
		if (std::find(joined.begin(), joined.end(), conn) == joined.end())
		{
			m_net->send(conn, kMsgPropertyTable, w, SendMode::ReliableOrdered);
			++m_stats.tablesSent;
		}
	}
	else
	{
		m_net->send(conn, kMsgPropertyTable, w, SendMode::ReliableOrdered);
		++m_stats.tablesSent;
	}
	m_stats.bytesSent += static_cast<std::uint32_t>((w.bitCount() + 7) / 8);
}

void PropertyReplicator::broadcastTableFor(std::uint32_t netId)
{
	for (const ConnectionId conn : joinedConnections())
		sendTableFor(netId, conn);
}

void PropertyReplicator::sendTablesTo(ConnectionId conn)
{
	if (!m_world || !isAuthority()) return;
	for (auto [entity, nc] : m_world->registry().view<NetworkComponent>().each())
	{
		(void)entity;
		if (!nc.replicates || nc.netId == 0) continue;
		sendTableFor(nc.netId, conn);
	}
}

// ── Host: the deltas ─────────────────────────────────────────────────────────

void PropertyReplicator::update()
{
	if (!m_world || !m_net || !isAuthority()) return;

	const std::vector<ConnectionId> conns = joinedConnections();

	// An entity that appeared since the last frame — a runtime spawn into a
	// live session — has no table anywhere, and a delta has no index to address
	// without one. Announced HERE rather than from the spawn path so there is
	// one rule and not two: whatever makes a replicated entity exist (the scene
	// walk at host time, a spawn, `Replicates` ticked on at run time) is noticed
	// the same way. With no clients this loop finds nothing to do, and the
	// tables are built for real when somebody joins (sendTablesTo).
	if (!conns.empty())
		for (auto [entity, nc] : m_world->registry().view<NetworkComponent>().each())
		{
			(void)entity;
			if (!nc.replicates || nc.netId == 0) continue;
			if (m_tables.find(nc.netId) == m_tables.end()) broadcastTableFor(nc.netId);
		}

	// And the other direction: an entity that is gone takes its tracking with
	// it, or a net id the session reuses would inherit a stranger's names.
	for (auto it = m_tables.begin(); it != m_tables.end();)
	{
		const Entity e = m_rep ? m_rep->entityOf(it->first) : entt::null;
		if (e == entt::null) it = m_tables.erase(it);
		else                 ++it;
	}

	for (auto [entity, nc] : m_world->registry().view<NetworkComponent>().each())
	{
		if (!nc.replicates || nc.netId == 0) continue;
		const auto it = m_tables.find(nc.netId);
		// No table means nobody has been told this entity's names, so there is
		// no index to address. It gets one the moment a client joins or the
		// entity is spawned into a live session.
		if (it == m_tables.end()) continue;
		Table& t = it->second;
		t.entity = entity;

		std::vector<std::string> names;
		std::vector<bool>        notify;
		std::vector<Value>       current;
		collect(entity, names, notify, current);

		// The DECLARATION changed since the last table — a script declared
		// another variable in a handler, or a hot reload took one away. Nothing
		// else would notice: a name the table does not know has no index and is
		// simply never sent, so without this a variable declared one frame after
		// the join would stay invisible for the whole session. Re-tabling is
		// also the only way to do it, because an index is a position in the
		// list, so a list that grew has to reach every client at once
		// (sendTableFor does that when it sees the names differ).
		if (names != t.names)
		{
			broadcastTableFor(nc.netId);
			continue;
		}
		if (t.names.empty()) continue;

		// Which indices changed. Looked up BY NAME rather than by position: a
		// script may declare another variable mid-session, which appends to its
		// list and would shift every position after it. A name the table does
		// not know is simply not replicated until the next table — sending it
		// under an index the client has never heard of is the failure this whole
		// design is trying to avoid.
		std::vector<std::uint8_t> changed;
		for (std::size_t i = 0; i < t.names.size() && i < kMaxProperties; ++i)
		{
			const auto at = std::find(names.begin(), names.end(), t.names[i]);
			if (at == names.end()) continue;
			const Value& now = current[static_cast<std::size_t>(at - names.begin())];
			if (valuesEqual(now, t.values[i])) continue;
			if (!valueTypesMatch(now, t.values[i]))
			{
				// The declaration changed shape under a running session (a hot
				// reload retyped the variable). The index still means the old
				// type on every client, so sending it would be a type mismatch
				// by construction; the whole table is resent instead.
				HE_LOG_WARN(Replication,
				            "Variable '%s' changed type during the session — resending net id %u's "
				            "property table", t.names[i].c_str(), nc.netId);
				changed.clear();
				broadcastTableFor(nc.netId);
				break;
			}
			t.values[i] = now;
			changed.push_back(static_cast<std::uint8_t>(i));
		}
		if (changed.empty()) continue;

		BitWriter w;
		w.writeUInt32(nc.netId);
		w.writeByte(static_cast<std::uint8_t>(changed.size()));
		for (const std::uint8_t idx : changed)
		{
			w.writeByte(idx);
			writeValue(w, t.values[idx]);
		}
		for (const ConnectionId conn : conns)
			m_net->send(conn, kMsgProperties, w, SendMode::ReliableOrdered);

		++m_stats.deltasSent;
		m_stats.propertiesSent += static_cast<std::uint32_t>(changed.size());
		const auto bytes = static_cast<std::uint32_t>((w.bitCount() + 7) / 8);
		m_stats.bytesSent += bytes * static_cast<std::uint32_t>(std::max<std::size_t>(conns.size(), 1));
		// Attributed per property, split evenly: what the overlay is for is
		// "which variable is expensive", and an even split answers that as well
		// as a per-value measurement would while costing one division.
		for (const std::uint8_t idx : changed)
			noteCost(t.names[idx], bytes / static_cast<std::uint32_t>(changed.size()));
	}
}

// ── Client ───────────────────────────────────────────────────────────────────

void PropertyReplicator::handleTable(BitReader& r)
{
	std::uint32_t netId = 0;
	std::uint16_t count = 0;
	if (!r.readUInt32(netId) || !r.readUInt16(count)) { ++m_stats.malformed; return; }
	if (count > kMaxProperties) { ++m_stats.malformed; return; }

	Table t;
	t.fromWire = true;
	t.entity   = m_rep ? m_rep->entityOf(netId) : entt::null;

	for (std::uint16_t i = 0; i < count; ++i)
	{
		std::string name;
		bool        notify = false;
		Value       v;
		if (!r.readString(name) || !r.readBool(notify) || !readValue(r, v))
		{ ++m_stats.malformed; return; }
		if (name.empty() || name.size() > kMaxNameLength) { ++m_stats.malformed; return; }
		t.names.push_back(std::move(name));
		t.notify.push_back(notify);
		t.values.push_back(std::move(v));
	}

	// A table for a net id whose entity has not arrived is kept anyway. The
	// binds and spawns go first and are ordered, so it should not happen — but
	// an authored entity whose bind found nothing leaves exactly this hole
	// (SpawnReplicator's bindsUnresolved), and dropping the table would leave
	// the client permanently blind to that entity's state.
	++m_stats.tablesReceived;
	if (t.entity != entt::null)
	{
		for (std::size_t i = 0; i < t.names.size(); ++i)
		{
			// Only the values that are actually NEW are applied and notified.
			// A table resent mid-session (a retyped variable, a second join in
			// the same process) must not fire every OnRep again.
			Value before;
			const bool had = getVar(t.entity, t.names[i], before);
			if (had && valuesEqual(before, t.values[i])) continue;
			applyLocally(t.entity, t.names[i], t.values[i]);
			++m_stats.propertiesApplied;
			// The baseline OnRep the plan asks for (§6.4): a door that was
			// already open when the player arrived has to hear about it once, or
			// it never sets its animation. The old value is what this side held
			// — its declared default when it had one, and the arriving value's
			// own type-zero when it did not, which is the honest "nothing" for a
			// property this machine is seeing for the first time.
			if (t.notify[i])
			{
				Notification n;
				n.entity   = t.entity;
				n.netId    = netId;
				n.name     = t.names[i];
				n.oldValue = had ? before : Value{};
				if (!had) n.oldValue.type = t.values[i].type;
				m_notifications.push_back(std::move(n));
			}
		}
	}
	m_tables[netId] = std::move(t);
}

void PropertyReplicator::handleDelta(BitReader& r)
{
	std::uint32_t netId = 0;
	std::uint8_t  count = 0;
	if (!r.readUInt32(netId) || !r.readByte(count)) { ++m_stats.malformed; return; }

	const auto it = m_tables.find(netId);
	++m_stats.deltasReceived;

	for (std::uint8_t k = 0; k < count; ++k)
	{
		std::uint8_t idx = 0;
		Value        v;
		// The value is READ even when the index is unusable: the stream is one
		// datagram and the values after this one are still in it. Skipping the
		// read would misalign everything that follows.
		if (!r.readByte(idx) || !readValue(r, v)) { ++m_stats.malformed; return; }

		if (it == m_tables.end() || idx >= it->second.names.size())
		{
			++m_stats.unknownIndex;
			HE_LOG_WARN(Replication,
			            "Property %u of net id %u arrived before its table — dropped",
			            static_cast<unsigned>(idx), netId);
			continue;
		}
		Table& t = it->second;
		if (t.entity == entt::null && m_rep) t.entity = m_rep->entityOf(netId);

		if (!valueTypesMatch(v, t.values[idx]))
		{
			++m_stats.typeMismatch;
			HE_LOG_WARN(Replication,
			            "Property '%s' of net id %u arrived as a different type than declared "
			            "— dropped", t.names[idx].c_str(), netId);
			continue;
		}

		const Value previous = t.values[idx];
		t.values[idx] = v;
		if (t.entity == entt::null) continue;

		// What this side ACTUALLY holds, which is not the same as what it last
		// received: a client may have written the variable itself (prediction,
		// plan §6.3). A delta that agrees with the prediction is applied — it is
		// already the value — but it is not a CHANGE, so no OnRep fires.
		Value local;
		const bool had = getVar(t.entity, t.names[idx], local);
		if (had && valuesEqual(local, v))
		{
			++m_stats.predictedMatches;
			continue;
		}

		applyLocally(t.entity, t.names[idx], v);
		++m_stats.propertiesApplied;
		if (t.notify[idx])
		{
			Notification n;
			n.entity   = t.entity;
			n.netId    = netId;
			n.name     = t.names[idx];
			// The value that was REPLACED here, not the one the host replaced:
			// a client that predicted sees its own previous value, which is what
			// "the value before this change" means on this machine.
			n.oldValue = had ? local : previous;
			m_notifications.push_back(std::move(n));
		}
	}
}

bool PropertyReplicator::takeNotification(Notification& out)
{
	if (m_notifications.empty()) return false;
	out = std::move(m_notifications.front());
	m_notifications.pop_front();
	return true;
}

// ── Diagnostics ──────────────────────────────────────────────────────────────

void PropertyReplicator::noteCost(const std::string& name, std::uint32_t bytes)
{
	PropertyCost& c = m_costs[name];
	if (c.name.empty()) c.name = name;
	c.bytes += bytes;
	++c.sends;
}

std::vector<PropertyReplicator::PropertyCost>
PropertyReplicator::costliestProperties(std::size_t howMany) const
{
	std::vector<PropertyCost> all;
	all.reserve(m_costs.size());
	for (const auto& [name, cost] : m_costs) { (void)name; all.push_back(cost); }
	// By bytes, then by name — the tie-break is there so the overlay does not
	// reshuffle two equally expensive properties every frame.
	std::sort(all.begin(), all.end(), [](const PropertyCost& a, const PropertyCost& b) {
		if (a.bytes != b.bytes) return a.bytes > b.bytes;
		return a.name < b.name;
	});
	if (all.size() > howMany) all.resize(howMany);
	return all;
}

const std::vector<std::string>* PropertyReplicator::namesOf(std::uint32_t netId) const
{
	const auto it = m_tables.find(netId);
	return it == m_tables.end() ? nullptr : &it->second.names;
}
