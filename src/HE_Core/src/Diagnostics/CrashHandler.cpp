// fopen/localtime/strncpy are fine here and deliberately kept identical to the
// POSIX branch — MSVC's "unsafe" advisories (C4996) add nothing to a crash path.
#ifdef _WIN32
  #ifndef _CRT_SECURE_NO_WARNINGS
    #define _CRT_SECURE_NO_WARNINGS
  #endif
#endif

#include "Diagnostics/CrashHandler.h"
#include "Diagnostics/Log.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

// ─── Module-level state (both platforms) ─────────────────────────────────────

static char s_crashDir[512] = {};  // set by install()
static bool s_installed     = false;

// Report path "<crashDir>/he_crash_<timestamp><ext>". Static scratch buffer on
// purpose: on Windows the fallback path runs the handler on whatever stack the
// crash left behind, and a stack overflow leaves very little of it.
static char s_reportPath[768] = {};

static void buildReportPath(char* out, std::size_t n, const char* ext)
{
    std::time_t t = std::time(nullptr);
    char ts[32]{};
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", std::localtime(&t));
    std::snprintf(out, n, "%s/he_crash_%s%s", s_crashDir, ts, ext);
}

// Recent log history. The stack trace says WHERE it died; these lines say
// what the engine was doing on the way there — which asset was streaming in,
// which shader had just compiled, which script had just run.
static void writeRecentLog(FILE* f)
{
    std::fprintf(f, "\n--- Last %d log lines ---\n", HE::Log::kRingCapacity);
    HE::Log::forEachRecent([](const char* line, void* user) {
        std::fprintf(static_cast<FILE*>(user), "  %s\n", line);
    }, f);
}

#ifndef _WIN32
// ═══ POSIX: signals ══════════════════════════════════════════════════════════
#include <csignal>
#include <cstdlib>
#include <execinfo.h>  // backtrace / backtrace_symbols (POSIX)
#include <unistd.h>

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
    buildReportPath(s_reportPath, sizeof(s_reportPath), ".crash");

    FILE* f = std::fopen(s_reportPath, "w");
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

    writeRecentLog(f);

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

#else
// ═══ Windows: structured exception handling ══════════════════════════════════
//
// The counterpart of the signal handler above. SetUnhandledExceptionFilter
// catches everything the kernel delivers as an exception (access violation,
// stack overflow, illegal instruction, integer/float faults, an uncaught C++
// exception). It does NOT see abort(): the UCRT ends abort() with __fastfail,
// which bypasses every handler in the process — so SIGABRT is hooked through
// the CRT's own signal() as well, which abort() consults first.
//
// The report is written on a fresh helper thread: after a stack overflow the
// faulting thread has no stack left to format anything on, and DbgHelp needs
// plenty. The faulting thread's context is walked from there (StackWalk64 takes
// an explicit CONTEXT, so the walker does not have to run on the thread it
// describes). Without a thread, the report is written inline as a last resort.
#include <windows.h>
#include <dbghelp.h>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#pragma comment(lib, "dbghelp.lib")

static LPTOP_LEVEL_EXCEPTION_FILTER s_prevFilter  = nullptr;
static void (*s_prevSIGABRT)(int)                 = SIG_DFL;
// One report per process: a second fault while the first report is being
// written (another thread dying, or the handler itself crashing) must not
// start a second writer over the same file.
static volatile LONG s_reporting = 0;

static const char* exceptionName(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:               return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT:    return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DENORMAL_OPERAND:     return "EXCEPTION_FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INEXACT_RESULT:       return "EXCEPTION_FLT_INEXACT_RESULT";
    case EXCEPTION_FLT_INVALID_OPERATION:    return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW:             return "EXCEPTION_FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK:          return "EXCEPTION_FLT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW:            return "EXCEPTION_FLT_UNDERFLOW";
    case EXCEPTION_GUARD_PAGE:               return "EXCEPTION_GUARD_PAGE";
    case EXCEPTION_ILLEGAL_INSTRUCTION:      return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:            return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:             return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION:      return "EXCEPTION_INVALID_DISPOSITION";
    case EXCEPTION_INVALID_HANDLE:           return "EXCEPTION_INVALID_HANDLE";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_PRIV_INSTRUCTION:         return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_SINGLE_STEP:              return "EXCEPTION_SINGLE_STEP";
    case EXCEPTION_STACK_OVERFLOW:           return "EXCEPTION_STACK_OVERFLOW";
    case 0xE06D7363:                         return "C++ exception (uncaught)";
    case 0xC0000409:                         return "STATUS_STACK_BUFFER_OVERRUN (fast fail)";
    default:                                 return "Unknown exception";
    }
}

// What the report thread needs to know about the thread that died.
struct CrashJob
{
    EXCEPTION_POINTERS* ep;        // never null — the abort path fakes one
    DWORD               threadId;  // the faulting thread, for the minidump
    HANDLE              thread;    // real handle to it (pseudo handles are per-thread)
    const char*         label;     // overrides exceptionName(); null → derive from ep
};

