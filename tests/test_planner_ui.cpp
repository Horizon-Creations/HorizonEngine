#include "doctest.h"
#include "ImGuiSoftwareRaster.h"
#include "TestFsUtil.h"

#include "EditorApplication.h"   // AppContext
#include "EditorCommands.h"      // uuidOf
#include "FakeTaskTeam.h"
#include "NotificationStore.h"
#include "TaskBoardTeam.h"
#include "EditorSelection.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include "Planner.h"
#include "PlannerImport.h"
#include "PlannerPanel.h"
#include "PlannerWorkspace.h"
#include "ProjectManager.h"
#include "TaskBoard.h"
#include "TaskBoardPanel.h"

#include <ContentManager/ContentManager.h>
#include <HorizonScene/HorizonWorld.h>

#include <imgui.h>

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// ── The Planner tab, driven headless ─────────────────────────────────────────
// The data and its files are Planner's (test_planner.cpp) and the workspace's
// (test_planner_workspace.cpp). What is checked here is the tab on top of them,
// over a real project on disk: that New Database and Import Task Board make
// databases and files, that a title typed into a cell is saved when the cell is
// left, that a header sorts, that a long table only lays out what is on screen,
// and that a card dragged to another lane changes its status. With
// HE_UI_DUMP_DIR set the frames are written out as well.

using namespace HE;
using namespace HE::Planner;

namespace
{
	constexpr int W = 1400, H = 620;

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
			TaskBoardPanel::reset();
			TaskBoardPanel::setAuthor("Tester");
			PlannerPanel::reset();
		}
		~Harness()
		{
			PlannerPanel::reset();
			TaskBoardPanel::reset();
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

	struct Project
	{
		std::filesystem::path dir;
		ProjectManager pm;
		explicit Project(const char* name)
		{
			dir = std::filesystem::temp_directory_path() / name;
			he_test::removeAllQuiet(dir);
			REQUIRE(pm.createNewProject(dir.string(), "Plan", ProjectPreset::Game));
		}
		~Project() { he_test::removeAllQuiet(dir); }
		std::filesystem::path root() const { return pm.projectRoot(); }
	};

	// The drag sources: what the Content Browser (an absolute path) and the World
	// Outliner (an entity handle) put in their payloads, as buttons in a strip at
	// the bottom left, under the sidebar's buttons and clear of the table and the details.
	struct Sources
	{
		std::string assetPath, scenePath;
		Entity      entity = entt::null;
		ImVec2      assetAt, sceneAt, entityAt;
	};

	void drawSources(Sources& s)
	{
		ImGui::SetNextWindowPos(ImVec2(20.0f, 400.0f));
		ImGui::SetNextWindowSize(ImVec2(210.0f, 160.0f));
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

	Sources* g_sources = nullptr;

	void frame(AppContext& ctx, bool mouseDown, he_ui::Image* shot = nullptr)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
		ImGui::NewFrame();
		TaskBoardPanel::tick(ctx);   // EditorUI does this every frame, whatever tab is open
		PlannerPanel::render(ctx, ImVec2(10, 10), ImVec2(float(W) - 20, float(H) - 20));
		if (g_sources) drawSources(*g_sources);
		EditorWidgets::drawQueuedHelp();
		ImGui::Render();
		if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
	}

	void settle(AppContext& ctx, int n = 3) { for (int i = 0; i < n; ++i) frame(ctx, false); }

	ImVec2 centre(const PlannerPanel::Rect& r) { return ImVec2((r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f); }

	// A click on a named control; false when it was not drawn.
	bool click(AppContext& ctx, const std::string& name)
	{
		const PlannerPanel::Rect r = PlannerPanel::controlRect(name);
		if (r.x1 <= r.x0) return false;
		const ImVec2 p = centre(r);
		ImGui::GetIO().AddMousePosEvent(p.x, p.y);
		frame(ctx, false);
		frame(ctx, false);
		frame(ctx, true);
		frame(ctx, false);
		frame(ctx, false);
		return true;
	}

	void dragTo(AppContext& ctx, ImVec2 from, ImVec2 to)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMousePosEvent(from.x, from.y);
		frame(ctx, false);
		frame(ctx, false);
		frame(ctx, true);
		for (int i = 1; i <= 8; ++i)
		{
			const float t = float(i) / 8.0f;
			io.AddMousePosEvent(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t);
			frame(ctx, true);
		}
		frame(ctx, true);
		frame(ctx, false);
		frame(ctx, false);
	}

	void shotTo(AppContext& ctx, const char* name)
	{
		const char* d = std::getenv("HE_UI_DUMP_DIR");
		if (!d || !*d) return;
		he_ui::Image img;
		frame(ctx, false, &img);
		he_ui::writeBmp(img, std::string(d) + "/" + name + ".bmp");
	}

	Database* tasksDb(AppContext& ctx)
	{
		Workspace* ws = PlannerPanel::workspace(ctx);
		return ws ? ws->find(tasksDatabaseId()) : nullptr;
	}

	void addTasks(AppContext& ctx, std::initializer_list<const char*> titles)
	{
		for (const char* t : titles)
			REQUIRE(TaskBoardPanel::edit(ctx, [&](TaskBoard& b) { b.add(t, "Tester", taskNowMs()); return true; }));
	}

	std::string readText(const std::filesystem::path& p)
	{
		std::ifstream f(p, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(f)), {});
	}

	void idle(AppContext& ctx, int n = 3)
	{
		ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
		settle(ctx, n);
	}
}

TEST_CASE("planner ui: no project, no planner; New Database makes a database and a file")
{
	Harness harness;
	Project proj("he_planner_ui_new");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);

	ContextBits closedBits;
	closedBits.projectLoaded = false;
	AppContext closed = closedBits.make(proj.pm);
	settle(closed);
	CHECK(PlannerPanel::workspace(closed) == nullptr);

	settle(ctx);
	Workspace* ws = PlannerPanel::workspace(ctx);
	REQUIRE(ws != nullptr);
	REQUIRE(ws->databases().size() == 1);   // Tasks is always there: it is the Task Board
	CHECK(ws->databases()[0]->id == tasksDatabaseId());
	shotTo(ctx, "planner_empty");

	REQUIRE(click(ctx, "New Database"));
	settle(ctx);
	ws = PlannerPanel::workspace(ctx);
	REQUIRE(ws->databases().size() == 2);
	const Database* made = nullptr;
	for (const Database* d : ws->databases())
		if (d->name == "New Database") made = d;
	REQUIRE(made != nullptr);
	CHECK(std::filesystem::exists(databasePath(proj.root(), made->id)));
	CHECK(PlannerPanel::selectedDatabase() == made->id);
	CHECK(PlannerPanel::controlRect("New Row").x1 > 0);   // the database is open
}

TEST_CASE("planner ui: Tasks is the Task Board; a new row is a task, and its title is saved when the cell is left")
{
	Harness harness;
	Project proj("he_planner_ui_tasks");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	addTasks(ctx, { "Retexture the crates", "Fix the bridge", "Odd one" });
	settle(ctx);

	CHECK(PlannerPanel::selectedDatabase() == tasksDatabaseId());
	CHECK(PlannerPanel::lastDrawStats().rows == 3);
	CHECK(PlannerPanel::controlRect("title:Fix the bridge").x1 > 0);
	shotTo(ctx, "planner_tasks_table");

	// New Row is a new task on the board, made by the user, and a file for it.
	REQUIRE(click(ctx, "New Row"));
	settle(ctx);
	TaskBoard* board = TaskBoardPanel::board(ctx);
	REQUIRE(board->tasks.size() == 4);
	CHECK(board->tasks[3].createdBy == "Tester");
	CHECK(board->tasks[3].status == TaskStatus::Open);
	const UUID fresh = board->tasks[3].id;
	CHECK(tasksDb(ctx)->findRow(fresh) != nullptr);   // the row has the task's id

	REQUIRE(click(ctx, "title:"));
	ImGuiIO& io = ImGui::GetIO();
	io.AddInputCharactersUTF8("Harbour level");
	settle(ctx);
	CHECK(TaskBoardPanel::board(ctx)->find(fresh)->title.empty());   // not saved while typing
	io.AddKeyEvent(ImGuiKey_Enter, true);
	frame(ctx, false);
	io.AddKeyEvent(ImGuiKey_Enter, false);
	settle(ctx);
	CHECK(TaskBoardPanel::board(ctx)->find(fresh)->title == "Harbour level");

	// ...in the file a teammate pulls, and nothing about the rows in the Planner's own.
	TaskBoard onDisk;
	REQUIRE(loadTaskBoard(proj.root(), onDisk).ok);
	REQUIRE(onDisk.find(fresh) != nullptr);
	CHECK(onDisk.find(fresh)->title == "Harbour level");
	CHECK_FALSE(std::filesystem::exists(databasePath(proj.root(), tasksDatabaseId())));
}

TEST_CASE("planner ui: a header sorts, twice reverses, a third time clears")
{
	Harness harness;
	Project proj("he_planner_ui_sort");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	addTasks(ctx, { "Charlie", "Alpha", "Bravo" });
	settle(ctx);

	auto order = [&] {
		std::vector<std::pair<float, std::string>> rows;
		for (const char* t : { "Charlie", "Alpha", "Bravo" })
			rows.push_back({ PlannerPanel::controlRect(std::string("title:") + t).y0, t });
		std::sort(rows.begin(), rows.end());
		std::string out;
		for (auto& r : rows) out += r.second + " ";
		return out;
	};
	const View* v = nullptr;
	auto sorts = [&]() -> const std::vector<Sort>& {
		v = tasksDb(ctx)->findView(PlannerPanel::selectedView());
		REQUIRE(v);
		return v->sorts;
	};
	// "All tasks" starts sorted by Status; sort by Title instead.
	REQUIRE(click(ctx, "header:Title"));
	settle(ctx);
	REQUIRE(sorts().size() == 1);
	CHECK(sorts()[0].property == UUID{});
	CHECK_FALSE(sorts()[0].descending);
	CHECK(order() == "Alpha Bravo Charlie ");
	REQUIRE(click(ctx, "header:Title"));
	settle(ctx);
	CHECK(sorts()[0].descending);
	CHECK(order() == "Charlie Bravo Alpha ");
	REQUIRE(click(ctx, "header:Title"));
	settle(ctx);
	CHECK(sorts().empty());
	CHECK(order() == "Charlie Alpha Bravo ");   // the board's order again

	// A view edit is the one thing the Tasks database saves: the views, no rows.
	const std::string file = readText(databasePath(proj.root(), tasksDatabaseId()));
	REQUIRE_FALSE(file.empty());
	CHECK(file.find("Charlie") == std::string::npos);
}

