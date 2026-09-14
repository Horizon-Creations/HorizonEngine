#include "doctest.h"
#include "ImGuiSoftwareRaster.h"

#include "TerrainTools.h"
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "EditorUndo.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"

#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/FoliageSystem.h>
#include <HorizonScene/FoliagePaint.h>
#include <HorizonScene/Components/FoliageComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonRendering/RenderWorld.h>
#include <ContentManager/DefaultAssets.h>

#include <imgui.h>
#include <imgui_internal.h>   // GetHoveredID / FindWindowByName: which item the pointer is on
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// ── The Landscape panel's Foliage brush, driven headless ─────────────────────
// FoliagePaint is covered on its own in test_foliage.cpp; what that cannot say
// is whether the Landscape panel actually arms it and whether a drag in the
// Scene viewport reaches it — the mode well, the Grow/Erase well, the
// unproject from the pointer to the ground, the per-stroke undo. So this drives
// the real TerrainTools (renderPanel + sculptInViewport, with a real AppContext)
// in a headless ImGui context, the way test_outliner_ui.cpp drives the
// Outliner: find the cells by asking ImGui what is under the pointer, click
// them, drag over a top-down viewport, and read the layer's mask afterwards.
// With HE_UI_DUMP_DIR set the panel is written out as well.

using namespace HE::Ed;

namespace
{
	constexpr int W = 360, H = 520;

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

	// Everything AppContext insists on being given, in one place (the same
	// shape as the Outliner test's). The panel reads editorConfig.mode, world
	// and undoSys; contentManager and renderer stay null, which every path
	// the brush takes tolerates.
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
			config.mode = EditorMode::Landscape;
			return ctx;
		}
	};

	constexpr const char* kPanelWindow = "Landscape###Quick Settings";

	// One frame of the panel at a fixed place, with the pointer where the
	// caller put it. Returns the id ImGui says the pointer is on.
	ImGuiID panelFrame(AppContext& ctx, bool mouseDown, he_ui::Image* shot = nullptr)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
		ImGui::SetNextWindowSize(ImVec2(float(W) - 20.0f, float(H) - 20.0f));
		ImGui::Begin(kPanelWindow, nullptr,
		             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
		TerrainTools::renderPanel(ctx);
		ImGui::End();
		EditorWidgets::drawQueuedHelp();
		const ImGuiID hovered = ImGui::GetHoveredID();
		ImGui::Render();
		if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
		return hovered;
	}

	ImGuiID idAt(AppContext& ctx, float x, float y)
	{
		ImGui::GetIO().AddMousePosEvent(x, y);
		panelFrame(ctx, false);
		return panelFrame(ctx, false);
	}

	void clickAt(AppContext& ctx, float x, float y)
	{
		ImGui::GetIO().AddMousePosEvent(x, y);
		panelFrame(ctx, false);
		panelFrame(ctx, false);
		panelFrame(ctx, true);
		panelFrame(ctx, false);
		panelFrame(ctx, false);
	}

	// The id a widget label resolves to inside the panel window. The toolbar
	// cells are InvisibleButtons under their "##id", a button is its label.
	ImGuiID panelId(const char* label)
	{
		ImGuiWindow* w = ImGui::FindWindowByName(kPanelWindow);
		REQUIRE(w != nullptr);
		return w->GetID(label);
	}

	// Where an item is, found the way a user finds it: by moving the pointer
	// over the panel until ImGui reports that id. Coarse grid first, so the
	// scan stays a few thousand frames.
	bool locate(AppContext& ctx, ImGuiID id, float& outX, float& outY)
	{
		for (float y = 14.0f; y < float(H) - 14.0f; y += 4.0f)
			for (float x = 14.0f; x < float(W) - 14.0f; x += 6.0f)
				if (idAt(ctx, x, y) == id) { outX = x; outY = y; return true; }
		return false;
	}

	void dump(const he_ui::Image& img, const char* name)
	{
		if (const char* dir = std::getenv("HE_UI_DUMP_DIR"); dir && *dir)
			he_ui::writeBmp(img, std::string(dir) + "/" + name + ".bmp");
	}

	// A top-down camera over the terrain's origin: the viewport's centre pixel
	// unprojects to world (0, 0, 0), and every pixel to a known ground spot.
	RenderWorld topDownSnapshot(float height)
	{
		RenderWorld rw;
		rw.camera.view = glm::lookAt(glm::vec3(0.0f, height, 0.0f), glm::vec3(0.0f),
		                             glm::vec3(0.0f, 0.0f, -1.0f));
		rw.camera.projection = glm::perspective(glm::radians(60.0f),
		                                        float(W) / float(H), 0.1f, 1000.0f);
		return rw;
	}

	// One viewport frame with the pointer at (x, y) and the button as given:
	// the Scene window fills the display, the brush code draws into it.
	void viewportFrame(AppContext& ctx, const RenderWorld& snap, float x, float y, bool down)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMousePosEvent(x, y);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(float(W), float(H)));
		ImGui::Begin("Scene", nullptr,
		             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
		             ImGuiWindowFlags_NoBackground);
		TerrainTools::sculptInViewport(ctx, snap, ImVec2(0.0f, 0.0f), ImVec2(float(W), float(H)),
		                               /*navigating=*/false, /*viewportHovered=*/true,
		                               /*dt=*/1.0f, [](const HE::UUID&) {});
		ImGui::End();
		ImGui::Render();
	}
}

