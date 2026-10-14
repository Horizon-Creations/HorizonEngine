#include "doctest.h"
#include "ImGuiSoftwareRaster.h"
#include "TestFsUtil.h"

#include "TaskBoardFooter.h"
#include "TaskBoardPanel.h"
#include "TaskBoard.h"
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include "ProjectManager.h"

#include <imgui.h>
#include <imgui_internal.h>   // GetHoveredID / FindWindowByName

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ── The task board's footer widget, driven headless ──────────────────────────
// TaskBoardFooter drawn inside a stand-in for the editor's footer — the same
// 24px strip, the same padding, the same font — over a real project on disk:
// that the widget is one text line tall whatever it says, that it never
// reaches the middle third where the status line lives (counted in pixels,
// with a long title), that its list opens above the footer, that a tick there
// writes Done into Config/TaskBoard.json and an untick puts the old status
// back, that a pull landing while the list is open is kept, and that the list
// asks for the panel. With HE_UI_DUMP_DIR set the frames are written out.

using namespace HE::Ed;

namespace
{
	constexpr int   W = 900, H = 320;
	constexpr float kFooterH = 24.0f;   // EditorUI's kFooterH
	constexpr float kLeftW   = 40.0f;   // stands in for Undo/Redo + the source-control status

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
			TaskBoardFooter::reset();
			TaskBoardPanel::setAuthor("Tester");
		}
		~Harness()
		{
			TaskBoardFooter::reset();
			TaskBoardPanel::reset();
			ImGui::DestroyContext();
		}
	};

	// Everything AppContext insists on being given (see test_taskboard_ui.cpp).
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

	// What one frame did.
	struct Frame
	{
		float footerX = 0.0f;   // where the widget started (window-local cursor x)
		float drawn   = 0.0f;   // DrawFooter's width
		bool  panelAsked = false;
	};

	// The footer, as EditorUI draws it: pinned to the bottom, 24px, padding
	// 8/4, a left-hand neighbour, then the widget with the real budget.
	Frame drawFooter(AppContext& ctx, float footerW)
	{
		Frame f;
		ImGui::SetNextWindowPos(ImVec2(0.0f, float(H) - kFooterH), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(footerW, kFooterH), ImGuiCond_Always);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(8.0f, 4.0f));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.13f, 0.13f, 0.15f, 1.0f));
		ImGui::Begin("##EditorFooter", nullptr,
		             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
		             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
		             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration);
		ImGui::PopStyleVar(3);
		ImGui::PopStyleColor();
		ImGui::Dummy(ImVec2(kLeftW, 16.0f));
		ImGui::SameLine(0.0f, 14.0f);
		f.footerX = ImGui::GetCursorPosX();
		f.drawn = TaskBoardFooter::DrawFooter(ctx, TaskBoardFooter::budget(f.footerX, ImGui::GetWindowWidth()));
		ImGui::End();
		return f;
	}

	float g_footerW = float(W);
	bool  g_footer  = true;   // false: a frame that skips the footer (the project hub)

	Frame frame(AppContext& ctx, bool mouseDown, he_ui::Image* shot = nullptr)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
		ImGui::NewFrame();
		Frame f;
		if (g_footer) f = drawFooter(ctx, g_footerW);
		f.panelAsked = TaskBoardFooter::DrawOverlay(ctx);
		EditorWidgets::drawQueuedHelp();
		ImGui::Render();
		if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
		return f;
	}

	// Settle with the mouse parked in a corner no window covers.
	Frame settle(AppContext& ctx, he_ui::Image* shot = nullptr)
	{
		ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, 2.0f);
		frame(ctx, false);
		frame(ctx, false);
		return frame(ctx, false, shot);
	}

	ImGuiID idAt(AppContext& ctx, ImVec2 p)
	{
		ImGui::GetIO().AddMousePosEvent(p.x, p.y);
		frame(ctx, false);
		frame(ctx, false);
		return ImGui::GetHoveredID();
	}

	// Returns whether any frame of the click asked for the panel.
	bool clickAt(AppContext& ctx, ImVec2 p)
	{
		ImGui::GetIO().AddMousePosEvent(p.x, p.y);
		bool asked = false;
		asked |= frame(ctx, false).panelAsked;
		asked |= frame(ctx, false).panelAsked;
		asked |= frame(ctx, true).panelAsked;
		asked |= frame(ctx, false).panelAsked;
		asked |= frame(ctx, false).panelAsked;
		return asked;
	}

	ImVec2 centre(const TaskBoardFooter::Rect& r) { return ImVec2((r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f); }

	void shotTo(const he_ui::Image& img, const char* name)
	{
		if (const char* d = std::getenv("HE_UI_DUMP_DIR"); d && *d)
			he_ui::writeBmp(img, std::string(d) + "/" + name + ".bmp");
	}

	// Pixels bright enough to be text (even the dimmed footer grey) inside a
	// rectangle — the footer's background is far darker.
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

	// Adds tasks through the panel's own edit() — the one writer of the file.
	struct Ids { HE::UUID a, b, c; };
	Ids addThree(AppContext& ctx)
	{
		Ids ids;
		REQUIRE(TaskBoardPanel::edit(ctx, [&](HE::TaskBoard& b) {
			const std::int64_t now = HE::taskNowMs();
			HE::Task* t1 = b.add("Fix the harbour lamps that flicker at night, the ones on the long pier past "
			                     "the fish market where the water meets the docks and the fog rolls in", "Tester", now);
			t1->priority = HE::TaskPriority::Urgent;
			ids.a = t1->id;   // before the next add moves the list
			ids.b = b.add("Retexture the crates", "Tester", now)->id;
			b.setStatus(ids.b, HE::TaskStatus::InProgress, "Tester", now);
			ids.c = b.add("Write the credits", "Tester", now)->id;
			return true;
		}));
		return ids;
	}

	const HE::Task* taskOf(AppContext& ctx, const HE::UUID& id)
	{
		HE::TaskBoard* b = TaskBoardPanel::board(ctx);
		return b ? b->find(id) : nullptr;
	}
}

