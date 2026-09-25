#include "doctest.h"
#include "ImGuiSoftwareRaster.h"

#include "UIEditorPanel.h"
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "EditorTheme.h"
#include "EditorUndo.h"
#include "EditorWidgets.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <HorizonScene/HorizonWorld.h>
#include <UIWidget/UIElements.h>
#include <UIWidget/UIWidgetTree.h>

#include <imgui.h>
#include <imgui_internal.h>   // the window list, to find the hierarchy's rows

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// ── The widget designer, driven headless ─────────────────────────────────────
// The real UIEditorPanel::render with a real AppContext and a real widget asset,
// in a headless ImGui context, rasterised by the software renderer the other
// "ui shot" scenes use. Written for Thema 92 (the designer's Details panel is
// to become easier to read): so the panel as it IS can be looked at, and so
// every intermediate state of that work can be shown as a picture before it
// lands, not described.
//
//     HE_UI_DUMP_DIR=/tmp/ui ./he_tests -tc="ui shot: widget designer*"
//     scripts/he_uishot.py OUT --filter "ui shot: widget designer*"
//
// What it cannot show: the texture on the canvas. The designer draws it from
// the thumbnail cache, which needs a GPU renderer to upload into; headless there
// is none, so the Image element draws its empty placeholder. The DETAILS side,
// which is what these shots are for, is complete.

using namespace HE::Ed;

namespace
{
	// Wide enough for the designer's three columns (230 + canvas + 300), and
	// tall enough that the Details column of a selected Image fits without
	// scrolling — the length of that column is half of what is being judged.
	constexpr int W = 1280, H = 1600;

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

	// Everything AppContext insists on being given (same shape as
	// test_inspector_ui.cpp).
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

		AppContext make(HorizonWorld& world, EditorUndo& undo)
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
			ctx.world   = &world;
			ctx.undoSys = &undo;
			return ctx;
		}
	};

	// A small but typical page: a background panel, a picture, a title and a
	// button. The picture is the element the Thema is about.
	HE::UIWidgetTree samplePage()
	{
		HE::UIWidgetTree t;
		t.canvasWidth = 1280.0f; t.canvasHeight = 720.0f;
		auto place = [&](HE::UIWidgetType type, const char* name,
		                 float x, float y, float w, float h) -> HE::UIElement&
		{
			HE::UIElement& e = *t.find(t.add(type));
			e.name = name;
			HE::uiSetAnchorPreset(e, 0);
			e.pivotX = e.pivotY = 0.0f;
			e.posX = x; e.posY = y; e.sizeX = w; e.sizeY = h;
			return e;
		};
		place(HE::UIWidgetType::Panel,  "Background", 0.0f,   0.0f,   1280.0f, 720.0f);
		HE::UIElement& logo = place(HE::UIWidgetType::Image, "Logo", 540.0f, 120.0f, 200.0f, 200.0f);
		logo.texture = "Textures/Logo.hasset";
		place(HE::UIWidgetType::Text,   "Title",      440.0f, 360.0f, 400.0f, 60.0f);
		place(HE::UIWidgetType::Button, "Start",      540.0f, 460.0f, 200.0f, 56.0f);
		return t;
	}

	struct Designer
	{
		AppContext& ctx;
		std::string assetPath;   // absolute, the key the panel's state is held by

		// One frame of the designer filling the whole screen. Returns the id
		// ImGui says the pointer is on; rasterises into `shot` when given one.
		ImGuiID frame(bool left, he_ui::Image* shot = nullptr)
		{
			ImGuiIO& io = ImGui::GetIO();
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, left);
			ImGui::NewFrame();
			UIEditorPanel::render(ctx, assetPath, ImVec2(0.0f, 0.0f), ImVec2(float(W), float(H)));
			EditorWidgets::drawQueuedHelp();
			const ImGuiID hovered = ImGui::GetHoveredID();
			ImGui::Render();
			if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
			return hovered;
		}

		// The id a row of the hierarchy tree carries: its label hashed into the
		// tree child's own id seed. Roots are drawn straight into that child,
		// without a PushID between, so this is the whole derivation.
		ImGuiID hierarchyRowId(const std::string& label) const
		{
			for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
				if (std::strstr(w->Name, "##uiw_tree"))
					return ImHashStr(label.c_str(), 0, w->ID);
			return 0;
		}

		// Click the row whose id is `wanted`, found by walking the pointer down
		// the hierarchy column. False when the walk never met it.
		bool clickRow(ImGuiID wanted)
		{
			if (wanted == 0) return false;
			ImGuiIO& io = ImGui::GetIO();
			for (float y = 40.0f; y < float(H) - 20.0f; y += 3.0f)
			{
				io.AddMousePosEvent(60.0f, y);
				frame(false);
				if (frame(false) != wanted) continue;
				frame(true);
				frame(false);
				frame(false);
				io.AddMousePosEvent(-1000.0f, -1000.0f);   // out of the way of the shot
				frame(false);
				frame(false);
				return true;
			}
			return false;
		}

		he_ui::Image shoot(const char* name)
		{
			he_ui::Image img;
			for (int i = 0; i < 3; ++i) frame(false, i == 2 ? &img : nullptr);
			if (const char* dir = std::getenv("HE_UI_DUMP_DIR"); dir && *dir)
				he_ui::writeBmp(img, std::string(dir) + "/" + name + ".bmp");
			return img;
		}
	};

	std::filesystem::path tempRoot()
	{
		return std::filesystem::temp_directory_path() / "he_widget_designer_ui";
	}
}

TEST_CASE("ui shot: widget designer — the Details panel as it is (Thema 92)")
{
	Harness harness;

	const std::filesystem::path root = tempRoot();
	std::filesystem::create_directories(root / "UI");
	ContentManager cm;
	cm.setContentRoot(root.string());

	UIWidgetAsset asset;
	asset.name     = "MainMenu";
	asset.path     = "UI/MainMenu.hasset";
	asset.treeJson = HE::uiWidgetTreeToJson(samplePage());
	REQUIRE(cm.registerWidget(std::move(asset)) != HE::UUID{});

	HorizonWorld world;
	EditorUndo   undo;
	ContextBits  bits;
	AppContext   ctx = bits.make(world, undo);
	ctx.contentManager = &cm;

	Designer d{ ctx, (root / "UI" / "MainMenu.hasset").string() };

	SUBCASE("nothing selected: the canvas settings")
	{
		const he_ui::Image img = d.shoot("widget-designer-canvas");
		REQUIRE(img.valid());
		CHECK(img.inkedPixels(20, 18, 15) > 10000);
	}

	SUBCASE("the Image selected: its Details column in full")
	{
		d.shoot("widget-designer-warmup");   // lay the hierarchy out once
		const ImGuiID logoRow = d.hierarchyRowId("Logo##hn2");
		REQUIRE(logoRow != 0);
		REQUIRE(d.clickRow(logoRow));
		const he_ui::Image img = d.shoot("widget-designer-image-details");
		REQUIRE(img.valid());
		CHECK(img.inkedPixels(20, 18, 15) > 10000);
	}

	UIEditorPanel::forget(d.assetPath);
	std::error_code ec;
	std::filesystem::remove_all(root, ec);
}
