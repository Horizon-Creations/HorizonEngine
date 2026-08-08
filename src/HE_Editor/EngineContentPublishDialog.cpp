#include "EngineContentPublishDialog.h"
#include "EditorApplication.h"           // AppContext
#include "EditorWidgets.h"               // pinDialogToEditorWindow

#ifdef HE_HAVE_LIBSSH2
#include <ContentSync/EngineContentPublish.h>
#endif
#include <ContentManager/ContentManager.h>

#include <functional>
#include <mutex>
#include <thread>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace EngineContentPublishDialog
{

#ifdef HE_HAVE_LIBSSH2
// PublishResult and RebuildManifestResult (EngineContentPublish.h) are two
// unrelated structs — both happen to carry {ok, error}, which is all this
// dialog needs to know to draw itself. Rather than adding an inheritance
// relationship across the module boundary for one caller, each open*()
// function below just reduces its own result type down to this before handing
// it to the shared runner.
struct RunOutcome { bool ok = false; std::string error; };

static std::thread       s_thread;
static std::mutex        s_logMutex;
static std::string       s_log;
static bool              s_running = false;
static bool              s_finished = false;
static bool              s_lastOk  = false;
static bool              s_openRequested = false;
// Both operations write manifest.json, so only one may run at a time (the
// s_running guard already enforces that) — this just says which one, for the
// title and so a stray double-open can't start the wrong worker mid-run.
static std::string       s_title;

static void appendLog(const std::string& line)
{
	std::lock_guard<std::mutex> lock(s_logMutex);
	s_log += line;
	s_log += '\n';
}

// Shared by open()/openRebuildFromServer(): resets the log/state and starts
// `work` on a fresh worker thread. `work` must itself call appendLog for
// progress and return {ok, error} — the two callers just differ in what they
// pass here and what title to show.
static void startRun(const std::string& title, std::function<RunOutcome()> work)
{
	if (s_running) return;
	if (s_thread.joinable()) s_thread.join(); // previous run, already finished

	{
		std::lock_guard<std::mutex> lock(s_logMutex);
		s_log.clear();
	}
	s_title         = title;
	s_finished      = false;
	s_running       = true;
	s_openRequested = true;

	s_thread = std::thread([work = std::move(work)]
	{
		const RunOutcome result = work();
		if (!result.ok) appendLog("FAILED: " + result.error);
		s_lastOk   = result.ok;
		s_running  = false;
		s_finished = true;
	});
}
#endif

void open(AppContext& ctx)
{
#ifdef HE_HAVE_LIBSSH2
	const std::string engineContentRoot = ctx.contentManager ? ctx.contentManager->engineContentRoot()
	                                                          : std::string();
	startRun("Publish Engine Content to Server", [engineContentRoot]() -> RunOutcome
	{
		const HE::Cs::PublishResult r = HE::Cs::publishEngineContentBlocking(
			engineContentRoot, [](const std::string& line) { appendLog(line); });
		return { r.ok, r.error };
	});
#else
	(void)ctx;
#endif
}

void openRebuildFromServer(AppContext& ctx)
{
#ifdef HE_HAVE_LIBSSH2
	(void)ctx;
	startRun("Rebuild Manifest from Server", []() -> RunOutcome
	{
		const HE::Cs::RebuildManifestResult r =
			HE::Cs::rebuildManifestFromServerBlocking([](const std::string& line) { appendLog(line); });
		return { r.ok, r.error };
	});
#else
	(void)ctx;
#endif
}

void Draw(AppContext& ctx)
{
#if defined(HE_HAVE_LIBSSH2) && defined(HE_IMGUI_ENABLED)
	(void)ctx;
	if (s_openRequested)
	{
		ImGui::OpenPopup("##engine_content_publish_popup");
		s_openRequested = false;
	}

	ImGui::SetNextWindowSize(ImVec2(520, 360), ImGuiCond_Appearing);
	EditorWidgets::pinDialogToEditorWindow();
	if (ImGui::BeginPopupModal("##engine_content_publish_popup", nullptr, ImGuiWindowFlags_NoTitleBar))
	{
		ImGui::Text("%s", s_title.c_str());
		ImGui::Separator();
		ImGui::Spacing();

		if (s_running)
		{
			ImGui::TextDisabled("Publishing...");
		}
		else if (s_finished)
		{
			if (s_lastOk)
				ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "Publish complete.");
			else
				ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.50f, 1.0f), "Publish failed — see log below.");
		}

		ImGui::Spacing();
		{
			std::lock_guard<std::mutex> lock(s_logMutex);
			ImGui::InputTextMultiline("##engine_content_publish_log",
				const_cast<char*>(s_log.c_str()), s_log.size() + 1,
				ImVec2(-1.0f, 240.0f), ImGuiInputTextFlags_ReadOnly);
		}
		ImGui::Spacing();

		if (!s_running)
		{
			if (EditorWidgets::primaryButton("Close", ImVec2(120, 0)))
				ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
#else
	(void)ctx;
#endif
}

} // namespace EngineContentPublishDialog
