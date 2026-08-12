#include "NotificationStore.h"

#include <algorithm>
#include <chrono>

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

} // namespace HE::Ed
