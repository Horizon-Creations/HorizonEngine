#include "doctest.h"
#include "ImGuiSoftwareRaster.h"
#include "TestFsUtil.h"

#include "EditorApplication.h"   // AppContext
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

#include <imgui.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
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

	void frame(AppContext& ctx, bool mouseDown, he_ui::Image* shot = nullptr)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
		ImGui::NewFrame();
		PlannerPanel::render(ctx, ImVec2(10, 10), ImVec2(float(W) - 20, float(H) - 20));
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
	REQUIRE(PlannerPanel::workspace(ctx) != nullptr);
	CHECK(PlannerPanel::workspace(ctx)->databases().empty());
	shotTo(ctx, "planner_empty");

	REQUIRE(click(ctx, "New Database"));
	settle(ctx);
	Workspace* ws = PlannerPanel::workspace(ctx);
	REQUIRE(ws->databases().size() == 1);
	const Database& db = *ws->databases()[0];
	CHECK(db.name == "New Database");
	CHECK(std::filesystem::exists(databasePath(proj.root(), db.id)));
	CHECK(PlannerPanel::selectedDatabase() == db.id);
	CHECK(PlannerPanel::controlRect("New Row").x1 > 0);   // the database is open
}

TEST_CASE("planner ui: Import Task Board fills Tasks, again changes nothing")
{
	Harness harness;
	Project proj("he_planner_ui_import");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	addTasks(ctx, { "Retexture the crates", "Fix the bridge", "Odd one" });
	settle(ctx);

	REQUIRE(click(ctx, "Import Task Board"));
	settle(ctx);
	Database* db = tasksDb(ctx);
	REQUIRE(db != nullptr);
	CHECK(db->rows.size() == 3);
	CHECK(PlannerPanel::notice().find("Imported 3 new") != std::string::npos);
	CHECK(PlannerPanel::selectedDatabase() == tasksDatabaseId());
	CHECK(PlannerPanel::controlRect("title:Fix the bridge").x1 > 0);
	shotTo(ctx, "planner_tasks_table");

	const std::string before = db->toJson();
	REQUIRE(click(ctx, "Import Task Board"));
	settle(ctx);
	CHECK(tasksDb(ctx)->toJson() == before);
	CHECK(PlannerPanel::notice().find("Imported 0 new and updated 0") != std::string::npos);
}

TEST_CASE("planner ui: a title typed into a cell is saved when the cell is left")
{
	Harness harness;
	Project proj("he_planner_ui_type");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	settle(ctx);
	REQUIRE(click(ctx, "New Database"));
	settle(ctx);
	REQUIRE(click(ctx, "New Row"));
	settle(ctx);
	Workspace* ws = PlannerPanel::workspace(ctx);
	const UUID dbId = PlannerPanel::selectedDatabase();
	REQUIRE(ws->find(dbId)->rows.size() == 1);
	CHECK(ws->find(dbId)->rows[0].createdBy == "Tester");

	// The new row's title is empty; its field is the first thing in the table.
	REQUIRE(click(ctx, "title:"));
	ImGuiIO& io = ImGui::GetIO();
	io.AddInputCharactersUTF8("Harbour level");
	settle(ctx);
	CHECK(ws->find(dbId)->rows[0].title.empty());   // not saved while typing
	io.AddKeyEvent(ImGuiKey_Enter, true);
	frame(ctx, false);
	io.AddKeyEvent(ImGuiKey_Enter, false);
	settle(ctx);
	CHECK(ws->find(dbId)->rows[0].title == "Harbour level");

	// ...and it is in the file, not just in memory.
	Database onDisk;
	REQUIRE(loadDatabase(proj.root(), dbId, onDisk).ok);
	CHECK(onDisk.rows[0].title == "Harbour level");
}

TEST_CASE("planner ui: a header sorts, twice reverses, a third time clears")
{
	Harness harness;
	Project proj("he_planner_ui_sort");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	addTasks(ctx, { "Charlie", "Alpha", "Bravo" });
	settle(ctx);
	REQUIRE(click(ctx, "Import Task Board"));
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
	// The imported "All tasks" view starts sorted by Status; sort by Title instead.
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
	CHECK(order() == "Charlie Alpha Bravo ");   // file order again
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

TEST_CASE("planner ui: a card dragged to another lane changes the status")
{
	Harness harness;
	Project proj("he_planner_ui_board");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	addTasks(ctx, { "Carry crates" });
	settle(ctx);
	REQUIRE(click(ctx, "Import Task Board"));
	settle(ctx);
	REQUIRE(click(ctx, "+ Board"));
	settle(ctx);
	const Database* db = tasksDb(ctx);
	REQUIRE(db);
	const UUID status = derivedId("property", "Status");
	const Property* prop = db->findProperty(status);
	REQUIRE(prop);
	const UUID open = prop->findOptionByName("Open")->id;
	const UUID done = prop->findOptionByName("Done")->id;
	REQUIRE(db->select(db->rows[0].id, status) == open);

	settle(ctx);
	const PlannerPanel::Rect card = PlannerPanel::controlRect("card:Carry crates");
	const PlannerPanel::Rect lane = PlannerPanel::controlRect("lane:Done");
	REQUIRE(card.x1 > card.x0);
	REQUIRE(lane.x1 > lane.x0);
	shotTo(ctx, "planner_board_before");
	dragTo(ctx, centre(card), centre(lane));
	settle(ctx);
	CHECK(tasksDb(ctx)->select(tasksDb(ctx)->rows[0].id, status) == done);
	shotTo(ctx, "planner_board_after");
}
