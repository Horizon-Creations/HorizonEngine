#include "HorizonScene/ScriptContext.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/PhysicsWorld.h"
#include "HorizonScene/ScriptApi.h"

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

// ─── Registry keys ────────────────────────────────────────────────────────────
static const char* kWorldKey   = "__horizonWorld";
static const char* kPhysicsKey = "__horizonPhysics";

static HorizonWorld* getWorld(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, kWorldKey);
    auto* w = static_cast<HorizonWorld*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return w;
}

static PhysicsWorld* getPhysics(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, kPhysicsKey);
    auto* pw = static_cast<PhysicsWorld*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return pw;
}

// ─── Lua C functions ─────────────────────────────────────────────────────────
// Thin marshalling shims: all behavior lives in the language-neutral ScriptApi
// (shared 1:1 with the Python backend).

static int lua_horizon_log(lua_State* L)
{
    ScriptApi::log(luaL_checkstring(L, 1));
    return 0;
}

static int lua_horizon_getName(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    lua_pushstring(L, ScriptApi::getName(*getWorld(L), id).c_str());
    return 1;
}

static int lua_horizon_getPosition(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    const glm::vec3 p = ScriptApi::getPosition(*getWorld(L), id);
    lua_pushnumber(L, p.x); lua_pushnumber(L, p.y); lua_pushnumber(L, p.z);
    return 3;
}

static int lua_horizon_setPosition(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    ScriptApi::setPosition(*getWorld(L), id,
        { static_cast<float>(luaL_checknumber(L, 2)),
          static_cast<float>(luaL_checknumber(L, 3)),
          static_cast<float>(luaL_checknumber(L, 4)) });
    return 0;
}

static int lua_horizon_getRotation(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    const glm::vec3 r = ScriptApi::getRotation(*getWorld(L), id);
    lua_pushnumber(L, r.x); lua_pushnumber(L, r.y); lua_pushnumber(L, r.z);
    return 3;
}

static int lua_horizon_setRotation(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    ScriptApi::setRotation(*getWorld(L), id,
        { static_cast<float>(luaL_checknumber(L, 2)),
          static_cast<float>(luaL_checknumber(L, 3)),
          static_cast<float>(luaL_checknumber(L, 4)) });
    return 0;
}

static int lua_horizon_getScale(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    const glm::vec3 s = ScriptApi::getScale(*getWorld(L), id);
    lua_pushnumber(L, s.x); lua_pushnumber(L, s.y); lua_pushnumber(L, s.z);
    return 3;
}

static int lua_horizon_setScale(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    ScriptApi::setScale(*getWorld(L), id,
        { static_cast<float>(luaL_checknumber(L, 2)),
          static_cast<float>(luaL_checknumber(L, 3)),
          static_cast<float>(luaL_checknumber(L, 4)) });
    return 0;
}

static int lua_horizon_spawn(lua_State* L)
{
    const auto parentId = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    const char* name    = luaL_optstring(L, 2, "Entity");
    const uint32_t child = ScriptApi::spawn(*getWorld(L), parentId, name);
    lua_pushinteger(L, static_cast<lua_Integer>(child));
    return 1;
}

static int lua_horizon_destroy(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    ScriptApi::destroy(*getWorld(L), id);
    return 0;
}

static int lua_horizon_raycast(lua_State* L)
{
    const glm::vec3 origin{ static_cast<float>(luaL_checknumber(L, 1)),
                            static_cast<float>(luaL_checknumber(L, 2)),
                            static_cast<float>(luaL_checknumber(L, 3)) };
    const glm::vec3 dir{ static_cast<float>(luaL_checknumber(L, 4)),
                         static_cast<float>(luaL_checknumber(L, 5)),
                         static_cast<float>(luaL_checknumber(L, 6)) };
    const float maxDist = static_cast<float>(luaL_optnumber(L, 7, 1000.0));

    const auto hit = ScriptApi::raycast(getPhysics(L), origin, dir, maxDist);
    if (!hit.hit) { lua_pushnil(L); return 1; }

    lua_newtable(L);
    lua_pushinteger(L, static_cast<lua_Integer>(hit.entityId));
    lua_setfield(L, -2, "entity");
    lua_pushnumber(L, hit.point.x);    lua_setfield(L, -2, "x");
    lua_pushnumber(L, hit.point.y);    lua_setfield(L, -2, "y");
    lua_pushnumber(L, hit.point.z);    lua_setfield(L, -2, "z");
    lua_pushnumber(L, hit.normal.x);   lua_setfield(L, -2, "nx");
    lua_pushnumber(L, hit.normal.y);   lua_setfield(L, -2, "ny");
    lua_pushnumber(L, hit.normal.z);   lua_setfield(L, -2, "nz");
    lua_pushnumber(L, hit.distance);   lua_setfield(L, -2, "distance");
    return 1;
}

