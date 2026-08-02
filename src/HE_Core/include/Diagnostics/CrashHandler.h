#pragma once
#include "Types/Defines.h"
#include <string>

// Signal-based crash handler: catches SIGSEGV, SIGABRT, SIGILL, SIGFPE, SIGBUS,
// writes a crash report (timestamp + signal + stack trace) to a .crash file, and
// then re-raises the signal so the OS can generate a core dump. POSIX only —
// install() is a no-op on Windows (no SEH handler yet).
//
// Call CrashHandler::install() once at application startup before any other
// work. The crash file path is "<crashDir>/he_crash_<timestamp>.crash".
class HE_API CrashHandler
{
public:
    // Install signal handlers. crashDir is the directory for .crash files;
    // empty string → the system temp directory ($TMPDIR), NOT the log directory.
    static void install(const std::string& crashDir = "");

    // Uninstall (restore previous signal handlers). Rarely needed.
    static void uninstall();
};