TEST_CASE("task board footer: the next task is mine, then not blocked, then by priority, then started")
{
	HE::TaskBoard b;
	const std::int64_t now = 1'760'000'000'000;
	auto make = [&](const char* title, HE::TaskStatus s, HE::TaskPriority p, std::vector<std::string> who = {}) {
		HE::Task* t = b.add(title, "Tester", now);
		t->status = s;
		t->priority = p;
		t->assignees = std::move(who);
		return t->id;
	};
	const HE::UUID t1 = make("open normal",          HE::TaskStatus::Open,       HE::TaskPriority::Normal);
	const HE::UUID t2 = make("blocked urgent",       HE::TaskStatus::Blocked,    HE::TaskPriority::Urgent);
	const HE::UUID t3 = make("in progress normal",   HE::TaskStatus::InProgress, HE::TaskPriority::Normal);
	const HE::UUID t4 = make("open high",            HE::TaskStatus::Open,       HE::TaskPriority::High);
	const HE::UUID t5 = make("open low, mine",       HE::TaskStatus::Open,       HE::TaskPriority::Low, { "Tester" });
	make("done urgent", HE::TaskStatus::Done, HE::TaskPriority::Urgent);
	const HE::UUID t7 = make("open normal, later",   HE::TaskStatus::Open,       HE::TaskPriority::Normal);

	CHECK(TaskBoardFooter::rankedOpen(b, "Tester") == std::vector<HE::UUID>{ t5, t4, t3, t1, t7, t2 });
	// Nobody's name: nothing is "mine", and a low priority sinks.
	CHECK(TaskBoardFooter::rankedOpen(b, "") == std::vector<HE::UUID>{ t4, t3, t1, t7, t5, t2 });
	CHECK(TaskBoardFooter::rankedOpen(b, "Someone Else") == TaskBoardFooter::rankedOpen(b, ""));

	const TaskBoardFooter::Summary s = TaskBoardFooter::summarize(b, "Tester");
	CHECK(s.open == 6);
	CHECK(s.mine == 1);
	CHECK(s.next == t5);
	CHECK(s.nextTitle == std::string("open low, mine"));

	// An untitled task still has something to show.
	HE::TaskBoard one;
	one.add("", "Tester", now);
	CHECK(TaskBoardFooter::summarize(one, "Tester").nextTitle == std::string("(untitled)"));
	// Nothing open: no next task.
	HE::TaskBoard none;
	CHECK(TaskBoardFooter::summarize(none, "Tester").next == HE::UUID{});

	// Up to a gap short of the middle third; nothing when the cursor is past it.
	CHECK(TaskBoardFooter::budget(100.0f, 1200.0f) == doctest::Approx(284.0f));
	CHECK(TaskBoardFooter::budget(500.0f, 1200.0f) == 0.0f);
}

