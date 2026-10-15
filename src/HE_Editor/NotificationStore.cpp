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

namespace
{
	// Puts the popups of a freshly posted (or re-posted) entry on the clock. A
	// tier decides which popup it gets: Info none, Important a toast, Critical a
	// modal — `popup == false` is the caller opting out of all of it.
	void armPopup(Notification& n, bool popup, std::uint32_t timeoutMs, std::uint64_t now)
	{
		if (!popup) return;
		if (n.level == NoteLevel::Critical)
		{
			n.modalPending = true;
		}
		else if (n.level == NoteLevel::Important)
		{
			n.timeoutMs = timeoutMs != 0 ? timeoutMs
			            : n.actions.empty() ? NotificationStore::k_toastMs
			                                : NotificationStore::k_toastActionMs;
			n.toastUntilMs = now + n.timeoutMs;
		}
	}
}

void NotificationStore::settle(Notification& n)
{
	n.toastUntilMs = 0;
	n.modalPending = false;
	n.seen         = true;
}

std::uint64_t NotificationStore::post(NoteLevel level, std::string text, std::string detail,
                                      std::string assetPath)
{
	NoteSpec spec;
	spec.level     = level;
	spec.text      = std::move(text);
	spec.detail    = std::move(detail);
	spec.assetPath = std::move(assetPath);
	return post(std::move(spec));
}

std::uint64_t NotificationStore::post(NoteSpec spec)
{
	if (spec.text.empty()) return 0;

	const std::uint64_t now = nowMs();
	std::lock_guard<std::mutex> lock(m_mutex);

	// Collapse a repeat of the row that is already at the end. A worker that
	// fails the same way on four hundred files says so once, with a count —
	// four hundred rows is not a notification, it is a denial of service against
	// the person reading it. Rows with buttons or a key are never merged: each is
	// an answerable thing in its own right, and a merged one would keep the old
	// callback.
	if (spec.key.empty() && spec.actions.empty() && !m_entries.empty())
	{
		Notification& last = m_entries.back();
		if (last.key.empty() && last.actions.empty() &&
		    last.level == spec.level && last.text == spec.text)
		{
			++last.count;
			last.whenMs = now;
			last.seen   = false;     // it happened again: worth seeing again
			armPopup(last, spec.popup && m_popupsEnabled, spec.timeoutMs, now);
			return last.id;
		}
	}

	// "The" notification for a key: the earlier one is replaced, not joined by a
	// second row that disagrees with it.
	if (!spec.key.empty())
	{
		m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
			[&spec](const Notification& e){ return e.key == spec.key; }), m_entries.end());
	}

	Notification n;
	n.id        = m_nextId++;
	n.level     = spec.level;
	n.title     = std::move(spec.title);
	n.text      = std::move(spec.text);
	n.detail    = std::move(spec.detail);
	n.assetPath = std::move(spec.assetPath);
	n.key       = std::move(spec.key);
	n.actions   = std::move(spec.actions);
	n.whenMs    = now;
	armPopup(n, spec.popup && m_popupsEnabled, spec.timeoutMs, now);
	const std::uint64_t id = n.id;
	m_entries.push_back(std::move(n));

	if (m_entries.size() > k_maxEntries)
	{
		// Drop the oldest SEEN entry rather than simply the oldest: an unread
		// problem must not be pushed out by a run of routine chatter behind it. An
		// entry whose buttons still work, or whose modal has not been shown, goes
		// last of all — losing it silently is worse than losing a log line.
		auto evictable = [](const Notification& e){ return !e.modalPending && e.actions.empty(); };
		const auto last = m_entries.end() - 1;   // the one just posted is never the victim
		auto victim = std::find_if(m_entries.begin(), last,
			[&](const Notification& e){ return e.seen && evictable(e); });
		if (victim == last)
			victim = std::find_if(m_entries.begin(), last, evictable);
		if (victim == last) victim = m_entries.begin();
		m_entries.erase(victim);
	}
	return id;
}

void NotificationStore::setPopupsEnabled(bool enabled)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_popupsEnabled == enabled) return;
	m_popupsEnabled = enabled;
	if (enabled) return;
	// Taking the switch away takes the popups away too — a modal that cannot be
	// switched off would defeat the point — but not the rows, and not "read".
	for (Notification& e : m_entries)
	{
		e.toastUntilMs = 0;
		e.modalPending = false;
	}
}

bool NotificationStore::popupsEnabled() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_popupsEnabled;
}

