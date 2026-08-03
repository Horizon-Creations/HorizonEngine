#pragma once

// ─── Running an external program ─────────────────────────────────────────────
// A real subprocess API: separate stdout and stderr, honest exit codes, an argv
// vector rather than a shell string, optional stdin, a timeout, and a kill that
// takes the whole process tree with it.
//
// This exists because the engine's previous way of doing this — a file-local
// popen() wrapper in HcCodegen.cpp — is fine for "run cmake once at startup" and
// wrong for anything interactive or long-running. The six differences that
// matter, each of which has bitten a real tool:
//
//   1. popen merges stdout and stderr, so structured output cannot be parsed
//      apart from progress chatter and warnings.
//   2. pclose returns a wait STATUS, not an exit code — a genuine exit 1 comes
//      back as 256. Programs that use exit codes as answers (git diff --quiet,
//      grep, cmp) are unusable through it.
//   3. It is a shell invocation, so every path with a space, quote or $ is a
//      quoting hazard, and a filename can inject a command.
//   4. There is no stdin, so nothing can answer a prompt or be fed a secret
//      without putting it on the command line where it lands in the process
//      table.
//   5. On Windows a GUI-subsystem process has no console, so each call flashes
//      one on screen. Fine once at startup; unusable for anything polled.
//   6. It cannot be cancelled. A network operation against an unreachable host
//      blocks forever, and the caller has no handle to kill.
//
// Everything here is synchronous and belongs on a worker thread. The line
// callbacks are invoked on the calling thread, so they may touch the caller's
// state without locking, but must not block.

#include "Types/Defines.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace HE::Proc {

struct Result
{
	// The program's own exit code — WEXITSTATUS on POSIX, GetExitCodeProcess on
	// Windows. A process killed by a signal reports 128 + signal, matching shell
	// convention. Meaningless when launchFailed or timedOut is set.
	int  exitCode     = -1;
	bool timedOut     = false;   // killed after Options::timeoutMs
	bool launchFailed = false;   // never started: not found, not executable, fork failed

	std::string out;             // full stdout, regardless of the line callback
	std::string err;             // full stderr, kept separate on purpose

	// Ran to completion and reported success. Deliberately not the same as
	// "exitCode == 0": a timed-out process can leave a stale zero behind.
	bool ok() const { return !launchFailed && !timedOut && exitCode == 0; }
};

struct Options
{
	// Program to run. A bare name is resolved through PATH by the OS; a path is
	// used as given. NOT passed through a shell — see the header note.
	std::filesystem::path exe;
	// Arguments, one element per argument. Spaces, quotes and metacharacters are
	// passed through literally; the caller never quotes anything.
	std::vector<std::string> args;

	// Working directory. Empty means "inherit".
	std::filesystem::path cwd;

	// Variables to add or override. With inheritEnv the parent's environment is
	// the base; without it, these are the entire environment.
	std::vector<std::pair<std::string, std::string>> env;
	bool inheritEnv = true;

	// Written to the child's stdin, which is then closed. A child that reads
	// until EOF therefore never hangs waiting for more.
	std::string stdinData;

	// Kill the child (and its whole group) after this many milliseconds.
	// 0 means "wait indefinitely" — only safe for work that cannot hang.
	std::uint32_t timeoutMs = 0;

	// Called per complete line as output arrives, for progress display. The
	// trailing newline (and a preceding \r) is stripped. A final unterminated
	// line is delivered too, when the stream closes.
	std::function<void(std::string_view)> onStdoutLine;
	std::function<void(std::string_view)> onStderrLine;
};

// Run to completion. Never throws; failures are reported in Result.
HE_API Result run(const Options& options);

// Prepend the common package-manager prefixes to PATH once per process, if they
// are not already there.
//
// A macOS or Linux app launched from Finder/Launchpad/Dock inherits a minimal
// PATH — /usr/bin:/bin:/usr/sbin:/sbin — without /opt/homebrew/bin (Apple
// Silicon) or /usr/local/bin (Intel). Tools installed by a package manager are
// then invisible, while the very same binary launched from a terminal finds them
// perfectly, which makes the failure look like anything except what it is.
//
// Idempotent and thread-safe; no-op on Windows, where PATH does not have this
// problem. Call before probing for or running any external tool.
HE_API void augmentToolPath();

// Locate an executable the way the OS would. Returns nothing when not found.
// Calls augmentToolPath() first, so a package-manager install is visible even
// from a Finder launch.
HE_API std::optional<std::filesystem::path> which(std::string_view exe);

} // namespace HE::Proc
