#pragma once
#include <HorizonCode/HorizonCode.h>   // HorizonCode::Value, PinType
#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class HorizonWorld;
class PhysicsWorld;
class ContentManager;
class AudioEngine;
struct DebugLine;   // HE_Core DebugDraw.h (renderer debug-line vertex pair)

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
    // Per-entity visibility: flips every renderable component the entity carries
    // (mesh, skeletal mesh, light, particles, foliage). getVisible reads the
    // first renderable found (true when the entity has none).
    void        setVisible(Ctx&, Entity e, bool visible);
    bool        getVisible(Ctx&, Entity e);
    // ── Savegame state (SaveStateComponent + the ACTIVE save) ────────────────
    // saveState writes the component-flagged attributes under the entity's
    // stable UUID into the active save; applySavedState applies every attribute
    // PRESENT there back onto the instance (partial by design). Both require
    // play mode (PIE/packaged — the SceneSerializer owns edit-mode persistence),
    // an active save, and an enabled SaveStateComponent — anything missing
    // fails LOUD (log + false). hasSavedState only needs an active save.
    bool saveState(Ctx&, Entity e);
    bool hasSavedState(Ctx&, Entity e);
    bool applySavedState(Ctx&, Entity e);
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

// ── Environment (the world's EnvironmentComponent) ───────────────────────────
// EVERY EnvironmentComponent field is scriptable. The X-lists below are the
// single source of truth: one row per field generates the typed get/set pair
// declared here, the implementation, the registry rows — which are what the
// HorizonCode interpreter, the editor's add-menu AND the generated-C++ path
// (hc::callApi by id) consume — and the editor display names. Adding a field
// to the component = adding one row to the matching list.
//   X(member, Name, "Display")  →  env::get<Name>/set<Name>, ids "env.get<Name>"…
// vec3 colours ride the wire as Color values (xyz, w unused), like camera/ui.
#define HE_ENV_FIELDS_FLOAT(X) \
    X(timeOfDay,           TimeOfDay,           "Time Of Day") \
    X(cycleSeconds,        CycleSeconds,        "Day Cycle Seconds") \
    X(sunIntensity,        SunIntensity,        "Sun Intensity") \
    X(moonIntensity,       MoonIntensity,       "Moon Intensity") \
    X(moonPhase,           MoonPhase,           "Moon Phase") \
    X(moonCycleDays,       MoonCycleDays,       "Moon Cycle Days") \
    X(cloudCoverage,       CloudCoverage,       "Cloud Coverage") \
    X(windDirection,       WindDirection,       "Wind Direction") \
    X(windSpeed,           WindSpeed,           "Wind Speed") \
    X(cloudHeight,         CloudHeight,         "Cloud Height") \
    X(cloudDensity,        CloudDensity,        "Cloud Density") \
    X(cloudFluffiness,     CloudFluffiness,     "Cloud Fluffiness") \
    X(contrailAmount,      ContrailAmount,      "Contrail Amount") \
    X(cirrusAmount,        CirrusAmount,        "Cirrus Amount") \
    X(cirrusSeed,          CirrusSeed,          "Cirrus Seed") \
    X(godRays,             GodRays,             "God Rays") \
    X(shootingStars,       ShootingStars,       "Shooting Stars") \
    X(lensFlare,           LensFlare,           "Lens Flare") \
    X(fogDensity,          FogDensity,          "Fog Density") \
    X(fogHeightFalloff,    FogHeightFalloff,    "Fog Height Falloff") \
    X(rainAmount,          RainAmount,          "Rain Amount") \
    X(snowAmount,          SnowAmount,          "Snow Amount") \
    X(wetness,             Wetness,             "Wetness") \
    X(flash,               Flash,               "Lightning Flash") \
    X(auroraIntensity,     AuroraIntensity,     "Aurora Intensity") \
    X(milkyWayIntensity,   MilkyWayIntensity,   "Milky Way Intensity") \
    X(nebulaIntensity,     NebulaIntensity,     "Nebula Intensity") \
    X(nebulaSeed,          NebulaSeed,          "Nebula Seed") \
    X(nebulaCoverage,      NebulaCoverage,      "Nebula Coverage") \
    X(auroraHeight,        AuroraHeight,        "Aurora Height") \
    X(auroraFragmentation, AuroraFragmentation, "Aurora Fragmentation") \
    X(starBrightness,      StarBrightness,      "Star Brightness") \
    X(starSize,            StarSize,            "Star Size") \
    X(starSizeVariation,   StarSizeVariation,   "Star Size Variation") \
    X(starGlow,            StarGlow,            "Star Glow") \
    X(starTwinkle,         StarTwinkle,         "Star Twinkle") \
    X(starDensity,         StarDensity,         "Star Density")
