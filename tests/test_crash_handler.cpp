#include "doctest.h"
#include <Diagnostics/CrashHandler.h>
#include <filesystem>
#include <string>

TEST_CASE("CrashHandler install / uninstall does not crash")
{
    auto tmpDir = std::filesystem::temp_directory_path().string();
    // Installing twice is safe; second call overwrites the first.
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
