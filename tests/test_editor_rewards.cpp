#include "doctest.h"

#include "EditorRewards.h"
#include "EditorApplication.h"   // AppContext, EditorConfig
#include "EditorToolbar.h"       // V9: the compile readout's strip
#include "HcGraphHost.h"         // V9: the failed node's halo
#include "ImGuiSoftwareRaster.h" // the footer, drawn and looked at
#include "NotificationBar.h"     // V8: the real bell and its ring
#include "NotificationStore.h"
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
#include <thread>
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
	// Topic 140: Build = Commit = Tour > Import > Compile > Save.
	CHECK(rankOf(Moment::Committed) == rankOf(Moment::BuildSucceeded));
	CHECK(rankOf(Moment::TourFinished) == rankOf(Moment::BuildSucceeded));
	CHECK(rankOf(Moment::AssetsImported) > rankOf(Moment::CompiledClean));
	CHECK(rankOf(Moment::CompiledClean) > rankOf(Moment::Saved));

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
	CHECK(lineFor(Moment::CompiledClean, 1) == "Compiles clean");
	CHECK(lineFor(Moment::Committed, kSyncCommit) == "Committed");
	CHECK(lineFor(Moment::Committed, kSyncPush) == "Pushed");
	CHECK(lineFor(Moment::Committed, kSyncCommit | kSyncPush) == "Committed and pushed");
	CHECK(lineFor(Moment::TourFinished, 1) == "Tutorial complete");
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
	CHECK(hasTone(Moment::Saved));
	CHECK(hasTone(Moment::BuildSucceeded));
	CHECK(hasTone(Moment::AssetsImported));
	// Topic 140, step 3: the new moments have tones of their own now (step 2
	// pinned them toneless; this is where that changes, on purpose).
	CHECK(hasTone(Moment::CompiledClean));
	CHECK(hasTone(Moment::Committed));
	CHECK(hasTone(Moment::TourFinished));
	CHECK(toneFor(Moment::CompiledClean) == Tone::CompileClean);
	CHECK(toneFor(Moment::Committed) == Tone::Commit);
	CHECK(toneFor(Moment::TourFinished) == Tone::TourDone);
}

namespace
{
// Every tone, in Tone order — the editor's cache is indexed by it.
const Tone kAllTones[] = { Tone::SaveTick,     Tone::BuildChime,    Tone::BuildFailed,
                           Tone::ImportPop,    Tone::CompileClean,  Tone::CompileFailed,
                           Tone::Commit,       Tone::TourDone,      Tone::Problem };
static_assert(std::size(kAllTones) == static_cast<size_t>(kToneCount));
} // namespace

TEST_CASE("Rewards: every switch can silence a tone, and the build tones want the editor unfocused")
{
	EditorConfig on;
	on.RewardsEnabled = true;
	on.RewardsSound   = true;
	on.RewardsVolume  = 0.5f;
	const auto& all = kAllTones;

	// Everything on, editor in the background, not playing: every tone.
	for (Tone t : all) CHECK(toneWanted(on, t, false, false));
	// With the editor focused: save, import, both compile tones and the tour —
	// not the build tones, the commit tone or the problem tone.
	CHECK(toneWanted(on, Tone::SaveTick, false, true));
	CHECK(toneWanted(on, Tone::ImportPop, false, true));
	CHECK(toneWanted(on, Tone::CompileClean, false, true));
	CHECK(toneWanted(on, Tone::CompileFailed, false, true));
	CHECK(toneWanted(on, Tone::TourDone, false, true));
	CHECK_FALSE(toneWanted(on, Tone::BuildChime, false, true));
	CHECK_FALSE(toneWanted(on, Tone::BuildFailed, false, true));
	CHECK_FALSE(toneWanted(on, Tone::Commit, false, true));
	CHECK_FALSE(toneWanted(on, Tone::Problem, false, true));
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
		{ &EditorConfig::RewardsSoundCompile,       Tone::CompileClean },
		{ &EditorConfig::RewardsSoundCompileFailed, Tone::CompileFailed },
		{ &EditorConfig::RewardsSoundCommit,        Tone::Commit },
		{ &EditorConfig::RewardsSoundTutorial,      Tone::TourDone },
		{ &EditorConfig::RewardsSoundProblem,       Tone::Problem },
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
	CHECK(d.RewardsSoundCompile);
	CHECK(d.RewardsSoundCompileFailed);
	CHECK(d.RewardsSoundCommit);
	CHECK(d.RewardsSoundTutorial);
	CHECK(d.RewardsSoundProblem);
	CHECK(d.RewardsProblemPulse);
	CHECK_FALSE(uiSoundPossible(d));
	// …so no tone at all, in front of the editor or behind it: a headless run
	// or a test with the default config never asks for a device.
	for (Tone t : kAllTones)
	{
		CHECK_FALSE(toneWanted(d, t, false, false));
		CHECK_FALSE(toneWanted(d, t, false, true));
	}
	// The visual cues are on (they were asked for; each can go), the motion
	// follows the system.
	CHECK(d.RewardsCheckMark);
	CHECK(d.RewardsLightEdge);
	CHECK(d.RewardsCounterTick);
	CHECK(d.RewardsTabCheck);
	CHECK(d.RewardsImportHighlight);
	CHECK(d.RewardsStreakTooltip);
	CHECK(d.RewardsReducedMotion == 0);
	// Topic 140's moments are on, like the three before them.
	CHECK(d.RewardsMomentCompile);
	CHECK(d.RewardsMomentCommit);
	CHECK(d.RewardsMomentTutorial);

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
		{ Tone::CompileClean, 1318.51 }, { Tone::CompileFailed, 1318.51 },
		{ Tone::Commit, 1318.51 }, { Tone::TourDone, 880.0 },
		{ Tone::Problem, 987.77 },
	};
	int peaks[kToneCount] = {};
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
	const auto peakOfTone = [&](Tone t) { return peaks[static_cast<int>(t)]; };
	for (Tone t : kAllTones)
		if (t != Tone::SaveTick) CHECK(peakOfTone(Tone::SaveTick) < peakOfTone(t));
	CHECK(peaks[2] <= peaks[1]);
	// Topic 140's tones: all below the chime, each failure below its success.
	for (Tone t : { Tone::CompileClean, Tone::CompileFailed, Tone::Commit, Tone::TourDone,
	                Tone::Problem })
	{
		CAPTURE(static_cast<int>(t));
		CHECK(peakOfTone(t) < peakOfTone(Tone::BuildChime));
	}
	CHECK(peakOfTone(Tone::CompileFailed) <= peakOfTone(Tone::CompileClean));
	// The answer to a click is shorter than the chime that answers a build.
	CHECK(samplesOf(tonePcm16(Tone::CompileClean, kRate)).size()
	      < samplesOf(tonePcm16(Tone::BuildChime, kRate)).size());
	// The newer tones land on exactly zero.
	for (Tone t : kAllTones)
		if (t != Tone::BuildChime) CHECK(samplesOf(tonePcm16(t, kRate)).back() == 0);
}

