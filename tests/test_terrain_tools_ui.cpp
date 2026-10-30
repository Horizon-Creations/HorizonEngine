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
#include <HorizonScene/LayerMeshPresets.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/TerrainMeshGenerator.h>   // terrainHeightAt
#include <HorizonScene/WaterField.h>             // the Water brush's result
#include <HorizonScene/WaterBrush.h>             // ... painted through its own API
#include <HorizonScene/WaterLake.h>              // ... and turned into a lake
#include <HorizonScene/Components/SplineComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonRendering/RenderWorld.h>
#include <ContentManager/DefaultAssets.h>
#include <ContentManager/ContentManager.h>
#include <HorizonScene/TerrainPaint.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <MaterialGraph/AutoLandscapeMaterial.h>

#include <imgui.h>
#include <imgui_internal.h>   // GetHoveredID / FindWindowByName: which item the pointer is on
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
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
// The Mountain area tool is driven the same way further down.
// With HE_UI_DUMP_DIR set the panel is written out as well.

using namespace HE::Ed;
namespace water = HE::water;

namespace
{
	constexpr int W = 360, H = 520;
	// The panel window is taller than the viewport frame: the Landscape panel is a card,
	// a segmented tab control, a scrolling body and a pinned brush, and the Foliage and
	// Mountain pages need the room to show every control without scrolling. The viewport
	// frames keep W x H, so the pixel-to-metre scale the stroke tests rely on is untouched.
	constexpr int PH = 760;

	struct Harness
	{
		Harness()
		{
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(float(W), float(PH));
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
		ImGui::SetNextWindowSize(ImVec2(float(W) - 20.0f, float(PH) - 20.0f));
		ImGui::Begin(kPanelWindow, nullptr,
		             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
		TerrainTools::renderPanel(ctx);
		ImGui::End();
		EditorWidgets::drawQueuedHelp();
		const ImGuiID hovered = ImGui::GetHoveredID();
		ImGui::Render();
		if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, PH);
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

	// A widget label inside the panel. The panel's own items (the card, the tab control)
	// live in the window itself; the tab's content and the pinned brush live in two child
	// windows, and an id is hashed against the window it was submitted in - so a label
	// can match under any of the three. An item inside an open TreeNode (the Mountain
	// page's "More" group) is hashed against the node's id as well, so `inTree` names
	// that node by its full label, "##" suffix included.
	struct PanelRef { const char* label; const char* inTree = nullptr; };
	PanelRef panelId(const char* label, const char* inTree = nullptr) { return { label, inTree }; }

	ImGuiID refIdIn(ImGuiWindow* w, const PanelRef& ref)
	{
		if (!ref.inTree) return w->GetID(ref.label);
		return ImHashStr(ref.label, 0, w->GetID(ref.inTree));
	}

	bool idMatches(ImGuiID hovered, const PanelRef& ref)
	{
		ImGuiWindow* w = ImGui::FindWindowByName(kPanelWindow);
		REQUIRE(w != nullptr);
		if (hovered == refIdIn(w, ref)) return true;
		for (ImGuiWindow* c : w->DC.ChildWindows)
			if (c && hovered == refIdIn(c, ref)) return true;
		return false;
	}

	// Where an item is, found the way a user finds it: by moving the pointer
	// over the panel until ImGui reports that id. Coarse grid first, so the
	// scan stays a few thousand frames.
	bool locate(AppContext& ctx, PanelRef ref, float& outX, float& outY)
	{
		for (float y = 14.0f; y < float(PH) - 14.0f; y += 4.0f)
			for (float x = 14.0f; x < float(W) - 14.0f; x += 6.0f)
				if (idMatches(idAt(ctx, x, y), ref)) { outX = x; outY = y; return true; }
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
	// `invalidateMeshAabb` stands in for the viewport's picking-cache hook.
	void viewportFrame(AppContext& ctx, const RenderWorld& snap, float x, float y, bool down,
	                   const std::function<void(const HE::UUID&)>& invalidateMeshAabb =
	                       [](const HE::UUID&) {})
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
		                               /*dt=*/1.0f, invalidateMeshAabb);
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
	fol.layers[0].meshAssetId = HE::kDefaultCubeMeshId;
	fol.layers[0].density     = 0.2f;
	fol.dirty       = true;
	FoliageSystem::update(world);
	const size_t uniformCount = fol.layers[0].cachedInstances.size();
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
	CHECK(fol.layers[0].densityMask.empty());
	viewportFrame(ctx, snap, cx, cy, false);
	viewportFrame(ctx, snap, cx, cy, false);
	// Strength 5 at dt 1 is 0.8 per frame; a dozen frames drive the mask to
	// exactly 0 (the residue snap), the way a held button does.
	for (int i = 0; i < 12; ++i) viewportFrame(ctx, snap, cx, cy, true);
	viewportFrame(ctx, snap, cx, cy, false);

	REQUIRE_FALSE(fol.layers[0].densityMask.empty());
	CHECK(fol.layers[0].dirty);
	const auto& tcc = reg.get<TerrainComponent>(terrain);
	CHECK(FoliagePaint::sample(fol.layers[0], tcc, 0.0f, 0.0f) == doctest::Approx(0.0f));
	// The default brush is 10 m + 5 m falloff: 40 m out is untouched.
	CHECK(FoliagePaint::sample(fol.layers[0], tcc, 40.0f, 40.0f) == doctest::Approx(1.0f));
	CHECK(FoliagePaint::coverage(fol.layers[0]) < 1.0f);
	CHECK(FoliagePaint::coverage(fol.layers[0]) > 0.9f);

	// The stroke re-scatters: fewer instances, none inside the erased circle.
	FoliageSystem::update(world);
	CHECK(fol.layers[0].cachedInstances.size() < uniformCount);
	CHECK(!fol.layers[0].cachedInstances.empty());
	for (const auto& m : fol.layers[0].cachedInstances)
		CHECK(m[3].x * m[3].x + m[3].z * m[3].z >= 9.0f * 9.0f);

	// One undo step for the whole stroke, and it brings the instances back.
	// Restoring remaps entity handles, so the terrain is looked up again.
	REQUIRE(undo.canUndo());
	REQUIRE(undo.undo());
	const Entity terrain2 = reg.view<TerrainComponent>().front();
	REQUIRE((terrain2 != entt::null));
	{
		auto& after = reg.get<FoliageComponent>(terrain2);
		CHECK(after.layers[0].densityMask.empty());
		after.dirty = true;
		FoliageSystem::update(world);
		CHECK(after.layers[0].cachedInstances.size() == uniformCount);
	}

	// ── Grow: paint a meadow INTO bare ground ──
	// Target Density is the panel's slider, at its default of 100 % here; what
	// this checks is the stroke's shape — full inside the 10 m radius, fading
	// over the 5 m falloff, nothing beyond.
	{
		auto& f2 = reg.get<FoliageComponent>(terrain2);
		const auto& tc2 = reg.get<TerrainComponent>(terrain2);
		FoliagePaint::fillMask(f2.layers[0], 0.0f);
		clickAt(ctx, gx, gy);
		// After ONE frame the falloff is visible: 0.8 at the centre, about
		// half that on the ring, nothing outside. Holding on converges the
		// whole brush to the target, ring included — that is the lerp.
		viewportFrame(ctx, snap, cx, cy, true);
		CHECK(FoliagePaint::sample(f2.layers[0], tc2, 0.0f, 0.0f) == doctest::Approx(0.8f).epsilon(0.02));
		const float ring = FoliagePaint::sample(f2.layers[0], tc2, 12.5f, 0.0f);
		CHECK(ring > 0.2f);
		CHECK(ring < 0.6f);
		CHECK(FoliagePaint::sample(f2.layers[0], tc2, 40.0f, 40.0f) == doctest::Approx(0.0f));
		for (int i = 0; i < 11; ++i) viewportFrame(ctx, snap, cx, cy, true);
		viewportFrame(ctx, snap, cx, cy, false);
		CHECK(FoliagePaint::sample(f2.layers[0], tc2, 0.0f, 0.0f) == doctest::Approx(1.0f));
		CHECK(FoliagePaint::sample(f2.layers[0], tc2, 40.0f, 40.0f) == doctest::Approx(0.0f));
		FoliageSystem::update(world);
		CHECK(!f2.layers[0].cachedInstances.empty());
		for (const auto& m : f2.layers[0].cachedInstances)
			CHECK(m[3].x * m[3].x + m[3].z * m[3].z <= 16.0f * 16.0f);

		// Sculpting never ran: a foliage stroke does not bake heights.
		CHECK(tc2.sculptHeights.empty());
	}

	// ── A second layer: the panel adds it and selects it, the brush paints it alone ──
	{
		auto& f3 = reg.get<FoliageComponent>(terrain2);
		const std::vector<uint8_t> firstMask = f3.layers[0].densityMask;
		float lx = 0.0f, ly = 0.0f;
		REQUIRE_MESSAGE(locate(ctx, panelId("Add Foliage Layer##more"), lx, ly),
		                "Foliage mode with a layer shows no button for another one");
		clickAt(ctx, lx, ly);
		REQUIRE(f3.layers.size() == 2u);
		CHECK(f3.activeIndex() == 1u);
		CHECK(f3.layers[1].seed != f3.layers[0].seed);
		CHECK(f3.layers[1].densityMask.empty());

		// The list grew by a row, so the mode cells moved: look for Erase again.
		ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
		for (int i = 0; i < 2; ++i) panelFrame(ctx, false);
		float e2x = 0.0f, e2y = 0.0f;
		REQUIRE(locate(ctx, panelId("##folErase"), e2x, e2y));
		clickAt(ctx, e2x, e2y);
		for (int i = 0; i < 4; ++i) viewportFrame(ctx, snap, cx, cy, true);
		viewportFrame(ctx, snap, cx, cy, false);
		REQUIRE_FALSE(f3.layers[1].densityMask.empty());
		CHECK(FoliagePaint::sample(f3.layers[1], reg.get<TerrainComponent>(terrain2), 0.0f, 0.0f) < 0.5f);
		CHECK(f3.layers[0].densityMask == firstMask);   // the first layer did not see the stroke
	}

	// Back to Sculpt, so the file-static mode does not leak into another test.
	float sx = 0.0f, sy = 0.0f;
	REQUIRE(locate(ctx, panelId("##lsSculpt"), sx, sy));
	clickAt(ctx, sx, sy);
	REQUIRE_FALSE(locate(ctx, panelId("##folErase"), ex, ey));
}

// ── The Mountain area tool: arm it, drag an area, let go ─────────────────────
// TerrainGenerate::mountain has its own tests (test_terrain_generate.cpp) for
// the shape of what it grows. This one asks what only the editor can answer:
// does the panel arm the tool and show its form instead of the brush numbers,
// does a drag in the viewport mark the area it should (corner to corner, or
// centre to rim) without touching the ground while the button is held, and is
// the release one undo step that invalidates the picking cache.
TEST_CASE("landscape ui: the Mountain tool grows a formation in the dragged area on release")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	auto& reg = world.registry();