TEST_CASE("planner ui: a long table only lays out the rows on screen")
{
	Harness harness;
	Project proj("he_planner_ui_long");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	settle(ctx);
	REQUIRE(click(ctx, "New Database"));
	settle(ctx);
	Workspace* ws = PlannerPanel::workspace(ctx);
	const UUID dbId = PlannerPanel::selectedDatabase();
	REQUIRE(ws->edit(dbId, [](Database& db) {
		for (int i = 0; i < 3000; ++i) db.addRow("Row " + std::to_string(i), "Tester", 1000 + i);
		return true;
	}));
	settle(ctx);
	const PlannerPanel::DrawStats s = PlannerPanel::lastDrawStats();
	CHECK(s.rows == 3000);
	CHECK(s.rowsDrawn > 5);
	CHECK(s.rowsDrawn < 60);
	shotTo(ctx, "planner_long_table");
}

TEST_CASE("planner ui: a card dragged to another lane changes the task's status")
{
	Harness harness;
	Project proj("he_planner_ui_board");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	addTasks(ctx, { "Carry crates" });
	settle(ctx);
	const UUID id = TaskBoardPanel::board(ctx)->tasks[0].id;
	REQUIRE(click(ctx, "view:By status"));
	settle(ctx);

	settle(ctx);
	const PlannerPanel::Rect card = PlannerPanel::controlRect("card:Carry crates");
	const PlannerPanel::Rect lane = PlannerPanel::controlRect("lane:Done");
	REQUIRE(card.x1 > card.x0);
	REQUIRE(lane.x1 > lane.x0);
	shotTo(ctx, "planner_board_before");
	dragTo(ctx, centre(card), centre(lane));
	settle(ctx);
	const Task* t = TaskBoardPanel::board(ctx)->find(id);
	REQUIRE(t != nullptr);
	CHECK(t->status == TaskStatus::Done);
	// The change is a status change in the task's history, which the team bridge announces.
	REQUIRE_FALSE(t->updates.empty());
	CHECK(t->updates.back().kind == TaskUpdateKind::Status);
	CHECK(t->updates.back().author == "Tester");
	shotTo(ctx, "planner_board_after");
}

TEST_CASE("planner ui: drops link an asset, a scene and an entity; chips jump to them")
{
	Harness harness;
	Project proj("he_planner_ui_links");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);

	// A real asset in the project's Content folder, and the scene that is open.
	const std::filesystem::path content = proj.root() / "Content";
	ContentManager cm(content.string());
	{
		StructTypeAsset a;
		a.type = HE::AssetType::StructType;
		a.name = "Notes";
		a.path = "Notes.hasset";
		a.json = R"({"fields":[{"name":"hp","type":1}]})";
		REQUIRE(cm.saveAsset(a));
	}
	const UUID assetId = cm.loadAsset("Notes.hasset");
	REQUIRE(assetId != UUID{});
	ctx.contentManager = &cm;

	std::filesystem::create_directories(content / "Levels");
	const std::filesystem::path scene = content / "Levels" / "Main.hescene";
	{ std::ofstream(scene) << "{}\n"; }
	bits.scenePath = scene.string();

	HorizonWorld world;
	const Entity crate = world.createEntity("Crate");
	world.createEntity("Door");
	ctx.world = &world;

	UUID taskId;
	REQUIRE(TaskBoardPanel::edit(ctx, [&](TaskBoard& b) {
		taskId = b.add("Fix the crate", "Tester", taskNowMs())->id;
		return true;
	}));

	Sources src;
	src.assetPath = (content / "Notes.hasset").string();
	src.scenePath = scene.string();
	src.entity    = crate;
	g_sources     = &src;
	idle(ctx);

	// The Links cell is the target: three drops, three links, in the order they came.
	PlannerPanel::Rect cell = PlannerPanel::controlRect("links:Fix the crate");
	REQUIRE(cell.x1 > cell.x0);
	dragTo(ctx, src.assetAt, centre(cell));
	dragTo(ctx, src.sceneAt, centre(cell));
	cell = PlannerPanel::controlRect("links:Fix the crate");
	dragTo(ctx, src.entityAt, centre(cell));

	TaskBoard* b = TaskBoardPanel::board(ctx);
	REQUIRE(b != nullptr);
	const Task* t = b->find(taskId);
	REQUIRE(t != nullptr);
	REQUIRE(t->links.size() == 3);
	CHECK(t->links[0].kind == TaskLinkKind::Asset);
	CHECK(t->links[0].id == assetId);
	CHECK(t->links[0].label == "Notes");
	CHECK(t->links[1].kind == TaskLinkKind::Scene);
	CHECK(t->links[1].scene == "Content/Levels/Main.hescene");
	CHECK(t->links[2].kind == TaskLinkKind::Entity);
	CHECK(t->links[2].scene == "Content/Levels/Main.hescene");
	CHECK(t->links[2].label == "Crate");
	CHECK(uuidToHex(t->links[2].id) == HE::Ed::uuidOf(world, crate));
	{
		// What the file holds is what a teammate pulls.
		TaskBoard again;
		REQUIRE(loadTaskBoard(proj.root(), again).ok);
		REQUIRE(again.tasks.size() == 1);
		CHECK(again.tasks[0].links.size() == 3);
	}
	// The cell shows the first chip, inside the cell.
	idle(ctx);
	PlannerPanel::Rect first = PlannerPanel::controlRect("chip:Fix the crate:0");
	REQUIRE(first.x1 > first.x0);
	CHECK(first.x1 <= PlannerPanel::controlRect("links:Fix the crate").x1);
	// The chip in the cell takes the click, not the drop zone underneath it.
	(void)TaskBoardPanel::takeJump();
	REQUIRE(click(ctx, "chip:Fix the crate:0"));
	CHECK(TaskBoardPanel::takeJump().kind == TaskBoardPanel::Jump::Kind::Asset);

	// Open the row: its details show every chip, and a click goes there.
	REQUIRE(click(ctx, "open:Fix the crate"));
	idle(ctx);
	CHECK(PlannerPanel::selectedRow() == taskId);
	shotTo(ctx, "planner_task_details");
	PlannerPanel::Rect chip = PlannerPanel::controlRect("drawer-chip:2");
	REQUIRE(chip.x1 > chip.x0);
	(void)TaskBoardPanel::takeJump();
	ctx.selection.clear();
	REQUIRE(click(ctx, "drawer-chip:2"));
	CHECK(ctx.selection.contains(crate));
	CHECK(TaskBoardPanel::takeJump().kind == TaskBoardPanel::Jump::Kind::Entity);
	CHECK(TaskBoardPanel::takeJump().kind == TaskBoardPanel::Jump::Kind::None);   // one-shot

	REQUIRE(click(ctx, "drawer-chip:0"));
	{
		const TaskBoardPanel::Jump j = TaskBoardPanel::takeJump();
		CHECK(j.kind == TaskBoardPanel::Jump::Kind::Asset);
		CHECK(std::filesystem::equivalent(j.target, content / "Notes.hasset"));
	}
	REQUIRE(click(ctx, "drawer-chip:1"));
	{
		const TaskBoardPanel::Jump j = TaskBoardPanel::takeJump();
		CHECK(j.kind == TaskBoardPanel::Jump::Kind::Scene);
		CHECK(std::filesystem::equivalent(j.target, scene));
	}

	// The same entity dropped twice is one link, not two.
	const PlannerPanel::Rect drop = PlannerPanel::controlRect("drawer-drop");
	REQUIRE(drop.x1 > drop.x0);
	dragTo(ctx, src.entityAt, centre(drop));
	CHECK(TaskBoardPanel::board(ctx)->find(taskId)->links.size() == 3);

	// A link comes off with its x.
	idle(ctx);
	CHECK(TaskBoardPanel::board(ctx)->find(taskId)->links.size() == 3);

	// A deleted entity is marked, not dropped: the chip still says what it was.
	world.destroyEntity(crate);
	std::this_thread::sleep_for(std::chrono::milliseconds(1100));   // the orphan check runs once a second
	idle(ctx);
	t = TaskBoardPanel::board(ctx)->find(taskId);
	REQUIRE(t != nullptr);
	REQUIRE(t->links.size() == 3);
	CHECK(t->links[2].orphaned);
	CHECK_FALSE(t->links[0].orphaned);
	CHECK_FALSE(t->links[1].orphaned);
	shotTo(ctx, "planner_orphan");
	g_sources = nullptr;
}

