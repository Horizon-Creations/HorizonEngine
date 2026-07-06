#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <string>

class HorizonWorld;
class PhysicsWorld;
class ContentManager;

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

	// Set/get a node-graph material parameter BY NAME on the entity's material at
	// runtime (the MaterialComponent's asset). All 4 vec4 components are written;
	// the shader reads only those its Param node uses, so passing a scalar as
	// (x,0,0,0) is safe. content may be null (no-op / false). Returns false if the
	// entity has no material or the parameter name is unknown.
	bool setMaterialParam(HorizonWorld& world, ContentManager* content,
	                      uint32_t entityId, const std::string& name, const glm::vec4& value);
	glm::vec4 getMaterialParam(HorizonWorld& world, ContentManager* content,
	                           uint32_t entityId, const std::string& name); // default (0,0,0,0)

	// ── In-game UI (entities carrying UI components) ───────────────────────
	// Text of a UITextComponent. Getter returns "" without one.
	void        setUIText(HorizonWorld& world, uint32_t entityId, const std::string& text);
	std::string getUIText(HorizonWorld& world, uint32_t entityId);
	// Primary color: image tint, text color, or button normal color (whichever
	// components exist — image first). Getter prefers the same order.
	void      setUIColor(HorizonWorld& world, uint32_t entityId, const glm::vec4& c);
	glm::vec4 getUIColor(HorizonWorld& world, uint32_t entityId); // default (1,1,1,1)
	// UIElementComponent active flag (hides the whole subtree when false).
	void setUIVisible(HorizonWorld& world, uint32_t entityId, bool visible);
	bool isUIVisible(HorizonWorld& world, uint32_t entityId);
	// UIElementComponent position/size in canvas units.
	void      setUIPosition(HorizonWorld& world, uint32_t entityId, const glm::vec2& p);
	glm::vec2 getUIPosition(HorizonWorld& world, uint32_t entityId);
	void      setUISize(HorizonWorld& world, uint32_t entityId, const glm::vec2& s);
	glm::vec2 getUISize(HorizonWorld& world, uint32_t entityId);
	// Set a node-graph material parameter on the entity's UI IMAGE material
	// (UIImageComponent.materialAssetId) — the UI counterpart of setMaterialParam.
	bool setUIMaterialParam(HorizonWorld& world, ContentManager* content,
	                        uint32_t entityId, const std::string& name, const glm::vec4& value);
}
