// Which Visual Studio the codegen builds with on Windows — and that it says so.
//
// Probe and buildDylib used to hand cmake no -G at all, so cmake took its default
// generator — or whatever a global CMAKE_GENERATOR said. VS Code CMake Tools, vcpkg
// and CLion like to set that to Ninja or NMake, which need a cl.exe on PATH that a
// normal shell does not have: the editor reported "No working C++ compiler" on a
// machine with Visual Studio installed (Thema 96, docs §4b Lauf B). Now vswhere names
// the instance and every configure carries -G/-A/CMAKE_GENERATOR_INSTANCE.
//
// The choice itself is data in, data out: vswhere's JSON and cmake's generator list
// go in, so several/no/only-Build-Tools installs are tested on any machine. The
// fixtures are trimmed from NN-WS03's real output (three instances). The Windows-only
// cases at the bottom run the real probe and build against the real installation.
#include "doctest.h"
#include <HorizonScene/HcCodegen.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

const char* kCommunity2026 = R"({
    "instanceId": "4178493d",
    "installationName": "VisualStudio/18.10.2+12217.157",
    "installationPath": "C:\\Program Files\\Microsoft Visual Studio\\18\\Community",
    "installationVersion": "18.10.12217.157",
    "productId": "Microsoft.VisualStudio.Product.Community",
    "isComplete": true,
    "isLaunchable": true,
    "isPrerelease": false,
    "displayName": "Visual Studio Community 2026",
    "description": "Alles, was Sie zum Erstellen moderner Apps benötigen – kostenlos für Sie.",
    "catalog": { "productDisplayVersion": "18.10.2", "productLineVersion": "18" }
  })";
const char* kBuildTools2026 = R"({
    "instanceId": "9c1a2b3d",
    "installationPath": "C:\\Program Files (x86)\\Microsoft Visual Studio\\18\\BuildTools",
    "installationVersion": "18.9.12128.139",
    "productId": "Microsoft.VisualStudio.Product.BuildTools",
    "isComplete": true,
    "isLaunchable": true,
    "isPrerelease": false,
    "displayName": "Visual Studio Build Tools 2026",
    "catalog": { "productDisplayVersion": "18.9.4", "productLineVersion": "18" }
  })";
const char* kCommunity2022 = R"({
    "instanceId": "a1b2c3d4",
    "installationPath": "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community",
    "installationVersion": "17.14.37628.2",
    "productId": "Microsoft.VisualStudio.Product.Community",
    "isComplete": true,
    "isLaunchable": true,
    "isPrerelease": false,
    "displayName": "Visual Studio Community 2022",
    "catalog": { "productDisplayVersion": "17.14.20", "productLineVersion": "2022" }
  })";
const char* kInsiders2026 = R"({
    "instanceId": "deadbeef",
    "installationPath": "C:\\Program Files\\Microsoft Visual Studio\\18\\Insiders",
    "installationVersion": "18.11.12301.12",
    "isPrerelease": true,
    "displayName": "Visual Studio Community 2026 Insiders"
  })";

std::string list(std::initializer_list<const char*> instances)
{
    std::string s = "[\n  ";
    bool first = true;
    for (const char* i : instances)
    {
        if (!first) s += ",\n  ";
        s += i;
        first = false;
    }
    return s + "\n]\n";
}

// What `cmake -E capabilities` lists on a cmake that knows VS 2026 (4.2+) and on
// one that does not.
const std::vector<std::string> kCmake44 = {
    "Visual Studio 18 2026", "Visual Studio 17 2022", "Visual Studio 16 2019",
    "Visual Studio 15 2017", "Visual Studio 14 2015", "Ninja", "NMake Makefiles" };
const std::vector<std::string> kCmake331 = {
    "Visual Studio 17 2022", "Visual Studio 16 2019", "Ninja", "NMake Makefiles" };

std::string pathOf(const HE::hccg::VsSelection& s) { return s.instancePath.string(); }

} // namespace

