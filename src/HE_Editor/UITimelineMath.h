#pragma once
#include <algorithm>
#include <cmath>

// ── The timeline's arithmetic ────────────────────────────────────────────────
// docs/he-apps-plan.md B8. Pulled out of the panel because it is the only part
// of an animation editor that can be checked without a window: seconds ⇄ pixels
// under zoom and scroll, and how far apart a ruler's labels stand.
//
// Header-only on purpose — it is arithmetic, and the test includes it directly
// rather than linking a panel that needs ImGui and an AppContext.
namespace HE::Ed
{

// A tick step in seconds, from the 1-2-5 ladder, chosen so consecutive labels
// stand at least `minLabelPx` apart.
//
// The ladder is what every ruler uses, and for a reason that is about reading
// rather than about numbers: "0.25 s" is something a person places between a
// tick marked 0.2 and one marked 0.4 without thinking, and cannot place between
// ticks marked 0.3 and 0.6 at all.
inline float uiTimelineTickStep(float pixelsPerSecond, float minLabelPx = 64.0f)
{
	// Seconds one label's worth of pixels covers, then the smallest rung at or
	// above it. Guarded rather than asserted: a lane can be one pixel wide for
	// a frame while a panel is being dragged, and a ruler is not worth a crash.
	const float want = minLabelPx / std::max(pixelsPerSecond, 0.0001f);
	const float mag  = std::pow(10.0f, std::floor(std::log10(std::max(want, 1e-6f))));
	if (mag         >= want) return mag;
	if (mag * 2.0f  >= want) return mag * 2.0f;
	if (mag * 5.0f  >= want) return mag * 5.0f;
	return mag * 10.0f;
}

// Seconds ⇄ pixels for one lane, which is the conversion the whole strip
// shares: the ruler, the key diamonds, the playhead and every hit test.
//
// Zoom is a multiple of FIT — 1 means the whole clip spans the lane — so there
// is no zooming out into empty space, and `scroll` is the second sitting at the
// lane's left edge.
struct UITimelineView
{
	float laneX    = 0.0f;   // screen x where the lane starts (right of the names)
	float laneW    = 1.0f;
	float duration = 1.0f;
	float zoom     = 1.0f;
	float scroll   = 0.0f;

	static constexpr float kMaxZoom = 200.0f;

	float pixelsPerSecond() const
	{
		return (laneW / std::max(duration, 1e-4f)) * std::clamp(zoom, 1.0f, kMaxZoom);
	}
	float xOf(float t) const { return laneX + (t - scroll) * pixelsPerSecond(); }
	// Clamped to the clip: every caller of this is answering "what time did the
	// user point at", and a negative one is not an answer.
	float tOf(float x) const
	{
		return std::clamp(scroll + (x - laneX) / pixelsPerSecond(), 0.0f, duration);
	}

	float visibleSpan() const { return duration / std::clamp(zoom, 1.0f, kMaxZoom); }
	float maxScroll()   const { return std::max(0.0f, duration - visibleSpan()); }
	void  clampScroll()       { scroll = std::clamp(scroll, 0.0f, maxScroll()); }

	// Zoom keeping the second under `anchorX` where it is. The only zoom that
	// does not feel like a jump: you point at the moment you care about and it
	// stays under the pointer while the ruler grows around it.
	void zoomAt(float newZoom, float anchorX)
	{
		// Deliberately NOT tOf(): the anchor may sit outside the clip (the lane
		// is wider than the visible span at zoom 1), and clamping it here would
		// drag the view sideways instead of leaving it alone.
		const float t = scroll + (anchorX - laneX) / pixelsPerSecond();
		zoom   = std::clamp(newZoom, 1.0f, kMaxZoom);
		scroll = t - (anchorX - laneX) / pixelsPerSecond();
		clampScroll();
	}

	// Bring `t` into view, centred, when it has left it — what playback does so
	// a zoomed-in timeline follows the playhead instead of losing it.
	void reveal(float t)
	{
		if (t >= scroll && t <= scroll + visibleSpan()) return;
		scroll = t - visibleSpan() * 0.5f;
		clampScroll();
	}
};

}