TEST_CASE("Rewards: the tones are the same every time")
{
	constexpr int kRate = 48000;
	for (Tone t : kAllTones)
		CHECK(tonePcm16(t, kRate) == tonePcm16(t, kRate));
}

TEST_CASE("Rewards: compile tones step up and down, the problem tone stays on B5")
{
	// Which way each goes, measured: the first 60 ms against the rest.
	constexpr int kRate = 44100;
	const auto split = [&](Tone t, double hzA, double hzB)
	{
		const std::vector<int16_t> s = samplesOf(tonePcm16(t, kRate));
		const size_t cut = kRate * 6 / 100;
		const std::vector<int16_t> head(s.begin(), s.begin() + cut), tail(s.begin() + cut, s.end());
		// Head: more of A than of B; tail: more of B than of A.
		return powerAt(head, kRate, hzA) > powerAt(head, kRate, hzB)
		    && powerAt(tail, kRate, hzB) > powerAt(tail, kRate, hzA);
	};
	CHECK(split(Tone::CompileClean, 1108.73, 1318.51));    // C#6 → E6, up
	CHECK(split(Tone::CompileFailed, 1318.51, 1108.73));   // E6 → C#6, down
	const std::vector<int16_t> p = samplesOf(tonePcm16(Tone::Problem, kRate));
	CHECK(powerAt(p, kRate, 987.77) > 50.0 * powerAt(p, kRate, 1318.51));
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

	for (Tone t : kAllTones)
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
		{ Tone::CompileClean, "compile_clean.wav" }, { Tone::CompileFailed, "compile_failed.wav" },
		{ Tone::Commit, "commit.wav" },            { Tone::TourDone, "tour_done.wav" },
		{ Tone::Problem, "problem.wav" },
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

	// The commits field (topic 140): read with and without it, and a bad one
	// drops the entry like any other hand edit.
	const std::vector<DayUse> mixed =
		parseRecent("2026-09-20:2,2026-09-21:0:3,2026-09-22:1:,2026-09-23:1:x,2026-09-24:4:1:9,2026-09-25:1:2");
	REQUIRE(mixed.size() == 3);
	CHECK(mixed[0].day == "2026-09-20");
	CHECK(mixed[0].commits == 0);
	CHECK(mixed[1].day == "2026-09-21");
	CHECK(mixed[1].builds == 0);
	CHECK(mixed[1].commits == 3);
	CHECK(mixed[2].day == "2026-09-25");
	CHECK(mixed[2].commits == 2);
	// Written only where there were commits, so a day without one reads in an
	// editor from before the field.
	CHECK(formatRecent(mixed) == "2026-09-20:2,2026-09-21:0:3,2026-09-25:1:2");

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
	CHECK(t.recent[2].commits == 0);
	Tally c;
	c.day = "2026-03-02"; c.commitsToday = 2; c.streakDays = 1;
	seedRecent(c);
	REQUIRE(c.recent.size() == 1);
	CHECK(c.recent[0].commits == 2);
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

// ── Topic 140: compile, commit and tour moments ──────────────────────────────

TEST_CASE("Rewards: a commit then a push merge into one line with both flags")
{
	Feed f;
	CHECK(f.take(Moment::Committed, kSyncCommit, 0.0, 1, false).shown);
	CHECK(f.look(0.0).line == "Committed");
	// The push lands while the line still shows: same kind, flags OR-ed —
	// never "added" into a third value.
	const Feed::Taken push = f.take(Moment::Committed, kSyncPush, 0.5, 2, false);
	CHECK(push.shown);
	CHECK(f.look(0.5).line == "Committed and pushed");
	CHECK(f.take(Moment::Committed, kSyncPush, 0.8, 3, false).shown);
	CHECK(f.look(0.8).line == "Committed and pushed");
	// A merge holds the line anew, as for any other kind.
	CHECK(f.look(0.8 + kHoldSec).active);
}

TEST_CASE("Rewards: equal rank, other kind: the newer moment replaces the line")
{
	Feed f;
	CHECK(f.take(Moment::BuildSucceeded, 1, 0.0, 1, false).shown);
	const Feed::Taken c = f.take(Moment::Committed, kSyncCommit, 0.3, 2, false);
	CHECK(c.shown);
	CHECK(f.look(0.3).line == "Committed");
	const Feed::Taken t = f.take(Moment::TourFinished, 1, 0.6, 3, false);
	CHECK(t.shown);
	CHECK(f.look(0.6).line == "Tutorial complete");
	// A compile under a tour line: lower, counted, not shown.
	CHECK_FALSE(f.take(Moment::CompiledClean, 1, 0.9, 4, false).shown);
	CHECK(f.look(0.9).line == "Tutorial complete");
	// A compile over a save line: higher, replaces it.
	Feed g;
	CHECK(g.take(Moment::Saved, 1, 0.0, 1, false).shown);
	CHECK(g.take(Moment::CompiledClean, 1, 0.2, 2, false).shown);
	CHECK(g.look(0.2).line == "Compiles clean");
	CHECK_FALSE(g.take(Moment::Saved, 1, 0.4, 3, false).shown);
}

TEST_CASE("Rewards: topic 140's moments sound with their own tone and book the gap")
{
	// Step 2 pinned these toneless; step 3 gave them tones. A caller that says
	// "may sound" now gets one…
	for (Moment m : { Moment::CompiledClean, Moment::Committed, Moment::TourFinished })
	{
		Feed g;
		CAPTURE(static_cast<int>(m));
		const Feed::Taken r = g.take(m, 1, 0.0, 1, true);
		CHECK(r.shown);
		CHECK(r.sound);
		// …and it books the tone gap like any other.
		CHECK_FALSE(g.takeTone(Tone::SaveTick, 0.1));
		CHECK(g.takeTone(Tone::SaveTick, kToneGapSec));
	}
	// A caller that says no (its switch, focus, Play) books nothing.
	Feed q;
	CHECK_FALSE(q.take(Moment::Committed, kSyncCommit, 0.0, 1, false).sound);
	CHECK(q.takeTone(Tone::SaveTick, 0.1));
}

TEST_CASE("Rewards: each new moment has its own switch, the old ones have none")
{
	EditorConfig c;
	for (Moment m : { Moment::Saved, Moment::BuildSucceeded, Moment::AssetsImported,
	                  Moment::CompiledClean, Moment::Committed, Moment::TourFinished })
		CHECK(momentWanted(c, m));
	c.RewardsMomentCompile = false;
	CHECK_FALSE(momentWanted(c, Moment::CompiledClean));
	CHECK(momentWanted(c, Moment::Committed));
	c.RewardsMomentCommit = false;
	CHECK_FALSE(momentWanted(c, Moment::Committed));
	CHECK(momentWanted(c, Moment::TourFinished));
	c.RewardsMomentTutorial = false;
	CHECK_FALSE(momentWanted(c, Moment::TourFinished));
	// The topic-75 moments answer to Visual/Sound, not to these.
	CHECK(momentWanted(c, Moment::Saved));
	CHECK(momentWanted(c, Moment::BuildSucceeded));
	CHECK(momentWanted(c, Moment::AssetsImported));
}

TEST_CASE("Rewards: compile, commit and tour count the day; only commits count commits")
{
	Tally t;
	// A clean compile is the day's first moment: the day is used, no build.
	CHECK(recordMoment(t, "2026-10-05", Moment::CompiledClean));
	CHECK(t.day == "2026-10-05");
	CHECK(t.streakDays == 1);
	CHECK(t.buildsToday == 0);
	CHECK(t.commitsToday == 0);
	// The day is already counted: a compile or a tour changes nothing more.
	CHECK_FALSE(recordMoment(t, "2026-10-05", Moment::CompiledClean));
	CHECK_FALSE(recordMoment(t, "2026-10-05", Moment::TourFinished));
	// A push alone is no commit.
	CHECK_FALSE(recordMoment(t, "2026-10-05", Moment::Committed, kSyncPush));
	CHECK(t.commitsToday == 0);
	// A commit (with or without its push) is one.
	CHECK(recordMoment(t, "2026-10-05", Moment::Committed, kSyncCommit));
	CHECK(recordMoment(t, "2026-10-05", Moment::Committed, kSyncCommit | kSyncPush));
	CHECK(t.commitsToday == 2);
	CHECK(t.buildsToday == 0);
	REQUIRE_FALSE(t.recent.empty());
	CHECK(t.recent.back().commits == 2);
	CHECK(t.recent.back().builds == 0);
	// Builds still count as before, beside the commits.
	CHECK(recordMoment(t, "2026-10-05", Moment::BuildSucceeded));
	CHECK(t.buildsToday == 1);
	CHECK(t.recent.back().builds == 1);
	CHECK(t.recent.back().commits == 2);
	CHECK(formatRecent(t.recent) == "2026-10-05:1:2");

	// The next day: a tour moment carries the streak; the commits start over.
	CHECK(recordMoment(t, "2026-10-06", Moment::TourFinished));
	CHECK(t.streakDays == 2);
	CHECK(t.commitsToday == 0);
	CHECK(t.buildsToday == 0);
	// The footer text does not grow a commits part — the tooltip only.
	CHECK(progressText(t, "2026-10-06") == "0 builds today · 2 days in a row");

	const std::vector<DayCell> week = recentDays(t, "2026-10-06");
	REQUIRE(week.size() == static_cast<size_t>(kRecentDays));
	CHECK(week[5].day == "2026-10-05");
	CHECK(week[5].commits == 2);
	CHECK(week[6].used);
	CHECK(week[6].commits == 0);

	CHECK(commitsPhrase(0).empty());
	CHECK(commitsPhrase(1) == "1 commit today");
	CHECK(commitsPhrase(4) == "4 commits today");
}

TEST_CASE("Rewards: SyncWatch fires once for a request that went through")
{
	SyncWatch w;
	// Nothing asked: nothing, however idle and however it says "Committed.".
	CHECK(w.poll(true, "", "Committed.") == 0);

	w.requested(kSyncCommit);
	CHECK(w.armed());
	// Still busy before the pump: not judged yet, and still armed.
	CHECK(w.poll(false, "", "") == 0);
	CHECK(w.armed());
	// Idle before the pump: judged once.
	CHECK(w.poll(true, "", "Committed.") == kSyncCommit);
	CHECK_FALSE(w.armed());
	CHECK(w.poll(true, "", "Committed.") == 0);

	// A failure: lastError set — no moment, and disarmed.
	w.requested(kSyncPush);
	CHECK(w.poll(true, "push rejected", "") == 0);
	CHECK_FALSE(w.armed());
	// A failure whose error a later status refresh cleared again: lastInfo is
	// what the error had wiped, so still no moment.
	w.requested(kSyncCommit);
	CHECK(w.poll(true, "", "") == 0);

	// Commit then push before the first was judged: one answer with both.
	w.requested(kSyncCommit);
	w.requested(kSyncPush);
	CHECK(w.poll(true, "", "Pushed.") == (kSyncCommit | kSyncPush));
	// Nonsense flags are not passed through.
	w.requested(8);
	CHECK_FALSE(w.armed());
}

TEST_CASE("Rewards: a moment's own switch hides it from the footer")
{
	FooterHarness h;
	AudioEngine project, ui;
	RewardsContextBits bits;
	AppContext ctx = bits.make(project, ui);
	settle(ctx);

	// On (the default): "Compiles clean" shows, with its light edge.
	CHECK(edgeInk(shotAfter(ctx, Moment::CompiledClean, 12)) > 20);
	settle(ctx);
	CHECK(edgeInk(shotAfter(ctx, Moment::TourFinished, 12)) > 20);
	settle(ctx);
	footerFrame(ctx, 1.0f / 60.0f, [&] { fire(ctx, Moment::Committed, kSyncCommit | kSyncPush); });
	for (int i = 1; i < 12; ++i) footerFrame(ctx);
	he_ui::Image committed = footerShot(ctx);
	dumpShot(committed, "rewards_footer_committed");
	CHECK(edgeInk(committed) > 20);
	settle(ctx);

	// Off: nothing shows for that moment…
	bits.config.RewardsMomentCompile = false;
	CHECK(edgeInk(shotAfter(ctx, Moment::CompiledClean, 12)) == 0);
	// …and a lower moment right after is not outranked by a hidden line.
	CHECK(edgeInk(shotAfter(ctx, Moment::Saved, 12)) > 20);
	bits.config.RewardsMomentCompile = true;
	settle(ctx);
	bits.config.RewardsMomentCommit = false;
	CHECK(edgeInk(shotAfter(ctx, Moment::Committed, 12)) == 0);
	bits.config.RewardsMomentCommit = true;
	settle(ctx);
	bits.config.RewardsMomentTutorial = false;
	CHECK(edgeInk(shotAfter(ctx, Moment::TourFinished, 12)) == 0);
	bits.config.RewardsMomentTutorial = true;
	settle(ctx);
}

TEST_CASE("Rewards: post() fires on the next pollBuild, not before")
{
	FooterHarness h;
	AudioEngine project, ui;
	RewardsContextBits bits;
	AppContext ctx = bits.make(project, ui);
	settle(ctx);

	// Posted in one frame (a hook without an AppContext): nothing yet.
	footerFrame(ctx, 1.0f / 60.0f, [&] { post(Moment::CompiledClean); });
	for (int i = 0; i < 11; ++i) footerFrame(ctx);
	CHECK(edgeInk(footerShot(ctx)) == 0);
	// The next frame's pollBuild fires it — in that frame's clock.
	footerFrame(ctx, 1.0f / 60.0f, [&] { pollBuild(ctx, 0, false, false, true); });
	for (int i = 1; i < 12; ++i) footerFrame(ctx);
	CHECK(edgeInk(footerShot(ctx)) > 20);
	settle(ctx);
	// Fired once: a second pollBuild has nothing left.
	footerFrame(ctx, 1.0f / 60.0f, [&] { pollBuild(ctx, 0, false, false, true); });
	for (int i = 1; i < 12; ++i) footerFrame(ctx);
	CHECK(edgeInk(footerShot(ctx)) == 0);

	// With the master off a posted moment is gone, not kept for later.
	bits.config.RewardsEnabled = false;
	post(Moment::CompiledClean);
	footerFrame(ctx, 1.0f / 60.0f, [&] { pollBuild(ctx, 0, false, false, true); });
	bits.config.RewardsEnabled = true;
	footerFrame(ctx, 1.0f / 60.0f, [&] { pollBuild(ctx, 0, false, false, true); });
	for (int i = 1; i < 12; ++i) footerFrame(ctx);
	CHECK(edgeInk(footerShot(ctx)) == 0);
	settle(ctx);
}

// ── Topic 140, step 3: the clocks of V8/V9 and the problem tone ──────────────
namespace HC = HorizonCode;
// The ring and the pulse with a hand clock, ProblemWatch's edge and its gap,
// then the editor side: the real footer bell over a real NotificationStore,
// the tones that have no moment, and the switches V9 reads from pollBuild.

TEST_CASE("Rewards: the bell's ring widens and fades within kPulseSec, only fades when reduced")
{
	CHECK(ringAt(-0.01, false).alpha == 0.0f);
	CHECK(ringAt(kPulseSec, false).alpha == 0.0f);
	CHECK(ringAt(0.0, false).alpha == 0.0f);   // rises in, no hard start
	const Ring a = ringAt(0.1, false), b = ringAt(0.4, false);
	CHECK(a.alpha > 0.5f);
	CHECK(a.alpha < 1.0f);                      // never full strength
	CHECK(b.grow > a.grow);                     // it moves out…
	CHECK(b.alpha < a.alpha);                   // …and fades as it goes
	CHECK(ringAt(kPulseSec - 0.001, false).alpha < 0.01f);
	// Reduced: the same fade, no movement.
	for (double t : { 0.05, 0.1, 0.3, 0.5 })
	{
		CAPTURE(t);
		CHECK(ringAt(t, true).grow == 0.0f);
		CHECK(ringAt(t, true).alpha == ringAt(t, false).alpha);
	}
}

TEST_CASE("Rewards: the failed node's pulse is one bump, back to nothing at kPulseSec")
{
	CHECK(pulseAt(-0.1) == 0.0f);
	CHECK(pulseAt(0.0) == 0.0f);
	CHECK(pulseAt(kPulseSec * 0.2) == doctest::Approx(1.0f));
	CHECK(pulseAt(kPulseSec) == 0.0f);
	CHECK(pulseAt(5.0) == 0.0f);
	// Down after the top, never up again: one pulse, not a blink.
	float last = 1.0f;
	for (double t = kPulseSec * 0.2; t < kPulseSec; t += 0.01)
	{
		const float p = pulseAt(t);
		CHECK(p <= last + 1e-6f);
		last = p;
	}
}

TEST_CASE("Rewards: ProblemWatch fires once per new problem, never for the first it sees")
{
	ProblemWatch w;
	CHECK_FALSE(w.observe(500));   // problems from before: where it starts
	CHECK_FALSE(w.observe(500));
	CHECK(w.observe(600));         // a new one (or the last one again, restamped)
	CHECK_FALSE(w.observe(600));
	CHECK_FALSE(w.observe(0));     // the store cleared: where it starts now
	CHECK(w.observe(10));

	ProblemWatch none;
	CHECK_FALSE(none.observe(0));
	CHECK(none.observe(1));

	// Its own gap, on top of the tone gap.
	ProblemWatch g;
	CHECK(g.toneDue(0.0));
	g.tonePlayed(5.0);
	CHECK_FALSE(g.toneDue(6.0));
	CHECK_FALSE(g.toneDue(5.0 + kProblemToneGapSec - 0.01));
	CHECK(g.toneDue(5.0 + kProblemToneGapSec));
	CHECK(g.toneDue(1.0));         // a clock behind the last tone is a new clock
}

namespace
{
constexpr float kBellX = 800.0f;

// A post a real millisecond after the last: the bell's edge is the store's
// millisecond stamp, and a test posts faster than that (see problemSeen).
void postProblem(HE::Ed::NotificationStore& store, HE::Ed::NoteLevel level, std::string text)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(2));
	store.post(level, std::move(text));
}

