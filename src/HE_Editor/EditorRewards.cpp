#include "EditorRewards.h"
#include "EditorApplication.h"   // AppContext, EditorConfig
#include <HorizonScene/AudioEngine.h>

#include <Diagnostics/GlobalState.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iterator>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace HE::Ed::Rewards
{

// ── Core ─────────────────────────────────────────────────────────────────────

std::string lineFor(Moment m, int count)
{
	switch (m)
	{
	case Moment::Saved:          return "Saved";
	case Moment::BuildSucceeded: return "Build succeeded";
	case Moment::AssetsImported:
		return count == 1 ? std::string("Imported 1 asset")
		                  : "Imported " + std::to_string(std::max(count, 0)) + " assets";
	}
	return {};
}

float strengthAt(double age)
{
	if (age <= kHoldSec) return 1.0f;
	const double t = (age - kHoldSec) / kFadeSec;
	if (t >= 1.0) return 0.0f;
	// Smoothstep down: no visible kink where the hold ends or where it lands.
	const double s = t * t * (3.0 - 2.0 * t);
	return static_cast<float>(1.0 - s);
}

float gainFor(float volume)
{
	// !(v > 0) also catches NaN from a hand-edited config.
	const float v = !(volume > 0.0f) ? 0.0f : std::min(volume, 1.0f);
	return v * v;
}

int rankOf(Moment m)
{
	switch (m)
	{
	case Moment::Saved:          return 0;
	case Moment::AssetsImported: return 1;
	case Moment::BuildSucceeded: return 2;
	}
	return 0;
}

Tone toneFor(Moment m)
{
	switch (m)
	{
	case Moment::Saved:          return Tone::SaveTick;
	case Moment::BuildSucceeded: return Tone::BuildChime;
	case Moment::AssetsImported: return Tone::ImportPop;
	}
	return Tone::SaveTick;
}

bool uiSoundPossible(const EditorConfig& cfg)
{
	return cfg.RewardsEnabled && cfg.RewardsSound && !cfg.EditorSoundsMuted;
}

bool toneWanted(const EditorConfig& cfg, Tone t, bool playing, bool appFocused)
{
	if (!uiSoundPossible(cfg) || !(gainFor(cfg.RewardsVolume) > 0.0f) || playing) return false;
	switch (t)
	{
	case Tone::SaveTick:    return cfg.RewardsSoundSave;
	case Tone::ImportPop:   return cfg.RewardsSoundImport;
	case Tone::BuildChime:  return cfg.RewardsSoundBuild && !appFocused;
	case Tone::BuildFailed: return cfg.RewardsSoundBuildFailed && !appFocused;
	}
	return false;
}

Feed::Taken Feed::take(Moment m, int count, double now, int frame, bool soundWanted)
{
	Taken r;
	// frame < 0: the caller has no frame clock (no ImGui context) — nothing to
	// fold against, every push counts.
	if (frame >= 0 && frame == m_lastFrame) return r;
	m_lastFrame = frame;
	r.taken = true;

	const bool showing = look(now).active;
	if (showing && m == m_moment)
	{
		// Rule 2: the same kind again — one line, counted up, held anew, silent.
		m_count += std::max(count, 0);
		m_at     = now;
		r.shown  = true;
		return r;
	}
	// Rule 3: a lower moment leaves a higher line alone (the tally has it).
	if (showing && rankOf(m) < rankOf(m_moment)) return r;

	m_has    = true;
	m_moment = m;
	m_count  = count;
	m_at     = now;
	r.shown  = true;

	r.sound = soundWanted && takeTone(toneFor(m), now);
	return r;
}

bool Feed::takeTone(Tone t, double now)
{
	// Rules 4 and 5: the gaps run from the last tone that played.
	const bool save = t == Tone::SaveTick;
	if (m_toned && now - m_toneAt < kToneGapSec) return false;
	if (save && m_saveToned && now - m_saveToneAt < kSaveToneGapSec) return false;
	m_toned  = true;
	m_toneAt = now;
	if (save)
	{
		m_saveToned  = true;
		m_saveToneAt = now;
	}
	return true;
}

Feed::BuildEnd Feed::buildEnded(unsigned long long run, bool finished, bool success)
{
	if (!finished || run == 0 || run == m_lastRun) return BuildEnd::None;
	m_lastRun = run;
	return success ? BuildEnd::Succeeded : BuildEnd::Failed;
}

Feed::Look Feed::look(double now) const
{
	Look lk;
	if (!m_has) return lk;
	const double age = now - m_at;
	if (age < 0.0 || age >= kHoldSec + kFadeSec) return lk;
	lk.active   = true;
	lk.line     = lineFor(m_moment, m_count);
	lk.strength = strengthAt(age);
	lk.bar      = static_cast<float>(1.0 - age / (kHoldSec + kFadeSec));
	return lk;
}

namespace
{
struct Ymd { int y = 0, m = 0, d = 0; };

int daysIn(int y, int m)
{
	static constexpr int kDays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
	return (m == 2 && leap) ? 29 : kDays[m - 1];
}

// Strict YYYY-MM-DD: exactly ten characters, a real calendar date.
bool parseDay(const std::string& s, Ymd& out)
{
	if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
	for (int i : { 0, 1, 2, 3, 5, 6, 8, 9 })
		if (s[i] < '0' || s[i] > '9') return false;
	const auto num = [&](int at, int len) { return std::stoi(s.substr(at, len)); };
	Ymd v{ num(0, 4), num(5, 2), num(8, 2) };
	if (v.y < 1 || v.m < 1 || v.m > 12 || v.d < 1 || v.d > daysIn(v.y, v.m)) return false;
	out = v;
	return true;
}

std::string formatDay(const Ymd& v)
{
	char buf[16];
	std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", v.y, v.m, v.d);
	return buf;
}
} // namespace

std::string dayBefore(const std::string& ymd)
{
	Ymd v;
	if (!parseDay(ymd, v)) return {};
	if (--v.d < 1)
	{
		if (--v.m < 1) { v.m = 12; --v.y; }
		if (v.y < 1) return {};
		v.d = daysIn(v.y, v.m);
	}
	return formatDay(v);
}

bool recordUse(Tally& t, const std::string& today, bool build)
{
	Ymd now;
	if (!parseDay(today, now)) return false;
	if (t.day == today)
	{
		if (!build) return false;   // the day is already counted
		++t.buildsToday;
		return true;
	}
	// The clock is behind the stored day. YYYY-MM-DD orders as text.
	Ymd stored;
	if (parseDay(t.day, stored) && today < t.day) return false;

	t.streakDays  = (t.day == dayBefore(today) && t.streakDays > 0) ? t.streakDays + 1 : 1;
	t.day         = today;
	t.buildsToday = build ? 1 : 0;
	return true;
}

std::string progressText(const Tally& t, const std::string& today)
{
	Ymd stored;
	if (!parseDay(t.day, stored)) return {};
	const bool isToday = t.day == today;
	const bool alive   = isToday || t.day == dayBefore(today);
	const int  builds  = isToday ? std::max(t.buildsToday, 0) : 0;

	std::string s = builds == 1 ? std::string("1 build today")
	                            : std::to_string(builds) + " builds today";
	if (alive && t.streakDays >= 2)
		s += " · " + std::to_string(t.streakDays) + " days in a row";
	return s;
}

std::string localDay()
{
	const std::time_t now = std::time(nullptr);
	std::tm tm{};
#ifdef _WIN32
	localtime_s(&tm, &now);
#else
	localtime_r(&now, &tm);
#endif
	return formatDay({ tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday });
}

namespace
{
// Own constant: M_PI needs _USE_MATH_DEFINES on MSVC.
constexpr double kTwoPi = 6.283185307179586;

void putSample(std::vector<uint8_t>& out, size_t i, double v)
{
	const int s = static_cast<int>(std::lround(std::clamp(v, -1.0, 1.0) * 32767.0));
	out[i * 2]     = static_cast<uint8_t>(s & 0xFF);
	out[i * 2 + 1] = static_cast<uint8_t>((s >> 8) & 0xFF);
}

// v scaled so its loudest sample sits exactly at `peak`, after the last
// tailSec were faded to land on exactly zero — so the level of a tone is a
// number here and not a side effect of how its parts happen to add up.
std::vector<uint8_t> toPcm16(std::vector<double> v, int sampleRate, double peak, double tailSec)
{
	const size_t n          = v.size();
	const double tailFrames = std::max(1.0, tailSec * sampleRate);
	double       loudest    = 0.0;
	for (size_t i = 0; i < n; ++i)
	{
		v[i] *= std::min(1.0, static_cast<double>(n - 1 - i) / tailFrames);
		loudest = std::max(loudest, std::abs(v[i]));
	}
	const double k = loudest > 0.0 ? peak / loudest : 0.0;
	std::vector<uint8_t> out(n * 2);
	for (size_t i = 0; i < n; ++i) putSample(out, i, v[i] * k);
	return out;
}

// Struck notes: each rises over attackSec and rings out with e-folding time
// decaySec. What the chime is made of, and the failed-build tone.
struct Note { double hz, startSec, level; };
std::vector<double> ringNotes(const Note* notes, size_t count, int sampleRate,
                              double lengthSec, double attackSec, double decaySec)
{
	std::vector<double> v(static_cast<size_t>(lengthSec * sampleRate));
	for (size_t i = 0; i < v.size(); ++i)
	{
		const double t = static_cast<double>(i) / sampleRate;
		for (size_t k = 0; k < count; ++k)
		{
			const double lt = t - notes[k].startSec;
			if (lt < 0.0) continue;
			const double env = std::min(1.0, lt / attackSec) * std::exp(-lt / decaySec);
			v[i] += std::sin(kTwoPi * notes[k].hz * lt) * env * notes[k].level;
		}
	}
	return v;
}
} // namespace

std::vector<uint8_t> saveTickPcm16(int sampleRate)
{
	// S1: E6 struck and damped almost at once, with a breath of band-passed
	// noise on the attack — a switch's click in the tones' key, not a note.
	// The quietest tone: it is the one heard most often.
	constexpr double kLengthSec  = 0.06;
	constexpr double kAttackSec  = 0.004;
	constexpr double kToneHz     = 1318.51;   // E6
	constexpr double kToneDecay  = 0.012;
	constexpr double kNoiseDecay = 0.005;
	constexpr double kNoiseLevel = 0.35;      // against the tone, both at unit peak
	constexpr double kPeak       = 0.16;      // ≈ −16 dBFS
	if (sampleRate <= 0) return {};

	const size_t n = static_cast<size_t>(kLengthSec * sampleRate);
	std::vector<double> tone(n), noise(n);
	// The noise band-passed to about 1.2–3 kHz: the difference of two one-pole
	// low-passes, so nothing of it reaches the bass. A fixed seed: the tick is
	// the same every time, and the tests can pin it.
	const double aHi = 1.0 - std::exp(-kTwoPi * 3000.0 / sampleRate);
	const double aLo = 1.0 - std::exp(-kTwoPi * 1200.0 / sampleRate);
	uint32_t     rng = 0x9E3779B9u;
	double       lpHi = 0.0, lpLo = 0.0, toneMax = 0.0, noiseMax = 0.0;
	for (size_t i = 0; i < n; ++i)
	{
		const double t   = static_cast<double>(i) / sampleRate;
		const double rise = std::min(1.0, t / kAttackSec);
		rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
		const double white = static_cast<double>(rng) / 4294967295.0 * 2.0 - 1.0;
		lpHi += aHi * (white - lpHi);
		lpLo += aLo * (white - lpLo);
		tone[i]  = std::sin(kTwoPi * kToneHz * t) * rise * std::exp(-t / kToneDecay);
		noise[i] = (lpHi - lpLo) * rise * std::exp(-t / kNoiseDecay);
		toneMax  = std::max(toneMax, std::abs(tone[i]));
		noiseMax = std::max(noiseMax, std::abs(noise[i]));
	}
	for (size_t i = 0; i < n; ++i)
		tone[i] = tone[i] / std::max(toneMax, 1e-9) + kNoiseLevel * noise[i] / std::max(noiseMax, 1e-9);
	return toPcm16(std::move(tone), sampleRate, kPeak, 0.01);
}

std::vector<uint8_t> importPopPcm16(int sampleRate)
{
	// I1: a sine that drops from 1.4 to 0.9 kHz in 70 ms — "something landed".
	// The glide is exponential (even in pitch) and integrated as phase: sin of
	// f(t)·t would sweep twice as far as it says.
	constexpr double kLengthSec = 0.10;
	constexpr double kAttackSec = 0.004;
	constexpr double kFromHz    = 1400.0;
	constexpr double kToHz      = 900.0;
	constexpr double kGlideSec  = 0.07;
	constexpr double kDecaySec  = 0.03;
	constexpr double kPeak      = 0.22;       // ≈ −13 dBFS
	if (sampleRate <= 0) return {};

	std::vector<double> v(static_cast<size_t>(kLengthSec * sampleRate));
	double phase = 0.0;
	for (size_t i = 0; i < v.size(); ++i)
	{
		const double t  = static_cast<double>(i) / sampleRate;
		const double hz = kFromHz * std::pow(kToHz / kFromHz, std::min(t, kGlideSec) / kGlideSec);
		v[i]   = std::sin(phase) * std::min(1.0, t / kAttackSec) * std::exp(-t / kDecaySec);
		phase += kTwoPi * hz / sampleRate;
	}
	return toPcm16(std::move(v), sampleRate, kPeak, 0.015);
}

std::vector<uint8_t> buildFailedPcm16(int sampleRate)
{
	// E6 then B5, a fourth DOWN — the chime turned downwards, on a note that
	// does not resolve: "look at the build", not "wrong". A softer attack and
	// a slower step than the chime, and quieter than it. No buzz, no low note.
	constexpr Note   kNotes[]   = { { 1318.51, 0.0, 1.0 }, { 987.77, 0.11, 0.85 } };
	constexpr double kLengthSec = 0.45;
	constexpr double kPeak      = 0.25;       // ≈ −12 dBFS
	if (sampleRate <= 0) return {};
	return toPcm16(ringNotes(kNotes, std::size(kNotes), sampleRate, kLengthSec, 0.008, 0.10),
	               sampleRate, kPeak, 0.03);
}

std::vector<uint8_t> tonePcm16(Tone t, int sampleRate)
{
	switch (t)
	{
	case Tone::SaveTick:    return saveTickPcm16(sampleRate);
	case Tone::BuildChime:  return chimePcm16(sampleRate);
	case Tone::BuildFailed: return buildFailedPcm16(sampleRate);
	case Tone::ImportPop:   return importPopPcm16(sampleRate);
	}
	return {};
}

std::vector<uint8_t> chimePcm16(int sampleRate)
{
	// A5 then E6 70 ms later, each struck and left to ring out. Quiet on
	// purpose — the peak stays well below full scale even where both overlap.
	// Topic 75's numbers, unchanged: kPeak scales the sum, it is not
	// normalised like the newer tones.
	constexpr Note   kNotes[]   = { { 880.0, 0.0, 1.0 }, { 1318.51, 0.07, 1.0 } };
	constexpr double kLengthSec = 0.42;
	constexpr double kAttackSec = 0.004;   // a ramp in, or the first sample clicks
	constexpr double kDecaySec  = 0.09;    // e-folding time of each note
	constexpr double kPeak      = 0.30;

	if (sampleRate <= 0) return {};
	const std::vector<double> v =
		ringNotes(kNotes, std::size(kNotes), sampleRate, kLengthSec, kAttackSec, kDecaySec);
	std::vector<uint8_t> out(v.size() * 2);
	for (size_t i = 0; i < v.size(); ++i)
	{
		// The tail fades to exactly zero at the end, so the voice stops silent.
		const double t    = static_cast<double>(i) / sampleRate;
		const double tail = std::min(1.0, (kLengthSec - t) / 0.02);
		putSample(out, i, v[i] * kPeak * tail);
	}
	return out;
}

// ── Editor side ──────────────────────────────────────────────────────────────

namespace
{
Feed  s_feed;
Tally s_tally;
bool  s_tallyLoaded = false;

void loadTally(AppContext& ctx)
{
	if (s_tallyLoaded) return;
	s_tallyLoaded = true;
	if (!ctx.globalState) return;
	const GlobalState& gs = *ctx.globalState;
	s_tally.day         = gs.getCustomConfigString("RewardsDay", "");
	s_tally.buildsToday = std::max(0, gs.getCustomConfigInt("RewardsBuildsToday", 0));
	s_tally.streakDays  = std::max(0, gs.getCustomConfigInt("RewardsStreakDays", 0));
}

// Before fire()'s once-per-frame fold — see "Rules" in the header.
void tallyMoment(AppContext& ctx, bool build)
{
	loadTally(ctx);
	if (!recordUse(s_tally, localDay(), build) || !ctx.globalState) return;
	ctx.globalState->setCustomConfigEntry("RewardsDay",         s_tally.day);
	ctx.globalState->setCustomConfigEntry("RewardsBuildsToday", s_tally.buildsToday);
	ctx.globalState->setCustomConfigEntry("RewardsStreakDays",  s_tally.streakDays);
	ctx.globalState->writeConfig();
}

bool s_appFocused   = true;    // the last pollBuild's word; focused = no build tone
bool s_uiAudioFailed = false;  // its device would not open — see keepUiAudio

// The UI-sound engine ("Routing" in the header): open while a tone is
// possible, closed while none is. Called every frame from pollBuild, so the
// device opens in a frame of its own and never in the frame of a save.
void keepUiAudio(AppContext& ctx)
{
	AudioEngine* a = ctx.uiAudioEngine;
	if (!a) return;
	if (!uiSoundPossible(ctx.editorConfig))
	{
		s_uiAudioFailed = false;   // switching sound back on tries again
		a->shutdown();             // no-op when it is not open
		return;
	}
	// AudioEngine::init logs its own failure; once is enough.
	if (!a->isInitialized() && !s_uiAudioFailed && !a->init())
		s_uiAudioFailed = true;
}

void playTone(AppContext& ctx, Tone t, float gain)
{
	AudioEngine* a = ctx.uiAudioEngine;
	if (!a || !a->isInitialized() || !(gain > 0.0f)) return;
	constexpr int kRate = 44100;
	// Indexed by Tone.
	static const std::vector<uint8_t> pcm[] = {
		tonePcm16(Tone::SaveTick, kRate),    tonePcm16(Tone::BuildChime, kRate),
		tonePcm16(Tone::BuildFailed, kRate), tonePcm16(Tone::ImportPop, kRate),
	};
	a->play(pcm[static_cast<int>(t)], kRate, 1, gain);
}

double nowSec()
{
#ifdef HE_IMGUI_ENABLED
	if (ImGui::GetCurrentContext()) return ImGui::GetTime();
#endif
	return 0.0;
}

int frameNo()
{
#ifdef HE_IMGUI_ENABLED
	if (ImGui::GetCurrentContext()) return ImGui::GetFrameCount();
#endif
	return -1;
}
} // namespace

void fire(AppContext& ctx, Moment m, int count)
{
	const EditorConfig& cfg = ctx.editorConfig;
	if (!cfg.RewardsEnabled) return;
	tallyMoment(ctx, m == Moment::BuildSucceeded);
	// Rule 6: the switches say whether this moment may sound at all; the Feed
	// says whether it does. Visual off still goes through the Feed, so what
	// is heard follows the same merging and rank either way.
	const Tone tone        = toneFor(m);
	const bool soundWanted = toneWanted(cfg, tone, ctx.isPlaying, s_appFocused);
	if (s_feed.take(m, count, nowSec(), frameNo(), soundWanted).sound)
		playTone(ctx, tone, gainFor(cfg.RewardsVolume));
}

void preview(AppContext& ctx, Tone t)
{
	// The button is a click, not a save: opening the device in this frame (if
	// Sound was switched on in this very frame, before pollBuild saw it) is
	// fine here. keepUiAudio also answers "muted" by leaving it closed.
	keepUiAudio(ctx);
	playTone(ctx, t, gainFor(ctx.editorConfig.RewardsVolume));
}

bool systemReducesMotion()
{
	return false;
}

bool reducedMotion(const AppContext& ctx)
{
	return ctx.editorConfig.RewardsReducedMotion == 0 && systemReducesMotion();
}

void pollBuild(AppContext& ctx, unsigned long long run, bool finished, bool success,
               bool appFocused)
{
	s_appFocused = appFocused;
	keepUiAudio(ctx);
	// Consumed whether or not the feature is on: switching it on later must not
	// reward a build that finished while it was off.
	switch (s_feed.buildEnded(run, finished, success))
	{
	case Feed::BuildEnd::Succeeded:
		fire(ctx, Moment::BuildSucceeded);
		break;
	case Feed::BuildEnd::Failed:
	{
		// Not a moment (no line, nothing counted) — only its tone, under the
		// same switches and the same gap as every other.
		const EditorConfig& cfg = ctx.editorConfig;
		if (toneWanted(cfg, Tone::BuildFailed, ctx.isPlaying, appFocused)
		    && s_feed.takeTone(Tone::BuildFailed, nowSec()))
			playTone(ctx, Tone::BuildFailed, gainFor(cfg.RewardsVolume));
		break;
	}
	case Feed::BuildEnd::None:
		break;
	}
}

#ifdef HE_IMGUI_ENABLED
namespace
{
// localDay() once a second rather than every frame; a new day still shows
// within a second of midnight.
const std::string& todayCached()
{
	static std::string day;
	static double      at = -1.0;
	const double now = ImGui::GetTime();
	if (at < 0.0 || now - at >= 1.0 || now < at) { day = localDay(); at = now; }
	return day;
}
} // namespace

void drawFooterStatus(AppContext& ctx, const char* idleTextIn)
{
	const float winW = ImGui::GetWindowWidth();

	// "Ready · 3 builds today · 5 days in a row" — or plain "Ready" when the
	// counters are off, empty, or would not fit in the middle third.
	std::string idle = idleTextIn;
	if (ctx.editorConfig.RewardsEnabled && ctx.editorConfig.RewardsShowProgress)
	{
		loadTally(ctx);
		const std::string p = progressText(s_tally, todayCached());
		if (!p.empty())
		{
			std::string full = idle + " · " + p;
			if (ImGui::CalcTextSize(full.c_str()).x <= winW / 3.0f) idle = std::move(full);
		}
	}
	const char* idleText = idle.c_str();
	const float idleW    = ImGui::CalcTextSize(idleText).x;
	const bool       visual = ctx.editorConfig.RewardsEnabled && ctx.editorConfig.RewardsVisual;
	const Feed::Look lk     = visual ? s_feed.look(ImGui::GetTime()) : Feed::Look{};
	if (!lk.active)
	{
		ImGui::SameLine((winW - idleW) * 0.5f);
		ImGui::TextDisabled("%s", idleText);
		return;
	}

	// The build window's "done" ring colour: the one green the editor already
	// uses for "this worked".
	const ImVec4 done(90.0f / 255.0f, 215.0f / 255.0f, 90.0f / 255.0f, 1.0f);
	const float  lineW = ImGui::CalcTextSize(lk.line.c_str()).x;
	ImGui::SameLine((winW - lineW) * 0.5f);
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	ImGui::TextColored(ImVec4(done.x, done.y, done.z, lk.strength), "%s", lk.line.c_str());

	ImDrawList* dl = ImGui::GetWindowDrawList();
	// The idle text fades in where it will stand, on top of the fading line —
	// drawn, not laid out, so the footer's layout is the same every frame.
	if (lk.strength < 1.0f)
		dl->AddText(ImVec2(pos.x + (lineW - idleW) * 0.5f, pos.y),
		            ImGui::GetColorU32(ImGuiCol_TextDisabled, 1.0f - lk.strength), idleText);
	// The underline shrinks towards the centre as the moment runs out — the one
	// moving thing, so reduced motion leaves it out.
	if (reducedMotion(ctx)) return;
	const float y    = pos.y + ImGui::GetTextLineHeight() + 1.0f;
	const float half = lineW * 0.5f * lk.bar;
	const float mid  = pos.x + lineW * 0.5f;
	if (half > 0.5f)
		dl->AddLine(ImVec2(mid - half, y), ImVec2(mid + half, y),
		            ImGui::GetColorU32(ImVec4(done.x, done.y, done.z, 0.8f * lk.strength)), 1.0f);
}
#else
void drawFooterStatus(AppContext&, const char*) {}
#endif

} // namespace HE::Ed::Rewards
