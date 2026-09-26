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
#define HE_INPUT_ABI_VERSION    4u   // 2 — + rumble, rumbleTriggers, stopRumble
                                     // 3 — + rebindBegin … saveBindings
                                     // 4 — + setStickDeadzone, stickDeadzone
#define HE_CONTENT_ABI_VERSION  1u
#define HE_ANTICHEAT_ABI_VERSION 1u
// The umbrella that carries the tables. Bumped when a table POINTER is appended
// to HeEngineServices, not when a table itself grows.
//   1 — save, physics, input
//   2 — + content
//   3 — + anticheat
#define HE_NET_ABI_VERSION 1u
#define HE_SERVICES_ABI_VERSION 4u

// Export decoration for the receiving symbol — same rule as <IGameLogic.h>,
// defined here too so this header stands alone (e.g. in tests).
#ifndef HE_GAME_API
#  ifdef _WIN32
#    define HE_GAME_API __declspec(dllexport)
#  elif defined(__GNUC__) || defined(__clang__)
#    define HE_GAME_API __attribute__((visibility("default")))
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

    // ── v2 ──
    // Rumble, every connected pad. Intensities 0..1, duration in seconds
    // (<= 0: until stopRumble). One effect per pad, a call replaces it. The
    // one entry in this table that writes to a device rather than reading a
    // snapshot — gated by the host exactly like the script rows (nothing
    // outside a running game, stopped on pause), so a module cannot outlive it.
    bool  (*rumble)(void* host, float low, float high, float duration);
    bool  (*rumbleTriggers)(void* host, float left, float right, float duration);
    void  (*stopRumble)(void* host);

    // ── v3 ──
    // The player's own bindings (HE::api::input::rebindBegin …). `device` is
    // "keyboard" (keys + mouse buttons) or "gamepad". The string getters are
    // two-call like the save table's: up to `cap` bytes incl. the NUL, the
    // FULL length returned.
    bool  (*rebindBegin)(void* host, const char* action, const char* device);
    void  (*rebindCancel)(void* host);
    bool  (*isRebinding)(void* host);
    int   (*rebindConflict)(void* host, char* buf, int cap);
    int   (*bindingName)(void* host, const char* action, const char* device, char* buf, int cap);
    void  (*resetBindings)(void* host);
    bool  (*saveBindings)(void* host);

    // ── v4 ──
    // The player's stick deadzone (HE::api::input::setStickDeadzone), 0..0.9.
    // Saved with the other player settings, which have no table of their own
    // yet (camera/app/audio are not C-ABI groups).
    void  (*setStickDeadzone)(void* host, float deadzone);
    float (*stickDeadzone)(void* host);
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

// The host's anti-cheat, game side (docs/anti-cheat-plan.md §4.3): declare a
// legitimate teleport, report what the engine cannot see, answer a report the
// engine made (IGameLogic::onCheatDetected hands over its ticket), and read the
// ticket's fields. Players and entities are the ids the engine uses on the
// wire — a ConnectionId, a network id — never pointers.
//
// `check` is the one row whose "no engine injected" answer is TRUE: a check
// the engine cannot make must not block the game (the wrapper below says so
// too). Everything else defaults to nothing, like the other tables.
typedef struct HeAntiCheatServices
{
    uint32_t abiVersion;   // HE_ANTICHEAT_ABI_VERSION
    void*    host;         // opaque engine context — pass to every call

    int   (*check)(void* host, const char* rule, float value, uint32_t player);
    void  (*expectDisplacement)(void* host, uint32_t entity, float maxDistance);
    void  (*report)(void* host, uint32_t player, const char* rule, float weight, const char* detail);
    void  (*setPlayerLabel)(void* host, uint32_t player, const char* label);
    // Replace the pending report's responses (bits: 1 log, 2 event, 4 telemetry,
    // 8 flag, 16 kick, 32 ban). Only inside onCheatDetected's frame.
    void  (*respond)(void* host, int reportId, int response);
    void  (*kick)(void* host, uint32_t player, int reasonCode);
    int   (*reportLevel)(void* host, int reportId);     // 0 Info … 3 Hard; 0 unknown
    int   (*reportRule)(void* host, int reportId, char* buf, int cap);     // two-call string
    uint32_t (*reportPlayer)(void* host, int reportId);
    uint32_t (*reportEntity)(void* host, int reportId);
    float (*reportScore)(void* host, int reportId);
    int   (*reportDetail)(void* host, int reportId, char* buf, int cap);   // two-call string
    int   (*reportReason)(void* host, int reportId);
    float (*playerScore)(void* host, uint32_t player);
    bool  (*isEnabled)(void* host);
} HeAntiCheatServices;

