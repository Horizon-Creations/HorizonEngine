#include "doctest.h"

#include "SequencerTimeline.h"
#include "UITimelineMath.h"

#include <ContentManager/Assets.h>
#include <HorizonScene/PropertyAnimationSystem.h>

#include <imgui.h>

#include <cmath>
#include <cstring>
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
	Result frame(const PropertyAnimClipAsset& clip, View& view, const Intent& intent = {})
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
	const PropertyAnimClipAsset clip = twoTrackClip();
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
	const PropertyAnimClipAsset clip = twoTrackClip();
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
	const PropertyAnimClipAsset clip = twoTrackClip();
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
