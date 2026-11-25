#include "doctest.h"
#include "ImGuiSoftwareRaster.h"

#include "ProjectHubPanel.h"
#include "ProjectThumbnail.h"
#include "ProjectPreflight.h"
#include "GitCloneDialog.h"
#include "TutorialPanel.h"
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"
#ifdef __APPLE__
#include "MacMenuBar.h"
#endif

#include <Application/AppIcon.h>
#include <Diagnostics/GlobalState.h>

#include <imgui.h>
#include <imgui_internal.h>   // GetHoveredID

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

// ── The Project Hub, driven headless ─────────────────────────────────────────
// Two columns: Projects (Browse / Clone, then one picture card per recent
// project) and New Project (template tiles, then the form). What matters: a
// card shows the picture the project was closed on, a project without one gets
// the placeholder, a missing one says so and does not open; a tile click picks
// that template; Clone reaches the clone dialog. HE_UI_DUMP_DIR writes the
// frames as BMP (scripts/he_uishot.py converts).

// ── Link stubs ──────────────────────────────────────────────────────────────
// The hub calls into the open pipeline, the clone dialog, the tutorial and the
// macOS menu; none of their sources are in this binary (they pull in EditorUI
// and the platform), so the calls are recorded here instead. If one of those
// sources is ever compiled into he_tests, delete its stub.
namespace
{
	std::vector<std::string> g_requested;
	int g_cloneOpens = 0;
}
namespace ProjectPreflight
{
	void request(AppContext&, const std::string& projectFile) { g_requested.push_back(projectFile); }
	bool busy() { return false; }
}
namespace GitCloneDialog
{
	void requestOpen() { ++g_cloneOpens; }
	bool takeOpenRequest(std::string&) { return false; }
}
namespace TutorialPanel { void showWelcome() {} }
#ifdef __APPLE__
namespace MacMenuBar
{
	void install() {}
	bool available() { return false; }
	void setProjectLoaded(bool) {}
	Cmd  take() { return Cmd::None; }
}
#endif

namespace fs = std::filesystem;

namespace
{
	constexpr const char* kHub = "##ProjectHub";

