// Export with the game compiled from an engine SDK (topic 211 step 8): which SDK
// is used, what the compiled-in configuration gets, what the cmake run is told,
// and what replaces the prebuilt runtime. The build itself (cmake + compiler) is
// the sdk_example_project ctest's business; this covers the decisions around it.
#include "doctest.h"
#include "ExportSdkBuild.h"
#include <Hpak/GameBuildConfig.h>
#include <Hpak/ProjectConfig.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

// The two files that make a directory an SDK for findGameSdk.
void makeSdk(const fs::path& root)
{
    touchFile(root / "lib" / "cmake" / "HorizonEngine" / "HorizonEngineConfig.cmake");
    touchFile(root / "share" / "HorizonEngine" / "template" / "CMakeLists.txt");
}

std::string readFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
} // namespace

TEST_CASE("findGameSdk: beside the editor, then beside its folder, profile must match")
{
    TempDir t;
    const fs::path editor = t.path / "Editor";
    fs::create_directories(editor);

    CHECK_FALSE(HE::ExportSdk::findGameSdk(editor, "release").valid());

    makeSdk(t.path / "SDK-release");   // DEPLOY_DIR layout of a dev tree
    // SDL_GetBasePath style (trailing separator) and plain both reach the parent.
    auto s = HE::ExportSdk::findGameSdk(editor / "", "release");
    REQUIRE(s.valid());
    CHECK(s.root == t.path / "SDK-release");
    CHECK(s.profile == "release");
    CHECK(HE::ExportSdk::findGameSdk(editor, "release").valid());
    // The dev SDK is not a stand-in for the release one.
    CHECK_FALSE(HE::ExportSdk::findGameSdk(editor, "dev").valid());

    makeSdk(editor / "SDK-release");   // the editor package wins
    CHECK(HE::ExportSdk::findGameSdk(editor, "release").root == editor / "SDK-release");

    // An incomplete SDK (no template) does not count.
    touchFile(editor / "SDK-dev" / "lib" / "cmake" / "HorizonEngine" / "HorizonEngineConfig.cmake");
    CHECK_FALSE(HE::ExportSdk::findGameSdk(editor, "dev").valid());

    // An override is used as given, never replaced by another SDK.
    makeSdk(t.path / "Mine");
    CHECK(HE::ExportSdk::findGameSdk(editor, "release", t.path / "Mine").root == t.path / "Mine");
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
    const auto spec = HE::ExportSdk::makeBuildSpec(sdk, "/work", "/work/HEGameBuildConfig.h");
    CHECK(spec.sourceDir == sdk.templateDir);
    CHECK(spec.buildDir == fs::path("/work") / "build");
    const auto has = [&](const std::string& d)
    { return std::find(spec.defines.begin(), spec.defines.end(), d) != spec.defines.end(); };
    CHECK(has("HorizonEngine_DIR=/sdk/lib/cmake/HorizonEngine"));
    CHECK(has("HE_GAME_BUILD_CONFIG=/work/HEGameBuildConfig.h"));
    CHECK(has("HE_GAME_NAME=HorizonGame"));
    CHECK(std::find(spec.artifactNames.begin(), spec.artifactNames.end(), "HorizonGame")
          != spec.artifactNames.end());
}

TEST_CASE("installBuiltGame swaps the executable and the SDK's engine libraries in")
{
    TempDir t;
    HE::ExportSdk::GameSdk sdk;
    sdk.root = t.path / "sdk";
    touchFile(sdk.root / "lib" / "libHorizonCore.dylib", "sdk-core");
    touchFile(sdk.root / "lib" / "libHorizonCore.so", "sdk-core-so");
    touchFile(sdk.root / "lib" / "HorizonCore.lib", "import-lib");
    touchFile(sdk.root / "bin" / "HorizonCore.dll", "sdk-dll");
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
    CHECK(copied.size() == 4);

    CHECK_FALSE(HE::ExportSdk::installBuiltGame(sdk, built, {}).empty());
}

