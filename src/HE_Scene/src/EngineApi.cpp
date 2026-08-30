#include "HorizonScene/EngineApi.h"
#include <cstdint>
#include "HorizonScene/ScriptApi.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/TransformHierarchy.h"   // world position, composed on demand
#include "HorizonScene/PhysicsWorld.h"
#include "HorizonScene/AudioEngine.h"
#include "HorizonScene/Components/CameraComponent.h"
#include "HorizonScene/Components/CameraRigComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/Components/EnvironmentComponent.h"
#include "HorizonScene/Components/NameComponent.h"
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/Components/SkeletalMeshComponent.h"
#include "HorizonScene/Components/AnimatorStateMachineComponent.h"
#include "HorizonScene/Components/MovementComponent.h"
#include "HorizonScene/Components/CharacterControllerComponent.h"
#include "HorizonScene/EntityHost.h"
#include <glm/gtc/quaternion.hpp>
#include "HorizonScene/Components/LightComponent.h"
#include "HorizonScene/Components/ParticleSystemComponent.h"
#include "HorizonScene/Components/FoliageComponent.h"
#include "HorizonScene/Components/EntityIdComponent.h"
#include "HorizonScene/Components/SaveStateComponent.h"
#include <Hpak/ProjectExporter.h>   // sceneUuidForPath (packed scene index)
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Types/TypeRegistry.h>   // save-template schemas + struct field values
#include <HorizonGameServices.h>   // the C-ABI table fillSaveServices populates
#include <DebugDraw/DebugDraw.h>
#include <Diagnostics/Logger.h>   // loud save-v2 failures
#include <SDL3/SDL.h>              // input::pushSdlSnapshot (live keyboard/mouse poll)
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <chrono>   // datetime::now
#include <ctime>    // datetime: strftime + localtime_r/localtime_s
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <unordered_set>

// File-local alias. It used to arrive transitively from the public
// HorizonRendering/ShaderManager.h, which declared it at global scope and so
// leaked `fs` into every consumer of that header.
namespace fs = std::filesystem;


// The engine surface (transform/physics/material/ui/widget/cursor/entity) is a
// thin promotion of the language-neutral ScriptApi — same behavior, now bundled
// behind an explicit Ctx and grouped by subsystem, so one call reads the same in
// every frontend. The math group is native (pure C++). A later phase inverts the
// dependency (ScriptApi becomes a shim over HE::api) and adds the subsystems that
// need new Ctx providers: audio, input, camera, time, scene, fs/save.

namespace HE::api {


// ── Debug ────────────────────────────────────────────────────────────────────
void log(Ctx&, const std::string& message) { ScriptApi::log(message.c_str()); }

namespace {
// Flip every renderable component the entity carries. One definition for both
// callers — the per-entity toggle (entity::setVisible) and zone hiding
// (scene::setZoneVisible) mean exactly the same thing by "visible", so a
// component type added here must show up in both. Caller validates the entity.
void setEntityVisible(entt::registry& reg, entt::entity e, bool visible)
{
    if (auto* m  = reg.try_get<MeshComponent>(e))           m->visible  = visible;
    if (auto* sm = reg.try_get<SkeletalMeshComponent>(e))   sm->visible = visible;
    if (auto* l  = reg.try_get<LightComponent>(e))          l->visible  = visible;
    if (auto* ps = reg.try_get<ParticleSystemComponent>(e)) ps->visible = visible;
    if (auto* f  = reg.try_get<FoliageComponent>(e))        f->visible  = visible;
}
} // namespace

// ── Entities ─────────────────────────────────────────────────────────────────
namespace entity {
std::string getName(Ctx& c, Entity e)                          { return c.world ? ScriptApi::getName(*c.world, e) : std::string(); }
Entity      spawn(Ctx& c, Entity parent, const std::string& n) { return c.world ? ScriptApi::spawn(*c.world, parent, n) : 0u; }
void        destroy(Ctx& c, Entity e)                          { if (c.world) ScriptApi::destroy(*c.world, e); }
float       distance(Ctx& c, Entity a, Entity b)
{
    if (!c.world) return -1.0f;
    return glm::length(ScriptApi::getPosition(*c.world, a) - ScriptApi::getPosition(*c.world, b));
}
Entity findByName(Ctx& c, const std::string& name)
{
    if (!c.world || name.empty()) return 0u;
    auto view = c.world->registry().view<NameComponent>();
    for (auto [e, nc] : view.each())
        if (nc.name == name) return (Entity)e;
    return 0u;
}
bool exists(Ctx& c, Entity e)
{
    return c.world && c.world->registry().valid((entt::entity)e);
}
Entity self(Ctx& c)
{
    return c.runtime ? c.runtime->ownedEntity(c.self) : 0u;
}
Entity owned(Ctx& c, uint32_t objectRef)
{
    return c.runtime ? c.runtime->ownedEntity(objectRef) : 0u;
}
uint32_t instance(Ctx& c, Entity e)
{
    // Asked of EntityHost rather than the Runtime on purpose: several instances
    // may own one entity — a character's class and its animator's sync graph
    // both do — so a reverse lookup in the runtime could not say which one is
    // meant. EntityHost holds exactly the class bindings, which is the answer.
    return c.entities ? static_cast<uint32_t>(c.entities->instanceOf((entt::entity)e)) : 0u;
}
uint32_t selfObject(Ctx& c)
{
    // NOT the caller's own instance: an animator's sync graph asking this means
    // "the character I animate", and that character's class is a different
    // instance sitting on the same entity.
    return instance(c, self(c));
}
void setVisible(Ctx& c, Entity e, bool visible)
{
    if (!c.world || !c.world->registry().valid((entt::entity)e)) return;
    setEntityVisible(c.world->registry(), (entt::entity)e, visible);
}
bool getVisible(Ctx& c, Entity e)
{
    if (!c.world || !c.world->registry().valid((entt::entity)e)) return true;
    auto& reg = c.world->registry();
    const auto en = (entt::entity)e;
    if (const auto* m  = reg.try_get<MeshComponent>(en))           return m->visible;
    if (const auto* sm = reg.try_get<SkeletalMeshComponent>(en))   return sm->visible;
    if (const auto* l  = reg.try_get<LightComponent>(en))          return l->visible;
    if (const auto* ps = reg.try_get<ParticleSystemComponent>(en)) return ps->visible;
    if (const auto* f  = reg.try_get<FoliageComponent>(en))        return f->visible;
    return true;
}

// ── Savegame state ───────────────────────────────────────────────────────────
namespace {
// The stable identity the save keys on ("hi:lo", the pak-index spelling).
std::string entityUuidKey(Ctx& c, Entity e)
{
    if (!c.world || !c.world->registry().valid((entt::entity)e)) return {};
    if (auto* idc = c.world->registry().try_get<EntityIdComponent>((entt::entity)e))
        return std::to_string(idc->id.hi) + ":" + std::to_string(idc->id.lo);
    return {};
}
// Shared precondition walk for saveState/applySavedState — every miss logs.
const SaveStateComponent* saveStateGuard(const char* op, Ctx& c, Entity e, std::string& uuid)
{
    if (!save::inPlayMode())
    {
        HE_LOG_WARN(Script, "%s", (std::string("entity.") + op +
            ": only available in play mode (the editor's SceneSerializer owns edit-mode state)").c_str());
        return nullptr;
    }
    if (save::activeId().empty())
    {
        HE_LOG_WARN(Script, "%s", (std::string("entity.") + op +
            ": no active save — call save.create/load first").c_str());
        return nullptr;
    }
    uuid = entityUuidKey(c, e);
    if (uuid.empty())
    {
        HE_LOG_WARN(Script, "%s", (std::string("entity.") + op + ": invalid entity").c_str());
        return nullptr;
    }
    auto* ss = c.world->registry().try_get<SaveStateComponent>((entt::entity)e);
    if (!ss || !ss->enabled)
    {
        HE_LOG_WARN(Script, "%s", (std::string("entity.") + op +
            ": entity has no enabled SaveStateComponent").c_str());
        return nullptr;
    }
    return ss;
}
} // namespace

bool saveState(Ctx& c, Entity e)
{
    std::string uuid;
    const SaveStateComponent* ss = saveStateGuard("saveState", c, e, uuid);
    if (!ss) return false;
    nlohmann::json j = nlohmann::json::object();
    if (ss->saveTransform)
    {
        if (auto* t = c.world->registry().try_get<TransformComponent>((entt::entity)e))
            j["transform"] = {
                { "pos", { t->position.x, t->position.y, t->position.z } },
                { "rot", { t->rotation.x, t->rotation.y, t->rotation.z } },
                { "scl", { t->scale.x,    t->scale.y,    t->scale.z } } };
    }
    if (ss->saveVisibility)
        j["visible"] = getVisible(c, e);
    return save::setEntityState(uuid, j.dump());
}

bool hasSavedState(Ctx& c, Entity e)
{
    const std::string uuid = entityUuidKey(c, e);
    return !uuid.empty() && save::hasEntityState(uuid);
}

bool applySavedState(Ctx& c, Entity e)
{
    std::string uuid;
    const SaveStateComponent* ss = saveStateGuard("applySavedState", c, e, uuid);
    if (!ss) return false;
    const std::string text = save::entityState(uuid);
    if (text.empty())
    {
        HE_LOG_WARN(Script, "%s",
            "entity.applySavedState: the active save holds no state for this entity");
        return false;
    }
    nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return false;
    // Partial application: only what the save carries touches the instance.
    if (auto t = j.find("transform"); t != j.end() && t->is_object() && ss->saveTransform)
    {
        if (auto* tc = c.world->registry().try_get<TransformComponent>((entt::entity)e))
        {
            auto vec3 = [&](const char* key, glm::vec3& out) {
                auto it = t->find(key);
                if (it != t->end() && it->is_array() && it->size() >= 3)
                    out = { (*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>() };
            };
            vec3("pos", tc->position);
            vec3("rot", tc->rotation);
            vec3("scl", tc->scale);
        }
    }
    if (auto v = j.find("visible"); v != j.end() && v->is_boolean() && ss->saveVisibility)
        setVisible(c, e, v->get<bool>());
    return true;
}
} // namespace entity

// ── Transform ────────────────────────────────────────────────────────────────
namespace transform {
glm::vec3 getPosition(Ctx& c, Entity e)                    { return c.world ? ScriptApi::getPosition(*c.world, e) : glm::vec3(0.0f); }
void      setPosition(Ctx& c, Entity e, const glm::vec3& p){ if (c.world) ScriptApi::setPosition(*c.world, e, p); }
glm::vec3 getRotation(Ctx& c, Entity e)                    { return c.world ? ScriptApi::getRotation(*c.world, e) : glm::vec3(0.0f); }
void      setRotation(Ctx& c, Entity e, const glm::vec3& r){ if (c.world) ScriptApi::setRotation(*c.world, e, r); }
glm::vec3 getScale(Ctx& c, Entity e)                       { return c.world ? ScriptApi::getScale(*c.world, e) : glm::vec3(1.0f); }
void      setScale(Ctx& c, Entity e, const glm::vec3& s)   { if (c.world) ScriptApi::setScale(*c.world, e, s); }

glm::vec3 getWorldPosition(Ctx& c, Entity e)
{
    return c.world ? HE::worldPositionOf(*c.world, static_cast<entt::entity>(e))
                   : glm::vec3(0.0f);
}

void setWorldPosition(Ctx& c, Entity e, const glm::vec3& p)
{
    // Written through the LOCAL setter on purpose: that one is what marks the
    // transform dirty and what physics and the hierarchy already listen to.
    // Converting here and delegating keeps one writer for the position.
    if (c.world)
        ScriptApi::setPosition(*c.world, e,
            HE::localPositionForWorld(*c.world, static_cast<entt::entity>(e), p));
}
} // namespace transform

// ── Physics ──────────────────────────────────────────────────────────────────
namespace physics {
RaycastHit raycast(Ctx& c, const glm::vec3& o, const glm::vec3& d, float maxDist)
{
    const auto r = ScriptApi::raycast(c.physics, o, d, maxDist);
    return { r.hit, r.entityId, r.point, r.normal, r.distance };
}
// The rest reach PhysicsWorld directly rather than through ScriptApi. ScriptApi
// exists to serve the flat Lua/Python bindings, and those are frozen (adding to
// them changes a script-visible arity, see the note in ScriptContext.cpp) — a
// shim there would be a function nothing can ever call.
RaycastHit sphereCast(Ctx& c, const glm::vec3& o, const glm::vec3& d, float radius, float maxDist)
{
    RaycastHit out;
    if (!c.physics) return out;
    const PhysicsWorld::RaycastHit r = c.physics->sphereCast(o, d, radius, maxDist);
    return { r.hit, r.entityId, r.point, r.normal, r.distance };
}
std::vector<Entity> overlapSphere(Ctx& c, const glm::vec3& center, float radius)
{
    return c.physics ? c.physics->overlapSphere(center, radius) : std::vector<Entity>{};
}
bool addForce(Ctx& c, Entity e, const glm::vec3& f)   { return c.physics && c.physics->addForce(e, f); }
bool addImpulse(Ctx& c, Entity e, const glm::vec3& i) { return c.physics && c.physics->addImpulse(e, i); }
bool addTorque(Ctx& c, Entity e, const glm::vec3& t)  { return c.physics && c.physics->addTorque(e, t); }
void setVelocity(Ctx& c, Entity e, const glm::vec3& v) { ScriptApi::setVelocity(c.physics, e, v); }
glm::vec3 getVelocity(Ctx& c, Entity e)                { return c.physics ? c.physics->getVelocity(e) : glm::vec3(0.0f); }
bool isGrounded(Ctx& c, Entity e)                      { return ScriptApi::isGrounded(c.physics, e); }
void setGravity(Ctx& c, const glm::vec3& g)            { if (c.physics) c.physics->setGravity(g); }
glm::vec3 getGravity(Ctx& c)                           { return c.physics ? c.physics->gravity() : glm::vec3(0.0f); }
} // namespace physics

// ── Materials ────────────────────────────────────────────────────────────────
namespace material {
glm::vec4 getParam(Ctx& c, Entity e, const std::string& n)                     { return c.world ? ScriptApi::getMaterialParam(*c.world, c.content, e, n) : glm::vec4(0.0f); }
bool      setParam(Ctx& c, Entity e, const std::string& n, const glm::vec4& v) { return c.world ? ScriptApi::setMaterialParam(*c.world, c.content, e, n, v) : false; }
} // namespace material

// ── Animator ─────────────────────────────────────────────────────────────────
namespace animator {
namespace {
AnimatorStateMachineComponent* smOf(Ctx& c, Entity e)
{
    if (!c.world) return nullptr;
    auto& reg = c.world->registry();
    const auto id = (entt::entity)e;
    return reg.valid(id) ? reg.try_get<AnimatorStateMachineComponent>(id) : nullptr;
}
}
void setParam(Ctx& c, Entity e, const std::string& name, float value)
{
    // operator[] on purpose: a name the asset never declared is still a usable
    // parameter, exactly as it is when the scene file carries one. A transition
    // that reads an unknown name evaluates false rather than erroring, so a typo
    // is inert instead of fatal.
    if (auto* sm = smOf(c, e)) sm->params[name] = value;
}
float getParam(Ctx& c, Entity e, const std::string& name)
{
    if (auto* sm = smOf(c, e))
        if (auto it = sm->params.find(name); it != sm->params.end()) return it->second;
    return 0.0f;
}
std::string getState(Ctx& c, Entity e)
{
    auto* sm = smOf(c, e);
    return sm ? sm->currentStateName : std::string();
}
} // namespace animator

// ── Movement / Locomotion ────────────────────────────────────────────────────
namespace {
const CharacterControllerComponent* ccOf(Ctx& c, Entity e)
{
    if (!c.world) return nullptr;
    auto& reg = c.world->registry();
    const auto id = (entt::entity)e;
    return reg.valid(id) ? reg.try_get<CharacterControllerComponent>(id) : nullptr;
}
MovementComponent* mvOf(Ctx& c, Entity e)
{
    if (!c.world) return nullptr;
    auto& reg = c.world->registry();
    const auto id = (entt::entity)e;
    return reg.valid(id) ? reg.try_get<MovementComponent>(id) : nullptr;
}
} // namespace

namespace movement {
glm::vec3 velocity(Ctx& c, Entity e)
{
    const auto* cc = ccOf(c, e);
    return cc ? cc->velocity : glm::vec3(0.0f);
}
float speed(Ctx& c, Entity e)
{
    const glm::vec3 v = velocity(c, e);
    return glm::length(glm::vec2(v.x, v.z));   // horizontal only — falling is not running
}
float verticalSpeed(Ctx& c, Entity e) { return velocity(c, e).y; }
bool  isGrounded(Ctx& c, Entity e)
{
    const auto* cc = ccOf(c, e);
    return cc && cc->isGrounded;
}
namespace {
// Travel direction in the character's own frame. Both amounts come from the
// same projection, so they are computed together and read apart.
glm::vec2 localTravel(Ctx& c, Entity e)
{
    if (!c.world) return glm::vec2(0.0f);
    auto& reg = c.world->registry();
    const auto id = (entt::entity)e;
    if (!reg.valid(id)) return glm::vec2(0.0f);
    const auto* t = reg.try_get<TransformComponent>(id);
    if (!t) return glm::vec2(0.0f);
    const glm::vec3 v = velocity(c, e);
    const glm::vec2 planar(v.x, v.z);
    if (glm::length(planar) < 1e-4f) return glm::vec2(0.0f);
    const glm::quat q = glm::quat(glm::radians(t->rotation));
    const glm::vec3 fwd = q * glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 rgt = q * glm::vec3(1.0f, 0.0f,  0.0f);
    return { glm::dot(planar, glm::vec2(fwd.x, fwd.z)),
             glm::dot(planar, glm::vec2(rgt.x, rgt.z)) };
}
}
float forwardAmount(Ctx& c, Entity e) { return localTravel(c, e).x; }
float rightAmount(Ctx& c, Entity e)   { return localTravel(c, e).y; }
} // namespace movement

namespace locomotion {
void move(Ctx& c, Entity e, const glm::vec3& direction)
{
    // Accumulated, not assigned: two calls in one frame are two pushes, which is
    // what a caller adding "forward" and "strafe" separately means.
    if (auto* mv = mvOf(c, e)) mv->moveInput += direction;
}
void look(Ctx& c, Entity e, float yawDegrees, float pitchDegrees)
{
    if (auto* mv = mvOf(c, e)) { mv->lookYaw += yawDegrees; mv->lookPitch += pitchDegrees; }
}
void setMaxSpeed(Ctx& c, Entity e, float v)          { if (auto* mv = mvOf(c, e)) mv->maxSpeed = v; }
void setOrientToMovement(Ctx& c, Entity e, bool on)  { if (auto* mv = mvOf(c, e)) mv->orientToMovement = on; }
} // namespace locomotion

// ── Entity UI ────────────────────────────────────────────────────────────────
namespace ui {
std::string getText(Ctx& c, Entity e)                        { return c.world ? ScriptApi::getUIText(*c.world, e) : std::string(); }
void        setText(Ctx& c, Entity e, const std::string& t)  { if (c.world) ScriptApi::setUIText(*c.world, e, t); }
glm::vec4   getColor(Ctx& c, Entity e)                       { return c.world ? ScriptApi::getUIColor(*c.world, e) : glm::vec4(1.0f); }
void        setColor(Ctx& c, Entity e, const glm::vec4& col) { if (c.world) ScriptApi::setUIColor(*c.world, e, col); }
bool        getVisible(Ctx& c, Entity e)                     { return c.world ? ScriptApi::isUIVisible(*c.world, e) : false; }
void        setVisible(Ctx& c, Entity e, bool v)             { if (c.world) ScriptApi::setUIVisible(*c.world, e, v); }
glm::vec2   getPosition(Ctx& c, Entity e)                    { return c.world ? ScriptApi::getUIPosition(*c.world, e) : glm::vec2(0.0f); }
void        setPosition(Ctx& c, Entity e, const glm::vec2& p){ if (c.world) ScriptApi::setUIPosition(*c.world, e, p); }
glm::vec2   getSize(Ctx& c, Entity e)                        { return c.world ? ScriptApi::getUISize(*c.world, e) : glm::vec2(0.0f); }
void        setSize(Ctx& c, Entity e, const glm::vec2& s)    { if (c.world) ScriptApi::setUISize(*c.world, e, s); }
bool        setMaterialParam(Ctx& c, Entity e, const std::string& n, const glm::vec4& v) { return c.world ? ScriptApi::setUIMaterialParam(*c.world, c.content, e, n, v) : false; }
bool        pointerOverUI(Ctx& c)                            { return c.world ? ScriptApi::pointerOverUI(*c.world) : false; }
} // namespace ui

// ── Live widgets ─────────────────────────────────────────────────────────────
namespace widget {
int  create(Ctx& c, const std::string& p)          { return c.world ? ScriptApi::createWidget(*c.world, c.content, p) : 0; }
void destroy(Ctx& c, int id)                        { if (c.world) ScriptApi::destroyWidget(*c.world, id); }
void show(Ctx& c, int id)                           { if (c.world) ScriptApi::showWidget(*c.world, id); }
void hide(Ctx& c, int id)                           { if (c.world) ScriptApi::hideWidget(*c.world, id); }
void setZOrder(Ctx& c, int id, int z)               { if (c.world) ScriptApi::setWidgetZOrder(*c.world, id, z); }
bool isVisible(Ctx& c, int id)                      { return c.world ? ScriptApi::isWidgetVisible(*c.world, id) : false; }
bool callFunction(Ctx& c, int id, const std::string& fn) { return c.world ? ScriptApi::callWidgetFunction(*c.world, id, fn) : false; }
int  addChild(Ctx& c, int id, const std::string& parent, const std::string& asset)
{ return c.world ? ScriptApi::addWidgetChild(*c.world, c.content, id, parent, asset) : 0; }
bool removeChild(Ctx& c, int id, int childId)
{ return c.world ? ScriptApi::removeWidgetChild(*c.world, id, childId) : false; }
int  clearChildren(Ctx& c, int id, const std::string& parent)
{ return c.world ? ScriptApi::clearWidgetChildren(*c.world, id, parent) : 0; }
} // namespace widget

// ── Theme ────────────────────────────────────────────────────────────────────
namespace theme {
bool set(Ctx& c, const std::string& p)
{ return c.world ? ScriptApi::setTheme(*c.world, c.content, p) : false; }
void setMode(Ctx& c, const std::string& m)
{ if (c.world) ScriptApi::setThemeMode(*c.world, m); }
std::string mode(Ctx& c)
{ return c.world ? ScriptApi::themeMode(*c.world) : std::string("Dark"); }
std::string preference(Ctx& c)
{ return c.world ? ScriptApi::themePreference(*c.world) : std::string("System"); }
} // namespace theme

// ── Cursor ───────────────────────────────────────────────────────────────────
namespace cursor {
void setVisible(Ctx&, bool show) { ScriptApi::setCursorVisible(show); }
} // namespace cursor

// ── Application ──────────────────────────────────────────────────────────────
namespace app {
void quit(Ctx& c)
{
    // Loud, because "my Quit button does nothing" is otherwise unexplainable
    // from the graph: the row is bound, the host just never said what quitting
    // means here (an editor tool window, a test rig).
    if (!c.requestQuit)
    {
        HE_LOG_WARN(Script, "%s", "app.quit: no quit handler bound by the host — ignored");
        return;
    }
    c.requestQuit();
}

void setTitle(Ctx& c, const std::string& title)
{
    if (!c.setWindowTitle)
    {
        HE_LOG_WARN(Script, "%s", "app.setTitle: no window bound by the host — ignored");
        return;
    }
    c.setWindowTitle(title);
}

void setSize(Ctx& c, int width, int height)
{
    if (!c.setWindowSize)
    {
        HE_LOG_WARN(Script, "%s", "app.setSize: no window bound by the host — ignored");
        return;
    }
    // A window of zero or negative size is not a window. Rejected here rather
    // than passed on, because SDL's behaviour for it differs per platform and
    // "my app vanished" is a miserable thing to debug.
    if (width <= 0 || height <= 0)
    {
        HE_LOG_WARN(Script, "app.setSize(%d, %d): both sides must be positive — ignored",
                    width, height);
        return;
    }
    c.setWindowSize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
}

glm::vec2 size(Ctx& c)
{
    // Silent zero rather than a warning: this is a getter, and a graph polling
    // the window size every tick would drown the log.
    return c.windowSize ? c.windowSize() : glm::vec2(0.0f);
}

void requestRedraw(Ctx& c)
{
    // Deliberately silent when unbound: a game host binds nothing here because a
    // game already draws every frame, and asking for a redraw there is a
    // harmless no-op rather than a mistake worth reporting.
    if (c.requestRedraw) c.requestRedraw();
}
} // namespace app

// ── Clipboard ────────────────────────────────────────────────────────────────
namespace clipboard {
std::string getText(Ctx&)
{
    // SDL hands back an owned copy even when the clipboard is empty (an empty
    // string), and null only on failure. Freed either way.
    char* raw = SDL_GetClipboardText();
    if (!raw) return {};
    std::string out(raw);
    SDL_free(raw);
    return out;
}

void setText(Ctx&, const std::string& text)
{
    if (!SDL_SetClipboardText(text.c_str()))
        HE_LOG_WARN(Script, "clipboard.setText failed: %s", SDL_GetError());
}

bool hasText(Ctx&) { return SDL_HasClipboardText(); }
} // namespace clipboard

// ── Native dialogs ───────────────────────────────────────────────────────────
namespace dialog {
void message(Ctx&, const std::string& title, const std::string& text, int kind)
{
    const SDL_MessageBoxFlags flag = kind == 2 ? SDL_MESSAGEBOX_ERROR
                                   : kind == 1 ? SDL_MESSAGEBOX_WARNING
                                               : SDL_MESSAGEBOX_INFORMATION;
    // Null parent window: HE_Scene has no window handle, and a modeless box is
    // better than none. The call blocks until the user dismisses it, which is
    // the whole point of reaching for a native dialog.
    if (!SDL_ShowSimpleMessageBox(flag, title.c_str(), text.c_str(), nullptr))
        HE_LOG_WARN(Script, "dialog.message failed: %s", SDL_GetError());
}

bool confirm(Ctx&, const std::string& title, const std::string& text,
             const std::string& affirmative, const std::string& negative)
{
    // Button 0 is the affirmative one and carries BOTH default flags: Return
    // takes it, Escape takes the other. A dialog where Escape does nothing traps
    // a keyboard user.
    SDL_MessageBoxButtonData buttons[2] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1,
          affirmative.empty() ? "OK" : affirmative.c_str() },
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0,
          negative.empty() ? "Cancel" : negative.c_str() },
    };
    SDL_MessageBoxData data{};
    data.flags      = SDL_MESSAGEBOX_INFORMATION;
    data.title      = title.c_str();
    data.message    = text.c_str();
    data.numbuttons = 2;
    data.buttons    = buttons;

