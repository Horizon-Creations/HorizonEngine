#include "ToolSetupDialog.h"
#include "EditorApplication.h"           // AppContext
#include "EditorHelp.h"                  // "Tool Setup" scope for the tooltips
#include "EditorTheme.h"
#include "EditorWidgets.h"               // pinDialogToEditorWindow, buttons, WrapText

#include <HorizonScene/HcCodegen.h>      // HE::hccg::ToolchainProbe
#include <SourceControl/GitInstall.h>    // gitInstallLaunch, the per-platform route
#include <SourceControl/GitProbe.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#if __has_include(<imgui.h>)
#include <imgui.h>
#endif

namespace ToolSetupDialog
{
namespace
{

using Clock = std::chrono::steady_clock;
namespace Theme = HE::Ed::Theme;

// The width the dialog is laid out for. The table below is fixed-width columns that
// add up to a little less, so the AlwaysAutoResize window has nothing to chase.
constexpr float kDialogWidth = 620.0f;
constexpr float kColIcon     = 28.0f;
constexpr float kColBody     = 394.0f;
constexpr float kColAction   = 148.0f;
constexpr float kContentW    = kColIcon + kColBody + kColAction;

constexpr unsigned kBitCmake    = 1u;
constexpr unsigned kBitCompiler = 2u;

// ── What the dialog remembers between frames ─────────────────────────────────
// Probes are kept as copies, like the two dialogs before it did: the pointer on
// AppContext goes null while a recheck runs, and the rows should keep showing the
// last answer instead of blanking.
HE::hccg::ToolchainProbe s_tc;
bool s_haveTc     = false;
bool s_checkingTc = false;
HE::Sc::GitProbe s_git;
bool s_haveGit     = false;
bool s_checkingGit = false;

// One slot per installer worker: when it started (for the clock on the row) and
// whether it was running last frame (the edge that resets the failure state).
struct Slot { bool running = false; Clock::time_point start{}; };
Slot s_tcSlot;
Slot s_gitSlot;

unsigned s_tcRequested = 0;     // which toolchain rows the current/last toolchain install is for
bool s_tcSeen          = false; // a toolchain install ran in this session
bool s_tcConsumed      = false; // the re-probe after it was triggered
bool s_ackTc           = false; // a failure was acknowledged (Retry / Recheck / Install All)
bool s_ackGit          = false;
bool s_logIsGit        = false; // which installer's log the "Install log" fold shows
bool s_openLog         = false; // unfold the log next frame ("Show Log" on a failure)

std::vector<Tool> s_queue;      // "Install All Missing", worked off one installer at a time

bool s_openRequested = false;
bool s_autoOpened    = false;   // the one automatic open per session
bool s_dontShowAgain = false;
bool s_sawMissing    = false;   // something was missing while the dialog was open
Tool   s_flashTool   = Tool::None;
double s_flashUntil  = 0.0;
bool   s_flashArmed  = false;   // set by requestShow, started once the dialog draws
std::string s_purpose;          // requestShowFor: who opened it and what waits
bool   s_showing     = false;   // the popup was drawn in the last DrawToolSetupDialog

// Pre-filled from whatever git already has, so a user with only one of the two set
// does not have to retype the other.
char s_name[128]  = {};
char s_email[128] = {};
bool s_identitySeeded = false;

// ── Small helpers ────────────────────────────────────────────────────────────

std::string lastLine(const std::string& text)
{
	std::size_t end = text.size();
	while (end > 0 && (text[end - 1] == '\n' || text[end - 1] == '\r' || text[end - 1] == ' ')) --end;
	if (end == 0) return {};
	const std::size_t nl = text.find_last_of('\n', end - 1);
	return text.substr(nl == std::string::npos ? 0 : nl + 1, end - (nl == std::string::npos ? 0 : nl + 1));
}

std::string clockText(const Slot& s)
{
	const long secs = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - s.start).count();
	char buf[24];
	std::snprintf(buf, sizeof(buf), "%ld:%02ld", secs / 60, secs % 60);
	return buf;
}

// "2:05" from milliseconds.
std::string mmss(long long ms)
{
	const long secs = static_cast<long>(std::max(0LL, ms) / 1000);
	char buf[24];
	std::snprintf(buf, sizeof(buf), "%ld:%02ld", secs / 60, secs % 60);
	return buf;
}

long long steadyNowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Past this long without output or a change in the installer's processes the row
// says so, and asks whether to keep waiting.
constexpr long long kStallWarnMs = 90 * 1000;

bool isToolchainTool(Tool t) { return t == Tool::Cmake || t == Tool::Compiler; }
bool isGitInstallTool(Tool t) { return t == Tool::Git || t == Tool::GitLfs; }

const char* toolName(Tool t)
{
	switch (t)
	{
	case Tool::Cmake:       return "cmake";
	case Tool::Compiler:    return "C++ compiler";
	case Tool::Git:         return "git";
	case Tool::GitLfs:      return "Git LFS";
	case Tool::GitIdentity: return "Git identity";
	case Tool::None:        break;
	}
	return "";
}

const char* toolPurpose(Tool t)
{
	switch (t)
	{
	case Tool::Cmake:       return "Builds every exported game, HorizonCode and C++ GameLogic.";
	case Tool::Compiler:    return "Compiles every exported game, HorizonCode and native GameLogic.";
	case Tool::Git:         return "Source control and cloning.";
	case Tool::GitLfs:      return "Keeps meshes, textures and audio out of the repository's own history.";
	case Tool::GitIdentity: return "git records who made each change and refuses to commit without it.";
	case Tool::None:        break;
	}
	return "";
}

HE::Sc::GitInstallTarget gitTarget(Tool t)
{
	return t == Tool::GitLfs ? HE::Sc::GitInstallTarget::GitLfs : HE::Sc::GitInstallTarget::Git;
}

// Can the editor run an install for this tool by itself? When not, the row says why
// and the manual way below carries the rest.
bool canInstall(const AppContext& ctx, Tool t)
{
	if (isToolchainTool(t)) return static_cast<bool>(ctx.startToolchainInstall);
	if (t == Tool::Git)     return ctx.startGitInstall    && HE::Sc::gitInstallLaunch(s_git.install, gitTarget(t)).has_value();
	if (t == Tool::GitLfs)  return ctx.startGitLfsInstall && HE::Sc::gitInstallLaunch(s_git.install, gitTarget(t)).has_value();
	return false;
}

// The package manager the install goes through, and what the user will see while it
// runs. One line each; both are shown on the row before anything is pressed.
std::string sourceLine(Tool t)
{
	if (t == Tool::Cmake)
	{
#if defined(_WIN32)
		return "winget: Kitware.CMake";
#elif defined(__APPLE__)
		return "Homebrew: cmake";
#else
		return "Your package manager: cmake";
#endif
	}
	if (t == Tool::Compiler)
	{
#if defined(_WIN32)
		return "winget: Visual Studio Build Tools, C++ workload (a download of a few GB)";
#elif defined(__APPLE__)
		return "Apple's Command Line Tools (a download of several hundred MB)";
#else
		return "Your package manager: build tools (g++, make)";
#endif
	}
	const auto launch = HE::Sc::gitInstallLaunch(s_git.install, gitTarget(t));
	if (launch)
		return std::string(HE::Sc::gitInstallRouteName(s_git.install.route)) + ": " +
		       (t == Tool::GitLfs ? "git-lfs" : "git");
	return {};
}

std::string promptLine(Tool t)
{
	if (isToolchainTool(t))
	{
#if defined(_WIN32)
		return "Windows asks for permission (UAC).";
#elif defined(__APPLE__)
		return t == Tool::Compiler ? "Apple's installer opens in its own window."
		                           : "If Homebrew is missing, Terminal opens to install it.";
#else
		return "A system dialog asks for an administrator password.";
#endif
	}
	const auto launch = HE::Sc::gitInstallLaunch(s_git.install, gitTarget(t));
	return launch ? launch->userPrompt : std::string();
}

struct Manual { std::string command; std::string url; };

Manual manualFor(Tool t)
{
	Manual m;
	switch (t)
	{
	case Tool::Cmake:
#if defined(_WIN32)
		m.command = "winget install --id Kitware.CMake -e";
#elif defined(__APPLE__)
		m.command = "brew install cmake";
#else
		m.command = "sudo apt install cmake";
#endif
		m.url = "https://cmake.org/download/";
		break;
	case Tool::Compiler:
#if defined(_WIN32)
		m.command = "winget install --id Microsoft.VisualStudio.2022.BuildTools -e --force "
		            "--override \"--add Microsoft.VisualStudio.Workload.VCTools --quiet\"";
		m.url = "https://visualstudio.microsoft.com/visual-cpp-build-tools/";
#elif defined(__APPLE__)
		m.command = "xcode-select --install";
#else
		m.command = "sudo apt install build-essential";
#endif
		break;
	case Tool::Git:
		m.command = s_git.install.manualCommand;
		m.url     = HE::Sc::gitDownloadPageUrl(HE::Sc::currentPlatform());
		break;
	case Tool::GitLfs:
		m.command = s_git.install.lfsManualCommand;
		m.url     = "https://git-lfs.com/";
		break;
	default:
		break;
	}
	return m;
}

// ── Probe cache and install edges ────────────────────────────────────────────

void refresh(AppContext& ctx)
{
	if (ctx.toolchainProbe) { s_tc = *ctx.toolchainProbe; s_haveTc = true; s_checkingTc = false; }
	else if (s_haveTc)      { s_checkingTc = true; }

	if (ctx.gitProbe)
	{
		s_git = *ctx.gitProbe; s_haveGit = true; s_checkingGit = false;
		if (!s_identitySeeded)
		{
			s_identitySeeded = true;
			std::snprintf(s_name,  sizeof(s_name),  "%s", s_git.userName.c_str());
			std::snprintf(s_email, sizeof(s_email), "%s", s_git.userEmail.c_str());
		}
	}
	else if (s_haveGit) { s_checkingGit = true; }

	// A new install resets what the last one left: its failure is no longer news.
	if (ctx.toolchainInstalling && !s_tcSlot.running)
	{
		s_tcSlot.start = Clock::now();
		s_ackTc = false; s_tcSeen = true; s_tcConsumed = false; s_logIsGit = false;
	}
	s_tcSlot.running = ctx.toolchainInstalling;
	if (ctx.gitInstalling && !s_gitSlot.running)
	{
		s_gitSlot.start = Clock::now();
		s_ackGit = false; s_logIsGit = true;
	}
	s_gitSlot.running = ctx.gitInstalling;

	// The toolchain install does not re-probe by itself (the git one does): do it
	// once when it ends, so the rows turn green without a click.
	if (s_tcSeen && ctx.toolchainInstallDone && !ctx.toolchainInstalling && !s_tcConsumed)
	{
		s_tcConsumed = true;
		if (ctx.recheckToolchain) ctx.recheckToolchain();
	}
}

bool toolchainFound(Tool t)
{
	return t == Tool::Cmake ? s_tc.cmakeFound : s_tc.compilerFound;
}

bool installRequested(Tool t)
{
	return (s_tcRequested & (t == Tool::Cmake ? kBitCmake : kBitCompiler)) != 0;
}

} // namespace

