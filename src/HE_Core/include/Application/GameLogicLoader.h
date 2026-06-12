#pragma once
#include "Types/Defines.h"
#include "IGameLogic.h"
#include "Platform/DynLib.h"
#include <filesystem>

class HorizonWorld;

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

private:
    DynLib             lib_;
    IGameLogic*        logic_      = nullptr;
    FnDestroyGameLogic destroyFn_  = nullptr;
};

} // namespace HE