TEST_CASE("task board footer ui: one line tall in every state, and never into the middle third")
{
	Harness harness;
	Project proj("he_taskboard_footer_ui_line");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	g_footer  = true;
	g_footerW = float(W);
	settle(ctx);   // the font's size is known once a frame has run
	const float line = ImGui::GetTextLineHeight();
	REQUIRE(line > 10.0f);
	const int   fy0 = int(float(H) - kFooterH), fy1 = H;

	// Every state: the widget's height is the text line's, and it ends before
	// the middle third — and so does every bright pixel in the footer.
	auto check = [&](const char* name, he_ui::Image& img) {
		const Frame f = settle(ctx, &img);
		REQUIRE(img.valid());
		shotTo(img, name);
		TaskBoardFooter::Rect r;
		REQUIRE(TaskBoardFooter::widgetRect(r));
		CHECK(f.drawn > 0.0f);
		CHECK(r.y1 - r.y0 == doctest::Approx(line));
		CHECK(r.y0 >= float(fy0));
		CHECK(r.y1 <= float(fy1));
		const float edge = g_footerW / 3.0f - 16.0f;   // window x is 0
		CHECK_MESSAGE(r.x1 <= edge + 0.5f, std::string(name));
		CHECK_MESSAGE(textPixelsIn(img, int(edge) + 1, fy0, W, fy1) == 0, std::string(name));
		return r;
	};

	// No tasks: "No open tasks", dimmed — the widget is there to be found.
	he_ui::Image empty, many, allDone;
	const TaskBoardFooter::Rect rEmpty = check("taskboard_footer_empty", empty);
	CHECK(textPixelsIn(empty, int(rEmpty.x0), fy0, int(rEmpty.x1) + 1, fy1) > 20);
	// Still no file: drawing the widget adds nothing to the project.
	CHECK_FALSE(std::filesystem::exists(HE::taskBoardPath(proj.root())));

	// Three tasks, the next one with a title far wider than the room: cut, not
	// spilled, and the widget grew to take the room it had.
	const Ids ids = addThree(ctx);
	const TaskBoardFooter::Rect rMany = check("taskboard_footer_three", many);
	CHECK(rMany.x1 - rMany.x0 > rEmpty.x1 - rEmpty.x0 + 100.0f);
	CHECK(diffIn(empty, many, 0, fy0, W, fy1) > 200);
	// The title's last letters (or its "...") reach the end of the widget.
	CHECK(textPixelsIn(many, int(rMany.x1) - 12, fy0, int(rMany.x1) + 1, fy1) > 0);

	// All done: the box ticked, the count dimmed, the same height.
	REQUIRE(TaskBoardPanel::edit(ctx, [&](HE::TaskBoard& b) {
		for (const HE::UUID& id : { ids.a, ids.b, ids.c }) b.setStatus(id, HE::TaskStatus::Done, "Tester", HE::taskNowMs());
		return true;
	}));
	check("taskboard_footer_all_done", allDone);
	CHECK(diffIn(empty, allDone, int(rEmpty.x0), fy0, int(rEmpty.x0 + line), fy1) > 0);   // the tick in the box

	// Narrow footers give way: the title goes, then the words, then all of it.
	REQUIRE(TaskBoardPanel::edit(ctx, [&](HE::TaskBoard& b) { return b.setStatus(ids.b, HE::TaskStatus::Open, "Tester", HE::taskNowMs()); }));
	float prev = 1e9f;
	for (float fw : { 900.0f, 560.0f, 420.0f, 300.0f })
	{
		g_footerW = fw;
		he_ui::Image img;
		const std::string name = "taskboard_footer_narrow_" + std::to_string(int(fw));
		const TaskBoardFooter::Rect r = check(name.c_str(), img);
		CHECK(r.x1 - r.x0 <= prev);
		prev = r.x1 - r.x0;
	}
	// No room at all: nothing drawn, nothing to click.
	g_footerW = 150.0f;
	CHECK(settle(ctx).drawn == 0.0f);
	g_footerW = float(W);

	// No project: nothing at all.
	bits.projectLoaded = false;
	CHECK(settle(ctx).drawn == 0.0f);
	bits.projectLoaded = true;
}

