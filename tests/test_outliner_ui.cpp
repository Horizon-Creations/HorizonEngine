#include "doctest.h"
#include "ImGuiSoftwareRaster.h"

#include "OutlinerPanel.h"
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "EditorUndo.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"

#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/EntityVisibility.h>
#include <HorizonScene/Components/EditorLockComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/DefaultAssets.h>

#include <imgui.h>
#include <imgui_internal.h>   // GetHoveredID: which item the pointer is on

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// ── The World Outliner, driven headless ──────────────────────────────────────
// The eye and the padlock sit ON the row: the tree node spans the panel's
// width and the two buttons are drawn over its right end, which only works
// because the node is flagged AllowOverlap. That is exactly the kind of claim
// nothing but a pointer can check — a unit test of EntityVisibility proves what
// the eye DOES, not that a click on it reaches the eye rather than selecting
// the row. So this drives the real panel (OutlinerPanel::render, with a real
// AppContext) in a headless ImGui context: it finds the buttons by asking ImGui
// what is under the pointer, clicks them, and reads the world afterwards. With
// HE_UI_DUMP_DIR set the frame is written out as well, so the icons can be
// looked at (scripts/he_uishot.py turns the dump into PNGs).

using namespace HE::Ed;

namespace
{
	constexpr int W = 360, H = 260;

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

	// Everything AppContext insists on being given, in one place. The panel
	// reads world, selection, undoSys and isPlaying; the rest is here because
	// the struct has no defaults for its references.
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

	// One frame of the panel at a fixed place, with the pointer where the
	// caller put it. Returns the id ImGui says the pointer is on.
	ImGuiID frame(AppContext& ctx, bool mouseDown, he_ui::Image* shot = nullptr)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
		ImGui::SetNextWindowSize(ImVec2(float(W) - 20.0f, float(H) - 20.0f));
		OutlinerPanel::render(ctx);
		EditorWidgets::drawQueuedHelp();
		const ImGuiID hovered = ImGui::GetHoveredID();
		ImGui::Render();
		if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
		return hovered;
	}

	// What is under the pointer at (x, y), settled: ImGui resolves hover from
	// the previous frame, so the answer is the second frame's.
	ImGuiID idAt(AppContext& ctx, float x, float y)
	{
		ImGui::GetIO().AddMousePosEvent(x, y);
		frame(ctx, false);
		return frame(ctx, false);
	}

	// A click at (x, y): settle, press, release.
	void clickAt(AppContext& ctx, float x, float y)
	{
		ImGui::GetIO().AddMousePosEvent(x, y);
		frame(ctx, false);
		frame(ctx, false);
		frame(ctx, true);
		frame(ctx, false);
		frame(ctx, false);
	}

	// The rows, top to bottom, as the pointer sees them: walking down the
	// middle of the panel, every new non-zero id is a new item. The window's
	// title bar comes first, then the header's search box, then the World
	// root, then each entity row — so the first entity is rows[3].
	struct Row { ImGuiID id; float yMid; };
	std::vector<Row> rowsDownTheMiddle(AppContext& ctx)
	{
		std::vector<Row> rows;
		ImGuiID last = 0; float yStart = 0.0f;
		const float x = 10.0f + (float(W) - 20.0f) * 0.4f;   // left of centre, clear of the icons
		for (float y = 12.0f; y < float(H) - 12.0f; y += 2.0f)
		{
			const ImGuiID id = idAt(ctx, x, y);
			if (id == last) continue;
			if (last != 0) rows.push_back({ last, (yStart + y - 2.0f) * 0.5f });
			last = id; yStart = y;
		}
		if (last != 0) rows.push_back({ last, (yStart + float(H) - 12.0f) * 0.5f });
		return rows;
	}

	// The items to the RIGHT of a row's own id on its line, right to left:
	// the padlock first, then the eye. Each is a different id from the row's.
	std::vector<float> iconXsOnRow(AppContext& ctx, ImGuiID rowId, float y)
	{
		std::vector<float> xs;
		ImGuiID last = rowId; float xStart = 0.0f;
		// From just inside the window's right border (the border itself is
		// the resize handle, an item of its own) to the middle.
		for (float x = float(W) - 18.0f; x > float(W) * 0.5f; x -= 1.0f)
		{
			const ImGuiID id = idAt(ctx, x, y);
			if (id == last) continue;
			if (last != rowId && last != 0) xs.push_back((xStart + x + 1.0f) * 0.5f);
			last = id; xStart = x;
		}
		return xs;
	}
}

