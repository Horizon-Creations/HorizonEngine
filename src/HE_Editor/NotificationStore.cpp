#include "NotificationStore.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace HE::Ed
{

std::uint64_t NotificationStore::nowMs()
{
	// steady_clock, not ImGui::GetTime(): producers run on worker threads (the
	// reference scan, the retarget worker, the SFTP sync), and ImGui's clock is
	// only valid on the frame thread.
	using namespace std::chrono;
	return static_cast<std::uint64_t>(
		duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void NotificationStore::post(NoteLevel level, std::string text, std::string detail,
                             std::string assetPath)
{
	if (text.empty()) return;

	std::lock_guard<std::mutex> lock(m_mutex);

	// Collapse a repeat of the row that is already at the end. A worker that
	// fails the same way on four hundred files says so once, with a count —
	// four hundred rows is not a notification, it is a denial of service against
	// the person reading it.
	if (!m_entries.empty())
	{
		Notification& last = m_entries.back();
		if (last.level == level && last.text == text)
		{
			++last.count;
			last.whenMs = nowMs();
			if (!last.seen) return;      // already counted as unread
			last.seen = false;           // it happened again: worth seeing again
			++m_unseen;
			return;
		}
	}

	Notification n;
	n.level     = level;
	n.text      = std::move(text);
	n.detail    = std::move(detail);
	n.assetPath = std::move(assetPath);
	n.whenMs    = nowMs();
	m_entries.push_back(std::move(n));
	++m_unseen;

	if (m_entries.size() > k_maxEntries)
	{
		// Drop the oldest SEEN entry rather than simply the oldest: an unread
		// problem must not be pushed out by a run of routine chatter behind it.
		auto victim = std::find_if(m_entries.begin(), m_entries.end(),
			[](const Notification& e){ return e.seen; });
		if (victim == m_entries.end()) victim = m_entries.begin();
		if (!victim->seen && m_unseen > 0) --m_unseen;
		m_entries.erase(victim);
	}
}

std::vector<Notification> NotificationStore::snapshot() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_entries;
}

std::size_t NotificationStore::unseenCount() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_unseen;
}

void NotificationStore::markAllSeen()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	for (Notification& e : m_entries) e.seen = true;
	m_unseen = 0;
}

void NotificationStore::clear()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_entries.clear();
	m_unseen = 0;
}

// ─── The engine-wide error channel ───────────────────────────────────────────

void NotificationStore::attachToEngineLog()
{
	std::lock_guard<std::mutex> lock(m_bridgeMutex);
	if (m_logSink != 0) return;   // already attached; twice would post twice
	m_logSink = HE::Log::addSink(&NotificationStore::logSink, this);
}

void NotificationStore::detachFromEngineLog()
{
	std::lock_guard<std::mutex> lock(m_bridgeMutex);
	if (m_logSink == 0) return;
	HE::Log::removeSink(m_logSink);
	m_logSink = 0;
}

void NotificationStore::logSink(const HE::Log::Record& record, void* user)
{
	// Error and Critical only. Warning is where "it did not fully work, nothing
	// is lost" lives, and a session produces hundreds of those legitimately —
	// routing them here would bury the handful of entries this surface exists
	// for. Compared rather than matched against Error alone so Critical, which is
	// strictly worse, cannot be the one severity that goes unreported.
	if (record.level < HE::Log::Level::Error) return;
	if (!user) return;
	static_cast<NotificationStore*>(user)->onLogError(record);
}

void NotificationStore::onLogError(const HE::Log::Record& record)
{
	const char* message = record.message ? record.message : "";
	if (*message == '\0') return;

	const std::uint64_t now = nowMs();
	std::string         text(message);

	{
		std::lock_guard<std::mutex> lock(m_bridgeMutex);

		// Same message again while it is still on screen: dropped, not counted.
		// post()'s own collapse only merges the row at the END of the list, so two
		// errors alternating every frame would otherwise interleave into an
		// unbounded pair of growing lists. Dropping is also the honest answer —
		// "this failed 4000 times" is a log fact, not something to act on.
		auto hit = std::find_if(m_recentErrors.begin(), m_recentErrors.end(),
			[&text](const auto& e){ return e.first == text; });
		if (hit != m_recentErrors.end())
		{
			if (now - hit->second < k_errorCooldownMs) return;
			hit->second = now;
		}
		else
		{
			if (m_recentErrors.size() >= k_recentErrorsMax)
			{
				// Evict the least recently seen — the one whose cooldown is most
				// likely to have lapsed anyway.
				auto oldest = std::min_element(m_recentErrors.begin(), m_recentErrors.end(),
					[](const auto& a, const auto& b){ return a.second < b.second; });
				m_recentErrors.erase(oldest);
			}
			m_recentErrors.emplace_back(text, now);
		}

		// A ceiling on how much of this surface one bad moment may take. Ten
		// DISTINCT errors already say "something is badly wrong"; the eleventh
		// only pushes the first one out of view.
		if (now - m_burstWindowMs >= k_burstWindowMs)
		{
			m_burstWindowMs = now;
			m_burstCount    = 0;
		}
		if (++m_burstCount > k_burstMax) return;
	}

	// Where it came from, in the log's own vocabulary, so the line can be found
	// again in HorizonEngine.log. The category alone is not enough for that; the
	// file and line are what make it a starting point rather than a shrug.
	std::string detail = HE::Log::categoryName(record.category);
	if (record.file && record.file[0] != '\0')
	{
		// Filename only: the absolute build path of a machine that is not the
		// user's says nothing to them and eats the whole line.
		const char* slash = std::strrchr(record.file, '/');
#ifdef _WIN32
		const char* back  = std::strrchr(record.file, '\\');
		if (back && (!slash || back > slash)) slash = back;
#endif
		char buf[160];
		std::snprintf(buf, sizeof(buf), " — %s:%d", slash ? slash + 1 : record.file,
		              record.line);
		detail += buf;
	}

	// m_bridgeMutex is NOT held here: post() takes m_mutex, and the two are only
	// ever taken in this order, never nested.
	post(NoteLevel::Problem, std::move(text), std::move(detail));
}

// ─── Posting from anywhere ───────────────────────────────────────────────────

namespace
{
	// Atomic rather than a plain pointer: worker threads read it while the main
	// thread installs and (at shutdown) clears it. Never dereferenced after
	// EditorApplication clears it, which is the whole reason it is set to null
	// before the store dies rather than simply left dangling.
	std::atomic<NotificationStore*> s_globalStore{nullptr};
}

void setGlobalNotifications(NotificationStore* store)
{
	s_globalStore.store(store, std::memory_order_release);
}

void notify(NoteLevel level, std::string text, std::string detail,
            std::string assetPath)
{
	NotificationStore* store = s_globalStore.load(std::memory_order_acquire);
	if (!store) return;   // headless, tests, the runtime game: nowhere to show it
	store->post(level, std::move(text), std::move(detail), std::move(assetPath));
}

} // namespace HE::Ed
