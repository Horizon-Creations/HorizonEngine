#pragma once
#include "Types/Defines.h"
#include "IGameLogic.h"
#include "Platform/DynLib.h"
#include <filesystem>

class HorizonWorld;
struct HeSaveServices;     // HorizonGameServices.h (global scope, C ABI)
struct HeEngineServices;   //   "  (the umbrella carrying all the tables)

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
    //
    // Deliberately RAW: it neither injects services nor calls onStart. A caller
    // that wants a module which is actually running again wants
    // reloadAndStart() below.
    bool reload(const std::filesystem::path& dllPath, HorizonWorld& world);

    // The whole bring-up in one call: load → injectServices → onStart, in that
    // order, because a game loads its save (or fires a first raycast) in
    // onStart and the tables have to be live by then.
    //
    // Exists because the order is the trap. load() alone leaves an image with
    // NO services: every he::* call in it is a silent no-op with a default
    // return — not a crash, not a log line, just a module that does nothing and
    // looks fine. The editor's "Build and Reload" is the path where that is
    // easiest to get wrong, so the sequence lives here rather than at each
    // call site. `services` may be null (a module that needs nothing).
    bool loadAndStart(const std::filesystem::path& dllPath, HorizonWorld& world,
                      const ::HeEngineServices* services);

    // Hot-swap: onStop the running image, then loadAndStart the new one.
    // Editor only. Returns false if the new library did not load — the old one
    // is gone either way, which is the honest outcome: its code is what was
    // just replaced on disk.
    bool reloadAndStart(const std::filesystem::path& dllPath, HorizonWorld& world,
                        const ::HeEngineServices* services);

    bool         isLoaded()  const;
    IGameLogic*  logic()     const;   // nullptr if not loaded

    // Hand the loaded library its engine-services tables (HorizonGameServices.h)
    // through its optional receiving exports. Call after load() and BEFORE
    // onStart, with tables that outlive the library.
    //
    // The umbrella overload prefers HE_SetEngineServicesV2 and falls back to the
    // v1 HE_SetEngineServices with `services->save` — a library built against an
    // older scaffold keeps its savegame API and reads the rest as unavailable.
    // Returns false only when the library has NEITHER export, which is still a
    // state and not an error: the game side then no-ops with defaults.
    bool injectServices(const ::HeEngineServices* services);
    // v1 form, kept for callers (and tests) that only have a save table.
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
