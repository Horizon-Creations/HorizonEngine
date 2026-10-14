#include "doctest.h"
#include "ImGuiSoftwareRaster.h"
#include "TestFsUtil.h"

#include "TaskBoardPanel.h"
#include "TaskBoard.h"
#include "EditorApplication.h"   // AppContext
#include "EditorCommands.h"      // uuidOf
#include "EditorSelection.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include "ProjectManager.h"

#include <ContentManager/ContentManager.h>
#include <HorizonScene/HorizonWorld.h>

#include <imgui.h>
#include <imgui_internal.h>   // GetHoveredID / FindWindowByName

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// ── The Task Board window, driven headless ───────────────────────────────────
// The board's data and its file are TaskBoard's (test_task_board.cpp); what is
// checked here is the window on top of them, over a real project on disk: that
// New Task makes a card and a file, that an asset, a scene and an entity
// dragged onto a card — with the payloads the Content Browser and the World
// Outliner really send — become links, that clicking a chip selects the entity
// or asks EditorUI for the jump, that a card dragged to another lane changes
// its status, that a long board only draws what is on screen, and that a pull
// landing under the open board is read instead of overwritten. With
// HE_UI_DUMP_DIR set the frames are written out as well.

using namespace HE::Ed;

namespace
{
	constexpr int W = 780, H = 660;
	// The board window, and a strip to its right that stands in for the
	// Content Browser and the Outliner: buttons that start the same drags.
	constexpr float kBoardX = 10.0f, kBoardY = 10.0f, kBoardW = 540.0f, kBoardH = 640.0f;
	constexpr float kSrcX = 570.0f, kSrcY = 10.0f;

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
			TaskBoardPanel::reset();
			TaskBoardPanel::setAuthor("Tester");
		}
		~Harness()
		{
			TaskBoardPanel::reset();
			ImGui::DestroyContext();
		}
	};

	// Everything AppContext insists on being given. The panel reads
	// projectLoaded, projectManager, contentManager, world, selection and
	// currentScenePath; the rest is here because the struct has no defaults
	// for its references.
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

	// The drag sources: what the Content Browser (an absolute path) and the
	// World Outliner (an entity handle) put in their payloads.
	struct Sources
	{
		std::string assetPath, scenePath;
		Entity      entity = entt::null;
		ImVec2      assetAt, sceneAt, entityAt;
	};

	void drawSources(Sources& s)
	{
		ImGui::SetNextWindowPos(ImVec2(kSrcX, kSrcY));
		ImGui::SetNextWindowSize(ImVec2(float(W) - kSrcX - 10.0f, 160.0f));
		ImGui::Begin("Sources", nullptr, ImGuiWindowFlags_NoDecoration);
		auto source = [](const char* label, const char* type, const void* data, size_t size, ImVec2& at)
		{
			ImGui::Button(label, ImVec2(-1.0f, 32.0f));
			at = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
			            (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
			if (ImGui::BeginDragDropSource())
			{
				ImGui::SetDragDropPayload(type, data, size);
				ImGui::TextUnformatted(label);
				ImGui::EndDragDropSource();
			}
		};
		source("Asset: Notes", "HE_ASSET_PATH", s.assetPath.c_str(), s.assetPath.size() + 1, s.assetAt);
		source("Scene: Main", "HE_ASSET_PATH", s.scenePath.c_str(), s.scenePath.size() + 1, s.sceneAt);
		source("Entity: Crate", "HE_ENTITY", &s.entity, sizeof(Entity), s.entityAt);
		ImGui::End();
	}

	bool g_open = true;

	ImGuiID frame(AppContext& ctx, bool mouseDown, he_ui::Image* shot = nullptr, Sources* src = nullptr)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(kBoardX, kBoardY));
		ImGui::SetNextWindowSize(ImVec2(kBoardW, kBoardH));
		TaskBoardPanel::DrawTaskBoardWindow(ctx, g_open);
		if (src) drawSources(*src);
		EditorWidgets::drawQueuedHelp();
		const ImGuiID hovered = ImGui::GetHoveredID();
		ImGui::Render();
		if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
		return hovered;
	}

	ImGuiID idAt(AppContext& ctx, ImVec2 p, Sources* src = nullptr)
	{
		ImGui::GetIO().AddMousePosEvent(p.x, p.y);
		frame(ctx, false, nullptr, src);
		return frame(ctx, false, nullptr, src);
	}

	void clickAt(AppContext& ctx, ImVec2 p, Sources* src = nullptr)
	{
		ImGui::GetIO().AddMousePosEvent(p.x, p.y);
		frame(ctx, false, nullptr, src);
		frame(ctx, false, nullptr, src);
		frame(ctx, true, nullptr, src);
		frame(ctx, false, nullptr, src);
		frame(ctx, false, nullptr, src);
	}

	// Press on `from`, move to `to` in steps (a drag has to travel past the
	// threshold and the target has to see the payload hover), release.
	void dragTo(AppContext& ctx, ImVec2 from, ImVec2 to, Sources* src = nullptr)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMousePosEvent(from.x, from.y);
		frame(ctx, false, nullptr, src);
		frame(ctx, false, nullptr, src);
		frame(ctx, true, nullptr, src);
		for (int i = 1; i <= 8; ++i)
		{
			const float t = float(i) / 8.0f;
			io.AddMousePosEvent(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t);
			frame(ctx, true, nullptr, src);
		}
		frame(ctx, true, nullptr, src);
		frame(ctx, false, nullptr, src);
		frame(ctx, false, nullptr, src);
	}

	ImVec2 centre(const TaskBoardPanel::Rect& r) { return ImVec2((r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f); }

	void shotTo(const he_ui::Image& img, const char* name)
	{
		if (const char* d = std::getenv("HE_UI_DUMP_DIR"); d && *d)
			he_ui::writeBmp(img, std::string(d) + "/" + name + ".bmp");
	}

	// Pixels that are not the clear colour inside a rectangle of the image.
	int inkIn(const he_ui::Image& img, int x0, int y0, int x1, int y1)
	{
		std::uint8_t br, bg, bb, ba;
		img.pixel(0, H - 1, br, bg, bb, ba);   // a corner no window covers
		int n = 0;
		for (int y = std::max(0, y0); y < std::min(H, y1); ++y)
			for (int x = std::max(0, x0); x < std::min(W, x1); ++x)
			{
				std::uint8_t r, g, b, a;
				img.pixel(x, y, r, g, b, a);
				n += (r != br || g != bg || b != bb);
			}
		return n;
	}

	// Pixels that differ between two frames inside a rectangle.
	int diffIn(const he_ui::Image& a, const he_ui::Image& b, int x0, int y0, int x1, int y1)
	{
		int n = 0;
		for (int y = std::max(0, y0); y < std::min(H, y1); ++y)
			for (int x = std::max(0, x0); x < std::min(W, x1); ++x)
			{
				std::uint8_t r1, g1, b1, a1, r2, g2, b2, a2;
				a.pixel(x, y, r1, g1, b1, a1);
				b.pixel(x, y, r2, g2, b2, a2);
				n += (r1 != r2 || g1 != g2 || b1 != b2);
			}
		return n;
	}

	// Pixels bright enough to be text (even dimmed text) inside a rectangle —
	// the panel's backgrounds and borders are all far darker.
	int textPixelsIn(const he_ui::Image& img, int x0, int y0, int x1, int y1)
	{
		int n = 0;
		for (int y = std::max(0, y0); y < std::min(H, y1); ++y)
			for (int x = std::max(0, x0); x < std::min(W, x1); ++x)
			{
				std::uint8_t r, g, b, a;
				img.pixel(x, y, r, g, b, a);
				n += (int(r) + int(g) + int(b)) > 240;
			}
		return n;
	}

	struct Project
	{
		std::filesystem::path dir;
		ProjectManager pm;
		explicit Project(const char* name)
		{
			dir = std::filesystem::temp_directory_path() / name;
			he_test::removeAllQuiet(dir);
			REQUIRE(pm.createNewProject(dir.string(), "Board", ProjectPreset::Game));
		}
		~Project() { he_test::removeAllQuiet(dir); }
		std::string root() const { return pm.projectRoot(); }
	};

	std::string readFile(const std::filesystem::path& p)
	{
		std::ifstream f(p, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(f), {});
	}
}