    int chosen = 0;
    if (!SDL_ShowMessageBox(&data, &chosen))
    {
        // Answering "no" on failure is the safe direction: the caller asked
        // whether to go ahead with something, and an unanswered question is not
        // a yes.
        HE_LOG_WARN(Script, "dialog.confirm failed: %s — answering no", SDL_GetError());
        return false;
    }
    return chosen == 1;
}
} // namespace dialog

// ── Camera ───────────────────────────────────────────────────────────────────
namespace {
// The world's main camera: isMain wins, else the first CameraComponent.
entt::entity mainCameraEntity(HorizonWorld* w)
{
    if (!w) return entt::null;
    entt::entity first = entt::null;
    auto view = w->registry().view<CameraComponent>();
    for (auto [e, cc] : view.each())
    {
        if (cc.isMain) return e;
        if (first == entt::null) first = e;
    }
    return first;
}
} // namespace
namespace camera {
glm::vec3 getPosition(Ctx& c)
{
    const entt::entity e = mainCameraEntity(c.world);
    if (e == entt::null) return glm::vec3(0.0f);
    const auto* t = c.world->registry().try_get<TransformComponent>(e);
    return t ? t->position : glm::vec3(0.0f);
}
void setPosition(Ctx& c, const glm::vec3& p)
{
    const entt::entity e = mainCameraEntity(c.world);
    if (e == entt::null) return;
    c.world->registry().get_or_emplace<TransformComponent>(e).position = p;
}
glm::vec3 getRotation(Ctx& c)
{
    const entt::entity e = mainCameraEntity(c.world);
    if (e == entt::null) return glm::vec3(0.0f);
    const auto* t = c.world->registry().try_get<TransformComponent>(e);
    return t ? t->rotation : glm::vec3(0.0f);
}
void setRotation(Ctx& c, const glm::vec3& r)
{
    const entt::entity e = mainCameraEntity(c.world);
    if (e == entt::null) return;
    c.world->registry().get_or_emplace<TransformComponent>(e).rotation = r;
}
float getFov(Ctx& c)
{
    const entt::entity e = mainCameraEntity(c.world);
    if (e == entt::null) return 0.0f;
    return c.world->registry().get<CameraComponent>(e).fovDegrees;
}
void setFov(Ctx& c, float degrees)
{
    const entt::entity e = mainCameraEntity(c.world);
    if (e == entt::null) return;
    c.world->registry().get<CameraComponent>(e).fovDegrees = degrees;
}

// ── Camera rig ───────────────────────────────────────────────────────────────
namespace {
CameraRigComponent* rigOf(Ctx& c)
{
    if (!c.world) return nullptr;
    const entt::entity e = mainCameraEntity(c.world);
    if (e == entt::null) return nullptr;
    return c.world->registry().try_get<CameraRigComponent>(e);
}
} // namespace

void setRigMode(Ctx& c, int mode)
{
    if (auto* r = rigOf(c))
        r->mode = (mode == 0) ? CameraRigComponent::Mode::FirstPerson
                              : CameraRigComponent::Mode::ThirdPerson;
}
int getRigMode(Ctx& c)
{
    auto* r = rigOf(c);
    return r ? static_cast<int>(r->mode) : 0;
}
void setRigTarget(Ctx& c, int entityId)
{
    auto* r = rigOf(c);
    if (!r) return;
    // An empty id is the documented "follow the possessed player", so an entity
    // that does not resolve lands on that rather than on nothing at all.
    const auto e = static_cast<entt::entity>(static_cast<uint32_t>(entityId));
    r->target = (entityId > 0 && c.world->registry().valid(e)) ? c.world->entityId(e)
                                                               : HE::UUID{};
}
void setArmLength(Ctx& c, float length)
{
    if (auto* r = rigOf(c)) r->armLength = length;
}
float getArmLength(Ctx& c)
{
    auto* r = rigOf(c);
    return r ? r->armLength : 0.0f;
}
void setTargetYawMode(Ctx& c, int mode)
{
    if (auto* r = rigOf(c))
        r->targetYaw = (mode == 0) ? CameraRigComponent::TargetYaw::Free
                                   : CameraRigComponent::TargetYaw::Follow;
}
int getTargetYawMode(Ctx& c)
{
    auto* r = rigOf(c);
    return r ? static_cast<int>(r->targetYaw) : 0;
}
float getRigYaw(Ctx& c)
{
    auto* r = rigOf(c);
    return r ? r->yaw : 0.0f;
}
float getRigPitch(Ctx& c)
{
    auto* r = rigOf(c);
    return r ? r->pitch : 0.0f;
}
void addYawPitch(Ctx& c, float dYaw, float dPitch)
{
    auto* r = rigOf(c);
    if (!r) return;
    r->yaw   += dYaw;
    r->pitch  = std::clamp(r->pitch + dPitch, r->pitchMin, r->pitchMax);
}
} // namespace camera

// ── Environment ──────────────────────────────────────────────────────────────
namespace {
EnvironmentComponent* envOf(HorizonWorld* w)
{
    if (!w) return nullptr;
    auto view = w->registry().view<EnvironmentComponent>();
    for (auto e : view) return &view.get<EnvironmentComponent>(e);
    return nullptr;
}
} // namespace
// Implementations generated from the HE_ENV_FIELDS_* X-lists (EngineApi.h) —
// one get/set pair per component field, all with the null-tolerant contract.
namespace env {
#define HE_ENV_IMPL_FLOAT(m, Name, disp) \
	float get##Name(Ctx& c)          { const auto* e = envOf(c.world); return e ? e->m : 0.0f; } \
	void  set##Name(Ctx& c, float v) { if (auto* e = envOf(c.world)) e->m = v; }
#define HE_ENV_IMPL_BOOL(m, Name, disp) \
	bool  get##Name(Ctx& c)          { const auto* e = envOf(c.world); return e && e->m; } \
	void  set##Name(Ctx& c, bool v)  { if (auto* e = envOf(c.world)) e->m = v; }
#define HE_ENV_IMPL_INT(m, Name, disp) \
	int   get##Name(Ctx& c)          { const auto* e = envOf(c.world); return e ? e->m : 0; } \
	void  set##Name(Ctx& c, int v)   { if (auto* e = envOf(c.world)) e->m = v; }
#define HE_ENV_IMPL_COLOR(m, Name, disp) \
	glm::vec3 get##Name(Ctx& c)                     { const auto* e = envOf(c.world); return e ? e->m : glm::vec3(0.0f); } \
	void      set##Name(Ctx& c, const glm::vec3& v) { if (auto* e = envOf(c.world)) e->m = v; }
HE_ENV_FIELDS_FLOAT(HE_ENV_IMPL_FLOAT)
HE_ENV_FIELDS_BOOL(HE_ENV_IMPL_BOOL)
HE_ENV_FIELDS_INT(HE_ENV_IMPL_INT)
HE_ENV_FIELDS_COLOR(HE_ENV_IMPL_COLOR)
#undef HE_ENV_IMPL_FLOAT
#undef HE_ENV_IMPL_BOOL
#undef HE_ENV_IMPL_INT
#undef HE_ENV_IMPL_COLOR
} // namespace env

// ── Audio ────────────────────────────────────────────────────────────────────
namespace audio {
namespace {
const AudioAsset* audioAsset(Ctx& c, const std::string& path)
{
    if (!c.content || path.empty()) return nullptr;
    const HE::UUID id = c.content->loadAsset(path);
    const AudioAsset* a = c.content->getAudio(id);
    return (a && !a->audioData.empty()) ? a : nullptr;
}
} // namespace
int play(Ctx& c, const std::string& path, float volume, float pitch, bool loop)
{
    const AudioAsset* a = audioAsset(c, path);
    if (!c.audio || !c.audio->isInitialized() || !a) return 0;
    return (int)c.audio->play(a->audioData, a->sampleRate, a->channels, volume, pitch, loop);
}
int playAt(Ctx& c, const std::string& path, const glm::vec3& pos,
           float volume, float pitch, bool loop, float minDist, float maxDist)
{
    const AudioAsset* a = audioAsset(c, path);
    if (!c.audio || !c.audio->isInitialized() || !a) return 0;
    return (int)c.audio->playSpatial(a->audioData, a->sampleRate, a->channels,
                                     volume, pitch, loop, pos.x, pos.y, pos.z, minDist, maxDist);
}
void stop(Ctx& c, int handle)      { if (c.audio) c.audio->stop((uint64_t)(uint32_t)handle); }
void stopAll(Ctx& c)               { if (c.audio) c.audio->stopAll(); }
bool isPlaying(Ctx& c, int handle) { return c.audio && c.audio->isPlaying((uint64_t)(uint32_t)handle); }
void setBusVolume(Ctx& c, const std::string& bus, float volume)
{
    if (!c.audio || !c.audio->isInitialized()) return;
    if (!c.audio->hasBus(bus)) c.audio->createBus(bus, volume);
    c.audio->setBusVolume(bus, volume);
}
void setSoundPosition(Ctx& c, int handle, const glm::vec3& pos)
{
    if (c.audio) c.audio->setSoundPosition((uint64_t)(uint32_t)handle, pos.x, pos.y, pos.z);
}
} // namespace audio

// ── Debug draw ───────────────────────────────────────────────────────────────
namespace debug {
namespace {
struct Timed { DebugLine seg; float ttl; };
std::vector<Timed>& queue() { static std::vector<Timed> q; return q; }
void push(const glm::vec3& a, const glm::vec3& b, const glm::vec3& col, float ttl)
{ queue().push_back({ { a, b, col }, ttl }); }
} // namespace
void line(const glm::vec3& a, const glm::vec3& b, const glm::vec3& color, float seconds)
{ push(a, b, color, seconds); }
void sphere(const glm::vec3& c0, float r, const glm::vec3& color, float seconds)
{
    // Expand to segments at submit time (three great circles), sharing one ttl.
    DebugDrawBuffer buf;
    buf.sphere(c0, r, color);
    for (const DebugLine& l : buf.lines()) queue().push_back({ l, seconds });
}
void box(const glm::vec3& mn, const glm::vec3& mx, const glm::vec3& color, float seconds)
{
    DebugDrawBuffer buf;
    buf.aabb(mn, mx, color);
    for (const DebugLine& l : buf.lines()) queue().push_back({ l, seconds });
}
void clear() { queue().clear(); }
void collect(float dt, std::vector<DebugLine>& out)
{
    auto& q = queue();
    for (const Timed& t : q) out.push_back(t.seg);     // draw everything alive NOW
    for (Timed& t : q) t.ttl -= dt;                     // then age
    q.erase(std::remove_if(q.begin(), q.end(),
        [](const Timed& t){ return t.ttl < 0.0f; }), q.end());
}
} // namespace debug