// One frame of the footer with the real bell at kBellX. `during` runs inside
// it, before the bell (pollBuild, a post), like the editor's frame.
he_ui::Image bellFrame(AppContext& ctx, float dt = 1.0f / 60.0f,
                       const std::function<void()>& during = {}, bool shot = false)
{
	ImGui::GetIO().DeltaTime = dt;
	ImGui::NewFrame();
	ImGui::SetNextWindowPos(ImVec2(0.0f, kFootY));
	ImGui::SetNextWindowSize(ImVec2(float(kShotW), kFootH));
	ImGui::Begin("##footer", nullptr,
	             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
	             ImGuiWindowFlags_NoSavedSettings);
	if (during) during();
	ImGui::SetCursorScreenPos(ImVec2(kBellX, kFootY + 6.0f));
	NotificationBar::DrawFooter(ctx);
	ImGui::End();
	ImGui::Render();
	return shot ? he_ui::rasterize(ImGui::GetDrawData(), kShotW, kShotH) : he_ui::Image{};
}

// The problem red, in the strip LEFT of the bell — the bell and its count
// are red themselves while a problem is unread; only the ring reaches here.
int ringInk(const he_ui::Image& img)
{
	int n = 0;
	for (int y = int(kFootY); y < int(kFootY + kFootH); ++y)
		for (int x = int(kBellX) - 16; x < int(kBellX) - 1; ++x)
		{
			uint8_t r, g, b, a;
			img.pixel(x, y, r, g, b, a);
			if (r > 90 && r > g + 40 && r > b + 40) ++n;
		}
	return n;
}
} // namespace

