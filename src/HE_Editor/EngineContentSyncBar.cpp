#include "EngineContentSyncBar.h"
#include "EditorApplication.h"   // AppContext

#ifdef HE_HAVE_LIBSSH2
#include <ContentSync/EngineContentSync.h>
#endif

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

#include <algorithm>
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

	// One shared line box for all three items. Every item's Y is set explicitly
	// from this baseline rather than nudged relatively, because SameLine() does
	// NOT continue from wherever the cursor happens to be: it snaps Y back to
	// the START OF THE PREVIOUS ITEM'S LINE. So offsetting the cursor for the
	// bar and then "undoing" it afterwards has no effect at all — the following
	// SameLine re-applies the bar's own (lowered) line Y, which is what dragged
	// the "n/m" count down out of alignment with the label next to it.
	const float baseY = ImGui::GetCursorPosY();
	// The bar is deliberately shorter than a text line (see kBarH), so it needs
	// centring on that line; the text items already sit on it.
	const float barY  = baseY + (ImGui::GetTextLineHeight() - kBarH) * 0.5f;

	ImGui::TextUnformatted(label.c_str());

	ImGui::SameLine(0.0f, kGap);
	ImGui::SetCursorPosY(barY);
	// A download whose size the server never reported has no meaningful
	// fraction. ImGui renders a negative fraction as an indeterminate marquee,
	// which is the honest answer — a bar frozen at 0% reads as "stuck".
	const bool  known = st.currentBytesTotal > 0 || st.completedInBatch > 0;
	const float frac  = st.totalInBatch > 0
		? static_cast<float>(std::clamp(st.progressFiles() / static_cast<double>(st.totalInBatch), 0.0, 1.0))
		: 0.0f;
	ImGui::ProgressBar(known ? frac : -1.0f * static_cast<float>(ImGui::GetTime()),
	                   ImVec2(kBarW, kBarH), "");
	const bool barHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);

	ImGui::SameLine(0.0f, kGap);
	ImGui::SetCursorPosY(baseY);   // back onto the label's line, not the bar's
	ImGui::TextDisabled("%s", count.c_str());

	if (barHovered || ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
		ImGui::SetTooltip("Downloading EngineContent from the server\n%s", st.currentRelativePath.c_str());
#else
	(void)ctx;
#endif
}

} // namespace EngineContentSyncBar
