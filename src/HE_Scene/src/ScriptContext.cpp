#include "HorizonScene/ScriptContext.h"
#include <cstdint>
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/PhysicsWorld.h"
#include "HorizonScene/ScriptApi.h"
#include "HorizonScene/EngineApi.h"   // HE::api registry (registry-driven groups)
#include "HorizonScene/Components/ScriptComponent.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Diagnostics/Log.h>

#include <string>
#include <vector>

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

// ─── Registry keys ────────────────────────────────────────────────────────────
static const char* kWorldKey   = "__horizonWorld";
static const char* kPhysicsKey = "__horizonPhysics";
static const char* kContentKey = "__horizonContent";

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

static ContentManager* getContent(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, kContentKey);
    auto* cm = static_cast<ContentManager*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return cm;
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

// horizon.setMaterialParam(entityId, name, x [, y, z, w]) — 1..4 numeric components;
// omitted components default to 0. Returns true if the parameter was found.
static int lua_horizon_setMaterialParam(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    const char* name = luaL_checkstring(L, 2);
    glm::vec4 v(0.0f);
    v.x = static_cast<float>(luaL_checknumber(L, 3));
    v.y = static_cast<float>(luaL_optnumber(L, 4, 0.0));
    v.z = static_cast<float>(luaL_optnumber(L, 5, 0.0));
    v.w = static_cast<float>(luaL_optnumber(L, 6, 0.0));
    const bool ok = ScriptApi::setMaterialParam(*getWorld(L), getContent(L), id, name, v);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// horizon.getMaterialParam(entityId, name) → x, y, z, w (four numbers).
static int lua_horizon_getMaterialParam(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    const char* name = luaL_checkstring(L, 2);
    const glm::vec4 v = ScriptApi::getMaterialParam(*getWorld(L), getContent(L), id, name);
    lua_pushnumber(L, v.x); lua_pushnumber(L, v.y);
    lua_pushnumber(L, v.z); lua_pushnumber(L, v.w);
    return 4;
}

// ── In-game UI ────────────────────────────────────────────────────────────────

// horizon.setUIText(entityId, text)
static int lua_horizon_setUIText(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    ScriptApi::setUIText(*getWorld(L), id, luaL_checkstring(L, 2));
    return 0;
}

// horizon.getUIText(entityId) → text
static int lua_horizon_getUIText(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    lua_pushstring(L, ScriptApi::getUIText(*getWorld(L), id).c_str());
    return 1;
}

// horizon.setUIColor(entityId, r, g, b [, a=1])
static int lua_horizon_setUIColor(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    ScriptApi::setUIColor(*getWorld(L), id,
        { static_cast<float>(luaL_checknumber(L, 2)),
          static_cast<float>(luaL_checknumber(L, 3)),
          static_cast<float>(luaL_checknumber(L, 4)),
          static_cast<float>(luaL_optnumber(L, 5, 1.0)) });
    return 0;
}

// horizon.getUIColor(entityId) → r, g, b, a
static int lua_horizon_getUIColor(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    const glm::vec4 c = ScriptApi::getUIColor(*getWorld(L), id);
    lua_pushnumber(L, c.r); lua_pushnumber(L, c.g);
    lua_pushnumber(L, c.b); lua_pushnumber(L, c.a);
    return 4;
}

// horizon.setUIVisible(entityId, visible)
static int lua_horizon_setUIVisible(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    ScriptApi::setUIVisible(*getWorld(L), id, lua_toboolean(L, 2) != 0);
    return 0;
}

// horizon.isUIVisible(entityId) → bool
static int lua_horizon_isUIVisible(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, ScriptApi::isUIVisible(*getWorld(L), id) ? 1 : 0);
    return 1;
}

// horizon.setUIPosition(entityId, x, y) — canvas units
static int lua_horizon_setUIPosition(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    ScriptApi::setUIPosition(*getWorld(L), id,
        { static_cast<float>(luaL_checknumber(L, 2)),
          static_cast<float>(luaL_checknumber(L, 3)) });
    return 0;
}