static int lua_horizon_setVelocity(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    ScriptApi::setVelocity(getPhysics(L), id,
        { static_cast<float>(luaL_checknumber(L, 2)),
          static_cast<float>(luaL_checknumber(L, 3)),
          static_cast<float>(luaL_checknumber(L, 4)) });
    return 0;
}

static int lua_horizon_isGrounded(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, ScriptApi::isGrounded(getPhysics(L), id) ? 1 : 0);
    return 1;
}

// ─── Registration table ──────────────────────────────────────────────────────

static const luaL_Reg kHorizonFuncs[] = {
    { "log",         lua_horizon_log         },
    { "getName",     lua_horizon_getName     },
    { "getPosition", lua_horizon_getPosition },
    { "setPosition", lua_horizon_setPosition },
    { "getRotation", lua_horizon_getRotation },
    { "setRotation", lua_horizon_setRotation },
    { "getScale",    lua_horizon_getScale    },
    { "setScale",    lua_horizon_setScale    },
    { "spawn",       lua_horizon_spawn       },
    { "destroy",     lua_horizon_destroy     },
    { "raycast",     lua_horizon_raycast     },
    { "setVelocity", lua_horizon_setVelocity },
    { "isGrounded",  lua_horizon_isGrounded  },
    { nullptr, nullptr }
};

// ─── ScriptContext ────────────────────────────────────────────────────────────

ScriptContext::ScriptContext(HorizonWorld& world)
    : m_world(&world)
{
    registerHorizonApi();
    if (PyScriptBackend::available())
        m_py = std::make_unique<PyScriptBackend>(world);
}

void ScriptContext::registerHorizonApi()
{
    lua_State* L = m_engine.state();

    // Store world pointer in Lua registry
    lua_pushlightuserdata(L, m_world);
    lua_setfield(L, LUA_REGISTRYINDEX, kWorldKey);

    // Physics starts as null; updated via setPhysicsWorld()
    lua_pushlightuserdata(L, nullptr);
    lua_setfield(L, LUA_REGISTRYINDEX, kPhysicsKey);

    // Create `horizon` global table and register all functions
    luaL_newlib(L, kHorizonFuncs);
    lua_setglobal(L, "horizon");
}

void ScriptContext::setPhysicsWorld(PhysicsWorld* pw)
{
    m_physicsWorld = pw;
    lua_State* L = m_engine.state();
    lua_pushlightuserdata(L, pw);
    lua_setfield(L, LUA_REGISTRYINDEX, kPhysicsKey);

    if (m_py) m_py->setPhysicsWorld(pw);
}

// ─── Backend routing ───────────────────────────────────────────────────────────

IScriptBackend* ScriptContext::backendForId(InstanceId id)
{
    if (langOf(id) == ScriptLanguage::Python && m_py) return m_py.get();
    return &m_engine;
}

IScriptBackend* ScriptContext::backendForName(const std::string& name)
{
    if (m_py && m_py->isScriptLoaded(name)) return m_py.get();
    return &m_engine;
}

bool ScriptContext::loadScript(const std::string& name, const std::string& source,
                               ScriptLanguage lang)
{
    if (lang == ScriptLanguage::Python)
    {
        if (!m_py) { m_lastBackend = &m_engine; return false; } // built without Python
        m_lastBackend = m_py.get();
        return m_py->loadScript(name, source);
    }
    m_lastBackend = &m_engine;
    return m_engine.loadScript(name, source);
}

