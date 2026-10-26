#include "Scripting/ScriptEngine.h"
#include <charconv>
#include <cstdint>
#include <cstdlib>

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

namespace HE
{
// The script log tag (declared in ScriptTypes.h). It is defined here because
// ScriptTypes.h is a header with no .cpp of its own and this is the only
// translation unit that already belongs to it — what matters is that there is
// exactly ONE, so every module that links HorizonCore shares the storage
// instead of getting a private copy behind a dllimport.
//
// No mutex: the tag is written once, by the application, before any script
// runs (project load in the editor), and only read afterwards. Same shape as
// the other process-wide script settings (EngineApi.cpp's sandbox root and
// default save template).
static std::string& scriptLogTagStorage()
{
	static std::string tag;   // empty = no prefix, see ScriptTypes.h
	return tag;
}

void setScriptLogTag(const std::string& tag) { scriptLogTagStorage() = tag; }

const std::string& scriptLogTag() { return scriptLogTagStorage(); }

std::string scriptLogLine(const std::string& message)
{
	// Untagged is not "tag plus empty string": with no tag the line must come
	// out exactly as it went in, so a project that never set one keeps the log
	// it always had.
	const std::string& tag = scriptLogTagStorage();
	return tag.empty() ? message : tag + message;
}

bool parseScriptErrorLocation(const std::string& message, ScriptErrorLocation& out)
{
	// Looking for `<script>:<line>:` — the first one. The wrapper text around it
	// ("Compile error in script 'x': ", "... failed in onUpdate(): ") has colons
	// of its own, but none of them is followed by digits and another colon, so
	// the scan can simply walk every ':' and test what follows.
	const std::size_t n = message.size();
	for (std::size_t colon = message.find(':'); colon != std::string::npos;
	     colon = message.find(':', colon + 1))
	{
		// digits, then the closing colon
		std::size_t p = colon + 1;
		while (p < n && message[p] >= '0' && message[p] <= '9') ++p;
		if (p == colon + 1 || p >= n || message[p] != ':') continue;

		// the name: the run of non-blank characters in front of the colon
		std::size_t start = colon;
		while (start > 0 && message[start - 1] != ' ' && message[start - 1] != '\t' &&
		       message[start - 1] != '\n' && message[start - 1] != '\'' &&
		       message[start - 1] != '"' && message[start - 1] != '(')
			--start;
		if (start == colon) continue;
		// A name that is all digits is not a script: "12:34:56" is a clock.
		bool allDigits = true;
		for (std::size_t i = start; i < colon && allDigits; ++i)
			allDigits = message[i] >= '0' && message[i] <= '9';
		if (allDigits) continue;

		// A digit run longer than an int is no line of any script: refuse it before
		// `out` is touched (atoi would be undefined behaviour on it).
		int line = 0;
		const char* digits = message.data() + colon + 1;
		if (std::from_chars(digits, message.data() + p, line).ec != std::errc()) return false;

		out.script = message.substr(start, colon - start);
		out.line   = line;
		return out.line > 0;
	}
	return false;
}
} // namespace HE

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
    // The new source runs first: an edit with a syntax error must not take the
    // working script and all of its live instances with it.
    if (!runModuleChunk(name, source))
        return false;

    // Unload any previous version
    if (m_scripts.count(name))
        unloadScript(name);

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
        // The key has to be duplicated before the store: lua_rawset consumes a
        // key/value pair, but lua_next needs the original key still on top to
        // find the next entry. Push a copy and slide it under the value so
        // rawset eats the copy and leaves the original behind.
        lua_pushvalue(m_L, -2); // key copy on top
        lua_insert(m_L, -2);    // stack: script, instance, key, key(copy), value
        lua_rawset(m_L, -4);    // instance_table[key] = value; pops copy+value
    }
    // stack: script_table, instance_table
    lua_remove(m_L, -2); // drop script_table, leave instance_table on top

    int ref = luaL_ref(m_L, LUA_REGISTRYINDEX);

    InstanceId id = m_nextId++;
    m_instances[id] = { ref, scriptName };
    return id;
}

