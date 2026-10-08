#include "doctest.h"
#include "ImGuiSoftwareRaster.h"
#include "TestFsUtil.h"
#include "StreamingDebugView.h"
#include "EditorApplication.h"   // AppContext
#include "EditorCamera.h"
#include "EditorSelection.h"
#include "EditorTheme.h"
#include "EditorUndo.h"
#include <ContentManager/ContentManager.h>
#include <DebugDraw/DebugDraw.h>
#include <HorizonScene/CellSplit.h>
#include <HorizonScene/CellStreamer.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <imgui.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ── The profiler's Streaming tab and the editor's cell split (Thema 153, S. 6) ─
// splitOpenScene / mergeOpenScene are the editor half of HE::CellSplit: where
// the files go (beside the scene, project-relative in the manifest), what the
// undo history says, what is left on disk. The tab itself is drawn and, with
// HE_UI_DUMP_DIR set, shot (scripts/he_uishot.py).

namespace fs = std::filesystem;

namespace
{
constexpr int W = 760, H = 900;

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

// A row of 100 m props along +X, `perCell` in each of `cells` squares.
void fillWorld(HorizonWorld& w, int cells, int perCell)
{
	for (int c = 0; c < cells; ++c)
		for (int i = 0; i < perCell; ++i)
		{
			const Entity e = w.createEntity("Prop " + std::to_string(c) + "." + std::to_string(i));
			TransformComponent t;
			t.position = { c * 100.0f + 10.0f + i, 0.0f, 50.0f };
			w.addComponent(e, t);
			w.registry().emplace<MeshComponent>(e);
		}
}

size_t meshCount(HorizonWorld& w) { return w.registry().view<MeshComponent>().size(); }

void frame(AppContext& ctx, he_ui::Image* shot = nullptr)
{
	ImGui::NewFrame();
	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(ImVec2(float(W), float(H)));
	ImGui::Begin("Performance Profiler", nullptr, ImGuiWindowFlags_NoSavedSettings);
	StreamingDebugView::draw(ctx);
	ImGui::End();
	ImGui::Render();
	if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
}

void dump(const he_ui::Image& img, const char* name)
{
	if (const char* dir = std::getenv("HE_UI_DUMP_DIR"))
	{
		fs::create_directories(dir);
		CHECK(he_ui::writeBmp(img, (fs::path(dir) / name).string()));
	}
}
} // namespace

TEST_CASE("streaming view: split beside the scene file, one undo step each, merge puts it back")
{
	const fs::path proj = fs::temp_directory_path() / "he_streaming_view_proj";
	he_test::removeAllQuiet(proj);
	fs::create_directories(proj / "Content" / "World.cells");
	// Left over from an earlier split with another grid, and a file that is not a cell.
	{ std::ofstream(proj / "Content" / "World.cells" / "cell_9_9.hescene") << "{}"; }
	{ std::ofstream(proj / "Content" / "World.cells" / "notes.txt") << "mine"; }

	ContentManager cm((proj / "Content").string());
	HorizonWorld   world;
	EditorUndo     undo;
	undo.setWorld(&world);
	fillWorld(world, 3, 4);
	ContextBits bits;
	AppContext ctx = bits.make();
	ctx.world          = &world;
	ctx.undoSys        = &undo;
	ctx.contentManager = &cm;

	HE::CellSplitOptions o;
	o.cellSize = 100.0f;
	std::string message;
	// Never saved: there is nowhere to put the cells yet.
	CHECK_FALSE(StreamingDebugView::splitOpenScene(ctx, o, message));
	CHECK(message.find("Save the scene") != std::string::npos);
	CHECK_FALSE(undo.canUndo());

	bits.scenePath = (proj / "Content" / "World.hescene").string();
	REQUIRE(StreamingDebugView::splitOpenScene(ctx, o, message));
	CHECK(meshCount(world) == 0);
	HE::CellManifest m;
	REQUIRE(HE::CellManifest::parse(world.cellManifestJson(), m));
	CHECK(m.dir == "Content/World.cells");   // project-relative, as the game reads it
	CHECK(m.cells.size() == 3);
	for (int x = 0; x < 3; ++x)
		CHECK(fs::exists(proj / "Content" / "World.cells" / ("cell_" + std::to_string(x) + "_0.hescene")));
	CHECK_FALSE(fs::exists(proj / "Content" / "World.cells" / "cell_9_9.hescene"));   // stale cell gone
	CHECK(fs::exists(proj / "Content" / "World.cells" / "notes.txt"));                // the rest stays
	CHECK(undo.canUndo());
	CHECK(StreamingDebugView::manifestOf(world) != nullptr);

	// The overlay draws the squares and the two rings for a split scene.
	DebugDrawBuffer lines;
	StreamingDebugView::appendCellLines(world, glm::vec3(50.0f, 10.0f, 50.0f), lines);
	CHECK(lines.lines().size() >= 3 * 4 + 2 * 96);

	REQUIRE(StreamingDebugView::mergeOpenScene(ctx, message));
	CHECK(meshCount(world) == 12);
	CHECK(world.cellManifestJson().empty());
	CHECK(StreamingDebugView::manifestOf(world) == nullptr);
	DebugDrawBuffer none;
	StreamingDebugView::appendCellLines(world, glm::vec3(0.0f), none);
	CHECK(none.lines().empty());

	// Undo takes back the merge (split again), then the split (whole again).
	REQUIRE(undo.undo());
	CHECK(meshCount(world) == 0);
	CHECK_FALSE(world.cellManifestJson().empty());
	REQUIRE(undo.undo());
	CHECK(meshCount(world) == 12);
	CHECK(world.cellManifestJson().empty());

	// A merge whose cells are gone changes nothing and adds no history.
	REQUIRE(undo.redo());
	he_test::removeQuiet(proj / "Content" / "World.cells" / "cell_1_0.hescene");
	const bool couldRedo = undo.canRedo();
	CHECK_FALSE(StreamingDebugView::mergeOpenScene(ctx, message));
	CHECK(message.find("cell_1_0") != std::string::npos);
	CHECK(meshCount(world) == 0);
	CHECK(undo.canRedo() == couldRedo);

	he_test::removeAllQuiet(proj);
}

TEST_CASE("ui shot: the profiler's Streaming tab, a split scene seen from the editor camera")
{
	Harness h;
	const fs::path proj = fs::temp_directory_path() / "he_streaming_view_shot";
	he_test::removeAllQuiet(proj);
	fs::create_directories(proj / "Content");
	ContentManager cm((proj / "Content").string());
	HorizonWorld   world;
	EditorUndo     undo;
	undo.setWorld(&world);
	fillWorld(world, 6, 5);
	EditorCamera cam;
	ContextBits bits;
	bits.scenePath = (proj / "Content" / "World.hescene").string();
	AppContext ctx = bits.make();
	ctx.world          = &world;
	ctx.undoSys        = &undo;
	ctx.contentManager = &cm;
	ctx.editorCamera   = &cam;

	// Unsplit first: the tab offers the split.
	he_ui::Image before;
	frame(ctx);
	frame(ctx, &before);
	dump(before, "streaming_tab_unsplit.bmp");

	HE::CellSplitOptions o;
	o.cellSize = 100.0f;
	std::string message;
	REQUIRE(StreamingDebugView::splitOpenScene(ctx, o, message));
	he_ui::Image after;
	frame(ctx);
	frame(ctx, &after);
	dump(after, "streaming_tab_split.bmp");
	CHECK(after.valid());
	CHECK(after.width == W);

	he_test::removeAllQuiet(proj);
}
