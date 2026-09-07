#include "GitMissingDialog.h"
#include "EditorApplication.h"        // AppContext
#include "EditorHelp.h"               // the Preferences page's "Source Control" scope, reused
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

// Forces the dialog open on the next completed probe, so an explicit request
// always shows the result even when the warning was permanently dismissed.
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
	// Through the wrapper, so each remedy explains itself. The label is a
	// run-time argument here, but the lookup is done on the string that is
	// actually drawn — and both places these are drawn (this dialog and
	// Preferences » Source Control) have the "Source Control" scope open.
	if (EditorWidgets::smallButton(buttonLabel)) ImGui::SetClipboardText(command);
	if (linkLabel && url)
	{
		ImGui::SameLine();
		if (EditorWidgets::smallButton(linkLabel)) SDL_OpenURL(url);
	}
}

} // namespace

// Public (shared with the Preferences ▸ Editor ▸ Source Control page).
void drawGitInstallRemedy()
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

void drawLfsInstallRemedy()
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

	// The width this dialog is written for: two of its lines are an absolute path
	// and a "name <address>" identity, and it reads as a paragraph, not a form.
	constexpr float kDialogWidth = 560.0f;

	ImGui::SetNextWindowSize(ImVec2(kDialogWidth, 0.0f), ImGuiCond_Appearing);
	EditorWidgets::pinDialogToEditorWindow();
	if (!ImGui::BeginPopupModal("##SourceControlUnavailable", nullptr,
	                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
		return;

	// The SAME scope the Preferences page pushes, on purpose: this dialog and
	// that page state the same facts about the same machine, so "Save Identity"
	// here and there is one entry, not two that will drift.
	HE::Ed::Help::Scope helpScope("Source Control");

	if (s_last.ready())
	{
		// A recheck came back clean — nothing left to say.
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}

	// ── Everything below wraps at the dialog's design width ──────────────────
	// This dialog's entire job is to say what is missing and where, and almost
	// every line it draws ends in the part that answers "where": the path git was
	// found at, the version string, the address in the identity. Those are the
	// halves that fell off the right edge, so the dialog said "git found" and
	// nothing about which git — the one thing the user opened it to learn. The
	// paragraphs were wrapped by hand already; this covers the lines that were not,
	// and the probe's raw detail text under "Details", which is a machine's output
	// and respects no width at all.
	//
	// Scoped, and the scope closes before EndPopup(): the wrap position belongs to
	// this popup's window, so it has to be popped while that window is still the
	// current one. Popped after EndPopup() it would land on whatever window is
	// current then — and this dialog is raised from outside any window, so that is
	// none at all.
	//
	// Wrapped at a FIXED column, not at the window edge, because this popup is
	// AlwaysAutoResize: an auto-sized window fits itself to its widest line, so
	// wrapping at that same edge makes the width its own input. The first version
	// of this guard did exactly that, and since it also replaced the unwrapped
	// BulletText lines — the long ones that were holding the dialog open — nothing
	// was left to push back: every frame the text re-wrapped a little narrower, the
	// window fitted to it, and the dialog visibly walked down toward ImGui's
	// minimum size. A column measured in window space breaks the loop, because it
	// does not depend on how wide the window currently is; the auto-fit then simply
	// settles at the width this dialog was written for. TextWrapped() below inherits
	// it rather than pushing its own 0.0f (it only pushes when nothing is set), so
	// the hand-wrapped paragraphs land on the same column.
	//
	// The alternative — a width FLOOR — would have to be routed through
	// pinDialogToEditorWindow's minSize, since the pin issues its own
	// SetNextWindowSizeConstraints right after any the caller sets. That treats the
	// symptom (the window may no longer shrink) and leaves the wrap width still
	// chasing the window width, so one mechanism, and it is this one.
	{
		EditorWidgets::WrapText wrap(kDialogWidth - ImGui::GetStyle().WindowPadding.x);

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

		// ── git ──────────────────────────────────────────────────────────────
		if (!s_last.gitFound)
		{
			ImGui::BulletText("git was not found.");
			ImGui::Indent();
			drawGitInstallRemedy();
			ImGui::Unindent();
			ImGui::Spacing();
		}
		else
		{
			// Bullet + Text, not BulletText: BulletText measures with plain
			// CalcTextSize and draws through RenderText, and neither of those
			// consults a wrap position — so this line, the one carrying an
			// absolute path, would have been the one line in the dialog still
			// running off the edge. Same strings, same order, drawn through the
			// path that wraps. The same is true of the two bullets below that
			// carry a version or an address; the short fixed ones can stay.
			//
			// No SameLine of our own: Bullet() ends by keeping the cursor on the
			// line, and at exactly the gap BulletText used — so the line lands
			// where it always did.
			ImGui::Bullet();
			ImGui::Text("git %s found%s.",
			            s_last.gitVersion.empty() ? "(unknown version)" : s_last.gitVersion.c_str(),
			            s_last.gitPath.empty() ? "" : (" at " + s_last.gitPath.string()).c_str());
		}

		// ── git-lfs ──────────────────────────────────────────────────────────
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
			drawLfsInstallRemedy();
			ImGui::Unindent();
			ImGui::Spacing();
		}
		else if (s_last.lfsFound)
		{
			ImGui::Bullet();
			ImGui::Text("Git LFS %s found.", s_last.lfsVersion.c_str());
		}

		// ── identity ─────────────────────────────────────────────────────────
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
			if (EditorWidgets::primaryButton("Save Identity") && ctx.setGitIdentity)
				ctx.setGitIdentity(s_name, s_email);
			if (!usable) ImGui::EndDisabled();
			if (applying) ImGui::EndDisabled();

			if (applying) { ImGui::SameLine(); ImGui::TextDisabled("Saving…"); }
			ImGui::Unindent();
			ImGui::Spacing();
		}
		else if (s_last.identityConfigured)
		{
			ImGui::Bullet();
			ImGui::Text("Identity: %s <%s>", s_last.userName.c_str(), s_last.userEmail.c_str());
		}

		// ── credential helper ────────────────────────────────────────────────
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
		EditorWidgets::checkbox("Don't show this again", &s_dontShowAgain);
		ImGui::Spacing();

		if (s_checking) ImGui::BeginDisabled();
		if (EditorWidgets::button("Recheck") && ctx.recheckGit) ctx.recheckGit();
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
	}

	ImGui::EndPopup();
#else
	(void)ctx;
#endif
}

} // namespace GitMissingDialog
