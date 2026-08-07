#include "EngineContentSyncBar.h"
#include "EditorApplication.h"   // AppContext

#ifdef HE_HAVE_LIBSSH2
#include <ContentSync/EngineContentSync.h>
#endif

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

#include <algorithm>
#include <filesystem>

namespace EngineContentSyncBar
{

float FooterWidth(AppContext& ctx)
{
#if defined(HE_HAVE_LIBSSH2) && defined(HE_IMGUI_ENABLED)
	(void)ctx;
	const HE::Cs::DownloadQueueStatus st = HE::Cs::EngineContentSync::instance().status();
	if (!st.active) return 0.0f;

	const std::string label = std::filesystem::path(st.currentRelativePath).filename().string();
	constexpr float kBarW = 90.0f;
	const float textW = ImGui::CalcTextSize(label.c_str()).x;
	return kBarW + 8.0f + textW + 40.0f /* "(n/m)" */ + 40.0f /* leading glyph + gaps */;
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

	const std::string label = std::filesystem::path(st.currentRelativePath).filename().string();
	ImGui::TextDisabled("Downloading");
	ImGui::SameLine();
	ImGui::TextUnformatted(label.c_str());
	ImGui::SameLine();

	const float frac = st.totalInBatch > 0
		? std::clamp(static_cast<float>(st.completedInBatch) / static_cast<float>(st.totalInBatch), 0.0f, 1.0f)
		: 0.0f;
	ImGui::SetNextItemWidth(90.0f);
	char overlay[32];
	std::snprintf(overlay, sizeof(overlay), "%zu/%zu",
	              st.completedInBatch + 1 /* the one in flight */, std::max<std::size_t>(st.totalInBatch, 1));
	ImGui::ProgressBar(frac, ImVec2(90.0f, 0.0f), overlay);

	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
		ImGui::SetTooltip("Fetching EngineContent assets from the SFTP server");
#else
	(void)ctx;
#endif
}

} // namespace EngineContentSyncBar
