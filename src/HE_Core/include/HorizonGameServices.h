#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ── Engine services for native C++ GameLogic ─────────────────────────────────
// A GameLogic library compiles against <IGameLogic.h> alone and links NOTHING —
// so it cannot call engine functions directly. This header closes that gap with
// C-ABI function-pointer tables the engine INJECTS after loading the library:
//
//   1. The game defines the receiving exports once (GameLogic.cpp):
//          #include <HorizonGameServices.h>
//          HE_IMPLEMENT_ENGINE_SERVICES()
//   2. The engine fills the tables (HE::api::fillSaveServices,
//      fillPhysicsServices, fillInputServices, fillContentServices), points an
//      HeEngineServices umbrella at them and calls the export right after the
//      library loads — before onStart.
//   3. Game code uses the he::save / he::entity / he::physics / he::input /
//      he::content wrappers below (or the raw tables). Before injection — or under an engine
//      too old to inject — every wrapper is a safe no-op returning its default,
//      mirroring the script API's loud-failure-not-crash contract (the engine
//      side logs).
//
// ABI rules: plain C types only, strings cross as UTF-8 char* (returns via
// caller buffer + required-length result), vectors cross as float[3]/float[2]
// (the game side has no glm — it sees this header and nothing else), structs
// cross as POD through an out-pointer, and every table is versioned and owned by
// the ENGINE (valid for the library's whole lifetime). Additions APPEND new
// pointers and bump that table's version.
//
// Version rule — the receiver accepts `table->abiVersion >= <what this module
// was built against>`, not equality. With append-only growth the module's view
// of the struct is a PREFIX of the engine's: an engine that is newer wrote more
// than the module reads, which is harmless. The other direction is not — a
// module newer than the engine would read past what the engine filled and call
// through an uninitialised pointer, and that is exactly what `>=` refuses.

#define HE_SAVE_ABI_VERSION     1u
#define HE_PHYSICS_ABI_VERSION  1u
#define HE_INPUT_ABI_VERSION    1u
#define HE_CONTENT_ABI_VERSION  1u
// The umbrella that carries the tables. Bumped when a table POINTER is appended
// to HeEngineServices, not when a table itself grows.
//   1 — save, physics, input
//   2 — + content
#define HE_SERVICES_ABI_VERSION 2u

// Export decoration for the receiving symbol — same rule as <IGameLogic.h>,
// defined here too so this header stands alone (e.g. in tests).
#ifndef HE_GAME_API
#  ifdef _WIN32
#    define HE_GAME_API __declspec(dllexport)
#  else
#    define HE_GAME_API
#  endif
#endif