TEST_CASE("planner ui: the details edit the description, add a comment, and the old panel link opens the tab")
{
	Harness harness;
	Project proj("he_planner_ui_details");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	addTasks(ctx, { "Paint the lighthouse" });
	const UUID id = TaskBoardPanel::board(ctx)->tasks[0].id;
	idle(ctx);

	PlannerPanel::openTask(id);
	idle(ctx);
	CHECK(PlannerPanel::selectedRow() == id);
	CHECK(TaskBoardPanel::selectedTask() == id);
	REQUIRE(PlannerPanel::controlRect("drawer:description").x1 > 0);

	// The description: typed in the details, saved when the field is left.
	REQUIRE(click(ctx, "drawer:description"));
	ImGui::GetIO().AddInputCharactersUTF8("White with a red stripe");
	settle(ctx);
	CHECK(TaskBoardPanel::board(ctx)->find(id)->description.empty());
	REQUIRE(click(ctx, "drawer:title"));   // leaving the field commits it
	settle(ctx);
	CHECK(TaskBoardPanel::board(ctx)->find(id)->description == "White with a red stripe");

	// A comment goes into the history under the user's name.
	REQUIRE(click(ctx, "comment-field"));
	ImGui::GetIO().AddInputCharactersUTF8("Needs two coats");
	settle(ctx);
	REQUIRE(click(ctx, "Add Comment"));
	idle(ctx);
	const Task* t = TaskBoardPanel::board(ctx)->find(id);
	REQUIRE_FALSE(t->updates.empty());
	CHECK(t->updates.back().kind == TaskUpdateKind::Comment);
	CHECK(t->updates.back().text == "Needs two coats");
	CHECK(t->updates.back().author == "Tester");
	shotTo(ctx, "planner_task_comment");

	// Close hides the details; the row stays.
	REQUIRE(click(ctx, "drawer:close"));
	idle(ctx);
	CHECK(PlannerPanel::selectedRow() == UUID{});
	CHECK(PlannerPanel::lastDrawStats().rows == 1);
}

TEST_CASE("planner ui: a pull under the open board is read, and an edit lands on top of it")
{
	Harness harness;
	Project proj("he_planner_ui_pull");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);

	UUID mine;
	REQUIRE(TaskBoardPanel::edit(ctx, [&](TaskBoard& b) {
		mine = b.add("Mine", "Tester", taskNowMs())->id;
		return true;
	}));
	idle(ctx);
	CHECK(PlannerPanel::lastDrawStats().rows == 1);

	// A teammate's task arrives with a pull.
	auto pullIn = [&](const char* title) {
		TaskBoard theirs;
		REQUIRE(loadTaskBoard(proj.root(), theirs).ok);
		theirs.add(title, "Cleo", taskNowMs());
		REQUIRE(saveTaskBoard(proj.root(), theirs));
	};
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	pullIn("Theirs");

	// Edited in the Planner before it noticed: the save is refused, the pull read,
	// the edit put on top of it — nothing of either is lost.
	Workspace* ws = PlannerPanel::workspace(ctx);
	const UUID done = tasksDb(ctx)->findProperty(derivedId("property", "Status"))->findOptionByName("Done")->id;
	REQUIRE(ws->edit(tasksDatabaseId(), [&](Database& db) {
		return db.setSelect(mine, derivedId("property", "Status"), done, taskNowMs());
	}));
	CHECK(TaskBoardPanel::lastError().empty());
	{
		TaskBoard disk;
		REQUIRE(loadTaskBoard(proj.root(), disk).ok);
		REQUIRE(disk.tasks.size() == 2);
		CHECK(disk.find(mine)->status == TaskStatus::Done);
		CHECK(disk.tasks[1].title == "Theirs");
	}
	idle(ctx);
	CHECK(PlannerPanel::lastDrawStats().rows == 2);

	// Left alone, the Planner picks a pull up by itself.
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	pullIn("Theirs too");
	std::this_thread::sleep_for(std::chrono::milliseconds(600));
	idle(ctx, 4);
	CHECK(PlannerPanel::lastDrawStats().rows == 3);
	CHECK(tasksDb(ctx)->findRow(TaskBoardPanel::board(ctx)->tasks[2].id)->title == "Theirs too");

	// A file nobody can read (a merge conflict) is never written over; the edit is
	// refused, the last good list stays on screen and the banner says why.
	{ std::ofstream(taskBoardPath(proj.root()), std::ios::binary) << "<<<<<<< HEAD\n{\n"; }
	std::this_thread::sleep_for(std::chrono::milliseconds(600));
	idle(ctx, 4);
	CHECK_FALSE(TaskBoardPanel::lastError().empty());
	CHECK(PlannerPanel::lastDrawStats().rows == 3);
	CHECK_FALSE(ws->edit(tasksDatabaseId(), [&](Database& db) { return db.setTitle(mine, "Renamed", taskNowMs()); }));
	CHECK(readText(taskBoardPath(proj.root())).rfind("<<<<<<< HEAD", 0) == 0);
	CHECK(tasksDb(ctx)->findRow(mine)->title == "Mine");
	shotTo(ctx, "planner_conflict");
}

TEST_CASE("planner ui: Assignees only with a signed-in team; a pick assigns on disk and reaches the assignee")
{
	Harness harness;
	Project proj("he_planner_ui_team");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);

	// Two editors of one team: Anna's is this tab, Bob's only its bridge.
	he_test::FakeTaskTeam server;
	for (const char* m : { "Anna", "Bob" }) server.addMember(m);
	auto& annaLink = server.editor("Anna");
	auto& bobLink  = server.editor("Bob");
	HE::TaskBoardTeam annaTeam, bobTeam;
	annaTeam.setLink(&annaLink);
	bobTeam.setLink(&bobLink);
	HE::Ed::NotificationStore bobBell;
	bobTeam.setNotifications(&bobBell);
	// One clock for every pump: the editor's and the server's.
	TaskBoardPanel::setTeamClock([&] { return server.nowMs; });

	UUID id;
	REQUIRE(TaskBoardPanel::edit(ctx, [&](TaskBoard& b) {
		id = b.add("Retexture the crates", TaskBoardPanel::author(), taskNowMs())->id;
		return true;
	}));
	const std::string assignees = "assignees:Retexture the crates";
	auto frames = [&](int n) { idle(ctx, n); };

	// No team server: no picker, and the board is the local one it always was.
	frames(3);
	CHECK(PlannerPanel::controlRect(assignees).x1 <= 0);
	CHECK(PlannerPanel::controlRect("From the Team").x1 <= 0);
	CHECK(TaskBoardPanel::author() == "Tester");

	// A team whose link is signed out: still none.
	annaLink.signedIn = false;
	TaskBoardPanel::setTeam(&annaTeam);
	frames(3);
	CHECK(PlannerPanel::controlRect(assignees).x1 <= 0);
	CHECK(TaskBoardPanel::author() == "Tester");

	// Signed in: the picker is there, and edits are made as the member.
	annaLink.signedIn = true;
	frames(3);
	REQUIRE(PlannerPanel::controlRect(assignees).x1 > 0);
	CHECK(TaskBoardPanel::author() == "Anna");
	shotTo(ctx, "planner_team_assignees");

	// Open the list and tick Bob.
	REQUIRE(click(ctx, assignees));
	REQUIRE(PlannerPanel::controlRect("member:Bob").x1 > 0);
	shotTo(ctx, "planner_team_assignee_list");
	REQUIRE(click(ctx, "member:Bob"));
	{
		TaskBoard disk;
		REQUIRE(loadTaskBoard(proj.root(), disk).ok);
		REQUIRE(disk.find(id) != nullptr);
		CHECK(disk.find(id)->assignees == std::vector<std::string>{ "Bob" });
		CHECK(disk.find(id)->assignedBy == "Anna");
		REQUIRE_FALSE(disk.find(id)->updates.empty());
		CHECK(disk.find(id)->updates.back().author == "Anna");
	}
	CHECK(annaTeam.pending() == 1);
	// Click away: the list closes.
	click(ctx, "New Database");   // anywhere that is not the list
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);

	// It settles, the editor's own pump sends it, the server carries it.
	server.pump(HE::TaskBoardTeam::kAssignSettleMs, { &bobTeam });
	frames(1);
	CHECK(annaLink.sent.size() == 1);
	server.pump(server.latencyMs, { &bobTeam });
	REQUIRE(bobBell.snapshot().size() == 1);
	CHECK(bobBell.snapshot()[0].text == "Anna assigned you \xE2\x80\x9CRetexture the crates\xE2\x80\x9D");

	// Bob's side: From the Team appears, and its row opens the task.
	TaskBoardPanel::setTeam(&bobTeam);
	TaskBoardPanel::selectTask(UUID{});
	frames(3);
	CHECK(TaskBoardPanel::author() == "Bob");
	REQUIRE(PlannerPanel::controlRect("From the Team").x1 > 0);
	REQUIRE(click(ctx, "From the Team"));
	REQUIRE(PlannerPanel::controlRect("team-row:0").x1 > 0);
	shotTo(ctx, "planner_team_from_the_team");
	REQUIRE(click(ctx, "team-row:0"));
	CHECK(TaskBoardPanel::selectedTask() == id);
	CHECK(PlannerPanel::selectedRow() == id);

	// Signing out takes the member name back off the edits.
	bobLink.signedIn = false;
	frames(2);
	CHECK(TaskBoardPanel::author() != "Bob");
	CHECK(PlannerPanel::controlRect(assignees).x1 <= 0);
}

TEST_CASE("planner ui: without a team, assignees are typed names kept on the board")
{
	Harness harness;
	Project proj("he_planner_ui_names");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	addTasks(ctx, { "Paint the lighthouse" });
	const UUID id = TaskBoardPanel::board(ctx)->tasks[0].id;
	idle(ctx);

	REQUIRE(PlannerPanel::controlRect("cell:Assignees").x1 > 0);
	REQUIRE(click(ctx, "cell:Assignees"));
	ImGui::GetIO().AddInputCharactersUTF8("Anna, Bob");
	settle(ctx);
	CHECK(TaskBoardPanel::board(ctx)->find(id)->assignees.empty());   // not saved while typing
	ImGuiIO& io = ImGui::GetIO();
	io.AddKeyEvent(ImGuiKey_Enter, true);
	frame(ctx, false);
	io.AddKeyEvent(ImGuiKey_Enter, false);
	idle(ctx);
	const Task* t = TaskBoardPanel::board(ctx)->find(id);
	CHECK(t->assignees == std::vector<std::string>{ "Anna", "Bob" });
	CHECK(t->assignedBy == "Tester");
	REQUIRE_FALSE(t->updates.empty());
	CHECK(t->updates.back().kind == TaskUpdateKind::Assignment);
}

