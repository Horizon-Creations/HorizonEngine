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
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/TerrainMeshGenerator.h>   // terrainHeightAt
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonRendering/RenderWorld.h>
#include <ContentManager/DefaultAssets.h>

#include <imgui.h>
#include <imgui_internal.h>   // GetHoveredID / FindWindowByName: which item the pointer is on
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
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
	CHECK_MESSAGE(locate(ctx, panelId("Max Height##mountain"), nx, ny), "Mountain armed shows no Max Height");
	CHECK_MESSAGE(locate(ctx, panelId("New Seed##mountain"), nx, ny), "Mountain armed shows no New Seed");
	CHECK_FALSE(locate(ctx, panelId("Radius##brush"), nx, ny));
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
	CHECK(locate(ctx, panelId("Radius##brush"), rx, ry));
}
