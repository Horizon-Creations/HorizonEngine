#include "doctest.h"
#include "ImGuiSoftwareRaster.h"

#include "ToolSetupDialog.h"
#include "ExportSdkGateUi.h"     // the export dialog's game-compile line (topic 222)
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"

#include <HorizonScene/HcCodegen.h>
#include <SourceControl/GitInstall.h>
#include <SourceControl/GitProbe.h>

#include <imgui.h>
#include <imgui_internal.h>   // FindWindowByName, ImGuiWindow::GetID

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

// ── The missing-tools dialog, driven headless ────────────────────────────────
// One row per tool (cmake, compiler, git, Git LFS, identity). What matters: each row
// shows the state the probes and the installers' flags say, the button on a row
// reaches the editor as exactly the installer call that belongs to it (cmake and the
// compiler through startToolchainInstall with their own flags, git and Git LFS
// through their own callback), "Install All Missing" works the rows off one installer
// at a time, and the banner other panels draw opens this dialog. Nothing installs:
// every callback is a recorder. HE_UI_DUMP_DIR writes the frames as BMP.

using namespace HE::Ed;
using namespace HE::Sc;
using ToolSetupDialog::RowState;
using ToolSetupDialog::Tool;

namespace
{
	constexpr int W = 700, H = 640;
	constexpr const char* kWindow = "##ToolSetupHost";
	// Where the action column sits in the host window: window padding, then the
	// 28 px icon column and the 394 px body column (see ToolSetupDialog.cpp).
	constexpr float kActionX = 8.0f + 28.0f + 394.0f + 112.0f;

	struct Harness
	{
		Harness()
		{
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(float(W), float(H));
			io.DeltaTime   = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
			io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
#ifdef HE_EDITOR_DEPS_DIR
			const std::string font = std::string(HE_EDITOR_DEPS_DIR) + "/Fonts/Roboto_Condensed-Bold.ttf";
			if (std::FILE* f = std::fopen(font.c_str(), "rb"))
			{
				std::fclose(f);
				ImFontConfig cfg;
				cfg.OversampleH = 2;
				cfg.OversampleV = 2;
				io.FontDefault = io.Fonts->AddFontFromFileTTF(font.c_str(), 15.0f, &cfg);
			}
#endif
			applyHorizonDarkTheme();
			ToolSetupDialog::resetStateForTests();
		}
		~Harness() { ImGui::DestroyContext(); }
	};

	struct ContextBits
	{
		EditorConfig    config;
		bool            vsync = false;
		std::string     backendName = "Software";
		EditorSelection selection;
		std::string     scenePath;
		bool            exitRequested = false, projectLoaded = true;
		bool            refreshPending = false, refreshDone = false;
		int   fpsOffset = 0, fpsCount = 0;
		float fpsAccum = 0.0f, smoothFps = 0.0f;
		std::vector<AppContext::EditorTab> tabs;
		int   activeTab = 0;
		float cbTreeWidth = 200.0f;
		int   hubPreset = 0, hubLang = 0;
		bool  hubFx = false;
		std::string hubCreateError, hubOpenError;
		int   hubRemoveIndex = -1;
		bool  hubRemoveRequested = false;
		std::string dirResult, fileResult;
		bool  dirReady = false, fileReady = false;

		// The machine as the probes saw it …
		HE::hccg::ToolchainProbe tc;
		bool         tcProbed = true;
		GitProbe     git;
		bool         gitProbed = true;
		// … and what the editor was asked to do.
		std::vector<std::pair<bool, bool>> toolchainInstalls;   // (needCmake, needCompiler)
		int          gitInstalls = 0, lfsInstalls = 0, cancels = 0;
		int          toolchainRechecks = 0, gitRechecks = 0;
		std::vector<std::pair<std::string, std::string>> identities;
		std::string  tcLog, gitLog;
		bool         tcInstalling = false, tcDone = false, tcOk = false;
		bool         gitInstalling = false, gitDone = false, gitOk = false, gitIsLfs = false;