	// A flat 100 m landscape at the origin whose heights are already baked, the
	// way Create Landscape leaves them — with an empty field the brush path
	// could never run, and "the ground does not move while the button is held"
	// would be true for the wrong reason. It carries a mesh id so the picking
	// cache hook has something to invalidate.
	const Entity terrain = world.createEntity("Terrain");
	reg.emplace<TransformComponent>(terrain);
	TerrainComponent tc;
	tc.sizeX = 100.0f; tc.sizeZ = 100.0f; tc.seed = 0;
	tc.resolution = 129;
	tc.sculptHeights.assign(129u * 129u, 0.0f);
	reg.emplace<TerrainComponent>(terrain, tc);
	auto flat = [](const TerrainComponent& t)
	{
		return std::all_of(t.sculptHeights.begin(), t.sculptHeights.end(),
		                   [](float h) { return h == 0.0f; });
	};
	MeshComponent mesh;
	mesh.meshAssetId = HE::kDefaultCubeMeshId;
	reg.emplace<MeshComponent>(terrain, mesh);

	ContextBits bits;
	AppContext ctx = bits.make(world, undo);

	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 4; ++i) panelFrame(ctx, false);

	// ── Arm Mountain: its form replaces Radius / Falloff / Strength ──
	float mx = 0.0f, my = 0.0f;
	REQUIRE_MESSAGE(locate(ctx, panelId("##lsTool6"), mx, my), "no Mountain cell in the tool wells");
	clickAt(ctx, mx, my);
	float nx = 0.0f, ny = 0.0f;
	CHECK_MESSAGE(locate(ctx, panelId("##mtRect"), nx, ny), "Mountain armed shows no Rectangle cell");
	CHECK_MESSAGE(locate(ctx, panelId("##mountain_maxh"), nx, ny), "Mountain armed shows no Max Height");
	// New Seed sits in the page's "More" group (open by default), so its id is hashed under that node.
	CHECK_MESSAGE(locate(ctx, panelId("New Seed##mountain", "More (Falloff, Frequency, Seed)##mountainmore"), nx, ny),
	              "Mountain armed shows no New Seed");
	CHECK_FALSE(locate(ctx, panelId("##radius_brush"), nx, ny));
	{
		he_ui::Image img;
		ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
		for (int i = 0; i < 3; ++i) panelFrame(ctx, false, i == 2 ? &img : nullptr);
		REQUIRE(img.valid());
		dump(img, "landscape-mountain-panel");
	}
	const size_t depth0 = undo.undoDepth();

	// Top-down at 120 m with a 60° field of view: 2·120·tan 30° / 520 px is
	// 0.2665 m a pixel, screen right is +X and screen down is +Z. So 75 px
	// right and down from the centre is (20, 20) on the ground.
	const RenderWorld snap = topDownSnapshot(120.0f);
	const float cx = float(W) * 0.5f, cy = float(H) * 0.5f;
	const float px = 75.0f;
	int invalidations = 0;
	HE::UUID invalidated{};
	auto hook = [&](const HE::UUID& id) { ++invalidations; invalidated = id; };

	// ── A click without a drag marks no area and takes no undo step ──
	viewportFrame(ctx, snap, cx, cy, false, hook);
	viewportFrame(ctx, snap, cx, cy, true,  hook);
	viewportFrame(ctx, snap, cx, cy, false, hook);
	CHECK(undo.undoDepth() == depth0);
	CHECK(invalidations == 0);
	CHECK(flat(reg.get<TerrainComponent>(terrain)));

	// ── Rectangle: corner (0, 0) to corner (20, 20) ──
	viewportFrame(ctx, snap, cx, cy, true, hook);
	for (int i = 1; i <= 5; ++i)
		viewportFrame(ctx, snap, cx + px * i / 5.0f, cy + px * i / 5.0f, true, hook);
	// Held: the outline is the preview, the ground has not moved yet.
	CHECK(flat(reg.get<TerrainComponent>(terrain)));
	CHECK(undo.undoDepth() == depth0);
	viewportFrame(ctx, snap, cx + px, cy + px, false, hook);

	{
		const auto& t = reg.get<TerrainComponent>(terrain);
		REQUIRE_FALSE(t.sculptHeights.empty());
		CHECK(undo.undoDepth() == depth0 + 1);
		CHECK(undo.undoLabel() == "Generate Mountain");
		CHECK(invalidations == 1);
		CHECK(invalidated == HE::kDefaultCubeMeshId);

		// The peak is the form's Max Height (30 m by default), inside the
		// ellipse inscribed in the rectangle — centre (10, 10), radii 10.
		const float peak = *std::max_element(t.sculptHeights.begin(), t.sculptHeights.end());
		CHECK(peak == doctest::Approx(30.0f).epsilon(0.001));
		CHECK(terrainHeightAt(t, 10.0f, 10.0f) > 10.0f);
		// Outside the ellipse (with a metre of slack for the pixel mapping)
		// not a vertex moved.
		const uint32_t res = t.resolution;
		const float step = t.sizeX / float(res - 1);
		int outside = 0, moved = 0;
		for (uint32_t zi = 0; zi < res; ++zi)
			for (uint32_t xi = 0; xi < res; ++xi)
			{
				const float x = -50.0f + xi * step, z = -50.0f + zi * step;
				const float ex = (x - 10.0f) / 11.0f, ez = (z - 10.0f) / 11.0f;
				if (ex * ex + ez * ez < 1.0f) continue;
				++outside;
				if (t.sculptHeights[zi * res + xi] != 0.0f) ++moved;
			}
		CHECK(outside > 10000);
		CHECK(moved == 0);
	}

	// One undo takes the whole mountain back.
	REQUIRE(undo.undo());
	const Entity terrain2 = reg.view<TerrainComponent>().front();
	REQUIRE((terrain2 != entt::null));
	CHECK(terrainHeightAt(reg.get<TerrainComponent>(terrain2), 10.0f, 10.0f) == 0.0f);

	// ── Circle: pressed at the centre (0, 0), dragged out 16 m to the rim ──
	float rx = 0.0f, ry = 0.0f;
	REQUIRE(locate(ctx, panelId("##mtCircle"), rx, ry));
	clickAt(ctx, rx, ry);
	const float rpx = 60.0f;   // ≈ 16 m
	viewportFrame(ctx, snap, cx, cy, false);
	viewportFrame(ctx, snap, cx, cy, true);
	for (int i = 1; i <= 4; ++i)
		viewportFrame(ctx, snap, cx + rpx * i / 4.0f, cy, true);
	viewportFrame(ctx, snap, cx + rpx, cy, false);
	{
		const auto& t = reg.get<TerrainComponent>(terrain2);
		REQUIRE_FALSE(t.sculptHeights.empty());
		CHECK(undo.undoLabel() == "Generate Mountain");
		// Round and centred on the press point: raised on both sides of it
		// along both axes, nothing past the 16 m rim in any direction.
		CHECK(terrainHeightAt(t, 0.0f, 0.0f) > 5.0f);
		const uint32_t res = t.resolution;
		const float step = t.sizeX / float(res - 1);
		float left = 0.0f, right = 0.0f, north = 0.0f, south = 0.0f;
		int moved = 0;
		for (uint32_t zi = 0; zi < res; ++zi)
			for (uint32_t xi = 0; xi < res; ++xi)
			{
				const float x = -50.0f + xi * step, z = -50.0f + zi * step;
				const float h = t.sculptHeights[zi * res + xi];
				const float d = std::sqrt(x * x + z * z);
				if (d >= 17.0f) { if (h != 0.0f) ++moved; continue; }
				if (d < 8.0f) continue;
				if (x < -8.0f) left  = std::max(left,  h);
				if (x >  8.0f) right = std::max(right, h);
				if (z < -8.0f) north = std::max(north, h);
				if (z >  8.0f) south = std::max(south, h);
			}
		CHECK(moved == 0);
		CHECK(left > 1.0f);
		CHECK(right > 1.0f);
		CHECK(north > 1.0f);
		CHECK(south > 1.0f);
	}

	// Back to Rectangle and Raise, so the file-static tool state does not
	// leak into another test.
	REQUIRE(locate(ctx, panelId("##mtRect"), rx, ry));
	clickAt(ctx, rx, ry);
	REQUIRE(locate(ctx, panelId("##lsTool0"), rx, ry));
	clickAt(ctx, rx, ry);
	CHECK(locate(ctx, panelId("##radius_brush"), rx, ry));
}

// ── Setup tab and Ctrl-invert ────────────────────────────────────────────────
// Two things the viewport has to get right that the panel only asks for: the
// Setup tab is a form, so a drag over the viewport while it is open must not
// sculpt; and Ctrl turns the armed brush around (Raise lowers) for as long as it
// is held.
TEST_CASE("landscape ui: the Setup tab switches the brush off, and Ctrl turns Raise into Lower")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	auto& reg = world.registry();

	const Entity terrain = world.createEntity("Terrain");
	reg.emplace<TransformComponent>(terrain);
	TerrainComponent tc;
	tc.sizeX = 100.0f; tc.sizeZ = 100.0f; tc.seed = 0;
	tc.resolution = 129;
	tc.sculptHeights.assign(129u * 129u, 0.0f);
	reg.emplace<TerrainComponent>(terrain, tc);
	MeshComponent mesh;
	mesh.meshAssetId = HE::kDefaultCubeMeshId;
	reg.emplace<MeshComponent>(terrain, mesh);

	ContextBits bits;
	AppContext ctx = bits.make(world, undo);
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 4; ++i) panelFrame(ctx, false);

	auto extremes = [&] {
		const auto& t = reg.get<TerrainComponent>(terrain);
		float lo = 0.0f, hi = 0.0f;
		for (float h : t.sculptHeights) { lo = std::min(lo, h); hi = std::max(hi, h); }
		return std::pair<float, float>(lo, hi);
	};
	const RenderWorld snap = topDownSnapshot(120.0f);
	const float cx = float(W) * 0.5f, cy = float(H) * 0.5f;
	auto drag = [&] {
		viewportFrame(ctx, snap, cx, cy, false);
		viewportFrame(ctx, snap, cx, cy, false);
		for (int i = 0; i < 4; ++i) viewportFrame(ctx, snap, cx, cy, true);
		viewportFrame(ctx, snap, cx, cy, false);
	};

	// ── Setup: the form is there, the brush is not ──
	float sx = 0.0f, sy = 0.0f;
	REQUIRE_MESSAGE(locate(ctx, panelId("##lsSetup"), sx, sy), "no Setup cell in the tab well");
	clickAt(ctx, sx, sy);
	float bx = 0.0f, by = 0.0f;
	CHECK_MESSAGE(locate(ctx, panelId("Reset Sculpting"), bx, by), "Setup shows no Reset Sculpting");
	CHECK_FALSE(locate(ctx, panelId("##radius_brush"), bx, by));
	const size_t depth0 = undo.undoDepth();
	drag();
	CHECK(extremes().first == 0.0f);
	CHECK(extremes().second == 0.0f);
	CHECK(undo.undoDepth() == depth0);

	// ── Back on Sculpt, Raise: the same drag lifts the ground ──
	REQUIRE(locate(ctx, panelId("##lsSculpt"), sx, sy));
	clickAt(ctx, sx, sy);
	CHECK_FALSE(locate(ctx, panelId("Reset Sculpting"), bx, by));
	REQUIRE(locate(ctx, panelId("##lsTool0"), bx, by));
	clickAt(ctx, bx, by);
	drag();
	CHECK(extremes().second > 1.0f);
	CHECK(extremes().first == 0.0f);

	// ── Ctrl held: Raise lowers ──
	// The panel reads io.KeyCtrl. Under ConfigMacOSXBehaviors (the default on Apple)
	// AddKeyEvent swaps Ctrl and Cmd, so ImGuiMod_Ctrl would arrive as Super - and
	// Super + left click is aliased into a RIGHT click, so the drag would never be a
	// left drag. The key that ends up as io.KeyCtrl there is ImGuiMod_Super (the Cmd
	// key, the platform's primary modifier).
	const ImGuiKey ctrlKey = ImGui::GetIO().ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
	// The ground is already lifted by the Raise drag above and the inverted stroke is the
	// same size the other way, so ONE Ctrl drag takes the hill back to flat; only a second
	// one digs below zero. Asserting the dig after a single drag would be asserting that
	// a Lower stroke is stronger than a Raise one.
	ImGui::GetIO().AddKeyEvent(ctrlKey, true);
	drag();
	viewportFrame(ctx, snap, cx, cy, false);
	CHECK(extremes().second < 0.5f);
	CHECK(extremes().first > -0.5f);
	drag();
	ImGui::GetIO().AddKeyEvent(ctrlKey, false);
	viewportFrame(ctx, snap, cx, cy, false);
	CHECK(extremes().first < -0.5f);
}

