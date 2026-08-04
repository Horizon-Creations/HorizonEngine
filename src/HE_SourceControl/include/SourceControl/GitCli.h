#pragma once

// ─── Running git ─────────────────────────────────────────────────────────────
// A concrete class, not an interface. There will only ever be one
// implementation, and a test double would be worse than the real thing: a
// `git init` fixture in a temp directory exercises the actual command lines,
// the actual output format and the actual exit codes, all of which are what
// break. The seam for tests is the fixture, not a mock.
//
// Every call blocks on a subprocess and belongs on a worker thread — GitService
// owns that thread; nothing here should be called from the frame loop.

#include "SourceControl/RepoStatus.h"
#include "SourceControl/ScCommon.h"

#include <filesystem>
#include <string>
#include <vector>

namespace HE::Sc {

struct GitResult
{
	bool        ok       = false;
	int         exitCode = -1;
	std::string out;
	std::string err;      // git's own message, already scrubbed of credentials
};

class HE_SC_API GitCli {
public:
	// Run git in `cwd`. The environment is fixed up so git can never block on a
	// prompt it has no way to display.
	static GitResult run(const std::filesystem::path& cwd,
	                     const std::vector<std::string>& args,
	                     std::uint32_t timeoutMs = 30000);

	// The working tree containing `anyPathInside`, or an empty path when it is
	// not in a repository. Uses git rather than looking for a .git directory,
	// because .git can be a file (worktrees, submodules) and the answer for a
	// path inside a submodule is not simply "the nearest .git".
	static std::filesystem::path findRepoRoot(const std::filesystem::path& anyPathInside);

	// Full working-tree status. Fills `out` completely, including root, branch,
	// ahead/behind and the dirty-folder rollup.
	static bool status(const std::filesystem::path& root, RepoStatus& out, std::string* err = nullptr);

	// ── Mutating operations ──────────────────────────────────────────────────
	// Each is one git invocation with the timeout that matches its nature:
	// local commands get seconds, network transfers get minutes — a large LFS
	// push legitimately runs that long, and cutting it off mid-transfer is
	// strictly worse than waiting.

	// git init -b main. `err` is filled from git's own message on failure.
	static bool init(const std::filesystem::path& dir, std::string* err = nullptr);

	// git add -A: stage everything, including deletions and untracked files.
	static bool addAll(const std::filesystem::path& root, std::string* err = nullptr);

	// git commit -m. Fails cleanly when there is nothing staged or no identity
	// is configured — both come back as git's message, not as a mystery.
	static bool commit(const std::filesystem::path& root, const std::string& message,
	                   std::string* err = nullptr);

	// git push, with -u origin HEAD on the first one (no upstream yet) so the
	// branch tracks its remote counterpart from then on.
	static bool push(const std::filesystem::path& root, bool upstreamConfigured,
	                 std::string* err = nullptr);

	// git pull --ff-only: never invents a merge commit on its own. A diverged
	// branch comes back as an error telling the user what to decide.
	static bool pull(const std::filesystem::path& root, std::string* err = nullptr);

	// The URL of `origin`, or empty when no remote is configured.
	static std::string remoteUrl(const std::filesystem::path& root);

	// Point `origin` at `url`, creating or updating it.
	static bool setRemote(const std::filesystem::path& root, const std::string& url,
	                      std::string* err = nullptr);

	// ── History ──────────────────────────────────────────────────────────────
	struct CommitInfo
	{
		std::string shortOid;
		std::string subject;
		std::string author;
		std::string relTime;    // "2 hours ago" — git's own phrasing
		bool        unpushed = false;   // not yet on the upstream branch
	};

	// The most recent commits, newest first. `unpushed` is filled only when an
	// upstream exists. Field/record separators are the ASCII control characters
	// (0x1F/0x1E), so commit messages cannot break the framing.
	static bool log(const std::filesystem::path& root, std::size_t maxCount,
	                std::vector<CommitInfo>& out, std::string* err = nullptr);

	// ── Restoring an old state ───────────────────────────────────────────────
	// Put the working tree and index back to exactly how `commit` had them —
	// files added since are removed, files changed are reverted, files deleted
	// come back — WITHOUT moving the branch or discarding any history. The
	// difference lands as staged changes, which the caller then commits; that
	// commit is itself undoable, and nothing that was ever committed is lost.
	//
	// Deliberately not `reset --hard`: that erases commits, and a mis-click
	// would destroy work no backup elsewhere covers. `read-tree -u --reset` is
	// the plumbing that expresses "make the tree look like this" exactly,
	// including deletions, which `restore --source` cannot do (it never removes
	// files that the source commit does not know about).
	//
	// The caller MUST have verified the tree is clean first: this overwrites
	// uncommitted work silently, exactly as git does.
	static bool restoreWorktreeTo(const std::filesystem::path& root,
	                              const std::string& commit, std::string* err = nullptr);

	// True when `commit` names something this repository actually has.
	static bool commitExists(const std::filesystem::path& root, const std::string& commit);

	// ── LFS ──────────────────────────────────────────────────────────────────
	// Whether `git lfs` answers at all in this environment.
	static bool lfsAvailable(const std::filesystem::path& root);

	// Track ONE exact path through LFS (appends to .gitattributes). --filename
	// treats the argument literally, so a path containing glob characters or
	// spaces cannot become a pattern by accident.
	static bool lfsTrack(const std::filesystem::path& root, const std::string& repoRelativePath,
	                     std::string* err = nullptr);

	// ── Credentials ──────────────────────────────────────────────────────────
	// The token is handed to git's OWN credential machinery and stored nowhere
	// else: `git credential approve` routes it into whichever helper is
	// configured (the platform keychain), and git-lfs authenticates through the
	// same chain — which is exactly why a custom store would be wrong.

	// The configured credential.helper, or empty when none is set anywhere.
	static std::string credentialHelper(const std::filesystem::path& root);

	// Configure the platform-default helper FOR THIS REPO when none is set:
	// osxkeychain on macOS, manager on Windows, cache on Linux. Fills
	// `outConfigured` with what was chosen (empty when one already existed).
	static bool ensureCredentialHelper(const std::filesystem::path& root,
	                                   std::string* outConfigured,
	                                   std::string* err = nullptr);

	// Feed one credential to the configured helper. `secret` travels via stdin,
	// never argv (argv is world-readable in a process list).
	static bool approveCredential(const std::filesystem::path& root,
	                              const std::string& host,
	                              const std::string& username,
	                              const std::string& secret,
	                              std::string* err = nullptr);
};

} // namespace HE::Sc