		AppContext make()
		{
			AppContext ctx{
				.editorConfig          = config,
				.vsync                 = vsync,
				.backendName           = backendName,
				.selection             = selection,
				.currentScenePath      = scenePath,
				.exitRequested         = exitRequested,
				.projectLoaded         = projectLoaded,
				.contentRefreshPending = refreshPending,
				.contentRefreshDone    = refreshDone,
				.fpsHistoryOffset      = fpsOffset,
				.fpsAccum              = fpsAccum,
				.fpsAccumCount         = fpsCount,
				.smoothFps             = smoothFps,
				.tabs                  = tabs,
				.activeTab             = activeTab,
				.cbTreeWidth           = cbTreeWidth,
				.hubSelectedPreset     = hubPreset,
				.hubSelectedLang       = hubLang,
				.hubAdvancedShaderFx   = hubFx,
				.hubCreateError        = hubCreateError,
				.hubOpenError          = hubOpenError,
				.hubRemoveIndex        = hubRemoveIndex,
				.hubRemoveRequested    = hubRemoveRequested,
				.pendingDirResult      = dirResult,
				.pendingDirReady       = dirReady,
				.pendingFileResult     = fileResult,
				.pendingFileReady      = fileReady,
			};
			ctx.toolchainProbe        = tcProbed ? &tc : nullptr;
			ctx.recheckToolchain      = [this]{ ++toolchainRechecks; };
			// The editor raises the running flag synchronously, so the next frame already
			// sees the install as running: the recorders do the same.
			ctx.startToolchainInstall = [this](bool c, bool k)
			{
				toolchainInstalls.emplace_back(c, k);
				tcInstalling = true; tcDone = false; tcOk = false;
			};
			ctx.toolchainInstallLog   = [this]{ return tcLog; };
			ctx.toolchainInstalling   = tcInstalling;
			ctx.toolchainInstallDone  = tcDone;
			ctx.toolchainInstallOk    = tcOk;

			ctx.gitProbe              = gitProbed ? &git : nullptr;
			ctx.recheckGit            = [this]{ ++gitRechecks; };
			ctx.setGitIdentity        = [this](std::string n, std::string e){ identities.emplace_back(n, e); };
			ctx.startGitInstall       = [this]{ ++gitInstalls;
				gitInstalling = true; gitDone = false; gitOk = false; gitIsLfs = false; };
			ctx.startGitLfsInstall    = [this]{ ++lfsInstalls;
				gitInstalling = true; gitDone = false; gitOk = false; gitIsLfs = true; };
			ctx.cancelGitInstall      = [this]{ ++cancels; };
			ctx.gitInstallLog         = [this]{ return gitLog; };
			ctx.gitInstalling         = gitInstalling;
			ctx.gitInstallDone        = gitDone;
			ctx.gitInstallOk          = gitOk;
			ctx.gitInstallIsLfs       = gitIsLfs;
			return ctx;
		}

		// A Linux machine with apt and a polkit agent: everything installs by button.
		void onLinux(bool cmake, bool compiler, bool gitFound, bool lfs, bool identity)
		{
			tc = {};
			tc.cmakeFound = cmake;       tc.cmakeVersion = cmake ? "3.28.3" : "";
			tc.compilerFound = compiler; tc.compilerId = compiler ? "GNU 13.2.0" : "";
			git = {};
			git.install = planGitInstall(HostPlatform::Linux,
				[](std::string_view t) { return t == "apt-get" || t == "pkexec"; }, false);
			git.gitFound = gitFound;      git.gitVersion = gitFound ? "2.43.0" : "";
			git.lfsFound = lfs;           git.lfsVersion = lfs ? "3.4.1" : "";
			git.identityConfigured = identity;
			if (identity) { git.userName = "Ada"; git.userEmail = "ada@example.com"; }
		}
	};