// The real chain against an installed SDK: export-folder hcfg + key -> header ->
// cmake build of the SDK template -> swap over a stand-in prebuilt game. Needs a
// toolchain and an SDK (cmake --build <tree> --target HorizonSDK), so it only runs
// with HE_EXPORT_SDK_E2E=<sdk root>; takes about a minute.
TEST_CASE("compile game from SDK end to end (HE_EXPORT_SDK_E2E)")
{
    const char* root = std::getenv("HE_EXPORT_SDK_E2E");
    if (!root || !*root) return;
    const fs::path sdkRoot = root;
    const std::string profile =
        sdkRoot.filename().string().find("release") != std::string::npos ? "release" : "dev";
    const auto sdk = HE::ExportSdk::findGameSdk({}, profile, sdkRoot);
    REQUIRE(sdk.valid());

    TempDir t;
    ProjectConfig cfg;
    cfg.projectName = "SdkE2E";
    cfg.encrypted   = true;
    for (int i = 0; i < 32; ++i) cfg.encKey[i] = (uint8_t)(0xA0 + i);
    const fs::path exportDir = t.path / "export";
    fs::create_directories(exportDir);
    const fs::path exe = exportDir / "HorizonGame";
    // HE_EXPORT_SDK_E2E_EXPORT=<a real prebuilt export folder>: work on a copy of
    // it, and start the swapped game afterwards.
    const char* realExport = std::getenv("HE_EXPORT_SDK_E2E_EXPORT");
    if (realExport && *realExport)
    {
        fs::copy(realExport, exportDir, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        REQUIRE(ProjectConfigLoader::load(exportDir, cfg));
    }
    else
    {
        REQUIRE(ProjectConfigLoader::save(exportDir, cfg));
        touchFile(exe, "prebuilt-game");
    }

    const fs::path header = t.path / "work" / "HEGameBuildConfig.h";
    REQUIRE(HE::ExportSdk::writeBuildConfigFromExport(exportDir, exe, profile, header).empty());
    {
        // Cancel: the first toolchain line stops the run, nothing is built.
        std::atomic<bool> stop{true};
        auto spec   = HE::ExportSdk::makeBuildSpec(sdk, t.path / "cancelled", header);
        spec.cancel = &stop;
        const auto cancelled = HE::hccg::buildDylib(spec);
        CHECK_FALSE(cancelled.ok);
        CHECK(cancelled.message == "cancelled");
    }
    const auto built = HE::hccg::buildDylib(HE::ExportSdk::makeBuildSpec(sdk, t.path / "work", header));
    INFO(built.message);
    REQUIRE(built.ok);
    std::vector<std::string> copied;
    REQUIRE(HE::ExportSdk::installBuiltGame(sdk, built.artifact, exe, &copied).empty());
    HE::ExportSdk::relinkToBundledDeps(exportDir, copied);
    CHECK(readFile(exe) != "prebuilt-game");
#if !defined(_WIN32)
    if (realExport && *realExport)
    {
        const std::string cmd = "cd \"" + exportDir.string() + "\" && HE_HIDDEN_WINDOW=1 "
                                "HE_EXIT_AFTER_FRAMES=30 ./HorizonGame > run.out 2>&1";
        CHECK(std::system(cmd.c_str()) == 0);
        const std::string log = readFile(exportDir / "HorizonEngine.log") + readFile(exportDir / "run.out");
        INFO(log.substr(0, 4000));
        CHECK(log.find("compiled in") != std::string::npos);
#if defined(__APPLE__)
        // No swapped file may still load a library from Homebrew.
        for (const std::string& f : copied)
        {
            const std::string out = exportDir.string() + "/otool.txt";
            std::system(("otool -L '" + (exportDir / f).string() + "' > '" + out + "'").c_str());
            INFO(f);
            CHECK(readFile(out).find("/opt/homebrew/") == std::string::npos);
        }
#endif
        fs::copy(exportDir / "run.out", fs::temp_directory_path() / "he_export_sdk_e2e_run.out",
                 fs::copy_options::overwrite_existing);
    }
#endif
    CHECK(fs::file_size(exe) > 100000);
    // The key went into the binary masked, never as one plain run.
    const std::string bin = readFile(exe);
    CHECK(bin.find(std::string(reinterpret_cast<const char*>(cfg.encKey), 8)) == std::string::npos);
    std::error_code ec;
    fs::copy(t.path / "work" / "build.log", fs::temp_directory_path() / "he_export_sdk_e2e_build.log",
             fs::copy_options::overwrite_existing, ec);
}
