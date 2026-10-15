#pragma once
#include <Diagnostics/Log.h>   // the engine-wide error channel this can attach to

#include <cstdint>
#include <functional>
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
// from ANY thread. It has three tiers, and the tier decides how loud the editor
// is about it (NoteLevel below); every tier is also kept in the notification
// centre (the footer bell and its flyout, NotificationBar.h), so a popup that
// was missed is never lost.
//
// It is deliberately NOT a log. A log answers "what happened"; this answers
// "what still needs you". Entries are few, they are written in sentences, and
// the ones that need an answer carry buttons for it (NoteAction).
//
// Everything about WHEN a popup is due — a toast's lifetime, the order the modals
// queue in, what a click on a button does to the entry — lives here and not in
// the ImGui code, so it is testable without a frame (NotificationPopups.cpp only
// draws what this decides).

namespace HE::Ed
{

enum class NoteLevel : std::uint8_t
{
	// Worth knowing, nothing to do. Notification centre only: no popup, no sound.
	// A teammate joined, an upload finished, a setting was applied.
	Info = 0,
	// Did not fully work, or wants an answer soon, but the user is not blocked.
	// A toast in the bottom-right corner that takes no focus and disappears on its
	// own (the centre keeps it). A collaboration invite, a push that was rejected.
	Important = 1,
	// The thing the user was just doing did not happen, or work is at risk. A
	// modal dialog: it must be acknowledged. Reserve it — a dialog per hiccup
	// teaches people to click it away unread.
	Critical = 2,
};

// A button on a toast, a modal or a centre row.
//
// `run` is called on the MAIN thread, from the frame that draws the button, even
// when the notification itself was posted from a worker. It may capture by value
// only things that outlive the notification (a session id, a path) — never a
// pointer to something a panel can close; resolve() the notification instead when
// the thing it points at goes away.
struct NoteAction
{
	std::string           label;
	std::function<void()> run;        // may be empty: the button then only dismisses
	// The accent-filled "do it" button. At most one per notification; if none is
	// marked, none is highlighted.
	bool                  primary   = false;
	// false: the click runs `run` but leaves the notification up (a "Copy details"
	// on a modal). Default true: acting on it is answering it.
	bool                  dismisses = true;
};

// What a caller hands to post(). A struct rather than a growing parameter list:
// notifications now carry a title, actions, a key, a lifetime, and the next
// thing someone wants will not fit in a fifth positional argument either.
struct NoteSpec
{
	NoteLevel   level = NoteLevel::Info;
	std::string title;       // optional short heading ("Could not join session")
	std::string text;        // one sentence, already phrased for a human
	std::string detail;      // optional second line: the reason, an error string
	std::string assetPath;   // optional: what it is about, absolute where possible
	// Non-empty: this notification is "the" one for that key. Posting again replaces
	// the earlier one (an invitation re-sent, a status that moved on), and
	// resolve(key) withdraws it. Empty: identical consecutive posts collapse.
	std::string key;
	std::vector<NoteAction> actions;
	// Toast lifetime. 0: the default for the tier (see k_toastMs / k_toastActionMs).
	std::uint32_t timeoutMs = 0;
	// false: skip the toast/modal and go to the centre only, whatever the level.
	// For tiers that matter to the bell's colour but would be noise as a popup
	// (the engine log bridge).
	bool popup = true;
};

struct Notification
{
	std::uint64_t id = 0;       // unique for the life of the store, never reused
	NoteLevel     level = NoteLevel::Info;
	std::string   title;
	std::string   text;
	std::string   detail;
	std::string   assetPath;
	std::string   key;
	std::vector<NoteAction> actions;
	std::uint64_t whenMs = 0;  // steady clock at post time — NOT ImGui::GetTime()
	int           count  = 1;  // consecutive identical posts collapse into one row
	bool          seen   = false;

	// Set once an action was taken or resolve() withdrew it: the buttons are gone,
	// the row stays as a record. handledBy is the label that was clicked ("" for a
	// withdrawal), so the centre can say "Join" rather than a bare tick.
	bool          handled = false;
	std::string   handledBy;

	// ── Popup state, owned by the store ──
	std::uint32_t timeoutMs    = 0;   // the toast's full lifetime, for its progress strip
	std::uint64_t toastUntilMs = 0;   // 0: no toast (never had one, dismissed, or over)
	bool          modalPending = false;
};

// Posting is thread-safe; everything else is main-thread only, because it is
// only ever called from the frame that draws.
class NotificationStore
{
public:
	// Safe from any thread. Returns the new entry's id (0 for an empty text).
	std::uint64_t post(NoteSpec spec);

	// The short form most call sites want: a sentence, a reason, a path.
	std::uint64_t post(NoteLevel level, std::string text, std::string detail = {},
	                   std::string assetPath = {});

