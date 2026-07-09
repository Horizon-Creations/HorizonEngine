#pragma once
#include <HorizonCode/HorizonCode.h>   // HorizonCode::Value, PinType
#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class HorizonWorld;
class PhysicsWorld;
class ContentManager;
class AudioEngine;

// ── HE::api ──────────────────────────────────────────────────────────────────
// The single, engine-wide C++ gameplay API. Every scripting frontend reaches the
// engine through THIS surface — Lua, Python, the HorizonCode interpreter, and the
// future HorizonCode→C++ codegen — described once in a machine-readable registry
// (see the bottom of this header). One row per function feeds all consumers, so
// "completing HorizonCode" is filling the registry, not growing an enum + switch.
// See docs/horizoncode-cpp-codegen-plan.md and docs/horizoncode-completion-plan.md.
//
// Design shape (the constraint every entry obeys, so the same signature binds a
// std::function thunk in the interpreter AND a direct call in generated C++):
//   • free functions over an explicit Ctx — no hidden globals;
//   • plain value/handle types on the boundary — no engine internals, no
//     std::function, so both frontends bind the same signature;
//   • a fixed side-effect classification (isExec) per function.
//
// Tolerance: any Ctx handle may be null — getters then return neutral defaults and
// setters/actions are no-ops (matching the old ScriptApi's forgiving contract).
//
// NB: HorizonCode has no Vec3 pin type yet, so a vec3 (position/rotation/scale,
// ray hit point/normal) is carried on the wire in a Color value's xyz (w unused).
// A dedicated Vec3 pin can replace this later without touching the C++ signatures.
namespace HE::api {

using Value   = HorizonCode::Value;
using PinType = HorizonCode::PinType;
using Entity  = uint32_t;   // raw entt handle, as scripts see it (self.entityId)

// The host handles every call needs; bundle them once instead of threading three
// pointers through every signature. Any may be null (see the tolerance note).
struct Ctx
{
    HorizonWorld*   world   = nullptr;
    PhysicsWorld*   physics = nullptr;
    ContentManager* content = nullptr;
    AudioEngine*    audio   = nullptr;
};

// ── Debug ────────────────────────────────────────────────────────────────────
void log(Ctx&, const std::string& message);

// ── Entities: identity / lifecycle / query ───────────────────────────────────
namespace entity {
    std::string getName(Ctx&, Entity e);                              // "" if invalid
    Entity      spawn(Ctx&, Entity parent, const std::string& name);  // 0 on failure
    void        destroy(Ctx&, Entity e);
    float       distance(Ctx&, Entity a, Entity b);                   // -1 if either invalid
    Entity      findByName(Ctx&, const std::string& name);            // first match, 0 if none
    bool        exists(Ctx&, Entity e);
}

// ── Transform (Euler degrees for rotation) ───────────────────────────────────
namespace transform {
    glm::vec3 getPosition(Ctx&, Entity e);                    // default (0,0,0)
    void      setPosition(Ctx&, Entity e, const glm::vec3& p);
    glm::vec3 getRotation(Ctx&, Entity e);                    // default (0,0,0)
    void      setRotation(Ctx&, Entity e, const glm::vec3& r);
    glm::vec3 getScale(Ctx&, Entity e);                       // default (1,1,1)
    void      setScale(Ctx&, Entity e, const glm::vec3& s);
}

// ── Physics (queries + character-controller helpers) ─────────────────────────
namespace physics {
    struct RaycastHit {
        bool      hit = false;
        Entity    entity = 0;
        glm::vec3 point{0.0f};
        glm::vec3 normal{0.0f};
        float     distance = 0.0f;
    };
    RaycastHit raycast(Ctx&, const glm::vec3& origin, const glm::vec3& dir, float maxDist);
    void       setVelocity(Ctx&, Entity e, const glm::vec3& v);
    bool       isGrounded(Ctx&, Entity e);
}

// ── Materials (node-graph param by name) ─────────────────────────────────────
namespace material {
    glm::vec4 getParam(Ctx&, Entity e, const std::string& name);                       // (0,0,0,0)
    bool      setParam(Ctx&, Entity e, const std::string& name, const glm::vec4& v);   // false if none
}

// ── Entity UI (entities carrying UI components) ──────────────────────────────
namespace ui {
    std::string getText(Ctx&, Entity e);
    void        setText(Ctx&, Entity e, const std::string& text);
    glm::vec4   getColor(Ctx&, Entity e);                     // default (1,1,1,1)
    void        setColor(Ctx&, Entity e, const glm::vec4& c);
    bool        getVisible(Ctx&, Entity e);
    void        setVisible(Ctx&, Entity e, bool visible);
    glm::vec2   getPosition(Ctx&, Entity e);
    void        setPosition(Ctx&, Entity e, const glm::vec2& p);
    glm::vec2   getSize(Ctx&, Entity e);
    void        setSize(Ctx&, Entity e, const glm::vec2& s);
    bool        setMaterialParam(Ctx&, Entity e, const std::string& name, const glm::vec4& v);
}

// ── Live widgets (WidgetManager — exist OUTSIDE the entity world) ────────────
namespace widget {
    int  create(Ctx&, const std::string& path);   // 0 on failure
    void destroy(Ctx&, int id);
    void show(Ctx&, int id);
    void hide(Ctx&, int id);
    void setZOrder(Ctx&, int id, int z);
    bool isVisible(Ctx&, int id);
    bool callFunction(Ctx&, int id, const std::string& fn);   // PUBLIC fns only
}

// ── Cursor (host-app hook) ───────────────────────────────────────────────────
namespace cursor {
    void setVisible(Ctx&, bool show);
}

// ── Camera (the world's main camera: isMain, else the first CameraComponent) ──
namespace camera {
    glm::vec3 getPosition(Ctx&);
    void      setPosition(Ctx&, const glm::vec3& p);
    glm::vec3 getRotation(Ctx&);                      // euler degrees
    void      setRotation(Ctx&, const glm::vec3& r);
    float     getFov(Ctx&);                           // degrees; 0 when no camera
    void      setFov(Ctx&, float degrees);
}

// ── Environment (the world's EnvironmentComponent — sky/fog/wind knobs) ───────
namespace env {
    float getTimeOfDay(Ctx&);        void setTimeOfDay(Ctx&, float t);       // 0..1
    float getCloudCoverage(Ctx&);    void setCloudCoverage(Ctx&, float c);   // 0..1
    float getFogDensity(Ctx&);       void setFogDensity(Ctx&, float d);
    float getWindDirection(Ctx&);    void setWindDirection(Ctx&, float deg);
    float getWindSpeed(Ctx&);        void setWindSpeed(Ctx&, float s);
}

// ── Audio (Ctx.audio — the app's AudioEngine; null → no-ops) ─────────────────
namespace audio {
    // Play an audio ASSET (content-relative .hasset path). Returns a handle
    // (0 on failure) for stop/isPlaying.
    int  play(Ctx&, const std::string& path, float volume, float pitch, bool loop);
    int  playAt(Ctx&, const std::string& path, const glm::vec3& pos,
                float volume, float pitch, bool loop, float minDist, float maxDist);
    void stop(Ctx&, int handle);
    void stopAll(Ctx&);
    bool isPlaying(Ctx&, int handle);
    void setBusVolume(Ctx&, const std::string& bus, float volume);
}

// ── String library (pure; complements the Concat/ToString nodes) ─────────────
// C++ namespace `str`, registry ids "string.*" (namespace `string` would shadow
// std::string in this header's users).
namespace str {
    int         length(const std::string& s);
    std::string substring(const std::string& s, int start, int count); // clamped
    bool        contains(const std::string& s, const std::string& needle);
    int         find(const std::string& s, const std::string& needle);   // -1 if absent
    std::string replace(const std::string& s, const std::string& from, const std::string& to); // all
    std::string toUpper(const std::string& s);   // ASCII
    std::string toLower(const std::string& s);   // ASCII
    std::string trim(const std::string& s);      // ASCII whitespace both ends
    bool        startsWith(const std::string& s, const std::string& prefix);
    bool        endsWith(const std::string& s, const std::string& suffix);
    float       toNumber(const std::string& s);  // 0 when unparsable
}

// ── Math library (pure; no engine state) ─────────────────────────────────────
// Beyond HorizonCode's built-in Add/Sub/Mul/Div/compare operator nodes — the
// standard-library functions gameplay needs. Deterministic, so codegen inlines
// them and the interpreter may freely re-evaluate.
namespace math {
    float sin(float x);
    float cos(float x);
    float tan(float x);
    float sqrt(float x);
    float abs(float x);
    float floor(float x);
    float ceil(float x);
    float round(float x);
    float sign(float x);                         // -1 / 0 / 1
    float pow(float base, float exp);
    float mod(float a, float b);                 // 0 if b == 0
    float atan2(float y, float x);
    float min(float a, float b);
    float max(float a, float b);
    float clamp(float x, float lo, float hi);
    float lerp(float a, float b, float t);
    float length(const glm::vec2& v);
    float distance(const glm::vec2& a, const glm::vec2& b);
}

// ── Random (seeded PRNG; process-global state) ───────────────────────────────
// Stateful — each draw advances the generator — so in HorizonCode these are exec
// nodes (isExec) that cache one value per run, not pure data chips (which would
// re-roll on every pin read). seed() makes a run reproducible, so the codegen
// parity harness can pin a seed and compare. No engine state → no Ctx.
namespace random {
    void  seed(uint32_t s);
    float value();                       // uniform [0, 1)
    float range(float min, float max);   // uniform [min, max)  (swaps if min > max)
    int   rangeInt(int min, int max);    // uniform [min, max]  inclusive
    bool  chance(float p);               // true with probability p (clamped 0..1)
}

// ── Time / frame (process-global clock; the app advances it once per frame) ───
// Getters are pure (constant within a frame) → pure data nodes in HorizonCode.
namespace time {
    void  advance(float dtSeconds);      // app hook: called once per rendered frame
    void  reset();                       // app hook: zero on play-start
    float deltaTime();                   // last frame's dt (seconds)
    float elapsed();                     // seconds since reset
    int   frameCount();                  // frames since reset
}

// ── Input (process-global snapshot; the app pushes it each frame) ─────────────
// Key names follow SDL scancode names ("W", "Space", "Left", "Escape", …) so the
// query side stays SDL-free here while the app populates it from real devices.
// Getters are pure (constant within a frame) → pure data nodes in HorizonCode.
namespace input {
    // App hooks (populate the snapshot).
    void setMouse(const glm::vec2& pos, const glm::vec2& delta, uint32_t buttonMask, float scroll);
    void setKeysDown(const std::vector<std::string>& downKeyNames);
    void clear();
    // Script queries.
    bool      keyDown(const std::string& name);
    bool      mouseButton(int index);    // 0 = left, 1 = right, 2 = middle
    glm::vec2 mousePosition();
    glm::vec2 mouseDelta();
    float     scrollDelta();
}

// ── Machine-readable registry ─────────────────────────────────────────────────
// One ApiFn per function. The interpreter looks a function up by `id` and calls
// `invoke`; the editor builds its add-menu from `category`/`params`/`results`;
// codegen emits `cppCall(args…)`. This is the single source of truth.

struct ApiParam { const char* name; PinType type; };

struct ApiFn
{
    const char* id;          // stable identifier, e.g. "transform.setPosition"
    const char* category;    // add-menu group, e.g. "Transform"
    bool        isExec;      // true = side-effecting (exec node); false = pure data node
    std::vector<ApiParam> params;    // typed inputs (in call order)
    std::vector<ApiParam> results;   // typed outputs (in return order)
    const char* cppCall;     // fully-qualified C++ callee, for the codegen back-end
    // Marshalling thunk: HorizonCode Values in → typed C++ call → Values out.
    // Missing/extra args are tolerated (defaults fill in), mirroring the API's
    // null-Ctx forgiveness.
    std::function<std::vector<Value>(Ctx&, const std::vector<Value>&)> invoke;
    // Human-readable editor name ("Sine", "Set Position") — what menus and node
    // titles show; `id` stays the stable machine identifier. Assigned in a post-
    // pass when the table is built (trailing member so rows stay positional).
    const char* displayName = nullptr;
};

// The full table (built once). Order is stable and grouped by category.
const std::vector<ApiFn>& registry();
// Look up a single entry by id; nullptr if unknown.
const ApiFn* find(const std::string& id);

} // namespace HE::api
