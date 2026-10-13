#include "GitMissingDialog.h"
#include "EditorApplication.h"        // AppContext
#include "EditorHelp.h"               // the Preferences page's "Source Control" scope, reused
#include "EditorWidgets.h"            // pinDialogToEditorWindow

#include <SourceControl/GitInstall.h>
#include <SourceControl/GitProbe.h>

#include <SDL3/SDL.h>
#include <cstring>
#include <optional>
#include <string>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace GitMissingDialog
{

#ifdef HE_IMGUI_ENABLED
namespace {

// What "Install Git" is about to do, in the terms the user will then see it
// happen in — a UAC prompt, Apple's window, a password dialog.
std::string routeSentence(const HE::Sc::GitInstallPlan& plan, bool lfs)
{
	using Route = HE::Sc::GitInstallRoute;
	const char* what = lfs ? "Git LFS" : "git";
	switch (plan.route)
	{
	case Route::Winget:
		return lfs ? "The editor can install Git LFS through winget."
		           : "The editor can install Git for Windows (Git LFS included) through winget.";
	case Route::Homebrew:
		return std::string("The editor can install ") + what + " with Homebrew. No password is needed.";
	case Route::XcodeCommandLineTools:
		return "The editor can start Apple's installer for the Command Line Tools, "
		       "which bring git (a download of several hundred MB).";
	default:
		break;
	}
	return std::string("The editor can install ") + what + " with " +
	       HE::Sc::gitInstallRouteName(plan.route) + ".";
}

// The remedy for one of the two, git or Git LFS: both go through the same routes and
// the same worker, so both read the same way — what will run, the button, the manual
// way, and the installer's output while it runs.
void drawInstallRemedy(AppContext& ctx, bool lfs)
{
	// Its own scope, so the places that draw this resolve one set of entries,
	// whichever scope they have open around it.
	HE::Ed::Help::Scope helpScope("Source Control");

	// The probe is null while the install's own re-probe runs; the plan it
	// answered with before is the one being carried out, so it stays on screen.
	static HE::Sc::GitInstallPlan s_plan;
	if (ctx.gitProbe) s_plan = ctx.gitProbe->install;
	const HE::Sc::GitInstallPlan& plan = s_plan;

	const HE::Sc::GitInstallTarget target =
		lfs ? HE::Sc::GitInstallTarget::GitLfs : HE::Sc::GitInstallTarget::Git;
	// One worker serves both: the button of the other target is off while it runs,
	// and the log below belongs to whichever was started last.
	const bool installing   = ctx.gitInstalling;
	const bool ownInstall   = ctx.gitInstallIsLfs == lfs;
	const std::optional<HE::Sc::GitInstallLaunch> launch = HE::Sc::gitInstallLaunch(plan, target);
	const std::string& manualCommand = lfs ? plan.lfsManualCommand : plan.manualCommand;

	if (launch)
	{
		ImGui::TextWrapped("%s", routeSentence(plan, lfs).c_str());
		if (!launch->userPrompt.empty()) ImGui::TextWrapped("%s", launch->userPrompt.c_str());
		ImGui::Spacing();

		if (installing) ImGui::BeginDisabled();
		if (lfs)
		{
			if (EditorWidgets::primaryButton("Install Git LFS") && ctx.startGitLfsInstall)
				ctx.startGitLfsInstall();
		}
		else if (EditorWidgets::primaryButton("Install Git") && ctx.startGitInstall)
		{
			ctx.startGitInstall();
		}
		if (installing) ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::TextDisabled(installing && ownInstall ? "Installing\xe2\x80\xa6 (progress below)"
		                                             : "or do it yourself:");
		// Only where it can keep its word: the wait for Apple's installer stops at
		// once (a dismissed dialog would otherwise hold this for an hour), while a
		// running winget or apt cannot be interrupted, so it gets no button.
		if (installing && ownInstall && launch->returnsBeforeInstalled && ctx.cancelGitInstall)
		{
			ImGui::SameLine();
			if (EditorWidgets::smallButton("Cancel")) ctx.cancelGitInstall();
		}
	}
	else if (!lfs && plan.route == HE::Sc::GitInstallRoute::DownloadInstaller)
	{
		// The engine ships no installer and does not fetch executables itself;
		// the official one is a download away, and the locator finds the git it
		// installs without a restart.
		ImGui::TextWrapped("Download and run the Git for Windows installer (it includes "
		                   "Git LFS), then press Recheck. No restart is needed.");
	}
	else
	{
		// The fallback says WHY, so "cannot" does not read as "broken".
		ImGui::TextWrapped("The editor cannot install %s here by itself: %s.", lfs ? "Git LFS" : "git",
		                   plan.reason.empty() ? "no install route was found" : plan.reason.c_str());
		if (!manualCommand.empty())
			ImGui::TextWrapped("Run this in a terminal, then press Recheck:");
	}

	if (!manualCommand.empty())
	{
		ImGui::TextWrapped("%s", manualCommand.c_str());
		if (lfs)
		{
			if (EditorWidgets::smallButton("Copy LFS Command")) ImGui::SetClipboardText(manualCommand.c_str());
		}
		else if (EditorWidgets::smallButton("Copy Install Command"))
		{
			ImGui::SetClipboardText(manualCommand.c_str());
		}
		ImGui::SameLine();
	}
	if (lfs)
	{
		if (EditorWidgets::smallButton("git-lfs.com")) SDL_OpenURL("https://git-lfs.com/");
	}
	else if (EditorWidgets::smallButton("Download Page"))
	{
		SDL_OpenURL(HE::Sc::gitDownloadPageUrl(HE::Sc::currentPlatform()));
	}

	// ── Live progress log ────────────────────────────────────────────────────
	// Shown in every place that draws this, not just where the button was
	// pressed: it is one install, and the user may have moved on to another panel.
	if (ctx.gitInstallLog && ownInstall && (installing || ctx.gitInstallDone))
	{
		const std::string log = ctx.gitInstallLog();
		if (!log.empty())
		{
			ImGui::Spacing();
			ImGui::BeginChild(lfs ? "##gitlfsinstalllog" : "##gitinstalllog", ImVec2(0.0f, 150.0f), true);
			// Wrapped for the same reason as the toolchain dialog's log: the
			// sentence that says why an install stopped is at the end of a long line.
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(log.c_str());
			ImGui::PopTextWrapPos();
			if (installing) ImGui::SetScrollHereY(1.0f);
			ImGui::EndChild();
		}
		if (!installing && ctx.gitInstallDone && !ctx.gitInstallOk)
			ImGui::TextDisabled(lfs ? "Git LFS was not installed. See the log above, then retry "
			                          "or install it yourself."
			                        : "git was not installed. See the log above, then retry "
			                          "or install it yourself.");
	}
}

} // namespace

// Public: drawn wherever an action needs git (see the header).
void drawGitInstallRemedy(AppContext& ctx) { drawInstallRemedy(ctx, /*lfs=*/false); }
void drawLfsInstallRemedy(AppContext& ctx) { drawInstallRemedy(ctx, /*lfs=*/true); }
#endif  // HE_IMGUI_ENABLED

} // namespace GitMissingDialog
