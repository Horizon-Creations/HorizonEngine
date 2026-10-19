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
// holds the package, <root>/share/HorizonEngine/template the game project,
// <root>/lib/<profile> (Windows: also bin/<profile>) one lib set per profile.
struct GameSdk
{
    std::filesystem::path root;
    std::filesystem::path packageDir;    // HorizonEngine_DIR
    std::filesystem::path templateDir;   // the game's CMakeLists.txt
    std::string           profile;       // "dev" | "release": the lib set built against
    bool valid() const { return !root.empty(); }
};

// The SDK folder's name beside the editor (HorizonSDK's default install dir is
// ${DEPLOY_DIR}/EngineSDK). Not "SDK": that one is the HorizonCode codegen SDK
// the editor stages into its own folder.
inline constexpr const char* kGameSdkDirName = "EngineSDK";

// Looks for an SDK holding the lib set of `profile` ("dev" or "release"), first
// match wins:
//   1. `overrideRoot` (the HE_GAME_SDK_DIR environment variable in the editor),
//      used only when it is a complete SDK — a wrong path is not silently
//      replaced by another SDK, it yields "not found" and the reason says so;
//   2. <editorBase>/EngineSDK (the editor package);
//   3. <editorBase>/../EngineSDK (DEPLOY_DIR beside the editor in a dev tree,
//      where HorizonSDK installs by default).
// Complete = package config, that profile's HorizonEngineTargets-<profile>.cmake
// AND the template present.
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

// Where an export keeps its data (project.hcfg, the pak) and its engine
// libraries, from its executable (Platform/GameLayout.h): Contents/Resources of
// a .app, else data/ beside the executable when the export has one; the engine
// in HorizonEngine/ beside the executable when there is one. An export of an
// old flat runtime has both beside the executable.
std::filesystem::path exportDataDir(const std::filesystem::path& exportedExe);
std::filesystem::path exportEngineDir(const std::filesystem::path& exportedExe);

// The lib set an export profile builds against: Development (devTools on) the
// dev set, Shipping (devTools off) the release set without any dev code.
inline const char* sdkProfileFor(bool devTools) { return devTools ? "dev" : "release"; }

// The cmake run: the SDK's template as source, <workDir>/build as build dir,
// the header compiled in, the executable named like the runtime's (HorizonGame;
// installBuiltGame copies it over the export's, whatever that is called), linked
// with the relative rpath only (HE_GAME_PORTABLE), so no path of this machine's
// SDK ships in the game.
HE::hccg::DylibBuildSpec makeBuildSpec(const GameSdk& sdk, const std::filesystem::path& workDir,
                                       const std::filesystem::path& header);

// ── Compiled HorizonCode (topic 211 step 13) ────────────────────────────────
// The HorizonCodeGen library is built against the same SDK and lib set as the
// game, through its package: it links the one HorizonEngine library of that
// profile, bakes the profile in (the game refuses another) and ships in the
// export's HorizonEngine/ beside that library.
// `gameFromSdk` = the export compiles the game AND unavailableReason(gameSdk) was
// empty, so the game will be built from exactly that SDK.
//   game compiled from a usable SDK -> that SDK (codegen SDK info with its profile)
//   otherwise, runtime with HorizonEngine/ -> nothing: the prebuilt game's engine
//       library is not the SDK's, and a library linked against another build of
//       it is an ABI mix nobody can check at load; `why` says so, the classes ship
//       interpreted
//   otherwise (flat runtime) -> `legacy`, the editor's codegen SDK (separate libraries)
// `gameSdkUnavailable`: unavailableReason() when the export asked for the SDK game
// and cannot have it (no compiler, no SDK); `why` then names that instead of
// asking for a checkbox that is already on.
struct CodegenSdkChoice
{
    HE::hccg::SdkInfo sdk;   // invalid = ship interpreted
    std::string       why;   // set when sdk is invalid
    bool              fromGameSdk = false;
};
CodegenSdkChoice chooseCodegenSdk(const GameSdk& gameSdk, bool gameFromSdk, bool runtimeHasEngineDir,
                                  const HE::hccg::SdkInfo& legacy,
                                  const std::string& gameSdkUnavailable = {});

// ── Build cache (topic 211 step 11) ─────────────────────────────────────────
// A project + profile keeps its work dir, and in it the cmake build tree, across
// exports: the next export compiles only what changed (the compiled-in header's
// one source file and the link, a few seconds) instead of configuring and
// compiling the game afresh (~45 s). A tree is reused only while everything it
// was configured from is unchanged; that is the key, one "name=value" line per
// input:
//   sdk        the SDK root
//   version    the package version (HorizonEngineConfigVersion.cmake)
//   headers    the header stamp of the lib set (HorizonEngineHeaders-<profile>.txt)
//   libs       SHA-256 over name, size and time of every file in the lib set
//   profile    dev | release
//   toolchain  cmake version and compiler (the editor's toolchain probe)
//   define     every -D of the build (game name, profile, rpath, header path)
// The export's settings reach the game only through those defines and the
// compiled-in header, whose content the build tool tracks itself.
//
// Work dirs: <cacheRoot>/<project>-<profile>, cacheRoot = <user data>/SdkGameBuilds
// (beside the EngineContentCache; private to the user, like the key in the header).
std::filesystem::path buildWorkDir(const std::filesystem::path& cacheRoot,
                                   const std::string& project, const std::string& profile);

std::string buildCacheKey(const GameSdk& sdk, const HE::hccg::DylibBuildSpec& spec,
                          const std::string& toolchain);

struct CacheCheck
{
    bool        hit = false;   // the tree of the same key is there and is reused
    std::string changed;       // on a miss: the first input that differs, or why there is no tree
};
// Compares `key` with <workDir>/cache-key.txt. On a miss the old build tree goes
// (a CMakeCache pinned to another compiler or SDK fails the configure or links
// the wrong set) and `key` is written for the build that follows.
CacheCheck prepareBuildCache(const std::filesystem::path& workDir, const std::string& key);

// After a failed build: the key goes, so the next export starts from a fresh
// tree instead of the one a broken configure left behind. (A cancelled build
// keeps it: what it compiled is valid for the same key.)
void forgetBuildCache(const std::filesystem::path& workDir);

// Swaps the built game in: `builtExe` over `exportedExe`, plus every engine
// library of the SDK's lib set (lib/<profile>/*.dylib|*.so*, bin/<profile>/*.dll) into
// exportEngineDir(exportedExe), overwriting the prebuilt runtime's copies. Empty
// string = done.
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