	struct Harness
	{
		ImFont* body = nullptr; ImFont* sub = nullptr; ImFont* head = nullptr;
		Harness(int w, int h)
		{
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(float(w), float(h));
			io.DeltaTime   = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
			io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
#ifdef HE_EDITOR_DEPS_DIR
			// The editor's own font at the editor's three sizes, so the shot is the hub.
			const std::string font = std::string(HE_EDITOR_DEPS_DIR) + "/Fonts/Roboto_Condensed-Bold.ttf";
			if (std::FILE* f = std::fopen(font.c_str(), "rb"))
			{
				std::fclose(f);
				ImFontConfig cfg;
				cfg.OversampleH = 2;
				cfg.OversampleV = 2;
				body = io.Fonts->AddFontFromFileTTF(font.c_str(), 16.0f, &cfg);
				sub  = io.Fonts->AddFontFromFileTTF(font.c_str(), 19.0f, &cfg);
				head = io.Fonts->AddFontFromFileTTF(font.c_str(), 23.0f, &cfg);
				io.FontDefault = body;
			}
#endif
			HE::Ed::applyHorizonDarkTheme();
			ProjectHubPanel::setTextureHooks(
				[](const void* px, int w, int h)
				{ return reinterpret_cast<void*>(static_cast<std::uintptr_t>(he_ui::registerTexture(px, w, h))); },
				[](void* t) { he_ui::unregisterTexture(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(t))); });
		}
		~Harness()
		{
			ProjectHubPanel::releaseTextures(nullptr);
			ProjectHubPanel::setTextureHooks({}, {});
			ImGui::DestroyContext();
		}
	};

	struct ContextBits
	{
		Harness*        harness = nullptr;
		EditorConfig    config;
		bool            vsync = false;
		std::string     backendName = "Software";
		EditorSelection selection;
		std::string     scenePath;
		bool            exitRequested = false, projectLoaded = false;
		bool            refreshPending = false, refreshDone = false;
		int   fpsOffset = 0, fpsCount = 0;
		float fpsAccum = 0.0f, smoothFps = 0.0f;
		std::vector<AppContext::EditorTab> tabs;
		int   activeTab = 0;
		float cbTreeWidth = 200.0f;
		int   hubPreset = 0, hubLang = 0;
		bool  hubFx = true;
		std::string hubCreateError, hubOpenError;
		int   hubRemoveIndex = -1;
		bool  hubRemoveRequested = false;
		std::string dirResult, fileResult;
		bool  dirReady = false, fileReady = false;
		char  name[256] = "Catania";
		char  dir[512]  = "~/HorizonEngineProjects";
		ProjectManager projects;

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
			ctx.globalState    = &GlobalState::getInstance();
			ctx.projectManager = &projects;
			ctx.hubProjectName = name; ctx.hubProjectNameSize = sizeof(name);
			ctx.hubProjectDir  = dir;  ctx.hubProjectDirSize  = sizeof(dir);
			if (harness)
			{
				ctx.fontBody = harness->body; ctx.fontSubheading = harness->sub; ctx.fontHeading = harness->head;
			}
			return ctx;
		}
	};

	ImGuiID frame(ContextBits& bits, bool mouseDown, he_ui::Image* shot = nullptr)
	{
		AppContext ctx = bits.make();
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
		ImGui::NewFrame();
		ProjectHubPanel::render(ctx);
		EditorWidgets::drawQueuedHelp();
		const ImGuiID hovered = ImGui::GetHoveredID();
		ImGui::Render();
		if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), int(io.DisplaySize.x), int(io.DisplaySize.y));
		return hovered;
	}

	ImGuiID idAt(ContextBits& bits, float x, float y)
	{
		ImGui::GetIO().AddMousePosEvent(x, y);
		frame(bits, false);
		return frame(bits, false);
	}

	// The distinct hoverable items along a line, in order, with a point in each.
	std::vector<ImVec2> itemsAlong(ContextBits& bits, ImVec2 from, ImVec2 step, int n)
	{
		std::vector<ImVec2> found;
		std::set<ImGuiID> seen;
		ImGuiID last = 0;
		for (int i = 0; i < n; ++i)
		{
			const ImVec2 p(from.x + step.x * i, from.y + step.y * i);
			const ImGuiID id = idAt(bits, p.x, p.y);
			if (id != 0 && id != last && seen.insert(id).second) found.push_back(p);
			last = id;
		}
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
		CHECK(img.inkedPixels(20, 18, 15) > 1000);
		if (const char* d = std::getenv("HE_UI_DUMP_DIR"); d && *d)
			he_ui::writeBmp(img, std::string(d) + "/" + name + ".bmp");
	}

	// A viewport-looking frame: sky over ground with a sun, w×h.
	std::vector<std::uint8_t> fakeViewport(std::uint32_t w, std::uint32_t h)
	{
		std::vector<std::uint8_t> px(size_t(w) * h * 4);
		for (std::uint32_t y = 0; y < h; ++y)
			for (std::uint32_t x = 0; x < w; ++x)
			{
				std::uint8_t* p = &px[(size_t(y) * w + x) * 4];
				const float fy = float(y) / h, dx = float(x) - w * 0.7f, dy = float(y) - h * 0.35f;
				if (dx * dx + dy * dy < (h * 0.08f) * (h * 0.08f)) { p[0] = 250; p[1] = 210; p[2] = 120; }
				else if (fy < 0.6f) { p[0] = std::uint8_t(60 + 120 * fy); p[1] = std::uint8_t(110 + 80 * fy); p[2] = 200; }
				else { p[0] = 70; p[1] = std::uint8_t(110 - 40 * fy); p[2] = 50; }
				p[3] = 7;   // the viewport's alpha is junk; the thumbnail must not keep it
			}
		return px;
	}

	// Three recent projects in a scratch folder: one closed with a picture, one
	// never closed, one deleted. Taken off the (non-persistent) list again after.
	struct Projects
	{
		fs::path root, withPic, noPic, missing;
		Projects()
		{
			root = fs::temp_directory_path() / "he_hub_ui_test";
			fs::remove_all(root);
			auto make = [&](const char* n)
			{
				fs::create_directories(root / n);
				const fs::path p = root / n / (std::string(n) + ".heproj");
				std::ofstream(p) << "{}";
				return p;
			};
			withPic = make("Catania");
			noPic   = make("Schach");
			missing = root / "Gone" / "Gone.heproj";
			const auto vp = fakeViewport(1600, 900);
			REQUIRE(ProjectThumbnail::save(withPic.string(), vp, 1600, 900));
			GlobalState& gs = GlobalState::getInstance();
			gs.setConfigPersistent(false);
			gs.addKnownProject(missing.string());
			gs.addKnownProject(noPic.string());
			gs.addKnownProject(withPic.string());
		}
		~Projects()
		{
			GlobalState& gs = GlobalState::getInstance();
			for (const fs::path& p : { withPic, noPic, missing }) gs.removeKnownProject(p.string());
			std::error_code ec;
			fs::remove_all(root, ec);
		}
	};
}