RowState rowState(AppContext& ctx, Tool t)
{
	refresh(ctx);

	if (isToolchainTool(t))
	{
		if (!s_haveTc) return RowState::Checking;
		if (toolchainFound(t)) return RowState::Ok;
		if (ctx.toolchainInstalling && installRequested(t)) return RowState::Installing;
		if (s_checkingTc) return RowState::Checking;
		if (ctx.toolchainInstallDone && !ctx.toolchainInstallOk && installRequested(t) && !s_ackTc)
			return RowState::Failed;
		return RowState::Missing;
	}

	if (!s_haveGit) return RowState::Checking;
	switch (t)
	{
	case Tool::Git:
		if (s_git.gitFound) return RowState::Ok;
		if (ctx.gitInstalling && !ctx.gitInstallIsLfs) return RowState::Installing;
		if (s_checkingGit) return RowState::Checking;
		if (ctx.gitInstallDone && !ctx.gitInstallOk && !ctx.gitInstallIsLfs && !s_ackGit)
			return RowState::Failed;
		return RowState::Missing;
	case Tool::GitLfs:
		if (!s_git.gitFound) return s_checkingGit ? RowState::Checking : RowState::Waiting;
		if (s_git.lfsFound) return RowState::Ok;
		if (ctx.gitInstalling && ctx.gitInstallIsLfs) return RowState::Installing;
		if (s_checkingGit) return RowState::Checking;
		if (ctx.gitInstallDone && !ctx.gitInstallOk && ctx.gitInstallIsLfs && !s_ackGit)
			return RowState::Failed;
		return RowState::Missing;
	case Tool::GitIdentity:
		if (!s_git.gitFound) return s_checkingGit ? RowState::Checking : RowState::Waiting;
		return s_git.identityConfigured ? RowState::Ok : RowState::Missing;
	default:
		break;
	}
	return RowState::Ok;
}

