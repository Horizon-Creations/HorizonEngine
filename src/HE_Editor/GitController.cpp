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
	// interval, so opening a project shows its state immediately.
	m_lastPollMs = 0;
}

void GitController::closeProject()
{
	m_service.close();
	m_projectRoot.clear();
	m_lastPollMs = 0;
}

void GitController::requestRefresh()
{
	m_service.requestStatus();
}

void GitController::update(std::uint64_t nowMs)
{
	// Drain first, unconditionally. The result of a discovery is what makes
	// isRepo() true, so gating this on isRepo() would mean the answer that
	// creates the state is never collected — the same ordering trap
	// CollabController::pumpDirectory documents.
	m_service.pump();

	if (m_projectRoot.empty()) return;

	// Never overlap requests: a poll while one is running would just queue work
	// behind it, and git serialises on index.lock anyway.
	if (m_service.busy()) return;

	const std::uint64_t interval = m_panelVisible ? kPollVisibleMs : kPollHiddenMs;
	if (m_lastPollMs != 0 && nowMs - m_lastPollMs < interval) return;

	m_lastPollMs = nowMs;
	m_service.requestStatus();
}

std::string GitController::toRepoRelative(const std::string& absolutePath) const
{
	const HE::Sc::RepoStatus& s = m_service.status();
	if (!s.isRepo || s.root.empty() || absolutePath.empty()) return {};

	const std::string abs  = normaliseSlashes(absolutePath);
	const std::string root = normaliseSlashes(s.root);

	// Plain prefix comparison rather than std::filesystem::relative: this runs
	// per visible tile, and relative() touches the filesystem.
	if (abs.size() <= root.size() || abs.compare(0, root.size(), root) != 0) return {};
	std::size_t start = root.size();
	if (abs[start] != '/') return {};   // "/repo-other" must not match "/repo"
	return abs.substr(start + 1);
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
