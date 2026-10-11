#include "HorizonScene/FloatingOrigin.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/PhysicsWorld.h"
#include "HorizonScene/TransformHierarchy.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/Components/HierarchyComponent.h"
#include "HorizonScene/Components/ParticleSystemComponent.h"
#include "HorizonScene/Components/TrailComponent.h"
#include "HorizonScene/Components/WeatherComponent.h"
#include "HorizonScene/Components/CameraRigComponent.h"
#include "HorizonScene/Components/IkComponent.h"
#include "HorizonScene/Components/NavAgentComponent.h"
#include <Diagnostics/Log.h>
#include <cmath>

namespace HE
{

glm::vec3 floatingOriginShift(const glm::vec3& cameraPos, float radius)
{
	if (!(radius > 0.0f)) return glm::vec3(0.0f);
	if (std::fabs(cameraPos.x) <= radius && std::fabs(cameraPos.y) <= radius &&
	    std::fabs(cameraPos.z) <= radius)
		return glm::vec3(0.0f);
	const auto axis = [radius](float v) { return std::round(v / radius) * radius; };
	return glm::vec3(axis(cameraPos.x), axis(cameraPos.y), axis(cameraPos.z));
}

size_t shiftWorldOrigin(HorizonWorld& world, PhysicsWorld* physics, const glm::vec3& shift)
{
	if (shift == glm::vec3(0.0f)) return 0;
	entt::registry& reg = world.registry();

	// Top-level entities carry world positions; everything below them is
	// relative to its parent and follows through the hierarchy.
	size_t moved = 0;
	if (const auto* root = reg.try_get<HierarchyComponent>(world.rootEntity()))
		for (Entity child : root->children)
			if (auto* tc = reg.try_get<TransformComponent>(child))
			{
				tc->position -= shift;
				++moved;
			}
	// The derived world matrices too, so a system that reads one before the next
	// propagateTransforms sees where things are now, not a kilometre off.
	for (auto [e, tc] : reg.view<TransformComponent>().each())
		tc.worldMatrix[3] -= glm::vec4(shift, 0.0f);
	// That is a matrix written by hand: not the product propagateTransforms would
	// make (the subtraction rounds differently from a fresh product, and an entity
	// that is no root child keeps a shifted matrix its own position never agreed
	// with). The pass only recomputes what it finds changed, and the root children's
	// new positions are found, but the rest of the world has to be rebuilt, not
	// trusted: the next propagateTransforms walks everything. Once per shift, which
	// is a kilometre of travel.
	HE::invalidateWorldMatrices(world);

	if (physics) physics->shiftOrigin(shift);

	// Runtime state that holds world positions of its own.
	for (auto [e, ps] : reg.view<ParticleSystemComponent>().each())
		for (Particle& p : ps.particles) p.position -= shift;
	for (auto [e, trail] : reg.view<TrailComponent>().each())
	{
		for (TrailComponent::Point& p : trail.points) p.worldPos -= shift;
		trail.lastEmitPos -= shift;
	}
	for (auto [e, wx] : reg.view<WeatherComponent>().each())
	{
		for (Particle& p : wx.precip) p.position -= shift;
		wx.gridMinX -= shift.x;
		wx.gridMinZ -= shift.z;
		for (float& h : wx.groundGrid) h -= shift.y;
	}
	for (auto [e, rig] : reg.view<CameraRigComponent>().each())
		rig.pivotLagged -= shift;
	for (auto [e, ik] : reg.view<IkComponent>().each())
	{
		ik.lookAt.targetWorld -= shift;
		ik.lastEntityPos -= shift;
	}
	for (auto [e, agent] : reg.view<NavAgentComponent>().each())
	{
		agent.targetPos  -= shift;
		agent.pathTarget -= shift;
		for (glm::vec3& p : agent.path) p -= shift;
	}

	world.setOrigin(world.origin() + glm::dvec3(shift));
	HE_LOG_INFO(World, "Floating origin: shifted by %.0f/%.0f/%.0f m, origin now %.0f/%.0f/%.0f, "
	                   "%zu top-level entity/-ies",
	            shift.x, shift.y, shift.z, world.origin().x, world.origin().y, world.origin().z, moved);
	return moved;
}

glm::vec3 updateFloatingOrigin(HorizonWorld& world, PhysicsWorld* physics,
                               const glm::vec3& cameraPos, float radius)
{
	const glm::vec3 shift = floatingOriginShift(cameraPos, radius);
	if (shift != glm::vec3(0.0f)) shiftWorldOrigin(world, physics, shift);
	return shift;
}

} // namespace HE
