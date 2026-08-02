#include "ToolchainDialog.h"
#include "EditorApplication.h"           // AppContext
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
// user has permanently suppressed it (Preferences > C++ Toolchain > Recheck
// bypasses the suppression, since that's an explicit request to see the result).
void DrawToolchainDialog(AppContext& ctx)
{
	static bool s_awaitingFirstResult = true; // consume exactly one auto-open per completed probe
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
		if (s_awaitingFirstResult)
		{
			s_awaitingFirstResult = false;
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
		const bool missing = !s_last.cmakeFound || !s_last.compilerFound;
		if (!missing)
		{
			// Resolved (Recheck came back clean) — nothing left to show.
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}

		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
		ImGui::TextUnformatted("C++ Toolchain Not Found");
		ImGui::PopStyleColor();
		ImGui::Separator();
		ImGui::Spacing();

		ImGui::TextWrapped(
			"HorizonCode C++ export codegen and native C++ GameLogic projects need "
			"cmake and a C++ compiler on this machine. Without them, those features "
			"fall back to interpreted execution or are unavailable.");
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
		ImGui::Spacing();

		if (installing) ImGui::BeginDisabled();
		if (ImGui::Button("Install Automatically") && ctx.startToolchainInstall)
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
		if (ImGui::SmallButton("Copy 'brew install cmake'")) ImGui::SetClipboardText("brew install cmake");
		ImGui::SameLine();
		if (ImGui::SmallButton("cmake.org")) SDL_OpenURL("https://cmake.org/download/");
#elif defined(_WIN32)
		static const char* kWinCmd =
			"winget install --id Kitware.CMake -e ; "
			"winget install --id Microsoft.VisualStudio.2022.BuildTools -e "
			"--override \"--add Microsoft.VisualStudio.Workload.VCTools --quiet\"";
		if (ImGui::SmallButton("Copy winget Command")) ImGui::SetClipboardText(kWinCmd);
		ImGui::SameLine();
		if (ImGui::SmallButton("Download Page"))
			SDL_OpenURL("https://visualstudio.microsoft.com/visual-cpp-build-tools/");
#else
		if (ImGui::SmallButton("Copy 'sudo apt install build-essential cmake'"))
			ImGui::SetClipboardText("sudo apt install build-essential cmake");
		ImGui::SameLine();
		if (ImGui::SmallButton("cmake.org")) SDL_OpenURL("https://cmake.org/download/");
#endif

		// ── Live progress log ────────────────────────────────────────────────
		if (s_installTriggered && ctx.toolchainInstallLog)
		{
			const std::string log = ctx.toolchainInstallLog();
			if (!log.empty())
			{
				ImGui::Spacing();
				ImGui::BeginChild("##installlog", ImVec2(0.0f, 150.0f), true,
					ImGuiWindowFlags_HorizontalScrollbar);
				ImGui::TextUnformatted(log.c_str());
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
		ImGui::Checkbox("Don't show this again", &s_dontShowAgain);
		ImGui::Spacing();

		if (s_checking) ImGui::BeginDisabled();
		if (ImGui::Button("Recheck") && ctx.recheckToolchain)
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