#define HE_ENV_FIELDS_BOOL(X) \
    X(dayNightCycle,       DayNightCycle,       "Day Night Cycle") \
    X(autoAdvance,         AutoAdvance,         "Auto Advance Time") \
    X(moonPhaseAuto,       MoonPhaseAuto,       "Moon Phase Auto") \
    X(lowResClouds,        LowResClouds,        "Low Res Clouds")
#define HE_ENV_FIELDS_INT(X) \
    X(cloudMode,           CloudMode,           "Cloud Mode") \
    X(cloudQuality,        CloudQuality,        "Cloud Quality") \
    X(nebulaQuality,       NebulaQuality,       "Nebula Quality")
#define HE_ENV_FIELDS_COLOR(X) \
    X(sunColor,            SunColor,            "Sun Color") \
    X(moonColor,           MoonColor,           "Moon Color") \
    X(cloudTint,           CloudTint,           "Cloud Tint") \
    X(nebulaColor,         NebulaColor,         "Nebula Color 1") \
    X(nebulaColor2,        NebulaColor2,        "Nebula Color 2") \
    X(nebulaColor3,        NebulaColor3,        "Nebula Color 3") \
    X(auroraColor,         AuroraColor,         "Aurora Color") \
    X(auroraColorTop,      AuroraColorTop,      "Aurora Color Top") \
    X(starColor,           StarColor,           "Star Color")

namespace env {
#define HE_ENV_DECL_FLOAT(m, Name, disp) float     get##Name(Ctx&); void set##Name(Ctx&, float);
#define HE_ENV_DECL_BOOL(m, Name, disp)  bool      get##Name(Ctx&); void set##Name(Ctx&, bool);
#define HE_ENV_DECL_INT(m, Name, disp)   int       get##Name(Ctx&); void set##Name(Ctx&, int);
#define HE_ENV_DECL_COLOR(m, Name, disp) glm::vec3 get##Name(Ctx&); void set##Name(Ctx&, const glm::vec3&);
HE_ENV_FIELDS_FLOAT(HE_ENV_DECL_FLOAT)
HE_ENV_FIELDS_BOOL(HE_ENV_DECL_BOOL)
HE_ENV_FIELDS_INT(HE_ENV_DECL_INT)
HE_ENV_FIELDS_COLOR(HE_ENV_DECL_COLOR)
#undef HE_ENV_DECL_FLOAT
#undef HE_ENV_DECL_BOOL
#undef HE_ENV_DECL_INT
#undef HE_ENV_DECL_COLOR
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
    void setSoundPosition(Ctx&, int handle, const glm::vec3& pos); // move a spatial sound
}

// ── Debug draw (process-global timed queue; the app drains it each frame) ─────
// Submissions live for `seconds` (0 = exactly one frame). collect() advances the
// timers and appends the alive primitives as line segments — the app forwards
// them to IRenderer::SetDebugLines.
namespace debug {
    void line(const glm::vec3& a, const glm::vec3& b, const glm::vec3& color, float seconds);
    void sphere(const glm::vec3& center, float radius, const glm::vec3& color, float seconds);
    void box(const glm::vec3& mn, const glm::vec3& mx, const glm::vec3& color, float seconds);
    void clear();
    // App hook: advance by dt, append alive segments to `out` (DebugLine from
    // HE_Core DebugDraw.h), drop expired entries.
    void collect(float dt, std::vector<DebugLine>& out);
}

// ── Sandboxed file I/O (fs) + save-game store (save) ─────────────────────────
// All paths are RELATIVE to a per-project sandbox root the app sets (editor: the
// project's Saved/ dir; game: the per-user pref dir). Absolute paths and ".."
// are rejected — scripts can never leave the sandbox.
namespace fs {
    void        setSandboxRoot(const std::string& absDir);  // app hook (created on demand)
    std::string sandboxRoot();
    bool        writeText(const std::string& rel, const std::string& text);
    std::string readText(const std::string& rel);            // "" when missing/invalid
    bool        exists(const std::string& rel);
    bool        remove(const std::string& rel);               // files only
    bool        makeDir(const std::string& rel);
}
// ── Savegames: one ACTIVE, template-shaped save document ─────────────────────
// A save is { id, templateRef, fields, entities }: its fields are declared by a
// SaveGameTemplate asset (typed, with defaults — so scripts never have to
// remember what lives where; save.fields() and the editor's field dropdowns
// enumerate them), and serialized entity state (SaveStateComponent) is appended
// under entity UUIDs. Exactly one save is active at a time.
//
// create()/load() and every field accessor work purely IN MEMORY — only
// write()/list()/exists()/remove() touch disk (Saves/<id>.json under the fs
// sandbox; write is atomic temp+rename). Ids are restricted to
// [A-Za-z0-9_-]+ so they round-trip as filenames. Failures are LOUD: a call
// without an active save, an unknown field, or a type mismatch logs and
// returns false / the default — never silently.
namespace save {
    // App hooks.
    void setDefaultTemplate(const std::string& contentRelPath); // editor: .heproj; game: hcfg
    std::string defaultTemplate();

