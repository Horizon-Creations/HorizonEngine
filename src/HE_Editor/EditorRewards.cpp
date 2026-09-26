#include "EditorRewards.h"
#include "EditorApplication.h"   // AppContext, EditorConfig
#include <HorizonScene/AudioEngine.h>

#include <Diagnostics/GlobalState.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

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

bool Feed::push(Moment m, int count, double now, int frame)
{
	// frame < 0: the caller has no frame clock (no ImGui context) — nothing to
	// fold against, every push counts.
	if (frame >= 0 && frame == m_lastFrame) return false;
	m_lastFrame = frame;
	m_has    = true;
	m_moment = m;
	m_count  = count;
	m_at     = now;
	return true;
}

bool Feed::buildSucceeded(unsigned long long run, bool finished, bool success)
{
	if (!finished || run == 0 || run == m_lastRun) return false;
	m_lastRun = run;
	return success;
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

std::vector<uint8_t> chimePcm16(int sampleRate)
{
	// Own constant: M_PI needs _USE_MATH_DEFINES on MSVC.
	constexpr double kTwoPi = 6.283185307179586;
	// A5 then E6 70 ms later, each struck and left to ring out. Quiet on
	// purpose — the peak stays well below full scale even where both overlap.
	struct Note { double hz, startSec; };
	constexpr Note   kNotes[]   = { { 880.0, 0.0 }, { 1318.51, 0.07 } };
	constexpr double kLengthSec = 0.42;
	constexpr double kAttackSec = 0.004;   // a ramp in, or the first sample clicks
	constexpr double kDecaySec  = 0.09;    // e-folding time of each note
	constexpr double kPeak      = 0.30;

	if (sampleRate <= 0) return {};
	const int frames = static_cast<int>(kLengthSec * sampleRate);
	std::vector<uint8_t> out(static_cast<size_t>(frames) * 2);
	for (int i = 0; i < frames; ++i)
	{
		const double t = static_cast<double>(i) / sampleRate;
		double v = 0.0;
		for (const Note& n : kNotes)
		{
			const double lt = t - n.startSec;
			if (lt < 0.0) continue;
			const double env = std::min(1.0, lt / kAttackSec) * std::exp(-lt / kDecaySec);
			v += std::sin(kTwoPi * n.hz * lt) * env;
		}
		// The tail fades to exactly zero at the end, so the voice stops silent.
		const double tail = std::min(1.0, (kLengthSec - t) / 0.02);
		const int s = static_cast<int>(std::lround(std::clamp(v * kPeak * tail, -1.0, 1.0) * 32767.0));
		out[static_cast<size_t>(i) * 2]     = static_cast<uint8_t>(s & 0xFF);
		out[static_cast<size_t>(i) * 2 + 1] = static_cast<uint8_t>((s >> 8) & 0xFF);
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

void playChime(AppContext& ctx)
{
	if (!ctx.audioEngine) return;
	constexpr int kRate = 44100;
	static const std::vector<uint8_t> pcm = chimePcm16(kRate);
	// Returns 0 when the engine never came up — nothing to do about that here.
	ctx.audioEngine->play(pcm, kRate, 1, 1.0f);
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
	if (!ctx.editorConfig.RewardsEnabled) return;
	tallyMoment(ctx, m == Moment::BuildSucceeded);
	if (!s_feed.push(m, count, nowSec(), frameNo())) return;
	if (ctx.editorConfig.RewardsSound) playChime(ctx);
}

void pollBuild(AppContext& ctx, unsigned long long run, bool finished, bool success)
{
	// Consumed whether or not the feature is on: switching it on later must not
	// reward a build that finished while it was off.
	if (s_feed.buildSucceeded(run, finished, success))
		fire(ctx, Moment::BuildSucceeded);
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
	const Feed::Look lk = ctx.editorConfig.RewardsEnabled ? s_feed.look(ImGui::GetTime())
	                                                      : Feed::Look{};
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
	// The underline shrinks towards the centre as the moment runs out.
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
