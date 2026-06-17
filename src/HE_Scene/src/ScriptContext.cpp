#include "HorizonScene/ScriptContext.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/Components/NameComponent.h"

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

// ─── Registry key used to store the HorizonWorld* ───────────────────────────
static const char* kWorldKey = "__horizonWorld";

static HorizonWorld* getWorld(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, kWorldKey);
    auto* w = static_cast<HorizonWorld*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return w;
}

static entt::entity toEntity(lua_Integer v)
{
    return static_cast<entt::entity>(static_cast<uint32_t>(v));
}

// ─── Lua C functions ─────────────────────────────────────────────────────────

static int lua_horizon_log(lua_State* L)
{
    const char* msg = luaL_checkstring(L, 1);
    printf("[Lua] %s\n", msg);
    return 0;
}

static int lua_horizon_getName(lua_State* L)
{
    lua_Integer id = luaL_checkinteger(L, 1);
    HorizonWorld* world = getWorld(L);
    auto& reg = world->registry();
    entt::entity e = toEntity(id);
    if (!reg.valid(e)) { lua_pushstring(L, ""); return 1; }
    const auto* nc = reg.try_get<NameComponent>(e);
    lua_pushstring(L, nc ? nc->name.c_str() : "");
    return 1;
}

static int lua_horizon_getPosition(lua_State* L)
{
    lua_Integer id = luaL_checkinteger(L, 1);
    HorizonWorld* world = getWorld(L);
    auto& reg = world->registry();
    entt::entity e = toEntity(id);
    const auto* t = reg.valid(e) ? reg.try_get<TransformComponent>(e) : nullptr;
    if (t) { lua_pushnumber(L, t->position.x); lua_pushnumber(L, t->position.y); lua_pushnumber(L, t->position.z); return 3; }
    lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
    return 3;
}

static int lua_horizon_setPosition(lua_State* L)
{
    lua_Integer id = luaL_checkinteger(L, 1);
    float x = static_cast<float>(luaL_checknumber(L, 2));
    float y = static_cast<float>(luaL_checknumber(L, 3));
    float z = static_cast<float>(luaL_checknumber(L, 4));
    HorizonWorld* world = getWorld(L);
    auto& reg = world->registry();
    entt::entity e = toEntity(id);
    if (reg.valid(e))
    {
        auto* t = reg.try_get<TransformComponent>(e);
        if (t) { t->position = {x, y, z}; t->dirty = true; }
    }
    return 0;
}

static int lua_horizon_getRotation(lua_State* L)
{
    lua_Integer id = luaL_checkinteger(L, 1);
    HorizonWorld* world = getWorld(L);
    auto& reg = world->registry();
    entt::entity e = toEntity(id);
    const auto* t = reg.valid(e) ? reg.try_get<TransformComponent>(e) : nullptr;
    if (t) { lua_pushnumber(L, t->rotation.x); lua_pushnumber(L, t->rotation.y); lua_pushnumber(L, t->rotation.z); return 3; }
    lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
    return 3;
}

static int lua_horizon_setRotation(lua_State* L)
{
    lua_Integer id = luaL_checkinteger(L, 1);
    float x = static_cast<float>(luaL_checknumber(L, 2));
    float y = static_cast<float>(luaL_checknumber(L, 3));
    float z = static_cast<float>(luaL_checknumber(L, 4));
    HorizonWorld* world = getWorld(L);
    auto& reg = world->registry();
    entt::entity e = toEntity(id);
    if (reg.valid(e))
    {
        auto* t = reg.try_get<TransformComponent>(e);
        if (t) { t->rotation = {x, y, z}; t->dirty = true; }
    }
    return 0;
}