TEST_CASE("vswhere: of three instances the newest version wins, not the first listed")
{
    // NN-WS03's machine. cmake's own default took BuildTools 18.9 here (docs §4a/§4b);
    // the explicit choice is Community 18.10.
    const auto s = HE::hccg::selectVsInstance(
        list({ kCommunity2026, kBuildTools2026, kCommunity2022 }), kCmake44);
    REQUIRE(s.valid());
    CHECK(s.generator == "Visual Studio 18 2026");
    CHECK(pathOf(s) == "C:\\Program Files\\Microsoft Visual Studio\\18\\Community");
    CHECK(s.version == "18.10.12217.157");
    CHECK(s.displayName == "Visual Studio Community 2026");

    // Order does not matter.
    const auto r = HE::hccg::selectVsInstance(
        list({ kCommunity2022, kBuildTools2026, kCommunity2026 }), kCmake44);
    CHECK(pathOf(r) == pathOf(s));
}

TEST_CASE("vswhere: 18.10 is newer than 18.9 (numeric, not string order)")
{
    for (const auto& json : { list({ kBuildTools2026, kCommunity2026 }),
                              list({ kCommunity2026, kBuildTools2026 }) })
    {
        const auto s = HE::hccg::selectVsInstance(json, kCmake44);
        CHECK(s.version == "18.10.12217.157");
    }
}

TEST_CASE("vswhere: only Build Tools, no IDE")
{
    const auto s = HE::hccg::selectVsInstance(list({ kBuildTools2026 }), kCmake44);
    REQUIRE(s.valid());
    CHECK(s.generator == "Visual Studio 18 2026");
    CHECK(pathOf(s) == "C:\\Program Files (x86)\\Microsoft Visual Studio\\18\\BuildTools");
}

TEST_CASE("vswhere: no installation, or no usable output")
{
    CHECK_FALSE(HE::hccg::selectVsInstance("[]", kCmake44).valid());
    CHECK_FALSE(HE::hccg::selectVsInstance("[\n]\n", kCmake44).valid());
    CHECK_FALSE(HE::hccg::selectVsInstance("", kCmake44).valid());
    // vswhere missing, as cmd reports it through 2>&1.
    CHECK_FALSE(HE::hccg::selectVsInstance(
        "'vswhere' is not recognized as an internal or external command,\r\n", kCmake44).valid());
    CHECK_FALSE(HE::hccg::selectVsInstance("[ { \"installationPath\": ", kCmake44).valid());
    // Entries missing what a generator needs are skipped, not fatal.
    CHECK_FALSE(HE::hccg::selectVsInstance(
        R"([ { "installationPath": "C:\\VS" }, { "installationVersion": "17.0" }, 42 ])",
        kCmake44).valid());
}

TEST_CASE("vswhere: stderr text in front of the JSON does not hide it")
{
    const auto s = HE::hccg::selectVsInstance(
        "Warning: something the installer wanted to say\r\n" + list({ kCommunity2022 }), kCmake44);
    CHECK(s.generator == "Visual Studio 17 2022");
}

TEST_CASE("vswhere: a cmake that does not know VS 2026 gets the newest it does know")
{
    const auto s = HE::hccg::selectVsInstance(
        list({ kCommunity2026, kBuildTools2026, kCommunity2022 }), kCmake331);
    REQUIRE(s.valid());
    CHECK(s.generator == "Visual Studio 17 2022");
    CHECK(pathOf(s) == "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community");

    // …and nothing at all when it knows none of them (e.g. only VS 2026 installed).
    CHECK_FALSE(HE::hccg::selectVsInstance(list({ kBuildTools2026 }), kCmake331).valid());
    // A cmake without any Visual Studio generator (an MSYS2 build).
    CHECK_FALSE(HE::hccg::selectVsInstance(
        list({ kCommunity2022 }), { "Unix Makefiles", "Ninja", "MSYS Makefiles" }).valid());
}

TEST_CASE("vswhere: without cmake's generator list a built-in table names the generator")
{
    const auto s = HE::hccg::selectVsInstance(list({ kBuildTools2026, kCommunity2022 }), {});
    CHECK(s.generator == "Visual Studio 18 2026");
    CHECK(HE::hccg::selectVsInstance(list({ kCommunity2022 }), {}).generator == "Visual Studio 17 2022");

    // A version the table does not know yet is skipped — unless cmake names it.
    const char* future = R"([ { "installationPath": "C:\\VS\\19", "installationVersion": "19.0.1.2" } ])";
    CHECK_FALSE(HE::hccg::selectVsInstance(future, {}).valid());
    CHECK(HE::hccg::selectVsInstance(future, { "Visual Studio 19 2028" }).generator ==
          "Visual Studio 19 2028");
}