static void writeStackTrace(FILE* f, const CrashJob& job)
{
    // StackWalk64 advances the context it is given — walk a copy.
    CONTEXT ctx = *job.ep->ContextRecord;
    STACKFRAME64 frame{};
    DWORD machine = 0;
#if defined(_M_X64)
    machine                 = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset     = ctx.Rip;
    frame.AddrFrame.Offset  = ctx.Rbp;
    frame.AddrStack.Offset  = ctx.Rsp;
#elif defined(_M_ARM64)
    machine                 = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset     = ctx.Pc;
    frame.AddrFrame.Offset  = ctx.Fp;
    frame.AddrStack.Offset  = ctx.Sp;
#elif defined(_M_IX86)
    machine                 = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset     = ctx.Eip;
    frame.AddrFrame.Offset  = ctx.Ebp;
    frame.AddrStack.Offset  = ctx.Esp;
#else
    std::fprintf(f, "  (no stack walker for this architecture)\n");
    return;
#endif
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;

    const HANDLE proc = ::GetCurrentProcess();
    ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES |
                    SYMOPT_FAIL_CRITICAL_ERRORS);
    // invadeProcess=TRUE enumerates the loaded modules, so module names resolve
    // even where no PDB exists — a shipped build still tells which DLL it was in.
    const BOOL symbols = ::SymInitialize(proc, nullptr, TRUE);

    alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 256];
    for (int i = 0; i < 64; ++i)
    {
        if (!::StackWalk64(machine, proc, job.thread, &frame, &ctx, nullptr,
                           ::SymFunctionTableAccess64, ::SymGetModuleBase64, nullptr))
            break;
        const DWORD64 pc = frame.AddrPC.Offset;
        if (pc == 0) break;

        char module[MAX_PATH] = "?";
        if (const DWORD64 base = ::SymGetModuleBase64(proc, pc))
        {
            if (::GetModuleFileNameA(reinterpret_cast<HMODULE>(static_cast<ULONG_PTR>(base)),
                                     module, MAX_PATH))
            {
                // Basename only: the directory is the same for every frame.
                const char* slash = std::strrchr(module, '\\');
                if (slash) std::memmove(module, slash + 1, std::strlen(slash + 1) + 1);
            }
        }

        auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        std::memset(sym, 0, sizeof(SYMBOL_INFO));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen   = 255;
        DWORD64 symOffset = 0;
        const bool haveSym = symbols && ::SymFromAddr(proc, pc, &symOffset, sym);

        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineOffset = 0;
        const bool haveLine = symbols && ::SymGetLineFromAddr64(proc, pc, &lineOffset, &line);

        std::fprintf(f, "  #%-2d 0x%016llX  %s!%s + 0x%llX", i,
                     static_cast<unsigned long long>(pc), module,
                     haveSym ? sym->Name : "?",
                     static_cast<unsigned long long>(symOffset));
        if (haveLine)
            std::fprintf(f, "  (%s:%lu)", line.FileName, static_cast<unsigned long>(line.LineNumber));
        std::fputc('\n', f);
    }
    if (symbols) ::SymCleanup(proc);
}

