#include "doctest.h"
#include "ImGuiSoftwareRaster.h"
#include "TestFsUtil.h"
#include "LandscapeWorldTools.h"
#include "StreamingDebugView.h"
#include "EditorApplication.h"   // AppContext
#include "EditorCamera.h"
#include "EditorSelection.h"
#include "EditorTheme.h"
#include "EditorUndo.h"
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/LandscapeWorldComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LandscapeRuntime.h>
#include <HorizonScene/LandscapeSource.h>
#include <imgui.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// ── The editor's tile-landscape tools (Thema 326, step 6) ─────────────────────────────────────────────────
// The Details panel's "Tile Landscape" section builds packs on a thread (generate, migrate a Terrain, import 16-bit height maps) and the
// component then points at the new pack; the Profiler's Streaming tab shows the store of a running tile landscape. The section and the tab
// are drawn headless and, with HE_UI_DUMP_DIR set, shot (scripts/he_uishot.py).

namespace fs = std::filesystem;
namespace LWT = LandscapeWorldTools;

namespace
{
constexpr int W = 760, H = 1500;

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
		HE::Ed::applyHorizonDarkTheme();
		LWT::resetState();
	}
	~Harness()
	{
		LWT::resetState();
		ImGui::DestroyContext();
	}
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

	AppContext make()
	{
		return AppContext{
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
	}
};

