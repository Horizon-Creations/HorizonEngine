#include "doctest.h"

#include "SequencerTimeline.h"
#include "UITimelineMath.h"
#include "AssetStubWriter.h"      // writeAssetStub — what the Content Browser makes
#include "TestFsUtil.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/PropertyAnimatorComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/PropertyAnimationSystem.h>

#include <imgui.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>

// ── The Sequencer's strip, driven without a window ───────────────────────────
// The strip is the part of the sequencer that can be WRONG in a way nobody
// notices: a scrub that lands a pixel off, a key whose hit box is not where its
// diamond is, a zoom that slides the moment under the pointer away. None of
// that needs a GPU — ImGui takes a context and mouse events — so it is checked
// here, over a PropertyAnimClipAsset on the stack, the way the UI Designer's
// strip arithmetic is checked in test_ui_widgets.cpp.

using namespace HE::Ed::Sequencer;

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
			// No renderer: claim texture support so ImGui never waits on a
			// backend to upload the font atlas.
			io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
			// No padding, so the strip starts where the window does and the
			// arithmetic below has nothing to guess at.
			ImGui::GetStyle().WindowPadding = ImVec2(0.0f, 0.0f);
		}
		~ImGuiCtx() { ImGui::DestroyContext(); }
	};

	constexpr float kWinW = 1000.0f, kWinH = 300.0f;

	// One frame: the strip in a bare window at the origin. Returns what the
	// strip reported — the lane's place on screen included.
	Result frame(PropertyAnimClipAsset& clip, View& view, const Intent& intent = {})
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(kWinW, kWinH));
		ImGui::Begin("##seqtest", nullptr,
		             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
		const Result r = draw(clip, view, ImVec2(kWinW, kWinH), intent);
		ImGui::End();
		ImGui::Render();
		return r;
	}

	// The conversion the strip used this frame, rebuilt from what it reported.
	HE::Ed::UITimelineView viewOf(const Result& r, const PropertyAnimClipAsset& clip, const View& v)
	{
		return HE::Ed::UITimelineView{ r.laneX, r.laneW, clip.duration, v.zoom, v.scroll };
	}

	PropertyAnimClipAsset twoTrackClip()
	{
		PropertyAnimClipAsset clip;
		clip.duration = 2.0f;
		PropertyAnimChannel pos;
		pos.target = PropTarget::PosX;
		pos.times  = { 0.0f, 0.5f, 2.0f };
		pos.values = { 0.0f, 1.0f, 3.0f };
		PropertyAnimChannel rough;
		rough.target = PropTarget::MatRoughness;
		rough.times  = { 0.0f, 1.0f };
		rough.values = { 0.2f, 0.8f };
		clip.channels = { pos, rough };
		return clip;
	}

	void mouseAt(float x, float y)              { ImGui::GetIO().AddMousePosEvent(x, y); }
	void mouseButton(bool down)                 { ImGui::GetIO().AddMouseButtonEvent(0, down); }
	void mouseWheel(float dy)                   { ImGui::GetIO().AddMouseWheelEvent(0.0f, dy); }
}

TEST_CASE("sequencer: every property target has a name, a group and a way to print")
{
	std::set<std::string> names;
	for (int i = 0; i <= static_cast<int>(PropTarget::MatOpacity); ++i)
	{
		const auto t = static_cast<PropTarget>(i);
		const std::string name = targetName(t);
		CHECK(name.size() >= 5);
		CHECK_MESSAGE(names.insert(name).second, "two targets share the name ", name);
		const std::string group = targetGroup(t);
		CHECK((group == "Transform" || group == "Material"));
		// The first nine write into the TransformComponent, the rest into the
		// material — the same split PropertyAnimationSystem's switch makes.
		CHECK(group == (i < 9 ? "Transform" : "Material"));
	}
	CHECK(names.size() == 15);

	char buf[32];
	formatValue(PropTarget::RotY, 90.0f, buf, sizeof(buf));
	// Rotation is Euler degrees on the component; the readout says so.
	CHECK(std::strstr(buf, "90.0") != nullptr);
	CHECK(std::strstr(buf, "\xC2\xB0") != nullptr);
	formatValue(PropTarget::PosX, 1.5f, buf, sizeof(buf));
	CHECK(std::string(buf) == "1.500");
}