TEST_CASE("task board footer ui: the list ticks a task off on disk, and an untick puts it back")
{
	Harness harness;
	Project proj("he_taskboard_footer_ui_tick");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	g_footer  = true;
	g_footerW = float(W);
	const Ids ids = addThree(ctx);
	settle(ctx);

	TaskBoardFooter::Rect w;
	REQUIRE(TaskBoardFooter::widgetRect(w));
	CHECK_FALSE(TaskBoardFooter::isOpen());
	CHECK_FALSE(clickAt(ctx, centre(w)));
	REQUIRE(TaskBoardFooter::isOpen());

	he_ui::Image open;
	settle(ctx, &open);
	shotTo(open, "taskboard_footer_list");
	TaskBoardFooter::Rect list;
	REQUIRE(TaskBoardFooter::listRect(list));
	// Above the widget, touching it — the mouse goes from one to the other
	// without crossing a third window — and over no more of the footer than
	// its top padding.
	CHECK(list.y1 <= w.y0 + 0.5f);
	CHECK(list.y1 >= w.y0 - 0.5f);
	CHECK(list.y1 - list.y0 > 3.0f * ImGui::GetFrameHeight());
	// Every open task has a row, the most urgent first.
	TaskBoardFooter::Rect ta, tb, tc;
	REQUIRE(TaskBoardFooter::tickRect(ids.a, ta));
	REQUIRE(TaskBoardFooter::tickRect(ids.b, tb));
	REQUIRE(TaskBoardFooter::tickRect(ids.c, tc));
	CHECK(ta.y0 < tb.y0);
	CHECK(tb.y0 < tc.y0);
	// The long title is cut at the list's edge.
	TaskBoardFooter::Rect title;
	REQUIRE(TaskBoardFooter::titleRect(ids.a, title));
	CHECK(title.x1 <= list.x1);
	CHECK(textPixelsIn(open, int(list.x1) + 1, int(list.y0), W, int(list.y1)) == 0);

	// Tick the started one: Done on disk, with the name, and the row stays.
	CHECK_FALSE(clickAt(ctx, centre(tb)));
	REQUIRE(taskOf(ctx, ids.b) != nullptr);
	CHECK(taskOf(ctx, ids.b)->status == HE::TaskStatus::Done);
	CHECK(taskOf(ctx, ids.b)->updates.back().author == std::string("Tester"));
	{
		const std::string file = readFile(HE::taskBoardPath(proj.root()));
		CHECK(file.find("\"done\"") != std::string::npos);
	}
	REQUIRE(TaskBoardFooter::isOpen());
	he_ui::Image ticked;
	settle(ctx, &ticked);
	shotTo(ticked, "taskboard_footer_list_ticked");
	REQUIRE(TaskBoardFooter::tickRect(ids.b, tb));

	// Untick: back to In Progress, not to Open.
	CHECK_FALSE(clickAt(ctx, centre(tb)));
	CHECK(taskOf(ctx, ids.b)->status == HE::TaskStatus::InProgress);
	CHECK(readFile(HE::taskBoardPath(proj.root())).find("\"in-progress\"") != std::string::npos);

	// A click anywhere else closes the list; so does a second click on the widget.
	clickAt(ctx, ImVec2(float(W) - 20.0f, 20.0f));
	CHECK_FALSE(TaskBoardFooter::isOpen());
	clickAt(ctx, centre(w));
	REQUIRE(TaskBoardFooter::isOpen());
	clickAt(ctx, centre(w));
	CHECK_FALSE(TaskBoardFooter::isOpen());
	// ...and Escape.
	clickAt(ctx, centre(w));
	REQUIRE(TaskBoardFooter::isOpen());
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
	frame(ctx, false);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
	frame(ctx, false);
	CHECK_FALSE(TaskBoardFooter::isOpen());

	// A frame without the footer (the project hub) leaves nothing hanging.
	clickAt(ctx, centre(w));
	REQUIRE(TaskBoardFooter::isOpen());
	g_footer = false;
	frame(ctx, false);
	g_footer = true;
	CHECK_FALSE(TaskBoardFooter::isOpen());
}