namespace
{

constexpr Tool kRows[] = { Tool::Cmake, Tool::Compiler, Tool::Git, Tool::GitLfs, Tool::GitIdentity };

bool anyBusy(const AppContext& ctx) { return ctx.toolchainInstalling || ctx.gitInstalling; }

bool everythingSetUp(AppContext& ctx)
{
	for (Tool t : kRows)
		if (rowState(ctx, t) != RowState::Ok) return false;
	return true;
}

// ── Starting installs ────────────────────────────────────────────────────────

void startOne(AppContext& ctx, Tool t, bool alsoOther = false)
{
	if (isToolchainTool(t))
	{
		const bool needCmake    = t == Tool::Cmake    || (alsoOther && !s_tc.cmakeFound);
		const bool needCompiler = t == Tool::Compiler || (alsoOther && !s_tc.compilerFound);
		s_tcRequested = (needCmake ? kBitCmake : 0u) | (needCompiler ? kBitCompiler : 0u);
		s_ackTc = false; s_tcSeen = true; s_tcConsumed = false; s_logIsGit = false;
		s_tcSlot.start = Clock::now();
		if (ctx.startToolchainInstall) ctx.startToolchainInstall(needCmake, needCompiler);
	}
	else if (t == Tool::Git && ctx.startGitInstall)
	{
		s_ackGit = false; s_logIsGit = true; s_gitSlot.start = Clock::now();
		ctx.startGitInstall();
	}
	else if (t == Tool::GitLfs && ctx.startGitLfsInstall)
	{
		s_ackGit = false; s_logIsGit = true; s_gitSlot.start = Clock::now();
		ctx.startGitLfsInstall();
	}
}

// Works the queue off one installer at a time: the next one starts when nothing is
// running and both probes have answered again (git's installer re-probes itself, the
// toolchain's is re-probed in refresh()). A failure empties the queue, so a broken
// first step does not drag a password prompt for every later one.
void pumpQueue(AppContext& ctx)
{
	if (s_queue.empty()) return;
	if (anyBusy(ctx) || !ctx.toolchainProbe || !ctx.gitProbe) return;

	for (Tool t : kRows)
		if (rowState(ctx, t) == RowState::Failed) { s_queue.clear(); return; }

	while (!s_queue.empty())
	{
		const Tool t = s_queue.front();
		s_queue.erase(s_queue.begin());
		if (rowState(ctx, t) != RowState::Missing) continue;   // came along with an earlier step

		bool both = false;
		if (isToolchainTool(t))
		{
			// cmake and the compiler in one call: one password prompt, one log.
			const Tool other = t == Tool::Cmake ? Tool::Compiler : Tool::Cmake;
			const auto it = std::find(s_queue.begin(), s_queue.end(), other);
			if (it != s_queue.end())
			{
				both = rowState(ctx, other) == RowState::Missing;
				s_queue.erase(it);
			}
		}
		startOne(ctx, t, both);
		return;
	}
}

void queueAll(AppContext& ctx)
{
	s_queue.clear();
	s_ackTc = s_ackGit = true;   // a failure from before is retried, not blocking
	for (Tool t : { Tool::Cmake, Tool::Compiler, Tool::Git, Tool::GitLfs })
	{
		const RowState st = rowState(ctx, t);
		if ((st == RowState::Missing || st == RowState::Failed) && canInstall(ctx, t)) s_queue.push_back(t);
		// LFS waits on git, but git is in the queue ahead of it and may bring it along.
		else if (t == Tool::GitLfs && st == RowState::Waiting && canInstall(ctx, t) &&
		         std::find(s_queue.begin(), s_queue.end(), Tool::Git) != s_queue.end())
			s_queue.push_back(t);
	}
}

int installableCount(AppContext& ctx)
{
	int n = 0;
	for (Tool t : { Tool::Cmake, Tool::Compiler, Tool::Git, Tool::GitLfs })
	{
		const RowState st = rowState(ctx, t);
		if ((st == RowState::Missing || st == RowState::Failed) && canInstall(ctx, t)) ++n;
	}
	return n;
}

// ── Drawing ──────────────────────────────────────────────────────────────────

// The ground under the row being drawn: the popup, or the popup lifted while the
// row is the one a banner sent the user to. Every coloured line in a row goes
// through rowText(), so it clears the theme's contrast floor on either.
ImVec4 s_rowGround   = Theme::warm(0.13f);
float  s_flashRowH   = 0.0f;   // the flashed row's height, as last drawn

ImVec4 rowText(const ImVec4& c) { return Theme::readableOn(c, s_rowGround); }

void dimWrapped(const char* text)
{
	ImGui::PushStyleColor(ImGuiCol_Text, rowText(Theme::TextDim));
	ImGui::TextWrapped("%s", text);
	ImGui::PopStyleColor();
}

// Drawn, not typed: a check mark or a cross is not in every font the editor loads.
void drawStateIcon(RowState st)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 p = ImGui::GetCursorScreenPos();
	constexpr float r = 8.0f;
	const ImVec2 c(p.x + r + 2.0f, p.y + r + 3.0f);
	const auto col = [](const ImVec4& v) { return ImGui::ColorConvertFloat4ToU32(v); };
	const auto at  = [&](float dx, float dy) { return ImVec2(c.x + dx, c.y + dy); };