TEST_CASE("sequencer: the value beside a track is the runtime's own sample")
{
	// Not a re-implementation: the strip prints PropertyAnimationSystem's
	// sampleChannel, so if this holds the readout can never disagree with what
	// the Property Animator writes into the entity.
	const PropertyAnimClipAsset clip = twoTrackClip();
	const PropertyAnimChannel& pos = clip.channels[0];
	CHECK(PropertyAnimationSystem::sampleChannel(pos, 0.25f) == doctest::Approx(0.5f));
	CHECK(PropertyAnimationSystem::sampleChannel(pos, 1.25f) == doctest::Approx(2.0f));
	// Held at the ends, not extrapolated.
	CHECK(PropertyAnimationSystem::sampleChannel(pos, -1.0f) == doctest::Approx(0.0f));
	CHECK(PropertyAnimationSystem::sampleChannel(pos,  9.0f) == doctest::Approx(3.0f));
	PropertyAnimChannel empty;
	CHECK(PropertyAnimationSystem::sampleChannel(empty, 0.5f) == 0.0f);
}

TEST_CASE("sequencer: dragging the ruler scrubs, and keeps scrubbing below it")
{
	ImGuiCtx ctx;
	PropertyAnimClipAsset clip = twoTrackClip();
	View view;

	// A frame with the pointer nowhere, to learn where the lane is.
	mouseAt(-100.0f, -100.0f);
	Result r = frame(clip, view);
	REQUIRE(r.laneW > 100.0f);
	const HE::Ed::UITimelineView tv = viewOf(r, clip, view);
	const float rulerY = r.top + metrics().rulerH * 0.5f;

	// Press on the ruler at a quarter of the lane.
	const float xA = r.laneX + r.laneW * 0.25f;
	mouseAt(xA, rulerY);
	mouseButton(true);
	r = frame(clip, view);
	CHECK(r.playheadMoved);
	CHECK(view.scrubbing);
	CHECK(view.playhead == doctest::Approx(tv.tOf(xA)).epsilon(0.01));
	CHECK(view.playhead == doctest::Approx(0.5f).epsilon(0.02));

	// Drag to three quarters — and DOWN, off the ruler's own strip, into the
	// track rows. The playhead has to keep following: a scrub that stopped
	// the moment the hand drifted would be a scrub you cannot do.
	const float xB = r.laneX + r.laneW * 0.75f;
	mouseAt(xB, r.top + 80.0f);
	r = frame(clip, view);
	CHECK(r.playheadMoved);
	CHECK(view.playhead == doctest::Approx(tv.tOf(xB)).epsilon(0.01));
	CHECK(view.playhead == doctest::Approx(1.5f).epsilon(0.02));

	// Past the right edge clamps to the clip, never beyond it.
	mouseAt(r.laneX + r.laneW + 300.0f, rulerY);
	frame(clip, view);
	CHECK(view.playhead == doctest::Approx(clip.duration));

	// Release ends the gesture; moving afterwards moves nothing.
	mouseButton(false);
	frame(clip, view);
	CHECK_FALSE(view.scrubbing);
	mouseAt(xA, rulerY);
	r = frame(clip, view);
	CHECK_FALSE(r.playheadMoved);
	CHECK(view.playhead == doctest::Approx(clip.duration));
}

TEST_CASE("sequencer: clicking a key selects it and puts the playhead on it")
{
	ImGuiCtx ctx;
	PropertyAnimClipAsset clip = twoTrackClip();
	View view;

	mouseAt(-100.0f, -100.0f);
	Result r = frame(clip, view);
	const HE::Ed::UITimelineView tv = viewOf(r, clip, view);
	const Metrics& M = metrics();
	const float rowsTop = r.top + M.rulerH + 2.0f;

	// The second key of the first track: 0.5 s, value 1.
	const float cx = tv.xOf(0.5f);
	const float cy = rowsTop + M.rowH * 0.5f;
	mouseAt(cx, cy);
	mouseButton(true);
	r = frame(clip, view);
	CHECK(r.selectionChanged);
	CHECK(r.playheadMoved);
	CHECK(view.trackSel == 0);
	CHECK(view.keySel   == 1);
	CHECK(view.playhead == doctest::Approx(0.5f));
	// A key click is not a scrub: releasing and moving must not drag the
	// playhead along.
	CHECK_FALSE(view.scrubbing);
	mouseButton(false);
	frame(clip, view);

	// The second track's last key, one row down.
	const float cx2 = tv.xOf(1.0f);
	const float cy2 = rowsTop + M.rowH * 1.5f;
	mouseAt(cx2, cy2);
	mouseButton(true);
	r = frame(clip, view);
	CHECK(view.trackSel == 1);
	CHECK(view.keySel   == 1);
	CHECK(view.playhead == doctest::Approx(1.0f));
	mouseButton(false);
	frame(clip, view);

	// Clicking the track NAME selects the track and drops the key. A
	// Selectable fires on release, unlike the diamonds, which take the press.
	mouseAt(r.laneX - M.gap - 100.0f, rowsTop + M.rowH * 0.5f);
	mouseButton(true);
	frame(clip, view);
	mouseButton(false);
	r = frame(clip, view);
	CHECK(r.selectionChanged);
	CHECK(view.trackSel == 0);
	CHECK(view.keySel   == -1);
	CHECK(view.playhead == doctest::Approx(1.0f));   // the name is not a key
}