TEST_CASE("planner ui: a relation cell lists the rows it can point at and ticks them")
{
	Harness harness;
	Project proj("he_planner_ui_relation");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	settle(ctx);
	REQUIRE(click(ctx, "New Database"));
	settle(ctx);
	Workspace* ws = PlannerPanel::workspace(ctx);
	const UUID dbId = PlannerPanel::selectedDatabase();
	UUID blockedBy, alpha, beta;
	REQUIRE(ws->edit(dbId, [&](Database& db) {
		Property* p = db.addProperty("Blocked by", PropertyType::Relation);
		p->relationDb = db.id;
		blockedBy = p->id;
		alpha = db.addRow("Alpha", "Tester", 1000)->id;
		beta  = db.addRow("Beta", "Tester", 1001)->id;
		return true;
	}));
	idle(ctx);

	REQUIRE(click(ctx, "relation:Alpha"));
	REQUIRE(PlannerPanel::controlRect("relrow:Beta").x1 > 0);
	CHECK(PlannerPanel::controlRect("relrow:Alpha").x1 <= 0);   // a row does not point at itself
	REQUIRE(click(ctx, "relrow:Beta"));
	CHECK(ws->find(dbId)->relation(alpha, blockedBy) == std::vector<UUID>{ beta });
	shotTo(ctx, "planner_relation");
	REQUIRE(click(ctx, "relrow:Beta"));   // the list stays open; a second click unticks
	CHECK(ws->find(dbId)->relation(alpha, blockedBy).empty());
}

TEST_CASE("planner ui: a timeline draws a bar per dated row, and a dragged bar moves its dates")
{
	Harness harness;
	Project proj("he_planner_ui_timeline");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	settle(ctx);
	REQUIRE(click(ctx, "New Database"));
	settle(ctx);
	Workspace* ws = PlannerPanel::workspace(ctx);
	const UUID dbId = PlannerPanel::selectedDatabase();
	auto day = [](const char* iso) {
		std::int64_t ms = 0;
		REQUIRE(taskTimeFromIso(iso, ms));
		return ms;
	};
	UUID start, end, a, b, c;
	REQUIRE(ws->edit(dbId, [&](Database& db) {
		start = db.addProperty("Start", PropertyType::Date)->id;
		end   = db.addProperty("End", PropertyType::Date)->id;
		a = db.addRow("Alpha", "Tester", 1000)->id;
		b = db.addRow("Beta", "Tester", 1001)->id;
		c = db.addRow("Gamma", "Tester", 1002)->id;
		db.setDate(a, start, day("2026-10-20T00:00:00.000Z"), 1);
		db.setDate(a, end,   day("2026-10-24T00:00:00.000Z"), 1);
		db.setDate(b, start, day("2026-10-26T00:00:00.000Z"), 1);
		return true;
	}));
	idle(ctx);

	REQUIRE(click(ctx, "+ Timeline"));
	idle(ctx);
	const View* v = ws->find(dbId)->findView(PlannerPanel::selectedView());
	REQUIRE(v != nullptr);
	CHECK(v->kind == ViewKind::Timeline);
	CHECK(v->dateProperty == start);   // the first two Date properties
	CHECK(v->endProperty == end);
	shotTo(ctx, "planner_timeline");

	// Alpha runs five days, Beta one; Gamma has no start date and no bar.
	const PlannerPanel::Rect ra = PlannerPanel::controlRect("bar:Alpha");
	const PlannerPanel::Rect rb = PlannerPanel::controlRect("bar:Beta");
	REQUIRE(ra.x1 > ra.x0);
	REQUIRE(rb.x1 > rb.x0);
	CHECK(ra.x1 - ra.x0 == doctest::Approx(5 * 24.0f));
	CHECK(rb.x1 - rb.x0 == doctest::Approx(24.0f));
	CHECK(rb.x0 - ra.x0 == doctest::Approx(6 * 24.0f));
	CHECK(PlannerPanel::controlRect("bar:Gamma").x1 <= 0);
	CHECK(PlannerPanel::lastDrawStats().rows == 3);
	CHECK(PlannerPanel::lastDrawStats().rowsDrawn == 3);

	// Drag Beta three days to the right: its start moves, and the row opens.
	dragTo(ctx, centre(rb), ImVec2(centre(rb).x + 72.0f, centre(rb).y));
	idle(ctx);
	std::int64_t ms = 0;
	REQUIRE(ws->find(dbId)->date(b, start, ms));
	CHECK(ms == day("2026-10-29T00:00:00.000Z"));
	CHECK(PlannerPanel::selectedRow() == UUID{});   // a drag is not a click: the row does not open
	REQUIRE(click(ctx, "bar:Beta"));
	CHECK(PlannerPanel::selectedRow() == b);
	// A click that does not move writes nothing.
	const std::string before = ws->find(dbId)->toJson();
	REQUIRE(click(ctx, "bar:Alpha"));
	CHECK(ws->find(dbId)->toJson() == before);

	// The right edge of Alpha: two days further, and the End date follows. A drag of the
	// edge is not a click and not a move.
	{
		const PlannerPanel::Rect edge = PlannerPanel::controlRect("bar-end:Alpha");
		REQUIRE(edge.x1 > edge.x0);
		dragTo(ctx, centre(edge), ImVec2(centre(edge).x + 48.0f, centre(edge).y));
		idle(ctx);
		REQUIRE(ws->find(dbId)->date(a, end, ms));
		CHECK(ms == day("2026-10-26T00:00:00.000Z"));
		REQUIRE(ws->find(dbId)->date(a, start, ms));
		CHECK(ms == day("2026-10-20T00:00:00.000Z"));   // the Start stayed
	}
	// Weeks: a day is half as wide, seven days of Alpha are 84 pixels.
	PlannerPanel::setTimelineZoom(1);
	idle(ctx);
	{
		const PlannerPanel::Rect ra2 = PlannerPanel::controlRect("bar:Alpha");
		CHECK(ra2.x1 - ra2.x0 == doctest::Approx(7 * 12.0f));
	}
	PlannerPanel::setTimelineZoom(0);
	idle(ctx);

	// Without a Start date there is nothing to draw, and the view says so.
	CHECK(ws->edit(dbId, [&](Database& db) { db.findView(PlannerPanel::selectedView())->dateProperty = UUID{}; return true; }));
	idle(ctx);
	CHECK(PlannerPanel::controlRect("bar:Alpha").x1 <= 0);
}

namespace
{
	void pressEnter(AppContext& ctx)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddKeyEvent(ImGuiKey_Enter, true);
		frame(ctx, false);
		io.AddKeyEvent(ImGuiKey_Enter, false);
		settle(ctx);
	}

	void typeText(AppContext& ctx, const char* text)
	{
		ImGui::GetIO().AddInputCharactersUTF8(text);
		settle(ctx);
	}

	const Page* currentPage(AppContext& ctx)
	{
		Workspace* ws = PlannerPanel::workspace(ctx);
		return ws ? ws->findPage(PlannerPanel::selectedPage()) : nullptr;
	}
}

TEST_CASE("planner ui pages: New Page, a title, blocks written like a document")
{
	Harness harness;
	Project proj("he_planner_ui_page");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	idle(ctx);

	REQUIRE(click(ctx, "New Page"));
	idle(ctx);
	REQUIRE(currentPage(ctx) != nullptr);
	const UUID pageId = currentPage(ctx)->id;
	CHECK(std::filesystem::exists(pagePath(proj.root(), pageId)));
	CHECK(currentPage(ctx)->parent == UUID{});

	// The title is saved when the field is left.
	REQUIRE(click(ctx, "page:title"));
	// "Untitled" goes (the click lands past its end, so Backspace eats from the end).
	for (int i = 0; i < 8; ++i)
	{
		ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, true);
		frame(ctx, false);
		ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, false);
		frame(ctx, false);
	}
	typeText(ctx, "Game design");
	pressEnter(ctx);
	CHECK(currentPage(ctx)->title == "Game design");
	CHECK(PlannerPanel::controlRect("page:Game design").x1 > 0);   // the sidebar says so too

	// Add Block puts the cursor in a new block; Enter makes the next one.
	REQUIRE(click(ctx, "add-block"));
	settle(ctx);
	REQUIRE(currentPage(ctx)->blocks.size() == 1);
	typeText(ctx, "Pillars");
	pressEnter(ctx);
	REQUIRE(currentPage(ctx)->blocks.size() == 2);
	CHECK(currentPage(ctx)->blocks[0].text == "Pillars");
	CHECK(currentPage(ctx)->blocks[0].type == BlockType::Paragraph);

	// Markdown shortcuts: "# " makes a heading, "[] " a to-do that goes on as a to-do.
	typeText(ctx, "# Overview");
	pressEnter(ctx);
	REQUIRE(currentPage(ctx)->blocks.size() == 3);
	CHECK(currentPage(ctx)->blocks[1].type == BlockType::Heading1);
	CHECK(currentPage(ctx)->blocks[1].text == "Overview");
	typeText(ctx, "[] Ship alpha");
	pressEnter(ctx);
	REQUIRE(currentPage(ctx)->blocks.size() == 4);
	CHECK(currentPage(ctx)->blocks[2].type == BlockType::Todo);
	CHECK(currentPage(ctx)->blocks[2].text == "Ship alpha");
	CHECK(currentPage(ctx)->blocks[3].type == BlockType::Todo);   // the list goes on
	pressEnter(ctx);   // Enter on an empty item ends the list
	REQUIRE(currentPage(ctx)->blocks.size() == 4);
	CHECK(currentPage(ctx)->blocks[3].type == BlockType::Paragraph);
	shotTo(ctx, "planner_page");

	// The to-do's box ticks it.
	REQUIRE(PlannerPanel::controlRect("check:2").x1 > 0);
	REQUIRE(click(ctx, "check:2"));
	CHECK(currentPage(ctx)->blocks[2].checked);

	// Backspace in an empty block takes it away.
	REQUIRE(click(ctx, "block:3"));
	typeText(ctx, "x");
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, true);
	frame(ctx, false);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, false);
	settle(ctx);
	CHECK(currentPage(ctx)->blocks.size() == 4);   // "x" went; the block is empty again
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, true);
	frame(ctx, false);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, false);
	settle(ctx);
	CHECK(currentPage(ctx)->blocks.size() == 3);

	// Everything is in the file a teammate pulls.
	Page onDisk;
	REQUIRE(loadPage(proj.root(), pageId, onDisk).ok);
	CHECK(onDisk.title == "Game design");
	REQUIRE(onDisk.blocks.size() == 3);
	CHECK(onDisk.blocks[1].text == "Overview");
	CHECK(onDisk.blocks[2].checked);
}

