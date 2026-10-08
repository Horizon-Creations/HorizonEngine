#pragma once

// ─── Editor ↔ HorizonSourceControl bridge ────────────────────────────────────
// Owns the GitService worker and decides when it runs, so neither
// EditorApplication nor the panel has to think about threading or cadence.
//
// Poll-driven like CollabController: update() must be called once per frame or
// nothing happens. That call is cheap — it drains finished work and decides
// whether enough time has passed to ask git anything.

#include <SourceControl/GitHubApi.h>
#include <SourceControl/GitService.h>
#include <SourceControl/RepoStatus.h>

#include "EditorRewards.h"   // SyncWatch (ImGui-free)

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class CollabController;

class GitController
{
public:
	GitController();
	~GitController();

	GitController(const GitController&)            = delete;
	GitController& operator=(const GitController&) = delete;

	// Point at a project. Repository discovery happens on the worker, so this
	// returns immediately and isRepo() stays false until the first answer lands.
	void openProject(const std::filesystem::path& projectRoot);
	void closeProject();

	// Once per frame.
	void update(std::uint64_t nowMs);

	// The panel tells the controller whether it is on screen. A hidden panel is
	// still worth refreshing — the Content Browser badges use the same data — but
	// far less often.
	void setPanelVisible(bool visible) { m_panelVisible = visible; }

	void requestRefresh();

	// ── Mutating operations ──────────────────────────────────────────────────
	// Every one of these is a no-op for a guest in a collaboration session —
	// mayModify() is enforced HERE, once, so no caller can forget it. The panel
	// additionally hides the buttons and explains why.
	void requestInit(bool lfsAvailable);
	void requestCommitAll(const std::string& message);
	// An empty token uses the GitHub sign-in (see GitService::requestSetupGitHub);
	// the editor always passes an empty one, it has no token field any more.
	void requestSetupGitHub(const std::string& repoName, bool isPrivate, std::string token);
	// Put the project folder back to how a commit had it, recorded as a new
	// commit so the restore is itself undoable. Refused on a dirty tree.
	void requestRestoreTo(const std::string& commit, const std::string& shortOid);
	// Create a branch at `startCommit` (empty = HEAD), optionally switching to it.
	void requestCreateBranch(const std::string& name, const std::string& startCommit,
	                         bool checkout);
	// Store an access token for an EXISTING remote (pasted URL, clone, expired
	// token) — no repository is created. Goes to git's credential helper, same
	// as the GitHub setup flow.
	void requestStoreCredential(const std::string& host, const std::string& username,
	                            std::string token);
	// Auto-push toggle: the panel binds a checkbox to this; requestCommitAll
	// reads it. Persisted by the panel, not here.
	bool autoPushAfterCommit = false;
	void requestPush();
	void requestPull();
	void requestSetRemote(const std::string& url);

	// ── Background fetch ─────────────────────────────────────────────────────
	// Update the remote-tracking refs on a timer so "3 behind" is a fact rather
	// than whatever was true when the project was opened. Nothing on disk moves,
	// which is what separates this from a pull and makes it safe to automate —
	// and safe for a collaboration guest, who may not pull at all.
	//
	// Persisted by the Preferences page, not here, exactly like
	// autoPushAfterCommit: the controller is where the schedule lives, the panel
	// is where the user's choice does.
	bool autoFetch        = false;
	int  autoFetchMinutes = 15;
	// Floor applied to autoFetchMinutes whatever the config says. A network call
	// on a timer is worth being conservative about.
	static constexpr int kMinFetchMinutes = 5;

	// Fetch now, on the user's say-so. Announces itself in lastInfo(), unlike
	// the timer, and restarts the automatic clock so the two do not stack up.
	void requestFetch();

	const std::string& lastInfo()  const { return m_service.lastInfo(); }
	const std::string& remoteUrl() const { return m_service.remoteUrl(); }

	// Reward moment (EditorRewards.h): COMMITTED. A commit or push the user
	// asked for went through: kSyncCommit | kSyncPush, handed out once (the
	// next call returns 0), or 0. EditorApplication fires it after update().
	int takeSyncMoment()
	{
		const int f = m_syncMoment;
		m_syncMoment = 0;
		return f;
	}

	// ── Clone an existing repository as a new project ────────────────────────
	// Runs on a GitService of its own, not the project's: GitService::requestClone
	// re-targets the service at the new working tree, and doing that to m_service
	// would swap the OPEN project's status for the clone's while the user is
	// still looking at it — and leave it swapped if the clone then fails or is
	// never opened. Opening the clone afterwards goes through openProject() like
	// any other project, which points m_service at it the normal way.
	//
	// Not gated on mayModify(): a clone writes into a new folder and moves
	// nothing in the open project, so a collaboration guest breaks no session by
	// making one. Opening it is what ends the session, and that goes through the
	// editor's guarded open path, which asks.

	// GET /user/repos for the token's owner, off the frame thread (one blocking
	// HTTPS round trip per page of 100). The token is wiped once the list is in.
	// A request while one is running is dropped.
	void requestListRepos(std::string token);
	// The same with the GitHub sign-in instead of a typed token: read from the
	// credential helper at `credentialRoot` on the list thread
	// (GitHubTokenStore::load). Not signed in lands in repoListError().
	void requestListReposWithSignIn(const std::filesystem::path& credentialRoot);
	bool listingRepos() const { return m_listing; }
	// True once a list request has answered, successfully or not.
	bool repoListLoaded() const { return m_repoListLoaded; }
	const std::vector<HE::Sc::RepoListEntry>& repoList() const { return m_repoList; }
	const std::string& repoListError() const { return m_repoListError; }

