#include "GitMissingDialog.h"
#include "EditorApplication.h"        // AppContext
#include "EditorWidgets.h"            // pinDialogToEditorWindow

#include <SourceControl/GitProbe.h>

#include <SDL3/SDL.h>
#include <cstring>
#include <string>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace GitMissingDialog
{

// Set by Preferences ▸ Source Control ▸ Recheck, so an explicit request always
// shows the result even when the warning was permanently dismissed.
static bool s_forceShow = false;

void requestShow() { s_forceShow = true; }

#ifdef HE_IMGUI_ENABLED
namespace {

// Copy-a-command + open-a-page pair, the same manual fallback shape the
// toolchain dialog uses. Kept together so every remedy in this dialog offers
// both — some people want the command, some want the download page.
void remedyButtons(const char* buttonLabel, const char* command,
                   const char* linkLabel, const char* url)
{
	if (ImGui::SmallButton(buttonLabel)) ImGui::SetClipboardText(command);
	if (linkLabel && url)
	{
		ImGui::SameLine();
		if (ImGui::SmallButton(linkLabel)) SDL_OpenURL(url);
	}
}

void gitInstallRemedy()
{
#if defined(__APPLE__)
	ImGui::TextWrapped("On macOS, git comes with the Xcode Command Line Tools.");
	remedyButtons("Copy 'xcode-select --install'", "xcode-select --install",
	              "git-scm.com", "https://git-scm.com/downloads");
#elif defined(_WIN32)
	ImGui::TextWrapped("Install Git for Windows, then restart the editor so it "
	                   "picks up the new PATH.");
	remedyButtons("Copy winget Command", "winget install --id Git.Git -e",
	              "Download Page", "https://git-scm.com/download/win");
#else
	ImGui::TextWrapped("Install git through your distribution's package manager.");
	remedyButtons("Copy 'sudo apt install git'", "sudo apt install git",
	              "git-scm.com", "https://git-scm.com/downloads");
#endif
}

void lfsInstallRemedy()
{
#if defined(__APPLE__)
	remedyButtons("Copy 'brew install git-lfs'", "brew install git-lfs",
	              "git-lfs.com", "https://git-lfs.com/");
	// Worth saying explicitly: a Homebrew install is invisible to an app started
	// from Finder unless PATH is repaired, which the engine does — but only for
	// itself, so a terminal check may disagree with what the editor sees.
	ImGui::TextDisabled("After installing, press Recheck — no restart needed.");
#elif defined(_WIN32)
	remedyButtons("Copy winget Command", "winget install --id GitHub.GitLFS -e",
	              "git-lfs.com", "https://git-lfs.com/");
#else
	remedyButtons("Copy 'sudo apt install git-lfs'", "sudo apt install git-lfs",
	              "git-lfs.com", "https://git-lfs.com/");
#endif
}

} // namespace
#endif  // HE_IMGUI_ENABLED

void DrawGitMissingDialog(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	static bool s_awaitingFirstResult = true;   // one auto-open per completed probe
	static bool s_dontShowAgain       = false;
	static bool s_checking            = false;  // a recheck is in flight
	static HE::Sc::GitProbe s_last;
	static bool s_haveLast = false;

	// Pre-filled from whatever git already has, so a user with only one of the
	// two set does not have to retype the other.
	static char s_name[128]  = {};
	static char s_email[128] = {};
	static bool s_identitySeeded = false;

	if (ctx.gitProbe)
	{
		s_checking = false;
		s_last     = *ctx.gitProbe;
		s_haveLast = true;

		if (!s_identitySeeded)
		{
			s_identitySeeded = true;
			std::snprintf(s_name,  sizeof(s_name),  "%s", s_last.userName.c_str());
			std::snprintf(s_email, sizeof(s_email), "%s", s_last.userEmail.c_str());
		}

		if (s_awaitingFirstResult)
		{
			s_awaitingFirstResult = false;
			const bool suppressed = ctx.globalState &&
				ctx.globalState->getCustomConfigBool("SuppressSourceControlWarning", false);
			// Only nag about what actually blocks work. A missing credential
			// helper is not raised on its own — it costs nothing until the first
			// push, and the source-control panel asks about it then.
			if (!s_last.ready() && (s_forceShow || !suppressed))
			{
				s_dontShowAgain = false;
				ImGui::OpenPopup("##SourceControlUnavailable");
			}
			s_forceShow = false;
		}
	}
	else if (s_haveLast)
	{
		// Recheck in flight: keep the popup up showing the previous result rather
		// than blanking it, which would look like the dialog broke.
		s_checking = true;
	}

	// Null probe and nothing cached means the first check is still running. This
	// is the reason the pointer-is-null convention matters: without it the dialog
	// would flash "git missing" during every startup.
	if (!s_haveLast) return;

	ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
	EditorWidgets::pinDialogToEditorWindow();
	if (!ImGui::BeginPopupModal("##SourceControlUnavailable", nullptr,
	                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
		return;

	if (s_last.ready())
	{
		// A recheck came back clean — nothing left to say.
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}

	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
	ImGui::TextUnformatted("Source Control Not Ready");
	ImGui::PopStyleColor();
	ImGui::Separator();
	ImGui::Spacing();

	ImGui::TextWrapped(
		"Syncing this project with GitHub, GitLab, Azure DevOps or any other git "
		"remote needs a few things this machine is missing. The editor works "
		"normally without them — only source control is unavailable.");
	ImGui::Spacing();

	// ── git ──────────────────────────────────────────────────────────────────
	if (!s_last.gitFound)
	{
		ImGui::BulletText("git was not found.");
		ImGui::Indent();
		gitInstallRemedy();
		ImGui::Unindent();
		ImGui::Spacing();
	}
	else
	{
		ImGui::BulletText("git %s found%s.",
		                  s_last.gitVersion.empty() ? "(unknown version)" : s_last.gitVersion.c_str(),
		                  s_last.gitPath.empty() ? "" : (" at " + s_last.gitPath.string()).c_str());
	}

	// ── git-lfs ──────────────────────────────────────────────────────────────
	if (s_last.gitFound && !s_last.lfsFound)
	{
		ImGui::BulletText("Git LFS was not found.");
		ImGui::Indent();
		// Said plainly, because "an optional extension is missing" badly
		// understates it: LFS is what carries meshes, textures and audio, which
		// are the files source control is needed for in the first place.
		ImGui::TextWrapped(
			"Git LFS stores large binary files — meshes, textures, audio. Without "
			"it those would be committed straight into the repository, which most "
			"hosts reject above 100 MB per file and which makes every clone "
			"download the entire history of every asset.");
		lfsInstallRemedy();
		ImGui::Unindent();
		ImGui::Spacing();
	}
	else if (s_last.lfsFound)
	{
		ImGui::BulletText("Git LFS %s found.", s_last.lfsVersion.c_str());
	}

	// ── identity ─────────────────────────────────────────────────────────────
	if (s_last.gitFound && !s_last.identityConfigured)
	{
		ImGui::BulletText("Your name and email are not set in git.");
		ImGui::Indent();
		ImGui::TextWrapped("git records who made each change and refuses to commit "
		                   "without them. This is set once, for all your projects.");
		ImGui::Spacing();

		const bool applying = ctx.gitIdentityApplying;
		if (applying) ImGui::BeginDisabled();
		ImGui::SetNextItemWidth(260.0f);
		ImGui::InputTextWithHint("##scname",  "Your Name",        s_name,  sizeof(s_name));
		ImGui::SetNextItemWidth(260.0f);
		ImGui::InputTextWithHint("##scemail", "you@example.com",  s_email, sizeof(s_email));

		const bool usable = s_name[0] != '\0' && s_email[0] != '\0';
		if (!usable) ImGui::BeginDisabled();
		if (ImGui::Button("Save Identity") && ctx.setGitIdentity)
			ctx.setGitIdentity(s_name, s_email);
		if (!usable) ImGui::EndDisabled();
		if (applying) ImGui::EndDisabled();

		if (applying) { ImGui::SameLine(); ImGui::TextDisabled("Saving…"); }
		ImGui::Unindent();
		ImGui::Spacing();
	}
	else if (s_last.identityConfigured)
	{
		ImGui::BulletText("Identity: %s <%s>", s_last.userName.c_str(), s_last.userEmail.c_str());
	}

	// ── credential helper ────────────────────────────────────────────────────
	// Informational only. Missing it does not block committing, and the panel
	// deals with it when a token is first needed — raising it here would make the
	// dialog look more alarming than the situation is.
	if (s_last.gitFound && s_last.credentialHelper.empty())
	{
		ImGui::BulletText("No credential helper is configured.");
		ImGui::Indent();
		ImGui::TextDisabled("Only needed to push. Source control will offer to set "
		                    "this up when you sign in.");
		ImGui::Unindent();
	}

	if (!s_last.detail.empty())
	{
		ImGui::Spacing();
		if (ImGui::TreeNode("Details"))
		{
			ImGui::TextUnformatted(s_last.detail.c_str());
			ImGui::TreePop();
		}
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Checkbox("Don't show this again", &s_dontShowAgain);
	ImGui::Spacing();

	if (s_checking) ImGui::BeginDisabled();
	if (ImGui::Button("Recheck") && ctx.recheckGit) ctx.recheckGit();
	if (s_checking) ImGui::EndDisabled();
	ImGui::SameLine();

	if (s_checking)
	{
		ImGui::TextDisabled("Rechecking…");
	}
	else if (ImGui::Button("Close"))
	{
		if (s_dontShowAgain && ctx.globalState)
		{
			ctx.globalState->setCustomConfigEntry("SuppressSourceControlWarning", true);
			ctx.globalState->writeConfig();
		}
		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
#else
	(void)ctx;
#endif
}

} // namespace GitMissingDialog