static int lua_horizon_getScale(lua_State* L)
{
    lua_Integer id = luaL_checkinteger(L, 1);
    HorizonWorld* world = getWorld(L);
    auto& reg = world->registry();
    entt::entity e = toEntity(id);
    const auto* t = reg.valid(e) ? reg.try_get<TransformComponent>(e) : nullptr;
    if (t) { lua_pushnumber(L, t->scale.x); lua_pushnumber(L, t->scale.y); lua_pushnumber(L, t->scale.z); return 3; }
    lua_pushnumber(L, 1); lua_pushnumber(L, 1); lua_pushnumber(L, 1);
    return 3;
}

static int lua_horizon_setScale(lua_State* L)
{
    lua_Integer id = luaL_checkinteger(L, 1);
    float x = static_cast<float>(luaL_checknumber(L, 2));
    float y = static_cast<float>(luaL_checknumber(L, 3));
    float z = static_cast<float>(luaL_checknumber(L, 4));
    HorizonWorld* world = getWorld(L);
    auto& reg = world->registry();
    entt::entity e = toEntity(id);
    if (reg.valid(e))
    {
        auto* t = reg.try_get<TransformComponent>(e);
        if (t) { t->scale = {x, y, z}; t->dirty = true; }
    }
    return 0;
}

static int lua_horizon_spawn(lua_State* L)
{
    lua_Integer parentId = luaL_checkinteger(L, 1);
    const char* name    = luaL_optstring(L, 2, "Entity");
    HorizonWorld* world = getWorld(L);
    entt::entity parent = toEntity(parentId);
    entt::entity child  = world->createEntity(name);
    if (world->registry().valid(parent))
        world->reparentEntity(child, parent);
    lua_pushinteger(L, static_cast<lua_Integer>(static_cast<uint32_t>(child)));
    return 1;
}

static int lua_horizon_destroy(lua_State* L)
{
    lua_Integer id = luaL_checkinteger(L, 1);
    HorizonWorld* world = getWorld(L);
    entt::entity e = toEntity(id);
    if (world->registry().valid(e))
        world->destroyEntity(e);
    return 0;
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
    { nullptr, nullptr }
};

// ─── ScriptContext ────────────────────────────────────────────────────────────

ScriptContext::ScriptContext(HorizonWorld& world)
    : m_world(&world)
{
    registerHorizonApi();
}

void ScriptContext::registerHorizonApi()
{
    lua_State* L = m_engine.state();

    // Store world pointer in Lua registry
    lua_pushlightuserdata(L, m_world);
    lua_setfield(L, LUA_REGISTRYINDEX, kWorldKey);

    // Create `horizon` global table and register all functions
    luaL_newlib(L, kHorizonFuncs);
    lua_setglobal(L, "horizon");
}

bool ScriptContext::loadScript(const std::string& name, const std::string& source)
{
    return m_engine.loadScript(name, source);
}

ScriptEngine::InstanceId ScriptContext::createInstance(const std::string& scriptName,
                                                        entt::entity       entity)
{
    auto id = m_engine.createInstance(scriptName);
    if (id == ScriptEngine::kInvalidInstance) return id;
    // Store entity ID in instance table so scripts can use self.entityId
    m_engine.setInstanceField(id, "entityId",
        static_cast<double>(static_cast<uint32_t>(entity)));
    return id;
}

void ScriptContext::destroyInstance(ScriptEngine::InstanceId id)
{
    m_engine.destroyInstance(id);
}

bool ScriptContext::callOnStart(ScriptEngine::InstanceId id)
{
    return m_engine.callOnStart(id);
}

bool ScriptContext::callOnUpdate(ScriptEngine::InstanceId id, float dt)
{
    return m_engine.callOnUpdate(id, dt);
}

bool ScriptContext::isScriptLoaded(const std::string& name) const
{
    return m_engine.isScriptLoaded(name);
}

size_t ScriptContext::loadedScriptCount() const
{
    return m_engine.loadedScriptCount();
}

size_t ScriptContext::instanceCount() const
{
    return m_engine.instanceCount();
}

const std::string& ScriptContext::lastError() const
{
    return m_engine.lastError();
}
