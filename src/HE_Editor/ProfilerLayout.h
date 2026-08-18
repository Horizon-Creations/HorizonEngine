#pragma once
// ─── ProfilerLayout ──────────────────────────────────────────────────────────
// The geometry behind the profiler's hand-drawn views: span → rectangle, zoom and
// pan of a time window, bar hit-testing, tick spacing.
//
// Deliberately free of ImGui and of any drawing. The editor cannot be run in the
// environment this was written in (no display), so the part that can silently be
// wrong — the arithmetic that decides where a 40 µs span lands at 12x zoom — lives
// here where a unit test can pin it down, and ProfilerWidgets.cpp is left with
// nothing but draw calls.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace HE::Prof::Layout
{

struct Rect
{
	float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
	float width()  const { return x1 - x0; }
	float height() const { return y1 - y0; }
};

// The horizontal window currently shown, in the timeline's capture-relative ns.
struct TimeView
{
	double startNs = 0.0;
	double endNs   = 0.0;
	double span()  const { return endNs - startNs; }
	bool   valid() const { return endNs > startNs; }
};

// Never let a view collapse to nothing: at zero span every pixel maps to the same
// instant and the next zoom-out has no anchor to work from.
inline constexpr double kMinViewSpanNs = 1000.0;   // 1 µs across the whole width

// ─── Span → rectangle ────────────────────────────────────────────────────────
// Maps [startNs,endNs) at `depth` into the lane's pixel band, clipped to
// [px0,px1]. Returns false when the span is entirely outside the view.
//
// minW exists because most spans in a real capture are microseconds wide: without
// a floor they round to zero pixels and a busy thread renders as an empty lane —
// worse than no timeline, because it looks like an answer. A floored span is
// visibly present but no longer proportional, which is the right trade at this
// zoom; zooming in restores true width.
inline bool spanRect(uint64_t startNs, uint64_t endNs, uint32_t depth,
                     const TimeView& view, float px0, float px1,
                     float laneTop, float rowH, float minW, Rect& out)
{
	if (!view.valid() || px1 <= px0) return false;
	const double s = static_cast<double>(startNs);
	const double e = static_cast<double>(endNs);
	if (e < view.startNs || s > view.endNs) return false;

	const double pxPerNs = static_cast<double>(px1 - px0) / view.span();
	float x0 = px0 + static_cast<float>((s - view.startNs) * pxPerNs);
	float x1 = px0 + static_cast<float>((e - view.startNs) * pxPerNs);

	x0 = std::clamp(x0, px0, px1);
	x1 = std::clamp(x1, px0, px1);
	if (x1 - x0 < minW)
	{
		x1 = x0 + minW;
		if (x1 > px1) { x1 = px1; x0 = std::max(px0, px1 - minW); }
	}

	out.x0 = x0;
	out.x1 = x1;
	out.y0 = laneTop + static_cast<float>(depth) * rowH;
	out.y1 = out.y0 + rowH;
	return true;
}

// ─── Zoom / pan ──────────────────────────────────────────────────────────────
// Zoom about the pixel under the cursor: that instant must stay under the cursor,
// which is what makes wheel-zoom feel like the view is attached to the mouse
// rather than to the window. factor < 1 zooms in.
inline TimeView zoomAt(const TimeView& v, float anchorPx, float px0, float px1,
                       double factor, double minSpanNs = kMinViewSpanNs)
{
	if (!v.valid() || px1 <= px0 || !(factor > 0.0)) return v;
	const double t   = std::clamp(static_cast<double>(anchorPx - px0) /
	                              static_cast<double>(px1 - px0), 0.0, 1.0);
	const double at  = v.startNs + t * v.span();
	double newSpan   = std::max(minSpanNs, v.span() * factor);

	TimeView out;
	out.startNs = at - t * newSpan;
	out.endNs   = out.startNs + newSpan;
	return out;
}

inline TimeView panByPixels(const TimeView& v, float dxPx, float px0, float px1)
{
	if (!v.valid() || px1 <= px0) return v;
	const double nsPerPx = v.span() / static_cast<double>(px1 - px0);
	TimeView out;
	out.startNs = v.startNs - static_cast<double>(dxPx) * nsPerPx;
	out.endNs   = out.startNs + v.span();
	return out;
}

// Keep the window inside the captured range, preserving its width. A view wider
// than the capture snaps to the whole capture rather than floating in empty space.
inline TimeView clampView(const TimeView& v, uint64_t loNs, uint64_t hiNs)
{
	const double lo = static_cast<double>(loNs);
	const double hi = static_cast<double>(hiNs);
	if (!(hi > lo)) return { lo, lo + kMinViewSpanNs };
	if (!v.valid()) return { lo, hi };

	TimeView out = v;
	if (out.span() >= hi - lo) return { lo, hi };
	if (out.startNs < lo)      { out.startNs = lo; out.endNs = lo + v.span(); }
	if (out.endNs   > hi)      { out.endNs   = hi; out.startNs = hi - v.span(); }
	return out;
}

// ─── Hit testing ─────────────────────────────────────────────────────────────
// Which of `count` equal-width bars sits under mouseX, or -1 outside the strip.
// Used to turn the frame-time graph from decoration into navigation: the spike a
// reader can see is the frame they want opened.
inline int barIndexAt(float mouseX, float x0, float x1, int count)
{
	if (count <= 0 || x1 <= x0) return -1;
	if (mouseX < x0 || mouseX >= x1) return -1;
	const float w  = (x1 - x0) / static_cast<float>(count);
	int         i  = static_cast<int>((mouseX - x0) / w);
	return std::clamp(i, 0, count - 1);
}

// ─── Axis ticks ──────────────────────────────────────────────────────────────
// A 1/2/5 x 10^n step just above the requested minimum spacing — the spacing that
// yields labels a person reads as round numbers (0.5 ms, 1 ms, 2 ms) instead of
// 0.7341 ms. Returns the step in the value's own units.
inline double niceTickStep(double range, int maxTicks)
{
	if (!(range > 0.0) || maxTicks <= 0) return 0.0;
	const double rough = range / static_cast<double>(maxTicks);
	const double mag   = std::pow(10.0, std::floor(std::log10(rough)));
	const double norm  = rough / mag;
	double step;
	if      (norm <= 1.0) step = 1.0;
	else if (norm <= 2.0) step = 2.0;
	else if (norm <= 5.0) step = 5.0;
	else                  step = 10.0;
	return step * mag;
}

// ─── Stable per-name colour ──────────────────────────────────────────────────
// FNV-1a over the scope name. A scope must keep the same hue between frames,
// between lanes and between sessions: on a flame graph the colour IS the identity,
// and one that reshuffles per frame makes the view unreadable while it updates.
inline uint32_t nameHash(const char* name)
{
	uint32_t h = 2166136261u;
	for (const char* p = name ? name : ""; *p; ++p)
	{
		h ^= static_cast<uint32_t>(static_cast<unsigned char>(*p));
		h *= 16777619u;
	}
	return h;
}

} // namespace HE::Prof::Layout
