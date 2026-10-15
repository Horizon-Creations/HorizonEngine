#include "doctest.h"
#include "TestFsUtil.h"

#include "McpToolRegistry.h"
#include "TaskBoardPanel.h"
#include "TaskBoard.h"
#include "TaskBoardTeam.h"
#include "FakeTaskTeam.h"
#include "NotificationStore.h"
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "ProjectManager.h"

#include <filesystem>
#include <string>
#include <vector>

// ─── The task board as tools, and the two-editor acceptance ──────────────────
// What a script drives to check the board without clicking: list, create,
// update, assign, and the notices the team sent. Editor A ("Anna") is the
// editor's own path — the tools' hooks go to TaskBoardPanel::board/edit/team,
// the one copy of the board and the one writer of its file, over a real project
// on disk. Editor B ("Bob") is a second editor in the same process: its own
// project folder, its own board and bridge, wired the way the panel wires its
// own (save, then afterEdit). The panel's state is one per process, so B cannot
// be a second panel. Between them FakeTaskTeam stands in for the team server —
// this branch has no team transport (TeamController/TeamClient are on
// release/0.7.0), so the run with two editor PROCESSES waits for that merge.
//
// The questions:
//   • does every change land in Config/TaskBoard.json, through the panel's writer,
//   • is assigning a team feature (no_team on a local board, unknown_member off
//     the roster, the roster's spelling on the task),
//   • does B hear "Anna assigned you …" through task_notices AND the bell,
//     while its board only gets the task with the pull,
//   • does B's status update reach A the same way.

using HE::Ed::McpTaskBoardHooks;
using HE::Ed::McpToolRegistry;
using HE::Ed::ToolResult;
using nlohmann::json;

namespace {

namespace fs = std::filesystem;

// Everything AppContext insists on being given (as in test_taskboard_ui.cpp).
// The board reads projectLoaded and projectManager, contentManager when there.
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
	fs::path dir;
	ProjectManager pm;
	explicit Project(const char* name)
	{
		dir = fs::temp_directory_path() / name;
		he_test::removeAllQuiet(dir);
		REQUIRE(pm.createNewProject(dir.string(), "Board", ProjectPreset::Game));
	}
	~Project() { he_test::removeAllQuiet(dir); }
	std::string root() const { return pm.projectRoot(); }
};

// The panel's board is process state: whatever a case does to it, the next
// case (and test_taskboard_ui after it) starts from nothing — also when a
// REQUIRE leaves early, or the next one would pump a bridge that is gone.
struct PanelGuard
{
	PanelGuard() { TaskBoardPanel::reset(); }
	~PanelGuard()
	{
		TaskBoardPanel::setTeam(nullptr);
		TaskBoardPanel::setTeamClock({});
		TaskBoardPanel::setAuthor({});
		TaskBoardPanel::reset();
	}
};

// Editor A: the tools over the panel, as EditorApplication::setupMcpTools wires them.
struct PanelEditor
{
	ContextBits     bits;
	AppContext      ctx;
	McpToolRegistry registry;

	explicit PanelEditor(ProjectManager& pm) : ctx(bits.make(pm))
	{
		McpTaskBoardHooks h;
		h.board     = [this] { return TaskBoardPanel::board(ctx); };
		h.edit      = [this](const std::function<bool(HE::TaskBoard&)>& fn) { return TaskBoardPanel::edit(ctx, fn); };
		h.lastError = [] { return TaskBoardPanel::lastError(); };
		h.author    = [] { return TaskBoardPanel::author(); };
		h.team      = [] { return TaskBoardPanel::team(); };
		HE::Ed::registerTaskBoardTools(registry, std::move(h));
	}

	ToolResult call(const char* name, const json& args = json::object())
	{
		const auto* t = registry.find(name);
		REQUIRE(t != nullptr);
		return t->handler(args);
	}
};

// Editor B: its own folder, board and bridge. Saves and tells its bridge the
// way TaskBoardPanel::edit does; `pull` is git bringing A's file over.
struct SecondEditor
{
	fs::path                  root;
	HE::TaskBoard             board;
	HE::TaskBoardTeam         team;
	HE::Ed::NotificationStore bell;
	McpToolRegistry           registry;
	std::uint64_t*            clock = nullptr;