// ── Sandboxed fs + save store ────────────────────────────────────────────────
namespace fs {
namespace {
std::string& root() { static std::string r; return r; }
// A sandbox-relative path is valid when it has no root and no ".." component.
bool validRel(const std::string& rel)
{
    if (rel.empty()) return false;
    const std::filesystem::path p(rel);
    // has_root_directory too: on Windows "/tmp/x" is NOT is_absolute() (no
    // drive) yet root()/"/tmp/x" replaces everything after the drive letter —
    // a sandbox escape to C:\tmp. Rooted-anything is rejected.
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return false;
    for (const auto& part : p)
        if (part == "..") return false;
    return true;
}
std::filesystem::path resolved(const std::string& rel)
{
    if (root().empty() || !validRel(rel)) return {};
    return std::filesystem::path(root()) / rel;
}
} // namespace
void setSandboxRoot(const std::string& absDir) { root() = absDir; }
std::string sandboxRoot() { return root(); }
bool writeText(const std::string& rel, const std::string& text)
{
    const auto p = resolved(rel);
    if (p.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(text.data(), (std::streamsize)text.size());
    return f.good();
}
std::string readText(const std::string& rel)
{
    const auto p = resolved(rel);
    if (p.empty()) return {};
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string out((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return out;
}
bool exists(const std::string& rel)
{
    const auto p = resolved(rel);
    std::error_code ec;
    return !p.empty() && std::filesystem::exists(p, ec);
}
bool remove(const std::string& rel)
{
    const auto p = resolved(rel);
    std::error_code ec;
    return !p.empty() && std::filesystem::is_regular_file(p, ec)
        && std::filesystem::remove(p, ec);
}
bool makeDir(const std::string& rel)
{
    const auto p = resolved(rel);
    std::error_code ec;
    return !p.empty() && (std::filesystem::create_directories(p, ec) ||
                          std::filesystem::is_directory(p, ec));
}
} // namespace fs

// ── JSON ─────────────────────────────────────────────────────────────────────
namespace json {
namespace {
// Walk a dotted path with optional [i] indices. Returns nullptr when any step is
// missing or the wrong kind. An empty path is the document itself, which is what
// makes count(text, "") work on a top-level array.
const nlohmann::json* walk(const nlohmann::json& doc, const std::string& path)
{
    const nlohmann::json* cur = &doc;
    size_t i = 0;
    while (i < path.size() && cur)
    {
        if (path[i] == '.') { ++i; continue; }
        if (path[i] == '[')
        {
            const size_t close = path.find(']', i);
            if (close == std::string::npos || !cur->is_array()) return nullptr;
            const std::string num = path.substr(i + 1, close - i - 1);
            // Reject anything that is not a plain non-negative integer: "[x]"
            // silently becoming index 0 would be a bug that reads like data.
            if (num.empty() || num.find_first_not_of("0123456789") != std::string::npos)
                return nullptr;
            const size_t idx = static_cast<size_t>(std::stoull(num));
            if (idx >= cur->size()) return nullptr;
            cur = &(*cur)[idx];
            i = close + 1;
            continue;
        }
        const size_t next = path.find_first_of(".[", i);
        const std::string key = path.substr(i, next == std::string::npos ? std::string::npos : next - i);
        if (!cur->is_object()) return nullptr;
        const auto it = cur->find(key);
        if (it == cur->end()) return nullptr;
        cur = &(*it);
        i = (next == std::string::npos) ? path.size() : next;
    }
    return cur;
}

// The setter twin: creates missing OBJECT steps on the way down (an array index
// is never created — growing an array by writing past its end is a different
// operation and one nobody asked for). Null when the path runs through a
// non-object.
nlohmann::json* walkCreate(nlohmann::json& doc, const std::string& path)
{
    nlohmann::json* cur = &doc;
    size_t i = 0;
    while (i < path.size() && cur)
    {
        if (path[i] == '.') { ++i; continue; }
        if (path[i] == '[')
        {
            const size_t close = path.find(']', i);
            if (close == std::string::npos || !cur->is_array()) return nullptr;
            const std::string num = path.substr(i + 1, close - i - 1);
            if (num.empty() || num.find_first_not_of("0123456789") != std::string::npos)
                return nullptr;
            const size_t idx = static_cast<size_t>(std::stoull(num));
            if (idx >= cur->size()) return nullptr;
            cur = &(*cur)[idx];
            i = close + 1;
            continue;
        }
        const size_t next = path.find_first_of(".[", i);
        const std::string key = path.substr(i, next == std::string::npos ? std::string::npos : next - i);
        if (cur->is_null()) *cur = nlohmann::json::object();
        if (!cur->is_object()) return nullptr;
        cur = &(*cur)[key];
        i = (next == std::string::npos) ? path.size() : next;
    }
    return cur;
}

nlohmann::json parse(const std::string& text)
{
    return nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
}

// Shared tail of the three setters: parse, walk, assign, re-serialise. Returns
// the input untouched on every failure, which is the documented contract.
template <typename T>
std::string setAt(const std::string& text, const std::string& path, T&& value)
{
    nlohmann::json doc = parse(text);
    if (doc.is_discarded())
    {
        // An empty (or blank) document is not a parse failure worth refusing —
        // building one up from nothing is the normal way to write JSON here.
        if (text.find_first_not_of(" \t\r\n") != std::string::npos) return text;
        doc = nlohmann::json::object();
    }
    nlohmann::json* slot = walkCreate(doc, path);
    if (!slot) return text;
    *slot = std::forward<T>(value);
    return doc.dump();
}
} // namespace

std::string getString(Ctx&, const std::string& text, const std::string& path,
                      const std::string& fallback)
{
    const nlohmann::json doc = parse(text);
    if (doc.is_discarded()) return fallback;
    const nlohmann::json* v = walk(doc, path);
    return (v && v->is_string()) ? v->get<std::string>() : fallback;
}

double getNumber(Ctx&, const std::string& text, const std::string& path, double fallback)
{
    const nlohmann::json doc = parse(text);
    if (doc.is_discarded()) return fallback;
    const nlohmann::json* v = walk(doc, path);
    return (v && v->is_number()) ? v->get<double>() : fallback;
}

bool getBool(Ctx&, const std::string& text, const std::string& path, bool fallback)
{
    const nlohmann::json doc = parse(text);
    if (doc.is_discarded()) return fallback;
    const nlohmann::json* v = walk(doc, path);
    return (v && v->is_boolean()) ? v->get<bool>() : fallback;
}

bool has(Ctx&, const std::string& text, const std::string& path)
{
    const nlohmann::json doc = parse(text);
    if (doc.is_discarded()) return false;
    return walk(doc, path) != nullptr;
}

int count(Ctx&, const std::string& text, const std::string& path)
{
    const nlohmann::json doc = parse(text);
    if (doc.is_discarded()) return 0;
    const nlohmann::json* v = walk(doc, path);
    return (v && v->is_array()) ? static_cast<int>(v->size()) : 0;
}

std::string setString(Ctx&, const std::string& text, const std::string& path,
                      const std::string& value) { return setAt(text, path, value); }
std::string setNumber(Ctx&, const std::string& text, const std::string& path, double value)
{ return setAt(text, path, value); }
std::string setBool(Ctx&, const std::string& text, const std::string& path, bool value)
{ return setAt(text, path, value); }
} // namespace json

// ── Preferences ──────────────────────────────────────────────────────────────
namespace prefs {
namespace {
constexpr const char* kFile = "Prefs.json";

nlohmann::json& doc()
{
    static nlohmann::json d = nlohmann::json::object();
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;   // set FIRST: a failed read must not retry on every call
        const std::string text = fs::readText(kFile);
        if (!text.empty())
        {
            nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
            if (parsed.is_object()) d = std::move(parsed);
            else
                HE_LOG_WARN(Script, "%s", "prefs: Prefs.json is not a JSON object — starting empty");
        }
    }
    return d;
}

void store()
{
    // Every set writes. These are a handful of keys, and the alternative is
    // losing the user's settings to a crash for the sake of an fwrite.
    if (!fs::writeText(kFile, doc().dump(2)))
        HE_LOG_WARN(Script, "%s", "prefs: could not write Prefs.json — settings will not persist");
}
} // namespace

std::string getString(Ctx&, const std::string& key, const std::string& fallback)
{
    const auto it = doc().find(key);
    return (it != doc().end() && it->is_string()) ? it->get<std::string>() : fallback;
}
double getNumber(Ctx&, const std::string& key, double fallback)
{
    const auto it = doc().find(key);
    return (it != doc().end() && it->is_number()) ? it->get<double>() : fallback;
}
bool getBool(Ctx&, const std::string& key, bool fallback)
{
    const auto it = doc().find(key);
    return (it != doc().end() && it->is_boolean()) ? it->get<bool>() : fallback;
}
void setString(Ctx&, const std::string& key, const std::string& value)
{ doc()[key] = value; store(); }
void setNumber(Ctx&, const std::string& key, double value) { doc()[key] = value; store(); }
void setBool  (Ctx&, const std::string& key, bool value)   { doc()[key] = value; store(); }
bool has      (Ctx&, const std::string& key) { return doc().contains(key); }
bool remove   (Ctx&, const std::string& key)
{
    if (!doc().contains(key)) return false;
    doc().erase(key);
    store();
    return true;
}
void clear(Ctx&) { doc() = nlohmann::json::object(); store(); }
} // namespace prefs

// ── Date and time ────────────────────────────────────────────────────────────
namespace datetime {
namespace {
// localtime_r/localtime_s rather than localtime: this is called from script
// threads and the shared static buffer is exactly the kind of race that shows up
// once a month as a wrong timestamp.
std::tm localParts(double epochSeconds)
{
    const std::time_t t = static_cast<std::time_t>(epochSeconds);
    std::tm out{};
#ifdef _WIN32
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}
} // namespace

double now(Ctx&)
{
    return static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string format(Ctx&, double epochSeconds, const std::string& fmt)
{
    if (fmt.empty()) return {};
    const std::tm parts = localParts(epochSeconds);
    // 512 is generous for a timestamp; a format that still overruns it gets an
    // empty string rather than a truncated half-date pretending to be one.
    char buf[512];
    const size_t n = std::strftime(buf, sizeof(buf), fmt.c_str(), &parts);
    return n > 0 ? std::string(buf, n) : std::string();
}

int year   (Ctx&, double s) { return localParts(s).tm_year + 1900; }
int month  (Ctx&, double s) { return localParts(s).tm_mon + 1; }
int day    (Ctx&, double s) { return localParts(s).tm_mday; }
int hour   (Ctx&, double s) { return localParts(s).tm_hour; }
int minute (Ctx&, double s) { return localParts(s).tm_min; }
int second (Ctx&, double s) { return localParts(s).tm_sec; }
int weekday(Ctx&, double s) { return localParts(s).tm_wday; }
} // namespace datetime

namespace save {
namespace {

using P = PinType;

// ── The one active save document ─────────────────────────────────────────────
struct ActiveSave
{
    std::string        id;
    std::string        templatePath;    // content-relative SaveGameTemplate asset
    HE::StructDef      schema;          // resolved template fields
    std::vector<Value> fields;          // values, schema order
    nlohmann::json     entities = nlohmann::json::object(); // uuid → state blob
};
std::unique_ptr<ActiveSave>& active() { static std::unique_ptr<ActiveSave> a; return a; }
std::string& defaultTpl() { static std::string t; return t; }
bool& playMode() { static bool p = false; return p; }

bool validId(const std::string& id)
{
    if (id.empty()) return false;
    for (char c : id)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
            return false;
    return true;
}
std::string savePath(const std::string& id) { return "Saves/" + id + ".json"; }

// Resolve a template asset into a field schema. Loud on every miss.
bool resolveTemplate(::ContentManager* cm, const std::string& path, HE::StructDef& out)
{
    if (!cm)
    { HE_LOG_WARN(Script, "%s", "save: no content manager — cannot resolve the template"); return false; }
    if (path.empty())
    { HE_LOG_WARN(Script, "%s", "save: no SaveGameTemplate configured (project default is empty)"); return false; }
    const HE::UUID id = cm->loadAsset(path);
    const SaveGameTemplateAsset* a = cm->getSaveGameTemplate(id);
    if (!a)
    {
        HE_LOG_WARN(Script, "%s",
            ("save: SaveGameTemplate '" + path + "' not found").c_str());
        return false;
    }
    if (!HE::TypeRegistry::structFromJson(a->json, out))
    {
        HE_LOG_WARN(Script, "%s",
            ("save: SaveGameTemplate '" + path + "' has invalid JSON").c_str());
        return false;
    }
    out.name = a->name;
    out.assetPath = path;
    return true;
}

// Seed a fresh field vector from the schema defaults. A SaveGameTemplate IS a
// StructDef, so this is field-for-field the same seeding the type registry does
// for a struct — and it USED to spell the rule out a second time here, which
// meant every new field shape (containers, most recently) had to be remembered
// in two places. It asks the registry now; the two cannot disagree.
std::vector<Value> seedFields(const HE::StructDef& schema)
{
    auto& reg = HE::TypeRegistry::instance();
    std::vector<Value> out;
    out.reserve(schema.fields.size());
    for (const HE::StructField& f : schema.fields) out.push_back(reg.makeFieldDefault(f));
    return out;
}

// ── Field values ⇄ JSON (name-keyed, mirrors the script-boundary shapes) ─────
nlohmann::json valueToJson(const Value& v);
Value valueFromJson(const nlohmann::json& j, const HE::StructField& f);

nlohmann::json scalarToJson(const Value& v)
{
    switch (v.type)
    {
    case P::Float:  return v.f;
    case P::Int:    return v.i;
    case P::Enum:   return v.i;
    case P::Bool:   return v.b;
    case P::String: return v.s;
    case P::Vec2:   return nlohmann::json::array({ v.v2.x, v.v2.y });
    case P::Color:  return nlohmann::json::array({ v.col.x, v.col.y, v.col.z, v.col.w });
    case P::Transform:
        return nlohmann::json{
            { "pos", { v.tpos.x, v.tpos.y, v.tpos.z } },
            { "rot", { v.trot.x, v.trot.y, v.trot.z } },
            { "scl", { v.tscl.x, v.tscl.y, v.tscl.z } } };
    case P::Struct:
    {
        HE::StructDef def;
        nlohmann::json o = nlohmann::json::object();
        o["__type"] = v.typeName;
        if (HE::TypeRegistry::instance().getStruct(v.typeName, def))
            for (size_t i = 0; i < def.fields.size() && i < v.items.size(); ++i)
                o[def.fields[i].name] = valueToJson(v.items[i]);
        return o;
    }
    default: return nullptr;
    }
}
nlohmann::json valueToJson(const Value& v)
{
    if (!v.isArray) return scalarToJson(v);
    if (v.kind() == HorizonCode::ContainerKind::Map)
    {
        // PARALLEL ARRAYS, never a JSON object keyed by the map's own keys: the
        // keys are not all strings (Int/Enum/Ref are legal), and nlohmann's
        // object is a sorted std::map — it would silently alphabetize them and
        // the save would come back in a different iteration order than it went
        // out, which is the one thing a map here promises not to do.
        nlohmann::json ks = nlohmann::json::array(), vs = nlohmann::json::array();
        const size_t n = std::min(v.keys.size(), v.items.size());
        for (size_t i = 0; i < n; ++i)
        { ks.push_back(scalarToJson(v.keys[i])); vs.push_back(scalarToJson(v.items[i])); }
        return nlohmann::json{ { "keys", std::move(ks) }, { "values", std::move(vs) } };
    }
    // Array and Set share the encoding: an ordered list. A set is re-deduped on
    // the way back in, so a hand-edited save cannot smuggle a duplicate in.
    nlohmann::json a = nlohmann::json::array();
    for (const Value& it : v.items) a.push_back(scalarToJson(it));
    return a;
}

Value scalarFromJson(const nlohmann::json& j, HorizonCode::PinType t, const std::string& typeName)
{
    Value v; v.type = t; v.typeName = typeName;
    switch (t)
    {
    case P::Float:  if (j.is_number())  v.f = j.get<float>(); break;
    case P::Int:
    case P::Enum:   if (j.is_number())  v.i = j.get<int>();   break;
    case P::Bool:   if (j.is_boolean()) v.b = j.get<bool>();  break;
    case P::String: if (j.is_string())  v.s = j.get<std::string>(); break;
    case P::Vec2:
        if (j.is_array() && j.size() >= 2)
            v.v2 = { j[0].get<float>(), j[1].get<float>() };
        break;
    case P::Color:
        if (j.is_array() && j.size() >= 4)
            v.col = { j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>() };
        break;
    case P::Transform:
        if (j.is_object())
        {
            auto vec3 = [&](const char* key, glm::vec3& out, const glm::vec3& def) {
                out = def;
                auto it = j.find(key);
                if (it != j.end() && it->is_array() && it->size() >= 3)
                    out = { (*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>() };
            };
            vec3("pos", v.tpos, glm::vec3(0.0f));
            vec3("rot", v.trot, glm::vec3(0.0f));
            vec3("scl", v.tscl, glm::vec3(1.0f));
        }
        break;
    case P::Struct:
    {
        // Seed defaults, then overwrite the fields present — a schema edit
        // between write and load keeps missing fields at their defaults.
        v = HE::TypeRegistry::instance().makeDefaultValue(typeName);
        HE::StructDef def;
        if (j.is_object() && HE::TypeRegistry::instance().getStruct(typeName, def))
            for (size_t i = 0; i < def.fields.size(); ++i)
                if (auto it = j.find(def.fields[i].name); it != j.end() && i < v.items.size())
                    v.items[i] = valueFromJson(*it, def.fields[i]);
        break;
    }
    default: break;
    }
    return v;
}
Value valueFromJson(const nlohmann::json& j, const HE::StructField& f)
{
    using CK = HorizonCode::ContainerKind;
    if (!f.isArray) return scalarFromJson(j, f.type, f.typeName);
    Value v; v.isArray = true; v.container = f.kind();
    v.type = f.type; v.typeName = f.typeName;
    if (v.container == CK::Map)
    {
        v.keyType = f.keyType; v.keyTypeName = f.keyTypeName;
        const auto ks = j.is_object() ? j.value("keys",   nlohmann::json::array())
                                      : nlohmann::json::array();
        const auto vs = j.is_object() ? j.value("values", nlohmann::json::array())
                                      : nlohmann::json::array();
        const size_t n = std::min(ks.size(), vs.size());
        for (size_t i = 0; i < n; ++i)
        {
            Value key = scalarFromJson(ks[i], f.keyType, f.keyTypeName);
            // Last write wins on a repeated key, first position kept — the same
            // rule a sequence of Map Set nodes follows.
            bool dup = false;
            for (size_t k = 0; k < v.keys.size(); ++k)
                if (HorizonCode::scalarValueEquals(v.keys[k], key, f.keyType))
                { v.items[k] = scalarFromJson(vs[i], f.type, f.typeName); dup = true; break; }
            if (dup) continue;
            v.keys.push_back(std::move(key));
            v.items.push_back(scalarFromJson(vs[i], f.type, f.typeName));
        }
        return v;
    }
    if (j.is_array())
        for (const auto& e : j)
        {
            Value it = scalarFromJson(e, f.type, f.typeName);
            if (v.container == CK::Set)
            {
                bool dup = false;
                for (const Value& have : v.items)
                    if (HorizonCode::scalarValueEquals(have, it, f.type)) { dup = true; break; }
                if (dup) continue;   // a set keeps the FIRST occurrence
            }
            v.items.push_back(std::move(it));
        }
    return v;
}

// Field lookup with the loud-failure contract.
int fieldIndex(const char* op, const std::string& name)
{
    if (!active())
    {
        HE_LOG_WARN(Script, "%s",
            (std::string("save.") + op + ": no active save — call save.create/load first").c_str());
        return -1;
    }
    for (size_t i = 0; i < active()->schema.fields.size(); ++i)
        if (active()->schema.fields[i].name == name) return (int)i;
    HE_LOG_WARN(Script, "%s",
        (std::string("save.") + op + ": template '" + active()->templatePath +
         "' has no field '" + name + "'").c_str());
    return -1;
}
bool typeMismatch(const char* op, const HE::StructField& f, std::initializer_list<P> accepted)
{
    for (P t : accepted) if (f.type == t && !f.isArray) return false;
    HE_LOG_WARN(Script, "%s",
        (std::string("save.") + op + ": field '" + f.name + "' has a different type").c_str());
    return true;
}

} // namespace

void setDefaultTemplate(const std::string& p) { defaultTpl() = p; }
std::string defaultTemplate() { return defaultTpl(); }
void setPlayMode(bool p) { playMode() = p; }
bool inPlayMode() { return playMode(); }

bool create(const std::string& id, ::ContentManager* cm, const std::string& templatePath)
{
    if (!validId(id))
    { HE_LOG_WARN(Script, "%s", ("save.create: invalid id '" + id + "' (use A-Z a-z 0-9 _ -)").c_str()); return false; }
    const std::string tpl = templatePath.empty() ? defaultTpl() : templatePath;
    HE::StructDef schema;
    if (!resolveTemplate(cm, tpl, schema)) return false;
    auto doc = std::make_unique<ActiveSave>();
    doc->id = id;
    doc->templatePath = tpl;
    doc->fields = seedFields(schema);
    doc->schema = std::move(schema);
    active() = std::move(doc);
    return true;
}

bool load(const std::string& id, ::ContentManager* cm)
{
    if (!validId(id))
    { HE_LOG_WARN(Script, "%s", ("save.load: invalid id '" + id + "'").c_str()); return false; }
    const std::string text = fs::readText(savePath(id));
    if (text.empty())
    { HE_LOG_WARN(Script, "%s", ("save.load: no save '" + id + "'").c_str()); return false; }
    nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object())
    { HE_LOG_WARN(Script, "%s", ("save.load: save '" + id + "' is corrupt").c_str()); return false; }

    HE::StructDef schema;
    if (!resolveTemplate(cm, j.value("template", std::string{}), schema)) return false;

    auto doc = std::make_unique<ActiveSave>();
    doc->id = id;
    doc->templatePath = schema.assetPath;
    doc->fields = seedFields(schema);          // defaults first — partial files load clean
    if (auto f = j.find("fields"); f != j.end() && f->is_object())
        for (size_t i = 0; i < schema.fields.size(); ++i)
            if (auto it = f->find(schema.fields[i].name); it != f->end())
                doc->fields[i] = valueFromJson(*it, schema.fields[i]);
    if (auto e = j.find("entities"); e != j.end() && e->is_object())
        doc->entities = *e;
    doc->schema = std::move(schema);
    active() = std::move(doc);
    return true;
}

bool write()
{
    if (!active())
    { HE_LOG_WARN(Script, "%s", "save.write: no active save"); return false; }
    nlohmann::json j;
    j["template"] = active()->templatePath;
    nlohmann::json f = nlohmann::json::object();
    for (size_t i = 0; i < active()->schema.fields.size(); ++i)
        f[active()->schema.fields[i].name] = valueToJson(active()->fields[i]);
    j["fields"]   = std::move(f);
    j["entities"] = active()->entities;

    // Atomic: temp + rename inside the sandbox (fs::writeText would truncate the
    // only copy before the new bytes are durable).
    const std::string rel = savePath(active()->id);
    const std::string tmpRel = rel + ".tmp";
    if (!fs::writeText(tmpRel, j.dump(2)))
    { HE_LOG_WARN(Script, "%s", "save.write: sandbox write failed (no sandbox root?)"); return false; }
    std::error_code ec;
    const std::filesystem::path root(fs::sandboxRoot());
    std::filesystem::rename(root / tmpRel, root / rel, ec);
    if (ec)
    {
        std::filesystem::remove(root / tmpRel, ec);
        HE_LOG_WARN(Script, "%s", "save.write: atomic replace failed");
        return false;
    }
    return true;
}

void close() { active().reset(); }
std::string activeId() { return active() ? active()->id : std::string{}; }

std::vector<std::string> list()
{
    std::vector<std::string> out;
    const std::string root = fs::sandboxRoot();
    if (root.empty()) return out;
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::path(root) / "Saves";
    std::filesystem::directory_iterator it(dir, ec), end;
    for (; !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec) || it->path().extension() != ".json") continue;
        const std::string id = it->path().stem().string();
        if (validId(id)) out.push_back(id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool exists(const std::string& id) { return validId(id) && fs::exists(savePath(id)); }
bool remove(const std::string& id) { return validId(id) && fs::remove(savePath(id)); }

std::vector<std::string> fields()
{
    std::vector<std::string> out;
    if (!active()) return out;
    for (const auto& f : active()->schema.fields) out.push_back(f.name);
    return out;
}

bool setNumber(const std::string& field, float v)
{
    const int i = fieldIndex("setNumber", field);
    if (i < 0) return false;
    const HE::StructField& f = active()->schema.fields[i];
    if (typeMismatch("setNumber", f, { P::Float, P::Int, P::Enum })) return false;
    Value& dst = active()->fields[i];
    dst.type = f.type; dst.typeName = f.typeName;
    if (f.type == P::Float) dst.f = v;
    else                    dst.i = (int)v;
    return true;
}
float getNumber(const std::string& field, float def)
{
    const int i = fieldIndex("getNumber", field);
    if (i < 0) return def;
    const HE::StructField& f = active()->schema.fields[i];
    if (typeMismatch("getNumber", f, { P::Float, P::Int, P::Enum })) return def;
    const Value& v = active()->fields[i];
    return f.type == P::Float ? v.f : (float)v.i;
}
bool setString(const std::string& field, const std::string& v)
{
    const int i = fieldIndex("setString", field);
    if (i < 0) return false;
    if (typeMismatch("setString", active()->schema.fields[i], { P::String })) return false;
    active()->fields[i] = Value::ofString(v);
    return true;
}
std::string getString(const std::string& field, const std::string& def)
{
    const int i = fieldIndex("getString", field);
    if (i < 0) return def;
    if (typeMismatch("getString", active()->schema.fields[i], { P::String })) return def;
    return active()->fields[i].s;
}
bool setBool(const std::string& field, bool v)
{
    const int i = fieldIndex("setBool", field);
    if (i < 0) return false;
    if (typeMismatch("setBool", active()->schema.fields[i], { P::Bool })) return false;
    active()->fields[i] = Value::ofBool(v);
    return true;
}
bool getBool(const std::string& field, bool def)
{
    const int i = fieldIndex("getBool", field);
    if (i < 0) return def;
    if (typeMismatch("getBool", active()->schema.fields[i], { P::Bool })) return def;
    return active()->fields[i].b;
}
bool setStructV(const std::string& field, const Value& v)
{
    const int i = fieldIndex("setStruct", field);
    if (i < 0) return false;
    const HE::StructField& f = active()->schema.fields[i];
    if (typeMismatch("setStruct", f, { P::Struct })) return false;
    if (v.type != P::Struct || v.typeName != f.typeName)
    {
        HE_LOG_WARN(Script, "%s",
            ("save.setStruct: field '" + field + "' holds '" + f.typeName +
             "', got '" + v.typeName + "'").c_str());
        return false;
    }
    active()->fields[i] = v;
    return true;
}
Value getStructV(const std::string& field)
{
    const int i = fieldIndex("getStruct", field);
    if (i < 0) return {};
    const HE::StructField& f = active()->schema.fields[i];
    if (typeMismatch("getStruct", f, { P::Struct })) return {};
    return active()->fields[i];
}

bool setStructJson(const std::string& field, const std::string& json)
{
    const int i = fieldIndex("setStructJson", field);
    if (i < 0) return false;
    const HE::StructField& f = active()->schema.fields[i];
    if (typeMismatch("setStructJson", f, { P::Struct })) return false;
    nlohmann::json j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object())
    {
        HE_LOG_WARN(Script, "%s",
            ("save.setStructJson: field '" + field + "' — invalid JSON").c_str());
        return false;
    }
    active()->fields[i] = valueFromJson(j, f);
    return true;
}
std::string getStructJson(const std::string& field)
{
    const int i = fieldIndex("getStructJson", field);
    if (i < 0) return {};
    const HE::StructField& f = active()->schema.fields[i];
    if (typeMismatch("getStructJson", f, { P::Struct })) return {};
    return valueToJson(active()->fields[i]).dump();
}

bool setEntityState(const std::string& uuid, const std::string& json)
{
    if (!active())
    { HE_LOG_WARN(Script, "%s", "save: entity state needs an active save"); return false; }
    nlohmann::json j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return false;
    active()->entities[uuid] = std::move(j);
    return true;
}
std::string entityState(const std::string& uuid)
{
    if (!active()) return {};
    auto it = active()->entities.find(uuid);
    return it != active()->entities.end() ? it->dump() : std::string{};
}
bool hasEntityState(const std::string& uuid)
{
    return active() && active()->entities.contains(uuid);
}

} // namespace save

// ── Scene transitions ────────────────────────────────────────────────────────
namespace scene {
namespace {
std::vector<Request>& requests() { static std::vector<Request> q; return q; }
int& nextZone() { static int z = 1; return z; }
std::unordered_map<int, ZoneInfo>& zones() { static std::unordered_map<int, ZoneInfo> z; return z; }
bool& pendingLevel() { static bool p = false; return p; }
} // namespace
void load(const std::string& scenePath, bool hidden)
{ Request r; r.kind = (int)RequestKind::Switch; r.path = scenePath; r.hidden = hidden; requests().push_back(std::move(r)); }
int loadAdditive(const std::string& scenePath, bool hidden, const glm::vec3& position)
{
    Request r; r.kind = (int)RequestKind::Additive; r.path = scenePath; r.zone = nextZone()++;
    r.hidden = hidden; r.pos = position;
    requests().push_back(r);
    return r.zone;
}
void unloadZone(int zone)
{ Request r; r.kind = (int)RequestKind::UnloadZone; r.zone = zone; requests().push_back(std::move(r)); }
void activate()
{ Request r; r.kind = (int)RequestKind::Activate; requests().push_back(std::move(r)); }
void requestZoneVisible(int zone, bool visible)
{ Request r; r.kind = (int)RequestKind::ZoneVisible; r.zone = zone; r.flag = visible; requests().push_back(std::move(r)); }
void showZone(int zone) { requestZoneVisible(zone, true); }
void hideZone(int zone) { requestZoneVisible(zone, false); }
void requestZonePosition(int zone, const glm::vec3& p)
{ Request r; r.kind = (int)RequestKind::ZonePosition; r.zone = zone; r.pos = p; requests().push_back(std::move(r)); }
std::vector<Request> takeRequests()
{
    std::vector<Request> out = std::move(requests());
    requests().clear();
    return out;
}

// ── Zone table (maintained by the app after executing requests) ──────────────
void noteZoneLoaded(int zone, ZoneInfo info) { zones()[zone] = std::move(info); }
void noteZoneUnloaded(int zone)              { zones().erase(zone); }
void clearZones()                            { zones().clear(); }
const ZoneInfo* zoneInfo(int zone)
{
    auto it = zones().find(zone);
    return it == zones().end() ? nullptr : &it->second;
}
void notePendingLevel(bool pending) { pendingLevel() = pending; }
bool hasPendingLevel()              { return pendingLevel(); }

std::vector<int> loadedZones()
{
    std::vector<int> out;
    out.reserve(zones().size());
    for (const auto& [id, z] : zones()) out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
}
std::string zoneScene(int zone)
{
    const ZoneInfo* z = zoneInfo(zone);
    return z ? z->path : std::string();
}
glm::vec3 zonePosition(Ctx& c, int zone)
{
    const ZoneInfo* z = zoneInfo(zone);
    if (!z || !c.world) return glm::vec3(0.0f);
    const auto e = (entt::entity)z->root;
    if (!c.world->registry().valid(e)) return glm::vec3(0.0f);
    const auto* t = c.world->registry().try_get<TransformComponent>(e);
    return t ? t->position : glm::vec3(0.0f);
}
void setZonePosition(Ctx& c, int zone, const glm::vec3& p)
{
    const ZoneInfo* z = zoneInfo(zone);
    if (!z || !c.world) return;
    const auto e = (entt::entity)z->root;
    if (!c.world->registry().valid(e)) return;
    // Children follow via the hierarchy's world-matrix composition — moving the
    // zone's sub-root moves the whole zone.
    c.world->registry().get_or_emplace<TransformComponent>(e).position = p;
}
void setZoneVisible(Ctx& c, int zone, bool visible)
{
    const ZoneInfo* z = zoneInfo(zone);
    if (!z || !c.world) return;
    auto& reg = c.world->registry();
    for (uint32_t id : z->entities)
    {
        const auto e = (entt::entity)id;
        if (reg.valid(e)) setEntityVisible(reg, e, visible);
    }
}
std::vector<std::string> availableScenes(Ctx& c)
{
    std::vector<std::string> out;
    if (!c.content) return out;
    // Shipped builds: the packed scene index (a JSON string array).
    const auto bytes = c.content->readMountedEntry(sceneUuidForPath(kSceneIndexEntry));
    if (!bytes.empty())
    {
        const auto j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
        if (j.is_array())
        {
            for (const auto& e : j) if (e.is_string()) out.push_back(e.get<std::string>());
            return out;
        }
    }
    // Dev builds: scan the project (parent of the content root) for .hescene.
    const std::string root = c.content->contentRoot();
    if (root.empty()) return out;
    const auto projRoot = std::filesystem::path(root).parent_path();
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        projRoot, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end)
    {
        const bool regular = it->is_regular_file(ec);
        if (!ec && regular && it->path().extension() == ".hescene")
            out.push_back(it->path().lexically_relative(projRoot).generic_string());
        ec.clear();
        it.increment(ec);
    }
    std::sort(out.begin(), out.end());
    return out;
}
} // namespace scene

// ── String library ───────────────────────────────────────────────────────────
namespace str {
int length(const std::string& s) { return (int)s.size(); }
std::string substring(const std::string& s, int start, int count)
{
    if (start < 0) { count += start; start = 0; }
    if (start >= (int)s.size() || count <= 0) return {};
    return s.substr((size_t)start, (size_t)count);
}
bool contains(const std::string& s, const std::string& needle)
{ return needle.empty() || s.find(needle) != std::string::npos; }
int find(const std::string& s, const std::string& needle)
{
    const size_t p = s.find(needle);
    return p == std::string::npos ? -1 : (int)p;
}
std::string replace(const std::string& s, const std::string& from, const std::string& to)
{
    if (from.empty()) return s;
    std::string out; out.reserve(s.size());
    size_t pos = 0;
    while (true)
    {
        const size_t hit = s.find(from, pos);
        if (hit == std::string::npos) { out.append(s, pos, std::string::npos); return out; }
        out.append(s, pos, hit - pos);
        out += to;
        pos = hit + from.size();
    }
}
std::string toUpper(std::string const& s)
{ std::string r = s; for (char& ch : r) ch = (char)std::toupper((unsigned char)ch); return r; }
std::string toLower(std::string const& s)
{ std::string r = s; for (char& ch : r) ch = (char)std::tolower((unsigned char)ch); return r; }
std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}
bool startsWith(const std::string& s, const std::string& p)
{ return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool endsWith(const std::string& s, const std::string& p)
{ return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }
float toNumber(const std::string& s)
{
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    return end == s.c_str() ? 0.0f : v;
}
} // namespace str

// ── Math ─────────────────────────────────────────────────────────────────────
namespace math {
float sin(float x)   { return std::sin(x); }
float cos(float x)   { return std::cos(x); }
float tan(float x)   { return std::tan(x); }
float sqrt(float x)  { return std::sqrt(x); }
float abs(float x)   { return std::fabs(x); }
float floor(float x) { return std::floor(x); }
float ceil(float x)  { return std::ceil(x); }
float round(float x) { return std::round(x); }
float sign(float x)  { return (float)((x > 0.0f) - (x < 0.0f)); }
float pow(float b, float e) { return std::pow(b, e); }
float mod(float a, float b) { return b != 0.0f ? std::fmod(a, b) : 0.0f; }
float atan2(float y, float x) { return std::atan2(y, x); }
float radians(float deg) { return glm::radians(deg); }
float degrees(float rad) { return glm::degrees(rad); }
float min(float a, float b) { return a < b ? a : b; }
float max(float a, float b) { return a > b ? a : b; }
float clamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
float lerp(float a, float b, float t)    { return a + (b - a) * t; }
float length(const glm::vec2& v)                   { return glm::length(v); }
float distance(const glm::vec2& a, const glm::vec2& b) { return glm::length(a - b); }
float     length3(const glm::vec3& v)   { return glm::length(v); }
float     distance3(const glm::vec3& a, const glm::vec3& b) { return glm::length(a - b); }
// glm::normalize divides by the length unconditionally, so a zero vector comes
// back as NaN and poisons everything downstream. Gameplay wants "no direction".
glm::vec3 normalize3(const glm::vec3& v)
{
    const float len = glm::length(v);
    return len > 1e-8f ? v / len : glm::vec3(0.0f);
}
float     dot3(const glm::vec3& a, const glm::vec3& b)   { return glm::dot(a, b); }
glm::vec3 cross(const glm::vec3& a, const glm::vec3& b)  { return glm::cross(a, b); }
} // namespace math

// ── Random ───────────────────────────────────────────────────────────────────
namespace random {
static std::mt19937& gen() { static std::mt19937 g(0x9E3779B9u); return g; } // fixed default seed → reproducible
void  seed(uint32_t s)          { gen().seed(s); }
float value()                   { return std::uniform_real_distribution<float>(0.0f, 1.0f)(gen()); }
float range(float lo, float hi) { if (hi < lo) std::swap(lo, hi);
                                  return lo == hi ? lo : std::uniform_real_distribution<float>(lo, hi)(gen()); }
int   rangeInt(int lo, int hi)  { if (hi < lo) std::swap(lo, hi);
                                  return std::uniform_int_distribution<int>(lo, hi)(gen()); }
bool  chance(float p)           { return value() < (p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p)); }
} // namespace random

// ── Time / frame ─────────────────────────────────────────────────────────────
namespace time {
namespace {
struct Clock
{
    float    delta         = 0.0f;   // scaled — what deltaTime() answers
    float    unscaledDelta = 0.0f;   // as the app measured it
    double   elapsed       = 0.0;    // sum of the SCALED deltas
    uint64_t frame         = 0;
    float    scale         = 1.0f;
};
Clock& clk() { static Clock c; return c; }
}
// The scale is applied ONCE, here: elapsed accumulates the scaled delta, so a
// paused game's clock stands still and a script that integrates elapsed() sees
// the same time base as one integrating deltaTime().
void  advance(float dt)          { Clock& c = clk(); c.unscaledDelta = dt; c.delta = dt * c.scale; c.elapsed += c.delta; ++c.frame; }
void  reset()                    { clk() = Clock{}; }  // scale 1 again: play never starts paused
float deltaTime()                { return clk().delta; }
float unscaledDeltaTime()        { return clk().unscaledDelta; }
float elapsed()                  { return (float)clk().elapsed; }
int   frameCount()               { return (int)clk().frame; }
void  setTimeScale(float scale)  { clk().scale = std::clamp(scale, 0.0f, kMaxTimeScale); }
float timeScale()                { return clk().scale; }
} // namespace time

// ── Player possession ────────────────────────────────────────────────────────
namespace player {
namespace {
struct Table
{
    // controller → possessed character. Small by construction (one entry per
    // local player), so a vector of pairs would do — the map is for the reverse
    // lookup staying honest when a controller possesses a different character.
    std::unordered_map<uint32_t, uint32_t> byController;
    std::vector<uint32_t>                  controllers;   // session order
};
Table& tbl() { static Table t; return t; }
}

void possess(uint32_t controller, uint32_t character)
{
    if (controller == 0) return;
    // One character has ONE controller: taking it away from whoever held it is
    // the whole point of possessing, and leaving both entries would make
    // controllerOf answer with whichever the map happened to hash first.
    if (character != 0)
        for (auto& [c, ch] : tbl().byController)
            if (ch == character && c != controller) ch = 0;
    tbl().byController[controller] = character;
}
void unpossess(uint32_t controller) { tbl().byController.erase(controller); }

uint32_t possessed(uint32_t controller)
{
    const auto it = tbl().byController.find(controller);
    return it != tbl().byController.end() ? it->second : 0u;
}
uint32_t controllerOf(uint32_t character)
{
    if (character == 0) return 0u;
    for (const auto& [c, ch] : tbl().byController)
        if (ch == character) return c;
    return 0u;
}
uint32_t controller() { return tbl().controllers.empty() ? 0u : tbl().controllers.front(); }
uint32_t character()  { return possessed(controller()); }

void setControllers(const std::vector<uint32_t>& controllers)
{ tbl().controllers = controllers; }
void clear() { tbl() = Table{}; }
} // namespace player

// ── Input ────────────────────────────────────────────────────────────────────
namespace input {
namespace {
struct Snapshot
{
    std::unordered_set<std::string> keys;
    uint32_t buttons = 0;
    glm::vec2 pos{0.0f}, delta{0.0f};
    float scroll = 0.0f;
    bool  padConnected = false;
    float padAxes[SDL_GAMEPAD_AXIS_COUNT] = {};
    bool  padButtons[SDL_GAMEPAD_BUTTON_COUNT] = {};
};
Snapshot& snap() { static Snapshot s; return s; }
}
void setMouse(const glm::vec2& p, const glm::vec2& d, uint32_t mask, float sc)
{ Snapshot& s = snap(); s.pos = p; s.delta = d; s.buttons = mask; s.scroll = sc; }
void setKeysDown(const std::vector<std::string>& names)
{ Snapshot& s = snap(); s.keys.clear(); for (const auto& n : names) s.keys.insert(n); }
void clear() { snap() = Snapshot{}; }
bool      keyDown(const std::string& n) { return snap().keys.count(n) != 0; }
bool      mouseButton(int i)            { return i >= 0 && i < 32 && (snap().buttons & (1u << i)) != 0; }
glm::vec2 mousePosition()               { return snap().pos; }
glm::vec2 mouseDelta()                  { return snap().delta; }
float     scrollDelta()                 { return snap().scroll; }
void pushSdlSnapshot(float dx, float dy)
{
    int n = 0;
    const bool* ks = SDL_GetKeyboardState(&n);
    std::vector<std::string> down;
    if (ks)
        for (int sc = 0; sc < n; ++sc)
            if (ks[sc]) { const char* name = SDL_GetScancodeName((SDL_Scancode)sc); if (name && name[0]) down.emplace_back(name); }
    float mx = 0.0f, my = 0.0f;
    const SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
    uint32_t buttons = 0;
    if (mb & SDL_BUTTON_MASK(SDL_BUTTON_LEFT))   buttons |= 1u << 0;
    if (mb & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT))  buttons |= 1u << 1;
    if (mb & SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE)) buttons |= 1u << 2;
    setMouse({ mx, my }, { dx, dy }, buttons, 0.0f);
    setKeysDown(down);
}
void setGamepad(bool connected,
                const float* axes, size_t axisCount,
                const bool* buttons, size_t buttonCount)
{
    Snapshot& s = snap();
    s.padConnected = connected;
    const size_t na = std::min(axisCount,   (size_t)SDL_GAMEPAD_AXIS_COUNT);
    const size_t nb = std::min(buttonCount, (size_t)SDL_GAMEPAD_BUTTON_COUNT);
    for (size_t i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i)
        s.padAxes[i] = (axes && i < na) ? axes[i] : 0.0f;
    for (size_t i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i)
        s.padButtons[i] = (buttons && i < nb) && buttons[i];
}
bool gamepadConnected() { return snap().padConnected; }
bool gamepadButton(const std::string& name)
{
    const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name.c_str());
    return b != SDL_GAMEPAD_BUTTON_INVALID && snap().padButtons[b];
}
float gamepadAxis(const std::string& name)
{
    const SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(name.c_str());
    return a != SDL_GAMEPAD_AXIS_INVALID ? snap().padAxes[a] : 0.0f;
}

