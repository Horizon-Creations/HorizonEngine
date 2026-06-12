# HorizonGame – Launcher + Packaged Build Task for GitHub Copilot

## Context

`HorizonGame` is the shipped executable. It is engine-generic — no game-specific code lives here.
Game logic lives in `GameLogic.dll` which is loaded at runtime via `DynLib` (from HorizonCore).

The final packaged output for any Horizon Engine game:
```
MyGame/
├── MyGame.exe            ← this module, renamed per project
├── GameLogic.dll         ← compiled C++ game code (hot-reloadable in editor)
├── MyGame.hpak           ← all assets + scenes, AES-256 encrypted
├── HorizonCore.dll
├── HorizonScene.dll
├── HorizonRendering.dll
├── HorizonAsset.dll
├── HorizonScripting.dll
└── HorizonPhysics.dll
```

In the **editor**, `GameLogic.dll` is hot-reloadable (unload → recompile → reload).
In the **packaged game**, it is loaded once at startup and never unloaded.

---

## Target folder structure

```
HE_Game/
├── CMakeLists.txt
└── src/
    ├── main.cpp
    ├── GameLauncher.h/cpp      ← orchestrates startup sequence
    ├── GameLoop.h/cpp          ← fixed-timestep main loop
    ├── GameLogicLoader.h/cpp   ← DLL loader + hot-reload logic
    └── ProjectConfig.h/cpp     ← reads project.hcfg (name, hpak path, key derivation)
```

---

## Step 1 — ProjectConfig

The launcher reads a small config file (`project.hcfg`) next to the .exe.
This tells it the .hpak filename and provides the salt for key derivation.

Create `src/ProjectConfig.h`:

```cpp
#pragma once
#include <string>
#include <filesystem>
#include <cstdint>

// project.hcfg is a small binary file placed next to MyGame.exe.
// It is NOT encrypted — it contains only non-secret metadata.
// The AES key is derived at runtime from:
//   secret  = HE_PROJECT_SECRET (injected at compile time via CMake define)
//   salt    = projectUuidBytes from this config
struct ProjectConfig {
    std::string  projectName;
    std::string  hpakFilename;       // e.g. "MyGame.hpak"
    std::string  mainSceneName;      // UUID string of the startup scene
    uint8_t      projectUuidBytes[16]; // used as PBKDF2 salt for key derivation
    bool         enableModSupport = false;
};

class ProjectConfigLoader {
public:
    // Reads project.hcfg from the same directory as the executable.
    static bool load(const std::filesystem::path& exeDir, ProjectConfig& outConfig);
    static bool save(const std::filesystem::path& exeDir, const ProjectConfig& config);
};
```

---

## Step 2 — IGameLogic interface (the contract between engine and game DLL)

This interface lives in `HorizonCore` so both `HorizonGame` and `GameLogic.dll` can reference it
without a circular dependency.

Add to `HorizonCore` — create `include/HorizonCore/IGameLogic.h`:

```cpp
// include/HorizonCore/IGameLogic.h
#pragma once
#include <cstdint>

class HorizonWorld;   // forward — GameLogic gets the world injected

// Every game DLL must export a C-compatible factory function:
//   extern "C" IGameLogic* HE_CreateGameLogic();
//   extern "C" void        HE_DestroyGameLogic(IGameLogic*);
//
// Using C linkage prevents name-mangling issues across DLL boundaries.
class IGameLogic {
public:
    virtual ~IGameLogic() = default;

    // Called once after the DLL is loaded and the world is ready.
    virtual void onStart(HorizonWorld& world) = 0;

    // Called every frame. deltaTime is in seconds.
    virtual void onUpdate(HorizonWorld& world, float deltaTime) = 0;

    // Called once before the DLL is unloaded (hot-reload or shutdown).
    virtual void onStop(HorizonWorld& world) = 0;
};

// Typedefs for the DLL export function pointers
using FnCreateGameLogic  = IGameLogic*(*)();
using FnDestroyGameLogic = void(*)(IGameLogic*);
```

---

## Step 3 — GameLogicLoader

Create `src/GameLogicLoader.h`:

