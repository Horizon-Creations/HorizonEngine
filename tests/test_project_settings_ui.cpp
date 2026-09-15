#include "doctest.h"
#include "ImGuiSoftwareRaster.h"
#include "TestFsUtil.h"

#include "ProjectSettingsPanel.h"
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include "ProjectManager.h"
#include <Project/ProjectSettings.h>

#include <imgui.h>
#include <imgui_internal.h>   // GetHoveredID: which item the pointer is on

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

// ── The Project Settings tab, driven headless ────────────────────────────────
// The tab is a rail and nine pages over a real ProjectData. What can be
// checked without a person: that every page draws over a real project (and
// that none of them writes a settings file merely by being LOOKED at — a
// project that never touched its settings must not grow one), and that one
// real edit — the first checkbox of Rendering ▸ Defaults — reaches
// Config/ProjectSettings.json through the ProjectManager. With HE_UI_DUMP_DIR
// set every page is written out as well (scripts/he_uishot.py turns them into
// PNGs), which is how the layout was looked at.

using namespace HE::Ed;

namespace
{
	constexpr int W = 900, H = 620;

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

	// Everything AppContext insists on being given. The tab reads
	// projectManager; the rest is here because the struct has no defaults for
	// its references.
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

		AppContext make(ProjectManager& pm)
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
			ctx.projectManager = &pm;
			return ctx;
		}
	};

	// One frame of the tab filling the display, with the pointer where the
	// caller put it. Returns the id ImGui says the pointer is on.
	ImGuiID frame(AppContext& ctx, bool mouseDown, he_ui::Image* shot = nullptr)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
		ImGui::NewFrame();
		ProjectSettingsPanel::render(ctx, ImVec2(0.0f, 0.0f), ImVec2(float(W), float(H)));
		EditorWidgets::drawQueuedHelp();
		const ImGuiID hovered = ImGui::GetHoveredID();
		ImGui::Render();
		if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
		return hovered;
	}

	ImGuiID idAt(AppContext& ctx, float x, float y)
	{
		ImGui::GetIO().AddMousePosEvent(x, y);
		frame(ctx, false);
		return frame(ctx, false);
	}

	void clickAt(AppContext& ctx, float x, float y)
	{
		ImGui::GetIO().AddMousePosEvent(x, y);
		frame(ctx, false);
		frame(ctx, false);
		frame(ctx, true);
		frame(ctx, false);
		frame(ctx, false);
	}

	// Switch the tab to a page the way EditorUI would: raise the request and
	// consume it (the tab strip does the consuming in the editor).
	void showPage(ProjectSettingsPanel::Page page)
	{
		ProjectSettingsPanel::requestOpen(page);
		ProjectSettingsPanel::takeOpenRequest();
	}

	struct Item { ImGuiID id; float mid; };

	// Every distinct item the pointer crosses walking DOWN at x. The page is a
	// child window, and the space between two controls is the child itself —
	// ImGui reports its id as hovered there — so anything seen more than once
	// is background and dropped.
	std::vector<Item> itemsDown(AppContext& ctx, float x, float y0, float y1)
	{
		std::vector<Item> raw;
		ImGuiID last = 0; float start = y0;
		for (float y = y0; y < y1; y += 2.0f)
		{
			const ImGuiID id = idAt(ctx, x, y);
			if (id == last) continue;
			if (last != 0) raw.push_back({ last, (start + y - 2.0f) * 0.5f });
			last = id; start = y;
		}
		if (last != 0) raw.push_back({ last, (start + y1) * 0.5f });
		std::vector<Item> out;
		for (const Item& it : raw)
		{
			int n = 0;
			for (const Item& o : raw) n += o.id == it.id;
			if (n == 1) out.push_back(it);
		}
		return out;
	}

	constexpr std::uint8_t kBgR = 20, kBgG = 18, kBgB = 15;

	// The rail is 210 px wide; the page's controls start right of it.
	constexpr float kPageX = 240.0f;
}

TEST_CASE("project settings ui: every page draws over a real project, and looking writes nothing")
{
	Harness harness;

	const auto dir = std::filesystem::temp_directory_path() / "he_project_settings_ui";
	he_test::removeAllQuiet(dir);
	ProjectManager pm;
	REQUIRE(pm.createNewProject(dir.string(), "Settings", ProjectPreset::Game));
	const auto settingsFile = HE::projectSettingsPath(pm.projectRoot());

	ContextBits bits;
	AppContext ctx = bits.make(pm);

	using P = ProjectSettingsPanel::Page;
	// Fonts is left out on purpose: its page asks the shared UI font whether
	// the project's weight is live, which bakes the atlas in this process —
	// and test_ui_font_weight has to be the one run that bakes it.
	const struct { P page; const char* name; } pages[] = {
		{ P::General,         "general" },
		{ P::Application,     "application" },
		{ P::Permissions,     "permissions" },
		{ P::RenderDefaults,  "render-defaults" },
		{ P::Shadows,         "shadows" },
		{ P::Simulation,      "physics" },
		{ P::CollisionLayers, "collision-layers" },
		{ P::AudioBuses,      "audio-buses" },
	};
	const char* dump = std::getenv("HE_UI_DUMP_DIR");
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (const auto& pg : pages)
	{
		showPage(pg.page);
		he_ui::Image img;
		for (int i = 0; i < 4; ++i) frame(ctx, false, i == 3 ? &img : nullptr);
		REQUIRE(img.valid());
		// The rail alone is a few hundred inked pixels; a page with a heading,
		// a hint and at least one control is thousands.
		CHECK_MESSAGE(img.inkedPixels(kBgR, kBgG, kBgB) > 4000, pg.name);
		if (dump && *dump)
			he_ui::writeBmp(img, std::string(dump) + "/project-settings-" + pg.name + ".bmp");
	}
	CHECK_FALSE(std::filesystem::exists(settingsFile));
	CHECK(pm.currentProject().settings.isDefault());

	he_test::removeAllQuiet(dir);
}

TEST_CASE("project settings ui: the first edit on Rendering > Defaults lands in Config/ProjectSettings.json")
{
	Harness harness;

	const auto dir = std::filesystem::temp_directory_path() / "he_project_settings_ui_edit";
	he_test::removeAllQuiet(dir);
	ProjectManager pm;
	REQUIRE(pm.createNewProject(dir.string(), "Settings", ProjectPreset::Game));
	const auto settingsFile = HE::projectSettingsPath(pm.projectRoot());

	ContextBits bits;
	AppContext ctx = bits.make(pm);
	showPage(ProjectSettingsPanel::Page::RenderDefaults);
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 3; ++i) frame(ctx, false);

	// Down the page's left edge: the first control is "Use the editor's
	// settings", and the checkbox square is the first thing on its row.
	const std::vector<Item> items = itemsDown(ctx, kPageX + 8.0f, 40.0f, float(H) - 40.0f);
	REQUIRE_MESSAGE(!items.empty(), "no control found down the Render Defaults page");
	REQUIRE(pm.currentProject().settings.renderDefaults.useEditorSettings);

	clickAt(ctx, kPageX + 8.0f, items[0].mid);
	CHECK_FALSE(pm.currentProject().settings.renderDefaults.useEditorSettings);
	REQUIRE(std::filesystem::exists(settingsFile));

	// …and the file, read back through a fresh manager, says so.
	ProjectManager again;
	REQUIRE(again.loadProject(pm.currentProject().path));
	CHECK_FALSE(again.currentProject().settings.renderDefaults.useEditorSettings);

	he_test::removeAllQuiet(dir);
}
