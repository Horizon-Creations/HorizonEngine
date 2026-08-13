#include "EngineContentSyncBar.h"
#include "EditorApplication.h"   // AppContext

#ifdef HE_HAVE_LIBSSH2
#include <ContentSync/EngineContentSync.h>
#endif

#ifdef HE_IMGUI_ENABLED
#include "EditorWidgets.h"   // WrapText
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

namespace EngineContentSyncBar
{

#if defined(HE_HAVE_LIBSSH2) && defined(HE_IMGUI_ENABLED)
namespace
{
	// The footer window is 24px tall with 4px vertical padding — 16px of usable
	// content height. A default-height ImGui::ProgressBar is GetFrameHeight()
	// (text height + 2*FramePadding.y ≈ 21px), which does not fit: it overflowed
	// the bar, got its bottom edge clipped, gave the footer a few pixels of
	// hidden scroll, and — because ProgressBar is a framed item — pushed the
	// text baseline of EVERYTHING drawn after it down by FramePadding.y, so the
	// centred "Ready" label and the FPS counter visibly jumped whenever a
	// download started. An explicit, deliberately short bar avoids all of that.
	constexpr float kBarW = 90.0f;
	constexpr float kBarH = 10.0f;
	constexpr float kGap  = 6.0f;

	// Longest label we let through before eliding, so the reserved width stays
	// stable while a name scrolls past.
	constexpr int kMaxLabelChars = 28;

	std::string currentLabel(const HE::Cs::DownloadQueueStatus& st)
	{
		std::string name = std::filesystem::path(st.currentRelativePath).filename().string();
		if (static_cast<int>(name.size()) > kMaxLabelChars)
			name = name.substr(0, kMaxLabelChars - 1) + "\xE2\x80\xA6"; // U+2026 HORIZONTAL ELLIPSIS
		return name;
	}

	// "3/7" — files finished (not the in-flight index) over the batch total, so
	// it always agrees with how full the bar is.
	std::string countText(const HE::Cs::DownloadQueueStatus& st)
	{
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%zu/%zu",
		              st.completedInBatch, st.totalInBatch);
		return buf;
	}
}
#endif

float FooterWidth(AppContext& ctx)
{
#if defined(HE_HAVE_LIBSSH2) && defined(HE_IMGUI_ENABLED)
	(void)ctx;
	const HE::Cs::DownloadQueueStatus st = HE::Cs::EngineContentSync::instance().status();
	if (!st.active) return 0.0f;

	// Must match DrawFooter's item sequence exactly: label, gap, bar, gap, count.
	// (It used to be a made-up formula with constants for items that were never
	// drawn, which is only harmless while nobody right-aligns against it.)
	return ImGui::CalcTextSize(currentLabel(st).c_str()).x + kGap
	     + kBarW + kGap
	     + ImGui::CalcTextSize(countText(st).c_str()).x;
#else
	(void)ctx;
	return 0.0f;
#endif
}