	// One frame of the body in a plain full-window host.
	ImGuiID frame(ContextBits& bits, bool mouseDown, he_ui::Image* shot = nullptr, bool* close = nullptr)
	{
		AppContext ctx = bits.make();
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(float(W), float(H)));
		ImGui::Begin(kWindow, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		                               ImGuiWindowFlags_NoSavedSettings);
		ToolSetupDialog::drawToolSetup(ctx, close);
		ImGui::End();
		EditorWidgets::drawQueuedHelp();
		const ImGuiID hovered = ImGui::GetHoveredID();
		ImGui::Render();
		if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
		return hovered;
	}

	ImGuiID idAt(ContextBits& bits, float x, float y)
	{
		ImGui::GetIO().AddMousePosEvent(x, y);
		frame(bits, false);
		return frame(bits, false);
	}

	ImGuiID idOf(const char* label)
	{
		ImGuiWindow* w = ImGui::FindWindowByName(kWindow);
		REQUIRE(w != nullptr);
		return w->GetID(label);
	}

	// The hoverable items down one column of the host window, top to bottom, each
	// with a point inside it. The rows' buttons sit in the action column, so their
	// order here is the order of the rows.
	std::vector<std::pair<ImGuiID, ImVec2>> column(ContextBits& bits, float x)
	{
		std::vector<std::pair<ImGuiID, ImVec2>> found;
		for (float y = 4.0f; y < float(H) - 4.0f; y += 4.0f)
			if (const ImGuiID id = idAt(bits, x, y); id != 0 && (found.empty() || found.back().first != id))
				found.emplace_back(id, ImVec2(x, y));
		return found;
	}

	// The same item seen on several scan lines counts once.
	std::vector<std::pair<ImGuiID, ImVec2>> actions(ContextBits& bits)
	{
		auto col = column(bits, kActionX);
		std::vector<std::pair<ImGuiID, ImVec2>> unique;
		std::set<ImGuiID> seen;
		for (auto& it : col)
			if (seen.insert(it.first).second) unique.push_back(it);
		return unique;
	}

	std::map<ImGuiID, ImVec2> itemsInFooter(ContextBits& bits)
	{
		std::map<ImGuiID, ImVec2> found;
		for (float y = 4.0f; y < float(H) - 4.0f; y += 4.0f)
			for (float x = 12.0f; x < 400.0f; x += 24.0f)
				if (const ImGuiID id = idAt(bits, x, y); id != 0 && !found.count(id))
					found[id] = ImVec2(x, y);
		return found;
	}

	void clickAt(ContextBits& bits, ImVec2 p)
	{
		ImGui::GetIO().AddMousePosEvent(p.x, p.y);
		frame(bits, false);
		frame(bits, false);
		frame(bits, true);
		frame(bits, false);
		frame(bits, false);
	}

	void dump(ContextBits& bits, const char* name)
	{
		ImGui::GetIO().AddMousePosEvent(-100.0f, -100.0f);
		frame(bits, false);
		he_ui::Image img;
		frame(bits, false, &img);
		if (const char* d = std::getenv("HE_UI_DUMP_DIR"); d && *d)
			he_ui::writeBmp(img, std::string(d) + "/" + name + ".bmp");
	}

	RowState state(ContextBits& bits, Tool t)
	{
		AppContext ctx = bits.make();
		return ToolSetupDialog::rowState(ctx, t);
	}
}

TEST_CASE("Tool setup: every row shows the state the probes report")
{
	Harness h;
	ContextBits bits;
	bits.onLinux(/*cmake*/ true, /*compiler*/ false, /*git*/ false, false, false);
	frame(bits, false);

	CHECK(state(bits, Tool::Cmake)       == RowState::Ok);
	CHECK(state(bits, Tool::Compiler)    == RowState::Missing);
	CHECK(state(bits, Tool::Git)         == RowState::Missing);
	// LFS and the identity need git first: nothing to press yet.
	CHECK(state(bits, Tool::GitLfs)      == RowState::Waiting);
	CHECK(state(bits, Tool::GitIdentity) == RowState::Waiting);
	dump(bits, "tool-setup-start");

	bits.onLinux(true, true, /*git*/ true, /*lfs*/ false, /*identity*/ false);
	frame(bits, false);
	CHECK(state(bits, Tool::Git)         == RowState::Ok);
	CHECK(state(bits, Tool::GitLfs)      == RowState::Missing);
	CHECK(state(bits, Tool::GitIdentity) == RowState::Missing);
}

