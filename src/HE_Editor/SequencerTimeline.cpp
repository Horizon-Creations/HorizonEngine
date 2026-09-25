#include "SequencerTimeline.h"

#include <HorizonScene/PropertyAnimationSystem.h>   // sampleChannel + advance — the runtime's rules

#include <cstdio>

namespace HE::Ed::Sequencer
{

const char* targetName(PropTarget t)
{
	switch (t)
	{
	case PropTarget::PosX:         return "Position X";
	case PropTarget::PosY:         return "Position Y";
	case PropTarget::PosZ:         return "Position Z";
	case PropTarget::RotX:         return "Rotation X";
	case PropTarget::RotY:         return "Rotation Y";
	case PropTarget::RotZ:         return "Rotation Z";
	case PropTarget::ScaleX:       return "Scale X";
	case PropTarget::ScaleY:       return "Scale Y";
	case PropTarget::ScaleZ:       return "Scale Z";
	case PropTarget::MatColorR:    return "Color R";
	case PropTarget::MatColorG:    return "Color G";
	case PropTarget::MatColorB:    return "Color B";
	case PropTarget::MatMetallic:  return "Metallic";
	case PropTarget::MatRoughness: return "Roughness";
	case PropTarget::MatOpacity:   return "Opacity";
	case PropTarget::CameraFov:    return "Field of View";
	case PropTarget::Visible:      return "Visible";
	}
	// Only reachable through a file whose byte nobody wrote: said out loud
	// rather than shown as "Position X", which would be a lie the runtime does
	// not tell (PropertyAnimationSystem's switch ignores it).
	return "(unknown target)";
}

const char* targetGroup(PropTarget t)
{
	switch (t)
	{
	case PropTarget::PosX:   case PropTarget::PosY:   case PropTarget::PosZ:
	case PropTarget::RotX:   case PropTarget::RotY:   case PropTarget::RotZ:
	case PropTarget::ScaleX: case PropTarget::ScaleY: case PropTarget::ScaleZ:
		return "Transform";
	case PropTarget::MatColorR:   case PropTarget::MatColorG: case PropTarget::MatColorB:
	case PropTarget::MatMetallic: case PropTarget::MatRoughness:
	case PropTarget::MatOpacity:
		return "Material";
	case PropTarget::CameraFov:
		return "Camera";
	case PropTarget::Visible:
		return "Visibility";
	}
	return "";
}

void formatValue(PropTarget t, float v, char* buf, size_t n)
{
	switch (t)
	{
	case PropTarget::RotX: case PropTarget::RotY: case PropTarget::RotZ:
		// TransformComponent::rotation is Euler degrees; the readout says so
		// rather than leaving a reader to guess radians.
		std::snprintf(buf, n, "%.1f\xC2\xB0", v);
		break;
	case PropTarget::CameraFov:
		std::snprintf(buf, n, "%.1f\xC2\xB0", v);
		break;
	case PropTarget::Visible:
		// A switch reads as one (the runtime's own threshold, applyChannel).
		std::snprintf(buf, n, "%s", v >= 0.5f ? "on" : "off");
		break;
	default:
		std::snprintf(buf, n, "%.3f", v);
		break;
	}
}

const Metrics& metrics()
{
	static const Metrics m;
	return m;
}

// ── Editing ──────────────────────────────────────────────────────────────────

namespace
{
	// The index of the key at `t`, within kKeyEpsilon, or -1. `skip` is a key
	// to look past — the one being moved, which of course sits at its own time.
	int keyAt(const PropertyAnimChannel& ch, float t, int skip = -1)
	{
		for (int i = 0; i < static_cast<int>(ch.times.size()); ++i)
			if (i != skip && std::fabs(ch.times[i] - t) < kKeyEpsilon) return i;
		return -1;
	}