TEST_CASE("sequencer: the wheel zooms around the pointer and Fit brings it back")
{
	ImGuiCtx ctx;
	PropertyAnimClipAsset clip = twoTrackClip();
	View view;

	mouseAt(-100.0f, -100.0f);
	Result r = frame(clip, view);
	const HE::Ed::UITimelineView before = viewOf(r, clip, view);

	// Hover the lane at 1.5 s and spin the wheel. The pointer needs a frame
	// over the strip before ImGui counts the window as hovered.
	const float mx = before.xOf(1.5f), my = r.top + 60.0f;
	mouseAt(mx, my);
	frame(clip, view);
	mouseWheel(1.0f);
	r = frame(clip, view);
	CHECK(view.zoom == doctest::Approx(1.25f));
	// The second under the pointer stayed where it was.
	const HE::Ed::UITimelineView after = viewOf(r, clip, view);
	CHECK(after.tOf(mx) == doctest::Approx(1.5f).epsilon(0.01));

	// Twice more, then the toolbar's Zoom In around the playhead.
	mouseWheel(1.0f); frame(clip, view);
	mouseWheel(1.0f); frame(clip, view);
	CHECK(view.zoom == doctest::Approx(1.25f * 1.25f * 1.25f));
	view.playhead = 0.25f;
	Intent in; in.zoom = 1;
	frame(clip, view, in);
	CHECK(view.zoom == doctest::Approx(1.25f * 1.25f * 1.25f * 1.5f));

	// Fit is the way back: zoom 1, scroll 0.
	Intent fit; fit.fit = true;
	frame(clip, view, fit);
	CHECK(view.zoom   == doctest::Approx(1.0f));
	CHECK(view.scroll == doctest::Approx(0.0f));

	// Zooming out below fit is refused — there is nothing outside a clip.
	Intent outI; outI.zoom = -1;
	frame(clip, view, outI);
	CHECK(view.zoom == doctest::Approx(1.0f));
}

TEST_CASE("sequencer: a selection outliving its track is dropped, not dereferenced")
{
	ImGuiCtx ctx;
	PropertyAnimClipAsset clip = twoTrackClip();
	View view;
	view.trackSel = 1;
	view.keySel   = 1;
	view.playhead = 5.0f;   // past the end, as a stale view might hold

	mouseAt(-100.0f, -100.0f);
	frame(clip, view);
	CHECK(view.playhead == doctest::Approx(clip.duration));
	CHECK(view.trackSel == 1);
	CHECK(view.keySel   == 1);

	// The track goes; the selection goes with it, and the strip still draws.
	clip.channels.pop_back();
	frame(clip, view);
	CHECK(view.trackSel == -1);
	CHECK(view.keySel   == -1);

	// An empty clip draws its hint and nothing else, and a zero length does
	// not divide by itself.
	clip.channels.clear();
	clip.duration = 0.0f;
	const Result r = frame(clip, view);
	CHECK(r.laneW > 0.0f);
	CHECK(std::isfinite(view.playhead));
}

// ── Authoring: the clip after the gesture ────────────────────────────────────
// Step 2 of the sequencer. The edit functions keep what sampleChannel silently
// relies on — times strictly ascending, one value per time — and the strip's
// gestures go through them. Checked here in that order: the functions on their
// own, then a drag, a double-click, a Delete, and the same in curve view.

namespace
{
	bool sortedPairs(const PropertyAnimChannel& ch)
	{
		if (ch.times.size() != ch.values.size()) return false;
		for (size_t i = 1; i < ch.times.size(); ++i)
			if (!(ch.times[i] > ch.times[i - 1])) return false;
		return true;
	}

	void keyEvent(ImGuiKey key, bool down) { ImGui::GetIO().AddKeyEvent(key, down); }
}