// ── The Water brush: arm it, drag, hold Ctrl, undo ───────────────────────────
// The stroke logic (which body, which level, the eraser, the dig) has its own
// tests in test_water_brush.cpp. This one asks what only the Landscape tool can
// answer: does the panel arm the Water tab and show its form instead of the
// sculpt tools, does a drag in the viewport leave water where the pointer went,
// is a whole stroke ONE undo step, does Ctrl erase (as it turns every brush
// around), does a water stroke leave the terrain's own rebuild flags alone, and
// does Dig Bed lower the ground.
TEST_CASE("landscape ui: the Water brush paints a drag as one undo step, Ctrl erases, Dig Bed lowers the ground")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	auto& reg = world.registry();

	// A flat 100 m landscape at the origin, a built one: dirty off, a 2ⁿ+1 grid, so
	// "the terrain wants a rebuild" can only come from the stroke.
	const Entity terrain = world.createEntity("Terrain");
	reg.emplace<TransformComponent>(terrain);
	TerrainComponent tc;
	tc.sizeX = 100.0f; tc.sizeZ = 100.0f; tc.seed = 0;
	tc.resolution = 129;
	tc.dirty = false;
	tc.water.res = 100;                       // a cell is a metre
	reg.emplace<TerrainComponent>(terrain, tc);
	MeshComponent mesh;                       // something for the picking-cache hook to invalidate
	mesh.meshAssetId = HE::kDefaultCubeMeshId;
	reg.emplace<MeshComponent>(terrain, mesh);
	auto land = [&]() -> TerrainComponent&
	{
		auto view = reg.view<TerrainComponent>();
		REQUIRE(view.size() == 1u);
		return reg.get<TerrainComponent>(*view.begin());   // undo mints new handles
	};

	ContextBits bits;
	AppContext ctx = bits.make(world, undo);
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 4; ++i) panelFrame(ctx, false);

	// ── Arm Water: its form replaces the sculpt tools ──
	float wx = 0.0f, wy = 0.0f;
	REQUIRE_MESSAGE(locate(ctx, panelId("##lsWater"), wx, wy), "no Water cell in the tab well");
	clickAt(ctx, wx, wy);
	float fx = 0.0f, fy = 0.0f;
	CHECK_MESSAGE(locate(ctx, panelId("##watErase"), fx, fy), "Water armed shows no Erase cell");
	CHECK_MESSAGE(locate(ctx, panelId("##radius_water"), fx, fy), "Water armed shows no Radius");
	CHECK_MESSAGE(locate(ctx, panelId("From Ground##water"), fx, fy), "Water armed shows no From Ground");
	float digX = 0.0f, digY = 0.0f;
	CHECK_MESSAGE(locate(ctx, panelId("Dig Bed##water"), digX, digY), "Water armed shows no Dig Bed");
	CHECK_FALSE(locate(ctx, panelId("##lsTool0"), fx, fy));          // the sculpt brushes are gone
	{
		he_ui::Image img;
		ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
		for (int i = 0; i < 3; ++i) panelFrame(ctx, false, i == 2 ? &img : nullptr);
		REQUIRE(img.valid());
		dump(img, "landscape-water-panel");
	}
	// Dig Bed is on by default (a pond without a bed is all shore, see its help):
	// off for the first strokes, which are about the water alone.
	clickAt(ctx, digX, digY);
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 3; ++i) panelFrame(ctx, false);
	const size_t depth0 = undo.undoDepth();
	REQUIRE(land().water.bodies.empty());

	// Top-down at 120 m, 60° field of view: 0.2665 m a pixel, +X right, +Z down
	// (see the Mountain test). 75 px is 20 m.
	const RenderWorld snap = topDownSnapshot(120.0f);
	const float cx = float(W) * 0.5f, cy = float(H) * 0.5f;
	const float px = 75.0f;
	int invalidations = 0;
	auto hook = [&](const HE::UUID&) { ++invalidations; };

	// ── A press that misses the landscape takes no undo step ──
	viewportFrame(ctx, snap, -50.0f, -50.0f, false, hook);       // pointer outside the viewport rectangle
	viewportFrame(ctx, snap, -50.0f, -50.0f, true, hook);
	viewportFrame(ctx, snap, -50.0f, -50.0f, false, hook);
	CHECK(undo.undoDepth() == depth0);
	CHECK(land().water.bodies.empty());

	// ── Paint: press at the middle, drag 20 m east, let go ──
	viewportFrame(ctx, snap, cx, cy, false, hook);
	viewportFrame(ctx, snap, cx, cy, true, hook);
	for (int i = 1; i <= 5; ++i)
		viewportFrame(ctx, snap, cx + px * i / 5.0f, cy, true, hook);
	// While the button is held the stroke is already in the model: water appears
	// under the cursor as you drag, no release needed.
	CHECK(land().water.wetCells() > 0u);
	CHECK(undo.undoDepth() == depth0 + 1);                        // one entry, taken when the stroke began
	viewportFrame(ctx, snap, cx + px, cy, false, hook);
	{
		const TerrainComponent& t = land();
		REQUIRE(t.water.bodies.size() == 1);
		CHECK(undo.undoDepth() == depth0 + 1);
		CHECK(undo.undoLabel() == "Paint Water");
		CHECK_FALSE(t.water.bodies[0].fromSpline());
		CHECK(t.water.bodies[0].level == doctest::Approx(0.3f));  // flat ground at 0 plus the default 0.3 m
		const uint16_t id = t.water.bodies[0].id;
		CHECK(water::bodyAt(t, 0.0f, 0.0f)   == id);
		CHECK(water::bodyAt(t, 10.0f, 0.0f)  == id);
		CHECK(water::bodyAt(t, 20.0f, 0.0f)  == id);
		CHECK(water::bodyAt(t, -10.0f, 0.0f) == id);              // radius 10 + falloff 5 behind the press
		CHECK(water::bodyAt(t, 10.0f, 25.0f) == water::kNoBody);
		CHECK(water::bodyAt(t, 40.0f, 40.0f) == water::kNoBody);
		// The landscape's own flags: a water stroke is not a terrain edit.
		CHECK_FALSE(t.dirty);
		CHECK_FALSE(t.regionDirty);
		CHECK(t.sculptHeights.empty());
		CHECK(t.water.dirty);                                      // the sheet is built from this next tick
		CHECK(invalidations == 0);                                 // the ground did not move
	}

	// ── One undo takes the whole stroke back, redo brings it back ──
	const water::Field painted = land().water;
	REQUIRE(undo.undo());
	CHECK(land().water.wetCells() == 0u);
	CHECK(land().water.bodies.empty());
	REQUIRE(undo.redo());
	CHECK(water::sameContent(land().water, painted));

	// ── Ctrl flips Paint to Erase: wipe the middle ──
	// The panel reads io.KeyCtrl. Under ConfigMacOSXBehaviors AddKeyEvent swaps Ctrl
	// and Cmd, and Ctrl + left click is aliased into a RIGHT click, so the key that
	// ends up as io.KeyCtrl there is ImGuiMod_Super (see the Setup / Ctrl test above).
	const ImGuiKey ctrlKey = ImGui::GetIO().ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
	const uint32_t wetBefore = land().water.wetCells();
	const size_t depth1 = undo.undoDepth();
	ImGui::GetIO().AddKeyEvent(ctrlKey, true);
	viewportFrame(ctx, snap, cx + px * 0.5f, cy, false, hook);   // a frame for the modifier to arrive
	viewportFrame(ctx, snap, cx + px * 0.5f, cy, true, hook);
	for (int i = 0; i < 3; ++i)
		viewportFrame(ctx, snap, cx + px * 0.5f, cy, true, hook);
	viewportFrame(ctx, snap, cx + px * 0.5f, cy, false, hook);
	ImGui::GetIO().AddKeyEvent(ctrlKey, false);
	viewportFrame(ctx, snap, cx + px * 0.5f, cy, false, hook);
	{
		const TerrainComponent& t = land();
		CHECK(t.water.wetCells() < wetBefore);
		CHECK(water::bodyAt(t, 10.0f, 0.0f) == water::kNoBody);   // the pointer's spot is dry
		CHECK(water::bodyAt(t, -10.0f, 0.0f) != water::kNoBody);  // the far end of the pond is not
		CHECK(undo.undoDepth() == depth1 + 1);
		CHECK(undo.undoLabel() == "Erase Water");
		CHECK(t.water.bodies.size() == 1);                         // the eraser creates nothing
	}
	REQUIRE(undo.undo());                                          // the erase alone
	CHECK(land().water.wetCells() == wetBefore);

	// ── Dig Bed: a second pond, the ground goes down with it ──
	clickAt(ctx, digX, digY);                                     // the checkbox
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 3; ++i) panelFrame(ctx, false);
	REQUIRE(land().sculptHeights.empty());
	// Undo rebuilt the landscape entity from a snapshot: its terrain comes back
	// "dirty" (a loaded landscape wants its chunks built) and without the mesh
	// component the terrain system owns. Put the test's premise back.
	land().dirty = false;
	{
		const Entity te = reg.view<TerrainComponent>().front();
		if (!reg.all_of<MeshComponent>(te)) reg.emplace<MeshComponent>(te, mesh);
	}
	invalidations = 0;
	const size_t depth2 = undo.undoDepth();
	const float gx = 30.0f, gz = 30.0f;                            // 112 px right and down of the middle
	const float spx = cx + gx / 0.2665f, spy = cy + gz / 0.2665f;
	viewportFrame(ctx, snap, spx, spy, false, hook);
	viewportFrame(ctx, snap, spx, spy, true, hook);
	for (int i = 0; i < 3; ++i) viewportFrame(ctx, snap, spx, spy, true, hook);
	viewportFrame(ctx, snap, spx, spy, false, hook);
	{
		const TerrainComponent& t = land();
		CHECK(undo.undoDepth() == depth2 + 1);
		REQUIRE(t.sculptHeights.size() == 129u * 129u);
		// Water 0.3 m up on flat ground; the bed under its middle has sunk below the
		// ground, and no deeper than Max Depth (2 m by default) under the level.
		CHECK(terrainHeightAt(t, gx, gz) < -0.2f);
		CHECK(terrainHeightAt(t, gx, gz) >= 0.3f - 2.0f * 1.3f);   // roughness may add up to 20 %
		CHECK(terrainHeightAt(t, -40.0f, -40.0f) == 0.0f);         // out of reach, untouched
		CHECK(t.regionDirty);                                      // only the chunks under the brush rebuild
		CHECK_FALSE(t.dirty);
		CHECK(invalidations > 0);                                  // the picking cache learned the ground moved
		CHECK(water::bodyAt(t, gx, gz) != water::kNoBody);
	}
	REQUIRE(undo.undo());                                          // water and pit go together
	CHECK(land().sculptHeights.empty());
	CHECK(water::bodyAt(land(), gx, gz) == water::kNoBody);

	// Back to Sculpt, Dig Bed left on (its default): the file-static mode must not
	// leak into another test.
	REQUIRE(locate(ctx, panelId("##lsSculpt"), wx, wy));
	clickAt(ctx, wx, wy);
	CHECK(locate(ctx, panelId("##lsTool0"), wx, wy));
	CHECK(locate(ctx, panelId("##radius_brush"), wx, wy));
	CHECK_FALSE(locate(ctx, panelId("##radius_water"), wx, wy));
}

