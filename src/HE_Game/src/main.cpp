#include "GameApplication.h"
#include <Diagnostics/GlobalState.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <filesystem>

int main(int argc, char** argv)
{
    // BEFORE the application object exists, because its base constructor is what
    // reads the settings — and until now it read the per-user file that belongs
    // to the EDITOR on a developer's machine. Every key the export did not write
    // showed through from there, "GameBackend" among them, and an editor that had
    // once exported an application remembered "Software": a game with a clean
    // config.json booted the UI-only rasterizer and drew no scene at all.
    //
    // SDL_GetBasePath needs no SDL_Init and resolves to Contents/Resources inside
    // a .app — the directory the exporter writes config.json into. With no base
    // path at all the pin still goes in, on the working directory: a game that
    // cannot find its own settings must run on the built-in defaults, and NOT
    // fall back to a file that belongs to something else.
    const char* base = SDL_GetBasePath();
    GlobalState::useShippedConfig(std::filesystem::path(base ? base : "."));

    GameApplication app(argv[0]);
    return app.Run(argc, argv);
}
