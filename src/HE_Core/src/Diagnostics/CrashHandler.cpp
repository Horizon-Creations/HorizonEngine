#include "Diagnostics/CrashHandler.h"

#ifndef _WIN32
#include <csignal>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <execinfo.h>  // backtrace / backtrace_symbols (POSIX)
#include <unistd.h>

// ─── Module-level state ──────────────────────────────────────────────────────

static char s_crashDir[512] = {};  // set by install()

static struct sigaction s_prevSIGSEGV {};
static struct sigaction s_prevSIGABRT {};
static struct sigaction s_prevSIGILL  {};
static struct sigaction s_prevSIGFPE  {};
static struct sigaction s_prevSIGBUS  {};

static const char* signalName(int sig)
{
    switch (sig)
    {
    case SIGSEGV: return "SIGSEGV (Segmentation fault)";
    case SIGABRT: return "SIGABRT (Abort)";
    case SIGILL:  return "SIGILL (Illegal instruction)";
    case SIGFPE:  return "SIGFPE (Floating-point exception)";
    case SIGBUS:  return "SIGBUS (Bus error)";
    default:      return "Unknown signal";
    }
}

static void crashHandler(int sig, siginfo_t*, void*)
{
    // Build crash file path: <crashDir>/he_crash_<timestamp>.crash
    char path[768];
    {
        std::time_t t = std::time(nullptr);
        char ts[32]{};
        std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", std::localtime(&t));
        std::snprintf(path, sizeof(path), "%s/he_crash_%s.crash", s_crashDir, ts);
    }

    FILE* f = std::fopen(path, "w");
    if (!f)
    {
        // Fallback to stderr only — still useful in CI
        f = stderr;
    }

    std::fprintf(f, "=== HorizonEngine Crash Report ===\n");
    std::fprintf(f, "Signal : %s (%d)\n", signalName(sig), sig);
    {
        std::time_t t = std::time(nullptr);
        std::fprintf(f, "Time   : %s", std::ctime(&t));
    }
    std::fprintf(f, "\n--- Stack trace ---\n");

    void* frames[64];
    const int depth = backtrace(frames, 64);
    char** syms = backtrace_symbols(frames, depth);
    if (syms)
    {
        for (int i = 0; i < depth; ++i)
            std::fprintf(f, "  #%d  %s\n", i, syms[i]);
        // backtrace_symbols allocates with malloc; safe to call free here
        // because the heap is likely still intact for non-heap corruption crashes.
        std::free(syms);
    }
    else
    {
        // Write raw addresses if symbol resolution failed
        for (int i = 0; i < depth; ++i)
            std::fprintf(f, "  #%d  %p\n", i, frames[i]);
    }

    std::fprintf(f, "===================================\n");
    if (f != stderr) std::fclose(f);

    // Re-raise with default handler so the OS generates a core dump and the
    // process exits with the correct non-zero status.
    struct sigaction dflt{};
    dflt.sa_handler = SIG_DFL;
    sigemptyset(&dflt.sa_mask);
    sigaction(sig, &dflt, nullptr);
    raise(sig);
}

#endif // !_WIN32

// ─── Public API ──────────────────────────────────────────────────────────────

void CrashHandler::install(const std::string& crashDir)
{
#ifndef _WIN32
    // Resolve crash directory (default: system temp dir)
    std::string dir = crashDir;
    if (dir.empty())
        dir = std::filesystem::temp_directory_path().string();

    std::strncpy(s_crashDir, dir.c_str(), sizeof(s_crashDir) - 1);
    s_crashDir[sizeof(s_crashDir) - 1] = '\0';

    struct sigaction sa{};
    sa.sa_sigaction = crashHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;

    sigaction(SIGSEGV, &sa, &s_prevSIGSEGV);
    sigaction(SIGABRT, &sa, &s_prevSIGABRT);
    sigaction(SIGILL,  &sa, &s_prevSIGILL);
    sigaction(SIGFPE,  &sa, &s_prevSIGFPE);
    sigaction(SIGBUS,  &sa, &s_prevSIGBUS);
#else
    (void)crashDir;
    // TODO: Windows structured exception handling (SEH) / SetUnhandledExceptionFilter
#endif
}

void CrashHandler::uninstall()
{
#ifndef _WIN32
    sigaction(SIGSEGV, &s_prevSIGSEGV, nullptr);
    sigaction(SIGABRT, &s_prevSIGABRT, nullptr);
    sigaction(SIGILL,  &s_prevSIGILL,  nullptr);
    sigaction(SIGFPE,  &s_prevSIGFPE,  nullptr);
    sigaction(SIGBUS,  &s_prevSIGBUS,  nullptr);
#endif
}
