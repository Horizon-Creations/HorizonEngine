#include "HorizonScene/ScriptContext.h"
#include <Types/TypeRegistry.h>
#include <algorithm>   // sort — the deterministic key order of an unordered Lua map
#include <functional>
#include <cstdint>
#include <filesystem>
// For thisLibraryDir(): finding the directory this very library was loaded from.
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
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
// Points AT the owning context's m_quitHandler rather than holding a copy, so a
// later setQuitHandler is visible without re-registering anything.
static const char* kQuitKey    = "__horizonQuit";

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

static const std::function<void()>* getQuit(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, kQuitKey);
    auto* q = static_cast<const std::function<void()>*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return q;
}

// THE one place in this backend that builds a Ctx. Every Lua call goes through
// it, because the alternative is what this file used to do: each call site
// assembled its own, and each one filled in the three handles the Lua registry
// happens to hold. Everything else — audio, the entity host, the HorizonCode
// runtime — silently stayed null, so those HE::api rows returned their neutral
// default and horizon.audio.* was a no-op in every Lua script ever written. One
// builder means the next row added to HE::api works from Lua on the day it
// lands, instead of the day someone notices.
static HE::api::Ctx apiCtx(lua_State* L)
{
    const ScriptContext::HostServices& hs = ScriptContext::hostServices();
    // Assigned member by member rather than built as an aggregate: Ctx grows,
    // and a positional list silently mis-assigns when a field is inserted.
    HE::api::Ctx c{};
    c.world    = getWorld(L);
    c.physics  = getPhysics(L);
    c.content  = getContent(L);
    c.audio    = hs.audio;
    c.entities = hs.entities;
    c.runtime  = hs.runtime;
    // `self` stays 0, and that is the answer, not an omission: a Lua script is
    // not a HorizonCode instance, so there is no object id to name. The rows
    // that ask (entity.self, entity.selfObject) correctly report "no object".
    c.self          = 0;
    c.createObject  = hs.createObject;
    c.destroyObject = hs.destroyObject;
    // What "quit" means, as the host bound it — horizon.app.quit is a logged
    // no-op without it, so a Lua-only project could not close its own game. Read
    // from the registry rather than from HostServices: the quit hook predates
    // that holder and points AT the owning context's member, so a later
    // setQuitHandler is visible without re-registering anything.
    if (const std::function<void()>* quit = getQuit(L)) c.requestQuit = *quit;
    return c;
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

// The two transform WRITES go through HE::api::transform rather than straight to
// ScriptApi: that layer turns a write on an entity with a body or a character
// into a teleport. Without it the physics step overwrote the transform in the
// same frame, so horizon.setPosition/setRotation were silent no-ops on exactly
// the entities a script moves most — a Lua respawn never arrived. The arity the
// bindings expose is unchanged; only who does the work is.
static int lua_horizon_setPosition(lua_State* L)
{
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    HE::api::Ctx c = apiCtx(L);
    HE::api::transform::setPosition(c, id,
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
    HE::api::Ctx c = apiCtx(L);
    HE::api::transform::setRotation(c, id,
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

// ── Struct values on the Lua boundary ────────────────────────────────────────
// A struct crosses as a plain table: named fields plus `__type` = the
// definition asset's path (what horizon.structs.<Name>() constructors emit).
// Field encodings inside the table: Vec2 = {x, y}, Color = {r, g, b, a},
// Transform = { pos={x,y,z}, rot={x,y,z}, scl={x,y,z} }, arrays = sequences,
// nested structs = nested tables. Missing fields keep the definition default;
// unknown keys are ignored. Depth-capped so a self-referencing table degrades
// instead of recursing forever.
static HorizonCode::Value luaToStructValue(lua_State* L, int idx, int depth);
static void luaPushStructTable(lua_State* L, const HorizonCode::Value& v, int depth);

static void luaPushVec3Table(lua_State* L, const glm::vec3& v)
{
    lua_createtable(L, 3, 0);
    lua_pushnumber(L, v.x); lua_rawseti(L, -2, 1);
    lua_pushnumber(L, v.y); lua_rawseti(L, -2, 2);
    lua_pushnumber(L, v.z); lua_rawseti(L, -2, 3);
}
static glm::vec3 luaToVec3(lua_State* L, int idx, const glm::vec3& def)
{
    glm::vec3 r = def;
    if (!lua_istable(L, idx)) return r;
    for (int i = 0; i < 3; ++i)
    { lua_rawgeti(L, idx, i + 1); if (lua_isnumber(L, -1)) r[i] = (float)lua_tonumber(L, -1); lua_pop(L, 1); }
    return r;
}

// Push ONE field value as ONE Lua value (unlike the top-level ABI, which
// spreads Vec2/Color — inside a table each field is a single slot).
static void luaPushFieldValue(lua_State* L, const HorizonCode::Value& v, int depth)
{
    using P = HorizonCode::PinType;
    // A MAP crosses as a plain key→value table PLUS a `__keys` array holding the
    // iteration order. Lua tables have no order of their own, and the whole
    // point of a HorizonCode map is that it has one — without the sidecar a
    // script could read the pairs but never the sequence, and a value handed
    // back would come home shuffled.
    if (v.kind() == HorizonCode::ContainerKind::Map)
    {
        const size_t n = std::min(v.keys.size(), v.items.size());
        lua_createtable(L, 0, (int)n + 1);
        for (size_t i = 0; i < n; ++i)
        {
            HorizonCode::Value key = v.keys[i]; key.isArray = false;
            key.container = HorizonCode::ContainerKind::None;
            HorizonCode::Value val = v.items[i]; val.isArray = false;
            val.container = HorizonCode::ContainerKind::None;
            luaPushFieldValue(L, key, depth + 1);
            luaPushFieldValue(L, val, depth + 1);
            lua_rawset(L, -3);
        }
        lua_createtable(L, (int)n, 0);
        for (size_t i = 0; i < n; ++i)
        {
            HorizonCode::Value key = v.keys[i]; key.isArray = false;
            key.container = HorizonCode::ContainerKind::None;
            luaPushFieldValue(L, key, depth + 1);
            lua_rawseti(L, -2, (int)i + 1);
        }
        lua_setfield(L, -2, "__keys");
        return;
    }
    // Array AND Set: an ordered list. A set's items ARE its iteration order, so
    // the two look the same from Lua — the declared type decides which it is,
    // and a list coming back into a set is de-duplicated there.
    if (v.isArray)
    {
        lua_createtable(L, (int)v.items.size(), 0);
        for (size_t i = 0; i < v.items.size(); ++i)
        {
            HorizonCode::Value item = v.items[i];
            item.isArray = false;
            item.container = HorizonCode::ContainerKind::None;
            luaPushFieldValue(L, item, depth + 1);
            lua_rawseti(L, -2, (int)i + 1);
        }
        return;
    }
    switch (v.type)
    {
    case P::Bool:   lua_pushboolean(L, v.b); break;
    case P::Int:    lua_pushinteger(L, v.i); break;
    case P::Enum:   lua_pushinteger(L, v.i); break;
    case P::String: lua_pushstring(L, v.s.c_str()); break;
    case P::Vec2:
        lua_createtable(L, 2, 0);
        lua_pushnumber(L, v.v2.x); lua_rawseti(L, -2, 1);
        lua_pushnumber(L, v.v2.y); lua_rawseti(L, -2, 2);
        break;
    case P::Color:
        lua_createtable(L, 4, 0);
        lua_pushnumber(L, v.col.x); lua_rawseti(L, -2, 1);
        lua_pushnumber(L, v.col.y); lua_rawseti(L, -2, 2);
        lua_pushnumber(L, v.col.z); lua_rawseti(L, -2, 3);
        lua_pushnumber(L, v.col.w); lua_rawseti(L, -2, 4);
        break;
    case P::Transform:
        lua_createtable(L, 0, 3);
        luaPushVec3Table(L, v.tpos); lua_setfield(L, -2, "pos");
        luaPushVec3Table(L, v.trot); lua_setfield(L, -2, "rot");
        luaPushVec3Table(L, v.tscl); lua_setfield(L, -2, "scl");
        break;
    case P::Struct:
        luaPushStructTable(L, v, depth + 1);
        break;
    case P::Float:
    default:        lua_pushnumber(L, v.f); break;
    }
}

static void luaPushStructTable(lua_State* L, const HorizonCode::Value& v, int depth)
{
    HE::StructDef def;
    if (depth > 16 || !HE::TypeRegistry::instance().getStruct(v.typeName, def))
    { lua_pushnil(L); return; }
    lua_createtable(L, 0, (int)def.fields.size() + 1);
    lua_pushstring(L, v.typeName.c_str());
    lua_setfield(L, -2, "__type");
    for (size_t i = 0; i < def.fields.size(); ++i)
    {
        if (i < v.items.size()) luaPushFieldValue(L, v.items[i], depth);
        else                    luaPushFieldValue(L, HorizonCode::Value{}, depth);
        lua_setfield(L, -2, def.fields[i].name.c_str());
    }
}

// Read ONE field value from ONE Lua slot at absolute index `idx`.
static HorizonCode::Value luaToFieldValue(lua_State* L, int idx, const HE::StructField& f, int depth)
{
    using P = HorizonCode::PinType; using V = HorizonCode::Value;
    using CK = HorizonCode::ContainerKind;
    // A SCALAR view of the field, for decoding one element or one key.
    auto scalarField = [&f](P t, const std::string& tn)
    {
        HE::StructField e = f;
        e.isArray = false; e.container = CK::None;
        e.type = t; e.typeName = tn;
        return e;
    };
    if (f.kind() == CK::Map)
    {
        V map; map.isArray = true; map.container = CK::Map;
        map.type = f.type; map.typeName = f.typeName;
        map.keyType = f.keyType; map.keyTypeName = f.keyTypeName;
        if (lua_istable(L, idx) && depth <= 16)
        {
            const HE::StructField kf = scalarField(f.keyType, f.keyTypeName);
            const HE::StructField vf = scalarField(f.type, f.typeName);
            auto take = [&](const V& key)
            {
                for (const V& have : map.keys)
                    if (HorizonCode::scalarValueEquals(have, key, f.keyType)) return;
                luaPushFieldValue(L, key, depth + 1);   // look the value up by this key
                lua_rawget(L, idx);
                map.keys.push_back(key);
                map.items.push_back(luaToFieldValue(L, lua_gettop(L), vf, depth + 1));
                lua_pop(L, 1);
            };
            lua_getfield(L, idx, "__keys");
            if (lua_istable(L, -1))
            {
                // The order the engine handed out, or one the script built on
                // purpose: authoritative.
                const int order = lua_gettop(L);
                const int n = (int)lua_rawlen(L, order);
                for (int i = 1; i <= n; ++i)
                {
                    lua_rawgeti(L, order, i);
                    take(luaToFieldValue(L, lua_gettop(L), kf, depth + 1));
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);
            }
            else
            {
                lua_pop(L, 1);
                // A table a script built by hand, with no order attached. `pairs`
                // order is a hash-table artefact and would differ between runs,
                // so the keys are SORTED instead: arbitrary, but the same every
                // time, which is the property the map actually needs.
                std::vector<V> keys;
                lua_pushnil(L);
                while (lua_next(L, idx) != 0)
                {
                    lua_pop(L, 1);                       // drop the value, keep the key
                    // Read a COPY of the key: lua_tostring converts a numeric
                    // key IN PLACE, and mutating the table's key mid-traversal
                    // is what breaks lua_next.
                    lua_pushvalue(L, -1);
                    const bool sidecar = lua_type(L, -1) == LUA_TSTRING &&
                                         std::string(lua_tostring(L, -1)) == "__keys";
                    if (!sidecar)
                        keys.push_back(luaToFieldValue(L, lua_gettop(L), kf, depth + 1));
                    lua_pop(L, 1);
                }
                std::sort(keys.begin(), keys.end(), [&](const V& a, const V& b)
                {
                    return f.keyType == P::String ? a.s < b.s
                         : f.keyType == P::Ref    ? a.ref < b.ref
                                                  : a.i < b.i;   // Int and Enum
                });
                for (const V& k : keys) take(k);
            }
        }
        return map;
    }
    if (f.isArray)
    {
        V arr; arr.isArray = true; arr.container = f.kind();
        arr.type = f.type; arr.typeName = f.typeName;
        if (lua_istable(L, idx) && depth <= 16)
        {
            const HE::StructField elem = scalarField(f.type, f.typeName);
            const int n = (int)lua_rawlen(L, idx);
            for (int i = 1; i <= n; ++i)
            {
                lua_rawgeti(L, idx, i);
                V item = luaToFieldValue(L, lua_gettop(L), elem, depth + 1);
                lua_pop(L, 1);
                if (arr.container == CK::Set)
                {
                    // A list from a script may repeat; a set keeps the FIRST
                    // occurrence, exactly like Set Add.
                    bool dup = false;
                    for (const V& have : arr.items)
                        if (HorizonCode::scalarValueEquals(have, item, f.type)) { dup = true; break; }
                    if (dup) continue;
                }
                arr.items.push_back(std::move(item));
            }
        }
        return arr;
    }
    switch (f.type)
    {
    case P::Bool:   return V::ofBool(lua_toboolean(L, idx) != 0);
    case P::Int:    return V::ofInt((int)lua_tointeger(L, idx));
    case P::Enum:
    {
        V v; v.type = P::Enum; v.typeName = f.typeName; v.i = (int)lua_tointeger(L, idx);
        return v;
    }
    case P::String: return V::ofString(lua_isstring(L, idx) ? lua_tostring(L, idx) : "");
    case P::Vec2:
    {
        glm::vec2 r{ 0.0f };
        if (lua_istable(L, idx))
            for (int i = 0; i < 2; ++i)
            { lua_rawgeti(L, idx, i + 1); if (lua_isnumber(L, -1)) r[i] = (float)lua_tonumber(L, -1); lua_pop(L, 1); }
        return V::ofVec2(r);
    }
    case P::Color:
    {
        glm::vec4 r{ 0.0f, 0.0f, 0.0f, 1.0f };
        if (lua_istable(L, idx))
            for (int i = 0; i < 4; ++i)
            { lua_rawgeti(L, idx, i + 1); if (lua_isnumber(L, -1)) r[i] = (float)lua_tonumber(L, -1); lua_pop(L, 1); }
        return V::ofColor(r);
    }
    case P::Transform:
    {
        glm::vec3 p{ 0.0f }, rot{ 0.0f }, scl{ 1.0f };
        if (lua_istable(L, idx))
        {
            lua_getfield(L, idx, "pos"); p   = luaToVec3(L, lua_gettop(L), p);   lua_pop(L, 1);
            lua_getfield(L, idx, "rot"); rot = luaToVec3(L, lua_gettop(L), rot); lua_pop(L, 1);
            lua_getfield(L, idx, "scl"); scl = luaToVec3(L, lua_gettop(L), scl); lua_pop(L, 1);
        }
        return V::ofTransform(p, rot, scl);
    }
    case P::Struct:
        return luaToStructValue(L, idx, depth + 1);
    case P::Float:
    default:        return V::ofFloat((float)lua_tonumber(L, idx));
    }
}

static HorizonCode::Value luaToStructValue(lua_State* L, int idx, int depth)
{
    using P = HorizonCode::PinType;
    HorizonCode::Value v; v.type = P::Struct;
    if (depth > 16 || !lua_istable(L, idx)) return v;
    idx = lua_absindex(L, idx);
    lua_getfield(L, idx, "__type");
    if (lua_isstring(L, -1)) v.typeName = lua_tostring(L, -1);
    lua_pop(L, 1);
    HE::StructDef def;
    if (!HE::TypeRegistry::instance().getStruct(v.typeName, def)) return v;
    // Seed the definition defaults, then overwrite fields present in the table.
    v = HE::TypeRegistry::instance().makeDefaultValue(v.typeName);
    for (size_t i = 0; i < def.fields.size(); ++i)
    {
        lua_getfield(L, idx, def.fields[i].name.c_str());
        if (!lua_isnil(L, -1) && i < v.items.size())
            v.items[i] = luaToFieldValue(L, lua_gettop(L), def.fields[i], depth);
        lua_pop(L, 1);
    }
    return v;
}

static HorizonCode::Value luaReadValue(lua_State* L, int& idx, HorizonCode::PinType t)
{
    using P = HorizonCode::PinType; using V = HorizonCode::Value;
    switch (t)
    {
    case P::Bool:   return V::ofBool(lua_toboolean(L, idx++) != 0);
    case P::Int:    return V::ofInt(static_cast<int>(luaL_checkinteger(L, idx++)));
    case P::Enum:   return V::ofInt(static_cast<int>(luaL_checkinteger(L, idx++)));
    case P::String: return V::ofString(luaL_checkstring(L, idx++));
    case P::Vec2:   { float x = (float)luaL_checknumber(L, idx++), y = (float)luaL_checknumber(L, idx++);
                      return V::ofVec2({ x, y }); }
    case P::Color:  { float r = (float)luaL_checknumber(L, idx++), g = (float)luaL_checknumber(L, idx++),
                            b = (float)luaL_checknumber(L, idx++), a = (float)luaL_checknumber(L, idx++);
                      return V::ofColor({ r, g, b, a }); }
    // A Vec3 takes THREE numbers, not four. That is the point of the type
    // existing: horizon.transform.setPosition(e, x, y, z) used to demand a
    // fourth number nobody had, because the parameter was a Color.
    case P::Vec3:   { float x = (float)luaL_checknumber(L, idx++), y = (float)luaL_checknumber(L, idx++),
                            z = (float)luaL_checknumber(L, idx++);
                      return V::ofVec3({ x, y, z }); }
    case P::Vec4:   { float x = (float)luaL_checknumber(L, idx++), y = (float)luaL_checknumber(L, idx++),
                            z = (float)luaL_checknumber(L, idx++), w = (float)luaL_checknumber(L, idx++);
                      return V::ofVec4({ x, y, z, w }); }
    case P::Ref:    return V::ofRef(static_cast<uint32_t>(luaL_checkinteger(L, idx++)));
    case P::Struct: return luaToStructValue(L, idx++, 0);
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
    case P::Vec3:   lua_pushnumber(L, v.v3.x); lua_pushnumber(L, v.v3.y);
                    lua_pushnumber(L, v.v3.z); return 3;
    case P::Vec4:   lua_pushnumber(L, v.v4.x); lua_pushnumber(L, v.v4.y);
                    lua_pushnumber(L, v.v4.z); lua_pushnumber(L, v.v4.w); return 4;
    case P::Ref:    lua_pushinteger(L, static_cast<lua_Integer>(v.ref)); return 1;
    case P::Enum:   lua_pushinteger(L, v.i); return 1;
    case P::Struct: luaPushStructTable(L, v, 0); return 1;
    case P::Float:
    default:        lua_pushnumber(L, v.f); return 1;
    }
}

// Upvalue 1 = the ApiFn* (a stable pointer into the process-lifetime registry).
static int lua_engine_dispatch(lua_State* L)
{
    const auto* fn = static_cast<const HE::api::ApiFn*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!fn) return 0;
    HE::api::Ctx c = apiCtx(L);
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

// ── User-defined types: horizon.enums.<Name>.<Entry> + horizon.structs.<Name>() ──
// Generated from the TypeRegistry at context creation (the registry is complete
// by then: editor = project open, game = the pak's type index). Constructors
// build the boundary-table form documented at luaToStructValue, so a table from
// horizon.structs.X() round-trips through any Struct-typed API parameter.
static void registerUserTypes(lua_State* L)
{
    auto& reg = HE::TypeRegistry::instance();
    auto luaStr = [](const std::string& in) {
        std::string out = "\"";
        for (char c : in)
        {
            if (c == '\\' || c == '\"') { out += '\\'; out += c; }
            else if (c == '\n') out += "\\n";
            else out += c;
        }
        out += "\"";
        return out;
    };
    std::function<std::string(const HorizonCode::Value&)> lit =
        [&](const HorizonCode::Value& v) -> std::string {
        using P = HorizonCode::PinType;
        // A MAP constructor emits the same shape luaPushFieldValue does: the
        // pairs plus a `__keys` array carrying the authored order.
        if (v.kind() == HorizonCode::ContainerKind::Map)
        {
            const size_t n = std::min(v.keys.size(), v.items.size());
            if (n == 0) return "{__keys={}}";
            auto scalar = [](HorizonCode::Value x)
            { x.isArray = false; x.container = HorizonCode::ContainerKind::None; return x; };
            std::string t = "{";
            for (size_t i = 0; i < n; ++i)
            {
                if (i) t += ",";
                t += "[" + lit(scalar(v.keys[i])) + "]=" + lit(scalar(v.items[i]));
            }
            t += ",__keys={";
            for (size_t i = 0; i < n; ++i)
            { if (i) t += ","; t += lit(scalar(v.keys[i])); }
            return t + "}}";
        }
        if (v.isArray)
        {
            // The authored slots, not an unconditional empty table (the value
            // arrives already seeded from TypeRegistry::makeDefaultValue).
            if (v.items.empty()) return "{}";
            std::string t = "{";
            for (size_t i = 0; i < v.items.size(); ++i)
            {
                if (i) t += ",";
                HorizonCode::Value item = v.items[i];
                item.isArray = false;
                item.container = HorizonCode::ContainerKind::None;
                t += lit(item);
            }
            return t + "}";
        }
        switch (v.type)
        {
        case P::Bool:   return v.b ? "true" : "false";
        case P::Int:
        case P::Enum:   return std::to_string(v.i);
        case P::String: return luaStr(v.s);
        case P::Vec2:   return "{" + std::to_string(v.v2.x) + "," + std::to_string(v.v2.y) + "}";
        case P::Color:  return "{" + std::to_string(v.col.x) + "," + std::to_string(v.col.y) + ","
                             + std::to_string(v.col.z) + "," + std::to_string(v.col.w) + "}";
        case P::Transform:
            return "{pos={" + std::to_string(v.tpos.x) + "," + std::to_string(v.tpos.y) + "," + std::to_string(v.tpos.z)
                 + "},rot={" + std::to_string(v.trot.x) + "," + std::to_string(v.trot.y) + "," + std::to_string(v.trot.z)
                 + "},scl={" + std::to_string(v.tscl.x) + "," + std::to_string(v.tscl.y) + "," + std::to_string(v.tscl.z) + "}}";
        case P::Struct:
        {
            HE::StructDef def;
            if (!HE::TypeRegistry::instance().getStruct(v.typeName, def)) return "nil";
            std::string t = "{__type=" + luaStr(v.typeName);
            for (size_t i = 0; i < def.fields.size() && i < v.items.size(); ++i)
                t += ",[" + luaStr(def.fields[i].name) + "]=" + lit(v.items[i]);
            t += "}";
            return t;
        }
        case P::Float:
        default:        return std::to_string(v.f);
        }
    };

    std::string src = "horizon.enums = horizon.enums or {}\n"
                      "horizon.structs = horizon.structs or {}\n";
    for (const auto& d : reg.enums())
    {
        src += "horizon.enums[" + luaStr(d.name) + "] = {";
        for (const auto& e : d.entries)
            src += "[" + luaStr(e.name) + "]=" + std::to_string(e.value) + ",";
        src += "}\n";
    }
    for (const auto& d : reg.structs())
        src += "horizon.structs[" + luaStr(d.name) + "] = function() return "
             + lit(reg.makeDefaultValue(d.assetPath)) + " end\n";

    if (luaL_dostring(L, src.c_str()) != LUA_OK)
    {
        HE_LOG_WARN(Script, "%s", ("ScriptContext: user-type bootstrap failed: " +
            std::string(lua_isstring(L, -1) ? lua_tostring(L, -1) : "?")).c_str());
        lua_pop(L, 1);
    }
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

namespace
{
// ── The Python plugin is loaded once per process and NEVER unloaded ──────────
// This is not laziness, it is a requirement, and it cost a crash to find.
//
// CPython keeps pointers INTO the plugin module: the inittab entry for the
// built-in `horizon` module, its init function, its type objects. The
// interpreter is also deliberately never finalised (Py_Finalize is a
// process-wide door the engine does not close, because a second ScriptContext
// may still want it). Unload the library and the live interpreter is left
// holding addresses in unmapped memory; load it again afterwards and the fresh
// copy sees its own statics reset, tries to register `horizon` a second time,
// and CPython aborts the process with
//   "PyImport_AppendInittab() may not be called after Py_Initialize()".
//
// So the handle outlives every ScriptContext. Backends are still created and
// destroyed per context — only the module stays.
struct PythonPlugin
{
    HE::DynLib        lib;
    HePythonCreateFn  create  = nullptr;
    HePythonDestroyFn destroy = nullptr;
    bool              tried   = false;   // resolution is attempted exactly once
};

PythonPlugin& pythonPlugin()
{
    static PythonPlugin p;
    return p;
}

// Where THIS library sits on disk. The Python plugin is deployed next to
// HorizonScene, and asking the loader where HorizonScene itself came from is
// the only answer that survives every layout we ship: flat beside the
// executable, a macOS .app (libraries in Frameworks/ while SDL_GetBasePath
// points at Resources/), and a developer build tree.
std::filesystem::path thisLibraryDir()
{
#if defined(_WIN32)
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&thisLibraryDir), &mod))
        return {};
    wchar_t buf[MAX_PATH]{};
    if (GetModuleFileNameW(mod, buf, MAX_PATH) == 0) return {};
    return std::filesystem::path(buf).parent_path();
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&thisLibraryDir), &info) == 0 || !info.dli_fname)
        return {};
    return std::filesystem::path(info.dli_fname).parent_path();
#endif
}
} // namespace

