#pragma once
#include <ContentManager/Assets.h>   // PropertyAnimClipAsset, PropTarget

#include <algorithm>
#include <cmath>

#if __has_include(<imgui.h>)
#include <imgui.h>
#endif

// ── The Sequencer's strip: tracks, ruler, playhead, keys and curves ──────────
// A PropertyAnimClipAsset is a list of scalar channels — "Position X over time",
// "Roughness over time" — and this is the picture of one: a row per channel with
// its name and its value at the playhead on the left, its keys as diamonds on
// a shared time axis on the right, a ruler above that you drag to scrub. Flip
// the view to curves and the right half becomes one graph of the selected
// track, value over time, with the keys as points you drag in both axes.
//
// Kept free of AppContext on purpose, the way MeshMaterialSlots is: the tab
// panel (SequencerPanel) owns loading, saving, undo and the toolbar, and hands
// the clip and the view in here. That is what lets the headless UI test drive
// the strip over an in-memory clip — drag a key, double-click a lane, spin the
// wheel — and check what the clip looks like afterwards, without a GPU and
// without a project.
//
// The seconds ⇄ pixels arithmetic is UITimelineView (UITimelineMath.h), shared
// with the UI Designer's animation strip: the same wheel, the same 1-2-5 ruler,
// so somebody who has used one already knows the other.
namespace HE::Ed::Sequencer
{
	// ── Editing the clip ─────────────────────────────────────────────────────
	// Every change the strip or the panel makes to a clip goes through these,
	// and they keep the two things PropertyAnimationSystem::sampleChannel
	// silently relies on: `times` strictly ascending (it binary-searches) and
	// one value per time. A drag that wrote into `times[k]` directly would
	// break the search the moment a key crossed its neighbour — and the value
	// beside the track name would go quietly wrong, not loudly.
	//
	// All of them are plain functions over the asset, outside the ImGui guard,
	// so they are tested without a context.

	// Two keys closer than this are the same moment: inserting on top of an
	// existing key replaces its value rather than making a second key the
	// runtime could not tell apart.
	constexpr float kKeyEpsilon = 1e-4f;

	// A key at `t` holding `v`. Returns the key's index. If a key already sits
	// at `t` (within kKeyEpsilon) its value is replaced and that index comes
	// back. Never a negative time.
	int insertKey(PropertyAnimChannel& ch, float t, float v);
	// Key `k` to time `t`, keeping the channel sorted. Returns the key's index
	// afterwards — it can change, because moving past a neighbour swaps their
	// order. Lands next to a key it would overlap rather than on it, so a drag
	// never silently merges two keys. -1 for an index out of range.
	int moveKey(PropertyAnimChannel& ch, int k, float t);
	// Key `k`, gone. False for an index out of range.
	bool removeKey(PropertyAnimChannel& ch, int k);

	// What a fresh key on this property holds when nothing better is known:
	// 1 for scale, colour and opacity (an unscaled, white, opaque thing), 0 for
	// position, rotation, metallic and roughness. Used by a new track's first
	// key; a key added to a track that has keys takes the sampled value instead.
	float defaultValue(PropTarget t);

	// A track for `target`, with one key at 0 holding defaultValue(). Returns
	// the track's index — or the EXISTING track's index if the clip already
	// animates this property: the runtime writes every channel in order, so a
	// second track for the same target would just overwrite the first.
	int addTrack(PropertyAnimClipAsset& clip, PropTarget target);
	// The track index of `target`, or -1.
	int findTrack(const PropertyAnimClipAsset& clip, PropTarget target);
	bool removeTrack(PropertyAnimClipAsset& clip, int track);

	// The last key on any track — the earliest moment the clip may end at.
	float lastKeyTime(const PropertyAnimClipAsset& clip);
	// A new length. Never shorter than the last key: UITimelineView clamps
	// every pointer to the clip's length, so a key past the end would be one
	// nobody could reach to move or delete. The length actually set is returned.
	float setDuration(PropertyAnimClipAsset& clip, float duration);
	// What a clip with no length gets when its first track is added — so the
	// first key has a lane to sit on.
	constexpr float kDefaultDuration = 1.0f;

