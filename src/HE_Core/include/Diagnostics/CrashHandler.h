#pragma once
#include "Types/Defines.h"
#include <string>

// Signal-based crash handler: catches SIGSEGV, SIGABRT, SIGILL, SIGFPE, SIGBUS,
// writes a crash report (timestamp + signal + stack trace) to a .crash file next
// to the log, and then re-raises the signal so the OS can generate a core dump.
//
// Call CrashHandler::install() once at application startup before any other
// work. The crash file path defaults to "<logDir>/he_crash_<timestamp>.crash".
// Pass a directory override to install() to redirect crash files.
class HE_API CrashHandler
{
public:
    // Install signal handlers. crashDir is the directory for .crash files;
    // empty string → same directory as the engine log file.
    static void install(const std::string& crashDir = "");

    // Uninstall (restore previous signal handlers). Rarely needed.
    static void uninstall();
};
