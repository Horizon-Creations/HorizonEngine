#pragma once

// ─── Git off the frame thread ────────────────────────────────────────────────
// One owned worker thread with a command queue in and an event queue out.
//
// Why not std::async, which the collaboration controller uses: a std::async
// future's destructor BLOCKS until its worker finishes. That is safe there only
// because every HTTPS call it makes carries a bounded timeout. Git has no such
// guarantee — a fetch against an unreachable host, or an LFS transfer of several
// gigabytes, runs as long as it runs — so closing a project would freeze the
// editor with no window updates and no way out.
//
// Commands are serialised by construction, which is also what git wants: two
// concurrent invocations in one working tree race on index.lock.
//
// Threading contract: the worker touches nothing but its own queues and the
// filesystem. Everything it produces is plain data, applied on the main thread
// in pump(). It never reaches into ContentManager, GlobalState or ImGui.

#include "SourceControl/GitCli.h"
#include "SourceControl/RepoStatus.h"
#include "SourceControl/ScCommon.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace HE::Sc {

class HE_SC_API GitService {
public:
	GitService();
	~GitService();

	GitService(const GitService&)            = delete;
	GitService& operator=(const GitService&) = delete;

	// Point the service at whatever repository contains `anyPathInside`, and
	// start the worker. Repo discovery itself runs on the worker, since it shells
	// out to git. Calling this again re-targets.
	void open(const std::filesystem::path& anyPathInside);

	// Stop the worker and forget the repository. Bounded by the timeout of
	// whichever command is in flight — see the note on cancellation below.
	void close();

	// Queue a status refresh. Repeated calls while one is pending collapse into
	// a single refresh: status is a whole-tree snapshot, so running it four times
	// in a row would just produce the same answer four times.
	void requestStatus();

	// ── Mutating operations ──────────────────────────────────────────────────
	// Serialized by construction on the worker (index.lock needs that anyway);
	// each runs the git command, then refreshes status so the UI reflects the
	// result without a second request. Outcome text lands in lastError() /
	// lastInfo() via pump(). The CALLER enforces the host-only collaboration
	// rule — this class knows git, not sessions.

	// git init in `projectRoot` + the generated .gitignore/.gitattributes +
	// `git lfs install --local` when git-lfs is available.
	void requestInit(const std::filesystem::path& projectRoot, bool lfsAvailable);

	// Stage everything and commit it with `message`; optionally push right
	// after a successful commit (the panel's auto-push toggle).
	void requestCommitAll(const std::string& message, bool pushAfter = false);

	// ── Per-file operations (see the matching GitCli functions) ──────────────
	// Stage / unstage / discard exactly these repository-relative paths. Staging
	// routes big media into LFS first, the same size pass a commit-all does -
	// a file added to the index BEFORE it is tracked would be stored as a plain blob.
	void requestStage(std::vector<std::string> paths);
	void requestUnstage(std::vector<std::string> paths);
	void requestDiscard(std::vector<std::string> paths);
	// Commit what is staged and nothing else. `amend` rewrites the last commit.
	void requestCommitStaged(const std::string& message, bool pushAfter, bool amend);
	// Keep one side of a conflicted path and stage it.
	void requestResolveConflict(const std::string& path, bool ours);
	// git switch. `stashFirst` stashes local changes (untracked included) so the
	// switch starts from a clean tree; without it git carries them along when it can
	// and refuses when it cannot.
	void requestSwitchBranch(const std::string& name, bool stashFirst);
	void requestStashPop();
	// Reads the files one commit touched; the answer lands in commitFiles().
	void requestCommitFiles(const std::string& commit);

	// The whole GitHub setup in one worker pass: create the repository via the
	// API, point origin at it, make sure a credential helper exists, hand the
	// token to that helper, and push. The token lives in the command for the
	// duration of the flow and is wiped when it completes. An empty `token`
	// means "the GitHub sign-in": read from the helper on the worker
	// (GitHubTokenStore::load).
	void requestSetupGitHub(const std::string& repoName, bool isPrivate,
	                        std::string token);

