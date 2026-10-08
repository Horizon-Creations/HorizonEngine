#include "AudioWaveformView.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>

#if __has_include(<imgui.h>)
#include "EditorWidgets.h"   // helpForKey — the tooltips are queued to the frame's end
#endif

namespace HE::Ed::AudioWave
{

// ── Peak pyramid ─────────────────────────────────────────────────────────────

Peaks buildPeaks(const Clip& clip)
{
	Peaks p;
	if (!clip.valid()) return p;
	const int    ch     = clip.channels;
	const size_t frames = clip.frames;
	p.channels = ch;
	p.frames   = frames;

	// Level 0 straight from the samples.
	{
		PeakLevel L;
		L.framesPerBucket = kBaseBucket;
		L.buckets = (frames + kBaseBucket - 1) / kBaseBucket;
		L.lo.assign(L.buckets * size_t(ch), 0);
		L.hi.assign(L.buckets * size_t(ch), 0);
		for (size_t b = 0; b < L.buckets; ++b)
		{
			const size_t f0 = b * kBaseBucket;
			const size_t f1 = std::min(frames, f0 + kBaseBucket);
			for (int c = 0; c < ch; ++c)
			{
				int16_t lo = std::numeric_limits<int16_t>::max();
				int16_t hi = std::numeric_limits<int16_t>::min();
				for (size_t f = f0; f < f1; ++f)
				{
					const int16_t v = clip.samples[f * size_t(ch) + size_t(c)];
					if (v < lo) lo = v;
					if (v > hi) hi = v;
				}
				L.lo[b * size_t(ch) + size_t(c)] = lo;
				L.hi[b * size_t(ch) + size_t(c)] = hi;
			}
		}
		p.levels.push_back(std::move(L));
	}

	// Every further level from the one below — never from the samples again.
	while (p.levels.back().buckets > 1)
	{
		const PeakLevel& below = p.levels.back();
		PeakLevel L;
		L.framesPerBucket = below.framesPerBucket * kLevelFactor;
		L.buckets = (below.buckets + kLevelFactor - 1) / kLevelFactor;
		L.lo.assign(L.buckets * size_t(ch), 0);
		L.hi.assign(L.buckets * size_t(ch), 0);
		for (size_t b = 0; b < L.buckets; ++b)
		{
			const size_t b0 = b * kLevelFactor;
			const size_t b1 = std::min(below.buckets, b0 + kLevelFactor);
			for (int c = 0; c < ch; ++c)
			{
				int16_t lo = std::numeric_limits<int16_t>::max();
				int16_t hi = std::numeric_limits<int16_t>::min();
				for (size_t s = b0; s < b1; ++s)
				{
					lo = std::min(lo, below.lo[s * size_t(ch) + size_t(c)]);
					hi = std::max(hi, below.hi[s * size_t(ch) + size_t(c)]);
				}
				L.lo[b * size_t(ch) + size_t(c)] = lo;
				L.hi[b * size_t(ch) + size_t(c)] = hi;
			}
		}
		p.levels.push_back(std::move(L));
	}
	return p;
}

void columnExtent(const Clip& clip, const Peaks& peaks, size_t f0, size_t f1, int c,
                  int& lo, int& hi, size_t* reads)
{
	const int ch = clip.channels;
	f1 = std::min(f1, clip.frames);
	if (!clip.valid() || f1 <= f0 || c < 0 || c >= ch) { lo = hi = 0; return; }

	lo = std::numeric_limits<int16_t>::max();
	hi = std::numeric_limits<int16_t>::min();
	const size_t span = f1 - f0;

	// The coarsest level whose bucket is no wider than the range: a range of
	// N frames then touches at most N / bucket + 2 buckets, and the level
	// above would already be wider than the range itself.
	const PeakLevel* level = nullptr;
	for (const PeakLevel& L : peaks.levels)
	{
		if (L.framesPerBucket > span) break;
		level = &L;
	}

	if (level)
	{
		const size_t fpb = level->framesPerBucket;
		const size_t b0  = f0 / fpb;
		const size_t b1  = std::min(level->buckets, (f1 + fpb - 1) / fpb);
		for (size_t b = b0; b < b1; ++b)
		{
			lo = std::min<int>(lo, level->lo[b * size_t(ch) + size_t(c)]);
			hi = std::max<int>(hi, level->hi[b * size_t(ch) + size_t(c)]);
		}
		if (reads) *reads += b1 - b0;
	}
	else
	{
		for (size_t f = f0; f < f1; ++f)
		{
			const int v = clip.samples[f * size_t(ch) + size_t(c)];
			lo = std::min(lo, v);
			hi = std::max(hi, v);
		}
		if (reads) *reads += span;
	}
	if (lo > hi) lo = hi = 0;
}

// ── View ─────────────────────────────────────────────────────────────────────

void fit(View& v, size_t frames, float width)
{
	if (width <= 0.0f) return;
	v.framesPerPx = double(std::max<size_t>(frames, 1)) / double(width);
	v.viewStart   = 0.0;
}

void clampView(View& v, size_t frames, float width)
{
	if (width <= 0.0f) return;
	if (v.framesPerPx <= 0.0) { fit(v, frames, width); return; }
	const double whole  = double(std::max<size_t>(frames, 1));
	const double maxFpp = whole / double(width);
	const double minFpp = std::min(kMinVisibleFrames, whole) / double(width);
	v.framesPerPx = std::clamp(v.framesPerPx, std::min(minFpp, maxFpp), maxFpp);
	const double span = v.framesPerPx * double(width);
	v.viewStart = std::clamp(v.viewStart, 0.0, std::max(0.0, whole - span));
}

void zoomAt(View& v, size_t frames, float width, double factor, float anchorPx)
{
	if (width <= 0.0f || factor <= 0.0) return;
	clampView(v, frames, width);
	const double anchor = frameAtPx(v, anchorPx);
	v.framesPerPx /= factor;
	clampView(v, frames, width);
	// Keep the frame under the pointer under the pointer.
	v.viewStart = anchor - double(anchorPx) * v.framesPerPx;
	clampView(v, frames, width);
}

void zoomToRange(View& v, size_t frames, float width, size_t f0, size_t f1)
{
	if (width <= 0.0f || f1 <= f0) return;
	const double span   = double(f1 - f0);
	const double margin = span * 0.05;
	v.framesPerPx = (span + 2.0 * margin) / double(width);
	clampView(v, frames, width);
	// Centre the range in whatever span the clamp allowed.
	v.viewStart = double(f0) + span * 0.5 - v.framesPerPx * double(width) * 0.5;
	clampView(v, frames, width);
}

void reveal(View& v, size_t frames, float width, size_t frame)
{
	if (width <= 0.0f) return;
	clampView(v, frames, width);
	const double span = v.framesPerPx * double(width);
	const double f    = double(frame);
	if (f >= v.viewStart && f <= v.viewStart + span) return;
	// Page rather than glide: the playhead lands a tenth in from the left, so
	// the next page turn is a whole canvas away instead of every frame.
	v.viewStart = f - span * 0.1;
	clampView(v, frames, width);
}

double frameAtPx(const View& v, float px)
{
	return v.viewStart + double(px) * v.framesPerPx;
}

float pxAtFrame(const View& v, double frame)
{
	return v.framesPerPx > 0.0 ? float((frame - v.viewStart) / v.framesPerPx) : 0.0f;
}

void select(View& v, size_t a, size_t b, size_t frames)
{
	a = std::min(a, frames);
	b = std::min(b, frames);
	v.selBegin = std::min(a, b);
	v.selEnd   = std::max(a, b);
}

void clearSelection(View& v)
{
	v.selBegin = v.selEnd = 0;
}

PlayRange playRange(const View& v, size_t frames)
{
	PlayRange r;
	if (v.hasSelection() && v.selBegin < frames)
	{
		r.begin = v.selBegin;
		r.end   = std::min(v.selEnd, frames);
	}
	else if (v.hasTrim() && v.trimBegin < frames)
	{
		r.begin = v.trimBegin;
		r.end   = std::min(v.trimEnd, frames);
	}
	else
	{
		r.begin = 0;
		r.end   = frames;
	}
	r.start = (v.playhead >= r.begin && v.playhead < r.end) ? v.playhead : r.begin;
	return r;
}

// ── Formatting ───────────────────────────────────────────────────────────────

std::string formatTime(double sec)
{
	if (!(sec > 0.0)) sec = 0.0;
	// Rounded to the millisecond first, so 59.9996 s reads 1:00.000 rather
	// than 0:59.1000.
	const long long msTotal = std::llround(sec * 1000.0);
	const long long total   = msTotal / 1000;
	const int ms = int(msTotal % 1000);
	const int h  = int(total / 3600);
	const int m  = int((total / 60) % 60);
	const int s  = int(total % 60);
	char buf[48];
	if (h > 0) std::snprintf(buf, sizeof(buf), "%d:%02d:%02d.%03d", h, m, s, ms);
	else       std::snprintf(buf, sizeof(buf), "%d:%02d.%03d", m, s, ms);
	return buf;
}

std::string formatTimeShort(double sec, double step)
{
	if (!(sec > 0.0)) sec = 0.0;
	char buf[48];
	if (step >= 1.0)
	{
		const long long total = std::llround(sec);
		const int h = int(total / 3600), m = int((total / 60) % 60), s = int(total % 60);
		if (h > 0) std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
		else       std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
		return buf;
	}
	// Just enough decimals to tell two neighbouring ticks apart.
	const int dec = step >= 0.1 ? 1 : step >= 0.01 ? 2 : 3;
	const double scale = std::pow(10.0, dec);
	const double r     = std::round(sec * scale) / scale;
	const int    m     = int(r / 60.0);
	if (m > 0)
		std::snprintf(buf, sizeof(buf), "%d:%0*.*f", m, dec + 3, dec, r - 60.0 * m);
	else
		std::snprintf(buf, sizeof(buf), "%.*fs", dec, r);
	return buf;
}

std::string formatFrames(size_t frames)
{
	std::string digits = std::to_string(frames);
	std::string out;
	out.reserve(digits.size() + digits.size() / 3);
	const size_t lead = digits.size() % 3;
	for (size_t i = 0; i < digits.size(); ++i)
	{
		if (i > 0 && (i % 3) == lead % 3) out.push_back(',');
		out.push_back(digits[i]);
	}
	return out;
}

std::string readout(const View& v, const Clip& clip, double hoverFrame)
{
	const double rate = clip.sampleRate > 0 ? double(clip.sampleRate) : 48000.0;
	std::string s = "Playhead " + formatTime(double(v.playhead) / rate) +
	                " (frame " + formatFrames(v.playhead) + ")";
	if (v.hasSelection())
	{
		s += "   \xc2\xb7   Selection " + formatTime(double(v.selBegin) / rate) + " to " +
		     formatTime(double(v.selEnd) / rate) + ", " +
		     formatTime(double(v.selectionLength()) / rate) + " long (" +
		     formatFrames(v.selectionLength()) + " frames)";
	}
	else
		s += "   \xc2\xb7   No selection";
	if (v.hasTrim())
		s += "   \xc2\xb7   Trimmed to " + formatTime(double(v.trimBegin) / rate) + " to " +
		     formatTime(double(v.trimEnd) / rate) + " (frames " + formatFrames(v.trimBegin) +
		     " to " + formatFrames(v.trimEnd) + ")";
	if (hoverFrame >= 0.0)
	{
		const size_t f = size_t(std::min(hoverFrame, double(clip.frames)));
		s += "   \xc2\xb7   Pointer " + formatTime(double(f) / rate) +
		     " (frame " + formatFrames(f) + ")";
	}
	return s;
}

#if __has_include(<imgui.h>)

// ── Drawing ──────────────────────────────────────────────────────────────────

namespace
{
	// Ruler steps in seconds, finest first. A tick is placed at the smallest
	// step whose on-screen spacing clears kMinTickPx, so labels never collide.
	constexpr double kTickSteps[] = { 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5,
	                                  1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0,
	                                  600.0, 900.0, 1800.0, 3600.0 };
	constexpr float kMinTickPx = 84.0f;
	// How close (px) a press has to land to a selection edge to grab it, and
	// how far a press has to move before it is a drag rather than a click.
	constexpr float kEdgeGrabPx = 4.0f;
	constexpr float kDragPx     = 3.0f;