TEST_CASE("Tool setup: before a probe has answered the rows are checking, never missing")
{
	Harness h;
	ContextBits bits;
	bits.onLinux(false, false, false, false, false);
	bits.tcProbed = false;
	bits.gitProbed = false;
	frame(bits, false);
	for (Tool t : { Tool::Cmake, Tool::Compiler, Tool::Git, Tool::GitLfs, Tool::GitIdentity })
		CHECK(state(bits, t) == RowState::Checking);
	// No install is offered for something that may well be there.
	CHECK(actions(bits).empty());
}

TEST_CASE("Tool setup: each Install button starts the installer that belongs to its row")
{
	Harness h;
	ContextBits bits;
	bits.onLinux(/*cmake*/ false, /*compiler*/ false, /*git*/ false, false, false);
	frame(bits, false);

	const auto buttons = actions(bits);
	REQUIRE(buttons.size() == 3);   // cmake, compiler, git. LFS and identity wait for git.

	clickAt(bits, buttons[0].second);
	REQUIRE(bits.toolchainInstalls.size() == 1);
	CHECK(bits.toolchainInstalls[0] == std::make_pair(true, false));   // cmake alone
	bits.tcInstalling = false;   // that one finished; the next button is live again

	clickAt(bits, buttons[1].second);
	REQUIRE(bits.toolchainInstalls.size() == 2);
	CHECK(bits.toolchainInstalls[1] == std::make_pair(false, true));   // the compiler alone
	bits.tcInstalling = false;

	clickAt(bits, buttons[2].second);
	CHECK(bits.gitInstalls == 1);
	CHECK(bits.lfsInstalls == 0);
}

TEST_CASE("Tool setup: Git LFS installs through its own callback once git is there")
{
	Harness h;
	ContextBits bits;
	bits.onLinux(true, true, /*git*/ true, /*lfs*/ false, /*identity*/ true);
	frame(bits, false);

	const auto buttons = actions(bits);
	REQUIRE(buttons.size() == 1);
	clickAt(bits, buttons[0].second);
	CHECK(bits.lfsInstalls == 1);
	CHECK(bits.gitInstalls == 0);
	dump(bits, "tool-setup-lfs-missing");
}

TEST_CASE("Tool setup: a running install shows the row as installing and switches the others off")
{
	Harness h;
	ContextBits bits;
	bits.onLinux(false, true, false, false, false);   // cmake and git missing, compiler found
	frame(bits, false);
	{
		const auto buttons = actions(bits);
		REQUIRE(buttons.size() == 2);
		clickAt(bits, buttons[0].second);   // the cmake row
	}
	REQUIRE(bits.toolchainInstalls.size() == 1);
	CHECK(bits.toolchainInstalls[0] == std::make_pair(true, false));
	CHECK(bits.tcInstalling);   // the editor raises it at once

	bits.tcLog = "$ pkexec /bin/sh -c 'apt-get install -y cmake'\nReading package lists...\n";
	frame(bits, false);
	CHECK(state(bits, Tool::Cmake) == RowState::Installing);
	CHECK(state(bits, Tool::Git)   == RowState::Missing);   // not part of this install
	dump(bits, "tool-setup-installing");

	// Nothing else starts while it runs: the buttons are drawn but off.
	for (const auto& b : actions(bits)) clickAt(bits, b.second);
	CHECK(bits.gitInstalls == 0);
	CHECK(bits.toolchainInstalls.size() == 1);
}

