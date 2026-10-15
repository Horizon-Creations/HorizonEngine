#include "doctest.h"

#include "../src/HE_Editor/NotificationStore.h"

#include <Diagnostics/Log.h>

#include <string>
#include <thread>
#include <vector>

// ── The editor's "something happened that you did not do" channel ────────────
// The collab paths that post into this store are covered in
// test_collab_controller.cpp. What is covered here is the store's own two
// engine-wide entry points, both of which exist to be called from code that has
// no editor context at all:
//
//   • HE::Ed::notify() — the global posting channel for worker threads and
//     download callbacks. Its no-op-when-uninstalled behaviour is what lets
//     those call sites exist unconditionally, so a headless run or a test that
//     never installs a store must not touch a dangling pointer.
//   • attachToEngineLog() — every HE_LOG_ERROR in the engine as a notification.
//     Its throttling is the entire reason it is safe to attach at all: the
//     failures worth catching this way are exactly the ones that repeat every
//     frame, and a surface with 4000 rows on it is not a surface.

namespace {

using HE::Ed::NoteLevel;
using HE::Ed::Notification;
using HE::Ed::NotificationStore;

int countWithText(const NotificationStore& store, const std::string& text)
{
	int n = 0;
	for (const Notification& e : store.snapshot())
		if (e.text == text) ++n;
	return n;
}

// A store that always detaches, whatever the test does — a sink left installed
// after its store dies is a dangling callback for the rest of the run, and the
// next test to log an error would find it.
struct AttachedStore
{
	NotificationStore store;
	AttachedStore()  { store.attachToEngineLog(); }
	~AttachedStore() { store.detachFromEngineLog(); }
};

// The bridge only listens to records that are actually EMITTED, so a category
// filtered above Error would make these tests pass for the wrong reason.
struct ErrorsEnabled
{
	HE::Log::Level previous;
	ErrorsEnabled()
		: previous(HE::Log::verbosity(HE::Log::Cat::Editor))
	{
		HE::Log::setVerbosity(HE::Log::Cat::Editor, HE::Log::Level::Trace);
	}
	~ErrorsEnabled() { HE::Log::setVerbosity(HE::Log::Cat::Editor, previous); }
};

} // namespace

TEST_CASE("notify() without an installed store is a no-op")
{
	// The whole point: ContentBrowserPanel and the download callbacks call this
	// unconditionally, including from tests and headless runs where nobody ever
	// installed a store. It must not crash and must not remember anything.
	HE::Ed::setGlobalNotifications(nullptr);
	HE::Ed::notify(NoteLevel::Important, "nobody is listening");

	NotificationStore store;
	HE::Ed::setGlobalNotifications(&store);
	HE::Ed::notify(NoteLevel::Important, "now somebody is");
	CHECK(store.snapshot().size() == 1);

	// Cleared exactly the way EditorApplication::OnShutdown clears it, so a post
	// that arrives after the store is gone is a no-op rather than a write through
	// a dangling pointer.
	HE::Ed::setGlobalNotifications(nullptr);
	HE::Ed::notify(NoteLevel::Important, "too late");
	CHECK(store.snapshot().size() == 1);
}

TEST_CASE("notify() carries level, detail and path through")
{
	NotificationStore store;
	HE::Ed::setGlobalNotifications(&store);
	HE::Ed::notify(NoteLevel::Important, "\"Rock.hasset\" could not be downloaded.",
	               "The server did not hand over the file.",
	               "Engine/Meshes/Rock.hasset");
	HE::Ed::setGlobalNotifications(nullptr);

	const std::vector<Notification> all = store.snapshot();
	REQUIRE(all.size() == 1);
	CHECK(all[0].level == NoteLevel::Important);
	CHECK(all[0].detail == "The server did not hand over the file.");
	CHECK(all[0].assetPath == "Engine/Meshes/Rock.hasset");
	CHECK(all[0].seen == false);
}

