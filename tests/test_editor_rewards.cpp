#include "doctest.h"

#include "EditorRewards.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

// ── The reward moments' core (EditorRewards.h) ───────────────────────────────
// The promises that are easy to break without seeing it: one moment per frame
// however many call sites fire in it, one build moment per run however often
// the run is polled or finished, nothing for a failed build, and a fade that
// ends where it says it does. Driven with a hand clock, no ImGui.

using namespace HE::Ed::Rewards;

TEST_CASE("Rewards: one moment per frame, the first one wins")
{
	Feed f;
	CHECK(f.push(Moment::Saved, 1, 10.0, 5));
	// The guard's assets and its scene, or a shortcut reaching two handlers.
	CHECK_FALSE(f.push(Moment::Saved, 1, 10.0, 5));
	CHECK_FALSE(f.push(Moment::AssetsImported, 3, 10.0, 5));
	CHECK(f.look(10.0).line == "Saved");
	// The next frame is a new user action.
	CHECK(f.push(Moment::AssetsImported, 3, 10.1, 6));
	CHECK(f.look(10.1).line == "Imported 3 assets");
}

TEST_CASE("Rewards: without a frame clock nothing is folded")
{
	Feed f;
	CHECK(f.push(Moment::Saved, 1, 0.0, -1));
	CHECK(f.push(Moment::Saved, 1, 0.0, -1));
}

TEST_CASE("Rewards: the build moment fires once per successful run")
{
	Feed f;
	// Nothing built yet, then a run in progress.
	CHECK_FALSE(f.buildSucceeded(0, false, false));
	CHECK_FALSE(f.buildSucceeded(1, false, false));
	// It finishes: once, and never again for the same run however long the
	// window keeps showing it, or if finish() is called on it a second time.
	CHECK(f.buildSucceeded(1, true, true));
	CHECK_FALSE(f.buildSucceeded(1, true, true));
	CHECK_FALSE(f.buildSucceeded(1, true, true));
	// A failed run is consumed without a moment.
	CHECK_FALSE(f.buildSucceeded(2, false, false));
	CHECK_FALSE(f.buildSucceeded(2, true, false));
	CHECK_FALSE(f.buildSucceeded(2, true, true));   // same run, re-read
	// A run that began and finished between two polls still counts.
	CHECK(f.buildSucceeded(4, true, true));
}

TEST_CASE("Rewards: the line holds, fades and is gone at the stated time")
{
	CHECK(strengthAt(0.0) == 1.0f);
	CHECK(strengthAt(kHoldSec) == 1.0f);
	CHECK(strengthAt(kHoldSec + kFadeSec) == 0.0f);
	float prev = 1.0f;
	for (double t = kHoldSec; t <= kHoldSec + kFadeSec; t += 0.01)
	{
		const float s = strengthAt(t);
		CHECK(s <= prev);
		CHECK(s >= 0.0f);
		prev = s;
	}

	Feed f;
	CHECK_FALSE(f.look(0.0).active);            // never fired: idle text
	f.push(Moment::BuildSucceeded, 1, 100.0, 1);
	const Feed::Look start = f.look(100.0);
	CHECK(start.active);
	CHECK(start.line == "Build succeeded");
	CHECK(start.strength == 1.0f);
	CHECK(start.bar == doctest::Approx(1.0f));
	const Feed::Look mid = f.look(100.0 + kHoldSec + kFadeSec * 0.5);
	CHECK(mid.active);
	CHECK(mid.strength > 0.0f);
	CHECK(mid.strength < 1.0f);
	CHECK(mid.bar < start.bar);
	CHECK_FALSE(f.look(100.0 + kHoldSec + kFadeSec).active);
}

TEST_CASE("Rewards: the footer lines")
{
	CHECK(lineFor(Moment::Saved, 1) == "Saved");
	CHECK(lineFor(Moment::BuildSucceeded, 1) == "Build succeeded");
	CHECK(lineFor(Moment::AssetsImported, 1) == "Imported 1 asset");
	CHECK(lineFor(Moment::AssetsImported, 12) == "Imported 12 assets");
}

TEST_CASE("Rewards: the chime is short, quiet and ends silent")
{
	constexpr int kRate = 44100;
	const std::vector<uint8_t> pcm = chimePcm16(kRate);
	REQUIRE(pcm.size() % 2 == 0);
	const size_t frames = pcm.size() / 2;
	CHECK(frames > static_cast<size_t>(kRate / 10));   // audible: > 100 ms
	CHECK(frames < static_cast<size_t>(kRate / 2));    // short:   < 500 ms

	auto sample = [&](size_t i) {
		return static_cast<int16_t>(pcm[i * 2] | (pcm[i * 2 + 1] << 8));
	};
	int peak = 0;
	for (size_t i = 0; i < frames; ++i) peak = std::max(peak, std::abs(int(sample(i))));
	CHECK(peak > 3000);                  // not silent
	CHECK(peak < 32767 * 6 / 10);        // and well below full scale
	CHECK(std::abs(int(sample(0))) < 200);            // no click in…
	CHECK(std::abs(int(sample(frames - 1))) < 200);   // …and none out
	CHECK(chimePcm16(0).empty());
}