TEST_CASE("Tool setup: a failed install says why and offers Retry")
{
	Harness h;
	ContextBits bits;
	bits.onLinux(true, false, true, true, true);   // only the compiler is missing
	frame(bits, false);
	{
		const auto buttons = actions(bits);
		REQUIRE(buttons.size() == 1);
		clickAt(bits, buttons[0].second);
		REQUIRE(bits.toolchainInstalls.size() == 1);
	}
	frame(bits, false);   // running
	bits.tcInstalling = false;
	bits.tcDone = true;
	bits.tcOk   = false;
	bits.tcLog  = "$ pkexec ...\nThe administrator password dialog was cancelled, so nothing was installed.\n";
	frame(bits, false);
	CHECK(state(bits, Tool::Compiler) == RowState::Failed);
	dump(bits, "tool-setup-failed");

	// Retry and Show Log take the place of Install; Retry (the left one) starts it again.
	const auto buttons = column(bits, 8.0f + 28.0f + 394.0f + 30.0f);
	REQUIRE(buttons.size() >= 1);
	clickAt(bits, buttons[0].second);
	CHECK(bits.toolchainInstalls.size() == 2);
}

// The row a banner sends the user to is marked for a moment after the dialog opens.
// That mark once filled the row with the accent at 35 %, under an amber Warn line and
// the dim detail lines: the frame a person reported as unreadable. Only the first
// frames after requestShow show it, so it gets its own shots (topic 224).
TEST_CASE("Tool setup: the row a banner points at is marked when the dialog opens")
{
	Harness h;
	ContextBits bits;
	bits.onLinux(true, false, true, true, true);   // only the compiler is missing
	frame(bits, false);
	ToolSetupDialog::requestShow(Tool::Compiler);
	frame(bits, false);
	CHECK(state(bits, Tool::Compiler) == RowState::Missing);
	dump(bits, "tool-setup-flash-missing");

	{
		const auto buttons = actions(bits);
		REQUIRE(buttons.size() == 1);
		clickAt(bits, buttons[0].second);
		REQUIRE(bits.toolchainInstalls.size() == 1);
	}
	bits.tcInstalling = false;
	bits.tcDone = true;
	bits.tcOk   = false;
	bits.tcLog  = "$ pkexec ...\nThe administrator password dialog was cancelled, so nothing was installed.\n";
	frame(bits, false);
	ToolSetupDialog::requestShow(Tool::Compiler);
	frame(bits, false);
	CHECK(state(bits, Tool::Compiler) == RowState::Failed);
	dump(bits, "tool-setup-flash-failed");
}

TEST_CASE("Tool setup: Install All Missing works through the installers one at a time")
{
	Harness h;
	ContextBits bits;
	bits.onLinux(false, false, false, false, false);
	frame(bits, false);

	const auto footer = itemsInFooter(bits);
	REQUIRE(footer.count(idOf("Install All Missing")) == 1);
	clickAt(bits, footer.at(idOf("Install All Missing")));
	frame(bits, false);

	// cmake and the compiler in ONE call (one password prompt), git waits.
	REQUIRE(bits.toolchainInstalls.size() == 1);
	CHECK(bits.toolchainInstalls[0] == std::make_pair(true, true));
	CHECK(bits.gitInstalls == 0);

	// The toolchain install runs: still nothing else.
	frame(bits, false);
	frame(bits, false);
	CHECK(bits.gitInstalls == 0);

	// Finished and found: git starts, once.
	bits.tcInstalling = false;
	bits.tcDone = true;
	bits.tcOk   = true;
	bits.tc.cmakeFound = bits.tc.compilerFound = true;
	frame(bits, false);
	frame(bits, false);
	CHECK(bits.toolchainRechecks == 1);   // re-probed once when the install ended
	CHECK(bits.gitInstalls == 1);
	CHECK(bits.lfsInstalls == 0);
	frame(bits, false);
	CHECK(bits.gitInstalls == 1);

	// Git LFS follows once git is in and LFS is still missing.
	bits.gitInstalling = false;
	bits.gitDone = bits.gitOk = true;
	bits.git.gitFound = true;
	frame(bits, false);
	frame(bits, false);
	CHECK(bits.lfsInstalls == 1);
}