    // Lifecycle. create() seeds the fields from the template's defaults
    // (explicit `templatePath` empty → the project default; none configured →
    // fail). load() reads Saves/<id>.json, resolves the template it names and
    // re-validates every stored field against it (unknown/missing template =
    // loud failure). Both replace the previously active save.
    bool create(const std::string& id, ::ContentManager* cm,
                const std::string& templatePath = {});
    bool load(const std::string& id, ::ContentManager* cm);
    bool write();                    // persist the active save (atomic)
    void close();                    // drop the active save (PIE stop / project switch)
    std::string activeId();          // "" = none

    // Disk queries (independent of the active save).
    std::vector<std::string> list(); // ids of every save under Saves/
    bool exists(const std::string& id);
    bool remove(const std::string& id);

    // Template introspection: the active save's field names.
    std::vector<std::string> fields();

    // Typed field access, validated against the template. getNumber/setNumber
    // cover Float, Int and Enum fields (int-backed); Vec2/Color/Transform
    // fields are reachable by wrapping them in a struct field.
    bool        setNumber(const std::string& field, float v);
    float       getNumber(const std::string& field, float def);
    bool        setString(const std::string& field, const std::string& v);
    std::string getString(const std::string& field, const std::string& def);
    bool        setBool(const std::string& field, bool v);
    bool        getBool(const std::string& field, bool def);
    bool               setStructV(const std::string& field, const HorizonCode::Value& v);
    HorizonCode::Value getStructV(const std::string& field);

    // Entity-state section (SaveStateComponent, see entity.saveState /
    // applySavedState): an opaque JSON object per entity UUID.
    bool        setEntityState(const std::string& uuid, const std::string& json);
    std::string entityState(const std::string& uuid);   // "" = none
    bool        hasEntityState(const std::string& uuid);

    // Play-mode gate for the entity-state API (the SceneSerializer owns
    // edit-mode persistence): set by the editor's PIE toggle and the game.
    void setPlayMode(bool inPlay);
    bool inPlayMode();
}

// ── Scene transitions (process-global request queue; the app executes) ────────
// load() requests a full deferred world switch at a safe frame boundary;
// loadAdditive() streams another scene INTO the running world (returns a zone id
// so it can be unloaded again) — together they give seamless level transitions:
// additively load the next zone, move the player, unload the zone behind.
// `hidden` defers presentation: a hidden zone loads with its meshes invisible
// until showZone; a hidden level PRELOADS in the background and swaps in on
// activate(). Zone queries (loaded zones, their scene + position) read a
// process-global zone table the app maintains after executing requests.
// Editor PIE consumes the requests with a notice (game-runtime feature).
namespace scene {
    void load(const std::string& scenePath, bool hidden = false);
    // `position` places the zone's root when it loads (unwired/zero = as
    // authored — the merge root is a fresh identity entity, so zero is a no-op).
    int  loadAdditive(const std::string& scenePath, bool hidden = false,
                      const glm::vec3& position = glm::vec3(0.0f)); // → zone id
    void unloadZone(int zone);
    void activate();   // swap in the level preloaded with load(path, hidden=true)
    // Queued variants of show/hide/move — they order correctly with a load
    // requested the SAME frame (the direct setters below only see zones that
    // already finished loading).
    void requestZoneVisible(int zone, bool visible);
    // Show Zone / Hide Zone as two callees rather than one with a bool: the
    // registry rows below expose them as separate functions, and ApiFn::cppCall
    // has to name a function whose signature matches that row's params exactly
    // (a shared requestZoneVisible would drop the bool that tells them apart).
    void showZone(int zone);
    void hideZone(int zone);
    void requestZonePosition(int zone, const glm::vec3& p);