// Deliberately NOT part of Snapshot: the snapshot is this frame's readings and
// is overwritten wholesale every frame, while the mode is a decision that has to
// outlive the frame that made it. A mode inside Snapshot would be reset by the
// next pushSdlSnapshot, which is the bug this comment exists to prevent.
Mode& modeState() { static Mode m = Mode::GameAndUI; return m; }

void setMode(Mode m) { modeState() = m; }
Mode mode()          { return modeState(); }

void setModeGameOnly()  { setMode(Mode::GameOnly); }
void setModeGameAndUI() { setMode(Mode::GameAndUI); }
void setModeUIOnly()    { setMode(Mode::UIOnly); }

std::string modeName()
{
    switch (mode())
    {
    case Mode::GameOnly:  return "GameOnly";
    case Mode::UIOnly:    return "UIOnly";
    case Mode::GameAndUI: break;
    }
    return "GameAndUI";
}
} // namespace input

// ── Registry ─────────────────────────────────────────────────────────────────
namespace {

using P  = PinType;
using VV = std::vector<Value>;

// Value readers — tolerant of missing args (return the type's zero).
float       aF (const VV& a, size_t k) { return k < a.size() ? a[k].f   : 0.0f; }
bool        aB (const VV& a, size_t k) { return k < a.size() ? a[k].b   : false; }
int         aI (const VV& a, size_t k) { return k < a.size() ? a[k].i   : 0; }
// A Ref pin carries its instance handle in `ref`, not in `i` — reading it as an
// int would silently hand every object reference through as 0.
uint32_t    aR (const VV& a, size_t k) { return k < a.size() ? a[k].ref : 0u; }
std::string aS (const VV& a, size_t k) { return k < a.size() ? a[k].s   : std::string(); }
glm::vec2   aV2(const VV& a, size_t k) { return k < a.size() ? a[k].v2  : glm::vec2(0.0f); }
// Vector reads go by the value's OWN type, not by one hard-coded field. These
// rows used to be Color-typed (Color doubled as the vec3 type), so a caller may
// still hand over a Color — from a graph authored back then, or from Lua/Python
// passing four numbers. Reading `.col` unconditionally would return zeros for
// the Vec3 values every current caller sends.
glm::vec3   aV3(const VV& a, size_t k)
{
    if (k >= a.size()) return glm::vec3(0.0f);
    const Value& v = a[k];
    switch (v.type)
    {
        case P::Vec3:  return v.v3;
        case P::Vec4:  return glm::vec3(v.v4);
        case P::Color: return glm::vec3(v.col);
        default:       return glm::vec3(0.0f);
    }
}
glm::vec4   aV4(const VV& a, size_t k)
{
    if (k >= a.size()) return glm::vec4(0.0f);
    const Value& v = a[k];
    switch (v.type)
    {
        case P::Vec4:  return v.v4;
        case P::Color: return v.col;
        case P::Vec3:  return glm::vec4(v.v3, 0.0f);
        default:       return glm::vec4(0.0f);
    }
}
Value       v3 (const glm::vec3& v)    { return Value::ofVec3(v); }
// An RGB colour. It is three floats like a vector, but it stays a Color pin so
// the editor offers a colour picker for it — and it is opaque, because that is
// what a colour without an alpha channel means.
Value       rgb(const glm::vec3& v)    { return Value::ofColor(glm::vec4(v, 1.0f)); }

} // namespace