extern "C" {

typedef struct HeSaveServices
{
    uint32_t abiVersion;   // HE_SAVE_ABI_VERSION
    void*    host;         // opaque engine context — pass to every call

    // ── Save document lifecycle (see HE::api::save for semantics) ────────────
    bool (*create)(void* host, const char* id);
    bool (*load)(void* host, const char* id);
    bool (*write)(void* host);
    void (*close)(void* host);
    bool (*exists)(void* host, const char* id);
    bool (*removeSave)(void* host, const char* id);
    // String getters: write up to `cap` bytes (incl. the NUL) into `buf` and
    // return the FULL length — call again with a bigger buffer when cap was
    // too small. listIds/fields separate entries with '\n'.
    int  (*activeId)(void* host, char* buf, int cap);
    int  (*listIds)(void* host, char* buf, int cap);
    int  (*fields)(void* host, char* buf, int cap);

    // ── Typed field access (validated against the template; loud on the engine
    //    side, defaults on this side) ──────────────────────────────────────────
    bool  (*setNumber)(void* host, const char* field, float v);
    float (*getNumber)(void* host, const char* field, float def);
    bool  (*setString)(void* host, const char* field, const char* v);
    int   (*getString)(void* host, const char* field, char* buf, int cap);
    bool  (*setBool)(void* host, const char* field, bool v);
    bool  (*getBool)(void* host, const char* field, bool def);
    // Struct fields cross as JSON text (the save file's own field encoding);
    // pair them with the generated Source/Generated/GameTypes.h types.
    bool  (*setStructJson)(void* host, const char* field, const char* json);
    int   (*getStructJson)(void* host, const char* field, char* buf, int cap);

    // ── Entity save-state (SaveStateComponent; play-mode + active-save gated) ─
    uint32_t (*findEntityByName)(void* host, const char* name);   // 0 = none
    bool (*entitySaveState)(void* host, uint32_t entity);
    bool (*entityHasSavedState)(void* host, uint32_t entity);
    bool (*entityApplySavedState)(void* host, uint32_t entity);
} HeSaveServices;

typedef void (*FnSetEngineServices)(const HeSaveServices*);

// ── Physics (see HE::api::physics for the semantics of every row) ────────────
// Vectors are float[3] in the caller's buffer; hits come back through an
// out-pointer. The one rule worth reading twice: raycast/sphereCast report WORLD
// points, while setPosition takes a LOCAL one — the same split the script API
// documents, because a position paired with an ENTITY is local unless the name
// says World. "Teleport onto what I hit" therefore needs the entity to be
// unparented (or a world-space conversion the engine side already owns).
typedef struct HeRaycastHit
{
    bool     hit;
    uint32_t entity;      // 0 when hit == false
    float    point[3];    // WORLD
    float    normal[3];   // WORLD
    float    distance;
} HeRaycastHit;

typedef struct HePhysicsServices
{
    uint32_t abiVersion;   // HE_PHYSICS_ABI_VERSION
    void*    host;         // opaque engine context — pass to every call

    // Queries. Without a PhysicsWorld every one of them is the neutral answer.
    void (*raycast)(void* host, const float origin[3], const float dir[3],
                    float maxDist, HeRaycastHit* out);
    // Same hit shape as raycast, but it ignores triggers — a sweep asks what
    // would BLOCK it, and a trigger blocks nothing.
    void (*sphereCast)(void* host, const float origin[3], const float dir[3],
                       float radius, float maxDist, HeRaycastHit* out);
    // Writes up to `cap` entities and returns the FULL count — call again with a
    // bigger buffer when cap was too small, exactly like the string getters.
    int  (*overlapSphere)(void* host, const float center[3], float radius,
                          uint32_t* out, int cap);

    // Pushing a body around. A force is continuous and has to be applied every
    // frame, an impulse lands once, a torque spins. All three need a DYNAMIC
    // rigid body on the entity and answer false when there is none.
    bool (*addForce)(void* host, uint32_t entity, const float force[3]);
    bool (*addImpulse)(void* host, uint32_t entity, const float impulse[3]);
    bool (*addTorque)(void* host, uint32_t entity, const float torque[3]);

    // Velocity in m/s. Addresses the character controller when the entity has
    // one and the rigid body otherwise.
    void (*setVelocity)(void* host, uint32_t entity, const float v[3]);
    void (*getVelocity)(void* host, uint32_t entity, float out[3]);
    bool (*isGrounded)(void* host, uint32_t entity);

    // TELEPORT — `position` is LOCAL (see the note above). ...AndReset zeroes
    // the velocity too, which is what a respawn wants. Both answer false for an
    // entity with neither a body nor a character.
    bool (*setPosition)(void* host, uint32_t entity, const float position[3]);
    bool (*setPositionAndReset)(void* host, uint32_t entity, const float position[3]);

    // The guard to ask before pushing, and the honest answer to "why did my
    // impulse do nothing".
    bool (*hasPhysics)(void* host, uint32_t entity);

    // World gravity in m/s². Rigid bodies only — a character controller falls by
    // its own component's gravity value.
    void (*setGravity)(void* host, const float g[3]);
    void (*getGravity)(void* host, float out[3]);
} HePhysicsServices;

// ── Input (see HE::api::input) ───────────────────────────────────────────────
// The snapshot is process-global and the app pushes it each frame; `host` is
// carried anyway so every table in this header looks the same. Deliberately
// READ-ONLY: the app hooks that WRITE the snapshot (setKeysDown, setGamepad,
// pushSdlSnapshot, …) are the host's, and a game module overwriting them would
// be a bug rather than a feature.
typedef struct HeInputServices
{
    uint32_t abiVersion;   // HE_INPUT_ABI_VERSION
    void*    host;

    // Keyboard/mouse. Key names are SDL scancode names ("W", "Space", "Escape").
    bool  (*keyDown)(void* host, const char* name);
    bool  (*mouseButton)(void* host, int index);   // 0 = left, 1 = right, 2 = middle
    void  (*mousePosition)(void* host, float out[2]);
    void  (*mouseDelta)(void* host, float out[2]);
    float (*scrollDelta)(void* host);

    // Gamepad. Names from SDL's mapping tables, Xbox layout: buttons "a"/"b"/
    // "x"/"y"/"leftshoulder"/"dpup"/…, axes "leftx"/"lefty"/"rightx"/"righty"/
    // "lefttrigger"/"righttrigger". Sticks read -1..+1 (Y positive DOWNWARD),
    // triggers 0..1, and the axes are deadzone-filtered — a resting stick is 0.
    bool  (*gamepadConnected)(void* host);
    bool  (*gamepadButton)(void* host, const char* name);
    float (*gamepadAxis)(void* host, const char* name);

    // Input routing: 0 = GameOnly, 1 = GameAndUI, 2 = UIOnly (HE::api::input::
    // Mode). setMode ignores anything outside that range.
    int   (*mode)(void* host);
    void  (*setMode)(void* host, int mode);
} HeInputServices;

// ── Content (see HE::api::content) ───────────────────────────────────────────
// An asset's identity, layout-identical to HE::UUID (Types/UUID.h). A struct of
// two integers rather than a string because the project HAS no UUID↔text
// conversion, and inventing one just to cross this boundary would be new surface
// for nothing.
typedef struct HeAssetId { uint64_t hi, lo; } HeAssetId;

// Residency, and nothing else. There is no getStaticMesh row in this table and
// there will not be one: the ContentManager's getters return pointers into a
// dense pool that the NEXT load moves — invalidating the pointer AND every
// std::string the asset owns. Inside the engine that is a documented trap; handed
// across a dylib boundary, where the engine cannot know when the module will
// dereference what it was given, it would be a crash waiting for a second load.
// What crosses here is values: an id, a bool, a name.
typedef struct HeContentServices
{
    uint32_t abiVersion;   // HE_CONTENT_ABI_VERSION
    void*    host;         // opaque engine context — pass to every call

    // Load (or return the already-resident) asset at a content-relative path
    // ("Meshes/Rock.hasset"). Writes the id to `out` and returns false — leaving
    // `out` zeroed — when the path is unknown or unreadable. The ONLY row here
    // that reads the disk.
    bool (*loadAsset)(void* host, const char* relativePath, HeAssetId* out);
    // Drop it again. false = that id was not loaded.
    bool (*unloadAsset)(void* host, HeAssetId id);
    bool (*isLoadedId)(void* host, HeAssetId id);
    // The asset's kind as text ("StaticMesh", "Texture", …; "" for an id this
    // manager does not know). Same two-call convention as the save table's
    // string getters: writes up to `cap` bytes including the NUL and returns the
    // FULL length.
    int  (*assetTypeName)(void* host, HeAssetId id, char* buf, int cap);
} HeContentServices;

// ── The umbrella ─────────────────────────────────────────────────────────────
// Sibling tables rather than one growing table, and one export that hands them
// over together. A new service appends a POINTER here and bumps
// HE_SERVICES_ABI_VERSION; the tables themselves keep their own versions, so a
// module built before a service existed simply reads null for it.
typedef struct HeEngineServices
{
    uint32_t                 abiVersion;   // HE_SERVICES_ABI_VERSION
    const HeSaveServices*    save;
    const HePhysicsServices* physics;
    const HeInputServices*   input;
    const HeContentServices* content;      // umbrella v2
} HeEngineServices;

typedef void (*FnSetEngineServicesV2)(const HeEngineServices*);

// Defined by HE_IMPLEMENT_ENGINE_SERVICES() in exactly one GameLogic .cpp.
extern const HeSaveServices*    g_heSaveServices;
extern const HePhysicsServices* g_hePhysicsServices;
extern const HeInputServices*   g_heInputServices;
extern const HeContentServices* g_heContentServices;

} // extern "C"

