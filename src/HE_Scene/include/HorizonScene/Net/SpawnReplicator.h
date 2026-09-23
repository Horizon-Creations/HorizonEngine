#pragma once

// ─── Layer 3a — which entities exist, and where they came from ───────────────
// GameReplication answers "where is net id 12", and only that. It has never
// been able to answer "what IS net id 12" — a client had to be handed matching
// entities by hand, which is exactly why every existing test builds both worlds
// itself (test_game_replication's mirrorEntity). This closes that gap.
//
// Two kinds of replicated entity, because they have two different identities:
//
//   AUTHORED entities are in the scene file both peers loaded. They already
//   exist on the client under the same scene uuid, so nothing is created: a
//   BIND simply says "the entity you know as uuid X is net id 12". That is why
//   the scene uuid travels rather than a class path — instantiating a second
//   copy of something the client already has would double every crate.
//
//   SPAWNED entities came from Create Object at runtime and exist only on the
//   host. A SPAWN carries the class path and the pose, and the client makes
//   one. Its class runs on the client too, with BeginPlay — the plan's §7.5
//   convention ("simulation only with Is Authority") is how a graph tells the
//   two sides apart.
//
// HOW IT REACHES THE CLIENT'S SPAWN PATH. Making an object is the host
// application's job (Ctx::createObject resolves the engine base class, sends
// only Entity classes through EntityHost, and registers a PlayerCharacter with
// the PlayerHost). HE_Scene cannot call any of that, so this takes a callback
// and step 5 binds Ctx::createObject to it. Unbound is an ordinary state: the
// spawn is counted, logged once, and dropped — which is also what makes this
// testable with no application at all.

#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Net/PlayerRoster.h"

#include <Net/NetSession.h>

#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>

class GameReplication;

// No HE_API — HorizonScene exports every symbol (see GameReplication.h).
class SpawnReplicator
{
public:
	// The client's way of making an object of a class, and of destroying one.
	// Returns the entity that was created, or entt::null when it refused.
	// `owner` is the PLAYER the thing belongs to, which is what decides whether
	// the local PlayerHost should hear its input at all (plan §5.4 point 4).
	using SpawnFn = std::function<Entity(const std::string& classPath,
	                                     const glm::vec3& position,
	                                     const glm::vec3& rotationEuler,
	                                     HE::Net::Game::PlayerId owner)>;
	using DespawnFn = std::function<void(Entity entity)>;

	// `replication` must outlive this object: every id this class hands out or
	// adopts is minted there, and a second id space would be two peers
	// disagreeing about what a snapshot refers to.
	SpawnReplicator(HE::Net::NetSession* net, HE::Net::NetRole role,
	                GameReplication* replication);

	void setWorld(HorizonWorld* world) { m_world = world; }
	void setSpawnFunction(SpawnFn fn)     { m_spawn = std::move(fn); }
	void setDespawnFunction(DespawnFn fn) { m_despawn = std::move(fn); }

	// ── Host ─────────────────────────────────────────────────────────────────
	// The registry walk (plan §5.5): every entity carrying a NetworkComponent
	// with `replicates` is registered for the session and recorded as a bind.
	// THIS is what the inspector switch buys — nobody calls registerEntity by
	// hand any more. Returns how many were registered; already-registered
	// entities are left alone, so calling it again after a scene change adds
	// only what is new.
	int bindSceneEntities();

	// A runtime spawn on the host: register the entity and tell every client to
	// make one. Returns the net id, or 0 when the entity cannot be replicated.
	//
	// The POSE is read off the entity rather than passed in. It was a parameter
	// first, and that made it possible for the message to say one thing while
	// the host's own transform said another — which is precisely the bug the
	// first run of the test caught: the spawn arrived on the client at the
	// authored position and was then dragged to the origin by the next
	// snapshot, because the origin was where the host actually was. The host's
	// spawn path has already placed the entity by the time this is called
	// (EntityHost::spawn does it before Construct and BeginPlay); there is only
	// one truth, and it is over there.
	std::uint32_t notifySpawned(Entity entity, const std::string& classPath,
	                            HE::Net::Game::PlayerId owner);
	// The other half: tell every client it is gone, and forget it here. Safe
	// for an entity that was never replicated (it is then a no-op).
	void notifyDespawned(Entity entity);
	void notifyDespawnedById(std::uint32_t netId);

	// Everything a joining client needs, in order: binds first, then spawns.
	// The caller sends the transform baseline AFTER this — the samples need
	// somewhere to land.
	void sendWorldTo(HE::Net::ConnectionId conn);

	// ── Client ───────────────────────────────────────────────────────────────
	// What arrived, for diagnostics and for the tests. Counts, not lists: the
	// entities themselves are reachable through GameReplication::entityOf.
	struct Stats
	{
		std::uint32_t bindsSent        = 0;
		std::uint32_t spawnsSent       = 0;
		std::uint32_t despawnsSent     = 0;
		std::uint32_t bindsReceived    = 0;
		std::uint32_t bindsUnresolved  = 0;   // no entity with that scene uuid here
		std::uint32_t spawnsReceived   = 0;
		std::uint32_t spawnsRefused    = 0;   // no spawn service, or it said no
		std::uint32_t despawnsReceived = 0;
	};
	const Stats& stats() const { return m_stats; }
	void         resetStats() { m_stats = {}; }

	// How many entities this side holds an identity for, by kind.
	std::size_t boundCount()   const { return m_binds.size(); }
	std::size_t spawnedCount() const { return m_spawns.size(); }

	// Drop everything. The session calls this on leave; a second session in the
	// same process must not inherit the first one's ids.
	void clear();

private:
	struct SpawnRecord
	{
		Entity                  entity = entt::null;
		std::string             classPath;
		HE::Net::Game::PlayerId owner = HE::Net::Game::kNoPlayer;
		glm::vec3               position { 0.0f };
		glm::vec3               rotation { 0.0f };
	};

	void handleBind(HE::Net::BitReader& r);
	void handleSpawn(HE::Net::BitReader& r);
	void handleDespawn(HE::Net::BitReader& r);

	void sendBind(HE::Net::ConnectionId conn, std::uint32_t netId, const HE::UUID& uuid);
	void sendSpawn(HE::Net::ConnectionId conn, std::uint32_t netId, const SpawnRecord& rec);
	void writeSpawn(HE::Net::BitWriter& w, std::uint32_t netId, const SpawnRecord& rec) const;

	bool isAuthority() const;

	HE::Net::NetSession* m_net  = nullptr;
	HE::Net::NetRole     m_role = HE::Net::NetRole::None;
	GameReplication*     m_rep  = nullptr;
	HorizonWorld*        m_world = nullptr;

	SpawnFn   m_spawn;
	DespawnFn m_despawn;
	bool      m_spawnServiceLogged = false;

	// Host: what to replay for a late joiner. Client: what it has adopted, so a
	// despawn knows which entity to take down.
	std::unordered_map<std::uint32_t, HE::UUID>      m_binds;
	std::unordered_map<std::uint32_t, SpawnRecord>   m_spawns;

	Stats m_stats;
};