void NotificationStore::resolve(const std::string& key)
{
	if (key.empty()) return;
	std::lock_guard<std::mutex> lock(m_mutex);
	for (Notification& e : m_entries)
	{
		if (e.key != key || e.handled) continue;
		e.handled = true;
		e.handledBy.clear();
		e.actions.clear();   // the captures go with them
		settle(e);
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
	return static_cast<std::size_t>(std::count_if(m_entries.begin(), m_entries.end(),
		[](const Notification& e){ return !e.seen; }));
}

void NotificationStore::markAllSeen()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	for (Notification& e : m_entries) e.seen = true;
}

void NotificationStore::clear()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_entries.clear();
}

// ─── What the popups draw ────────────────────────────────────────────────────

std::vector<Notification> NotificationStore::liveToasts(std::uint64_t now) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	std::vector<Notification> out;
	for (const Notification& e : m_entries)
		if (e.toastUntilMs > now) out.push_back(e);
	return out;
}

bool NotificationStore::nextModal(Notification& out) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	for (const Notification& e : m_entries)
	{
		if (!e.modalPending) continue;
		out = e;
		return true;
	}
	return false;
}

void NotificationStore::holdToast(std::uint64_t id, std::uint64_t now, std::uint32_t graceMs)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	for (Notification& e : m_entries)
	{
		if (e.id != id) continue;
		// A toast that already ran out is not revived by a pointer that happens to
		// be over where it was.
		if (e.toastUntilMs > now) e.toastUntilMs = std::max(e.toastUntilMs, now + graceMs);
		return;
	}
}

void NotificationStore::dismissToast(std::uint64_t id)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	for (Notification& e : m_entries)
		if (e.id == id) { e.toastUntilMs = 0; return; }
}

void NotificationStore::acknowledge(std::uint64_t id)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	for (Notification& e : m_entries)
		if (e.id == id) { settle(e); return; }
}

bool NotificationStore::runAction(std::uint64_t id, std::size_t index)
{
	std::function<void()> fn;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = std::find_if(m_entries.begin(), m_entries.end(),
			[id](const Notification& e){ return e.id == id; });
		if (it == m_entries.end() || it->handled || index >= it->actions.size()) return false;

		fn = it->actions[index].run;
		if (it->actions[index].dismisses)
		{
			it->handled   = true;
			it->handledBy = it->actions[index].label;
			it->actions.clear();
			settle(*it);
		}
	}
	// Outside the lock: the action is allowed to post, resolve or clear.
	if (fn) fn();
	return true;
}

// ─── The engine-wide error channel ───────────────────────────────────────────

// Both of these take NO lock of this store's, and that is the point. addSink and
// removeSink take the LOG's mutex, and the log calls sinks with that same mutex
// held — so holding m_bridgeMutex across either of them would establish
// bridge→log here against the log→bridge the sink itself walks, and any thread
// logging an error during a startup or a shutdown would deadlock against it.
// m_logSink needs no lock instead: attach and detach are called once each, from
// the main thread (EditorApplication's OnInit and OnShutdown), and the sink
// never reads it.
void NotificationStore::attachToEngineLog()
{
	if (m_logSink != 0) return;   // already attached; twice would post twice
	m_logSink = HE::Log::addSink(&NotificationStore::logSink, this);
}

void NotificationStore::detachFromEngineLog()
{
	if (m_logSink == 0) return;
	const int handle = m_logSink;
	// Cleared BEFORE the removal, so a record already inside the sink finds a
	// store that is still fully alive — removeSink only guarantees no FURTHER
	// calls, and this is the ordering that makes a second detach a no-op.
	m_logSink = 0;
	HE::Log::removeSink(handle);
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
	NoteSpec spec;
	// An Error is "something failed somewhere": it colours the bell and sits in the
	// centre, but a popup per log line would bury the editor — the call sites that
	// know the user is waiting on the outcome post their own. A Critical record is
	// strictly worse and is the one the log bridge lets through to a modal.
	spec.level  = record.level >= HE::Log::Level::Critical ? NoteLevel::Critical
	                                                       : NoteLevel::Important;
	spec.popup  = spec.level == NoteLevel::Critical;
	spec.text   = std::move(text);
	spec.detail = std::move(detail);
	post(std::move(spec));
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

std::uint64_t notify(NoteSpec spec)
{
	NotificationStore* store = s_globalStore.load(std::memory_order_acquire);
	if (!store) return 0;   // headless, tests, the runtime game: nowhere to show it
	return store->post(std::move(spec));
}

std::uint64_t notify(NoteLevel level, std::string text, std::string detail,
                     std::string assetPath)
{
	NotificationStore* store = s_globalStore.load(std::memory_order_acquire);
	if (!store) return 0;
	return store->post(level, std::move(text), std::move(detail), std::move(assetPath));
}

void resolveNotification(const std::string& key)
{
	if (NotificationStore* store = s_globalStore.load(std::memory_order_acquire))
		store->resolve(key);
}

} // namespace HE::Ed