// The receiving exports — define in exactly ONE .cpp of the GameLogic library.
//
// Both are defined, because either side can be the older one:
//   · old module / new engine — the engine finds no …V2 export and falls back to
//     HE_SetEngineServices, so save keeps working and the rest reads unavailable.
//   · new module / old engine — the engine only knows the v1 export, so only
//     g_heSaveServices is set and the rest reads unavailable.
// Both are a STATE, not an error. HE_SetEngineServices touches nothing but the
// save table: it is the v1 contract and stays exactly what it was.
// The umbrella version is checked PER POINTER, at the version that pointer was
// appended at — not once against HE_SERVICES_ABI_VERSION. The prefix rule is the
// reason: an engine that only knows umbrella v1 wrote a struct that ends after
// `input`, so reading `s->content` from it would read past its storage; but
// save/physics/input ARE there and refusing them too would mean a module built
// today loses its savegame API on last month's engine — exactly the failure the
// V2 export was introduced to avoid.
#define HE_IMPLEMENT_ENGINE_SERVICES() \
    extern "C" { \
    const HeSaveServices*    g_heSaveServices    = nullptr; \
    const HePhysicsServices* g_hePhysicsServices = nullptr; \
    const HeInputServices*   g_heInputServices   = nullptr; \
    const HeContentServices* g_heContentServices = nullptr; \
    HE_GAME_API void HE_SetEngineServices(const HeSaveServices* s) \
    { g_heSaveServices = (s && s->abiVersion >= HE_SAVE_ABI_VERSION) ? s : nullptr; } \
    HE_GAME_API void HE_SetEngineServicesV2(const HeEngineServices* s) \
    { \
        const bool v1 = s && s->abiVersion >= 1u; \
        const bool v2 = s && s->abiVersion >= 2u; \
        g_heSaveServices = (v1 && s->save && \
            s->save->abiVersion >= HE_SAVE_ABI_VERSION) ? s->save : nullptr; \
        g_hePhysicsServices = (v1 && s->physics && \
            s->physics->abiVersion >= HE_PHYSICS_ABI_VERSION) ? s->physics : nullptr; \
        g_heInputServices = (v1 && s->input && \
            s->input->abiVersion >= HE_INPUT_ABI_VERSION) ? s->input : nullptr; \
        g_heContentServices = (v2 && s->content && \
            s->content->abiVersion >= HE_CONTENT_ABI_VERSION) ? s->content : nullptr; \
    } \
    }

