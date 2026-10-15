#pragma once

// ─── Editor-side entry point for the takeover ─────────────────────────────────
// Everything that can still be shown to a user runs HERE, in the editor process,
// before it commits to anything: preflightTakeover() answers "can this even be
// tried?" without changing anything outside a scratch directory, and
// beginTakeover() does the one-way part — extract, copy HorizonUpdater out,
// start it — right before the caller quits the editor. A failure from either
// function leaves the running editor exactly as it was: nothing under
// `installRoot` is touched until HorizonUpdater runs, and that only happens
// after beginTakeover() has already returned ok.
//
// What this does NOT do: decide whether it is safe to quit (unsaved changes,
// a running Play session) — that is the editor's existing save/discard/cancel
// gate (ProjectPreflight.cpp, the GuardedAction pattern in
// ProjectLaunchOpen.h). Call beginTakeover() from the same place that gate
// already guards, the way closing the window or opening another project does.

#include "SelfUpdate/SelfUpdateCheck.h"
#include "SelfUpdate/SuCommon.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace HE::Su {

struct TakeoverOptions
{
	// Same directory SelfUpdateCheck.h staged the build into.
	std::filesystem::path stagingRoot;
	// Passed through to the relaunched editor, e.g. the currently open project's path.
	std::vector<std::string> relaunchArgs;
	std::uint32_t waitTimeoutMs  = 120'000;
	std::uint32_t healthMs       = 8'000;
	std::uint32_t renameRetryMs  = 30'000;
};

struct TakeoverCheck
{
	bool ok = false;
	std::string error;   // human-readable; show it rather than offering the restart
};

// Checks everything that does not require committing to anything: the staged
// build re-verifies, the install directory's parent is writable, and
// HorizonUpdater exists next to the running editor. Does not extract and does
// not spawn anything — cheap enough to call before showing "Restart to update?".
HE_SU_API TakeoverCheck preflightTakeover(const TakeoverOptions& options);

struct TakeoverResult
{
	bool ok = false;
	std::string error;
};

// Extracts the staged build, copies HorizonUpdater to a temp location, starts
// it detached (waiting on THIS process's pid) and returns. The caller must quit
// right after a successful return — the updater is already waiting for exactly
// that. On failure, any scratch directory this created is removed again and the
// editor's own install is untouched; the caller should report the error and NOT quit.
HE_SU_API TakeoverResult beginTakeover(const TakeoverOptions& options);

// ─── What earlier takeovers left beside the install ───────────────────────────
// Every takeover works in <install parent>/.heselfupdate-<version>-<suffix>/,
// and after a successful swap its backup/ is the complete previous install
// (~120-150 MB). That is the manual way back, so one is kept: the one the
// running build was swapped in from. Every OLDER one is deleted at the next
// start of the editor (pruneTakeoverLeftoversForRunningBuild, on a worker).
//
// Which one is kept: the work directory last-apply.json names, when it says
// "applied" to exactly the running version. Right after a swap it does not say
// that yet — HorizonUpdater writes it only after its health watch, while the
// relaunched editor is already starting — so otherwise it is the newest one
// (by modification time, which the swap's two renames set) holding a backup/.
// Nothing younger than `grace` is touched at all: a second editor on the same
// install may be in the middle of its own takeover, and a takeover's payload/
// looks like any leftover until HorizonUpdater has swapped it in.
struct PruneOptions
{
	std::filesystem::path installRoot;     // installRootForRunningBuild(); its PARENT is searched
	std::filesystem::path lastApplyFile;   // <stagingRoot>/last-apply.json, may be missing
	std::string           runningVersion;  // runningBuild().version
	std::chrono::seconds  grace = std::chrono::minutes(15);
};

struct PruneResult
{
	std::string              kept;      // the one work directory kept, empty if there was none
	std::vector<std::string> removed;   // deleted
	std::vector<std::string> young;     // left alone, younger than `grace`
	std::vector<std::string> errors;    // could not be deleted (retried at the next start)
};

HE_SU_API PruneResult pruneTakeoverLeftovers(const PruneOptions& options);

// The same for the running executable's own install and the given staging
// directory (SelfUpdateCheck.h's defaultStagingDir()). Nothing to do — an
// empty result — for a build that does not sit where its platform expects.
HE_SU_API PruneResult pruneTakeoverLeftoversForRunningBuild(const std::filesystem::path& stagingRoot);

} // namespace HE::Su