TEST_CASE("the engine log bridge reports errors and ignores everything below")
{
	ErrorsEnabled levels;
	AttachedStore attached;

	HE_LOG_INFO (Editor, "%s", "bridge test: an info line");
	HE_LOG_WARN (Editor, "%s", "bridge test: a warning line");
	HE_LOG_ERROR(Editor, "%s", "bridge test: an error line");

	const std::vector<Notification> all = attached.store.snapshot();
	REQUIRE(all.size() == 1);
	CHECK(all[0].level == NoteLevel::Important);
	CHECK(all[0].text == "bridge test: an error line");
	// The detail names the category and the source line, which is what makes the
	// row a starting point in HorizonEngine.log rather than a shrug.
	CHECK(all[0].detail.find("Editor") != std::string::npos);
	CHECK(all[0].detail.find("test_notification_store.cpp") != std::string::npos);
}

TEST_CASE("the same error inside the cooldown is dropped, not counted")
{
	ErrorsEnabled levels;
	AttachedStore attached;

	// The failure mode this exists for: something that fails every frame. The
	// store's own collapse only merges the row at the END of the list, so two
	// errors alternating would interleave into two unbounded lists — hence the
	// bridge dedupes by message before it ever calls post().
	for (int i = 0; i < 50; ++i)
	{
		HE_LOG_ERROR(Editor, "%s", "bridge test: repeated failure A");
		HE_LOG_ERROR(Editor, "%s", "bridge test: repeated failure B");
	}

	CHECK(countWithText(attached.store, "bridge test: repeated failure A") == 1);
	CHECK(countWithText(attached.store, "bridge test: repeated failure B") == 1);
	CHECK(attached.store.snapshot().size() == 2);
}

TEST_CASE("a burst of distinct errors is capped")
{
	ErrorsEnabled levels;
	AttachedStore attached;

	// Distinct messages, so the dedup above cannot be what bounds this. Ten in a
	// window already says "something is badly wrong"; the rest would only push
	// the first — the one that probably caused the others — out of view.
	for (int i = 0; i < 40; ++i)
		HE_LOG_ERROR(Editor, "bridge test: distinct failure %d", i);

	const std::size_t posted = attached.store.snapshot().size();
	CHECK(posted > 0);
	CHECK(posted <= 10);
}

TEST_CASE("detaching stops the bridge")
{
	ErrorsEnabled levels;
	NotificationStore store;

	store.attachToEngineLog();
	// Attaching twice must not post twice — OnInit is not the only place that
	// could reach this, and a doubled sink doubles every row.
	store.attachToEngineLog();
	HE_LOG_ERROR(Editor, "%s", "bridge test: while attached");
	CHECK(countWithText(store, "bridge test: while attached") == 1);

	store.detachFromEngineLog();
	HE_LOG_ERROR(Editor, "%s", "bridge test: after detaching");
	CHECK(countWithText(store, "bridge test: after detaching") == 0);
}

TEST_CASE("posting from several threads at once keeps the store consistent")
{
	// post() promises to be callable from any thread, and every producer that
	// matters — the SFTP worker, the download callbacks, the reference scan — is
	// one. Distinct texts, because identical ones would legitimately collapse.
	NotificationStore store;
	std::vector<std::thread> threads;
	for (int t = 0; t < 4; ++t)
	{
		threads.emplace_back([&store, t]
		{
			for (int i = 0; i < 25; ++i)
				store.post(NoteLevel::Important,
				           "thread " + std::to_string(t) + " item " + std::to_string(i));
		});
	}
	for (std::thread& th : threads) th.join();

	CHECK(store.snapshot().size() == 100);
	CHECK(store.unseenCount() == 100);
	store.markAllSeen();
	CHECK(store.unseenCount() == 0);
}

// ── Tiers, popups and buttons ────────────────────────────────────────────────
// The store decides what is due (a toast's lifetime, which modal is next, what a
// click does to the row); the ImGui code only draws it. So the whole behaviour of
// the three tiers is testable here, with the clock passed in.

namespace {

using HE::Ed::NoteAction;
using HE::Ed::NoteSpec;

NoteSpec spec(NoteLevel level, const char* text)
{
	NoteSpec s;
	s.level = level;
	s.text  = text;
	return s;
}


} // namespace