	// A channel whose two vectors disagree in length is a file nobody wrote;
	// the shorter one is the truth (sampleChannel reads pairs), and editing
	// starts from that rather than indexing past the end of the shorter.
	void reconcile(PropertyAnimChannel& ch)
	{
		const size_t n = std::min(ch.times.size(), ch.values.size());
		ch.times.resize(n);
		ch.values.resize(n);
	}
}

int insertKey(PropertyAnimChannel& ch, float t, float v)
{
	reconcile(ch);
	t = std::max(t, 0.0f);
	if (const int k = keyAt(ch, t); k >= 0) { ch.values[k] = v; return k; }
	const auto it = std::lower_bound(ch.times.begin(), ch.times.end(), t);
	const int  k  = static_cast<int>(it - ch.times.begin());
	ch.times.insert(it, t);
	ch.values.insert(ch.values.begin() + k, v);
	return k;
}

int moveKey(PropertyAnimChannel& ch, int k, float t)
{
	reconcile(ch);
	if (k < 0 || k >= static_cast<int>(ch.times.size())) return -1;
	const float from = ch.times[k];
	const float v    = ch.values[k];
	t = std::max(t, 0.0f);
	// Not onto another key: nudged to just beside it, on the side the key came
	// from. Bounded, because two keys can themselves sit a nudge apart.
	for (int guard = 0; guard < 64; ++guard)
	{
		const int other = keyAt(ch, t, k);
		if (other < 0) break;
		t = from < ch.times[other] ? ch.times[other] - 2.0f * kKeyEpsilon
		                           : ch.times[other] + 2.0f * kKeyEpsilon;
		t = std::max(t, 0.0f);
	}
	ch.times.erase(ch.times.begin() + k);
	ch.values.erase(ch.values.begin() + k);
	const auto it = std::lower_bound(ch.times.begin(), ch.times.end(), t);
	const int  nk = static_cast<int>(it - ch.times.begin());
	ch.times.insert(it, t);
	ch.values.insert(ch.values.begin() + nk, v);
	return nk;
}

bool removeKey(PropertyAnimChannel& ch, int k)
{
	reconcile(ch);
	if (k < 0 || k >= static_cast<int>(ch.times.size())) return false;
	ch.times.erase(ch.times.begin() + k);
	ch.values.erase(ch.values.begin() + k);
	return true;
}

float defaultValue(PropTarget t)
{
	switch (t)
	{
	case PropTarget::ScaleX: case PropTarget::ScaleY: case PropTarget::ScaleZ:
	case PropTarget::MatColorR: case PropTarget::MatColorG: case PropTarget::MatColorB:
	case PropTarget::MatOpacity:
	case PropTarget::Visible:
		return 1.0f;
	case PropTarget::CameraFov:
		return 60.0f;   // CameraComponent's own default
	default:
		return 0.0f;
	}
}

int findTrack(const PropertyAnimClipAsset& clip, PropTarget target)
{
	for (int i = 0; i < static_cast<int>(clip.channels.size()); ++i)
		if (clip.channels[i].target == target) return i;
	return -1;
}

int addTrack(PropertyAnimClipAsset& clip, PropTarget target)
{
	if (const int i = findTrack(clip, target); i >= 0) return i;
	PropertyAnimChannel ch;
	ch.target = target;
	ch.times  = { 0.0f };
	ch.values = { defaultValue(target) };
	clip.channels.push_back(std::move(ch));
	if (clip.duration <= 0.0f) clip.duration = kDefaultDuration;
	return static_cast<int>(clip.channels.size()) - 1;
}

bool removeTrack(PropertyAnimClipAsset& clip, int track)
{
	if (track < 0 || track >= static_cast<int>(clip.channels.size())) return false;
	clip.channels.erase(clip.channels.begin() + track);
	return true;
}

float lastKeyTime(const PropertyAnimClipAsset& clip)
{
	float last = 0.0f;
	for (const PropertyAnimChannel& ch : clip.channels)
		for (size_t i = 0; i < std::min(ch.times.size(), ch.values.size()); ++i)
			last = std::max(last, ch.times[i]);
	return last;
}

float setDuration(PropertyAnimClipAsset& clip, float duration)
{
	clip.duration = std::max(duration, lastKeyTime(clip));
	return clip.duration;
}

bool advancePlayhead(View& view, float duration, float dt)
{
	if (!view.playing) return false;
	if (duration <= 0.0f) { view.playing = false; return false; }
	const float before = view.playhead;
	// Speed 1: the transport previews the clip as authored. A Property
	// Animator's own speed is that component's setting, not the clip's.
	PropertyAnimationSystem::advance(view.playhead, view.playing, 1.0f, view.loop, duration, dt);
	return view.playhead != before;
}

void valueRange(const PropertyAnimChannel& ch, float& lo, float& hi)
{
	const size_t n = std::min(ch.times.size(), ch.values.size());
	if (n == 0) { lo = -1.0f; hi = 1.0f; return; }
	lo = hi = ch.values[0];
	for (size_t i = 1; i < n; ++i)
	{
		lo = std::min(lo, ch.values[i]);
		hi = std::max(hi, ch.values[i]);
	}
	float span = hi - lo;
	if (span < 1e-4f)
	{
		// A flat channel gets a span of one around its value: a line in the
		// middle of the graph rather than one on its floor, and never a
		// division by nothing. Anything that moves at all fills the graph
		// with its own range, however small — that is what a curve view is for.
		const float mid = (lo + hi) * 0.5f;
		lo = mid - 0.5f; hi = mid + 0.5f;
		span = 1.0f;
	}
	lo -= span * 0.1f;
	hi += span * 0.1f;
}

} // namespace HE::Ed::Sequencer

#if __has_include(<imgui.h>)

#include "EditorHelp.h"                          // Help::Scope — "Sequencer/<label>"
#include "EditorWidgets.h"                       // helpForKey — the strip's tooltips
#include "UITimelineMath.h"                      // seconds ⇄ pixels, the 1-2-5 ruler