	SecondEditor(const char* name, std::uint64_t* now) : clock(now)
	{
		root = fs::temp_directory_path() / name;
		he_test::removeAllQuiet(root);
		fs::create_directories(root / "Config");
		REQUIRE(HE::loadTaskBoard(root, board).ok);
		team.setNotifications(&bell);

		McpTaskBoardHooks h;
		h.board = [this] { return &board; };
		h.edit  = [this](const std::function<bool(HE::TaskBoard&)>& fn) {
			const HE::TaskBoard before = board;
			if (!fn(board)) return false;
			if (!HE::saveTaskBoard(root, board)) return false;
			team.afterEdit(before, board, "Board", *clock);
			return true;
		};
		h.author = [this] { return team.self(); };
		h.team   = [this] { return &team; };
		HE::Ed::registerTaskBoardTools(registry, std::move(h));
	}
	~SecondEditor() { he_test::removeAllQuiet(root); }

	void pull(const fs::path& fromRoot)
	{
		fs::copy_file(HE::taskBoardPath(fromRoot), HE::taskBoardPath(root),
		              fs::copy_options::overwrite_existing);
		board = HE::TaskBoard{};
		REQUIRE(HE::loadTaskBoard(root, board).ok);
	}

	ToolResult call(const char* name, const json& args = json::object())
	{
		const auto* t = registry.find(name);
		REQUIRE(t != nullptr);
		return t->handler(args);
	}
};

HE::Task onDisk(const std::string& root, const std::string& idHex)
{
	HE::TaskBoard disk;
	REQUIRE(HE::loadTaskBoard(root, disk).ok);
	HE::UUID id;
	REQUIRE(HE::uuidFromHex(idHex, id));
	const HE::Task* t = disk.find(id);
	REQUIRE(t != nullptr);
	return *t;
}

} // namespace

TEST_CASE("task tools: registered with schemas; reading tools do not mutate; no project refuses")
{
	PanelGuard guard;
	McpToolRegistry registry;
	McpTaskBoardHooks h;
	h.board = [] { return static_cast<HE::TaskBoard*>(nullptr); };
	h.edit  = [](const std::function<bool(HE::TaskBoard&)>&) { return false; };
	HE::Ed::registerTaskBoardTools(registry, std::move(h));

	for (const char* name : { "task_list", "task_create", "task_update", "task_assign", "task_notices" })
	{
		const auto* t = registry.find(name);
		REQUIRE_MESSAGE(t != nullptr, name);
		CHECK(t->inputSchema.value("type", std::string()) == "object");
		CHECK_FALSE(t->description.empty());
	}
	CHECK_FALSE(registry.find("task_list")->mutates);
	CHECK_FALSE(registry.find("task_notices")->mutates);
	CHECK(registry.find("task_create")->mutates);
	CHECK(registry.find("task_update")->mutates);
	CHECK(registry.find("task_assign")->mutates);

	CHECK(registry.find("task_list")->handler(json::object()).errorCode == "no_project");
	CHECK(registry.find("task_create")->handler(json{ { "title", "x" } }).errorCode == "no_project");
	// No team is not an error for reading the notices: there are none.
	const ToolResult n = registry.find("task_notices")->handler(json::object());
	REQUIRE_FALSE(n.isError);
	CHECK(n.content["connected"] == false);
	CHECK(n.content["notices"].empty());
}

