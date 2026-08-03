#pragma once

// ─── What the working tree looks like right now ──────────────────────────────
// One immutable snapshot, built whole on a worker thread and swapped in on the
// main thread. Never partially mutated: a half-updated status is how "the badge
// says modified but the file is clean" bugs happen, and rebuilding costs one
// git invocation.
//
// Paths are repository-relative with forward slashes, exactly as git reports
// them. Deliberately NOT keyed on HE::File*/HE::Folder* — the Content Browser
// rebuilds those trees wholesale on every refresh (GlobalState::populateFolder),
// so any pointer cached across a frame dangles.

#include "SourceControl/ScCommon.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace HE::Sc {

// The states git distinguishes in porcelain v2's XY field. Kept as a single
// enum used for both halves, because the same letters mean the same things on
// either side — what differs is only whether they describe the index or the
// working tree.
enum class FileState : std::uint8_t {
	Unmodified = 0,
	Modified,
	Added,
	Deleted,
	Renamed,
	Copied,
	TypeChanged,
	Untracked,
	Ignored,
	Conflicted,
};

HE_SC_API const char* fileStateName(FileState s);

struct FileEntry
{
	// Staged and unstaged status are separate because a file can be both: edited,
	// staged, then edited again. Collapsing them into one value is what makes a
	// UI claim a commit will include changes it will not.
	FileState index    = FileState::Unmodified;
	FileState worktree = FileState::Unmodified;

	// Set for renames and copies — the path this came from.
	std::string origPath;

	bool conflicted() const {
		return index == FileState::Conflicted || worktree == FileState::Conflicted;
	}
	// Anything a commit would care about. Ignored files are tracked in the
	// snapshot only so the UI can grey them out, and are not "changes".
	bool dirty() const {
		return conflicted() ||
		       (index    != FileState::Unmodified && index    != FileState::Ignored) ||
		       (worktree != FileState::Unmodified && worktree != FileState::Ignored);
	}
	bool staged() const {
		return index != FileState::Unmodified && index != FileState::Untracked &&
		       index != FileState::Ignored;
	}
};

struct RepoStatus
{
	bool        isRepo = false;
	std::string root;          // absolute path of the working tree

	std::string branch;        // "main"; empty when detached
	std::string upstream;      // "origin/main"; empty when none is configured
	std::string headOid;
	int         ahead  = 0;
	int         behind = 0;
	bool        detached      = false;
	bool        initialCommit = false;   // repository exists but HEAD has no commit yet

	// Repo-relative path → entry, only for files that are not plain unmodified.
	// A clean repository has an empty map, which is what makes the common case
	// cheap.
	std::unordered_map<std::string, FileEntry> files;

	// Every ancestor directory of every dirty file, precomputed here rather than
	// walked per folder tile. A folder badge is then one hash lookup instead of a
	// subtree scan, which matters because the Content Browser draws these every
	// frame.
	std::unordered_set<std::string> dirtyFolders;

	// Bumped on every successful refresh, so a consumer can tell "nothing
	// changed" from "not refreshed yet" without comparing whole maps.
	std::uint64_t generation = 0;

	// Lowercased path → the key as git reported it. git is case-sensitive;
	// macOS and Windows filesystems are not, so a path that came from the
	// filesystem can differ in case from the one git recorded and an exact
	// lookup would silently find nothing.
	std::unordered_map<std::string, std::string> caseIndex;

	// nullptr when the path has no entry, i.e. it is unmodified or untracked-
	// but-ignored. Handles the case difference described above.
	HE_SC_API const FileEntry* find(const std::string& repoRelativePath) const;

	// Inline rather than exported: these are one-line scans, and a trivial
	// predicate is not worth a cross-DLL call or a place in the module's ABI.
	// (They were declared-but-not-exported at first, which links fine everywhere
	// except Windows — the failure mode the repo's export discipline exists for.)
	bool hasConflicts() const
	{
		for (const auto& [path, e] : files) if (e.conflicted()) return true;
		return false;
	}
	// Files a commit would include, i.e. something is staged.
	bool hasStagedChanges() const
	{
		for (const auto& [path, e] : files) if (e.staged()) return true;
		return false;
	}
	std::size_t dirtyCount() const
	{
		std::size_t n = 0;
		for (const auto& [path, e] : files) if (e.dirty()) ++n;
		return n;
	}
};

// ── The parser, exposed so it can be tested without a repository ─────────────
// Input is the raw stdout of:
//   git --no-optional-locks status --porcelain=v2 -z --branch --untracked-files=all
//
// -z matters: porcelain v1 C-escapes and quotes paths containing spaces, quotes
// or non-ASCII, so parsing it means reimplementing git's unquoting. v2 with -z
// emits raw bytes with NUL terminators and no escaping at all, and rename
// records carry both paths.
//
// Returns false only on input that cannot be interpreted at all; unknown record
// types are skipped, so a newer git that adds one does not break the parse.
HE_SC_API bool parsePorcelainV2(const std::string& raw, RepoStatus& out);

// Fills dirtyFolders from files. Separate so the parser stays a pure
// transformation and the rollup can be tested on its own.
HE_SC_API void buildDirtyFolders(RepoStatus& status);

} // namespace HE::Sc
