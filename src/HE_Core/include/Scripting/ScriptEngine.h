#pragma once
#include "Types/Defines.h"
#include "Scripting/ScriptTypes.h"
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
class HE_API ScriptEngine
{
public:
    using InstanceId = uint64_t;
    static constexpr InstanceId kInvalidInstance = 0;

    ScriptEngine();
    ~ScriptEngine();

    ScriptEngine(const ScriptEngine&)            = delete;
    ScriptEngine& operator=(const ScriptEngine&) = delete;

    // Compile and store a named script from a Lua source string.
    // Returns false on compile error; check lastError().
    bool loadScript(const std::string& name, const std::string& source);

    // Remove a script and destroy all instances created from it.
    void unloadScript(const std::string& name);

    bool   isScriptLoaded(const std::string& name) const;
    size_t loadedScriptCount() const { return m_scripts.size(); }

    // Create an instance (own table) of a named script.
    // Returns kInvalidInstance if the script is not loaded.
    InstanceId createInstance(const std::string& scriptName);

    // Destroy an instance and remove it from the Lua registry.
    void destroyInstance(InstanceId id);

    size_t instanceCount() const { return m_instances.size(); }

    // Call script.onStart(self). No-op (returns true) if not defined.
    bool callOnStart(InstanceId id);

    // Call script.onUpdate(self, dt). No-op (returns true) if not defined.
    bool callOnUpdate(InstanceId id, float dt);

    // Last error string from any failed compile or call.
    const std::string& lastError() const { return m_lastError; }

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
                          const std::unordered_map<std::string, ScriptPropValue>& props);

    // Read the M.properties table of a loaded script and return its declared properties.
    // Returns an empty vector if the script has no M.properties table.
    std::vector<ScriptPropDef> getScriptProperties(const std::string& name) const;

    // Recompile a loaded script and patch function fields in all live instances.
    // Data fields (non-function keys) in instance tables are preserved.
    // Returns false (and leaves state unchanged) if the new source fails to compile.
    bool hotReloadScript(const std::string& name, const std::string& source);

    // Direct lua_State access for advanced binding (ScriptContext in HE_Scene uses this).
    lua_State* state() { return m_L; }

private:
    // Returns false and sets m_lastError on Lua error.
    bool pcall(int nargs, int nresults);

    struct ScriptRef  { int luaRef = -1; };   // LUA_NOREF sentinel
    struct InstanceRef { int luaRef = -1; std::string scriptName; };

    lua_State*  m_L = nullptr;
    std::unordered_map<std::string, ScriptRef>   m_scripts;
    std::unordered_map<InstanceId, InstanceRef>  m_instances;
    InstanceId  m_nextId = 1;
    std::string m_lastError;
};
