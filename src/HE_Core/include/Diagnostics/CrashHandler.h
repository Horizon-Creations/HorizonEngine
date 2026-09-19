#pragma once
#include "Types/Defines.h"
#include <string>

// Crash handler: writes a crash report (timestamp + cause + stack trace + the
// last log lines) to a .crash file and then lets the process die the way it
// would have anyway.
//
//   POSIX   — sigaction for SIGSEGV, SIGABRT, SIGILL, SIGFPE, SIGBUS; the signal
//             is re-raised afterwards so the OS still produces a core dump.
//   Windows — SetUnhandledExceptionFilter for structured exceptions (access
//             violation, stack overflow, illegal instruction, uncaught C++
//             exception, …) plus a SIGABRT hook for abort(), which the UCRT ends
//             with a fast fail that no exception filter ever sees. Next to the
//             .crash file a minidump (.dmp, same stem) is written for a debugger.
//             The process then exits with the exception code as its exit status.
//
// Call CrashHandler::install() once at application startup before any other
// work. The crash file path is "<crashDir>/he_crash_<timestamp>.crash".
class HE_API CrashHandler
{
public:
    // Install the handlers. crashDir is the directory for .crash files;
    // empty string → the system temp directory ($TMPDIR), NOT the log directory.
    // Installing twice only updates the directory.
    static void install(const std::string& crashDir = "");

    // Uninstall (restore previous handlers). Rarely needed.
    static void uninstall();
};