// ── Convenience wrappers (game side) ─────────────────────────────────────────
namespace he {

// The game side has no glm — it sees this header and <IGameLogic.h>, nothing
// else. These two are the whole vector vocabulary, laid out so they can be
// handed straight to a float[3]/float[2] parameter.
struct Vec3 { float x = 0.0f, y = 0.0f, z = 0.0f; };
struct Vec2 { float x = 0.0f, y = 0.0f; };

struct RaycastHit
{
    bool     hit      = false;
    uint32_t entity   = 0;
    Vec3     point;      // WORLD
    Vec3     normal;     // WORLD
    float    distance = 0.0f;
};

// An asset's identity as the engine hands it over. A value, not a handle into
// anything: holding one across a load is safe, which is the whole reason this
// boundary trades ids and never pointers.
struct AssetId
{
    uint64_t hi = 0, lo = 0;
    bool valid() const { return hi != 0 || lo != 0; }
    bool operator==(const AssetId&) const = default;
};

namespace detail {
inline const HeSaveServices*    svc()        { return g_heSaveServices; }
inline const HePhysicsServices* physSvc()    { return g_hePhysicsServices; }
inline const HeInputServices*   inputSvc()   { return g_heInputServices; }
inline const HeContentServices* contentSvc() { return g_heContentServices; }
inline ::HeAssetId toC(const AssetId& id)   { return ::HeAssetId{ id.hi, id.lo }; }
inline AssetId     fromC(const ::HeAssetId& id) { return AssetId{ id.hi, id.lo }; }
inline RaycastHit fromC(const HeRaycastHit& h)
{
    RaycastHit r;
    r.hit      = h.hit;
    r.entity   = h.entity;
    r.point    = { h.point[0], h.point[1], h.point[2] };
    r.normal   = { h.normal[0], h.normal[1], h.normal[2] };
    r.distance = h.distance;
    return r;
}
// Two-call string fetch through a (host, buf, cap) → length getter. `host` is
// passed in rather than read off the save table: every table carries its own,
// and a shared helper that reached for one of them would hand the content
// table's getter the save table's context — and return nothing at all whenever
// save happened to be unavailable.
template <typename Fn>
inline std::string fetchString(void* host, Fn fn)
{
    char small[256];
    const int need = fn(host, small, (int)sizeof small);
    if (need < 0) return {};
    if (need < (int)sizeof small) return std::string(small);
    std::string big((size_t)need + 1, '\0');
    fn(host, big.data(), (int)big.size());
    big.resize((size_t)need);
    return big;
}
inline std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size())
    {
        const size_t nl = text.find('\n', start);
        if (nl == std::string::npos)
        {
            if (start < text.size()) out.push_back(text.substr(start));
            break;
        }
        if (nl > start) out.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}
} // namespace detail