TEST_CASE("landscape ui: the Water panel lists the bodies, and To Lake turns a painted pond into a lake in one undo step")
{
	namespace L = HE::water::lake;
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	auto& reg = world.registry();

	const Entity terrain = world.createEntity("Terrain");
	reg.emplace<TransformComponent>(terrain);
	TerrainComponent tc;
	tc.sizeX = 100.0f; tc.sizeZ = 100.0f; tc.seed = 0;
	tc.resolution = 129;
	tc.dirty = false;
	tc.water.res = 100;
	reg.emplace<TerrainComponent>(terrain, tc);
	auto land = [&]() -> TerrainComponent&
	{
		auto view = reg.view<TerrainComponent>();
		REQUIRE(view.size() == 1u);
		return reg.get<TerrainComponent>(*view.begin());
	};

	// A pond painted through the brush's own API, and a second one.
	{
		HE::water::brush::Params p;
		p.radius = 8.0f; p.falloff = 0.0f; p.amount = 1.0f; p.levelFromGround = true; p.levelOffset = 0.5f;
		HE::water::brush::Stroke s;
		HE::water::brush::begin(s, land(), -20.0f, 0.0f, p);
		for (int i = 0; i <= 6; ++i) HE::water::brush::dab(s, land(), -20.0f + 4.0f * i, 0.0f, p);
		HE::water::brush::end(s, land());
		HE::water::brush::begin(s, land(), 35.0f, 30.0f, p);
		HE::water::brush::dab(s, land(), 35.0f, 30.0f, p);
		HE::water::brush::end(s, land());
	}
	REQUIRE(land().water.bodies.size() == 2u);
	const uint16_t pond = land().water.bodies[0].id;
	const float level = land().water.bodies[0].level;

	ContextBits bits;
	AppContext ctx = bits.make(world, undo);
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 4; ++i) panelFrame(ctx, false);

	// Arm Water on the normal-sized panel, then give the panel room: the list of
	// bodies sits below the brush settings.
	float wx = 0.0f, wy = 0.0f;
	REQUIRE(locate(ctx, panelId("##lsWater"), wx, wy));
	clickAt(ctx, wx, wy);
	ImGui::GetIO().DisplaySize = ImVec2(float(W), 1100.0f);
	auto tall = [&](bool down, he_ui::Image* shot = nullptr) -> ImGuiID
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
		ImGui::SetNextWindowSize(ImVec2(float(W) - 20.0f, 1080.0f));
		ImGui::Begin(kPanelWindow, nullptr,
		             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
		TerrainTools::renderPanel(ctx);
		ImGui::End();
		EditorWidgets::drawQueuedHelp();
		const ImGuiID hovered = ImGui::GetHoveredID();
		ImGui::Render();
		if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, 1100);
		return hovered;
	};
	for (int i = 0; i < 3; ++i) tall(false);

	// Two bodies, both ponds: each gets a To Lake button (ids are pushed per body,
	// so the same label is two different items). The tab's content is a child
	// window of the panel, and an id is hashed against the window it was submitted
	// in, so the button's id is the one the child computes.
	float bx = 0.0f, by = 0.0f;
	ImGuiWindow* win = ImGui::FindWindowByName(kPanelWindow);
	REQUIRE(win != nullptr);
	auto isToLake = [&](ImGuiID hovered) -> bool
	{
		for (ImGuiWindow* c : win->DC.ChildWindows)
		{
			if (!c) continue;
			c->IDStack.push_back(c->GetID(static_cast<int>(pond)));
			const ImGuiID id = c->GetID("To Lake");
			c->IDStack.pop_back();
			if (id == hovered) return true;
		}
		return false;
	};
	bool found = false;
	for (float yy = 14.0f; yy < 1090.0f && !found; yy += 3.0f)
	{
		ImGui::GetIO().AddMousePosEvent(30.0f, yy);
		tall(false);
		if (isToLake(tall(false))) { bx = 30.0f; by = yy; found = true; }
	}
	REQUIRE_MESSAGE(found, "no To Lake button for the first pond");
	{
		he_ui::Image img;
		ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, 1090.0f);
		for (int i = 0; i < 3; ++i) tall(false, i == 2 ? &img : nullptr);
		dump(img, "landscape-water-bodies");
	}

	const size_t depth0 = undo.undoDepth();
	const size_t splines0 = reg.view<SplineComponent>().size();
	ImGui::GetIO().AddMousePosEvent(bx, by);
	tall(false); tall(false); tall(true); tall(false); tall(false);

	// ── The pond is a lake now, its spline a child of the landscape ──
	REQUIRE(reg.view<SplineComponent>().size() == splines0 + 1);
	CHECK(undo.undoDepth() == depth0 + 1);
	CHECK(undo.undoLabel() == "Water To Lake");
	const water::Body* b = land().water.findBody(pond);
	REQUIRE(b != nullptr);
	CHECK(b->fromSpline());
	CHECK(b->level == level);                                      // converting does not move the surface
	const Entity sp = *reg.view<SplineComponent>().begin();
	CHECK(reg.get<HierarchyComponent>(sp).parent == reg.view<TerrainComponent>().front());
	CHECK(reg.get<SplineComponent>(sp).closed);
	CHECK(L::linkOf(world, sp).body == pond);
	CHECK(bits.config.mode == EditorMode::Spline);                 // over to the Spline tool, where the points are
	CHECK(ctx.selection.primary() == sp);
	CHECK(land().water.findBody(land().water.bodies[1].id)->fromSpline() == false);   // the other pond is untouched

	// ── One undo gives the pond back as it was ──
	REQUIRE(undo.undo());
	CHECK(reg.view<SplineComponent>().size() == splines0);
	REQUIRE(land().water.findBody(pond) != nullptr);
	CHECK_FALSE(land().water.findBody(pond)->fromSpline());
	CHECK(land().water.bodies.size() == 2u);

	// Leave the file-static mode as the next test expects it.
	bits.config.mode = EditorMode::Landscape;
	ImGui::GetIO().DisplaySize = ImVec2(float(W), float(PH));
	for (int i = 0; i < 4; ++i) panelFrame(ctx, false);
	REQUIRE(locate(ctx, panelId("##lsSculpt"), wx, wy));
	clickAt(ctx, wx, wy);
	CHECK(locate(ctx, panelId("##lsTool0"), wx, wy));
}