ScriptEngine::InstanceId ScriptEngine::createInstance(const std::string& scriptName,
                                                      uint32_t entityId)
{
    const InstanceId id = createInstance(scriptName);
    if (id != kInvalidInstance)
        setInstanceField(id, "entityId", static_cast<double>(entityId));
    return id;
}

void ScriptEngine::destroyInstance(InstanceId id)
{
    auto it = m_instances.find(id);
    if (it == m_instances.end()) return;
    luaL_unref(m_L, LUA_REGISTRYINDEX, it->second.luaRef);
    m_instances.erase(it);
}

// The instance behind `id`, or nullptr with m_lastError set: the shared first
// line of every callOnX below (hasInstanceMethod is only a question and leaves
// the error alone).
ScriptEngine::InstanceRef* ScriptEngine::requireInstance(InstanceId id)
{
    auto it = m_instances.find(id);
    if (it != m_instances.end()) return &it->second;
    m_lastError = "Invalid instance id";
    return nullptr;
}

// Shared prologue of every callOnX below: push instance.<method> and the `self`
// argument, leaving the stack as [function, self] ready for pcall(1 + extra args).
// Returns false with a CLEAN stack when the method is not defined — an undefined
// callback is a silent no-op in every one of them, never an error (m_lastError is
// deliberately left untouched in that case, as it always has been).
static bool pushInstanceMethod(lua_State* L, int instanceRef, const char* method)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, instanceRef); // instance table
    lua_getfield(L, -1, method);                    // function (or nil)
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 2); // pop nil + instance
        return false;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, instanceRef); // self = instance table
    lua_remove(L, -3);                              // remove instance from bottom
    return true;
}

bool ScriptEngine::callOnStart(InstanceId id)
{
    InstanceRef* inst = requireInstance(id);
    if (!inst) return false;

    if (!pushInstanceMethod(m_L, inst->luaRef, "onStart")) return true; // not defined
    return pcall(1, 0);
}

bool ScriptEngine::callOnUpdate(InstanceId id, float dt)
{
    InstanceRef* inst = requireInstance(id);
    if (!inst) return false;

    if (!pushInstanceMethod(m_L, inst->luaRef, "onUpdate")) return true;
    lua_pushnumber(m_L, static_cast<lua_Number>(dt));
    return pcall(2, 0);
}

// The handlers that take one integer differ only in the method name, so they
// share one body.
bool ScriptEngine::callIntegerMethod(InstanceId id, const char* method, long long arg)
{
    InstanceRef* inst = requireInstance(id);
    if (!inst) return false;

    if (!pushInstanceMethod(m_L, inst->luaRef, method)) return true;
    lua_pushinteger(m_L, static_cast<lua_Integer>(arg));
    return pcall(2, 0);
}

bool ScriptEngine::callOnCollisionEnter(InstanceId id, uint32_t otherEntityId)
{ return callIntegerMethod(id, "onCollisionEnter", otherEntityId); }

bool ScriptEngine::callOnCollisionExit(InstanceId id, uint32_t otherEntityId)
{ return callIntegerMethod(id, "onCollisionExit", otherEntityId); }

bool ScriptEngine::callOnBeginOverlap(InstanceId id, uint32_t otherEntityId)
{ return callIntegerMethod(id, "onBeginOverlap", otherEntityId); }

bool ScriptEngine::callOnEndOverlap(InstanceId id, uint32_t otherEntityId)
{ return callIntegerMethod(id, "onEndOverlap", otherEntityId); }

// The three notify handlers differ only in the method name, so they share one
// body — three copies of five lines is three places for a typo in a Lua method
// name that would fail by simply never being called.
bool ScriptEngine::callNotifyMethod(InstanceId id, const char* method, const std::string& name)
{
    InstanceRef* inst = requireInstance(id);
    if (!inst) return false;

    if (!pushInstanceMethod(m_L, inst->luaRef, method)) return true;
    lua_pushlstring(m_L, name.c_str(), name.size());
    return pcall(2, 0);
}

