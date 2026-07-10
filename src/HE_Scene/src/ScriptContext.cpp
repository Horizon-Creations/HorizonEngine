#include "HorizonScene/ScriptContext.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/PhysicsWorld.h"
#include "HorizonScene/ScriptApi.h"
#include "HorizonScene/EngineApi.h"   // HE::api registry (registry-driven groups)

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
// familiar way. First registry-driven group: the pure Math library (horizon.math.*,
// which no frontend could reach before). Gameplay groups keep their ergonomic
// hand-written bindings until ScriptApi is inverted onto HE::api.

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
        // Registry-driven groups exposed as horizon.<group>.<fn>. The flat
        // gameplay functions keep their ergonomic hand-written bindings until
        // ScriptApi is inverted onto HE::api. NB: a packed vec3 (Color) param
        // spreads as 4 numbers (x, y, z, _) on this path. Widening = add a name.
        static const char* kGroups[] = { "math", "random", "time", "input",
                                         "string", "camera", "env", "entity", "audio",
	                                 "debug", "fs", "save", "scene" };
        bool exposed = false;
        for (const char* gname : kGroups) if (group == gname) { exposed = true; break; }
        if (!exposed) continue;
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

bool ScriptContext::callOnUIEvent(ScriptEngine::InstanceId id, UIScriptEvent ev)
{
    IScriptBackend* b = backendForId(id); m_lastBackend = b;
    return b->callOnUIEvent(rawId(id), ev);
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
