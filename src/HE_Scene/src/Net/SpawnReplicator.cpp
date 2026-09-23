#include "HorizonScene/Net/SpawnReplicator.h"

#include "HorizonScene/Components/NameComponent.h"
#include "HorizonScene/Components/NetworkComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/GameReplication.h"
#include "HorizonScene/Net/NetMessages.h"

#include <Diagnostics/Log.h>

#include <algorithm>
#include <vector>

using namespace HE::Net;
using namespace HE::Net::Game;

namespace
{
	// A class path is an asset path, not a document. Anything longer did not
	// come from our writer, and a client must not be made to allocate for it.
	constexpr std::size_t kMaxClassPathLength = 512;

	void writeVec3(BitWriter& w, const glm::vec3& v)
	{
		w.writeFloat(v.x); w.writeFloat(v.y); w.writeFloat(v.z);
	}
	bool readVec3(BitReader& r, glm::vec3& v)
	{
		return r.readFloat(v.x) && r.readFloat(v.y) && r.readFloat(v.z);
	}
}

SpawnReplicator::SpawnReplicator(NetSession* net, NetRole role, GameReplication* replication)
	: m_net(net), m_role(role), m_rep(replication)
{
	if (!m_net) return;

	// Only a client is told what exists. A host that accepted a spawn message
	// would be letting a peer create entities in the authoritative world, which
	// is the whole thing this topology exists to prevent.
	if (isAuthority()) return;

	m_net->on(kMsgBind,    [this](ConnectionId, BitReader& r) { handleBind(r); });
	m_net->on(kMsgSpawn,   [this](ConnectionId, BitReader& r) { handleSpawn(r); });
	m_net->on(kMsgDespawn, [this](ConnectionId, BitReader& r) { handleDespawn(r); });
}

bool SpawnReplicator::isAuthority() const
{
	return m_role == NetRole::Server || m_role == NetRole::Host;
}

void SpawnReplicator::clear()
{
	m_binds.clear();
	m_spawns.clear();
}

// ── Host ─────────────────────────────────────────────────────────────────────

int SpawnReplicator::bindSceneEntities()
{
	if (!isAuthority() || !m_world || !m_rep) return 0;

	auto& reg = m_world->registry();
	int registered = 0;

	// Collected first, then registered: registerEntity writes NetworkComponent,
	// and mutating a component while iterating its own view is the kind of thing
	// that works until the pool happens to reallocate.
	std::vector<Entity> candidates;
	for (const Entity e : reg.view<NetworkComponent>())
	{
		const auto& nc = reg.get<NetworkComponent>(e);
		if (!nc.replicates) continue;
		if (nc.netId != 0) continue;   // already in this session
		candidates.push_back(e);
	}

	for (const Entity e : candidates)
	{
		const HE::UUID uuid = m_world->entityId(e);
		if (uuid.hi == 0 && uuid.lo == 0)
		{
			// Nothing to bind against: the client resolves a bind by scene uuid,
			// and an entity with none cannot be named on the other side. A
			// runtime spawn is the path for those, not this walk.
			const auto* name = reg.try_get<NameComponent>(e);
			HE_LOG_WARN(Replication,
			               "Entity '%s' has Replicates on but no scene id; not replicated. "
			               "Runtime-created entities replicate through Create Object, not the scene walk.",
			               name ? name->name.c_str() : "?");
			continue;
		}

		const std::uint32_t netId = m_rep->registerEntity(e);
		if (netId == 0) continue;
		m_binds[netId] = uuid;
		++registered;

		// Somebody may already be in the session when a scene streams in.
		for (const ConnectionId conn : m_net->connections())
			sendBind(conn, netId, uuid);
	}

	HE_LOG_INFO(Replication, "Session walk: %d replicated scene entities", registered);
	return registered;
}