TEST_CASE("each tier gets exactly its own popup")
{
	NotificationStore store;
	store.post(spec(NoteLevel::Info, "info"));
	store.post(spec(NoteLevel::Important, "important"));
	store.post(spec(NoteLevel::Critical, "critical"));

	// Important: a toast. Info and Critical: not.
	const auto toasts = store.liveToasts(NotificationStore::nowMs());
	REQUIRE(toasts.size() == 1);
	CHECK(toasts[0].text == "important");

	// Critical: a modal. Info and Important: not.
	Notification modal;
	REQUIRE(store.nextModal(modal));
	CHECK(modal.text == "critical");

	// All three are in the centre, unread.
	CHECK(store.snapshot().size() == 3);
	CHECK(store.unseenCount() == 3);
}

TEST_CASE("popup = false keeps a notification in the centre only")
{
	NotificationStore store;
	NoteSpec a = spec(NoteLevel::Important, "quiet");
	a.popup = false;
	NoteSpec b = spec(NoteLevel::Critical, "also quiet");
	b.popup = false;
	store.post(a);
	store.post(b);

	CHECK(store.liveToasts(NotificationStore::nowMs()).empty());
	Notification modal;
	CHECK_FALSE(store.nextModal(modal));
	CHECK(store.snapshot().size() == 2);
}

TEST_CASE("a toast expires on its own, and one with buttons waits longer")
{
	NotificationStore store;
	store.post(spec(NoteLevel::Important, "plain"));
	NoteSpec withButtons = spec(NoteLevel::Important, "asks");
	withButtons.actions.push_back({ "Yes", {}, true });
	store.post(withButtons);

	const std::uint64_t t0 = NotificationStore::nowMs();
	CHECK(store.liveToasts(t0).size() == 2);
	// Past the plain lifetime, inside the longer one.
	const auto mid = store.liveToasts(t0 + NotificationStore::k_toastMs + 1000);
	REQUIRE(mid.size() == 1);
	CHECK(mid[0].text == "asks");
	CHECK(store.liveToasts(t0 + NotificationStore::k_toastActionMs + 1000).empty());

	// Gone from the screen, not from the centre.
	CHECK(store.snapshot().size() == 2);
	CHECK(store.unseenCount() == 2);
}

TEST_CASE("an explicit timeout overrides the tier default")
{
	NotificationStore store;
	NoteSpec s = spec(NoteLevel::Important, "short");
	s.timeoutMs = 500;
	store.post(s);
	const std::uint64_t t0 = NotificationStore::nowMs();
	CHECK(store.liveToasts(t0).size() == 1);
	CHECK(store.liveToasts(t0 + 1000).empty());
}

TEST_CASE("hovering a toast keeps it, dismissing removes it, and neither revives an expired one")
{
	NotificationStore store;
	const std::uint64_t id = store.post(spec(NoteLevel::Important, "hover me"));
	const std::uint64_t t0 = NotificationStore::nowMs();

	const std::uint64_t nearEnd = t0 + NotificationStore::k_toastMs - 100;
	store.holdToast(id, nearEnd);
	// Would have expired at +k_toastMs; the hold pushed it out by the grace period.
	CHECK(store.liveToasts(t0 + NotificationStore::k_toastMs + 1000).size() == 1);

	store.dismissToast(id);
	CHECK(store.liveToasts(t0).empty());
	// The close button is not "read": the row stays unread in the centre.
	CHECK(store.unseenCount() == 1);

	NotificationStore other;
	const std::uint64_t id2 = other.post(spec(NoteLevel::Important, "late"));
	other.holdToast(id2, NotificationStore::nowMs() + NotificationStore::k_toastMs + 5000);
	CHECK(other.liveToasts(NotificationStore::nowMs() + NotificationStore::k_toastMs + 5000).empty());
}