ScriptContext::ScriptContext(HorizonWorld& world)
    : m_world(&world)
{
    registerHorizonApi();
    loadPythonPlugin();
}

void ScriptContext::loadPythonPlugin()
{
    PythonPlugin& plugin = pythonPlugin();

    // Resolve once per process; every later ScriptContext reuses the answer,
    // including the answer "there is no plugin".
    if (!plugin.tried)
    {
        plugin.tried = true;

        const std::filesystem::path dir = thisLibraryDir();
        if (dir.empty())
        {
            HE_LOG_WARN(Script, "%s", "Could not determine the engine library directory — "
                                      "Python scripting unavailable");
        }
        else
        {
            const std::filesystem::path lib = dir / HE::PythonPlugin::fileName();
            std::error_code ec;
            if (!std::filesystem::exists(lib, ec) || ec)
            {
                // Not a failure. A build without CPython, and every exported
                // game whose project does not use Python, has no plugin here.
                HE_LOG_INFO(Script, "No Python plugin at %s — Lua only", lib.string().c_str());
            }
            else if (!plugin.lib.load(lib))
            {
                // From here on every exit IS a failure and says so. The
                // distinction matters: "this project has no Python" and "Python
                // is here but broken" used to look identical from the outside,
                // and only the second one is a bug report.
                HE_LOG_ERROR(Script, "Python plugin present but failed to load: %s — usually "
                                     "a missing libpython beside it, or an architecture "
                                     "mismatch", lib.string().c_str());
            }
            else
            {
                plugin.create = reinterpret_cast<HePythonCreateFn>(
                    plugin.lib.getSymbol(HE::PythonPlugin::kCreateSymbol));
                plugin.destroy = reinterpret_cast<HePythonDestroyFn>(
                    plugin.lib.getSymbol(HE::PythonPlugin::kDestroySymbol));
                if (!plugin.create || !plugin.destroy)
                {
                    HE_LOG_ERROR(Script, "Python plugin %s is missing its entry points — "
                                         "stale or mismatched build",
                                 lib.filename().string().c_str());
                    plugin.create  = nullptr;
                    plugin.destroy = nullptr;
                    // Deliberately NOT unloaded — see the note above. Nothing
                    // ran, but keeping one rule is simpler than two.
                }
                else
                {
                    HE_LOG_INFO(Script, "Python plugin loaded from %s",
                                lib.filename().string().c_str());
                }
            }
        }
    }

    if (!plugin.create) return;   // no Python in this process; Lua only

    IScriptBackend* backend = plugin.create(m_world);
    if (!backend)
    {
        HE_LOG_ERROR(Script, "%s", "Python plugin loaded but the interpreter would not start");
        return;
    }
    m_py = std::unique_ptr<IScriptBackend, PyBackendDeleter>(
        backend, PyBackendDeleter{plugin.destroy});
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

    // The quit hook is registered as the ADDRESS of the member, once: the member
    // outlives this state, so setQuitHandler only has to assign it.
    lua_pushlightuserdata(L, &m_quitHandler);
    lua_setfield(L, LUA_REGISTRYINDEX, kQuitKey);

    // Create `horizon` global table and register all functions
    luaL_newlib(L, kHorizonFuncs);
    lua_setglobal(L, "horizon");

    // Registry-driven groups (horizon.math.*, …) layered on top of the flat API.
    registerEngineApiGroups(L);
    registerUserTypes(L);
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

// The published copy of the active context's quit handler. It sits at file scope
// because its reader is in ANOTHER module — the CPython plugin, which has no
// ScriptContext to ask and reaches it through hostQuitHandler() below.
static std::function<void()> s_hostQuitHandler;
// Who published it, so a context that dies clears only its own handler and never
// a newer context's.
static const ScriptContext* s_quitPublisher = nullptr;

const std::function<void()>& ScriptContext::hostQuitHandler() { return s_hostQuitHandler; }

// The active context's host services, published for the same reason and in the
// same shape as the quit handler above: the reader is in ANOTHER module — the
// CPython plugin, which has no ScriptContext to ask — and the Lua side reads it
// here too, so both languages get one source instead of one of them going
// without. Everything in it is a raw pointer into the HOST, which is why the
// publisher is tracked and withdrawn (see the destructor): these outlive a
// context only until its session ends, and a pointer to a destroyed EntityHost
// is worse than a null one.
static ScriptContext::HostServices s_hostServices;
static const ScriptContext*        s_servicesPublisher = nullptr;

const ScriptContext::HostServices& ScriptContext::hostServices() { return s_hostServices; }

ScriptContext::~ScriptContext()
{
    if (s_quitPublisher == this)
    {
        s_hostQuitHandler = nullptr;
        s_quitPublisher   = nullptr;
    }
    // Same withdrawal, same reason: a context that dies clears only its OWN
    // publication and never a newer context's. This is what keeps a scene switch
    // or a play stop from leaving audio/entity-host/runtime pointers aimed at
    // freed hosts. It also runs BEFORE m_py is destroyed (destructor body, then
    // members), so a Python finalizer firing during that teardown builds a Ctx
    // with those services already null — the same protection ~PyScriptBackend
    // gives itself by nulling g_physics.
    if (s_servicesPublisher == this)
    {
        s_hostServices      = {};
        s_servicesPublisher = nullptr;
    }
}

void ScriptContext::setHostServices(HostServices s)
{
    s_hostServices      = std::move(s);
    s_servicesPublisher = this;
}

void ScriptContext::setQuitHandler(std::function<void()> fn)
{
    m_quitHandler = std::move(fn);
    // Lua already sees the member through the registry pointer. Python cannot:
    // its backend is a separate library that builds its own Ctx, so the handler
    // is published where that module can read it — one source for both, instead
    // of quit working in one language and not the other.
    s_hostQuitHandler = m_quitHandler;
    s_quitPublisher   = this;
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

    // The same component slot also carries HorizonCode CLASSES, which EntityHost
    // runs. Quietly not ours — every entity in the world is offered to both, and
    // the split is the referenced asset's TYPE. Without this the error below
    // would fire for every scripted-in-HorizonCode entity in the scene.
    if (cm.assetType(sc->scriptAssetId) == HE::AssetType::HorizonCodeClass)
        return ScriptEngine::kInvalidInstance;

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
        // HorizonCode classes share this component but belong to EntityHost —
        // counting them here would report every one of them as a script that
        // "failed to start".
        if (cm.assetType(sc.scriptAssetId) == HE::AssetType::HorizonCodeClass) continue;
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

bool ScriptContext::callOnBeginOverlap(ScriptEngine::InstanceId id, uint32_t otherEntityId)
{
    IScriptBackend* b = backendForId(id); m_lastBackend = b;
    HE_SCRIPT_CALL("onBeginOverlap", b->callOnBeginOverlap(rawId(id), otherEntityId));
}

bool ScriptContext::callOnEndOverlap(ScriptEngine::InstanceId id, uint32_t otherEntityId)
{
    IScriptBackend* b = backendForId(id); m_lastBackend = b;
    HE_SCRIPT_CALL("onEndOverlap", b->callOnEndOverlap(rawId(id), otherEntityId));
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
