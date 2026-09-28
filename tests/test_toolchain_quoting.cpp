// The toolchain probe through a BUNDLED cmake — the path every packaged editor takes.
//
// A packaged editor calls setBundledCmakeDir(<Editor>/cmake), so resolveCmake()
// hands back a quoted absolute path instead of the bare word "cmake". On Windows
// every command then runs as `cmd.exe /c <line>`, and cmd strips the first and the
// last quote of any line that starts with one and holds more than two: the
// configure line `"…\cmake.exe" -S "…" -B "…" 2>&1` arrived cut apart, the probe
// failed with "The filename, directory name, or volume label syntax is incorrect",
// and Tool Status showed cmake green but "C++ compiler: not found" on a machine with
// a perfectly good Visual Studio (Thema 96). `cmake --version` has only two quotes
// and survived, which is why nothing else noticed; the tests only ever ran the bare
// "cmake" from PATH.
//
// So: the cmake on PATH, once as the bare word (the control — does this machine
// have a compiler at all?) and once pointed at as if it were bundled. Both must
// agree.
#include "doctest.h"
#include <HorizonScene/HcCodegen.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

namespace {

// <prefix> such that <prefix>/bin/cmake[.exe] is the cmake PATH resolves to —
// the layout setBundledCmakeDir expects. Empty when there is none.
std::filesystem::path cmakePrefixOnPath()
{
    namespace fs = std::filesystem;
#if defined(_WIN32)
    const char sep = ';';
    const char* exe = "cmake.exe";
#else
    const char sep = ':';
    const char* exe = "cmake";
#endif
    const char* path = std::getenv("PATH");
    std::istringstream iss(path ? path : "");
    for (std::string dir; std::getline(iss, dir, sep); )
    {
        if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"')
            dir = dir.substr(1, dir.size() - 2);
        if (dir.empty()) continue;
        std::error_code ec;
        const fs::path candidate = fs::path(dir) / exe;
        if (!fs::is_regular_file(candidate, ec)) continue;
        const fs::path bin = fs::canonical(candidate, ec).parent_path();
        if (ec || bin.filename() != "bin") return {};
        return bin.parent_path();
    }
    return {};
}

// A failed REQUIRE throws; the bundle must not outlive this test either way, or
// every later test that builds would silently run through it.
struct BundledCmakeGuard
{
    explicit BundledCmakeGuard(const std::filesystem::path& dir) { HE::hccg::setBundledCmakeDir(dir); }
    ~BundledCmakeGuard() { HE::hccg::setBundledCmakeDir({}); }
};

} // namespace

TEST_CASE("Toolchain probe: a bundled (quoted) cmake finds the compiler the bare one finds")
{
    const std::filesystem::path prefix = cmakePrefixOnPath();
    if (prefix.empty())
    {
        MESSAGE("no cmake on PATH in a <prefix>/bin layout — nothing to compare against");
        return;
    }

    HE::hccg::setBundledCmakeDir({});
    const HE::hccg::ToolchainProbe control = HE::hccg::probeToolchain();
    if (!control.cmakeFound || !control.compilerFound)
    {
        MESSAGE("no C++ toolchain on this machine (cmake: " << control.cmakeFound
                << ", compiler: " << control.compilerFound << ") — nothing to compare");
        return;
    }

    const BundledCmakeGuard guard(prefix);
    const HE::hccg::ToolchainProbe bundled = HE::hccg::probeToolchain();
    INFO("bundled cmake prefix: " << prefix.string());
    INFO("probe detail: " << bundled.detail);
    CHECK(bundled.cmakeFound);
    CHECK(bundled.cmakeVersion == control.cmakeVersion);
    REQUIRE(bundled.compilerFound);
    CHECK(bundled.compilerId == control.compilerId);
}

// The build the probe stands in for: configure + build + the -D defines, through
// the bundled cmake, with a space in the source dir, the build dir AND a define's
// value. The CMakeLists refuses to configure unless that value arrives whole.
TEST_CASE("Toolchain build: buildDylib through a bundled (quoted) cmake, spaces everywhere")
{
    namespace fs = std::filesystem;
    const fs::path prefix = cmakePrefixOnPath();
    if (prefix.empty())
    {
        MESSAGE("no cmake on PATH in a <prefix>/bin layout — nothing to build with");
        return;
    }
    HE::hccg::setBundledCmakeDir({});
    const HE::hccg::ToolchainProbe control = HE::hccg::probeToolchain();
    if (!control.cmakeFound || !control.compilerFound)
    {
        MESSAGE("no C++ toolchain on this machine — the build cannot be exercised here");
        return;
    }

    const fs::path root = fs::temp_directory_path() / "he quoting build";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path source = root / "Source Dir";
    const fs::path marker = root / "marker dir";
    fs::create_directories(source);
    fs::create_directories(marker);
    {
        std::ofstream f(source / "CMakeLists.txt", std::ios::binary | std::ios::trunc);
        f << "cmake_minimum_required(VERSION 3.20)\n"
             "project(he_quoting_build CXX)\n"
             "if(NOT IS_DIRECTORY \"${HE_QUOTING_MARKER}\")\n"
             "  message(FATAL_ERROR \"define arrived cut apart: '${HE_QUOTING_MARKER}'\")\n"
             "endif()\n"
             "add_library(GameLogic SHARED probe.cpp)\n"
             "set_target_properties(GameLogic PROPERTIES PREFIX \"\")\n";
    }
    {
        std::ofstream f(source / "probe.cpp", std::ios::binary | std::ios::trunc);
        f << "int he_quoting_probe() { return 42; }\n";
    }

    HE::hccg::DylibBuildSpec spec;
    spec.sourceDir     = source;
    spec.buildDir      = root / "build dir";
    spec.logFile       = root / "build.log";
    spec.artifactNames = HE::hccg::gameLogicArtifactNames();
    spec.defines       = { "HE_QUOTING_MARKER=" + marker.generic_string() };
    spec.what          = "quoting probe library";

    HE::hccg::BuildOutcome out;
    {
        const BundledCmakeGuard guard(prefix);
        out = HE::hccg::buildDylib(spec);
    }
    std::string log;
    {
        std::ifstream f(spec.logFile, std::ios::binary);
        log.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    INFO("build message: " << out.message);
    INFO("build log tail: " << (log.size() > 1500 ? log.substr(log.size() - 1500) : log));
    REQUIRE(out.ok);
    CHECK(out.artifact.filename().stem() == "GameLogic");
    fs::remove_all(root, ec);
}