TEST_CASE("sequencer: editing keeps the channel sorted and paired")
{
	PropertyAnimChannel ch;
	ch.target = PropTarget::PosY;

	// Inserted out of order, stored in order; the index says where it went.
	CHECK(insertKey(ch, 1.0f, 10.0f) == 0);
	CHECK(insertKey(ch, 0.0f,  0.0f) == 0);
	CHECK(insertKey(ch, 0.5f,  5.0f) == 1);
	CHECK(sortedPairs(ch));
	CHECK(ch.times.size() == 3);
	// Onto an existing moment: the value is replaced, nothing is added.
	CHECK(insertKey(ch, 0.5f + kKeyEpsilon * 0.5f, 7.0f) == 1);
	CHECK(ch.times.size() == 3);
	CHECK(ch.values[1] == doctest::Approx(7.0f));
	// A negative time is not a time.
	CHECK(insertKey(ch, -3.0f, 1.0f) == 0);
	CHECK(ch.times[0] == 0.0f);
	CHECK(ch.values[0] == doctest::Approx(1.0f));

	// Moving past a neighbour: the key comes back under its new index, in
	// order, and the value travelled with it.
	CHECK(moveKey(ch, 1, 1.5f) == 2);
	CHECK(sortedPairs(ch));
	CHECK(ch.times[2] == doctest::Approx(1.5f));
	CHECK(ch.values[2] == doctest::Approx(7.0f));
	// Onto another key: nudged beside it, on the side it came from, never
	// merged — dragging must not silently lose a key.
	CHECK(moveKey(ch, 2, 1.0f) == 2);
	CHECK(ch.times[2] > 1.0f);
	CHECK(ch.times[2] - 1.0f < 3.0f * kKeyEpsilon);
	CHECK(sortedPairs(ch));
	CHECK(moveKey(ch, 0, 1.0f) == 0);   // from the left: lands just before
	CHECK(ch.times[0] < 1.0f);
	CHECK(sortedPairs(ch));
	CHECK(moveKey(ch, 7, 0.0f) == -1);

	CHECK(removeKey(ch, 1));
	CHECK(ch.times.size() == 2);
	CHECK(sortedPairs(ch));
	CHECK_FALSE(removeKey(ch, 2));

	// A file whose two vectors disagree is edited from the shorter one.
	ch.values.push_back(99.0f);
	insertKey(ch, 5.0f, 1.0f);
	CHECK(sortedPairs(ch));
}

TEST_CASE("sequencer: tracks, defaults and the clip's length")
{
	PropertyAnimClipAsset clip;
	CHECK(clip.duration == 0.0f);
	// The first track gives a lengthless clip a lane to sit on.
	CHECK(addTrack(clip, PropTarget::ScaleX) == 0);
	CHECK(clip.duration == doctest::Approx(kDefaultDuration));
	CHECK(clip.channels[0].times  == std::vector<float>{ 0.0f });
	CHECK(clip.channels[0].values == std::vector<float>{ 1.0f });   // scale rests at 1
	CHECK(addTrack(clip, PropTarget::MatRoughness) == 1);
	CHECK(clip.channels[1].values == std::vector<float>{ 0.0f });   // roughness at 0
	// The same property twice is the same track.
	CHECK(addTrack(clip, PropTarget::ScaleX) == 0);
	CHECK(clip.channels.size() == 2);
	CHECK(findTrack(clip, PropTarget::PosZ) == -1);

	// Every default is 0 or 1, and the ones that mean "unchanged" are 1.
	for (int i = 0; i < kTargetCount; ++i)
	{
		const float d = defaultValue(static_cast<PropTarget>(i));
		CHECK((d == 0.0f || d == 1.0f));
	}
	CHECK(defaultValue(PropTarget::MatOpacity) == 1.0f);
	CHECK(defaultValue(PropTarget::MatColorG)  == 1.0f);
	CHECK(defaultValue(PropTarget::PosX)       == 0.0f);
	CHECK(defaultValue(PropTarget::RotZ)       == 0.0f);

	// The length never drops under the last key: a key past the end could
	// not be pointed at.
	insertKey(clip.channels[1], 1.8f, 0.5f);
	CHECK(lastKeyTime(clip) == doctest::Approx(1.8f));
	CHECK(setDuration(clip, 0.5f) == doctest::Approx(1.8f));
	CHECK(setDuration(clip, 3.0f) == doctest::Approx(3.0f));

	CHECK(removeTrack(clip, 0));
	CHECK(clip.channels.size() == 1);
	CHECK(clip.channels[0].target == PropTarget::MatRoughness);
	CHECK_FALSE(removeTrack(clip, 1));

	// The curve view's axis: the range with a margin, and a flat track gets
	// a span of one around itself rather than none.
	float lo = 0.0f, hi = 0.0f;
	valueRange(clip.channels[0], lo, hi);
	CHECK(lo == doctest::Approx(0.0f - 0.05f));
	CHECK(hi == doctest::Approx(0.5f + 0.05f));
	PropertyAnimChannel flat;
	flat.times = { 0.0f, 1.0f }; flat.values = { 2.0f, 2.0f };
	valueRange(flat, lo, hi);
	CHECK(lo < 2.0f);
	CHECK(hi > 2.0f);
	CHECK(hi - lo == doctest::Approx(1.2f));
	ValueAxis axis{ 100.0f, 200.0f, lo, hi };
	CHECK(axis.yOf(hi) == doctest::Approx(100.0f));
	CHECK(axis.yOf(lo) == doctest::Approx(300.0f));
	CHECK(axis.vOf(axis.yOf(2.0f)) == doctest::Approx(2.0f));
}

