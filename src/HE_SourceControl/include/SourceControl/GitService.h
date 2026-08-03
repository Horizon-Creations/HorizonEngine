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

#include "SourceControl/RepoStatus.h"
#include "SourceControl/ScCommon.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

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
	enum class Kind : std::uint8_t { Open, Status, Quit };

	struct Command
	{
		Kind                  kind = Kind::Status;
		std::filesystem::path path;
	};

	struct Event
	{
		bool        statusValid = false;
		RepoStatus  status;
		std::string error;
	};

	void workerMain();
	void push(Command c);

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
	std::uint64_t                          m_generation = 0;
	std::function<void(const RepoStatus&)> m_onStatus;
};

} // namespace HE::Sc