	// Put the project back to how `commit` had it and record that as a NEW
	// commit — history is preserved, so the restore itself can be undone. Runs
	// only on a clean tree; the worker refuses otherwise rather than silently
	// overwriting work in progress. `shortOid` is what the commit message says.
	void requestRestoreTo(const std::string& commit, const std::string& shortOid);

	// Create `name` at `startCommit` (empty = HEAD). `checkout` also moves onto
	// it, which replaces the working tree — refused on a dirty tree, exactly
	// like a restore. Without checkout only a ref is written, so a dirty tree
	// is no obstacle: nothing on disk changes.
	void requestCreateBranch(const std::string& name, const std::string& startCommit,
	                         bool checkout);

	// Store an access token for `host` without creating anything: make sure a
	// credential helper exists, then hand the token to it. This is the path for
	// a repository whose remote already exists (a pasted URL, a clone, a token
	// that expired) — the same credential machinery requestSetupGitHub uses, and
	// the token is likewise wiped once the helper has it.
	void requestStoreCredential(const std::string& host, const std::string& username,
	                            std::string token);

	// Clone `cloneUrl` into `targetDir` (absent or empty) and make that the
	// service's repository. Starts the worker when none is running, because a
	// clone is what happens BEFORE any project is open.
	//
	// One worker pass, in phases, each announced through lastInfo():
	//   1. `token` (optional — empty when the helper already holds one) goes to
	//      the credential helper, then is wiped. With no helper configured
	//      anywhere, the platform default is passed per command, since there is
	//      no repository yet to hold a --local setting.
	//   2. git clone, WITHOUT LFS content (see GitCli::clone).
	//   3. The same helper is pinned repo-locally, so later push/pull find it.
	//   4. If the tree uses LFS: `git lfs install --local` + `git lfs pull`.
	// The URL must pass GitCli::isSafeCloneUrl — a token in the URL is refused,
	// never stripped and used.
	//
	// lastClonedRoot() names the new working tree as soon as the git part
	// succeeded — even when the LFS download then failed, because the
	// repository exists and requestLfsPull() can finish it.
	void requestClone(const std::string& cloneUrl, const std::filesystem::path& targetDir,
	                  std::string token = {});

	// Download the LFS objects the checkout is missing: the retry after a cut
	// off or failed download, which a plain pull does not perform.
	void requestLfsPull();

	void requestPush(bool upstreamConfigured);
	void requestPull();

	// Update the remote-tracking refs without touching anything on disk, then
	// refresh, so ahead/behind become true. `quiet` suppresses the "Fetched."
	// note in lastInfo(): a fetch the user asked for is worth confirming, one
	// that fired on a timer is not — a panel that keeps announcing itself every
	// quarter of an hour trains people to stop reading it.
	void requestFetch(bool quiet = false);
	void requestSetRemote(const std::string& url);

	// Human-readable outcome of the last completed operation ("Pushed.",
	// "Committed 12 file(s)."), cleared by the next one.
	const std::string& lastInfo() const { return m_lastInfo; }

	// Working tree of the last clone whose git part succeeded; empty until then.
	const std::filesystem::path& lastClonedRoot() const { return m_lastClonedRoot; }

	// origin's URL as of the last status refresh; empty = none configured.
	const std::string& remoteUrl() const { return m_remoteUrl; }

	// Recent commits, newest first, as of the last status refresh.
	const std::vector<GitCli::CommitInfo>& recentCommits() const { return m_commits; }