namespace save {
inline bool available() { return detail::svc() != nullptr; }
inline bool create(const std::string& id)
{ auto* s = detail::svc(); return s && s->create(s->host, id.c_str()); }
inline bool load(const std::string& id)
{ auto* s = detail::svc(); return s && s->load(s->host, id.c_str()); }
inline bool write()
{ auto* s = detail::svc(); return s && s->write(s->host); }
inline void close()
{ if (auto* s = detail::svc()) s->close(s->host); }
inline bool exists(const std::string& id)
{ auto* s = detail::svc(); return s && s->exists(s->host, id.c_str()); }
inline bool remove(const std::string& id)
{ auto* s = detail::svc(); return s && s->removeSave(s->host, id.c_str()); }
inline std::string activeId()
{ auto* s = detail::svc(); return s ? detail::fetchString(s->host, s->activeId) : std::string(); }
inline std::vector<std::string> list()
{ auto* s = detail::svc(); return s ? detail::splitLines(detail::fetchString(s->host, s->listIds)) : std::vector<std::string>{}; }
inline std::vector<std::string> fields()
{ auto* s = detail::svc(); return s ? detail::splitLines(detail::fetchString(s->host, s->fields)) : std::vector<std::string>{}; }

inline bool setNumber(const std::string& field, float v)
{ auto* s = detail::svc(); return s && s->setNumber(s->host, field.c_str(), v); }
inline float getNumber(const std::string& field, float def = 0.0f)
{ auto* s = detail::svc(); return s ? s->getNumber(s->host, field.c_str(), def) : def; }
inline bool setString(const std::string& field, const std::string& v)
{ auto* s = detail::svc(); return s && s->setString(s->host, field.c_str(), v.c_str()); }
inline std::string getString(const std::string& field)
{
    auto* s = detail::svc();
    if (!s) return {};
    return detail::fetchString(s->host, [&](void* h, char* b, int c){ return s->getString(h, field.c_str(), b, c); });
}
inline bool setBool(const std::string& field, bool v)
{ auto* s = detail::svc(); return s && s->setBool(s->host, field.c_str(), v); }
inline bool getBool(const std::string& field, bool def = false)
{ auto* s = detail::svc(); return s ? s->getBool(s->host, field.c_str(), def) : def; }
inline bool setStructJson(const std::string& field, const std::string& json)
{ auto* s = detail::svc(); return s && s->setStructJson(s->host, field.c_str(), json.c_str()); }
inline std::string getStructJson(const std::string& field)
{
    auto* s = detail::svc();
    if (!s) return {};
    return detail::fetchString(s->host, [&](void* h, char* b, int c){ return s->getStructJson(h, field.c_str(), b, c); });
}
} // namespace save

namespace entity {
inline uint32_t findByName(const std::string& name)
{ auto* s = detail::svc(); return s ? s->findEntityByName(s->host, name.c_str()) : 0u; }
inline bool saveState(uint32_t entity)
{ auto* s = detail::svc(); return s && s->entitySaveState(s->host, entity); }
inline bool hasSavedState(uint32_t entity)
{ auto* s = detail::svc(); return s && s->entityHasSavedState(s->host, entity); }
inline bool applySavedState(uint32_t entity)
{ auto* s = detail::svc(); return s && s->entityApplySavedState(s->host, entity); }
} // namespace entity