// ── Thema 179 step 5: what a body of water does to the bodies in it ───────────
// The Physics rows under each body of the Water tab. What these ask that no model test can:
// is the section on the panel and does it open, does a click on Enabled and a drag on Density
// reach the body of water, is each gesture ONE undo step that takes the number back, and does
// none of it make the landscape rebuild anything (setPhysics, never TerrainComponent::dirty).
TEST_CASE("landscape ui: the Water panel's Physics rows set what a body of water does, one undo step each, nothing rebuilt")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	auto& reg = world.registry();

	const Entity terrain = world.createEntity("Terrain");
	reg.emplace<TransformComponent>(terrain);
	TerrainComponent tc;
	tc.sizeX = 100.0f; tc.sizeZ = 100.0f; tc.seed = 0;
	tc.resolution = 129;
	tc.dirty = false;
	tc.water.res = 100;
	reg.emplace<TerrainComponent>(terrain, tc);
	auto land = [&]() -> TerrainComponent&
	{
		auto view = reg.view<TerrainComponent>();
		REQUIRE(view.size() == 1u);
		return reg.get<TerrainComponent>(*view.begin());
	};

	// One pond, painted through the brush's own API.
	{
		HE::water::brush::Params p;
		p.radius = 8.0f; p.falloff = 0.0f; p.amount = 1.0f; p.levelFromGround = true; p.levelOffset = 0.5f;
		HE::water::brush::Stroke s;
		HE::water::brush::begin(s, land(), -20.0f, 0.0f, p);
		for (int i = 0; i <= 6; ++i) HE::water::brush::dab(s, land(), -20.0f + 4.0f * i, 0.0f, p);
		HE::water::brush::end(s, land());
	}
	REQUIRE(land().water.bodies.size() == 1u);
	const uint16_t pond = land().water.bodies[0].id;
	REQUIRE(land().water.bodies[0].physics == water::BodyPhysics{});
	const bool terrainDirty = land().dirty;

	ContextBits bits;
	AppContext ctx = bits.make(world, undo);
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 4; ++i) panelFrame(ctx, false);

	// Arm Water, then give the panel room: the list of bodies sits below the brush settings.
	float wx = 0.0f, wy = 0.0f;
	REQUIRE(locate(ctx, panelId("##lsWater"), wx, wy));
	clickAt(ctx, wx, wy);
	// Tall enough for the brush settings, the body list AND the opened Physics rows below it.
	constexpr int PT = 1500;
	ImGui::GetIO().DisplaySize = ImVec2(float(W), float(PT));
	auto tall = [&](bool down, he_ui::Image* shot = nullptr) -> ImGuiID
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
		ImGui::SetNextWindowSize(ImVec2(float(W) - 20.0f, float(PT) - 20.0f));
		ImGui::Begin(kPanelWindow, nullptr,
		             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
		TerrainTools::renderPanel(ctx);
		ImGui::End();
		EditorWidgets::drawQueuedHelp();
		const ImGuiID hovered = ImGui::GetHoveredID();
		ImGui::Render();
		if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, PT);
		return hovered;
	};
	for (int i = 0; i < 3; ++i) tall(false);

	// An item under a body's PushID and, when it is inside an open tree node, under the node's
	// id as well. The tab's content is a child window of the panel, and an id is hashed against
	// the window it was submitted in (see the To Lake test above).
	ImGuiWindow* win = ImGui::FindWindowByName(kPanelWindow);
	REQUIRE(win != nullptr);
	auto idOf = [&](ImGuiWindow* c, const char* tree, const char* label) -> ImGuiID
	{
		c->IDStack.push_back(c->GetID(static_cast<int>(pond)));
		if (tree) c->IDStack.push_back(c->GetID(tree));
		const ImGuiID id = c->GetID(label);
		if (tree) c->IDStack.pop_back();
		c->IDStack.pop_back();
		return id;
	};
	auto find = [&](const char* tree, const char* label, float& fx, float& fy) -> bool
	{
		for (float yy = 14.0f; yy < float(PT) - 10.0f; yy += 3.0f)
			for (float xx : { 30.0f, 60.0f, 140.0f, 230.0f, 300.0f })   // 60: inside an indented checkbox
			{
				ImGui::GetIO().AddMousePosEvent(xx, yy);
				tall(false);
				const ImGuiID hovered = tall(false);
				for (ImGuiWindow* c : win->DC.ChildWindows)
					if (c && idOf(c, tree, label) == hovered) { fx = xx; fy = yy; return true; }
			}
		return false;
	};
	const auto parkPointer = [&] { ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(PT) - 6.0f); tall(false); tall(false); };

	// The section is there, closed, and a click opens it.
	float px = 0.0f, py = 0.0f;
	REQUIRE_MESSAGE(find(nullptr, "Physics##wphys", px, py), "no Physics section under the body of water");
	float ex = 0.0f, ey = 0.0f;
	CHECK_FALSE(find("Physics##wphys", "Enabled##wphys", ex, ey));          // closed: no rows yet
	REQUIRE(find(nullptr, "Physics##wphys", px, py));
	ImGui::GetIO().AddMousePosEvent(px, py);
	tall(false); tall(false); tall(true); tall(false); tall(false);
	{
		he_ui::Image img;
		parkPointer();
		tall(false, &img);
		dump(img, "landscape-water-physics-open");
	}
	REQUIRE_MESSAGE(find("Physics##wphys", "Enabled##wphys", ex, ey), "Physics opened but shows no Enabled row");
	float dx = 0.0f, dy = 0.0f;
	REQUIRE_MESSAGE(find("Physics##wphys", "##wphys_density", dx, dy), "no Density row");
	CHECK(find("Physics##wphys", "##wphys_ldrag", wx, wy));
	CHECK(find("Physics##wphys", "##wphys_adrag", wx, wy));
	CHECK(find("Physics##wphys", "##wphys_fx", wx, wy));
	CHECK(find("Physics##wphys", "##wphys_fz", wx, wy));
	CHECK(find("Physics##wphys", "##wphys_splash", wx, wy));
	{
		he_ui::Image img;
		parkPointer();
		tall(false, &img);
		dump(img, "landscape-water-physics");
	}

	const bool waterDirty = land().water.dirty;
	const uint64_t revision0 = land().water.revision;
	const size_t depth0 = undo.undoDepth();

	// ── A click on Enabled: off, one undo step ──
	REQUIRE(find("Physics##wphys", "Enabled##wphys", ex, ey));
	ImGui::GetIO().AddMousePosEvent(ex, ey);
	tall(false); tall(false); tall(true); tall(false); tall(false);
	CHECK_FALSE(land().water.findBody(pond)->physics.enabled);
	CHECK(undo.undoDepth() == depth0 + 1);
	CHECK(undo.undoLabel() == "Water Physics");
	CHECK(land().water.revision > revision0);                      // the physics is told
	// A water setting is not a terrain edit: nothing to rebuild, no chunk to regenerate. (Read
	// before the undo: restoring a scene marks the landscape dirty, as loading one does.)
	CHECK(land().dirty == terrainDirty);
	CHECK(land().water.dirty == waterDirty);
	REQUIRE(undo.undo());
	CHECK(land().water.findBody(pond)->physics.enabled);           // and the click is taken back whole
	CHECK(undo.undoDepth() == depth0);

	// ── A drag on Density: one undo step for the whole drag, however many frames it took ──
	parkPointer();
	REQUIRE(find("Physics##wphys", "##wphys_density", dx, dy));
	const bool terrainDirty2 = land().dirty, waterDirty2 = land().water.dirty;
	ImGui::GetIO().AddMousePosEvent(dx, dy);
	tall(false); tall(false); tall(true);
	for (int i = 1; i <= 6; ++i)
	{
		ImGui::GetIO().AddMousePosEvent(dx + 5.0f * static_cast<float>(i), dy);
		tall(true);
	}
	tall(false); tall(false);
	const float dragged = land().water.findBody(pond)->physics.density;
	CHECK(dragged > 1050.0f);                                      // 5 kg/m3 per pixel, to the right
	CHECK(dragged < 1250.0f);
	CHECK(undo.undoDepth() == depth0 + 1);                         // not one step per frame
	CHECK(undo.undoLabel() == "Water Physics");
	CHECK(land().dirty == terrainDirty2);
	CHECK(land().water.dirty == waterDirty2);
	REQUIRE(undo.undo());
	CHECK(land().water.findBody(pond)->physics.density == water::kDefaultDensity);
	CHECK(land().water.findBody(pond)->physics == water::BodyPhysics{});   // nothing else moved with it

	// Leave the file-static mode as the next test expects it.
	bits.config.mode = EditorMode::Landscape;
	ImGui::GetIO().DisplaySize = ImVec2(float(W), float(PH));
	for (int i = 0; i < 4; ++i) panelFrame(ctx, false);
	REQUIRE(locate(ctx, panelId("##lsSculpt"), wx, wy));
	clickAt(ctx, wx, wy);
	CHECK(locate(ctx, panelId("##lsTool0"), wx, wy));
}

#ifdef HE_EDITOR_DEPS_DIR
// ── Texture groups: the Setup picker and the Paint list (Thema 233, step 3) ───
// A group IS the terrain's landscape material. What this asks of the real panel: the picker lists
// exactly the group table, picking Forest binds Forest's material and moves no weight, the Paint list
// is then Forest's, and a stroke on one row writes only that channel (normalised to 255) at the centre
// of the brush and nothing outside it.
TEST_CASE("landscape ui: the Texture Group picker switches the material only, painting writes the chosen channel")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	auto& reg = world.registry();

	ContentManager cm(".");
	cm.setEngineContentRoot((std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent").string());
	for (const HE::AutoLandscapeGroup& g : HE::kAutoLandscapeGroups)
		REQUIRE_MESSAGE(cm.loadAsset(g.materialPath) == g.materialId, g.name);

	const Entity terrain = world.createEntity("Terrain");
	reg.emplace<TransformComponent>(terrain);
	TerrainComponent tc;
	tc.sizeX = 100.0f; tc.sizeZ = 100.0f; tc.seed = 0; tc.weightRes = 64;
	TerrainPaint::ensureWeightmap(tc);
	TerrainPaint::paint(tc, 30.0f, 30.0f, 2, 8.0f, 4.0f, 1.0f);   // some paint to keep
	reg.emplace<TerrainComponent>(terrain, tc);
	MaterialComponent mc;
	mc.materialAssetId = HE::kAutoLandscapeMaterialId;              // a scene from before groups
	reg.emplace<MaterialComponent>(terrain, mc);

	ContextBits bits;
	AppContext ctx = bits.make(world, undo);
	ctx.contentManager = &cm;
	const std::vector<uint8_t> weights0 = tc.layerWeights;

	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 4; ++i) panelFrame(ctx, false);

	// ── Setup: one cell per row of the group table, and nothing else ──
	float sx = 0.0f, sy = 0.0f;
	REQUIRE(locate(ctx, panelId("##lsSetup"), sx, sy));
	clickAt(ctx, sx, sy);
	float gx[HE::kAutoLandscapeGroupCount], gy[HE::kAutoLandscapeGroupCount];
	for (int i = 0; i < HE::kAutoLandscapeGroupCount; ++i)
	{
		const std::string id = std::string("##lsGroup") + HE::kAutoLandscapeGroups[i].name;
		REQUIRE_MESSAGE(locate(ctx, panelId(id.c_str()), gx[i], gy[i]), id);
	}
	CHECK_FALSE(locate(ctx, panelId("##lsGroupMixed"), sx, sy));    // no "mixed" cell (step 7)
	he_ui::Image img;
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 3; ++i) panelFrame(ctx, false, i == 2 ? &img : nullptr);
	dump(img, "landscape-group-picker");

	// Clicking the group the terrain already has is nothing: no undo step, no change.
	const size_t depth0 = undo.undoDepth();
	clickAt(ctx, gx[0], gy[0]);
	CHECK(undo.undoDepth() == depth0);

	// ── Forest: the material changes, the weights do not ──
	clickAt(ctx, gx[1], gy[1]);
	const HE::AutoLandscapeGroup& forest = HE::autoLandscapeGroup(HE::AutoLandscapeGroupId::Forest);
	CHECK(reg.get<MaterialComponent>(terrain).materialAssetId == forest.materialId);
	CHECK(reg.get<MaterialComponent>(terrain).dirty);
	CHECK(reg.get<TerrainComponent>(terrain).layerWeights == weights0);
	CHECK(reg.get<TerrainComponent>(terrain).layerWeights2.empty());
	CHECK(undo.undoDepth() == depth0 + 1);

	// ── Desert, the fourth cell: its own material, the weights still untouched; then back to Forest ──
	clickAt(ctx, gx[3], gy[3]);
	CHECK(reg.get<MaterialComponent>(terrain).materialAssetId == HE::autoLandscapeGroup(HE::AutoLandscapeGroupId::Desert).materialId);
	CHECK(reg.get<TerrainComponent>(terrain).layerWeights == weights0);
	clickAt(ctx, gx[1], gy[1]);
	CHECK(reg.get<MaterialComponent>(terrain).materialAssetId == forest.materialId);

	// ── Paint: Forest's list; the stroke writes the row's channel ──
	REQUIRE(locate(ctx, panelId("##lsPaint"), sx, sy));
	clickAt(ctx, sx, sy);
	const MaterialAsset* fm = cm.getMaterial(forest.materialId);
	REQUIRE(fm);
	REQUIRE(fm->graphLayerNames.size() == static_cast<size_t>(HE::kAutoLandscapePaintLayerCount));
	CHECK(fm->graphLayerNames[0] == "Auto");
	CHECK(fm->graphLayerNames[1] == "Forest Floor");
	CHECK(fm->graphLayerNames[3] == "Mossy Rock");
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 3; ++i) panelFrame(ctx, false, i == 2 ? &img : nullptr);
	dump(img, "landscape-paint-forest");

	// A row's id: PushID(i) under the body child, then "##layer".
	auto layerRow = [&](int i, float& x, float& y)
	{
		ImGuiWindow* w = ImGui::FindWindowByName(kPanelWindow);
		REQUIRE(w != nullptr);
		for (float py = 14.0f; py < float(PH) - 14.0f; py += 4.0f)
			for (float px = 14.0f; px < float(W) - 14.0f; px += 12.0f)
			{
				const ImGuiID hov = idAt(ctx, px, py);
				for (ImGuiWindow* c : w->DC.ChildWindows)
					if (c && hov == ImHashStr("##layer", 0, ImHashData(&i, sizeof(i), c->ID)))
					{ x = px; y = py; return true; }
			}
		return false;
	};
	float rx = 0.0f, ry = 0.0f;
	REQUIRE_MESSAGE(layerRow(3, rx, ry), "no 4th row (Mossy Rock) in Forest's paint list");
	clickAt(ctx, rx, ry);

	const RenderWorld snap = topDownSnapshot(120.0f);
	const float cx = float(W) * 0.5f, cy = float(H) * 0.5f;
	viewportFrame(ctx, snap, cx, cy, false);
	viewportFrame(ctx, snap, cx, cy, false);
	viewportFrame(ctx, snap, cx, cy, true);
	viewportFrame(ctx, snap, cx, cy, false);

	const TerrainComponent& painted = reg.get<TerrainComponent>(terrain);
	REQUIRE(painted.layerWeights.size() == weights0.size());
	auto texel = [&](const TerrainComponent& t, float lx, float lz, uint8_t* out)
	{
		const uint32_t n = t.weightRes;
		const uint32_t tx = static_cast<uint32_t>(std::lround((lx / t.sizeX + 0.5f) * float(n - 1)));
		const uint32_t tz = static_cast<uint32_t>(std::lround((lz / t.sizeZ + 0.5f) * float(n - 1)));
		REQUIRE(TerrainPaint::texelWeights(t, tx, tz, out));
	};
	uint8_t centre[kTerrainMaxLayers], was[kTerrainMaxLayers], far[kTerrainMaxLayers], farWas[kTerrainMaxLayers];
	texel(painted, 0.0f, 0.0f, centre);
	TerrainComponent before;
	before.sizeX = before.sizeZ = 100.0f; before.weightRes = 64;
	before.layerWeights = weights0;
	texel(before, 0.0f, 0.0f, was);
	CHECK(centre[3] == 255);                                  // the chosen channel took the centre
	int sum = 0;
	for (int k = 0; k < kTerrainMaxLayers; ++k) sum += centre[k];
	CHECK(sum == 255);                                        // and the texel is still normalised
	CHECK(was[3] == 0);
	texel(painted, 30.0f, 30.0f, far);
	texel(before, 30.0f, 30.0f, farWas);
	for (int k = 0; k < kTerrainMaxLayers; ++k) CHECK(far[k] == farWas[k]);   // outside the brush nothing moved
	CHECK(far[2] > 0);                                        // the earlier paint is still there

	// ── Auto (row 0) paints back to the default: a channel-0 stroke takes a texture choice back ──
	REQUIRE(layerRow(0, rx, ry));
	clickAt(ctx, rx, ry);
	viewportFrame(ctx, snap, cx, cy, false);
	viewportFrame(ctx, snap, cx, cy, true);
	viewportFrame(ctx, snap, cx, cy, false);
	texel(reg.get<TerrainComponent>(terrain), 0.0f, 0.0f, centre);
	CHECK(centre[0] == 255);

	// Back to Sculpt, so the file-static mode does not leak into another test.
	for (int i = 0; i < 4; ++i) panelFrame(ctx, false);
	REQUIRE(locate(ctx, panelId("##lsSculpt"), sx, sy));
	clickAt(ctx, sx, sy);
}
TEST_CASE("landscape ui: Apply Group Meshes adds the group's layers, never removes or repeats one")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	auto& reg = world.registry();

	ContentManager cm(".");
	cm.setEngineContentRoot((std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent").string());
	for (const HE::AutoLandscapeGroup& g : HE::kAutoLandscapeGroups)
		REQUIRE_MESSAGE(cm.loadAsset(g.materialPath) == g.materialId, g.name);

	const Entity terrain = world.createEntity("Terrain");
	reg.emplace<TransformComponent>(terrain);
	TerrainComponent tc;
	tc.sizeX = 100.0f; tc.sizeZ = 100.0f; tc.seed = 0; tc.weightRes = 64;
	reg.emplace<TerrainComponent>(terrain, tc);
	MaterialComponent mc;
	mc.materialAssetId = HE::autoLandscapeGroup(HE::AutoLandscapeGroupId::Forest).materialId;
	reg.emplace<MaterialComponent>(terrain, mc);
	// A layer the author made: it carries a mesh of its own and tuned numbers, and must survive.
	FoliageComponent fol;
	fol.layers[0].name = "My Bush";
	fol.layers[0].meshAssetId = HE::UUID{ 0xABCDEFULL, 0x1ULL };
	fol.layers[0].density = 0.777f;
	reg.emplace<FoliageComponent>(terrain, fol);

	ContextBits bits;
	AppContext ctx = bits.make(world, undo);
	ctx.contentManager = &cm;
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 4; ++i) panelFrame(ctx, false);

	float sx = 0.0f, sy = 0.0f;
	REQUIRE(locate(ctx, panelId("##lsSetup"), sx, sy));
	clickAt(ctx, sx, sy);
	float bx = 0.0f, by = 0.0f;
	REQUIRE_MESSAGE(locate(ctx, panelId("Apply Group Meshes"), bx, by), "no Apply Group Meshes button in Setup");

	const auto layerNames = [&] {
		std::vector<std::string> n;
		for (const FoliageLayer& l : reg.get<FoliageComponent>(terrain).layers) n.push_back(l.name);
		return n;
	};
	const size_t depth0 = undo.undoDepth();
	clickAt(ctx, bx, by);
	// Forest's four kinds, after the author's own layer (which is untouched).
	CHECK(layerNames() == std::vector<std::string>{ "My Bush", "Fern", "Forest Stump", "Fallen Log", "Mossy Rock" });
	{
		const FoliageComponent& f = reg.get<FoliageComponent>(terrain);
		CHECK(f.layers[0].density == 0.777f);
		CHECK(f.layers[0].meshAssetId == HE::UUID{ 0xABCDEFULL, 0x1ULL });
		CHECK(f.layers[1].meshAssetId == HE::layerMeshLevel0(HE::LayerMeshKind::Fern));
		// Forest's row: the Mossy Rock stands from 0.13, not Mountains' 0.15.
		CHECK(f.layers[4].slopeMin == 0.13f);
	}
	CHECK(undo.undoDepth() == depth0 + 1);

	// Again: nothing is missing, so nothing is added and no undo step is spent.
	clickAt(ctx, bx, by);
	CHECK(layerNames().size() == 5u);
	CHECK(undo.undoDepth() == depth0 + 1);

	// Switching the group keeps every layer; Apply then adds that group's kinds beside them.
	float gx = 0.0f, gy = 0.0f;
	REQUIRE(locate(ctx, panelId("##lsGroupMountains"), gx, gy));
	clickAt(ctx, gx, gy);
	CHECK(reg.get<MaterialComponent>(terrain).materialAssetId == HE::kAutoLandscapeMaterialId);
	CHECK(layerNames().size() == 5u);
	clickAt(ctx, bx, by);
	CHECK(layerNames().size() == 5u + 6u);
	CHECK(layerNames()[5] == "Grass Tuft");
	CHECK(reg.get<FoliageComponent>(terrain).layers[0].name == "My Bush");
}

