#include "SequencerTimeline.h"

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

} // namespace HE::Ed::Sequencer

#if __has_include(<imgui.h>)

#include "EditorWidgets.h"                       // helpForKey — the strip's tooltips
#include "UITimelineMath.h"                      // seconds ⇄ pixels, the 1-2-5 ruler
#include <HorizonScene/PropertyAnimationSystem.h> // sampleChannel — the runtime's rule

#include <imgui.h>

#include <algorithm>
#include <cmath>
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

	ImU32 groupColour(PropTarget t)
	{
		return targetGroup(t)[0] == 'T' ? kGroupXform : kGroupMat;
	}
}

Result draw(const PropertyAnimClipAsset& clip, View& view,
            const ImVec2& size, const Intent& intent)
{
	Result out;
	const Metrics& M = metrics();

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
	const ImVec2 mp = ImGui::GetIO().MousePos;
	const bool overLane = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
	                      mp.x >= laneL && mp.x <= laneR &&
	                      mp.y >= top.y && mp.y <= stripBottom;
	const float wheel = ImGui::GetIO().MouseWheel;
	if (overLane && wheel != 0.0f)
	{
		if (ImGui::GetIO().KeyShift)
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

	// ── One row per track, and the keys on it ────────────────────────────────
	for (int i = 0; i < trackCount; ++i)
	{
		const PropertyAnimChannel& ch = clip.channels[i];
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

		char val[32];
		formatValue(ch.target, PropertyAnimationSystem::sampleChannel(ch, view.playhead),
		            val, sizeof(val));
		const ImVec2 valSize = ImGui::CalcTextSize(val);
		dl->AddText(ImVec2(laneL - M.gap - 6.0f - valSize.x, rowY + (M.rowH - valSize.y) * 0.5f),
		            kValueText, val);

		// The lane, and a diamond per key. Clicking one selects it and goes
		// TO it: the playhead lands on the key's time, so the value beside
		// the name is the key's own.
		dl->PushClipRect(ImVec2(laneL, top.y), ImVec2(laneR, stripBottom), true);
		dl->AddRectFilled(ImVec2(laneL, rowY), ImVec2(laneR, rowY + M.rowH),
		                  selected ? kRowBgSel : kRowBg);
		dl->AddLine(ImVec2(laneL, rowY + M.rowH), ImVec2(laneR, rowY + M.rowH), kRowLine);
		dl->PopClipRect();

		const int keyCount = static_cast<int>(std::min(ch.times.size(), ch.values.size()));
		for (int k = 0; k < keyCount; ++k)
		{
			const float cx = tv.xOf(ch.times[k]), cy = rowY + M.rowH * 0.5f;
			// A key scrolled out of the lane gets no button at all. Clipping is
			// a drawing matter; an invisible button at x = -400 would still sit
			// on top of the track NAME and eat its clicks.
			if (cx < laneL - 6.0f || cx > laneR + 6.0f) continue;
			ImGui::PushID(k);
			ImGui::SetCursorScreenPos(ImVec2(cx - 6.0f, cy - 6.0f));
			ImGui::InvisibleButton("##seq_key", ImVec2(12.0f, 12.0f));
			EditorWidgets::helpForKey("sequencer.key");
			const bool hot    = ImGui::IsItemHovered() || ImGui::IsItemActive();
			const bool picked = selected && view.keySel == k;
			if (ImGui::IsItemActivated())
			{
				if (!picked) out.selectionChanged = true;
				view.trackSel = i;
				view.keySel   = k;
				if (view.playhead != ch.times[k]) out.playheadMoved = true;
				view.playhead = std::clamp(ch.times[k], 0.0f, duration);
				view.scrubbing = false;
			}
			// No in-place tooltip with the key's numbers, deliberately: the help
			// above is drawn late (EditorWidgets.h, "why the tooltip is drawn
			// LATE"), and a second one drawn here would stack on it or swap
			// places with it the moment the pointer paused. The numbers are in
			// the readout under the strip, one click away.
			const ImU32 col = hot ? kKeyHot : kKey;
			const ImVec2 d[4] = { { cx, cy - 6.0f }, { cx + 6.0f, cy }, { cx, cy + 6.0f }, { cx - 6.0f, cy } };
			dl->PushClipRect(ImVec2(laneL, top.y), ImVec2(laneR, stripBottom), true);
			dl->AddConvexPolyFilled(d, 4, col);
			// The selected key wears a ring rather than another fill: which key
			// you are looking at has to be readable next to which key the
			// pointer happens to be over, and two shades of amber are not.
			if (picked)
			{
				const ImVec2 o[4] = { { cx, cy - 9.0f }, { cx + 9.0f, cy }, { cx, cy + 9.0f }, { cx - 9.0f, cy } };
				dl->AddPolyline(o, 4, kKeyRing, ImDrawFlags_Closed, 1.5f);
			}
			dl->PopClipRect();
			ImGui::PopID();
		}
		ImGui::PopID();
	}

	const float rowsBottom = rowsTop + M.rowH * static_cast<float>(trackCount);

	if (trackCount == 0)
	{
		ImGui::SetCursorScreenPos(ImVec2(top.x + 8.0f, rowsTop + 6.0f));
		ImGui::TextDisabled("No tracks. A track is one property of the entity this clip");
		ImGui::SetCursorScreenPos(ImVec2(top.x + 8.0f, rowsTop + 6.0f + ImGui::GetTextLineHeight()));
		ImGui::TextDisabled("plays on, and a key is what that property is at a moment.");
	}

	// ── The playhead ─────────────────────────────────────────────────────────
	// Over everything, ruler to last row, with its time in the ruler so a
	// zoomed-in view still says where it is without reading the ticks.
	{
		const float x = tv.xOf(view.playhead);
		dl->PushClipRect(ImVec2(laneL, top.y), ImVec2(laneR, stripBottom), true);
		dl->AddLine(ImVec2(x, top.y), ImVec2(x, std::max(rowsBottom, top.y + M.rulerH + 24.0f)),
		            kPlayhead, 1.5f);
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
