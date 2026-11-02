#include "EditorSelfUpdate.h"

#include <SelfUpdate/SelfUpdateEndpoint.h>
#include <SelfUpdate/SelfUpdateTransport.h>

#include <Diagnostics/Log.h>

#include <utility>

namespace EditorSelfUpdate
{
namespace Su = HE::Su;

std::vector<std::string> relaunchArgsFor(bool projectLoaded, const std::string& projectPath)
{
	if (!projectLoaded || projectPath.empty()) return {};
	return { projectPath };
}

const char* phaseName(Phase p)
{
	switch (p)
	{
	case Phase::Idle:          return "idle";
	case Phase::Checking:      return "checking";
	case Phase::NotConfigured: return "not-configured";
	case Phase::UpToDate:      return "up-to-date";
	case Phase::RemoteOlder:   return "remote-older";
	case Phase::NoBuild:       return "no-build-for-platform";
	case Phase::Available:     return "available";
	case Phase::Downloading:   return "downloading";
	case Phase::Ready:         return "ready";
	case Phase::Installing:    return "installing";
	case Phase::Installed:     return "installed";
	case Phase::Failed:        return "failed";
	}
	return "?";
}

Backend productBackend()
{
	Backend b;
	b.stagingRoot = Su::defaultStagingDir();
	b.check = [] {
		return Su::checkForUpdate(Su::loadSelfUpdateConfig());
	};
	b.download = [root = b.stagingRoot](const Su::CheckResult& check, const Su::SuProgressFn& progress) {
		// The account is read again rather than carried over from the check: the
		// result holds no endpoint (and so no password) on purpose.
		const Su::SelfUpdateConfig config = Su::loadSelfUpdateConfig();
		if (!config.ready())
		{
			Su::DownloadResult r;
			r.error = config.problem.empty() ? "self-update is not configured" : config.problem;
			return r;
		}
		const std::unique_ptr<Su::IUpdateTransport> transport = Su::makeSftpTransport(config.endpoint);
		return Su::downloadUpdate(*transport, check, root, progress);
	};
	b.preflight = [](const Su::TakeoverOptions& o) { return Su::preflightTakeover(o); };
	b.begin     = [](const Su::TakeoverOptions& o) { return Su::beginTakeover(o); };
	return b;
}

Session::Session(Backend backend) : m_backend(std::move(backend))
{
	m_state.localVersion = Su::runningBuild().version;
}

Session::~Session() { join(); }

void Session::join()
{
	if (m_worker.joinable()) m_worker.join();
}

Snapshot Session::snapshot() const
{
	std::lock_guard<std::mutex> lk(m_mutex);
	return m_state;
}

void Session::set(const std::function<void(Snapshot&)>& edit)
{
	std::lock_guard<std::mutex> lk(m_mutex);
	edit(m_state);
}

void Session::runWorker(std::function<void()> body)
{
	// The previous worker has finished (m_running is false), but a finished
	// std::thread is still joinable until someone joins it.
	join();
	m_running = true;
	m_worker = std::thread([this, body = std::move(body)] {
		body();
		m_running = false;
	});
}

bool Session::startCheck()
{
	if (m_running) return false;
	set([](Snapshot& s) {
		s.phase = Phase::Checking;
		s.message.clear();
		s.installBlocker.clear();
		s.done = s.total = 0;
	});
	runWorker([this] {
		Su::CheckResult r = m_backend.check ? m_backend.check() : Su::CheckResult{};
		Phase phase = Phase::Failed;
		switch (r.status)
		{
		case Su::CheckStatus::NotConfigured:      phase = Phase::NotConfigured; break;
		case Su::CheckStatus::UpToDate:           phase = Phase::UpToDate;      break;
		case Su::CheckStatus::RemoteOlder:        phase = Phase::RemoteOlder;   break;
		case Su::CheckStatus::NoBuildForPlatform: phase = Phase::NoBuild;       break;
		case Su::CheckStatus::UpdateAvailable:    phase = Phase::Available;     break;
		case Su::CheckStatus::Failed:             phase = Phase::Failed;        break;
		}
		HE_LOG_INFO(SelfUpdate, "Editor: update check -> %s%s%s", phaseName(phase),
		            r.remoteVersion.empty() ? "" : " (server: ", r.remoteVersion.empty() ? "" : (r.remoteVersion + ")").c_str());
		std::lock_guard<std::mutex> lk(m_mutex);
		m_state.phase         = phase;
		if (!r.localVersion.empty()) m_state.localVersion = r.localVersion;
		m_state.remoteVersion = r.remoteVersion;
		m_state.notes         = r.manifest.notes;
		m_state.message       = r.error;
		m_check               = std::move(r);
	});
	return true;
}

bool Session::startDownload()
{
	if (m_running) return false;
	Su::CheckResult check;
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		if (m_state.phase != Phase::Available) return false;
		check = m_check;
		m_state.phase = Phase::Downloading;
		m_state.message.clear();
		m_state.done  = 0;
		m_state.total = check.artifact.size;
	}
	runWorker([this, check = std::move(check)] {
		const Su::SuProgressFn progress = [this](std::uint64_t done, std::uint64_t total) {
			std::lock_guard<std::mutex> lk(m_mutex);
			m_state.done = done;
			if (total) m_state.total = total;
		};
		const Su::DownloadResult d = m_backend.download
			? m_backend.download(check, progress) : Su::DownloadResult{};
		if (!d.ok)
		{
			HE_LOG_WARN(SelfUpdate, "Editor: download failed: %s", d.error.c_str());
			set([&](Snapshot& s) { s.phase = Phase::Failed; s.message = d.error; });
			return;
		}
		// Asked now, not when the button is pressed: "Restart to Update" is only
		// worth offering when the takeover can be started at all (the install
		// directory writable, HorizonUpdater shipped beside the editor).
		Su::TakeoverOptions o;
		o.stagingRoot = m_backend.stagingRoot;
		const Su::TakeoverCheck pre = m_backend.preflight ? m_backend.preflight(o) : Su::TakeoverCheck{};
		HE_LOG_INFO(SelfUpdate, "Editor: %s staged%s%s", d.staged.version.c_str(),
		            pre.ok ? "" : ", but cannot be installed from here: ", pre.ok ? "" : pre.error.c_str());
		set([&](Snapshot& s) {
			s.phase          = Phase::Ready;
			s.done           = s.total;
			s.installBlocker = pre.ok ? std::string{} : pre.error;
		});
	});
	return true;
}