TEST_CASE("Tool setup: Install All skips what an earlier step brought along")
{
	Harness h;
	ContextBits bits;
	bits.onLinux(true, true, false, false, false);   // git and LFS missing
	frame(bits, false);
	const auto footer = itemsInFooter(bits);
	clickAt(bits, footer.at(idOf("Install All Missing")));
	frame(bits, false);
	CHECK(bits.gitInstalls == 1);

	// Git for Windows ships LFS: git comes in and so does LFS, so there is nothing left.
	bits.gitInstalling = false;
	bits.gitDone = bits.gitOk = true;
	bits.git.gitFound = true;
	bits.git.lfsFound = true;
	frame(bits, false);
	frame(bits, false);
	CHECK(bits.lfsInstalls == 0);
}

TEST_CASE("Tool setup: a failed step empties the queue")
{
	Harness h;
	ContextBits bits;
	bits.onLinux(false, false, false, false, false);
	frame(bits, false);
	const auto footer = itemsInFooter(bits);
	clickAt(bits, footer.at(idOf("Install All Missing")));
	frame(bits, false);
	REQUIRE(bits.toolchainInstalls.size() == 1);

	frame(bits, false);   // running
	bits.tcInstalling = false;
	bits.tcDone = true;
	bits.tcOk   = false;   // the password dialog was cancelled
	frame(bits, false);
	frame(bits, false);
	CHECK(bits.gitInstalls == 0);   // no second password prompt for git
}

TEST_CASE("Tool setup: the identity is saved from the row, only once both fields are filled")
{
	Harness h;
	ContextBits bits;
	bits.onLinux(true, true, true, true, /*identity*/ false);
	bits.git.userName = "Ada";   // one of the two was already set
	frame(bits, false);
	CHECK(state(bits, Tool::GitIdentity) == RowState::Missing);
	dump(bits, "tool-setup-identity");

	// The name is pre-filled and the email is not, so "Save Identity" is off: the one
	// action button on the identity row does nothing yet.
	for (const auto& b : actions(bits)) clickAt(bits, b.second);
	CHECK(bits.identities.empty());
}

// The automatic open happens once per session. Spend it, so a test can look at what
// the next open does.
// ImGui::IsPopupOpen needs a current window, which only exists inside a frame.
static bool toolSetupOpen()
{
	ImGui::NewFrame();
	const bool open = ImGui::IsPopupOpen("##ToolSetup");
	ImGui::Render();
	return open;
}

static void spendAutoOpen(ContextBits& bits)
{
	AppContext ctx = bits.make();
	ImGui::NewFrame();
	ToolSetupDialog::DrawToolSetupDialog(ctx);
	REQUIRE(ImGui::IsPopupOpen("##ToolSetup"));
	ImGui::ClosePopupToLevel(0, false);
	ImGui::Render();
	CHECK_FALSE(toolSetupOpen());
}

TEST_CASE("Tool setup: opens by itself, once, when a probe finds something missing")
{
	Harness h;
	ContextBits bits;
	bits.onLinux(true, true, true, true, true);
	{
		// Everything there: nothing to say.
		AppContext ctx = bits.make();
		ImGui::NewFrame();
		ToolSetupDialog::DrawToolSetupDialog(ctx);
		CHECK_FALSE(ImGui::IsPopupOpen("##ToolSetup"));
		ImGui::Render();
	}
	bits.onLinux(true, false, true, true, true);
	spendAutoOpen(bits);   // opened by itself …
	{
		// … and not again, however long the compiler stays missing.
		AppContext ctx = bits.make();
		ImGui::NewFrame();
		ToolSetupDialog::DrawToolSetupDialog(ctx);
		CHECK_FALSE(ImGui::IsPopupOpen("##ToolSetup"));
		ImGui::Render();
	}
}