// ── Multiplayer: remote calls (docs/gameplay-replication-plan.md §7) ─────────
// "Run this function over there", from native game code. The counterpart of
// IGameLogic::onRpc, which is how one ARRIVES.
//
// Arguments travel as a JSON ARRAY string, the same trade every other wide
// value in this header makes: a HorizonCode::Value is a C++ type with strings
// and vectors in it, and this boundary is a hot-loaded dylib rebuilt on its own
// schedule. "[]" and null both mean no arguments.
//
// No return values, ever — an RPC is fire-and-forget (§7.2).
typedef struct HeNetServices
{
    uint32_t abiVersion;   // HE_NET_ABI_VERSION
    void*    host;         // opaque engine context — pass to every call

    bool  (*callServer)(void* host, uint32_t entity, const char* fn, const char* argsJson);
    bool  (*callClient)(void* host, uint32_t player, uint32_t entity, const char* fn,
                        const char* argsJson);
    bool  (*callAllClients)(void* host, uint32_t entity, const char* fn, const char* argsJson);
    // May a client that does not own this entity call `fn` on it? The
    // HorizonCode twin is a checkbox at the function header; a native class has
    // no header, so it says it here.
    bool  (*allowAnyClient)(void* host, uint32_t entity, const char* fn);
    // Who asked for the call being delivered right now. 0 at any other moment.
    uint32_t (*rpcSender)(void* host);
    // The session, for the handful of questions a handler actually asks.
    bool  (*isAuthority)(void* host);
    uint32_t (*localPlayer)(void* host);
} HeNetServices;

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
    const HeAntiCheatServices* anticheat;  // umbrella v3
    const HeNetServices*       net;        // umbrella v4
} HeEngineServices;

typedef void (*FnSetEngineServicesV2)(const HeEngineServices*);

// Defined by HE_IMPLEMENT_ENGINE_SERVICES() in exactly one GameLogic .cpp.
extern const HeSaveServices*    g_heSaveServices;
extern const HePhysicsServices* g_hePhysicsServices;
extern const HeInputServices*   g_heInputServices;
extern const HeContentServices* g_heContentServices;
extern const HeAntiCheatServices* g_heAntiCheatServices;
extern const HeNetServices*       g_heNetServices;

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
    const HeAntiCheatServices* g_heAntiCheatServices = nullptr; \
    const HeNetServices*       g_heNetServices       = nullptr; \
    HE_GAME_API void HE_SetEngineServices(const HeSaveServices* s) \
    { g_heSaveServices = (s && s->abiVersion >= HE_SAVE_ABI_VERSION) ? s : nullptr; } \
    HE_GAME_API void HE_SetEngineServicesV2(const HeEngineServices* s) \
    { \
        const bool v1 = s && s->abiVersion >= 1u; \
        const bool v2 = s && s->abiVersion >= 2u; \
        const bool v3 = s && s->abiVersion >= 3u; \
        const bool v4 = s && s->abiVersion >= 4u; \
        g_heSaveServices = (v1 && s->save && \
            s->save->abiVersion >= HE_SAVE_ABI_VERSION) ? s->save : nullptr; \
        g_hePhysicsServices = (v1 && s->physics && \
            s->physics->abiVersion >= HE_PHYSICS_ABI_VERSION) ? s->physics : nullptr; \
        g_heInputServices = (v1 && s->input && \
            s->input->abiVersion >= HE_INPUT_ABI_VERSION) ? s->input : nullptr; \
        g_heContentServices = (v2 && s->content && \
            s->content->abiVersion >= HE_CONTENT_ABI_VERSION) ? s->content : nullptr; \
        g_heAntiCheatServices = (v3 && s->anticheat && \
            s->anticheat->abiVersion >= HE_ANTICHEAT_ABI_VERSION) ? s->anticheat : nullptr; \
        g_heNetServices = (v4 && s->net && \
            s->net->abiVersion >= HE_NET_ABI_VERSION) ? s->net : nullptr; \
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
    // Spelled out rather than `= default`: a generated C++ game project builds
    // at C++17 (CppScaffold::cmakeLists), where defaulted comparison does not
    // exist and != is not synthesised from ==. This header has to compile on the
    // standard the scaffold hands out, not on the one the engine uses.
    bool operator==(const AssetId& o) const { return hi == o.hi && lo == o.lo; }
    bool operator!=(const AssetId& o) const { return !(*this == o); }
};