// ── Physics ──────────────────────────────────────────────────────────────────
// The one thing to keep in mind while writing gameplay against these: a hit
// reports a WORLD point, setPosition takes a LOCAL one. For an unparented entity
// those are the same value; for a parented one they are not, and "teleport onto
// what I hit" has to account for it.
namespace physics {
inline bool available() { return detail::physSvc() != nullptr; }

inline RaycastHit raycast(const Vec3& origin, const Vec3& dir, float maxDist)
{
    auto* s = detail::physSvc();
    if (!s) return {};
    const float o[3] = { origin.x, origin.y, origin.z };
    const float d[3] = { dir.x, dir.y, dir.z };
    HeRaycastHit hit{};
    s->raycast(s->host, o, d, maxDist, &hit);
    return detail::fromC(hit);
}
inline RaycastHit sphereCast(const Vec3& origin, const Vec3& dir, float radius, float maxDist)
{
    auto* s = detail::physSvc();
    if (!s) return {};
    const float o[3] = { origin.x, origin.y, origin.z };
    const float d[3] = { dir.x, dir.y, dir.z };
    HeRaycastHit hit{};
    s->sphereCast(s->host, o, d, radius, maxDist, &hit);
    return detail::fromC(hit);
}
// Two-call fetch like the string getters: ask, and ask again with room when the
// first buffer was too small.
inline std::vector<uint32_t> overlapSphere(const Vec3& center, float radius)
{
    auto* s = detail::physSvc();
    if (!s) return {};
    const float c[3] = { center.x, center.y, center.z };
    uint32_t small[64];
    const int need = s->overlapSphere(s->host, c, radius, small, (int)(sizeof small / sizeof small[0]));
    if (need <= (int)(sizeof small / sizeof small[0]))
        return std::vector<uint32_t>(small, small + (need < 0 ? 0 : need));
    std::vector<uint32_t> big((size_t)need, 0u);
    s->overlapSphere(s->host, c, radius, big.data(), need);
    return big;
}

inline bool addForce(uint32_t entity, const Vec3& force)
{
    auto* s = detail::physSvc();
    if (!s) return false;
    const float f[3] = { force.x, force.y, force.z };
    return s->addForce(s->host, entity, f);
}
inline bool addImpulse(uint32_t entity, const Vec3& impulse)
{
    auto* s = detail::physSvc();
    if (!s) return false;
    const float i[3] = { impulse.x, impulse.y, impulse.z };
    return s->addImpulse(s->host, entity, i);
}
inline bool addTorque(uint32_t entity, const Vec3& torque)
{
    auto* s = detail::physSvc();
    if (!s) return false;
    const float t[3] = { torque.x, torque.y, torque.z };
    return s->addTorque(s->host, entity, t);
}

inline void setVelocity(uint32_t entity, const Vec3& v)
{
    auto* s = detail::physSvc();
    if (!s) return;
    const float raw[3] = { v.x, v.y, v.z };
    s->setVelocity(s->host, entity, raw);
}
inline Vec3 getVelocity(uint32_t entity)
{
    auto* s = detail::physSvc();
    if (!s) return {};
    float out[3] = { 0.0f, 0.0f, 0.0f };
    s->getVelocity(s->host, entity, out);
    return { out[0], out[1], out[2] };
}
inline bool isGrounded(uint32_t entity)
{ auto* s = detail::physSvc(); return s && s->isGrounded(s->host, entity); }

// `position` is LOCAL — the same space transform.setPosition speaks.
inline bool setPosition(uint32_t entity, const Vec3& position)
{
    auto* s = detail::physSvc();
    if (!s) return false;
    const float p[3] = { position.x, position.y, position.z };
    return s->setPosition(s->host, entity, p);
}
inline bool setPositionAndReset(uint32_t entity, const Vec3& position)
{
    auto* s = detail::physSvc();
    if (!s) return false;
    const float p[3] = { position.x, position.y, position.z };
    return s->setPositionAndReset(s->host, entity, p);
}
inline bool hasPhysics(uint32_t entity)
{ auto* s = detail::physSvc(); return s && s->hasPhysics(s->host, entity); }

inline void setGravity(const Vec3& g)
{
    auto* s = detail::physSvc();
    if (!s) return;
    const float raw[3] = { g.x, g.y, g.z };
    s->setGravity(s->host, raw);
}
inline Vec3 getGravity()
{
    auto* s = detail::physSvc();
    if (!s) return {};
    float out[3] = { 0.0f, 0.0f, 0.0f };
    s->getGravity(s->host, out);
    return { out[0], out[1], out[2] };
}
} // namespace physics

