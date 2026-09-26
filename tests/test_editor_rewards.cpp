#include "doctest.h"

#include "EditorRewards.h"

#include <algorithm>
#include <cmath>
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

// ── Several moments close together (EditorRewards.h, topic 95) ───────────────

TEST_CASE("Rewards: the same moment again merges, counts up and stays silent")
{
	Feed f;
	CHECK(f.take(Moment::AssetsImported, 1, 0.0, 1, true).sound);
	// Every import lands inside the previous one's line: one line, counted up,
	// the hold restarted each time — and no tone, even once the 2 s tone gap
	// has run out (t = 3).
	for (int i = 1; i <= 3; ++i)
	{
		const Feed::Taken t = f.take(Moment::AssetsImported, 2, double(i), 1 + i, true);
		CHECK(t.taken);
		CHECK(t.shown);
		CHECK_FALSE(t.sound);
	}
	CHECK(f.look(3.0).line == "Imported 7 assets");
	CHECK(f.look(3.0 + kHoldSec).strength == 1.0f);   // held from t = 3
	// (+1 ms: 3.0 + 0.9 + 0.7 - 3.0 rounds to just under 1.6.)
	CHECK_FALSE(f.look(3.0 + kHoldSec + kFadeSec + 0.001).active);

	// A second "Saved" keeps the line as it is.
	Feed s;
	s.push(Moment::Saved, 1, 0.0, 1);
	s.push(Moment::Saved, 1, 0.5, 2);
	CHECK(s.look(0.5).line == "Saved");
}

TEST_CASE("Rewards: a lower moment leaves a higher line alone")
{
	CHECK(rankOf(Moment::BuildSucceeded) > rankOf(Moment::AssetsImported));
	CHECK(rankOf(Moment::AssetsImported) > rankOf(Moment::Saved));

	Feed f;
	CHECK(f.take(Moment::BuildSucceeded, 1, 0.0, 1, true).sound);
	// A save and an import while "Build succeeded" shows: taken (the tally
	// counted them), not shown, not heard — even with the tone gap over — and
	// the build's line is not held any longer for them.
	const Feed::Taken save = f.take(Moment::Saved, 1, 1.0, 2, true);
	CHECK(save.taken);
	CHECK_FALSE(save.shown);
	CHECK_FALSE(save.sound);
	const Feed::Taken imp = f.take(Moment::AssetsImported, 4, 1.5, 3, true);
	CHECK_FALSE(imp.shown);
	CHECK_FALSE(imp.sound);
	CHECK(f.look(1.5).line == "Build succeeded");
	CHECK_FALSE(f.look(kHoldSec + kFadeSec).active);

	// Once the line has faded, anything replaces it.
	const Feed::Taken later = f.take(Moment::Saved, 1, 5.0, 4, true);
	CHECK(later.shown);
	CHECK(later.sound);
	CHECK(f.look(5.0).line == "Saved");

	// A higher moment replaces a lower line that still shows.
	const Feed::Taken up = f.take(Moment::AssetsImported, 2, 5.5, 5, false);
	CHECK(up.shown);
	CHECK(f.look(5.5).line == "Imported 2 assets");
}

TEST_CASE("Rewards: two tones are at least the tone gap apart")
{
	Feed f;
	CHECK(f.take(Moment::Saved, 1, 0.0, 1, true).sound);
	// Import outranks the save's line and is shown — inside the gap, silent.
	const Feed::Taken imp = f.take(Moment::AssetsImported, 1, 1.0, 2, true);
	CHECK(imp.shown);
	CHECK_FALSE(imp.sound);
	// Just short of the gap: still silent.
	CHECK_FALSE(f.take(Moment::BuildSucceeded, 1, kToneGapSec - 0.01, 3, true).sound);

	// The gap runs from the last tone that PLAYED (t = 0), not from the silent
	// import at t = 1: a build at exactly t = 2 is heard.
	Feed g;
	CHECK(g.take(Moment::Saved, 1, 0.0, 1, true).sound);
	CHECK_FALSE(g.take(Moment::AssetsImported, 1, 1.0, 2, true).sound);
	CHECK(g.take(Moment::BuildSucceeded, 1, kToneGapSec, 3, true).sound);
}

TEST_CASE("Rewards: saves are heard at most every save gap")
{
	Feed f;
	CHECK(f.take(Moment::Saved, 1, 0.0, 1, true).sound);
	// Another kind after the tone gap is heard; the save gap is saves' own.
	CHECK(f.take(Moment::AssetsImported, 1, 3.0, 2, true).sound);
	// Cmd+S again at t = 10: shown (the import's line is gone), not heard.
	const Feed::Taken again = f.take(Moment::Saved, 1, 10.0, 3, true);
	CHECK(again.shown);
	CHECK_FALSE(again.sound);
	CHECK_FALSE(f.take(Moment::Saved, 1, kSaveToneGapSec - 0.5, 4, true).sound);
	// Past twenty seconds after the last save TONE (t = 0), whatever came
	// between — and after the t = 19.5 line has faded, or it would merge.
	CHECK(f.take(Moment::Saved, 1, kSaveToneGapSec + 2.0, 5, true).sound);
}

TEST_CASE("Rewards: a moment that may not sound does not book a gap")
{
	// Sound off, Play-in-Editor, volume 0: the caller says no. The next moment
	// that may sound is heard at once.
	Feed f;
	const Feed::Taken quiet = f.take(Moment::BuildSucceeded, 1, 0.0, 1, false);
	CHECK(quiet.taken);
	CHECK(quiet.shown);
	CHECK_FALSE(quiet.sound);
	CHECK(f.take(Moment::BuildSucceeded, 1, kHoldSec + kFadeSec + 0.1, 2, true).sound);

	// A save that was shown silently does not start the save gap either.
	Feed s;
	s.take(Moment::Saved, 1, 0.0, 1, false);
	CHECK(s.take(Moment::Saved, 1, 5.0, 2, true).sound);

	// And the once-per-frame fold still comes first.
	Feed o;
	CHECK(o.take(Moment::Saved, 1, 0.0, 7, true).sound);
	const Feed::Taken folded = o.take(Moment::BuildSucceeded, 1, 0.0, 7, true);
	CHECK_FALSE(folded.taken);
	CHECK_FALSE(folded.sound);
}

TEST_CASE("Rewards: the volume slider is squared into a gain")
{
	CHECK(gainFor(0.0f) == 0.0f);
	CHECK(gainFor(0.5f) == doctest::Approx(0.25f));
	CHECK(gainFor(1.0f) == 1.0f);
	// Out of range from a hand-edited config: clamped, never louder than 1.
	CHECK(gainFor(2.0f) == 1.0f);
	CHECK(gainFor(-1.0f) == 0.0f);
	CHECK(gainFor(std::nanf("")) == 0.0f);
	float prev = 0.0f;
	for (float v = 0.0f; v <= 1.0f; v += 0.05f)
	{
		CHECK(gainFor(v) >= prev);
		prev = gainFor(v);
	}
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