	switch (st)
	{
	case RowState::Ok:
		dl->AddCircleFilled(c, r, col(Theme::alpha(Theme::Ok, 0.22f)));
		dl->AddCircle(c, r, col(Theme::Ok), 0, 1.5f);
		dl->PathLineTo(at(-3.8f, 0.2f)); dl->PathLineTo(at(-1.0f, 3.2f)); dl->PathLineTo(at(4.2f, -3.2f));
		dl->PathStroke(col(Theme::Ok), 0, 2.0f);
		break;
	case RowState::Missing:
		dl->AddCircleFilled(c, r, col(Theme::alpha(Theme::Bad, 0.22f)));
		dl->AddCircle(c, r, col(Theme::Bad), 0, 1.5f);
		dl->AddLine(at(-3.2f, -3.2f), at(3.2f, 3.2f), col(Theme::Bad), 2.0f);
		dl->AddLine(at(-3.2f, 3.2f), at(3.2f, -3.2f), col(Theme::Bad), 2.0f);
		break;
	case RowState::Failed:
		dl->AddCircleFilled(c, r, col(Theme::alpha(Theme::Warn, 0.22f)));
		dl->AddCircle(c, r, col(Theme::Warn), 0, 1.5f);
		dl->AddLine(at(0.0f, -4.5f), at(0.0f, 1.0f), col(Theme::Warn), 2.0f);
		dl->AddCircleFilled(at(0.0f, 4.2f), 1.3f, col(Theme::Warn));
		break;
	case RowState::Waiting:
		dl->AddCircle(c, r, col(Theme::TextDim), 0, 1.2f);
		dl->AddLine(at(-3.5f, 0.0f), at(3.5f, 0.0f), col(Theme::TextDim), 1.6f);
		break;
	case RowState::Checking:
	case RowState::Installing:
	{
		const float a0 = static_cast<float>(ImGui::GetTime()) * 5.0f;
		dl->AddCircle(c, r - 1.0f, col(Theme::alpha(Theme::TextDim, 0.35f)), 0, 2.0f);
		dl->PathArcTo(c, r - 1.0f, a0, a0 + 4.3f, 24);
		dl->PathStroke(col(st == RowState::Installing ? Theme::AccentBright : Theme::TextDim), 0, 2.5f);
		break;
	}
	}
	ImGui::Dummy(ImVec2(2.0f * r + 4.0f, 2.0f * r + 4.0f));
}

// Preparing > <manager> > Verifying, the current phase lit, the finished ones green.
void drawPhases(int current, const char* middle)
{
	const char* labels[3] = { "Preparing", middle, "Verifying" };
	for (int i = 0; i < 3; ++i)
	{
		if (i > 0) { ImGui::SameLine(0.0f, 6.0f); ImGui::TextDisabled(">"); ImGui::SameLine(0.0f, 6.0f); }
		const ImVec4 c = rowText(i < current ? Theme::Ok : (i == current ? Theme::Text : Theme::TextDim));
		ImGui::TextColored(c, "%s", labels[i]);
	}
}