TEST_CASE("task board ui: New Task makes a card, a file and the details")
{
	Harness harness;
	Project proj("he_taskboard_ui_new");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	g_open = true;

	he_ui::Image empty;
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 3; ++i) frame(ctx, false, i == 2 ? &empty : nullptr);
	REQUIRE(empty.valid());
	shotTo(empty, "taskboard_empty");
	HE::TaskBoard* b = TaskBoardPanel::board(ctx);
	REQUIRE(b != nullptr);
	CHECK(b->tasks.empty());
	// Opening the panel on a project without tasks adds no file to it.
	CHECK_FALSE(std::filesystem::exists(HE::taskBoardPath(proj.root())));

	// New Task is the first control under the title bar.
	ImGuiWindow* win = ImGui::FindWindowByName("Task Board");
	REQUIRE(win != nullptr);
	const ImGuiID newTask = win->GetID("New Task");
	const ImVec2 at(kBoardX + ImGui::GetStyle().WindowPadding.x + 20.0f,
	                kBoardY + ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y + ImGui::GetFrameHeight() * 0.5f);
	REQUIRE(idAt(ctx, at) == newTask);
	clickAt(ctx, at);

	REQUIRE(b->tasks.size() == 1);
	const HE::Task& t = b->tasks[0];
	CHECK(t.title == "New task");
	CHECK(t.createdBy == "Tester");
	CHECK(TaskBoardPanel::selectedTask() == t.id);
	// Saved on the spot, where git will see it.
	const std::string file = readFile(HE::taskBoardPath(proj.root()));
	CHECK(file.find("\"New task\"") != std::string::npos);

	TaskBoardPanel::Rect card;
	REQUIRE(TaskBoardPanel::cardRect(t.id, card));
	CHECK(card.x0 >= kBoardX);
	CHECK(card.x1 <= kBoardX + kBoardW);

	he_ui::Image img;
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 3; ++i) frame(ctx, false, i == 2 ? &img : nullptr);
	REQUIRE(img.valid());
	shotTo(img, "taskboard_new_task");
	// The card and the details below it: thousands of pixels the empty board
	// did not have.
	CHECK(diffIn(empty, img, int(kBoardX), int(kBoardY), int(kBoardX + kBoardW), int(kBoardY + kBoardH)) > 2000);

	// A second click on the selected card closes its details; a third opens them.
	REQUIRE(TaskBoardPanel::cardRect(t.id, card));
	clickAt(ctx, ImVec2(card.x0 + 40.0f, card.y0 + 10.0f));
	CHECK(TaskBoardPanel::selectedTask() == HE::UUID{});
	REQUIRE(TaskBoardPanel::cardRect(t.id, card));
	clickAt(ctx, ImVec2(card.x0 + 40.0f, card.y0 + 10.0f));
	CHECK(TaskBoardPanel::selectedTask() == t.id);
}

