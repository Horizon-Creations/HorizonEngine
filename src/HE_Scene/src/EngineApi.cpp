#include "HorizonScene/EngineApi.h"
#include "HorizonScene/ScriptApi.h"
#include <cmath>
#include <random>
#include <utility>

// The engine surface (transform/physics/material/ui/widget/cursor/entity) is a
// thin promotion of the language-neutral ScriptApi — same behavior, now bundled
// behind an explicit Ctx and grouped by subsystem, so one call reads the same in
// every frontend. The math group is native (pure C++). A later phase inverts the
// dependency (ScriptApi becomes a shim over HE::api) and adds the subsystems that
// need new Ctx providers: audio, input, camera, time, scene, fs/save.

namespace HE::api {

// ── Debug ────────────────────────────────────────────────────────────────────
void log(Ctx&, const std::string& message) { ScriptApi::log(message.c_str()); }

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
} // namespace entity

// ── Transform ────────────────────────────────────────────────────────────────
namespace transform {
glm::vec3 getPosition(Ctx& c, Entity e)                    { return c.world ? ScriptApi::getPosition(*c.world, e) : glm::vec3(0.0f); }
void      setPosition(Ctx& c, Entity e, const glm::vec3& p){ if (c.world) ScriptApi::setPosition(*c.world, e, p); }
glm::vec3 getRotation(Ctx& c, Entity e)                    { return c.world ? ScriptApi::getRotation(*c.world, e) : glm::vec3(0.0f); }
void      setRotation(Ctx& c, Entity e, const glm::vec3& r){ if (c.world) ScriptApi::setRotation(*c.world, e, r); }
glm::vec3 getScale(Ctx& c, Entity e)                       { return c.world ? ScriptApi::getScale(*c.world, e) : glm::vec3(1.0f); }
void      setScale(Ctx& c, Entity e, const glm::vec3& s)   { if (c.world) ScriptApi::setScale(*c.world, e, s); }
} // namespace transform

// ── Physics ──────────────────────────────────────────────────────────────────
namespace physics {
RaycastHit raycast(Ctx& c, const glm::vec3& o, const glm::vec3& d, float maxDist)
{
    const auto r = ScriptApi::raycast(c.physics, o, d, maxDist);
    return { r.hit, r.entityId, r.point, r.normal, r.distance };
}
void setVelocity(Ctx& c, Entity e, const glm::vec3& v) { ScriptApi::setVelocity(c.physics, e, v); }
bool isGrounded(Ctx& c, Entity e)                      { return ScriptApi::isGrounded(c.physics, e); }
} // namespace physics

// ── Materials ────────────────────────────────────────────────────────────────
namespace material {
glm::vec4 getParam(Ctx& c, Entity e, const std::string& n)                     { return c.world ? ScriptApi::getMaterialParam(*c.world, c.content, e, n) : glm::vec4(0.0f); }
bool      setParam(Ctx& c, Entity e, const std::string& n, const glm::vec4& v) { return c.world ? ScriptApi::setMaterialParam(*c.world, c.content, e, n, v) : false; }
} // namespace material

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
} // namespace widget

// ── Cursor ───────────────────────────────────────────────────────────────────
namespace cursor {
void setVisible(Ctx&, bool show) { ScriptApi::setCursorVisible(show); }
} // namespace cursor

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
float min(float a, float b) { return a < b ? a : b; }
float max(float a, float b) { return a > b ? a : b; }
float clamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
float lerp(float a, float b, float t)    { return a + (b - a) * t; }
float length(const glm::vec2& v)                   { return glm::length(v); }
float distance(const glm::vec2& a, const glm::vec2& b) { return glm::length(a - b); }
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

// ── Registry ─────────────────────────────────────────────────────────────────
namespace {

using P  = PinType;
using VV = std::vector<Value>;

// Value readers — tolerant of missing args (return the type's zero).
float       aF (const VV& a, size_t k) { return k < a.size() ? a[k].f   : 0.0f; }
bool        aB (const VV& a, size_t k) { return k < a.size() ? a[k].b   : false; }
int         aI (const VV& a, size_t k) { return k < a.size() ? a[k].i   : 0; }
std::string aS (const VV& a, size_t k) { return k < a.size() ? a[k].s   : std::string(); }
glm::vec2   aV2(const VV& a, size_t k) { return k < a.size() ? a[k].v2  : glm::vec2(0.0f); }
glm::vec4   aV4(const VV& a, size_t k) { return k < a.size() ? a[k].col : glm::vec4(0.0f); }
// vec3 rides in a Color value's xyz (HorizonCode has no Vec3 pin yet).
glm::vec3   aV3(const VV& a, size_t k) { return k < a.size() ? glm::vec3(a[k].col) : glm::vec3(0.0f); }
Value       v3 (const glm::vec3& v)    { return Value::ofColor(glm::vec4(v, 0.0f)); }

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
        t.push_back({ "entity.distance", "Entity", false, {{"a", P::Int}, {"b", P::Int}}, {{"distance", P::Float}}, "HE::api::entity::distance",
            [](Ctx& c, const VV& a){ return VV{ Value::ofFloat(entity::distance(c, (Entity)aI(a, 0), (Entity)aI(a, 1))) }; } });