#include <imgui.h>

#include <string>

namespace HE::Ed::Sequencer
{

namespace
{
	// The palette. Warm near-blacks like the rest of the editor, the brand amber
	// on the keys, and one cool tint per group so a material track is told from
	// a transform track at a glance without reading the name.
	constexpr ImU32 kRulerBg     = IM_COL32( 28,  26,  24, 255);
	constexpr ImU32 kRulerTick   = IM_COL32( 90,  86,  80, 255);
	constexpr ImU32 kRulerHalf   = IM_COL32( 64,  61,  57, 255);
	constexpr ImU32 kRulerText   = IM_COL32(150, 145, 138, 255);
	constexpr ImU32 kRowBg       = IM_COL32( 34,  32,  30, 255);
	constexpr ImU32 kRowBgSel    = IM_COL32( 46,  42,  38, 255);
	constexpr ImU32 kRowLine     = IM_COL32( 24,  22,  20, 255);
	constexpr ImU32 kKey         = IM_COL32(230, 170,  60, 255);
	constexpr ImU32 kKeyHot      = IM_COL32(255, 214, 140, 255);
	constexpr ImU32 kKeyRing     = IM_COL32(255, 250, 240, 255);
	constexpr ImU32 kPlayhead    = IM_COL32(255, 236, 200, 230);
	constexpr ImU32 kGroupXform  = IM_COL32(120, 180, 255, 255);
	constexpr ImU32 kGroupMat    = IM_COL32(190, 130, 255, 255);
	constexpr ImU32 kValueText   = IM_COL32(150, 145, 138, 255);
	constexpr ImU32 kGraphBg     = IM_COL32( 30,  28,  26, 255);
	constexpr ImU32 kGraphGrid   = IM_COL32( 48,  45,  42, 255);
	constexpr ImU32 kGraphZero   = IM_COL32( 80,  76,  70, 255);
	constexpr ImU32 kGraphHold   = IM_COL32(110, 100,  85, 255);

	ImU32 groupColour(PropTarget t)
	{
		return targetGroup(t)[0] == 'T' ? kGroupXform : kGroupMat;
	}

	// Is the pointer inside this rectangle, and is this window the one under
	// it? Both, because a rectangle is not a hit test on its own: a popup or a
	// tooltip over the strip must not have its double-clicks fall through.
	bool pointerIn(const ImVec2& mp, float x0, float y0, float x1, float y1)
	{
		return ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
		       mp.x >= x0 && mp.x <= x1 && mp.y >= y0 && mp.y <= y1;
	}

	// The diamond, the ring around the picked one, and the button under both.
	// Shared by the dope sheet's rows and the curve view's points because the
	// gestures are the same: press selects and arms a drag, right-click opens
	// the key's menu.
	struct KeyHit { bool pressed = false; bool hovered = false; bool remove = false; };
	KeyHit keyButton(int k, float cx, float cy, bool dragged, const Metrics& M)
	{
		KeyHit hit;
		ImGui::PushID(k);
		ImGui::SetCursorScreenPos(ImVec2(cx - M.keyR, cy - M.keyR));
		ImGui::InvisibleButton("##seq_key", ImVec2(2.0f * M.keyR, 2.0f * M.keyR));
		EditorWidgets::helpForKey("sequencer.key");
		hit.hovered = ImGui::IsItemHovered() || dragged;
		hit.pressed = ImGui::IsItemActivated();
		// A right-click on a key is where its removal lives — the one thing
		// you want on a key and nowhere else to put it. The menu is the item's
		// own, so it opens over the diamond and not somewhere in the strip.
		// The scope is pushed here as well as in draw(): the help audit reads
		// the file top to bottom, and this helper stands above draw().
		if (ImGui::BeginPopupContextItem("##seq_keymenu"))
		{
			HE::Ed::Help::Scope helpScope("Sequencer");
			if (EditorWidgets::dangerMenuItem("Delete Key")) hit.remove = true;
			ImGui::EndPopup();
		}
		ImGui::PopID();
		return hit;
	}