TEST_CASE("task board ui: drops link an asset, a scene and an entity; chips jump to them")
{
	Harness harness;
	Project proj("he_taskboard_ui_links");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	g_open = true;

	// A real asset in the project's Content folder, and the scene that is open.
	const std::filesystem::path content = std::filesystem::path(proj.root()) / "Content";
	ContentManager cm(content.string());
	{
		StructTypeAsset a;
		a.type = HE::AssetType::StructType;
		a.name = "Notes";
		a.path = "Notes.hasset";
		a.json = R"({"fields":[{"name":"hp","type":1}]})";
		REQUIRE(cm.saveAsset(a));
	}
	const HE::UUID assetId = cm.loadAsset("Notes.hasset");
	REQUIRE(assetId != HE::UUID{});
	ctx.contentManager = &cm;

	std::filesystem::create_directories(content / "Levels");
	const std::filesystem::path scene = content / "Levels" / "Main.hescene";
	{ std::ofstream(scene) << "{}\n"; }
	bits.scenePath = scene.string();

	HorizonWorld world;
	const Entity crate = world.createEntity("Crate");
	world.createEntity("Door");
	ctx.world = &world;

	HE::UUID taskId;
	REQUIRE(TaskBoardPanel::edit(ctx, [&](HE::TaskBoard& b) {
		HE::Task* t = b.add("Fix the crate's collision", "Tester", HE::taskNowMs());
		taskId = t->id;
		return true;
	}));
	TaskBoardPanel::selectTask(HE::UUID{});

	Sources src;
	src.assetPath = (content / "Notes.hasset").string();
	src.scenePath = scene.string();
	src.entity    = crate;
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 3; ++i) frame(ctx, false, nullptr, &src);

	TaskBoardPanel::Rect card;
	REQUIRE(TaskBoardPanel::cardRect(taskId, card));
	dragTo(ctx, src.assetAt, centre(card), &src);
	dragTo(ctx, src.sceneAt, centre(card), &src);
	REQUIRE(TaskBoardPanel::cardRect(taskId, card));   // the details opened below; the card stays
	dragTo(ctx, src.entityAt, centre(card), &src);

	HE::TaskBoard* b = TaskBoardPanel::board(ctx);
	REQUIRE(b != nullptr);
	const HE::Task* t = b->find(taskId);
	REQUIRE(t != nullptr);
	REQUIRE(t->links.size() == 3);
	CHECK(t->links[0].kind == HE::TaskLinkKind::Asset);
	CHECK(t->links[0].id == assetId);
	CHECK(t->links[0].label == "Notes");
	CHECK(t->links[1].kind == HE::TaskLinkKind::Scene);
	CHECK(t->links[1].scene == "Content/Levels/Main.hescene");
	CHECK(t->links[2].kind == HE::TaskLinkKind::Entity);
	CHECK(t->links[2].scene == "Content/Levels/Main.hescene");
	CHECK(t->links[2].label == "Crate");
	CHECK(HE::uuidToHex(t->links[2].id) == uuidOf(world, crate));
	// A drop selects the task it landed on.
	CHECK(TaskBoardPanel::selectedTask() == taskId);

	// What the file holds is what a teammate pulls.
	{
		HE::TaskBoard again;
		REQUIRE(HE::loadTaskBoard(proj.root(), again).ok);
		REQUIRE(again.tasks.size() == 1);
		CHECK(again.tasks[0].links.size() == 3);
	}

	he_ui::Image img;
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 3; ++i) frame(ctx, false, i == 2 ? &img : nullptr, &src);
	REQUIRE(img.valid());
	shotTo(img, "taskboard_links");

	// ── The chips on the card ──
	TaskBoardPanel::Rect chip;
	REQUIRE(TaskBoardPanel::chipRect(taskId, 2, chip));
	CHECK(chip.x1 <= card.x1);   // inside the card, not past its edge
	(void)TaskBoardPanel::takeJump();
	ctx.selection.clear();
	clickAt(ctx, centre(chip), &src);
	CHECK(ctx.selection.contains(crate));
	CHECK(TaskBoardPanel::takeJump().kind == TaskBoardPanel::Jump::Kind::Entity);
	CHECK(TaskBoardPanel::takeJump().kind == TaskBoardPanel::Jump::Kind::None);   // one-shot

	REQUIRE(TaskBoardPanel::chipRect(taskId, 0, chip));
	clickAt(ctx, centre(chip), &src);
	{
		const TaskBoardPanel::Jump j = TaskBoardPanel::takeJump();
		CHECK(j.kind == TaskBoardPanel::Jump::Kind::Asset);
		CHECK(std::filesystem::equivalent(j.target, content / "Notes.hasset"));
	}

	REQUIRE(TaskBoardPanel::chipRect(taskId, 1, chip));
	clickAt(ctx, centre(chip), &src);
	{
		const TaskBoardPanel::Jump j = TaskBoardPanel::takeJump();
		CHECK(j.kind == TaskBoardPanel::Jump::Kind::Scene);
		CHECK(std::filesystem::equivalent(j.target, scene));
	}

	// The same entity dropped twice is one link, not two.
	REQUIRE(TaskBoardPanel::cardRect(taskId, card));
	dragTo(ctx, src.entityAt, centre(card), &src);
	CHECK(TaskBoardPanel::board(ctx)->find(taskId)->links.size() == 3);

	// A deleted entity is marked, not dropped: the chip still says what it was.
	world.destroyEntity(crate);
	std::this_thread::sleep_for(std::chrono::milliseconds(1100));   // the orphan check runs once a second
	for (int i = 0; i < 3; ++i) frame(ctx, false, i == 2 ? &img : nullptr, &src);
	t = TaskBoardPanel::board(ctx)->find(taskId);
	REQUIRE(t != nullptr);
	REQUIRE(t->links.size() == 3);
	CHECK(t->links[2].orphaned);
	CHECK_FALSE(t->links[0].orphaned);
	CHECK_FALSE(t->links[1].orphaned);
	shotTo(img, "taskboard_orphan");
}