TEST_CASE("task board footer ui: a pull while the list is open is kept, and the tick lands on top of it")
{
	Harness harness;
	Project proj("he_taskboard_footer_ui_pull");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	g_footer  = true;
	g_footerW = float(W);
	const Ids ids = addThree(ctx);
	settle(ctx);
	TaskBoardFooter::Rect w;
	REQUIRE(TaskBoardFooter::widgetRect(w));
	clickAt(ctx, centre(w));
	REQUIRE(TaskBoardFooter::isOpen());
	settle(ctx);

	// A teammate's task arrives with a pull, written by another board.
	HE::UUID pulled;
	{
		HE::TaskBoard other;
		REQUIRE(HE::loadTaskBoard(proj.root(), other).ok);
		pulled = other.add("Pulled from origin", "Ana", HE::taskNowMs())->id;
		REQUIRE(HE::saveTaskBoard(proj.root(), other));
	}

	TaskBoardFooter::Rect tc;
	REQUIRE(TaskBoardFooter::tickRect(ids.c, tc));
	clickAt(ctx, centre(tc));

	HE::TaskBoard disk;
	REQUIRE(HE::loadTaskBoard(proj.root(), disk).ok);
	REQUIRE(disk.find(pulled) != nullptr);
	REQUIRE(disk.find(ids.c) != nullptr);
	CHECK(disk.find(ids.c)->status == HE::TaskStatus::Done);
	CHECK(disk.tasks.size() == 4);
}

TEST_CASE("task board footer ui: Open Task Board and a task's title ask for the panel")
{
	Harness harness;
	Project proj("he_taskboard_footer_ui_open");
	ContextBits bits;
	AppContext ctx = bits.make(proj.pm);
	g_footer  = true;
	g_footerW = float(W);
	const Ids ids = addThree(ctx);
	settle(ctx);
	TaskBoardFooter::Rect w;
	REQUIRE(TaskBoardFooter::widgetRect(w));

	// The button is the last control in the list, at its bottom-left.
	clickAt(ctx, centre(w));
	settle(ctx);
	TaskBoardFooter::Rect list;
	REQUIRE(TaskBoardFooter::listRect(list));
	ImGuiWindow* win = ImGui::FindWindowByName("##taskBoardFooterList");
	REQUIRE(win != nullptr);
	const ImVec2 at(list.x0 + 10.0f + 20.0f, list.y1 - 8.0f - ImGui::GetFrameHeight() * 0.5f);
	REQUIRE(idAt(ctx, at) == win->GetID("Open Task Board"));
	CHECK(clickAt(ctx, at));
	CHECK_FALSE(TaskBoardFooter::isOpen());

	// A title: the panel, with that task selected.
	CHECK(TaskBoardPanel::selectedTask() == HE::UUID{});
	clickAt(ctx, centre(w));
	settle(ctx);
	TaskBoardFooter::Rect title;
	REQUIRE(TaskBoardFooter::titleRect(ids.c, title));
	CHECK(clickAt(ctx, centre(title)));
	CHECK(TaskBoardPanel::selectedTask() == ids.c);
	CHECK_FALSE(TaskBoardFooter::isOpen());
	// Nothing was ticked on the way.
	CHECK(taskOf(ctx, ids.c)->status == HE::TaskStatus::Open);
}