std::string okDetail(Tool t)
{
	switch (t)
	{
	case Tool::Cmake:
		return s_tc.cmakeVersion.empty() ? std::string("Found.") : "cmake " + s_tc.cmakeVersion;
	case Tool::Compiler:
		return s_tc.compilerId.empty() ? std::string("Found.") : s_tc.compilerId;
	case Tool::Git:
		return "git " + (s_git.gitVersion.empty() ? std::string("(unknown version)") : s_git.gitVersion) +
		       (s_git.gitPath.empty() ? std::string() : " at " + s_git.gitPath.string());
	case Tool::GitLfs:
		return "Git LFS " + s_git.lfsVersion;
	case Tool::GitIdentity:
		return s_git.userName + " <" + s_git.userEmail + ">";
	case Tool::None:
		break;
	}
	return {};
}

void drawIdentityForm(AppContext& ctx)
{
	// The same scope the Preferences page pushes: "Save Identity" is one entry there
	// and here, not two that drift apart.
	HE::Ed::Help::Scope helpScope("Source Control");

	const bool applying = ctx.gitIdentityApplying;
	if (applying) ImGui::BeginDisabled();
	ImGui::SetNextItemWidth(186.0f);
	ImGui::InputTextWithHint("##tsname", "Your Name", s_name, sizeof(s_name));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(186.0f);
	ImGui::InputTextWithHint("##tsemail", "you@example.com", s_email, sizeof(s_email));

	const bool usable = s_name[0] != '\0' && s_email[0] != '\0';
	if (!usable) ImGui::BeginDisabled();
	if (EditorWidgets::primaryButton("Save Identity") && ctx.setGitIdentity)
		ctx.setGitIdentity(s_name, s_email);
	if (!usable) ImGui::EndDisabled();
	if (applying) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextDisabled("Saving…"); }
}

void drawRow(AppContext& ctx, Tool t)
{
	HE::Ed::Help::Scope helpScope("Tool Setup");

	const RowState st = rowState(ctx, t);
	const bool busy   = anyBusy(ctx);
	const bool gitSlot = isGitInstallTool(t);
	const Slot& slot   = gitSlot ? s_gitSlot : s_tcSlot;

	ImGui::TableNextRow();
	// The row a banner sent the user to is marked for 1.6 s. The mark used to be
	// the accent at 35 % across the whole row, which put this row's amber Warn
	// line on an amber-brown ground (4.7:1, and reported as unreadable) and its
	// dim lines at 1.9:1. The accent now goes into a bar at the row's edge, where
	// no text is; the row itself only lifts a little, and its text is held to the
	// floor on that lift by rowText().
	// The popup's ground, not the host window's: the dialog is a modal popup, and
	// PopupBg is the lighter of the two, so text that clears it clears both.
	s_rowGround = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
	const bool flashing = t == s_flashTool && ImGui::GetTime() < s_flashUntil;
	const float k = flashing ? static_cast<float>((s_flashUntil - ImGui::GetTime()) / 1.6) : 0.0f;
	if (flashing)
	{
		const ImVec4 lift = Theme::alpha(Theme::Ivory, 0.08f * k);
		ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, Theme::u32(lift));
		s_rowGround = Theme::over(lift, s_rowGround);
	}
	ImGui::PushStyleColor(ImGuiCol_TextDisabled, rowText(Theme::TextDim));
	ImGui::PushID(static_cast<int>(t));

	// ── Icon ────────────────────────────────────────────────────────────────
	ImGui::TableSetColumnIndex(0);
	const ImVec2 cellPad = ImGui::GetStyle().CellPadding;
	const ImVec2 rowTop(ImGui::GetCursorScreenPos().x - cellPad.x, ImGui::GetCursorScreenPos().y - cellPad.y);
	if (flashing)
	{
		// The row's height is only known once its tallest column is drawn, so the
		// bar takes last frame's; the first frame falls back to the icon's.
		const float h = s_flashRowH > 0.0f ? s_flashRowH : ImGui::GetFrameHeightWithSpacing();
		ImGui::GetWindowDrawList()->AddRectFilled(rowTop, ImVec2(rowTop.x + 3.0f, rowTop.y + h),
		                                          Theme::u32(Theme::alpha(Theme::Accent, k)));
	}
	drawStateIcon(st);

	// ── Body ────────────────────────────────────────────────────────────────
	ImGui::TableSetColumnIndex(1);
	ImGui::TextColored(rowText(Theme::TextHeading), "%s", toolName(t));
	if (st != RowState::Ok) dimWrapped(toolPurpose(t));

	switch (st)
	{
	case RowState::Ok:
		dimWrapped(okDetail(t).c_str());
		break;

	case RowState::Checking:
		dimWrapped((ctx.toolchainInstallDone || ctx.gitInstallDone) && !busy ? "Verifying…" : "Checking…");
		break;

	case RowState::Waiting:
		dimWrapped(t == Tool::GitLfs ? "Available once git is installed."
		                             : "Set once git is installed.");
		break;

	case RowState::Missing:
		if (t == Tool::GitIdentity)
		{
			drawIdentityForm(ctx);
			break;
		}
		if (canInstall(ctx, t))
		{
			dimWrapped(sourceLine(t).c_str());
			const std::string prompt = promptLine(t);
			if (!prompt.empty()) dimWrapped(prompt.c_str());
			// The installer ended cleanly but the probe still does not see the tool
			// (a new PATH entry, or Homebrew being installed in Terminal).
			const bool finishedOk = isToolchainTool(t)
				? (ctx.toolchainInstallDone && ctx.toolchainInstallOk && installRequested(t) && !s_ackTc)
				: (ctx.gitInstallDone && ctx.gitInstallOk && (ctx.gitInstallIsLfs == (t == Tool::GitLfs)) && !s_ackGit);
			if (finishedOk)
				dimWrapped("The installer finished, but the tool is not detected yet. Press Recheck.");
		}
		else
		{
			const std::string& why = s_git.install.reason;
			dimWrapped(gitSlot && !why.empty()
				? ("The editor cannot install this here by itself: " + why + ".").c_str()
				: "The editor cannot install this here by itself. See Manual install below.");
		}
		break;

	case RowState::Installing:
	{
		const std::string log = gitSlot ? (ctx.gitInstallLog ? ctx.gitInstallLog() : std::string())
		                                : (ctx.toolchainInstallLog ? ctx.toolchainInstallLog() : std::string());
		const bool apple = log.find("Apple") != std::string::npos;
		drawPhases(log.empty() ? 0 : 1,
		           apple ? "Apple's installer"
#if defined(_WIN32)
		                 : "winget");
#elif defined(__APPLE__)
		                 : "Homebrew");
#else
		                 : "Package manager");