// ── Why every wrapper below is HIDDEN ────────────────────────────────────────
// These are inline functions in a header, so each image that uses one emits its
// own out-of-line copy as a WEAK definition. dyld coalesces weak definitions
// across images: if the host executable also contains this header — and a host
// that runs an in-process test of the same wrappers does — then the dlopen'd
// game module ends up calling the HOST's copy of detail::svc(), which reads the
// HOST's g_heSaveServices. Those are never injected: the loader writes into the
// module's globals, and the module's own accessors are the ones nobody calls.
// The result is silent and total — every he::* call in the module returns its
// "no engine injected" default while the tables sit right there, correctly
// filled, one symbol away.
//
// Hidden visibility ends it at the source: a hidden weak definition is not a
// candidate for coalescing, so each image resolves to the copy that sits next to
// its own globals. It belongs in the header rather than in a build file because
// the constraint is the header's, not any one project's — a generated C++ game
// project (CppScaffold::cmakeLists) sets no visibility flags at all, and this
// has to hold there too. Nothing here is meant to cross a library boundary: the
// exported surface is HE_CreateGameLogic and HE_SetEngineServices*, and those
// carry HE_GAME_API.
//
// Windows needs none of this — a DLL never coalesces with its host — and MSVC
// warns (C4068) about the pragma it does not know, so it stays behind the guard.
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility push(hidden)
#endif

