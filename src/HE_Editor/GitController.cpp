#include "GitController.h"
#include "CollabController.h"

#include <Diagnostics/Log.h>

#include <algorithm>

namespace {

// How often git is asked. Deliberately slow: status walks the whole working
// tree, which for a project full of assets is not free, and nothing about the
// answer changes between frames. Never per frame.
constexpr std::uint64_t kPollVisibleMs = 3000;
constexpr std::uint64_t kPollHiddenMs  = 15000;

std::string normaliseSlashes(std::string s)
{
	std::replace(s.begin(), s.end(), '\\', '/');
	return s;
}

} // namespace

GitController::GitController()  = default;
GitController::~GitController() = default;

void GitController::openProject(const std::filesystem::path& projectRoot)
{
	closeProject();
	if (projectRoot.empty()) return;

	m_projectRoot = projectRoot;
	m_service.open(projectRoot);
	// Force the first poll to happen on the next update rather than after a full
	// interval, so opening a project shows its state immediately. The FETCH clock
	// restarts too, so switching projects does not inherit the previous one's
	// timer and hit the network on the first frame.
	m_lastPollMs  = 0;
	m_lastFetchMs = 0;
}

void GitController::closeProject()
{
	m_service.close();
	m_projectRoot.clear();
	m_lastPollMs  = 0;
	m_lastFetchMs = 0;
}

void GitController::requestRefresh()
{
	m_service.requestStatus();
}

void GitController::requestInit(bool lfsAvailable)
{
	if (!mayModify() || m_projectRoot.empty()) return;
	m_service.requestInit(m_projectRoot, lfsAvailable);
}

void GitController::requestCommitAll(const std::string& message)
{
	if (!mayModify() || message.empty()) return;
	m_service.requestCommitAll(message, autoPushAfterCommit);
}

void GitController::requestRestoreTo(const std::string& commit, const std::string& shortOid)
{
	if (!mayModify() || commit.empty()) return;
	m_service.requestRestoreTo(commit, shortOid);
}

void GitController::requestCreateBranch(const std::string& name,
                                       const std::string& startCommit, bool checkout)
{
	if (!mayModify() || name.empty()) return;
	m_service.requestCreateBranch(name, startCommit, checkout);
}

void GitController::requestSetupGitHub(const std::string& repoName, bool isPrivate,
                                       std::string token)
{
	if (!mayModify() || repoName.empty() || token.empty()) return;
	m_service.requestSetupGitHub(repoName, isPrivate, std::move(token));
}

void GitController::requestStoreCredential(const std::string& host,
                                           const std::string& username,
                                           std::string token)
{
	if (!mayModify() || host.empty() || username.empty() || token.empty()) return;
	m_service.requestStoreCredential(host, username, std::move(token));
}

void GitController::requestPush()
{
	if (!mayModify()) return;
	m_service.requestPush(!m_service.status().upstream.empty());
}

void GitController::requestPull()
{
	if (!mayModify()) return;
	m_service.requestPull();
}

void GitController::requestSetRemote(const std::string& url)
{
	if (!mayModify() || url.empty()) return;
	m_service.requestSetRemote(url);
}

