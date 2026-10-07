#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#if __has_include(<imgui.h>)
#include <imgui.h>
#endif

// ── The Audio Editor's waveform: peaks, view, selection ──────────────────────
// What the Audio Editor tab shows of a clip: a ruler, one lane per channel with
// the min/max outline of the samples, a playhead, a selected range, and under it
// an overview of the whole clip that doubles as the scrollbar.
//
// Kept free of AppContext and of the AudioEngine on purpose, the way
// SequencerTimeline is: the tab (AudioEditorPanel) owns loading, decoding and
// the transport, and hands the samples and the view in here. That is what lets
// the headless tests draw the real waveform over a synthetic clip, drag a
// selection across it and read the frames back, without an audio device and
// without a project.
//
// Two rules hold everywhere below:
//
//  * Frames, not samples, not seconds. A frame is one sample per channel; the
//    playhead, the selection and the view all count in frames of the clip as
//    stored, so `frame / sampleRate` is seconds and nothing drifts at the edges.
//  * Nothing reads every sample per frame. The waveform is drawn from a peak
//    pyramid built once per clip (buildPeaks): each screen column asks the
//    coarsest level whose bucket still fits inside it, so a column costs a
//    handful of reads at any zoom. Only once a column is narrower than the
//    finest bucket does it read the samples themselves — and then there are
//    at most a few hundred of them per column.
namespace HE::Ed::AudioWave
{
	// The clip as the view sees it: interleaved int16 PCM. The panel decodes a
	// Vorbis clip into exactly this once, so every clip looks the same here.
	struct Clip
	{
		const int16_t* samples    = nullptr;
		size_t         frames     = 0;
		int            channels   = 0;
		int            sampleRate = 0;

		bool valid() const { return samples && frames > 0 && channels > 0 && sampleRate > 0; }
		double seconds() const
		{
			return sampleRate > 0 ? double(frames) / double(sampleRate) : 0.0;
		}
	};

	// ── Peak pyramid ─────────────────────────────────────────────────────────
	// Level 0 holds one min/max pair per kBaseBucket frames per channel; every
	// level above it merges kLevelFactor buckets of the one below. 256 frames
	// per bucket costs ~0.4 % of the PCM (a 20-minute stereo bed: well under a
	// megabyte for all levels together), and the factor of 16 means a column
	// never reads more than ~2×16 buckets, however far you zoom out.
	constexpr size_t kBaseBucket  = 256;
	constexpr size_t kLevelFactor = 16;

	struct PeakLevel
	{
		size_t framesPerBucket = 0;
		size_t buckets         = 0;
		// Indexed [bucket * channels + channel]. int16 because the samples are.
		std::vector<int16_t> lo, hi;
	};

	struct Peaks
	{
		int                    channels = 0;
		size_t                 frames   = 0;
		std::vector<PeakLevel> levels;   // [0] finest; empty for an empty clip

		bool empty() const { return levels.empty(); }
	};

	// The whole pyramid, in one pass over the samples plus a pass per level over
	// the level below. Levels stop once one bucket covers the clip.
	Peaks buildPeaks(const Clip& clip);

	// Min/max of channel `c` over frames [f0, f1) — the vertical extent of one
	// screen column. Uses the coarsest level whose bucket fits in the range and
	// rounds the range OUT to that level's bucket edges (a bucket narrower than
	// the column is not worth the ragged-edge bookkeeping); reads raw samples
	// when the range is shorter than kBaseBucket. An empty range gives 0/0.
	// `reads`, when given, is incremented by the number of buckets or samples
	// it touched — the test's witness that a column stays cheap.
	void columnExtent(const Clip& clip, const Peaks& peaks, size_t f0, size_t f1, int c,
	                  int& lo, int& hi, size_t* reads = nullptr);

	// ── View ─────────────────────────────────────────────────────────────────
	// Where the canvas looks and what is marked on it. Owned by the panel's
	// per-tab state, read and written by draw() below.
	struct View
	{
		double viewStart   = 0.0;   // leftmost frame on the canvas
		double framesPerPx = 0.0;   // 0 = not fitted yet (the next draw fits the clip)

		size_t playhead = 0;        // frames; survives Stop, decides where Play starts

		// The selection, [selBegin, selEnd) in frames. Empty (begin == end) is
		// "nothing selected" — a click without a drag is a seek, not a
		// zero-length selection.
		size_t selBegin = 0;
		size_t selEnd   = 0;

		bool rulerInSamples = false;   // ruler labels: time (default) or frame numbers

