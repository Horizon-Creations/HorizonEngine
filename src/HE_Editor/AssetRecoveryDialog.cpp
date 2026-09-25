#include "AssetRecoveryDialog.h"
#include "EditorApplication.h"   // AppContext
#include "EditorHelp.h"          // the "Asset Recovery" scope for the buttons
#include "EditorWidgets.h"       // pinDialogToEditorWindow, the button verbs, WrapText

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace AssetRecoveryDialog
{

#ifdef HE_IMGUI_ENABLED
namespace
{
	constexpr const char* kTitle = "##AssetRecovery";

	// Why the last Restore did not happen ("" = it did, or none was tried).
	std::string s_error;

	// The same "15 Sep 2026, 04:31" SceneRecoveryDialog shows.
	std::string savedAtText(std::int64_t unixSecs)
	{
		if (unixSecs <= 0) return {};
		const std::time_t secs = static_cast<std::time_t>(unixSecs);
		std::tm tmBuf{};
#ifdef _WIN32
		if (localtime_s(&tmBuf, &secs) != 0) return {};
#else
		if (!localtime_r(&secs, &tmBuf)) return {};
#endif
		char buf[64];
		if (std::strftime(buf, sizeof(buf), "%d %b %Y, %H:%M", &tmBuf) == 0) return {};
		return buf;
	}

	const ImVec4 kWarn(1.0f, 0.75f, 0.3f, 1.0f);
}
#endif

void Draw(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	if (!ctx.assetRecoveryOffers || ctx.assetRecoveryOffers->empty())
	{
		// Every row answered from inside the dialog: close it on the frame after.
		if (ImGui::IsPopupOpen(kTitle) && ImGui::BeginPopupModal(kTitle))
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
		s_error.clear();
		return;
	}

	// The scene's offer goes first, and never over another root-level modal —
	// OpenPopup at root level replaces whatever is open (SceneRecoveryDialog
	// spells this out). The offers are still there next frame.
	if (!ctx.recoveryOffer && !ImGui::IsPopupOpen(kTitle) &&
	    !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
		ImGui::OpenPopup(kTitle);

	constexpr float kDialogWidth = 620.0f;
	ImGui::SetNextWindowSize(ImVec2(kDialogWidth, 0.0f), ImGuiCond_Appearing);
	EditorWidgets::pinDialogToEditorWindow();
	if (!ImGui::BeginPopupModal(kTitle, nullptr,
	                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
	                            ImGuiWindowFlags_NoSavedSettings))
		return;

	HE::Ed::Help::Scope helpScope("Asset Recovery");
	// A copy: the buttons remove entries from the vector it points at.
	const std::vector<HE::Ed::AssetRecoveryEntry> offers = *ctx.assetRecoveryOffers;
	const float wrapWidth = kDialogWidth - ImGui::GetStyle().WindowPadding.x;

	{
		EditorWidgets::WrapText wrap(wrapWidth);
		ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
		ImGui::TextUnformatted("Unsaved Asset Edits Found");
		ImGui::PopStyleColor();
		ImGui::Separator();
		ImGui::Spacing();
		if (offers.size() == 1)
			ImGui::TextWrapped(
				"The last session did not end cleanly and left an autosaved copy of one "
				"asset with unsaved edits. The file itself was not changed.");
		else
			ImGui::TextWrapped(
				"The last session did not end cleanly and left autosaved copies of %d "
				"assets with unsaved edits. The files themselves were not changed.",
				static_cast<int>(offers.size()));
		ImGui::Spacing();
	}

	// The rows. Answers are collected and run after the loop, so the list being
	// drawn is never the one being changed.
	enum class Act { None, Restore, Discard };
	Act         act = Act::None;
	std::string actKey;
	const float listH = std::min(260.0f, 58.0f * static_cast<float>(offers.size()) + 8.0f);
	ImGui::BeginChild("##asset_recovery_rows", ImVec2(0.0f, listH), true);
	for (const HE::Ed::AssetRecoveryEntry& e : offers)
	{
		ImGui::PushID(e.key.c_str());
		const std::string name = std::filesystem::path(e.relativePath).filename().string();
		ImGui::TextUnformatted(name.c_str());
		ImGui::SameLine();
		ImGui::TextDisabled("%s", e.relativePath.c_str());

		const std::string when = savedAtText(e.savedAtUnix);
		if (!when.empty()) ImGui::TextDisabled("Copied %s", when.c_str());
		if (e.targetPath.empty())
		{
			ImGui::SameLine();
			ImGui::TextColored(kWarn, "Outside this project, cannot be restored here.");
		}
		else if (e.targetMissing)
		{
			ImGui::SameLine();
			ImGui::TextColored(kWarn, "The file is gone; Restore creates it again.");
		}
		else if (e.changedSince)
		{
			ImGui::SameLine();
			ImGui::TextColored(kWarn, "The file changed after this copy; Restore rolls that back.");
		}

		const float btnW = 100.0f;
		ImGui::SameLine(ImGui::GetContentRegionMax().x - 2.0f * btnW - ImGui::GetStyle().ItemSpacing.x);
		ImGui::BeginDisabled(e.targetPath.empty());
		if (EditorWidgets::smallButton("Restore"))
		{
			act = Act::Restore;
			actKey = e.key;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (EditorWidgets::dangerSmallButton("Delete Copy"))
		{
			act = Act::Discard;
			actKey = e.key;
		}
		ImGui::Separator();
		ImGui::PopID();
	}
	ImGui::EndChild();

	if (act == Act::Restore && ctx.restoreAssetRecovery)
	{
		std::string why;
		if (ctx.restoreAssetRecovery(actKey, &why)) s_error.clear();
		else s_error = why.empty() ? std::string("The copy could not be restored.") : why;
	}
	else if (act == Act::Discard && ctx.discardAssetRecovery)
	{
		ctx.discardAssetRecovery(actKey);
		s_error.clear();
	}

	{
		EditorWidgets::WrapText wrap(wrapWidth);
		if (!s_error.empty())
		{
			ImGui::Spacing();
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "%s", s_error.c_str());
		}
		ImGui::Spacing();
		ImGui::TextWrapped(
			"Restore writes the copy into the file and reloads any tab showing it. The "
			"version it replaces is kept in %s. Keep for Later leaves the copies where "
			"they are and asks again next time.",
			ctx.assetRecoveryReplacedDir.empty() ? "Saved/Autosave/Assets/Replaced"
			                                     : ctx.assetRecoveryReplacedDir.c_str());
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
	}

	// "All" runs over the rows that are left; a failed one stays, with its reason.
	if (EditorWidgets::primaryButton("Restore All", ImVec2(120, 0)) && ctx.restoreAssetRecovery)
	{
		s_error.clear();
		for (const HE::Ed::AssetRecoveryEntry& e : offers)
		{
			if (e.targetPath.empty()) continue;
			std::string why;
			if (!ctx.restoreAssetRecovery(e.key, &why) && s_error.empty())
				s_error = why.empty() ? "Could not restore " + e.relativePath + "." : why;
		}
	}
	ImGui::SameLine();
	if (EditorWidgets::dangerButton("Delete All", ImVec2(120, 0)) && ctx.discardAssetRecovery)
	{
		for (const HE::Ed::AssetRecoveryEntry& e : offers) ctx.discardAssetRecovery(e.key);
		s_error.clear();
	}
	ImGui::SameLine();
	if (EditorWidgets::cancelButton("Keep for Later", ImVec2(130, 0)) ||
	    ImGui::IsKeyPressed(ImGuiKey_Escape))
	{
		if (ctx.deferAssetRecovery) ctx.deferAssetRecovery();
		s_error.clear();
		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
#else
	(void)ctx;
#endif
}

} // namespace AssetRecoveryDialog