TEST_CASE("sequencer: dragging a key moves it in time, past its neighbour, as one edit")
{
	ImGuiCtx ctx;
	PropertyAnimClipAsset clip = twoTrackClip();
	// Keys at 0, 0.5, 1.0, 2.0 on the first track, so there is a neighbour
	// to cross.
	insertKey(clip.channels[0], 1.0f, 2.0f);
	View view;

	mouseAt(-100.0f, -100.0f);
	Result r = frame(clip, view);
	const HE::Ed::UITimelineView tv = viewOf(r, clip, view);
	const Metrics& M = metrics();
	const float rowsTop = r.top + M.rulerH + 2.0f;
	const float cy = rowsTop + M.rowH * 0.5f;

	// Press the 0.5 s key. A press is a look, not a move: nothing is edited.
	mouseAt(tv.xOf(0.5f), cy);
	mouseButton(true);
	r = frame(clip, view);
	CHECK(view.keySel == 1);
	CHECK(view.keyArmed);
	CHECK_FALSE(view.dragging);
	CHECK_FALSE(r.edited);

	// Drag to 1.5 s, across the key at 1.0 s. The key keeps being the
	// selected one under its NEW index, the track stays sorted, the playhead
	// rides along, and the value went with the key.
	mouseAt(tv.xOf(1.5f), cy);
	r = frame(clip, view);
	CHECK(view.dragging);
	CHECK(r.edited);
	CHECK_FALSE(r.committed);
	CHECK(view.keySel == 2);
	CHECK(clip.channels[0].times[2]  == doctest::Approx(1.5f).epsilon(0.01));
	CHECK(clip.channels[0].values[2] == doctest::Approx(1.0f));
	CHECK(clip.channels[0].times[1]  == doctest::Approx(1.0f));
	CHECK(sortedPairs(clip.channels[0]));
	CHECK(view.playhead == doctest::Approx(1.5f).epsilon(0.01));
	CHECK(r.playheadMoved);

	// Further, onto the last key at 2 s: beside it, never merged.
	mouseAt(tv.xOf(2.0f) + 40.0f, cy);
	r = frame(clip, view);
	CHECK(clip.channels[0].times.size() == 4);
	CHECK(sortedPairs(clip.channels[0]));
	CHECK(view.keySel == 2);
	CHECK(clip.channels[0].times[2] < 2.0f);

	// Release: the drag is over and it is ONE undo point.
	mouseButton(false);
	r = frame(clip, view);
	CHECK(r.committed);
	CHECK_FALSE(view.keyArmed);
	CHECK_FALSE(view.dragging);
	// Moving afterwards moves nothing.
	mouseAt(tv.xOf(0.25f), cy);
	r = frame(clip, view);
	CHECK_FALSE(r.edited);
	CHECK(clip.channels[0].times.size() == 4);
}

TEST_CASE("sequencer: double-clicking a lane adds a key holding the sampled value")
{
	ImGuiCtx ctx;
	PropertyAnimClipAsset clip = twoTrackClip();
	View view;

	mouseAt(-100.0f, -100.0f);
	Result r = frame(clip, view);
	const HE::Ed::UITimelineView tv = viewOf(r, clip, view);
	const Metrics& M = metrics();
	const float rowsTop = r.top + M.rulerH + 2.0f;

	// The second track (roughness 0.2 → 0.8 over 0..1 s) at 0.5 s: no key
	// there, and the curve is 0.5 at that moment.
	const float x = tv.xOf(0.5f), y = rowsTop + M.rowH * 1.5f;
	mouseAt(x, y);
	mouseButton(true);  frame(clip, view);
	mouseButton(false); frame(clip, view);
	mouseButton(true);  r = frame(clip, view);
	mouseButton(false); frame(clip, view);
	REQUIRE(clip.channels[1].times.size() == 3);
	CHECK(r.edited);
	CHECK(r.committed);
	CHECK(view.trackSel == 1);
	CHECK(view.keySel   == 1);
	CHECK(clip.channels[1].times[1]  == doctest::Approx(0.5f).epsilon(0.01));
	CHECK(clip.channels[1].values[1] == doctest::Approx(0.5f).epsilon(0.02));
	CHECK(sortedPairs(clip.channels[1]));
	CHECK(view.playhead == doctest::Approx(clip.channels[1].times[1]));
	// The curve did not bend: the same sample before and after.
	CHECK(PropertyAnimationSystem::sampleChannel(clip.channels[1], 0.75f) == doctest::Approx(0.65f).epsilon(0.02));

	// Delete removes the selected key. The window is focused by the clicks
	// above; the strip listens for Delete only, never Backspace.
	keyEvent(ImGuiKey_Delete, true);
	r = frame(clip, view);
	keyEvent(ImGuiKey_Delete, false);
	frame(clip, view);
	CHECK(r.edited);
	CHECK(r.committed);
	CHECK(clip.channels[1].times.size() == 2);
	CHECK(view.keySel == -1);
	CHECK(view.trackSel == 1);
	CHECK(sortedPairs(clip.channels[1]));

	// Backspace, with a key selected, does nothing.
	view.keySel = 0;
	keyEvent(ImGuiKey_Backspace, true);
	r = frame(clip, view);
	keyEvent(ImGuiKey_Backspace, false);
	frame(clip, view);
	CHECK_FALSE(r.edited);
	CHECK(clip.channels[1].times.size() == 2);
}