    // ── Zone queries / control (direct; operate on the app-maintained table) ──
    std::vector<int>         loadedZones();
    std::string              zoneScene(int zone);            // "" unknown
    glm::vec3                zonePosition(Ctx&, int zone);   // the zone root's position
    void                     setZonePosition(Ctx&, int zone, const glm::vec3& p); // move the whole zone
    void                     setZoneVisible(Ctx&, int zone, bool visible);        // flip its meshes
    // Every scene the game can load: the packed scene index in shipped builds,
    // a project scan (.hescene, project-relative) in dev builds.
    std::vector<std::string> availableScenes(Ctx&);
    bool                     hasPendingLevel();              // a hidden load awaits activate()

    // ── App hooks ─────────────────────────────────────────────────────────────
    // What a queued Request asks the app to do. The app dispatch (game runtime and
    // editor PIE) switches on Request::kindOf() instead of repeating the raw numbers.
    enum class RequestKind : int
    {
        Switch       = 0,  // full world switch — or, with hidden, a background PRELOAD
        Additive     = 1,  // additive zone load
        UnloadZone   = 2,  // unload an additive zone
        Activate     = 3,  // present the level preloaded by Switch + hidden
        ZoneVisible  = 4,  // show/hide a zone (queued so it orders after a load)
        ZonePosition = 5,  // move a zone (queued so it orders after a load)
    };
    struct Request
    {
        // Stays `int` on the wire: the numbering is the queue's public contract
        // (test_engine_api.cpp asserts it) and RequestKind is the typed view of it.
        int         kind = 0;
        std::string path;
        int         zone = 0;
        bool        hidden = false;                 // load: defer presentation
        bool        flag = false;                   // ZoneVisible: target visibility
        glm::vec3   pos{ 0.0f };                    // Additive: placement; ZonePosition: target
        RequestKind kindOf() const { return static_cast<RequestKind>(kind); }
    };
    std::vector<Request> takeRequests();
    struct ZoneInfo { std::string path; uint32_t root = 0; std::vector<uint32_t> entities; };
    void            noteZoneLoaded(int zone, ZoneInfo info);
    void            noteZoneUnloaded(int zone);
    void            clearZones();          // world switched — zones are gone
    const ZoneInfo* zoneInfo(int zone);    // nullptr unknown
    void            notePendingLevel(bool pending);
    // The well-known scene-index name (packed under sceneUuidForPath(this)).
    inline const char* kSceneIndexEntry = "__scene_index__";
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
    // Push the current SDL keyboard/mouse state into the snapshot above, so the
    // input.* registry nodes and scripts poll fresh values this frame. Mouse delta +
    // scroll are left at 0 here to avoid consuming SDL's relative-motion accumulator
    // the free-fly camera controller uses; position + buttons + keys (by SDL scancode
    // name, e.g. "W"/"Space") are polled. Called once per frame by whichever app owns
    // the SDL window — the packaged game every frame, the editor only while playing
    // (edit mode leaves the snapshot untouched).
    void pushSdlSnapshot();
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
// codegen emits the generic `hc::callApi(ctx, "<id>", …)` thunk, which lands on
// the same `invoke`. This is the single source of truth.

struct ApiParam { const char* name; PinType type; bool isArray = false; };

struct ApiFn
{
    const char* id;          // stable identifier, e.g. "transform.setPosition"
    const char* category;    // add-menu group, e.g. "Transform"
    bool        isExec;      // true = side-effecting (exec node); false = pure data node
    std::vector<ApiParam> params;    // typed inputs (in call order)
    std::vector<ApiParam> results;   // typed outputs (in return order)
    // Fully-qualified C++ callee this row stands for. Staged input for a planned
    // codegen migration (emit the direct call instead of routing through
    // hc::callApi) — nothing emits it today, so it is only as correct as the
    // invariant test in test_engine_api.cpp keeps it: one distinct callee per row,
    // its signature matching `params` in order.
    const char* cppCall;
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

// True for a registry id's namespace ("math" of "math.clamp") that the text
// scripting frontends expose as horizon.<group>.<fn>. Lua (ScriptContext) and
// Python (PyScriptBackend) MUST agree on this, or the same script works in one
// language and not the other — hence one list, not one per backend. The flat
// gameplay functions keep their ergonomic hand-written bindings until ScriptApi
// is inverted onto HE::api. NB: a packed vec3 (Color) param spreads as 4 numbers
// (x, y, z, _) on this path. Widening the surface = adding a name to the list.
bool isScriptGroup(std::string_view group);

} // namespace HE::api
