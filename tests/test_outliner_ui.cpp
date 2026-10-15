#include "doctest.h"
#include "ImGuiSoftwareRaster.h"

#include "OutlinerPanel.h"
#include "OutlinerFilter.h"
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "EditorUndo.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"

#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/EntityVisibility.h>
#include <HorizonScene/Components/EditorLockComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/Components/PrefabInstanceComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/DefaultAssets.h>

#include <imgui.h>
#include <imgui_internal.h>   // GetHoveredID: which item the pointer is on

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
	// The window the panel is drawn in. Not constants: the showcase below wants a taller one.
	int W = 360, H = 260;

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
	std::vector<Row> rowsDownTheMiddle(AppContext& ctx, float x = -1.0f)
	{
		std::vector<Row> rows;
		ImGuiID last = 0; float yStart = 0.0f;
		if (x < 0.0f) x = 10.0f + (float(W) - 20.0f) * 0.4f;   // left of centre, clear of the icons
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


// ── Chips, icons, badges, renaming, dropping ─────────────────────────────────
namespace
{
	ImGuiWindow* outlinerWindow() { return ImGui::FindWindowByName("World Outliner"); }

	// The id of one type chip: PushID(kind) then "##chip", under the window.
	ImGuiID chipId(int kind)
	{
		const ImGuiID seed = ImHashData(&kind, sizeof(int), outlinerWindow()->ID);
		return ImHashStr("##chip", 0, seed);
	}

	int kindIndex(const char* label)
	{
		for (int i = 0; i < OutlinerFilter::kindCount(); ++i)
			if (std::string(OutlinerFilter::kindAt(i).label) == label) return i;
		return -1;
	}

	// The middle of an item, found by what is under the pointer along a row of
	// the window: first x where the hovered id is `id`, to the last.
	bool findAlongX(AppContext& ctx, ImGuiID id, float y, float& xMid)
	{
		float first = -1.0f, last = -1.0f;
		for (float x = 12.0f; x < float(W) - 12.0f; x += 1.0f)
			if (idAt(ctx, x, y) == id) { if (first < 0.0f) first = x; last = x; }
		xMid = (first + last) * 0.5f;
		return first >= 0.0f;
	}
	bool findAlongY(AppContext& ctx, ImGuiID id, float x, float& yMid)
	{
		float first = -1.0f, last = -1.0f;
		for (float y = 12.0f; y < float(H) - 12.0f; y += 1.0f)
			if (idAt(ctx, x, y) == id) { if (first < 0.0f) first = y; last = y; }
		yMid = (first + last) * 0.5f;
		return first >= 0.0f;
	}

	// The panel at rest: pointer in the corner, a few frames to settle.
	void settle(AppContext& ctx, he_ui::Image* shot = nullptr)
	{
		ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
		for (int i = 0; i < 4; ++i) frame(ctx, false, i == 3 ? shot : nullptr);
	}

	void key(AppContext& ctx, ImGuiKey k)
	{
		ImGui::GetIO().AddKeyEvent(k, true);
		frame(ctx, false);
		ImGui::GetIO().AddKeyEvent(k, false);
		frame(ctx, false);
	}

	// A press on (x0, y0), a drag to (x1, y1) in steps, a release.
	void dragFromTo(AppContext& ctx, float x0, float y0, float x1, float y1)
	{
		ImGui::GetIO().AddMousePosEvent(x0, y0);
		frame(ctx, false);
		frame(ctx, false);
		frame(ctx, true);
		for (int i = 1; i <= 8; ++i)
		{
			const float t = float(i) / 8.0f;
			ImGui::GetIO().AddMousePosEvent(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t);
			frame(ctx, true);
		}
		frame(ctx, true);
		frame(ctx, false);
		frame(ctx, false);
	}

	// Pixels in a box around (x, y) that differ from the panel's own background,
	// taken from (refX, y) — a blank spot on the same row.
	int inkAround(const he_ui::Image& img, float x, float y, float refX, int r = 5)
	{
		std::uint8_t br, bg, bb, ba;
		img.pixel(int(refX), int(y), br, bg, bb, ba);
		int n = 0;
		for (int py = int(y) - r; py <= int(y) + r; ++py)
			for (int px = int(x) - r; px <= int(x) + r; ++px)
			{
				if (px < 0 || py < 0 || px >= img.width || py >= img.height) continue;
				std::uint8_t cr, cg, cb, ca;
				img.pixel(px, py, cr, cg, cb, ca);
				if (std::abs(int(cr) - br) + std::abs(int(cg) - bg) + std::abs(int(cb) - bb) > 24) ++n;
			}
		return n;
	}

	std::vector<std::string> rootNames(HorizonWorld& world, const std::vector<std::string>& only)
	{
		std::vector<std::string> out;
		for (const Entity c : world.registry().get<HierarchyComponent>(world.rootEntity()).children)
		{
			const std::string& n = world.registry().get<NameComponent>(c).name;
			if (std::find(only.begin(), only.end(), n) != only.end()) out.push_back(n);
		}
		return out;
	}
}

TEST_CASE("outliner ui: the type chips count the scene; a click narrows to one kind, a second click lifts it")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	auto& reg = world.registry();
	for (const char* n : { "Crate", "Barrel" })
	{
		const Entity e = world.createEntity(n);
		reg.emplace<TransformComponent>(e);
		reg.emplace<MeshComponent>(e, MeshComponent{ HE::kDefaultCubeMeshId });
	}
	const Entity lamp = world.createEntity("Lamp");
	reg.emplace<TransformComponent>(lamp);
	reg.emplace<LightComponent>(lamp);
	const Entity cam = world.createEntity("Camera");
	reg.emplace<TransformComponent>(cam);
	reg.emplace<CameraComponent>(cam);

	ContextBits bits;
	AppContext ctx = bits.make(world, undo);
	settle(ctx);
	const size_t all = rowsDownTheMiddle(ctx).size();   // title, search, World, four entities

	// Chips for the kinds in the scene and for "All" — and none for the rest.
	float y = 0.0f, xMesh = 0.0f, xLight = 0.0f, xRig = 0.0f;
	REQUIRE(findAlongY(ctx, chipId(OutlinerFilter::kAllKinds), 24.0f, y));
	CHECK(findAlongX(ctx, chipId(kindIndex("Mesh")), y, xMesh));
	CHECK(findAlongX(ctx, chipId(kindIndex("Light")), y, xLight));
	CHECK_FALSE(findAlongX(ctx, chipId(kindIndex("Rigid Body")), y, xRig));

	// Mesh: the World (as the path) and the two crates.
	clickAt(ctx, xMesh, y);
	const size_t meshes = rowsDownTheMiddle(ctx).size();
	CHECK(meshes == all - 2);
	// The same chip again: everything is back.
	clickAt(ctx, xMesh, y);
	CHECK(rowsDownTheMiddle(ctx).size() == all);
	// Light: the World and the lamp.
	clickAt(ctx, xLight, y);
	CHECK(rowsDownTheMiddle(ctx).size() == all - 3);
	clickAt(ctx, xLight, y);
	CHECK(rowsDownTheMiddle(ctx).size() == all);
}