const std::vector<ApiFn>& registry()
{
    static const std::vector<ApiFn> table = []
    {
        std::vector<ApiFn> t;

        // Debug
        t.push_back({ "log", "Debug", true, {{"message", P::String}}, {}, "HE::api::log",
            [](Ctx& c, const VV& a){ log(c, aS(a, 0)); return VV{}; } });

        // Entities
        t.push_back({ "entity.getName", "Entity", false, {{"entity", P::Int}}, {{"name", P::String}}, "HE::api::entity::getName",
            [](Ctx& c, const VV& a){ return VV{ Value::ofString(entity::getName(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "entity.spawn", "Entity", true, {{"parent", P::Int}, {"name", P::String}}, {{"entity", P::Int}}, "HE::api::entity::spawn",
            [](Ctx& c, const VV& a){ return VV{ Value::ofInt((int)entity::spawn(c, (Entity)aI(a, 0), aS(a, 1))) }; } });
        t.push_back({ "entity.destroy", "Entity", true, {{"entity", P::Int}}, {}, "HE::api::entity::destroy",
            [](Ctx& c, const VV& a){ entity::destroy(c, (Entity)aI(a, 0)); return VV{}; } });
        // Pure: which entity an Entity-class object sits on. `self` takes no
        // argument because the caller's identity travels in the Ctx.
        t.push_back({ "entity.self", "Entity", false, {}, {{"entity", P::Int}}, "HE::api::entity::self",
            [](Ctx& c, const VV&){ return VV{ Value::ofInt((int)entity::self(c)) }; } });
        t.push_back({ "entity.selfObject", "Entity", false, {}, {{"object", P::Ref}}, "HE::api::entity::selfObject",
            [](Ctx& c, const VV&){ return VV{ Value::ofRef(entity::selfObject(c)) }; } });
        t.push_back({ "entity.instance", "Entity", false, {{"entity", P::Int}}, {{"object", P::Ref}}, "HE::api::entity::instance",
            [](Ctx& c, const VV& a){ return VV{ Value::ofRef(entity::instance(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "entity.owned", "Entity", false, {{"object", P::Ref}}, {{"entity", P::Int}}, "HE::api::entity::owned",
            [](Ctx& c, const VV& a){ return VV{ Value::ofInt((int)entity::owned(c, aR(a, 0))) }; } });
        t.push_back({ "entity.distance", "Entity", false, {{"a", P::Int}, {"b", P::Int}}, {{"distance", P::Float}}, "HE::api::entity::distance",
            [](Ctx& c, const VV& a){ return VV{ Value::ofFloat(entity::distance(c, (Entity)aI(a, 0), (Entity)aI(a, 1))) }; } });

        // Transform
        t.push_back({ "transform.getPosition", "Transform", false, {{"entity", P::Int}}, {{"position", P::Vec3}}, "HE::api::transform::getPosition",
            [](Ctx& c, const VV& a){ return VV{ v3(transform::getPosition(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "transform.setPosition", "Transform", true, {{"entity", P::Int}, {"position", P::Vec3}}, {}, "HE::api::transform::setPosition",
            [](Ctx& c, const VV& a){ transform::setPosition(c, (Entity)aI(a, 0), aV3(a, 1)); return VV{}; } });
        t.push_back({ "transform.getRotation", "Transform", false, {{"entity", P::Int}}, {{"rotation", P::Vec3}}, "HE::api::transform::getRotation",
            [](Ctx& c, const VV& a){ return VV{ v3(transform::getRotation(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "transform.setRotation", "Transform", true, {{"entity", P::Int}, {"rotation", P::Vec3}}, {}, "HE::api::transform::setRotation",
            [](Ctx& c, const VV& a){ transform::setRotation(c, (Entity)aI(a, 0), aV3(a, 1)); return VV{}; } });
        t.push_back({ "transform.getScale", "Transform", false, {{"entity", P::Int}}, {{"scale", P::Vec3}}, "HE::api::transform::getScale",
            [](Ctx& c, const VV& a){ return VV{ v3(transform::getScale(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "transform.setScale", "Transform", true, {{"entity", P::Int}, {"scale", P::Vec3}}, {}, "HE::api::transform::setScale",
            [](Ctx& c, const VV& a){ transform::setScale(c, (Entity)aI(a, 0), aV3(a, 1)); return VV{}; } });
        // World space. The six above are local — for a parented entity that is
        // its offset inside the parent, not where it stands.
        t.push_back({ "transform.getWorldPosition", "Transform", false, {{"entity", P::Int}}, {{"position", P::Vec3}}, "HE::api::transform::getWorldPosition",
            [](Ctx& c, const VV& a){ return VV{ v3(transform::getWorldPosition(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "transform.setWorldPosition", "Transform", true, {{"entity", P::Int}, {"position", P::Vec3}}, {}, "HE::api::transform::setWorldPosition",
            [](Ctx& c, const VV& a){ transform::setWorldPosition(c, (Entity)aI(a, 0), aV3(a, 1)); return VV{}; } });

        // Physics
        t.push_back({ "physics.raycast", "Physics", false,
            {{"origin", P::Vec3}, {"direction", P::Vec3}, {"maxDistance", P::Float}},
            {{"hit", P::Bool}, {"entity", P::Int}, {"point", P::Vec3}, {"normal", P::Vec3}, {"distance", P::Float}},
            "HE::api::physics::raycast",
            [](Ctx& c, const VV& a){ auto r = physics::raycast(c, aV3(a, 0), aV3(a, 1), aF(a, 2));
                return VV{ Value::ofBool(r.hit), Value::ofInt((int)r.entity), v3(r.point), v3(r.normal), Value::ofFloat(r.distance) }; } });
        t.push_back({ "physics.setVelocity", "Physics", true, {{"entity", P::Int}, {"velocity", P::Vec3}}, {}, "HE::api::physics::setVelocity",
            [](Ctx& c, const VV& a){ physics::setVelocity(c, (Entity)aI(a, 0), aV3(a, 1)); return VV{}; } });
        t.push_back({ "physics.isGrounded", "Physics", false, {{"entity", P::Int}}, {{"grounded", P::Bool}}, "HE::api::physics::isGrounded",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(physics::isGrounded(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "physics.sphereCast", "Physics", false,
            {{"origin", P::Vec3}, {"direction", P::Vec3}, {"radius", P::Float}, {"maxDistance", P::Float}},
            {{"hit", P::Bool}, {"entity", P::Int}, {"point", P::Vec3}, {"normal", P::Vec3}, {"distance", P::Float}},
            "HE::api::physics::sphereCast",
            [](Ctx& c, const VV& a){ auto r = physics::sphereCast(c, aV3(a, 0), aV3(a, 1), aF(a, 2), aF(a, 3));
                return VV{ Value::ofBool(r.hit), Value::ofInt((int)r.entity), v3(r.point), v3(r.normal), Value::ofFloat(r.distance) }; } });
        t.push_back({ "physics.overlapSphere", "Physics", false,
            {{"center", P::Vec3}, {"radius", P::Float}}, {{"entities", P::Int, /*isArray=*/true}},
            "HE::api::physics::overlapSphere",
            [](Ctx& c, const VV& a){
                Value arr; arr.isArray = true; arr.type = P::Int;
                for (Entity e : physics::overlapSphere(c, aV3(a, 0), aF(a, 1)))
                    arr.items.push_back(Value::ofInt((int)e));
                return VV{ std::move(arr) }; } });
        t.push_back({ "physics.addForce", "Physics", true, {{"entity", P::Int}, {"force", P::Vec3}}, {{"ok", P::Bool}}, "HE::api::physics::addForce",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(physics::addForce(c, (Entity)aI(a, 0), aV3(a, 1))) }; } });
        t.push_back({ "physics.addImpulse", "Physics", true, {{"entity", P::Int}, {"impulse", P::Vec3}}, {{"ok", P::Bool}}, "HE::api::physics::addImpulse",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(physics::addImpulse(c, (Entity)aI(a, 0), aV3(a, 1))) }; } });
        t.push_back({ "physics.addTorque", "Physics", true, {{"entity", P::Int}, {"torque", P::Vec3}}, {{"ok", P::Bool}}, "HE::api::physics::addTorque",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(physics::addTorque(c, (Entity)aI(a, 0), aV3(a, 1))) }; } });
        t.push_back({ "physics.getVelocity", "Physics", false, {{"entity", P::Int}}, {{"velocity", P::Vec3}}, "HE::api::physics::getVelocity",
            [](Ctx& c, const VV& a){ return VV{ v3(physics::getVelocity(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "physics.setGravity", "Physics", true, {{"gravity", P::Vec3}}, {}, "HE::api::physics::setGravity",
            [](Ctx& c, const VV& a){ physics::setGravity(c, aV3(a, 0)); return VV{}; } });
        t.push_back({ "physics.getGravity", "Physics", false, {}, {{"gravity", P::Vec3}}, "HE::api::physics::getGravity",
            [](Ctx& c, const VV&){ return VV{ v3(physics::getGravity(c)) }; } });

        // Materials
        t.push_back({ "material.getParam", "Material", false, {{"entity", P::Int}, {"name", P::String}}, {{"value", P::Color}}, "HE::api::material::getParam",
            [](Ctx& c, const VV& a){ return VV{ Value::ofColor(material::getParam(c, (Entity)aI(a, 0), aS(a, 1))) }; } });
        t.push_back({ "material.setParam", "Material", true, {{"entity", P::Int}, {"name", P::String}, {"value", P::Color}}, {{"ok", P::Bool}}, "HE::api::material::setParam",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(material::setParam(c, (Entity)aI(a, 0), aS(a, 1), aV4(a, 2))) }; } });

        // Animator — the state machine's parameters, and the only way to steer it
        t.push_back({ "animator.setParam", "Animator", true, {{"entity", P::Int}, {"name", P::String}, {"value", P::Float}}, {}, "HE::api::animator::setParam",
            [](Ctx& c, const VV& a){ animator::setParam(c, (Entity)aI(a, 0), aS(a, 1), aF(a, 2)); return VV{}; } });
        t.push_back({ "animator.getParam", "Animator", false, {{"entity", P::Int}, {"name", P::String}}, {{"value", P::Float}}, "HE::api::animator::getParam",
            [](Ctx& c, const VV& a){ return VV{ Value::ofFloat(animator::getParam(c, (Entity)aI(a, 0), aS(a, 1))) }; } });
        t.push_back({ "animator.getState", "Animator", false, {{"entity", P::Int}}, {{"state", P::String}}, "HE::api::animator::getState",
            [](Ctx& c, const VV& a){ return VV{ Value::ofString(animator::getState(c, (Entity)aI(a, 0))) }; } });

        // Movement — the reads an animator asks for. Derived from the character
        // controller on the spot, so there is no second copy to go stale.
        t.push_back({ "movement.speed", "Movement", false, {{"entity", P::Int}}, {{"speed", P::Float}}, "HE::api::movement::speed",
            [](Ctx& c, const VV& a){ return VV{ Value::ofFloat(movement::speed(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "movement.verticalSpeed", "Movement", false, {{"entity", P::Int}}, {{"speed", P::Float}}, "HE::api::movement::verticalSpeed",
            [](Ctx& c, const VV& a){ return VV{ Value::ofFloat(movement::verticalSpeed(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "movement.isGrounded", "Movement", false, {{"entity", P::Int}}, {{"grounded", P::Bool}}, "HE::api::movement::isGrounded",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(movement::isGrounded(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "movement.velocity", "Movement", false, {{"entity", P::Int}}, {{"velocity", P::Vec3}}, "HE::api::movement::velocity",
            [](Ctx& c, const VV& a){ return VV{ v3(movement::velocity(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "movement.forwardAmount", "Movement", false, {{"entity", P::Int}}, {{"amount", P::Float}}, "HE::api::movement::forwardAmount",
            [](Ctx& c, const VV& a){ return VV{ Value::ofFloat(movement::forwardAmount(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "movement.rightAmount", "Movement", false, {{"entity", P::Int}}, {{"amount", P::Float}}, "HE::api::movement::rightAmount",
            [](Ctx& c, const VV& a){ return VV{ Value::ofFloat(movement::rightAmount(c, (Entity)aI(a, 0))) }; } });

        // Locomotion — the writes. Not offered to a sync graph (see its palette).
        t.push_back({ "locomotion.move", "Locomotion", true, {{"entity", P::Int}, {"direction", P::Vec3}}, {}, "HE::api::locomotion::move",
            [](Ctx& c, const VV& a){ locomotion::move(c, (Entity)aI(a, 0), aV3(a, 1)); return VV{}; } });
        t.push_back({ "locomotion.look", "Locomotion", true, {{"entity", P::Int}, {"yaw", P::Float}, {"pitch", P::Float}}, {}, "HE::api::locomotion::look",
            [](Ctx& c, const VV& a){ locomotion::look(c, (Entity)aI(a, 0), aF(a, 1), aF(a, 2)); return VV{}; } });
        t.push_back({ "locomotion.setMaxSpeed", "Locomotion", true, {{"entity", P::Int}, {"speed", P::Float}}, {}, "HE::api::locomotion::setMaxSpeed",
            [](Ctx& c, const VV& a){ locomotion::setMaxSpeed(c, (Entity)aI(a, 0), aF(a, 1)); return VV{}; } });
        t.push_back({ "locomotion.setOrientToMovement", "Locomotion", true, {{"entity", P::Int}, {"on", P::Bool}}, {}, "HE::api::locomotion::setOrientToMovement",
            [](Ctx& c, const VV& a){ locomotion::setOrientToMovement(c, (Entity)aI(a, 0), aB(a, 1)); return VV{}; } });

        // Entity UI
        t.push_back({ "ui.getText", "UI", false, {{"entity", P::Int}}, {{"text", P::String}}, "HE::api::ui::getText",
            [](Ctx& c, const VV& a){ return VV{ Value::ofString(ui::getText(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "ui.setText", "UI", true, {{"entity", P::Int}, {"text", P::String}}, {}, "HE::api::ui::setText",
            [](Ctx& c, const VV& a){ ui::setText(c, (Entity)aI(a, 0), aS(a, 1)); return VV{}; } });
        t.push_back({ "ui.getColor", "UI", false, {{"entity", P::Int}}, {{"color", P::Color}}, "HE::api::ui::getColor",
            [](Ctx& c, const VV& a){ return VV{ Value::ofColor(ui::getColor(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "ui.setColor", "UI", true, {{"entity", P::Int}, {"color", P::Color}}, {}, "HE::api::ui::setColor",
            [](Ctx& c, const VV& a){ ui::setColor(c, (Entity)aI(a, 0), aV4(a, 1)); return VV{}; } });
        t.push_back({ "ui.getVisible", "UI", false, {{"entity", P::Int}}, {{"visible", P::Bool}}, "HE::api::ui::getVisible",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(ui::getVisible(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "ui.setVisible", "UI", true, {{"entity", P::Int}, {"visible", P::Bool}}, {}, "HE::api::ui::setVisible",
            [](Ctx& c, const VV& a){ ui::setVisible(c, (Entity)aI(a, 0), aB(a, 1)); return VV{}; } });
        t.push_back({ "ui.getPosition", "UI", false, {{"entity", P::Int}}, {{"position", P::Vec2}}, "HE::api::ui::getPosition",
            [](Ctx& c, const VV& a){ return VV{ Value::ofVec2(ui::getPosition(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "ui.setPosition", "UI", true, {{"entity", P::Int}, {"position", P::Vec2}}, {}, "HE::api::ui::setPosition",
            [](Ctx& c, const VV& a){ ui::setPosition(c, (Entity)aI(a, 0), aV2(a, 1)); return VV{}; } });
        t.push_back({ "ui.getSize", "UI", false, {{"entity", P::Int}}, {{"size", P::Vec2}}, "HE::api::ui::getSize",
            [](Ctx& c, const VV& a){ return VV{ Value::ofVec2(ui::getSize(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "ui.setSize", "UI", true, {{"entity", P::Int}, {"size", P::Vec2}}, {}, "HE::api::ui::setSize",
            [](Ctx& c, const VV& a){ ui::setSize(c, (Entity)aI(a, 0), aV2(a, 1)); return VV{}; } });
        t.push_back({ "ui.setMaterialParam", "UI", true, {{"entity", P::Int}, {"name", P::String}, {"value", P::Color}}, {{"ok", P::Bool}}, "HE::api::ui::setMaterialParam",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(ui::setMaterialParam(c, (Entity)aI(a, 0), aS(a, 1), aV4(a, 2))) }; } });
        // Pure: a snapshot the app refreshes once per frame, like the input rows.
        t.push_back({ "ui.pointerOverUI", "UI", false, {}, {{"over", P::Bool}}, "HE::api::ui::pointerOverUI",
            [](Ctx& c, const VV&){ return VV{ Value::ofBool(ui::pointerOverUI(c)) }; } });

        // Live widgets — the EXTRAS only. A widget's lifecycle belongs to the four
        // built-in nodes (Create/Show/Hide/Destroy Widget), which are the better
        // half: Create Widget picks its asset from a list instead of a typed path.
        // Registry twins of those four existed and were a trap, because the node
        // hands its id out as a Ref and these rows took an Int — and Ref/Int do not
        // convert (canConvertPinType), so the two halves could not be wired
        // together at all. Hence Ref here too: the id IS the widget reference
        // (widget id == scriptId), so these three plug straight into the node.
        t.push_back({ "widget.setZOrder", "Widget", true, {{"widget", P::Ref}, {"z", P::Int}}, {}, "HE::api::widget::setZOrder",
            [](Ctx& c, const VV& a){ widget::setZOrder(c, (int)aR(a, 0), aI(a, 1)); return VV{}; } });
        t.push_back({ "widget.isVisible", "Widget", false, {{"widget", P::Ref}}, {{"visible", P::Bool}}, "HE::api::widget::isVisible",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(widget::isVisible(c, (int)aR(a, 0))) }; } });
        t.push_back({ "widget.callFunction", "Widget", true, {{"widget", P::Ref}, {"function", P::String}}, {{"ok", P::Bool}}, "HE::api::widget::callFunction",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(widget::callFunction(c, (int)aR(a, 0), aS(a, 1))) }; } });
        // Building the interface while it runs. The child comes back as a Ref
        // for the same reason the widget goes in as one: it IS a live object,
        // and Set External / Call External / Bind Event are how one talks to it.
        t.push_back({ "widget.addChild", "Widget", true,
            {{"widget", P::Ref}, {"parent", P::String}, {"widgetAsset", P::String}},
            {{"child", P::Ref}}, "HE::api::widget::addChild",
            [](Ctx& c, const VV& a){ return VV{ Value::ofRef(
                (uint64_t)widget::addChild(c, (int)aR(a, 0), aS(a, 1), aS(a, 2))) }; } });
        t.push_back({ "widget.removeChild", "Widget", true,
            {{"widget", P::Ref}, {"child", P::Ref}}, {{"ok", P::Bool}},
            "HE::api::widget::removeChild",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(
                widget::removeChild(c, (int)aR(a, 0), (int)aR(a, 1))) }; } });
        t.push_back({ "widget.clearChildren", "Widget", true,
            {{"widget", P::Ref}, {"parent", P::String}}, {{"removed", P::Int}},
            "HE::api::widget::clearChildren",
            [](Ctx& c, const VV& a){ return VV{ Value::ofInt(
                widget::clearChildren(c, (int)aR(a, 0), aS(a, 1))) }; } });

        // Theme — one place decides what the whole application looks like.
        t.push_back({ "theme.set", "Theme", true, {{"themeAsset", P::String}}, {{"ok", P::Bool}},
            "HE::api::theme::set",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(theme::set(c, aS(a, 0))) }; } });
        t.push_back({ "theme.setMode", "Theme", true, {{"mode", P::String}}, {},
            "HE::api::theme::setMode",
            [](Ctx& c, const VV& a){ theme::setMode(c, aS(a, 0)); return VV{}; } });
        t.push_back({ "theme.getMode", "Theme", false, {}, {{"mode", P::String}},
            "HE::api::theme::mode",
            [](Ctx& c, const VV&){ return VV{ Value::ofString(theme::mode(c)) }; } });
        t.push_back({ "theme.getPreference", "Theme", false, {}, {{"preference", P::String}},
            "HE::api::theme::preference",
            [](Ctx& c, const VV&){ return VV{ Value::ofString(theme::preference(c)) }; } });

        // Cursor
        t.push_back({ "cursor.setVisible", "Cursor", true, {{"show", P::Bool}}, {}, "HE::api::cursor::setVisible",
            [](Ctx& c, const VV& a){ cursor::setVisible(c, aB(a, 0)); return VV{}; } });

        // Application — what a main menu's last button does, plus the window an
        // application owns (docs/he-apps-plan.md A4). A game leaves the window
        // rows unbound, so they warn once and do nothing there.
        t.push_back({ "app.quit", "App", true, {}, {}, "HE::api::app::quit",
            [](Ctx& c, const VV&){ app::quit(c); return VV{}; } });
        t.push_back({ "app.setTitle", "App", true, {{"title", P::String}}, {}, "HE::api::app::setTitle",
            [](Ctx& c, const VV& a){ app::setTitle(c, aS(a, 0)); return VV{}; } });
        t.push_back({ "app.setSize", "App", true, {{"width", P::Int}, {"height", P::Int}}, {},
            "HE::api::app::setSize",
            [](Ctx& c, const VV& a){ app::setSize(c, aI(a, 0), aI(a, 1)); return VV{}; } });
        t.push_back({ "app.size", "App", false, {}, {{"size", P::Vec2}}, "HE::api::app::size",
            [](Ctx& c, const VV&){ return VV{ Value::ofVec2(app::size(c)) }; } });
        t.push_back({ "app.requestRedraw", "App", true, {}, {}, "HE::api::app::requestRedraw",
            [](Ctx& c, const VV&){ app::requestRedraw(c); return VV{}; } });

        // Clipboard — the same system clipboard Ctrl+C/V already use in a focused
        // text field, reachable from a graph.
        t.push_back({ "clipboard.getText", "Clipboard", false, {}, {{"text", P::String}},
            "HE::api::clipboard::getText",
            [](Ctx& c, const VV&){ return VV{ Value::ofString(clipboard::getText(c)) }; } });
        t.push_back({ "clipboard.setText", "Clipboard", true, {{"text", P::String}}, {},
            "HE::api::clipboard::setText",
            [](Ctx& c, const VV& a){ clipboard::setText(c, aS(a, 0)); return VV{}; } });
        t.push_back({ "clipboard.hasText", "Clipboard", false, {}, {{"has", P::Bool}},
            "HE::api::clipboard::hasText",
            [](Ctx& c, const VV&){ return VV{ Value::ofBool(clipboard::hasText(c)) }; } });

        // Native dialogs — the blocking, OS-drawn kind.
        t.push_back({ "dialog.message", "Dialog", true,
            {{"title", P::String}, {"text", P::String}, {"kind", P::Int}}, {},
            "HE::api::dialog::message",
            [](Ctx& c, const VV& a){ dialog::message(c, aS(a, 0), aS(a, 1), aI(a, 2)); return VV{}; } });
        t.push_back({ "dialog.confirm", "Dialog", true,
            {{"title", P::String}, {"text", P::String},
             {"affirmative", P::String}, {"negative", P::String}},
            {{"confirmed", P::Bool}}, "HE::api::dialog::confirm",
            [](Ctx& c, const VV& a){
                return VV{ Value::ofBool(dialog::confirm(c, aS(a, 0), aS(a, 1), aS(a, 2), aS(a, 3))) }; } });

        // JSON — text in, text out, addressed by a dotted path. Numbers cross as
        // Float, which is what every numeric pin in a graph already is.
        t.push_back({ "json.getString", "JSON", false,
            {{"text", P::String}, {"path", P::String}, {"fallback", P::String}},
            {{"value", P::String}}, "HE::api::json::getString",
            [](Ctx& c, const VV& a){ return VV{ Value::ofString(json::getString(c, aS(a, 0), aS(a, 1), aS(a, 2))) }; } });
        t.push_back({ "json.getNumber", "JSON", false,
            {{"text", P::String}, {"path", P::String}, {"fallback", P::Float}},
            {{"value", P::Float}}, "HE::api::json::getNumber",
            [](Ctx& c, const VV& a){ return VV{ Value::ofFloat((float)json::getNumber(c, aS(a, 0), aS(a, 1), aF(a, 2))) }; } });
        t.push_back({ "json.getBool", "JSON", false,
            {{"text", P::String}, {"path", P::String}, {"fallback", P::Bool}},
            {{"value", P::Bool}}, "HE::api::json::getBool",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(json::getBool(c, aS(a, 0), aS(a, 1), aB(a, 2))) }; } });
        t.push_back({ "json.has", "JSON", false,
            {{"text", P::String}, {"path", P::String}}, {{"present", P::Bool}}, "HE::api::json::has",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(json::has(c, aS(a, 0), aS(a, 1))) }; } });
        t.push_back({ "json.count", "JSON", false,
            {{"text", P::String}, {"path", P::String}}, {{"count", P::Int}}, "HE::api::json::count",
            [](Ctx& c, const VV& a){ return VV{ Value::ofInt(json::count(c, aS(a, 0), aS(a, 1))) }; } });
        t.push_back({ "json.setString", "JSON", false,
            {{"text", P::String}, {"path", P::String}, {"value", P::String}},
            {{"result", P::String}}, "HE::api::json::setString",
            [](Ctx& c, const VV& a){ return VV{ Value::ofString(json::setString(c, aS(a, 0), aS(a, 1), aS(a, 2))) }; } });
        t.push_back({ "json.setNumber", "JSON", false,
            {{"text", P::String}, {"path", P::String}, {"value", P::Float}},
            {{"result", P::String}}, "HE::api::json::setNumber",
            [](Ctx& c, const VV& a){ return VV{ Value::ofString(json::setNumber(c, aS(a, 0), aS(a, 1), aF(a, 2))) }; } });
        t.push_back({ "json.setBool", "JSON", false,
            {{"text", P::String}, {"path", P::String}, {"value", P::Bool}},
            {{"result", P::String}}, "HE::api::json::setBool",
            [](Ctx& c, const VV& a){ return VV{ Value::ofString(json::setBool(c, aS(a, 0), aS(a, 1), aB(a, 2))) }; } });

        // Preferences — small persistent settings, separate from the save system.
        t.push_back({ "prefs.getString", "Prefs", false,
            {{"key", P::String}, {"fallback", P::String}}, {{"value", P::String}},
            "HE::api::prefs::getString",
            [](Ctx& c, const VV& a){ return VV{ Value::ofString(prefs::getString(c, aS(a, 0), aS(a, 1))) }; } });
        t.push_back({ "prefs.getNumber", "Prefs", false,
            {{"key", P::String}, {"fallback", P::Float}}, {{"value", P::Float}},
            "HE::api::prefs::getNumber",
            [](Ctx& c, const VV& a){ return VV{ Value::ofFloat((float)prefs::getNumber(c, aS(a, 0), aF(a, 1))) }; } });
        t.push_back({ "prefs.getBool", "Prefs", false,
            {{"key", P::String}, {"fallback", P::Bool}}, {{"value", P::Bool}},
            "HE::api::prefs::getBool",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(prefs::getBool(c, aS(a, 0), aB(a, 1))) }; } });
        t.push_back({ "prefs.setString", "Prefs", true,
            {{"key", P::String}, {"value", P::String}}, {}, "HE::api::prefs::setString",
            [](Ctx& c, const VV& a){ prefs::setString(c, aS(a, 0), aS(a, 1)); return VV{}; } });
        t.push_back({ "prefs.setNumber", "Prefs", true,
            {{"key", P::String}, {"value", P::Float}}, {}, "HE::api::prefs::setNumber",
            [](Ctx& c, const VV& a){ prefs::setNumber(c, aS(a, 0), aF(a, 1)); return VV{}; } });
        t.push_back({ "prefs.setBool", "Prefs", true,
            {{"key", P::String}, {"value", P::Bool}}, {}, "HE::api::prefs::setBool",
            [](Ctx& c, const VV& a){ prefs::setBool(c, aS(a, 0), aB(a, 1)); return VV{}; } });
        t.push_back({ "prefs.has", "Prefs", false,
            {{"key", P::String}}, {{"present", P::Bool}}, "HE::api::prefs::has",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(prefs::has(c, aS(a, 0))) }; } });
        t.push_back({ "prefs.remove", "Prefs", true,
            {{"key", P::String}}, {{"removed", P::Bool}}, "HE::api::prefs::remove",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(prefs::remove(c, aS(a, 0))) }; } });
        t.push_back({ "prefs.clear", "Prefs", true, {}, {}, "HE::api::prefs::clear",
            [](Ctx& c, const VV&){ prefs::clear(c); return VV{}; } });

        // Date and time — the WALL clock, unlike the time group.
        t.push_back({ "datetime.now", "DateTime", false, {}, {{"epochSeconds", P::Float}},
            "HE::api::datetime::now",
            [](Ctx& c, const VV&){ return VV{ Value::ofFloat((float)datetime::now(c)) }; } });
        t.push_back({ "datetime.format", "DateTime", false,
            {{"epochSeconds", P::Float}, {"format", P::String}}, {{"text", P::String}},
            "HE::api::datetime::format",
            [](Ctx& c, const VV& a){ return VV{ Value::ofString(datetime::format(c, aF(a, 0), aS(a, 1))) }; } });
        {
            // One row per field, all the same shape.
            struct Part { const char* id; const char* cpp; int (*fn)(Ctx&, double); };
            static const Part kParts[] = {
                { "datetime.year",    "HE::api::datetime::year",    &datetime::year    },
                { "datetime.month",   "HE::api::datetime::month",   &datetime::month   },
                { "datetime.day",     "HE::api::datetime::day",     &datetime::day     },
                { "datetime.hour",    "HE::api::datetime::hour",    &datetime::hour    },
                { "datetime.minute",  "HE::api::datetime::minute",  &datetime::minute  },
                { "datetime.second",  "HE::api::datetime::second",  &datetime::second  },
                { "datetime.weekday", "HE::api::datetime::weekday", &datetime::weekday },
            };
            for (const Part& p : kParts)
                t.push_back({ p.id, "DateTime", false, {{"epochSeconds", P::Float}},
                    {{"value", P::Int}}, p.cpp,
                    [fn = p.fn](Ctx& c, const VV& a){ return VV{ Value::ofInt(fn(c, aF(a, 0))) }; } });
        }

        // Math (pure)
        auto unary  = [&](const char* id, const char* cpp, float(*fn)(float)) {
            t.push_back({ id, "Math", false, {{"x", P::Float}}, {{"result", P::Float}}, cpp,
                [fn](Ctx&, const VV& a){ return VV{ Value::ofFloat(fn(aF(a, 0))) }; } }); };
        auto binary = [&](const char* id, const char* cpp, float(*fn)(float, float),
                          const char* p0, const char* p1) {
            t.push_back({ id, "Math", false, {{p0, P::Float}, {p1, P::Float}}, {{"result", P::Float}}, cpp,
                [fn](Ctx&, const VV& a){ return VV{ Value::ofFloat(fn(aF(a, 0), aF(a, 1))) }; } }); };

        unary("math.sin",   "HE::api::math::sin",   math::sin);
        unary("math.cos",   "HE::api::math::cos",   math::cos);
        unary("math.tan",   "HE::api::math::tan",   math::tan);
        unary("math.sqrt",  "HE::api::math::sqrt",  math::sqrt);
        unary("math.abs",   "HE::api::math::abs",   math::abs);
        unary("math.floor", "HE::api::math::floor", math::floor);
        unary("math.ceil",  "HE::api::math::ceil",  math::ceil);
        unary("math.round", "HE::api::math::round", math::round);
        unary("math.sign",  "HE::api::math::sign",  math::sign);
        unary("math.radians", "HE::api::math::radians", math::radians);
        unary("math.degrees", "HE::api::math::degrees", math::degrees);
        binary("math.pow",   "HE::api::math::pow",   math::pow,   "base", "exp");
        binary("math.mod",   "HE::api::math::mod",   math::mod,   "a", "b");
        binary("math.atan2", "HE::api::math::atan2", math::atan2, "y", "x");
        binary("math.min",   "HE::api::math::min",   math::min,   "a", "b");
        binary("math.max",   "HE::api::math::max",   math::max,   "a", "b");
        t.push_back({ "math.clamp", "Math", false, {{"x", P::Float}, {"lo", P::Float}, {"hi", P::Float}}, {{"result", P::Float}}, "HE::api::math::clamp",
            [](Ctx&, const VV& a){ return VV{ Value::ofFloat(math::clamp(aF(a, 0), aF(a, 1), aF(a, 2))) }; } });
        t.push_back({ "math.lerp", "Math", false, {{"a", P::Float}, {"b", P::Float}, {"t", P::Float}}, {{"result", P::Float}}, "HE::api::math::lerp",
            [](Ctx&, const VV& a){ return VV{ Value::ofFloat(math::lerp(aF(a, 0), aF(a, 1), aF(a, 2))) }; } });
        t.push_back({ "math.length", "Math", false, {{"v", P::Vec2}}, {{"result", P::Float}}, "HE::api::math::length",
            [](Ctx&, const VV& a){ return VV{ Value::ofFloat(math::length(aV2(a, 0))) }; } });
        t.push_back({ "math.distance", "Math", false, {{"a", P::Vec2}, {"b", P::Vec2}}, {{"result", P::Float}}, "HE::api::math::distance",
            [](Ctx&, const VV& a){ return VV{ Value::ofFloat(math::distance(aV2(a, 0), aV2(a, 1))) }; } });
        t.push_back({ "math.length3", "Math", false, {{"v", P::Vec3}}, {{"result", P::Float}}, "HE::api::math::length3",
            [](Ctx&, const VV& a){ return VV{ Value::ofFloat(math::length3(aV3(a, 0))) }; } });
        t.push_back({ "math.distance3", "Math", false, {{"a", P::Vec3}, {"b", P::Vec3}}, {{"result", P::Float}}, "HE::api::math::distance3",
            [](Ctx&, const VV& a){ return VV{ Value::ofFloat(math::distance3(aV3(a, 0), aV3(a, 1))) }; } });
        t.push_back({ "math.normalize3", "Math", false, {{"v", P::Vec3}}, {{"result", P::Vec3}}, "HE::api::math::normalize3",
            [](Ctx&, const VV& a){ return VV{ v3(math::normalize3(aV3(a, 0))) }; } });
        t.push_back({ "math.dot3", "Math", false, {{"a", P::Vec3}, {"b", P::Vec3}}, {{"result", P::Float}}, "HE::api::math::dot3",
            [](Ctx&, const VV& a){ return VV{ Value::ofFloat(math::dot3(aV3(a, 0), aV3(a, 1))) }; } });
        t.push_back({ "math.cross", "Math", false, {{"a", P::Vec3}, {"b", P::Vec3}}, {{"result", P::Vec3}}, "HE::api::math::cross",
            [](Ctx&, const VV& a){ return VV{ v3(math::cross(aV3(a, 0), aV3(a, 1))) }; } });

        // Random (stateful → isExec, so a HorizonCode node caches one draw per run)
        t.push_back({ "random.seed", "Random", true, {{"seed", P::Int}}, {}, "HE::api::random::seed",
            [](Ctx&, const VV& a){ random::seed((uint32_t)aI(a, 0)); return VV{}; } });
        t.push_back({ "random.value", "Random", true, {}, {{"value", P::Float}}, "HE::api::random::value",
            [](Ctx&, const VV&){ return VV{ Value::ofFloat(random::value()) }; } });
        t.push_back({ "random.range", "Random", true, {{"min", P::Float}, {"max", P::Float}}, {{"value", P::Float}}, "HE::api::random::range",
            [](Ctx&, const VV& a){ return VV{ Value::ofFloat(random::range(aF(a, 0), aF(a, 1))) }; } });
        t.push_back({ "random.rangeInt", "Random", true, {{"min", P::Int}, {"max", P::Int}}, {{"value", P::Int}}, "HE::api::random::rangeInt",
            [](Ctx&, const VV& a){ return VV{ Value::ofInt(random::rangeInt(aI(a, 0), aI(a, 1))) }; } });
        t.push_back({ "random.chance", "Random", true, {{"p", P::Float}}, {{"value", P::Bool}}, "HE::api::random::chance",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(random::chance(aF(a, 0))) }; } });

        // Time / frame (pure getters; the app advances the clock each frame)
        t.push_back({ "time.deltaTime", "Time", false, {}, {{"dt", P::Float}}, "HE::api::time::deltaTime",
            [](Ctx&, const VV&){ return VV{ Value::ofFloat(time::deltaTime()) }; } });
        t.push_back({ "time.elapsed", "Time", false, {}, {{"seconds", P::Float}}, "HE::api::time::elapsed",
            [](Ctx&, const VV&){ return VV{ Value::ofFloat(time::elapsed()) }; } });
        t.push_back({ "time.frameCount", "Time", false, {}, {{"frame", P::Int}}, "HE::api::time::frameCount",
            [](Ctx&, const VV&){ return VV{ Value::ofInt(time::frameCount()) }; } });
        // Time control. setTimeScale is the only stateful one (isExec) — the two
        // getters are constant within a frame like the rest of the group.
        t.push_back({ "time.setTimeScale", "Time", true, {{"scale", P::Float}}, {}, "HE::api::time::setTimeScale",
            [](Ctx&, const VV& a){ time::setTimeScale(aF(a, 0)); return VV{}; } });
        t.push_back({ "time.timeScale", "Time", false, {}, {{"scale", P::Float}}, "HE::api::time::timeScale",
            [](Ctx&, const VV&){ return VV{ Value::ofFloat(time::timeScale()) }; } });
        t.push_back({ "time.unscaledDeltaTime", "Time", false, {}, {{"dt", P::Float}}, "HE::api::time::unscaledDeltaTime",
            [](Ctx&, const VV&){ return VV{ Value::ofFloat(time::unscaledDeltaTime()) }; } });

        // Player possession. The two that TAKE a controller are also the
        // PlayerController base class's member surface (HorizonCode.h
        // engineClasses()), which is why their target parameter comes first.
        t.push_back({ "player.possess", "Player", true, {{"controller", P::Ref}, {"character", P::Ref}}, {}, "HE::api::player::possess",
            [](Ctx&, const VV& a){ player::possess(aR(a, 0), aR(a, 1)); return VV{}; } });
        t.push_back({ "player.unpossess", "Player", true, {{"controller", P::Ref}}, {}, "HE::api::player::unpossess",
            [](Ctx&, const VV& a){ player::unpossess(aR(a, 0)); return VV{}; } });
        t.push_back({ "player.possessed", "Player", false, {{"controller", P::Ref}}, {{"character", P::Ref}}, "HE::api::player::possessed",
            [](Ctx&, const VV& a){ return VV{ Value::ofRef(player::possessed(aR(a, 0))) }; } });
        t.push_back({ "player.controllerOf", "Player", false, {{"character", P::Ref}}, {{"controller", P::Ref}}, "HE::api::player::controllerOf",
            [](Ctx&, const VV& a){ return VV{ Value::ofRef(player::controllerOf(aR(a, 0))) }; } });
        t.push_back({ "player.controller", "Player", false, {}, {{"controller", P::Ref}}, "HE::api::player::controller",
            [](Ctx&, const VV&){ return VV{ Value::ofRef(player::controller()) }; } });
        t.push_back({ "player.character", "Player", false, {}, {{"character", P::Ref}}, "HE::api::player::character",
            [](Ctx&, const VV&){ return VV{ Value::ofRef(player::character()) }; } });

        // Input (pure getters; the app pushes the snapshot each frame)
        t.push_back({ "input.keyDown", "Input", false, {{"key", P::String}}, {{"down", P::Bool}}, "HE::api::input::keyDown",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(input::keyDown(aS(a, 0))) }; } });
        t.push_back({ "input.mouseButton", "Input", false, {{"button", P::Int}}, {{"down", P::Bool}}, "HE::api::input::mouseButton",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(input::mouseButton(aI(a, 0))) }; } });
        t.push_back({ "input.mousePosition", "Input", false, {}, {{"position", P::Vec2}}, "HE::api::input::mousePosition",
            [](Ctx&, const VV&){ return VV{ Value::ofVec2(input::mousePosition()) }; } });
        t.push_back({ "input.mouseDelta", "Input", false, {}, {{"delta", P::Vec2}}, "HE::api::input::mouseDelta",
            [](Ctx&, const VV&){ return VV{ Value::ofVec2(input::mouseDelta()) }; } });
        t.push_back({ "input.scrollDelta", "Input", false, {}, {{"scroll", P::Float}}, "HE::api::input::scrollDelta",
            [](Ctx&, const VV&){ return VV{ Value::ofFloat(input::scrollDelta()) }; } });
        // Gamepad (names are SDL's mapping strings, Xbox layout: buttons
        // "a"/"leftshoulder"/"dpup"/…, axes "leftx"/"righty"/"lefttrigger"/…).
        // Axes come deadzone-filtered from the app; a resting stick reads 0.0.
        t.push_back({ "input.gamepadConnected", "Input", false, {}, {{"connected", P::Bool}}, "HE::api::input::gamepadConnected",
            [](Ctx&, const VV&){ return VV{ Value::ofBool(input::gamepadConnected()) }; } });
        t.push_back({ "input.gamepadButton", "Input", false, {{"button", P::String}}, {{"down", P::Bool}}, "HE::api::input::gamepadButton",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(input::gamepadButton(aS(a, 0))) }; } });
        t.push_back({ "input.gamepadAxis", "Input", false, {{"axis", P::String}}, {{"value", P::Float}}, "HE::api::input::gamepadAxis",
            [](Ctx&, const VV& a){ return VV{ Value::ofFloat(input::gamepadAxis(aS(a, 0))) }; } });

        // Input routing — see input::Mode in the header for what each one gates.
        // Three rows rather than one taking a number: the palette entry has to
        // say which mode it sets, and the registry's one-callee-per-row rule
        // wants three functions anyway.
        t.push_back({ "input.setModeGameOnly", "Input", true, {}, {}, "HE::api::input::setModeGameOnly",
            [](Ctx&, const VV&){ input::setModeGameOnly(); return VV{}; } });
        t.push_back({ "input.setModeGameAndUI", "Input", true, {}, {}, "HE::api::input::setModeGameAndUI",
            [](Ctx&, const VV&){ input::setModeGameAndUI(); return VV{}; } });
        t.push_back({ "input.setModeUIOnly", "Input", true, {}, {}, "HE::api::input::setModeUIOnly",
            [](Ctx&, const VV&){ input::setModeUIOnly(); return VV{}; } });
        t.push_back({ "input.mode", "Input", false, {}, {{"mode", P::String}}, "HE::api::input::modeName",
            [](Ctx&, const VV&){ return VV{ Value::ofString(input::modeName()) }; } });

        // Entity queries
        t.push_back({ "entity.findByName", "Entity", false, {{"name", P::String}}, {{"entity", P::Int}}, "HE::api::entity::findByName",
            [](Ctx& c, const VV& a){ return VV{ Value::ofInt((int)entity::findByName(c, aS(a, 0))) }; } });
        t.push_back({ "entity.exists", "Entity", false, {{"entity", P::Int}}, {{"exists", P::Bool}}, "HE::api::entity::exists",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(entity::exists(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "entity.setVisible", "Entity", true, {{"entity", P::Int}, {"visible", P::Bool}}, {}, "HE::api::entity::setVisible",
            [](Ctx& c, const VV& a){ entity::setVisible(c, (Entity)aI(a, 0), aB(a, 1)); return VV{}; } });
        t.push_back({ "entity.saveState", "Entity", true, {{"entity", P::Int}}, {{"ok", P::Bool}}, "HE::api::entity::saveState",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(entity::saveState(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "entity.hasSavedState", "Entity", false, {{"entity", P::Int}}, {{"has", P::Bool}}, "HE::api::entity::hasSavedState",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(entity::hasSavedState(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "entity.applySavedState", "Entity", true, {{"entity", P::Int}}, {{"ok", P::Bool}}, "HE::api::entity::applySavedState",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(entity::applySavedState(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "entity.getVisible", "Entity", false, {{"entity", P::Int}}, {{"visible", P::Bool}}, "HE::api::entity::getVisible",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(entity::getVisible(c, (Entity)aI(a, 0))) }; } });

        // Camera (the world's main camera)
        t.push_back({ "camera.getPosition", "Camera", false, {}, {{"position", P::Vec3}}, "HE::api::camera::getPosition",
            [](Ctx& c, const VV&){ return VV{ v3(camera::getPosition(c)) }; } });
        t.push_back({ "camera.setPosition", "Camera", true, {{"position", P::Vec3}}, {}, "HE::api::camera::setPosition",
            [](Ctx& c, const VV& a){ camera::setPosition(c, aV3(a, 0)); return VV{}; } });
        t.push_back({ "camera.getRotation", "Camera", false, {}, {{"rotation", P::Vec3}}, "HE::api::camera::getRotation",
            [](Ctx& c, const VV&){ return VV{ v3(camera::getRotation(c)) }; } });
        t.push_back({ "camera.setRotation", "Camera", true, {{"rotation", P::Vec3}}, {}, "HE::api::camera::setRotation",
            [](Ctx& c, const VV& a){ camera::setRotation(c, aV3(a, 0)); return VV{}; } });
        t.push_back({ "camera.getFov", "Camera", false, {}, {{"degrees", P::Float}}, "HE::api::camera::getFov",
            [](Ctx& c, const VV&){ return VV{ Value::ofFloat(camera::getFov(c)) }; } });
        t.push_back({ "camera.setFov", "Camera", true, {{"degrees", P::Float}}, {}, "HE::api::camera::setFov",
            [](Ctx& c, const VV& a){ camera::setFov(c, aF(a, 0)); return VV{}; } });

        // Camera rig (CameraRigComponent on that same main camera)
        t.push_back({ "camera.setRigMode", "Camera", true, {{"mode", P::Int}}, {}, "HE::api::camera::setRigMode",
            [](Ctx& c, const VV& a){ camera::setRigMode(c, aI(a, 0)); return VV{}; } });
        t.push_back({ "camera.getRigMode", "Camera", false, {}, {{"mode", P::Int}}, "HE::api::camera::getRigMode",
            [](Ctx& c, const VV&){ return VV{ Value::ofInt(camera::getRigMode(c)) }; } });
        t.push_back({ "camera.setRigTarget", "Camera", true, {{"entity", P::Int}}, {}, "HE::api::camera::setRigTarget",
            [](Ctx& c, const VV& a){ camera::setRigTarget(c, aI(a, 0)); return VV{}; } });
        t.push_back({ "camera.setArmLength", "Camera", true, {{"length", P::Float}}, {}, "HE::api::camera::setArmLength",
            [](Ctx& c, const VV& a){ camera::setArmLength(c, aF(a, 0)); return VV{}; } });
        t.push_back({ "camera.getArmLength", "Camera", false, {}, {{"length", P::Float}}, "HE::api::camera::getArmLength",
            [](Ctx& c, const VV&){ return VV{ Value::ofFloat(camera::getArmLength(c)) }; } });
        t.push_back({ "camera.setTargetYawMode", "Camera", true, {{"mode", P::Int}}, {}, "HE::api::camera::setTargetYawMode",
            [](Ctx& c, const VV& a){ camera::setTargetYawMode(c, aI(a, 0)); return VV{}; } });
        t.push_back({ "camera.getTargetYawMode", "Camera", false, {}, {{"mode", P::Int}}, "HE::api::camera::getTargetYawMode",
            [](Ctx& c, const VV&){ return VV{ Value::ofInt(camera::getTargetYawMode(c)) }; } });
        t.push_back({ "camera.getRigYaw", "Camera", false, {}, {{"degrees", P::Float}}, "HE::api::camera::getRigYaw",
            [](Ctx& c, const VV&){ return VV{ Value::ofFloat(camera::getRigYaw(c)) }; } });
        t.push_back({ "camera.getRigPitch", "Camera", false, {}, {{"degrees", P::Float}}, "HE::api::camera::getRigPitch",
            [](Ctx& c, const VV&){ return VV{ Value::ofFloat(camera::getRigPitch(c)) }; } });
        t.push_back({ "camera.addYawPitch", "Camera", true, {{"deltaYaw", P::Float}, {"deltaPitch", P::Float}}, {}, "HE::api::camera::addYawPitch",
            [](Ctx& c, const VV& a){ camera::addYawPitch(c, aF(a, 0), aF(a, 1)); return VV{}; } });

        // Environment — EVERY EnvironmentComponent field, generated from the
        // HE_ENV_FIELDS_* X-lists in EngineApi.h (get = pure read, set = exec).
#define HE_ENV_ROW_FLOAT(m, Name, disp) \
        t.push_back({ "env.get" #Name, "Environment", false, {}, {{"value", P::Float}}, "HE::api::env::get" #Name, \
            [](Ctx& c, const VV&){ return VV{ Value::ofFloat(env::get##Name(c)) }; } }); \
        t.push_back({ "env.set" #Name, "Environment", true, {{"value", P::Float}}, {}, "HE::api::env::set" #Name, \
            [](Ctx& c, const VV& a){ env::set##Name(c, aF(a, 0)); return VV{}; } });
#define HE_ENV_ROW_BOOL(m, Name, disp) \
        t.push_back({ "env.get" #Name, "Environment", false, {}, {{"value", P::Bool}}, "HE::api::env::get" #Name, \
            [](Ctx& c, const VV&){ return VV{ Value::ofBool(env::get##Name(c)) }; } }); \
        t.push_back({ "env.set" #Name, "Environment", true, {{"value", P::Bool}}, {}, "HE::api::env::set" #Name, \
            [](Ctx& c, const VV& a){ env::set##Name(c, aB(a, 0)); return VV{}; } });
#define HE_ENV_ROW_INT(m, Name, disp) \
        t.push_back({ "env.get" #Name, "Environment", false, {}, {{"value", P::Int}}, "HE::api::env::get" #Name, \
            [](Ctx& c, const VV&){ return VV{ Value::ofInt(env::get##Name(c)) }; } }); \
        t.push_back({ "env.set" #Name, "Environment", true, {{"value", P::Int}}, {}, "HE::api::env::set" #Name, \
            [](Ctx& c, const VV& a){ env::set##Name(c, aI(a, 0)); return VV{}; } });
#define HE_ENV_ROW_COLOR(m, Name, disp) \
        t.push_back({ "env.get" #Name, "Environment", false, {}, {{"color", P::Color}}, "HE::api::env::get" #Name, \
            [](Ctx& c, const VV&){ return VV{ rgb(env::get##Name(c)) }; } }); \
        t.push_back({ "env.set" #Name, "Environment", true, {{"color", P::Color}}, {}, "HE::api::env::set" #Name, \
            [](Ctx& c, const VV& a){ env::set##Name(c, aV3(a, 0)); return VV{}; } });
        HE_ENV_FIELDS_FLOAT(HE_ENV_ROW_FLOAT)
        HE_ENV_FIELDS_BOOL(HE_ENV_ROW_BOOL)
        HE_ENV_FIELDS_INT(HE_ENV_ROW_INT)
        HE_ENV_FIELDS_COLOR(HE_ENV_ROW_COLOR)
#undef HE_ENV_ROW_FLOAT
#undef HE_ENV_ROW_BOOL
#undef HE_ENV_ROW_INT
#undef HE_ENV_ROW_COLOR

        // Audio
        t.push_back({ "audio.play", "Audio", true,
            {{"asset", P::String}, {"volume", P::Float}, {"pitch", P::Float}, {"loop", P::Bool}},
            {{"handle", P::Int}}, "HE::api::audio::play",
            [](Ctx& c, const VV& a){ return VV{ Value::ofInt(audio::play(c, aS(a, 0),
                a.size() > 1 ? aF(a, 1) : 1.0f, a.size() > 2 ? aF(a, 2) : 1.0f, aB(a, 3))) }; } });
        t.push_back({ "audio.playAt", "Audio", true,
            {{"asset", P::String}, {"position", P::Vec3}, {"volume", P::Float}, {"pitch", P::Float},
             {"loop", P::Bool}, {"minDist", P::Float}, {"maxDist", P::Float}},
            {{"handle", P::Int}}, "HE::api::audio::playAt",
            [](Ctx& c, const VV& a){ return VV{ Value::ofInt(audio::playAt(c, aS(a, 0), aV3(a, 1),
                a.size() > 2 ? aF(a, 2) : 1.0f, a.size() > 3 ? aF(a, 3) : 1.0f, aB(a, 4),
                a.size() > 5 ? aF(a, 5) : 1.0f, a.size() > 6 ? aF(a, 6) : 20.0f)) }; } });
        t.push_back({ "audio.stop", "Audio", true, {{"handle", P::Int}}, {}, "HE::api::audio::stop",
            [](Ctx& c, const VV& a){ audio::stop(c, aI(a, 0)); return VV{}; } });
        t.push_back({ "audio.stopAll", "Audio", true, {}, {}, "HE::api::audio::stopAll",
            [](Ctx& c, const VV&){ audio::stopAll(c); return VV{}; } });
        t.push_back({ "audio.isPlaying", "Audio", false, {{"handle", P::Int}}, {{"playing", P::Bool}}, "HE::api::audio::isPlaying",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(audio::isPlaying(c, aI(a, 0))) }; } });
        t.push_back({ "audio.setBusVolume", "Audio", true, {{"bus", P::String}, {"volume", P::Float}}, {}, "HE::api::audio::setBusVolume",
            [](Ctx& c, const VV& a){ audio::setBusVolume(c, aS(a, 0), aF(a, 1)); return VV{}; } });
        t.push_back({ "audio.setSoundPosition", "Audio", true, {{"handle", P::Int}, {"position", P::Vec3}}, {}, "HE::api::audio::setSoundPosition",
            [](Ctx& c, const VV& a){ audio::setSoundPosition(c, aI(a, 0), aV3(a, 1)); return VV{}; } });

        // Debug draw (timed world-space primitives; drained by the app per frame)
        t.push_back({ "debug.line", "Debug", true,
            {{"from", P::Color}, {"to", P::Color}, {"color", P::Color}, {"seconds", P::Float}}, {}, "HE::api::debug::line",
            [](Ctx&, const VV& a){ debug::line(aV3(a, 0), aV3(a, 1),
                a.size() > 2 ? aV3(a, 2) : glm::vec3(1.0f, 1.0f, 0.0f), aF(a, 3)); return VV{}; } });
        t.push_back({ "debug.sphere", "Debug", true,
            {{"center", P::Color}, {"radius", P::Float}, {"color", P::Color}, {"seconds", P::Float}}, {}, "HE::api::debug::sphere",
            [](Ctx&, const VV& a){ debug::sphere(aV3(a, 0), a.size() > 1 ? aF(a, 1) : 1.0f,
                a.size() > 2 ? aV3(a, 2) : glm::vec3(1.0f, 1.0f, 0.0f), aF(a, 3)); return VV{}; } });
        t.push_back({ "debug.box", "Debug", true,
            {{"min", P::Color}, {"max", P::Color}, {"color", P::Color}, {"seconds", P::Float}}, {}, "HE::api::debug::box",
            [](Ctx&, const VV& a){ debug::box(aV3(a, 0), aV3(a, 1),
                a.size() > 2 ? aV3(a, 2) : glm::vec3(1.0f, 1.0f, 0.0f), aF(a, 3)); return VV{}; } });
        t.push_back({ "debug.clear", "Debug", true, {}, {}, "HE::api::debug::clear",
            [](Ctx&, const VV&){ debug::clear(); return VV{}; } });

        // Sandboxed file I/O
        t.push_back({ "fs.writeText", "File", true, {{"path", P::String}, {"text", P::String}}, {{"ok", P::Bool}}, "HE::api::fs::writeText",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(fs::writeText(aS(a, 0), aS(a, 1))) }; } });
        t.push_back({ "fs.readText", "File", true, {{"path", P::String}}, {{"text", P::String}}, "HE::api::fs::readText",
            [](Ctx&, const VV& a){ return VV{ Value::ofString(fs::readText(aS(a, 0))) }; } });
        t.push_back({ "fs.exists", "File", false, {{"path", P::String}}, {{"exists", P::Bool}}, "HE::api::fs::exists",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(fs::exists(aS(a, 0))) }; } });
        t.push_back({ "fs.remove", "File", true, {{"path", P::String}}, {{"ok", P::Bool}}, "HE::api::fs::remove",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(fs::remove(aS(a, 0))) }; } });
        t.push_back({ "fs.makeDir", "File", true, {{"path", P::String}}, {{"ok", P::Bool}}, "HE::api::fs::makeDir",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(fs::makeDir(aS(a, 0))) }; } });

        // Savegames: ONE active template-shaped document (see the header block).
        // create/load resolve the SaveGameTemplate through the Ctx's content
        // manager; field access validates against it and fails LOUD.
        t.push_back({ "save.create", "Save", true, {{"id", P::String}}, {{"ok", P::Bool}}, "HE::api::save::create",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(save::create(aS(a, 0), c.content)) }; } });
        t.push_back({ "save.load", "Save", true, {{"id", P::String}}, {{"ok", P::Bool}}, "HE::api::save::load",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(save::load(aS(a, 0), c.content)) }; } });
        t.push_back({ "save.write", "Save", true, {}, {{"ok", P::Bool}}, "HE::api::save::write",
            [](Ctx&, const VV&){ return VV{ Value::ofBool(save::write()) }; } });
        t.push_back({ "save.close", "Save", true, {}, {}, "HE::api::save::close",
            [](Ctx&, const VV&){ save::close(); return VV{}; } });
        t.push_back({ "save.activeId", "Save", false, {}, {{"id", P::String}}, "HE::api::save::activeId",
            [](Ctx&, const VV&){ return VV{ Value::ofString(save::activeId()) }; } });
        t.push_back({ "save.list", "Save", false, {}, {{"ids", P::String, /*isArray=*/true}}, "HE::api::save::list",
            [](Ctx&, const VV&){
                Value arr; arr.isArray = true; arr.type = P::String;
                for (auto& id : save::list()) arr.items.push_back(Value::ofString(std::move(id)));
                return VV{ std::move(arr) }; } });
        t.push_back({ "save.exists", "Save", false, {{"id", P::String}}, {{"exists", P::Bool}}, "HE::api::save::exists",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(save::exists(aS(a, 0))) }; } });
        t.push_back({ "save.delete", "Save", true, {{"id", P::String}}, {{"ok", P::Bool}}, "HE::api::save::remove",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(save::remove(aS(a, 0))) }; } });
        t.push_back({ "save.fields", "Save", false, {}, {{"names", P::String, /*isArray=*/true}}, "HE::api::save::fields",
            [](Ctx&, const VV&){
                Value arr; arr.isArray = true; arr.type = P::String;
                for (auto& n : save::fields()) arr.items.push_back(Value::ofString(std::move(n)));
                return VV{ std::move(arr) }; } });
        t.push_back({ "save.setNumber", "Save", true, {{"field", P::String}, {"value", P::Float}}, {{"ok", P::Bool}}, "HE::api::save::setNumber",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(save::setNumber(aS(a, 0), aF(a, 1))) }; } });
        t.push_back({ "save.getNumber", "Save", false, {{"field", P::String}, {"default", P::Float}}, {{"value", P::Float}}, "HE::api::save::getNumber",
            [](Ctx&, const VV& a){ return VV{ Value::ofFloat(save::getNumber(aS(a, 0), aF(a, 1))) }; } });
        t.push_back({ "save.setString", "Save", true, {{"field", P::String}, {"value", P::String}}, {{"ok", P::Bool}}, "HE::api::save::setString",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(save::setString(aS(a, 0), aS(a, 1))) }; } });
        t.push_back({ "save.getString", "Save", false, {{"field", P::String}, {"default", P::String}}, {{"value", P::String}}, "HE::api::save::getString",
            [](Ctx&, const VV& a){ return VV{ Value::ofString(save::getString(aS(a, 0), aS(a, 1))) }; } });
        t.push_back({ "save.setBool", "Save", true, {{"field", P::String}, {"value", P::Bool}}, {{"ok", P::Bool}}, "HE::api::save::setBool",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(save::setBool(aS(a, 0), aB(a, 1))) }; } });
        t.push_back({ "save.getBool", "Save", false, {{"field", P::String}, {"default", P::Bool}}, {{"value", P::Bool}}, "HE::api::save::getBool",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(save::getBool(aS(a, 0), aB(a, 1))) }; } });
        t.push_back({ "save.setStruct", "Save", true, {{"field", P::String}, {"value", P::Struct}}, {{"ok", P::Bool}}, "HE::api::save::setStructV",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(save::setStructV(aS(a, 0), a.size() > 1 ? a[1] : Value{})) }; } });
        t.push_back({ "save.getStruct", "Save", false, {{"field", P::String}}, {{"value", P::Struct}}, "HE::api::save::getStructV",
            [](Ctx&, const VV& a){ return VV{ save::getStructV(aS(a, 0)) }; } });

        // Scene transitions (deferred; the app runtime executes the requests).
        // "hidden": unwired = false = present immediately (today's behavior); true
        // defers presentation — a hidden zone loads invisible until Show Zone, a
        // hidden level PRELOADS and swaps in on Activate.
        t.push_back({ "scene.load", "Scene", true, {{"scene", P::String}, {"hidden", P::Bool}}, {}, "HE::api::scene::load",
            [](Ctx&, const VV& a){ scene::load(aS(a, 0), aB(a, 1)); return VV{}; } });
        t.push_back({ "scene.loadAdditive", "Scene", true,
            {{"scene", P::String}, {"hidden", P::Bool}, {"position", P::Color}}, {{"zone", P::Int}}, "HE::api::scene::loadAdditive",
            [](Ctx&, const VV& a){ return VV{ Value::ofInt(scene::loadAdditive(aS(a, 0), aB(a, 1), aV3(a, 2))) }; } });
        t.push_back({ "scene.unloadZone", "Scene", true, {{"zone", P::Int}}, {}, "HE::api::scene::unloadZone",
            [](Ctx&, const VV& a){ scene::unloadZone(aI(a, 0)); return VV{}; } });
        t.push_back({ "scene.activate", "Scene", true, {}, {}, "HE::api::scene::activate",
            [](Ctx&, const VV&){ scene::activate(); return VV{}; } });
        t.push_back({ "scene.hasPendingLevel", "Scene", false, {}, {{"pending", P::Bool}}, "HE::api::scene::hasPendingLevel",
            [](Ctx&, const VV&){ return VV{ Value::ofBool(scene::hasPendingLevel()) }; } });
        // Show/Hide/Move queue as requests so they order correctly with a Load
        // Additive in the SAME exec chain (the load itself is deferred).
        t.push_back({ "scene.showZone", "Scene", true, {{"zone", P::Int}}, {}, "HE::api::scene::showZone",
            [](Ctx&, const VV& a){ scene::showZone(aI(a, 0)); return VV{}; } });
        t.push_back({ "scene.hideZone", "Scene", true, {{"zone", P::Int}}, {}, "HE::api::scene::hideZone",
            [](Ctx&, const VV& a){ scene::hideZone(aI(a, 0)); return VV{}; } });
        t.push_back({ "scene.zonePosition", "Scene", false, {{"zone", P::Int}}, {{"position", P::Vec3}}, "HE::api::scene::zonePosition",
            [](Ctx& c, const VV& a){ return VV{ v3(scene::zonePosition(c, aI(a, 0))) }; } });
        t.push_back({ "scene.setZonePosition", "Scene", true, {{"zone", P::Int}, {"position", P::Vec3}}, {}, "HE::api::scene::requestZonePosition",
            [](Ctx&, const VV& a){ scene::requestZonePosition(aI(a, 0), aV3(a, 1)); return VV{}; } });
        t.push_back({ "scene.zoneScene", "Scene", false, {{"zone", P::Int}}, {{"scene", P::String}}, "HE::api::scene::zoneScene",
            [](Ctx&, const VV& a){ return VV{ Value::ofString(scene::zoneScene(aI(a, 0))) }; } });
        t.push_back({ "scene.loadedZones", "Scene", false, {}, {{"zones", P::Int, /*isArray=*/true}}, "HE::api::scene::loadedZones",
            [](Ctx&, const VV&){
                Value arr; arr.isArray = true; arr.type = P::Int;
                for (int z : scene::loadedZones()) arr.items.push_back(Value::ofInt(z));
                return VV{ std::move(arr) }; } });
        t.push_back({ "scene.available", "Scene", false, {}, {{"scenes", P::String, /*isArray=*/true}}, "HE::api::scene::availableScenes",
            [](Ctx& c, const VV&){
                Value arr; arr.isArray = true; arr.type = P::String;
                for (auto& s : scene::availableScenes(c)) arr.items.push_back(Value::ofString(std::move(s)));
                return VV{ std::move(arr) }; } });

        // String library (pure)
        t.push_back({ "string.length", "String", false, {{"s", P::String}}, {{"length", P::Int}}, "HE::api::str::length",
            [](Ctx&, const VV& a){ return VV{ Value::ofInt(str::length(aS(a, 0))) }; } });
        t.push_back({ "string.substring", "String", false,
            {{"s", P::String}, {"start", P::Int}, {"count", P::Int}}, {{"result", P::String}}, "HE::api::str::substring",
            [](Ctx&, const VV& a){ return VV{ Value::ofString(str::substring(aS(a, 0), aI(a, 1), aI(a, 2))) }; } });
        t.push_back({ "string.contains", "String", false,
            {{"s", P::String}, {"needle", P::String}}, {{"contains", P::Bool}}, "HE::api::str::contains",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(str::contains(aS(a, 0), aS(a, 1))) }; } });
        t.push_back({ "string.find", "String", false,
            {{"s", P::String}, {"needle", P::String}}, {{"index", P::Int}}, "HE::api::str::find",
            [](Ctx&, const VV& a){ return VV{ Value::ofInt(str::find(aS(a, 0), aS(a, 1))) }; } });
        t.push_back({ "string.replace", "String", false,
            {{"s", P::String}, {"from", P::String}, {"to", P::String}}, {{"result", P::String}}, "HE::api::str::replace",
            [](Ctx&, const VV& a){ return VV{ Value::ofString(str::replace(aS(a, 0), aS(a, 1), aS(a, 2))) }; } });
        t.push_back({ "string.toUpper", "String", false, {{"s", P::String}}, {{"result", P::String}}, "HE::api::str::toUpper",
            [](Ctx&, const VV& a){ return VV{ Value::ofString(str::toUpper(aS(a, 0))) }; } });
        t.push_back({ "string.toLower", "String", false, {{"s", P::String}}, {{"result", P::String}}, "HE::api::str::toLower",
            [](Ctx&, const VV& a){ return VV{ Value::ofString(str::toLower(aS(a, 0))) }; } });
        t.push_back({ "string.trim", "String", false, {{"s", P::String}}, {{"result", P::String}}, "HE::api::str::trim",
            [](Ctx&, const VV& a){ return VV{ Value::ofString(str::trim(aS(a, 0))) }; } });
        t.push_back({ "string.startsWith", "String", false,
            {{"s", P::String}, {"prefix", P::String}}, {{"result", P::Bool}}, "HE::api::str::startsWith",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(str::startsWith(aS(a, 0), aS(a, 1))) }; } });
        t.push_back({ "string.endsWith", "String", false,
            {{"s", P::String}, {"suffix", P::String}}, {{"result", P::Bool}}, "HE::api::str::endsWith",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(str::endsWith(aS(a, 0), aS(a, 1))) }; } });
        t.push_back({ "string.toNumber", "String", false, {{"s", P::String}}, {{"number", P::Float}}, "HE::api::str::toNumber",
            [](Ctx&, const VV& a){ return VV{ Value::ofFloat(str::toNumber(aS(a, 0))) }; } });

        // ── Readable editor names (post-pass; id stays the stable identifier) ──
        // What menus and node titles show — "Sine" under Math, not "math.sin".
        static const std::pair<const char*, const char*> kNames[] = {
            { "log", "Log" },
            { "entity.getName", "Get Name" },       { "entity.spawn", "Spawn Entity" },
            { "entity.destroy", "Destroy Entity" }, { "entity.distance", "Distance Between" },
            { "entity.self", "Get Owning Entity" }, { "entity.owned", "Get Entity Of" },
            { "entity.instance", "Get Object On Entity" },
            { "entity.selfObject", "Get Owning Object" },
            { "transform.getPosition", "Get Position" }, { "transform.setPosition", "Set Position" },
            { "transform.getWorldPosition", "Get World Position" },
            { "transform.setWorldPosition", "Set World Position" },
            { "transform.getRotation", "Get Rotation" }, { "transform.setRotation", "Set Rotation" },
            { "transform.getScale", "Get Scale" },       { "transform.setScale", "Set Scale" },
            { "animator.setParam", "Set Animator Param" }, { "animator.getParam", "Get Animator Param" },
            { "animator.getState", "Get Animator State" },
            { "movement.speed", "Get Speed" }, { "movement.verticalSpeed", "Get Vertical Speed" },
            { "movement.isGrounded", "Is Grounded" }, { "movement.velocity", "Get Velocity" },
            { "movement.forwardAmount", "Get Forward Amount" },
            { "movement.rightAmount", "Get Right Amount" },
            { "locomotion.move", "Move" }, { "locomotion.look", "Look" },
            { "locomotion.setMaxSpeed", "Set Max Speed" },
            { "locomotion.setOrientToMovement", "Set Orient To Movement" },
            { "physics.raycast", "Raycast" }, { "physics.setVelocity", "Set Velocity" },
            { "physics.isGrounded", "Is Grounded" },
            { "physics.sphereCast", "Sphere Cast" },   { "physics.overlapSphere", "Overlap Sphere" },
            { "physics.addForce", "Add Force" },       { "physics.addImpulse", "Add Impulse" },
            { "physics.addTorque", "Add Torque" },
            { "physics.setGravity", "Set Gravity" },   { "physics.getGravity", "Get Gravity" },
            // Suffixed like "Length (Vec2)"/"Length (Vec3)": Movement already
            // owns a "Get Velocity", and the drag-off menu lists the registry
            // flat, where two identically named rows are a coin toss.
            { "physics.getVelocity", "Get Velocity (Physics)" },
            { "material.getParam", "Get Material Param" }, { "material.setParam", "Set Material Param" },
            { "ui.getText", "Get UI Text" },        { "ui.setText", "Set UI Text" },
            { "ui.getColor", "Get UI Color" },      { "ui.setColor", "Set UI Color" },
            { "ui.getVisible", "Get UI Visible" },  { "ui.setVisible", "Set UI Visible" },
            { "ui.getPosition", "Get UI Position" },{ "ui.setPosition", "Set UI Position" },
            { "ui.getSize", "Get UI Size" },        { "ui.setSize", "Set UI Size" },
            { "ui.setMaterialParam", "Set UI Material Param" },
            { "ui.pointerOverUI", "Is Pointer Over UI" },
            { "widget.setZOrder", "Set Widget Z-Order" }, { "widget.isVisible", "Is Widget Visible" },
            { "widget.callFunction", "Call Widget Function" },
            { "widget.addChild", "Add Widget Child" },
            { "widget.removeChild", "Remove Widget Child" },
            { "widget.clearChildren", "Clear Widget Children" },
            { "theme.set", "Set Theme" }, { "theme.setMode", "Set Theme Mode" },
            { "theme.getMode", "Get Theme Mode" },
            { "theme.getPreference", "Get Theme Preference" },
            { "cursor.setVisible", "Set Cursor Visible" },
            { "app.quit", "Quit Game" },
            { "app.setTitle", "Set Window Title" }, { "app.setSize", "Set Window Size" },
            { "app.size", "Get Window Size" },      { "app.requestRedraw", "Request Redraw" },
            { "clipboard.getText", "Get Clipboard Text" },
            { "clipboard.setText", "Set Clipboard Text" },
            { "clipboard.hasText", "Has Clipboard Text" },
            { "dialog.message", "Show Message Dialog" },
            { "dialog.confirm", "Show Confirm Dialog" },
            { "json.getString", "JSON Get String" }, { "json.getNumber", "JSON Get Number" },
            { "json.getBool", "JSON Get Bool" },     { "json.has", "JSON Has" },
            { "json.count", "JSON Array Count" },
            { "json.setString", "JSON Set String" }, { "json.setNumber", "JSON Set Number" },
            { "json.setBool", "JSON Set Bool" },
            { "prefs.getString", "Get Pref String" }, { "prefs.getNumber", "Get Pref Number" },
            { "prefs.getBool", "Get Pref Bool" },     { "prefs.setString", "Set Pref String" },
            { "prefs.setNumber", "Set Pref Number" }, { "prefs.setBool", "Set Pref Bool" },
            { "prefs.has", "Has Pref" }, { "prefs.remove", "Remove Pref" },
            { "prefs.clear", "Clear Prefs" },
            { "datetime.now", "Now" }, { "datetime.format", "Format Time" },
            { "datetime.year", "Year" }, { "datetime.month", "Month" }, { "datetime.day", "Day" },
            { "datetime.hour", "Hour" }, { "datetime.minute", "Minute" },
            { "datetime.second", "Second" }, { "datetime.weekday", "Weekday" },
            { "math.sin", "Sine" },   { "math.cos", "Cosine" }, { "math.tan", "Tangent" },
            { "math.sqrt", "Square Root" }, { "math.abs", "Absolute" },
            { "math.floor", "Floor" }, { "math.ceil", "Ceil" }, { "math.round", "Round" },
            { "math.sign", "Sign" },   { "math.pow", "Power" }, { "math.mod", "Modulo" },
            // Spelled out both ways round: "Radians" alone says nothing about
            // which direction it converts when you meet it in the add menu.
            { "math.radians", "Degrees to Radians" }, { "math.degrees", "Radians to Degrees" },
            { "math.atan2", "Atan2" }, { "math.min", "Min" },   { "math.max", "Max" },
            { "math.clamp", "Clamp" }, { "math.lerp", "Lerp" },
            { "math.length", "Length (Vec2)" }, { "math.distance", "Distance (Vec2)" },
            { "math.length3", "Length (Vec3)" }, { "math.distance3", "Distance (Vec3)" },
            { "math.normalize3", "Normalize (Vec3)" }, { "math.dot3", "Dot Product" },
            { "math.cross", "Cross Product" },
            { "random.seed", "Seed Random" },   { "random.value", "Random Value" },
            { "random.range", "Random Range" }, { "random.rangeInt", "Random Range (Int)" },
            { "random.chance", "Random Chance" },
            { "time.deltaTime", "Delta Time" }, { "time.elapsed", "Elapsed Time" },
            { "time.frameCount", "Frame Count" },
            { "time.setTimeScale", "Set Time Scale" }, { "time.timeScale", "Get Time Scale" },
            { "time.unscaledDeltaTime", "Unscaled Delta Time" },
            { "player.possess", "Possess" },          { "player.unpossess", "Un Possess" },
            { "player.possessed", "Get Possessed Character" },
            { "player.controllerOf", "Get Controller" },
            { "player.controller", "Get Player Controller" },
            { "player.character", "Get Player Character" },
            { "input.keyDown", "Key Down" },          { "input.mouseButton", "Mouse Button" },
            { "input.mousePosition", "Mouse Position" }, { "input.mouseDelta", "Mouse Delta" },
            { "input.scrollDelta", "Scroll Delta" },
            { "input.gamepadConnected", "Gamepad Connected" },
            { "input.gamepadButton", "Gamepad Button" },
            { "input.gamepadAxis", "Gamepad Axis" },
            { "input.setModeGameOnly", "Set Input Mode: Game Only" },
            { "input.setModeGameAndUI", "Set Input Mode: Game and UI" },
            { "input.setModeUIOnly", "Set Input Mode: UI Only" },
            { "input.mode", "Input Mode" },
            { "entity.findByName", "Find By Name" },  { "entity.exists", "Entity Exists" },
            { "entity.setVisible", "Set Entity Visible" }, { "entity.getVisible", "Get Entity Visible" },
            { "entity.saveState", "Save Entity State" },
            { "entity.hasSavedState", "Has Saved State" },
            { "entity.applySavedState", "Apply Saved State" },
            { "camera.getPosition", "Get Camera Position" }, { "camera.setPosition", "Set Camera Position" },
            { "camera.getRotation", "Get Camera Rotation" }, { "camera.setRotation", "Set Camera Rotation" },
            { "camera.getFov", "Get Camera FOV" },           { "camera.setFov", "Set Camera FOV" },
            { "camera.setRigMode", "Set Camera Mode" },      { "camera.getRigMode", "Get Camera Mode" },
            { "camera.setRigTarget", "Set Camera Target" },
            { "camera.setArmLength", "Set Camera Distance" },{ "camera.getArmLength", "Get Camera Distance" },
            { "camera.setTargetYawMode", "Set Target Rotation Mode" },
            { "camera.getTargetYawMode", "Get Target Rotation Mode" },
            { "camera.getRigYaw", "Get Camera Yaw" },        { "camera.getRigPitch", "Get Camera Pitch" },
            { "camera.addYawPitch", "Turn Camera" },
            // Environment display names — generated from the same X-lists as
            // the functions ("Get "/"Set " + the display string per field).
#define HE_ENV_NAME_ROW(m, Name, disp) { "env.get" #Name, "Get " disp }, { "env.set" #Name, "Set " disp },
            HE_ENV_FIELDS_FLOAT(HE_ENV_NAME_ROW)
            HE_ENV_FIELDS_BOOL(HE_ENV_NAME_ROW)
            HE_ENV_FIELDS_INT(HE_ENV_NAME_ROW)
            HE_ENV_FIELDS_COLOR(HE_ENV_NAME_ROW)
#undef HE_ENV_NAME_ROW
            { "audio.play", "Play Sound" },        { "audio.playAt", "Play Sound At" },
            { "audio.stop", "Stop Sound" },        { "audio.stopAll", "Stop All Sounds" },
            { "audio.isPlaying", "Is Sound Playing" }, { "audio.setBusVolume", "Set Bus Volume" },
            { "string.length", "String Length" },  { "string.substring", "Substring" },
            { "string.contains", "String Contains" }, { "string.find", "String Find" },
            { "string.replace", "String Replace" },   { "string.toUpper", "To Upper" },
            { "string.toLower", "To Lower" },         { "string.trim", "Trim" },
            { "string.startsWith", "Starts With" },   { "string.endsWith", "Ends With" },
            { "string.toNumber", "To Number" },
            { "audio.setSoundPosition", "Set Sound Position" },
            { "debug.line", "Draw Debug Line" },   { "debug.sphere", "Draw Debug Sphere" },
            { "debug.box", "Draw Debug Box" },     { "debug.clear", "Clear Debug Draw" },
            { "fs.writeText", "Write Text File" }, { "fs.readText", "Read Text File" },
            { "fs.exists", "File Exists" },        { "fs.remove", "Delete File" },
            { "fs.makeDir", "Make Directory" },
            { "save.create", "Create Save" },        { "save.load", "Load Save" },
            { "save.write", "Write Save" },          { "save.close", "Close Save" },
            { "save.activeId", "Active Save Id" },   { "save.list", "List Saves" },
            { "save.exists", "Save Exists" },        { "save.delete", "Delete Save" },
            { "save.fields", "Save Fields" },
            { "save.setNumber", "Save Set Number" }, { "save.getNumber", "Save Get Number" },
            { "save.setString", "Save Set String" }, { "save.getString", "Save Get String" },
            { "save.setBool", "Save Set Bool" },     { "save.getBool", "Save Get Bool" },
            { "save.setStruct", "Save Set Struct" }, { "save.getStruct", "Save Get Struct" },
            { "scene.load", "Load Scene" },          { "scene.loadAdditive", "Load Scene Additive" },
            { "scene.unloadZone", "Unload Zone" },   { "scene.activate", "Activate Loaded Scene" },
            { "scene.hasPendingLevel", "Has Pending Scene" },
            { "scene.showZone", "Show Zone" },       { "scene.hideZone", "Hide Zone" },
            { "scene.zonePosition", "Get Zone Position" },
            { "scene.setZonePosition", "Set Zone Position" },
            { "scene.zoneScene", "Get Zone Scene" }, { "scene.loadedZones", "Loaded Zones" },
            { "scene.available", "Available Scenes" },
        };
        for (auto& fn : t)
        {
            fn.displayName = fn.id; // fallback: never null, worst case the id shows
            for (const auto& [id, name] : kNames)
                if (std::strcmp(fn.id, id) == 0) { fn.displayName = name; break; }
        }

        return t;
    }();
    return table;
}

bool isScriptGroup(std::string_view group)
{
    static constexpr std::string_view kGroups[] = { "math", "random", "time", "input",
                                                    "string", "camera", "env", "entity", "audio",
                                                    "debug", "fs", "save", "scene", "player",
                                                    "animator", "movement", "locomotion",
                                                    // "physics" carries the whole force/overlap
                                                    // surface, and the flat bindings only ever
                                                    // got raycast/setVelocity/isGrounded — so
                                                    // this is the only door a text script has
                                                    // to it at all.
                                                    "physics",
                                                    // "ui" adds no capability the flat
                                                    // setUIText/… bindings lack — except
                                                    // ui.pointerOverUI, which has no flat twin
                                                    // and is exactly what a script needs to
                                                    // stop a menu click reaching the game.
                                                    "ui", "app" };
    for (std::string_view g : kGroups) if (group == g) return true;
    return false;
}

bool groupAllowed(std::string_view apiId, const std::vector<const char*>& allowed)
{
    if (allowed.empty()) return true;
    // The group is the id up to the first dot. Comparing the whole segment, not
    // a prefix — "entityfoo.bar" must not pass as "entity".
    const size_t dot = apiId.find('.');
    const std::string_view group = apiId.substr(0, dot == std::string_view::npos ? apiId.size() : dot);
    for (const char* g : allowed) if (g && group == g) return true;
    return false;
}

const ApiFn* find(const std::string& id)
{
    // Every script call (Lua, Python, the HorizonCode interpreter) and every
    // codegen validation pass lands here, so this must not be a linear walk over
    // ~140 rows. The index is built once from registry(), whose vector lives for
    // the process and never reallocates after construction — so the ApiFn* stay
    // valid, and string_view keys can borrow the rows' string-literal ids instead
    // of copying them. Function-local static ⇒ the build is thread-safe, which it
    // needs to be: codegen runs on a worker thread while scripts run on the main one.
    // Duplicate ids (should not exist) resolve to the first row, as the scan did.
    static const std::unordered_map<std::string_view, const ApiFn*> index = []
    {
        const std::vector<ApiFn>& table = registry();
        std::unordered_map<std::string_view, const ApiFn*> m;
        m.reserve(table.size());
        for (const ApiFn& fn : table) m.emplace(fn.id, &fn);
        return m;
    }();

    const auto it = index.find(std::string_view(id));
    return it == index.end() ? nullptr : it->second;
}

} // namespace HE::api

// ── C++ GameLogic services (HorizonGameServices.h) ───────────────────────────
// Every bridge below is a captureless lambda decaying to a C function pointer;
// `host` carries the SaveServicesBinding. Entity calls resolve the world PER
// CALL through the binding so a scene switch never leaves a stale pointer in
// the game library's hands.

namespace HE::api {
namespace {

Ctx bindingCtx(void* host)
{
    auto* b = static_cast<SaveServicesBinding*>(host);
    Ctx c;
    c.world   = b && b->world ? b->world() : nullptr;
    c.content = b ? b->content : nullptr;
    return c;
}
// (host, buf, cap) string return: copy up to cap-1 bytes, always NUL-terminate,
// report the FULL length so the caller can grow and retry.
int copyOut(const std::string& s, char* buf, int cap)
{
    if (buf && cap > 0)
    {
        const int n = (int)std::min<size_t>(s.size(), (size_t)cap - 1);
        std::memcpy(buf, s.data(), (size_t)n);
        buf[n] = '\0';
    }
    return (int)s.size();
}
std::string joinLines(const std::vector<std::string>& v)
{
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) { if (i) out += '\n'; out += v[i]; }
    return out;
}

} // namespace

void fillSaveServices(::HeSaveServices& out, SaveServicesBinding* binding)
{
    out = {};
    out.abiVersion = HE_SAVE_ABI_VERSION;
    out.host       = binding;

    out.create = [](void* h, const char* id) {
        auto* b = static_cast<SaveServicesBinding*>(h);
        return save::create(id ? id : "", b ? b->content : nullptr); };
    out.load = [](void* h, const char* id) {
        auto* b = static_cast<SaveServicesBinding*>(h);
        return save::load(id ? id : "", b ? b->content : nullptr); };
    out.write      = [](void*) { return save::write(); };
    out.close      = [](void*) { save::close(); };
    out.exists     = [](void*, const char* id) { return save::exists(id ? id : ""); };
    out.removeSave = [](void*, const char* id) { return save::remove(id ? id : ""); };
    out.activeId   = [](void*, char* buf, int cap) { return copyOut(save::activeId(), buf, cap); };
    out.listIds    = [](void*, char* buf, int cap) { return copyOut(joinLines(save::list()), buf, cap); };
    out.fields     = [](void*, char* buf, int cap) { return copyOut(joinLines(save::fields()), buf, cap); };

    out.setNumber = [](void*, const char* f, float v) { return save::setNumber(f ? f : "", v); };
    out.getNumber = [](void*, const char* f, float d) { return save::getNumber(f ? f : "", d); };
    out.setString = [](void*, const char* f, const char* v) { return save::setString(f ? f : "", v ? v : ""); };
    out.getString = [](void*, const char* f, char* buf, int cap) {
        return copyOut(save::getString(f ? f : "", ""), buf, cap); };
    out.setBool = [](void*, const char* f, bool v) { return save::setBool(f ? f : "", v); };
    out.getBool = [](void*, const char* f, bool d) { return save::getBool(f ? f : "", d); };
    out.setStructJson = [](void*, const char* f, const char* json) {
        return save::setStructJson(f ? f : "", json ? json : ""); };
    out.getStructJson = [](void*, const char* f, char* buf, int cap) {
        return copyOut(save::getStructJson(f ? f : ""), buf, cap); };

    out.findEntityByName = [](void* h, const char* name) {
        Ctx c = bindingCtx(h);
        return (uint32_t)entity::findByName(c, name ? name : ""); };
    out.entitySaveState = [](void* h, uint32_t e) {
        Ctx c = bindingCtx(h);
        return entity::saveState(c, e); };
    out.entityHasSavedState = [](void* h, uint32_t e) {
        Ctx c = bindingCtx(h);
        return entity::hasSavedState(c, e); };
    out.entityApplySavedState = [](void* h, uint32_t e) {
        Ctx c = bindingCtx(h);
        return entity::applySavedState(c, e); };
}

} // namespace HE::api
