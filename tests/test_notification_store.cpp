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
	HE::Ed::notify(NoteLevel::Problem, "nobody is listening");

	NotificationStore store;
	HE::Ed::setGlobalNotifications(&store);
	HE::Ed::notify(NoteLevel::Warning, "now somebody is");
	CHECK(store.snapshot().size() == 1);

	// Cleared exactly the way EditorApplication::OnShutdown clears it, so a post
	// that arrives after the store is gone is a no-op rather than a write through
	// a dangling pointer.
	HE::Ed::setGlobalNotifications(nullptr);
	HE::Ed::notify(NoteLevel::Problem, "too late");
	CHECK(store.snapshot().size() == 1);
}

TEST_CASE("notify() carries level, detail and path through")
{
	NotificationStore store;
	HE::Ed::setGlobalNotifications(&store);
	HE::Ed::notify(NoteLevel::Warning, "\"Rock.hasset\" could not be downloaded.",
	               "The server did not hand over the file.",
	               "Engine/Meshes/Rock.hasset");
	HE::Ed::setGlobalNotifications(nullptr);

	const std::vector<Notification> all = store.snapshot();
	REQUIRE(all.size() == 1);
	CHECK(all[0].level == NoteLevel::Warning);
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
	CHECK(all[0].level == NoteLevel::Problem);
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
				store.post(NoteLevel::Warning,
				           "thread " + std::to_string(t) + " item " + std::to_string(i));
		});
	}
	for (std::thread& th : threads) th.join();

	CHECK(store.snapshot().size() == 100);
	CHECK(store.unseenCount() == 100);
	store.markAllSeen();
	CHECK(store.unseenCount() == 0);
}