	// ── The view ─────────────────────────────────────────────────────────────
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
		// Curves instead of diamonds: the lane becomes a graph of the selected
		// track, value up, time across.
		bool  curves = false;
		// The mouse went down on the selected key: a drag from here moves it.
		// Held in the view rather than on the key's own button because moving
		// past a neighbour changes the key's index, and with it the button.
		bool  keyArmed = false;
		// ...and it has actually moved. What turns the release into ONE undo
		// point rather than sixty, and a plain click into none.
		bool  dragging = false;
		// The curve view's value range, kept between frames so it can be held
		// still while a key is dragged (see draw()).
		float axisLo = 0.0f, axisHi = 1.0f;
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
		// The clip changed this frame — the panel's dirty flag.
		bool edited = false;
		// An edit is COMPLETE: a key was added or removed, or a drag ended.
		// The moment for an undo snapshot; a drag reports `edited` on every
		// frame it moves and `committed` once, on release.
		bool committed = false;
		// Where the lane ended up on screen this frame: its left edge, its
		// width and the strip's top. With these and the View, UITimelineView
		// reproduces every pixel ⇄ second conversion the strip made — which
		// is how the headless test knows where to click.
		float laneX = 0.0f, laneW = 0.0f, top = 0.0f;
		// In curve view: the graph's vertical extent on screen and the value
		// range it maps onto it — the same for the value axis as the lane
		// numbers above are for time.
		float graphTop = 0.0f, graphH = 0.0f;
		float valueLo = 0.0f, valueHi = 1.0f;
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
	// One past the last PropTarget — how many properties can be animated.
	constexpr int kTargetCount = static_cast<int>(PropTarget::MatOpacity) + 1;

	// The geometry the strip is drawn with. Exposed so a test can compute where
	// a key or a second sits without repeating the constants.
	struct Metrics
	{
		float nameW  = 210.0f;   // the name column
		float rulerH = 18.0f;
		float rowH   = 22.0f;
		float gap    = 8.0f;     // between the name column and the lane
		float keyR   = 6.0f;     // a diamond's half-size, and a curve point's radius
	};
	const Metrics& metrics();

	// ── The value axis of the curve view ─────────────────────────────────────
	// Value ⇄ pixels, the vertical counterpart of UITimelineView. Top of the
	// graph is `hi`, bottom is `lo`; the range is the channel's own with a
	// margin, so the curve fills the graph rather than hugging one edge.
	struct ValueAxis
	{
		float top = 0.0f, height = 1.0f;
		float lo  = 0.0f, hi = 1.0f;

		float yOf(float v) const
		{
			return top + (hi - v) / std::max(hi - lo, 1e-6f) * height;
		}
		float vOf(float y) const
		{
			return hi - (y - top) / std::max(height, 1e-6f) * (hi - lo);
		}
	};
	// The range a channel's values want on the axis: min..max padded by a
	// tenth of the span on either side, and a span of at least one around a
	// flat channel so a constant still draws as a line in the middle rather
	// than a line on the floor.
	void valueRange(const PropertyAnimChannel& ch, float& lo, float& hi);

#if __has_include(<imgui.h>)
	// Draws the strip at the cursor, `size` big (a zero component means "the
	// rest of the window"). The ruler takes drags (scrub), a key takes a click
	// (select it and put the playhead on it) and a drag (move it in time — and
	// in value, in curve view), an empty spot on a lane takes a double-click
	// (a key there, holding what the track already is at that moment), the
	// Delete key removes the selected key, the wheel over the lane zooms at
	// the pointer and Shift+wheel pans.
	//
	// The clip is mutable because the gestures above edit it; everything they
	// do goes through the functions at the top of this file.
	Result draw(PropertyAnimClipAsset& clip, View& view,
	            const ImVec2& size, const Intent& intent = {});
#endif
}
