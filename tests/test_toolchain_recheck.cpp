// Tool Setup's Recheck after an automatic install (Thema 231).
//
// The install dialog installs cmake, then re-runs probeToolchain() so the rows turn
// green. resolveCmake() used to cache its answer for the process lifetime, the
// negative one included, and the editor's Recheck only re-sets the (unchanged) bundle
// directory, which is a no-op. So "cmake not found" at startup outlived the install
// and only a restart showed the new cmake.
//
// The test plays that sequence with a stand-in cmake: probe with nothing on PATH,
// "install" a cmake into a directory that is already on PATH, probe again the way
// Recheck does. The Windows case below plays the same sequence through the
// known-install-dir fallback instead (winget/the cmake.org MSI, not PATH).
#include "doctest.h"
#include <HorizonScene/HcCodegen.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>

namespace {

struct ScopedPath
{
    std::string saved;
    bool        had = false;
    ScopedPath()
    {
        if (const char* p = std::getenv("PATH")) { saved = p; had = true; }
    }
    void set(const std::string& v) { setenv("PATH", v.c_str(), 1); }
    ~ScopedPath()
    {
        if (had) setenv("PATH", saved.c_str(), 1); else unsetenv("PATH");
        // Do not leave the stand-in cmake in resolveCmake()'s cache for later tests.
        HE::hccg::probeToolchain();
    }
};

} // namespace

TEST_CASE("Toolchain Recheck: a cmake installed after a 'not found' probe is seen without a restart")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "he_toolchain_recheck";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "bin");

    HE::hccg::setBundledCmakeDir(root / "warmup");
    HE::hccg::probeToolchain(); // spends the one-time Homebrew PATH augmentation before PATH is replaced

    ScopedPath path;
    path.set((root / "bin").string());
    // A changed bundle directory is the one thing that drops the cache today; it
    // makes the next probe resolve against the PATH set above.
    HE::hccg::setBundledCmakeDir(root / "nobundle");

    const HE::hccg::ToolchainProbe before = HE::hccg::probeToolchain();
    REQUIRE_FALSE(before.cmakeFound);

    // "Install": a cmake that answers --version, in a directory that was on PATH all along.
    const fs::path fake = root / "bin" / "cmake";
    {
        std::ofstream f(fake, std::ios::binary | std::ios::trunc);
        f << "#!/bin/sh\necho 'cmake version 9.9.9'\nexit 0\n";
    }
    chmod(fake.c_str(), 0755);

    // What Recheck does: re-set the same bundle directory, then probe.
    HE::hccg::setBundledCmakeDir(root / "nobundle");
    const HE::hccg::ToolchainProbe after = HE::hccg::probeToolchain();
    CHECK(after.cmakeFound);
    CHECK(after.cmakeVersion == "9.9.9");

    fs::remove_all(root, ec);
}
#endif

#if defined(_WIN32)
#include <windows.h>

namespace {

// Finds the real cmake.exe this machine already has on PATH, before the test
// clears PATH to force resolveCmake() past the "cmake" bare-word lookup and into
// cmakeInKnownWindowsDir(). Using the real binary (not a stub) means the fallback
// is exercised exactly as winget/the cmake.org MSI leave it.
std::string findSystemCmakeExe()
{
    char buf[MAX_PATH];
    const DWORD n = SearchPathA(nullptr, "cmake.exe", nullptr, MAX_PATH, buf, nullptr);
    if (n == 0 || n > MAX_PATH) return {};
    return std::string(buf, n);
}

// Sets/deletes a batch of environment variables for the scope of one test, then
// restores exactly what was there before — PATH included, so a later test does
// not inherit the empty PATH or the stand-in ProgramFiles this test sets up.
struct ScopedEnvWin
{
    struct Saved { std::string name; bool had = false; std::string value; };
    std::vector<Saved> saved;

    void set(const char* name, const std::string& value)
    {
        Saved s; s.name = name;
        char*  buf = nullptr;
        size_t len = 0;
        if (_dupenv_s(&buf, &len, name) == 0 && buf) { s.had = true; s.value = buf; }
        if (buf) free(buf);
        saved.push_back(s);
        _putenv_s(name, value.c_str()); // an empty value_string deletes the variable (MSVC _putenv_s)
    }

    ~ScopedEnvWin()
    {
        for (auto it = saved.rbegin(); it != saved.rend(); ++it)
            _putenv_s(it->name.c_str(), it->had ? it->value.c_str() : "");
        // Do not leave the known-dir cmake or the cleared PATH in resolveCmake()'s
        // cache for later tests.
        HE::hccg::probeToolchain();
    }
};

} // namespace

TEST_CASE("Toolchain Recheck (Windows): cmake installed into a known install dir is seen without a restart")
{
    namespace fs = std::filesystem;

    const std::string systemCmake = findSystemCmakeExe();
    REQUIRE_FALSE(systemCmake.empty()); // this machine needs a real cmake for the stand-in

    const fs::path root = fs::temp_directory_path() / "he_toolchain_recheck_win";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "pf" / "CMake" / "bin", ec);

    ScopedEnvWin env;
    env.set("PATH", "");                           // "cmake" on PATH must not answer
    env.set("ProgramFiles", (root / "pf").string());
    env.set("ProgramW6432", "");
    env.set("ProgramFiles(x86)", "");
    env.set("LOCALAPPDATA", "");

    // A bundle directory that never exists: the bundled-cmake check always misses,
    // same as a fresh install with no bundled cmake next to the editor.
    HE::hccg::setBundledCmakeDir(root / "nobundle");
    const HE::hccg::ToolchainProbe before = HE::hccg::probeToolchain();
    REQUIRE_FALSE(before.cmakeFound);

    // "Install": winget (Kitware.CMake) / the cmake.org MSI drop cmake.exe at
    // %ProgramFiles%\CMake\bin\cmake.exe — a path this already-running process's
    // PATH never sees, which is exactly the bug (Thema 231).
    fs::copy_file(systemCmake, root / "pf" / "CMake" / "bin" / "cmake.exe",
                  fs::copy_options::overwrite_existing, ec);
    REQUIRE_FALSE(ec);

    // What Recheck does: re-set the same bundle directory, then probe.
    HE::hccg::setBundledCmakeDir(root / "nobundle");
    const HE::hccg::ToolchainProbe after = HE::hccg::probeToolchain();
    CHECK(after.cmakeFound);
    CHECK_FALSE(after.cmakeVersion.empty());
    // buildDylib()/toolchainAvailable() read the cache resolveCmake() just refreshed,
    // not a fresh probe — this is what lets a GameLogic build right after Recheck
    // use the newly-found cmake instead of the stale "not found".
    CHECK(HE::hccg::toolchainAvailable());

    fs::remove_all(root, ec);
}
#endif