namespace detail {
inline const HeSaveServices*    svc()        { return g_heSaveServices; }
inline const HePhysicsServices* physSvc()    { return g_hePhysicsServices; }
inline const HeInputServices*   inputSvc()   { return g_heInputServices; }
inline const HeContentServices* contentSvc() { return g_heContentServices; }
inline const HeAntiCheatServices* antiCheatSvc() { return g_heAntiCheatServices; }
inline const HeNetServices*       netSvc()      { return g_heNetServices; }
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

// Rumble every connected pad: `low` heavy motor, `high` light motor, 0..1;
// `duration` in seconds, <= 0 until stopRumble(). A call replaces the running
// rumble. False when no pad took it — or outside a running game, where the
// host keeps the pads quiet.
inline bool rumble(float low, float high, float duration)
{ auto* s = detail::inputSvc(); return s && s->rumble && s->rumble(s->host, low, high, duration); }
// Trigger motors (Xbox One/Series, DualSense); false on every other pad.
inline bool rumbleTriggers(float left, float right, float duration)
{
    auto* s = detail::inputSvc();
    return s && s->rumbleTriggers && s->rumbleTriggers(s->host, left, right, duration);
}
inline void stopRumble()
{ if (auto* s = detail::inputSvc(); s && s->stopRumble) s->stopRumble(s->host); }

// The player's own bindings. `device` "keyboard" (keys + mouse buttons) or
// "gamepad". rebindBegin listens for the next press on that device (Escape or
// Start cancels); poll isRebinding() for the end, then rebindConflict() for the
// other actions that input also triggers ("" none, else comma-separated).
// resetBindings drops them all, saveBindings persists them for next launch.
inline bool rebindBegin(const std::string& action, const std::string& device)
{
    auto* s = detail::inputSvc();
    return s && s->rebindBegin && s->rebindBegin(s->host, action.c_str(), device.c_str());
}
inline void rebindCancel()
{ if (auto* s = detail::inputSvc(); s && s->rebindCancel) s->rebindCancel(s->host); }
inline bool isRebinding()
{ auto* s = detail::inputSvc(); return s && s->isRebinding && s->isRebinding(s->host); }
inline std::string rebindConflict()
{
    auto* s = detail::inputSvc();
    return s && s->rebindConflict ? detail::fetchString(s->host, s->rebindConflict) : std::string();
}
inline std::string bindingName(const std::string& action, const std::string& device)
{
    auto* s = detail::inputSvc();
    if (!s || !s->bindingName) return {};
    return detail::fetchString(s->host, [&](void* h, char* b, int c)
        { return s->bindingName(h, action.c_str(), device.c_str(), b, c); });
}
inline void resetBindings()
{ if (auto* s = detail::inputSvc(); s && s->resetBindings) s->resetBindings(s->host); }
inline bool saveBindings()
{ auto* s = detail::inputSvc(); return s && s->saveBindings && s->saveBindings(s->host); }

// The player's stick deadzone, 0..0.9 — what a settings menu's slider sets.
// Applied from the next frame; persisted by the engine's settings save.
inline void setStickDeadzone(float deadzone)
{ if (auto* s = detail::inputSvc(); s && s->setStickDeadzone) s->setStickDeadzone(s->host, deadzone); }
inline float stickDeadzone()
{ auto* s = detail::inputSvc(); return s && s->stickDeadzone ? s->stickDeadzone(s->host) : 0.15f; }

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

// ── Anti-cheat ───────────────────────────────────────────────────────────────
// The host's anti-cheat from native game code (docs/anti-cheat-plan.md §4.3).
// A report arrives as IGameLogic::onCheatDetected(reportId); the readers here
// open it. Levels: 0 Info, 1 Suspect, 2 Confirmed, 3 Hard.
//
// The ONE inverted default in this header: `check` answers TRUE when nothing
// was injected. A check the engine cannot make must never block the game — a
// client, a session with anti-cheat off, a module under an older engine all
// still have to apply the damage they were asked about.
namespace anticheat {
enum class Level : int { Info = 0, Suspect = 1, Confirmed = 2, Hard = 3 };
enum Response : int
{ Log = 1, Event = 2, Telemetry = 4, Flag = 8, Kick = 16, Ban = 32 };

inline bool available() { return detail::antiCheatSvc() != nullptr; }

inline bool check(const std::string& rule, float value, uint32_t player)
{ auto* s = detail::antiCheatSvc(); return s ? s->check(s->host, rule.c_str(), value, player) != 0 : true; }
inline void expectDisplacement(uint32_t entity, float maxDistance)
{ if (auto* s = detail::antiCheatSvc()) s->expectDisplacement(s->host, entity, maxDistance); }
inline void report(uint32_t player, const std::string& rule, float weight,
                   const std::string& detailText = std::string())
{ if (auto* s = detail::antiCheatSvc()) s->report(s->host, player, rule.c_str(), weight, detailText.c_str()); }
inline void setPlayerLabel(uint32_t player, const std::string& label)
{ if (auto* s = detail::antiCheatSvc()) s->setPlayerLabel(s->host, player, label.c_str()); }
// Replace what the host would do for this report — the whole set, Log always
// included, so respond(id, 0) is "log only". Inside onCheatDetected's frame.
inline void respond(int reportId, int responses)
{ if (auto* s = detail::antiCheatSvc()) s->respond(s->host, reportId, responses); }
inline void kick(uint32_t player, int reasonCode)
{ if (auto* s = detail::antiCheatSvc()) s->kick(s->host, player, reasonCode); }

inline Level reportLevel(int reportId)
{ auto* s = detail::antiCheatSvc(); return s ? (Level)s->reportLevel(s->host, reportId) : Level::Info; }
inline std::string reportRule(int reportId)
{
    auto* s = detail::antiCheatSvc();
    if (!s) return {};
    return detail::fetchString(s->host, [&](void* h, char* b, int c)
    { return s->reportRule(h, reportId, b, c); });
}
inline uint32_t reportPlayer(int reportId)
{ auto* s = detail::antiCheatSvc(); return s ? s->reportPlayer(s->host, reportId) : 0u; }
inline uint32_t reportEntity(int reportId)
{ auto* s = detail::antiCheatSvc(); return s ? s->reportEntity(s->host, reportId) : 0u; }
inline float reportScore(int reportId)
{ auto* s = detail::antiCheatSvc(); return s ? s->reportScore(s->host, reportId) : 0.0f; }
inline std::string reportDetail(int reportId)
{
    auto* s = detail::antiCheatSvc();
    if (!s) return {};
    return detail::fetchString(s->host, [&](void* h, char* b, int c)
    { return s->reportDetail(h, reportId, b, c); });
}
inline int reportReason(int reportId)
{ auto* s = detail::antiCheatSvc(); return s ? s->reportReason(s->host, reportId) : 0; }
inline float playerScore(uint32_t player)
{ auto* s = detail::antiCheatSvc(); return s ? s->playerScore(s->host, player) : 0.0f; }
inline bool isEnabled()
{ auto* s = detail::antiCheatSvc(); return s && s->isEnabled(s->host); }
} // namespace anticheat

// ── Multiplayer: remote calls ────────────────────────────────────────────────
// "Run this function over there" from a native module
// (docs/gameplay-replication-plan.md §7). The counterpart of
// IGameLogic::onRpc, which is how one arrives.
//
// `argsJson` is a JSON ARRAY of the arguments, in call order — the shape
// HE::Net::Game::argsToJson produces and onRpc hands you. Omitting it means no
// arguments. No return values: an RPC is fire-and-forget.
//
// Every row defaults to FALSE with nothing injected, which is the truth about
// it: no engine, no session, nothing ran anywhere. The one exception is
// isAuthority, which answers TRUE — a module with no session under it is its
// own authority, exactly as net.isAuthority does for a graph, so the same
// "only the authority simulates" guard works offline.
namespace net {
inline bool available() { return detail::netSvc() != nullptr; }

inline bool callServer(uint32_t entity, const std::string& fn,
                       const std::string& argsJson = "[]")
{ auto* s = detail::netSvc(); return s && s->callServer(s->host, entity, fn.c_str(), argsJson.c_str()); }
inline bool callClient(uint32_t player, uint32_t entity, const std::string& fn,
                       const std::string& argsJson = "[]")
{ auto* s = detail::netSvc(); return s && s->callClient(s->host, player, entity, fn.c_str(), argsJson.c_str()); }
inline bool callAllClients(uint32_t entity, const std::string& fn,
                           const std::string& argsJson = "[]")
{ auto* s = detail::netSvc(); return s && s->callAllClients(s->host, entity, fn.c_str(), argsJson.c_str()); }
inline bool allowAnyClient(uint32_t entity, const std::string& fn)
{ auto* s = detail::netSvc(); return s && s->allowAnyClient(s->host, entity, fn.c_str()); }
inline uint32_t rpcSender()
{ auto* s = detail::netSvc(); return s ? s->rpcSender(s->host) : 0u; }
inline bool isAuthority()
{ auto* s = detail::netSvc(); return s ? s->isAuthority(s->host) : true; }
inline uint32_t localPlayer()
{ auto* s = detail::netSvc(); return s ? s->localPlayer(s->host) : 1u; }
} // namespace net

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility pop
#endif
} // namespace he