TEST_CASE("ui shot: a new problem rings the footer bell once")
{
	FooterHarness h;
	AudioEngine project, ui;
	RewardsContextBits bits;
	AppContext ctx = bits.make(project, ui);
	HE::Ed::NotificationStore store;
	ctx.notifications = &store;

	// Nothing new: no ring. (A problem from before the footer was first
	// drawn is ProblemWatch's case above — the watch here is the editor's
	// one, and an earlier test may already have started it.)
	bellFrame(ctx, 1.0f);
	bellFrame(ctx, 1.0f);
	for (int i = 0; i < 5; ++i) bellFrame(ctx);
	CHECK(ringInk(bellFrame(ctx, 1.0f / 60.0f, {}, true)) == 0);

	// A new one: the ring, a few frames in — and gone after kPulseSec.
	bellFrame(ctx, 1.0f / 60.0f, [&] { postProblem(store, HE::Ed::NoteLevel::Problem, "New problem."); });
	for (int i = 0; i < 5; ++i) bellFrame(ctx);
	const he_ui::Image ringing = bellFrame(ctx, 1.0f / 60.0f, {}, true);
	dumpShot(ringing, "rewards_bell_problem_ring");
	CHECK(ringInk(ringing) > 6);
	bellFrame(ctx, 1.0f);
	CHECK(ringInk(bellFrame(ctx, 1.0f / 60.0f, {}, true)) == 0);

	// A warning is not a problem: no ring.
	bellFrame(ctx, 1.0f / 60.0f, [&] { postProblem(store, HE::Ed::NoteLevel::Warning, "Just a warning."); });
	for (int i = 0; i < 5; ++i) bellFrame(ctx);
	CHECK(ringInk(bellFrame(ctx, 1.0f / 60.0f, {}, true)) == 0);

	// Problem Pulse off: none — and switching it back on does not ring for
	// the problem that came while it was off.
	bits.config.RewardsProblemPulse = false;
	bellFrame(ctx, 1.0f / 60.0f, [&] { postProblem(store, HE::Ed::NoteLevel::Problem, "Quiet problem."); });
	for (int i = 0; i < 5; ++i) bellFrame(ctx);
	CHECK(ringInk(bellFrame(ctx, 1.0f / 60.0f, {}, true)) == 0);
	bits.config.RewardsProblemPulse = true;
	bellFrame(ctx, 1.0f);
	for (int i = 0; i < 5; ++i) bellFrame(ctx);
	CHECK(ringInk(bellFrame(ctx, 1.0f / 60.0f, {}, true)) == 0);

	// The master off: none either.
	bits.config.RewardsEnabled = false;
	bellFrame(ctx, 1.0f / 60.0f, [&] { postProblem(store, HE::Ed::NoteLevel::Problem, "Off problem."); });
	for (int i = 0; i < 5; ++i) bellFrame(ctx);
	CHECK(ringInk(bellFrame(ctx, 1.0f / 60.0f, {}, true)) == 0);
	bits.config.RewardsEnabled = true;
	bellFrame(ctx, 1.0f);
}