TEST_CASE("task board ui: a card dragged to another lane changes its status")
{
	Harness harness;
	Project proj("he_taskboard_ui_columns");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	g_open = true;

	HE::UUID a, c;
	REQUIRE(TaskBoardPanel::edit(ctx, [&](HE::TaskBoard& b) {
		a = b.add("Block out the harbour", "Tester", HE::taskNowMs())->id;
		c = b.add("Light the tavern", "Tester", HE::taskNowMs())->id;
		b.setStatus(c, HE::TaskStatus::InProgress, "Tester", HE::taskNowMs());
		b.setPriority(c, HE::TaskPriority::Urgent, HE::taskNowMs());
		return true;
	}));
	TaskBoardPanel::setView(TaskBoardPanel::View::Columns);
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 3; ++i) frame(ctx, false);

	TaskBoardPanel::Rect ra, rc;
	REQUIRE(TaskBoardPanel::cardRect(a, ra));
	REQUIRE(TaskBoardPanel::cardRect(c, rc));
	// Open is the first lane, In Progress the second.
	CHECK(rc.x0 > ra.x1);
	CHECK(ra.x1 - ra.x0 < kBoardW / 4.0f + 1.0f);

	// Into the Done lane, below its (empty) list of cards.
	const float laneW = (kBoardW - 16.0f) / 4.0f;
	const ImVec2 done(kBoardX + 8.0f + laneW * 3.5f, ra.y1 + 60.0f);
	dragTo(ctx, centre(ra), done);
	HE::TaskBoard* b = TaskBoardPanel::board(ctx);
	CHECK(b->find(a)->status == HE::TaskStatus::Done);
	CHECK(b->find(a)->updates.back().kind == HE::TaskUpdateKind::Status);
	CHECK(b->find(a)->updates.back().author == "Tester");
	CHECK(b->find(c)->status == HE::TaskStatus::InProgress);

	he_ui::Image img;
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 3; ++i) frame(ctx, false, i == 2 ? &img : nullptr);
	REQUIRE(img.valid());
	shotTo(img, "taskboard_columns");
	TaskBoardPanel::Rect moved;
	REQUIRE(TaskBoardPanel::cardRect(a, moved));
	CHECK(moved.x0 > kBoardX + laneW * 3.0f);

	// A lane is narrower than a card's texts: they are cut at the card's
	// border, not run on into the (empty) Blocked lane beside it.
	REQUIRE(TaskBoardPanel::cardRect(c, rc));
	CHECK(textPixelsIn(img, int(rc.x0) + 4, int(rc.y0) + 2, int(rc.x1) - 2, int(rc.y1) - 2) > 50);   // the probe sees text
	CHECK(textPixelsIn(img, int(rc.x1) + 6, int(rc.y0), int(rc.x1) + 60, int(rc.y1)) == 0);
}