```cpp
#pragma once
#include <HorizonCore/IGameLogic.h>
#include <HorizonCore/Platform/DynLib.h>
#include <filesystem>
#include <memory>

// Loads GameLogic.dll and manages the IGameLogic lifecycle.
// In packaged game: load once, never reload.
// In editor (hot-reload): unload → recompile trigger → reload.
class GameLogicLoader {
public:
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
    void*              libHandle_  = nullptr;
    IGameLogic*        logic_      = nullptr;
    FnDestroyGameLogic destroyFn_  = nullptr;
};
```

---

## Step 4 — GameLoop

Fixed-timestep loop with variable rendering. Standard game loop pattern.

Create `src/GameLoop.h`:

```cpp
#pragma once
#include <cstdint>

class HorizonWorld;
class IGameLogic;
class RenderGraph;
class RenderExtractor;
class SceneGraph;

struct GameLoopConfig {
    float    fixedTimestep   = 1.0f / 60.0f;  // physics + logic tick rate
    uint32_t maxFixedSteps   = 5;              // max catch-up steps per frame
    bool     vsync           = true;
};

class GameLoop {
public:
    explicit GameLoop(const GameLoopConfig& config = {});

    // Run until the window is closed or stop() is called.
    // Blocks the calling thread.
    void run(HorizonWorld&    world,
             IGameLogic&      logic,
             SceneGraph&      sceneGraph,
             RenderExtractor& extractor,
             RenderGraph&     renderGraph);

    void stop();

private:
    GameLoopConfig config_;
    bool           running_ = false;

    // Per-frame: accumulate time, run fixed steps, render.
    void tick(HorizonWorld&    world,
              IGameLogic&      logic,
              SceneGraph&      sceneGraph,
              RenderExtractor& extractor,
              RenderGraph&     renderGraph,
              float            deltaTime);
};
```

---

## Step 5 — GameLauncher

Create `src/GameLauncher.h`:

```cpp
#pragma once
#include "ProjectConfig.h"
#include "GameLogicLoader.h"
#include "GameLoop.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneGraph.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonAsset/AssetRegistry.h>
#include <HorizonAsset/AssetLoader.h>
#include <HorizonAsset/HpakReader.h>
#include <memory>

// Orchestrates the full startup → run → shutdown sequence.
class GameLauncher {
public:
    // Returns exit code (0 = success).
    int run(int argc, char** argv);

private:
    // Startup phases — called in order by run()
    bool initConfig   (const std::filesystem::path& exeDir);
    bool initAssets   ();          // open .hpak, populate registry
    bool initRenderer ();          // create IRenderDevice, RenderGraph
    bool initScene    ();          // load main scene from hpak
    bool initLogic    ();          // load GameLogic.dll, call onStart()
    void runLoop      ();          // enter GameLoop::run()
    void shutdown     ();          // reverse order teardown

    ProjectConfig                    config_;
    HpakReader                       hpakReader_;
    std::unique_ptr<AssetRegistry>   assetRegistry_;
    std::unique_ptr<AssetLoader>     assetLoader_;
    std::unique_ptr<HorizonWorld>    world_;
    std::unique_ptr<SceneGraph>      sceneGraph_;
    std::unique_ptr<SceneSerializer> serializer_;
    GameLogicLoader                  logicLoader_;
    GameLoop                         gameLoop_;
};
```

---

## Step 6 — main.cpp

```cpp
// src/main.cpp
#include "GameLauncher.h"

int main(int argc, char** argv) {
    GameLauncher launcher;
    return launcher.run(argc, argv);
}
```

---

## Step 7 — GameLogic.dll template (starter for game developers)

This is the template a game developer fills in. It lives outside the engine — in the game project.
Create this as a comment/documentation block, not an engine file:

```cpp
// GameLogic/src/GameLogic.cpp  (game project, not engine source)
#include <HorizonCore/IGameLogic.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TransformComponent.h>

class MyGame final : public IGameLogic {
public:
    void onStart(HorizonWorld& world) override {
        // spawn entities, set up initial state
    }

    void onUpdate(HorizonWorld& world, float deltaTime) override {
        // game logic per frame
    }

    void onStop(HorizonWorld& world) override {
        // cleanup
    }
};

// C-compatible exports — required by GameLogicLoader
extern "C" IGameLogic* HE_CreateGameLogic()            { return new MyGame(); }
extern "C" void        HE_DestroyGameLogic(IGameLogic* p) { delete p; }
```