TEST_CASE("task tools: a local board — create, list, update land in the file; assigning needs a team")
{
	PanelGuard guard;
	Project proj("he_mcp_taskboard_local");
	PanelEditor a(proj.pm);
	TaskBoardPanel::setAuthor("Tester");

	const ToolResult c = a.call("task_create", json{
		{ "title", "Retexture the crates" }, { "description", "Wood, not plastic." },
		{ "priority", "high" } });
	REQUIRE_FALSE(c.isError);
	const std::string id = c.content["task"]["id"].get<std::string>();
	CHECK(c.content["task"]["status"] == "open");
	CHECK(c.content["task"]["priority"] == "high");
	CHECK(c.content["task"]["createdBy"] == "Tester");
	CHECK(c.content["team"]["connected"] == false);
	{
		const HE::Task t = onDisk(proj.root(), id);
		CHECK(t.title == "Retexture the crates");
		CHECK(t.description == "Wood, not plastic.");
		CHECK(t.priority == HE::TaskPriority::High);
	}
	// The panel's board is the one the tool wrote: no second copy.
	REQUIRE(TaskBoardPanel::board(a.ctx) != nullptr);
	CHECK(TaskBoardPanel::board(a.ctx)->tasks.size() == 1);

	REQUIRE_FALSE(a.call("task_create", json{ { "title", "Light the pier" }, { "status", "blocked" } }).isError);

	// List, filters.
	ToolResult l = a.call("task_list");
	REQUIRE_FALSE(l.isError);
	CHECK(l.content["tasks"].size() == 2);
	CHECK(l.content["openCount"] == 2);
	CHECK(l.content["author"] == "Tester");
	CHECK(a.call("task_list", json{ { "status", "blocked" } }).content["tasks"].size() == 1);

	// A status with a note, then a comment on its own.
	const ToolResult u = a.call("task_update", json{ { "id", id }, { "status", "done" }, { "comment", "Shipped." } });
	REQUIRE_FALSE(u.isError);
	CHECK(u.content["task"]["status"] == "done");
	REQUIRE_FALSE(a.call("task_update", json{ { "id", id }, { "comment", "Looks good." } }).isError);
	// The same status again is not a second status update; its comment still lands.
	REQUIRE_FALSE(a.call("task_update", json{ { "id", id }, { "status", "done" }, { "comment", "Really." } }).isError);
	{
		const HE::Task t = onDisk(proj.root(), id);
		CHECK(t.status == HE::TaskStatus::Done);
		REQUIRE(t.updates.size() == 3);
		CHECK(t.updates[0].kind == HE::TaskUpdateKind::Status);
		CHECK(t.updates[0].text == "Shipped.");
		CHECK(t.updates[1].text == "Looks good.");
		CHECK(t.updates[2].kind == HE::TaskUpdateKind::Comment);
		CHECK(t.updates[2].text == "Really.");
	}
	CHECK(a.call("task_list", json{ { "includeDone", false } }).content["tasks"].size() == 1);
	l = a.call("task_list", json{ { "status", "done" }, { "history", true } });
	REQUIRE(l.content["tasks"].size() == 1);
	CHECK(l.content["tasks"][0]["updates"].size() == 3);
	CHECK(l.content["tasks"][0]["updates"][0]["author"] == "Tester");

	// Assigning is the team's: refused on a local board, the file untouched.
	const ToolResult as = a.call("task_assign", json{ { "id", id }, { "assignees", json::array({ "Bob" }) } });
	CHECK(as.errorCode == "no_team");
	CHECK(onDisk(proj.root(), id).assignees.empty());

	// Refusals a client can act on.
	CHECK(a.call("task_create", json{ { "title", "" } }).errorCode == "invalid_payload");
	CHECK(a.call("task_create", json{ { "title", "x" }, { "priority", "asap" } }).errorCode == "invalid_payload");
	CHECK(a.call("task_update", json{ { "id", "nope" }, { "comment", "x" } }).errorCode == "invalid_payload");
	CHECK(a.call("task_update", json{ { "id", id } }).errorCode == "invalid_payload");
	CHECK(a.call("task_update", json{ { "id", HE::uuidToHex(HE::UUID::generate()) }, { "comment", "x" } })
	          .errorCode == "not_found");
}

