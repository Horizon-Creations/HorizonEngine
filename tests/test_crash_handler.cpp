#include "doctest.h"
#include <Diagnostics/CrashHandler.h>
#include <Platform/Process.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

TEST_CASE("CrashHandler install / uninstall does not crash")
{
    auto tmpDir = std::filesystem::temp_directory_path().string();
    // Installing twice is safe; second call only moves the directory.
    CrashHandler::install(tmpDir);
    CrashHandler::install(tmpDir);
    CrashHandler::uninstall();
    CrashHandler::uninstall(); // idempotent
    CHECK(true); // reached without crashing
}

TEST_CASE("CrashHandler install with empty dir uses temp directory")
{
    // Should silently default to the temp directory — just verify no throw/crash
    CrashHandler::install();
    CrashHandler::uninstall();
    CHECK(true);
}

// ─── End to end: a child process that really dies ────────────────────────────
// Everything above only shows that installing is harmless. Whether the handler
// RUNS when it matters is only visible from outside the dying process, so the
// cases below spawn he_crash_child (see crash_child_main.cpp) and read what it
// left behind. The report file's stem is a one-second timestamp, so every case
// gets its own directory and expects exactly one report in it.

namespace {

fs::path freshCrashDir(const char* stem)
{
    static const auto salt = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    static int counter = 0;
    const fs::path dir = fs::temp_directory_path() /
        ("he_crash_test_" + std::string(stem) + "_" + std::to_string(salt) + "_" +
         std::to_string(counter++));
    fs::create_directories(dir);
    return dir;
}

std::vector<fs::path> filesWithExtension(const fs::path& dir, const char* ext)
{
    std::vector<fs::path> out;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().extension() == ext) out.push_back(e.path());
    return out;
}

std::string listing(const fs::path& dir)
{
    std::string s;
    for (const auto& e : fs::directory_iterator(dir))
        s += e.path().filename().string() + " ";
    return s;
}

std::string slurp(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

HE::Proc::Result runCrashChild(const fs::path& crashDir, const char* mode)
{
    HE::Proc::Options o;
    o.exe       = fs::path(HE_TEST_CRASH_CHILD);
    o.args      = { crashDir.string(), mode };
    o.timeoutMs = 60000;   // a handler that hangs must fail the test, not the suite
    return HE::Proc::run(o);
}

// The parts every crash report must have, whatever killed the process.
void checkCommonReport(const std::string& text, const char* mode)
{
    CHECK(text.find("=== HorizonEngine Crash Report ===") != std::string::npos);
    CHECK(text.find("--- Stack trace ---") != std::string::npos);
    CHECK(text.find("  #0") != std::string::npos);
    // The log ring made it in: the line the child wrote just before dying.
    CHECK(text.find(std::string("crash-child-marker ") + mode) != std::string::npos);
    CHECK(text.find("===================================") != std::string::npos);
}

} // namespace

TEST_CASE("A null-pointer write leaves a crash report and a non-zero exit")
{
    const fs::path dir = freshCrashDir("av");
    const HE::Proc::Result r = runCrashChild(dir, "av");
    CAPTURE(r.exitCode);
    CAPTURE(r.out);
    CAPTURE(r.err);
    CAPTURE(listing(dir));
    REQUIRE_FALSE(r.launchFailed);
    REQUIRE_FALSE(r.timedOut);
    // Not pinned to a number: 128+11 on POSIX, the (negative) NTSTATUS on
    // Windows. What matters is that the handler did not turn a crash into
    // a clean exit.
    CHECK(r.exitCode != 0);

    const auto reports = filesWithExtension(dir, ".crash");
    REQUIRE(reports.size() == 1);
    const std::string text = slurp(reports[0]);
    CAPTURE(text);
    checkCommonReport(text, "av");
#ifdef _WIN32
    CHECK(text.find("EXCEPTION_ACCESS_VIOLATION") != std::string::npos);
    CHECK(text.find("Access    : write of 0x0000000000000000") != std::string::npos);
    // The minidump sits next to the text report and is not empty.
    const auto dumps = filesWithExtension(dir, ".dmp");
    REQUIRE(dumps.size() == 1);
    CHECK(fs::file_size(dumps[0]) > 0);
    CHECK(text.find("Minidump  : ") != std::string::npos);
#else
    CHECK(text.find("SIGSEGV") != std::string::npos);
    CHECK(r.exitCode == 128 + 11);
#endif
    fs::remove_all(dir);
}

TEST_CASE("abort() leaves a crash report too")
{
    // Windows is the reason this case exists: the UCRT ends abort() with a
    // fast fail that no exception filter sees, so SIGABRT needs its own hook.
    const fs::path dir = freshCrashDir("abort");
    const HE::Proc::Result r = runCrashChild(dir, "abort");
    CAPTURE(r.exitCode);
    CAPTURE(r.out);
    CAPTURE(r.err);
    CAPTURE(listing(dir));
    REQUIRE_FALSE(r.launchFailed);
    REQUIRE_FALSE(r.timedOut);
    CHECK(r.exitCode != 0);

    const auto reports = filesWithExtension(dir, ".crash");
    REQUIRE(reports.size() == 1);
    const std::string text = slurp(reports[0]);
    CAPTURE(text);
    checkCommonReport(text, "abort");
    CHECK(text.find("SIGABRT") != std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("A process that does not crash leaves no report")
{
    const fs::path dir = freshCrashDir("none");
    const HE::Proc::Result r = runCrashChild(dir, "none");
    CAPTURE(r.exitCode);
    CAPTURE(r.err);
    CAPTURE(listing(dir));
    REQUIRE_FALSE(r.launchFailed);
    CHECK(r.ok());
    CHECK(filesWithExtension(dir, ".crash").empty());
    CHECK(filesWithExtension(dir, ".dmp").empty());
    fs::remove_all(dir);
}