std::uint32_t SpawnReplicator::notifySpawned(Entity entity, const std::string& classPath,
                                             PlayerId owner)
{
	if (!isAuthority() || !m_world || !m_rep) return 0;
	if (!m_world->registry().valid(entity)) return 0;

	auto& reg = m_world->registry();

	// Where the host has it, which is the only pose there is.
	glm::vec3 position { 0.0f };
	glm::vec3 rotationEuler { 0.0f };
	if (const auto* tc = reg.try_get<TransformComponent>(entity))
	{
		position      = tc->position;
		rotationEuler = tc->rotation;
	}
	// A spawned thing that is meant to replicate says so the same way an
	// authored one does — through the component — so the two paths cannot drift
	// into different answers for "is this replicated".
	auto& nc = reg.get_or_emplace<NetworkComponent>(entity);
	if (!nc.replicates) return 0;

	const std::uint32_t netId = m_rep->registerEntity(entity, owner);
	if (netId == 0) return 0;

	SpawnRecord rec;
	rec.entity    = entity;
	rec.classPath = classPath;
	rec.owner     = owner;
	rec.position  = position;
	rec.rotation  = rotationEuler;
	m_spawns[netId] = rec;

	for (const ConnectionId conn : m_net->connections())
		sendSpawn(conn, netId, rec);

	HE_LOG_DEBUG(Replication, "Spawned '%s' as net id %u (owner %u)",
	             classPath.c_str(), netId, owner);
	return netId;
}

void SpawnReplicator::notifyDespawned(Entity entity)
{
	if (!isAuthority() || !m_world) return;
	auto& reg = m_world->registry();
	const auto* nc = reg.try_get<NetworkComponent>(entity);
	if (!nc || nc->netId == 0) return;
	notifyDespawnedById(nc->netId);
}

void SpawnReplicator::notifyDespawnedById(std::uint32_t netId)
{
	if (!isAuthority() || netId == 0) return;

	// Not "if we know it": a client may hold an id this side has already
	// forgotten, and telling it twice costs one message where not telling it
	// leaves a ghost standing in its world forever.
	BitWriter w;
	w.writeUInt32(netId);
	if (m_net) m_net->broadcast(kMsgDespawn, w, SendMode::ReliableOrdered);
	++m_stats.despawnsSent;

	m_binds.erase(netId);
	m_spawns.erase(netId);

	if (m_rep)
	{
		const Entity e = m_rep->entityOf(netId);
		if (e != entt::null) m_rep->unregisterEntity(e);
	}
}

void SpawnReplicator::sendWorldTo(ConnectionId conn)
{
	if (!isAuthority() || !m_net) return;

	// Deterministic order for the same reason the baseline sorts: a joining
	// client's first hundred messages are the ones a test reads one by one.
	std::vector<std::uint32_t> ids;
	ids.reserve(m_binds.size());
	for (const auto& [netId, uuid] : m_binds) ids.push_back(netId);
	std::sort(ids.begin(), ids.end());
	for (const std::uint32_t netId : ids) sendBind(conn, netId, m_binds[netId]);

	ids.clear();
	ids.reserve(m_spawns.size());
	for (const auto& [netId, rec] : m_spawns) ids.push_back(netId);
	std::sort(ids.begin(), ids.end());
	for (const std::uint32_t netId : ids) sendSpawn(conn, netId, m_spawns[netId]);

	HE_LOG_DEBUG(Replication, "World to connection %u: %zu binds, %zu spawns",
	             conn, m_binds.size(), m_spawns.size());
}

void SpawnReplicator::sendBind(ConnectionId conn, std::uint32_t netId, const HE::UUID& uuid)
{
	BitWriter w;
	w.writeUInt32(netId);
	w.writeUInt64(uuid.hi);
	w.writeUInt64(uuid.lo);
	m_net->send(conn, kMsgBind, w, SendMode::ReliableOrdered);
	++m_stats.bindsSent;
}

void SpawnReplicator::writeSpawn(BitWriter& w, std::uint32_t netId, const SpawnRecord& rec) const
{
	w.writeUInt32(netId);
	w.writeUInt32(rec.owner);
	w.writeString(rec.classPath);
	writeVec3(w, rec.position);
	writeVec3(w, rec.rotation);
}

void SpawnReplicator::sendSpawn(ConnectionId conn, std::uint32_t netId, const SpawnRecord& rec)
{
	BitWriter w;
	writeSpawn(w, netId, rec);
	m_net->send(conn, kMsgSpawn, w, SendMode::ReliableOrdered);
	++m_stats.spawnsSent;
}

// ── Client ───────────────────────────────────────────────────────────────────

