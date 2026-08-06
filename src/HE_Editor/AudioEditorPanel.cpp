#include "AudioEditorPanel.h"
#include "EditorToolbar.h"        // shared toolbar strip
#include "EditorApplication.h"    // AppContext
#include "EditorAssetTypeCache.h" // shared, invalidatable path → AssetType sniff
#include "EditorPanelState.h"     // shared per-tab state map + lazy asset open
#include "AudioImporter.h"        // raw .wav decode + the Import button
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Diagnostics/Logger.h>
#include <Types/Enums.h>
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace AudioEditorPanel
{

namespace
{

// ── Peak table ───────────────────────────────────────────────────────────────
// Drawing a waveform straight from the samples means touching every sample in
// view every frame, and these clips run to tens of millions. So: one min/max
// pair per bucket of frames, built once. 256 frames/bucket costs ~0.4% of the
// PCM size (a 20-minute stereo ambience → well under a megabyte) and still holds
// more detail than a 4K-wide canvas can show until you zoom past ~5 s of clip.
// Past that the drawing code reads raw samples instead — see columnExtent().
constexpr size_t kFramesPerBucket = 256;

struct Peaks
{
	// Indexed [bucket * channels + channel]. int16 because the samples are.
	std::vector<int16_t> lo, hi;
	size_t buckets  = 0;
	int    channels = 0;
};

// What the analysis pane reports. Everything is derived in one pass over the PCM
// (analyze() below) and cached for the tab's lifetime.
struct Analysis
{
	size_t frames        = 0;
	double durationSec   = 0.0;

	float  peakDb        = -144.0f;  // loudest single sample, dBFS
	float  rmsDb         = -144.0f;  // whole-clip RMS, dBFS
	size_t clipped       = 0;        // samples pinned at full scale
	float  dcOffset      = 0.0f;     // mean sample as a fraction of full scale

	double leadSilenceSec = 0.0;     // silence padding the head …
	double tailSilenceSec = 0.0;     // … and the tail (below kSilenceDb)

	// Loop-seam check: what happens at the wrap point when the clip loops.
	// `seamStepDb` is the instantaneous jump from the last frame to the first —
	// the click you hear. `seamLevelDb` is how far the first 250 ms sit from the
	// last 250 ms in RMS — a match in level, which is what stops a loop from
	// "breathing" even when there is no click.
	float  seamStepDb    = -144.0f;
	float  seamLevelDb   = 0.0f;
};

// Anything quieter than this counts as silence for the head/tail padding report.
constexpr float kSilenceDb = -60.0f;

struct State
{
	bool        loaded = false;
	std::string relPath;
	std::string name;

	// Exactly one of these two holds the clip: an imported .hasset lives in the
	// ContentManager (`clipId`), a raw .wav is decoded into the panel (`raw`).
	HE::UUID    clipId;
	AudioAsset  raw;
	bool        isRawWav     = false;
	bool        decodeFailed = false;

	Peaks    peaks;     bool peaksDone    = false;
	Analysis analysis;  bool analysisDone = false;

	// ── Transport ────────────────────────────────────────────────────────────
	// The engine outlives every tab (EditorApplication owns it), so holding the
	// pointer here is what lets forget() — which gets no AppContext — silence a
	// tab that is being closed mid-playback.
	AudioEngine* audio  = nullptr;
	uint64_t     handle = 0;
	bool         paused = false;      // handle alive but stopped; NOT finished
	size_t       playhead = 0;        // frames; survives stop, drives Play's start
	bool         loop   = true;       // ambience is the reason this tab exists
	float        volume = 1.0f;
	float        pitch  = 1.0f;

	// ── Waveform view ────────────────────────────────────────────────────────
	double viewStart   = 0.0;   // leftmost frame
	double framesPerPx = 0.0;   // 0 = not fitted yet (first render fits the clip)
	bool   scrubbing   = false;
};

AssetPanelState<State> s_states;

// ── Clip access ──────────────────────────────────────────────────────────────

const AudioAsset* clipOf(AppContext& ctx, State& st)
{
	if (st.isRawWav) return st.raw.audioData.empty() ? nullptr : &st.raw;
	return ctx.contentManager ? ctx.contentManager->getAudio(st.clipId) : nullptr;
}

const int16_t* samplesOf(const AudioAsset& a)
{
	return reinterpret_cast<const int16_t*>(a.audioData.data());
}

size_t frameCountOf(const AudioAsset& a)
{
	if (a.channels <= 0) return 0;
	return a.audioData.size() / (sizeof(int16_t) * static_cast<size_t>(a.channels));
}

// ── Analysis ─────────────────────────────────────────────────────────────────

float toDb(double linear)
{
	// -144 dB is the int16 noise floor; anything at or below it reads as silence
	// rather than as -inf, which formats badly and means the same thing here.
	return linear <= 1.0e-7 ? -144.0f : static_cast<float>(20.0 * std::log10(linear));
}

// RMS of a frame range, as a fraction of full scale. Used for the whole clip and
// for the two 250 ms windows the seam check compares.
double rmsOf(const AudioAsset& a, size_t f0, size_t f1)
{
	const int ch = a.channels;
	if (ch <= 0 || f1 <= f0) return 0.0;
	const int16_t* s = samplesOf(a);
	double sum = 0.0;
	for (size_t f = f0; f < f1; ++f)
		for (int c = 0; c < ch; ++c)
		{
			const double v = static_cast<double>(s[f * ch + c]) / 32768.0;
			sum += v * v;
		}
	return std::sqrt(sum / (static_cast<double>(f1 - f0) * ch));
}

Analysis analyze(const AudioAsset& a)
{
	Analysis an;
	const int ch = a.channels;
	an.frames = frameCountOf(a);
	if (ch <= 0 || an.frames == 0 || a.sampleRate <= 0) return an;

	const int16_t* s    = samplesOf(a);
	const double   rate = static_cast<double>(a.sampleRate);
	an.durationSec = static_cast<double>(an.frames) / rate;

	// One pass for peak, RMS, clipping and DC — they all want every sample, and at
	// tens of millions of samples per clip a second pass is a visible stall.
	// Integer accumulators: a squared sample tops out at 2^30, so int64 covers
	// ~8.6 billion samples (≈25 hours of 48 kHz stereo) before it could overflow.
	int      peak  = 0;
	int64_t  sum   = 0;
	uint64_t sumSq = 0;
	const size_t total = an.frames * static_cast<size_t>(ch);
	for (size_t i = 0; i < total; ++i)
	{
		const int v  = s[i];
		const int av = v < 0 ? -v : v;
		if (av > peak) peak = av;
		if (av >= 32767) ++an.clipped;
		sum   += v;
		sumSq += static_cast<uint64_t>(static_cast<int64_t>(v) * v);
	}
	const double totalD = static_cast<double>(total);
	an.peakDb   = toDb(static_cast<double>(peak) / 32768.0);
	an.rmsDb    = toDb(std::sqrt(static_cast<double>(sumSq) / totalD) / 32768.0);
	an.dcOffset = static_cast<float>(static_cast<double>(sum) / totalD / 32768.0);

	// Head/tail silence: walk in from both ends while every channel of the frame
	// stays under the threshold.
	const int silenceAmp = static_cast<int>(32768.0 * std::pow(10.0, kSilenceDb / 20.0));
	auto frameIsSilent = [&](size_t f)
	{
		for (int c = 0; c < ch; ++c)
		{
			const int v  = s[f * ch + c];
			const int av = v < 0 ? -v : v;
			if (av > silenceAmp) return false;
		}
		return true;
	};
	size_t lead = 0;
	while (lead < an.frames && frameIsSilent(lead)) ++lead;
	an.leadSilenceSec = static_cast<double>(lead) / rate;
	if (lead < an.frames)   // an all-silent clip is not "silence at both ends"
	{
		size_t tail = 0;
		while (tail < an.frames - lead && frameIsSilent(an.frames - 1 - tail)) ++tail;
		an.tailSilenceSec = static_cast<double>(tail) / rate;
	}

	// Loop seam. The step is the worst per-channel jump across the wrap; the
	// level delta compares a quarter-second at each end.
	int step = 0;
	for (int c = 0; c < ch; ++c)
	{
		const int d = std::abs(static_cast<int>(s[(an.frames - 1) * ch + c]) -
		                       static_cast<int>(s[c]));
		if (d > step) step = d;
	}
	an.seamStepDb = toDb(static_cast<double>(step) / 32768.0);

	const size_t win = std::min<size_t>(an.frames / 2, static_cast<size_t>(rate * 0.25));
	if (win > 0)
	{
		const double head = rmsOf(a, 0, win);
		const double tail = rmsOf(a, an.frames - win, an.frames);
		an.seamLevelDb = toDb(head) - toDb(tail);
	}
	return an;
}

// ── Peaks ────────────────────────────────────────────────────────────────────

Peaks buildPeaks(const AudioAsset& a)
{
	Peaks p;
	const int ch = a.channels;
	const size_t frames = frameCountOf(a);
	if (ch <= 0 || frames == 0) return p;

	p.channels = ch;
	p.buckets  = (frames + kFramesPerBucket - 1) / kFramesPerBucket;
	p.lo.assign(p.buckets * static_cast<size_t>(ch), 0);
	p.hi.assign(p.buckets * static_cast<size_t>(ch), 0);

	const int16_t* s = samplesOf(a);
	for (size_t b = 0; b < p.buckets; ++b)
	{
		const size_t f0 = b * kFramesPerBucket;
		const size_t f1 = std::min(frames, f0 + kFramesPerBucket);
		for (int c = 0; c < ch; ++c)
		{
			int16_t lo = std::numeric_limits<int16_t>::max();
			int16_t hi = std::numeric_limits<int16_t>::min();
			for (size_t f = f0; f < f1; ++f)
			{
				const int16_t v = s[f * ch + c];
				if (v < lo) lo = v;
				if (v > hi) hi = v;
			}
			p.lo[b * ch + c] = lo;
			p.hi[b * ch + c] = hi;
		}
	}
	return p;
}

// Min/max of one channel over [f0, f1) — the vertical extent of one screen
// column. Reads the peak table when the column spans at least a bucket, raw
// samples when zoomed in past that. The bucket path rounds the range OUT to
// bucket boundaries: at that zoom a bucket is under a pixel wide, so the
// over-inclusion is invisible and it saves the ragged-edge bookkeeping.
void columnExtent(const AudioAsset& a, const Peaks& p, size_t f0, size_t f1, int c,
                  int& lo, int& hi)
{
	lo = std::numeric_limits<int16_t>::max();
	hi = std::numeric_limits<int16_t>::min();
	const int ch = a.channels;
	if (f1 <= f0 || ch <= 0) { lo = hi = 0; return; }

	if (f1 - f0 >= kFramesPerBucket && p.buckets > 0)
	{
		const size_t b0 = f0 / kFramesPerBucket;
		const size_t b1 = std::min(p.buckets, (f1 + kFramesPerBucket - 1) / kFramesPerBucket);
		for (size_t b = b0; b < b1; ++b)
		{
			lo = std::min<int>(lo, p.lo[b * ch + c]);
			hi = std::max<int>(hi, p.hi[b * ch + c]);
		}
	}
	else
	{
		const int16_t* s = samplesOf(a);
		for (size_t f = f0; f < f1; ++f)
		{
			const int16_t v = s[f * ch + c];
			lo = std::min<int>(lo, v);
			hi = std::max<int>(hi, v);
		}
	}
	if (lo > hi) { lo = hi = 0; }
}

// ── Formatting ───────────────────────────────────────────────────────────────

void formatTime(double sec, char* buf, size_t n)
{
	if (sec < 0.0) sec = 0.0;
	const int total = static_cast<int>(sec);
	const int ms    = static_cast<int>((sec - total) * 1000.0);
	const int h     = total / 3600;
	const int m     = (total / 60) % 60;
	const int s     = total % 60;
	if (h > 0) std::snprintf(buf, n, "%d:%02d:%02d.%03d", h, m, s, ms);
	else       std::snprintf(buf, n, "%d:%02d.%03d", m, s, ms);
}

// Tick labels drop the milliseconds — a ruler wants to be read, not parsed.
void formatTimeShort(double sec, char* buf, size_t n)
{
	if (sec < 0.0) sec = 0.0;
	const int total = static_cast<int>(sec);
	const int h = total / 3600, m = (total / 60) % 60, s = total % 60;
	if (h > 0)            std::snprintf(buf, n, "%d:%02d:%02d", h, m, s);
	else if (sec < 10.0)  std::snprintf(buf, n, "%.2fs", sec);
	else                  std::snprintf(buf, n, "%d:%02d", m, s);
}

void formatBytes(size_t bytes, char* buf, size_t n)
{
	const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
	if (mb >= 1.0) std::snprintf(buf, n, "%.1f MB", mb);
	else           std::snprintf(buf, n, "%.0f KB", static_cast<double>(bytes) / 1024.0);
}

// ── Waveform canvas ──────────────────────────────────────────────────────────

// Ruler tick spacings, coarsest usable first. A tick is placed at the smallest
// step whose on-screen spacing clears kMinTickPx, so the labels never collide
// whatever the zoom.
constexpr double kTickSteps[] = { 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5,
                                  1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0,
                                  600.0, 900.0, 1800.0, 3600.0 };
constexpr float  kMinTickPx = 84.0f;
constexpr float  kRulerH    = 20.0f;

void clampView(State& st, size_t frames, float width)
{
	if (width <= 0.0f || frames == 0) return;
	// Never zoom past ~4 frames per canvas (pointless) nor out past the whole clip.
	const double maxFpp = static_cast<double>(frames) / width;
	const double minFpp = 4.0 / width;
	st.framesPerPx = std::clamp(st.framesPerPx, std::min(minFpp, maxFpp), maxFpp);
	const double span = st.framesPerPx * width;
	st.viewStart = std::clamp(st.viewStart, 0.0, std::max(0.0, static_cast<double>(frames) - span));
}

void drawWaveform(AppContext& ctx, const AudioAsset& clip, State& st, const ImVec2& size)
{
	ImDrawList*  dl     = ImGui::GetWindowDrawList();
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const ImVec2 canvas(std::max(64.0f, size.x), std::max(80.0f, size.y));
	const float  width  = canvas.x;
	const size_t frames = frameCountOf(clip);
	const double rate   = clip.sampleRate > 0 ? static_cast<double>(clip.sampleRate) : 48000.0;

	if (st.framesPerPx <= 0.0)   // first render: fit the whole clip
	{
		st.framesPerPx = static_cast<double>(frames) / width;
		st.viewStart   = 0.0;
	}
	clampView(st, frames, width);

	ImGui::InvisibleButton("##wavecanvas", canvas,
		ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
	const bool hovered = ImGui::IsItemHovered();
	const ImVec2 mouse = ImGui::GetMousePos();

	auto frameAtX = [&](float x) -> double
	{
		return st.viewStart + static_cast<double>(x - origin.x) * st.framesPerPx;
	};
	auto xAtFrame = [&](double f) -> float
	{
		return origin.x + static_cast<float>((f - st.viewStart) / st.framesPerPx);
	};

	// Wheel zooms around the cursor; shift+wheel pans. Middle-drag pans too, so
	// the pointer stays free for scrubbing.
	if (hovered && ImGui::GetIO().MouseWheel != 0.0f)
	{
		const float wheel = ImGui::GetIO().MouseWheel;
		if (ImGui::GetIO().KeyShift)
			st.viewStart -= static_cast<double>(wheel) * st.framesPerPx * 80.0;
		else
		{
			const double anchor = frameAtX(mouse.x);
			st.framesPerPx *= static_cast<double>(std::pow(0.86f, wheel));
			clampView(st, frames, width);
			// Keep the frame under the cursor under the cursor.
			st.viewStart = anchor - static_cast<double>(mouse.x - origin.x) * st.framesPerPx;
		}
		clampView(st, frames, width);
	}
	if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
	{
		st.viewStart -= static_cast<double>(ImGui::GetIO().MouseDelta.x) * st.framesPerPx;
		clampView(st, frames, width);
	}

	// Left press/drag scrubs: the playhead follows the pointer, and a clip that is
	// already playing seeks with it.
	if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		const double f = std::clamp(frameAtX(mouse.x), 0.0, static_cast<double>(frames));
		st.playhead  = static_cast<size_t>(f);
		st.scrubbing = true;
		if (st.handle && st.audio) st.audio->seekSound(st.handle, st.playhead);
	}
	else st.scrubbing = false;

	const ImVec2 br(origin.x + canvas.x, origin.y + canvas.y);
	dl->PushClipRect(origin, br, true);
	dl->AddRectFilled(origin, br, IM_COL32(20, 21, 25, 255));

	// ── Time ruler ───────────────────────────────────────────────────────────
	const double secPerPx = st.framesPerPx / rate;
	double step = kTickSteps[std::size(kTickSteps) - 1];
	for (double candidate : kTickSteps)
		if (candidate / secPerPx >= kMinTickPx) { step = candidate; break; }

	const float rulerBottom = origin.y + kRulerH;
	dl->AddRectFilled(origin, ImVec2(br.x, rulerBottom), IM_COL32(28, 29, 34, 255));
	dl->AddLine(ImVec2(origin.x, rulerBottom), ImVec2(br.x, rulerBottom), IM_COL32(255, 255, 255, 24));

	const double firstTick = std::ceil((st.viewStart / rate) / step) * step;
	const double viewEndSec = (st.viewStart + st.framesPerPx * width) / rate;
	for (double t = firstTick; t <= viewEndSec; t += step)
	{
		const float x = xAtFrame(t * rate);
		dl->AddLine(ImVec2(x, origin.y), ImVec2(x, br.y), IM_COL32(255, 255, 255, 14));
		dl->AddLine(ImVec2(x, rulerBottom - 5.0f), ImVec2(x, rulerBottom), IM_COL32(255, 255, 255, 60));
		char lbl[32];
		formatTimeShort(t, lbl, sizeof(lbl));
		dl->AddText(ImVec2(x + 4.0f, origin.y + 3.0f), IM_COL32(190, 195, 205, 255), lbl);
	}

	// ── Channel lanes ────────────────────────────────────────────────────────
	const int   ch     = std::max(1, clip.channels);
	const float laneH  = (canvas.y - kRulerH) / static_cast<float>(ch);
	const ImU32 fill   = IM_COL32(110, 200, 255, 205);
	const ImU32 mid    = IM_COL32(255, 255, 255, 34);

	for (int c = 0; c < ch; ++c)
	{
		const float top    = rulerBottom + laneH * static_cast<float>(c);
		const float centre = top + laneH * 0.5f;
		const float halfH  = laneH * 0.45f;

		if (c > 0)
			dl->AddLine(ImVec2(origin.x, top), ImVec2(br.x, top), IM_COL32(255, 255, 255, 18));
		dl->AddLine(ImVec2(origin.x, centre), ImVec2(br.x, centre), mid);

		for (int px = 0; px < static_cast<int>(width); ++px)
		{
			const double fa = st.viewStart + static_cast<double>(px)       * st.framesPerPx;
			const double fb = st.viewStart + static_cast<double>(px + 1.0) * st.framesPerPx;
			if (fb <= 0.0 || fa >= static_cast<double>(frames)) continue;
			const size_t f0 = static_cast<size_t>(std::max(0.0, fa));
			const size_t f1 = std::min(frames, static_cast<size_t>(std::max(fa + 1.0, fb)));
			if (f1 <= f0) continue;

			int lo = 0, hi = 0;
			columnExtent(clip, st.peaks, f0, f1, c, lo, hi);
			const float x  = origin.x + static_cast<float>(px) + 0.5f;
			const float y0 = centre - static_cast<float>(hi) / 32768.0f * halfH;
			const float y1 = centre - static_cast<float>(lo) / 32768.0f * halfH;
			// A near-flat column would round to nothing; give it a hairline so
			// silence still reads as a line rather than as a gap in the clip.
			dl->AddLine(ImVec2(x, y0), ImVec2(x, std::max(y1, y0 + 1.0f)), fill);
		}
	}

	// ── Playhead ─────────────────────────────────────────────────────────────
	{
		const float x = xAtFrame(static_cast<double>(st.playhead));
		if (x >= origin.x - 1.0f && x <= br.x + 1.0f)
		{
			dl->AddLine(ImVec2(x, origin.y), ImVec2(x, br.y), IM_COL32(255, 190, 90, 230), 1.5f);
			dl->AddTriangleFilled(ImVec2(x - 5.0f, origin.y), ImVec2(x + 5.0f, origin.y),
			                      ImVec2(x, origin.y + 7.0f), IM_COL32(255, 190, 90, 255));
		}
	}
	dl->PopClipRect();
	dl->AddRect(origin, br, IM_COL32(255, 255, 255, 26));

	(void)ctx;
}

// ── Transport ────────────────────────────────────────────────────────────────

void stopPreview(State& st)
{
	if (st.audio && st.handle) st.audio->stop(st.handle);
	st.handle = 0;
	st.paused = false;
}

void startPreview(AppContext& ctx, const AudioAsset& clip, State& st)
{
	if (!st.audio || !st.audio->isInitialized()) return;
	stopPreview(st);

	const size_t frames = frameCountOf(clip);
	if (st.playhead >= frames) st.playhead = 0;   // Play after the end restarts

	st.handle = st.audio->play(clip.audioData, clip.sampleRate, clip.channels,
	                           st.volume, st.pitch, st.loop, {});
	if (!st.handle)
	{
		HE_LOG_ERROR(Editor, "%s", ("Audio preview failed to start for " + st.name).c_str());
		return;
	}
	if (st.playhead > 0) st.audio->seekSound(st.handle, st.playhead);
	(void)ctx;
}

} // namespace

// ── Public API ───────────────────────────────────────────────────────────────

bool isAudioAsset(const std::string& path)
{
	std::string ext = std::filesystem::path(path).extension().string();
	for (auto& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
	if (ext == ".wav") return true;
	return EditorAssetTypeCache::is(path, HE::AssetType::Audio);
}

void forget(const std::string& assetPath)
{
	if (State* st = s_states.find(assetPath)) stopPreview(*st);
	s_states.forget(assetPath);
}

void render(AppContext& ctx, const std::string& assetPath, const ImVec2& pos, const ImVec2& size)
{
	State& st = s_states[assetPath];
	st.audio  = ctx.audioEngine;

	if (!st.loaded)
	{
		std::string ext = std::filesystem::path(assetPath).extension().string();
		for (auto& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
		st.isRawWav = (ext == ".wav");

		if (st.isRawWav)
		{
			// Decoded into the panel, never registered: a source .wav is not an
			// asset, and the ContentManager addresses assets by UUID.
			st.name    = std::filesystem::path(assetPath).filename().string();
			st.relPath = ctx.contentManager
				? ctx.contentManager->toContentRelativePath(assetPath) : assetPath;
			st.decodeFailed = !AudioImporter::decode(assetPath, st.raw);
		}
		else if (ctx.contentManager)
		{
			st.clipId = openPanelAsset(ctx, assetPath, st.name, st.relPath);
		}
		st.loaded = true;
	}

	ImGui::SetCursorScreenPos(pos);
	ImGui::BeginChild("##audioEditorRoot", size, false);

	const AudioAsset* clip = clipOf(ctx, st);
	// A zero sample rate is as unusable as no samples at all — every time readout
	// below divides by it — and the importer always writes a real one, so a clip
	// without one is a broken asset rather than a case to render around.
	if (!clip || clip->channels <= 0 || clip->sampleRate <= 0 || frameCountOf(*clip) == 0)
	{
		ImGui::TextDisabled(st.decodeFailed
			? "Could not decode '%s'. Only uncompressed WAV is supported "
			  "(mp3/ogg/flac have no decoder linked in)."
			: "Could not load '%s' as an audio clip.", st.name.c_str());
		ImGui::EndChild();
		return;
	}

	if (!st.peaksDone)    { st.peaks    = buildPeaks(*clip); st.peaksDone    = true; }
	if (!st.analysisDone) { st.analysis = analyze(*clip);    st.analysisDone = true; }

	const Analysis& an     = st.analysis;
	const size_t    frames = an.frames;
	const double    rate   = static_cast<double>(clip->sampleRate);
	const bool      audioReady = st.audio && st.audio->isInitialized();

	// ── Follow the running voice ─────────────────────────────────────────────
	// A paused voice is alive but not playing, so it must be excluded from the
	// finished-voice reaping below — otherwise Pause would free the clip and the
	// next Resume would have nothing to resume.
	if (st.handle && audioReady && !st.paused)
	{
		if (st.audio->isPlaying(st.handle))
		{
			if (!st.scrubbing) st.playhead = st.audio->getSoundCursorFrames(st.handle);
		}
		else
		{
			// Ran off the end (a non-looping clip). Park the playhead there and
			// give the PCM copy back — these clips are tens of megabytes.
			st.playhead = frames;
			stopPreview(st);
		}
	}

	// ── Toolbar ──────────────────────────────────────────────────────────────
	{
		namespace T = EditorToolbar;
		T::Bar bar;
		bar.group();
		bar.readout(T::iconWave, st.name.c_str());
		bar.endGroup();

		bar.group();
		bar.readout(nullptr, st.relPath.c_str(), T::kFgDim);
		if (st.isRawWav)
		{
			bar.divider();
			bar.readout(nullptr, "source .wav — not imported", T::kFgDim);
		}
		bar.endGroup();

		bar.rightGroup(bar.iconGroupWidth(4));
		const bool playing = st.handle != 0 && !st.paused;
		if (bar.item("##audioplay", playing ? T::iconPause : T::iconPlay, nullptr,
		             playing, audioReady,
		             audioReady ? "Play / Pause (from the playhead)"
		                        : "No audio device — the editor's audio engine failed to start"))
		{
			if (!st.handle)          startPreview(ctx, *clip, st);
			else if (st.paused)      { st.audio->resumeSound(st.handle); st.paused = false; }
			else                     { st.audio->pauseSound(st.handle);  st.paused = true;  }
		}
		if (bar.item("##audiostop", T::iconStop, nullptr, false, st.handle != 0, "Stop and rewind"))
		{
			stopPreview(st);
			st.playhead = 0;
		}
		if (bar.item("##audioloop", T::iconRefresh, nullptr, st.loop, true,
		             "Loop the clip — the seam check below says whether it will click"))
		{
			st.loop = !st.loop;
			if (st.handle) st.audio->setSoundLooping(st.handle, st.loop);
		}
		if (bar.item("##audiofit", T::iconFit, nullptr, false, true, "Fit the whole clip"))
		{
			st.framesPerPx = 0.0;   // refitted on the next draw, which knows the width
			st.viewStart   = 0.0;
		}
		bar.endGroup();
	}

	// ── Left: format, levels, loop ───────────────────────────────────────────
	ImGui::BeginChild("##audioInfo", ImVec2(290.0f, 0.0f), true);
	char buf[64];

	ImGui::SeparatorText("Format");
	formatTime(an.durationSec, buf, sizeof(buf));
	ImGui::Text("Duration    %s", buf);
	ImGui::Text("Sample rate %d Hz", clip->sampleRate);
	ImGui::Text("Channels    %d%s", clip->channels,
	            clip->channels == 1 ? " (mono)" : clip->channels == 2 ? " (stereo)" : "");
	ImGui::Text("Frames      %zu", frames);
	formatBytes(clip->audioData.size(), buf, sizeof(buf));
	ImGui::Text("PCM in RAM  %s (int16)", buf);

	ImGui::SeparatorText("Levels");
	ImGui::Text("Peak        %.1f dBFS", an.peakDb);
	ImGui::Text("RMS         %.1f dBFS", an.rmsDb);
	if (an.clipped > 0)
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%zu clipped samples", an.clipped);
	else
		ImGui::TextDisabled("No clipping.");
	if (std::fabs(an.dcOffset) > 0.005f)
	{
		ImGui::TextColored(ImVec4(0.9f, 0.85f, 0.4f, 1.0f), "DC offset %.3f", an.dcOffset);
		ImGui::TextWrapped("The waveform sits off centre. It wastes headroom and can thump "
		                   "when the sound starts or stops.");
	}
	if (an.peakDb < -12.0f)
		ImGui::TextWrapped("Quiet master — %.1f dB of headroom is left unused.", -an.peakDb);

	ImGui::SeparatorText("Silence");
	if (an.leadSilenceSec > 0.01 || an.tailSilenceSec > 0.01)
	{
		ImGui::Text("Head        %.2f s", an.leadSilenceSec);
		ImGui::Text("Tail        %.2f s", an.tailSilenceSec);
		ImGui::TextWrapped("Padding below %.0f dBFS. Harmless for a one-shot, but it is a "
		                   "gap in a loop.", kSilenceDb);
	}
	else
		ImGui::TextDisabled("None at either end.");

	ImGui::SeparatorText("Loop seam");
	// What matters for the ambience beds this tab was built for: what the wrap
	// from the last frame back to the first actually sounds like.
	if (an.seamStepDb > -30.0f)
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "Step  %.1f dBFS — audible click", an.seamStepDb);
	else if (an.seamStepDb > -50.0f)
		ImGui::TextColored(ImVec4(0.9f, 0.85f, 0.4f, 1.0f), "Step  %.1f dBFS — faint tick", an.seamStepDb);
	else
		ImGui::Text("Step  %.1f dBFS — clean", an.seamStepDb);
	ImGui::Text("Level %+.1f dB head vs tail", an.seamLevelDb);
	if (std::fabs(an.seamLevelDb) > 3.0f)
		ImGui::TextWrapped("The two ends sit at different loudness — the loop will breathe "
		                   "even without a click. A crossfade fixes both.");

	ImGui::SeparatorText("Preview");
	if (!audioReady)
		ImGui::TextDisabled("No audio device.");
	if (ImGui::SliderFloat("Volume", &st.volume, 0.0f, 2.0f, "%.2f") && st.handle)
		st.audio->setSoundVolume(st.handle, st.volume);
	if (ImGui::SliderFloat("Pitch", &st.pitch, 0.25f, 2.0f, "%.2f") && st.handle)
		st.audio->setSoundPitch(st.handle, st.pitch);
	ImGui::TextDisabled("Preview only — an Audio Source component\ncarries its own volume and pitch.");

	// ── Import, for a raw .wav ───────────────────────────────────────────────
	if (st.isRawWav && ctx.contentManager)
	{
		ImGui::SeparatorText("Import");
		const std::filesystem::path src(assetPath);
		const bool engineLocked = ctx.contentManager->isEngineDefaultPath(assetPath) &&
		                          !ContentManager::isEngineContentDevMode();

		// Where the .hasset lands. Normally next to the source; for a locked engine
		// .wav there is no writable spot beside it, so it goes to the project's own
		// Content/Audio — which is somewhere the project can actually reference.
		std::filesystem::path root, relDir;
		if (engineLocked)
		{
			root   = ctx.contentManager->contentRoot();
			relDir = "Audio";
		}
		else
		{
			root = ctx.contentManager->isEngineDefaultPath(assetPath)
				? std::filesystem::path(ctx.contentManager->engineContentRoot())
				: std::filesystem::path(ctx.contentManager->contentRoot());
			std::error_code ec;
			relDir = std::filesystem::relative(src.parent_path(), root, ec);
			if (ec || relDir == ".") relDir.clear();
		}

		const std::string target =
			(relDir.empty() ? src.stem().string() : (relDir / src.stem()).string()) + ".hasset";

		if (root.empty())
			ImGui::TextDisabled("Open a project to import.");
		else
		{
			if (ImGui::Button("Import as Audio Asset", ImVec2(-FLT_MIN, 0.0f)))
			{
				if (AudioImporter::import(src, root, relDir))
					ctx.contentRefreshPending = true;
				else
					HE_LOG_ERROR(Editor, "%s", ("Editor: audio import failed for " + assetPath).c_str());
			}
			ImGui::TextWrapped("Writes %s", target.c_str());
			if (engineLocked)
				ImGui::TextWrapped("Engine content is read-only, so this goes to the project "
				                   "instead. Set HE_ENGINE_CONTENT_EDITABLE=1 to import into "
				                   "the engine library itself.");
			formatBytes(clip->audioData.size(), buf, sizeof(buf));
			ImGui::TextDisabled("The asset stores decoded int16 PCM (%s) and ships with every "
			                    "packaged build.", buf);
		}
	}
	ImGui::EndChild();

	// ── Right: waveform + position readout ───────────────────────────────────
	ImGui::SameLine();
	ImGui::BeginChild("##audioWave", ImVec2(0.0f, 0.0f), true);
	{
		const float  footerH = ImGui::GetFrameHeightWithSpacing();
		const ImVec2 avail   = ImGui::GetContentRegionAvail();
		// Captured before the footer is drawn: the "visible" readout describes the
		// CANVAS, and asking for the region again after a SameLine would measure
		// what is left of the footer row instead.
		const float  canvasW = avail.x;
		drawWaveform(ctx, *clip, st, ImVec2(canvasW, std::max(80.0f, avail.y - footerH)));

		char now[32], total[32];
		formatTime(static_cast<double>(st.playhead) / rate, now, sizeof(now));
		formatTime(an.durationSec, total, sizeof(total));
		ImGui::Text("%s / %s", now, total);
		ImGui::SameLine();
		const double visibleSec = st.framesPerPx * canvasW / rate;
		ImGui::TextDisabled("   |   %.3g s visible   |   drag to scrub, wheel to zoom, "
		                    "middle-drag to pan", visibleSec);
	}
	ImGui::EndChild();

	ImGui::EndChild();
}

} // namespace AudioEditorPanel
