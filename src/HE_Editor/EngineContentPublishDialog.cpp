#include "EngineContentPublishDialog.h"
#include "EditorApplication.h"           // AppContext
#include "EditorWidgets.h"               // pinDialogToEditorWindow

#ifdef HE_HAVE_LIBSSH2
#include <ContentSync/EngineContentPublish.h>
#endif
#include <ContentManager/ContentManager.h>

#include <atomic>
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
// Written by the worker, read by the render thread every frame — atomics, not
// plain bools. Nothing here is a lock-free algorithm; the point is simply that
// a torn/never-observed read of a plain bool across threads is UB, and the
// render thread polls these at frame rate.
static std::atomic<bool> s_running{ false };
static std::atomic<bool> s_finished{ false };
static std::atomic<bool> s_lastOk{ false };
// Set once when a run finishes SUCCESSFULLY; drained (exchange(false)) by
// EditorApplication, which then re-fetches the manifest. Both operations
// rewrite the server's manifest.json, so the Editor's in-memory copy is stale
// the moment either one succeeds — without this the Content Browser keeps
// showing the pre-publish catalogue until the next Editor restart.
static std::atomic<bool> s_runSucceeded{ false };
static bool              s_openRequested = false;   // render thread only
// Both operations write manifest.json, so only one may run at a time (the
// s_running guard already enforces that) — this just says which one, for the
// title and so a stray double-open can't start the wrong worker mid-run.
static std::string       s_title;                   // render thread only

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
		s_lastOk = result.ok;
		if (result.ok) s_runSucceeded.store(true, std::memory_order_release);
		// `finished` before `running`: the render thread treats "not running and
		// not finished" as the idle state, so clearing running first would flash
		// an idle dialog for one frame between the two stores.
		s_finished.store(true,  std::memory_order_release);
		s_running.store(false, std::memory_order_release);
	});
}
#endif

bool takeRunSucceeded()
{
#ifdef HE_HAVE_LIBSSH2
	return s_runSucceeded.exchange(false, std::memory_order_acq_rel);
#else
	return false;
#endif
}

void shutdown()
{
#ifdef HE_HAVE_LIBSSH2
	// A joinable std::thread that reaches its destructor calls std::terminate.
	// startRun only joins the PREVIOUS run's thread when a new run starts, so
	// after the last publish/rebuild of a session the object sits joinable until
	// static destruction — i.e. every editor session that used this dialog once
	// aborted on quit. Same rule (and same fix) as EditorApplication's own probe
	// threads; see the join block in OnDetach.
	if (s_thread.joinable()) s_thread.join();
#endif
}

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
		// One snapshot per frame: reading each atomic twice could otherwise show
		// "running" in one branch and "finished" in the next within a single frame.
		const bool running  = s_running.load(std::memory_order_acquire);
		const bool finished = s_finished.load(std::memory_order_acquire);

		ImGui::Text("%s", s_title.c_str());
		ImGui::Separator();
		ImGui::Spacing();

		if (running)
		{
			ImGui::TextDisabled("Working...");
		}
		else if (finished)
		{
			if (s_lastOk.load(std::memory_order_acquire))
				ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "Finished.");
			else
				ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.50f, 1.0f), "Failed — see log below.");
		}

		ImGui::Spacing();
		{
			std::lock_guard<std::mutex> lock(s_logMutex);
			ImGui::InputTextMultiline("##engine_content_publish_log",
				const_cast<char*>(s_log.c_str()), s_log.size() + 1,
				ImVec2(-1.0f, 240.0f), ImGuiInputTextFlags_ReadOnly);
		}
		ImGui::Spacing();

		if (!running)
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