void SpawnReplicator::handleBind(BitReader& r)
{
	std::uint32_t netId = 0;
	HE::UUID uuid;
	if (!r.readUInt32(netId) || !r.readUInt64(uuid.hi) || !r.readUInt64(uuid.lo)) return;
	if (netId == 0) return;

	++m_stats.bindsReceived;

	if (!m_world || !m_rep)
	{
		++m_stats.bindsUnresolved;
		return;
	}

	const Entity e = m_world->findByEntityId(uuid);
	if (e == entt::null)
	{
		// The two peers are not looking at the same scene. Worth a warning and
		// not a disconnect: the rest of the session still works, and the host's
		// scene message (step 5) is the mechanism that fixes it.
		++m_stats.bindsUnresolved;
		HE_LOG_WARN(Replication,
		               "Bind for net id %u names a scene entity this peer does not have",
		               netId);
		return;
	}

	m_rep->adoptEntity(e, netId);
	m_binds[netId] = uuid;
}

void SpawnReplicator::handleSpawn(BitReader& r)
{
	std::uint32_t netId = 0;
	std::uint32_t owner = 0;
	std::string   classPath;
	glm::vec3     position { 0.0f };
	glm::vec3     rotation { 0.0f };
	if (!r.readUInt32(netId) || !r.readUInt32(owner) || !r.readString(classPath) ||
	    !readVec3(r, position) || !readVec3(r, rotation))
		return;
	if (netId == 0) return;
	if (classPath.size() > kMaxClassPathLength)
	{
		HE_LOG_WARN(Replication, "Spawn for net id %u has an implausible class path (%zu bytes); dropped",
		               netId, classPath.size());
		return;
	}

	++m_stats.spawnsReceived;

	// A duplicate would build a second copy of something already standing here.
	if (m_spawns.count(netId) != 0) return;

	if (!m_spawn)
	{
		++m_stats.spawnsRefused;
		if (!m_spawnServiceLogged)
		{
			m_spawnServiceLogged = true;
			HE_LOG_WARN(Replication,
			               "Spawn of '%s' ignored: this peer has no object-creation service bound. "
			               "Replicated spawns need the application's Create Object path.",
			               classPath.c_str());
		}
		return;
	}

	const Entity e = m_spawn(classPath, position, rotation, owner);
	if (e == entt::null)
	{
		++m_stats.spawnsRefused;
		HE_LOG_WARN(Replication, "Spawn of '%s' (net id %u) produced no entity",
		               classPath.c_str(), netId);
		return;
	}

	if (m_world)
	{
		auto& reg = m_world->registry();
		if (reg.valid(e))
		{
			// The pose travels because the class's BeginPlay must already see
			// where it stands (EntityHost::spawn documents why that order is not
			// negotiable) — but a spawn service that placed it differently, or
			// none at all, is corrected here before anything reads it.
			if (auto* tc = reg.try_get<TransformComponent>(e))
			{
				tc->position = position;
				tc->rotation = rotation;
			}
		}
	}

	if (m_rep) m_rep->adoptEntity(e, netId, owner);

	SpawnRecord rec;
	rec.entity    = e;
	rec.classPath = classPath;
	rec.owner     = owner;
	rec.position  = position;
	rec.rotation  = rotation;
	m_spawns[netId] = rec;
}

void SpawnReplicator::handleDespawn(BitReader& r)
{
	std::uint32_t netId = 0;
	if (!r.readUInt32(netId) || netId == 0) return;

	++m_stats.despawnsReceived;

	Entity e = entt::null;
	const auto it = m_spawns.find(netId);
	if (it != m_spawns.end())
	{
		e = it->second.entity;
		m_spawns.erase(it);
	}
	else if (m_rep)
	{
		// A bound SCENE entity. It is not destroyed — it belongs to the scene
		// file and will still be there next session; it simply stops being
		// replicated.
		m_binds.erase(netId);
		const Entity bound = m_rep->entityOf(netId);
		if (bound != entt::null) m_rep->unregisterEntity(bound);
		return;
	}

	if (m_rep)
	{
		const Entity known = m_rep->entityOf(netId);
		if (e == entt::null) e = known;
		if (known != entt::null) m_rep->unregisterEntity(known);
	}

	if (e == entt::null) return;
	if (m_despawn) m_despawn(e);
	else if (m_world && m_world->registry().valid(e)) m_world->destroyEntity(e);
}