TEST_CASE("vswhere: a release beats a newer prerelease; a prerelease alone is used")
{
    const auto s = HE::hccg::selectVsInstance(
        list({ kInsiders2026, kBuildTools2026 }), kCmake44);
    CHECK(s.version == "18.9.12128.139");
    const auto only = HE::hccg::selectVsInstance(list({ kInsiders2026 }), kCmake44);
    CHECK(only.version == "18.11.12301.12");
}

TEST_CASE("cmake -E capabilities: generator names")
{
    const std::string caps = R"({"debugger":true,"fileApi":{"requests":[]},)"
        R"("generators":[{"extraGenerators":[],"name":"Visual Studio 18 2026","platformSupport":true,"toolsetSupport":true},)"
        R"({"extraGenerators":[],"name":"NMake Makefiles","platformSupport":false,"toolsetSupport":false}],)"
        R"("serverMode":false,"tls":true,"version":{"major":4,"minor":4,"patch":0,"string":"4.4.0"}})";
    CHECK(HE::hccg::cmakeGeneratorNames(caps) ==
          std::vector<std::string>{ "Visual Studio 18 2026", "NMake Makefiles" });
    CHECK(HE::hccg::cmakeGeneratorNames("").empty());
    CHECK(HE::hccg::cmakeGeneratorNames("CMake Error: unknown option -E capabilities").empty());
}

