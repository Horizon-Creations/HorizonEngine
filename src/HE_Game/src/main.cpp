#include "GameApplication.h"
#include <Diagnostics/GlobalState.h>
#include <Platform/BuildProfile.h>
#include <Platform/EngineProfile.h>
#include <Platform/GameLayout.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

int main(int argc, char** argv)
{
    // The engine library beside this executable must be of this executable's
    // build profile. The SDK ships a dev and a release lib set, and the export
    // swaps a freshly compiled game into a folder that already holds one; a
    // dev game on a release engine (or the reverse) would run with half its
    // dev features compiled out underneath it. Where the two differ in their
    // symbols the loader refuses before main(); this catches the rest. Before
    // anything else, so a mismatched pair writes no settings and opens nothing.
    const char* engineProfile = HorizonEngine_BuildProfile();
    if (!engineProfile || std::strcmp(engineProfile, HE::kBuildProfileName) != 0)
    {
        const std::string msg = std::string("This game was built with the '")
            + HE::kBuildProfileName + "' profile, but the engine library in "
            + HE::GameLayout::kEngineDirName + "/ was built with '"
            + (engineProfile ? engineProfile : "?") + "'. Export the game again.";
        std::fprintf(stderr, "HorizonGame: %s\n", msg.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Engine profile mismatch",
                                 msg.c_str(), nullptr);
        return 3;
    }

    // BEFORE the application object exists, because its base constructor is what
    // reads the settings — and until now it read the per-user file that belongs
    // to the EDITOR on a developer's machine. Every key the export did not write
    // showed through from there, "GameBackend" among them, and an editor that had
    // once exported an application remembered "Software": a game with a clean
    // config.json booted the UI-only rasterizer and drew no scene at all.
    //
    // The data folder (GameLayout::dataDir: data/ beside the executable, or
    // Contents/Resources inside a .app) is where the exporter writes config.json;
    // SDL_GetBasePath behind it needs no SDL_Init. With no base path at all the
    // pin still goes in, on the working directory: a game that cannot find its
    // own settings must run on the built-in defaults, and NOT fall back to a file
    // that belongs to something else.
    const std::filesystem::path data = HE::GameLayout::dataDir();
    GlobalState::useShippedConfig(data.empty() ? std::filesystem::path(".") : data);

    GameApplication app(argv[0]);
    return app.Run(argc, argv);
}