TEST_CASE("outliner ui: the eye and the padlock take the click, the row keeps the selection")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	auto& reg = world.registry();

	// Two meshes at the root, one group with a mesh under it.
	const Entity crate = world.createEntity("Crate");
	reg.emplace<TransformComponent>(crate);
	reg.emplace<MeshComponent>(crate, MeshComponent{ HE::kDefaultCubeMeshId });
	const Entity house = world.createEntity("House");
	reg.emplace<TransformComponent>(house);
	const Entity door = world.createEntity("Door");
	reg.emplace<TransformComponent>(door);
	reg.emplace<MeshComponent>(door, MeshComponent{ HE::kDefaultCubeMeshId });
	world.reparentEntity(door, house);

	ContextBits bits;
	AppContext ctx = bits.make(world, undo);

	// Pointer away, a few frames to settle the layout; one shot to look at.
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	he_ui::Image img;
	for (int i = 0; i < 4; ++i) frame(ctx, false, i == 3 ? &img : nullptr);
	REQUIRE(img.valid());
	if (const char* dir = std::getenv("HE_UI_DUMP_DIR"); dir && *dir)
		he_ui::writeBmp(img, std::string(dir) + "/outliner-rows.bmp");

	// Rows: title bar, search box, World, Crate, House, Door (House opens by
	// default).
	const std::vector<Row> rows = rowsDownTheMiddle(ctx);
	REQUIRE_MESSAGE(rows.size() >= 6, "found " << rows.size() << " items down the middle");
	const Row worldRow = rows[2];
	const Row crateRow = rows[3];
	const Row houseRow = rows[4];

	// The World root is the tree's built-in top: no eye, no padlock.
	CHECK(iconXsOnRow(ctx, worldRow.id, worldRow.yMid).empty());

	// ── The padlock and the eye are on the Crate row, right of its name ──
	const std::vector<float> icons = iconXsOnRow(ctx, crateRow.id, crateRow.yMid);
	REQUIRE_MESSAGE(icons.size() >= 2, "found " << icons.size() << " icons on the Crate row");
	const float lockX = icons[0];
	const float eyeX  = icons[1];
	CHECK(lockX > eyeX);

	// A click on the eye hides the crate — and does NOT select the row.
	REQUIRE(reg.get<MeshComponent>(crate).visible);
	clickAt(ctx, eyeX, crateRow.yMid);
	CHECK_FALSE(reg.get<MeshComponent>(crate).visible);
	CHECK(bits.selection.empty());
	// It was an undo step.
	CHECK(undo.canUndo());
	// Again: shown.
	clickAt(ctx, eyeX, crateRow.yMid);
	CHECK(reg.get<MeshComponent>(crate).visible);

	// The padlock locks it; the row is still not selected.
	CHECK_FALSE(reg.all_of<EditorLockComponent>(crate));
	clickAt(ctx, lockX, crateRow.yMid);
	CHECK(reg.all_of<EditorLockComponent>(crate));
	CHECK(bits.selection.empty());
	clickAt(ctx, lockX, crateRow.yMid);
	CHECK_FALSE(reg.all_of<EditorLockComponent>(crate));

	// A click on the NAME selects the row, as it always did.
	clickAt(ctx, 10.0f + (float(W) - 20.0f) * 0.4f, crateRow.yMid);
	CHECK(bits.selection.contains(crate));
	CHECK(bits.selection.primary() == crate);

	// ── The eye on a group reaches the mesh under it ──
	const std::vector<float> houseIcons = iconXsOnRow(ctx, houseRow.id, houseRow.yMid);
	REQUIRE(houseIcons.size() >= 2);
	REQUIRE(reg.get<MeshComponent>(door).visible);
	clickAt(ctx, houseIcons[1], houseRow.yMid);
	CHECK_FALSE(reg.get<MeshComponent>(door).visible);
	CHECK(HE::subtreeVisibility(reg, house) == HE::Visibility::Hidden);

	// And a shot of that state, for the eyes: an open eye, two shut, a locked
	// padlock beside the unlocked ones, the selected row.
	reg.emplace<EditorLockComponent>(door);
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 3; ++i) frame(ctx, false, i == 2 ? &img : nullptr);
	if (const char* dir = std::getenv("HE_UI_DUMP_DIR"); dir && *dir)
		he_ui::writeBmp(img, std::string(dir) + "/outliner-rows-hidden.bmp");
}