	// Withdraws the notification posted under `key`: its buttons go, its toast
	// closes, its modal is dismissed, and the row stays in the centre as a record.
	// For "the invitation was answered somewhere else" and "the session this
	// button joins has ended". Safe from any thread; a no-op for an unknown key.
	void resolve(const std::string& key);

	// The user's switch for popups (Preferences ▸ Feedback ▸ Show Notification
	// Popups). Off: every notification goes to the centre only, whatever its tier —
	// no toast, no modal — and the popups that are up right now are taken down (the
	// rows stay, unread). On again: only notifications posted from then on pop up;
	// what arrived meanwhile does not all appear at once. Safe from any thread.
	void setPopupsEnabled(bool enabled);
	bool popupsEnabled() const;

	// A copy, taken under the lock, for the frame to draw. Copying rather than
	// exposing the vector is what keeps the lock off the ImGui call path.
	std::vector<Notification> snapshot() const;

	std::size_t unseenCount() const;
	void markAllSeen();
	void clear();

	// The steady clock the entries are stamped with, so the UI measures an age
	// against the same one that recorded it.
	static std::uint64_t nowMs();

	// ── What the popups draw ─────────────────────────────────────────────────
	// Toasts that are still on the clock, oldest first.
	std::vector<Notification> liveToasts(std::uint64_t nowMs) const;
	// The next modal to show: the OLDEST one still pending, so a burst is read in
	// the order it happened. False when none is waiting.
	bool nextModal(Notification& out) const;
	// The mouse is over a toast: it stays at least `graceMs` longer from now. Asked
	// every frame it is hovered, so it expires `graceMs` after the pointer leaves.
	void holdToast(std::uint64_t id, std::uint64_t nowMs, std::uint32_t graceMs = k_holdMs);
	// The toast's own close button. The row stays in the centre, unread.
	void dismissToast(std::uint64_t id);
	// A modal's dismiss. Marks the row read — it was put in front of the user.
	void acknowledge(std::uint64_t id);
	// Runs action `index` of entry `id` on the calling (main) thread and settles
	// the entry unless the action opted out (NoteAction::dismisses). False when the
	// entry is gone, was already handled, or has no such action. The callback runs
	// with NO lock held, so it may post, resolve or clear.
	bool runAction(std::uint64_t id, std::size_t index);

	// ── The engine-wide error channel ────────────────────────────────────────
	// Forwards every HE_LOG_ERROR record into this store as an Important entry
	// WITHOUT a popup, so a failure nobody thought to wire up by hand still reaches
	// the user (the bell turns amber, the entry is in the centre) instead of dying
	// in a log file nobody has open. Errors only — Warning is where routine "did
	// not fully work" lives, and there are hundreds of those per session. A
	// Critical log record is the exception and does get its modal.
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

	// A toast with buttons waits longer than one without: an invitation read at
	// the speed of a toast is an invitation missed.
	static constexpr std::uint32_t k_toastMs       = 8'000;
	static constexpr std::uint32_t k_toastActionMs = 20'000;
	static constexpr std::uint32_t k_holdMs        = 4'000;

private:
	// The sink half of attachToEngineLog. A static member rather than a lambda:
	// HE::Log::addSink takes a plain function pointer plus a user pointer, and a
	// capturing lambda does not convert to one.
	static void logSink(const HE::Log::Record& record, void* user);
	void        onLogError(const HE::Log::Record& record);

	// Closes the popups of `n` and marks it read; the caller holds m_mutex.
	static void settle(Notification& n);

	mutable std::mutex        m_mutex;
	bool                      m_popupsEnabled = true;   // guarded by m_mutex
	std::vector<Notification> m_entries;
	std::uint64_t             m_nextId = 1;

	// Bounded on purpose: this is a surface, not storage. The oldest SEEN entry
	// goes first so an unread problem is never pushed out by a run of chatter, and
	// an entry whose buttons can still be pressed is never pushed out at all.
	static constexpr std::size_t k_maxEntries = 200;

	// ── Bridge state ─────────────────────────────────────────────────────────
	// Its own mutex, taken BEFORE m_mutex and never while holding it, so the
	// throttling bookkeeping cannot serialise against a UI snapshot — and never
	// held across an HE::Log call, which would invert against the log mutex the
	// sink is already called under.
	std::mutex                                          m_bridgeMutex;
	// Main thread only (attach/detach), never read by the sink — see the note on
	// attachToEngineLog's definition for why it is deliberately lock-free.
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
// Same thread-safety as post(): safe from anywhere. They return the entry's id,
// or 0 when there is no store.
void setGlobalNotifications(NotificationStore* store);
std::uint64_t notify(NoteSpec spec);
std::uint64_t notify(NoteLevel level, std::string text, std::string detail = {},
                     std::string assetPath = {});
void resolveNotification(const std::string& key);

} // namespace HE::Ed
