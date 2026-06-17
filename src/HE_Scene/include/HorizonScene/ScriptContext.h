#pragma once
#include <Scripting/ScriptEngine.h>
#include <entt/entt.hpp>
#include <string>

class HorizonWorld;

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

    bool   isScriptLoaded(const std::string& name) const;
    size_t loadedScriptCount() const;
    size_t instanceCount() const;
    const std::string& lastError() const;

    ScriptEngine& engine() { return m_engine; }

private:
    void registerHorizonApi();

    HorizonWorld* m_world;
    ScriptEngine  m_engine;
};