#endif
		const HE::hccg::ToolchainInstallControl* ctl = gitSlot ? nullptr : ctx.toolchainInstallControl;
		const int percent = ctl ? ctl->percent.load() : -1;
		// A percentage the installer printed fills the bar; without one it keeps sweeping.
		ImGui::ProgressBar(percent >= 0 ? percent / 100.0f : -1.0f * static_cast<float>(ImGui::GetTime()),
		                   ImVec2(kColBody - 16.0f, 6.0f), "");
		if (ctl)
		{
			std::string names;
			{ std::lock_guard<std::mutex> lk(ctl->mx); names = ctl->processNames; }
			if (ctl->cancel.load())
				dimWrapped("Cancelling…");
			else if (!names.empty())
				dimWrapped(("Running: " + names + (percent >= 0 ? "  ·  " + std::to_string(percent) + " %" : std::string())).c_str());
			else
				dimWrapped("Waiting for the package manager to start…");
			const long long last = ctl->lastActivityMs.load();
			const long long idle = last > 0 ? steadyNowMs() - last : 0;
			if (!ctl->cancel.load() && idle >= kStallWarnMs)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, Theme::Warn);
				ImGui::TextWrapped("No change for %s. A large download by an installer that runs with "
				                   "administrator rights cannot be measured from here; if the processes "
				                   "above are gone or nothing moves for much longer, cancel and retry.",
				                   mmss(idle).c_str());
				ImGui::PopStyleColor();
			}
		}
		const std::string prompt = promptLine(t);
		if (!prompt.empty()) dimWrapped(prompt.c_str());
		break;
	}

	case RowState::Failed:
	{
		const std::string log = gitSlot ? (ctx.gitInstallLog ? ctx.gitInstallLog() : std::string())
		                                : (ctx.toolchainInstallLog ? ctx.toolchainInstallLog() : std::string());
		ImGui::PushStyleColor(ImGuiCol_Text, rowText(Theme::Warn));
		ImGui::TextWrapped("%s was not installed.", toolName(t));
		ImGui::PopStyleColor();
		const std::string why = lastLine(log);
		if (!why.empty()) dimWrapped(why.c_str());
		break;
	}
	}

	float rowBottom = ImGui::GetCursorScreenPos().y;

	// ── Action ──────────────────────────────────────────────────────────────
	ImGui::TableSetColumnIndex(2);
	switch (st)
	{
	case RowState::Missing:
		if (t != Tool::GitIdentity && canInstall(ctx, t))
		{
			if (busy) ImGui::BeginDisabled();
			if (EditorWidgets::primaryButton("Install", ImVec2(kColAction - 16.0f, 0.0f)))
				startOne(ctx, t);
			if (busy) ImGui::EndDisabled();
		}
		break;

	case RowState::Failed:
		if (busy) ImGui::BeginDisabled();
		if (EditorWidgets::smallButton("Retry"))
		{
			(gitSlot ? s_ackGit : s_ackTc) = true;
			startOne(ctx, t);
		}
		if (busy) ImGui::EndDisabled();
		ImGui::SameLine();
		if (EditorWidgets::smallButton("Show Log")) s_openLog = true;
		break;

	case RowState::Installing:
		ImGui::TextDisabled("Installing · %s", clockText(slot).c_str());
		// Only where it keeps its word: the wait for Apple's installer stops at once,
		// while a package manager that is already running cannot be interrupted.
		if (gitSlot && ctx.cancelGitInstall)
		{
			const auto launch = HE::Sc::gitInstallLaunch(s_git.install, gitTarget(t));
			if (launch && launch->returnsBeforeInstalled && EditorWidgets::smallButton("Cancel"))
				ctx.cancelGitInstall();
		}
		// The toolchain install can always be ended: the watcher kills the process tree.
		if (!gitSlot && ctx.cancelToolchainInstall && EditorWidgets::smallButton("Cancel"))
			ctx.cancelToolchainInstall();
		break;

	default:
		break;
	}

	rowBottom = std::max(rowBottom, ImGui::GetCursorScreenPos().y);
	if (flashing)
		s_flashRowH = rowBottom - ImGui::GetStyle().ItemSpacing.y + cellPad.y - rowTop.y;
	ImGui::PopID();
	ImGui::PopStyleColor();   // TextDisabled
	s_rowGround = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
}