TEST_CASE("Rewards: the problem tone plays in the background, once per kProblemToneGapSec")
{
	FooterHarness h;
	AudioEngine project, ui;
	REQUIRE(project.init(true));
	REQUIRE(ui.init(true));
	RewardsContextBits bits;
	bits.config.RewardsSound  = true;
	bits.config.RewardsVolume = 1.0f;
	AppContext ctx = bits.make(project, ui);
	HE::Ed::NotificationStore store;
	ctx.notifications = &store;
	const auto background = [&] { pollBuild(ctx, 0, false, false, /*appFocused=*/false); };
	const auto foreground = [&] { pollBuild(ctx, 0, false, false, /*appFocused=*/true); };
	const auto problem = [&](const char* text)
	{
		return [&, text] { background(); postProblem(store, HE::Ed::NoteLevel::Problem, text); };
	};

	// Far past any tone an earlier test booked on the shared Feed.
	bellFrame(ctx, 1000.0f, background);   // empty store: where the watch starts
	bellFrame(ctx, 1.0f, background);
	ui.stopAll();

	// A new problem with the editor in the background: heard.
	bellFrame(ctx, 1.0f / 60.0f, problem("First."));
	bellFrame(ctx, 1.0f / 60.0f, background);
	CHECK(loudestOut(ui) > 0.02f);
	CHECK(loudestOut(project) == 0.0f);
	ui.stopAll();

	// Another one 5 s later: the ring, but no second tone inside the gap.
	bellFrame(ctx, 5.0f, background);
	bellFrame(ctx, 1.0f / 60.0f, problem("Second."));
	bellFrame(ctx, 1.0f / 60.0f, background);
	CHECK(loudestOut(ui) == 0.0f);

	// Past the gap: heard again.
	bellFrame(ctx, float(kProblemToneGapSec), background);
	bellFrame(ctx, 1.0f / 60.0f, problem("Third."));
	bellFrame(ctx, 1.0f / 60.0f, background);
	CHECK(loudestOut(ui) > 0.02f);
	ui.stopAll();

	// In front of the editor: the bell rings, the speakers stay quiet — and a
	// tone that did not play books no gap.
	bellFrame(ctx, float(kProblemToneGapSec), foreground);
	bellFrame(ctx, 1.0f / 60.0f, [&] { foreground(); postProblem(store, HE::Ed::NoteLevel::Problem, "Fourth."); });
	bellFrame(ctx, 1.0f / 60.0f, foreground);
	CHECK(loudestOut(ui) == 0.0f);
	bellFrame(ctx, 3.0f, background);
	bellFrame(ctx, 1.0f / 60.0f, problem("Fifth."));
	bellFrame(ctx, 1.0f / 60.0f, background);
	CHECK(loudestOut(ui) > 0.02f);
	ui.stopAll();

	// Its own switch.
	bits.config.RewardsSoundProblem = false;
	bellFrame(ctx, float(kProblemToneGapSec), background);
	bellFrame(ctx, 1.0f / 60.0f, problem("Sixth."));
	bellFrame(ctx, 1.0f / 60.0f, background);
	CHECK(loudestOut(ui) == 0.0f);
}

