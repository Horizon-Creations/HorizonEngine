#include "EditorRewards.h"
#include "EditorApplication.h"   // AppContext, EditorConfig
#include <HorizonScene/AudioEngine.h>

#include <algorithm>
#include <cmath>

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
Feed s_feed;

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
void drawFooterStatus(AppContext& ctx, const char* idleText)
{
	const float winW  = ImGui::GetWindowWidth();
	const float idleW = ImGui::CalcTextSize(idleText).x;
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