TEST_CASE("Tool setup: the banner opens the dialog on the row it is about")
{
	Harness h;
	ContextBits bits;
	bits.onLinux(true, false, true, true, true);
	spendAutoOpen(bits);

	bool popupOpen = false;   // as seen inside the last frame
	const auto bannerFrame = [&](bool mouseDown)
	{
		AppContext ctx = bits.make();
		ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(float(W), 200.0f));
		ImGui::Begin("##BannerHost", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		                                      ImGuiWindowFlags_NoSavedSettings);
		ToolSetupDialog::drawMissingBanner(ctx, Tool::Compiler, "No C++ compiler found.");
		ToolSetupDialog::drawMissingBanner(ctx, Tool::Cmake,    "No cmake found.");   // found: draws nothing
		ImGui::End();
		ToolSetupDialog::DrawToolSetupDialog(ctx);
		popupOpen = ImGui::IsPopupOpen("##ToolSetup");
		const ImGuiID hovered = ImGui::GetHoveredID();
		ImGui::Render();
		return hovered;
	};

	// The banner draws exactly one button. Find it.
	ImVec2 at(-1.0f, -1.0f);
	for (float y = 4.0f; y < 120.0f && at.x < 0.0f; y += 4.0f)
		for (float x = 8.0f; x < 200.0f && at.x < 0.0f; x += 8.0f)
		{
			ImGui::GetIO().AddMousePosEvent(x, y);
			bannerFrame(false);
			if (bannerFrame(false) != 0) at = ImVec2(x, y);
		}
	REQUIRE(at.x >= 0.0f);

	ImGui::GetIO().AddMousePosEvent(at.x, at.y);
	bannerFrame(false);
	CHECK_FALSE(popupOpen);
	bannerFrame(true);
	bannerFrame(false);
	bannerFrame(false);
	CHECK(popupOpen);
}

TEST_CASE("Tool setup: the banner says nothing for a tool that is there or still being checked")
{
	Harness h;
	ContextBits bits;
	bits.onLinux(true, true, true, true, true);

	const auto count = [&](Tool t)
	{
		AppContext ctx = bits.make();
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(float(W), 200.0f));
		ImGui::Begin("##BannerHost2", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
		ToolSetupDialog::drawMissingBanner(ctx, t, "banner");
		const float h2 = ImGui::GetCursorPosY();
		ImGui::End();
		ImGui::Render();
		return h2;
	};
	const float empty = count(Tool::Compiler);   // found: drawn nothing
	bits.onLinux(true, false, true, true, true);
	CHECK(count(Tool::Compiler) > empty);        // missing: text and a button

	bits.tcProbed = false;                       // the probe has not answered yet
	ToolSetupDialog::resetStateForTests();
	CHECK(count(Tool::Compiler) == empty);
}

TEST_CASE("Tool setup: with everything installed the dialog says so and closes itself")
{
	Harness h;
	ContextBits bits;
	bits.onLinux(false, true, true, true, true);
	bool close = false;
	frame(bits, false, nullptr, &close);   // cmake missing: stays open
	CHECK_FALSE(close);

	bits.tc.cmakeFound = true;
	bits.tc.cmakeVersion = "3.28.3";
	frame(bits, false, nullptr, &close);
	CHECK(close);   // it had something missing while it was open
	dump(bits, "tool-setup-done");
}

// ── The export's way in (topic 222 step 3) ───────────────────────────────────
// Every export compiles its game from the engine SDK, so the export dialog's
// line says what it will compile against, or carries this dialog's "Set Up…"
// when the compiler is missing; an export that waits for it opens the dialog
// with a sentence saying so and watches it close.
namespace
{
	// The export dialog's line in a host window, plus the tool setup popup the
	// way EditorUI draws it every frame. Returns the hovered item.
	ImGuiID exportFrame(ContextBits& bits, bool mouseDown, he_ui::Image* shot = nullptr)
	{
		AppContext ctx = bits.make();
		HE::ExportSdk::GameSdk sdk;
		sdk.root       = "/Applications/HorizonEditor.app/Contents/Resources/EngineSDK";
		sdk.packageDir = sdk.root / "lib" / "cmake" / "HorizonEngine";
		sdk.profile    = "dev";
		const auto gate = HE::ExportSdk::checkExportGate(sdk, "dev", true, ctx.toolchainProbe);
		ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(float(W), float(H)));
		ImGui::Begin("##ExportHost", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		                                      ImGuiWindowFlags_NoSavedSettings);
		{
			EditorWidgets::WrapText wrap(550.0f);
			ExportSdkGateUi::drawGameCompileStatus(ctx, gate);
		}
		ImGui::End();
		ToolSetupDialog::DrawToolSetupDialog(ctx);
		const ImGuiID hovered = ImGui::GetHoveredID();
		ImGui::Render();
		if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
		return hovered;
	}

	void exportDump(ContextBits& bits, const char* name)
	{
		ImGui::GetIO().AddMousePosEvent(-100.0f, -100.0f);
		exportFrame(bits, false);
		he_ui::Image img;
		exportFrame(bits, false, &img);
		if (const char* d = std::getenv("HE_UI_DUMP_DIR"); d && *d)
			he_ui::writeBmp(img, std::string(d) + "/" + name + ".bmp");
	}

	// The first hoverable item in the line's band (its only button, if any).
	ImVec2 firstItem(ContextBits& bits)
	{
		for (float y = 4.0f; y < 120.0f; y += 4.0f)
			for (float x = 8.0f; x < 300.0f; x += 8.0f)
			{
				ImGui::GetIO().AddMousePosEvent(x, y);
				exportFrame(bits, false);
				if (exportFrame(bits, false) != 0) return ImVec2(x, y);
			}
		return ImVec2(-1.0f, -1.0f);
	}
}

