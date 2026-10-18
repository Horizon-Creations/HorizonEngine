#pragma once

// The folder layout of an exported game (Thema 211 step 10), in one place for
// the exporter that writes it (Hpak/ProjectExporter.cpp), the game that reads it
// (HE_Game) and the backends that load runtime files from it:
//
//   <ProjectName>(.exe)
//   data/             the pak, project.hcfg, config.json, Splash.png, AppIcon.png,
//                     GameLogic, Mods/
//   HorizonEngine/    the engine library, SDL3, the bundled native deps, the
//                     Python plugin, Shaders/
//
// A macOS .app keeps its own split: executable in Contents/MacOS (HorizonEngine/
// beside it), the data in Contents/Resources, which is SDL_GetBasePath there.
// A game in the old flat layout (everything beside the executable) has no data/
// folder, and every lookup below falls back to the base path.

#include <SDL3/SDL_filesystem.h>

#include <filesystem>
#include <system_error>

namespace HE::GameLayout
{

inline constexpr const char* kDataDirName   = "data";
inline constexpr const char* kEngineDirName = "HorizonEngine";

// SDL_GetBasePath (no SDL_Init needed): the executable's directory, or
// Contents/Resources inside a .app. Empty when SDL cannot tell.
inline std::filesystem::path baseDir()
{
    const char* base = SDL_GetBasePath();
    return base ? std::filesystem::path(base) : std::filesystem::path();
}

// Where the shipped data is: <base>/data when the export made one, else the
// base path itself (.app, old flat exports, a dev run from the deploy folder).
inline std::filesystem::path dataDir()
{
    const std::filesystem::path base = baseDir();
    if (base.empty()) return base;
    std::error_code ec;
    const std::filesystem::path data = base / kDataDirName;
    return std::filesystem::is_directory(data, ec) ? data : base;
}

// HorizonEngine/ beside the executable: <base>/HorizonEngine, or
// Contents/MacOS/HorizonEngine inside a .app (base is Contents/Resources there).
// Empty for a game in the old flat layout, which has no such folder.
inline std::filesystem::path engineDir()
{
    std::filesystem::path base = baseDir();
    if (base.empty()) return base;
    if (!base.has_filename()) base = base.parent_path();   // SDL's trailing separator
    std::error_code ec;
    for (const std::filesystem::path& dir :
         { base / kEngineDirName, base.parent_path() / "MacOS" / kEngineDirName })
        if (std::filesystem::is_directory(dir, ec)) return dir;
    return {};
}

} // namespace HE::GameLayout