TEST_CASE("planner ui pages: a block's menu moves and deletes it")
{
	Harness harness;
	Project proj("he_planner_ui_blockmenu");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	idle(ctx);
	REQUIRE(click(ctx, "New Page"));
	idle(ctx);
	Workspace* ws = PlannerPanel::workspace(ctx);
	const UUID pageId = PlannerPanel::selectedPage();
	REQUIRE(ws->editPage(pageId, [](Page& p) {
		p.addBlock(BlockType::Paragraph, "first", 1);
		p.addBlock(BlockType::Paragraph, "second", 2);
		p.addBlock(BlockType::Paragraph, "third", 3);
		return true;
	}));
	idle(ctx);

	REQUIRE(click(ctx, "block-menu:0"));
	REQUIRE(click(ctx, "menu:Move Down"));
	CHECK(ws->findPage(pageId)->blocks[0].text == "second");
	CHECK(ws->findPage(pageId)->blocks[1].text == "first");

	REQUIRE(click(ctx, "block-menu:1"));
	REQUIRE(click(ctx, "menu:Indent"));   // "first" under "second"
	REQUIRE(ws->findPage(pageId)->blocks.size() == 2);
	CHECK(ws->findPage(pageId)->blocks[0].children.size() == 1);
	idle(ctx);
	shotTo(ctx, "planner_page_indent");

	REQUIRE(click(ctx, "block-menu:1"));
	REQUIRE(click(ctx, "menu:Outdent"));
	CHECK(ws->findPage(pageId)->blocks.size() == 3);

	REQUIRE(click(ctx, "block-menu:2"));
	REQUIRE(click(ctx, "menu:Delete Block"));
	CHECK(ws->findPage(pageId)->blocks.size() == 2);
	CHECK(ws->findPage(pageId)->blocks[1].text == "first");
}

TEST_CASE("planner ui pages: a row opens as a page that is saved only once it has a block")
{
	Harness harness;
	Project proj("he_planner_ui_rowpage");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	addTasks(ctx, { "Paint the lighthouse" });
	const UUID id = TaskBoardPanel::board(ctx)->tasks[0].id;
	idle(ctx);

	REQUIRE(click(ctx, "open:Paint the lighthouse"));
	idle(ctx);
	REQUIRE(click(ctx, "Open Page"));
	idle(ctx);
	CHECK(PlannerPanel::selectedPage() == id);
	REQUIRE(currentPage(ctx) != nullptr);
	CHECK(currentPage(ctx)->parent == tasksDatabaseId());
	CHECK_FALSE(std::filesystem::exists(pagePath(proj.root(), id)));   // opening adds no file
	shotTo(ctx, "planner_row_page");

	REQUIRE(click(ctx, "add-block"));
	settle(ctx);
	typeText(ctx, "White with a red stripe");
	pressEnter(ctx);
	CHECK(std::filesystem::exists(pagePath(proj.root(), id)));

	// A new session sees the block; the page of a deleted task goes with it.
	{
		Workspace again;
		again.setProject(proj.root());
		REQUIRE(again.findPage(id));
		CHECK(again.findPage(id)->blocks[0].text == "White with a red stripe");
	}
	REQUIRE(click(ctx, "page:back"));
	idle(ctx);
	CHECK(PlannerPanel::selectedPage() == UUID{});
	CHECK(PlannerPanel::selectedDatabase() == tasksDatabaseId());
	CHECK(PlannerPanel::lastDrawStats().rows == 1);
}

TEST_CASE("planner ui pages: a drop adds an engine object, and a database view block opens rows as pages")
{
	Harness harness;
	Project proj("he_planner_ui_pageblocks");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);

	const std::filesystem::path content = proj.root() / "Content";
	ContentManager cm(content.string());
	{
		StructTypeAsset a;
		a.type = HE::AssetType::StructType;
		a.name = "Notes";
		a.path = "Notes.hasset";
		a.json = R"({"fields":[{"name":"hp","type":1}]})";
		REQUIRE(cm.saveAsset(a));
	}
	const UUID assetId = cm.loadAsset("Notes.hasset");
	ctx.contentManager = &cm;
	addTasks(ctx, { "Paint the lighthouse" });
	const UUID taskId = TaskBoardPanel::board(ctx)->tasks[0].id;

	Sources src;
	src.assetPath = (content / "Notes.hasset").string();
	g_sources = &src;
	idle(ctx);
	REQUIRE(click(ctx, "New Page"));
	idle(ctx);

	// Dropped on the page: a new block with the asset, whose chip jumps to it.
	const PlannerPanel::Rect drop = PlannerPanel::controlRect("page-drop");
	REQUIRE(drop.x1 > drop.x0);
	dragTo(ctx, src.assetAt, centre(drop));
	REQUIRE(currentPage(ctx)->blocks.size() == 1);
	CHECK(currentPage(ctx)->blocks[0].type == BlockType::EngineLink);
	CHECK(currentPage(ctx)->blocks[0].link.id == assetId);
	idle(ctx);
	(void)TaskBoardPanel::takeJump();
	REQUIRE(click(ctx, "link-chip:0"));
	CHECK(TaskBoardPanel::takeJump().kind == TaskBoardPanel::Jump::Kind::Asset);

	// A database view block lists the rows; a row opens as a page.
	Workspace* ws = PlannerPanel::workspace(ctx);
	const UUID pageId = PlannerPanel::selectedPage();
	REQUIRE(ws->editPage(pageId, [](Page& p) {
		Block* b = p.addBlock(BlockType::DatabaseView, "", 5);
		b->database = tasksDatabaseId();
		return true;
	}));
	idle(ctx);
	REQUIRE(click(ctx, "embedded-row:Paint the lighthouse"));
	idle(ctx);
	CHECK(PlannerPanel::selectedPage() == taskId);
	g_sources = nullptr;
}

TEST_CASE("planner ui: a calendar shows rows on their days, a drag moves one, a double-click adds one")
{
	Harness harness;
	Project proj("he_planner_ui_calendar");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	settle(ctx);
	REQUIRE(click(ctx, "New Database"));
	settle(ctx);
	Workspace* ws = PlannerPanel::workspace(ctx);
	const UUID dbId = PlannerPanel::selectedDatabase();
	auto day = [](const char* iso) {
		std::int64_t ms = 0;
		REQUIRE(taskTimeFromIso(iso, ms));
		return ms;
	};
	UUID when, end, alpha, beta;
	REQUIRE(ws->edit(dbId, [&](Database& db) {
		when = db.addProperty("When", PropertyType::Date)->id;
		end  = db.addProperty("Until", PropertyType::Date)->id;
		alpha = db.addRow("Alpha", "Tester", 1000)->id;
		beta  = db.addRow("Beta", "Tester", 1001)->id;
		db.addRow("Gamma", "Tester", 1002);   // no date: not on the calendar
		db.setDate(alpha, when, day("2026-10-20T00:00:00.000Z"), 1);
		db.setDate(beta, when,  day("2026-10-20T00:00:00.000Z"), 1);
		db.setDate(beta, end,   day("2026-10-22T00:00:00.000Z"), 1);
		return true;
	}));
	idle(ctx);
	REQUIRE(click(ctx, "+ Calendar"));
	REQUIRE(ws->edit(dbId, [&](Database& db) { db.findView(PlannerPanel::selectedView())->endProperty = end; return true; }));
	PlannerPanel::showCalendarMonth(2026, 10);
	idle(ctx);
	const View* v = ws->find(dbId)->findView(PlannerPanel::selectedView());
	REQUIRE(v != nullptr);
	CHECK(v->kind == ViewKind::Calendar);
	CHECK(v->dateProperty == when);
	shotTo(ctx, "planner_calendar");

	auto inside = [](const PlannerPanel::Rect& inner, const PlannerPanel::Rect& outer) {
		return inner.x0 >= outer.x0 && inner.x1 <= outer.x1 && inner.y0 >= outer.y0 && inner.y1 <= outer.y1;
	};
	const PlannerPanel::Rect d20 = PlannerPanel::controlRect("day:2026-10-20");
	const PlannerPanel::Rect d22 = PlannerPanel::controlRect("day:2026-10-22");
	REQUIRE(d20.x1 > d20.x0);
	REQUIRE(d22.x1 > d22.x0);
	const PlannerPanel::Rect chipA = PlannerPanel::controlRect("cal-chip:Alpha");
	const PlannerPanel::Rect chipB = PlannerPanel::controlRect("cal-chip:Beta");
	CHECK(inside(chipA, d20));
	CHECK(inside(chipB, d22));   // Beta runs 20-22: its last chip is on the 22nd
	CHECK(PlannerPanel::controlRect("cal-chip:Gamma").x1 <= 0);

	// Navigation.
	CHECK(PlannerPanel::calendarMonth() == 10);
	REQUIRE(click(ctx, "cal:next"));
	CHECK(PlannerPanel::calendarMonth() == 11);
	REQUIRE(click(ctx, "cal:previous"));
	REQUIRE(click(ctx, "cal:previous"));
	CHECK(PlannerPanel::calendarMonth() == 9);
	PlannerPanel::showCalendarMonth(2026, 10);
	idle(ctx);

	// Drag Alpha to the 27th: its date moves.
	const PlannerPanel::Rect d27 = PlannerPanel::controlRect("day:2026-10-27");
	REQUIRE(d27.x1 > d27.x0);
	dragTo(ctx, centre(PlannerPanel::controlRect("cal-chip:Alpha")), centre(d27));
	idle(ctx);
	std::int64_t ms = 0;
	REQUIRE(ws->find(dbId)->date(alpha, when, ms));
	CHECK(ms == day("2026-10-27T00:00:00.000Z"));

	// A double-click on the 5th adds a row dated that day.
	const ImVec2 p = centre(PlannerPanel::controlRect("day:2026-10-05"));
	ImGui::GetIO().AddMousePosEvent(p.x, p.y);
	frame(ctx, false);
	frame(ctx, false);
	frame(ctx, true);
	frame(ctx, false);
	frame(ctx, true);
	frame(ctx, false);
	idle(ctx);
	const Database* db = ws->find(dbId);
	REQUIRE(db->rows.size() == 4);
	REQUIRE(db->date(db->rows[3].id, when, ms));
	CHECK(ms == day("2026-10-05T00:00:00.000Z"));
}

