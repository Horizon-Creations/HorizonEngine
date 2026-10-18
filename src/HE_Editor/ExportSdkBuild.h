#pragma once
// Export: compile the game itself against an engine SDK (topic 211 step 8).
//
// The prebuilt-runtime export runs first and stays the answer whenever this
// cannot run: no SDK of the wanted profile, no cmake or compiler, a configure or
// compile that fails. Only a game that actually built replaces the prebuilt
// executable (and the engine libraries beside it, which must come from the same
// SDK). The game then carries the project configuration and the pak key compiled
// in (HEGameBuildConfig.h, written into a private work dir, never into the
// export folder) and only the features its profile has.
//
// ImGui-free by construction, so the decisions are testable in he_tests.
#include <HorizonScene/HcCodegen.h>   // DylibBuildSpec: the cmake configure + build
#include <filesystem>
#include <string>
#include <vector>

namespace HE::ExportSdk
{
// An installed engine SDK (cmake/HorizonSDK.cmake): <root>/lib/cmake/HorizonEngine
// holds the package, <root>/share/HorizonEngine/template the game project.
struct GameSdk
{
    std::filesystem::path root;
    std::filesystem::path packageDir;    // HorizonEngine_DIR
    std::filesystem::path templateDir;   // the game's CMakeLists.txt
    std::string           profile;       // "dev" | "release", as asked for
    bool valid() const { return !root.empty(); }
};

// Looks for the SDK of `profile` ("dev" or "release"), first match wins:
//   1. `overrideRoot` (the HE_GAME_SDK_DIR environment variable in the editor),
//      used only when it is a complete SDK — a wrong path is not silently
//      replaced by another SDK, it yields "not found" and the reason says so;
//   2. <editorBase>/SDK-<profile> (the editor package);
//   3. <editorBase>/../SDK-<profile> (DEPLOY_DIR beside the editor in a dev tree,
//      where HorizonSDK installs by default).
// Complete = package config AND template present.
GameSdk findGameSdk(const std::filesystem::path& editorBase, const std::string& profile,
                    const std::filesystem::path& overrideRoot = {});

// Why the SDK compile cannot run, in one sentence for the dialog and the build log;
// empty when it can. `profile` names the SDK that was looked for.
std::string unavailableReason(const GameSdk& sdk, const std::string& profile,
                              bool cmakeFound, bool compilerFound);

// Writes <header> (HEGameBuildConfig.h) from what the prebuilt export just put
// down: project.hcfg in `dataDir`, Config/ProjectSettings.json under it, and the
// pak key — from the hcfg, or (when the exporter patched it into the prebuilt
// executable instead) read back out of `prebuiltExe`. Empty string = written;
// otherwise why not.
std::string writeBuildConfigFromExport(const std::filesystem::path& dataDir,
                                       const std::filesystem::path& prebuiltExe,
                                       const std::string&           profile,
                                       const std::filesystem::path& header);

// The cmake run: the SDK's template as source, <workDir>/build as build dir,
// the header compiled in, the executable named like the prebuilt one
// (HorizonGame, which the exporter's bundle and launch code key on).
HE::hccg::DylibBuildSpec makeBuildSpec(const GameSdk& sdk, const std::filesystem::path& workDir,
                                       const std::filesystem::path& header);

// Swaps the built game in: `builtExe` over `exportedExe`, plus every engine
// library of the SDK (lib/*.dylib|*.so*, bin/*.dll) into the executable's folder,
// overwriting the prebuilt runtime's copies. Empty string = done.
std::string installBuiltGame(const GameSdk& sdk, const std::filesystem::path& builtExe,
                             const std::filesystem::path& exportedExe,
                             std::vector<std::string>* copied = nullptr);

// macOS: the SDK's libraries name Homebrew lz4/zstd/openssl by absolute path,
// where the prebuilt runtime's copies were rewritten to the bundled ones
// (scripts/bundle_native_deps.sh). Does the same to `files` in `dir`: every
// /opt/homebrew or /usr/local dependency whose file sits in `dir` becomes
// @rpath/<name>, then the file is signed ad hoc again. Returns how many links
// were rewritten; elsewhere a no-op returning 0.
int relinkToBundledDeps(const std::filesystem::path& dir, const std::vector<std::string>& files);
} // namespace HE::ExportSdk