		// Interaction in flight; not meant to be set from outside. Grabbing a
		// selection edge is a Select drag anchored on the OTHER edge, so the two
		// edges may cross mid-drag without a special case.
		enum class Drag { None, Scrub, Select, Overview };
		Drag   drag         = Drag::None;
		size_t dragAnchor   = 0;      // frame the selection drag is anchored on
		float  pressX       = 0.0f;   // pointer x at the press, to tell click from drag
		bool   dragMoved    = false;  // the press has become a drag (or started as one)
		double overviewGrab = 0.0;    // frames between the view's start and the grab point
		size_t pressSelBegin = 0, pressSelEnd = 0;   // the selection at the press, to tell a real change

		bool   hasSelection() const { return selEnd > selBegin; }
		size_t selectionLength() const { return hasSelection() ? selEnd - selBegin : 0; }
	};

	// Smallest span the canvas zooms in to, in frames across the whole canvas.
	// Past that a pixel is several frames wide and nothing new appears.
	constexpr double kMinVisibleFrames = 32.0;

	// The whole clip across `width` pixels, from the start.
	void fit(View& v, size_t frames, float width);
	// Keep the zoom between kMinVisibleFrames and the whole clip, and the view
	// inside the clip. A view that was never fitted gets fitted.
	void clampView(View& v, size_t frames, float width);
	// Zoom by `factor` (>1 = closer) keeping the frame under canvas x `anchorPx`
	// (pixels from the canvas' left edge) under it.
	void zoomAt(View& v, size_t frames, float width, double factor, float anchorPx);
	// Frame span [f0, f1) across the canvas, with a sliver of margin either side
	// so the edges of the range stay visible. Range must be non-empty.
	void zoomToRange(View& v, size_t frames, float width, size_t f0, size_t f1);
	// Scroll so `frame` is on screen, leaving the zoom alone. Used to follow
	// the playhead during playback.
	void reveal(View& v, size_t frames, float width, size_t frame);

	double frameAtPx(const View& v, float px);   // px from the canvas' left edge
	float  pxAtFrame(const View& v, double frame);

	// Set the selection from two frames in either order, clamped to the clip.
	void   select(View& v, size_t a, size_t b, size_t frames);
	void   clearSelection(View& v);

	// What Play plays: the selection if there is one, else the whole clip.
	struct PlayRange { size_t begin = 0, end = 0, start = 0; };
	// `start` is where in the range playback begins: the playhead when it sits
	// inside the range, else the range's start. A playhead parked at the very
	// end (the last play ran off it) restarts from the beginning.
	PlayRange playRange(const View& v, size_t frames);

	// ── Formatting ───────────────────────────────────────────────────────────
	// "1:02.345", "1:02:03.456" past an hour.
	std::string formatTime(double sec);
	// Tick labels drop the milliseconds where the step allows it — a ruler
	// wants to be read, not parsed.
	std::string formatTimeShort(double sec, double step);
	// A frame count with grouping: "1,234,567". Seven-digit sample numbers are
	// unreadable without it.
	std::string formatFrames(size_t frames);
	// The line under the canvas: playhead, selection, pointer — each as time
	// AND frame number. `hoverFrame` < 0 leaves the pointer out.
	std::string readout(const View& v, const Clip& clip, double hoverFrame);

#if __has_include(<imgui.h>)
	// What draw() did this frame that the panel has to act on — the transport
	// lives there, not here.
	struct Result
	{
		bool   seek = false;              // playhead moved by the pointer (scrub / click)
		bool   selectionChanged = false;  // a selection drag finished, or one was cleared
		double hoverFrame = -1.0;         // frame under the pointer, -1 when not over the canvas
		float  canvasW = 0.0f;            // canvas width the view was laid out for
	};

	// Heights of the strips, in pixels.
	struct Metrics
	{
		float rulerH    = 20.0f;
		float overviewH = 26.0f;
	};
	const Metrics& metrics();

	// Draws at the cursor and takes `size` (ruler + lanes + overview).
	//
	// Pointer grammar:
	//  * Ruler: press and drag scrubs — the playhead follows the pointer.
	//  * Lanes: drag marks a selection; a click without a drag puts the
	//    playhead there and clears the selection; shift-click stretches the
	//    selection (or makes one from the playhead) to the pointer; pressing on
	//    a selection edge grabs that edge.
	//  * Overview strip: click or drag moves the visible window.
	//  * Wheel zooms around the pointer, shift+wheel pans; with `trackpad`
	//    the swipe pans and Cmd/Ctrl+scroll zooms. Middle-drag pans in both.
	Result draw(const Clip& clip, const Peaks& peaks, View& view, const ImVec2& size,
	            bool trackpad);
#endif
}
