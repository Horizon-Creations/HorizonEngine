// Export with the game compiled from an engine SDK (topic 211 step 8): which SDK
// is used, what the compiled-in configuration gets, what the cmake run is told,
// and what replaces the prebuilt runtime. The build itself (cmake + compiler) is
// the sdk_example_project ctest's business; this covers the decisions around it.
#include "doctest.h"
#include "ExportSdkBuild.h"
#include "HorizonVersion.h"
#include "fixtures.h"   // tests/fixtures/hcodegen — the shared graphs (e2e HorizonCode build)
#include <HorizonCode/HcCompiledLoader.h>
#include <Hpak/GameBuildConfig.h>
#include <Hpak/ProjectConfig.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace
{
struct TempDir
{
    fs::path path;
    TempDir()
        : path(fs::temp_directory_path() /
               ("he_export_sdk_" + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

void touchFile(const fs::path& p, const std::string& text = "x")
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// The files that make a directory an SDK holding these lib sets for findGameSdk.
void makeSdk(const fs::path& root, std::initializer_list<const char*> profiles)
{
    touchFile(root / "lib" / "cmake" / "HorizonEngine" / "HorizonEngineConfig.cmake");
    for (const char* p : profiles)
        touchFile(root / "lib" / "cmake" / "HorizonEngine" /
                  (std::string("HorizonEngineTargets-") + p + ".cmake"));
    touchFile(root / "share" / "HorizonEngine" / "template" / "CMakeLists.txt");
}

std::string readFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
} // namespace

TEST_CASE("findGameSdk: beside the editor, then beside its folder, lib set must be there")
{
    TempDir t;
    const fs::path editor = t.path / "Editor";
    fs::create_directories(editor);

    CHECK_FALSE(HE::ExportSdk::findGameSdk(editor, "release").valid());

    makeSdk(t.path / "EngineSDK", { "release" });   // DEPLOY_DIR layout of a dev tree
    // SDL_GetBasePath style (trailing separator) and plain both reach the parent.
    auto s = HE::ExportSdk::findGameSdk(editor / "", "release");
    REQUIRE(s.valid());
    CHECK(s.root == t.path / "EngineSDK");
    CHECK(s.profile == "release");
    CHECK(HE::ExportSdk::findGameSdk(editor, "release").valid());
    // Only the release lib set is installed: no dev SDK there.
    CHECK_FALSE(HE::ExportSdk::findGameSdk(editor, "dev").valid());

    // One SDK, both lib sets: either profile finds it; the editor package wins.
    makeSdk(editor / "EngineSDK", { "dev", "release" });
    CHECK(HE::ExportSdk::findGameSdk(editor, "release").root == editor / "EngineSDK");
    CHECK(HE::ExportSdk::findGameSdk(editor, "dev").root == editor / "EngineSDK");
    CHECK(HE::ExportSdk::findGameSdk(editor, "dev").profile == "dev");

    // An incomplete SDK (no template) does not count; neither does the old
    // one-folder-per-profile layout.
    touchFile(t.path / "Other" / "EngineSDK" / "lib" / "cmake" / "HorizonEngine" / "HorizonEngineConfig.cmake");
    touchFile(t.path / "Other" / "EngineSDK" / "lib" / "cmake" / "HorizonEngine" / "HorizonEngineTargets-dev.cmake");
    CHECK_FALSE(HE::ExportSdk::findGameSdk(t.path / "Other", "dev").valid());
    makeSdk(t.path / "Old" / "SDK-dev", {});
    CHECK_FALSE(HE::ExportSdk::findGameSdk(t.path / "Old", "dev").valid());

    // An override is used as given, never replaced by another SDK.
    makeSdk(t.path / "Mine", { "release" });
    CHECK(HE::ExportSdk::findGameSdk(editor, "release", t.path / "Mine").root == t.path / "Mine");
    CHECK_FALSE(HE::ExportSdk::findGameSdk(editor, "dev", t.path / "Mine").valid());
    CHECK_FALSE(HE::ExportSdk::findGameSdk(editor, "release", t.path / "Nope").valid());
}

TEST_CASE("unavailableReason names what is missing, empty when the compile can run")
{
    HE::ExportSdk::GameSdk none;
    HE::ExportSdk::GameSdk sdk;
    sdk.root = "/sdk";
    CHECK(HE::ExportSdk::unavailableReason(none, "release", true, true).find("'release'") != std::string::npos);
    CHECK(HE::ExportSdk::unavailableReason(sdk, "release", false, true) == "cmake not found");
    CHECK(HE::ExportSdk::unavailableReason(sdk, "release", true, false).find("compiler") != std::string::npos);
    CHECK(HE::ExportSdk::unavailableReason(sdk, "release", true, true).empty());
}

TEST_CASE("writeBuildConfigFromExport compiles in the exported hcfg and its key")
{
    TempDir t;
    ProjectConfig cfg;
    cfg.projectName = "SdkGame";
    cfg.encrypted   = true;
    for (int i = 0; i < 32; ++i) cfg.encKey[i] = (uint8_t)(i * 7 + 1);
    REQUIRE(ProjectConfigLoader::save(t.path, cfg));

    const fs::path header = t.path / "private" / "HEGameBuildConfig.h";
    CHECK(HE::ExportSdk::writeBuildConfigFromExport(t.path, {}, "release", header).empty());
    const std::string text = readFile(header);
    CHECK(text.find("kGameBuildConfigFormat") != std::string::npos);
    CHECK(text.find("release") != std::string::npos);
    uint8_t key[32] = {};
    REQUIRE(HE::readGameBuildConfigPakKey(header, key));
    CHECK(std::memcmp(key, cfg.encKey, 32) == 0);

    // Key patched into the exe (hcfg carries none) and no exe to read it from:
    // refused, not a header that cannot open the pak.
    std::memset(cfg.encKey, 0, 32);
    REQUIRE(ProjectConfigLoader::save(t.path, cfg));
    CHECK(HE::ExportSdk::writeBuildConfigFromExport(t.path, {}, "release", header)
              .find("pak key") != std::string::npos);

    CHECK_FALSE(HE::ExportSdk::writeBuildConfigFromExport(t.path / "empty", {}, "release",
                                                          header).empty());
}

TEST_CASE("makeBuildSpec: SDK template, private build dir, header compiled in")
{
    HE::ExportSdk::GameSdk sdk;
    sdk.root        = "/sdk";
    sdk.packageDir  = "/sdk/lib/cmake/HorizonEngine";
    sdk.templateDir = "/sdk/share/HorizonEngine/template";
    sdk.profile     = "release";
    const auto spec = HE::ExportSdk::makeBuildSpec(sdk, "/work", "/work/HEGameBuildConfig.h");
    CHECK(spec.sourceDir == sdk.templateDir);
    CHECK(spec.buildDir == fs::path("/work") / "build");
    const auto has = [&](const std::string& d)
    { return std::find(spec.defines.begin(), spec.defines.end(), d) != spec.defines.end(); };
    CHECK(has("HorizonEngine_DIR=/sdk/lib/cmake/HorizonEngine"));
    CHECK(has("HE_BUILD_PROFILE=release"));   // which lib set of the one SDK
    CHECK(has("HE_GAME_BUILD_CONFIG=/work/HEGameBuildConfig.h"));
    CHECK(has("HE_GAME_NAME=HorizonGame"));
    CHECK(has("HE_GAME_PORTABLE=ON"));        // no absolute rpath to the SDK in the game
    CHECK(std::find(spec.artifactNames.begin(), spec.artifactNames.end(), "HorizonGame")
          != spec.artifactNames.end());
}

// Topic 211 step 13: compiled HorizonCode links the engine the game ships with.
TEST_CASE("chooseCodegenSdk: the game's SDK and lib set, never the prebuilt game's engine")
{
    HE::ExportSdk::GameSdk sdk;
    sdk.root       = "/sdk";
    sdk.packageDir = "/sdk/lib/cmake/HorizonEngine";
    sdk.profile    = "release";
    HE::hccg::SdkInfo legacy;
    legacy.includeDirs = { "/editor/SDK/include" };
    legacy.libDir      = "/editor/SDK/lib";

    // Game compiled from the SDK: the same package and lib set, nothing else.
    auto c = HE::ExportSdk::chooseCodegenSdk(sdk, true, true, legacy);
    REQUIRE(c.sdk.valid());
    CHECK(c.fromGameSdk);
    CHECK(c.why.empty());
    CHECK(c.sdk.packageDir == sdk.packageDir);
    CHECK(c.sdk.profile == "release");
    CHECK(c.sdk.includeDirs.empty());
    CHECK(c.sdk.libDir.empty());

    // Prebuilt game with HorizonEngine/: no library at all, and why.
    for (const bool gameFromSdk : { false, true })
    {
        c = HE::ExportSdk::chooseCodegenSdk(gameFromSdk ? HE::ExportSdk::GameSdk{} : sdk,
                                            gameFromSdk, true, legacy);
        CHECK_FALSE(c.sdk.valid());
        CHECK_FALSE(c.fromGameSdk);
        CHECK(c.why.find("Compile game from SDK") != std::string::npos);
    }
    // Asked for, but the game cannot be built here (step 14, seen live without a
    // compiler): the reason, not a request for the checkbox that is already on.
    c = HE::ExportSdk::chooseCodegenSdk(sdk, false, true, legacy, "no working C++ compiler found");
    CHECK_FALSE(c.sdk.valid());
    CHECK(c.why.find("no working C++ compiler found") != std::string::npos);
    CHECK(c.why.find("turn on") == std::string::npos);

    // A flat runtime (no HorizonEngine/) keeps the editor's codegen SDK.
    c = HE::ExportSdk::chooseCodegenSdk(sdk, false, false, legacy);
    REQUIRE(c.sdk.valid());
    CHECK_FALSE(c.fromGameSdk);
    CHECK(c.sdk.libDir == legacy.libDir);
    CHECK(c.sdk.profile.empty());
    CHECK_FALSE(HE::ExportSdk::chooseCodegenSdk(sdk, false, false, {}).why.empty());
}

TEST_CASE("Export profile picks the lib set: Development dev, Shipping release")
{
    CHECK(std::string(HE::ExportSdk::sdkProfileFor(true)) == "dev");
    CHECK(std::string(HE::ExportSdk::sdkProfileFor(false)) == "release");
}

TEST_CASE("buildWorkDir: one folder per project and profile, a safe name")
{
    CHECK(HE::ExportSdk::buildWorkDir("/c", "My Game", "dev") == fs::path("/c") / "My_Game-dev");
    CHECK(HE::ExportSdk::buildWorkDir("/c", "a/b:c", "release") == fs::path("/c") / "a_b_c-release");
    CHECK(HE::ExportSdk::buildWorkDir("/c", "", "dev") == fs::path("/c") / "Game-dev");
}

TEST_CASE("Build cache: the key follows SDK, lib set, profile and toolchain; a miss clears the tree")
{
    TempDir t;
    const fs::path root = t.path / "sdk";
    makeSdk(root, { "dev", "release" });
    const fs::path pkg = root / "lib" / "cmake" / "HorizonEngine";
    touchFile(pkg / "HorizonEngineConfigVersion.cmake", "set(PACKAGE_VERSION \"0.7.0\")\n");
    touchFile(pkg / "HorizonEngineHeaders-dev.txt", "aaaa\n");
    touchFile(pkg / "HorizonEngineHeaders-release.txt", "aaaa\n");
    touchFile(root / "lib" / "dev" / "libHorizonEngine.dylib", "dev-engine");
    touchFile(root / "lib" / "release" / "libHorizonEngine.dylib", "release-engine");
    const auto dev = HE::ExportSdk::findGameSdk({}, "dev", root);
    const auto rel = HE::ExportSdk::findGameSdk({}, "release", root);
    REQUIRE(dev.valid());
    REQUIRE(rel.valid());
    const fs::path work = t.path / "work";
    const auto spec = HE::ExportSdk::makeBuildSpec(dev, work, work / "HEGameBuildConfig.h");
    const std::string tc = "cmake 3.31.0, AppleClang 17.0.0";
    const std::string key = HE::ExportSdk::buildCacheKey(dev, spec, tc);

    CHECK(key == HE::ExportSdk::buildCacheKey(dev, spec, tc));          // stable
    CHECK(key.find("version=0.7.0\n") != std::string::npos);
    CHECK(key.find("headers=aaaa\n") != std::string::npos);
    CHECK(key.find("profile=dev\n") != std::string::npos);
    CHECK(key.find("define=HE_GAME_PORTABLE=ON\n") != std::string::npos);
    CHECK(key != HE::ExportSdk::buildCacheKey(dev, spec, "cmake 3.31.0, AppleClang 18.0.0"));
    CHECK(key != HE::ExportSdk::buildCacheKey(
                     rel, HE::ExportSdk::makeBuildSpec(rel, work, work / "HEGameBuildConfig.h"), tc));

    // No tree yet: a miss, and the key is written for the build that follows.
    auto c = HE::ExportSdk::prepareBuildCache(work, key);
    CHECK_FALSE(c.hit);
    CHECK(c.changed == "no earlier build");
    CHECK(readFile(work / "cache-key.txt") == key);
    touchFile(work / "build" / "CMakeCache.txt");   // what a configure leaves
    touchFile(work / "build" / "game.o");

    // Same key, tree there: a hit, the tree stays.
    c = HE::ExportSdk::prepareBuildCache(work, key);
    CHECK(c.hit);
    CHECK(c.changed.empty());
    CHECK(fs::exists(work / "build" / "game.o"));

    // Another compiler: a miss that names it, and the tree goes.
    const std::string otherTc = HE::ExportSdk::buildCacheKey(dev, spec, "cmake 3.31.0, AppleClang 18.0.0");
    c = HE::ExportSdk::prepareBuildCache(work, otherTc);
    CHECK_FALSE(c.hit);
    CHECK(c.changed == "toolchain");
    CHECK_FALSE(fs::exists(work / "build"));

    // New headers in the SDK (a newer engine): miss on "headers".
    touchFile(work / "build" / "CMakeCache.txt");
    touchFile(pkg / "HorizonEngineHeaders-dev.txt", "bbbb\n");
    c = HE::ExportSdk::prepareBuildCache(work, HE::ExportSdk::buildCacheKey(dev, spec, "cmake 3.31.0, AppleClang 18.0.0"));
    CHECK_FALSE(c.hit);
    CHECK(c.changed == "headers");

    // A reinstalled lib set with the same headers: miss on "libs".
    touchFile(work / "build" / "CMakeCache.txt");
    const std::string before = HE::ExportSdk::buildCacheKey(dev, spec, tc);
    touchFile(root / "lib" / "dev" / "libHorizonEngine.dylib", "dev-engine, rebuilt");
    const std::string after = HE::ExportSdk::buildCacheKey(dev, spec, tc);
    CHECK(before != after);
    HE::ExportSdk::prepareBuildCache(work, before);
    touchFile(work / "build" / "CMakeCache.txt");
    c = HE::ExportSdk::prepareBuildCache(work, after);
    CHECK(c.changed == "libs");

    // A failed build forgets the key: the next export starts afresh.
    touchFile(work / "build" / "CMakeCache.txt");
    REQUIRE(HE::ExportSdk::prepareBuildCache(work, after).hit);
    HE::ExportSdk::forgetBuildCache(work);
    c = HE::ExportSdk::prepareBuildCache(work, after);
    CHECK_FALSE(c.hit);
    CHECK(c.changed == "no earlier build");
}

TEST_CASE("installBuiltGame swaps the executable and the SDK's engine libraries in")
{
    TempDir t;
    HE::ExportSdk::GameSdk sdk;
    sdk.root    = t.path / "sdk";
    sdk.profile = "release";
    touchFile(sdk.root / "lib" / "release" / "libHorizonCore.dylib", "sdk-core");
    touchFile(sdk.root / "lib" / "release" / "libHorizonCore.so", "sdk-core-so");
    touchFile(sdk.root / "lib" / "release" / "HorizonCore.lib", "import-lib");
    touchFile(sdk.root / "bin" / "release" / "HorizonCore.dll", "sdk-dll");
    // The other lib set of the same SDK stays where it is.
    touchFile(sdk.root / "lib" / "dev" / "libHorizonNet.dylib", "dev-net");
    const fs::path built = t.path / "build" / "HorizonGame";
    touchFile(built, "compiled-game");
    const fs::path exe = t.path / "export" / "HorizonGame";
    touchFile(exe, "prebuilt-game");
    touchFile(exe.parent_path() / "libHorizonCore.dylib", "prebuilt-core");

    std::vector<std::string> copied;
    CHECK(HE::ExportSdk::installBuiltGame(sdk, built, exe, &copied).empty());
    CHECK(readFile(exe) == "compiled-game");
    CHECK(readFile(exe.parent_path() / "libHorizonCore.dylib") == "sdk-core");
    CHECK(fs::exists(exe.parent_path() / "libHorizonCore.so"));
    CHECK(fs::exists(exe.parent_path() / "HorizonCore.dll"));
    CHECK_FALSE(fs::exists(exe.parent_path() / "HorizonCore.lib"));   // link-time only
    CHECK_FALSE(fs::exists(exe.parent_path() / "libHorizonNet.dylib"));
    CHECK(copied.size() == 4);

    CHECK_FALSE(HE::ExportSdk::installBuiltGame(sdk, built, {}).empty());
}

// Thema 211 step 10: an export with data/ and HorizonEngine/ beside a renamed
// executable gets the engine libraries into HorizonEngine/, and the configuration
// is read from data/.
TEST_CASE("installBuiltGame puts the libraries into the export's HorizonEngine/ folder")
{
    TempDir t;
    HE::ExportSdk::GameSdk sdk;
    sdk.root    = t.path / "sdk";
    sdk.profile = "dev";
    touchFile(sdk.root / "lib" / "dev" / "libHorizonEngine.dylib", "sdk-engine");
    touchFile(sdk.root / "lib" / "dev" / "libHorizonPython.dylib", "sdk-plugin");
    const fs::path built = t.path / "build" / "HorizonGame";
    touchFile(built, "compiled-game");
    const fs::path exe = t.path / "export" / "Space Game";
    touchFile(exe, "prebuilt-game");
    touchFile(exe.parent_path() / "HorizonEngine" / "libHorizonEngine.dylib", "prebuilt-engine");
    touchFile(exe.parent_path() / "data" / "project.hcfg", "hcfg");

    CHECK(HE::ExportSdk::exportDataDir(exe) == exe.parent_path() / "data");
    CHECK(HE::ExportSdk::exportEngineDir(exe) == exe.parent_path() / "HorizonEngine");
    CHECK(HE::ExportSdk::installBuiltGame(sdk, built, exe).empty());
    CHECK(readFile(exe) == "compiled-game");
    CHECK(readFile(exe.parent_path() / "HorizonEngine" / "libHorizonEngine.dylib") == "sdk-engine");
    CHECK_FALSE(fs::exists(exe.parent_path() / "libHorizonEngine.dylib"));
    // A non-Python export shipped no plugin, and still has none.
    CHECK_FALSE(fs::exists(exe.parent_path() / "HorizonEngine" / "libHorizonPython.dylib"));
    touchFile(exe.parent_path() / "HorizonEngine" / "libHorizonPython.dylib", "prebuilt-plugin");
    CHECK(HE::ExportSdk::installBuiltGame(sdk, built, exe).empty());
    CHECK(readFile(exe.parent_path() / "HorizonEngine" / "libHorizonPython.dylib") == "sdk-plugin");

    // The old flat layout: both beside the executable; a .app: Resources.
    const fs::path flat = t.path / "flat" / "HorizonGame";
    touchFile(flat, "x");
    CHECK(HE::ExportSdk::exportDataDir(flat) == flat.parent_path());
    CHECK(HE::ExportSdk::exportEngineDir(flat) == flat.parent_path());
    const fs::path app = t.path / "G.app" / "Contents" / "MacOS" / "G";
    CHECK(HE::ExportSdk::exportDataDir(app) == t.path / "G.app" / "Contents" / "Resources");
}

// The real chain against an installed SDK: export-folder hcfg + key -> header ->
// cmake build of the SDK template -> swap over a stand-in prebuilt game. Needs a
// toolchain and an SDK (cmake --build <tree> --target HorizonSDK), so it only runs
// with HE_EXPORT_SDK_E2E=<sdk root> [HE_EXPORT_SDK_E2E_PROFILE=dev|release, default
// dev]; takes about a minute. HE_EXPORT_SDK_E2E_EXPORT=<export folder> works on a
// copy of a real export, whose executable HE_EXPORT_SDK_E2E_EXE names (default
// HorizonGame; an export with HorizonEngine/ carries the project's name).
TEST_CASE("compile game from SDK end to end (HE_EXPORT_SDK_E2E)")
{
    const char* root = std::getenv("HE_EXPORT_SDK_E2E");
    if (!root || !*root) return;
    const fs::path sdkRoot = root;
    const char* wanted = std::getenv("HE_EXPORT_SDK_E2E_PROFILE");
    const std::string profile = wanted && *wanted ? wanted : "dev";
    const auto sdk = HE::ExportSdk::findGameSdk({}, profile, sdkRoot);
    REQUIRE(sdk.valid());

    TempDir t;
    ProjectConfig cfg;
    cfg.projectName = "SdkE2E";
    cfg.encrypted   = true;
    for (int i = 0; i < 32; ++i) cfg.encKey[i] = (uint8_t)(0xA0 + i);
    const fs::path exportDir = t.path / "export";
    fs::create_directories(exportDir);
    const char* exeName = std::getenv("HE_EXPORT_SDK_E2E_EXE");
    const fs::path exe = exportDir / (exeName && *exeName ? exeName : "HorizonGame");
    // HE_EXPORT_SDK_E2E_EXPORT=<a real prebuilt export folder>: work on a copy of
    // it, and start the swapped game afterwards.
    const char* realExport = std::getenv("HE_EXPORT_SDK_E2E_EXPORT");
    if (realExport && *realExport)
    {
        fs::copy(realExport, exportDir, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        REQUIRE(ProjectConfigLoader::load(HE::ExportSdk::exportDataDir(exe), cfg));
    }
    else
    {
        REQUIRE(ProjectConfigLoader::save(exportDir, cfg));
        touchFile(exe, "prebuilt-game");
    }

    const fs::path header = t.path / "work" / "HEGameBuildConfig.h";
    REQUIRE(HE::ExportSdk::writeBuildConfigFromExport(HE::ExportSdk::exportDataDir(exe), exe,
                                                      profile, header).empty());
    {
        // Cancel: the first toolchain line stops the run, nothing is built.
        std::atomic<bool> stop{true};
        auto spec   = HE::ExportSdk::makeBuildSpec(sdk, t.path / "cancelled", header);
        spec.cancel = &stop;
        const auto cancelled = HE::hccg::buildDylib(spec);
        CHECK_FALSE(cancelled.ok);
        CHECK(cancelled.message == "cancelled");
    }
    // Through the build cache, as the export does: the first build misses, the
    // second one (a re-export, header rewritten) hits and reuses the tree.
    const auto spec = HE::ExportSdk::makeBuildSpec(sdk, t.path / "work", header);
    const std::string key = HE::ExportSdk::buildCacheKey(sdk, spec, "e2e toolchain");
    CHECK_FALSE(HE::ExportSdk::prepareBuildCache(t.path / "work", key).hit);
    const auto built = HE::hccg::buildDylib(spec);
    INFO(built.message);
    REQUIRE(built.ok);
    {
        REQUIRE(HE::ExportSdk::writeBuildConfigFromExport(HE::ExportSdk::exportDataDir(exe), exe,
                                                          profile, header).empty());
        const auto again = HE::ExportSdk::prepareBuildCache(t.path / "work", key);
        CHECK(again.hit);
        const auto t0 = std::chrono::steady_clock::now();
        const auto rebuilt = HE::hccg::buildDylib(spec);
        const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        MESSAGE("cache-hit rebuild took " << secs << " s");
        INFO(rebuilt.message);
        REQUIRE(rebuilt.ok);
        // Only the compiled-in configuration's source was compiled again.
        const std::string log = readFile(t.path / "work" / "build.log");
        CHECK(log.find("CompiledBuildConfig.cpp") != std::string::npos);
        CHECK(log.find("GameApplication.cpp") == std::string::npos);
    }
#if defined(__APPLE__)
    {
        // Linked PORTABLE: only the relative rpaths, no path of this machine's SDK.
        const std::string out = (t.path / "rpath.txt").string();
        std::system(("otool -l '" + built.artifact.string() + "' > '" + out + "'").c_str());
        const std::string loads = readFile(out);
        INFO(loads.substr(0, 4000));
        CHECK(loads.find("@executable_path/HorizonEngine") != std::string::npos);
        CHECK(loads.find(sdk.root.string()) == std::string::npos);
        CHECK(loads.find("path /") == std::string::npos);   // LC_RPATH "path /abs/..."
    }
#endif
    std::vector<std::string> copied;
    REQUIRE(HE::ExportSdk::installBuiltGame(sdk, built.artifact, exe, &copied).empty());
    const fs::path engineDir = HE::ExportSdk::exportEngineDir(exe);
    HE::ExportSdk::relinkToBundledDeps(engineDir, copied);
    CHECK(readFile(exe) != "prebuilt-game");

    // Compiled HorizonCode (topic 211 step 13), the export's way: generated
    // classes built against the same SDK and lib set as the game, through its
    // package, into build-<profile>, shipped in the export's HorizonEngine/.
    const auto hcSdk = HE::ExportSdk::chooseCodegenSdk(sdk, true, true, {});
    REQUIRE(hcSdk.fromGameSdk);
    const fs::path genDir = t.path / "hcgen";
    {
        HE::hccg::Options opt;
        opt.engineVersion = HE_VERSION_STRING;
        opt.onFailure     = HE::hccg::OnFailure::Stop;
        const auto gen = HE::hccg::generate({ hcfix::fxEngineWorld(), hcfix::fxAnimatorSync() }, opt);
        REQUIRE(gen.ok);
        std::vector<std::string> cppFiles;
        for (const auto& f : gen.files)
        {
            touchFile(genDir / f.name, f.contents);
            if (fs::path(f.name).extension() == ".cpp") cppFiles.push_back(f.name);
        }
        touchFile(genDir / "CMakeLists.txt", HE::hccg::generateCMakeLists(opt, cppFiles));
    }
    const auto hc = HE::hccg::buildDylib(genDir, hcSdk.sdk);
    INFO(hc.message);
    REQUIRE(hc.ok);
    CHECK(hc.artifact.parent_path() == genDir / ("build-" + profile));
    CHECK(readFile(genDir / ("build-" + profile) / "CMakeCache.txt")
              .find("HE_BUILD_PROFILE:UNINITIALIZED=" + profile) != std::string::npos);
#if defined(__APPLE__)
    {
        // The one engine library of the lib set, found beside the library; no
        // part library, no path of this machine.
        const std::string out = (t.path / "hc_otool.txt").string();
        std::system(("otool -L '" + hc.artifact.string() + "' > '" + out + "'").c_str());
        std::system(("otool -l '" + hc.artifact.string() + "' >> '" + out + "'").c_str());
        const std::string loads = readFile(out);
        INFO(loads.substr(0, 4000));
        CHECK(loads.find("@rpath/libHorizonEngine.dylib") != std::string::npos);
        CHECK(loads.find("libHorizonScene") == std::string::npos);
        CHECK(loads.find("libHorizonCore") == std::string::npos);
        CHECK(loads.find("@loader_path") != std::string::npos);
        CHECK(loads.find("path /") == std::string::npos);
    }
#endif
    // Not loaded here: it finds its engine only beside itself, and he_tests
    // already holds the four separate libraries. The game below loads it.
    fs::copy_file(hc.artifact, engineDir / HorizonCode::compiledLibraryName(),
                  fs::copy_options::overwrite_existing);
#if !defined(_WIN32)
    // Started only in the dev profile: a release game has no hidden mode and no
    // frame budget (HE_FEATURE_ENV_OVERRIDES=0), so it would open a window on the
    // desktop and run until someone closes it.
    if (realExport && *realExport && profile == "dev")
    {
        const std::string cmd = "cd \"" + exportDir.string() + "\" && HE_HIDDEN_WINDOW=1 "
                                "HE_EXIT_AFTER_FRAMES=30 \"./" + exe.filename().string() + "\" > run.out 2>&1";
        CHECK(std::system(cmd.c_str()) == 0);
        const std::string log = readFile(exportDir / "HorizonEngine.log") + readFile(exportDir / "run.out");
        INFO(log.substr(0, 4000));
        CHECK(log.find("compiled in") != std::string::npos);
        // The HorizonCode library of the same lib set, taken from HorizonEngine/.
        CHECK(log.find("HorizonCode: 2 compiled classes") != std::string::npos);
        CHECK(log.find("profile but this engine is") == std::string::npos);
#if defined(__APPLE__)
        // No swapped file may still load a library from Homebrew.
        for (const std::string& f : copied)
        {
            const std::string out = exportDir.string() + "/otool.txt";
            const fs::path file = f == exe.filename().string() ? exe : engineDir / f;
            std::system(("otool -L '" + file.string() + "' > '" + out + "'").c_str());
            INFO(f);
            CHECK(readFile(out).find("/opt/homebrew/") == std::string::npos);
        }
#endif
        fs::copy(exportDir / "run.out", fs::temp_directory_path() / "he_export_sdk_e2e_run.out",
                 fs::copy_options::overwrite_existing);
    }
#endif
    CHECK(fs::file_size(exe) > 100000);
    // The key went into the binary masked, never as one plain run. (A real export
    // may be unencrypted: no key, and eight zero bytes are in every binary.)
    if (cfg.encrypted)
    {
        const std::string bin = readFile(exe);
        CHECK(bin.find(std::string(reinterpret_cast<const char*>(cfg.encKey), 8)) == std::string::npos);
    }
    std::error_code ec;
    fs::copy(t.path / "work" / "build.log", fs::temp_directory_path() / "he_export_sdk_e2e_build.log",
             fs::copy_options::overwrite_existing, ec);
}