ScriptEngine::InstanceId ScriptContext::createInstance(const std::string& scriptName,
                                                        entt::entity       entity)
{
    IScriptBackend* backend = backendForName(scriptName);
    const ScriptLanguage lang = (backend == m_py.get()) ? ScriptLanguage::Python
                                                        : ScriptLanguage::Lua;
    return createInstance(scriptName, entity, lang);
}

ScriptEngine::InstanceId ScriptContext::createInstance(const std::string& scriptName,
                                                        entt::entity       entity,
                                                        ScriptLanguage     lang)
{
    IScriptBackend* backend = (lang == ScriptLanguage::Python)
                                  ? static_cast<IScriptBackend*>(m_py.get())
                                  : &m_engine;
    if (!backend) return IScriptBackend::kInvalidInstance; // Python requested, unavailable
    m_lastBackend = backend;
    // The entity binding (self.entityId / self.entity_id) is set by the backend.
    const InstanceId raw = backend->createInstance(scriptName, static_cast<uint32_t>(entity));
    if (raw == IScriptBackend::kInvalidInstance) return IScriptBackend::kInvalidInstance;
    return tagId(raw, lang);
}

void ScriptContext::destroyInstance(ScriptEngine::InstanceId id)
{
    backendForId(id)->destroyInstance(rawId(id));
}

bool ScriptContext::callOnStart(ScriptEngine::InstanceId id)
{
    IScriptBackend* b = backendForId(id); m_lastBackend = b;
    return b->callOnStart(rawId(id));
}

bool ScriptContext::callOnUpdate(ScriptEngine::InstanceId id, float dt)
{
    IScriptBackend* b = backendForId(id); m_lastBackend = b;
    return b->callOnUpdate(rawId(id), dt);
}

bool ScriptContext::callOnCollisionEnter(ScriptEngine::InstanceId id, uint32_t otherEntityId)
{
    IScriptBackend* b = backendForId(id); m_lastBackend = b;
    return b->callOnCollisionEnter(rawId(id), otherEntityId);
}

bool ScriptContext::callOnCollisionExit(ScriptEngine::InstanceId id, uint32_t otherEntityId)
{
    IScriptBackend* b = backendForId(id); m_lastBackend = b;
    return b->callOnCollisionExit(rawId(id), otherEntityId);
}

bool ScriptContext::hotReloadScript(const std::string& name, const std::string& source)
{
    IScriptBackend* b = backendForName(name); m_lastBackend = b;
    return b->hotReloadScript(name, source);
}

bool ScriptContext::hotReloadScript(const std::string& name, const std::string& source,
                                    ScriptLanguage lang)
{
    IScriptBackend* b = (lang == ScriptLanguage::Python)
                            ? static_cast<IScriptBackend*>(m_py.get())
                            : &m_engine;
    if (!b) { m_lastBackend = &m_engine; return false; } // Python requested, unavailable
    m_lastBackend = b;
    return b->hotReloadScript(name, source);
}

bool ScriptContext::isScriptLoaded(const std::string& name) const
{
    if (m_engine.isScriptLoaded(name)) return true;
    return m_py && m_py->isScriptLoaded(name);
}

bool ScriptContext::isScriptLoaded(const std::string& name, ScriptLanguage lang) const
{
    if (lang == ScriptLanguage::Python) return m_py && m_py->isScriptLoaded(name);
    return m_engine.isScriptLoaded(name);
}

size_t ScriptContext::loadedScriptCount() const
{
    return m_engine.loadedScriptCount() + (m_py ? m_py->loadedScriptCount() : 0);
}

size_t ScriptContext::instanceCount() const
{
    return m_engine.instanceCount() + (m_py ? m_py->instanceCount() : 0);
}

const std::string& ScriptContext::lastError() const
{
    return m_lastBackend->lastError();
}

void ScriptContext::injectProperties(ScriptEngine::InstanceId id,
                                     const std::unordered_map<std::string, ScriptPropValue>& props)
{
    backendForId(id)->injectProperties(rawId(id), props);
}