TEST_CASE("sequencer: the curve view drags a key in value as well as time")
{
	ImGuiCtx ctx;
	PropertyAnimClipAsset clip = twoTrackClip();
	View view;
	view.curves   = true;
	view.trackSel = 0;   // Position X: 0 → 1 → 3

	mouseAt(-100.0f, -100.0f);
	Result r = frame(clip, view);
	const HE::Ed::UITimelineView tv = viewOf(r, clip, view);
	const ValueAxis axis{ r.graphTop, r.graphH, r.valueLo, r.valueHi };
	float lo = 0.0f, hi = 0.0f;
	valueRange(clip.channels[0], lo, hi);
	CHECK(r.valueLo == doctest::Approx(lo));
	CHECK(r.valueHi == doctest::Approx(hi));
	REQUIRE(r.graphH > 60.0f);

	// Press the middle key — (0.5 s, 1) — and drag it to (1.0 s, 2.5).
	mouseAt(tv.xOf(0.5f), axis.yOf(1.0f));
	mouseButton(true);
	r = frame(clip, view);
	CHECK(view.keySel == 1);
	CHECK(view.keyArmed);
	mouseAt(tv.xOf(1.0f), axis.yOf(2.5f));
	r = frame(clip, view);
	CHECK(r.edited);
	CHECK(view.dragging);
	CHECK(clip.channels[0].times[1]  == doctest::Approx(1.0f).epsilon(0.01));
	CHECK(clip.channels[0].values[1] == doctest::Approx(2.5f).epsilon(0.02));
	// The axis held still while the key was held: the range under the
	// pointer is the one it was pressed in, so the value did not run away.
	CHECK(r.valueLo == doctest::Approx(lo));
	CHECK(r.valueHi == doctest::Approx(hi));
	mouseButton(false);
	r = frame(clip, view);
	CHECK(r.committed);
	CHECK(sortedPairs(clip.channels[0]));

	// A double-click on empty graph puts a key at the pointer's time AND
	// value — here the pointer is a place on the graph, not a moment on a
	// lane. Re-read the axis first: the range grew with the drag above.
	r = frame(clip, view);
	const ValueAxis axis2{ r.graphTop, r.graphH, r.valueLo, r.valueHi };
	const float x = tv.xOf(1.5f), y = axis2.yOf(0.5f);
	mouseAt(x, y);
	mouseButton(true);  frame(clip, view);
	mouseButton(false); frame(clip, view);
	mouseButton(true);  r = frame(clip, view);
	mouseButton(false); frame(clip, view);
	REQUIRE(clip.channels[0].times.size() == 4);
	CHECK(r.committed);
	CHECK(view.keySel == 2);
	CHECK(clip.channels[0].times[2]  == doctest::Approx(1.5f).epsilon(0.01));
	CHECK(clip.channels[0].values[2] == doctest::Approx(0.5f).epsilon(0.05));
	CHECK(sortedPairs(clip.channels[0]));

	// The track list on the left still selects: click the second track's
	// name and the graph is its.
	mouseAt(r.laneX - metrics().gap - 100.0f, r.top + metrics().rulerH + 2.0f + metrics().rowH * 1.5f);
	mouseButton(true);  frame(clip, view);
	mouseButton(false); r = frame(clip, view);
	CHECK(view.trackSel == 1);
	CHECK(view.keySel == -1);
	r = frame(clip, view);
	valueRange(clip.channels[1], lo, hi);
	CHECK(r.valueLo == doctest::Approx(lo));
	CHECK(r.valueHi == doctest::Approx(hi));
}

// ── The clip on disk, and the clip in the scene ──────────────────────────────
// Everything above works on a clip on the stack. These are the two ends the
// panel hangs between: the file the Content Browser makes and the Sequencer
// saves, and the entity the preview writes into.

