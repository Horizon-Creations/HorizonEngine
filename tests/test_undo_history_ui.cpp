#include "doctest.h"
#include "ImGuiSoftwareRaster.h"

#include "UndoHistoryPanel.h"
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "EditorUndo.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"

#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/EnvironmentLightComponent.h>

#include <imgui.h>
#include <imgui_internal.h>   // GetHoveredID: which item the pointer is on

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// ── The Undo History window, driven headless ─────────────────────────────────
// The window is a list whose rows are the undo stack's labels. What can be
// checked without a person: that the real panel, on a real EditorUndo over a
// real world, puts one row per step on screen with the current-state marker
// between the undo and redo halves, and that a click on a row far up the list
// rewinds the WORLD to that point in one go (and one on the redo half brings it
// forward again). With HE_UI_DUMP_DIR set the frames are written out as well.

using namespace HE::Ed;

namespace
{
	constexpr int W = 320, H = 360;

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

	// Everything AppContext insists on being given. The panel reads world,
	// undoSys, selection and projectLoaded; the rest is here because the
	// struct has no defaults for its references.
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

	// One frame of the window at a fixed place, with the pointer where the
	// caller put it. Returns the id ImGui says the pointer is on.
	ImGuiID frame(AppContext& ctx, bool mouseDown, he_ui::Image* shot = nullptr)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
		ImGui::SetNextWindowSize(ImVec2(float(W) - 20.0f, float(H) - 20.0f));
		bool open = true;
		UndoHistoryPanel::DrawUndoHistoryWindow(ctx, open);
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

	struct Item { ImGuiID id; float mid; };

	// Every distinct item the pointer crosses walking DOWN at x. The rows sit in
	// a child window whose own id is reported between them; anything crossed
	// more than once is that background and dropped.
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

	// Root + authored entities, without the two built-in environment lights.
	int entityCount(HorizonWorld& w)
	{
		auto& reg = w.registry();
		int n = 0;
		for (auto e : reg.view<entt::entity>())
			if (!reg.all_of<EnvironmentLightComponent>(e)) ++n;
		return n;
	}

	constexpr std::uint8_t kBgR = 20, kBgG = 18, kBgB = 15;
}

TEST_CASE("undo history ui: one row per step, and a click rewinds the world to that row")
{
	Harness harness;

	HorizonWorld world;
	EditorUndo undo;
	undo.setWorld(&world);
	undo.snapshotNow("Create A"); world.createEntity("A");
	undo.snapshotNow("Create B"); world.createEntity("B");
	undo.snapshotNow("Create C"); world.createEntity("C");
	undo.snapshotNow("Create D"); world.createEntity("D");
	REQUIRE(entityCount(world) == 5);

	ContextBits bits;
	AppContext ctx = bits.make(world, undo);

	// Pointer away, a few frames to settle; one shot to look at.
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	he_ui::Image img;
	for (int i = 0; i < 4; ++i) frame(ctx, false, i == 3 ? &img : nullptr);
	REQUIRE(img.valid());
	CHECK(img.inkedPixels(kBgR, kBgG, kBgB) > 2000);
	if (const char* d = std::getenv("HE_UI_DUMP_DIR"); d && *d)
		he_ui::writeBmp(img, std::string(d) + "/undo-history.bmp");

	// Down the list: the four undo rows, then the marker (disabled, but a
	// disabled Selectable is still an item the pointer is reported on).
	// Started below the header line and its Clear button.
	std::vector<Item> rows = itemsDown(ctx, 60.0f, 68.0f, float(H) - 30.0f);
	REQUIRE_MESSAGE(rows.size() == 5, "found " << rows.size() << " rows in a 4-step history");
	// Clicking the marker is a no-op: the world stays, the stacks stay.
	clickAt(ctx, 60.0f, rows[4].mid);
	CHECK(entityCount(world) == 5);
	CHECK(undo.undoDepth() == 4);

	// Click the second row ("Create B"): the world goes back to before B —
	// three steps in one click — and the selection is cleared with it.
	ctx.selection.set(world.rootEntity());
	clickAt(ctx, 60.0f, rows[1].mid);
	CHECK(entityCount(world) == 2);          // root + A
	CHECK(ctx.selection.empty());
	CHECK(undo.undoDepth() == 1);
	CHECK(undo.redoDepth() == 3);

	// Now the list is one undo row, the marker, three redo rows.
	rows = itemsDown(ctx, 60.0f, 68.0f, float(H) - 30.0f);
	REQUIRE_MESSAGE(rows.size() == 5, "found " << rows.size() << " rows after the jump");
	frame(ctx, false, &img);
	if (const char* d = std::getenv("HE_UI_DUMP_DIR"); d && *d)
		he_ui::writeBmp(img, std::string(d) + "/undo-history-rewound.bmp");

	// Click the LAST redo row ("Create D"): everything comes back at once.
	clickAt(ctx, 60.0f, rows[4].mid);
	CHECK(entityCount(world) == 5);
	CHECK(undo.undoDepth() == 4);
	CHECK(undo.redoDepth() == 0);
}

TEST_CASE("undo history ui: no undo system (play mode) says so instead of listing")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo undo;
	undo.setWorld(&world);
	undo.snapshotNow("Create A"); world.createEntity("A");

	ContextBits bits;
	AppContext ctx = bits.make(world, undo);
	ctx.undoSys   = nullptr;
	ctx.isPlaying = true;

	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 3; ++i) frame(ctx, false);
	const std::vector<Item> rows = itemsDown(ctx, 60.0f, 68.0f, float(H) - 30.0f);
	CHECK(rows.empty());                     // a sentence, not rows
	CHECK(entityCount(world) == 2);          // and nothing was touched
}
