#include "SourceControl/GitService.h"

#include "SourceControl/GitCli.h"
#include "ScLog.h"

#include <utility>

namespace HE::Sc {

GitService::GitService() = default;

GitService::~GitService() { close(); }

void GitService::push(Command c)
{
	{
		std::lock_guard<std::mutex> lock(m_inMutex);
		m_in.push_back(std::move(c));
	}
	m_busy.store(true, std::memory_order_release);
	m_inCv.notify_one();
}

void GitService::open(const std::filesystem::path& anyPathInside)
{
	close();

	m_quit.store(false, std::memory_order_release);
	m_status    = RepoStatus{};
	m_lastError.clear();

	m_worker = std::thread([this] { workerMain(); });
	push(Command{ Kind::Open, anyPathInside });
}

void GitService::close()
{
	if (!m_worker.joinable()) return;

	m_quit.store(true, std::memory_order_release);
	push(Command{ Kind::Quit, {} });
	// Bounded by the timeout on whichever command is already running — every
	// GitCli call carries one, so this cannot wait forever. Once mutating
	// commands arrive (push/pull, which legitimately run for minutes), this needs
	// a real kill handle on the child rather than a timeout; HE::Proc already
	// terminates the whole process group, so that is a matter of exposing the
	// handle, not of changing how the child dies.
	m_worker.join();

	m_busy.store(false, std::memory_order_release);
	m_statusPending.store(false, std::memory_order_release);
	{
		std::lock_guard<std::mutex> lock(m_outMutex);
		m_out.clear();
	}
	{
		std::lock_guard<std::mutex> lock(m_inMutex);
		m_in.clear();
	}
	m_status = RepoStatus{};
}

void GitService::requestStatus()
{
	if (!m_worker.joinable()) return;
	// Collapse a burst. Status is a whole-tree snapshot, so four queued refreshes
	// would produce the same answer four times and delay the one that matters.
	if (m_statusPending.exchange(true, std::memory_order_acq_rel)) return;
	push(Command{ Kind::Status, {} });
}

void GitService::workerMain()
{
	for (;;)
	{
		Command cmd;
		{
			std::unique_lock<std::mutex> lock(m_inMutex);
			m_inCv.wait(lock, [this] {
				return !m_in.empty() || m_quit.load(std::memory_order_acquire);
			});
			if (m_in.empty())
			{
				if (m_quit.load(std::memory_order_acquire)) return;
				continue;
			}
			cmd = std::move(m_in.front());
			m_in.pop_front();
		}

		if (cmd.kind == Kind::Quit) return;

		Event ev;
		switch (cmd.kind)
		{
		case Kind::Open:
		{
			m_root = GitCli::findRepoRoot(cmd.path);
			if (m_root.empty())
			{
				// Not an error — most projects are simply not in a repository yet,
				// and the UI offers to create one.
				HE_SC_INFO("No git repository found at or above %s", cmd.path.string().c_str());
				ev.statusValid = true;   // a valid answer: "there is no repo"
				break;
			}
			HE_SC_INFO("Repository: %s", m_root.string().c_str());
			// Fall through to an immediate first status, so opening a project
			// shows its state without a second round trip.
			[[fallthrough]];
		}
		case Kind::Status:
		{
			m_statusPending.store(false, std::memory_order_release);
			if (m_root.empty()) { ev.statusValid = true; break; }

			std::string err;
			RepoStatus  s;
			if (GitCli::status(m_root, s, &err))
			{
				ev.statusValid = true;
				ev.status      = std::move(s);
			}
			else
			{
				ev.error = err;
			}
			break;
		}
		case Kind::Quit:
			return;
		}

		{
			std::lock_guard<std::mutex> lock(m_outMutex);
			m_out.push_back(std::move(ev));
		}

		// Only idle once the queue is genuinely empty, or a burst of commands
		// would flicker the UI's busy indicator between each one.
		std::lock_guard<std::mutex> lock(m_inMutex);
		if (m_in.empty()) m_busy.store(false, std::memory_order_release);
	}
}

void GitService::pump(std::size_t maxEvents)
{
	for (std::size_t handled = 0; handled < maxEvents; ++handled)
	{
		Event ev;
		{
			std::lock_guard<std::mutex> lock(m_outMutex);
			if (m_out.empty()) return;
			ev = std::move(m_out.front());
			m_out.pop_front();
		}

		if (!ev.error.empty())
		{
			m_lastError = ev.error;
			HE_SC_WARN("Status refresh failed: %s", ev.error.c_str());
			continue;
		}

		m_lastError.clear();
		// Swapped in whole. A partially updated snapshot is the bug class this
		// design exists to remove.
		ev.status.generation = ++m_generation;
		m_status = std::move(ev.status);

		if (m_onStatus) m_onStatus(m_status);
	}
}

} // namespace HE::Sc