namespace
{
	struct TempContentDir
	{
		std::filesystem::path path;
		explicit TempContentDir(const char* name)
		{
			path = std::filesystem::temp_directory_path() / name;
			he_test::removeAllQuiet(path);
			std::filesystem::create_directories(path);
		}
		~TempContentDir() { he_test::removeAllQuiet(path); }
	};
}

TEST_CASE("sequencer: a clip survives a save and a reload, and a stub reads as an empty clip")
{
	TempContentDir dir("he_test_sequencer_panm");

	// What the Content Browser's "Property Animation Clip" writes: META and
	// nothing else. The loader must hand back a clip for it — one of no
	// length with no tracks — rather than nothing, or the tab would open on
	// "could not be loaded" for every clip anybody ever creates.
	{
		const std::filesystem::path abs = dir.path / "NewPropertyAnimation.hasset";
		REQUIRE(HE::Ed::isCreatableAssetType(HE::AssetType::PropertyAnimClip));
		REQUIRE(HE::Ed::writeAssetStub(abs.string(), "NewPropertyAnimation.hasset",
		                               "NewPropertyAnimation", HE::AssetType::PropertyAnimClip));
		ContentManager cm(dir.path.string());
		const HE::UUID id = cm.loadAsset("NewPropertyAnimation.hasset");
		REQUIRE(id != HE::UUID{});
		PropertyAnimClipAsset* clip = cm.getPropertyAnimClipMutable(id);
		REQUIRE(clip != nullptr);
		CHECK(clip->type == HE::AssetType::PropertyAnimClip);
		CHECK(clip->duration == 0.0f);
		CHECK(clip->channels.empty());

		// The Sequencer's first edits, then the save button.
		addTrack(*clip, PropTarget::PosX);
		insertKey(clip->channels[0], 0.5f, 1.0f);
		insertKey(clip->channels[0], 2.0f, 3.0f);
		addTrack(*clip, PropTarget::MatRoughness);
		insertKey(clip->channels[1], 1.0f, 0.8f);
		setDuration(*clip, 2.5f);
		REQUIRE(cm.saveAsset(*clip));
	}

	// A fresh manager reads back exactly what was authored: the length, both
	// tracks in order, every key's time and value.
	{
		ContentManager cm(dir.path.string());
		const HE::UUID id = cm.loadAsset("NewPropertyAnimation.hasset");
		const PropertyAnimClipAsset* clip = cm.getPropertyAnimClip(id);
		REQUIRE(clip != nullptr);
		CHECK(clip->duration == doctest::Approx(2.5f));
		REQUIRE(clip->channels.size() == 2);
		CHECK(clip->channels[0].target == PropTarget::PosX);
		CHECK(clip->channels[0].times  == std::vector<float>{ 0.0f, 0.5f, 2.0f });
		CHECK(clip->channels[0].values == std::vector<float>{ 0.0f, 1.0f, 3.0f });
		CHECK(clip->channels[1].target == PropTarget::MatRoughness);
		CHECK(clip->channels[1].times  == std::vector<float>{ 0.0f, 1.0f });
		CHECK(clip->channels[1].values == std::vector<float>{ 0.0f, 0.8f });
	}

	// A clip whose tracks were all removed is still a clip of its length, not
	// a stub again: the length is written even with nothing under it.
	{
		ContentManager cm(dir.path.string());
		PropertyAnimClipAsset* clip = cm.getPropertyAnimClipMutable(cm.loadAsset("NewPropertyAnimation.hasset"));
		REQUIRE(clip != nullptr);
		REQUIRE(removeTrack(*clip, 1));
		REQUIRE(removeTrack(*clip, 0));
		REQUIRE(cm.saveAsset(*clip));
	}
	{
		ContentManager cm(dir.path.string());
		const PropertyAnimClipAsset* clip = cm.getPropertyAnimClip(cm.loadAsset("NewPropertyAnimation.hasset"));
		REQUIRE(clip != nullptr);
		CHECK(clip->duration == doctest::Approx(2.5f));
		CHECK(clip->channels.empty());
	}
}

