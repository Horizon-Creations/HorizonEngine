#include "HorizonScene/ScriptApi.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/PhysicsWorld.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/Components/NameComponent.h"
#include <cstdio>

namespace {

entt::entity toEntity(uint32_t v) { return static_cast<entt::entity>(v); }

TransformComponent* transformOf(HorizonWorld& world, uint32_t entityId)
{
	auto& reg = world.registry();
	entt::entity e = toEntity(entityId);
	return reg.valid(e) ? reg.try_get<TransformComponent>(e) : nullptr;
}

} // namespace

namespace ScriptApi
{

void log(const char* message)
{
	std::printf("[Script] %s\n", message ? message : "");
}

std::string getName(HorizonWorld& world, uint32_t entityId)
{
	auto& reg = world.registry();
	entt::entity e = toEntity(entityId);
	if (!reg.valid(e)) return {};
	const auto* nc = reg.try_get<NameComponent>(e);
	return nc ? nc->name : std::string{};
}

glm::vec3 getPosition(HorizonWorld& world, uint32_t entityId)
{
	const auto* t = transformOf(world, entityId);
	return t ? t->position : glm::vec3(0.0f);
}

void setPosition(HorizonWorld& world, uint32_t entityId, const glm::vec3& p)
{
	if (auto* t = transformOf(world, entityId)) { t->position = p; t->dirty = true; }
}

glm::vec3 getRotation(HorizonWorld& world, uint32_t entityId)
{
	const auto* t = transformOf(world, entityId);
	return t ? t->rotation : glm::vec3(0.0f);
}

void setRotation(HorizonWorld& world, uint32_t entityId, const glm::vec3& r)
{
	if (auto* t = transformOf(world, entityId)) { t->rotation = r; t->dirty = true; }
}

glm::vec3 getScale(HorizonWorld& world, uint32_t entityId)
{
	const auto* t = transformOf(world, entityId);
	return t ? t->scale : glm::vec3(1.0f);
}

void setScale(HorizonWorld& world, uint32_t entityId, const glm::vec3& s)
{
	if (auto* t = transformOf(world, entityId)) { t->scale = s; t->dirty = true; }
}

uint32_t spawn(HorizonWorld& world, uint32_t parentId, const std::string& name)
{
	entt::entity parent = toEntity(parentId);
	entt::entity child  = world.createEntity(name.empty() ? "Entity" : name);
	if (world.registry().valid(parent))
		world.reparentEntity(child, parent);
	return static_cast<uint32_t>(child);
}

void destroy(HorizonWorld& world, uint32_t entityId)
{
	entt::entity e = toEntity(entityId);
	if (world.registry().valid(e))
		world.destroyEntity(e);
}

RaycastResult raycast(PhysicsWorld* physics, const glm::vec3& origin,
                      const glm::vec3& dir, float maxDist)
{
	RaycastResult out;
	if (!physics) return out;
	PhysicsWorld::RaycastHit hit = physics->raycast(origin, dir, maxDist);
	if (!hit.hit) return out;
	out.hit      = true;
	out.entityId = hit.entityId;
	out.point    = hit.point;
	out.normal   = hit.normal;
	out.distance = hit.distance;
	return out;
}

void setVelocity(PhysicsWorld* physics, uint32_t entityId, const glm::vec3& v)
{
	if (physics) physics->setCharacterVelocity(entityId, v);
}

bool isGrounded(PhysicsWorld* physics, uint32_t entityId)
{
	return physics && physics->isCharacterGrounded(entityId);
}

} // namespace ScriptApi
