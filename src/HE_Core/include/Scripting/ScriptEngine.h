#pragma once
#include "Types/Defines.h"
#include "Scripting/IScriptBackend.h"
#include "Scripting/ScriptTypes.h"
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

struct lua_State;

// Lightweight Lua scripting engine.
//
// Script format (one module table returned from the chunk):
//   local M = {}
//   function M.onStart(self)  ... end
//   function M.onUpdate(self, dt) ... end
//   return M
//
// Usage:
//   ScriptEngine engine;
//   engine.loadScript("player", luaSource);
//   auto id = engine.createInstance("player");
//   engine.callOnStart(id);
//   engine.callOnUpdate(id, 0.016f);
//   engine.destroyInstance(id);
class HE_API ScriptEngine : public IScriptBackend
{
public:
    using InstanceId = IScriptBackend::InstanceId;
    static constexpr InstanceId kInvalidInstance = IScriptBackend::kInvalidInstance;

    ScriptEngine();
    ~ScriptEngine() override;

    ScriptEngine(const ScriptEngine&)            = delete;
    ScriptEngine& operator=(const ScriptEngine&) = delete;

    // Compile and store a named script from a Lua source string.
    // Returns false on compile error; check lastError().
    bool loadScript(const std::string& name, const std::string& source) override;

    // Remove a script and destroy all instances created from it.
    void unloadScript(const std::string& name) override;

    bool   isScriptLoaded(const std::string& name) const override;
    size_t loadedScriptCount() const override { return m_scripts.size(); }

    // Create an instance (own table) of a named script.
    // Returns kInvalidInstance if the script is not loaded.
    InstanceId createInstance(const std::string& scriptName);

    // IScriptBackend variant: create an instance bound to an entity —
    // sets self.entityId in the instance table.
    InstanceId createInstance(const std::string& scriptName, uint32_t entityId) override;

    // Destroy an instance and remove it from the Lua registry.
    void destroyInstance(InstanceId id) override;

    size_t instanceCount() const override { return m_instances.size(); }

    // Call script.onStart(self). No-op (returns true) if not defined.
    bool callOnStart(InstanceId id) override;

    // Call script.onUpdate(self, dt). No-op (returns true) if not defined.
    bool callOnUpdate(InstanceId id, float dt) override;

    // Call script.onCollisionEnter(self, otherEntityId). No-op if not defined.
    bool callOnCollisionEnter(InstanceId id, uint32_t otherEntityId) override;

    // Call script.onCollisionExit(self, otherEntityId). No-op if not defined.
    bool callOnCollisionExit(InstanceId id, uint32_t otherEntityId) override;

    // Call script.onBeginOverlap/onEndOverlap(self, otherEntityId) — the trigger
    // pair. No-op if not defined.
    bool callOnBeginOverlap(InstanceId id, uint32_t otherEntityId) override;
    bool callOnEndOverlap(InstanceId id, uint32_t otherEntityId) override;

    // Call script.onAnimationNotify/…Begin/…End(self, name) — the events an
    // artist put on a clip's timeline. No-op if not defined.
    bool callOnAnimationNotify(InstanceId id, const std::string& name) override;
    bool callOnAnimationNotifyBegin(InstanceId id, const std::string& name) override;
    bool callOnAnimationNotifyEnd(InstanceId id, const std::string& name) override;

    // Call script.onClick/onHoverEnter/onHoverExit(self). No-op if not defined.
    bool callOnUIEvent(InstanceId id, UIScriptEvent ev) override;

    // Call script.onInputPressed/onInputReleased(self, action),
    // script.onInputAxis(self, action, value) and
    // script.onInputAxis2D(self, action, x, y) — the input actions the
    // project's mapping contexts bind. No-op if not defined.
    bool callOnInputPressed(InstanceId id, const std::string& action) override;
    bool callOnInputReleased(InstanceId id, const std::string& action) override;
    bool callOnInputAxis(InstanceId id, const std::string& action, float value) override;
    bool callOnInputAxis2D(InstanceId id, const std::string& action, float x, float y) override;