TEST_CASE("critical notifications queue as modals, oldest first, and acknowledging reads them")
{
	NotificationStore store;
	const std::uint64_t first  = store.post(spec(NoteLevel::Critical, "first"));
	const std::uint64_t second = store.post(spec(NoteLevel::Critical, "second"));

	Notification m;
	REQUIRE(store.nextModal(m));
	CHECK(m.id == first);

	store.acknowledge(first);
	REQUIRE(store.nextModal(m));
	CHECK(m.id == second);

	store.acknowledge(second);
	CHECK_FALSE(store.nextModal(m));
	CHECK(store.unseenCount() == 0);
	// Still in the centre afterwards.
	CHECK(store.snapshot().size() == 2);
}

TEST_CASE("pressing a button runs it once, settles the row, and leaves a record")
{
	NotificationStore store;
	int joined = 0, ignored = 0;
	NoteSpec s = spec(NoteLevel::Important, "Anna invited you.");
	s.actions.push_back({ "Join",   [&] { ++joined; },  true });
	s.actions.push_back({ "Ignore", [&] { ++ignored; }, false });
	const std::uint64_t id = store.post(s);

	REQUIRE(store.runAction(id, 0));
	CHECK(joined == 1);
	CHECK(ignored == 0);

	// Answered: no second press, no toast, read, and the row says which button.
	CHECK_FALSE(store.runAction(id, 1));
	CHECK_FALSE(store.runAction(id, 0));
	CHECK(joined == 1);
	CHECK(store.liveToasts(NotificationStore::nowMs()).empty());
	const auto rows = store.snapshot();
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].handled);
	CHECK(rows[0].handledBy == "Join");
	CHECK(rows[0].actions.empty());
	CHECK(rows[0].seen);
}

TEST_CASE("a button that does not dismiss leaves the notification up")
{
	NotificationStore store;
	int copied = 0;
	NoteSpec s = spec(NoteLevel::Critical, "boom");
	s.actions.push_back({ "Copy details", [&] { ++copied; }, false, /*dismisses*/ false });
	const std::uint64_t id = store.post(s);

	CHECK(store.runAction(id, 0));
	CHECK(store.runAction(id, 0));   // still there to press again
	CHECK(copied == 2);
	Notification m;
	CHECK(store.nextModal(m));
}

TEST_CASE("a button's callback may post and resolve without deadlocking")
{
	NotificationStore store;
	NoteSpec s = spec(NoteLevel::Important, "ask");
	s.actions.push_back({ "Go", [&] {
		store.post(NoteLevel::Info, "it worked");
		store.resolve("other");
		store.clear();
	}, true });
	const std::uint64_t id = store.post(s);
	CHECK(store.runAction(id, 0));   // would hang here if the lock were held
}

TEST_CASE("a key makes a notification replace its earlier self, and resolve withdraws it")
{
	NotificationStore store;
	int n = 0;
	NoteSpec s = spec(NoteLevel::Important, "invite");
	s.key = "invite.abc";
	s.actions.push_back({ "Join", [&] { ++n; }, true });
	store.post(s);
	s.text = "invite again";
	store.post(s);

	auto rows = store.snapshot();
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].text == "invite again");
	CHECK(store.liveToasts(NotificationStore::nowMs()).size() == 1);

	store.resolve("invite.abc");
	rows = store.snapshot();
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].handled);
	CHECK(rows[0].handledBy.empty());
	CHECK(rows[0].actions.empty());
	CHECK(store.liveToasts(NotificationStore::nowMs()).empty());
	CHECK_FALSE(store.runAction(rows[0].id, 0));
	CHECK(n == 0);

	// An unknown key is not an error.
	store.resolve("nothing.here");
	store.resolve("");
}

TEST_CASE("resolve closes a pending modal")
{
	NotificationStore store;
	NoteSpec s = spec(NoteLevel::Critical, "stale");
	s.key = "k";
	store.post(s);
	store.resolve("k");
	Notification m;
	CHECK_FALSE(store.nextModal(m));
}