TEST_CASE("Rewards: a failed compile sounds without a moment, posted ones on the next pollBuild")
{
	FooterHarness h;
	AudioEngine project, ui;
	REQUIRE(project.init(true));
	REQUIRE(ui.init(true));
	RewardsContextBits bits;
	bits.config.RewardsSound  = true;
	bits.config.RewardsVolume = 1.0f;
	AppContext ctx = bits.make(project, ui);
	const auto focused = [&] { pollBuild(ctx, 0, false, false, true); };
	// Far past any tone an earlier test booked on the shared Feed.
	footerFrame(ctx, 1000.0f, focused);
	footerFrame(ctx, 3.0f, focused);
	ui.stopAll();

	// Direct (the widget graph): heard with the editor focused, no line.
	footerFrame(ctx, 1.0f / 60.0f, [&] { focused(); sound(ctx, Tone::CompileFailed); });
	CHECK(loudestOut(ui) > 0.02f);
	ui.stopAll();
	for (int i = 0; i < 12; ++i) footerFrame(ctx);
	CHECK(edgeInk(footerShot(ctx)) == 0);

	// Posted (the class graph has no AppContext): nothing until the next
	// pollBuild, then heard.
	footerFrame(ctx, 3.0f, focused);
	footerFrame(ctx, 1.0f / 60.0f, [&] { postSound(Tone::CompileFailed); });
	CHECK(loudestOut(ui) == 0.0f);
	footerFrame(ctx, 1.0f / 60.0f, focused);
	CHECK(loudestOut(ui) > 0.02f);
	ui.stopAll();

	// It keeps the tone gap: right after another tone, silent.
	footerFrame(ctx, 3.0f, focused);
	footerFrame(ctx, 1.0f / 60.0f, [&] { focused(); fire(ctx, Moment::CompiledClean); });
	CHECK(loudestOut(ui) > 0.02f);   // the clean compile's own tone
	ui.stopAll();
	footerFrame(ctx, 0.5f, [&] { focused(); sound(ctx, Tone::CompileFailed); });
	CHECK(loudestOut(ui) == 0.0f);

	// Its own switch.
	bits.config.RewardsSoundCompileFailed = false;
	footerFrame(ctx, 3.0f, [&] { focused(); sound(ctx, Tone::CompileFailed); });
	CHECK(loudestOut(ui) == 0.0f);
	settle(ctx);
}

TEST_CASE("Rewards: no device, no sound — nothing opens, nothing hangs, nothing plays")
{
	// What a headless run is: the default config (Sound off) and, in the
	// worst case, no UI engine at all.
	FooterHarness h;
	AudioEngine project, ui;
	RewardsContextBits bits;
	AppContext ctx = bits.make(project, ui);
	HE::Ed::NotificationStore store;
	ctx.notifications = &store;

	// Defaults: a whole session of moments, failures and problems never
	// opens the editor's device.
	for (int i = 0; i < 3; ++i)
		bellFrame(ctx, 3.0f, [&]
		{
			pollBuild(ctx, 0, false, false, false);
			fire(ctx, Moment::CompiledClean);
			sound(ctx, Tone::CompileFailed);
			postSound(Tone::Problem);
			postProblem(store, HE::Ed::NoteLevel::Problem, "Problem " + std::to_string(i));
		});
	pollBuild(ctx, 7, true, false, false);   // a failed build
	CHECK_FALSE(ui.isInitialized());

	// No engine at all, sound on: every entry point is a no-op, not a crash.
	bits.config.RewardsSound = true;
	ctx.uiAudioEngine = nullptr;
	bellFrame(ctx, 3.0f, [&]
	{
		pollBuild(ctx, 0, false, false, false);
		for (Tone t : kAllTones) preview(ctx, t);
		sound(ctx, Tone::CompileFailed);
		fire(ctx, Moment::Committed, kSyncCommit);
		postProblem(store, HE::Ed::NoteLevel::Problem, "No engine.");
	});
	pollBuild(ctx, 8, true, false, false);
	CHECK(true);   // reaching here is the check
	settle(ctx);
}

TEST_CASE("Rewards: V9 follows the switches pollBuild saw, and reduced motion")
{
	FooterHarness h;
	AudioEngine project, ui;
	RewardsContextBits bits;
	AppContext ctx = bits.make(project, ui);
	const auto poll = [&] { pollBuild(ctx, 0, false, false, true); };

	poll();
	// The check is written over kCheckDrawSec, like V1.
	CHECK(compileCheck(0.0) == 0.0f);
	CHECK(compileCheck(kCheckDrawSec * 0.5) > 0.0f);
	CHECK(compileCheck(kCheckDrawSec * 0.5) < 1.0f);
	CHECK(compileCheck(3.0) == 1.0f);
	CHECK(errorPulse(kPulseSec * 0.2) == doctest::Approx(1.0f));

	// Check Mark off, or Visual Cues off, or the master off: the static icon.
	for (bool EditorConfig::*f : { &EditorConfig::RewardsCheckMark, &EditorConfig::RewardsVisual,
	                               &EditorConfig::RewardsEnabled })
	{
		bits.config.*f = false;
		poll();
		CHECK(compileCheck(3.0) < 0.0f);
		bits.config.*f = true;
		poll();
		CHECK(compileCheck(3.0) == 1.0f);
	}
	// Problem Pulse off, or the master off: no node pulse.
	bits.config.RewardsProblemPulse = false;
	poll();
	CHECK(errorPulse(kPulseSec * 0.2) == 0.0f);
	bits.config.RewardsProblemPulse = true;
	bits.config.RewardsEnabled      = false;
	poll();
	CHECK(errorPulse(kPulseSec * 0.2) == 0.0f);
	bits.config.RewardsEnabled = true;

	// Reduced motion: the check is whole at once; the pulse is a colour and stays.
	s_fakeReduce = true;
	setSystemMotionQuery(&fakeReduceQuery);
	poll();
	CHECK(compileCheck(0.0) == 1.0f);
	CHECK(errorPulse(kPulseSec * 0.2) == doctest::Approx(1.0f));
	setSystemMotionQuery(nullptr);
	s_fakeReduce = false;
	poll();
}

