#pragma once
#include "Types/Defines.h"
#include "IGameLogic.h"
#include "Platform/DynLib.h"
#include <filesystem>

class HorizonWorld;
struct HeSaveServices;   // HorizonGameServices.h (global scope, C ABI)

namespace HE {

// Loads GameLogic.dll and manages the IGameLogic lifecycle.
// In packaged game: load once, never reload.
// In editor (hot-reload): unload → recompile trigger → reload.
class HE_API GameLogicLoader {
public:
    GameLogicLoader();
    ~GameLogicLoader();

    GameLogicLoader(const GameLogicLoader&)            = delete;
    GameLogicLoader& operator=(const GameLogicLoader&) = delete;

    // Load the DLL. Returns false if not found or missing exports.
    bool load(const std::filesystem::path& dllPath);

    // Unload the DLL. Calls onStop() first if logic is running.
    // Safe to call even if not loaded.
    void unload(HorizonWorld& world);

    // Hot-reload: unload + load in one step.
    // Editor only — do not call in packaged builds.
    bool reload(const std::filesystem::path& dllPath, HorizonWorld& world);

    bool         isLoaded()  const;
    IGameLogic*  logic()     const;   // nullptr if not loaded

    // Hand the loaded library its engine-services table (HorizonGameServices.h)
    // through its optional HE_SetEngineServices export. Call after load() and
    // BEFORE onStart, with a table that outlives the library. Returns false when
    // the library predates the export (older scaffold) — save APIs then read as
    // unavailable on the game side, which is a state, not an error.
    bool injectServices(const ::HeSaveServices* services);

private:
    DynLib                m_lib;
    IGameLogic*           m_logic     = nullptr;
    FnDestroyGameLogic    m_destroyFn = nullptr;
    // The uniquely-named hot-copy actually dlopen'ed (see load()); removed on
    // unload. Empty when the original path was loaded directly.
    std::filesystem::path m_loadedCopyPath;
};

} // namespace HE