// Thema 327 step 4: a material with no Landscape Foliage node (every engine group
// material, and any custom one nobody set the node on) carries no `foliageSetId`,
// so the button that applies a set must not even offer itself there — there is
// nothing to apply, and showing a disabled row would just be another thing to
// explain in a tool nobody is forced to use.
TEST_CASE("landscape ui: Apply Foliage does not appear for a material with no foliage set")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	auto& reg = world.registry();

	ContentManager cm(".");
	cm.setEngineContentRoot((std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent").string());

	const Entity terrain = world.createEntity("Terrain");
	reg.emplace<TransformComponent>(terrain);
	TerrainComponent tc;
	tc.sizeX = 100.0f; tc.sizeZ = 100.0f; tc.seed = 0; tc.weightRes = 64;
	reg.emplace<TerrainComponent>(terrain, tc);
	MaterialComponent mc;
	mc.materialAssetId = HE::kAutoLandscapeMaterialId;   // the built-in Mountains material: no node
	reg.emplace<MaterialComponent>(terrain, mc);

	ContextBits bits;
	AppContext ctx = bits.make(world, undo);
	ctx.contentManager = &cm;
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 4; ++i) panelFrame(ctx, false);

	float sx = 0.0f, sy = 0.0f;
	REQUIRE(locate(ctx, panelId("##lsSetup"), sx, sy));
	clickAt(ctx, sx, sy);
	float bx = 0.0f, by = 0.0f;
	CHECK_FALSE(locate(ctx, panelId("Apply Foliage"), bx, by));
}

// Thema 327 step 4: the one place that turns a LandscapeFoliageLayer asset (the
// Landscape Foliage node's set, step 3) into something that actually scatters.
// Placeholder meshes stand in for real foliage meshes (a human provides those
// later, design §0); this proves the WIRING — entries become tagged layers that
// FoliageSystem scatters exactly like a hand-added one — not any particular mesh.
TEST_CASE("landscape ui: Apply Foliage materializes a set's entries, tags them, and re-apply "
          "refreshes them without touching a hand-added layer or what the author tuned on its own")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	auto& reg = world.registry();

	ContentManager cm(".");
	cm.setEngineContentRoot((std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent").string());

	// Placeholder meshes (Thema 163/182's own instancing/pooling needs only a mesh
	// UUID to scatter against, not a real file — the human supplies real foliage
	// meshes later, design §0).
	const HE::UUID tuftMesh{ 0x7001ULL, 0x1ULL };
	const HE::UUID rockMesh{ 0x7002ULL, 0x1ULL };

	LandscapeFoliageLayerAsset set;
	set.name = "Test Set";
	set.densityScale = 2.0f;   // "make this biome sparser/denser" (design §3), applies at Apply time
	{
		LandscapeFoliageEntry e;
		e.name = "Tuft"; e.meshAssetId = tuftMesh; e.density = 0.2f;
		e.slopeMin = 0.0f; e.slopeMax = 0.5f; e.paintLayer = 1;
		set.entries.push_back(e);
	}
	{
		LandscapeFoliageEntry e;
		e.name = "Rock"; e.meshAssetId = rockMesh; e.density = 0.05f;
		e.slopeMin = 0.4f; e.heightMin = 10.0f; e.paintLayer = 2;
		set.entries.push_back(e);
	}
	const HE::UUID setId = cm.registerLandscapeFoliageLayer(set);

	MaterialAsset mat;
	mat.foliageSetId = setId;   // as if a Landscape Foliage node named it (step 3)
	const HE::UUID matId = cm.registerMaterial(mat);

	const Entity terrain = world.createEntity("Terrain");
	reg.emplace<TransformComponent>(terrain);
	TerrainComponent tc;
	tc.sizeX = 100.0f; tc.sizeZ = 100.0f; tc.seed = 0; tc.weightRes = 64;
	reg.emplace<TerrainComponent>(terrain, tc);
	MaterialComponent mc;
	mc.materialAssetId = matId;
	reg.emplace<MaterialComponent>(terrain, mc);
	// A layer the author made by hand: no tag, must survive untouched.
	FoliageComponent fol;
	fol.layers[0].name = "My Bush";
	fol.layers[0].meshAssetId = HE::UUID{ 0xABCDEFULL, 0x1ULL };
	fol.layers[0].density = 0.777f;
	reg.emplace<FoliageComponent>(terrain, fol);

	ContextBits bits;
	AppContext ctx = bits.make(world, undo);
	ctx.contentManager = &cm;
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 4; ++i) panelFrame(ctx, false);

	float sx = 0.0f, sy = 0.0f;
	REQUIRE(locate(ctx, panelId("##lsSetup"), sx, sy));
	clickAt(ctx, sx, sy);
	float bx = 0.0f, by = 0.0f;
	REQUIRE_MESSAGE(locate(ctx, panelId("Apply Foliage"), bx, by), "no Apply Foliage button in Setup");

	const size_t depth0 = undo.undoDepth();
	clickAt(ctx, bx, by);
	{
		const FoliageComponent& f = reg.get<FoliageComponent>(terrain);
		REQUIRE(f.layers.size() == 3u);
		CHECK(f.layers[0].name == "My Bush");          // untouched
		CHECK(f.layers[0].density == 0.777f);
		CHECK(f.layers[0].sourceAssetId == HE::UUID{});
		CHECK(f.layers[1].name == "Tuft");
		CHECK(f.layers[1].sourceAssetId == setId);
		CHECK(f.layers[1].sourceEntry == 0);
		CHECK(f.layers[1].meshAssetId == tuftMesh);
		CHECK(f.layers[1].density == doctest::Approx(0.4f));   // 0.2 * densityScale 2.0
		CHECK(f.layers[1].slopeMax == 0.5f);
		CHECK(f.layers[1].weightLayer == 1);
		CHECK(f.layers[1].dirty);
		CHECK(f.layers[2].name == "Rock");
		CHECK(f.layers[2].sourceAssetId == setId);
		CHECK(f.layers[2].sourceEntry == 1);
		CHECK(f.layers[2].heightMin == 10.0f);
		CHECK(f.layers[2].weightLayer == 2);
	}
	CHECK(undo.undoDepth() == depth0 + 1);

	// Tune the applied layer by hand (visibility, a painted mask, its own seed) and
	// edit the set's entry — re-apply must refresh the asset-owned fields and bring
	// no new layer, but leave the hand-tuned per-scene state exactly as it was.
	{
		FoliageComponent& f = reg.get<FoliageComponent>(terrain);
		f.layers[1].visible = false;
		f.layers[1].seed = 999;
		f.layers[1].densityMask.assign(16, 128);
		f.layers[1].maskRes = 4;
	}
	LandscapeFoliageLayerAsset* mutSet = cm.getLandscapeFoliageLayerMutable(setId);
	REQUIRE(mutSet);
	mutSet->entries[0].name    = "Tuft v2";
	mutSet->entries[0].density = 0.3f;

	clickAt(ctx, bx, by);
	{
		const FoliageComponent& f = reg.get<FoliageComponent>(terrain);
		REQUIRE(f.layers.size() == 3u);                // no duplicate, the tag found its layer again
		CHECK(f.layers[1].name == "Tuft v2");           // asset-owned field refreshed
		CHECK(f.layers[1].density == doctest::Approx(0.6f));   // 0.3 * densityScale 2.0
		CHECK(f.layers[1].visible == false);            // the editor's own state, untouched
		CHECK(f.layers[1].seed == 999);
		CHECK(f.layers[1].densityMask.size() == 16u);
		CHECK(f.layers[0].name == "My Bush");           // the hand-added layer, still untouched
	}
	CHECK(undo.undoDepth() == depth0 + 2);
}