// ── Input ────────────────────────────────────────────────────────────────────
namespace input {
enum class Mode : int { GameOnly = 0, GameAndUI = 1, UIOnly = 2 };

inline bool available() { return detail::inputSvc() != nullptr; }

inline bool keyDown(const std::string& name)
{ auto* s = detail::inputSvc(); return s && s->keyDown(s->host, name.c_str()); }
inline bool mouseButton(int index)
{ auto* s = detail::inputSvc(); return s && s->mouseButton(s->host, index); }
inline Vec2 mousePosition()
{
    auto* s = detail::inputSvc();
    if (!s) return {};
    float out[2] = { 0.0f, 0.0f };
    s->mousePosition(s->host, out);
    return { out[0], out[1] };
}
inline Vec2 mouseDelta()
{
    auto* s = detail::inputSvc();
    if (!s) return {};
    float out[2] = { 0.0f, 0.0f };
    s->mouseDelta(s->host, out);
    return { out[0], out[1] };
}
inline float scrollDelta()
{ auto* s = detail::inputSvc(); return s ? s->scrollDelta(s->host) : 0.0f; }

inline bool gamepadConnected()
{ auto* s = detail::inputSvc(); return s && s->gamepadConnected(s->host); }
inline bool gamepadButton(const std::string& name)
{ auto* s = detail::inputSvc(); return s && s->gamepadButton(s->host, name.c_str()); }
inline float gamepadAxis(const std::string& name)
{ auto* s = detail::inputSvc(); return s ? s->gamepadAxis(s->host, name.c_str()) : 0.0f; }

inline Mode mode()
{ auto* s = detail::inputSvc(); return s ? (Mode)s->mode(s->host) : Mode::GameAndUI; }
// Three named setters rather than one taking a number, for the reason the script
// API gives: "Set Input Mode: UI Only" says what it does, "setMode(2)" does not.
inline void setModeGameOnly()
{ if (auto* s = detail::inputSvc()) s->setMode(s->host, (int)Mode::GameOnly); }
inline void setModeGameAndUI()
{ if (auto* s = detail::inputSvc()) s->setMode(s->host, (int)Mode::GameAndUI); }
inline void setModeUIOnly()
{ if (auto* s = detail::inputSvc()) s->setMode(s->host, (int)Mode::UIOnly); }
} // namespace input

// ── Content ──────────────────────────────────────────────────────────────────
// Residency: get an asset into memory before the moment it is needed, and let
// go of it afterwards. Deliberately NOT access — there is no getMesh here and
// there will not be one. The engine's own accessors return pointers into a pool
// the next load moves, together with every string those assets own; the only
// safe thing to hand a module that the engine cannot see into is a value.
//
// So an AssetId is what you keep. It stays correct across any number of further
// loads, which is exactly what a pointer would not.
namespace content {
inline bool available() { return detail::contentSvc() != nullptr; }

// Load (or return the already-resident) asset at a content-relative path
// ("Meshes/Rock.hasset"). An invalid id (valid() == false) means the path is
// unknown or unreadable — or that no engine injected this table.
inline AssetId load(const std::string& path)
{
    auto* s = detail::contentSvc();
    if (!s) return {};
    ::HeAssetId out{};
    return s->loadAsset(s->host, path.c_str(), &out) ? detail::fromC(out) : AssetId{};
}
// Drop it again. false = it was not loaded.
inline bool unload(const AssetId& id)
{ auto* s = detail::contentSvc(); return s && s->unloadAsset(s->host, detail::toC(id)); }
inline bool isLoaded(const AssetId& id)
{ auto* s = detail::contentSvc(); return s && s->isLoadedId(s->host, detail::toC(id)); }
// "StaticMesh", "Texture", "Material", … — "" for an id the engine does not
// know. The same spelling the editor and the asset headers use.
inline std::string typeName(const AssetId& id)
{
    auto* s = detail::contentSvc();
    if (!s) return {};
    const ::HeAssetId cid = detail::toC(id);
    return detail::fetchString(s->host, [&](void* h, char* b, int c)
    { return s->assetTypeName(h, cid, b, c); });
}
} // namespace content
} // namespace he