	void drawDiamond(ImDrawList* dl, float cx, float cy, bool hot, bool picked, const Metrics& M)
	{
		const float r = M.keyR;
		const ImVec2 d[4] = { { cx, cy - r }, { cx + r, cy }, { cx, cy + r }, { cx - r, cy } };
		dl->AddConvexPolyFilled(d, 4, hot ? kKeyHot : kKey);
		// The selected key wears a ring rather than another fill: which key
		// you are looking at has to be readable next to which key the pointer
		// happens to be over, and two shades of amber are not.
		if (picked)
		{
			const float o = r + 3.0f;
			const ImVec2 ring[4] = { { cx, cy - o }, { cx + o, cy }, { cx, cy + o }, { cx - o, cy } };
			dl->AddPolyline(ring, 4, kKeyRing, ImDrawFlags_Closed, 1.5f);
		}
	}
}

Result draw(PropertyAnimClipAsset& clip, View& view,
            const ImVec2& size, const Intent& intent)
{
	Result out;
	const Metrics& M = metrics();
	// The strip's menu items — "Delete Key", "Remove Track" — explain
	// themselves as "Sequencer/<label>", whoever drew the window around it.
	HE::Ed::Help::Scope helpScope("Sequencer");

	// NoScrollWithMouse because the wheel belongs to the lane: it zooms the
	// time axis, and a strip that scrolled its rows instead the moment a
	// sixteenth track appeared would be zoom that works until you use it.
	ImGui::BeginChild("##seq_strip", size, ImGuiChildFlags_Borders,
	                  ImGuiWindowFlags_NoScrollWithMouse);

	const int trackCount = static_cast<int>(clip.channels.size());
	if (view.trackSel >= trackCount) { view.trackSel = -1; view.keySel = -1; }
	if (view.trackSel >= 0 &&
	    view.keySel >= static_cast<int>(clip.channels[view.trackSel].times.size()))
		view.keySel = -1;
	if (view.keySel < 0) { view.keyArmed = false; view.dragging = false; }

	// A clip of no length has no axis. The ruler would divide by it; say so
	// instead and draw nothing that pretends to be a timeline.
	const float duration = std::max(clip.duration, 1e-4f);
	view.playhead = std::clamp(view.playhead, 0.0f, duration);

	const ImVec2 area  = ImGui::GetContentRegionAvail();
	const float  laneW = std::max(60.0f, area.x - M.nameW - M.gap);
	const ImVec2 top   = ImGui::GetCursorScreenPos();
	ImDrawList*  dl    = ImGui::GetWindowDrawList();
	const float  laneL = top.x + M.nameW + M.gap, laneR = laneL + laneW;
	const float  stripBottom = top.y + area.y;
	out.laneX = laneL; out.laneW = laneW; out.top = top.y;

	// Seconds ⇄ pixels, the one conversion everything below shares — the
	// ruler, the diamonds, the playhead and every hit test.
	HE::Ed::UITimelineView tv{ laneL, laneW, duration, view.zoom, view.scroll };
	tv.clampScroll();   // the clip may have shrunk under a scrolled view

	// The wheel over the lane zooms at the pointer, Shift+wheel pans. Plain
	// wheel zooms rather than pans on purpose: at fit there is nothing to pan
	// to, so panning would be the gesture that does nothing most of the time.
	const ImGuiIO& io = ImGui::GetIO();
	const ImVec2 mp = io.MousePos;
	const bool overLane = pointerIn(mp, laneL, top.y, laneR, stripBottom);
	const float wheel = io.MouseWheel;
	if (overLane && wheel != 0.0f)
	{
		if (io.KeyShift)
		{ tv.scroll -= wheel * tv.visibleSpan() * 0.15f; tv.clampScroll(); }
		else tv.zoomAt(tv.zoom * std::pow(1.25f, wheel), mp.x);
	}
	// The buttons zoom around the PLAYHEAD — that is what somebody pressing +
	// is looking at — falling back to the middle of the view when it is off
	// screen, because anchoring on something invisible reads as a jump.
	if (intent.zoom != 0)
	{
		const bool onScreen = view.playhead >= tv.scroll &&
		                      view.playhead <= tv.scroll + tv.visibleSpan();
		tv.zoomAt(tv.zoom * (intent.zoom > 0 ? 1.5f : 1.0f / 1.5f),
		          onScreen ? tv.xOf(view.playhead) : laneL + laneW * 0.5f);
	}
	if (intent.fit) { tv.zoom = 1.0f; tv.scroll = 0.0f; }
	view.zoom = tv.zoom; view.scroll = tv.scroll;

	// ── The ruler ────────────────────────────────────────────────────────────
	// Labels stand on the 1-2-5 rung that keeps them ~64 px apart, so zooming
	// in turns seconds into milliseconds by itself, and a half-step tick
	// without a label gives the eye something to halve.
	dl->PushClipRect(ImVec2(laneL, top.y), ImVec2(laneR, stripBottom), true);
	dl->AddRectFilled(ImVec2(laneL, top.y), ImVec2(laneR, top.y + M.rulerH), kRulerBg);
	const float step  = HE::Ed::uiTimelineTickStep(tv.pixelsPerSecond());
	const float first = std::floor(tv.scroll / step) * step;
	const float last  = tv.scroll + tv.visibleSpan();
	// Counted, not accumulated: a step of a millisecond added a thousand times
	// is not the same number as a thousand milliseconds.
	for (int n = 0; n < 4096; ++n)
	{
		const float t = first + step * static_cast<float>(n);
		if (t > last + step) break;
		if (t < -0.0001f || t > duration + 0.0001f) continue;
		const float x = tv.xOf(t);
		dl->AddLine(ImVec2(x, top.y), ImVec2(x, top.y + M.rulerH), kRulerTick);
		if (t + step * 0.5f <= duration)
		{
			const float hx = tv.xOf(t + step * 0.5f);
			dl->AddLine(ImVec2(hx, top.y + M.rulerH * 0.6f), ImVec2(hx, top.y + M.rulerH), kRulerHalf);
		}
		char lbl[24];
		if (step >= 1.0f)        std::snprintf(lbl, sizeof(lbl), "%.0f s", t);
		else if (step >= 0.001f) std::snprintf(lbl, sizeof(lbl), "%.0f ms", t * 1000.0f);
		else                     std::snprintf(lbl, sizeof(lbl), "%.2f ms", t * 1000.0f);
		dl->AddText(ImVec2(x + 3.0f, top.y + 2.0f), kRulerText, lbl);
	}
	dl->PopClipRect();

	// The ruler strip takes clicks and drags: that is the scrub. The playhead
	// follows the pointer for as long as the button is held, even once the
	// pointer has left the ruler's own eighteen pixels — a scrub that stopped
	// the moment your hand drifted downward would be a scrub you cannot do.
	ImGui::SetCursorScreenPos(ImVec2(laneL, top.y));
	ImGui::InvisibleButton("##seq_ruler", ImVec2(laneW, M.rulerH));
	EditorWidgets::helpForKey("sequencer.ruler");
	if (ImGui::IsItemActivated()) view.scrubbing = true;
	if (view.scrubbing)
	{
		if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			const float t = tv.tOf(mp.x);
			if (t != view.playhead) { view.playhead = t; out.playheadMoved = true; }
		}
		else view.scrubbing = false;
	}