	const ImU32 kBg        = IM_COL32(20, 21, 25, 255);
	const ImU32 kRulerBg   = IM_COL32(28, 29, 34, 255);
	const ImU32 kWave      = IM_COL32(110, 200, 255, 205);
	const ImU32 kWaveSel   = IM_COL32(190, 232, 255, 240);
	const ImU32 kSelFill   = IM_COL32(90, 150, 255, 52);
	const ImU32 kSelEdge   = IM_COL32(140, 190, 255, 200);
	const ImU32 kPlayhead  = IM_COL32(255, 190, 90, 230);
	const ImU32 kOverBg    = IM_COL32(15, 16, 19, 255);
	const ImU32 kOverWave  = IM_COL32(110, 200, 255, 120);
	const ImU32 kOverWin   = IM_COL32(255, 255, 255, 30);
	const ImU32 kOverWinLn = IM_COL32(255, 255, 255, 120);
	// Outside the trim: the clip is still there, it just does not play.
	const ImU32 kTrimShade = IM_COL32(8, 8, 10, 170);
	const ImU32 kTrimEdge  = IM_COL32(255, 120, 90, 220);

	// 1-2-5 steps in frames for the sample ruler.
	double frameTickStep(double framesPerPx)
	{
		double base = 1.0;
		for (;;)
		{
			for (double m : { 1.0, 2.0, 5.0 })
				if (base * m / framesPerPx >= kMinTickPx) return base * m;
			base *= 10.0;
			if (base > 1e12) return base;
		}
	}
}

const Metrics& metrics()
{
	static const Metrics m;
	return m;
}

Result draw(const Clip& clip, const Peaks& peaks, View& view, const ImVec2& size, bool trackpad)
{
	Result out;
	const Metrics& M = metrics();
	ImDrawList*  dl     = ImGui::GetWindowDrawList();
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const float  width  = std::max(64.0f, size.x);
	const float  height = std::max(80.0f + M.overviewH, size.y);
	const size_t frames = clip.frames;
	const double rate   = clip.sampleRate > 0 ? double(clip.sampleRate) : 48000.0;
	out.canvasW = width;

	clampView(view, frames, width);

	const float rulerTop    = origin.y;
	const float rulerBottom = origin.y + M.rulerH;
	const float overTop     = origin.y + height - M.overviewH;
	const float lanesBottom = overTop - 4.0f;
	const float right       = origin.x + width;

	// ── Hit areas ────────────────────────────────────────────────────────────
	// Three buttons, one per strip, so each answers hover with its own help
	// entry and the press says by itself which grammar it starts.
	ImGui::SetCursorScreenPos(ImVec2(origin.x, rulerTop));
	ImGui::InvisibleButton("##aw_ruler", ImVec2(width, M.rulerH));
	EditorWidgets::helpForKey("Audio Editor/Ruler");
	const bool rulerHovered   = ImGui::IsItemHovered();
	const bool rulerActivated = ImGui::IsItemActivated();

	ImGui::SetCursorScreenPos(ImVec2(origin.x, rulerBottom));
	ImGui::InvisibleButton("##aw_lanes", ImVec2(width, std::max(1.0f, lanesBottom - rulerBottom)),
	                       ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
	EditorWidgets::helpForKey("Audio Editor/Waveform");
	const bool lanesHovered = ImGui::IsItemHovered();
	const bool lanesActive  = ImGui::IsItemActive();
	const bool lanesPressed = ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

	ImGui::SetCursorScreenPos(ImVec2(origin.x, overTop));
	ImGui::InvisibleButton("##aw_overview", ImVec2(width, M.overviewH));
	EditorWidgets::helpForKey("Audio Editor/Overview");
	const bool overHovered   = ImGui::IsItemHovered();
	const bool overActivated = ImGui::IsItemActivated();

	ImGuiIO&     io    = ImGui::GetIO();
	const ImVec2 mouse = io.MousePos;
	const float  mpx   = mouse.x - origin.x;
	auto frameUnderPointer = [&]() -> size_t
	{
		return size_t(std::clamp(std::llround(frameAtPx(view, mpx)), 0LL, (long long)frames));
	};
	auto overviewFrameAt = [&](float x) -> double
	{
		return double(x - origin.x) / double(width) * double(frames);
	};

	// ── Wheel ────────────────────────────────────────────────────────────────
	// Mouse: wheel zooms around the pointer, shift+wheel pans. Trackpad: the
	// two-finger swipe pans the timeline (both axes fold into the one axis a
	// waveform has), zoom moves behind Cmd/Ctrl+scroll — the rule every
	// preview pane follows.
	if ((rulerHovered || lanesHovered || overHovered) &&
	    (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f))
	{
		const bool zoomMod = io.KeyCtrl || io.KeySuper;
		const bool panning = trackpad ? !zoomMod : io.KeyShift;
		if (panning)
		{
			const float pan = trackpad ? (io.MouseWheelH + io.MouseWheel) : io.MouseWheel;
			view.viewStart -= double(pan) * view.framesPerPx * 80.0;
			clampView(view, frames, width);
		}
		else if (io.MouseWheel != 0.0f)
			zoomAt(view, frames, width, std::pow(1.0 / 0.86, double(io.MouseWheel)), mpx);
	}
	if (lanesActive && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
	{
		view.viewStart -= double(io.MouseDelta.x) * view.framesPerPx;
		clampView(view, frames, width);
	}

	// ── Presses ──────────────────────────────────────────────────────────────
	if (rulerActivated)
		view.drag = View::Drag::Scrub;
	else if (lanesPressed)
	{
		const size_t f = frameUnderPointer();
		view.pressX    = mouse.x;
		view.dragMoved = false;
		view.drag      = View::Drag::Select;
		view.pressSelBegin = view.selBegin;
		view.pressSelEnd   = view.selEnd;
		const float xb = pxAtFrame(view, double(view.selBegin));
		const float xe = pxAtFrame(view, double(view.selEnd));
		if (view.hasSelection() && std::fabs(mpx - xe) <= kEdgeGrabPx)
		{
			view.dragAnchor = view.selBegin;   // holding the end edge
			view.dragMoved  = true;
		}
		else if (view.hasSelection() && std::fabs(mpx - xb) <= kEdgeGrabPx)
		{
			view.dragAnchor = view.selEnd;     // holding the start edge
			view.dragMoved  = true;
		}
		else if (io.KeyShift)
		{
			// Stretch: keep the far edge, move the near one to the pointer. With
			// nothing selected the playhead is the other end.
			if (view.hasSelection())
				view.dragAnchor = (f < (view.selBegin + view.selEnd) / 2) ? view.selEnd : view.selBegin;
			else
				view.dragAnchor = view.playhead;
			view.dragMoved = true;
			select(view, view.dragAnchor, f, frames);
		}
		else
			view.dragAnchor = f;
	}
	else if (overActivated)
	{
		view.drag = View::Drag::Overview;
		const double span = view.framesPerPx * double(width);
		const double at   = overviewFrameAt(mouse.x);
		// Grabbing the window keeps the pointer where it took hold of it;
		// clicking beside it brings the window's centre to the pointer.
		view.overviewGrab = (at >= view.viewStart && at <= view.viewStart + span)
			? at - view.viewStart : span * 0.5;
	}

	// ── Drags ────────────────────────────────────────────────────────────────
	if (view.drag != View::Drag::None)
	{
		const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
		switch (view.drag)
		{
		case View::Drag::Scrub:
			if (down)
			{
				const size_t f = frameUnderPointer();
				if (f != view.playhead) { view.playhead = f; out.seek = true; }
			}
			break;
		case View::Drag::Select:
			if (down)
			{
				if (!view.dragMoved && std::fabs(mouse.x - view.pressX) > kDragPx)
					view.dragMoved = true;
				if (view.dragMoved)
				{
					// Past either end of the canvas the view follows the drag,
					// faster the further out the pointer is.
					if (mouse.x < origin.x)
						view.viewStart -= double(origin.x - mouse.x) * view.framesPerPx * 0.25;
					else if (mouse.x > right)
						view.viewStart += double(mouse.x - right) * view.framesPerPx * 0.25;
					clampView(view, frames, width);
					select(view, view.dragAnchor, frameUnderPointer(), frames);
				}
			}
			else
			{
				if (view.dragMoved)
				{
					// Only a selection that is really different counts: an edge
					// pressed and let go in place must not restart a playing
					// selection. Empty after the drag = cleared, also a change.
					out.selectionChanged = view.selBegin != view.pressSelBegin ||
					                       view.selEnd   != view.pressSelEnd;
				}
				else
				{
					// A click: the playhead goes there and the selection goes away.
					if (view.hasSelection()) out.selectionChanged = true;
					clearSelection(view);
					view.playhead = view.dragAnchor;
					out.seek = true;
				}
			}
			break;
		case View::Drag::Overview:
			if (down)
			{
				view.viewStart = overviewFrameAt(mouse.x) - view.overviewGrab;
				clampView(view, frames, width);
			}
			break;
		case View::Drag::None:
			break;
		}
		if (!down) view.drag = View::Drag::None;
	}

	// The edge cursor, so the grab is discoverable.
	if (lanesHovered && view.drag == View::Drag::None && view.hasSelection())
	{
		const float xb = pxAtFrame(view, double(view.selBegin));
		const float xe = pxAtFrame(view, double(view.selEnd));
		if (std::fabs(mpx - xb) <= kEdgeGrabPx || std::fabs(mpx - xe) <= kEdgeGrabPx)
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
	}
	if (rulerHovered || lanesHovered)
		out.hoverFrame = std::clamp(frameAtPx(view, mpx), 0.0, double(frames));

	// ── Picture ──────────────────────────────────────────────────────────────
	const ImVec2 br(right, lanesBottom);
	auto xAt = [&](double f) { return origin.x + pxAtFrame(view, f); };

	dl->PushClipRect(origin, br, true);
	dl->AddRectFilled(origin, br, kBg);

	// Ruler band and ticks.
	dl->AddRectFilled(origin, ImVec2(right, rulerBottom), kRulerBg);
	dl->AddLine(ImVec2(origin.x, rulerBottom), ImVec2(right, rulerBottom), IM_COL32(255, 255, 255, 24));
	const double viewEnd = view.viewStart + view.framesPerPx * double(width);
	if (view.rulerInSamples)
	{
		const double step = frameTickStep(view.framesPerPx);
		for (double f = std::ceil(view.viewStart / step) * step; f <= viewEnd; f += step)
		{
			const float x = xAt(f);
			dl->AddLine(ImVec2(x, origin.y), ImVec2(x, br.y), IM_COL32(255, 255, 255, 14));
			dl->AddLine(ImVec2(x, rulerBottom - 5.0f), ImVec2(x, rulerBottom), IM_COL32(255, 255, 255, 60));
			const std::string lbl = formatFrames(size_t(f));
			dl->AddText(ImVec2(x + 4.0f, origin.y + 3.0f), IM_COL32(190, 195, 205, 255), lbl.c_str());
		}
	}
	else
	{
		const double secPerPx = view.framesPerPx / rate;
		double step = kTickSteps[std::size(kTickSteps) - 1];
		for (double candidate : kTickSteps)
			if (candidate / secPerPx >= kMinTickPx) { step = candidate; break; }
		const double first = std::ceil((view.viewStart / rate) / step) * step;
		// Counting ticks instead of accumulating t keeps 0.1-steps from drifting
		// into labels like 0.30000000004.
		for (int i = 0;; ++i)
		{
			const double t = first + step * double(i);
			if (t * rate > viewEnd) break;
			const float x = xAt(t * rate);
			dl->AddLine(ImVec2(x, origin.y), ImVec2(x, br.y), IM_COL32(255, 255, 255, 14));
			dl->AddLine(ImVec2(x, rulerBottom - 5.0f), ImVec2(x, rulerBottom), IM_COL32(255, 255, 255, 60));
			const std::string lbl = formatTimeShort(t, step);
			dl->AddText(ImVec2(x + 4.0f, origin.y + 3.0f), IM_COL32(190, 195, 205, 255), lbl.c_str());
		}
	}

	// The selection, under the waveform so the samples stay readable on it.
	const bool  hasSel = view.hasSelection();
	const float selX0  = hasSel ? xAt(double(view.selBegin)) : 0.0f;
	const float selX1  = hasSel ? xAt(double(view.selEnd))   : 0.0f;
	if (hasSel)
	{
		dl->AddRectFilled(ImVec2(selX0, origin.y), ImVec2(std::max(selX1, selX0 + 1.0f), br.y), kSelFill);
		dl->AddLine(ImVec2(selX0, origin.y), ImVec2(selX0, br.y), kSelEdge);
		dl->AddLine(ImVec2(selX1, origin.y), ImVec2(selX1, br.y), kSelEdge);
	}

	// Channel lanes.
	const int   ch    = std::max(1, clip.channels);
	const float laneH = (lanesBottom - rulerBottom) / float(ch);
	for (int c = 0; c < ch; ++c)
	{
		const float top    = rulerBottom + laneH * float(c);
		const float centre = top + laneH * 0.5f;
		const float halfH  = laneH * 0.45f;
		if (c > 0) dl->AddLine(ImVec2(origin.x, top), ImVec2(right, top), IM_COL32(255, 255, 255, 18));
		dl->AddLine(ImVec2(origin.x, centre), ImVec2(right, centre), IM_COL32(255, 255, 255, 34));
		if (!clip.valid()) continue;

		if (view.framesPerPx < 1.0)
		{
			// Zoomed past one frame per pixel: a column would hold one sample
			// or none, so draw the samples themselves, joined, with a dot each
			// once there is room for one.
			const size_t f0 = size_t(std::max(0.0, std::floor(view.viewStart)));
			const size_t f1 = std::min(frames, size_t(std::ceil(viewEnd)) + 1);
			ImVec2 prev;
			for (size_t f = f0; f < f1; ++f)
			{
				const float  v = float(clip.samples[f * size_t(clip.channels) + size_t(c)]) / 32768.0f;
				const ImVec2 p(xAt(double(f) + 0.5), centre - v * halfH);
				const bool   in = hasSel && f >= view.selBegin && f < view.selEnd;
				if (f > f0) dl->AddLine(prev, p, in ? kWaveSel : kWave, 1.5f);
				if (view.framesPerPx < 0.25) dl->AddCircleFilled(p, 2.0f, in ? kWaveSel : kWave);
				prev = p;
			}
			continue;
		}

		for (int px = 0; px < int(width); ++px)
		{
			const double fa = frameAtPx(view, float(px));
			const double fb = frameAtPx(view, float(px + 1));
			if (fb <= 0.0 || fa >= double(frames)) continue;
			const size_t f0 = size_t(std::max(0.0, fa));
			const size_t f1 = std::min(frames, size_t(std::max(fa + 1.0, fb)));
			if (f1 <= f0) continue;
			int lo = 0, hi = 0;
			columnExtent(clip, peaks, f0, f1, c, lo, hi);
			const float x  = origin.x + float(px) + 0.5f;
			const float y0 = centre - float(hi) / 32768.0f * halfH;
			const float y1 = centre - float(lo) / 32768.0f * halfH;
			const bool  in = hasSel && x >= selX0 && x < selX1;
			// A near-flat column would round to nothing; a hairline keeps
			// silence reading as a line rather than as a gap in the clip.
			dl->AddLine(ImVec2(x, y0), ImVec2(x, std::max(y1, y0 + 1.0f)), in ? kWaveSel : kWave);
		}
	}

	// The trim, OVER the waveform: what does not play is shaded, so the part
	// that does reads at a glance and the samples cut away stay visible.
	if (view.hasTrim())
	{
		const float tx0 = xAt(double(view.trimBegin));
		const float tx1 = xAt(double(std::min(view.trimEnd, frames)));
		if (tx0 > origin.x)
			dl->AddRectFilled(ImVec2(origin.x, rulerBottom), ImVec2(std::min(tx0, right), br.y), kTrimShade);
		if (tx1 < right)
			dl->AddRectFilled(ImVec2(std::max(tx1, origin.x), rulerBottom), br, kTrimShade);
		dl->AddLine(ImVec2(tx0, origin.y), ImVec2(tx0, br.y), kTrimEdge, 1.5f);
		dl->AddLine(ImVec2(tx1, origin.y), ImVec2(tx1, br.y), kTrimEdge, 1.5f);
	}

	// Pointer line (only while nothing is being dragged), then the playhead.
	if (lanesHovered && view.drag == View::Drag::None)
		dl->AddLine(ImVec2(mouse.x, rulerBottom), ImVec2(mouse.x, br.y), IM_COL32(255, 255, 255, 50));
	{
		const float x = xAt(double(view.playhead));
		if (x >= origin.x - 1.0f && x <= right + 1.0f)
		{
			dl->AddLine(ImVec2(x, origin.y), ImVec2(x, br.y), kPlayhead, 1.5f);
			dl->AddTriangleFilled(ImVec2(x - 5.0f, origin.y), ImVec2(x + 5.0f, origin.y),
			                      ImVec2(x, origin.y + 7.0f), kPlayhead);
		}
	}
	dl->PopClipRect();
	dl->AddRect(origin, br, IM_COL32(255, 255, 255, 26));

	// ── Overview / scrollbar ─────────────────────────────────────────────────
	// The whole clip at a glance, all channels folded into one outline, from
	// the coarse end of the pyramid — a few hundred reads for the whole strip.
	const ImVec2 o0(origin.x, overTop), o1(right, overTop + M.overviewH);
	dl->PushClipRect(o0, o1, true);
	dl->AddRectFilled(o0, o1, kOverBg);
	if (clip.valid())
	{
		const float oc = overTop + M.overviewH * 0.5f;
		const float oh = M.overviewH * 0.45f;
		const double fpp = double(frames) / double(width);
		for (int px = 0; px < int(width); ++px)
		{
			const size_t f0 = size_t(double(px) * fpp);
			const size_t f1 = std::min(frames, std::max(f0 + 1, size_t(double(px + 1) * fpp)));
			int lo = 0, hi = 0;
			for (int c = 0; c < clip.channels; ++c)
			{
				int l = 0, h = 0;
				columnExtent(clip, peaks, f0, f1, c, l, h);
				lo = std::min(lo, l);
				hi = std::max(hi, h);
			}
			const float x = o0.x + float(px) + 0.5f;
			dl->AddLine(ImVec2(x, oc - float(hi) / 32768.0f * oh),
			            ImVec2(x, std::max(oc - float(lo) / 32768.0f * oh, oc - float(hi) / 32768.0f * oh + 1.0f)),
			            kOverWave);
		}
		auto ox = [&](double f) { return o0.x + float(f / double(frames) * double(width)); };
		if (hasSel)
			dl->AddRectFilled(ImVec2(ox(double(view.selBegin)), o0.y),
			                  ImVec2(std::max(ox(double(view.selEnd)), ox(double(view.selBegin)) + 1.0f), o1.y),
			                  kSelFill);
		if (view.hasTrim())
		{
			const float tx0 = ox(double(view.trimBegin));
			const float tx1 = ox(double(std::min(view.trimEnd, frames)));
			dl->AddRectFilled(o0, ImVec2(tx0, o1.y), kTrimShade);
			dl->AddRectFilled(ImVec2(tx1, o0.y), o1, kTrimShade);
		}
		const float wx0 = ox(view.viewStart);
		const float wx1 = std::max(wx0 + 4.0f, ox(viewEnd));
		dl->AddRectFilled(ImVec2(wx0, o0.y), ImVec2(wx1, o1.y), kOverWin);
		dl->AddRect(ImVec2(wx0, o0.y), ImVec2(wx1, o1.y), kOverWinLn);
		const float phx = ox(double(view.playhead));
		dl->AddLine(ImVec2(phx, o0.y), ImVec2(phx, o1.y), kPlayhead);
	}
	dl->PopClipRect();
	dl->AddRect(o0, o1, IM_COL32(255, 255, 255, 26));

	// Leave the cursor under the whole canvas, as a single item of `size`
	// would have.
	ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + height));
	ImGui::Dummy(ImVec2(width, 0.0f));
	return out;
}

#endif // __has_include(<imgui.h>)

} // namespace HE::Ed::AudioWave