TEST_CASE("outliner ui: a hidden or locked row keeps its eye or padlock showing, a plain one shows them only under the pointer")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	auto& reg = world.registry();
	const Entity crate = world.createEntity("Crate");
	reg.emplace<TransformComponent>(crate);
	reg.emplace<MeshComponent>(crate, MeshComponent{ HE::kDefaultCubeMeshId });
	ContextBits bits;
	AppContext ctx = bits.make(world, undo);
	settle(ctx);

	const std::vector<Row> rows = rowsDownTheMiddle(ctx);
	REQUIRE(rows.size() >= 4);
	const Row crateRow = rows[3];
	const std::vector<float> icons = iconXsOnRow(ctx, crateRow.id, crateRow.yMid);
	REQUIRE(icons.size() >= 2);
	const float lockX = icons[0], eyeX = icons[1];
	const float refX  = float(W) * 0.5f;

	he_ui::Image img;
	settle(ctx, &img);
	CHECK(inkAround(img, eyeX, crateRow.yMid, refX) == 0);
	CHECK(inkAround(img, lockX, crateRow.yMid, refX) == 0);

	// Hidden: the slashed eye stays after the pointer has gone.
	clickAt(ctx, eyeX, crateRow.yMid);
	REQUIRE_FALSE(reg.get<MeshComponent>(crate).visible);
	settle(ctx, &img);
	CHECK(inkAround(img, eyeX, crateRow.yMid, refX) > 0);
	CHECK(inkAround(img, lockX, crateRow.yMid, refX) == 0);

	// Locked: the padlock too.
	clickAt(ctx, lockX, crateRow.yMid);
	REQUIRE(reg.all_of<EditorLockComponent>(crate));
	settle(ctx, &img);
	CHECK(inkAround(img, lockX, crateRow.yMid, refX) > 0);
}

