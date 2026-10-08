#include "doctest.h"

#include "AudioWaveformView.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

// ── The Audio Editor's waveform, without a window or an audio device ─────────
// Two halves. The model — peak pyramid, zoom and scroll, selection, what Play
// plays — is plain arithmetic over a View and is checked directly. The canvas
// is then driven through real ImGui mouse events, the way test_sequencer_
// timeline.cpp drives the Sequencer's strip: "does a drag across the lanes
// select exactly the frames under the pointer" is a question about hit tests,
// and the answer should not need anyone to look.

using namespace HE::Ed::AudioWave;

namespace
{
	// A clip the tests can reason about: a sine whose amplitude ramps up over
	// the clip, so every stretch has a different peak and a pyramid that merged
	// the wrong buckets reads visibly wrong.
	struct TestClip
	{
		std::vector<int16_t> pcm;
		Clip clip;

		TestClip(size_t frames, int channels, int rate)
		{
			pcm.resize(frames * size_t(channels));
			for (size_t f = 0; f < frames; ++f)
				for (int c = 0; c < channels; ++c)
				{
					const double amp = 0.05 + 0.9 * double(f) / double(frames);
					const double v   = amp * std::sin(double(f) * (0.013 + 0.007 * c));
					pcm[f * size_t(channels) + size_t(c)] = int16_t(std::lround(v * 32767.0));
				}
			clip.samples    = pcm.data();
			clip.frames     = frames;
			clip.channels   = channels;
			clip.sampleRate = rate;
		}
	};

	void bruteExtent(const Clip& clip, size_t f0, size_t f1, int c, int& lo, int& hi)
	{
		lo = 32767; hi = -32768;
		for (size_t f = f0; f < f1; ++f)
		{
			const int v = clip.samples[f * size_t(clip.channels) + size_t(c)];
			lo = std::min(lo, v);
			hi = std::max(hi, v);
		}
	}
}