bool ScriptEngine::callOnAnimationNotify(InstanceId id, const std::string& name)
{ return callNotifyMethod(id, "onAnimationNotify", name); }

bool ScriptEngine::callOnAnimationNotifyBegin(InstanceId id, const std::string& name)
{ return callNotifyMethod(id, "onAnimationNotifyBegin", name); }

bool ScriptEngine::callOnAnimationNotifyEnd(InstanceId id, const std::string& name)
{ return callNotifyMethod(id, "onAnimationNotifyEnd", name); }

// The input handlers share the notify body: a string is the whole payload of
// the button pair, and the axis forms only append their numbers.
bool ScriptEngine::callOnInputPressed(InstanceId id, const std::string& action)
{ return callNotifyMethod(id, "onInputPressed", action); }

bool ScriptEngine::callOnInputReleased(InstanceId id, const std::string& action)
{ return callNotifyMethod(id, "onInputReleased", action); }

bool ScriptEngine::callOnInputAxis(InstanceId id, const std::string& action, float value)
{
    InstanceRef* inst = requireInstance(id);
    if (!inst) return false;

    if (!pushInstanceMethod(m_L, inst->luaRef, "onInputAxis")) return true;
    lua_pushlstring(m_L, action.c_str(), action.size());
    lua_pushnumber(m_L, static_cast<lua_Number>(value));
    return pcall(3, 0);
}

bool ScriptEngine::callOnInputAxis2D(InstanceId id, const std::string& action, float x, float y)
{
    InstanceRef* inst = requireInstance(id);
    if (!inst) return false;

    if (!pushInstanceMethod(m_L, inst->luaRef, "onInputAxis2D")) return true;
    lua_pushlstring(m_L, action.c_str(), action.size());
    lua_pushnumber(m_L, static_cast<lua_Number>(x));
    lua_pushnumber(m_L, static_cast<lua_Number>(y));
    return pcall(4, 0);
}

bool ScriptEngine::callOnTimer(InstanceId id, int handle)
{ return callIntegerMethod(id, "onTimer", handle); }

bool ScriptEngine::callOnCheatDetected(InstanceId id, int reportId)
{ return callIntegerMethod(id, "onCheatDetected", reportId); }

bool ScriptEngine::callOnNetEvent(InstanceId id, NetScriptEvent ev, int arg)
{
    InstanceRef* inst = requireInstance(id);
    if (!inst) return false;

    // The name is the whole difference between the six (ScriptTypes.h).
    const char* fn = nullptr;
    bool carriesArg = true;
    switch (ev)
    {
        case NetScriptEvent::PlayerJoined:   fn = "onPlayerJoined";   break;
        case NetScriptEvent::PlayerLeft:     fn = "onPlayerLeft";     break;
        case NetScriptEvent::Connected:      fn = "onConnected";      carriesArg = false; break;
        case NetScriptEvent::Disconnected:   fn = "onDisconnected";   break;
        case NetScriptEvent::SessionStarted: fn = "onSessionStarted"; carriesArg = false; break;
        case NetScriptEvent::SessionEnded:   fn = "onSessionEnded";   carriesArg = false; break;
    }
    if (!fn) return true;

    if (!pushInstanceMethod(m_L, inst->luaRef, fn)) return true;
    if (!carriesArg) return pcall(1, 0);
    lua_pushinteger(m_L, static_cast<lua_Integer>(arg));
    return pcall(2, 0);
}

bool ScriptEngine::callInstanceMethod(InstanceId id, const char* fn, const ArgPusher& pushArgs)
{
    InstanceRef* inst = requireInstance(id);
    if (!inst) return false;
    if (!fn) return true;

    if (!pushInstanceMethod(m_L, inst->luaRef, fn)) return true;
    // `self` is already on the stack, so the argument count is one more than
    // whatever the caller pushed.
    const int pushed = pushArgs ? pushArgs(m_L) : 0;
    return pcall(1 + pushed, 0);
}