TEST_CASE("outliner ui: F2 and a double-click rename a row in place; Enter keeps the name, Esc drops it")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	auto& reg = world.registry();
	const Entity crate = world.createEntity("Crate");
	reg.emplace<TransformComponent>(crate);
	const auto nameOf = [&] { return reg.get<NameComponent>(crate).name; };
	ContextBits bits;
	AppContext ctx = bits.make(world, undo);
	settle(ctx);
	const Row crateRow = rowsDownTheMiddle(ctx)[3];
	const float nameX = 10.0f + (float(W) - 20.0f) * 0.4f;

	// Select it, F2, type, Enter.
	clickAt(ctx, nameX, crateRow.yMid);
	REQUIRE(bits.selection.contains(crate));
	ImGui::SetWindowFocus("World Outliner");
	frame(ctx, false);
	key(ctx, ImGuiKey_F2);
	frame(ctx, false);
	ImGui::GetIO().AddInputCharactersUTF8("Barrel");
	frame(ctx, false);
	frame(ctx, false);
	key(ctx, ImGuiKey_Enter);
	frame(ctx, false);
	CHECK(nameOf() == "Barrel");
	CHECK(undo.canUndo());

	// Esc leaves the name alone.
	key(ctx, ImGuiKey_F2);
	frame(ctx, false);
	ImGui::GetIO().AddInputCharactersUTF8("Nope");
	frame(ctx, false);
	frame(ctx, false);
	key(ctx, ImGuiKey_Escape);
	frame(ctx, false);
	CHECK(nameOf() == "Barrel");

	// A double-click on the name opens the box too.
	ImGui::GetIO().AddMousePosEvent(nameX, crateRow.yMid);
	frame(ctx, false);
	frame(ctx, false);
	for (int i = 0; i < 2; ++i)
	{
		frame(ctx, true);
		frame(ctx, false);
	}
	frame(ctx, false);
	ImGui::GetIO().AddInputCharactersUTF8("Crate");
	frame(ctx, false);
	frame(ctx, false);
	key(ctx, ImGuiKey_Enter);
	frame(ctx, false);
	CHECK(nameOf() == "Crate");
}

TEST_CASE("outliner ui: dropping on the top or bottom edge of a row sorts the entity beside it, the middle makes it a child")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	const std::vector<std::string> names = { "A", "B", "C" };
	std::vector<Entity> es;
	for (const auto& n : names) es.push_back(world.createEntity(n));
	ContextBits bits;
	AppContext ctx = bits.make(world, undo);
	settle(ctx);
	std::vector<Row> rows = rowsDownTheMiddle(ctx);
	REQUIRE(rows.size() >= 6);   // title, search, World, A, B, C
	const float x = 10.0f + (float(W) - 20.0f) * 0.4f;
	REQUIRE(rootNames(world, names) == names);

	// A onto the bottom edge of C: B, C, A.
	dragFromTo(ctx, x, rows[3].yMid, x, rows[5].yMid + 9.0f);
	CHECK(rootNames(world, names) == std::vector<std::string>{ "B", "C", "A" });

	// The list as the panel shows it now: B, C, A. A onto the top edge of B: A, B, C.
	settle(ctx);
	rows = rowsDownTheMiddle(ctx);
	dragFromTo(ctx, x, rows[5].yMid, x, rows[3].yMid - 9.0f);
	CHECK(rootNames(world, names) == names);

	// The middle of a row: A becomes a child of B.
	settle(ctx);
	rows = rowsDownTheMiddle(ctx);
	dragFromTo(ctx, x, rows[3].yMid, x, rows[4].yMid);
	CHECK(world.registry().get<HierarchyComponent>(es[0]).parent == es[1]);
}

TEST_CASE("outliner ui: Alt on a folded row's arrow opens everything under it")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	const Entity group = world.createEntity("Group");
	const Entity inner = world.createEntity("Inner");
	const Entity leaf  = world.createEntity("Leaf");
	world.reparentEntity(inner, group);
	world.reparentEntity(leaf, inner);
	world.markHierarchyDirty();
	ContextBits bits;
	AppContext ctx = bits.make(world, undo);
	settle(ctx);
	ImGuiWindow* win = outlinerWindow();
	REQUIRE(win != nullptr);

	const auto idOf = [](Entity e, ImGuiID seed)
	{
		const void* ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(e)));
		return ImHashData(&ptr, sizeof(void*), seed);
	};
	const ImGuiID groupId = idOf(group, idOf(world.rootEntity(), win->ID));
	const ImGuiID innerId = idOf(inner, groupId);
	win->StateStorage.SetInt(groupId, 0);
	win->StateStorage.SetInt(innerId, 0);
	settle(ctx);

	// The arrow of the Group row, one level in.
	const std::vector<Row> rows = rowsDownTheMiddle(ctx);
	REQUIRE(rows.size() >= 4);
	const ImGuiStyle& st = ImGui::GetStyle();
	const float arrowX = win->WorkRect.Min.x + st.IndentSpacing + st.FramePadding.x + ImGui::GetFontSize() * 0.5f;
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, true);
	clickAt(ctx, arrowX, rows[3].yMid);
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, false);
	frame(ctx, false);
	CHECK(win->StateStorage.GetInt(groupId, 0) != 0);
	CHECK(win->StateStorage.GetInt(innerId, 0) != 0);
}