void DrawFooter(AppContext& ctx)
{
#if defined(HE_HAVE_LIBSSH2) && defined(HE_IMGUI_ENABLED)
	(void)ctx;
	const HE::Cs::DownloadQueueStatus st = HE::Cs::EngineContentSync::instance().status();
	if (!st.active) return;

	const std::string label = currentLabel(st);
	const std::string count = countText(st);

	// ── Why the bar is drawn by hand instead of via ImGui::ProgressBar ──────
	// All three items must sit on one line. That is only guaranteed when they
	// are the same HEIGHT as a layout item: ImGui lays a line out from the item
	// heights, and moving the cursor around a shorter/taller item to "centre"
	// it perturbs the line box that every following item on that line inherits
	// — which is what kept dragging the "n/m" count out of line with the label.
	//
	// So: reserve a slot of exactly one text line for the bar (Dummy), and paint
	// the deliberately shorter bar centred INSIDE that slot with the draw list.
	// Layout-wise the bar is then indistinguishable from a piece of text, no
	// cursor is touched, and the alignment cannot depend on ImGui's line-box
	// bookkeeping at all.
	ImGui::TextUnformatted(label.c_str());
	ImGui::SameLine(0.0f, kGap);

	const float  lineH   = ImGui::GetTextLineHeight();
	const ImVec2 slotPos = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(kBarW, lineH));

	const float  top = slotPos.y + (lineH - kBarH) * 0.5f;
	const ImVec2 p0(slotPos.x, top);
	const ImVec2 p1(slotPos.x + kBarW, top + kBarH);
	const float  radius = kBarH * 0.5f;

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg), radius);

	// A download whose size the server never reported has no meaningful
	// fraction; a bar frozen at 0% would read as "stuck". Slide a short segment
	// instead — the honest "working, length unknown".
	const bool known = st.currentBytesTotal > 0 || st.completedInBatch > 0;
	const ImU32 fillCol = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
	if (known)
	{
		const float frac = st.totalInBatch > 0
			? static_cast<float>(std::clamp(st.progressFiles() / static_cast<double>(st.totalInBatch), 0.0, 1.0))
			: 0.0f;
		const float w = kBarW * frac;
		if (w > 0.0f)
			dl->AddRectFilled(p0, ImVec2(p0.x + w, p1.y), fillCol, std::min(radius, w * 0.5f));
	}
	else
	{
		constexpr float kSegW   = 0.3f;   // fraction of the bar
		constexpr float kPeriod = 1.6f;   // seconds for one sweep
		const float t     = static_cast<float>(std::fmod(ImGui::GetTime(), kPeriod)) / kPeriod;
		const float x0    = p0.x + (kBarW * (1.0f + kSegW)) * t - kBarW * kSegW;
		const float clipL = std::max(x0, p0.x);
		const float clipR = std::min(x0 + kBarW * kSegW, p1.x);
		if (clipR > clipL)
			dl->AddRectFilled(ImVec2(clipL, p0.y), ImVec2(clipR, p1.y), fillCol,
			                   std::min(radius, (clipR - clipL) * 0.5f));
	}

	ImGui::SameLine(0.0f, kGap);
	ImGui::TextDisabled("%s", count.c_str());

	// Hover-tested against the painted rectangles rather than the last item, so
	// the tooltip covers the whole cluster (label, bar and count) and does not
	// depend on Dummy() carrying a hoverable id.
	//
	// IsWindowHovered() is the second half of that test and not decoration.
	// IsMouseHoveringRect answers "is the pointer over this rectangle", full stop:
	// it clips to the window's clip rect but never asks whether this window is the
	// one under the pointer. The footer is drawn with NoBringToFrontOnFocus, so a
	// floating panel is free to lie across it — and then hovering an item on THAT
	// panel raises its tooltip while these coordinates still say "hovered" and this
	// one is submitted too. Two tooltips in a frame do not replace each other: only
	// SetTooltip passes ImGuiTooltipFlags_OverridePrevious (imgui_internal.h, which
	// this file has no other reason to pull in), a plain BeginTooltip APPENDS into
	// the tooltip already open. The download path would show up glued to the bottom
	// of somebody else's hint. Asking whether this window is the hovered one is the
	// same exclusivity an IsItemHovered gate gives every other tooltip here, without
	// the internal header: exactly one window is hovered, and none is while a popup
	// blocks it.
	const ImVec2 clusterMax = ImGui::GetItemRectMax();
	if (ImGui::IsWindowHovered() &&
	    ImGui::IsMouseHoveringRect(ImVec2(slotPos.x - kGap, slotPos.y), clusterMax))
	{
		// Spelled out instead of SetTooltip (which is exactly this, minus the wrap)
		// because the second line is a content-relative path, and a tooltip has no
		// width of its own: it grows to whatever it is given. An unwrapped deep
		// path therefore paints a single strip of text across the entire screen,
		// which is the "cheap" look a sideways scrollbar has — the same failure
		// without the scrollbar. The wrap column is given explicitly rather than
		// as 0.0f: a tooltip auto-sizes to its contents, so wrapping "at the
		// window edge" would be wrapping at the width the last frame happened to
		// produce, and the box would breathe in and out while it is hovered.
		if (ImGui::BeginTooltip())   // EndTooltip only on true — same contract SetTooltip honours
		{
			{
				EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 35.0f);
				ImGui::Text("Downloading EngineContent from the server\n%s",
				            st.currentRelativePath.c_str());
			}
			ImGui::EndTooltip();
		}
	}
#else
	(void)ctx;
#endif
}

} // namespace EngineContentSyncBar
