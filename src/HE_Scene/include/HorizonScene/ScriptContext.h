#pragma once
#include <cstdint>
#include <Scripting/ScriptEngine.h>
#include <Scripting/ScriptTypes.h>
#include <Scripting/PythonPluginAbi.h>
#include <Platform/DynLib.h>
#include <entt/entt.hpp>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class HorizonWorld;
class PhysicsWorld;
class ContentManager;

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
    // Out of line: it has to withdraw this context's quit handler from the
    // file-scope copy the CPython plugin reads, or a context that dies before
    // its host leaves a std::function pointing at nothing.
    ~ScriptContext();

    ScriptContext(const ScriptContext&)            = delete;
    ScriptContext& operator=(const ScriptContext&) = delete;

    // Load a named script in the given language (Lua by default). Python routes
    // to the CPython backend when the engine was built with it; otherwise fails.
    bool loadScript(const std::string& name, const std::string& source,
                    HE::ScriptLanguage lang = HE::ScriptLanguage::Lua);

    // Create an instance of a named script bound to an entity.
    // self.entityId is set to the raw entt::entity value.
    //
    // The 2-arg form routes by which backend currently owns the name (Python
    // first) — convenient, but ambiguous if the same name is loaded in both
    // languages. Prefer the 3-arg form, which routes by the caller's known
    // language so two entities can share a moduleName across languages.
    ScriptEngine::InstanceId createInstance(const std::string& scriptName,
                                            entt::entity       entity);
    ScriptEngine::InstanceId createInstance(const std::string& scriptName,
                                            entt::entity       entity,
                                            HE::ScriptLanguage     lang);

    void destroyInstance(ScriptEngine::InstanceId id);

    // ── Bulk start of a scene's ECS scripts ───────────────────────────────
    // entity → live instance, as the hosting app keeps it (game runtime and
    // play-in-editor both map by the raw entt handle).
    using InstanceMap = std::unordered_map<uint32_t, ScriptEngine::InstanceId>;

    // Start ONE entity's enabled ScriptComponent: load its module (once per
    // name+language), create the instance, inject the authored property overrides
    // and fire onStart. Returns kInvalidInstance when the entity is stale, has no
    // enabled ScriptComponent, its script asset is missing/empty, or the backend
    // refused to create the instance — the caller simply skips it.
    ScriptEngine::InstanceId startEntityScript(entt::entity entity, ContentManager& cm);

    // Start every enabled ScriptComponent in the bound world, recording
    // entity → instance in `out`. Returns how many actually started. This is the
    // packaged game's startup path AND the editor's enter-play-mode path — they
    // must stay the same code so a shipped game behaves like PIE.
    int startWorldScripts(ContentManager& cm, InstanceMap& out);

    // Same, restricted to `entities` (an additively loaded zone's fresh entities).
    int startScriptsFor(const std::vector<entt::entity>& entities,
                        ContentManager& cm, InstanceMap& out);

    // Call onStart(self) on the instance.
    bool callOnStart(ScriptEngine::InstanceId id);

    // Call onUpdate(self, dt) on the instance.
    bool callOnUpdate(ScriptEngine::InstanceId id, float dt);

    // Call onCollisionEnter(self, otherEntityId). No-op if not defined.
    bool callOnCollisionEnter(ScriptEngine::InstanceId id, uint32_t otherEntityId);

    // Call onCollisionExit(self, otherEntityId). No-op if not defined.
    bool callOnCollisionExit(ScriptEngine::InstanceId id, uint32_t otherEntityId);

    // Call onBeginOverlap/onEndOverlap(self, otherEntityId) — the TRIGGER pair
    // (snake_case in Python). The collision pair above fires for both kinds of
    // contact, so a script that only knows it loses nothing.
    bool callOnBeginOverlap(ScriptEngine::InstanceId id, uint32_t otherEntityId);
    bool callOnEndOverlap(ScriptEngine::InstanceId id, uint32_t otherEntityId);

    // Call the UI pointer-event handler (onClick / onHoverEnter / onHoverExit,
    // snake_case in Python). No-op if not defined.
    bool callOnUIEvent(ScriptEngine::InstanceId id, UIScriptEvent ev);

    // Hot-reload: recompile script and patch function fields in live instances.
    // Data fields (non-function keys in instance tables) are preserved. The
    // 2-arg form routes by which backend owns the name (ambiguous across
    // languages); prefer the 3-arg form with the script's known language.
    bool hotReloadScript(const std::string& name, const std::string& source);
    bool hotReloadScript(const std::string& name, const std::string& source, HE::ScriptLanguage lang);

    // Inject stored property overrides into a live instance before onStart.
    void injectProperties(ScriptEngine::InstanceId id,
                          const std::unordered_map<std::string, ScriptPropValue>& props);

    // True if the name is loaded in either backend; the 2-arg form checks the
    // specific language (so a Lua-loaded name reads as not-loaded for Python).
    bool   isScriptLoaded(const std::string& name) const;
    bool   isScriptLoaded(const std::string& name, HE::ScriptLanguage lang) const;
    size_t loadedScriptCount() const;
    size_t instanceCount() const;
    const std::string& lastError() const;

    // Provide access to the active PhysicsWorld so scripts can call horizon.raycast.
    // Pass nullptr to disable raycasting (default, and safe for editor prop inspection).
    void setPhysicsWorld(PhysicsWorld* pw);

    // Provide the ContentManager so scripts can call horizon.setMaterialParam /
    // getMaterialParam. Pass nullptr to disable (default) — those calls then no-op.
    void setContentManager(ContentManager* cm);

    // What "quit" means to the host: this is what horizon.app.quit calls, in Lua
    // and in Python. HE_Scene owns no Application, and the answer differs per
    // host — the packaged game leaves its loop, the editor stops play mode — so
    // the host supplies it. Unset (the default) leaves app.quit a logged no-op,
    // which is the right answer for a context with no session to end (editor
    // property inspection, tests).
    void setQuitHandler(std::function<void()> fn);

    // The same handler for a backend that lives in ANOTHER module: the CPython
    // plugin builds its own HE::api::Ctx and never holds a ScriptContext, so it
    // reads the hook from here. Process-wide and last-writer-wins, like the
    // world/physics/content pointers that backend already keeps in file-statics.
    // Empty until a host binds one.
    static const std::function<void()>& hostQuitHandler();

    ScriptEngine& engine() { return m_engine; }