TEST_CASE("planner ui pages: a block dragged by its handle lands before or after another")
{
	Harness harness;
	Project proj("he_planner_ui_blockdrag");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	idle(ctx);
	REQUIRE(click(ctx, "New Page"));
	idle(ctx);
	Workspace* ws = PlannerPanel::workspace(ctx);
	const UUID pageId = PlannerPanel::selectedPage();
	REQUIRE(ws->editPage(pageId, [](Page& p) {
		p.addBlock(BlockType::Paragraph, "a", 1);
		p.addBlock(BlockType::Paragraph, "b", 2);
		p.addBlock(BlockType::Paragraph, "c", 3);
		return true;
	}));
	idle(ctx);
	auto order = [&] {
		std::string out;
		for (const Block& b : ws->findPage(pageId)->blocks) out += b.text;
		return out;
	};

	// The lower half of the last block: "a" goes after "c".
	const PlannerPanel::Rect c = PlannerPanel::controlRect("block:2");
	REQUIRE(c.x1 > c.x0);
	dragTo(ctx, centre(PlannerPanel::controlRect("block-menu:0")), ImVec2(centre(c).x, c.y1 - 1.0f));
	idle(ctx);
	CHECK(order() == "bca");

	// The upper half of the first: "a" goes before "b".
	const PlannerPanel::Rect b = PlannerPanel::controlRect("block:0");
	dragTo(ctx, centre(PlannerPanel::controlRect("block-menu:2")), ImVec2(centre(b).x, b.y0 + 1.0f));
	idle(ctx);
	CHECK(order() == "abc");
	CHECK_FALSE(ImGui::GetDragDropPayload());

	// A press and release on the handle without a drag is still the menu.
	REQUIRE(click(ctx, "block-menu:1"));
	CHECK(PlannerPanel::controlRect("menu:Move Up").x1 > 0);
}

TEST_CASE("planner ui pages: a sub-page is linked from its parent, and the way back is the parent")
{
	Harness harness;
	Project proj("he_planner_ui_subpage");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	idle(ctx);
	REQUIRE(click(ctx, "New Page"));
	idle(ctx);
	Workspace* ws = PlannerPanel::workspace(ctx);
	const UUID parentId = PlannerPanel::selectedPage();
	REQUIRE(ws->editPage(parentId, [](Page& p) { p.title = "Design"; return true; }));
	idle(ctx);

	REQUIRE(click(ctx, "new-subpage"));
	idle(ctx);
	REQUIRE(ws->pages().size() == 2);
	const Page* parent = ws->findPage(parentId);
	REQUIRE(parent->blocks.size() == 1);
	CHECK(parent->blocks[0].type == BlockType::PageLink);
	const UUID childId = parent->blocks[0].page;
	REQUIRE(ws->findPage(childId) != nullptr);
	CHECK(ws->findPage(childId)->parent == parentId);
	CHECK(std::filesystem::exists(pagePath(proj.root(), childId)));
	shotTo(ctx, "planner_subpage_parent");

	// The link opens the sub-page; Back returns to the parent.
	REQUIRE(click(ctx, "page-link:0"));
	idle(ctx);
	CHECK(PlannerPanel::selectedPage() == childId);
	REQUIRE(PlannerPanel::controlRect("page:parent").x1 > 0);
	shotTo(ctx, "planner_subpage");
	REQUIRE(click(ctx, "page:parent"));
	idle(ctx);
	CHECK(PlannerPanel::selectedPage() == parentId);

	// Deleting the parent deletes the sub-page too (the workspace keeps that rule).
	CHECK(ws->removePage(parentId));
	CHECK(ws->findPage(childId) == nullptr);
}

TEST_CASE("planner ui pages: a page link block offers the pages, then shows the one chosen")
{
	Harness harness;
	Project proj("he_planner_ui_pagelink");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	idle(ctx);
	Workspace* ws = PlannerPanel::workspace(ctx);
	const UUID notes = ws->createPage("Notes")->id;
	const UUID home = ws->createPage("Home")->id;
	PlannerPanel::selectPage(home);
	REQUIRE(ws->editPage(home, [](Page& p) { p.addBlock(BlockType::PageLink, "", 1); return true; }));
	idle(ctx);

	REQUIRE(click(ctx, "page-link:0"));   // the chooser
	REQUIRE(PlannerPanel::controlRect("pick-page:Notes").x1 > 0);
	CHECK(PlannerPanel::controlRect("pick-page:Home").x1 <= 0);   // not itself
	REQUIRE(click(ctx, "pick-page:Notes"));
	idle(ctx);
	CHECK(ws->findPage(home)->blocks[0].page == notes);

	// The link goes to the page; it survives in the file.
	Page onDisk;
	REQUIRE(loadPage(proj.root(), home, onDisk).ok);
	CHECK(onDisk.blocks[0].page == notes);
	REQUIRE(click(ctx, "page-link:0"));
	CHECK(PlannerPanel::selectedPage() == notes);
}

TEST_CASE("planner ui: a linked row in a relation opens that row")
{
	Harness harness;
	Project proj("he_planner_ui_relopen");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	settle(ctx);
	REQUIRE(click(ctx, "New Database"));
	settle(ctx);
	Workspace* ws = PlannerPanel::workspace(ctx);
	const UUID dbId = PlannerPanel::selectedDatabase();
	UUID alpha, beta;
	REQUIRE(ws->edit(dbId, [&](Database& db) {
		Property* p = db.addProperty("Blocked by", PropertyType::Relation);
		p->relationDb = db.id;
		const UUID pid = p->id;
		alpha = db.addRow("Alpha", "Tester", 1000)->id;
		beta  = db.addRow("Beta", "Tester", 1001)->id;
		return db.setRelation(alpha, pid, { beta }, 1);
	}));
	idle(ctx);
	REQUIRE(click(ctx, "open:Alpha"));
	idle(ctx);
	CHECK(PlannerPanel::selectedRow() == alpha);
	REQUIRE(PlannerPanel::controlRect("relation-open:Beta").x1 > 0);
	REQUIRE(click(ctx, "relation-open:Beta"));
	idle(ctx);
	CHECK(PlannerPanel::selectedRow() == beta);
	CHECK(PlannerPanel::selectedDatabase() == dbId);
}

TEST_CASE("planner ui pages: the arrow keys leave a block past its first or last line")
{
	Harness harness;
	Project proj("he_planner_ui_arrows");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	idle(ctx);
	REQUIRE(click(ctx, "New Page"));
	idle(ctx);
	Workspace* ws = PlannerPanel::workspace(ctx);
	const UUID pageId = PlannerPanel::selectedPage();
	REQUIRE(ws->editPage(pageId, [](Page& p) {
		p.addBlock(BlockType::Paragraph, "first", 1);
		p.addBlock(BlockType::Divider, "", 2);   // no text: the arrows skip it
		p.addBlock(BlockType::Paragraph, "second", 3);
		return true;
	}));
	idle(ctx);
	auto press = [&](ImGuiKey key) {
		ImGuiIO& io = ImGui::GetIO();
		io.AddKeyEvent(key, true);
		frame(ctx, false);
		io.AddKeyEvent(key, false);
		settle(ctx);
	};
	REQUIRE(click(ctx, "block:0"));   // a click right of the text: the cursor is at its end
	press(ImGuiKey_DownArrow);
	typeText(ctx, "!");
	press(ImGuiKey_UpArrow);   // leaving a block commits what was typed in it
	REQUIRE(ws->findPage(pageId)->blocks.size() == 3);
	CHECK(ws->findPage(pageId)->blocks[2].text == "!second");   // down went past the divider; the cursor was at its start
	CHECK(ws->findPage(pageId)->blocks[0].text == "first");
	typeText(ctx, "?");   // up came in at the end of "first"
	// At the top of the page there is nothing above: the key stays in the block.
	press(ImGuiKey_UpArrow);
	typeText(ctx, "#");
	REQUIRE(click(ctx, "page:title"));   // leaving commits again
	CHECK(ws->findPage(pageId)->blocks[0].text == "first?#");
	CHECK(ws->findPage(pageId)->blocks[2].text == "!second");
}

TEST_CASE("planner ui pages: text typed in a block is kept when another block is clicked")
{
	Harness harness;
	Project proj("he_planner_ui_blockkeep");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	idle(ctx);
	REQUIRE(click(ctx, "New Page"));
	idle(ctx);
	Workspace* ws = PlannerPanel::workspace(ctx);
	const UUID pageId = PlannerPanel::selectedPage();
	REQUIRE(ws->editPage(pageId, [](Page& p) {
		p.addBlock(BlockType::Paragraph, "one", 1);
		p.addBlock(BlockType::Paragraph, "two", 2);
		return true;
	}));
	idle(ctx);

	// The second block is edited, then the FIRST is clicked: the first draws before the
	// second, so it has taken the focus by the time the second notices it lost it.
	REQUIRE(click(ctx, "block:1"));
	typeText(ctx, "!");
	REQUIRE(click(ctx, "block:0"));
	CHECK(ws->findPage(pageId)->blocks[1].text == "two!");
	CHECK(ws->findPage(pageId)->blocks[0].text == "one");
}

// ── The whiteboard ───────────────────────────────────────────────────────────

#include "PlannerBoardView.h"

namespace
{
	// A board in the project, shown; the view at the origin at 100 %.
	struct BoardFixture
	{
		Workspace* ws = nullptr;
		UUID       id;
	};

