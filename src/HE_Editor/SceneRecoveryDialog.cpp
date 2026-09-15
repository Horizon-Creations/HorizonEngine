#include "SceneRecoveryDialog.h"
#include "EditorApplication.h"   // AppContext
#include "EditorHelp.h"          // the "Scene Recovery" scope for the three buttons
#include "EditorWidgets.h"       // pinDialogToEditorWindow, the button verbs, WrapText

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace SceneRecoveryDialog
{

#ifdef HE_IMGUI_ENABLED
namespace
{
	constexpr const char* kTitle = "##SceneRecovery";

	// "15 Sep 2026, 04:31" in local time, or nothing when the manifest carried
	// no stamp (a snapshot from before the field existed, or a clock at zero).
	std::string savedAtText(std::int64_t unix)
	{
		if (unix <= 0) return {};
		const std::time_t secs = static_cast<std::time_t>(unix);
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
}
#endif

void Draw(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	if (!ctx.recoveryOffer) return;

	// Raised while the offer stands, but never over another root-level modal:
	// OpenPopup at root level REPLACES whatever popup is open there, and the
	// toolchain and source-control checks raise theirs at the same moment of
	// startup. Whichever is up first stays up; this one waits its turn, since
	// the offer is still there next frame.
	if (!ImGui::IsPopupOpen(kTitle) &&
	    !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
		ImGui::OpenPopup(kTitle);

	// The width the text below is written for; the paragraph wraps at a fixed
	// column rather than the window edge, for the reason GitMissingDialog
	// spells out (an auto-resizing window that wraps at its own edge shrinks
	// a little every frame).
	constexpr float kDialogWidth = 520.0f;
	ImGui::SetNextWindowSize(ImVec2(kDialogWidth, 0.0f), ImGuiCond_Appearing);
	EditorWidgets::pinDialogToEditorWindow();
	if (!ImGui::BeginPopupModal(kTitle, nullptr,
	                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
	                            ImGuiWindowFlags_NoSavedSettings))
		return;

	HE::Ed::Help::Scope helpScope("Scene Recovery");
	const HE::Ed::RecoveryInfo offer = *ctx.recoveryOffer;   // a copy: the buttons clear it

	const std::string sceneName = offer.scenePath.empty()
		? std::string("an unsaved scene")
		: "\"" + std::filesystem::path(offer.scenePath).stem().string() + "\"";
	const std::string when = savedAtText(offer.savedAtUnix);

	{
		EditorWidgets::WrapText wrap(kDialogWidth - ImGui::GetStyle().WindowPadding.x);

		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
		ImGui::TextUnformatted("Unsaved Work Found");
		ImGui::PopStyleColor();
		ImGui::Separator();
		ImGui::Spacing();

		ImGui::TextWrapped(
			"The last session did not end cleanly and left an autosaved copy of %s%s%s. "
			"The scene file itself was not changed.",
			sceneName.c_str(),
			when.empty() ? "" : " from ",
			when.c_str());
		ImGui::Spacing();

		if (!offer.scenePath.empty())
		{
			ImGui::TextDisabled("Scene:");
			ImGui::SameLine();
			ImGui::TextUnformatted(offer.scenePath.c_str());
		}
		ImGui::TextDisabled("Copy:");
		ImGui::SameLine();
		ImGui::TextUnformatted(offer.snapshotPath.c_str());
		ImGui::Spacing();

		ImGui::TextWrapped(
			"Restore loads the copy over the scene as one undo step, so Undo takes you "
			"back to the file on disk and nothing is written until you save. "
			"Keep for Later leaves the copy where it is and asks again next time.");
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
	}

	if (EditorWidgets::primaryButton("Restore", ImVec2(110, 0)))
	{
		if (ctx.restoreRecovery) ctx.restoreRecovery();
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (EditorWidgets::dangerButton("Delete Snapshot", ImVec2(140, 0)))
	{
		if (ctx.discardRecovery) ctx.discardRecovery();
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (EditorWidgets::cancelButton("Keep for Later", ImVec2(130, 0)) ||
	    ImGui::IsKeyPressed(ImGuiKey_Escape))
	{
		if (ctx.deferRecovery) ctx.deferRecovery();
		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
#else
	(void)ctx;
#endif
}

} // namespace SceneRecoveryDialog
