#include "ExportSdkBuild.h"

#include <Hpak/GameBuildConfig.h>
#include <Hpak/ProjectConfig.h>
#include <Hpak/ProjectExporter.h>     // readEmbeddedPakKey
#include <Project/ProjectSettings.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <system_error>

namespace HE::ExportSdk
{
namespace fs = std::filesystem;

namespace
{
GameSdk sdkAt(const fs::path& root, const std::string& profile)
{
    std::error_code ec;
    if (root.empty()) return {};
    GameSdk sdk;
    sdk.packageDir  = root / "lib" / "cmake" / "HorizonEngine";
    sdk.templateDir = root / "share" / "HorizonEngine" / "template";
    // One SDK holds a lib set per profile (cmake/HorizonSDK.cmake); the one
    // asked for must be installed. Whether its header stamp still matches is
    // the package's call: a refusal there fails the configure, and the
    // prebuilt game stays.
    if (!fs::is_regular_file(sdk.packageDir / "HorizonEngineConfig.cmake", ec) ||
        !fs::is_regular_file(sdk.packageDir / ("HorizonEngineTargets-" + profile + ".cmake"), ec) ||
        !fs::is_regular_file(sdk.templateDir / "CMakeLists.txt", ec))
        return {};
    sdk.root    = root;
    sdk.profile = profile;
    return sdk;
}

// What the game executable and its engine libraries are called on disk.
bool isEngineLibrary(const fs::path& p)
{
    const std::string n = p.filename().string();
    const std::string e = p.extension().string();
    if (e == ".dylib" || e == ".dll") return true;
    // libHorizonCore.so, libSDL3.so.0, libSDL3.so.0.2.10
    return n.find(".so") != std::string::npos && n.rfind("lib", 0) == 0;
}
} // namespace

GameSdk findGameSdk(const fs::path& editorBase, const std::string& profile,
                    const fs::path& overrideRoot)
{
    if (!overrideRoot.empty()) return sdkAt(overrideRoot, profile);
    const char* dir = kGameSdkDirName;
    if (GameSdk s = sdkAt(editorBase / dir, profile); s.valid()) return s;
    if (!editorBase.empty())
    {
        // SDL_GetBasePath ends in a separator: parent_path() of ".../Editor/" is
        // ".../Editor", so strip the empty last element first.
        fs::path b = editorBase;
        if (!b.has_filename()) b = b.parent_path();
        if (GameSdk s = sdkAt(b.parent_path() / dir, profile); s.valid()) return s;
    }
    return {};
}

std::string unavailableReason(const GameSdk& sdk, const std::string& profile,
                              bool cmakeFound, bool compilerFound)
{
    if (!sdk.valid())
        return "no engine SDK with the '" + profile + "' lib set (" + kGameSdkDirName +
               " beside the editor, or HE_GAME_SDK_DIR)";
    if (!cmakeFound)    return "cmake not found";
    if (!compilerFound) return "no working C++ compiler found";
    return {};
}

std::string writeBuildConfigFromExport(const fs::path& dataDir, const fs::path& prebuiltExe,
                                       const std::string& profile, const fs::path& header)
{
    ProjectConfig cfg;
    if (!ProjectConfigLoader::load(dataDir, cfg))
        return "cannot read project.hcfg in " + dataDir.string();
    if (cfg.encrypted &&
        std::all_of(std::begin(cfg.encKey), std::end(cfg.encKey), [](uint8_t b) { return b == 0; }))
    {
        // The exporter patched the key into the prebuilt executable and left the
        // hcfg without it: the compiled game must get it from the header instead.
        if (prebuiltExe.empty() || !readEmbeddedPakKey(prebuiltExe, cfg.encKey))
            return "the pak key is neither in project.hcfg nor in the prebuilt game";
    }
    ProjectSettings settings;
    if (!loadProjectSettings(dataDir, settings))
        return "cannot read " + projectSettingsPath(dataDir).string();
    if (!writeGameBuildConfigHeader(header, cfg, &settings, profile.c_str()))
        return "cannot write " + header.string();
    return {};
}

HE::hccg::DylibBuildSpec makeBuildSpec(const GameSdk& sdk, const fs::path& workDir,
                                       const fs::path& header)
{
    HE::hccg::DylibBuildSpec spec;
    spec.sourceDir     = sdk.templateDir;
    spec.buildDir      = workDir / "build";
    spec.logFile       = workDir / "build.log";
    spec.artifactNames = { "HorizonGame", "HorizonGame.exe" };
    spec.what          = "game executable";
    spec.defines       = {
        "HorizonEngine_DIR=" + sdk.packageDir.generic_string(),
        "HE_BUILD_PROFILE=" + sdk.profile,
        "HE_GAME_NAME=HorizonGame",
        "HE_GAME_BUILD_CONFIG=" + header.generic_string(),
    };
    return spec;
}

std::string installBuiltGame(const GameSdk& sdk, const fs::path& builtExe,
                             const fs::path& exportedExe, std::vector<std::string>* copied)
{
    std::error_code ec;
    if (exportedExe.empty()) return "the export shipped no game executable to replace";
    const fs::path dstDir = exportedExe.parent_path();
    fs::copy_file(builtExe, exportedExe, fs::copy_options::overwrite_existing, ec);
    if (ec) return "cannot copy " + builtExe.string() + ": " + ec.message();
    if (copied) copied->push_back(exportedExe.filename().string());
    for (const char* sub : { "lib", "bin" })
    {
        const fs::path dir = sdk.root / sub / sdk.profile;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& e : fs::directory_iterator(dir, ec))
        {
            // Symlinks (libSDL3.dylib -> libSDL3.0.dylib) are copied as the file
            // they name: the export folder holds plain files, like the prebuilt one.
            if (!e.is_regular_file(ec) || !isEngineLibrary(e.path())) continue;
            fs::copy_file(e.path(), dstDir / e.path().filename(),
                          fs::copy_options::overwrite_existing, ec);
            if (ec) return "cannot copy " + e.path().string() + ": " + ec.message();
            if (copied) copied->push_back(e.path().filename().string());
        }
    }
    return {};
}
int relinkToBundledDeps(const fs::path& dir, const std::vector<std::string>& files)
{
    int rewritten = 0;
#if defined(__APPLE__)
    const auto q = [](const fs::path& p) { return "'" + p.string() + "'"; };
    for (const std::string& name : files)
    {
        const fs::path f = dir / name;
        FILE* pipe = popen(("otool -L " + q(f) + " 2>/dev/null").c_str(), "r");
        if (!pipe) continue;
        std::vector<std::string> deps;
        char buf[1024];
        while (std::fgets(buf, sizeof buf, pipe))
        {
            std::string line(buf);
            if (line.empty() || (line[0] != '\t' && line[0] != ' ')) continue;   // "<file>:" header
            line.erase(0, line.find_first_not_of(" \t"));
            line = line.substr(0, line.find(" ("));
            if (line.rfind("/opt/homebrew/", 0) == 0 || line.rfind("/usr/local/", 0) == 0)
                deps.push_back(line);
        }
        pclose(pipe);
        bool changed = false;
        for (const std::string& dep : deps)
        {
            const std::string base = fs::path(dep).filename().string();
            std::error_code ec;
            if (!fs::is_regular_file(dir / base, ec)) continue;
            const std::string cmd = "install_name_tool -change '" + dep + "' '@rpath/" + base + "' " +
                                    q(f) + " 2>/dev/null";
            if (std::system(cmd.c_str()) == 0) { ++rewritten; changed = true; }
        }
        if (changed)
            std::system(("codesign --force --sign - " + q(f) + " >/dev/null 2>&1").c_str());
    }
#else
    (void)dir; (void)files;
#endif
    return rewritten;
}
} // namespace HE::ExportSdk
