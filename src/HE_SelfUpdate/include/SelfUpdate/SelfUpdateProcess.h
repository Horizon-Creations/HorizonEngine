#pragma once

// ─── The few operating-system calls the takeover needs ───────────────────────
// Starting a program that must OUTLIVE the editor, asking whether a process is
// still there, and the UTF-8 <-> path conversions around both. Everything here
// is a thin, separately testable piece, and none of it depends on the engine: the
// same source file is compiled into HorizonSelfUpdate (the editor starts the
// updater with it) AND into HorizonUpdater (the updater starts the new editor
// with it), the latter without any engine library — see SelfUpdateApplyCore.h.
//
// Why not HE::Proc::run: it is synchronous (it waits for the child and kills its
// process group when the timeout passes), which is exactly what an updater that
// has to outlive its parent must not do.

#include "SelfUpdate/SuCommon.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace HE::Su {

// A path from / to UTF-8. On Windows a narrow std::string is read in the active
// code page, so a user name with an umlaut turns a perfectly good path into
// another one; every path that crosses a command line or a file in this module
// goes through these two.
HE_SU_API std::filesystem::path pathFromUtf8(const std::string& utf8);
HE_SU_API std::string           utf8FromPath(const std::filesystem::path& path);

struct SpawnOptions
{
	std::filesystem::path    exe;
	std::vector<std::string> args;      // UTF-8, one element per argument, never quoted by the caller
	std::filesystem::path    cwd;       // empty = inherit
	// Where stdout and stderr go (appended to). Empty = the null device. stdin is
	// always the null device: a detached program has nobody to ask.
	std::filesystem::path    logFile;
	// 0 = return at once. Otherwise wait up to this long for the program to end:
	// SpawnResult::exited / exitCode then say how it went. A program that is still
	// running at the deadline is left running (exited = false) — the use is "did it
	// die at once?", not "run it to completion".
	std::uint32_t            waitForExitMs = 0;
};

struct SpawnResult
{
	bool        ok = false;
	std::string error;      // the OS's reason when !ok
	std::int64_t pid = 0;
	bool         exited   = false;   // only when waitForExitMs was set
	int          exitCode = 0;       // valid when exited; a signal death reads as 128 + signal
};

// Starts `exe` as a program of its own: its own session / no console, no
// inherited handles beyond the three standard ones, not a child that dies with
// the caller's process group or job. Returns as soon as the OS has created the
// process — it says nothing about whether the program then runs well.
HE_SU_API SpawnResult spawnDetached(const SpawnOptions& options);

HE_SU_API std::int64_t currentProcessId();

// Alive and not merely waiting to be reaped: a process that has exited but whose
// parent has not collected it yet (a zombie) counts as gone, because everything
// it held is released.
HE_SU_API bool processExists(std::int64_t pid);

// Blocks until the process is gone, or `timeoutMs` has passed. True = gone (or
// there never was such a process), false = still alive at the deadline.
HE_SU_API bool waitForProcessExit(std::int64_t pid, std::uint32_t timeoutMs);

// The running executable, symlinks resolved. Empty if the OS will not say.
HE_SU_API std::filesystem::path currentExecutablePath();

} // namespace HE::Su