TEST_CASE("Export: the game-compile line with a toolchain and without a compiler")
{
	Harness h;
	ContextBits bits;

	// With the toolchain: what it compiles against, nothing to press.
	bits.onLinux(true, true, true, true, true);
	exportDump(bits, "export-sdk-toolchain-ok");
	CHECK(firstItem(bits).x < 0.0f);

	// Without a compiler (simulated: the probe says so): the line carries the
	// way out, and pressing it opens Tool Setup.
	bits.onLinux(true, false, true, true, true);
	spendAutoOpen(bits);
	exportDump(bits, "export-sdk-no-compiler");
	const ImVec2 at = firstItem(bits);
	REQUIRE(at.x >= 0.0f);
	CHECK_FALSE(toolSetupOpen());
	// Two frames over it first: the empty frame toolSetupOpen() just ran left the
	// host window unhovered, and a press then lands on the window, not the button.
	ImGui::GetIO().AddMousePosEvent(at.x, at.y);
	exportFrame(bits, false);
	exportFrame(bits, false);
	exportFrame(bits, true);
	exportFrame(bits, false);
	exportFrame(bits, false);
	CHECK(toolSetupOpen());
	CHECK(ToolSetupDialog::isShowing());
}

TEST_CASE("Tool setup: opened for an export, it says what waits and closes once the compiler is there")
{
	Harness h;
	ContextBits bits;
	bits.onLinux(true, false, true, true, true);
	spendAutoOpen(bits);
	exportFrame(bits, false);   // isShowing() is about the last frame that drew the dialog
	CHECK_FALSE(ToolSetupDialog::isShowing());

	ToolSetupDialog::requestShowFor(Tool::Compiler, ExportSdkGateUi::kToolSetupPurpose);
	exportFrame(bits, false);
	exportFrame(bits, false);
	CHECK(ToolSetupDialog::isShowing());
	CHECK(std::string(ToolSetupDialog::purpose()) == ExportSdkGateUi::kToolSetupPurpose);
	exportDump(bits, "export-tool-setup-waiting");

	// The compiler arrives (installed, and the recheck found it): the dialog
	// closes itself, the purpose goes with it — the parked export sees
	// isShowing() false and the gate Ready, and starts.
	bits.tc.compilerFound = true;
	bits.tc.compilerId    = "GNU 13.2.0";
	exportFrame(bits, false);
	exportFrame(bits, false);
	exportFrame(bits, false);
	CHECK_FALSE(ToolSetupDialog::isShowing());
	CHECK(std::string(ToolSetupDialog::purpose()).empty());

	// A plain requestShow (the startup check, Preferences) carries no purpose.
	bits.tc.compilerFound = false;
	ToolSetupDialog::requestShow(Tool::Compiler);
	exportFrame(bits, false);
	CHECK(ToolSetupDialog::isShowing());
	CHECK(std::string(ToolSetupDialog::purpose()).empty());
}