// Thema 327 step 7: the review's scenario for the passive pull in the "Foliage
// Set" row — a material names a set EngineContent has not pulled yet. The first
// frame's ask is a true no-op (an unknown UUID, step 4's own comment: nothing is
// in any pak/disk/remote registry for it). The set then "arrives" the way a real
// sync delivers it: a file appears under the EngineContent root and the editor's
// own refresh re-indexes it (ContentManager::scanContentDirectory(), which bumps
// contentEpoch() — the same signal the "##arr" texture-array fix keys on). Before
// this fix the function-static s_asked set only ever asked once per UUID per
// process, so a file that arrived after that first ask was never picked up and
// "Apply Foliage" stayed disabled until the editor restarted.
TEST_CASE("landscape ui: Apply Foliage recovers once a pending foliage set arrives, "
          "without an editor restart")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	auto& reg = world.registry();

	struct TempDir
	{
		std::filesystem::path path;
		explicit TempDir(const char* name)
		{
			path = std::filesystem::temp_directory_path() / name;
			std::error_code ec;
			std::filesystem::remove_all(path, ec);
			std::filesystem::create_directories(path);
		}
		~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
	};
	TempDir projectRoot("he_test_tt_foliage_pending_project");
	TempDir engineRoot("he_test_tt_foliage_pending_engine");

	ContentManager cm(projectRoot.path.string());
	cm.setEngineContentRoot(engineRoot.path.string());

	// As if a Landscape Foliage node (step 3) named a set the editor has not
	// seen a file for yet — freshly generated, so it cannot collide with a UUID
	// any other test case asked this same TerrainTools.cpp translation unit
	// about (the retry map is a function-static, shared by every TEST_CASE in
	// this binary).
	const HE::UUID setId = HE::UUID::generate();

	MaterialAsset mat;
	mat.foliageSetId = setId;
	const HE::UUID matId = cm.registerMaterial(mat);

	const Entity terrain = world.createEntity("Terrain");
	reg.emplace<TransformComponent>(terrain);
	TerrainComponent tc;
	tc.sizeX = 100.0f; tc.sizeZ = 100.0f; tc.seed = 0; tc.weightRes = 64;
	reg.emplace<TerrainComponent>(terrain, tc);
	MaterialComponent mc;
	mc.materialAssetId = matId;
	reg.emplace<MaterialComponent>(terrain, mc);

	ContextBits bits;
	AppContext ctx = bits.make(world, undo);
	ctx.contentManager = &cm;
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 4; ++i) panelFrame(ctx, false);

	float sx = 0.0f, sy = 0.0f;
	REQUIRE(locate(ctx, panelId("##lsSetup"), sx, sy));
	clickAt(ctx, sx, sy);
	float bx = 0.0f, by = 0.0f;
	REQUIRE_MESSAGE(locate(ctx, panelId("Apply Foliage"), bx, by), "no Apply Foliage button in Setup");

	// Not resident, nothing on disk yet: the ask made inside panelFrame() above
	// is a clean no-op. Clicking must do nothing.
	const size_t depth0 = undo.undoDepth();
	clickAt(ctx, bx, by);
	CHECK_FALSE(reg.any_of<FoliageComponent>(terrain));
	CHECK(undo.undoDepth() == depth0);
	CHECK(cm.getLandscapeFoliageLayer(setId) == nullptr);

	// The set "arrives": EngineContent writes the real file under the engine
	// content root, and the editor's own refresh re-indexes it — in production
	// this is what bumps contentEpoch() (ContentManager::scanContentDirectory()).
	{
		ContentManager author(engineRoot.path.string());
		LandscapeFoliageLayerAsset set;
		set.type = HE::AssetType::LandscapeFoliageLayer;
		set.name = "LF_Pending";
		set.path = "LandscapeFoliage/LF_Pending.hasset";
		set.id   = setId;
		LandscapeFoliageEntry e;
		e.name = "Tuft"; e.meshAssetId = HE::UUID{ 0x7003ULL, 0x1ULL }; e.density = 0.3f;
		set.entries.push_back(e);
		REQUIRE(author.saveAsset(set));
	}
	cm.scanContentDirectory();

	// One more frame: the retry must fire now that the epoch moved, queuing the
	// real disk load — it has not completed on THIS frame yet, the panel never
	// blocks for it.
	panelFrame(ctx, false);
	CHECK(cm.getLandscapeFoliageLayer(setId) == nullptr);

	// Drain the async read, the same way ContentManager's own worker completes it.
	bool loaded = false;
	for (int i = 0; i < 400 && !loaded; ++i)
	{
		cm.pollAsyncResults(16);
		loaded = cm.getLandscapeFoliageLayer(setId) != nullptr;
		if (!loaded) std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	REQUIRE_MESSAGE(loaded, "the retried loadAssetAsync never resolved the pending set");

	// One more draw: the row now shows the set, and Apply Foliage works without
	// any restart.
	panelFrame(ctx, false);
	REQUIRE(locate(ctx, panelId("Apply Foliage"), bx, by));
	clickAt(ctx, bx, by);
	const FoliageComponent* fol = reg.try_get<FoliageComponent>(terrain);
	REQUIRE(fol);
	bool found = false;
	for (const FoliageLayer& l : fol->layers)
		if (l.sourceAssetId == setId) { found = true; CHECK(l.name == "Tuft"); }
	CHECK(found);
	CHECK(undo.undoDepth() == depth0 + 1);
}

// Step 9 review-fix (point 1): ContentManager only LOGS
// validateLandscapeFoliageLayer's problems when it loads a LFOL chunk
// (LandscapeFoliageLayerJson.cpp / ContentManager.cpp) — it never rejects or
// repairs them, so a hand-edited or stale .hasset can still reach Apply with a
// negative density, an inverted min/max or slope range, or an unordered LOD
// list. Apply must clamp every numeric field to what FoliageSystem actually
// scatters against, not hand it straight through.
TEST_CASE("landscape ui: Apply Foliage clamps out-of-range values from the asset "
          "instead of writing them straight into the layer")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	auto& reg = world.registry();

	ContentManager cm(".");
	cm.setEngineContentRoot((std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent").string());

	const HE::UUID badMesh{ 0x7101ULL, 0x1ULL };
	const HE::UUID negMesh{ 0x7102ULL, 0x1ULL };

	LandscapeFoliageLayerAsset set;
	set.name = "Bad Ranges Set";
	{
		LandscapeFoliageEntry e;
		e.name = "BadRanges"; e.meshAssetId = badMesh; e.density = 0.3f;
		e.minScale = 2.0f;  e.maxScale = 0.5f;            // inverted
		e.slopeMin = 1.5f;  e.slopeMax = -0.5f;           // out of [0,1] and inverted
		e.heightMin = 100.0f; e.heightMax = -100.0f;      // inverted
		e.drawDistance   = -10.0f;                        // must be positive
		e.bucketSize     = 0.0f;                          // must be positive (a scatter-grid divisor)
		e.shadowDistance = -5.0f;                         // must be >= 0
		e.lodHysteresis  = -2.0f;                         // must be >= 0
		LandscapeFoliageLod lod0; lod0.distance = 50.0f; lod0.density = 2.0f;    // out of [0,1]
		LandscapeFoliageLod lod1; lod1.distance = 10.0f; lod1.density = -1.0f;   // out of [0,1] and out of order
		e.lods.push_back(lod0);
		e.lods.push_back(lod1);
		set.entries.push_back(e);
	}
	{
		LandscapeFoliageEntry e;
		e.name = "NegDensity"; e.meshAssetId = negMesh; e.density = -5.0f;   // must never scatter negatively
		set.entries.push_back(e);
	}
	const HE::UUID setId = cm.registerLandscapeFoliageLayer(set);

	MaterialAsset mat;
	mat.foliageSetId = setId;
	const HE::UUID matId = cm.registerMaterial(mat);

	const Entity terrain = world.createEntity("Terrain");
	reg.emplace<TransformComponent>(terrain);
	TerrainComponent tc;
	tc.sizeX = 100.0f; tc.sizeZ = 100.0f; tc.seed = 0; tc.weightRes = 64;
	reg.emplace<TerrainComponent>(terrain, tc);
	MaterialComponent mc;
	mc.materialAssetId = matId;
	reg.emplace<MaterialComponent>(terrain, mc);

	ContextBits bits;
	AppContext ctx = bits.make(world, undo);
	ctx.contentManager = &cm;
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 4; ++i) panelFrame(ctx, false);

	float sx = 0.0f, sy = 0.0f;
	REQUIRE(locate(ctx, panelId("##lsSetup"), sx, sy));
	clickAt(ctx, sx, sy);
	float bx = 0.0f, by = 0.0f;
	REQUIRE_MESSAGE(locate(ctx, panelId("Apply Foliage"), bx, by), "no Apply Foliage button in Setup");
	clickAt(ctx, bx, by);

	FoliageComponent& f = reg.get<FoliageComponent>(terrain);
	REQUIRE(f.layers.size() == 2u);
	FoliageLayer* bad = nullptr;
	FoliageLayer* neg = nullptr;
	for (FoliageLayer& l : f.layers)
	{
		if (l.sourceAssetId == setId && l.sourceEntry == 0) bad = &l;
		if (l.sourceAssetId == setId && l.sourceEntry == 1) neg = &l;
	}
	REQUIRE(bad);
	REQUIRE(neg);

	CHECK(bad->minScale     == doctest::Approx(0.5f));     // swapped into min <= max
	CHECK(bad->maxScale     == doctest::Approx(2.0f));
	CHECK(bad->slopeMin     == doctest::Approx(0.0f));      // clamped into 0..1, then swapped
	CHECK(bad->slopeMax     == doctest::Approx(1.0f));
	CHECK(bad->heightMin    == doctest::Approx(-100.0f));   // swapped into min <= max
	CHECK(bad->heightMax    == doctest::Approx(100.0f));
	CHECK(bad->drawDistance   > 0.0f);                      // negative rejected, a positive default kept
	CHECK(bad->bucketSize     > 0.0f);                      // zero rejected (it divides the scatter grid)
	CHECK(bad->shadowDistance >= 0.0f);
	CHECK(bad->lodHysteresis  >= 0.0f);
	REQUIRE(bad->lods.size() == 2u);
	CHECK(bad->lods[0].distance == doctest::Approx(10.0f));  // sorted ascending by distance
	CHECK(bad->lods[0].density  == doctest::Approx(0.0f));   // -1 clamped into 0..1
	CHECK(bad->lods[1].distance == doctest::Approx(50.0f));
	CHECK(bad->lods[1].density  == doctest::Approx(1.0f));   // 2.0 clamped into 0..1

	CHECK(neg->density == doctest::Approx(0.0f));   // negative density clamped, never scatters backwards

	// Not just the fields: the scatter itself must tolerate the corrected ranges
	// (no crash, no NaN instances), and the clamped-to-zero density must really
	// spawn nothing rather than something with a garbage transform.
	FoliageSystem::update(world);
	CHECK_FALSE(bad->cachedInstances.empty());
	for (const auto& m : bad->cachedInstances)
	{
		CHECK(std::isfinite(m[3].x));
		CHECK(std::isfinite(m[3].y));
		CHECK(std::isfinite(m[3].z));
	}
	CHECK(neg->cachedInstances.empty());
}