// The report: same section layout as the POSIX .crash file, plus a minidump
// next to it, which is what a debugger on another machine opens.
static void writeCrashReport(CrashJob& job)
{
    buildReportPath(s_reportPath, sizeof(s_reportPath), ".crash");

    FILE* f = std::fopen(s_reportPath, "w");
    if (!f)
    {
        // Fallback to stderr only — still useful in CI
        f = stderr;
    }

    const EXCEPTION_RECORD* rec = job.ep->ExceptionRecord;
    std::fprintf(f, "=== HorizonEngine Crash Report ===\n");
    std::fprintf(f, "Exception : %s (0x%08lX)\n",
                 job.label ? job.label : exceptionName(rec->ExceptionCode),
                 static_cast<unsigned long>(rec->ExceptionCode));
    std::fprintf(f, "Address   : 0x%p\n", rec->ExceptionAddress);
    if ((rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
         rec->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && rec->NumberParameters >= 2)
    {
        // ExceptionInformation[0]: 0 read, 1 write, 8 execute (DEP); [1]: the address.
        const ULONG_PTR kind = rec->ExceptionInformation[0];
        std::fprintf(f, "Access    : %s of 0x%016llX\n",
                     kind == 0 ? "read" : kind == 1 ? "write" : kind == 8 ? "execute" : "?",
                     static_cast<unsigned long long>(rec->ExceptionInformation[1]));
    }
    std::fprintf(f, "Thread    : %lu\n", static_cast<unsigned long>(job.threadId));
    {
        std::time_t t = std::time(nullptr);
        std::fprintf(f, "Time      : %s", std::ctime(&t));
    }

    // Minidump beside the text report. Data segments + referenced memory keep
    // it small enough to attach to a bug report while still showing the values
    // the crashing frames were looking at.
    {
        static char dumpPath[768];
        buildReportPath(dumpPath, sizeof(dumpPath), ".dmp");
        const HANDLE h = ::CreateFileA(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            MINIDUMP_EXCEPTION_INFORMATION mei{};
            mei.ThreadId          = job.threadId;
            mei.ExceptionPointers = job.ep;
            mei.ClientPointers    = FALSE;   // pointers live in this process
            const auto type = static_cast<MINIDUMP_TYPE>(
                MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory |
                MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
            const BOOL ok = ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(),
                                                h, type, &mei, nullptr, nullptr);
            ::CloseHandle(h);
            if (ok) std::fprintf(f, "Minidump  : %s\n", dumpPath);
            else    std::fprintf(f, "Minidump  : failed (error %lu)\n",
                                 static_cast<unsigned long>(::GetLastError()));
        }
    }

    std::fprintf(f, "\n--- Stack trace ---\n");
    writeStackTrace(f, job);

    writeRecentLog(f);

    std::fprintf(f, "===================================\n");
    if (f != stderr) std::fclose(f);
}

static DWORD WINAPI reportThreadMain(LPVOID p)
{
    writeCrashReport(*static_cast<CrashJob*>(p));
    return 0;
}

static void reportFromFaultingThread(CrashJob& job)
{
    // Serialise: whoever gets here second parks until the first writer has
    // finished and ended the process. A crash INSIDE the report thread lands
    // here too, so the wait below is bounded rather than infinite.
    if (::InterlockedCompareExchange(&s_reporting, 1, 0) != 0)
    {
        for (;;) ::Sleep(1000);
    }

    job.threadId = ::GetCurrentThreadId();
    job.thread   = ::OpenThread(THREAD_ALL_ACCESS, FALSE, job.threadId);
    if (!job.thread) job.thread = ::GetCurrentThread();   // inline fallback only

    const HANDLE t = ::CreateThread(nullptr, 512 * 1024, reportThreadMain, &job, 0, nullptr);
    if (t)
    {
        ::WaitForSingleObject(t, 60000);
        ::CloseHandle(t);
    }
    else
    {
        writeCrashReport(job);
    }
    if (job.thread != ::GetCurrentThread()) ::CloseHandle(job.thread);
}

static LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep)
{
    CrashJob job{ ep, 0, nullptr, nullptr };
    reportFromFaultingThread(job);

    // Hand on to whoever was installed before us (a debugger tool, a
    // telemetry SDK). Otherwise EXECUTE_HANDLER: the process ends with the
    // exception code as exit status and no WER dialog — our minidump is
    // already on disk, the POSIX counterpart of re-raising for a core dump.
    if (s_prevFilter && s_prevFilter != crashFilter)
        return s_prevFilter(ep);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void abortHandler(int sig)
{
    // No kernel exception to describe, so build the context by hand: the
    // stack walker starts from here and the minidump gets a real thread state.
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    ::RtlCaptureContext(&ctx);
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode    = 0xC0000409;   // what the CRT's fast fail will report
    rec.ExceptionAddress = reinterpret_cast<PVOID>(&abortHandler);
    EXCEPTION_POINTERS ep{ &rec, &ctx };

    CrashJob job{ &ep, 0, nullptr, "SIGABRT (Abort)" };
    reportFromFaultingThread(job);

    // Same courtesy as the filter: a handler that was there before us runs.
    if (s_prevSIGABRT && s_prevSIGABRT != SIG_DFL && s_prevSIGABRT != SIG_IGN &&
        s_prevSIGABRT != abortHandler)
        s_prevSIGABRT(sig);
    // Return and let abort() carry on to its fast fail — the process still
    // dies the way it would have without us.
}

#endif // _WIN32

// ─── Public API ──────────────────────────────────────────────────────────────

void CrashHandler::install(const std::string& crashDir)
{
    // Resolve crash directory (default: system temp dir)
    std::string dir = crashDir;
    if (dir.empty())
        dir = std::filesystem::temp_directory_path().string();

    std::strncpy(s_crashDir, dir.c_str(), sizeof(s_crashDir) - 1);
    s_crashDir[sizeof(s_crashDir) - 1] = '\0';

    // A second install only moves the directory. Hooking again would record
    // OUR handler as the "previous" one — on Windows the filter would then
    // call itself, and uninstall() could never restore the real predecessor.
    if (s_installed) return;
    s_installed = true;

#ifndef _WIN32
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
    s_prevFilter  = ::SetUnhandledExceptionFilter(crashFilter);
    s_prevSIGABRT = std::signal(SIGABRT, abortHandler);
    // Room for the filter itself after a stack overflow on the installing
    // (main) thread — the report proper runs on its own thread anyway.
    ULONG guarantee = 64 * 1024;
    ::SetThreadStackGuarantee(&guarantee);
#endif
}

void CrashHandler::uninstall()
{
    if (!s_installed) return;
    s_installed = false;
#ifndef _WIN32
    sigaction(SIGSEGV, &s_prevSIGSEGV, nullptr);
    sigaction(SIGABRT, &s_prevSIGABRT, nullptr);
    sigaction(SIGILL,  &s_prevSIGILL,  nullptr);
    sigaction(SIGFPE,  &s_prevSIGFPE,  nullptr);
    sigaction(SIGBUS,  &s_prevSIGBUS,  nullptr);
#else
    ::SetUnhandledExceptionFilter(s_prevFilter);
    std::signal(SIGABRT, s_prevSIGABRT);
#endif
}