TEST_CASE("rows with buttons are never merged into a repeat")
{
	NotificationStore store;
	NoteSpec s = spec(NoteLevel::Important, "same");
	s.actions.push_back({ "A", {}, false });
	store.post(s);
	store.post(s);
	CHECK(store.snapshot().size() == 2);

	// ...whereas plain repeats still collapse, and a repeat of a toast re-arms it.
	NotificationStore plain;
	plain.post(NoteLevel::Important, "again");
	plain.markAllSeen();
	plain.post(NoteLevel::Important, "again");
	const auto rows = plain.snapshot();
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].count == 2);
	CHECK_FALSE(rows[0].seen);
}

TEST_CASE("the cap never throws away a notification whose buttons still work")
{
	NotificationStore store;
	NoteSpec keep = spec(NoteLevel::Important, "keep me");
	keep.actions.push_back({ "Yes", {}, true });
	keep.popup = false;
	store.post(keep);
	// Far more than the cap of plain, already-read chatter.
	for (int i = 0; i < 400; ++i)
	{
		store.post(NoteLevel::Info, "chatter " + std::to_string(i));
		store.markAllSeen();
	}
	bool found = false;
	for (const Notification& e : store.snapshot())
		if (e.text == "keep me") found = true;
	CHECK(found);
	CHECK(store.snapshot().size() <= 200);
}

TEST_CASE("the log bridge colours the bell but never raises a popup for an error")
{
	ErrorsEnabled levels;
	AttachedStore attached;
	HE_LOG_ERROR(Editor, "%s", "bridge test: no popup please");

	CHECK(attached.store.snapshot().size() == 1);
	CHECK(attached.store.liveToasts(NotificationStore::nowMs()).empty());
	Notification m;
	CHECK_FALSE(attached.store.nextModal(m));
}

TEST_CASE("notify() returns the id, and resolveNotification reaches the installed store")
{
	CHECK(HE::Ed::notify(NoteLevel::Info, "nobody home") == 0);

	NotificationStore store;
	HE::Ed::setGlobalNotifications(&store);
	NoteSpec s = spec(NoteLevel::Important, "via global");
	s.key = "g";
	s.actions.push_back({ "X", {}, false });
	CHECK(HE::Ed::notify(s) != 0);
	HE::Ed::resolveNotification("g");
	HE::Ed::setGlobalNotifications(nullptr);

	const auto rows = store.snapshot();
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].handled);
}

TEST_CASE("switching popups off sends everything to the centre and takes down what is up")
{
	NotificationStore store;
	store.post(spec(NoteLevel::Important, "up already"));
	store.post(spec(NoteLevel::Critical, "dialog already"));
	CHECK(store.popupsEnabled());

	store.setPopupsEnabled(false);
	CHECK_FALSE(store.popupsEnabled());
	// What was up is gone from the screen, not from the centre, and still unread.
	CHECK(store.liveToasts(NotificationStore::nowMs()).empty());
	Notification m;
	CHECK_FALSE(store.nextModal(m));
	CHECK(store.snapshot().size() == 2);
	CHECK(store.unseenCount() == 2);

	// New ones, whatever their tier, are centre-only — but buttons still work there.
	NoteSpec asks = spec(NoteLevel::Important, "invite");
	asks.actions.push_back({ "Join", {}, true });
	const std::uint64_t id = store.post(asks);
	store.post(spec(NoteLevel::Critical, "boom"));
	// A repeat of a plain row re-arms nothing either.
	store.post(NoteLevel::Important, "again");
	store.post(NoteLevel::Important, "again");
	CHECK(store.liveToasts(NotificationStore::nowMs()).empty());
	CHECK_FALSE(store.nextModal(m));
	CHECK(store.runAction(id, 0));

	// On again: only what is posted from now on pops up.
	store.setPopupsEnabled(true);
	CHECK(store.liveToasts(NotificationStore::nowMs()).empty());
	CHECK_FALSE(store.nextModal(m));
	store.post(spec(NoteLevel::Important, "fresh"));
	store.post(spec(NoteLevel::Critical, "fresh dialog"));
	CHECK(store.liveToasts(NotificationStore::nowMs()).size() == 1);
	CHECK(store.nextModal(m));
	CHECK(m.text == "fresh dialog");
}