// Step 9 review-fix (point 2): the reuse pass used to grab ANY layer with no
// mesh and no asset tag, even one the author had already started hand-tuning
// (a name, a placement rule, a painted mask) before picking a mesh — an empty
// FoliageComponent starts with exactly one such layer (FoliageComponent.h),
// and an author can set its rules up before its mesh. Apply must never
// silently adopt that layer, name or index collision with an asset entry or
// not.
TEST_CASE("landscape ui: Apply Foliage never adopts a hand-tuned layer that has no mesh yet, "
          "even when its name collides with an asset entry's")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	auto& reg = world.registry();

	ContentManager cm(".");
	cm.setEngineContentRoot((std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent").string());

	const HE::UUID tuftMesh{ 0x7201ULL, 0x1ULL };

	LandscapeFoliageLayerAsset set;
	set.name = "Collide Set";
	{
		LandscapeFoliageEntry e;
		e.name = "Helper Rule";       // same name as the hand layer below, on purpose
		e.meshAssetId = tuftMesh; e.density = 0.1f;
		set.entries.push_back(e);
	}
	const HE::UUID setId = cm.registerLandscapeFoliageLayer(set);

	MaterialAsset mat;
	mat.foliageSetId = setId;
	const HE::UUID matId = cm.registerMaterial(mat);

	const Entity terrain = world.createEntity("Terrain");
	reg.emplace<TransformComponent>(terrain);
	TerrainComponent tc;
	tc.sizeX = 100.0f; tc.sizeZ = 100.0f; tc.seed = 0; tc.weightRes = 64;
	reg.emplace<TerrainComponent>(terrain, tc);
	MaterialComponent mc;
	mc.materialAssetId = matId;
	reg.emplace<MaterialComponent>(terrain, mc);

	// The author set placement rules and painted a mask on this landscape's one
	// starter layer, same name as the asset entry above, but never picked a
	// mesh — a plausible, real editing order (rules first, mesh later).
	FoliageComponent fol;
	fol.layers[0].name        = "Helper Rule";
	fol.layers[0].slopeMin    = 0.2f;
	fol.layers[0].slopeMax    = 0.6f;
	fol.layers[0].weightLayer = 3;
	fol.layers[0].maskRes     = 4;
	fol.layers[0].densityMask.assign(16, 200);
	reg.emplace<FoliageComponent>(terrain, fol);

	ContextBits bits;
	AppContext ctx = bits.make(world, undo);
	ctx.contentManager = &cm;
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 4; ++i) panelFrame(ctx, false);

	float sx = 0.0f, sy = 0.0f;
	REQUIRE(locate(ctx, panelId("##lsSetup"), sx, sy));
	clickAt(ctx, sx, sy);
	float bx = 0.0f, by = 0.0f;
	REQUIRE_MESSAGE(locate(ctx, panelId("Apply Foliage"), bx, by), "no Apply Foliage button in Setup");
	clickAt(ctx, bx, by);

	const FoliageComponent& f = reg.get<FoliageComponent>(terrain);
	REQUIRE(f.layers.size() == 2u);   // the hand layer kept, a new one added — never merged into one

	// The hand layer: completely untouched, still mesh-less, still unmarked.
	CHECK(f.layers[0].name == "Helper Rule");
	CHECK(f.layers[0].meshAssetId == HE::UUID{});
	CHECK(f.layers[0].slopeMin == doctest::Approx(0.2f));
	CHECK(f.layers[0].slopeMax == doctest::Approx(0.6f));
	CHECK(f.layers[0].weightLayer == 3);
	CHECK(f.layers[0].densityMask.size() == 16u);
	CHECK(f.layers[0].sourceAssetId == HE::UUID{});
	CHECK(f.layers[0].sourceEntry == -1);

	// The asset entry got its OWN new layer instead of stealing the hand one.
	CHECK(f.layers[1].name == "Helper Rule");      // same text, different layer
	CHECK(f.layers[1].meshAssetId == tuftMesh);
	CHECK(f.layers[1].sourceAssetId == setId);
	CHECK(f.layers[1].sourceEntry == 0);
}

// Step 10/11 review-fix: the same guard also has to see a layer whose ONLY hand-tuning
// is one scalar field (default name, mesh, mask, LODs, weight layer): a slope/height
// rule, density, seed, scale range, draw/shadow distance, bucket size, the shadow/AO
// switches, LOD hysteresis or visibility. Each SUBCASE changes exactly one field;
// "pristine" is the positive control: an untouched starter is still reused, so the
// test discriminates.
TEST_CASE("landscape ui: Apply Foliage reuses only a starter layer whose hand-tunable fields are default")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	auto& reg = world.registry();

	ContentManager cm(".");
	cm.setEngineContentRoot((std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent").string());

	const HE::UUID tuftMesh{ 0x7301ULL, 0x1ULL };

	LandscapeFoliageLayerAsset set;
	set.name = "Rule Set";
	{
		LandscapeFoliageEntry e;
		e.name = "Tuft"; e.meshAssetId = tuftMesh; e.density = 0.1f;
		set.entries.push_back(e);
	}
	const HE::UUID setId = cm.registerLandscapeFoliageLayer(set);

	MaterialAsset mat;
	mat.foliageSetId = setId;
	const HE::UUID matId = cm.registerMaterial(mat);

	const Entity terrain = world.createEntity("Terrain");
	reg.emplace<TransformComponent>(terrain);
	TerrainComponent tc;
	tc.sizeX = 100.0f; tc.sizeZ = 100.0f; tc.seed = 0; tc.weightRes = 64;
	reg.emplace<TerrainComponent>(terrain, tc);
	MaterialComponent mc;
	mc.materialAssetId = matId;
	reg.emplace<MaterialComponent>(terrain, mc);

	FoliageComponent fol;                       // one default starter layer
	bool handTuned = true;
	SUBCASE("pristine starter")  { handTuned = false; }
	SUBCASE("slopeMin")          { fol.layers[0].slopeMin  = 0.3f; }
	SUBCASE("slopeMax")          { fol.layers[0].slopeMax  = 0.7f; }
	SUBCASE("heightMin")         { fol.layers[0].heightMin = 5.0f; }
	SUBCASE("heightMax")         { fol.layers[0].heightMax = 50.0f; }
	SUBCASE("density")           { fol.layers[0].density        = 0.5f; }
	SUBCASE("seed")              { fol.layers[0].seed           = 7; }
	SUBCASE("minScale")          { fol.layers[0].minScale       = 0.5f; }
	SUBCASE("maxScale")          { fol.layers[0].maxScale       = 2.0f; }
	SUBCASE("drawDistance")      { fol.layers[0].drawDistance   = 150.0f; }
	SUBCASE("bucketSize")        { fol.layers[0].bucketSize     = 64.0f; }
	SUBCASE("castsShadow")       { fol.layers[0].castsShadow    = false; }
	SUBCASE("contributesAO")     { fol.layers[0].contributesAO  = false; }
	SUBCASE("shadowDistance")    { fol.layers[0].shadowDistance = 40.0f; }
	SUBCASE("lodHysteresis")     { fol.layers[0].lodHysteresis  = 5.0f; }
	SUBCASE("visible")           { fol.layers[0].visible        = false; }
	const FoliageLayer before = fol.layers[0];
	reg.emplace<FoliageComponent>(terrain, fol);

	ContextBits bits;
	AppContext ctx = bits.make(world, undo);
	ctx.contentManager = &cm;
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 4; ++i) panelFrame(ctx, false);

	float sx = 0.0f, sy = 0.0f;
	REQUIRE(locate(ctx, panelId("##lsSetup"), sx, sy));
	clickAt(ctx, sx, sy);
	float bx = 0.0f, by = 0.0f;
	REQUIRE_MESSAGE(locate(ctx, panelId("Apply Foliage"), bx, by), "no Apply Foliage button in Setup");
	clickAt(ctx, bx, by);

	const FoliageComponent& f = reg.get<FoliageComponent>(terrain);
	if (handTuned)
	{
		REQUIRE(f.layers.size() == 2u);                  // the rule layer kept, a new one added
		CHECK(f.layers[0].sourceAssetId == HE::UUID{});
		CHECK(f.layers[0].meshAssetId == HE::UUID{});
		CHECK(f.layers[0].slopeMin  == before.slopeMin);
		CHECK(f.layers[0].slopeMax  == before.slopeMax);
		CHECK(f.layers[0].heightMin == before.heightMin);
		CHECK(f.layers[0].heightMax == before.heightMax);
		CHECK(f.layers[0].density        == before.density);
		CHECK(f.layers[0].seed           == before.seed);
		CHECK(f.layers[0].minScale       == before.minScale);
		CHECK(f.layers[0].maxScale       == before.maxScale);
		CHECK(f.layers[0].drawDistance   == before.drawDistance);
		CHECK(f.layers[0].bucketSize     == before.bucketSize);
		CHECK(f.layers[0].castsShadow    == before.castsShadow);
		CHECK(f.layers[0].contributesAO  == before.contributesAO);
		CHECK(f.layers[0].shadowDistance == before.shadowDistance);
		CHECK(f.layers[0].lodHysteresis  == before.lodHysteresis);
		CHECK(f.layers[0].visible        == before.visible);
		CHECK(f.layers[1].sourceAssetId == setId);
		CHECK(f.layers[1].meshAssetId == tuftMesh);
	}
	else
	{
		REQUIRE(f.layers.size() == 1u);                  // the starter itself was reused
		CHECK(f.layers[0].sourceAssetId == setId);
		CHECK(f.layers[0].meshAssetId == tuftMesh);
	}
}

#endif // HE_EDITOR_DEPS_DIR
