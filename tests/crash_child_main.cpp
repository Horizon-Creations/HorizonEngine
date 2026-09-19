// Test subject for CrashHandler — a program that installs the handler and then
// dies in one specified way, so a test can look at what it left behind.
//
// A crash handler cannot be tested in-process: the whole point is what happens
// AFTER the process is beyond saving. Only a real child that really dies shows
// whether the handler runs at all, whether it manages to write its file before
// the OS pulls the plug, and whether the exit status still says "crashed".
//
//   he_crash_child <crashDir> av     — write through a null pointer
//   he_crash_child <crashDir> abort  — std::abort()
//   he_crash_child <crashDir> none   — install, log, exit 0 (no report expected)

#include <Diagnostics/CrashHandler.h>
#include <Diagnostics/Log.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/resource.h>
#endif

int main(int argc, char** argv)
{
	if (argc < 3) { std::fputs("usage: he_crash_child <crashDir> <mode>\n", stderr); return 2; }
	const char* crashDir = argv[1];
	const char* mode     = argv[2];

#ifdef _WIN32
	// No "has stopped working" dialog and no "abort() has been called" box: a
	// modal window on a CI runner is a hang, not a failure.
	::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	_set_abort_behavior(0, _WRITE_ABORT_MSG);
#else
	// The handler re-raises the signal for a core dump; the test does not want
	// one littering /cores or the CI workspace.
	rlimit noCore{ 0, 0 };
	::setrlimit(RLIMIT_CORE, &noCore);
#endif

	CrashHandler::install(crashDir);
	// Ends up in the log ring, so the test can prove the report carries the
	// recent history and not just the trace.
	HE_LOG_INFO(Core, "crash-child-marker %s", mode);

	if (std::strcmp(mode, "av") == 0)
	{
		// Null derived from a runtime value so no compiler can prove the store
		// is undefined and delete it — the store has to really happen.
		const std::uintptr_t zero = argc > 1000 ? 1u : 0u;
		volatile int* p = reinterpret_cast<volatile int*>(zero);
		*p = 42;
		return 0;   // not reached
	}
	if (std::strcmp(mode, "abort") == 0)
	{
		std::abort();
	}
	if (std::strcmp(mode, "none") == 0)
	{
		return 0;
	}

	std::fprintf(stderr, "unknown mode: %s\n", mode);
	return 2;
}