	// Clone `cloneUrl` into `targetDir` (absent or empty). `token` goes to the
	// credential helper, never into the URL — see GitService::requestClone.
	void requestClone(const std::string& cloneUrl, const std::filesystem::path& targetDir,
	                  std::string token);
	// Retry the LFS download of the last clone (git-lfs was missing, the
	// connection dropped).
	void requestCloneLfsPull();
	// Stop the clone worker once its result has been used (the project opened,
	// or the dialog closed). Harmless when none is running.
	void finishClone();
	// Not GitService::busy() directly: see m_cloneBusy.
	bool cloneBusy() const { return m_cloneBusy; }
	const std::string& cloneInfo()  const { return m_cloneService.lastInfo(); }
	const std::string& cloneError() const { return m_cloneService.lastError(); }
	// Set once the git part of the clone succeeded — also when LFS then failed.
	const std::filesystem::path& clonedRoot() const { return m_cloneService.lastClonedRoot(); }

	// ── State for the UI ─────────────────────────────────────────────────────
	bool                     isRepo() const { return m_service.isRepo(); }
	const HE::Sc::RepoStatus& status() const { return m_service.status(); }
	bool                     busy()   const { return m_service.busy(); }
	const std::string&       lastError() const { return m_service.lastError(); }
	const std::vector<HE::Sc::GitCli::CommitInfo>& recentCommits() const
	{
		return m_service.recentCommits();
	}
	const std::vector<std::string>& branches() const { return m_service.branches(); }
	const std::filesystem::path& projectRoot() const { return m_projectRoot; }

	// Status for a file given its ABSOLUTE path, which is what the Content
	// Browser has. Returns nullptr when the path is outside the repository or
	// simply unmodified. Cheap enough to call per visible tile: a string
	// relativisation plus one hash lookup.
	const HE::Sc::FileEntry* entryForAbsolutePath(const std::string& absolutePath) const;
	// Whether any file below this absolute directory path has changes.
	bool folderHasChanges(const std::string& absoluteFolderPath) const;

	// ── Who may write ────────────────────────────────────────────────────────
	// Inside a collaboration session only the host may sync: everyone else's
	// editor is a client of the host's world, so a client pulling a different
	// revision mid-session would diverge from the scene state the host is
	// authoritative for, and a commit from a client would record other people's
	// live edits as its own.
	//
	// Reading status is always allowed — it is only writing and moving HEAD that
	// would desync the session. Nothing mutating exists yet (that is a later
	// checkpoint); the predicate is here so the panel can explain itself from the
	// start rather than gaining a restriction later.
	void setCollab(const CollabController* collab) { m_collab = collab; }
	bool mayModify() const;
	// True when the only reason mayModify() is false is that we are a client in a
	// session — which the panel says out loud instead of just hiding buttons.
	bool blockedByCollabSession() const;

private:
	// Absolute → repository-relative with forward slashes, or empty when outside.
	std::string toRepoRelative(const std::string& absolutePath) const;

	// Main thread: move a finished repository list over from the worker.
	void collectRepoList();
	void startListRepos(std::string token, std::filesystem::path credentialRoot);

	HE::Sc::GitService    m_service;
	HE::Sc::GitService    m_cloneService;
	// The worker queues its last event and only THEN reports idle, so a
	// busy() read after this frame's pump can say "done" while the result is
	// still waiting for the next one — a finished clone with no root and no
	// error. update() therefore reads busy() BEFORE pumping and clears this
	// only then: idle before the pump means the pump got everything.
	bool                  m_cloneBusy = false;
	std::filesystem::path m_projectRoot;
	// The user's commit/push, judged by the same "idle before the pump" rule.
	HE::Ed::Rewards::SyncWatch m_syncWatch;
	int                        m_syncMoment = 0;

	// Repository list. The thread writes only the m_listResult* members, under
	// m_listMutex; everything else is main-thread-only.
	std::thread                         m_listThread;
	std::mutex                          m_listMutex;
	bool                                m_listDone = false;
	std::vector<HE::Sc::RepoListEntry>  m_listResult;
	std::string                         m_listResultError;
	bool                                m_listing        = false;
	bool                                m_repoListLoaded = false;
	std::vector<HE::Sc::RepoListEntry>  m_repoList;
	std::string                         m_repoListError;

	const CollabController* m_collab = nullptr;
	// Project-path spelling of the repo root when the two are the same directory
	// through a symlink; empty otherwise. See toRepoRelative.
	std::string   m_rootAlias;
	std::uint64_t m_aliasGeneration = 0;

	bool          m_panelVisible = false;
	std::uint64_t m_lastPollMs   = 0;
	// 0 = the fetch clock has not started. update() stamps it on the first frame
	// with a repo rather than firing straight away — opening a project should not
	// reach out to the network before the user has done anything.
	std::uint64_t m_lastFetchMs  = 0;
};