void drawLog(AppContext& ctx)
{
	HE::Ed::Help::Scope helpScope("Tool Setup");

	std::string log;
	if (s_logIsGit) { if (ctx.gitInstallLog)       log = ctx.gitInstallLog(); }
	else            { if (ctx.toolchainInstallLog) log = ctx.toolchainInstallLog(); }
	if (log.empty()) return;

	if (s_openLog) { ImGui::SetNextItemOpen(true); s_openLog = false; }
	if (!ImGui::TreeNode("Install log")) return;
	ImGui::BeginChild("##toolsetuplog", ImVec2(kContentW, 140.0f), true);
	// Wrapped, not scrolled sideways: the sentence that says why an install stopped
	// is at the end of one long line.
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextUnformatted(log.c_str());
	ImGui::PopTextWrapPos();
	if (anyBusy(ctx)) ImGui::SetScrollHereY(1.0f);
	ImGui::EndChild();
	ImGui::TreePop();
}

void drawManual(AppContext& ctx)
{
	HE::Ed::Help::Scope helpScope("Tool Setup");

	std::vector<Tool> open;
	for (Tool t : { Tool::Cmake, Tool::Compiler, Tool::Git, Tool::GitLfs })
	{
		const RowState st = rowState(ctx, t);
		if (st == RowState::Missing || st == RowState::Failed || st == RowState::Waiting) open.push_back(t);
	}
	if (open.empty()) return;
	if (!ImGui::TreeNode("Manual install")) return;

	{
		EditorWidgets::WrapText wrap(kContentW);
		dimWrapped("If the editor cannot install a tool, run the command yourself in a terminal, "
		           "then press Recheck. No restart is needed.");
	}
	for (Tool t : open)
	{
		const Manual m = manualFor(t);
		if (m.command.empty() && m.url.empty()) continue;
		ImGui::PushID(static_cast<int>(t));
		ImGui::Spacing();
		ImGui::TextUnformatted(toolName(t));
		if (!m.command.empty())
		{
			{
				EditorWidgets::WrapText wrap(kContentW);
				dimWrapped(m.command.c_str());
			}
			if (EditorWidgets::smallButton("Copy Command")) ImGui::SetClipboardText(m.command.c_str());
		}
		if (!m.url.empty())
		{
			if (!m.command.empty()) ImGui::SameLine();
			if (EditorWidgets::smallButton("Open Download Page")) SDL_OpenURL(m.url.c_str());
		}
		ImGui::PopID();
	}
	ImGui::TreePop();
}

const char* platformPromptSentence()
{
#if defined(_WIN32)
	return "Windows may ask for permission (UAC) while an installer runs.";
#elif defined(__APPLE__)
	return "macOS may ask for your password or open Apple's installer in its own window.";
#else
	return "A system dialog asks for an administrator password once per install.";
#endif
}

} // namespace

// ── Public ───────────────────────────────────────────────────────────────────

void resetStateForTests()
{
	s_tc = {}; s_haveTc = s_checkingTc = false;
	s_git = {}; s_haveGit = s_checkingGit = false;
	s_tcSlot = {}; s_gitSlot = {};
	s_tcRequested = 0;
	s_tcSeen = s_tcConsumed = s_ackTc = s_ackGit = s_logIsGit = s_openLog = false;
	s_queue.clear();
	s_openRequested = s_autoOpened = s_dontShowAgain = s_sawMissing = false;
	s_flashTool = Tool::None; s_flashUntil = 0.0; s_flashArmed = false; s_flashRowH = 0.0f;
	s_purpose.clear(); s_showing = false;
	s_name[0] = s_email[0] = '\0';
	s_identitySeeded = false;
}

void requestShow(Tool focus)
{
	s_openRequested = true;
	s_flashTool     = focus;
	s_flashArmed    = focus != Tool::None;
	s_purpose.clear();
}

void requestShowFor(Tool focus, const char* purpose)
{
	requestShow(focus);
	s_purpose = purpose ? purpose : "";
}

const char* purpose() { return s_purpose.c_str(); }

bool isShowing() { return s_showing; }