    // Call script.onTimer(self, handle) — a horizon.timer.after/every came due.
    // No-op if not defined.
    bool callOnTimer(InstanceId id, int handle) override;
    // Call script.onCheatDetected(self, reportId) — the anti-cheat made a report
    // (or the host sent this client a notice). No-op if not defined.
    bool callOnCheatDetected(InstanceId id, int reportId) override;
    // Call script.onPlayerJoined(self, player) and its five siblings — the
    // multiplayer session's lifecycle. No-op if not defined.
    bool callOnNetEvent(InstanceId id, NetScriptEvent ev, int arg) override;

    // Last error string from any failed compile or call.
    const std::string& lastError() const override { return m_lastError; }

    // Execute a raw Lua string in the global state (useful for tests/REPL).
    bool exec(const std::string& code);

    // Read a global number from the Lua state (useful for tests).
    double getGlobalNumber(const std::string& name) const;

    // Read a global string from the Lua state.
    std::string getGlobalString(const std::string& name) const;

    // Set a typed field in the instance table.
    void setInstanceField(InstanceId id, const std::string& key, double value);
    void setInstanceField(InstanceId id, const std::string& key, bool value);
    void setInstanceField(InstanceId id, const std::string& key, const std::string& value);

    // Inject all properties from a map into the instance table (sets each field).
    void injectProperties(InstanceId id,
                          const std::unordered_map<std::string, ScriptPropValue>& props) override;

    // Read the M.properties table of a loaded script and return its declared properties.
    // Returns an empty vector if the script has no M.properties table.
    std::vector<ScriptPropDef> getScriptProperties(const std::string& name) const override;

    // Recompile a loaded script and patch function fields in all live instances.
    // Data fields (non-function keys) in instance tables are preserved.
    // Returns false (and leaves state unchanged) if the new source fails to compile.
    bool hotReloadScript(const std::string& name, const std::string& source) override;

    // Direct lua_State access for advanced binding (ScriptContext in HE_Scene uses this).
    lua_State* state() { return m_L; }

    // ── Calling a handler whose ARGUMENTS this class cannot marshal ──────────
    // Every callOn… above takes numbers and strings, which HE_Core can push by
    // itself. OnRep_<Var> (docs/gameplay-replication-plan.md §6.4) takes a
    // HorizonCode::Value, and the one place that knows how to put one of those
    // on a Lua stack — as a struct table, as a map with its `__keys` sidecar —
    // is ScriptContext in HE_Scene, together with the reader that takes it back.
    // A second implementation down here would be a second answer to the same
    // question, and the two would drift the first time a type was added.
    //
    // So the CALLER pushes: `pushArgs` receives the state with the function and
    // `self` already on it, pushes however many arguments it likes, and returns
    // how many. False only on a Lua error (see lastError); a script that does
    // not define `fn` is an ordinary no-op and answers true, like every handler
    // above.
    using ArgPusher = std::function<int(lua_State*)>;
    bool callInstanceMethod(InstanceId id, const char* fn, const ArgPusher& pushArgs);
    // Does this instance define `fn` at all? callInstanceMethod deliberately
    // answers TRUE for a method that is not there ("nothing to call went
    // wrong" is not an error), which is right for a hook and wrong for a
    // remote call: the RPC router has to know whether Lua took it or whether
    // the next frontend should be asked (NetEvents::dispatchRpc).
    bool hasInstanceMethod(InstanceId id, const char* fn);

private:
    // Compile `source` as a chunk named `name` and leave it on the stack.
    // Returns false and sets m_lastError on a compile error.
    bool loadChunk(const std::string& name, const std::string& source);
    // Returns false and sets m_lastError on Lua error.
    bool pcall(int nargs, int nresults);

    // Shared body of the three notify handlers above: they differ only in the
    // Lua method name they look for.
    bool callNotifyMethod(InstanceId id, const char* method, const std::string& name);

    struct ScriptRef  { int luaRef = -1; };   // LUA_NOREF sentinel
    struct InstanceRef { int luaRef = -1; std::string scriptName; };

    lua_State*  m_L = nullptr;
    std::unordered_map<std::string, ScriptRef>   m_scripts;
    std::unordered_map<InstanceId, InstanceRef>  m_instances;
    InstanceId  m_nextId = 1;
    std::string m_lastError;
};