// ── Progress: the counters beside "Ready" (EditorRewards.h, "Rules") ─────────

TEST_CASE("Rewards: dayBefore crosses months, years and leap days")
{
	CHECK(dayBefore("2026-09-26") == "2026-09-25");
	CHECK(dayBefore("2026-10-01") == "2026-09-30");
	CHECK(dayBefore("2026-03-01") == "2026-02-28");
	CHECK(dayBefore("2028-03-01") == "2028-02-29");   // leap year
	CHECK(dayBefore("2100-03-01") == "2100-02-28");   // century, not leap
	CHECK(dayBefore("2000-03-01") == "2000-02-29");   // 400th, leap
	CHECK(dayBefore("2027-01-01") == "2026-12-31");
	// Not a date: nothing, not a crash.
	CHECK(dayBefore("").empty());
	CHECK(dayBefore("2026-9-26").empty());
	CHECK(dayBefore("2026-02-30").empty());
	CHECK(dayBefore("2026-13-01").empty());
	CHECK(dayBefore("yesterday!").empty());
}

TEST_CASE("Rewards: a day counts once, builds every time")
{
	Tally t;
	CHECK(progressText(t, "2026-09-26").empty());         // never counted: plain "Ready"
	CHECK(recordUse(t, "2026-09-26", false));             // first moment of the day
	CHECK(t.streakDays == 1);
	CHECK(t.buildsToday == 0);
	CHECK(progressText(t, "2026-09-26") == "0 builds today");
	CHECK_FALSE(recordUse(t, "2026-09-26", false));       // a second save: nothing to write
	CHECK(recordUse(t, "2026-09-26", true));
	CHECK(progressText(t, "2026-09-26") == "1 build today");
	CHECK(recordUse(t, "2026-09-26", true));
	CHECK(progressText(t, "2026-09-26") == "2 builds today");
	CHECK(t.streakDays == 1);
}

TEST_CASE("Rewards: consecutive days make a streak, a gap restarts it")
{
	Tally t;
	CHECK(recordUse(t, "2026-12-30", false));
	CHECK(recordUse(t, "2026-12-31", true));              // next day, a build first
	CHECK(t.streakDays == 2);
	CHECK(t.buildsToday == 1);
	CHECK(progressText(t, "2026-12-31") == "1 build today · 2 days in a row");
	CHECK(recordUse(t, "2027-01-01", false));             // across the year
	CHECK(t.streakDays == 3);
	CHECK(t.buildsToday == 0);                            // builds are per day

	// The next morning, before any moment: the streak is still alive, today's
	// builds are 0 — the day is not claimed by opening the editor.
	CHECK(progressText(t, "2027-01-02") == "0 builds today · 3 days in a row");
	// A day missed: the streak is gone from the display, and restarts at 1.
	CHECK(progressText(t, "2027-01-03") == "0 builds today");
	CHECK(recordUse(t, "2027-01-03", false));
	CHECK(t.streakDays == 1);
}

TEST_CASE("Rewards: a clock set back or a hand-edited day does not break the tally")
{
	Tally t;
	CHECK(recordUse(t, "2026-09-26", true));
	CHECK(recordUse(t, "2026-09-27", false));
	REQUIRE(t.streakDays == 2);
	// Clock behind the stored day: left alone, nothing counted.
	CHECK_FALSE(recordUse(t, "2026-09-25", true));
	CHECK(t.day == "2026-09-27");
	CHECK(t.streakDays == 2);
	// A malformed "today" counts nothing either.
	CHECK_FALSE(recordUse(t, "garbage", true));

	// A stored day that does not parse: as if nothing was ever counted.
	Tally bad{ "26.09.2026", 7, 9 };
	CHECK(progressText(bad, "2026-09-26").empty());
	CHECK(recordUse(bad, "2026-09-26", false));
	CHECK(bad.streakDays == 1);
	CHECK(bad.buildsToday == 0);
}

TEST_CASE("Rewards: localDay is a date dayBefore understands")
{
	const std::string today = localDay();
	CHECK(today.size() == 10);
	CHECK_FALSE(dayBefore(today).empty());
}
