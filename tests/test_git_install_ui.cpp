#include "doctest.h"
#include "ImGuiSoftwareRaster.h"

#include "GitMissingDialog.h"
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"

#include <SourceControl/GitInstall.h>

#include <imgui.h>
#include <imgui_internal.h>   // FindWindowByName, ImGuiWindow::GetID

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

// ── The "git is missing" remedy, driven headless ─────────────────────────────
// GitMissingDialog::drawGitInstallRemedy is what every place that needs git
// shows when there is none. Three things matter, per install plan: "Install
// Git" is there exactly when the editor can run the route; pressing it reaches
// the editor as startGitInstall and nothing else; and when the editor cannot
// install (no rights, no package manager, Windows without winget) the manual
// way is there instead — the command to copy where there is one, the download
// page always. Nothing here installs anything: startGitInstall is a counter.
// Same harness as the recovery dialog test; HE_UI_DUMP_DIR writes the frames.

using namespace HE::Ed;
using namespace HE::Sc;

namespace
{
	constexpr int W = 640, H = 420;
	constexpr const char* kWindow = "##GitRemedyHost";

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
		}
		~Harness() { ImGui::DestroyContext(); }
	};

	// Everything AppContext insists on being given; the remedy reads only the
	// probe and the git-install members.
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

		// The machine as the probe saw it, and what the editor would do.
		GitProbe    probe;
		int         installs = 0, logReads = 0, cancels = 0;
		std::string log;
		bool        installing = false, done = false, ok = false;

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
			ctx.gitProbe        = &probe;
			ctx.startGitInstall = [this]{ ++installs; };
			ctx.cancelGitInstall = [this]{ ++cancels; };
			ctx.gitInstallLog   = [this]{ ++logReads; return log; };
			ctx.gitInstalling   = installing;
			ctx.gitInstallDone  = done;
			ctx.gitInstallOk    = ok;
			return ctx;
		}

		void plan(HostPlatform platform, std::set<std::string> tools, bool isRoot = false)
		{
			probe = GitProbe{};
			probe.install = planGitInstall(platform,
				[&tools](std::string_view t) { return tools.count(std::string(t)) != 0; }, isRoot);
		}
	};

	ImGuiID frame(ContextBits& bits, bool mouseDown, he_ui::Image* shot = nullptr)
	{
		AppContext ctx = bits.make();
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(float(W), float(H)));
		ImGui::Begin(kWindow, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		                               ImGuiWindowFlags_NoSavedSettings);
		{
			// The column the startup dialog wraps at, so the shots read like it.
			EditorWidgets::WrapText wrap(560.0f);
			GitMissingDialog::drawGitInstallRemedy(ctx);
		}
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

	// The id a labelled control in the host window has, the way ImGui hashes it.
	ImGuiID idOf(const char* label)
	{
		ImGuiWindow* w = ImGui::FindWindowByName(kWindow);
		REQUIRE(w != nullptr);
		return w->GetID(label);
	}

	// Every hoverable item on screen, with a point inside it. Coarse grid — the
	// buttons are far bigger than a step — and only the top part, where the
	// remedy draws.
	std::map<ImGuiID, ImVec2> itemsOnScreen(ContextBits& bits)
	{
		std::map<ImGuiID, ImVec2> found;
		for (float y = 4.0f; y < 260.0f; y += 5.0f)
			for (float x = 4.0f; x < float(W) - 4.0f; x += 10.0f)
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
}

TEST_CASE("Git remedy: a route the editor can run gets Install Git, and it starts the install")
{
	Harness h;
	ContextBits bits;
	bits.plan(HostPlatform::MacOS, { "brew", "xcode-select" });
	REQUIRE(gitInstallLaunch(bits.probe.install).has_value());

	frame(bits, false);
	const auto items = itemsOnScreen(bits);
	REQUIRE(items.count(idOf("Install Git")) == 1);
	CHECK(items.count(idOf("Copy Install Command")) == 1);   // "brew install git" by hand
	CHECK(items.count(idOf("Download Page")) == 1);
	dump(bits, "git-remedy-homebrew");

	CHECK(bits.installs == 0);   // drawing it installs nothing
	clickAt(bits, items.at(idOf("Install Git")));
	CHECK(bits.installs == 1);
}

