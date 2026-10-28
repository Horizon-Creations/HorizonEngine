// CollabSettle: when a live session's state is meant to be kept. Pure and
// clock-driven, so every case drives time instead of sleeping.

#include "doctest.h"

#include "../src/HE_Editor/CollabSettle.h"

#include <string>
#include <vector>

using HE::Ed::CollabSettle;

namespace
{
using Reason = CollabSettle::Reason;

const CollabSettle::Persist kAlways = [](const std::string&, Reason) { return true; };

bool has(const std::vector<CollabSettle::Settled>& v, const std::string& key, Reason r)
{
	for (const auto& s : v)
		if (s.key == key && s.reason == r) return true;
	return false;
}
} // namespace

TEST_CASE("Settle: a document nobody touches for kIdleMs settles once")
{
	CollabSettle s;
	s.noteChanged("Content/Door.hasset", 1000);
	CHECK(s.update(1000 + CollabSettle::kIdleMs - 1, kAlways).empty());

	const auto out = s.update(1000 + CollabSettle::kIdleMs, kAlways);
	REQUIRE(out.size() == 1);
	CHECK(has(out, "Content/Door.hasset", Reason::Idle));
	CHECK_FALSE(s.isPending("Content/Door.hasset"));
	// Once is once: nothing new happened, nothing settles again.
	CHECK(s.update(1000 + 10 * CollabSettle::kIdleMs, kAlways).empty());
}

TEST_CASE("Settle: every change restarts the quiet time")
{
	CollabSettle s;
	// A drag: a transform every frame for three seconds.
	std::uint64_t t = 0;
	for (; t < 3000; t += 16) s.noteChanged(CollabSettle::kSceneKey, t);
	CHECK(s.update(t - 16 + CollabSettle::kIdleMs - 1, kAlways).empty());
	CHECK(has(s.update(t - 16 + CollabSettle::kIdleMs, kAlways), CollabSettle::kSceneKey, Reason::Idle));
}

TEST_CASE("Settle: a save settles after the batch window, Save All as one batch")
{
	CollabSettle s;
	s.noteChanged("A.hasset", 0);
	s.noteChanged("B.hasset", 0);
	s.noteSaved("A.hasset", 100);
	s.noteSaved("B.hasset", 300);   // pushes the batch out
	CHECK(s.update(100 + CollabSettle::kSaveBatchMs, kAlways).empty());

	const auto out = s.update(300 + CollabSettle::kSaveBatchMs, kAlways);
	REQUIRE(out.size() == 2);
	CHECK(has(out, "A.hasset", Reason::Saved));
	CHECK(has(out, "B.hasset", Reason::Saved));
	// The save covered the change before it: no second, idle settle later.
	CHECK(s.update(10 * CollabSettle::kIdleMs, kAlways).empty());
}

TEST_CASE("Settle: a change after the save still settles on its own")
{
	CollabSettle s;
	s.noteSaved("A.hasset", 0);
	s.noteChanged("A.hasset", 500);   // the holder kept editing
	CHECK(has(s.update(CollabSettle::kSaveBatchMs, kAlways), "A.hasset", Reason::Saved));
	CHECK(s.isPending("A.hasset"));
	CHECK(has(s.update(500 + CollabSettle::kIdleMs, kAlways), "A.hasset", Reason::Idle));
}

TEST_CASE("Settle: the Level Script settles as the scene it is stored in")
{
	CollabSettle s;
	s.noteChanged(CollabSettle::kLevelScriptKey, 0);
	s.noteChanged(CollabSettle::kSceneKey, 10);
	const auto out = s.update(10 + CollabSettle::kIdleMs, kAlways);
	REQUIRE(out.size() == 1);
	CHECK(out[0].key == CollabSettle::kSceneKey);
}

TEST_CASE("Settle: a persist step that cannot write now keeps the key pending")
{
	// Play, an unnamed scene, a disk conflict: the host cannot write the scene,
	// and reporting it as kept would hand the team server a stale file.
	CollabSettle s;
	s.noteChanged(CollabSettle::kSceneKey, 0);
	s.noteSaved("A.hasset", 0);
	int asked = 0;
	const CollabSettle::Persist notNow = [&](const std::string& key, Reason) {
		++asked;
		return key != CollabSettle::kSceneKey;
	};
	const auto first = s.update(CollabSettle::kIdleMs, notNow);
	CHECK(has(first, "A.hasset", Reason::Saved));
	CHECK_FALSE(has(first, CollabSettle::kSceneKey, Reason::Idle));
	CHECK(s.isPending(CollabSettle::kSceneKey));
	CHECK(asked == 2);

	// Back in the editor: the next update writes it.
	CHECK(has(s.update(CollabSettle::kIdleMs + 16, kAlways), CollabSettle::kSceneKey, Reason::Idle));
	CHECK(s.pendingCount() == 0);
}

TEST_CASE("Settle: a save that cannot be written is retried, not dropped")
{
	CollabSettle s;
	s.noteSaved(CollabSettle::kSceneKey, 0);
	const CollabSettle::Persist never = [](const std::string&, Reason) { return false; };
	CHECK(s.update(CollabSettle::kSaveBatchMs, never).empty());
	CHECK(s.isPending(CollabSettle::kSceneKey));
	const auto out = s.update(3 * CollabSettle::kSaveBatchMs, kAlways);
	CHECK(has(out, CollabSettle::kSceneKey, Reason::Saved));
}
