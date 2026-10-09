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
	// `amend` rewrites the last commit with what is staged (an empty message keeps
	// the old one). The caller decides whether that commit may still be rewritten.
	static bool commit(const std::filesystem::path& root, const std::string& message,
	                   std::string* err = nullptr,
	                   bool amend = false);

	// git push, with -u origin HEAD on the first one (no upstream yet) so the
	// branch tracks its remote counterpart from then on.
	static bool push(const std::filesystem::path& root, bool upstreamConfigured,
	                 std::string* err = nullptr);

	// git pull --ff-only: never invents a merge commit on its own. A diverged
	// branch comes back as an error telling the user what to decide.
	static bool pull(const std::filesystem::path& root, std::string* err = nullptr);

	// git fetch --prune: update the remote-tracking refs and nothing else. The
	// working tree, the index and HEAD are all untouched, which is what makes
	// this safe to run on a timer — the ahead/behind counters become true
	// without anything on disk moving under the user.
	//
	// --prune so branches deleted on the remote stop being reported as existing;
	// without it a long-lived editor accumulates refs to things that are gone.
	static bool fetch(const std::filesystem::path& root, std::string* err = nullptr);

	// The URL of `origin`, or empty when no remote is configured.
	static std::string remoteUrl(const std::filesystem::path& root);

	// Point `origin` at `url`, creating or updating it.
	static bool setRemote(const std::filesystem::path& root, const std::string& url,
	                      std::string* err = nullptr);

	// ── Cloning ──────────────────────────────────────────────────────────────
	// Is `url` something the editor may clone from? https:// WITHOUT userinfo,
	// or file:// (a local bare repository — what the tests clone from). Refused:
	// anything carrying a user or token before the host (the token would end up
	// in .git/config and every error message), plain http:// (the credential
	// would cross the wire readable), ssh/scp forms (no credential-helper path),
	// a leading '-' (option injection) and control characters. `why` names the
	// reason for the refusal.
	static bool isSafeCloneUrl(const std::string& url, std::string* why = nullptr);

	// The host of an https:// URL ("github.com"), empty for anything else. What
	// a credential for that URL is stored under.
	static std::string urlHost(const std::string& url);

	// git clone `url` into `targetDir`, which must not exist yet or be an empty
	// directory — a clone never merges into a folder that already holds files.
	//
	// LFS content is deliberately NOT downloaded here (GIT_LFS_SKIP_SMUDGE):
	// with the global smudge filter active, one failed LFS download aborts the
	// clone in the middle of checkout and leaves half a repository. Skipping it
	// yields a complete git checkout first; lfsPull() then fetches the large
	// files as a separate step that can be retried on its own.
	//
	// `helperOverride`, when non-empty, becomes the only credential helper for
	// this command (`-c credential.helper= -c credential.helper=…`): before the
	// clone there is no repository to hold a --local setting, and git exports
	// -c config to every child, so git-lfs authenticates through the same
	// helper. On failure the target is left as empty as it was found.
	static bool clone(const std::string& url, const std::filesystem::path& targetDir,
	                  const std::string& helperOverride, std::string* err = nullptr);

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

	// ── Branches ─────────────────────────────────────────────────────────────
	// Does git consider this a legal branch name? Checked separately from
	// creating one so the panel can say "that name will not work" while the
	// user is still typing, rather than after a failed command.
	static bool isValidBranchName(const std::filesystem::path& root,
	                              const std::string& name);

	// Already taken? A create would fail anyway; asking first turns that into a
	// clear message instead of git's.
	static bool branchExists(const std::filesystem::path& root, const std::string& name);

	// Create `name` pointing at `startCommit` (empty = HEAD). With `checkout`
	// the new branch also becomes the current one — which REPLACES the working
	// tree, so callers must have verified it is clean first. Without it, only a
	// ref is written and nothing on disk changes.
	static bool createBranch(const std::filesystem::path& root, const std::string& name,
	                         const std::string& startCommit, bool checkout,
	                         std::string* err = nullptr);

	// Every local branch, sorted, plus which one is checked out.
	static bool listBranches(const std::filesystem::path& root,
	                         std::vector<std::string>& out, std::string& outCurrent,
	                         std::string* err = nullptr);

	// ── Per-file operations (the Source Control panel's stage / discard) ─────
	// Paths are repository-relative with forward slashes, as RepoStatus keys them.
	// They are passed as LITERAL pathspecs, so a file called "[draft]*.hasset"
	// means that file and not a glob. Long lists are split into several git
	// invocations (Windows caps a command line near 32k characters).

	// True when HEAD points at a commit (false in a repository with no commits).
	static bool hasHead(const std::filesystem::path& root);

	// git add -A -- paths: stages additions, edits AND deletions of exactly these.
	static bool stage(const std::filesystem::path& root,
	                  const std::vector<std::string>& paths, std::string* err = nullptr);

	// Takes paths back out of the index without touching the working tree. A rename
	// is undone as a pair (the new path and the one it came from). Works in a
	// repository with no commits too.
	static bool unstage(const std::filesystem::path& root,
	                    const std::vector<std::string>& paths, std::string* err = nullptr);

	// Throws the changes to these paths away, staged and unstaged alike:
	//   untracked            -> deleted from disk
	//   new in the index     -> removed from index and disk
	//   edited/deleted/...   -> back to how HEAD has them
	// Conflicted paths are refused (resolveConflict is the way out for those).
	// Irreversible - the caller asks first.
	static bool discard(const std::filesystem::path& root,
	                    const std::vector<std::string>& paths, std::string* err = nullptr);

	// Settles a conflicted path with one side's version and stages the result.
	// `ours` = the branch you are on, otherwise the incoming side.
	static bool resolveConflict(const std::filesystem::path& root, const std::string& path,
	                            bool ours, std::string* err = nullptr);

	// git switch <name>. A remote-only name ("origin/x" or just "x" when exactly one
	// remote has it) is checked out as a new local branch tracking it. Refuses -
	// with git's own wording - when local changes would be overwritten.
	static bool switchBranch(const std::filesystem::path& root, const std::string& name,
	                         std::string* err = nullptr);

	// Remote-tracking branches ("origin/main"), without the symbolic HEAD entry.
	static bool listRemoteBranches(const std::filesystem::path& root,
	                               std::vector<std::string>& out, std::string* err = nullptr);

	// Stash everything including untracked files, so a branch switch starts clean.
	static bool stashPush(const std::filesystem::path& root, const std::string& message,
	                      std::string* err = nullptr);
	// Re-apply the newest stash and drop it. On a conflict the stash is kept (git's rule).
	static bool stashPop(const std::filesystem::path& root, std::string* err = nullptr);
	// One line per stash, newest first ("stash@{0}: On main: message").
	static bool stashList(const std::filesystem::path& root, std::vector<std::string>& out,
	                      std::string* err = nullptr);

	// The files one commit touched, with git's one-letter state (A/M/D/R/C/T).
	struct ChangedFile
	{
		char        state = 'M';
		std::string path;
		std::string origPath;   // renames and copies
	};
	static bool commitFiles(const std::filesystem::path& root, const std::string& commit,
	                        std::vector<ChangedFile>& out, std::string* err = nullptr);

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

	// Does the checked-out tree route anything through LFS? Answered from the
	// committed .gitattributes files (any "filter=lfs" line), so it works on a
	// machine without git-lfs — which is exactly the machine that needs to be
	// told its assets are pointer files. Attributes configured outside the
	// repository (global gitattributes) are not seen.
	static bool usesLfs(const std::filesystem::path& root);

	// Download and check out every LFS object the current checkout references.
	// A plain `git pull` does not do this for objects that are merely missing,
	// so this is also the retry after a cut-off download.
	static bool lfsPull(const std::filesystem::path& root, std::string* err = nullptr);

	// ── Credentials ──────────────────────────────────────────────────────────
	// The token is handed to git's OWN credential machinery and stored nowhere
	// else: `git credential approve` routes it into whichever helper is
	// configured (the platform keychain), and git-lfs authenticates through the
	// same chain — which is exactly why a custom store would be wrong.

	// The configured credential.helper, or empty when none is set anywhere.
	// Outside a repository this is the system + global answer.
	static std::string credentialHelper(const std::filesystem::path& root);

	// The helper this module picks when none is configured: osxkeychain on
	// macOS, manager on Windows, a bounded in-memory cache on Linux.
	static std::string defaultCredentialHelper();

	// Configure the platform-default helper FOR THIS REPO when none is set:
	// osxkeychain on macOS, manager on Windows, cache on Linux. Fills
	// `outConfigured` with what was chosen (empty when one already existed).
	static bool ensureCredentialHelper(const std::filesystem::path& root,
	                                   std::string* outConfigured,
	                                   std::string* err = nullptr);

	// Feed one credential to the configured helper. `secret` travels via stdin,
	// never argv (argv is world-readable in a process list). `helperOverride`
	// works as in clone(): for storing a token before any repository exists.
	static bool approveCredential(const std::filesystem::path& root,
	                              const std::string& host,
	                              const std::string& username,
	                              const std::string& secret,
	                              std::string* err = nullptr,
	                              const std::string& helperOverride = {});

	// Read a stored credential back out of the helper (`git credential fill`),
	// so a user already signed in for source control is not asked to produce a
	// token a second time for something else.
	//
	// Strictly non-interactive: terminal prompting and the GUI helpers'
	// interactive modes are disabled, so "nothing stored" comes back as false
	// rather than as a dialog appearing behind the editor window. Returns false
	// with an empty `outSecret` for that case — not an error worth showing.
	// `helperOverride` as in approveCredential: a token stored with an override
	// is only found again through the same one.
	static bool fillCredential(const std::filesystem::path& root,
	                           const std::string& host,
	                           std::string& outUsername,
	                           std::string& outSecret,
	                           std::string* err = nullptr,
	                           const std::string& helperOverride = {});

	// Tell the helper to forget a credential (`git credential reject`) — the
	// sign-out. Pass the username and secret that fillCredential returned: some
	// helpers (newer osxkeychain) only erase an entry whose password matches.
	// `secret` travels via stdin, like approve.
	static bool rejectCredential(const std::filesystem::path& root,
	                             const std::string& host,
	                             const std::string& username,
	                             const std::string& secret,
	                             std::string* err = nullptr,
	                             const std::string& helperOverride = {});
};

} // namespace HE::Sc