TEST_CASE("outliner ui: nothing to draw under a row means no eye, and the lock is still there")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	auto& reg = world.registry();
	const Entity group = world.createEntity("Empty Group");
	reg.emplace<TransformComponent>(group);

	ContextBits bits;
	AppContext ctx = bits.make(world, undo);
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 4; ++i) frame(ctx, false);

	const std::vector<Row> rows = rowsDownTheMiddle(ctx);
	REQUIRE(rows.size() >= 4);
	const Row groupRow = rows[3];
	// Only ONE item right of the name: the padlock. The eye's place is a
	// Dummy, which has no id.
	const std::vector<float> icons = iconXsOnRow(ctx, groupRow.id, groupRow.yMid);
	CHECK(icons.size() == 1);
	REQUIRE(!icons.empty());
	clickAt(ctx, icons[0], groupRow.yMid);
	CHECK(reg.all_of<EditorLockComponent>(group));
}

// ── Rows out of view (Thema 153, Schritt 6) ──────────────────────────────────
// The panel skips rows scrolled out of view instead of submitting them, and
// rebuilds the IDs, folds and indentation it would have had. Whatever it does,
// the panel must look and scroll exactly as when every row is drawn: the same
// picture to the pixel and the same scroll range — at the top, in the middle
// of a branch, past a folded one, and at the very bottom.
TEST_CASE("outliner ui: rows out of view are skipped, the panel looks and scrolls exactly as before")
{
	Harness h;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	ContextBits bits;
	AppContext  ctx = bits.make(world, undo);
	std::vector<Entity> groups;
	for (int g = 0; g < 10; ++g)
	{
		const Entity group = world.createEntity("Group " + std::to_string(g));
		groups.push_back(group);
		for (int i = 0; i < 40; ++i)
		{
			const Entity e = world.createEntity("Item " + std::to_string(g) + "." + std::to_string(i));
			world.reparentEntity(e, group);
		}
	}
	world.markHierarchyDirty();
	ImGui::GetIO().AddMousePosEvent(-1.0f, -1.0f);   // nothing hovered: no highlight to tell apart
	for (int i = 0; i < 3; ++i) frame(ctx, false);
	ImGuiWindow* win = ImGui::FindWindowByName("World Outliner");
	REQUIRE(win != nullptr);
	const float unfolded = win->ContentSize.y;

	// Fold Group 3 the way a click on its arrow does: its id in the window's
	// storage. The World root's row hangs off the window's own id; Group 3's
	// off the root's (TreeNodeEx pushes the row's id for its children).
	const auto idOf = [](Entity e, ImGuiID seed)
	{
		const void* ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(e)));
		return ImHashData(&ptr, sizeof(void*), seed);
	};
	win->StateStorage.SetInt(idOf(groups[3], idOf(world.rootEntity(), win->ID)), 0);

	struct Look { he_ui::Image img; float contentH; float scrollMax; float scroll; };
	const auto look = [&](bool clipping, float scrollY) -> Look
	{
		OutlinerPanel::setRowClipping(clipping);
		ImGui::SetScrollY(win, scrollY);
		for (int i = 0; i < 3; ++i) frame(ctx, false);   // scroll applied, row height measured
		Look l;
		frame(ctx, false, &l.img);
		l.contentH  = win->ContentSize.y;
		l.scrollMax = win->ScrollMax.y;
		l.scroll    = win->Scroll.y;
		if (const char* dir = std::getenv("HE_UI_DUMP_DIR"))
		{
			char name[128];
			std::snprintf(name, sizeof(name), "%s/outliner_clip_%s_%d.bmp", dir, clipping ? "on" : "off",
			              static_cast<int>(scrollY));
			he_ui::writeBmp(l.img, name);
		}
		return l;
	};
	for (const float scrollY : { 0.0f, 900.0f, 2600.0f, 1.0e6f })
	{
		CAPTURE(scrollY);
		const Look all  = look(false, scrollY);
		const Look some = look(true, scrollY);
		CHECK(all.scrollMax > 1000.0f);               // a long list: most rows are out of view
		CHECK(all.contentH < unfolded - 30 * 15.0f);  // …and Group 3's forty rows are folded away
		CHECK(some.contentH == all.contentH);
		CHECK(some.scrollMax == all.scrollMax);
		CHECK(some.scroll == all.scroll);
		REQUIRE(some.img.valid());
		CHECK(some.img.rgba == all.img.rgba);
	}
	OutlinerPanel::setRowClipping(true);
}