void drawToolSetup(AppContext& ctx, bool* closeRequested)
{
	HE::Ed::Help::Scope helpScope("Tool Setup");
	refresh(ctx);
	pumpQueue(ctx);

	if (s_flashArmed) { s_flashUntil = ImGui::GetTime() + 1.6; s_flashArmed = false; }

	const bool allOk = everythingSetUp(ctx);
	const bool anyKnownMissing = !allOk;
	if (anyKnownMissing) s_sawMissing = true;

	// ── Heading and the one paragraph that matters ───────────────────────────
	{
		EditorWidgets::WrapText wrap(kContentW);
		ImGui::PushStyleColor(ImGuiCol_Text, allOk ? Theme::Ok : Theme::Warn);
		ImGui::TextUnformatted(allOk ? "Everything Is Set Up" : "Missing Tools");
		ImGui::PopStyleColor();
		ImGui::Separator();
		ImGui::Spacing();
		if (allOk)
		{
			ImGui::TextWrapped("cmake, the C++ compiler, git and Git LFS are installed and git knows who you are.");
		}
		else
		{
			ImGui::TextWrapped(
				"The editor keeps working without these. cmake and a C++ compiler are needed "
				"to export a game (it is compiled from the engine SDK), HorizonCode and native "
				"GameLogic; git, Git LFS and your name are "
				"needed for source control. Installs go through this system's package manager. %s",
				platformPromptSentence());
		}
		// Opened for something that is waiting on a tool (the export): say what
		// waits and that it carries on by itself.
		if (!s_purpose.empty())
		{
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, Theme::Warn);
			ImGui::TextWrapped("%s", s_purpose.c_str());
			ImGui::PopStyleColor();
		}
		ImGui::Spacing();
	}

	// ── The rows ─────────────────────────────────────────────────────────────
	constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersOuterH |
	                                   ImGuiTableFlags_SizingFixedFit;
	if (ImGui::BeginTable("##toolsetuprows", 3, kFlags))
	{
		ImGui::TableSetupColumn("##state",  ImGuiTableColumnFlags_WidthFixed, kColIcon);
		ImGui::TableSetupColumn("##tool",   ImGuiTableColumnFlags_WidthFixed, kColBody);
		ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthFixed, kColAction);
		for (Tool t : kRows) drawRow(ctx, t);
		ImGui::EndTable();
	}

	ImGui::Spacing();
	drawLog(ctx);
	drawManual(ctx);

	// ── Footer ───────────────────────────────────────────────────────────────
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	const int  installable = installableCount(ctx);
	const bool queued      = !s_queue.empty();
	const bool busy        = anyBusy(ctx);
	// Nothing to install and nothing missing: the button has no job, so it is not drawn.
	if (!allOk)
	{
		if (installable == 0 || busy || queued) ImGui::BeginDisabled();
		if (EditorWidgets::primaryButton("Install All Missing"))
			queueAll(ctx);
		if (installable == 0 || busy || queued) ImGui::EndDisabled();
		ImGui::SameLine();
		if (queued) ImGui::TextDisabled("%d more queued", static_cast<int>(s_queue.size()));
		else if (installable > 0) ImGui::TextDisabled("%d to install", installable);
		ImGui::Spacing();
	}
	EditorWidgets::checkbox("Don't show this at startup", &s_dontShowAgain);
	ImGui::Spacing();

	const bool recheckBlocked = busy || s_checkingTc || s_checkingGit;
	if (recheckBlocked) ImGui::BeginDisabled();
	if (EditorWidgets::button("Recheck"))
	{
		s_queue.clear();
		s_ackTc = s_ackGit = true;
		if (ctx.recheckToolchain) ctx.recheckToolchain();
		if (ctx.recheckGit)       ctx.recheckGit();
	}
	if (recheckBlocked) ImGui::EndDisabled();
	ImGui::SameLine();
	if (s_checkingTc || s_checkingGit) ImGui::TextDisabled("Rechecking…");
	else if (ImGui::Button("Close"))
	{
		if (s_dontShowAgain && ctx.globalState)
		{
			ctx.globalState->setCustomConfigEntry("SuppressToolchainWarning", true);
			ctx.globalState->setCustomConfigEntry("SuppressSourceControlWarning", true);
			ctx.globalState->writeConfig();
		}
		if (closeRequested) *closeRequested = true;
	}

	// Set up while this was open: close on our own, like the dialogs before did.
	if (allOk && s_sawMissing && !busy && closeRequested) *closeRequested = true;
}

void DrawToolSetupDialog(AppContext& ctx)
{
	refresh(ctx);

	// ── When to open ─────────────────────────────────────────────────────────
	// The first probe that finds something missing (unless that part was silenced),
	// once per session; and any explicit request, whenever it comes.
	if (!s_autoOpened)
	{
		const bool tcMissing  = s_haveTc  && (!s_tc.cmakeFound || !s_tc.compilerFound);
		const bool gitMissing = s_haveGit && !s_git.ready();
		const bool tcSuppressed  = ctx.globalState && ctx.globalState->getCustomConfigBool("SuppressToolchainWarning", false);
		const bool gitSuppressed = ctx.globalState && ctx.globalState->getCustomConfigBool("SuppressSourceControlWarning", false);
		if ((tcMissing && !tcSuppressed) || (gitMissing && !gitSuppressed))
		{
			s_autoOpened    = true;
			s_openRequested = true;
		}
	}
	if (s_openRequested)
	{
		s_openRequested = false;
		s_dontShowAgain = false;
		s_sawMissing    = false;
		ImGui::OpenPopup("##ToolSetup");
	}

	ImGui::SetNextWindowSize(ImVec2(kDialogWidth, 0.0f), ImGuiCond_Appearing);
	EditorWidgets::pinDialogToEditorWindow();
	s_showing = false;
	if (!ImGui::BeginPopupModal("##ToolSetup", nullptr,
	                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
	{
		// Closed (or never opened): whatever opened it for a purpose has had its answer.
		s_purpose.clear();
		return;
	}

	bool close = false;
	drawToolSetup(ctx, &close);
	if (close) ImGui::CloseCurrentPopup();
	else s_showing = true;
	ImGui::EndPopup();
}

void drawMissingBanner(AppContext& ctx, Tool tool, const char* message)
{
	HE::Ed::Help::Scope helpScope("Tool Setup");

	const RowState st = rowState(ctx, tool);
	if (st == RowState::Ok || st == RowState::Checking) return;

	ImGui::PushStyleColor(ImGuiCol_Text, Theme::Warn);
	ImGui::TextWrapped("%s", message);
	ImGui::PopStyleColor();

	if (st == RowState::Installing)
	{
		const Slot& slot = isGitInstallTool(tool) ? s_gitSlot : s_tcSlot;
		ImGui::TextDisabled("Installing · %s", clockText(slot).c_str());
	}
	else if (EditorWidgets::smallButton("Set Up…"))
	{
		requestShow(tool);
	}
}

} // namespace ToolSetupDialog