	const float rowsTop = top.y + M.rulerH + 2.0f;

	// ── The curve view's value axis ──────────────────────────────────────────
	// One graph for the selected track, from under the ruler to the strip's
	// bottom. Its range is the track's own — but FROZEN while a key is held:
	// an axis that grew as the value under the pointer grew would move the
	// pointer's own value out from under it, and the drag would run away.
	const bool curves = view.curves;
	const float graphTop = rowsTop + 4.0f;
	const float graphH   = std::max(40.0f, stripBottom - graphTop - 6.0f);
	ValueAxis axis{ graphTop, graphH, view.axisLo, view.axisHi };
	if (curves && view.trackSel >= 0 && !view.keyArmed)
	{
		valueRange(clip.channels[view.trackSel], axis.lo, axis.hi);
		view.axisLo = axis.lo; view.axisHi = axis.hi;
	}
	out.graphTop = graphTop; out.graphH = graphH;
	out.valueLo  = axis.lo;  out.valueHi = axis.hi;

	if (curves)
	{
		// The graph's floor and its grid, before any row draws over it. Value
		// gridlines stand on the same 1-2-5 ladder as the ruler, and the zero
		// line is a shade brighter — the one line a value is read against.
		dl->PushClipRect(ImVec2(laneL, rowsTop), ImVec2(laneR, stripBottom), true);
		dl->AddRectFilled(ImVec2(laneL, rowsTop), ImVec2(laneR, stripBottom), kGraphBg);
		if (view.trackSel >= 0)
		{
			const float pxPerUnit = graphH / std::max(axis.hi - axis.lo, 1e-6f);
			const float vstep = HE::Ed::uiTimelineTickStep(pxPerUnit, 36.0f);
			const float vfirst = std::floor(axis.lo / vstep) * vstep;
			for (int n = 0; n < 1024; ++n)
			{
				const float v = vfirst + vstep * static_cast<float>(n);
				if (v > axis.hi + vstep) break;
				if (v < axis.lo || v > axis.hi) continue;
				const float y = axis.yOf(v);
				dl->AddLine(ImVec2(laneL, y), ImVec2(laneR, y),
				            std::fabs(v) < vstep * 0.01f ? kGraphZero : kGraphGrid);
				char lbl[32];
				formatValue(clip.channels[view.trackSel].target, v, lbl, sizeof(lbl));
				dl->AddText(ImVec2(laneL + 4.0f, y - ImGui::GetTextLineHeight() - 1.0f), kRulerText, lbl);
			}
		}
		dl->PopClipRect();
	}

	// The Delete key removes the selected key. Delete only — Backspace is the
	// text-editing key, and a near-miss on it must not destroy a key (the same
	// rule GraphEditor and the UI Designer follow). Gated on the panel having
	// focus and nobody typing, so a Delete in a number field stays there.
	bool deleteSelected = false;
	if (view.trackSel >= 0 && view.keySel >= 0 && !io.WantTextInput &&
	    ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
	    ImGui::IsKeyPressed(ImGuiKey_Delete))
		deleteSelected = true;