TEST_CASE("ui shot: the compile readout writes its check")
{
	FooterHarness h;
	AudioEngine project, ui;
	RewardsContextBits bits;
	AppContext ctx = bits.make(project, ui);
	constexpr int W = 420, H = 60;

	// The panels' strip, as LevelScriptPanel / UIEditorPanel draw it.
	const auto strip = [&](double age, bool useCheck)
	{
		ImGui::GetIO().DisplaySize = ImVec2(float(W), float(H));
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImVec2(float(W), float(H)));
		ImGui::Begin("##graph", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
		pollBuild(ctx, 0, false, false, true);
		{
			EditorToolbar::Bar bar;
			bar.group();
			const float stroke = useCheck ? compileCheck(age) : -1.0f;
			if (stroke >= 0.0f)
			{
				const ImVec2 c = bar.readout([](ImDrawList*, const ImVec2&, float, ImU32) {},
				                             "compiles clean — 120 lines of C++",
				                             EditorToolbar::kGood);
				const float s = bar.iconSize();
				drawCheckMark(c.x - s * 0.5f, c.y - s * 0.5f, s, stroke, 1.0f);
			}
			else
				bar.readout(EditorToolbar::iconCheck, "compiles clean — 120 lines of C++",
				            EditorToolbar::kGood);
			bar.endGroup();
		}
		ImGui::End();
		ImGui::Render();
		return he_ui::rasterize(ImGui::GetDrawData(), W, H);
	};
	// The icon slot: the left end of the first well.
	const auto slotInk = [&](const he_ui::Image& img)
	{
		int n = 0;
		for (int y = 0; y < H; ++y)
			for (int x = 0; x < 40; ++x)
			{
				uint8_t r, g, b, a;
				img.pixel(x, y, r, g, b, a);
				if (g > 120 && g > r + 40 && g > b + 30) ++n;
			}
		return n;
	};

	const he_ui::Image half = strip(kCheckDrawSec * 0.4, true);
	const he_ui::Image whole = strip(1.0, true);
	dumpShot(half, "rewards_compile_readout_writing");
	dumpShot(whole, "rewards_compile_readout_written");
	CHECK(slotInk(half) > 0);
	CHECK(slotInk(whole) > slotInk(half));   // more of it written
	// Check Mark off: the static icon, still in the slot.
	bits.config.RewardsCheckMark = false;
	CHECK(slotInk(strip(1.0, true)) > 0);
	bits.config.RewardsCheckMark = true;
	ImGui::GetIO().DisplaySize = ImVec2(float(kShotW), float(kShotH));
}

