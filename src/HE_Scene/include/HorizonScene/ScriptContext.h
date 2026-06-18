#pragma once
#include <Scripting/ScriptEngine.h>
#include <Scripting/ScriptTypes.h>
#include <entt/entt.hpp>
#include <string>
#include <unordered_map>

class HorizonWorld;
class PhysicsWorld;

// Wraps a ScriptEngine and binds it to a HorizonWorld, exposing the engine
// API (Transform read/write, spawn/destroy, log) to Lua via a `horizon` table.
//
// Lua API available to scripts:
//   horizon.log(msg)
//   horizon.getName(entityId)           → string
//   horizon.getPosition(entityId)       → x, y, z
//   horizon.setPosition(entityId, x, y, z)
//   horizon.getRotation(entityId)       → x, y, z  (Euler degrees)
//   horizon.setRotation(entityId, x, y, z)
//   horizon.getScale(entityId)          → x, y, z
//   horizon.setScale(entityId, x, y, z)
//   horizon.spawn(parentId, name)       → entityId
//   horizon.destroy(entityId)
//   horizon.setVelocity(entityId, vx, vy, vz)   (character controller only)
//   horizon.isGrounded(entityId)        → bool   (character controller only)
//
// Each script instance has self.entityId set to the owning entity's raw handle.
class ScriptContext
{
public:
    explicit ScriptContext(HorizonWorld& world);
    ~ScriptContext() = default;

    ScriptContext(const ScriptContext&)            = delete;
    ScriptContext& operator=(const ScriptContext&) = delete;

    // Load a named Lua script (delegates to ScriptEngine).
    bool loadScript(const std::string& name, const std::string& source);

    // Create an instance of a named script bound to an entity.
    // self.entityId is set to the raw entt::entity value.
    ScriptEngine::InstanceId createInstance(const std::string& scriptName,
                                            entt::entity       entity);

    void destroyInstance(ScriptEngine::InstanceId id);

    // Call onStart(self) on the instance.
    bool callOnStart(ScriptEngine::InstanceId id);

    // Call onUpdate(self, dt) on the instance.
    bool callOnUpdate(ScriptEngine::InstanceId id, float dt);

    // Call onCollisionEnter(self, otherEntityId). No-op if not defined.
    bool callOnCollisionEnter(ScriptEngine::InstanceId id, uint32_t otherEntityId);

    // Call onCollisionExit(self, otherEntityId). No-op if not defined.
    bool callOnCollisionExit(ScriptEngine::InstanceId id, uint32_t otherEntityId);

    // Hot-reload: recompile script and patch function fields in live instances.
    // Data fields (non-function keys in instance tables) are preserved.
    bool hotReloadScript(const std::string& name, const std::string& source);

    // Inject stored property overrides into a live instance before onStart.
    void injectProperties(ScriptEngine::InstanceId id,
                          const std::unordered_map<std::string, ScriptPropValue>& props);

    bool   isScriptLoaded(const std::string& name) const;
    size_t loadedScriptCount() const;
    size_t instanceCount() const;
    const std::string& lastError() const;

    // Provide access to the active PhysicsWorld so scripts can call horizon.raycast.
    // Pass nullptr to disable raycasting (default, and safe for editor prop inspection).
    void setPhysicsWorld(PhysicsWorld* pw);

    ScriptEngine& engine() { return m_engine; }

private:
    void registerHorizonApi();

    HorizonWorld* m_world;
    PhysicsWorld* m_physicsWorld = nullptr;
    ScriptEngine  m_engine;
};