TEST_CASE("audio waveform: the peak pyramid matches the samples at every level")
{
	TestClip t(300'000, 2, 48000);
	const Peaks p = buildPeaks(t.clip);
	REQUIRE_FALSE(p.empty());
	CHECK(p.channels == 2);
	CHECK(p.levels[0].framesPerBucket == kBaseBucket);
	CHECK(p.levels[0].buckets == (300'000 + kBaseBucket - 1) / kBaseBucket);
	// Each level is kLevelFactor coarser, and the top one covers the clip.
	for (size_t i = 1; i < p.levels.size(); ++i)
		CHECK(p.levels[i].framesPerBucket == p.levels[i - 1].framesPerBucket * kLevelFactor);
	CHECK(p.levels.back().buckets == 1);

	// The top bucket is the whole clip's extent.
	int lo = 0, hi = 0;
	bruteExtent(t.clip, 0, t.clip.frames, 1, lo, hi);
	CHECK(int(p.levels.back().lo[1]) == lo);
	CHECK(int(p.levels.back().hi[1]) == hi);

	// columnExtent over random ranges equals the brute-force min/max over the
	// range rounded out to the bucket edges of the level it chose.
	std::mt19937 rng(7);
	for (int i = 0; i < 400; ++i)
	{
		const size_t f0  = rng() % t.clip.frames;
		const size_t len = 1 + rng() % std::min<size_t>(t.clip.frames - f0, (i % 4 == 0) ? 200 : 200'000);
		const size_t f1  = f0 + len;
		const int    c   = int(rng() % 2);

		size_t fpb = 0;
		for (const PeakLevel& L : p.levels)
			if (L.framesPerBucket <= len) fpb = L.framesPerBucket;
		const size_t r0 = fpb ? (f0 / fpb) * fpb : f0;
		const size_t r1 = fpb ? std::min(t.clip.frames, ((f1 + fpb - 1) / fpb) * fpb) : f1;

		int elo = 0, ehi = 0, glo = 0, ghi = 0;
		bruteExtent(t.clip, r0, r1, c, elo, ehi);
		columnExtent(t.clip, p, f0, f1, c, glo, ghi);
		CHECK(glo == elo);
		CHECK(ghi == ehi);
	}

	// Below the finest bucket the samples themselves are read: exact.
	bruteExtent(t.clip, 1000, 1100, 0, lo, hi);
	int glo = 0, ghi = 0;
	columnExtent(t.clip, p, 1000, 1100, 0, glo, ghi);
	CHECK(glo == lo);
	CHECK(ghi == hi);

	// Nothing to read: 0/0, and out-of-range channels do not crash.
	columnExtent(t.clip, p, 50, 50, 0, glo, ghi);
	CHECK((glo == 0 && ghi == 0));
	columnExtent(t.clip, p, 0, 100, 5, glo, ghi);
	CHECK((glo == 0 && ghi == 0));
}

TEST_CASE("audio waveform: a whole canvas costs a few reads per column, not the samples")
{
	// ~87 s of mono at 48 kHz: 4.2 million samples. A canvas that touched them
	// all per frame is exactly what the pyramid exists to avoid.
	TestClip t(size_t(1) << 22, 1, 48000);
	const Peaks p = buildPeaks(t.clip);

	for (float width : { 300.0f, 1000.0f, 1700.0f })
	{
		View v;
		fit(v, t.clip.frames, width);
		size_t reads = 0;
		for (int px = 0; px < int(width); ++px)
		{
			const size_t f0 = size_t(frameAtPx(v, float(px)));
			const size_t f1 = std::min(t.clip.frames, size_t(frameAtPx(v, float(px + 1))));
			int lo, hi;
			columnExtent(t.clip, p, f0, f1, 0, lo, hi, &reads);
		}
		INFO("width " << width << ": " << reads << " reads");
		// At most kLevelFactor buckets per column plus the two partial ones.
		CHECK(reads <= size_t(width) * (kLevelFactor + 2));
		CHECK(reads < t.clip.frames / 100);
	}

	// Zoomed in to 1000 frames across the canvas, the raw path reads ~1000.
	View v;
	fit(v, t.clip.frames, 1000.0f);
	zoomToRange(v, t.clip.frames, 1000.0f, 2'000'000, 2'001'000);
	size_t reads = 0;
	for (int px = 0; px < 1000; ++px)
	{
		const double fa = frameAtPx(v, float(px)), fb = frameAtPx(v, float(px + 1));
		int lo, hi;
		columnExtent(t.clip, p, size_t(fa), size_t(std::max(fa + 1.0, fb)), 0, lo, hi, &reads);
	}
	CHECK(reads < 3000);
}

TEST_CASE("audio waveform: zoom keeps the frame under the pointer, and the view in the clip")
{
	const size_t frames = 480'000;
	const float  W      = 800.0f;
	View v;
	clampView(v, frames, W);   // never fitted → fitted
	CHECK(v.framesPerPx == doctest::Approx(double(frames) / W));
	CHECK(v.viewStart == 0.0);

	const float  anchorPx = 523.0f;
	const double before   = frameAtPx(v, anchorPx);
	zoomAt(v, frames, W, 4.0, anchorPx);
	CHECK(v.framesPerPx == doctest::Approx(double(frames) / W / 4.0));
	CHECK(frameAtPx(v, anchorPx) == doctest::Approx(before).epsilon(1e-9));

	// Zooming out past the whole clip stops at the whole clip, from the start.
	zoomAt(v, frames, W, 1e-6, 100.0f);
	CHECK(v.framesPerPx == doctest::Approx(double(frames) / W));
	CHECK(v.viewStart == 0.0);

	// Zooming in stops at kMinVisibleFrames across the canvas.
	zoomAt(v, frames, W, 1e9, 400.0f);
	CHECK(v.framesPerPx * W == doctest::Approx(kMinVisibleFrames));

	// Scrolled past the end comes back to the last full canvas.
	v.viewStart = 1e12;
	clampView(v, frames, W);
	CHECK(v.viewStart + v.framesPerPx * W == doctest::Approx(double(frames)));

	// Zoom to a range: all of it on screen, centred.
	zoomToRange(v, frames, W, 100'000, 148'000);
	CHECK(v.viewStart <= 100'000.0);
	CHECK(v.viewStart + v.framesPerPx * W >= 148'000.0);
	CHECK(frameAtPx(v, W * 0.5f) == doctest::Approx(124'000.0).epsilon(1e-6));

	// reveal: a frame off screen pulls the view to it, one on screen does not.
	const double start = v.viewStart;
	reveal(v, frames, W, 120'000);
	CHECK(v.viewStart == start);
	reveal(v, frames, W, 400'000);
	CHECK(pxAtFrame(v, 400'000.0) >= 0.0f);
	CHECK(pxAtFrame(v, 400'000.0) <= W);
}

TEST_CASE("audio waveform: selection and what Play plays")
{
	const size_t frames = 96'000;
	View v;
	CHECK_FALSE(v.hasSelection());

	// Either order, clamped to the clip.
	select(v, 70'000, 20'000, frames);
	CHECK(v.selBegin == 20'000);
	CHECK(v.selEnd == 70'000);
	CHECK(v.selectionLength() == 50'000);
	select(v, 90'000, 500'000, frames);
	CHECK(v.selEnd == frames);

	// No selection: the whole clip, from the playhead.
	clearSelection(v);
	v.playhead = 30'000;
	PlayRange r = playRange(v, frames);
	CHECK(r.begin == 0);
	CHECK(r.end == frames);
	CHECK(r.start == 30'000);

	// A playhead parked at the very end (the last play ran off it) restarts.
	v.playhead = frames;
	CHECK(playRange(v, frames).start == 0);

	// A selection: just the selection, from the playhead when it is inside …
	select(v, 20'000, 70'000, frames);
	v.playhead = 50'000;
	r = playRange(v, frames);
	CHECK(r.begin == 20'000);
	CHECK(r.end == 70'000);
	CHECK(r.start == 50'000);
	// … and from its start when the playhead is outside or at its end.
	v.playhead = 5'000;
	CHECK(playRange(v, frames).start == 20'000);
	v.playhead = 70'000;
	CHECK(playRange(v, frames).start == 20'000);
}

TEST_CASE("audio waveform: a trim is what Play plays when nothing is selected")
{
	const size_t frames = 96'000;
	View v;
	CHECK_FALSE(v.hasTrim());

	// Trimmed to [12'000, 60'000): Play without a selection plays that range,
	// as a voice of the asset in the game does.
	v.trimBegin = 12'000;
	v.trimEnd   = 60'000;
	REQUIRE(v.hasTrim());
	v.playhead = 30'000;
	PlayRange r = playRange(v, frames);
	CHECK(r.begin == 12'000);
	CHECK(r.end == 60'000);
	CHECK(r.start == 30'000);
	// A playhead in the cut-away part starts at the trim's start.
	v.playhead = 70'000;
	CHECK(playRange(v, frames).start == 12'000);
	v.playhead = 1'000;
	CHECK(playRange(v, frames).start == 12'000);

	// A selection still wins — even one outside the trim: auditioning a part
	// that was cut away is how you decide to bring it back.
	select(v, 70'000, 80'000, frames);
	r = playRange(v, frames);
	CHECK(r.begin == 70'000);
	CHECK(r.end == 80'000);

	// The readout says what the trim is, in time and frames.
	clearSelection(v);
	TestClip t(frames, 1, 48'000);
	const std::string s = readout(v, t.clip, -1.0);
	CHECK(s.find("Trim 0:00.250 to 0:01.250 (frames 12,000 to 60,000)") != std::string::npos);

	// Untrimmed again: the whole clip.
	v.trimBegin = v.trimEnd = 0;
	r = playRange(v, frames);
	CHECK(r.begin == 0);
	CHECK(r.end == frames);
	CHECK(readout(v, t.clip, -1.0).find("Trim ") == std::string::npos);
}

TEST_CASE("audio waveform: time and frame readouts")
{
	CHECK(formatTime(0.0) == "0:00.000");
	CHECK(formatTime(62.345) == "1:02.345");
	CHECK(formatTime(59.9996) == "1:00.000");
	CHECK(formatTime(3723.5) == "1:02:03.500");
	CHECK(formatFrames(0) == "0");
	CHECK(formatFrames(999) == "999");
	CHECK(formatFrames(48000) == "48,000");
	CHECK(formatFrames(1234567) == "1,234,567");
	CHECK(formatTimeShort(0.3, 0.1) == "0.3s");
	CHECK(formatTimeShort(0.25, 0.05) == "0.25s");
	CHECK(formatTimeShort(75.0, 5.0) == "1:15");
	CHECK(formatTimeShort(61.5, 0.5) == "1:01.5");

	TestClip t(96'000, 1, 48'000);
	View v;
	v.playhead = 24'000;
	std::string s = readout(v, t.clip, -1.0);
	CHECK(s.find("0:00.500") != std::string::npos);
	CHECK(s.find("frame 24,000") != std::string::npos);
	CHECK(s.find("No selection") != std::string::npos);
	CHECK(s.find("Pointer") == std::string::npos);

	select(v, 48'000, 72'000, t.clip.frames);
	s = readout(v, t.clip, 12'000.0);
	CHECK(s.find("Selection 0:01.000 to 0:01.500") != std::string::npos);
	CHECK(s.find("24,000 frames") != std::string::npos);
	CHECK(s.find("Pointer 0:00.250 (frame 12,000)") != std::string::npos);
}

// ── The canvas, driven ───────────────────────────────────────────────────────

namespace
{
	struct ImGuiCtx
	{
		ImGuiCtx()
		{
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(1280.0f, 720.0f);
			io.DeltaTime   = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
			// No padding: the canvas starts at the window's origin, so the
			// arithmetic below has nothing to guess at.
			ImGui::GetStyle().WindowPadding = ImVec2(0.0f, 0.0f);
		}
		~ImGuiCtx() { ImGui::DestroyContext(); }
	};

	constexpr float kW = 1000.0f, kH = 300.0f;

	Result frame(const Clip& clip, const Peaks& peaks, View& view)
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(kW, kH));
		ImGui::Begin("##awtest", nullptr,
		             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
		const Result r = draw(clip, peaks, view, ImVec2(kW, kH), false);
		ImGui::End();
		ImGui::Render();
		return r;
	}

	void mouseAt(float x, float y) { ImGui::GetIO().AddMousePosEvent(x, y); }
	void mouseButton(bool down)    { ImGui::GetIO().AddMouseButtonEvent(0, down); }

	// Mid-height of the lanes, well clear of the ruler and the overview.
	float lanesY() { return metrics().rulerH + 60.0f; }
	float rulerY() { return metrics().rulerH * 0.5f; }
	float overviewY() { return kH - metrics().overviewH * 0.5f; }
}

TEST_CASE("audio waveform: a drag across the lanes selects the frames under it")
{
	ImGuiCtx ctx;
	TestClip t(100'000, 2, 50'000);   // 2 s; fitted to 1000 px → 100 frames per px
	const Peaks p = buildPeaks(t.clip);
	View v;

	mouseAt(200.0f, lanesY()); frame(t.clip, p, v);
	frame(t.clip, p, v);
	CHECK(v.framesPerPx == doctest::Approx(100.0));

	mouseButton(true);  Result r = frame(t.clip, p, v);
	CHECK_FALSE(v.hasSelection());   // a press alone selects nothing yet
	mouseAt(500.0f, lanesY()); r = frame(t.clip, p, v);
	CHECK(v.hasSelection());
	CHECK(v.selBegin == 20'000);
	CHECK(v.selEnd == 50'000);
	CHECK_FALSE(r.selectionChanged);   // still dragging
	mouseButton(false); r = frame(t.clip, p, v);
	CHECK(r.selectionChanged);
	CHECK(v.selBegin == 20'000);
	CHECK(v.selEnd == 50'000);
	CHECK(v.drag == View::Drag::None);

	// Leftward drags select the same way round.
	mouseAt(700.0f, lanesY()); frame(t.clip, p, v);
	mouseButton(true);  frame(t.clip, p, v);
	mouseAt(650.0f, lanesY()); frame(t.clip, p, v);
	mouseButton(false); frame(t.clip, p, v);
	CHECK(v.selBegin == 65'000);
	CHECK(v.selEnd == 70'000);

	// Pressing an edge and letting go in place changes nothing — and says so,
	// or a playing selection would restart on every such press.
	mouseAt(650.0f, lanesY()); frame(t.clip, p, v);
	mouseButton(true);  frame(t.clip, p, v);
	mouseButton(false); r = frame(t.clip, p, v);
	CHECK_FALSE(r.selectionChanged);
	CHECK(v.selBegin == 65'000);
	CHECK(v.selEnd == 70'000);

	// Grab the end edge and drag it out: the start stays put.
	mouseAt(700.0f, lanesY()); frame(t.clip, p, v);
	mouseButton(true);  frame(t.clip, p, v);
	mouseAt(800.0f, lanesY()); frame(t.clip, p, v);
	mouseButton(false); frame(t.clip, p, v);
	CHECK(v.selBegin == 65'000);
	CHECK(v.selEnd == 80'000);

	// Shift-click past the end stretches the selection to the pointer.
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
	mouseAt(900.0f, lanesY()); frame(t.clip, p, v);
	mouseButton(true);  frame(t.clip, p, v);
	mouseButton(false); r = frame(t.clip, p, v);
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
	frame(t.clip, p, v);
	CHECK(v.selBegin == 65'000);
	CHECK(v.selEnd == 90'000);
	CHECK(r.selectionChanged);
}

TEST_CASE("audio waveform: a click seeks and drops the selection, the ruler only scrubs")
{
	ImGuiCtx ctx;
	TestClip t(100'000, 1, 50'000);
	const Peaks p = buildPeaks(t.clip);
	View v;
	frame(t.clip, p, v);
	select(v, 10'000, 30'000, t.clip.frames);

	// The ruler: press and drag moves the playhead, the selection stays.
	mouseAt(400.0f, rulerY()); frame(t.clip, p, v);
	mouseButton(true);  Result r = frame(t.clip, p, v);
	CHECK(r.seek);
	CHECK(v.playhead == 40'000);
	mouseAt(450.0f, rulerY()); r = frame(t.clip, p, v);
	CHECK(r.seek);
	CHECK(v.playhead == 45'000);
	mouseButton(false); frame(t.clip, p, v);
	CHECK(v.selBegin == 10'000);
	CHECK(v.selEnd == 30'000);

	// A click without a drag in the lanes: playhead there, selection gone.
	mouseAt(600.0f, lanesY()); frame(t.clip, p, v);
	mouseButton(true);  frame(t.clip, p, v);
	mouseButton(false); r = frame(t.clip, p, v);
	CHECK(r.seek);
	CHECK(r.selectionChanged);
	CHECK(v.playhead == 60'000);
	CHECK_FALSE(v.hasSelection());

	// The pointer is reported over the canvas, and only there.
	mouseAt(250.0f, lanesY()); r = frame(t.clip, p, v);
	CHECK(r.hoverFrame == doctest::Approx(25'000.0));
	mouseAt(250.0f, kH + 50.0f); r = frame(t.clip, p, v);
	CHECK(r.hoverFrame < 0.0);
}

TEST_CASE("audio waveform: the wheel zooms around the pointer and the overview scrolls")
{
	ImGuiCtx ctx;
	TestClip t(100'000, 1, 50'000);
	const Peaks p = buildPeaks(t.clip);
	View v;
	mouseAt(300.0f, lanesY()); frame(t.clip, p, v);
	frame(t.clip, p, v);
	const double under = frameAtPx(v, 300.0f);

	ImGui::GetIO().AddMouseWheelEvent(0.0f, 5.0f);
	frame(t.clip, p, v);
	CHECK(v.framesPerPx < 100.0 * 0.5);
	CHECK(frameAtPx(v, 300.0f) == doctest::Approx(under).epsilon(1e-6));

	// Click far right in the overview: the visible window centres there.
	const double span = v.framesPerPx * kW;
	mouseAt(900.0f, overviewY()); frame(t.clip, p, v);
	mouseButton(true);  frame(t.clip, p, v);
	mouseButton(false); frame(t.clip, p, v);
	const double centre = v.viewStart + span * 0.5;
	CHECK(centre == doctest::Approx(std::min(90'000.0, 100'000.0 - span * 0.5)).epsilon(1e-3));
	// The playhead and the selection are not the overview's business.
	CHECK(v.playhead == 0);
	CHECK_FALSE(v.hasSelection());
}

// ── The volume curve over the waveform (Thema 168, step 4) ───────────────────

TEST_CASE("audio waveform: the curve's dB axis, its labels and the point helpers")
{
	const float top = HE::AudioEnvelope::kMaxGain;
	CHECK(curveFracOfGain(0.0f) == 0.0f);
	CHECK(curveFracOfGain(top) == doctest::Approx(1.0f));
	CHECK(curveFracOfGain(100.0f) == doctest::Approx(1.0f));   // above the top: the top
	// Round trip over the editable range: the y a point is drawn at gives its gain back.
	for (float g : { 0.004f, 0.01f, 0.1f, 0.5f, 1.0f, 1.7f, top })
		CHECK(curveGainOfFrac(curveFracOfGain(g)) == doctest::Approx(g).epsilon(1e-4));
	// Unity sits above the middle: the room under it is for fades.
	CHECK(curveFracOfGain(1.0f) > 0.75f);
	// The bottom band is silence, so a point dragged down really goes quiet.
	CHECK(curveGainOfFrac(kCurveSilentFrac * 0.5f) == 0.0f);
	CHECK(curveGainOfFrac(-1.0f) == 0.0f);

	CHECK(formatGain(1.0f)  == "+0.0 dB (x1.00)");
	CHECK(formatGain(0.5f)  == "-6.0 dB (x0.50)");
	CHECK(formatGain(2.0f)  == "+6.0 dB (x2.00)");
	CHECK(formatGain(0.0f)  == "-inf dB (x0)");

	HE::AudioEnvelope e;
	CHECK(curveInsert(e, 1.0, 0.5f, HE::AudioCurveInterp::Linear) == 0);
	CHECK(curveInsert(e, 0.5, 1.0f, HE::AudioCurveInterp::Smooth) == 0);   // sorted in front
	CHECK(curveInsert(e, 1.0, 2.0f, HE::AudioCurveInterp::Linear) == 2);   // same time: after
	CHECK(curveInsert(e, 9.0, 99.0f, HE::AudioCurveInterp::Linear) == 3);
	CHECK(e.points[3].gain == top);                                        // clamped
	// A move is held between the neighbours, so the order never changes.
	curveMove(e, 1, 0.1, 0.25f, 10.0);
	CHECK(e.points[1].timeSec == doctest::Approx(0.5));
	curveMove(e, 3, 20.0, 1.0f, 10.0);
	CHECK(e.points[3].timeSec == doctest::Approx(10.0));                   // and inside the clip
	curveErase(e, 0);
	CHECK(e.points.size() == 3);
	curveErase(e, 7);                                                      // out of range: nothing
	CHECK(e.points.size() == 3);

	// The readout gives the curve's gain where the pointer is.
	TestClip t(48'000, 1, 48'000);
	View v;
	HE::AudioEnvelope half;
	half.points = { { 0.0, 0.5f, HE::AudioCurveInterp::Linear } };
	CHECK(readout(v, t.clip, 100.0, &half).find("Curve -6.0 dB (x0.50) at the pointer") != std::string::npos);
	CHECK(readout(v, t.clip, -1.0, &half).find("at the playhead") != std::string::npos);
	CHECK(readout(v, t.clip, 100.0, nullptr).find("Curve") == std::string::npos);
	CHECK(readout(v, t.clip, 100.0, &e).find("Curve") != std::string::npos);
}

namespace
{
	Result curveFrame(const Clip& clip, const Peaks& peaks, View& view, HE::AudioEnvelope& env, bool editable)
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(kW, kH));
		ImGui::Begin("##awtest", nullptr,
		             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
		const Result r = draw(clip, peaks, view, ImVec2(kW, kH), false, &env, editable);
		ImGui::End();
		ImGui::Render();
		return r;
	}

	// The lanes run from under the ruler to just above the overview (draw()).
	float lanesTop()    { return metrics().rulerH; }
	float lanesBottom() { return kH - metrics().overviewH - 4.0f; }
	float yOfGain(float g) { return lanesBottom() - curveFracOfGain(g) * (lanesBottom() - lanesTop()); }
}

TEST_CASE("audio waveform: in curve mode a click adds a point, a drag moves it, a right-click deletes it")
{
	ImGuiCtx ctx;
	TestClip t(100'000, 2, 50'000);   // 2 s; 100 frames per px once fitted
	const Peaks p = buildPeaks(t.clip);
	View v;
	HE::AudioEnvelope env;
	curveFrame(t.clip, p, v, env, true);
	curveFrame(t.clip, p, v, env, true);
	v.curveMode = true;

	// Click at 0.4 s, at the height of -6 dB: one point, there.
	mouseAt(200.0f, yOfGain(0.5f)); curveFrame(t.clip, p, v, env, true);
	mouseButton(true);  Result r = curveFrame(t.clip, p, v, env, true);
	CHECK(r.curveEdited);
	CHECK_FALSE(r.curveCommitted);   // still held
	REQUIRE(env.points.size() == 1);
	CHECK(env.points[0].timeSec == doctest::Approx(0.4));
	CHECK(env.points[0].gain == doctest::Approx(0.5f).epsilon(0.02));
	mouseButton(false); r = curveFrame(t.clip, p, v, env, true);
	CHECK(r.curveCommitted);         // one undo point per gesture
	CHECK_FALSE(v.hasSelection());   // the lanes did not select

	// A second point at 1.2 s, dragged on to 1.4 s and +6 dB in the same press.
	mouseAt(600.0f, yOfGain(1.0f)); curveFrame(t.clip, p, v, env, true);
	mouseButton(true);  curveFrame(t.clip, p, v, env, true);
	mouseAt(700.0f, yOfGain(2.0f)); r = curveFrame(t.clip, p, v, env, true);
	CHECK(r.curveEdited);
	mouseButton(false); r = curveFrame(t.clip, p, v, env, true);
	CHECK(r.curveCommitted);
	REQUIRE(env.points.size() == 2);
	CHECK(env.points[1].timeSec == doctest::Approx(1.4));
	CHECK(env.points[1].gain == doctest::Approx(2.0f).epsilon(0.02));
	CHECK(v.curveSel == 1);

	// Grab the first point and drag it past the second: it stops at its
	// neighbour, the order holds.
	mouseAt(200.0f, yOfGain(0.5f)); curveFrame(t.clip, p, v, env, true);
	mouseButton(true);  curveFrame(t.clip, p, v, env, true);
	mouseAt(900.0f, yOfGain(0.5f)); curveFrame(t.clip, p, v, env, true);
	mouseButton(false); curveFrame(t.clip, p, v, env, true);
	REQUIRE(env.points.size() == 2);
	CHECK(env.points[0].timeSec == doctest::Approx(1.4));
	CHECK(env.points[0].gain == doctest::Approx(0.5f).epsilon(0.02));

	// Pressing a point and letting go in place is no edit.
	mouseAt(700.0f, yOfGain(2.0f)); curveFrame(t.clip, p, v, env, true);
	mouseButton(true);  r = curveFrame(t.clip, p, v, env, true);
	CHECK_FALSE(r.curveEdited);
	mouseButton(false); r = curveFrame(t.clip, p, v, env, true);
	CHECK_FALSE(r.curveCommitted);

	// Right-click on it deletes it.
	ImGui::GetIO().AddMouseButtonEvent(1, true);  r = curveFrame(t.clip, p, v, env, true);
	ImGui::GetIO().AddMouseButtonEvent(1, false); curveFrame(t.clip, p, v, env, true);
	CHECK(r.curveCommitted);
	CHECK(env.points.size() == 1);

	// Curve mode on a curve that is shown read-only (engine content): the lanes
	// select as always, the curve is left alone.
	const HE::AudioEnvelope kept = env;
	mouseAt(300.0f, lanesY()); curveFrame(t.clip, p, v, env, false);
	mouseButton(true);  curveFrame(t.clip, p, v, env, false);
	mouseAt(400.0f, lanesY()); curveFrame(t.clip, p, v, env, false);
	mouseButton(false); r = curveFrame(t.clip, p, v, env, false);
	CHECK(r.selectionChanged);
	CHECK_FALSE(r.curveEdited);
	CHECK(env.points.size() == kept.points.size());

	// And with curve mode off, the same.
	v.curveMode = false;
	mouseAt(100.0f, yOfGain(1.0f)); curveFrame(t.clip, p, v, env, true);
	mouseButton(true);  curveFrame(t.clip, p, v, env, true);
	mouseButton(false); r = curveFrame(t.clip, p, v, env, true);
	CHECK_FALSE(r.curveEdited);
	CHECK(env.points.size() == kept.points.size());
}
