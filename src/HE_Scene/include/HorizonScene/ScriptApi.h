#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <string>

class HorizonWorld;
class PhysicsWorld;

// Language-neutral gameplay-script API — the single implementation behind the
// `horizon` module of every scripting backend (Lua today, Python next). Each
// backend is only a thin marshalling shim around these functions, which keeps
// the exposed API surface identical across languages by construction.
//
// Entity ids are the raw entt handle as uint32 (what scripts see as
// self.entityId / self.entity_id). Invalid ids are tolerated: getters return
// neutral defaults, setters are no-ops.
namespace ScriptApi
{
	void        log(const char* message);
	std::string getName(HorizonWorld& world, uint32_t entityId);

	glm::vec3 getPosition(HorizonWorld& world, uint32_t entityId); // default (0,0,0)
	void      setPosition(HorizonWorld& world, uint32_t entityId, const glm::vec3& p);
	glm::vec3 getRotation(HorizonWorld& world, uint32_t entityId); // Euler degrees, default (0,0,0)
	void      setRotation(HorizonWorld& world, uint32_t entityId, const glm::vec3& r);
	glm::vec3 getScale(HorizonWorld& world, uint32_t entityId);    // default (1,1,1)
	void      setScale(HorizonWorld& world, uint32_t entityId, const glm::vec3& s);

	// Create a named entity, reparented under parentId when valid. Returns the id.
	uint32_t spawn(HorizonWorld& world, uint32_t parentId, const std::string& name);
	void     destroy(HorizonWorld& world, uint32_t entityId);

	struct RaycastResult {
		bool      hit = false;
		uint32_t  entityId = 0;
		glm::vec3 point{0.0f};
		glm::vec3 normal{0.0f};
		float     distance = 0.0f;
	};
	// physics may be null (raycasting disabled) → miss.
	RaycastResult raycast(PhysicsWorld* physics, const glm::vec3& origin,
	                      const glm::vec3& dir, float maxDist);

	// Character-controller helpers; physics may be null (no-op / false).
	void setVelocity(PhysicsWorld* physics, uint32_t entityId, const glm::vec3& v);
	bool isGrounded(PhysicsWorld* physics, uint32_t entityId);
}
