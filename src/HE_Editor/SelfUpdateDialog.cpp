#include "SelfUpdateDialog.h"

#ifdef HE_HAVE_SELFUPDATE
#include "EditorApplication.h"   // AppContext (open project for the relaunch)
#include "EditorHelp.h"          // "Self-Update/<label>" scope for the tooltips
#include "EditorSelfUpdate.h"
#include "EditorWidgets.h"       // pinDialogToEditorWindow, primaryButton, WrapText

#include <SelfUpdate/SelfUpdateApplyCore.h>   // readApplyResult: how the last update ended
#include <SelfUpdate/Takeover.h>              // pruneTakeoverLeftovers: the older backups

#include <Diagnostics/Log.h>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace SelfUpdateDialog
{
bool available() { return true; }

namespace
{
constexpr const char* kPopupId = "Check for Updates##selfupdate";

bool        s_openRequested = false;
bool        s_isOpen        = false;
bool        s_installRequested = false;
std::string s_lastApply;   // one line about the previous takeover, read on open

std::unique_ptr<EditorSelfUpdate::Session> s_session;
// startHousekeeping(). Unlike the session's worker this one exists after every
// start, so an exit that never reaches joinPendingWork() (std::exit somewhere
// past the start-up probes) must not meet a joinable std::thread at static
// teardown — that is std::terminate.
struct JoiningThread
{
	std::thread t;
	~JoiningThread() { if (t.joinable()) t.join(); }
};
JoiningThread s_housekeeping;

// The acceptance driver (SelfUpdateDialog.h). Read once.
struct E2E
{
	bool                  active = false;
	PromptAnswer          answer = PromptAnswer::None;
	std::filesystem::path trigger;
	enum class Stage { WaitTrigger, Running, Requested, Done } stage = Stage::WaitTrigger;
	bool                  answerPending = false;
};
E2E& e2e()
{
	static E2E s = [] {
		E2E e;
		const char* mode = std::getenv("HE_SELFUPDATE_E2E");
		const char* trig = std::getenv("HE_SELFUPDATE_E2E_TRIGGER");
		if (!mode || !trig || !*trig) return e;
		const std::string m = mode;
		e.answer = m == "save" ? PromptAnswer::Save : m == "discard" ? PromptAnswer::Discard
		         : m == "cancel" ? PromptAnswer::Cancel : PromptAnswer::None;
		if (e.answer == PromptAnswer::None) return e;
		e.trigger = trig;
		e.active  = true;
		HE_LOG_INFO(SelfUpdate, "E2E driver armed: prompt answer '%s', waiting for %s", mode, trig);
		return e;
	}();
	return s;
}
const char* answerName(PromptAnswer a)
{
	switch (a)
	{
	case PromptAnswer::Save:    return "Save";
	case PromptAnswer::Discard: return "Discard";
	case PromptAnswer::Cancel:  return "Cancel";
	case PromptAnswer::None:    break;
	}
	return "none";
}

EditorSelfUpdate::Session& session()
{
	if (!s_session)
		s_session = std::make_unique<EditorSelfUpdate::Session>(EditorSelfUpdate::productBackend());
	return *s_session;
}

// HorizonUpdater writes last-apply.json into staging whatever the outcome, and
// a rollback is exactly what a user who is back in the OLD version wants to be
// told about. Read when the dialog opens, not at start-up: nobody asked yet.
std::string lastApplyLine()
{
	const std::filesystem::path root = HE::Su::defaultStagingDir();
	if (root.empty()) return {};
	HE::Su::ApplyResult r;
	std::string err;
	if (!HE::Su::readApplyResult((root / "last-apply.json").string(), r, err)) return {};
	std::string line = std::string("Last update ") + HE::Su::applyStatusName(r.status) +
	                   ": " + r.fromVersion + " -> " + r.toVersion;
	if (!r.error.empty()) line += " (" + r.error + ")";
	return line;
}
}

PromptAnswer takeE2EPromptAnswer()
{
	E2E& d = e2e();
	if (!d.active || !d.answerPending) return PromptAnswer::None;
	d.answerPending = false;
	d.stage = E2E::Stage::Done;
	HE_LOG_INFO(SelfUpdate, "E2E: unsaved-changes prompt answered with %s", answerName(d.answer));
	return d.answer;
}

void open()
{
	s_openRequested = true;
}

bool isOpen() { return s_isOpen || s_openRequested; }

bool takeInstallRequest()
{
	const bool r = s_installRequested;
	s_installRequested = false;
	return r;
}

bool beginInstall(AppContext& ctx)
{
	const std::string project = (ctx.projectLoaded && ctx.projectManager)
		? ctx.projectManager->currentProject().path : std::string{};
	const HE::Su::TakeoverResult r =
		session().install(EditorSelfUpdate::relaunchArgsFor(ctx.projectLoaded, project));
	if (!r.ok)
	{
		// Back in front, with the reason where the Restart button was. The editor
		// has not quit and nothing of the install was touched.
		s_isOpen = false;
		s_openRequested = true;
	}
	return r.ok;
}

void Draw(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	(void)ctx;
	HE::Ed::Help::Scope helpScope("Self-Update");
	EditorSelfUpdate::Session& su = session();

	E2E& drive = e2e();
	if (drive.active && drive.stage == E2E::Stage::WaitTrigger)
	{
		std::error_code ec;
		if (std::filesystem::exists(drive.trigger, ec))
		{
			std::filesystem::remove(drive.trigger, ec);
			HE_LOG_INFO(SelfUpdate, "E2E: trigger seen -- opening Check for Updates");
			drive.stage = E2E::Stage::Running;
			open();
		}
	}

	if (s_openRequested)
	{
		s_openRequested = false;
		s_isOpen        = true;
		s_lastApply     = lastApplyLine();
		// Opening asks the server again unless something is already under way
		// or waiting for the user (a staged build, a failed install's reason).
		const EditorSelfUpdate::Phase p = su.snapshot().phase;
		if (!su.snapshot().busy() && p != EditorSelfUpdate::Phase::Ready &&
		    p != EditorSelfUpdate::Phase::Available)
			su.startCheck();
		ImGui::OpenPopup(kPopupId);
	}
	if (!s_isOpen) return;

	const EditorSelfUpdate::Snapshot s = su.snapshot();
	using P = EditorSelfUpdate::Phase;

	// The driver presses the buttons below; it stops (and says where) on any
	// state a user would have had to read.
	const bool driving = drive.active && drive.stage == E2E::Stage::Running;
	if (driving && !s.busy() && s.phase != P::Idle && s.phase != P::Available && !s.canInstall())
	{
		HE_LOG_WARN(SelfUpdate, "E2E: stopped at %s: %s", EditorSelfUpdate::phaseName(s.phase),
		            (s.message.empty() ? s.installBlocker : s.message).c_str());
		drive.stage = E2E::Stage::Done;
	}

	EditorWidgets::pinDialogToEditorWindow(ImVec2(460.0f, 0.0f));
	if (!ImGui::BeginPopupModal(kPopupId, nullptr,
	                            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
	{
		s_isOpen = false;   // dismissed with Escape; a running check carries on
		return;
	}
	{
		EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 32.0f);
		ImGui::Text("Running version: %s", s.localVersion.c_str());
		ImGui::Spacing();
		switch (s.phase)
		{
		case P::Idle:
		case P::Checking:
			ImGui::TextUnformatted("Checking the update server...");
			break;
		case P::NotConfigured:
			ImGui::TextUnformatted("Updates are not set up for this editor.");
			ImGui::TextDisabled("%s", s.message.c_str());
			break;
		case P::UpToDate:
			ImGui::TextUnformatted("This is the newest version.");
			break;
		case P::RemoteOlder:
			ImGui::Text("The server offers %s, which is older than this build. Nothing to do.",
			            s.remoteVersion.c_str());
			break;
		case P::NoBuild:
			ImGui::Text("Version %s is out, but there is no build of it for this platform yet.",
			            s.remoteVersion.c_str());
			break;
		case P::Available:
			ImGui::Text("Version %s is available.", s.remoteVersion.c_str());
			break;
		case P::Downloading:
		{
			ImGui::Text("Downloading %s...", s.remoteVersion.c_str());
			const float f = s.total ? static_cast<float>(static_cast<double>(s.done) / static_cast<double>(s.total)) : 0.0f;
			char overlay[64];
			std::snprintf(overlay, sizeof overlay, "%.1f / %.1f MB",
			              static_cast<double>(s.done) / 1048576.0, static_cast<double>(s.total) / 1048576.0);
			ImGui::ProgressBar(f, ImVec2(-1.0f, 0.0f), overlay);
			break;
		}
		case P::Ready:
		case P::Installing:
			ImGui::Text("Version %s is downloaded and verified.", s.remoteVersion.c_str());
			if (s.installBlocker.empty())
				ImGui::TextDisabled("Restarting closes the editor. You are asked about unsaved "
				                    "changes first, and the open project reopens in the new version.");
			break;
		case P::Failed:
			ImGui::TextUnformatted("The update did not go through.");
			break;
		}
		if (!s.notes.empty() && (s.phase == P::Available || s.phase == P::Downloading || s.phase == P::Ready))
		{
			ImGui::Spacing();
			ImGui::TextDisabled("What's new:");
			ImGui::TextUnformatted(s.notes.c_str());
		}
		const std::string& problem = s.phase == P::Failed ? s.message : s.installBlocker;
		if (!problem.empty() && s.phase != P::NotConfigured)
		{
			ImGui::Spacing();
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", problem.c_str());
		}
		if (!s_lastApply.empty())
		{
			ImGui::Spacing();
			ImGui::TextDisabled("%s", s_lastApply.c_str());
		}
	}
	ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

	if (s.phase == P::Available)
	{
		if (EditorWidgets::primaryButton("Download", ImVec2(130, 0)) || driving)
		{
			if (driving) HE_LOG_INFO(SelfUpdate, "E2E: pressing Download");
			su.startDownload();
		}
		ImGui::SameLine();
	}
	if (s.canInstall())
	{
		if (EditorWidgets::primaryButton("Restart to Update", ImVec2(150, 0)) || driving)
		{
			if (driving)
			{
				HE_LOG_INFO(SelfUpdate, "E2E: pressing Restart to Update");
				drive.stage         = E2E::Stage::Requested;
				drive.answerPending = true;
			}
			// Not installed from here: the guard in EditorUI asks about unsaved
			// work first, exactly as Quit does, and then calls beginInstall().
			HE_LOG_INFO(SelfUpdate, "Editor: Restart to Update requested -- handing over to the unsaved-changes guard");
			s_installRequested = true;
			s_isOpen = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
	}
	if (!s.busy() && s.phase != P::Installing)
	{
		if (EditorWidgets::button("Check Again", ImVec2(110, 0))) su.startCheck();
		ImGui::SameLine();
	}
	if (EditorWidgets::cancelButton("Close", ImVec2(100, 0)))
	{
		s_isOpen = false;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
#else
	(void)ctx;
#endif
}

void startHousekeeping()
{
	if (s_housekeeping.t.joinable()) return;
	s_housekeeping.t = std::thread([] {
		const HE::Su::PruneResult r = HE::Su::pruneTakeoverLeftoversForRunningBuild(HE::Su::defaultStagingDir());
		for (const std::string& p : r.removed)
			HE_LOG_INFO(SelfUpdate, "removed the leftovers of an earlier update: %s", p.c_str());
		for (const std::string& e : r.errors)
			HE_LOG_WARN(SelfUpdate, "could not remove the leftovers of an earlier update (next start retries): %s",
			            e.c_str());
		HE_LOG_INFO(SelfUpdate, "update leftovers: kept '%s', removed %zu, %zu too recent to touch, %zu failed",
		            r.kept.c_str(), r.removed.size(), r.young.size(), r.errors.size());
	});
}

void joinPendingWork()
{
	if (s_session) s_session->join();
	if (s_housekeeping.t.joinable()) s_housekeeping.t.join();
}
}

#else // !HE_HAVE_SELFUPDATE — no libssh2, no HorizonSelfUpdate: nothing to offer

namespace SelfUpdateDialog
{
bool available() { return false; }
void open() {}
bool isOpen() { return false; }
void Draw(AppContext&) {}
bool takeInstallRequest() { return false; }
bool beginInstall(AppContext&) { return false; }
void startHousekeeping() {}
void joinPendingWork() {}
PromptAnswer takeE2EPromptAnswer() { return PromptAnswer::None; }
}

#endif
