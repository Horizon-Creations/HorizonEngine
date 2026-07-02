#pragma once
#include <Scripting/ScriptEngine.h>
#include <Scripting/ScriptTypes.h>
#include <HorizonScene/PyScriptBackend.h>
#include <entt/entt.hpp>
#include <memory>
#include <string>
#include <unordered_map>

class HorizonWorld;
class PhysicsWorld;

// Hosts the per-language script backends (Lua via ScriptEngine, Python via
// PyScriptBackend) and binds them to a HorizonWorld, exposing the identical
// `horizon` API to every language (both backends marshal to the shared
// ScriptApi). Calls are routed per script language:
//   - name-keyed calls (load/props/hot-reload) go to whichever backend owns the
//     name; loadScript picks the backend from the passed ScriptLanguage.
//   - the returned InstanceId carries the language in its high byte (Lua == 0,
//     so Lua-only ids stay bit-identical); per-instance calls decode it and
//     dispatch to the right backend.
//
// API available to scripts (Lua `horizon.*` table / Python `import horizon`):
//   log(msg), getName(id)→str, get/setPosition(id[,x,y,z]),
//   get/setRotation (Euler degrees), get/setScale, spawn(parentId,name)→id,
//   destroy(id), setVelocity(id,vx,vy,vz), isGrounded(id)→bool.
//
// Each instance is bound to its entity (self.entityId in Lua / self.entity_id
// in Python) set to the owning entity's raw handle.
class ScriptContext
{
public:
    explicit ScriptContext(HorizonWorld& world);
    ~ScriptContext() = default;

    ScriptContext(const ScriptContext&)            = delete;
    ScriptContext& operator=(const ScriptContext&) = delete;

    // Load a named script in the given language (Lua by default). Python routes
    // to the CPython backend when the engine was built with it; otherwise fails.
    bool loadScript(const std::string& name, const std::string& source,
                    ScriptLanguage lang = ScriptLanguage::Lua);

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

    using InstanceId = ScriptEngine::InstanceId; // == IScriptBackend::InstanceId

    // Language lives in the high byte of the public InstanceId. Lua == 0 keeps
    // existing ids untouched; backends store instances under their own raw ids.
    static constexpr int kLangShift = 56;
    static InstanceId tagId(InstanceId raw, ScriptLanguage lang)
    { return raw | (static_cast<InstanceId>(static_cast<uint8_t>(lang)) << kLangShift); }
    static InstanceId rawId(InstanceId id)
    { return id & ((static_cast<InstanceId>(1) << kLangShift) - 1); }
    static ScriptLanguage langOf(InstanceId id)
    { return static_cast<ScriptLanguage>(static_cast<uint8_t>(id >> kLangShift)); }

    // Backend selection: by id (per-instance calls) or by name (name-keyed calls).
    IScriptBackend* backendForId(InstanceId id);
    IScriptBackend* backendForName(const std::string& name);

    HorizonWorld* m_world;
    PhysicsWorld* m_physicsWorld = nullptr;
    ScriptEngine  m_engine;                        // Lua
    std::unique_ptr<PyScriptBackend> m_py;         // Python (null if unavailable)
    IScriptBackend* m_lastBackend = &m_engine;     // whose lastError() to report
};