private:
    void registerHorizonApi();

    using InstanceId = ScriptEngine::InstanceId; // == IScriptBackend::InstanceId

    // Language lives in the high byte of the public InstanceId. Lua == 0 keeps
    // existing ids untouched; backends store instances under their own raw ids.
    static constexpr int kLangShift = 56;
    static InstanceId tagId(InstanceId raw, HE::ScriptLanguage lang)
    { return raw | (static_cast<InstanceId>(static_cast<uint8_t>(lang)) << kLangShift); }
    static InstanceId rawId(InstanceId id)
    { return id & ((static_cast<InstanceId>(1) << kLangShift) - 1); }
    static HE::ScriptLanguage langOf(InstanceId id)
    { return static_cast<HE::ScriptLanguage>(static_cast<uint8_t>(id >> kLangShift)); }

    // Backend selection: by id (per-instance calls) or by name (name-keyed calls).
    IScriptBackend* backendForId(InstanceId id);
    IScriptBackend* backendForName(const std::string& name);

    // Brings up the CPython backend from the HorizonPython plugin, if it is on
    // disk. Absent plugin == no Python support, which is an ordinary state.
    void loadPythonPlugin();

    // Destroys the backend through the plugin's own destroy function: it was
    // allocated over there, by that module's allocator.
    // User-provided constructors instead of an NSDMI: GCC refuses to
    // default-construct a unique_ptr whose deleter is a nested class relying
    // on a default member initializer (the _DeleterConstraint trait sees it
    // as not default-constructible; Clang/MSVC accept it).
    struct PyBackendDeleter
    {
        HePythonDestroyFn destroy;
        PyBackendDeleter() noexcept : destroy(nullptr) {}
        explicit PyBackendDeleter(HePythonDestroyFn fn) noexcept : destroy(fn) {}
        void operator()(IScriptBackend* p) const { if (p && destroy) destroy(p); }
    };

    HorizonWorld*   m_world;
    PhysicsWorld*   m_physicsWorld   = nullptr;
    ContentManager* m_contentManager = nullptr;
    // The Lua side reaches this through a registry pointer registered once, so
    // the address must stay put — which it does: the context is neither copyable
    // nor movable, and its lua_State dies with it.
    std::function<void()> m_quitHandler;
    ScriptEngine  m_engine;                        // Lua
    // The backend only; the plugin MODULE is process-wide and outlives every
    // context (ScriptContext.cpp explains why unloading it is not allowed).
    std::unique_ptr<IScriptBackend, PyBackendDeleter> m_py;  // null if unavailable
    IScriptBackend* m_lastBackend = &m_engine;     // whose lastError() to report
};