// ── A look at all of it ──────────────────────────────────────────────────────
// Not a check of anything but that it draws: the frames written out are for
// looking at (HE_UI_DUMP_DIR), one with the pointer on a row, one searched.
TEST_CASE("outliner ui: showcase frames for the eye")
{
	const int w0 = W, h0 = H;
	W = 440; H = 560;
	{
		Harness harness;
		HorizonWorld world;
		EditorUndo   undo;
		undo.setWorld(&world);
		auto& reg = world.registry();
		const auto make = [&](const char* name, Entity parent = entt::null) {
			const Entity e = world.createEntity(name);
			reg.emplace<TransformComponent>(e);
			if (parent != entt::null) world.reparentEntity(e, parent);
			return e;
		};
		const auto mesh = [&](Entity e) { reg.emplace<MeshComponent>(e, MeshComponent{ HE::kDefaultCubeMeshId }); };
		reg.emplace<CameraComponent>(make("Main Camera"));
		reg.emplace<LightComponent>(make("Sun"));
		const Entity level = make("Level");
		const Entity floor = make("Floor", level);
		mesh(floor);
		reg.emplace<ColliderComponent>(floor);
		const Entity c1 = make("Crate_01", level); mesh(c1);
		const Entity c2 = make("Crate_02", level); mesh(c2);
		const Entity c3 = make("Crate_03", level); mesh(c3);
		const Entity props = make("Props", level);
		for (const char* n : { "Barrel", "Lantern", "Sign" }) mesh(make(n, props));
		const Entity torch = make("Torch", level);
		mesh(torch);
		reg.emplace<PrefabInstanceComponent>(torch);
		const Entity longName = make("An_entity_with_a_name_that_is_far_too_long_for_this_panel");
		reg.emplace<LightComponent>(longName);
		ContextBits bits;
		AppContext ctx = bits.make(world, undo);
		world.markHierarchyDirty();
		settle(ctx);

		ImGuiWindow* win = outlinerWindow();
		const auto idOf = [](Entity e, ImGuiID seed)
		{
			const void* ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(e)));
			return ImHashData(&ptr, sizeof(void*), seed);
		};
		win->StateStorage.SetInt(idOf(props, idOf(level, idOf(world.rootEntity(), win->ID))), 0);   // Props folded

		reg.get<MeshComponent>(c2).visible = false;
		reg.emplace<EditorLockComponent>(c3);
		bits.selection.set(c1);
		settle(ctx);

		he_ui::Image img;
		const std::vector<Row> rows = rowsDownTheMiddle(ctx);
		REQUIRE(rows.size() >= 8);
		ImGui::GetIO().AddMousePosEvent(40.0f + float(W) * 0.3f, rows[7].yMid);
		for (int i = 0; i < 3; ++i) frame(ctx, false, i == 2 ? &img : nullptr);
		REQUIRE(img.valid());
		if (const char* dir = std::getenv("HE_UI_DUMP_DIR"); dir && *dir)
			he_ui::writeBmp(img, std::string(dir) + "/outliner-showcase.bmp");
		CHECK(img.inkedPixels(20, 18, 15) > 500);

		// Searched: the hits marked in their names, the rest folded away; then
		// the cross puts everything back.
		clickAt(ctx, 40.0f + float(W) * 0.3f, rows[1].yMid);
		ImGui::GetIO().AddInputCharactersUTF8("crate");
		for (int i = 0; i < 4; ++i) frame(ctx, false, i == 3 ? &img : nullptr);
		if (const char* dir = std::getenv("HE_UI_DUMP_DIR"); dir && *dir)
			he_ui::writeBmp(img, std::string(dir) + "/outliner-showcase-search.bmp");
		CHECK(rowsDownTheMiddle(ctx).size() < rows.size());
		clickAt(ctx, win->WorkRect.Max.x - ImGui::GetFrameHeight() * 0.5f, rows[1].yMid);
		CHECK(rowsDownTheMiddle(ctx).size() == rows.size());
	}
	W = w0; H = h0;
}