	BoardFixture openBoard(AppContext& ctx)
	{
		BoardFixture f;
		idle(ctx);
		REQUIRE(click(ctx, "New Board"));
		idle(ctx);
		f.ws = PlannerPanel::workspace(ctx);
		f.id = PlannerPanel::selectedBoard();
		REQUIRE(f.ws->findBoard(f.id) != nullptr);
		PlannerBoardView::setView(f.id, { 0, 0, 1.0 });
		idle(ctx);
		return f;
	}

	ImVec2 canvasPoint(double wx, double wy)
	{
		const PlannerPanel::Rect c = PlannerPanel::controlRect("canvas");
		const PlannerBoardView::ViewState v = PlannerBoardView::viewOf(PlannerPanel::selectedBoard());
		return ImVec2(c.x0 + static_cast<float>((wx - v.x) * v.zoom), c.y0 + static_cast<float>((wy - v.y) * v.zoom));
	}

	void pressMouse(ImGuiMouseButton button, bool down) { ImGui::GetIO().AddMouseButtonEvent(button, down); }

	void doubleClickAt(AppContext& ctx, ImVec2 p)
	{
		ImGui::GetIO().AddMousePosEvent(p.x, p.y);
		frame(ctx, false);
		frame(ctx, false);
		frame(ctx, true);
		frame(ctx, false);
		frame(ctx, true);
		frame(ctx, false);
		settle(ctx);
	}

	// A drag with the left button, from a screen point to another.
	void dragScreen(AppContext& ctx, ImVec2 from, ImVec2 to) { dragTo(ctx, from, to); idle(ctx); }

	void keyPress(AppContext& ctx, ImGuiKey key, bool ctrl = false, bool shift = false)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMousePosEvent(centre(PlannerPanel::controlRect("canvas")).x, centre(PlannerPanel::controlRect("canvas")).y);
		if (ctrl)  io.AddKeyEvent(ImGuiMod_Ctrl, true);
		if (shift) io.AddKeyEvent(ImGuiMod_Shift, true);
		io.AddKeyEvent(key, true);
		frame(ctx, false);
		io.AddKeyEvent(key, false);
		if (ctrl)  io.AddKeyEvent(ImGuiMod_Ctrl, false);
		if (shift) io.AddKeyEvent(ImGuiMod_Shift, false);
		settle(ctx);
	}

	const Node* nodeTexted(Workspace* ws, const UUID& board, const char* text)
	{
		const Board* b = ws->findBoard(board);
		for (const Node& n : b->nodes)
			if (n.text == text) return &n;
		return nullptr;
	}
}

TEST_CASE("planner ui board: New Board makes a file; a double-click makes a card and typing fills it")
{
	Harness harness;
	Project proj("he_planner_ui_board_new");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	BoardFixture f = openBoard(ctx);
	CHECK(std::filesystem::exists(boardPath(proj.root(), f.id)));
	CHECK(PlannerPanel::controlRect("canvas").x1 > 0);
	shotTo(ctx, "planner_board_empty");

	// Double-click empty space: a card centred there, the cursor in it.
	doubleClickAt(ctx, canvasPoint(300, 200));
	REQUIRE(f.ws->findBoard(f.id)->nodes.size() == 1);
	const Node& n = f.ws->findBoard(f.id)->nodes[0];
	CHECK(n.type == NodeType::Text);
	CHECK(n.x == doctest::Approx(300 - 120));
	CHECK(n.y == doctest::Approx(200 - 60));
	CHECK(PlannerBoardView::selection() == std::vector<UUID>{ n.id });
	typeText(ctx, "Fog rolls in at dusk");
	// Clicking away commits it.
	click(ctx, "canvas");
	idle(ctx);
	REQUIRE(f.ws->findBoard(f.id)->nodes.size() == 1);
	CHECK(f.ws->findBoard(f.id)->nodes[0].text == "Fog rolls in at dusk");
	CHECK(PlannerBoardView::canUndo(f.id));

	// It is in the file a teammate pulls.
	Board onDisk;
	REQUIRE(loadBoard(proj.root(), f.id, onDisk).ok);
	REQUIRE(onDisk.nodes.size() == 1);
	CHECK(onDisk.nodes[0].text == "Fog rolls in at dusk");
	shotTo(ctx, "planner_board_card");

	// The toolbar's New Card puts one in the middle of what is seen; the title is saved when left.
	REQUIRE(click(ctx, "New Card"));
	idle(ctx);
	CHECK(f.ws->findBoard(f.id)->nodes.size() == 2);
	REQUIRE(click(ctx, "board:title"));
	for (int i = 0; i < 14; ++i)   // "Untitled board" goes
	{
		ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, true);
		frame(ctx, false);
		ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, false);
		frame(ctx, false);
	}
	typeText(ctx, "Harbour ideas");
	click(ctx, "canvas");
	idle(ctx);
	CHECK(f.ws->findBoard(f.id)->title == "Harbour ideas");
}

TEST_CASE("planner ui board: dragging moves a card, undo and redo walk it back and forth")
{
	Harness harness;
	Project proj("he_planner_ui_board_move");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	BoardFixture f = openBoard(ctx);
	UUID a;
	REQUIRE(f.ws->editBoard(f.id, [&](Board& b) { a = b.addNode(NodeType::Text, 100, 100, 200, 100, "alpha", 1)->id; return true; }));
	idle(ctx);

	const PlannerPanel::Rect r = PlannerPanel::controlRect("node:alpha");
	REQUIRE(r.x1 > r.x0);
	dragScreen(ctx, centre(r), ImVec2(centre(r).x + 150, centre(r).y + 60));
	CHECK(f.ws->findBoard(f.id)->findNode(a)->x == doctest::Approx(250));
	CHECK(f.ws->findBoard(f.id)->findNode(a)->y == doctest::Approx(160));
	CHECK(PlannerBoardView::selection() == std::vector<UUID>{ a });
	CHECK(PlannerBoardView::canUndo(f.id));
	Board onDisk;
	REQUIRE(loadBoard(proj.root(), f.id, onDisk).ok);
	CHECK(onDisk.findNode(a)->x == doctest::Approx(250));

	// A click that does not move writes nothing and adds no step.
	const std::string before = f.ws->findBoard(f.id)->toJson();
	REQUIRE(click(ctx, "node:alpha"));
	CHECK(f.ws->findBoard(f.id)->toJson() == before);

	// Undo and redo, by the buttons and by the keys.
	REQUIRE(click(ctx, "Undo"));
	idle(ctx);
	CHECK(f.ws->findBoard(f.id)->findNode(a)->x == doctest::Approx(100));
	CHECK(PlannerBoardView::canRedo(f.id));
	REQUIRE(click(ctx, "Redo"));
	idle(ctx);
	CHECK(f.ws->findBoard(f.id)->findNode(a)->x == doctest::Approx(250));
	keyPress(ctx, ImGuiKey_Z, /*ctrl=*/true);
	CHECK(f.ws->findBoard(f.id)->findNode(a)->x == doctest::Approx(100));
	keyPress(ctx, ImGuiKey_Z, /*ctrl=*/true, /*shift=*/true);
	CHECK(f.ws->findBoard(f.id)->findNode(a)->x == doctest::Approx(250));
	// Undo is saved: the file says so.
	keyPress(ctx, ImGuiKey_Z, true);
	REQUIRE(loadBoard(proj.root(), f.id, onDisk).ok);
	CHECK(onDisk.findNode(a)->x == doctest::Approx(100));
}

TEST_CASE("planner ui board: corner handle resizes, arrows are dragged from a dot, delete takes the arrows with the card")
{
	Harness harness;
	Project proj("he_planner_ui_board_arrows");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	BoardFixture f = openBoard(ctx);
	UUID a, b;
	REQUIRE(f.ws->editBoard(f.id, [&](Board& bd) {
		a = bd.addNode(NodeType::Text, 100, 100, 200, 100, "alpha", 1)->id;
		b = bd.addNode(NodeType::Text, 500, 120, 200, 100, "beta", 1)->id;
		return true;
	}));
	idle(ctx);

	// Select one card: its handles appear; drag the south-east corner.
	REQUIRE(click(ctx, "node:alpha"));
	const PlannerPanel::Rect se = PlannerPanel::controlRect("handle:se");
	REQUIRE(se.x1 > se.x0);
	dragScreen(ctx, centre(se), ImVec2(centre(se).x + 100, centre(se).y + 50));
	const Node* alpha = f.ws->findBoard(f.id)->findNode(a);
	CHECK(alpha->x == doctest::Approx(100));
	CHECK(alpha->y == doctest::Approx(100));
	CHECK(alpha->w == doctest::Approx(300));
	CHECK(alpha->h == doctest::Approx(150));
	// The north-west corner moves the other way and never shrinks below the minimum.
	const PlannerPanel::Rect nw = PlannerPanel::controlRect("handle:nw");
	REQUIRE(nw.x1 > nw.x0);
	dragScreen(ctx, centre(nw), ImVec2(centre(nw).x + 1000, centre(nw).y + 1000));
	alpha = f.ws->findBoard(f.id)->findNode(a);
	CHECK(alpha->w == doctest::Approx(Board::kMinSize));
	CHECK(alpha->h == doctest::Approx(Board::kMinSize));
	REQUIRE(click(ctx, "Undo"));
	REQUIRE(click(ctx, "Undo"));
	idle(ctx);
	CHECK(f.ws->findBoard(f.id)->findNode(a)->w == doctest::Approx(200));

	// An arrow: from the east dot of alpha onto beta.
	REQUIRE(click(ctx, "node:alpha"));
	const PlannerPanel::Rect dot = PlannerPanel::controlRect("dot:E");
	REQUIRE(dot.x1 > dot.x0);
	dragScreen(ctx, centre(dot), centre(PlannerPanel::controlRect("node:beta")));
	REQUIRE(f.ws->findBoard(f.id)->edges.size() == 1);
	CHECK(f.ws->findBoard(f.id)->edges[0].from == a);
	CHECK(f.ws->findBoard(f.id)->edges[0].to == b);
	shotTo(ctx, "planner_board_arrow");
	// The same arrow again is refused; one onto the card itself too.
	REQUIRE(click(ctx, "node:alpha"));
	dragScreen(ctx, centre(PlannerPanel::controlRect("dot:E")), centre(PlannerPanel::controlRect("node:beta")));
	CHECK(f.ws->findBoard(f.id)->edges.size() == 1);

	// Click the arrow: its label and head can be set.
	const UUID edgeId = f.ws->findBoard(f.id)->edges[0].id;
	REQUIRE(click(ctx, "edge:"));
	CHECK(PlannerBoardView::selectedEdge() == edgeId);
	REQUIRE(click(ctx, "edge:label"));
	typeText(ctx, "leads to");
	click(ctx, "canvas");
	idle(ctx);
	CHECK(f.ws->findBoard(f.id)->edges[0].label == "leads to");

	// Delete a card: its arrows go with it; undo brings both back.
	REQUIRE(click(ctx, "node:beta"));
	keyPress(ctx, ImGuiKey_Delete);
	CHECK(f.ws->findBoard(f.id)->nodes.size() == 1);
	CHECK(f.ws->findBoard(f.id)->edges.empty());
	keyPress(ctx, ImGuiKey_Z, true);
	CHECK(f.ws->findBoard(f.id)->nodes.size() == 2);
	CHECK(f.ws->findBoard(f.id)->edges.size() == 1);
}

