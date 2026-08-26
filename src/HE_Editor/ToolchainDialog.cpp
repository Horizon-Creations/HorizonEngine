#include "ToolchainDialog.h"
#include "EditorApplication.h"           // AppContext
#include "EditorHelp.h"                  // "Build Tools/<label>" scope for the tooltips
#include "EditorWidgets.h"               // pinDialogToEditorWindow
#include <HorizonScene/HcCodegen.h>      // HE::hccg::ToolchainProbe
#include <SDL3/SDL.h>
#include <string>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace ToolchainDialog
{

// Set by the Preferences "Recheck" button to force the toolchain dialog open
// even when the persistent "don't show again" suppression is set — an
// explicit user request should always show the result.
static bool s_forceShowToolchainDialog = false;

void requestShow() { s_forceShowToolchainDialog = true; }

// Startup toolchain check (cmake + a working C++ compiler, see HcCodegen). The
// probe runs once on a background thread (EditorApplication::startToolchainProbe);
// this only reacts to its result. Self-triggering (no `open` flag like the other
// dialogs here) — it watches ctx.toolchainProbe transition from null (checking)
// to non-null (done) and opens itself the first time that happens, unless the
// user has permanently suppressed it (Preferences > Tools > Status > "Fix"
// bypasses the suppression, since that's an explicit request to see the result).
void DrawToolchainDialog(AppContext& ctx)
{
	static bool s_awaitingFirstResult = true; // the one automatic open, at the first result
	static bool s_dontShowAgain       = false;
	static bool s_checking            = false; // a Recheck is in flight — keep showing the last result
	static HE::hccg::ToolchainProbe s_last;
	static bool s_haveLast = false;
	static bool s_installTriggered = false; // an auto-install has been started at least once
	static bool s_installConsumed  = false; // the last install's "done" transition was handled

	if (ctx.toolchainProbe)
	{
		s_checking = false;
		s_last     = *ctx.toolchainProbe;
		s_haveLast = true;
		// Two ways in: the session's first completed probe (unless the user
		// suppressed the warning), and an explicit request from elsewhere in the
		// editor. The second one has to work whenever it is made — the first
		// result is long past by then, so it cannot ride on that transition.
		const bool firstResult = s_awaitingFirstResult;
		s_awaitingFirstResult = false;
		if (firstResult || s_forceShowToolchainDialog)
		{
			const bool missing = !s_last.cmakeFound || !s_last.compilerFound;
			const bool suppressed = ctx.globalState &&
				ctx.globalState->getCustomConfigBool("SuppressToolchainWarning", false);
			if (missing && (s_forceShowToolchainDialog || !suppressed))
			{
				s_dontShowAgain = false;
				ImGui::OpenPopup("##ToolchainMissing");
			}
			s_forceShowToolchainDialog = false;
		}
	}
	else if (s_haveLast)
	{
		s_checking = true; // Recheck in flight; the popup (if open) stays up on stale data
	}

	if (!s_haveLast) return;

	ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
	EditorWidgets::pinDialogToEditorWindow();
	if (ImGui::BeginPopupModal("##ToolchainMissing", nullptr,
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
	{
		// "Build Tools/<label>" for everything in this dialog. Its own scope and
		// not Preferences': the "Recheck" here and the "Recheck" on the settings
		// page are the same word for the same probe, but this one is the last
		// thing standing between the user and a build that works, so it gets to
		// say so.
		HE::Ed::Help::Scope helpScope("Build Tools");

		const bool missing = !s_last.cmakeFound || !s_last.compilerFound;
		if (!missing)
		{
			// Resolved (Recheck came back clean) — nothing left to show.
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}

		// The column the whole dialog wraps at. Absolute, not the window edge: this
		// popup is AlwaysAutoResize and its SetNextWindowSize is only
		// ImGuiCond_Appearing, so from the second frame its width follows its
		// content. Wrapping at that width refits the text to the window and the
		// window to the text, a little narrower every frame, while the user is
		// trying to read it. The 520 this dialog is laid out for puts its content
		// edge at 510.
		//
		// Applied to the two paragraphs below and not once at the top of this body
		// because EndPopup() is the LAST STATEMENT INSIDE this body: a guard
		// declared here would be destroyed after it, and pop the wrap position off
		// whatever window is current by then. Each block below closes first.
		//
		// These two paragraphs are what actually drive the width. The probe detail
		// has its own guard, but it sits inside a TreeNode that is collapsed by
		// default, so on its own it never gets the chance to matter.
		constexpr float kContentW = 510.0f;

		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
		ImGui::TextUnformatted("C++ Toolchain Not Found");
		ImGui::PopStyleColor();
		ImGui::Separator();
		ImGui::Spacing();

		{
			EditorWidgets::WrapText wrap(kContentW);
			ImGui::TextWrapped(
				"HorizonCode C++ export codegen and native C++ GameLogic projects need "
				"cmake and a C++ compiler on this machine. Without them, those features "
				"fall back to interpreted execution or are unavailable.");
		}
		ImGui::Spacing();

		if (!s_last.cmakeFound)
		{
			ImGui::BulletText("cmake was not found on PATH.");
		}
		else
		{
			ImGui::BulletText("cmake %s found.", s_last.cmakeVersion.c_str());
			ImGui::BulletText("No working C++ compiler was detected.");
			if (!s_last.detail.empty() && ImGui::TreeNode("Details"))
			{
				// The raw probe output — compiler paths, the xcrun error that
				// explains WHY no compiler was found. Long single lines, and this
				// is the one string the user opened a disclosure triangle
				// specifically to read, so it must not run off the right edge.
				// Same column as the paragraphs above, for the same reason.
				EditorWidgets::WrapText wrap(kContentW);
				ImGui::TextUnformatted(s_last.detail.c_str());
				ImGui::TreePop();
			}
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		const bool needCmake    = !s_last.cmakeFound;
		const bool needCompiler = !s_last.compilerFound;
		const bool installing   = ctx.toolchainInstalling;

		// ── Auto-install (primary path) ──────────────────────────────────────
		{
			EditorWidgets::WrapText wrap(kContentW);
			ImGui::TextWrapped(
				"The engine can install these for you using this system's package manager "
#if defined(__APPLE__)
				"(Homebrew for cmake — installed for you in Terminal if missing — and the "
				"Xcode Command Line Tools for the compiler). "
#elif defined(_WIN32)
				"(winget: CMake + the Visual Studio C++ Build Tools). "
#else
				"(pkexec + apt/dnf/pacman). You may be prompted for your password. "
#endif
				"A download of several hundred MB can take a few minutes.");
		}
		ImGui::Spacing();

		if (installing) ImGui::BeginDisabled();
		if (EditorWidgets::primaryButton("Install Automatically") && ctx.startToolchainInstall)
		{
			s_installTriggered = true;
			s_installConsumed  = false;
			ctx.startToolchainInstall(needCmake, needCompiler);
		}
		if (installing) ImGui::EndDisabled();
		ImGui::SameLine();
		if (installing)
			ImGui::TextDisabled("Installing… (progress below)");
		else
			ImGui::TextDisabled("or do it yourself:");

		// Manual fallbacks — copy the command / open the download page.
#if defined(__APPLE__)
		if (EditorWidgets::smallButton("Copy 'brew install cmake'")) ImGui::SetClipboardText("brew install cmake");
		ImGui::SameLine();
		if (EditorWidgets::smallButton("cmake.org")) SDL_OpenURL("https://cmake.org/download/");
#elif defined(_WIN32)
		static const char* kWinCmd =
			"winget install --id Kitware.CMake -e ; "
			"winget install --id Microsoft.VisualStudio.2022.BuildTools -e "
			"--override \"--add Microsoft.VisualStudio.Workload.VCTools --quiet\"";
		if (EditorWidgets::smallButton("Copy winget Command")) ImGui::SetClipboardText(kWinCmd);
		ImGui::SameLine();
		if (EditorWidgets::smallButton("Download Page"))
			SDL_OpenURL("https://visualstudio.microsoft.com/visual-cpp-build-tools/");
#else
		if (EditorWidgets::smallButton("Copy 'sudo apt install build-essential cmake'"))
			ImGui::SetClipboardText("sudo apt install build-essential cmake");
		ImGui::SameLine();
		if (EditorWidgets::smallButton("cmake.org")) SDL_OpenURL("https://cmake.org/download/");
#endif

		// ── Live progress log ────────────────────────────────────────────────
		if (s_installTriggered && ctx.toolchainInstallLog)
		{
			const std::string log = ctx.toolchainInstallLog();
			if (!log.empty())
			{
				ImGui::Spacing();
				ImGui::BeginChild("##installlog", ImVec2(0.0f, 150.0f), true);
				// Wrapped, not scrolled sideways. Homebrew and the platform
				// installers emit long single-line messages, and the sentence that
				// says WHY an install stopped is at the end of one of them — off
				// the right edge, behind a scrollbar, in the dialog the user is
				// stuck in.
				ImGui::PushTextWrapPos(0.0f);
				ImGui::TextUnformatted(log.c_str());
				ImGui::PopTextWrapPos();
				if (installing) ImGui::SetScrollHereY(1.0f); // autoscroll while running
				ImGui::EndChild();
			}
			if (!installing && ctx.toolchainInstallDone)
				ImGui::TextDisabled(ctx.toolchainInstallOk
					? "Install finished — rechecking…"
					: "Install did not complete — see the log above, then retry or install manually.");
		}

		// Re-run the probe once when an install finishes; a clean result auto-closes
		// this dialog (see the !missing branch at the top).
		if (s_installTriggered && ctx.toolchainInstallDone && !installing && !s_installConsumed)
		{
			s_installConsumed = true;
			if (ctx.recheckToolchain) ctx.recheckToolchain();
		}

		ImGui::Spacing();
		ImGui::Separator();
		EditorWidgets::checkbox("Don't show this again", &s_dontShowAgain);
		ImGui::Spacing();

		if (s_checking) ImGui::BeginDisabled();
		if (EditorWidgets::button("Recheck") && ctx.recheckToolchain)
			ctx.recheckToolchain();
		if (s_checking) ImGui::EndDisabled();
		ImGui::SameLine();
		if (s_checking) ImGui::TextDisabled("Rechecking...");
		else
		{
			if (ImGui::Button("Close"))
			{
				if (s_dontShowAgain && ctx.globalState)
				{
					ctx.globalState->setCustomConfigEntry("SuppressToolchainWarning", true);
					ctx.globalState->writeConfig();
				}
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::EndPopup();
	}
}

} // namespace ToolchainDialog