TEST_CASE("task board ui: a long board draws only what is on screen, and no title runs past its card")
{
	Harness harness;
	Project proj("he_taskboard_ui_clip");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	g_open = true;

	const std::string longTitle = "A task whose title goes on and on well past the width of any card "
	                              "the panel could ever give it, so it has to be cut";
	REQUIRE(TaskBoardPanel::edit(ctx, [&](HE::TaskBoard& b) {
		for (int i = 0; i < 300; ++i) b.add(longTitle + " #" + std::to_string(i), "Tester", HE::taskNowMs());
		return true;
	}));
	TaskBoardPanel::setView(TaskBoardPanel::View::List);
	he_ui::Image img;
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 3; ++i) frame(ctx, false, i == 2 ? &img : nullptr);
	REQUIRE(img.valid());
	shotTo(img, "taskboard_long");

	const TaskBoardPanel::DrawStats st = TaskBoardPanel::lastDrawStats();
	INFO("drawn " << st.drawn << ", skipped " << st.skipped);
	CHECK(st.drawn > 3);
	CHECK(st.drawn < 20);
	CHECK(st.drawn + st.skipped == 300);
	// Nothing of the board lands right of its window.
	CHECK(inkIn(img, int(kBoardX + kBoardW) + 1, 0, W, H) == 0);
}

