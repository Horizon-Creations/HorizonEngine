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
// Recheck does. POSIX only; the Windows install path (winget + a PATH the running
// process never sees) cannot be driven from a unit test.
#include "doctest.h"
#include <HorizonScene/HcCodegen.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

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
