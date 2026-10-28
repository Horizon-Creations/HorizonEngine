#include "HorizonScene/CellPhysics.h"
#include "HorizonScene/CellStreamer.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/PhysicsWorld.h"

namespace HE
{

int cellSliceBodies(PhysicsWorld& physics, HorizonWorld& world, const std::vector<entt::entity>& created)
{
	std::vector<uint32_t> ids;
	ids.reserve(created.size());
	for (entt::entity e : created) ids.push_back(static_cast<uint32_t>(e));
	return physics.addEntities(world, ids);
}

int cellUnloadBodies(PhysicsWorld& physics, HorizonWorld& world, entt::entity root)
{
	return physics.removeEntityTree(world, static_cast<uint32_t>(root), /*requeueJoints=*/true);
}

bool cellBodiesFit(const PhysicsWorld& physics, uint32_t bodies)
{
	constexpr size_t reserve = PhysicsWorld::kMaxBodies / 10 * 9;
	return physics.bodyCount() + bodies <= reserve;
}

bool cellHolds(const CellStreamer& streamer, const HorizonWorld& world, const glm::vec3& worldPosition)
{
	return streamer.holdsAt(glm::dvec3(worldPosition) + world.origin());
}

} // namespace HE
