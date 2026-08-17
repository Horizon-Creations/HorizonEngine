#pragma once
#include <Diagnostics/Log.h>   // the engine-wide error channel this can attach to

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
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

	// ── The engine-wide error channel ────────────────────────────────────────
	// Forwards every HE_LOG_ERROR record into this store as a Problem, so a
	// failure nobody thought to wire up by hand still reaches the user instead of
	// dying in a log file nobody has open. Errors only — Warning is where routine
	// "did not fully work" lives, and there are hundreds of those per session.
	//
	// Deliberately NOT a log window: identical messages inside a cooldown are
	// dropped rather than counted, and a burst is capped, because the failures
	// this catches are exactly the ones that repeat every frame (a shader that
	// will not compile, an asset that will not load). What the user needs from
	// those is one line, not four thousand.
	//
	// Lock order is log-mutex → this store's mutex: HE::Log calls sinks with its
	// own mutex held, and post() must therefore never log. It does not, and must
	// not start to.
	void attachToEngineLog();
	void detachFromEngineLog();

private:
	// The sink half of attachToEngineLog. A static member rather than a lambda:
	// HE::Log::addSink takes a plain function pointer plus a user pointer, and a
	// capturing lambda does not convert to one.
	static void logSink(const HE::Log::Record& record, void* user);
	void        onLogError(const HE::Log::Record& record);

	mutable std::mutex        m_mutex;
	std::vector<Notification> m_entries;
	std::size_t               m_unseen = 0;

	// Bounded on purpose: this is a surface, not storage. The oldest SEEN entry
	// goes first so an unread problem is never pushed out by a run of chatter.
	static constexpr std::size_t k_maxEntries = 200;

	// ── Bridge state ─────────────────────────────────────────────────────────
	// Its own mutex, taken BEFORE m_mutex and never while holding it, so the
	// throttling bookkeeping cannot serialise against a UI snapshot.
	std::mutex                                          m_bridgeMutex;
	int                                                 m_logSink = 0;
	std::vector<std::pair<std::string, std::uint64_t>>  m_recentErrors;  // message → last posted
	std::uint64_t                                       m_burstWindowMs = 0;
	int                                                 m_burstCount    = 0;

	// The same error is worth repeating after a minute, not after a frame.
	static constexpr std::uint64_t k_errorCooldownMs = 60'000;
	// A hard ceiling on how much of this surface one bad second can take. Ten
	// DISTINCT errors in a window already tells the user "something is badly
	// wrong"; the eleventh adds nothing the log file does not have.
	static constexpr std::uint64_t k_burstWindowMs   = 10'000;
	static constexpr int           k_burstMax        = 10;
	// Bounded like everything else here: without it a session that produces a
	// thousand distinct error strings grows this list a thousand entries long.
	static constexpr std::size_t   k_recentErrorsMax = 64;
};

// ── Posting from anywhere ────────────────────────────────────────────────────
// The store is owned by EditorApplication and reached through AppContext, which
// is exactly what the code that needs to post most often does NOT have: worker
// threads, download callbacks, static panel helpers. This is the way in for
// them. It is a no-op when no store is installed (headless, tests, the runtime
// game), so a call site never has to ask whether it is running in the editor.
//
// Same thread-safety as post(): safe from anywhere.
void setGlobalNotifications(NotificationStore* store);
void notify(NoteLevel level, std::string text, std::string detail = {},
            std::string assetPath = {});

} // namespace HE::Ed