#if defined(_WIN32)
namespace {

// he_tests is one process: whatever a test puts into the environment, the next
// test must not inherit.
struct EnvGuard
{
    std::string name;
    std::optional<std::string> old;
    EnvGuard(const char* n, const char* value) : name(n)
    {
        if (const char* v = std::getenv(n)) old = v;
        _putenv_s(n, value);
    }
    ~EnvGuard() { _putenv_s(name.c_str(), old ? old->c_str() : ""); }
};

// The real vswhere at the Installer's place: without it there is no Visual Studio
// for these tests to be about.
bool vsInstallerPresent()
{
    const char* root = std::getenv("ProgramFiles(x86)");
    std::error_code ec;
    return root && std::filesystem::is_regular_file(
        std::filesystem::path(root) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe", ec);
}

} // namespace

// docs §4b Lauf B: a global CMAKE_GENERATOR=NMake Makefiles in a shell without cl.exe
// on PATH made the probe report no compiler. The second value fails in every shell,
// a Developer Prompt included — so this case is red without the explicit -G even on
// a CI runner whose PATH does have cl.exe.
TEST_CASE("Toolchain probe: a global CMAKE_GENERATOR does not hide Visual Studio")
{
    if (!vsInstallerPresent())
    {
        MESSAGE("no Visual Studio Installer (vswhere.exe) on this machine — nothing to find");
        return;
    }
    HE::hccg::setBundledCmakeDir({});
    const HE::hccg::ToolchainProbe control = HE::hccg::probeToolchain();
    if (!control.cmakeFound || !control.compilerFound)
    {
        MESSAGE("no working toolchain even without CMAKE_GENERATOR — nothing to compare "
                "(detail: " << control.detail << ")");
        return;
    }
    for (const std::string generator : { "NMake Makefiles", "HorizonEngine No Such Generator" })
    {
        const EnvGuard env("CMAKE_GENERATOR", generator.c_str());
        const HE::hccg::ToolchainProbe p = HE::hccg::probeToolchain();
        INFO("CMAKE_GENERATOR=" << generator);
        INFO("probe detail: " << p.detail);
        CHECK(p.cmakeFound);
        CHECK(p.compilerFound);
        CHECK(p.compilerId == control.compilerId);
    }
}

// H3: a build directory someone configured before — by hand with NMake/Ninja, or by
// an editor that passed no -G/-A — has a cache cmake will not reconfigure with
// another generator, platform or instance. buildDylib must build there anyway.
TEST_CASE("Toolchain build: a build directory configured with another generator still builds")
{
    namespace fs = std::filesystem;
    if (!vsInstallerPresent())
    {
        MESSAGE("no Visual Studio Installer (vswhere.exe) on this machine — nothing to build with");
        return;
    }
    HE::hccg::setBundledCmakeDir({});
    const HE::hccg::ToolchainProbe control = HE::hccg::probeToolchain();
    if (!control.cmakeFound || !control.compilerFound)
    {
        MESSAGE("no working toolchain on this machine — the build cannot be exercised here");
        return;
    }

    const fs::path root = fs::temp_directory_path() / "he vswhere stale cache";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path source = root / "Source";
    fs::create_directories(source);
    {
        std::ofstream f(source / "CMakeLists.txt", std::ios::binary | std::ios::trunc);
        f << "cmake_minimum_required(VERSION 3.20)\n"
             "project(he_stale_cache CXX)\n"
             "add_library(GameLogic SHARED probe.cpp)\n"
             "set_target_properties(GameLogic PROPERTIES PREFIX \"\")\n";
    }
    {
        std::ofstream f(source / "probe.cpp", std::ios::binary | std::ios::trunc);
        f << "int he_stale_cache_probe() { return 7; }\n";
    }

    const auto buildOnce = [&](const fs::path& buildDir) -> std::string
    {
        HE::hccg::DylibBuildSpec spec;
        spec.sourceDir     = source;
        spec.buildDir      = buildDir;
        spec.logFile       = root / (buildDir.filename().string() + ".log");
        spec.artifactNames = HE::hccg::gameLogicArtifactNames();
        spec.what          = "stale-cache probe library";
        const HE::hccg::BuildOutcome out = HE::hccg::buildDylib(spec);
        std::ifstream f(spec.logFile, std::ios::binary);
        const std::string log((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        INFO("build dir: " << buildDir.string());
        INFO("build message: " << out.message);
        INFO("build log tail: " << (log.size() > 1500 ? log.substr(log.size() - 1500) : log));
        CHECK(out.ok);
        return log;
    };
    // The foreign cache goes once — and the one buildDylib wrote itself must then
    // STAY: were the instance cmake caches compared wrongly, every Build and Reload
    // would silently reconfigure and rebuild from scratch, and still succeed.
    const auto build = [&](const fs::path& buildDir)
    {
        const std::string first = buildOnce(buildDir);
        CHECK(first.find("dropping its CMakeCache.txt") != std::string::npos);
        std::ifstream cache(buildDir / "CMakeCache.txt", std::ios::binary);
        for (std::string line; std::getline(cache, line); )
            if (line.rfind("CMAKE_GENERATOR", 0) == 0) MESSAGE("cache: " << line);
        cache.close();
        const std::string second = buildOnce(buildDir);
        CHECK(second.find("dropping its CMakeCache.txt") == std::string::npos);
    };

    SUBCASE("configured by hand with NMake")
    {
        const fs::path dir = root / "nmake build";
        fs::create_directories(dir / "CMakeFiles");
        std::ofstream f(dir / "CMakeCache.txt", std::ios::binary | std::ios::trunc);
        f << "# This is the CMakeCache file.\n"
             "CMAKE_GENERATOR:INTERNAL=NMake Makefiles\n"
             "CMAKE_GENERATOR_PLATFORM:INTERNAL=\n"
             "CMAKE_GENERATOR_INSTANCE:INTERNAL=\n";
        f.close();
        build(dir);
    }
    SUBCASE("configured by an editor that passed no -G / -A")
    {
        // What every GameLogic build dir from before this change holds: cmake's
        // default Visual Studio generator, no platform, cmake's own instance pick.
        const fs::path dir = root / "old editor build";
        const EnvGuard env("CMAKE_GENERATOR", "");
        const std::string line = "\"cmake -S \"" + source.string() + "\" -B \"" + dir.string() +
                                 "\" >NUL 2>&1\"";
        if (std::system(line.c_str()) != 0)
        {
            MESSAGE("plain `cmake` on PATH could not configure — cannot stage an old cache");
            return;
        }
        build(dir);
    }
    fs::remove_all(root, ec);
}
#endif
