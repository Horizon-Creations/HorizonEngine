#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ── Engine services for native C++ GameLogic ─────────────────────────────────
// A GameLogic library compiles against <IGameLogic.h> alone and links NOTHING —
// so it cannot call engine functions directly. This header closes that gap for
// the savegame API with a C-ABI function-pointer table the engine INJECTS after
// loading the library:
//
//   1. The game defines the receiving export once (GameLogic.cpp):
//          #include <HorizonGameServices.h>
//          HE_IMPLEMENT_ENGINE_SERVICES()
//   2. The engine fills an HeSaveServices table (HE::api::fillSaveServices) and
//      calls the export right after the library loads — before onStart.
//   3. Game code uses the he::save / he::entity wrappers below (or the raw
//      table). Before injection — or under an engine too old to inject — every
//      wrapper is a safe no-op returning its default, mirroring the script
//      API's loud-failure-not-crash contract (the engine side logs).
//
// ABI rules: plain C types only, strings cross as UTF-8 char* (returns via
// caller buffer + required-length result), the table is versioned and owned by
// the ENGINE (valid for the library's whole lifetime). Additions append new
// pointers and bump HE_SAVE_ABI_VERSION; the receiver rejects a mismatch.

#define HE_SAVE_ABI_VERSION 1u

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

// Defined by HE_IMPLEMENT_ENGINE_SERVICES() in exactly one GameLogic .cpp.
extern const HeSaveServices* g_heSaveServices;

} // extern "C"

// The receiving export — define in exactly ONE .cpp of the GameLogic library.
#define HE_IMPLEMENT_ENGINE_SERVICES() \
    extern "C" { \
    const HeSaveServices* g_heSaveServices = nullptr; \
    HE_GAME_API void HE_SetEngineServices(const HeSaveServices* s) \
    { g_heSaveServices = (s && s->abiVersion == HE_SAVE_ABI_VERSION) ? s : nullptr; } \
    }

// ── Convenience wrappers (game side) ─────────────────────────────────────────
namespace he {
namespace detail {
inline const HeSaveServices* svc() { return g_heSaveServices; }
// Two-call string fetch through a (host, buf, cap) → length getter.
template <typename Fn>
inline std::string fetchString(Fn fn)
{
    const HeSaveServices* s = svc();
    if (!s) return {};
    char small[256];
    const int need = fn(s->host, small, (int)sizeof small);
    if (need < (int)sizeof small) return std::string(small);
    std::string big((size_t)need + 1, '\0');
    fn(s->host, big.data(), (int)big.size());
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
{ auto* s = detail::svc(); return s ? detail::fetchString(s->activeId) : std::string(); }
inline std::vector<std::string> list()
{ auto* s = detail::svc(); return s ? detail::splitLines(detail::fetchString(s->listIds)) : std::vector<std::string>{}; }
inline std::vector<std::string> fields()
{ auto* s = detail::svc(); return s ? detail::splitLines(detail::fetchString(s->fields)) : std::vector<std::string>{}; }

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
    return detail::fetchString([&](void* h, char* b, int c){ return s->getString(h, field.c_str(), b, c); });
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
    return detail::fetchString([&](void* h, char* b, int c){ return s->getStructJson(h, field.c_str(), b, c); });
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
} // namespace he