	// Local branches as of the last status refresh (RepoStatus::branch already
	// names the current one).
	const std::vector<std::string>& branches() const { return m_branches; }
	// Remote-tracking branches ("origin/main"), and the stash list, newest first.
	const std::vector<std::string>& remoteBranches() const { return m_remoteBranches; }
	const std::vector<std::string>& stashes() const { return m_stashes; }
	// The files of a commit asked for with requestCommitFiles; nullptr until it answered.
	const std::vector<GitCli::ChangedFile>* commitFiles(const std::string& commit) const;

	// Drain finished work on the MAIN thread. `maxEvents` bounds how much is
	// applied per frame — a clone that produced twenty thousand entries should
	// not be absorbed in one frame.
	void pump(std::size_t maxEvents = 64);

	// ── State, valid to read from the main thread between pumps ──────────────
	bool               isRepo()  const { return m_status.isRepo; }
	const RepoStatus&  status()  const { return m_status; }
	// A command is queued or running. The UI shows progress rather than a stale
	// value, the same way the collaboration panel does for directory calls.
	bool               busy()    const { return m_busy.load(std::memory_order_acquire); }
	const std::string& lastError() const { return m_lastError; }

	// Called from pump(), i.e. on the main thread, after a refresh lands.
	void setOnStatusChanged(std::function<void(const RepoStatus&)> fn) { m_onStatus = std::move(fn); }

private:
	enum class Kind : std::uint8_t {
		Open, Status, Init, CommitAll, Push, Pull, Fetch, SetRemote, SetupGitHub,
		StoreCredential, RestoreTo, CreateBranch, Clone, LfsPull,
		Stage, Unstage, Discard, CommitStaged, ResolveConflict, SwitchBranch, StashPop,
		CommitFiles, Quit
	};

	struct Command
	{
		Kind                  kind = Kind::Status;
		std::filesystem::path path;
		std::string           text;   // commit message / remote url / repo name / host
		std::string           user;   // credential username (StoreCredential)
		std::string           secret; // PAT — wiped after use
		bool                  flag = false;
		bool                  flag2 = false;
		std::vector<std::string> paths;   // Stage / Unstage / Discard
	};

	struct Event
	{
		bool        statusValid = false;
		std::string info;                    // completed-operation feedback
		std::string remoteUrl;
		bool        remoteUrlValid = false;
		std::vector<GitCli::CommitInfo> commits;
		std::vector<std::string>        branches;
		std::vector<std::string>        remoteBranches;
		std::vector<std::string>        stashes;
		std::string                     commitFilesFor;   // set when commitFiles is the answer
		std::vector<GitCli::ChangedFile> commitFiles;
		RepoStatus  status;
		std::string error;
		std::filesystem::path clonedRoot;    // applied even alongside an error
	};

	void workerMain();
	void push(Command c);
	void startWorker();
	// Worker-side: deliver a phase update before the command has finished.
	void post(Event ev);

	std::thread             m_worker;
	std::mutex              m_inMutex;
	std::condition_variable m_inCv;
	std::deque<Command>     m_in;

	std::mutex         m_outMutex;
	std::deque<Event>  m_out;

	std::atomic<bool> m_quit{false};
	std::atomic<bool> m_busy{false};
	// Set when a refresh is already queued, so a burst of requests collapses.
	std::atomic<bool> m_statusPending{false};

	// Worker-only. Not shared: the main thread reads m_status instead.
	std::filesystem::path m_root;

	// Main-thread-only.
	RepoStatus                             m_status;
	std::string                            m_lastError;
	std::string                            m_lastInfo;
	std::string                            m_remoteUrl;
	std::filesystem::path                  m_lastClonedRoot;
	std::vector<GitCli::CommitInfo>        m_commits;
	std::vector<std::string>               m_branches;
	std::vector<std::string>               m_remoteBranches;
	std::vector<std::string>               m_stashes;
	std::map<std::string, std::vector<GitCli::ChangedFile>> m_commitFiles;
	std::uint64_t                          m_generation = 0;
	std::function<void(const RepoStatus&)> m_onStatus;
};

} // namespace HE::Sc