// horizon.getUIPosition(entityId) → x, y
static int lua_horizon_getUIPosition(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    const glm::vec2 v = ScriptApi::getUIPosition(*getWorld(L), id);
    lua_pushnumber(L, v.x); lua_pushnumber(L, v.y);
    return 2;
}

// horizon.setUISize(entityId, w, h) — canvas units
static int lua_horizon_setUISize(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    ScriptApi::setUISize(*getWorld(L), id,
        { static_cast<float>(luaL_checknumber(L, 2)),
          static_cast<float>(luaL_checknumber(L, 3)) });
    return 0;
}

// horizon.getUISize(entityId) → w, h
static int lua_horizon_getUISize(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    const glm::vec2 v = ScriptApi::getUISize(*getWorld(L), id);
    lua_pushnumber(L, v.x); lua_pushnumber(L, v.y);
    return 2;
}

// horizon.setUIMaterialParam(entityId, name, x [, y, z, w]) → bool
static int lua_horizon_setUIMaterialParam(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    const char* name = luaL_checkstring(L, 2);
    glm::vec4 v(0.0f);
    v.x = static_cast<float>(luaL_checknumber(L, 3));
    v.y = static_cast<float>(luaL_optnumber(L, 4, 0.0));
    v.z = static_cast<float>(luaL_optnumber(L, 5, 0.0));
    v.w = static_cast<float>(luaL_optnumber(L, 6, 0.0));
    const bool ok = ScriptApi::setUIMaterialParam(*getWorld(L), getContent(L), id, name, v);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// ── Live widgets + cursor ─────────────────────────────────────────────────────

// horizon.createWidget(assetPath) → widgetId (0 = failed)
static int lua_horizon_createWidget(lua_State* L)
{
    const int id = ScriptApi::createWidget(*getWorld(L), getContent(L), luaL_checkstring(L, 1));
    lua_pushinteger(L, id);
    return 1;
}

static int lua_horizon_destroyWidget(lua_State* L)
{
    ScriptApi::destroyWidget(*getWorld(L), (int)luaL_checkinteger(L, 1));
    return 0;
}

static int lua_horizon_showWidget(lua_State* L)
{
    ScriptApi::showWidget(*getWorld(L), (int)luaL_checkinteger(L, 1));
    return 0;
}

static int lua_horizon_hideWidget(lua_State* L)
{
    ScriptApi::hideWidget(*getWorld(L), (int)luaL_checkinteger(L, 1));
    return 0;
}

// horizon.setWidgetZOrder(widgetId, z)
static int lua_horizon_setWidgetZOrder(lua_State* L)
{
    ScriptApi::setWidgetZOrder(*getWorld(L), (int)luaL_checkinteger(L, 1),
                               (int)luaL_checkinteger(L, 2));
    return 0;
}

static int lua_horizon_isWidgetVisible(lua_State* L)
{
    lua_pushboolean(L, ScriptApi::isWidgetVisible(*getWorld(L), (int)luaL_checkinteger(L, 1)) ? 1 : 0);
    return 1;
}

// horizon.callWidgetFunction(widgetId, name) → bool (public functions only)
static int lua_horizon_callWidgetFunction(lua_State* L)
{
    const bool ok = ScriptApi::callWidgetFunction(*getWorld(L),
        (int)luaL_checkinteger(L, 1), luaL_checkstring(L, 2));
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// horizon.showCursor() / horizon.hideCursor() — release/re-grab the mouse.
static int lua_horizon_showCursor(lua_State* L)
{
    (void)L;
    ScriptApi::setCursorVisible(true);
    return 0;
}

static int lua_horizon_hideCursor(lua_State* L)
{
    (void)L;
    ScriptApi::setCursorVisible(false);
    return 0;
}

// ─── Registry-driven engine API (HE::api) ─────────────────────────────────────
// One generic dispatcher marshals HorizonCode Values by pin type, so a whole
// group of engine functions is exposed by iterating the registry — no per-function
// C shim. The Value ABI carries a vec2 as 2 numbers and a Color as 4 (the same
// spread the hand-written bindings use), so scalars/vectors read and return the
// familiar way. WHICH namespaces arrive here is decided by HE::api::isScriptGroup,
// not by this file — it started as math only and has grown since; read that list,
// not this comment, for the current surface.
// The FLAT gameplay functions above (horizon.setPosition, horizon.setUIText, the
// widget calls, …) keep their ergonomic hand-written shims. Routing them through
// here instead would change their script-visible arity (a packed vec3 spreads as 4
// numbers on this path), i.e. it breaks existing user scripts — so it is a
// migration, not a cleanup, and is deliberately deferred:
// docs/rework-2026-07-deferrals.md §1.

static HorizonCode::Value luaReadValue(lua_State* L, int& idx, HorizonCode::PinType t)
{
    using P = HorizonCode::PinType; using V = HorizonCode::Value;
    switch (t)
    {
    case P::Bool:   return V::ofBool(lua_toboolean(L, idx++) != 0);
    case P::Int:    return V::ofInt(static_cast<int>(luaL_checkinteger(L, idx++)));
    case P::String: return V::ofString(luaL_checkstring(L, idx++));
    case P::Vec2:   { float x = (float)luaL_checknumber(L, idx++), y = (float)luaL_checknumber(L, idx++);
                      return V::ofVec2({ x, y }); }
    case P::Color:  { float r = (float)luaL_checknumber(L, idx++), g = (float)luaL_checknumber(L, idx++),
                            b = (float)luaL_checknumber(L, idx++), a = (float)luaL_checknumber(L, idx++);
                      return V::ofColor({ r, g, b, a }); }
    case P::Ref:    return V::ofRef(static_cast<uint32_t>(luaL_checkinteger(L, idx++)));
    case P::Float:
    default:        return V::ofFloat((float)luaL_checknumber(L, idx++));
    }
}

static int luaPushValue(lua_State* L, const HorizonCode::Value& v, HorizonCode::PinType t)
{
    using P = HorizonCode::PinType;
    switch (t)
    {
    case P::Bool:   lua_pushboolean(L, v.b); return 1;
    case P::Int:    lua_pushinteger(L, v.i); return 1;
    case P::String: lua_pushstring(L, v.s.c_str()); return 1;
    case P::Vec2:   lua_pushnumber(L, v.v2.x); lua_pushnumber(L, v.v2.y); return 2;
    case P::Color:  lua_pushnumber(L, v.col.x); lua_pushnumber(L, v.col.y);
                    lua_pushnumber(L, v.col.z); lua_pushnumber(L, v.col.w); return 4;
    case P::Ref:    lua_pushinteger(L, static_cast<lua_Integer>(v.ref)); return 1;
    case P::Float:
    default:        lua_pushnumber(L, v.f); return 1;
    }
}

// Upvalue 1 = the ApiFn* (a stable pointer into the process-lifetime registry).
static int lua_engine_dispatch(lua_State* L)
{
    const auto* fn = static_cast<const HE::api::ApiFn*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!fn) return 0;
    HE::api::Ctx c{ getWorld(L), getPhysics(L), getContent(L) };
    std::vector<HorizonCode::Value> args; args.reserve(fn->params.size());
    int idx = 1;
    for (const auto& p : fn->params) args.push_back(luaReadValue(L, idx, p.type));
    const std::vector<HorizonCode::Value> res = fn->invoke(c, args);
    int pushed = 0;
    for (size_t i = 0; i < fn->results.size(); ++i)
        pushed += luaPushValue(L, i < res.size() ? res[i] : HorizonCode::Value{}, fn->results[i].type);
    return pushed;
}

// Expose namespaced HE::api entries as nested tables: horizon.<group>.<fn>.
static void registerEngineApiGroups(lua_State* L)
{
    lua_getglobal(L, "horizon");                          // [horizon]
    for (const HE::api::ApiFn& fn : HE::api::registry())
    {
        const std::string id = fn.id;
        const auto dot = id.find('.');
        if (dot == std::string::npos) continue;           // only namespaced ("math.clamp")
        const std::string group = id.substr(0, dot), name = id.substr(dot + 1);
        // Registry-driven groups exposed as horizon.<group>.<fn> — one shared list
        // (HE::api::isScriptGroup) so Lua and Python expose the same surface.
        if (!HE::api::isScriptGroup(group)) continue;
        lua_getfield(L, -1, group.c_str());               // [horizon, group?]
        if (!lua_istable(L, -1))
        {
            lua_pop(L, 1);                                 // [horizon]
            lua_newtable(L);                               // [horizon, group]
            lua_pushvalue(L, -1);                          // [horizon, group, group]
            lua_setfield(L, -3, group.c_str());            // horizon[group]=group → [horizon, group]
        }
        lua_pushlightuserdata(L, const_cast<HE::api::ApiFn*>(&fn));  // [horizon, group, fn*]
        lua_pushcclosure(L, lua_engine_dispatch, 1);       // [horizon, group, closure]
        lua_setfield(L, -2, name.c_str());                 // group[name]=closure → [horizon, group]
        lua_pop(L, 1);                                     // [horizon]
    }
    lua_pop(L, 1);                                         // []
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
    { "setMaterialParam", lua_horizon_setMaterialParam },
    { "getMaterialParam", lua_horizon_getMaterialParam },
    { "setUIText",     lua_horizon_setUIText     },
    { "getUIText",     lua_horizon_getUIText     },
    { "setUIColor",    lua_horizon_setUIColor    },
    { "getUIColor",    lua_horizon_getUIColor    },
    { "setUIVisible",  lua_horizon_setUIVisible  },
    { "isUIVisible",   lua_horizon_isUIVisible   },
    { "setUIPosition", lua_horizon_setUIPosition },
    { "getUIPosition", lua_horizon_getUIPosition },
    { "setUISize",     lua_horizon_setUISize     },
    { "getUISize",     lua_horizon_getUISize     },
    { "setUIMaterialParam", lua_horizon_setUIMaterialParam },
    { "createWidget",       lua_horizon_createWidget       },
    { "destroyWidget",      lua_horizon_destroyWidget      },
    { "showWidget",         lua_horizon_showWidget         },
    { "hideWidget",         lua_horizon_hideWidget         },
    { "setWidgetZOrder",    lua_horizon_setWidgetZOrder    },
    { "isWidgetVisible",    lua_horizon_isWidgetVisible    },
    { "callWidgetFunction", lua_horizon_callWidgetFunction },
    { "showCursor",         lua_horizon_showCursor         },
    { "hideCursor",         lua_horizon_hideCursor         },
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

    // Content manager starts as null; updated via setContentManager()
    lua_pushlightuserdata(L, nullptr);
    lua_setfield(L, LUA_REGISTRYINDEX, kContentKey);

    // Create `horizon` global table and register all functions
    luaL_newlib(L, kHorizonFuncs);
    lua_setglobal(L, "horizon");

    // Registry-driven groups (horizon.math.*, …) layered on top of the flat API.
    registerEngineApiGroups(L);
}

void ScriptContext::setPhysicsWorld(PhysicsWorld* pw)
{
    m_physicsWorld = pw;
    lua_State* L = m_engine.state();
    lua_pushlightuserdata(L, pw);
    lua_setfield(L, LUA_REGISTRYINDEX, kPhysicsKey);

    if (m_py) m_py->setPhysicsWorld(pw);
}

void ScriptContext::setContentManager(ContentManager* cm)
{
    m_contentManager = cm;
    lua_State* L = m_engine.state();
    lua_pushlightuserdata(L, cm);
    lua_setfield(L, LUA_REGISTRYINDEX, kContentKey);

    if (m_py) m_py->setContentManager(cm);
}

// ─── Backend routing ───────────────────────────────────────────────────────────

IScriptBackend* ScriptContext::backendForId(InstanceId id)
{
    if (langOf(id) == HE::ScriptLanguage::Python && m_py) return m_py.get();
    return &m_engine;
}

IScriptBackend* ScriptContext::backendForName(const std::string& name)
{
    if (m_py && m_py->isScriptLoaded(name)) return m_py.get();
    return &m_engine;
}

// ─── Script diagnostics ──────────────────────────────────────────────────────
// ScriptContext is the single funnel every Lua/Python/HorizonCode call passes
// through, and until now a failing script just returned false: the error text
// sat in lastError() and nobody read it, so a typo in onUpdate() looked exactly
// like "the script does nothing". Everything below reports through the log.
namespace
{
	HE::Log::Cat catFor(HE::ScriptLanguage lang)
	{
		return lang == HE::ScriptLanguage::Python ? HE::Log::Cat::Python
		                                          : HE::Log::Cat::Lua;
	}

	const char* langName(HE::ScriptLanguage lang)
	{
		switch (lang)
		{
		case HE::ScriptLanguage::Python: return "Python";
		case HE::ScriptLanguage::Lua:    return "Lua";
		default:                         return "unknown";
		}
	}
}

bool ScriptContext::loadScript(const std::string& name, const std::string& source,
                               HE::ScriptLanguage lang)
{
    if (lang == HE::ScriptLanguage::Python)
    {
        if (!m_py)
        {
            m_lastBackend = &m_engine;
            HE_LOG_ERROR(Python, "Cannot load script '%s': this build has no Python backend",
                         name.c_str());
            return false;
        }
        m_lastBackend = m_py.get();
        if (!m_py->loadScript(name, source))
        {
            HE_LOG_ERROR(Python, "Compile error in script '%s': %s",
                         name.c_str(), m_py->lastError().c_str());
            return false;
        }
        HE_LOG_DEBUG(Python, "Loaded script '%s' (%zu bytes)", name.c_str(), source.size());
        return true;
    }
    m_lastBackend = &m_engine;
    if (!m_engine.loadScript(name, source))
    {
        HE_LOG_ERROR(Lua, "Compile error in script '%s': %s",
                     name.c_str(), m_engine.lastError().c_str());
        return false;
    }
    HE_LOG_DEBUG(Lua, "Loaded script '%s' (%zu bytes)", name.c_str(), source.size());
    return true;
}

ScriptEngine::InstanceId ScriptContext::createInstance(const std::string& scriptName,
                                                        entt::entity       entity)
{
    IScriptBackend* backend = backendForName(scriptName);
    const HE::ScriptLanguage lang = (backend == m_py.get()) ? HE::ScriptLanguage::Python
                                                        : HE::ScriptLanguage::Lua;
    return createInstance(scriptName, entity, lang);
}

ScriptEngine::InstanceId ScriptContext::createInstance(const std::string& scriptName,
                                                        entt::entity       entity,
                                                        HE::ScriptLanguage     lang)
{
    IScriptBackend* backend = (lang == HE::ScriptLanguage::Python)
                                  ? static_cast<IScriptBackend*>(m_py.get())
                                  : &m_engine;
    if (!backend)
    {
        HE_LOG_ERROR(Script, "Cannot instantiate '%s' on entity %u: this build has no "
                             "Python backend", scriptName.c_str(), static_cast<uint32_t>(entity));
        return IScriptBackend::kInvalidInstance; // Python requested, unavailable
    }
    m_lastBackend = backend;
    // The entity binding (self.entityId / self.entity_id) is set by the backend.
    const InstanceId raw = backend->createInstance(scriptName, static_cast<uint32_t>(entity));
    if (raw == IScriptBackend::kInvalidInstance)
    {
        HE_LOG(Script, Error, "Failed to instantiate %s script '%s' on entity %u: %s",
               langName(lang), scriptName.c_str(), static_cast<uint32_t>(entity),
               backend->lastError().c_str());
        return IScriptBackend::kInvalidInstance;
    }
    HE_LOG_TRACE(Script, "Instantiated %s script '%s' on entity %u",
                 langName(lang), scriptName.c_str(), static_cast<uint32_t>(entity));
    return tagId(raw, lang);
}

void ScriptContext::destroyInstance(ScriptEngine::InstanceId id)
{
    backendForId(id)->destroyInstance(rawId(id));
}

// ─── Bulk start of a scene's ECS scripts ──────────────────────────────────────

ScriptEngine::InstanceId ScriptContext::startEntityScript(entt::entity entity, ContentManager& cm)
{
    if (!m_world) return ScriptEngine::kInvalidInstance;
    auto& reg = m_world->registry();
    if (!reg.valid(entity)) return ScriptEngine::kInvalidInstance;
    auto* sc = reg.try_get<ScriptComponent>(entity);
    if (!sc || !sc->enabled) return ScriptEngine::kInvalidInstance;

    // "My script isn't running" is nearly always one of the two cases below: the
    // component points at an asset that is gone, or at one that is empty. Both
    // used to be a silent early return.
    const ScriptAsset* asset = cm.getScript(sc->scriptAssetId);
    if (!asset)
    {
        HE_LOG_ERROR(Script, "Entity %u: script asset %016llx%016llx referenced by '%s' was "
                             "not found — no script will run on this entity",
                     static_cast<uint32_t>(entity),
                     static_cast<unsigned long long>(sc->scriptAssetId.hi),
                     static_cast<unsigned long long>(sc->scriptAssetId.lo),
                     sc->moduleName.c_str());
        return ScriptEngine::kInvalidInstance;
    }
    if (asset->sourceCode.empty())
    {
        HE_LOG_WARN(Script, "Entity %u: script '%s' is empty — nothing to run",
                    static_cast<uint32_t>(entity), asset->name.c_str());
        return ScriptEngine::kInvalidInstance;
    }
    if (!isScriptLoaded(sc->moduleName, asset->language))
    {
        if (!loadScript(sc->moduleName, asset->sourceCode, asset->language))
            return ScriptEngine::kInvalidInstance;   // loadScript already logged why
    }
    const auto instId = createInstance(sc->moduleName, entity, asset->language);
    if (instId == ScriptEngine::kInvalidInstance) return ScriptEngine::kInvalidInstance;
    injectProperties(instId, sc->properties);
    callOnStart(instId);
    return instId;
}

int ScriptContext::startWorldScripts(ContentManager& cm, InstanceMap& out)
{
    if (!m_world) return 0;
    int started = 0, attempted = 0;
    for (auto [entity, sc] : m_world->registry().view<ScriptComponent>().each())
    {
        ++attempted;
        const auto instId = startEntityScript(entity, cm);
        if (instId == ScriptEngine::kInvalidInstance) continue;
        out[static_cast<uint32_t>(entity)] = instId;
        ++started;
    }
    if (started != attempted)
        HE_LOG_WARN(Script, "Started %d of %d entity script(s) — %d failed to start",
                    started, attempted, attempted - started);
    else if (started > 0)
        HE_LOG_INFO(Script, "Started %d entity script(s)", started);
    return started;
}

int ScriptContext::startScriptsFor(const std::vector<entt::entity>& entities,
                                   ContentManager& cm, InstanceMap& out)
{
    if (!m_world) return 0;
    int started = 0;
    for (entt::entity entity : entities)
    {
        const auto instId = startEntityScript(entity, cm);
        if (instId == ScriptEngine::kInvalidInstance) continue;
        out[static_cast<uint32_t>(entity)] = instId;
        ++started;
    }
    return started;
}

// Shared reporting for the per-callback entry points. A runtime error in a
// script callback is a genuine bug the developer must see, but onUpdate runs
// every frame for every instance — so the report is throttled per callback kind
// instead of being dropped or spamming 60 lines a second.
#define HE_SCRIPT_CALL(cbName, expr)                                            \
	do {                                                                        \
		if (expr) return true;                                                  \
		HE_LOG_THROTTLE(Script, Error, 2.0,                                     \
		                "%s script instance %llu failed in " cbName "(): %s",   \
		                langName(langOf(id)),                                   \
		                static_cast<unsigned long long>(rawId(id)),             \
		                b->lastError().c_str());                                \
		return false;                                                           \
	} while (0)

bool ScriptContext::callOnStart(ScriptEngine::InstanceId id)
{
    IScriptBackend* b = backendForId(id); m_lastBackend = b;
    HE_SCRIPT_CALL("onStart", b->callOnStart(rawId(id)));
}

bool ScriptContext::callOnUpdate(ScriptEngine::InstanceId id, float dt)
{
    IScriptBackend* b = backendForId(id); m_lastBackend = b;
    HE_SCRIPT_CALL("onUpdate", b->callOnUpdate(rawId(id), dt));
}

bool ScriptContext::callOnCollisionEnter(ScriptEngine::InstanceId id, uint32_t otherEntityId)
{
    IScriptBackend* b = backendForId(id); m_lastBackend = b;
    HE_SCRIPT_CALL("onCollisionEnter", b->callOnCollisionEnter(rawId(id), otherEntityId));
}

bool ScriptContext::callOnCollisionExit(ScriptEngine::InstanceId id, uint32_t otherEntityId)
{
    IScriptBackend* b = backendForId(id); m_lastBackend = b;
    HE_SCRIPT_CALL("onCollisionExit", b->callOnCollisionExit(rawId(id), otherEntityId));
}

bool ScriptContext::callOnUIEvent(ScriptEngine::InstanceId id, UIScriptEvent ev)
{
    IScriptBackend* b = backendForId(id); m_lastBackend = b;
    HE_SCRIPT_CALL("onUIEvent", b->callOnUIEvent(rawId(id), ev));
}

#undef HE_SCRIPT_CALL

bool ScriptContext::hotReloadScript(const std::string& name, const std::string& source)
{
    IScriptBackend* b = backendForName(name); m_lastBackend = b;
    const bool ok = b->hotReloadScript(name, source);
    if (ok) HE_LOG_INFO(Script, "Hot-reloaded script '%s'", name.c_str());
    else    HE_LOG_ERROR(Script, "Hot reload of '%s' failed (previous version stays live): %s",
                         name.c_str(), b->lastError().c_str());
    return ok;
}

bool ScriptContext::hotReloadScript(const std::string& name, const std::string& source,
                                    HE::ScriptLanguage lang)
{
    IScriptBackend* b = (lang == HE::ScriptLanguage::Python)
                            ? static_cast<IScriptBackend*>(m_py.get())
                            : &m_engine;
    if (!b)
    {
        m_lastBackend = &m_engine;
        HE_LOG_ERROR(Python, "Hot reload of '%s' skipped: this build has no Python backend",
                     name.c_str());
        return false;
    }
    m_lastBackend = b;
    const bool ok = b->hotReloadScript(name, source);
    if (ok) HE_LOG(Script, Info, "Hot-reloaded %s script '%s'", langName(lang), name.c_str());
    else    HE_LOG(Script, Error, "Hot reload of %s script '%s' failed "
                                  "(previous version stays live): %s",
                   langName(lang), name.c_str(), b->lastError().c_str());
    return ok;
}

bool ScriptContext::isScriptLoaded(const std::string& name) const
{
    if (m_engine.isScriptLoaded(name)) return true;
    return m_py && m_py->isScriptLoaded(name);
}

bool ScriptContext::isScriptLoaded(const std::string& name, HE::ScriptLanguage lang) const
{
    if (lang == HE::ScriptLanguage::Python) return m_py && m_py->isScriptLoaded(name);
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