TEST_CASE("planner ui board: scroll pans, Ctrl+scroll zooms at the cursor, Fit shows everything")
{
	Harness harness;
	Project proj("he_planner_ui_board_view");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	BoardFixture f = openBoard(ctx);
	REQUIRE(f.ws->editBoard(f.id, [&](Board& b) {
		b.addNode(NodeType::Text, 100, 100, 200, 100, "alpha", 1);
		b.addNode(NodeType::Text, 3000, 2000, 200, 100, "far away", 1);
		return true;
	}));
	idle(ctx);
	CHECK(PlannerPanel::controlRect("node:far away").x1 <= 0);   // off screen: not drawn

	// Scroll pans by the wheel (down moves the view down).
	ImGuiIO& io = ImGui::GetIO();
	const ImVec2 mid = centre(PlannerPanel::controlRect("canvas"));
	io.AddMousePosEvent(mid.x, mid.y);
	frame(ctx, false);
	io.AddMouseWheelEvent(0.0f, -2.0f);
	frame(ctx, false);
	settle(ctx);
	CHECK(PlannerBoardView::viewOf(f.id).y == doctest::Approx(60.0));

	// Ctrl + wheel zooms and keeps the world point under the cursor.
	double wx = 0, wy = 0;
	{
		const PlannerBoardView::ViewState v = PlannerBoardView::viewOf(f.id);
		const PlannerPanel::Rect c = PlannerPanel::controlRect("canvas");
		wx = v.x + (mid.x - c.x0) / v.zoom;
		wy = v.y + (mid.y - c.y0) / v.zoom;
	}
	io.AddKeyEvent(ImGuiMod_Ctrl, true);
	io.AddMouseWheelEvent(0.0f, 3.0f);
	frame(ctx, false);
	io.AddKeyEvent(ImGuiMod_Ctrl, false);
	settle(ctx);
	const PlannerBoardView::ViewState zoomed = PlannerBoardView::viewOf(f.id);
	CHECK(zoomed.zoom == doctest::Approx(std::pow(1.1, 3.0)));
	{
		const PlannerPanel::Rect c = PlannerPanel::controlRect("canvas");
		CHECK(zoomed.x + (mid.x - c.x0) / zoomed.zoom == doctest::Approx(wx).epsilon(0.001));
		CHECK(zoomed.y + (mid.y - c.y0) / zoomed.zoom == doctest::Approx(wy).epsilon(0.001));
	}

	// Pan with the middle button.
	const PlannerBoardView::ViewState before = PlannerBoardView::viewOf(f.id);
	io.AddMousePosEvent(mid.x, mid.y);
	frame(ctx, false);
	pressMouse(ImGuiMouseButton_Middle, true);
	frame(ctx, false);
	io.AddMousePosEvent(mid.x + 100, mid.y + 40);
	frame(ctx, false);
	pressMouse(ImGuiMouseButton_Middle, false);
	frame(ctx, false);
	settle(ctx);
	CHECK(PlannerBoardView::viewOf(f.id).x == doctest::Approx(before.x - 100 / before.zoom));
	CHECK(PlannerBoardView::viewOf(f.id).y == doctest::Approx(before.y - 40 / before.zoom));

	// Fit: both cards are in view.
	REQUIRE(click(ctx, "Fit"));
	idle(ctx);
	CHECK(PlannerBoardView::lastStats().nodes == 2);
	CHECK(PlannerBoardView::lastStats().drawn == 2);
	CHECK(PlannerBoardView::viewOf(f.id).zoom < 1.0);
	shotTo(ctx, "planner_board_fit");
	REQUIRE(click(ctx, "100%"));
	CHECK(PlannerBoardView::viewOf(f.id).zoom == doctest::Approx(1.0));
}

TEST_CASE("planner ui board: a rectangle selects, colours apply, a frame moves what is inside it, only visible cards are drawn")
{
	Harness harness;
	Project proj("he_planner_ui_board_select");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	BoardFixture f = openBoard(ctx);
	UUID frameId, inside, outside;
	REQUIRE(f.ws->editBoard(f.id, [&](Board& b) {
		frameId = b.addNode(NodeType::Frame, 40, 40, 400, 300, "Level 1", 1)->id;
		inside  = b.addNode(NodeType::Text, 80, 100, 160, 80, "inside", 1)->id;
		outside = b.addNode(NodeType::Text, 600, 100, 160, 80, "outside", 1)->id;
		return true;
	}));
	idle(ctx);

	// A rectangle drawn over empty space round both cards (the press must start on nothing).
	dragScreen(ctx, canvasPoint(20, 20), canvasPoint(800, 400));
	CHECK(PlannerBoardView::selection().size() == 3);
	// Colour what is selected.
	REQUIRE(click(ctx, "swatch:yellow"));
	CHECK(f.ws->findBoard(f.id)->findNode(inside)->color == "yellow");
	CHECK(f.ws->findBoard(f.id)->findNode(outside)->color == "yellow");
	shotTo(ctx, "planner_board_selected");
	REQUIRE(click(ctx, "swatch:none"));
	CHECK(f.ws->findBoard(f.id)->findNode(inside)->color.empty());

	// Click empty space: nothing is selected. Then drag the frame: its contents follow.
	dragScreen(ctx, canvasPoint(900, 380), canvasPoint(910, 390));   // a tiny rectangle on nothing
	CHECK(PlannerBoardView::selection().empty());
	const PlannerPanel::Rect fr = PlannerPanel::controlRect("node:Level 1");
	REQUIRE(fr.x1 > fr.x0);
	dragScreen(ctx, ImVec2(fr.x0 + 6, fr.y1 - 6), ImVec2(fr.x0 + 6 + 100, fr.y1 - 6 + 50));   // the empty corner of the frame
	CHECK(f.ws->findBoard(f.id)->findNode(frameId)->x == doctest::Approx(140));
	CHECK(f.ws->findBoard(f.id)->findNode(inside)->x == doctest::Approx(180));
	CHECK(f.ws->findBoard(f.id)->findNode(inside)->y == doctest::Approx(150));
	CHECK(f.ws->findBoard(f.id)->findNode(outside)->x == doctest::Approx(600));   // not inside: it stays

	// Many cards: only those in view are laid out.
	REQUIRE(f.ws->editBoard(f.id, [&](Board& b) {
		for (int i = 0; i < 2000; ++i) b.addNode(NodeType::Text, 2000.0 + (i % 50) * 300, 2000.0 + (i / 50) * 200, 240, 120, "card " + std::to_string(i), 1);
		return true;
	}));
	idle(ctx);
	CHECK(PlannerBoardView::lastStats().nodes == 2003);
	CHECK(PlannerBoardView::lastStats().drawn < 20);
}

TEST_CASE("planner ui board: boards switch cleanly; delete asks first; a pull under the open board is read")
{
	Harness harness;
	Project proj("he_planner_ui_board_switch");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	BoardFixture f = openBoard(ctx);
	REQUIRE(f.ws->editBoard(f.id, [&](Board& b) { b.title = "First"; b.addNode(NodeType::Text, 100, 100, 200, 100, "one", 1); return true; }));
	idle(ctx);
	REQUIRE(click(ctx, "node:one"));
	CHECK(PlannerBoardView::selection().size() == 1);

	// A second board: the selection of the first means nothing there.
	REQUIRE(click(ctx, "New Board"));
	idle(ctx);
	CHECK(PlannerPanel::selectedBoard() != f.id);
	CHECK(PlannerBoardView::selection().empty());
	REQUIRE(f.ws->boards().size() == 2);

	// Back to the first by the sidebar; a database or a page replaces the board.
	REQUIRE(click(ctx, "board:First"));
	idle(ctx);
	CHECK(PlannerPanel::selectedBoard() == f.id);
	PlannerPanel::selectDatabase(tasksDatabaseId());
	idle(ctx);
	CHECK(PlannerPanel::selectedBoard() == UUID{});
	CHECK(PlannerPanel::controlRect("canvas").x1 <= 0);
	PlannerPanel::selectBoard(f.id);
	idle(ctx);
	CHECK(PlannerPanel::controlRect("canvas").x1 > 0);

	// A teammate's card arrives with a pull; the open board shows it.
	{
		Workspace other;
		other.setProject(proj.root());
		REQUIRE(other.editBoard(f.id, [](Board& b) { b.addNode(NodeType::Text, 400, 100, 200, 100, "theirs", 2); return true; }));
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(600));
	idle(ctx, 4);
	CHECK(PlannerPanel::controlRect("node:theirs").x1 > 0);
	// Undo after a pull does not take the pulled card away (it was not ours to undo).
	REQUIRE(f.ws->findBoard(f.id)->nodes.size() == 2);
}