	// ── One row per track ────────────────────────────────────────────────────
	// In the dope sheet each row carries its keys; in curve view the rows are
	// the track LIST on the left, and the selected one's keys are drawn on the
	// graph to the right instead.
	int removeTrack = -1, removeKeyTrack = -1, removeKeyIdx = -1;
	for (int i = 0; i < trackCount; ++i)
	{
		PropertyAnimChannel& ch = clip.channels[i];
		const float rowY = rowsTop + M.rowH * static_cast<float>(i);
		const bool  selected = view.trackSel == i;
		ImGui::PushID(i);

		// The name column: a group-coloured dot, the target, and its value at
		// the playhead — which is what makes scrubbing SHOW something before
		// the viewport preview exists: drag the ruler and the numbers move.
		dl->AddRectFilled(ImVec2(top.x, rowY), ImVec2(laneL - M.gap, rowY + M.rowH),
		                  selected ? kRowBgSel : kRowBg);
		dl->AddCircleFilled(ImVec2(top.x + 10.0f, rowY + M.rowH * 0.5f), 3.5f,
		                    groupColour(ch.target));
		ImGui::SetCursorScreenPos(ImVec2(top.x + 20.0f, rowY + 2.0f));
		// The label is data — targetName() — so this row explains itself by
		// key rather than by label (the audit cannot see a name built at run
		// time, and neither could a label lookup).
		const std::string label = std::string(targetName(ch.target)) + "##seq_track";
		if (ImGui::Selectable(label.c_str(), selected, 0,
		                      ImVec2(M.nameW - 20.0f, M.rowH - 4.0f)))
		{
			if (!selected) out.selectionChanged = true;
			view.trackSel = i;
			view.keySel   = -1;
		}
		EditorWidgets::helpForKey("sequencer.track");
		// A right-click on the name is where the track's removal lives, next
		// to the name that says which one goes.
		if (ImGui::BeginPopupContextItem("##seq_trackmenu"))
		{
			if (EditorWidgets::dangerMenuItem("Remove Track")) removeTrack = i;
			ImGui::EndPopup();
		}

		char val[32];
		formatValue(ch.target, PropertyAnimationSystem::sampleChannel(ch, view.playhead),
		            val, sizeof(val));
		const ImVec2 valSize = ImGui::CalcTextSize(val);
		dl->AddText(ImVec2(laneL - M.gap - 6.0f - valSize.x, rowY + (M.rowH - valSize.y) * 0.5f),
		            kValueText, val);

		if (curves) { ImGui::PopID(); continue; }

		// The lane, and a diamond per key. Clicking one selects it and goes
		// TO it: the playhead lands on the key's time, so the value beside
		// the name is the key's own.
		dl->PushClipRect(ImVec2(laneL, top.y), ImVec2(laneR, stripBottom), true);
		dl->AddRectFilled(ImVec2(laneL, rowY), ImVec2(laneR, rowY + M.rowH),
		                  selected ? kRowBgSel : kRowBg);
		dl->AddLine(ImVec2(laneL, rowY + M.rowH), ImVec2(laneR, rowY + M.rowH), kRowLine);
		dl->PopClipRect();

		const float cy = rowY + M.rowH * 0.5f;
		const int keyCount = static_cast<int>(std::min(ch.times.size(), ch.values.size()));
		bool anyKeyHovered = false;
		for (int k = 0; k < keyCount; ++k)
		{
			const float cx = tv.xOf(ch.times[k]);
			// A key scrolled out of the lane gets no button at all. Clipping is
			// a drawing matter; an invisible button at x = -400 would still sit
			// on top of the track NAME and eat its clicks.
			if (cx < laneL - M.keyR || cx > laneR + M.keyR) continue;
			const bool picked  = selected && view.keySel == k;
			const bool dragged = picked && view.dragging;
			const KeyHit hit = keyButton(k, cx, cy, dragged, M);
			anyKeyHovered = anyKeyHovered || hit.hovered;
			if (hit.pressed)
			{
				if (!picked) out.selectionChanged = true;
				view.trackSel = i;
				view.keySel   = k;
				if (view.playhead != ch.times[k]) out.playheadMoved = true;
				view.playhead  = std::clamp(ch.times[k], 0.0f, duration);
				view.scrubbing = false;
				// Armed, not yet dragging: the press alone moves nothing (a
				// click is how you look at a key), the pointer travelling
				// afterwards is what moves it.
				view.keyArmed = true;
				view.dragging = false;
			}
			if (hit.remove) { removeKeyTrack = i; removeKeyIdx = k; }
			// No in-place tooltip with the key's numbers, deliberately: the help
			// is drawn late (EditorWidgets.h, "why the tooltip is drawn LATE"),
			// and a second one drawn here would stack on it or swap places with
			// it the moment the pointer paused. The numbers are in the readout
			// under the strip, one click away.
			dl->PushClipRect(ImVec2(laneL, top.y), ImVec2(laneR, stripBottom), true);
			drawDiamond(dl, cx, cy, hit.hovered, picked, M);
			dl->PopClipRect();
		}

		// A double-click on the lane where no key is: a key there, holding
		// what the track already is at that moment — so adding a key never
		// changes the curve, it only pins it. The key is then selected and
		// the playhead is on it, the way a clicked key would be.
		if (!anyKeyHovered && !view.keyArmed &&
		    pointerIn(mp, laneL, rowY, laneR, rowY + M.rowH) &&
		    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		{
			const float t = tv.tOf(mp.x);
			const float v = keyCount > 0 ? PropertyAnimationSystem::sampleChannel(ch, t)
			                             : defaultValue(ch.target);
			view.trackSel = i;
			view.keySel   = insertKey(ch, t, v);
			view.playhead = std::clamp(ch.times[view.keySel], 0.0f, duration);
			out.edited = out.committed = out.selectionChanged = out.playheadMoved = true;
		}
		ImGui::PopID();
	}

	const float rowsBottom = rowsTop + M.rowH * static_cast<float>(trackCount);

	// ── The curve, in curve view ─────────────────────────────────────────────
	if (curves && view.trackSel >= 0)
	{
		PropertyAnimChannel& ch = clip.channels[view.trackSel];
		const int keyCount = static_cast<int>(std::min(ch.times.size(), ch.values.size()));
		const ImU32 col = groupColour(ch.target);
		dl->PushClipRect(ImVec2(laneL, rowsTop), ImVec2(laneR, stripBottom), true);
		if (keyCount > 0)
		{
			// Held flat before the first key and after the last, dimmer, and
			// straight between keys: the runtime interpolates linearly
			// (sampleChannel), and a curve drawn any smoother would be a
			// promise the Property Animator does not keep.
			const float y0 = axis.yOf(ch.values[0]), yN = axis.yOf(ch.values[keyCount - 1]);
			dl->AddLine(ImVec2(laneL, y0), ImVec2(tv.xOf(ch.times[0]), y0), kGraphHold, 1.5f);
			dl->AddLine(ImVec2(tv.xOf(ch.times[keyCount - 1]), yN), ImVec2(laneR, yN), kGraphHold, 1.5f);
			for (int k = 0; k + 1 < keyCount; ++k)
				dl->AddLine(ImVec2(tv.xOf(ch.times[k]), axis.yOf(ch.values[k])),
				            ImVec2(tv.xOf(ch.times[k + 1]), axis.yOf(ch.values[k + 1])), col, 2.0f);
			// Where the playhead crosses the curve: the value the readout on
			// the left is printing, as a point on the line it comes from.
			const float pv = PropertyAnimationSystem::sampleChannel(ch, view.playhead);
			dl->AddCircle(ImVec2(tv.xOf(view.playhead), axis.yOf(pv)), 4.0f, kPlayhead, 0, 1.5f);
		}
		dl->PopClipRect();

		bool anyKeyHovered = false;
		for (int k = 0; k < keyCount; ++k)
		{
			const float cx = tv.xOf(ch.times[k]), cy = axis.yOf(ch.values[k]);
			if (cx < laneL - M.keyR || cx > laneR + M.keyR) continue;
			const bool picked  = view.keySel == k;
			const bool dragged = picked && view.dragging;
			const KeyHit hit = keyButton(k, cx, cy, dragged, M);
			anyKeyHovered = anyKeyHovered || hit.hovered;
			if (hit.pressed)
			{
				if (!picked) out.selectionChanged = true;
				view.keySel = k;
				if (view.playhead != ch.times[k]) out.playheadMoved = true;
				view.playhead  = std::clamp(ch.times[k], 0.0f, duration);
				view.scrubbing = false;
				view.keyArmed  = true;
				view.dragging  = false;
			}
			if (hit.remove) { removeKeyTrack = view.trackSel; removeKeyIdx = k; }
			dl->PushClipRect(ImVec2(laneL, rowsTop), ImVec2(laneR, stripBottom), true);
			dl->AddCircleFilled(ImVec2(cx, cy), M.keyR, hit.hovered ? kKeyHot : kKey);
			if (picked) dl->AddCircle(ImVec2(cx, cy), M.keyR + 3.0f, kKeyRing, 0, 1.5f);
			dl->PopClipRect();
		}

		// A double-click on the graph: a key at the pointer's time AND value.
		// Here the pointer is a place on the graph, so the key goes where it
		// points rather than onto the curve — which is one drag away anyway.
		if (!anyKeyHovered && !view.keyArmed &&
		    pointerIn(mp, laneL, rowsTop, laneR, stripBottom) &&
		    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		{
			view.keySel   = insertKey(ch, tv.tOf(mp.x), axis.vOf(mp.y));
			view.playhead = std::clamp(ch.times[view.keySel], 0.0f, duration);
			out.edited = out.committed = out.selectionChanged = out.playheadMoved = true;
		}
	}
	else if (curves && trackCount > 0)
	{
		// Only with tracks to pick from: an empty clip prints its own hint
		// below, across the whole strip, and two hints on one row overlap.
		ImGui::SetCursorScreenPos(ImVec2(laneL + 8.0f, rowsTop + 6.0f));
		ImGui::TextDisabled("Select a track on the left to see its curve.");
	}

	// ── The drag of the armed key ────────────────────────────────────────────
	// Held in the view rather than on the key's button, because moving a key
	// past its neighbour changes its index — and with it the button's ID, which
	// would end ImGui's own notion of the drag mid-gesture. The pointer is the
	// key: its time follows the x, and in curve view its value follows the y.
	if (view.keyArmed && view.trackSel >= 0 && view.keySel >= 0)
	{
		PropertyAnimChannel& ch = clip.channels[view.trackSel];
		if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			if (view.dragging || ImGui::IsMouseDragging(ImGuiMouseButton_Left))
			{
				view.dragging = true;
				const float t  = tv.tOf(mp.x);
				const int   nk = moveKey(ch, view.keySel, t);
				if (nk >= 0)
				{
					view.keySel = nk;
					if (curves) ch.values[nk] = axis.vOf(mp.y);
					if (view.playhead != ch.times[nk]) out.playheadMoved = true;
					view.playhead = std::clamp(ch.times[nk], 0.0f, duration);
					out.edited = true;
				}
			}
		}
		else
		{
			// Released. One undo point for the whole drag — and none at all
			// for a press that never moved.
			if (view.dragging) out.committed = true;
			view.keyArmed = false;
			view.dragging = false;
		}
	}

	// ── Removals, after the loops that would have been walking the vectors ──
	if (deleteSelected && removeKeyIdx < 0) { removeKeyTrack = view.trackSel; removeKeyIdx = view.keySel; }
	if (removeKeyTrack >= 0 && removeKey(clip.channels[removeKeyTrack], removeKeyIdx))
	{
		if (view.trackSel == removeKeyTrack) { view.keySel = -1; view.keyArmed = false; view.dragging = false; }
		out.edited = out.committed = out.selectionChanged = true;
	}
	if (removeTrack >= 0 && Sequencer::removeTrack(clip, removeTrack))
	{
		if (view.trackSel == removeTrack) { view.trackSel = -1; view.keySel = -1; }
		else if (view.trackSel > removeTrack) --view.trackSel;
		view.keyArmed = false; view.dragging = false;
		out.edited = out.committed = out.selectionChanged = true;
	}

	if (trackCount == 0)
	{
		ImGui::SetCursorScreenPos(ImVec2(top.x + 8.0f, rowsTop + 6.0f));
		ImGui::TextDisabled("No tracks. A track is one property of the entity this clip");
		ImGui::SetCursorScreenPos(ImVec2(top.x + 8.0f, rowsTop + 6.0f + ImGui::GetTextLineHeight()));
		ImGui::TextDisabled("plays on, and a key is what that property is at a moment.");
		ImGui::SetCursorScreenPos(ImVec2(top.x + 8.0f, rowsTop + 6.0f + 2.0f * ImGui::GetTextLineHeight()));
		ImGui::TextDisabled("Add Track above puts the first one here.");
	}

	// ── The playhead ─────────────────────────────────────────────────────────
	// Over everything, ruler to last row — or the graph's bottom, in curve
	// view — so a zoomed-in view still says where it is without reading the ticks.
	{
		const float x = tv.xOf(view.playhead);
		const float bottom = curves ? stripBottom
		                            : std::max(rowsBottom, top.y + M.rulerH + 24.0f);
		dl->PushClipRect(ImVec2(laneL, top.y), ImVec2(laneR, stripBottom), true);
		dl->AddLine(ImVec2(x, top.y), ImVec2(x, bottom), kPlayhead, 1.5f);
		const ImVec2 tri[3] = { { x - 5.0f, top.y }, { x + 5.0f, top.y }, { x, top.y + 6.0f } };
		dl->AddConvexPolyFilled(tri, 3, kPlayhead);
		dl->PopClipRect();
	}

	// Reserve what was drawn so the child scrolls when there are more rows
	// than height — the dummy is what tells ImGui how tall the content is.
	ImGui::SetCursorScreenPos(ImVec2(top.x, std::max(rowsBottom, rowsTop + 2.0f * M.rowH)));
	ImGui::Dummy(ImVec2(1.0f, 1.0f));

	ImGui::EndChild();
	return out;
}

} // namespace HE::Ed::Sequencer

#endif // __has_include(<imgui.h>)