TEST_CASE("task tools: two editors — Anna assigns through the tool, Bob's notices and bell say so, his update comes back")
{
	PanelGuard guard;
	Project proj("he_mcp_taskboard_anna");
	PanelEditor anna(proj.pm);

	he_test::FakeTaskTeam server;
	for (const char* m : { "Anna", "Bob" }) server.addMember(m);
	auto& annaLink = server.editor("Anna");
	auto& bobLink  = server.editor("Bob");
	HE::TaskBoardTeam annaTeam;
	annaTeam.setLink(&annaLink);
	HE::Ed::NotificationStore annaBell;
	annaTeam.setNotifications(&annaBell);
	SecondEditor bob("he_mcp_taskboard_bob", &server.nowMs);
	bob.team.setLink(&bobLink);

	// Anna's editor: the panel owns the bridge and pumps it on one clock with
	// the server's. One pump before the first edit puts the member name on it.
	TaskBoardPanel::setTeamClock([&] { return server.nowMs; });
	TaskBoardPanel::setTeam(&annaTeam);
	TaskBoardPanel::pumpTeam();
	REQUIRE(TaskBoardPanel::author() == "Anna");
	auto frame = [&](std::uint64_t ms) {
		server.pump(ms, { &bob.team });
		TaskBoardPanel::pumpTeam();
	};

	// What a script reads first: the team, and who is online.
	const ToolResult l0 = anna.call("task_list");
	REQUIRE_FALSE(l0.isError);
	CHECK(l0.content["team"]["connected"] == true);
	CHECK(l0.content["team"]["self"] == "Anna");
	REQUIRE(l0.content["team"]["members"].size() == 2);
	CHECK(l0.content["team"]["members"][1]["name"] == "Bob");
	CHECK(l0.content["team"]["members"][1]["online"] == true);

	const ToolResult c = anna.call("task_create", json{ { "title", "Retexture the crates" } });
	REQUIRE_FALSE(c.isError);
	const std::string id = c.content["task"]["id"].get<std::string>();
	CHECK(c.content["task"]["createdBy"] == "Anna");

	// Off the roster: refused, nothing written, nothing queued.
	const ToolResult bad = anna.call("task_assign", json{ { "id", id }, { "assignees", json::array({ "Carol" }) } });
	CHECK(bad.errorCode == "unknown_member");
	CHECK(bad.errorMessage.find("Bob") != std::string::npos);
	CHECK(onDisk(proj.root(), id).assignees.empty());
	CHECK(annaTeam.pending() == 0);

	// "bob" is Bob: the roster's spelling goes on the task.
	const ToolResult as = anna.call("task_assign", json{ { "id", id }, { "assignees", json::array({ "bob", "Bob" }) } });
	REQUIRE_FALSE(as.isError);
	CHECK(as.content["task"]["assignees"] == json::array({ "Bob" }));
	CHECK(as.content["task"]["assignedBy"] == "Anna");
	CHECK(as.content["team"]["pending"] == 1);
	{
		const HE::Task t = onDisk(proj.root(), id);
		CHECK(t.assignees == std::vector<std::string>{ "Bob" });
		CHECK(t.assignedBy == "Anna");
	}

	// Nothing yet on Bob's side: the assignment settles first.
	frame(server.latencyMs);
	CHECK(bob.call("task_notices").content["notices"].empty());

	// Settled, sent by Anna's panel pump, carried, read by Bob's bridge.
	frame(HE::TaskBoardTeam::kAssignSettleMs);
	CHECK(annaLink.sent.size() == 1);
	frame(server.latencyMs);
	const ToolResult n = bob.call("task_notices");
	REQUIRE_FALSE(n.isError);
	CHECK(n.content["connected"] == true);
	REQUIRE(n.content["notices"].size() == 1);
	const json& got = n.content["notices"][0];
	CHECK(got["text"] == "Anna assigned you “Retexture the crates”");
	CHECK(got["kind"] == "assigned");
	CHECK(got["from"] == "Anna");
	CHECK(got["task"] == id);
	CHECK(got["assignees"] == json::array({ "Bob" }));
	REQUIRE(bob.bell.snapshot().size() == 1);
	CHECK(bob.bell.snapshot()[0].text == "Anna assigned you “Retexture the crates”");
	// Once: the next frames bring nothing new.
	frame(HE::TaskBoardTeam::kAssignSettleMs);
	CHECK(bob.call("task_notices").content["notices"].size() == 1);
	CHECK(bobLink.received == 1);

	// A notice is not a sync: Bob's board has the task with the pull.
	CHECK(bob.call("task_list").content["tasks"].empty());
	bob.pull(proj.root());
	const ToolResult mine = bob.call("task_list", json{ { "assignee", "Bob" } });
	REQUIRE(mine.content["tasks"].size() == 1);
	CHECK(mine.content["tasks"][0]["id"] == id);
	CHECK(mine.content["openMine"] == 1);

	// Bob takes it on; Anna — creator and assigner — hears it.
	const ToolResult u = bob.call("task_update", json{ { "id", id }, { "status", "in-progress" }, { "comment", "On it" } });
	REQUIRE_FALSE(u.isError);
	CHECK(u.content["task"]["status"] == "in-progress");
	CHECK(u.content["team"]["pending"] == 1);
	frame(0);
	frame(server.latencyMs);
	const ToolResult back = anna.call("task_notices");
	REQUIRE(back.content["notices"].size() == 1);
	CHECK(back.content["notices"][0]["text"] == "Bob set “Retexture the crates” to In Progress");
	CHECK(back.content["notices"][0]["status"] == "in-progress");
	CHECK(back.content["notices"][0]["comment"] == "On it");
	REQUIRE(annaBell.snapshot().size() == 1);
	// Bob does not hear his own update.
	CHECK(bob.call("task_notices").content["notices"].size() == 1);

	// Signed out: the board is local again, and assigning says so.
	annaLink.signedIn = false;
	TaskBoardPanel::pumpTeam();
	CHECK(anna.call("task_assign", json{ { "id", id }, { "assignees", json::array() } }).errorCode == "no_team");
	CHECK(anna.call("task_list").content["team"]["connected"] == false);
}
