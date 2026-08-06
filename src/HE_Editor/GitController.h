#pragma once

// ─── Editor ↔ HorizonSourceControl bridge ────────────────────────────────────
// Owns the GitService worker and decides when it runs, so neither
// EditorApplication nor the panel has to think about threading or cadence.
//
// Poll-driven like CollabController: update() must be called once per frame or
// nothing happens. That call is cheap — it drains finished work and decides
// whether enough time has passed to ask git anything.

#include <SourceControl/GitService.h>
#include <SourceControl/RepoStatus.h>

#include <cstdint>
#include <filesystem>
#include <string>

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

	HE::Sc::GitService    m_service;
	std::filesystem::path m_projectRoot;

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