bool Session::install(const std::vector<std::string>& relaunchArgs)
{
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		if (m_running || !m_state.canInstall()) return false;
		m_state.phase = Phase::Installing;
	}
	Su::TakeoverOptions o;
	o.stagingRoot  = m_backend.stagingRoot;
	o.relaunchArgs = relaunchArgs;
	// On the worker: beginTakeover re-hashes the staged build and unpacks it
	// (tar / hdiutil, waited on for up to ten minutes) before it starts
	// HorizonUpdater. On the frame that froze the editor for the whole time.
	runWorker([this, o = std::move(o)] {
		Su::TakeoverResult r = m_backend.begin ? m_backend.begin(o) : Su::TakeoverResult{};
		if (r.ok)
		{
			HE_LOG_INFO(SelfUpdate, "Editor: HorizonUpdater started (relaunch with %zu argument%s) -- quitting",
			            o.relaunchArgs.size(), o.relaunchArgs.size() == 1 ? "" : "s");
			set([](Snapshot& s) { s.phase = Phase::Installed; });
			return;
		}
		if (r.error.empty()) r.error = "the takeover could not be started";
		HE_LOG_WARN(SelfUpdate, "Editor: update not started, the editor keeps running: %s", r.error.c_str());
		set([&](Snapshot& s) { s.phase = Phase::Ready; s.installBlocker = r.error; });
	});
	return true;
}
}
