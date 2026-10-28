#include "SceneDiskWatch.h"

#include "EditorApplication.h"   // AppContext
#include "EditorHelp.h"          // "Scene Changed" scope for the buttons
#include "EditorWidgets.h"       // pinDialogToEditorWindow, buttons
#include "SceneFileStamp.h"

#include <Diagnostics/Logger.h>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

#include <chrono>
#include <filesystem>

namespace SceneDiskWatch
{
namespace
{
SceneFileStamp s_stamp;
bool           s_asking = false;
std::chrono::steady_clock::time_point s_nextPoll{};
}

void remember(const std::string& scenePath)
{
	s_stamp.remember(scenePath);
	s_asking = false;
}

bool asking() { return s_asking; }

bool blocksSave(const std::string& scenePath)
{
	if (scenePath.empty() || scenePath != s_stamp.path()) return false;
	if (!s_asking && !s_stamp.changed()) return false;
	if (!s_asking) HE_LOG_WARN(Editor, "Save refused: %s changed on disk since it was loaded", scenePath.c_str());
	s_asking = true;
	return true;
}

void render(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	const std::string& path = ctx.currentScenePath;

	// A different scene than the one stamped (opened, saved as, created): take its
	// stamp as it is now. The explicit remember() calls after the editor's own reads
	// and writes cover the case this cannot see - the same path, rewritten by us.
	if (path != s_stamp.path())
	{
		s_stamp.remember(path);
		s_asking = false;
	}

	if (!s_asking && !path.empty() && !ctx.isPlaying)
	{
		const auto now = std::chrono::steady_clock::now();
		if (now >= s_nextPoll)
		{
			s_nextPoll = now + std::chrono::seconds(1);
			if (s_stamp.changed())
			{
				s_asking = true;
				HE_LOG_INFO(Editor, "Scene changed on disk: %s", path.c_str());
			}
			else
			{
				s_stamp.refreshTimes();   // a touch with identical bytes: stop asking the cheap question
			}
		}
	}

	if (!s_asking) return;

	HE::Ed::Help::Scope helpScope("Scene Changed");
	if (!ImGui::IsPopupOpen("##scene_changed_on_disk")) ImGui::OpenPopup("##scene_changed_on_disk");
	EditorWidgets::pinDialogToEditorWindow(ImVec2(520.0f, 0.0f));
	if (ImGui::BeginPopupModal("##scene_changed_on_disk", nullptr,
	        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
	{
		const std::string name = std::filesystem::path(path).stem().string();
		ImGui::Text("\"%s\" changed on disk", name.c_str());
		ImGui::Separator();
		ImGui::Spacing();
		{
			EditorWidgets::WrapText wrap(490.0f);
			ImGui::TextWrapped("The scene file was changed outside the editor, most likely by a git "
			                   "pull or a checkout. What you see is still the version that was loaded "
			                   "before. Reload to see what is on disk now.");
			ImGui::Spacing();
			if (ctx.sceneDirty)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
				ImGui::TextWrapped("You have unsaved changes in this scene. Reloading drops them; "
				                   "keeping your version means the next save overwrites the file that "
				                   "came from disk.");
				ImGui::PopStyleColor();
			}
			else
			{
				ImGui::TextDisabled("Keeping your version means the next save overwrites the file that "
				                    "came from disk.");
			}
		}
		ImGui::Spacing();

		bool reload = false, keep = false;
		if (EditorWidgets::primaryButton("Reload Scene", ImVec2(160.0f, 0.0f))) reload = true;
		ImGui::SameLine();
		if (EditorWidgets::button("Keep My Version", ImVec2(160.0f, 0.0f))) keep = true;
		if (reload || keep) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();

		if (reload)
		{
			HE_LOG_INFO(Editor, "Scene changed on disk: reloading %s", path.c_str());
			const std::string p = path;      // openScene may rewrite ctx.currentScenePath
			if (ctx.openScene) ctx.openScene(p);
			s_stamp.remember(p);
			s_asking = false;
		}
		else if (keep)
		{
			// Asked, and answered: the disk version as it is now is what "mine" is being
			// kept against. Another change on disk asks again.
			s_stamp.remember(path);
			s_asking = false;
		}
	}
#else
	(void)ctx;
#endif
}

} // namespace SceneDiskWatch
