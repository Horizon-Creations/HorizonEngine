#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// ── "Something happened that you did not do" ─────────────────────────────────
// The editor had no such channel. It had four half-channels: a one-line collab
// activity string in the footer that is destroyed the moment you click it, a
// read-only banner that only exists for the ACTIVE tab, a play-session report
// that only opens after play stops, and log lines nobody has open (there is no
// console panel). So everything that goes wrong quietly — a peer that could not
// apply a delete, a reference scan that could not read a file, an asset a host
// never answered about — went nowhere at all.
//
// This is that channel: a bounded list of things worth telling the user, posted
// from ANY thread, drawn by the footer bell and its flyout.
//
// It is deliberately NOT a log. A log answers "what happened"; this answers
// "what still needs you". Entries are few, they are written in sentences, and
// the ones that need an action carry the path that action applies to.

namespace HE::Ed
{

enum class NoteLevel : std::uint8_t
{
	Info = 0,     // it worked, and somebody other than you should know
	Warning = 1,  // it did not fully work, and nothing is lost
	Problem = 2,  // something is out of step and will stay that way until someone acts
};

struct Notification
{
	NoteLevel     level = NoteLevel::Info;
	std::string   text;        // one sentence, already phrased for a human
	std::string   detail;      // optional second line: the reason, an error string
	std::string   assetPath;   // optional: what it is about, absolute where possible
	std::uint64_t whenMs = 0;  // steady clock at post time — NOT ImGui::GetTime()
	int           count  = 1;  // consecutive identical posts collapse into one row
	bool          seen   = false;
};

// Posting is thread-safe; everything else is main-thread only, because it is
// only ever called from the frame that draws.
class NotificationStore
{
public:
	// Safe from any thread. Identical consecutive posts (same level and text)
	// increment the last entry's count instead of adding a row — a worker that
	// fails on four hundred files must not produce four hundred rows.
	void post(NoteLevel level, std::string text, std::string detail = {},
	          std::string assetPath = {});

	// A copy, taken under the lock, for the frame to draw. Copying rather than
	// exposing the vector is what keeps the lock off the ImGui call path.
	std::vector<Notification> snapshot() const;

	std::size_t unseenCount() const;
	void markAllSeen();
	void clear();

	// The steady clock the entries are stamped with, so the UI measures an age
	// against the same one that recorded it.
	static std::uint64_t nowMs();

private:
	mutable std::mutex        m_mutex;
	std::vector<Notification> m_entries;
	std::size_t               m_unseen = 0;

	// Bounded on purpose: this is a surface, not storage. The oldest SEEN entry
	// goes first so an unread problem is never pushed out by a run of chatter.
	static constexpr std::size_t k_maxEntries = 200;
};

} // namespace HE::Ed