TEST_CASE("landscape ui: the Foliage brush arms from the panel and a drag erases the ground under it")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	auto& reg = world.registry();

	// A flat 100 m landscape at the origin, with no foliage layer yet.
	const Entity terrain = world.createEntity("Terrain");
	reg.emplace<TransformComponent>(terrain);
	TerrainComponent tc;
	tc.sizeX = 100.0f; tc.sizeZ = 100.0f; tc.seed = 0;
	reg.emplace<TerrainComponent>(terrain, tc);

	ContextBits bits;
	AppContext ctx = bits.make(world, undo);

	// Settle, and a look at the Sculpt state for the record.
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	he_ui::Image img;
	for (int i = 0; i < 4; ++i) panelFrame(ctx, false, i == 3 ? &img : nullptr);
	REQUIRE(img.valid());

	// ── Arm the Foliage mode ──
	float fx = 0.0f, fy = 0.0f;
	REQUIRE_MESSAGE(locate(ctx, panelId("##lsFoliage"), fx, fy), "no Foliage cell in the mode well");
	clickAt(ctx, fx, fy);

	// Without a layer the panel offers to add one — and does.
	float ax = 0.0f, ay = 0.0f;
	REQUIRE_MESSAGE(locate(ctx, panelId("Add Foliage Layer"), ax, ay),
	                "Foliage mode without a layer shows no Add Foliage Layer button");
	CHECK_FALSE(reg.all_of<FoliageComponent>(terrain));
	clickAt(ctx, ax, ay);
	REQUIRE(reg.all_of<FoliageComponent>(terrain));
	CHECK(undo.canUndo());
	auto& fol = reg.get<FoliageComponent>(terrain);
	fol.meshAssetId = HE::kDefaultCubeMeshId;
	fol.density     = 0.2f;
	fol.dirty       = true;
	FoliageSystem::update(world);
	const size_t uniformCount = fol.cachedInstances.size();
	REQUIRE(uniformCount == 2000u);

	// The brush controls are there now: Grow armed, Erase beside it.
	float ex = 0.0f, ey = 0.0f;
	REQUIRE_MESSAGE(locate(ctx, panelId("##folErase"), ex, ey), "no Erase cell in the brush well");
	float gx = 0.0f, gy = 0.0f;
	REQUIRE(locate(ctx, panelId("##folGrow"), gx, gy));
	CHECK(ex > gx);
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 3; ++i) panelFrame(ctx, false, i == 2 ? &img : nullptr);
	dump(img, "landscape-foliage-panel");

	// ── Erase: a drag at the viewport's centre clears the ground at the origin ──
	clickAt(ctx, ex, ey);
	const RenderWorld snap = topDownSnapshot(120.0f);
	const float cx = float(W) * 0.5f, cy = float(H) * 0.5f;
	CHECK(fol.densityMask.empty());
	viewportFrame(ctx, snap, cx, cy, false);
	viewportFrame(ctx, snap, cx, cy, false);
	// Strength 5 at dt 1 is 0.8 per frame; a dozen frames drive the mask to
	// exactly 0 (the residue snap), the way a held button does.
	for (int i = 0; i < 12; ++i) viewportFrame(ctx, snap, cx, cy, true);
	viewportFrame(ctx, snap, cx, cy, false);

	REQUIRE_FALSE(fol.densityMask.empty());
	CHECK(fol.dirty);
	const auto& tcc = reg.get<TerrainComponent>(terrain);
	CHECK(FoliagePaint::sample(fol, tcc, 0.0f, 0.0f) == doctest::Approx(0.0f));
	// The default brush is 10 m + 5 m falloff: 40 m out is untouched.
	CHECK(FoliagePaint::sample(fol, tcc, 40.0f, 40.0f) == doctest::Approx(1.0f));
	CHECK(FoliagePaint::coverage(fol) < 1.0f);
	CHECK(FoliagePaint::coverage(fol) > 0.9f);

	// The stroke re-scatters: fewer instances, none inside the erased circle.
	FoliageSystem::update(world);
	CHECK(fol.cachedInstances.size() < uniformCount);
	CHECK(!fol.cachedInstances.empty());
	for (const auto& m : fol.cachedInstances)
		CHECK(m[3].x * m[3].x + m[3].z * m[3].z >= 9.0f * 9.0f);

	// One undo step for the whole stroke, and it brings the instances back.
	// Restoring remaps entity handles, so the terrain is looked up again.
	REQUIRE(undo.canUndo());
	REQUIRE(undo.undo());
	const Entity terrain2 = reg.view<TerrainComponent>().front();
	REQUIRE((terrain2 != entt::null));
	{
		auto& after = reg.get<FoliageComponent>(terrain2);
		CHECK(after.densityMask.empty());
		after.dirty = true;
		FoliageSystem::update(world);
		CHECK(after.cachedInstances.size() == uniformCount);
	}

	// ── Grow: paint a meadow INTO bare ground ──
	// Target Density is the panel's slider, at its default of 100 % here; what
	// this checks is the stroke's shape — full inside the 10 m radius, fading
	// over the 5 m falloff, nothing beyond.
	{
		auto& f2 = reg.get<FoliageComponent>(terrain2);
		const auto& tc2 = reg.get<TerrainComponent>(terrain2);
		FoliagePaint::fillMask(f2, 0.0f);
		clickAt(ctx, gx, gy);
		// After ONE frame the falloff is visible: 0.8 at the centre, about
		// half that on the ring, nothing outside. Holding on converges the
		// whole brush to the target, ring included — that is the lerp.
		viewportFrame(ctx, snap, cx, cy, true);
		CHECK(FoliagePaint::sample(f2, tc2, 0.0f, 0.0f) == doctest::Approx(0.8f).epsilon(0.02));
		const float ring = FoliagePaint::sample(f2, tc2, 12.5f, 0.0f);
		CHECK(ring > 0.2f);
		CHECK(ring < 0.6f);
		CHECK(FoliagePaint::sample(f2, tc2, 40.0f, 40.0f) == doctest::Approx(0.0f));
		for (int i = 0; i < 11; ++i) viewportFrame(ctx, snap, cx, cy, true);
		viewportFrame(ctx, snap, cx, cy, false);
		CHECK(FoliagePaint::sample(f2, tc2, 0.0f, 0.0f) == doctest::Approx(1.0f));
		CHECK(FoliagePaint::sample(f2, tc2, 40.0f, 40.0f) == doctest::Approx(0.0f));
		FoliageSystem::update(world);
		CHECK(!f2.cachedInstances.empty());
		for (const auto& m : f2.cachedInstances)
			CHECK(m[3].x * m[3].x + m[3].z * m[3].z <= 16.0f * 16.0f);

		// Sculpting never ran: a foliage stroke does not bake heights.
		CHECK(tc2.sculptHeights.empty());
	}

	// Back to Sculpt, so the file-static mode does not leak into another test.
	float sx = 0.0f, sy = 0.0f;
	REQUIRE(locate(ctx, panelId("##lsSculpt"), sx, sy));
	clickAt(ctx, sx, sy);
	REQUIRE_FALSE(locate(ctx, panelId("##folErase"), ex, ey));
}