fs::path tempDir(const char* tag)
{
	static int n = 0;
	const fs::path p = fs::temp_directory_path() / ("he_landscape_world_ui_" + std::string(tag) + "_" + std::to_string(++n) + "_" +
	                                                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	he_test::removeAllQuiet(p);
	fs::create_directories(p);
	return p;
}

void dump(const he_ui::Image& img, const char* name)
{
	if (const char* dir = std::getenv("HE_UI_DUMP_DIR"))
	{
		fs::create_directories(dir);
		CHECK(he_ui::writeBmp(img, (fs::path(dir) / name).string()));
	}
}

// Pumps until the running build has ended (and the component has been given the result).
void waitForBuild(HorizonWorld& world)
{
	for (int i = 0; i < 6000 && LWT::buildRunning(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
	REQUIRE_FALSE(LWT::buildRunning());
	LWT::pump(world);
}

he_ui::Image drawSectionShot(AppContext& ctx, Entity e)
{
	ImGui::NewFrame();
	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(ImVec2(float(W), float(H)));
	ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoSavedSettings);
	LWT::drawSection(ctx, e);
	ImGui::End();
	ImGui::Render();
	return he_ui::rasterize(ImGui::GetDrawData(), W, H);
}

he_ui::Image drawStreamingShot(AppContext& ctx)
{
	ImGui::NewFrame();
	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(ImVec2(float(W), float(H)));
	ImGui::Begin("Performance Profiler", nullptr, ImGuiWindowFlags_NoSavedSettings);
	StreamingDebugView::draw(ctx);
	ImGui::End();
	ImGui::Render();
	return he_ui::rasterize(ImGui::GetDrawData(), W, H);
}

size_t pixelsOf(const he_ui::Image& img, uint8_t r, uint8_t g, uint8_t b)
{
	size_t n = 0;
	for (int y = 0; y < img.height; ++y)
		for (int x = 0; x < img.width; ++x)
		{
			uint8_t pr, pg, pb, pa;
			img.pixel(x, y, pr, pg, pb, pa);
			if (pr == r && pg == g && pb == b) ++n;
		}
	return n;
}

// Pixels that are not the window's background (read at the corner).
int ink(const he_ui::Image& img)
{
	uint8_t r, g, b, a;
	img.pixel(1, 1, r, g, b, a);
	return img.inkedPixels(r, g, b);
}

// 16-bit little-endian square, the .r16 shape.
void writeR16(const fs::path& file, int side, int fx, int fz)
{
	std::vector<uint16_t> px(static_cast<size_t>(side) * side);
	for (int y = 0; y < side; ++y)
		for (int x = 0; x < side; ++x) px[static_cast<size_t>(y) * side + x] = static_cast<uint16_t>(((fx * side + x) * 400 + (fz * side + y) * 300) & 0xFFFF);
	std::ofstream o(file, std::ios::binary);
	o.write(reinterpret_cast<const char*>(px.data()), static_cast<std::streamsize>(px.size() * 2));
}
} // namespace

TEST_CASE("landscape world tools: generate builds a pack on a thread, the component takes it, the runtime opens it")
{
	Harness h;
	const fs::path dir = tempDir("gen");
	HorizonWorld world;
	const Entity land = world.createEntity("Landscape");
	world.registry().emplace<LandscapeWorldComponent>(land);
	const Entity cam = world.createEntity("Camera");
	world.registry().get_or_emplace<TransformComponent>(cam).position = { 1000.0f, 600.0f, 1000.0f };
	world.registry().emplace<CameraComponent>(cam);

	LWT::GenerateSettings gs;
	gs.tilesX = gs.tilesZ = 4;
	const std::string pack = (dir / "world.hlp").string();
	CHECK_FALSE(LWT::startGenerate(world, entt::null, pack, gs).empty());          // no target entity
	CHECK_FALSE(LWT::startGenerate(world, land, "", gs).empty());                    // no output file
	{
		LWT::GenerateSettings bad = gs;
		bad.tilesX = 0;
		CHECK_FALSE(LWT::startGenerate(world, land, pack, bad).empty());
	}
	REQUIRE(LWT::startGenerate(world, land, pack, gs).empty());
	CHECK_FALSE(LWT::startGenerate(world, land, pack, gs).empty());                  // one build at a time
	waitForBuild(world);
	CHECK_FALSE(LWT::lastStatusIsError());
	CHECK(LWT::lastStatus().find("16 tiles") != std::string::npos);

	const auto& lw = world.registry().get<LandscapeWorldComponent>(land);
	CHECK(fs::path(lw.pack) == fs::path(pack));
	CHECK(lw.heightScaleM == gs.heightRangeM);
	CHECK(lw.heightMinM == 0.0f);
	CHECK(lw.enabled);

	HE::LandscapeRuntime rt;
	for (int i = 0; i < 3000 && !(rt.active() && rt.store()->settled()); ++i)
	{
		rt.update(world, 0.016f);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	REQUIRE(rt.active());
	CHECK(rt.store()->source().tilesX() == 4);
	CHECK(rt.store()->stats().resident == 16);

	// Again over the SAME path with another seed: the component is switched off for two pumps so the runtime lets go of the old file, then on again.
	gs.seed = 99;
	REQUIRE(LWT::startGenerate(world, land, pack, gs).empty());
	waitForBuild(world);
	CHECK_FALSE(world.registry().get<LandscapeWorldComponent>(land).enabled);
	rt.update(world, 0.016f);
	CHECK_FALSE(rt.active());
	LWT::pump(world);
	LWT::pump(world);
	CHECK(world.registry().get<LandscapeWorldComponent>(land).enabled);
	rt.update(world, 0.016f);
	CHECK(rt.active());
	he_test::removeAllQuiet(dir);
}

TEST_CASE("landscape world tools: migrate takes the scene's Terrain; no Terrain, no migration")
{
	Harness h;
	const fs::path dir = tempDir("mig");
	HorizonWorld world;
	const Entity land = world.createEntity("Landscape");
	world.registry().emplace<LandscapeWorldComponent>(land);
	const std::string pack = (dir / "mig.hlp").string();
	CHECK((LWT::migrationSource(world, land) == entt::null));
	CHECK(LWT::startMigrate(world, land, pack).find("no Terrain") != std::string::npos);

	const Entity ground = world.createEntity("Ground");
	TerrainComponent tc;
	tc.sizeX = tc.sizeZ = 1024.0f;
	tc.resolution = 65;
	tc.sculptHeights.assign(65u * 65u, 0.0f);
	for (uint32_t z = 0; z < 65; ++z)
		for (uint32_t x = 0; x < 65; ++x) tc.sculptHeights[z * 65 + x] = 40.0f + static_cast<float>(x) + 0.5f * static_cast<float>(z);
	world.registry().emplace<TerrainComponent>(ground, tc);
	world.registry().get_or_emplace<TransformComponent>(ground).position = { 0.0f, 10.0f, 0.0f };
	CHECK((LWT::migrationSource(world, land) == ground));

	REQUIRE(LWT::startMigrate(world, land, pack).empty());
	waitForBuild(world);
	REQUIRE_FALSE(LWT::lastStatusIsError());
	const auto& lw = world.registry().get<LandscapeWorldComponent>(land);
	std::string err;
	const auto src = HE::LandscapePackSource::open(lw.pack, &err);
	REQUIRE_MESSAGE(src, err);
	CHECK(src->tilesX() == 2);
	CHECK(src->tilesZ() == 2);
	// The terrain entity's height (10 m) is part of the ground: tile (0,0), sample (0,0) is 40 + 10.
	HE::LandscapeTile t;
	REQUIRE(src->load({ 0, 0 }, 1, t));
	CHECK(lw.heightMinM + t.heights[0][0] * lw.heightScaleM / 65535.0f == doctest::Approx(50.0f).epsilon(0.01));
	he_test::removeAllQuiet(dir);
}

TEST_CASE("landscape world tools: import reads a folder of tiles or a picture; a bad one is an error in words and changes nothing")
{
	Harness h;
	const fs::path dir = tempDir("imp");
	HorizonWorld world;
	const Entity land = world.createEntity("Landscape");
	world.registry().emplace<LandscapeWorldComponent>(land);
	const fs::path maps = dir / "maps";
	fs::create_directories(maps);
	for (int fz = 0; fz < 2; ++fz)
		for (int fx = 0; fx < 2; ++fx) writeR16(maps / ("Gaea_x" + std::to_string(fx) + "_y" + std::to_string(fz) + ".r16"), 129, fx, fz);

	LWT::ImportSettings is;
	is.metersPerPixel = 4.0f;
	is.heightRangeM = 800.0f;
	const std::string pack = (dir / "imp.hlp").string();
	CHECK_FALSE(LWT::startImport(world, land, pack, "", is).empty());
	CHECK_FALSE(LWT::startImport(world, land, pack, (dir / "nothing_here").string(), is).empty());
	CHECK_FALSE(fs::exists(pack));
	REQUIRE(LWT::startImport(world, land, pack, maps.string(), is).empty());
	waitForBuild(world);
	REQUIRE_FALSE(LWT::lastStatusIsError());
	const auto& lw = world.registry().get<LandscapeWorldComponent>(land);
	CHECK(lw.heightScaleM == 800.0f);
	const auto src = HE::LandscapePackSource::open(lw.pack);
	REQUIRE(src);
	CHECK(src->tilesX() == 3);   // 258 pixels = 257 gaps of 4 m = 1028 m: just over two tiles
	HE::LandscapeTile t;
	REQUIRE(src->load({ 0, 0 }, 1, t));
	CHECK(t.heights[0][0] == 0);
	CHECK(t.heights[0][1] == 200);   // a sample every 2 m, a pixel every 4 m: halfway between the pixels 0 and 400

	// A single picture in a file that is not a height map.
	{ std::ofstream o(dir / "junk.r16", std::ios::binary); o << "abc"; }
	const std::string pack2 = (dir / "junk.hlp").string();
	REQUIRE(LWT::startImport(world, land, pack2, (dir / "junk.r16").string(), is).empty());
	waitForBuild(world);
	CHECK(LWT::lastStatusIsError());
	CHECK_FALSE(fs::exists(pack2));
	CHECK(fs::path(world.registry().get<LandscapeWorldComponent>(land).pack) == fs::path(pack));   // still the earlier pack
	he_test::removeAllQuiet(dir);
}

TEST_CASE("landscape world tools: the section draws idle, busy and with a status; cancelling leaves the old pack")
{
	Harness h;
	const fs::path dir = tempDir("ui");
	HorizonWorld world;
	EditorUndo undo;
	undo.setWorld(&world);
	const Entity land = world.createEntity("Landscape");
	world.registry().emplace<LandscapeWorldComponent>(land);
	ContextBits bits;
	AppContext ctx = bits.make();
	ctx.world   = &world;
	ctx.undoSys = &undo;

	const he_ui::Image idle = drawSectionShot(ctx, land);
	dump(idle, "landscape_world_section_idle.bmp");
	CHECK(idle.valid());
	CHECK(ink(idle) > 2000);

	LWT::GenerateSettings gs;
	gs.tilesX = gs.tilesZ = 40;   // enough tiles that the build is still running for the next frames
	const std::string pack = (dir / "big.hlp").string();
	REQUIRE(LWT::startGenerate(world, land, pack, gs).empty());
	const he_ui::Image busy = drawSectionShot(ctx, land);
	dump(busy, "landscape_world_section_busy.bmp");
	CHECK(busy.valid());
	LWT::cancelBuild();
	waitForBuild(world);
	CHECK(LWT::lastStatus() == "Cancelled.");
	CHECK_FALSE(LWT::lastStatusIsError());
	CHECK_FALSE(fs::exists(pack));
	CHECK(world.registry().get<LandscapeWorldComponent>(land).pack.empty());

	// A refusal is drawn as the status line.
	CHECK_FALSE(LWT::startMigrate(world, land, pack).empty());
	const he_ui::Image after = drawSectionShot(ctx, land);
	dump(after, "landscape_world_section_status.bmp");
	CHECK(after.valid());
	he_test::removeAllQuiet(dir);
}

TEST_CASE("streaming view: a running tile landscape shows its tiles and memory; a scene without one shows nothing extra")
{
	Harness h;
	const fs::path dir = tempDir("sv");
	HorizonWorld world;
	const Entity land = world.createEntity("Landscape");
	world.registry().emplace<LandscapeWorldComponent>(land);
	const Entity cam = world.createEntity("Camera");
	world.registry().get_or_emplace<TransformComponent>(cam).position = { 1000.0f, 600.0f, 1000.0f };
	world.registry().emplace<CameraComponent>(cam);
	ContextBits bits;
	AppContext ctx = bits.make();
	EditorCamera editorCamera;
	ctx.world = &world;
	ctx.editorCamera = &editorCamera;

	// No runtime on the world: the tab is what it was.
	const he_ui::Image plain = drawStreamingShot(ctx);
	CHECK(pixelsOf(plain, 70, 170, 90) == 0);

	LWT::GenerateSettings gs;
	gs.tilesX = gs.tilesZ = 4;
	REQUIRE(LWT::startGenerate(world, land, (dir / "sv.hlp").string(), gs).empty());
	waitForBuild(world);
	HE::LandscapeRuntime rt;
	for (int i = 0; i < 3000 && !(rt.active() && rt.store()->settled()); ++i)
	{
		rt.update(world, 0.016f);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	REQUIRE(rt.active());
	REQUIRE(rt.store()->stats().resident == 16);
	world.setLandscapeRuntime(&rt);
	editorCamera.setOrientation(glm::vec3(1000.0f, 300.0f, 1000.0f), glm::vec3(0.0f, 0.0f, -1.0f));

	const he_ui::Image shown = drawStreamingShot(ctx);
	dump(shown, "streaming_tab_tile_landscape.bmp");
	// 16 resident tiles in the map: each 8 x 8 pixels of the "resident" green, one of them framed.
	CHECK(pixelsOf(shown, 70, 170, 90) >= 15 * 8 * 8);
	world.setLandscapeRuntime(nullptr);
	he_test::removeAllQuiet(dir);
}