---

## Step 8 — CMakeLists.txt

```cmake
# ── HorizonGame executable ───────────────────────────────────────────────────
add_executable(HorizonGame
    src/main.cpp
    src/GameLauncher.cpp
    src/GameLoop.cpp
    src/GameLogicLoader.cpp
    src/ProjectConfig.cpp
)

target_include_directories(HorizonGame PRIVATE src)

target_link_libraries(HorizonGame
    PRIVATE HorizonCore
    PRIVATE HorizonScene
    PRIVATE HorizonRendering
    PRIVATE HorizonAsset
    PRIVATE HorizonScripting
    PRIVATE HorizonPhysics
)

target_compile_definitions(HorizonGame
    PRIVATE HE_PROJECT_SECRET="${HE_PROJECT_SECRET}"  # injected via cmake -DHE_PROJECT_SECRET=...
)

# ── Post-build: copy engine DLLs next to the executable ─────────────────────
add_custom_command(TARGET HorizonGame POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:HorizonCore>
        $<TARGET_FILE:HorizonScene>
        $<TARGET_FILE:HorizonRendering>
        $<TARGET_FILE:HorizonAsset>
        $<TARGET_FILE:HorizonScripting>
        $<TARGET_FILE:HorizonPhysics>
        $<TARGET_FILE_DIR:HorizonGame>
    COMMENT "Copying engine DLLs to output directory"
)
```

---

## Hot-reload flow (editor only)

```
Editor detects source file change
    → triggers recompile of GameLogic project
    → on success: GameLogicLoader::reload(dllPath, world)
        → calls logic->onStop(world)
        → unloads old DLL (FreeLibrary / dlclose)
        → copies new .dll to a temp filename (avoids file lock)
        → loads new DLL
        → calls logic->onStart(world)
    → game continues from current world state
```

The temp-copy step is critical on Windows: `LoadLibrary` locks the .dll file,
so the compiler cannot overwrite it while it is loaded. Copy to e.g.
`GameLogic_hot_0.dll`, `GameLogic_hot_1.dll` alternating, then load the copy.

---

## Key derivation at runtime

```
HorizonGame startup:
    1. Load project.hcfg → read projectUuidBytes (salt)
    2. Read HE_PROJECT_SECRET from compile-time define
    3. Call KeyDerivation::derive(secret, salt, outKey)
    4. Call HpakReader::open(hpakPath, outKey)
    5. Proceed — all asset reads are transparently decrypted
```

The secret never appears in the .hpak or project.hcfg.
For CI/CD: pass `-DHE_PROJECT_SECRET=...` as a cmake argument in the pipeline,
never commit it to source control.

---

## Notes for Copilot

- `GameLogicLoader` uses `DynLib` from `HorizonCore/Platform/DynLib.h` —
  implement `DynLib` as a thin wrapper around `LoadLibrary`/`GetProcAddress`
  on Windows and `dlopen`/`dlsym` on Linux/Mac.
- `IGameLogic` must use `extern "C"` factory functions to avoid C++ ABI issues
  across DLL boundaries (different compilers, different STL versions).
  Never pass `std::string` or STL containers across the DLL boundary directly.
- `GameLoop` uses a fixed-timestep accumulator pattern:
  accumulate delta time, consume in fixedTimestep chunks calling `onUpdate`,
  render once with the remainder as interpolation alpha.
- `HorizonGame` must NOT contain any game-specific `#include` or logic.
  All game behaviour flows through `IGameLogic`.
- `project.hcfg` format: write as a simple binary struct with a magic header
  `0x48 0x43 0x46 0x47` ("HCFG") + version uint32_t + fixed-size fields.
  Keep it simple — this file is generated by HorizonEditor's export step.
- Apply `HE_API` only to classes in shared libs, not to `HorizonGame` internals
  (it is an executable, not a DLL).