TEST_CASE("Project thumbnail: cover-crop to 16:9, opaque, flat frames are not saved")
{
	// 4:3, left half red, right half blue: the crop keeps the full width and
	// trims top and bottom, so both halves survive at the same split.
	const std::uint32_t w = 640, h = 480;
	std::vector<std::uint8_t> px(size_t(w) * h * 4);
	for (std::uint32_t y = 0; y < h; ++y)
		for (std::uint32_t x = 0; x < w; ++x)
		{
			std::uint8_t* p = &px[(size_t(y) * w + x) * 4];
			p[0] = x < w / 2 ? 255 : 0; p[1] = 0; p[2] = x < w / 2 ? 0 : 255; p[3] = 0;
		}
	std::vector<std::uint8_t> out;
	REQUIRE(ProjectThumbnail::fit(px, w, h, out));
	REQUIRE(out.size() == size_t(ProjectThumbnail::kWidth) * ProjectThumbnail::kHeight * 4);
	const auto at = [&](int x, int y) { return &out[(size_t(y) * ProjectThumbnail::kWidth + x) * 4]; };
	CHECK(at(10, 135)[0] == 255);  CHECK(at(10, 135)[2] == 0);
	CHECK(at(470, 135)[0] == 0);   CHECK(at(470, 135)[2] == 255);
	CHECK(at(10, 135)[3] == 255);
	CHECK_FALSE(ProjectThumbnail::isFlat(out));
	CHECK_FALSE(ProjectThumbnail::fit(px, 0, 0, out));

	// Round trip through the file, and an application's empty viewport is not written.
	const fs::path dir = fs::temp_directory_path() / "he_thumb_test";
	fs::remove_all(dir);
	fs::create_directories(dir);
	const std::string proj = (dir / "P.heproj").string();
	CHECK(ProjectThumbnail::fileFor(proj) == dir / "Saved" / "ProjectThumbnail.png");
	REQUIRE(ProjectThumbnail::save(proj, px, w, h));
	std::vector<std::uint8_t> back;
	int bw = 0, bh = 0;
	REQUIRE(HE::heLoadPngRGBA(ProjectThumbnail::fileFor(proj), back, bw, bh));
	CHECK(bw == ProjectThumbnail::kWidth);
	CHECK(bh == ProjectThumbnail::kHeight);
	CHECK(back == out);

	fs::remove(ProjectThumbnail::fileFor(proj));
	const std::vector<std::uint8_t> flat(size_t(64) * 36 * 4, 40);
	CHECK_FALSE(ProjectThumbnail::save(proj, flat, 64, 36));
	CHECK_FALSE(fs::exists(ProjectThumbnail::fileFor(proj)));
	fs::remove_all(dir);
}

TEST_CASE("Project Hub: cards open their project, tiles pick a template, Clone reaches the dialog")
{
	Harness h(1280, 800);
	Projects pr;
	ContextBits bits;
	bits.harness = &h;
	g_requested.clear();
	g_cloneOpens = 0;
	frame(bits, false);
	dump(bits, "project_hub_1280x800");

	// Down the left edge of the Projects column: Browse, then the first card of
	// each row (two cards per row at this width: Catania | Schach, then Gone).
	const auto left = itemsAlong(bits, ImVec2(40.0f, 70.0f), ImVec2(0.0f, 6.0f), 120);
	REQUIRE(left.size() >= 3);

	clickAt(bits, left[1]);
	REQUIRE(g_requested.size() == 1);
	CHECK(g_requested[0] == pr.withPic.string());

	clickAt(bits, left[2]);   // "Gone": not found, does not open
	CHECK(g_requested.size() == 1);

	// Clone sits right of Browse on the same row.
	const auto buttons = itemsAlong(bits, ImVec2(30.0f, left[0].y), ImVec2(8.0f, 0.0f), 64);
	REQUIRE(buttons.size() >= 2);
	clickAt(bits, buttons[1]);
	CHECK(g_cloneOpens == 1);

	// The template tiles: down the first tile column finds the two groups' first
	// tiles (Empty Project, Application); across the first row, Game is second.
	const float tilesX = std::floor(1280 * 0.42f) + 24.0f + 12.0f;
	const auto tiles = itemsAlong(bits, ImVec2(tilesX, 70.0f), ImVec2(0.0f, 6.0f), 60);
	REQUIRE(tiles.size() >= 2);
	const auto row = itemsAlong(bits, ImVec2(tilesX, tiles[0].y), ImVec2(10.0f, 0.0f), 70);
	REQUIRE(row.size() >= 2);
	clickAt(bits, row[1]);
	CHECK(bits.hubPreset == static_cast<int>(ProjectPreset::Game));
	clickAt(bits, tiles[1]);
	CHECK(bits.hubPreset == static_cast<int>(ProjectPreset::Application));
	dump(bits, "project_hub_application_selected");
}

TEST_CASE("Project Hub: a small window keeps every template tile on screen")
{
	Harness h(1000, 620);
	Projects pr;
	ContextBits bits;
	bits.harness = &h;
	frame(bits, false);
	dump(bits, "project_hub_1000x620");

	// Down the first tile column: both groups' first tiles are there without
	// scrolling (the form below may scroll; the choice of template may not).
	const float tilesX = std::floor(1000 * 0.42f) + 24.0f + 12.0f;
	const auto tiles = itemsAlong(bits, ImVec2(tilesX, 70.0f), ImVec2(0.0f, 6.0f), 90);
	CHECK(tiles.size() >= 2);
}
