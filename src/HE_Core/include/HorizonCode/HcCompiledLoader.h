#pragma once
#include <Types/Defines.h>
#include "HorizonCodeCompiled.h"
#include <Platform/DynLib.h>
#include <filesystem>
#include <string>
#include <unordered_map>

// ── HorizonCode::CompiledClassTable ──────────────────────────────────────────
// Loads the export-generated HorizonCodeGen.<dylib|dll|so> (beside
// GameLogicLoader, reusing DynLib), resolves its C manifest export
//
//   extern "C" const CompiledClassEntry* HE_HorizonCodeGenClasses(int*, const char**);
//
// checks the baked engineVersion against the running engine's, and serves
// key → factory lookups to the four hosts (GameInstance, createObject,
// WidgetManager, level scripts). A missing/rejected library is NOT an error —
// every lookup then misses and the graphs run interpreted (per-asset hybrid).
// The library stays loaded for process lifetime; packaged builds never
// hot-reload it.

namespace HorizonCode {

class HE_API CompiledClassTable
{
public:
    // Load + validate the manifest. Returns false (and stays empty) when the
    // file is missing, exports don't resolve, or the version doesn't match —
    // each with its own log line so a misconfiguration is loud.
    bool load(const std::filesystem::path& libPath, const std::string& engineVersion);

    const CompiledClassEntry* find(const std::string& key) const;
    // Instantiate `key`, or a null CompiledPtr when the table has no entry
    // (= the caller's cue to fall back to the interpreter).
    CompiledPtr create(const std::string& key) const;

    bool   loaded() const { return m_lib.isLoaded(); }
    size_t size()   const { return m_entries.size(); }

private:
    HE::DynLib m_lib;
    // Values point into the dylib's static manifest — valid for process
    // lifetime because the library is never unloaded.
    std::unordered_map<std::string, const CompiledClassEntry*> m_entries;
};

// The process-wide table. The packaged game loads it once at startup; in the
// editor it is never loaded, so every lookup misses and scripts run
// interpreted — the hosts (createObject, WidgetManager, level scripts) consult
// it without any plumbing.
HE_API CompiledClassTable& compiledClasses();

// The platform's artifact filename ("libHorizonCodeGen.dylib" / ".so",
// "HorizonCodeGen.dll") — shared by the exporter (artifact normalization) and
// the game (load probe) so the two can't drift.
HE_API const char* compiledLibraryName();

} // namespace HorizonCode