        // Transform
        t.push_back({ "transform.getPosition", "Transform", false, {{"entity", P::Int}}, {{"position", P::Color}}, "HE::api::transform::getPosition",
            [](Ctx& c, const VV& a){ return VV{ v3(transform::getPosition(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "transform.setPosition", "Transform", true, {{"entity", P::Int}, {"position", P::Color}}, {}, "HE::api::transform::setPosition",
            [](Ctx& c, const VV& a){ transform::setPosition(c, (Entity)aI(a, 0), aV3(a, 1)); return VV{}; } });
        t.push_back({ "transform.getRotation", "Transform", false, {{"entity", P::Int}}, {{"rotation", P::Color}}, "HE::api::transform::getRotation",
            [](Ctx& c, const VV& a){ return VV{ v3(transform::getRotation(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "transform.setRotation", "Transform", true, {{"entity", P::Int}, {"rotation", P::Color}}, {}, "HE::api::transform::setRotation",
            [](Ctx& c, const VV& a){ transform::setRotation(c, (Entity)aI(a, 0), aV3(a, 1)); return VV{}; } });
        t.push_back({ "transform.getScale", "Transform", false, {{"entity", P::Int}}, {{"scale", P::Color}}, "HE::api::transform::getScale",
            [](Ctx& c, const VV& a){ return VV{ v3(transform::getScale(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "transform.setScale", "Transform", true, {{"entity", P::Int}, {"scale", P::Color}}, {}, "HE::api::transform::setScale",
            [](Ctx& c, const VV& a){ transform::setScale(c, (Entity)aI(a, 0), aV3(a, 1)); return VV{}; } });

        // Physics
        t.push_back({ "physics.raycast", "Physics", false,
            {{"origin", P::Color}, {"direction", P::Color}, {"maxDistance", P::Float}},
            {{"hit", P::Bool}, {"entity", P::Int}, {"point", P::Color}, {"normal", P::Color}, {"distance", P::Float}},
            "HE::api::physics::raycast",
            [](Ctx& c, const VV& a){ auto r = physics::raycast(c, aV3(a, 0), aV3(a, 1), aF(a, 2));
                return VV{ Value::ofBool(r.hit), Value::ofInt((int)r.entity), v3(r.point), v3(r.normal), Value::ofFloat(r.distance) }; } });
        t.push_back({ "physics.setVelocity", "Physics", true, {{"entity", P::Int}, {"velocity", P::Color}}, {}, "HE::api::physics::setVelocity",
            [](Ctx& c, const VV& a){ physics::setVelocity(c, (Entity)aI(a, 0), aV3(a, 1)); return VV{}; } });
        t.push_back({ "physics.isGrounded", "Physics", false, {{"entity", P::Int}}, {{"grounded", P::Bool}}, "HE::api::physics::isGrounded",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(physics::isGrounded(c, (Entity)aI(a, 0))) }; } });

        // Materials
        t.push_back({ "material.getParam", "Material", false, {{"entity", P::Int}, {"name", P::String}}, {{"value", P::Color}}, "HE::api::material::getParam",
            [](Ctx& c, const VV& a){ return VV{ Value::ofColor(material::getParam(c, (Entity)aI(a, 0), aS(a, 1))) }; } });
        t.push_back({ "material.setParam", "Material", true, {{"entity", P::Int}, {"name", P::String}, {"value", P::Color}}, {{"ok", P::Bool}}, "HE::api::material::setParam",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(material::setParam(c, (Entity)aI(a, 0), aS(a, 1), aV4(a, 2))) }; } });

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

        // Live widgets
        t.push_back({ "widget.create", "Widget", true, {{"path", P::String}}, {{"widget", P::Int}}, "HE::api::widget::create",
            [](Ctx& c, const VV& a){ return VV{ Value::ofInt(widget::create(c, aS(a, 0))) }; } });
        t.push_back({ "widget.destroy", "Widget", true, {{"widget", P::Int}}, {}, "HE::api::widget::destroy",
            [](Ctx& c, const VV& a){ widget::destroy(c, aI(a, 0)); return VV{}; } });
        t.push_back({ "widget.show", "Widget", true, {{"widget", P::Int}}, {}, "HE::api::widget::show",
            [](Ctx& c, const VV& a){ widget::show(c, aI(a, 0)); return VV{}; } });
        t.push_back({ "widget.hide", "Widget", true, {{"widget", P::Int}}, {}, "HE::api::widget::hide",
            [](Ctx& c, const VV& a){ widget::hide(c, aI(a, 0)); return VV{}; } });
        t.push_back({ "widget.setZOrder", "Widget", true, {{"widget", P::Int}, {"z", P::Int}}, {}, "HE::api::widget::setZOrder",
            [](Ctx& c, const VV& a){ widget::setZOrder(c, aI(a, 0), aI(a, 1)); return VV{}; } });
        t.push_back({ "widget.isVisible", "Widget", false, {{"widget", P::Int}}, {{"visible", P::Bool}}, "HE::api::widget::isVisible",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(widget::isVisible(c, aI(a, 0))) }; } });
        t.push_back({ "widget.callFunction", "Widget", true, {{"widget", P::Int}, {"function", P::String}}, {{"ok", P::Bool}}, "HE::api::widget::callFunction",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(widget::callFunction(c, aI(a, 0), aS(a, 1))) }; } });

        // Cursor
        t.push_back({ "cursor.setVisible", "Cursor", true, {{"show", P::Bool}}, {}, "HE::api::cursor::setVisible",
            [](Ctx& c, const VV& a){ cursor::setVisible(c, aB(a, 0)); return VV{}; } });

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

        return t;
    }();
    return table;
}

const ApiFn* find(const std::string& id)
{
    for (const auto& fn : registry())
        if (id == fn.id) return &fn;
    return nullptr;
}

} // namespace HE::api