TEST_CASE("ui shot: a failed compile's node halo brightens once")
{
	FooterHarness h;
	constexpr int W = 360, H = 200;
	HC::Graph graph;
	GraphEditor::State ge;
	ge.pan  = ImVec2(0.0f, 0.0f);
	ge.zoom = 1.0f;
	int selected = 0;
	const int bad = HcGraphHost::addNode(graph, HC::NodeType::Branch, ImVec2(60.0f, 60.0f), 0);
	const int ok  = HcGraphHost::addNode(graph, HC::NodeType::Sequence, ImVec2(60.0f, 400.0f), 0);
	REQUIRE(bad != 0);

	HcGraphHost::Host host;
	host.graph        = &graph;
	host.ge           = &ge;
	host.selectedNode = &selected;
	host.title        = [](const HC::Node&) { return std::string("Branch"); };
	host.errorNode    = bad;

	// The colour the halo gets: the plain error red, a paler red at the top of
	// the pulse — the same hue family, never another state's colour.
	host.errorPulse = 0.0f;
	{
		GraphEditor::Model m = HcGraphHost::buildModel(host);
		CHECK(m.nodeOutline(bad) == IM_COL32(230, 70, 70, 255));
		CHECK(m.nodeOutline(ok) == 0u);
	}
	host.errorPulse = 1.0f;
	{
		GraphEditor::Model m = HcGraphHost::buildModel(host);
		const ImU32 c = m.nodeOutline(bad);
		CHECK(c == IM_COL32(255, 190, 180, 255));
		CHECK(m.nodeOutline(ok) == 0u);
	}

	// And on the canvas, mid-pulse.
	const auto shoot = [&](float pulse)
	{
		host.errorPulse = pulse;
		GraphEditor::Model m = HcGraphHost::buildModel(host);
		he_ui::Image img;
		ImGui::GetIO().DisplaySize = ImVec2(float(W), float(H));
		for (int f = 0; f < 3; ++f)
		{
			ImGui::NewFrame();
			ImGui::SetNextWindowPos(ImVec2(0, 0));
			ImGui::SetNextWindowSize(ImVec2(float(W), float(H)));
			ImGui::Begin("##canvas", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
			GraphEditor::draw("##ge", m, ge, ImVec2(float(W), float(H)));
			ImGui::End();
			ImGui::Render();
		}
		img = he_ui::rasterize(ImGui::GetDrawData(), W, H);
		return img;
	};
	// Pale-red pixels: only the pulse's halo has them.
	const auto pale = [&](const he_ui::Image& img)
	{
		int n = 0;
		for (int y = 0; y < H; ++y)
			for (int x = 0; x < W; ++x)
			{
				uint8_t r, g, b, a;
				img.pixel(x, y, r, g, b, a);
				if (r > 220 && g > 140 && g < 215 && b > 130 && b < 205 && r > g + 30) ++n;
			}
		return n;
	};
	const he_ui::Image top  = shoot(1.0f);
	const he_ui::Image rest = shoot(0.0f);
	dumpShot(top, "rewards_error_node_pulse");
	dumpShot(rest, "rewards_error_node_rest");
	CHECK(pale(top) > 10);
	CHECK(pale(rest) == 0);
	ImGui::GetIO().DisplaySize = ImVec2(float(kShotW), float(kShotH));
}

// ── Drag and drop cues (EditorDragCues.h, topic 140 step 4) ──────────────────

TEST_CASE("Rewards: the drag cues are shorter and quieter than the tick")
{
	constexpr int kRate = 44100;
	const int tickPeak = peakOf(samplesOf(saveTickPcm16(kRate)));
	struct Case { HE::Ed::DragCue cue; double mainHz; };
	using C = HE::Ed::DragCue;
	const Case cases[] = { { C::Pickup, 1500.0 }, { C::OverValid, 2217.46 },
	                       { C::OverInvalid, 987.77 }, { C::Drop, 1318.51 },
	                       { C::Cancel, 1500.0 } };
	static_assert(std::size(cases) == static_cast<size_t>(HE::Ed::kDragCueCount));
	for (const Case& c : cases)
	{
		CAPTURE(static_cast<int>(c.cue));
		const std::vector<int16_t> s = samplesOf(dragCuePcm16(c.cue, kRate));
		REQUIRE_FALSE(s.empty());
		CHECK(s.size() <= static_cast<size_t>(0.06 * kRate));        // ≤ 60 ms
		const int peak = peakOf(s);
		CHECK(peak > 0);
		CHECK(peak <= tickPeak * 6 / 10);                             // ≤ 0.6 × the tick
		CHECK(s.back() == 0);                                         // ends on exactly 0
		const std::vector<int16_t> firstMs(s.begin(), s.begin() + kRate / 1000);
		CHECK(peakOf(firstMs) < peak / 2);                            // eased in
		CHECK(powerAt(s, kRate, c.mainHz) > 10.0 * powerAt(s, kRate, 300.0));   // no bass
	}
	CHECK(dragCuePcm16(C::Drop, 0).empty());
}

TEST_CASE("Rewards: the drag cue gate — hover edges paced, drop and cancel always")
{
	using C = HE::Ed::DragCue;
	HE::Ed::DragCues::Gate g;
	CHECK(g.take(C::Pickup, 1.0));
	CHECK(g.take(C::OverValid, 1.0));          // a different cue in the same instant
	CHECK_FALSE(g.take(C::OverInvalid, 1.02)); // zig-zag over a pin row: within 50 ms
	CHECK(g.take(C::OverInvalid, 1.06));
	CHECK(g.take(C::Drop, 1.061));             // the end of a gesture is never paced away
	CHECK_FALSE(g.take(C::Drop, 1.07));        // …but the same event twice (two canvases) is
	CHECK(g.take(C::Cancel, 1.07));
	CHECK(g.take(C::Drop, 1.2));
	// A clock that went backwards (a new ImGui context) does not block.
	CHECK(g.take(C::Drop, 0.0));
}

TEST_CASE("Rewards: drag cues follow the feedback switches")
{
	EditorConfig c;
	CHECK_FALSE(dragCueWanted(c, false));      // Success Sound starts off
	c.RewardsSound = true;
	CHECK(dragCueWanted(c, false));
	CHECK(c.RewardsSoundDragDrop);             // on by default under Success Sound
	CHECK_FALSE(dragCueWanted(c, true));       // not during Play
	c.RewardsSoundDragDrop = false;
	CHECK_FALSE(dragCueWanted(c, false));
	c.RewardsSoundDragDrop = true;
	c.RewardsVolume = 0.0f;
	CHECK_FALSE(dragCueWanted(c, false));
	c.RewardsVolume = 0.5f;
	c.EditorSoundsMuted = true;
	CHECK_FALSE(dragCueWanted(c, false));
	c.EditorSoundsMuted = false;
	c.RewardsEnabled = false;
	CHECK_FALSE(dragCueWanted(c, false));
}

namespace
{
std::vector<HE::Ed::DragCue> s_probed;
void probeCue(HE::Ed::DragCue c) { s_probed.push_back(c); }
} // namespace

TEST_CASE("Rewards: posted drag cues play once, on the editor's engine, through the probe")
{
	using C = HE::Ed::DragCue;
	AudioEngine project, ui;
	REQUIRE(project.init(true));
	RewardsContextBits bits;
	bits.config.RewardsSound  = true;
	bits.config.RewardsVolume = 1.0f;
	AppContext ctx = bits.make(project, ui);

	// A context of its own, so the frame counter and the clock move.
	ImGuiContext* prev = ImGui::GetCurrentContext();
	ImGuiContext* mine = ImGui::CreateContext();
	ImGui::SetCurrentContext(mine);
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2(640.0f, 480.0f);
	io.DeltaTime   = 0.1f;
	io.IniFilename = nullptr;
	io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	auto frame = [&]{ ImGui::NewFrame(); pollBuild(ctx, 0, false, false, true); ImGui::EndFrame(); };

	setDragCueProbe(&probeCue);
	s_probed.clear();
	frame();                                    // opens the UI-sound device
	REQUIRE(ui.isInitialized());

	postDragCue(C::Pickup);
	postDragCue(C::OverValid);
	CHECK(s_probed.empty());                    // queued, not played yet
	frame();
	CHECK(s_probed == std::vector<C>{ C::Pickup, C::OverValid });
	CHECK(loudestOut(ui) > 0.01f);              // heard on the editor's engine…
	CHECK(loudestOut(project) == 0.0f);         // …never on the project's
	ui.stopAll();
	frame();
	CHECK(s_probed.size() == 2);                // once, not again next frame

	// Switched off, during Play: dropped, not kept for later.
	s_probed.clear();
	bits.config.RewardsSoundDragDrop = false;
	postDragCue(C::Drop);
	frame();
	bits.config.RewardsSoundDragDrop = true;
	ctx.isPlaying = true;
	postDragCue(C::Drop);
	frame();
	ctx.isPlaying = false;
	frame();
	CHECK(s_probed.empty());

	// A reward tone in the same frame wins over the cue.
	postDragCue(C::Drop);
	ImGui::NewFrame();
	preview(ctx, Tone::CompileFailed);          // a tone that played this frame
	pollBuild(ctx, 0, false, false, true);
	ImGui::EndFrame();
	CHECK(s_probed.empty());
	postDragCue(C::Drop);
	frame();
	CHECK(s_probed == std::vector<C>{ C::Drop });

	setDragCueProbe(nullptr);
	ui.stopAll();
	ImGui::DestroyContext(mine);
	ImGui::SetCurrentContext(prev);
}