TEST_CASE("Git remedy: without rights or a package manager there is no button, only the manual way")
{
	Harness h;
	ContextBits bits;

	SUBCASE("Linux with apt but no pkexec: the sudo command to copy")
	{
		bits.plan(HostPlatform::Linux, { "apt-get" });
		REQUIRE_FALSE(bits.probe.install.canInstallAutomatically());
		frame(bits, false);
		const auto items = itemsOnScreen(bits);
		CHECK(items.count(idOf("Install Git")) == 0);
		CHECK(items.count(idOf("Copy Install Command")) == 1);
		CHECK(items.count(idOf("Download Page")) == 1);
		dump(bits, "git-remedy-linux-no-pkexec");
	}
	SUBCASE("Windows without winget: the installer's download page")
	{
		bits.plan(HostPlatform::Windows, {});
		REQUIRE(bits.probe.install.route == GitInstallRoute::DownloadInstaller);
		frame(bits, false);
		const auto items = itemsOnScreen(bits);
		CHECK(items.count(idOf("Install Git")) == 0);
		CHECK(items.count(idOf("Copy Install Command")) == 0);   // no command exists
		CHECK(items.count(idOf("Download Page")) == 1);
		dump(bits, "git-remedy-windows-no-winget");
	}
	SUBCASE("No route at all")
	{
		bits.plan(HostPlatform::MacOS, {});
		frame(bits, false);
		const auto items = itemsOnScreen(bits);
		CHECK(items.count(idOf("Install Git")) == 0);
		CHECK(items.count(idOf("Download Page")) == 1);
	}
	CHECK(bits.installs == 0);
}

TEST_CASE("Git remedy: while installing, the button is off and the installer's output is shown")
{
	Harness h;
	ContextBits bits;
	bits.plan(HostPlatform::Linux, { "apt-get", "pkexec" });
	bits.installing = true;
	bits.log = "Installing git through apt.\nA system dialog asks for an administrator password.\n"
	           "$ pkexec /bin/sh -c 'export DEBIAN_FRONTEND=noninteractive; apt-get update && "
	           "apt-get install -y git'\nReading package lists...\n";

	frame(bits, false);
	CHECK(bits.logReads > 0);   // the log is asked for every frame while it runs
	dump(bits, "git-remedy-installing");
	// A running apt cannot be stopped, so nothing offers to.
	CHECK(itemsOnScreen(bits).count(idOf("Cancel")) == 0);

	// Disabled, so pressing where it is does nothing — the button itself is
	// still drawn, so its place is found the same way.
	bits.installing = false;
	const auto items = itemsOnScreen(bits);
	REQUIRE(items.count(idOf("Install Git")) == 1);
	bits.installing = true;
	clickAt(bits, items.at(idOf("Install Git")));
	CHECK(bits.installs == 0);

	// A finished install that left no runnable git keeps the log and says so.
	bits.installing = false;
	bits.done = true;
	bits.ok = false;
	bits.logReads = 0;
	frame(bits, false);
	CHECK(bits.logReads > 0);
	dump(bits, "git-remedy-install-failed");
}

TEST_CASE("Git remedy: the wait for Apple's installer can be cancelled")
{
	Harness h;
	ContextBits bits;
	bits.plan(HostPlatform::MacOS, { "xcode-select" });
	REQUIRE(gitInstallLaunch(bits.probe.install)->returnsBeforeInstalled);

	// Not before the install runs: there is nothing to stop yet.
	frame(bits, false);
	CHECK(itemsOnScreen(bits).count(idOf("Cancel")) == 0);

	bits.installing = true;
	bits.log = "Installing git through xcode-select.\nWaiting for Apple's installer to finish\xe2\x80\xa6\n";
	frame(bits, false);
	const auto items = itemsOnScreen(bits);
	REQUIRE(items.count(idOf("Cancel")) == 1);
	dump(bits, "git-remedy-clt-waiting");
	clickAt(bits, items.at(idOf("Cancel")));
	CHECK(bits.cancels == 1);
	CHECK(bits.installs == 0);
}