TEST_CASE("sequencer: the transport follows the runtime's playhead rule")
{
	View view;
	// Not playing: nothing moves, whatever the dt.
	view.playhead = 0.25f;
	CHECK_FALSE(advancePlayhead(view, 2.0f, 0.5f));
	CHECK(view.playhead == doctest::Approx(0.25f));

	// Playing, looping: on by dt, and round the end into [0, duration).
	view.playing = true;
	view.loop    = true;
	CHECK(advancePlayhead(view, 2.0f, 0.5f));
	CHECK(view.playhead == doctest::Approx(0.75f));
	CHECK(advancePlayhead(view, 2.0f, 1.5f));
	CHECK(view.playhead == doctest::Approx(0.25f));
	CHECK(view.playing);

	// Not looping: the end is where it stops, and Play goes off by itself,
	// exactly what a non-looping Property Animator does in the game.
	view.loop = false;
	CHECK(advancePlayhead(view, 2.0f, 5.0f));
	CHECK(view.playhead == doctest::Approx(2.0f));
	CHECK_FALSE(view.playing);
	// Sitting at the end and not playing: no movement to report.
	CHECK_FALSE(advancePlayhead(view, 2.0f, 0.1f));

	// A clip of no length cannot play: Play on the empty clip switches
	// itself off rather than sitting "playing" over nothing.
	view.playing  = true;
	view.playhead = 0.0f;
	CHECK_FALSE(advancePlayhead(view, 0.0f, 0.1f));
	CHECK_FALSE(view.playing);
	CHECK(view.playhead == 0.0f);
}

TEST_CASE("sequencer: the preview writes an actor the way the runtime would, whatever its own clock says")
{
	ContentManager cm;
	HorizonWorld   world;

	MaterialAsset mat;
	mat.name = "actorMat";
	mat.roughness = 0.1f;
	const HE::UUID matId = cm.registerMaterial(std::move(mat));

	PropertyAnimClipAsset clip;
	clip.duration = 2.0f;
	PropertyAnimChannel pos;   pos.target = PropTarget::PosX;         pos.times = { 0.0f, 2.0f }; pos.values = { 0.0f, 4.0f };
	PropertyAnimChannel rot;   rot.target = PropTarget::RotY;         rot.times = { 0.0f, 1.0f }; rot.values = { 0.0f, 90.0f };
	PropertyAnimChannel rough; rough.target = PropTarget::MatRoughness; rough.times = { 0.0f, 2.0f }; rough.values = { 0.2f, 0.8f };
	clip.channels = { pos, rot, rough };

	const entt::entity e = world.createEntity();
	TransformComponent tc; tc.position = {}; tc.rotation = {}; tc.scale = glm::vec3(1.0f); tc.dirty = false;
	world.addComponent(e, tc);
	MaterialComponent mc; mc.materialAssetId = matId; mc.dirty = false;
	world.addComponent(e, mc);
	// The actor is paused on its own clock and somewhere else in the clip:
	// the preview is the Sequencer's playhead, not the component's, and it
	// does not need the component to be playing.
	PropertyAnimatorComponent pa;
	pa.playing = false; pa.playbackTime = 1.7f;
	world.addComponent(e, pa);

	PropertyAnimationSystem::applyAt(world, cm, e, clip, 0.5f);
	auto& reg = world.registry();
	CHECK(reg.get<TransformComponent>(e).position.x == doctest::Approx(1.0f));
	CHECK(reg.get<TransformComponent>(e).rotation.y == doctest::Approx(45.0f));
	CHECK(reg.get<TransformComponent>(e).dirty);
	CHECK(cm.getMaterial(matId)->roughness == doctest::Approx(0.35f));
	CHECK(reg.get<MaterialComponent>(e).dirty);
	// The component's own clock is not this function's business.
	CHECK(reg.get<PropertyAnimatorComponent>(e).playbackTime == doctest::Approx(1.7f));

	// Past the last key the track holds its last value, the same rule the
	// strip's readout uses (sampleChannel), so what the tab prints beside the
	// track name is what lands in the entity.
	PropertyAnimationSystem::applyAt(world, cm, e, clip, 1.5f);
	CHECK(reg.get<TransformComponent>(e).rotation.y == doctest::Approx(90.0f));
	CHECK(reg.get<TransformComponent>(e).position.x ==
	      doctest::Approx(PropertyAnimationSystem::sampleChannel(clip.channels[0], 1.5f)));

	// An entity without the component the track writes into is left alone,
	// not crashed into: a clip with material tracks on a bare transform.
	const entt::entity bare = world.createEntity();
	world.addComponent(bare, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
	PropertyAnimationSystem::applyAt(world, cm, bare, clip, 1.0f);
	CHECK(reg.get<TransformComponent>(bare).position.x == doctest::Approx(2.0f));

	// And update() is applyAt after the advance: a playing actor ticked by dt
	// lands where applyAt would put it at its new time.
	const HE::UUID clipId = cm.registerPropertyAnimClip(std::move(clip));
	auto& live = reg.get<PropertyAnimatorComponent>(e);
	live.clipId = clipId; live.playing = true; live.playbackTime = 0.0f; live.looping = false;
	PropertyAnimationSystem::update(world, cm, 0.25f);
	CHECK(live.playbackTime == doctest::Approx(0.25f));
	CHECK(reg.get<TransformComponent>(e).position.x == doctest::Approx(0.5f));
}