TEST_CASE("task board ui: a pull under the open board is read, and an edit lands on top of it")
{
	Harness harness;
	Project proj("he_taskboard_ui_pull");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	g_open = true;

	HE::UUID mine;
	REQUIRE(TaskBoardPanel::edit(ctx, [&](HE::TaskBoard& b) {
		mine = b.add("Mine", "Tester", HE::taskNowMs())->id;
		return true;
	}));

	// A teammate's task arrives with a pull.
	auto pullIn = [&](const char* title) {
		HE::TaskBoard theirs;
		REQUIRE(HE::loadTaskBoard(proj.root(), theirs).ok);
		theirs.add(title, "Cleo", HE::taskNowMs());
		REQUIRE(HE::saveTaskBoard(proj.root(), theirs));
	};
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	pullIn("Theirs");

	// Edited before the panel noticed: the save is refused, the pull read,
	// the edit put on top of it — nothing of either is lost.
	REQUIRE(TaskBoardPanel::edit(ctx, [&](HE::TaskBoard& b) {
		return b.setStatus(mine, HE::TaskStatus::Done, "Tester", HE::taskNowMs());
	}));
	CHECK(TaskBoardPanel::lastError().empty());
	{
		HE::TaskBoard disk;
		REQUIRE(HE::loadTaskBoard(proj.root(), disk).ok);
		REQUIRE(disk.tasks.size() == 2);
		CHECK(disk.find(mine)->status == HE::TaskStatus::Done);
		CHECK(disk.tasks[1].title == "Theirs");
	}

	// Left alone, the panel picks a pull up by itself.
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	pullIn("Theirs too");
	std::this_thread::sleep_for(std::chrono::milliseconds(600));
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 2; ++i) frame(ctx, false);
	REQUIRE(TaskBoardPanel::board(ctx)->tasks.size() == 3);
	CHECK(TaskBoardPanel::board(ctx)->tasks[2].title == "Theirs too");

	// A file nobody can read (a merge conflict) is never written over; the
	// edit waits in memory and the banner says why.
	{ std::ofstream(HE::taskBoardPath(proj.root()), std::ios::binary) << "<<<<<<< HEAD\n{\n"; }
	std::this_thread::sleep_for(std::chrono::milliseconds(600));
	for (int i = 0; i < 2; ++i) frame(ctx, false);
	CHECK_FALSE(TaskBoardPanel::lastError().empty());
	CHECK(TaskBoardPanel::board(ctx)->tasks.size() == 3);   // the last good list stays on screen
	CHECK_FALSE(TaskBoardPanel::edit(ctx, [&](HE::TaskBoard& b) {
		return b.setTitle(mine, "Renamed", HE::taskNowMs());
	}));
	CHECK(readFile(HE::taskBoardPath(proj.root())).rfind("<<<<<<< HEAD", 0) == 0);
	he_ui::Image img;
	for (int i = 0; i < 2; ++i) frame(ctx, false, i == 1 ? &img : nullptr);
	shotTo(img, "taskboard_conflict");
}