void GitController::update(std::uint64_t nowMs)
{
	// Drain first, unconditionally. The result of a discovery is what makes
	// isRepo() true, so gating this on isRepo() would mean the answer that
	// creates the state is never collected — the same ordering trap
	// CollabController::pumpDirectory documents.
	m_service.pump();

	// One filesystem probe per status refresh, never per tile: when the repo
	// root and the project root are the same directory under two spellings
	// (symlinks), remember the project's spelling for toRepoRelative.
	if (m_service.status().generation != m_aliasGeneration)
	{
		m_aliasGeneration = m_service.status().generation;
		m_rootAlias.clear();
		const std::string& root = m_service.status().root;
		if (!root.empty() && !m_projectRoot.empty())
		{
			const std::string proj = normaliseSlashes(m_projectRoot.string());
			std::error_code ec;
			if (proj != normaliseSlashes(root) &&
			    std::filesystem::equivalent(root, m_projectRoot, ec) && !ec)
			{
				m_rootAlias = proj;
			}
		}
	}

	if (m_projectRoot.empty()) return;

	// Never overlap requests: a poll while one is running would just queue work
	// behind it, and git serialises on index.lock anyway.
	if (m_service.busy()) return;

	// ── Background fetch ─────────────────────────────────────────────────────
	// Off by default and never on the first frame after opening a project: the
	// point is to keep the ahead/behind counters honest over a long session, not
	// to reach out to the network the moment the editor starts.
	//
	// Read-only by construction — git fetch updates the remote-tracking refs and
	// nothing else, so it is safe to run on a timer AND safe for a collaboration
	// guest, who may not push or pull. mayModify() is deliberately not consulted
	// here: nothing on disk moves.
	//
	// Checked before the status poll, since a fetch queues its own refresh and
	// doing both would be one whole-tree walk too many.
	if (autoFetch && autoFetchMinutes > 0 && m_service.isRepo() &&
	    !m_service.remoteUrl().empty())
	{
		// Floored rather than trusted. The preference offers sane values, but it
		// lands in config.json where a typo (or a zero) turns a background task
		// into a request storm against someone's forge.
		const std::uint64_t minutes =
			static_cast<std::uint64_t>(std::max(kMinFetchMinutes, autoFetchMinutes));
		const std::uint64_t interval = minutes * 60u * 1000u;
		if (m_lastFetchMs == 0)
		{
			// Start the clock rather than firing immediately.
			m_lastFetchMs = nowMs;
		}
		else if (nowMs - m_lastFetchMs >= interval)
		{
			m_lastFetchMs = nowMs;
			// Quiet: a fetch nobody asked for has no business writing into the
			// panel's status line. A failure still surfaces through lastError(),
			// because an expired token IS worth knowing about — and the minimum
			// interval keeps a broken remote from being retried more than a
			// handful of times an hour, which is why there is no backoff here.
			m_service.requestFetch(/*quiet=*/true);
			return;
		}
	}

	const std::uint64_t interval = m_panelVisible ? kPollVisibleMs : kPollHiddenMs;
	if (m_lastPollMs != 0 && nowMs - m_lastPollMs < interval) return;

	m_lastPollMs = nowMs;
	m_service.requestStatus();
}

void GitController::requestFetch()
{
	if (m_service.remoteUrl().empty()) return;
	m_lastFetchMs = 0;   // a manual fetch restarts the automatic clock
	m_service.requestFetch(/*quiet=*/false);
}

std::string GitController::toRepoRelative(const std::string& absolutePath) const
{
	const HE::Sc::RepoStatus& s = m_service.status();
	if (!s.isRepo || s.root.empty() || absolutePath.empty()) return {};

	const std::string abs = normaliseSlashes(absolutePath);

	// Plain prefix comparison rather than std::filesystem::relative: this runs
	// per visible tile, and relative() touches the filesystem.
	const auto stripPrefix = [&abs](const std::string& root) -> std::string {
		if (root.empty()) return {};
		if (abs.size() <= root.size() || abs.compare(0, root.size(), root) != 0) return {};
		const std::size_t start = root.size();
		if (abs[start] != '/') return {};   // "/repo-other" must not match "/repo"
		return abs.substr(start + 1);
	};

	// Two spellings of the same directory: git resolves symlinks when it
	// reports the toplevel ("/var/…" comes back "/private/var/…" on macOS),
	// while the Content Browser builds its paths from the project path AS
	// CONFIGURED. A project living behind any symlink then never prefix-matches
	// the git root — every badge silently vanishes. m_rootAlias holds the
	// project-path spelling, but ONLY when it is verified to be the same
	// directory as the repo root (update() checks once per status refresh) —
	// a repo that starts one level above the project must not have its keys
	// rebased onto the project by accident.
	std::string rel = stripPrefix(normaliseSlashes(s.root));
	if (rel.empty() && !m_rootAlias.empty())
		rel = stripPrefix(m_rootAlias);
	return rel;
}

const HE::Sc::FileEntry* GitController::entryForAbsolutePath(const std::string& absolutePath) const
{
	const std::string rel = toRepoRelative(absolutePath);
	if (rel.empty()) return nullptr;
	return m_service.status().find(rel);
}

bool GitController::folderHasChanges(const std::string& absoluteFolderPath) const
{
	const std::string rel = toRepoRelative(absoluteFolderPath);
	if (rel.empty()) return false;
	// Precomputed on the worker, so this is one hash lookup rather than a walk
	// of the folder's subtree.
	return m_service.status().dirtyFolders.count(rel) != 0;
}

bool GitController::blockedByCollabSession() const
{
	return m_collab && m_collab->inSession() && !m_collab->isHost();
}

bool GitController::mayModify() const
{
	return !blockedByCollabSession();
}
