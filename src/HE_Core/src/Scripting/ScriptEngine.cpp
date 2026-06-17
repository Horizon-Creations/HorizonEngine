#include "Scripting/ScriptEngine.h"

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

ScriptEngine::ScriptEngine()
{
    m_L = luaL_newstate();
    luaL_openlibs(m_L);
}

ScriptEngine::~ScriptEngine()
{
    // Release all Lua registry refs
    for (auto& [id, inst] : m_instances)
        luaL_unref(m_L, LUA_REGISTRYINDEX, inst.luaRef);
    for (auto& [name, sc] : m_scripts)
        luaL_unref(m_L, LUA_REGISTRYINDEX, sc.luaRef);
    lua_close(m_L);
}

bool ScriptEngine::loadScript(const std::string& name, const std::string& source)
{
    // Unload any previous version
    if (m_scripts.count(name))
        unloadScript(name);

    // Compile the chunk
    if (luaL_loadstring(m_L, source.c_str()) != LUA_OK)
    {
        m_lastError = lua_tostring(m_L, -1);
        lua_pop(m_L, 1);
        return false;
    }

    // Execute the chunk; it should return a table
    if (!pcall(0, 1))
        return false;

    if (!lua_istable(m_L, -1))
    {
        m_lastError = "Script '" + name + "' must return a table";
        lua_pop(m_L, 1);
        return false;
    }

    int ref = luaL_ref(m_L, LUA_REGISTRYINDEX);
    m_scripts[name] = { ref };
    return true;
}

void ScriptEngine::unloadScript(const std::string& name)
{
    auto it = m_scripts.find(name);
    if (it == m_scripts.end()) return;

    // Destroy instances that use this script
    for (auto iit = m_instances.begin(); iit != m_instances.end(); )
    {
        if (iit->second.scriptName == name)
        {
            luaL_unref(m_L, LUA_REGISTRYINDEX, iit->second.luaRef);
            iit = m_instances.erase(iit);
        }
        else ++iit;
    }

    luaL_unref(m_L, LUA_REGISTRYINDEX, it->second.luaRef);
    m_scripts.erase(it);
}

bool ScriptEngine::isScriptLoaded(const std::string& name) const
{
    return m_scripts.count(name) > 0;
}

ScriptEngine::InstanceId ScriptEngine::createInstance(const std::string& scriptName)
{
    auto sit = m_scripts.find(scriptName);
    if (sit == m_scripts.end())
    {
        m_lastError = "Script '" + scriptName + "' is not loaded";
        return kInvalidInstance;
    }

    // Create a fresh instance table; copy function references from the script table
    lua_rawgeti(m_L, LUA_REGISTRYINDEX, sit->second.luaRef); // script table
    lua_newtable(m_L);                                         // instance table

    // Shallow-copy all keys (mostly functions) from script → instance
    lua_pushnil(m_L);
    while (lua_next(m_L, -3) != 0)
    {
        // stack: script_table, instance_table, key, value
        lua_pushvalue(m_L, -2); // duplicate key
        lua_insert(m_L, -2);    // stack: ..., key(dup), key, value → ..., key(dup), value
        // Actually we want: instance[key] = value
        // stack: script_table, instance_table, key, value
        // We need to rawset(instance_table, key, value)
        // rawset pops key and value
        lua_rawset(m_L, -4); // instance_table[key] = value; pops key+value
    }
    // stack: script_table, instance_table
    lua_remove(m_L, -2); // drop script_table, leave instance_table on top

    int ref = luaL_ref(m_L, LUA_REGISTRYINDEX);

    InstanceId id = m_nextId++;
    m_instances[id] = { ref, scriptName };
    return id;
}

void ScriptEngine::destroyInstance(InstanceId id)
{
    auto it = m_instances.find(id);
    if (it == m_instances.end()) return;
    luaL_unref(m_L, LUA_REGISTRYINDEX, it->second.luaRef);
    m_instances.erase(it);
}

bool ScriptEngine::callOnStart(InstanceId id)
{
    auto it = m_instances.find(id);
    if (it == m_instances.end())
    {
        m_lastError = "Invalid instance id";
        return false;
    }

    lua_rawgeti(m_L, LUA_REGISTRYINDEX, it->second.luaRef); // instance table

    lua_getfield(m_L, -1, "onStart"); // function (or nil)
    if (lua_isnil(m_L, -1))
    {
        lua_pop(m_L, 2); // pop nil + instance
        return true;     // not defined → silent success
    }

    lua_rawgeti(m_L, LUA_REGISTRYINDEX, it->second.luaRef); // self = instance table
    lua_remove(m_L, -3);                                     // remove instance from bottom

    return pcall(1, 0);
}

bool ScriptEngine::callOnUpdate(InstanceId id, float dt)
{
    auto it = m_instances.find(id);
    if (it == m_instances.end())
    {
        m_lastError = "Invalid instance id";
        return false;
    }

    lua_rawgeti(m_L, LUA_REGISTRYINDEX, it->second.luaRef);

    lua_getfield(m_L, -1, "onUpdate");
    if (lua_isnil(m_L, -1))
    {
        lua_pop(m_L, 2);
        return true;
    }

    lua_rawgeti(m_L, LUA_REGISTRYINDEX, it->second.luaRef); // self
    lua_remove(m_L, -3);
    lua_pushnumber(m_L, static_cast<lua_Number>(dt));

    return pcall(2, 0);
}

bool ScriptEngine::exec(const std::string& code)
{
    if (luaL_loadstring(m_L, code.c_str()) != LUA_OK)
    {
        m_lastError = lua_tostring(m_L, -1);
        lua_pop(m_L, 1);
        return false;
    }
    return pcall(0, 0);
}

double ScriptEngine::getGlobalNumber(const std::string& name) const
{
    lua_getglobal(m_L, name.c_str());
    double v = lua_isnumber(m_L, -1) ? lua_tonumber(m_L, -1) : 0.0;
    lua_pop(m_L, 1);
    return v;
}

std::string ScriptEngine::getGlobalString(const std::string& name) const
{
    lua_getglobal(m_L, name.c_str());
    std::string v = lua_isstring(m_L, -1) ? lua_tostring(m_L, -1) : "";
    lua_pop(m_L, 1);
    return v;
}

bool ScriptEngine::pcall(int nargs, int nresults)
{
    if (lua_pcall(m_L, nargs, nresults, 0) != LUA_OK)
    {
        m_lastError = lua_tostring(m_L, -1);
        lua_pop(m_L, 1);
        return false;
    }
    m_lastError.clear();
    return true;
}
