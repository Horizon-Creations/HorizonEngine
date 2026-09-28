#include "doctest.h"

#include "EditorRewards.h"
#include "EditorApplication.h"   // AppContext, EditorConfig
#include "ImGuiSoftwareRaster.h" // the footer, drawn and looked at
#include "TestFsUtil.h"
#include <HorizonScene/AudioEngine.h>

#include <imgui.h>
#include <imgui_internal.h>      // FindWindowByName: is the tooltip up

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

TEST_CASE("Rewards: a build ends once, as success or as failure")
{
	using End = Feed::BuildEnd;
	Feed f;
	CHECK(f.buildEnded(0, true, false) == End::None);    // nothing built yet
	CHECK(f.buildEnded(1, false, false) == End::None);   // running
	CHECK(f.buildEnded(1, true, false) == End::Failed);
	CHECK(f.buildEnded(1, true, false) == End::None);    // same run, re-read
	CHECK(f.buildEnded(2, true, true) == End::Succeeded);
	CHECK(f.buildEnded(2, true, true) == End::None);
	CHECK(f.buildEnded(3, true, false) == End::Failed);
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

TEST_CASE("Rewards: the failed-build tone keeps the tone gap, and only saves the save gap")
{
	Feed f;
	// A failed build right after a heard save: inside the tone gap, silent.
	CHECK(f.take(Moment::Saved, 1, 10.0, 1, true).sound);
	CHECK_FALSE(f.takeTone(Tone::BuildFailed, 11.0));
	// Past the tone gap it plays — the save gap is for save tones only.
	CHECK(f.takeTone(Tone::BuildFailed, 12.5));
	// And it books the tone gap like any other tone.
	CHECK_FALSE(f.take(Moment::AssetsImported, 1, 13.0, 2, true).sound);
	CHECK(f.take(Moment::AssetsImported, 1, 15.0, 3, true).sound);
	// A save tone still waits for its own gap from the last SAVE tone.
	CHECK_FALSE(f.takeTone(Tone::SaveTick, 18.0));
	CHECK(f.takeTone(Tone::SaveTick, 30.5));
}

TEST_CASE("Rewards: which tone each moment plays")
{
	CHECK(toneFor(Moment::Saved) == Tone::SaveTick);
	CHECK(toneFor(Moment::BuildSucceeded) == Tone::BuildChime);
	CHECK(toneFor(Moment::AssetsImported) == Tone::ImportPop);
}

TEST_CASE("Rewards: every switch can silence a tone, and the build tones want the editor unfocused")
{
	EditorConfig on;
	on.RewardsEnabled = true;
	on.RewardsSound   = true;
	on.RewardsVolume  = 0.5f;
	const Tone all[] = { Tone::SaveTick, Tone::BuildChime, Tone::BuildFailed, Tone::ImportPop };

	// Everything on, editor in the background, not playing: every tone.
	for (Tone t : all) CHECK(toneWanted(on, t, false, false));
	// With the editor focused only the save and import tones.
	CHECK(toneWanted(on, Tone::SaveTick, false, true));
	CHECK(toneWanted(on, Tone::ImportPop, false, true));
	CHECK_FALSE(toneWanted(on, Tone::BuildChime, false, true));
	CHECK_FALSE(toneWanted(on, Tone::BuildFailed, false, true));
	// During Play none.
	for (Tone t : all) CHECK_FALSE(toneWanted(on, t, true, false));

	// Each of the shared switches silences all of them.
	const auto noneWith = [&](auto change) {
		EditorConfig c = on;
		change(c);
		for (Tone t : all) CHECK_FALSE(toneWanted(c, t, false, false));
	};
	noneWith([](EditorConfig& c) { c.RewardsEnabled = false; });
	noneWith([](EditorConfig& c) { c.RewardsSound = false; });
	noneWith([](EditorConfig& c) { c.EditorSoundsMuted = true; });
	noneWith([](EditorConfig& c) { c.RewardsVolume = 0.0f; });

	// Each tone's own switch silences that tone and no other.
	struct Own { bool EditorConfig::*field; Tone tone; };
	const Own own[] = {
		{ &EditorConfig::RewardsSoundSave,        Tone::SaveTick },
		{ &EditorConfig::RewardsSoundBuild,       Tone::BuildChime },
		{ &EditorConfig::RewardsSoundBuildFailed, Tone::BuildFailed },
		{ &EditorConfig::RewardsSoundImport,      Tone::ImportPop },
	};
	for (const Own& o : own)
	{
		EditorConfig c = on;
		c.*o.field = false;
		for (Tone t : all) CHECK(toneWanted(c, t, false, false) == (t != o.tone));
	}
}

TEST_CASE("Rewards: the defaults hear nothing, and what keeps the UI device open")
{
	// A fresh install: the per-tone switches are on, the sound itself is not.
	const EditorConfig d;
	CHECK_FALSE(d.RewardsSound);
	CHECK_FALSE(d.EditorSoundsMuted);
	CHECK(d.RewardsSoundSave);
	CHECK(d.RewardsSoundBuild);
	CHECK(d.RewardsSoundBuildFailed);
	CHECK(d.RewardsSoundImport);
	CHECK_FALSE(uiSoundPossible(d));
	// The visual cues are on (they were asked for; each can go), the motion
	// follows the system.
	CHECK(d.RewardsCheckMark);
	CHECK(d.RewardsLightEdge);
	CHECK(d.RewardsCounterTick);
	CHECK(d.RewardsTabCheck);
	CHECK(d.RewardsImportHighlight);
	CHECK(d.RewardsStreakTooltip);
	CHECK(d.RewardsReducedMotion == 0);

	EditorConfig c;
	c.RewardsSound = true;
	CHECK(uiSoundPossible(c));
	// Volume does not close the device: dragging through zero must not reopen it.
	c.RewardsVolume = 0.0f;
	CHECK(uiSoundPossible(c));
	c.EditorSoundsMuted = true;
	CHECK_FALSE(uiSoundPossible(c));
	c.EditorSoundsMuted = false;
	c.RewardsEnabled    = false;
	CHECK_FALSE(uiSoundPossible(c));
}

namespace
{
std::vector<int16_t> samplesOf(const std::vector<uint8_t>& pcm)
{
	std::vector<int16_t> s(pcm.size() / 2);
	for (size_t i = 0; i < s.size(); ++i)
		s[i] = static_cast<int16_t>(pcm[i * 2] | (pcm[i * 2 + 1] << 8));
	return s;
}

// The signal's energy at one frequency (Goertzel), for "no bass".
double powerAt(const std::vector<int16_t>& s, int rate, double hz)
{
	const double c = 2.0 * std::cos(6.283185307179586 * hz / rate);
	double s1 = 0.0, s2 = 0.0;
	for (int16_t x : s)
	{
		const double s0 = x + c * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return s1 * s1 + s2 * s2 - c * s1 * s2;
}

int peakOf(const std::vector<int16_t>& s)
{
	int p = 0;
	for (int16_t x : s) p = std::max(p, std::abs(int(x)));
	return p;
}
} // namespace

TEST_CASE("Rewards: every tone is short, soft at both ends, quiet and free of bass")
{
	constexpr int kRate = 44100;
	struct Case { Tone tone; double mainHz; };
	// mainHz: where each tone's energy is — the pop glides, so its middle.
	const Case cases[] = {
		{ Tone::SaveTick, 1318.51 }, { Tone::BuildChime, 880.0 },
		{ Tone::BuildFailed, 1318.51 }, { Tone::ImportPop, 1100.0 },
	};
	int peaks[4] = {};
	for (const Case& c : cases)
	{
		CAPTURE(static_cast<int>(c.tone));
		const std::vector<int16_t> s = samplesOf(tonePcm16(c.tone, kRate));
		REQUIRE(s.size() > static_cast<size_t>(kRate / 50));       // > 20 ms
		CHECK(s.size() <= static_cast<size_t>(kRate * 45 / 100));  // ≤ 0.45 s

		const int peak = peakOf(s);
		peaks[static_cast<int>(c.tone)] = peak;
		// Audible, and at most half scale (−6 dBFS). The chime is the loudest,
		// ≈ −7.7 dBFS where its two notes overlap — topic 75's level, kept.
		CHECK(peak > 32767 / 20);
		CHECK(peak < 32767 / 2);

		// At least 4 ms of fade-in: nothing in the first millisecond comes
		// near the peak, so the voice does not start with a click.
		const std::vector<int16_t> firstMs(s.begin(), s.begin() + kRate / 1000);
		CHECK(std::abs(int(s.front())) < 200);
		CHECK(peakOf(firstMs) < peak / 3);
		CHECK(std::abs(int(s.back())) < 200);

		// No bass: below 600 Hz there is next to nothing against the tone.
		const double main = powerAt(s, kRate, c.mainHz);
		for (double hz : { 60.0, 120.0, 250.0, 400.0 })
		{
			CAPTURE(hz);
			CHECK(powerAt(s, kRate, hz) < main * 0.01);
		}
		CHECK(tonePcm16(c.tone, 0).empty());
	}
	// The one heard most is the quietest; the failure is not louder than
	// the success.
	for (int p : { peaks[1], peaks[2], peaks[3] }) CHECK(peaks[0] < p);
	CHECK(peaks[2] <= peaks[1]);
	// The newer tones land on exactly zero.
	for (Tone t : { Tone::SaveTick, Tone::BuildFailed, Tone::ImportPop })
		CHECK(samplesOf(tonePcm16(t, kRate)).back() == 0);
}

TEST_CASE("Rewards: the tones are the same every time")
{
	constexpr int kRate = 48000;
	for (Tone t : { Tone::SaveTick, Tone::BuildChime, Tone::BuildFailed, Tone::ImportPop })
		CHECK(tonePcm16(t, kRate) == tonePcm16(t, kRate));
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

// ── Routing: the editor's own engine (EditorRewards.h, "Routing") ────────────
// Two device-less engines standing in for the project's and the editor's: a
// tone must reach the second and never the first, survive the stopAll() that
// ends Play, and a mute must close the editor's device.

namespace
{
// Everything AppContext insists on being given (its references have no
// defaults); the rewards only read editorConfig and the two engines.
struct RewardsContextBits
{
	EditorConfig    config;
	bool            vsync = false;
	std::string     backendName = "Software";
	EditorSelection selection;
	std::string     scenePath;
	bool            exitRequested = false, projectLoaded = true;
	bool            refreshPending = false, refreshDone = false;
	int   fpsOffset = 0, fpsCount = 0;
	float fpsAccum = 0.0f, smoothFps = 0.0f;
	std::vector<AppContext::EditorTab> tabs;
	int   activeTab = 0;
	float cbTreeWidth = 200.0f;
	int   hubPreset = 0, hubLang = 0;
	bool  hubFx = false;
	std::string hubCreateError, hubOpenError;
	int   hubRemoveIndex = -1;
	bool  hubRemoveRequested = false;
	std::string dirResult, fileResult;
	bool  dirReady = false, fileReady = false;

	AppContext make(AudioEngine& project, AudioEngine& ui)
	{
		AppContext ctx{
			.editorConfig          = config,
			.vsync                 = vsync,
			.backendName           = backendName,
			.selection             = selection,
			.currentScenePath      = scenePath,
			.exitRequested         = exitRequested,
			.projectLoaded         = projectLoaded,
			.contentRefreshPending = refreshPending,
			.contentRefreshDone    = refreshDone,
			.fpsHistoryOffset      = fpsOffset,
			.fpsAccum              = fpsAccum,
			.fpsAccumCount         = fpsCount,
			.smoothFps             = smoothFps,
			.tabs                  = tabs,
			.activeTab             = activeTab,
			.cbTreeWidth           = cbTreeWidth,
			.hubSelectedPreset     = hubPreset,
			.hubSelectedLang       = hubLang,
			.hubAdvancedShaderFx   = hubFx,
			.hubCreateError        = hubCreateError,
			.hubOpenError          = hubOpenError,
			.hubRemoveIndex        = hubRemoveIndex,
			.hubRemoveRequested    = hubRemoveRequested,
			.pendingDirResult      = dirResult,
			.pendingDirReady       = dirReady,
			.pendingFileResult     = fileResult,
			.pendingFileReady      = fileReady,
		};
		ctx.audioEngine   = &project;
		ctx.uiAudioEngine = &ui;
		return ctx;
	}
};

// The loudest sample the engine's mixer puts out over the next 0.1 s.
float loudestOut(AudioEngine& e)
{
	constexpr uint64_t kFrames = 4800;
	std::vector<float> buf(kFrames * static_cast<size_t>(std::max(e.outputChannels(), 1)));
	const uint64_t got = e.readMixedFrames(buf.data(), kFrames);
	float m = 0.0f;
	for (size_t i = 0; i < got * static_cast<size_t>(e.outputChannels()); ++i)
		m = std::max(m, std::abs(buf[i]));
	return m;
}
} // namespace

TEST_CASE("Rewards: the tones play on the editor's engine, never on the project's")
{
	AudioEngine project, ui;
	REQUIRE(project.init(true));
	REQUIRE(ui.init(true));
	RewardsContextBits bits;
	bits.config.RewardsSound  = true;
	bits.config.RewardsVolume = 1.0f;
	AppContext ctx = bits.make(project, ui);

	for (Tone t : { Tone::SaveTick, Tone::BuildChime, Tone::BuildFailed, Tone::ImportPop })
	{
		CAPTURE(static_cast<int>(t));
		preview(ctx, t);
		project.stopAll();   // what ending Play does to the project's engine
		CHECK(loudestOut(ui) > 0.02f);
		CHECK(loudestOut(project) == 0.0f);
		ui.stopAll();
	}

	// The project's master mute does not reach the editor's tones.
	project.setMasterMuted(true);
	preview(ctx, Tone::BuildChime);
	CHECK(loudestOut(ui) > 0.02f);
	ui.stopAll();
}

TEST_CASE("Rewards: muting or switching sound off closes the editor's device")
{
	AudioEngine project, ui;
	REQUIRE(project.init(true));
	RewardsContextBits bits;
	bits.config.RewardsSound = true;
	AppContext ctx = bits.make(project, ui);

	// Muted: the preview plays nothing and the engine is closed.
	REQUIRE(ui.init(true));
	bits.config.EditorSoundsMuted = true;
	preview(ctx, Tone::BuildChime);
	CHECK_FALSE(ui.isInitialized());

	// Sound off: the per-frame poll closes it (run 0 = no build to report).
	bits.config.EditorSoundsMuted = false;
	REQUIRE(ui.init(true));
	bits.config.RewardsSound = false;
	pollBuild(ctx, 0, false, false, true);
	CHECK_FALSE(ui.isInitialized());

	// The master off closes it too.
	bits.config.RewardsSound = true;
	REQUIRE(ui.init(true));
	bits.config.RewardsEnabled = false;
	pollBuild(ctx, 0, false, false, true);
	CHECK_FALSE(ui.isInitialized());
}

// Not a check: HE_DUMP_REWARD_TONES=<dir> writes the four tones as 44.1 kHz
// mono WAV files there, so a person can listen to them without an editor.
// Nothing happens without the variable.
TEST_CASE("Rewards: HE_DUMP_REWARD_TONES writes the tones for listening")
{
	const char* dir = std::getenv("HE_DUMP_REWARD_TONES");
	if (!dir || !*dir) return;
	constexpr int kRate = 44100;
	const std::pair<Tone, const char*> tones[] = {
		{ Tone::SaveTick, "save_tick.wav" },       { Tone::BuildChime, "build_chime.wav" },
		{ Tone::BuildFailed, "build_failed.wav" }, { Tone::ImportPop, "import_pop.wav" },
	};
	std::filesystem::create_directories(dir);
	for (const auto& [tone, name] : tones)
	{
		const std::vector<uint8_t> pcm = tonePcm16(tone, kRate);
		std::ofstream f(std::filesystem::path(dir) / name, std::ios::binary);
		const auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
		const auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
		f.write("RIFF", 4); u32(36 + static_cast<uint32_t>(pcm.size())); f.write("WAVE", 4);
		f.write("fmt ", 4); u32(16); u16(1); u16(1); u32(kRate); u32(kRate * 2); u16(2); u16(16);
		f.write("data", 4); u32(static_cast<uint32_t>(pcm.size()));
		f.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size()));
		CHECK(f.good());
	}
}

// ── The visual cues (topic 95, step 4) ───────────────────────────────────────
// The clocks behind V1–V5 with a hand clock, the history behind the tooltip
// with string dates, and then the footer itself drawn on the CPU: green where
// the check, the edge and a ticking number should be, and none where a switch
// or reduced motion says no.

TEST_CASE("Rewards: the check is written over kCheckDrawSec, whole at once when reduced")
{
	CHECK(checkStroke(0.0, false) == 0.0f);
	CHECK(checkStroke(-1.0, false) == 0.0f);
	float last = 0.0f;
	for (double t = 0.01; t < kCheckDrawSec; t += 0.01)
	{
		const float s = checkStroke(t, false);
		CHECK(s > last);
		CHECK(s < 1.0f);
		last = s;
	}
	CHECK(checkStroke(kCheckDrawSec, false) == doctest::Approx(1.0f));
	CHECK(checkStroke(5.0, false) == doctest::Approx(1.0f));
	// Reduced motion: no writing, the whole check from the first frame.
	CHECK(checkStroke(0.0, true) == 1.0f);
}

TEST_CASE("Rewards: the check's polyline grows along its two strokes and stays in its box")
{
	CHECK(checkPolyline(0.0f, 0, 0, 10).empty());
	CHECK(checkPolyline(1.0f, 0, 0, 0).empty());

	const std::vector<Pt> whole = checkPolyline(1.0f, 100, 50, 10);
	REQUIRE(whole.size() == 3);
	// Down to the knee, then up to the top right (y grows downwards).
	CHECK(whole[1].y > whole[0].y);
	CHECK(whole[2].y < whole[1].y);
	CHECK(whole[2].x > whole[1].x);
	for (const Pt& p : whole)
	{
		CHECK(p.x >= 100.0f); CHECK(p.x <= 110.0f);
		CHECK(p.y >= 50.0f);  CHECK(p.y <= 60.0f);
	}
	// A little of it: two points, both on the first stroke.
	const std::vector<Pt> start = checkPolyline(0.1f, 100, 50, 10);
	REQUIRE(start.size() == 2);
	CHECK(start[1].x < whole[1].x);
	CHECK(start[1].y < whole[1].y);
	// Most of it: three points, the last one short of the end.
	const std::vector<Pt> most = checkPolyline(0.8f, 100, 50, 10);
	REQUIRE(most.size() == 3);
	CHECK(most[2].x < whole[2].x);
}

TEST_CASE("Rewards: the light edge is one pulse that spreads and is gone at kEdgeSec")
{
	float lastSpread = 0.0f, peak = 0.0f;
	for (double t = 0.0; t < kEdgeSec; t += 0.02)
	{
		const Edge e = edgeAt(t, false);
		CHECK(e.spread >= lastSpread);
		lastSpread = e.spread;
		peak = std::max(peak, e.alpha);
		CHECK(e.alpha <= 0.6f);   // soft: a 1-px line, never full strength
	}
	CHECK(peak > 0.3f);
	CHECK(edgeAt(kEdgeSec, false).alpha == 0.0f);
	CHECK(edgeAt(10.0, false).alpha == 0.0f);
	// Reduced motion: no edge at all.
	for (double t = 0.0; t < kEdgeSec; t += 0.05) CHECK(edgeAt(t, true).alpha == 0.0f);
}

TEST_CASE("Rewards: a merged moment does not restart the line's own clock")
{
	Feed f;
	REQUIRE(f.push(Moment::Saved, 1, 10.0, 1));
	CHECK(f.look(10.0).lineAge == doctest::Approx(0.0));
	// A second save while "Saved" shows: held anew, but the check and the
	// edge must not start over — their clock runs on.
	REQUIRE(f.push(Moment::Saved, 1, 10.5, 2));
	CHECK(f.look(10.6).lineAge == doctest::Approx(0.6));
	CHECK(f.look(10.6).strength == 1.0f);   // …while the hold did restart
	// A higher moment is a new line: a new check, a new pulse.
	REQUIRE(f.push(Moment::BuildSucceeded, 1, 11.0, 3));
	CHECK(f.look(11.1).lineAge == doctest::Approx(0.1));
}

TEST_CASE("Rewards: a counter ticks on a rise, after the line, and never on a fall")
{
	CounterTick c;
	// The first value is where it starts, not a rise.
	c.observe(3);
	c.start(1.0);
	CHECK(c.look(1.1, false).glow == 0.0f);
	CHECK(c.look(1.1, false).shown == 3);

	// A build: 3 → 4. Until start() (the line is still up) the old number shows.
	c.observe(4);
	CHECK(c.look(2.0, false).shown == 3);
	CHECK(c.look(2.0, false).glow == 0.0f);
	c.start(2.0);
	const CounterTick::Look mid = c.look(2.1, false);
	CHECK(mid.shown == 4);
	CHECK(mid.old == 3);
	CHECK(mid.glow > 0.5f);
	CHECK(mid.roll > 0.0f);
	CHECK(mid.roll < 1.0f);
	CHECK(c.look(2.0 + kRollSec, false).roll == doctest::Approx(1.0f));
	CHECK(c.look(2.0 + kTickSec, false).glow == 0.0f);
	// Reduced motion: it lights up, it does not roll.
	CHECK(c.look(2.1, true).roll == 1.0f);
	CHECK(c.look(2.1, true).glow > 0.5f);

	// Two rises while the line is up: rolls from what the screen last showed.
	c.observe(5);
	c.observe(6);
	c.start(5.0);
	CHECK(c.look(5.1, false).old == 4);
	CHECK(c.look(5.1, false).shown == 6);

	// Midnight: 6 → 0 is not a tick, and neither is the first build after it
	// a roll from 6.
	c.observe(0);
	c.start(7.0);
	CHECK(c.look(7.1, false).glow == 0.0f);
	CHECK(c.look(7.1, false).shown == 0);
	c.observe(1);
	c.start(8.0);
	CHECK(c.look(8.1, false).old == 0);
	CHECK(c.look(8.1, false).shown == 1);
}

TEST_CASE("Rewards: a tab gets its check for a save, not for an undo back to clean")
{
	SaveMarks m;
	// First sight: whatever the tab is, no check.
	CHECK(m.update("a", true, 1.0, -1.0) < 0.0);
	// Saved: dirty → clean right after a Saved moment.
	CHECK(m.update("a", false, 2.0, 1.9) == doctest::Approx(0.0));
	CHECK(m.update("a", false, 2.3, 1.9) == doctest::Approx(0.3));
	// …and gone after kTabCheckSec.
	CHECK(m.update("a", false, 2.0 + kTabCheckSec, 1.9) < 0.0);

	// Undo back to clean with no save near it: no check.
	CHECK(m.update("a", true, 3.0, 1.9) < 0.0);
	CHECK(m.update("a", false, 5.0, 1.9) < 0.0);

	// Edited again while the check shows: the marker is back at once.
	CHECK(m.update("a", true, 6.0, 1.9) < 0.0);
	CHECK(m.update("a", false, 6.1, 6.05) >= 0.0);
	CHECK(m.update("a", true, 6.2, 6.05) < 0.0);
	CHECK(m.update("a", false, 6.3, 6.05) >= 0.0);   // still within the match window of that save

	// A clean tab stays unmarked through a save of another tab.
	CHECK(m.update("b", false, 7.0, -1.0) < 0.0);
	CHECK(m.update("b", false, 7.1, 7.05) < 0.0);
	// Another tab saved in the same batch gets its own.
	CHECK(m.update("c", true, 7.0, -1.0) < 0.0);
	CHECK(m.update("c", false, 7.1, 7.05) >= 0.0);
}

TEST_CASE("Rewards: an import's new and rewritten files are told from the rest")
{
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / "he_rewards_snapshot";
	he_test::removeAllQuiet(dir);
	fs::create_directories(dir / "sub");
	const auto write = [&](const char* name, const char* text)
	{
		std::ofstream(dir / name, std::ios::binary) << text;
	};
	write("kept.hasset", "k");
	write("rewritten.hasset", "r");
	// Written times on some file systems are coarse; set them apart by hand.
	const auto past = fs::last_write_time(dir / "kept.hasset") - std::chrono::hours(1);
	fs::last_write_time(dir / "kept.hasset", past);
	fs::last_write_time(dir / "rewritten.hasset", past);

	const DirSnapshot before = snapshotDir(dir.string());
	CHECK(before.files.size() == 2);   // the sub-folder is not a file

	write("new.hasset", "n");
	write("rewritten.hasset", "r2");
	fs::last_write_time(dir / "rewritten.hasset", past + std::chrono::minutes(30));

	std::vector<std::string> changed = changedSince(before);
	std::sort(changed.begin(), changed.end());
	REQUIRE(changed.size() == 2);
	CHECK(changed[0] == normalPath((dir / "new.hasset").string()));
	CHECK(changed[1] == normalPath((dir / "rewritten.hasset").string()));

	// Not taken (the highlight was off): nothing, and nothing listed.
	CHECK(changedSince(DirSnapshot{}).empty());
	// A folder that is not there lists as empty, not as an error.
	CHECK(snapshotDir((dir / "missing").string()).files.empty());
	he_test::removeAllQuiet(dir);
}

TEST_CASE("Rewards: a fresh import's frame counts from the tile's first frame on screen")
{
	FreshImports f;
	CHECK(f.empty());
	CHECK(f.strength("/c/a.hasset", 0.0) == 0.0f);

	f.mark({ "/c/a.hasset", "/c/b.hasset" }, 10.0);
	CHECK_FALSE(f.empty());
	CHECK(f.strength("/c/other.hasset", 10.0) == 0.0f);
	// a is on screen at once: full for the hold, fading, gone.
	CHECK(f.strength("/c/a.hasset", 10.0) == 1.0f);
	CHECK(f.strength("/c/a.hasset", 10.0 + kImportHoldSec) == 1.0f);
	const float fading = f.strength("/c/a.hasset", 10.0 + kImportHoldSec + kImportFadeSec * 0.5);
	CHECK(fading > 0.0f);
	CHECK(fading < 1.0f);
	CHECK(f.strength("/c/a.hasset", 10.0 + kImportHoldSec + kImportFadeSec) == 0.0f);
	CHECK(f.strength("/c/a.hasset", 10.0) == 0.0f);   // forgotten, not restarted

	// b is scrolled to five seconds later: it still gets its whole frame.
	CHECK(f.strength("/c/b.hasset", 15.0) == 1.0f);
	CHECK(f.strength("/c/b.hasset", 15.0 + kImportHoldSec) == 1.0f);

	// Something that never came on screen within the window gets none.
	f.mark({ "/c/late.hasset" }, 20.0);
	CHECK(f.strength("/c/late.hasset", 20.0 + kImportWindowSec + 1.0) == 0.0f);

	// …and prune() lets go of it without anyone asking, so the Content
	// Browser's per-tile check goes back to one branch.
	FreshImports g;
	g.mark({ "/c/unseen.hasset" }, 30.0);
	g.prune(30.0 + kImportWindowSec * 0.5);
	CHECK_FALSE(g.empty());
	g.prune(30.0 + kImportWindowSec + 1.0);
	CHECK(g.empty());
}

TEST_CASE("Rewards: the recent days are kept, written and read back")
{
	Tally t;
	CHECK(recordUse(t, "2026-09-20", false));
	CHECK(recordUse(t, "2026-09-20", true));
	CHECK(recordUse(t, "2026-09-20", true));
	CHECK(recordUse(t, "2026-09-22", true));
	REQUIRE(t.recent.size() == 2);
	CHECK(t.recent[0].day == "2026-09-20");
	CHECK(t.recent[0].builds == 2);
	CHECK(t.recent[1].day == "2026-09-22");
	CHECK(t.recent[1].builds == 1);

	const std::string text = formatRecent(t.recent);
	CHECK(text == "2026-09-20:2,2026-09-22:1");
	const std::vector<DayUse> back = parseRecent(text);
	REQUIRE(back.size() == 2);
	CHECK(back[1].day == "2026-09-22");
	CHECK(back[1].builds == 1);

	// A hand-edited list: what does not parse, or goes backwards, is dropped.
	const std::vector<DayUse> odd =
		parseRecent("2026-09-20:2,garbage,2026-02-30:1,2026-09-19:4,2026-09-21:x,2026-09-23:0,");
	REQUIRE(odd.size() == 2);
	CHECK(odd[0].day == "2026-09-20");
	CHECK(odd[1].day == "2026-09-23");
	CHECK(parseRecent("").empty());

	// Only the last kRecentDays days with a moment are kept.
	Tally w;
	for (int d = 1; d <= 20; ++d)
	{
		char day[16];
		std::snprintf(day, sizeof day, "2026-08-%02d", d);
		recordUse(w, day, false);
	}
	REQUIRE(w.recent.size() == static_cast<size_t>(kRecentDays));
	CHECK(w.recent.back().day == "2026-08-20");
	CHECK(w.recent.front().day == "2026-08-14");
	CHECK(w.streakDays == 20);   // the streak is not limited by the history
}

TEST_CASE("Rewards: a tally from before the history is seeded from its streak")
{
	Tally t;
	t.day = "2026-03-02"; t.buildsToday = 4; t.streakDays = 3;
	seedRecent(t);
	REQUIRE(t.recent.size() == 3);
	CHECK(t.recent[0].day == "2026-02-28");
	CHECK(t.recent[1].day == "2026-03-01");
	CHECK(t.recent[2].day == "2026-03-02");
	CHECK(t.recent[2].builds == 4);
	CHECK(t.recent[0].builds == 0);
	// Seeded once: a history that exists is left alone.
	t.streakDays = 30;
	seedRecent(t);
	CHECK(t.recent.size() == 3);
	// A long streak fills the week and no more; nothing counted, nothing seeded.
	Tally l; l.day = "2026-03-02"; l.streakDays = 40;
	seedRecent(l);
	CHECK(l.recent.size() == static_cast<size_t>(kRecentDays));
	Tally none;
	seedRecent(none);
	CHECK(none.recent.empty());
}

TEST_CASE("Rewards: weekdays and the tooltip's seven days")
{
	CHECK(weekdayOf("2026-09-26") == 5);   // a Saturday
	CHECK(weekdayOf("2000-01-01") == 5);   // a Saturday
	CHECK(weekdayOf("2024-02-29") == 3);   // a Thursday
	CHECK(weekdayOf("2026-09-28") == 0);   // a Monday
	CHECK(weekdayOf("2026-02-30") == -1);

	Tally t;
	recordUse(t, "2026-09-20", false);
	recordUse(t, "2026-09-24", true);
	recordUse(t, "2026-09-26", true);
	recordUse(t, "2026-09-26", true);
	const std::vector<DayCell> week = recentDays(t, "2026-09-26");
	REQUIRE(week.size() == 7);
	CHECK(week.front().day == "2026-09-20");
	CHECK(week.back().day == "2026-09-26");
	CHECK(week.back().weekday == 5);
	CHECK(week[0].used);
	CHECK_FALSE(week[1].used);
	CHECK(week[4].used);
	CHECK(week[4].builds == 1);
	CHECK(week[6].used);
	CHECK(week[6].builds == 2);
	int used = 0;
	for (const DayCell& c : week) used += c.used ? 1 : 0;
	CHECK(used == 3);

	// Nothing counted: seven empty days, no error.
	const std::vector<DayCell> empty = recentDays(Tally{}, "2026-09-26");
	REQUIRE(empty.size() == 7);
	for (const DayCell& c : empty) CHECK_FALSE(c.used);
	CHECK(recentDays(t, "not a day").empty());
}

TEST_CASE("Rewards: the counters as numbers say what the text says")
{
	Tally t; t.day = "2026-03-01"; t.buildsToday = 1; t.streakDays = 5;
	const Progress p = progressOf(t, "2026-03-01");
	CHECK(p.any);
	CHECK(p.builds == 1);
	CHECK(p.streak == 5);
	CHECK(progressText(t, "2026-03-01") == buildsPhrase(1) + " · " + streakPhrase(5));
	CHECK(buildsPhrase(1) == "1 build today");
	CHECK(buildsPhrase(0) == "0 builds today");
	// Yesterday's streak still shows, today's builds are 0; the day after, gone.
	CHECK(progressOf(t, "2026-03-02").builds == 0);
	CHECK(progressOf(t, "2026-03-02").streak == 5);
	CHECK(progressOf(t, "2026-03-03").streak == 0);
	CHECK_FALSE(progressOf(Tally{}, "2026-03-01").any);
}

namespace
{
bool s_fakeReduce = false;
bool fakeReduceQuery() { return s_fakeReduce; }
} // namespace

TEST_CASE("Rewards: Follow System follows the system, Off never reduces")
{
	AudioEngine project, ui;
	RewardsContextBits bits;
	AppContext ctx = bits.make(project, ui);

	setSystemMotionQuery(nullptr);
	CHECK_FALSE(systemReducesMotion());
	CHECK_FALSE(reducedMotion(ctx));

	s_fakeReduce = true;
	setSystemMotionQuery(&fakeReduceQuery);
	CHECK(systemReducesMotion());
	bits.config.RewardsReducedMotion = 0;   // Follow System
	CHECK(reducedMotion(ctx));
	bits.config.RewardsReducedMotion = 1;   // Off: the full motion whatever the system says
	CHECK_FALSE(reducedMotion(ctx));

	// Setting the query forgets the cached answer.
	s_fakeReduce = false;
	setSystemMotionQuery(&fakeReduceQuery);
	bits.config.RewardsReducedMotion = 0;
	CHECK_FALSE(reducedMotion(ctx));
	setSystemMotionQuery(nullptr);
}

// ── The footer, drawn ────────────────────────────────────────────────────────
namespace
{
// Room above the footer for the tooltip, as in the editor.
constexpr int   kShotW = 900, kShotH = 320;
constexpr float kFootY = 280.0f, kFootH = 28.0f;

struct FooterHarness
{
	FooterHarness()
	{
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(float(kShotW), float(kShotH));
		io.DeltaTime   = 1.0f / 60.0f;
		io.IniFilename = nullptr;
		io.LogFilename = nullptr;
		io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
		io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
#ifdef HE_EDITOR_DEPS_DIR
		const std::string font = std::string(HE_EDITOR_DEPS_DIR) + "/Fonts/Roboto_Condensed-Bold.ttf";
		if (std::FILE* f = std::fopen(font.c_str(), "rb"))
		{
			std::fclose(f);
			ImFontConfig cfg;
			cfg.OversampleH = 2;
			cfg.OversampleV = 2;
			io.FontDefault = io.Fonts->AddFontFromFileTTF(font.c_str(), 15.0f, &cfg);
		}
#endif
		setSystemMotionQuery(nullptr);
	}
	~FooterHarness()
	{
		setSystemMotionQuery(nullptr);
		ImGui::DestroyContext();
	}
};

// One frame of a footer-shaped window at the bottom: an item on the left, like
// Undo/Redo, then the status label. `during` runs inside the frame (fire()
// wants the frame's clock), `dt` is the time since the last frame. Only a
// frame that is looked at is rasterised (`shot`): on a Debug build the CPU
// raster is most of this test's time.
he_ui::Image footerFrame(AppContext& ctx, float dt = 1.0f / 60.0f,
                         const std::function<void()>& during = {}, bool* tooltip = nullptr,
                         bool shot = false)
{
	ImGui::GetIO().DeltaTime = dt;
	ImGui::NewFrame();
	ImGui::SetNextWindowPos(ImVec2(0.0f, kFootY));
	ImGui::SetNextWindowSize(ImVec2(float(kShotW), kFootH));
	ImGui::Begin("##footer", nullptr,
	             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
	             ImGuiWindowFlags_NoSavedSettings);
	if (during) during();
	ImGui::TextDisabled("Undo");
	drawFooterStatus(ctx, "Ready");
	ImGui::End();
	if (tooltip)
	{
		const ImGuiWindow* tip = ImGui::FindWindowByName("##Tooltip_00");
		*tooltip = tip && tip->Active;
	}
	ImGui::Render();
	return shot ? he_ui::rasterize(ImGui::GetDrawData(), kShotW, kShotH) : he_ui::Image{};
}

he_ui::Image footerShot(AppContext& ctx, bool* tooltip = nullptr)
{
	return footerFrame(ctx, 1.0f / 60.0f, {}, tooltip, true);
}

// Pixels in the box that read as the "done" green (on the dark footer).
int greenIn(const he_ui::Image& img, int x0, int y0, int x1, int y1)
{
	int n = 0;
	for (int y = std::max(y0, 0); y < std::min(y1, img.height); ++y)
		for (int x = std::max(x0, 0); x < std::min(x1, img.width); ++x)
		{
			uint8_t r, g, b, a;
			img.pixel(x, y, r, g, b, a);
			if (g > 60 && g > r + 30 && g > b + 30) ++n;
		}
	return n;
}

void dumpShot(const he_ui::Image& img, const char* name)
{
	if (const char* d = std::getenv("HE_UI_DUMP_DIR"); d && *d)
		he_ui::writeBmp(img, std::string(d) + "/" + name + ".bmp");
}

// Let whatever the footer shows run out, so the next fire() is a new line.
void settle(AppContext& ctx)
{
	for (int i = 0; i < 4; ++i) footerFrame(ctx, 1.0f);
}

// A moment, then `frames` frames of 1/60 s: the picture at that point.
he_ui::Image shotAfter(AppContext& ctx, Moment m, int frames)
{
	footerFrame(ctx, 1.0f / 60.0f, [&] { fire(ctx, m); });
	for (int i = 1; i < frames; ++i) footerFrame(ctx);
	return footerShot(ctx);
}

// Where each cue lands: the check in the strip left of the centred line, the
// edge on the footer's top row, the counters around the middle.
int checkInk(const he_ui::Image& img)
{
	return greenIn(img, kShotW / 2 - 70, int(kFootY) + 2, kShotW / 2 - 20, int(kFootY + kFootH));
}
int edgeInk(const he_ui::Image& img)
{
	return greenIn(img, 0, int(kFootY) - 1, kShotW, int(kFootY) + 1);
}
int middleInk(const he_ui::Image& img)
{
	return greenIn(img, kShotW / 2 - 150, int(kFootY) + 2, kShotW / 2 + 150, int(kFootY + kFootH));
}
} // namespace

TEST_CASE("Rewards: the footer draws the check and the light edge, each on its own switch")
{
	FooterHarness h;
	AudioEngine project, ui;
	RewardsContextBits bits;
	AppContext ctx = bits.make(project, ui);
	settle(ctx);

	// Everything on, a fifth of a second in: the check is written, the edge
	// is mid-pulse.
	he_ui::Image all = shotAfter(ctx, Moment::Saved, 12);
	dumpShot(all, "rewards_footer_saved");
	CHECK(checkInk(all) > 8);
	CHECK(edgeInk(all) > 20);
	settle(ctx);

	bits.config.RewardsCheckMark = false;
	he_ui::Image noCheck = shotAfter(ctx, Moment::Saved, 12);
	CHECK(checkInk(noCheck) == 0);
	CHECK(edgeInk(noCheck) > 20);
	bits.config.RewardsCheckMark = true;
	settle(ctx);

	bits.config.RewardsLightEdge = false;
	he_ui::Image noEdge = shotAfter(ctx, Moment::Saved, 12);
	CHECK(checkInk(noEdge) > 8);
	CHECK(edgeInk(noEdge) == 0);
	bits.config.RewardsLightEdge = true;
	settle(ctx);

	// Visual Cues off: no line, so no check and no edge either.
	bits.config.RewardsVisual = false;
	he_ui::Image noLine = shotAfter(ctx, Moment::Saved, 12);
	CHECK(checkInk(noLine) == 0);
	CHECK(edgeInk(noLine) == 0);
	bits.config.RewardsVisual = true;
	settle(ctx);

	// One frame in, the check is only begun…
	const int begun = checkInk(shotAfter(ctx, Moment::Saved, 1));
	settle(ctx);
	// …but with reduced motion it is whole from the first frame, and there is
	// no edge.
	s_fakeReduce = true;
	setSystemMotionQuery(&fakeReduceQuery);
	he_ui::Image reduced = shotAfter(ctx, Moment::Saved, 1);
	dumpShot(reduced, "rewards_footer_reduced");
	CHECK(checkInk(reduced) > begun);
	CHECK(edgeInk(reduced) == 0);
	settle(ctx);
	// Reduced Motion "Off" overrides the system: the edge is back.
	bits.config.RewardsReducedMotion = 1;
	CHECK(edgeInk(shotAfter(ctx, Moment::Saved, 12)) > 20);
	bits.config.RewardsReducedMotion = 0;
	s_fakeReduce = false;
	setSystemMotionQuery(nullptr);
	settle(ctx);
}

TEST_CASE("Rewards: a counter that went up lights up, and only with its switch on")
{
	FooterHarness h;
	AudioEngine project, ui;
	RewardsContextBits bits;
	// No line: the tick has nothing to wait for and starts on the next frame.
	bits.config.RewardsVisual = false;
	AppContext ctx = bits.make(project, ui);
	settle(ctx);
	footerFrame(ctx, 1.0f / 60.0f, [&] { fire(ctx, Moment::Saved); });   // counted at all
	settle(ctx);
	CHECK(middleInk(footerShot(ctx)) == 0);   // the counters, grey

	// Mid-roll, for the picture; then past the roll with the glow still on.
	dumpShot(shotAfter(ctx, Moment::BuildSucceeded, 6), "rewards_footer_tick_rolling");
	for (int i = 0; i < 60; ++i) footerFrame(ctx);
	he_ui::Image ticking = shotAfter(ctx, Moment::BuildSucceeded, 16);
	dumpShot(ticking, "rewards_footer_tick");
	CHECK(middleInk(ticking) > 8);
	// Gone after kTickSec.
	for (int i = 0; i < 60; ++i) footerFrame(ctx);
	CHECK(middleInk(footerShot(ctx)) == 0);

	bits.config.RewardsCounterTick = false;
	CHECK(middleInk(shotAfter(ctx, Moment::BuildSucceeded, 6)) == 0);
	bits.config.RewardsCounterTick = true;
	settle(ctx);
}

TEST_CASE("Rewards: the recent days show on hover of the counters, and only then")
{
	FooterHarness h;
	AudioEngine project, ui;
	RewardsContextBits bits;
	AppContext ctx = bits.make(project, ui);
	settle(ctx);
	footerFrame(ctx, 1.0f / 60.0f, [&] { fire(ctx, Moment::Saved); });   // something counted
	settle(ctx);

	const auto hoverFor = [&](float x, float y, int frames)
	{
		ImGui::GetIO().AddMousePosEvent(x, y);
		bool up = false;
		for (int i = 1; i < frames; ++i) footerFrame(ctx);
		he_ui::Image img = footerShot(ctx, &up);
		return std::make_pair(up, img);
	};
	const float midY = kFootY + kFootH * 0.5f;

	// Nobody hovering: never, however long.
	CHECK_FALSE(hoverFor(-100.0f, -100.0f, 60).first);
	// Over the counters, resting: the tooltip.
	const auto [up, shot] = hoverFor(kShotW * 0.5f, midY, 60);
	dumpShot(shot, "rewards_footer_recent_days");
	CHECK(up);
	// Over the footer but beside the label: none.
	CHECK_FALSE(hoverFor(40.0f + kShotW * 0.8f, midY, 60).first);
	// Switched off: none.
	bits.config.RewardsStreakTooltip = false;
	CHECK_FALSE(hoverFor(kShotW * 0.5f, midY, 60).first);
	bits.config.RewardsStreakTooltip = true;
	// Show Progress off: no counters, nothing to hover.
	bits.config.RewardsShowProgress = false;
	CHECK_FALSE(hoverFor(kShotW * 0.5f, midY, 60).first);
	bits.config.RewardsShowProgress = true;
	hoverFor(-100.0f, -100.0f, 2);
}
