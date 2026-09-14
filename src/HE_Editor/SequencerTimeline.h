#pragma once
#include <ContentManager/Assets.h>   // PropertyAnimClipAsset, PropTarget

#if __has_include(<imgui.h>)
#include <imgui.h>
#endif

// ── The Sequencer's strip: tracks, ruler, playhead ───────────────────────────
// A PropertyAnimClipAsset is a list of scalar channels — "Position X over time",
// "Roughness over time" — and this is the picture of one: a row per channel with
// its name and its value at the playhead on the left, its keys as diamonds on
// a shared time axis on the right, a ruler above that you drag to scrub.
//
// Kept free of AppContext on purpose, the way MeshMaterialSlots is: the tab
// panel (SequencerPanel) owns loading, saving and the toolbar, and hands the
// clip and the view in here. That is what lets the headless UI test drive the
// strip over an in-memory clip — drag the ruler, click a key, spin the wheel —
// and check where the playhead landed, without a GPU and without a project.
//
// The seconds ⇄ pixels arithmetic is UITimelineView (UITimelineMath.h), shared
// with the UI Designer's animation strip: the same wheel, the same 1-2-5 ruler,
// so somebody who has used one already knows the other.
namespace HE::Ed::Sequencer
{
	// What the strip remembers between frames, per open tab. Editor state, never
	// written into the asset: where you are looking is not a property of the clip.
	struct View
	{
		float playhead = 0.0f;   // seconds
		float zoom     = 1.0f;   // multiple of fit (UITimelineView)
		float scroll   = 0.0f;   // the second at the lane's left edge
		int   trackSel = -1;     // channel index, or -1
		int   keySel   = -1;     // key index within trackSel, or -1
		// The ruler is being dragged. Held across frames because a scrub is a
		// gesture that leaves the ruler's rectangle on the way.
		bool  scrubbing = false;
	};

	// The toolbar's zoom buttons record their intent up there and it is applied
	// down here, once the lane's width is known — zooming around a point needs
	// the point in pixels.
	struct Intent
	{
		int  zoom = 0;      // +1 in, -1 out, 0 nothing
		bool fit  = false;  // the whole clip across the lane again
	};

	// What draw() reports back, so the panel can react without re-deriving it.
	struct Result
	{
		bool playheadMoved = false;   // scrubbed or jumped to a key
		bool selectionChanged = false;
		// Where the lane ended up on screen this frame: its left edge, its
		// width and the strip's top. With these and the View, UITimelineView
		// reproduces every pixel ⇄ second conversion the strip made — which
		// is how the headless test knows where to click.
		float laneX = 0.0f, laneW = 0.0f, top = 0.0f;
	};

	// "Position X", "Roughness" — the label a track wears, one per PropTarget.
	const char* targetName(PropTarget t);
	// "Transform" or "Material": which component the target writes into. The
	// strip colours the two groups apart, and a track list sorted by it reads
	// as two short lists instead of one long one.
	const char* targetGroup(PropTarget t);
	// How a value of this target is printed: degrees for rotation, plain for
	// everything else. Writes into `buf`.
	void formatValue(PropTarget t, float v, char* buf, size_t n);

	// The geometry the strip is drawn with. Exposed so a test can compute where
	// a key or a second sits without repeating the constants.
	struct Metrics
	{
		float nameW  = 210.0f;   // the name column
		float rulerH = 18.0f;
		float rowH   = 22.0f;
		float gap    = 8.0f;     // between the name column and the lane
	};
	const Metrics& metrics();

#if __has_include(<imgui.h>)
	// Draws the strip at the cursor, `size` big (a zero component means "the
	// rest of the window"). The ruler takes drags (scrub), a key takes a click
	// (select it and put the playhead on it), the wheel over the lane zooms at
	// the pointer and Shift+wheel pans.
	//
	// Step 1 of the sequencer: keys are shown and selected, not moved or made —
	// that is the authoring step, and it lands on top of this.
	Result draw(const PropertyAnimClipAsset& clip, View& view,
	            const ImVec2& size, const Intent& intent = {});
#endif
}