bool ScriptEngine::hasInstanceMethod(InstanceId id, const char* fn)
{
    auto it = m_instances.find(id);
    if (it == m_instances.end() || !fn) return false;
    if (!pushInstanceMethod(m_L, it->second.luaRef, fn)) return false;
    // pushInstanceMethod leaves the function and `self` on the stack for a
    // call that is not going to happen here.
    lua_pop(m_L, 2);
    return true;
}

bool ScriptEngine::callOnUIEvent(InstanceId id, UIScriptEvent ev)
{
    InstanceRef* inst = requireInstance(id);
    if (!inst) return false;

    const char* fn = ev == UIScriptEvent::Click      ? "onClick" :
                     ev == UIScriptEvent::HoverEnter ? "onHoverEnter" : "onHoverExit";
    if (!pushInstanceMethod(m_L, inst->luaRef, fn)) return true;
    return pcall(1, 0);
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

bool ScriptEngine::loadChunk(const std::string& name, const std::string& source)
{
    // The chunk is named after the script, not after its own text. luaL_loadstring
    // names a chunk by its source, so every error read `[string "local M = {}..."]:7:`
    // — the line was there, the script was not, and nothing could lead back to
    // the file. With `=name` Lua prints the name verbatim, so a compile error
    // and every runtime error inside the chunk come out as `name:7: message`,
    // which is the spelling HE::parseScriptErrorLocation reads.
    const std::string chunkName = "=" + name;
    if (luaL_loadbuffer(m_L, source.data(), source.size(), chunkName.c_str()) != LUA_OK)
    {
        m_lastError = lua_tostring(m_L, -1);
        lua_pop(m_L, 1);
        return false;
    }
    return true;
}

bool ScriptEngine::pcall(int nargs, int nresults)
{
    if (lua_pcall(m_L, nargs, nresults, 0) != LUA_OK)
    {
        // error() may carry any value; lua_tostring is NULL for a table or nil.
        m_lastError = lua_isstring(m_L, -1) ? lua_tostring(m_L, -1) : "non-string error";
        lua_pop(m_L, 1);
        return false;
    }
    m_lastError.clear();
    return true;
}

bool ScriptEngine::runModuleChunk(const std::string& name, const std::string& source)
{
    if (!loadChunk(name, source)) return false;
    if (!pcall(0, 1)) return false;          // execute chunk → module table on stack
    if (!lua_istable(m_L, -1))
    {
        // Said, not just refused: pcall just cleared m_lastError, and the caller
        // (ScriptContext::hotReloadScript) reports whatever is in it — which was
        // an empty reason for this one failure.
        m_lastError = "Script '" + name + "' must return a table";
        lua_pop(m_L, 1);
        return false;
    }
    return true;
}

void ScriptEngine::setInstanceField(InstanceId id, const std::string& key, double value)
{
    auto it = m_instances.find(id);
    if (it == m_instances.end()) return;
    lua_rawgeti(m_L, LUA_REGISTRYINDEX, it->second.luaRef);
    lua_pushnumber(m_L, static_cast<lua_Number>(value));
    lua_setfield(m_L, -2, key.c_str());
    lua_pop(m_L, 1);
}

void ScriptEngine::setInstanceField(InstanceId id, const std::string& key, bool value)
{
    auto it = m_instances.find(id);
    if (it == m_instances.end()) return;
    lua_rawgeti(m_L, LUA_REGISTRYINDEX, it->second.luaRef);
    lua_pushboolean(m_L, value ? 1 : 0);
    lua_setfield(m_L, -2, key.c_str());
    lua_pop(m_L, 1);
}

void ScriptEngine::setInstanceField(InstanceId id, const std::string& key, const std::string& value)
{
    auto it = m_instances.find(id);
    if (it == m_instances.end()) return;
    lua_rawgeti(m_L, LUA_REGISTRYINDEX, it->second.luaRef);
    lua_pushstring(m_L, value.c_str());
    lua_setfield(m_L, -2, key.c_str());
    lua_pop(m_L, 1);
}

void ScriptEngine::injectProperties(InstanceId id,
                                    const std::unordered_map<std::string, ScriptPropValue>& props)
{
    for (const auto& [key, val] : props)
    {
        switch (val.type)
        {
        case ScriptPropType::Float:  setInstanceField(id, key, (double)val.f); break;
        case ScriptPropType::Int:    setInstanceField(id, key, (double)val.i); break;
        case ScriptPropType::Bool:   setInstanceField(id, key, val.b);         break;
        case ScriptPropType::String: setInstanceField(id, key, val.s);         break;
        }
    }
}

std::vector<ScriptPropDef> ScriptEngine::getScriptProperties(const std::string& name) const
{
    std::vector<ScriptPropDef> result;
    auto it = m_scripts.find(name);
    if (it == m_scripts.end()) return result;

    lua_rawgeti(m_L, LUA_REGISTRYINDEX, it->second.luaRef);
    if (!lua_istable(m_L, -1)) { lua_pop(m_L, 1); return result; }

    lua_getfield(m_L, -1, "properties");
    if (!lua_istable(m_L, -1)) { lua_pop(m_L, 2); return result; }

    int propIdx = lua_gettop(m_L);
    lua_pushnil(m_L);
    while (lua_next(m_L, propIdx) != 0)
    {
        if (lua_type(m_L, -2) == LUA_TSTRING)
        {
            ScriptPropDef def;
            def.name = lua_tostring(m_L, -2);
            int vt = lua_type(m_L, -1);
            if (vt == LUA_TNUMBER)
            {
                if (lua_isinteger(m_L, -1))
                {
                    def.defaultVal.type = ScriptPropType::Int;
                    def.defaultVal.i    = (int)lua_tointeger(m_L, -1);
                }
                else
                {
                    def.defaultVal.type = ScriptPropType::Float;
                    def.defaultVal.f    = (float)lua_tonumber(m_L, -1);
                }
            }
            else if (vt == LUA_TBOOLEAN)
            {
                def.defaultVal.type = ScriptPropType::Bool;
                def.defaultVal.b    = lua_toboolean(m_L, -1) != 0;
            }
            else if (vt == LUA_TSTRING)
            {
                def.defaultVal.type = ScriptPropType::String;
                def.defaultVal.s    = lua_tostring(m_L, -1);
            }
            else
            {
                lua_pop(m_L, 1);
                continue;
            }
            result.push_back(std::move(def));
        }
        lua_pop(m_L, 1); // pop value, keep key for next iteration
    }

    lua_pop(m_L, 2); // pop properties table + module table
    return result;
}

bool ScriptEngine::hotReloadScript(const std::string& name, const std::string& source)
{
    auto it = m_scripts.find(name);
    if (it == m_scripts.end()) return false;

    // Compile and run the new source: its module table lands on the stack
    if (!runModuleChunk(name, source)) return false;

    // Replace old module reference with new one
    luaL_unref(m_L, LUA_REGISTRYINDEX, it->second.luaRef);
    it->second.luaRef = luaL_ref(m_L, LUA_REGISTRYINDEX); // pops module from stack

    // Patch function fields in all live instances that use this script
    lua_rawgeti(m_L, LUA_REGISTRYINDEX, it->second.luaRef); // push new module
    int modIdx = lua_gettop(m_L);

    for (auto& [instId, instRef] : m_instances)
    {
        if (instRef.scriptName != name) continue;
        lua_rawgeti(m_L, LUA_REGISTRYINDEX, instRef.luaRef); // push instance
        int instIdx = lua_gettop(m_L);

        lua_pushnil(m_L); // first key for iteration
        while (lua_next(m_L, modIdx) != 0) // key at -2, value at -1
        {
            if (lua_isfunction(m_L, -1))
            {
                lua_pushvalue(m_L, -2); // dup key
                lua_pushvalue(m_L, -2); // dup value (val is now at -2 after key dup)
                lua_settable(m_L, instIdx);
            }
            lua_pop(m_L, 1); // pop value, leave key for next iteration
        }
        lua_pop(m_L, 1); // pop instance
    }
    lua_pop(m_L, 1); // pop module
    m_lastError.clear();
    return true;
}
