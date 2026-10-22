// The Planner as tools (McpToolsPlanner.cpp): what a script or a model drives to read and
// write databases, rows and pages without clicking. The tools stand on a real
// HE::Planner::Workspace over a project folder — the editor's own path, minus the tab — so
// what they promise is checked where it matters: the files. The questions: does every change
// land in Config/Planner (and in the Task Board for "Tasks"), does a value that does not fit
// refuse the whole change and say which, do names and ids both find things, and does a page
// survive being written as Markdown and read back.
#include "doctest.h"
#include "TestFsUtil.h"

#include "McpToolHooks.h"
#include "McpToolRegistry.h"
#include "Planner.h"
#include "PlannerBoard.h"
#include "PlannerImport.h"
#include "PlannerTasks.h"
#include "PlannerWorkspace.h"
#include "TaskBoard.h"

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace HE;
using namespace HE::Planner;
using HE::Ed::McpPlannerHooks;
using HE::Ed::McpToolRegistry;
using HE::Ed::ToolResult;
using nlohmann::json;

namespace
{

struct TempProject
{
	fs::path root;
	TempProject()
	{
		root = fs::temp_directory_path() / ("he_test_mcp_planner_" + uuidToHex(UUID::generate()));
		fs::create_directories(root);
	}
	~TempProject() { he_test::removeAllQuiet(root); }
};

// The editor's tools over a workspace.
struct Tools
{
	Workspace       ws;
	McpToolRegistry registry;
	bool            open = true;

	explicit Tools(const fs::path& root)
	{
		ws.setProject(root);
		McpPlannerHooks h;
		h.workspace = [this] { return open ? &ws : nullptr; };
		h.author    = [] { return std::string("Claude"); };
		HE::Ed::registerPlannerTools(registry, std::move(h));
	}

	ToolResult call(const char* name, const json& args = json::object())
	{
		const auto* t = registry.find(name);
		REQUIRE_MESSAGE(t != nullptr, name);
		return t->handler(args);
	}
	json ok(const char* name, const json& args = json::object())
	{
		const ToolResult r = call(name, args);
		REQUIRE_MESSAGE(!r.isError, name << ": " << r.errorCode << " " << r.errorMessage);
		return r.content;
	}
};

// A board in memory with a change counter, standing in for TaskBoardPanel (as in
// test_planner_workspace.cpp): "Tasks" reads from it and writes to it.
struct FakeBoard
{
	TaskBoard board;
	unsigned  gen = 1;

	Workspace::External external()
	{
		Workspace::External e;
		e.id   = tasksDatabaseId();
		e.name = "Tasks";
		e.refresh    = [this](Database& db) { refreshTasks(db, board); };
		e.generation = [this] { return gen; };
		e.apply = [this](const Database& before, const Database& after, std::string& err) {
			bool changed = false;
			const bool ok = applyTasksEdit(before, after, board, "Claude", taskNowMs(), changed, err);
			if (changed) ++gen;
			return ok;
		};
		return e;
	}
};

} // namespace

TEST_CASE("planner tools: registered with schemas; readers do not mutate; no project refuses")
{
	TempProject tp;
	Tools t(tp.root);
	const char* const readers[] = { "planner_overview", "planner_read_database", "planner_read_page", "planner_read_board" };
	const char* const writers[] = { "planner_add_row", "planner_update_row", "planner_delete_row", "planner_create_database",
	                                "planner_add_property", "planner_write_page", "planner_delete_page", "planner_create_board",
	                                "planner_add_board_card", "planner_update_board_card", "planner_delete_board_card",
	                                "planner_connect_board_cards", "planner_delete_board_arrow" };
	for (const char* name : readers)
	{
		const auto* tool = t.registry.find(name);
		REQUIRE_MESSAGE(tool != nullptr, name);
		CHECK(tool->inputSchema.value("type", std::string()) == "object");
		CHECK_FALSE(tool->description.empty());
		CHECK_FALSE(tool->mutates);
	}
	for (const char* name : writers)
	{
		const auto* tool = t.registry.find(name);
		REQUIRE_MESSAGE(tool != nullptr, name);
		CHECK(tool->inputSchema.value("type", std::string()) == "object");
		CHECK(tool->mutates);
	}
	t.open = false;
	for (const char* name : { "planner_overview", "planner_read_database", "planner_add_row", "planner_read_page", "planner_write_page" })
		CHECK(t.call(name, json{ { "database", "x" }, { "title", "x" }, { "page", "x" } }).errorCode == "no_project");
}

TEST_CASE("planner tools: a database with typed properties, rows with values, all in the files")
{
	TempProject tp;
	Tools t(tp.root);

	const json created = t.ok("planner_create_database", json{
		{ "name", "Features" },
		{ "properties", json::array({
			json{ { "name", "Status" }, { "type", "select" }, { "options", { "Open", "Done" } } },
			json{ { "name", "Points" }, { "type", "number" } },
			json{ { "name", "Due" }, { "type", "date" } },
			json{ { "name", "Owner" }, { "type", "person" } },
			json{ { "name", "Tags" }, { "type", "multi-select" } },
			json{ { "name", "Shipped" }, { "type", "checkbox" } },
			json{ { "name", "Blocked by" }, { "type", "relation" } },
			json{ { "name", "Links" }, { "type", "engine-link" } },
		}) },
	});
	const std::string dbId = created["database"]["id"];
	CHECK(created["database"]["properties"].size() == 8);
	CHECK(created["database"]["views"].size() == 1);
	CHECK(fs::exists(databasePath(tp.root, [&] { UUID id; uuidFromHex(dbId, id); return id; }())));

	// Add rows by names; "Blocked by" points at another row by its title.
	const json a = t.ok("planner_add_row", json{
		{ "database", "features" },   // by name, any case
		{ "title", "Harbour level" },
		{ "values", json{
			{ "Status", "Open" }, { "Points", 5 }, { "Due", "2026-11-01" }, { "Owner", "Anna, Bob" },
			{ "Tags", { "Level", "Art" } },   // options that do not exist yet are made
			{ "Shipped", false },
			{ "Links", json::array({ json{ { "kind", "scene" }, { "scene", "Content/Levels/Harbour.hescene" }, { "label", "Harbour" } },
			                         json{ { "kind", "panel" }, { "panel", "Console" } } }) },
		} },
	});
	CHECK(a["row"]["title"] == "Harbour level");
	CHECK(a["row"]["values"]["Status"] == "Open");
	CHECK(a["row"]["values"]["Points"] == 5);
	CHECK(a["row"]["values"]["Due"] == "2026-11-01");
	CHECK(a["row"]["values"]["Owner"] == json::array({ "Anna", "Bob" }));
	CHECK(a["row"]["values"]["Tags"] == json::array({ "Level", "Art" }));
	CHECK(a["row"]["values"]["Links"].size() == 2);
	CHECK(a["row"]["values"]["Links"][0]["scene"] == "Content/Levels/Harbour.hescene");
	CHECK(a["row"]["createdBy"] == "Claude");
	const std::string aId = a["row"]["id"];

	const json b = t.ok("planner_add_row", json{
		{ "database", dbId }, { "title", "Climbing" },
		{ "values", json{ { "Status", "Done" }, { "Blocked by", { "Harbour level" } } } } });
	CHECK(b["row"]["values"]["Blocked by"][0]["id"] == aId);
	CHECK(b["row"]["values"]["Blocked by"][0]["title"] == "Harbour level");

	// What the tools wrote is what a new session loads.
	{
		Workspace again;
		again.setProject(tp.root);
		REQUIRE(again.databases().size() == 1);
		const Database* db = again.databases()[0];
		CHECK(db->rows.size() == 2);
		const Property* tags = db->findPropertyByName("Tags");
		REQUIRE(tags);
		CHECK(tags->options.size() == 2);
	}

	// Reading: a view's filters apply, search narrows, limit and offset page.
	const json all = t.ok("planner_read_database", json{ { "database", "Features" } });
	CHECK(all["total"] == 2);
	CHECK(all["rows"].size() == 2);
	CHECK(all["database"]["rowCount"] == 2);
	const json limited = t.ok("planner_read_database", json{ { "database", "Features" }, { "limit", 1 }, { "offset", 1 } });
	CHECK(limited["rows"].size() == 1);
	CHECK(limited["rows"][0]["title"] == "Climbing");
	CHECK(limited["total"] == 2);
	const json found = t.ok("planner_read_database", json{ { "database", "Features" }, { "search", "HARBOUR" } });
	CHECK(found["total"] == 1);
	CHECK(t.call("planner_read_database", json{ { "database", "Nope" } }).errorCode == "not_found");
	CHECK(t.call("planner_read_database", json{ { "database", "Features" }, { "view", "Nope" } }).errorCode == "not_found");

	// A view with a filter, made in the editor's own model.
	{
		UUID id;
		uuidFromHex(dbId, id);
		const Database* db = t.ws.find(id);
		const UUID status = db->findPropertyByName("Status")->id;
		REQUIRE(t.ws.edit(id, [&](Database& d) {
			View* v = d.addView("Open only", ViewKind::Table);
			v->filters = { { status, "equals", "\"Open\"" } };
			return true;
		}));
	}
	const json open = t.ok("planner_read_database", json{ { "database", "Features" }, { "view", "open only" } });
	CHECK(open["total"] == 1);
	CHECK(open["rows"][0]["title"] == "Harbour level");

	// The overview lists it all.
	const json overview = t.ok("planner_overview");
	bool sawFeatures = false;
	for (const json& d : overview["databases"])
		if (d["name"] == "Features")
		{
			sawFeatures = true;
			CHECK(d["rowCount"] == 2);
			CHECK(d["views"].size() == 2);
		}
	CHECK(sawFeatures);
}

TEST_CASE("planner tools: a value that does not fit refuses the whole change and says which")
{
	TempProject tp;
	Tools t(tp.root);
	t.ok("planner_create_database", json{ { "name", "Bugs" }, { "properties", json::array({
		json{ { "name", "Severity" }, { "type", "select" }, { "options", { "Low", "High" } } },
		json{ { "name", "Score" }, { "type", "number" } },
		json{ { "name", "Found" }, { "type", "date" } } }) } });

	const ToolResult bad = t.call("planner_add_row", json{ { "database", "Bugs" }, { "title", "Crash" },
		{ "values", json{ { "Score", "lots" }, { "Found", "yesterday" }, { "Colour", "red" }, { "Severity", "Medium" } } },
		{ "createOptions", false } });
	CHECK(bad.errorCode == "invalid_payload");
	const json errors = json::parse(bad.errorMessage);
	CHECK(errors.size() == 4);
	CHECK(bad.errorMessage.find("Score") != std::string::npos);
	CHECK(bad.errorMessage.find("Colour") != std::string::npos);
	CHECK(bad.errorMessage.find("Medium") != std::string::npos);
	// Nothing was written, not even the title.
	CHECK(t.ok("planner_read_database", json{ { "database", "Bugs" } })["total"] == 0);

	// A numeric string is a number; createOptions (default) makes the option.
	const json fine = t.ok("planner_add_row", json{ { "database", "Bugs" }, { "title", "Crash" },
		{ "values", json{ { "Score", "7.5" }, { "Severity", "Medium" } } } });
	CHECK(fine["row"]["values"]["Score"] == 7.5);
	CHECK(fine["row"]["values"]["Severity"] == "Medium");
	CHECK(t.call("planner_add_row", json{ { "database", "Bugs" } }).isError == false);   // title is required by the schema; an empty one is a row
	CHECK(t.call("planner_add_row", json{ { "database", "Bugs" }, { "title", "x" }, { "values", 5 } }).errorCode == "invalid_payload");
}

TEST_CASE("planner tools: update changes a cell, clears one with null, renames; delete takes the row and its page")
{
	TempProject tp;
	Tools t(tp.root);
	t.ok("planner_create_database", json{ { "name", "Features" }, { "properties", json::array({
		json{ { "name", "Status" }, { "type", "select" }, { "options", { "Open", "Done" } } },
		json{ { "name", "Points" }, { "type", "number" } } }) } });
	const json row = t.ok("planner_add_row", json{ { "database", "Features" }, { "title", "Harbour" },
		{ "values", json{ { "Status", "Open" }, { "Points", 3 } } } });
	const std::string rowId = row["row"]["id"];

	// By title, then by id; null clears; a key that is left out is not touched.
	json r = t.ok("planner_update_row", json{ { "database", "Features" }, { "row", "Harbour" },
		{ "values", json{ { "Status", "Done" }, { "Points", nullptr } } } });
	CHECK(r["row"]["values"]["Status"] == "Done");
	CHECK_FALSE(r["row"]["values"].contains("Points"));
	r = t.ok("planner_update_row", json{ { "database", "Features" }, { "row", rowId }, { "title", "Harbour level" } });
	CHECK(r["row"]["title"] == "Harbour level");
	CHECK(r["row"]["values"]["Status"] == "Done");
	CHECK(t.call("planner_update_row", json{ { "database", "Features" }, { "row", rowId } }).errorCode == "invalid_payload");
	CHECK(t.call("planner_update_row", json{ { "database", "Features" }, { "row", "Nope" }, { "title", "x" } }).errorCode == "not_found");
	CHECK(t.call("planner_update_row", json{ { "database", "Features" }, { "row", rowId }, { "values", json{ { "Nope", 1 } } } }).errorCode == "invalid_payload");

	// A row's page, then both go.
	t.ok("planner_write_page", json{ { "database", "Features" }, { "row", rowId }, { "markdown", "Foggy harbour.\n" } });
	UUID rid;
	uuidFromHex(rowId, rid);
	CHECK(fs::exists(pagePath(tp.root, rid)));
	const json d = t.ok("planner_delete_row", json{ { "database", "Features" }, { "row", "Harbour level" } });
	CHECK(d["deleted"] == rowId);
	CHECK(t.ok("planner_read_database", json{ { "database", "Features" } })["total"] == 0);
	CHECK_FALSE(fs::exists(pagePath(tp.root, rid)));
	CHECK(t.call("planner_delete_row", json{ { "database", "Features" }, { "row", rowId } }).errorCode == "not_found");

	// A property added later; two of one name are refused.
	const json p = t.ok("planner_add_property", json{ { "database", "Features" }, { "name", "Phase" }, { "type", "select" },
	                                                  { "options", { "Alpha", "Beta" } } });
	CHECK(p["property"]["options"].size() == 2);
	CHECK(t.call("planner_add_property", json{ { "database", "Features" }, { "name", "phase" }, { "type", "text" } }).errorCode == "exists");
	CHECK(t.call("planner_add_property", json{ { "database", "Features" }, { "name", "X" }, { "type", "sparkle" } }).errorCode == "invalid_payload");
}

TEST_CASE("planner tools: pages are written as Markdown and read back; sub-pages, links, append, rename, delete")
{
	TempProject tp;
	Tools t(tp.root);
	t.ok("planner_create_database", json{ { "name", "Features" } });

	const json made = t.ok("planner_write_page", json{ { "title", "Game design" }, { "markdown",
		"# Pillars\n"
		"- Exploration\n"
		"  - Climbing\n"
		"- [ ] Ship alpha\n"
		"```cpp\n"
		"int x;\n"
		"```\n"
		"{{database: Features}}\n" } });
	const std::string pageId = made["page"]["id"];
	CHECK(made["page"]["title"] == "Game design");
	CHECK(made["page"]["blockCount"] == 5);
	UUID pid;
	uuidFromHex(pageId, pid);
	CHECK(fs::exists(pagePath(tp.root, pid)));

	// Read by id and by title; the Markdown is what was written, the blocks are the tree.
	json read = t.ok("planner_read_page", json{ { "page", "game design" } });
	CHECK(read["page"]["id"] == pageId);
	CHECK(read["page"]["markdown"] ==
	      "# Pillars\n"
	      "- Exploration\n"
	      "  - Climbing\n"
	      "- [ ] Ship alpha\n"
	      "```cpp\n"
	      "int x;\n"
	      "```\n"
	      "{{database: Features}}\n");
	CHECK(read["page"]["blocks"][1]["children"].size() == 1);
	CHECK(read["page"]["blocks"][4]["type"] == "database-view");

	// A sub-page under it, linked by a [[Title]] line; append keeps what is there.
	const json sub = t.ok("planner_write_page", json{ { "title", "Level notes" }, { "parent", "Game design" }, { "markdown", "Harbour first.\n" } });
	CHECK(sub["page"]["parent"] == pageId);
	t.ok("planner_write_page", json{ { "page", pageId }, { "mode", "append" }, { "markdown", "[[Level notes]]\n" } });
	read = t.ok("planner_read_page", json{ { "page", pageId } });
	CHECK(read["page"]["blockCount"] == 6);
	CHECK(read["page"]["blocks"][5]["type"] == "page-link");
	CHECK(read["page"]["markdown"].get<std::string>().find("[[Level notes]]") != std::string::npos);

	// Replace swaps everything; a title on an existing page renames it.
	t.ok("planner_write_page", json{ { "page", pageId }, { "markdown", "Fresh start.\n" }, { "title", "Design" } });
	read = t.ok("planner_read_page", json{ { "page", pageId } });
	CHECK(read["page"]["title"] == "Design");
	CHECK(read["page"]["markdown"] == "Fresh start.\n");

	// The overview lists the pages; what is on disk is what a new session sees.
	const json overview = t.ok("planner_overview");
	CHECK(overview["pages"].size() == 2);
	{
		Workspace again;
		again.setProject(tp.root);
		REQUIRE(again.findPage(pid));
		CHECK(again.findPage(pid)->title == "Design");
	}

	// Errors: nothing named, nothing found, two pages of one title, a row's page deleted.
	CHECK(t.call("planner_read_page", json::object()).errorCode == "invalid_payload");
	CHECK(t.call("planner_read_page", json{ { "page", "Nope" } }).errorCode == "not_found");
	CHECK(t.call("planner_write_page", json::object()).errorCode == "invalid_payload");
	t.ok("planner_write_page", json{ { "title", "Twin" } });
	t.ok("planner_write_page", json{ { "title", "twin" } });
	CHECK(t.call("planner_read_page", json{ { "page", "TWIN" } }).errorCode == "ambiguous");

	// Deleting takes the sub-page with it.
	const json del = t.ok("planner_delete_page", json{ { "page", pageId } });
	CHECK(del["deleted"] == pageId);
	CHECK(t.call("planner_read_page", json{ { "page", pageId } }).errorCode == "not_found");
	CHECK(t.call("planner_read_page", json{ { "page", "Level notes" } }).errorCode == "not_found");
	CHECK_FALSE(fs::exists(pagePath(tp.root, pid)));
}

TEST_CASE("planner tools: a row's page belongs to the row")
{
	TempProject tp;
	Tools t(tp.root);
	t.ok("planner_create_database", json{ { "name", "Features" } });
	const json row = t.ok("planner_add_row", json{ { "database", "Features" }, { "title", "Harbour" } });
	const std::string rowId = row["row"]["id"];
	UUID rid;
	uuidFromHex(rowId, rid);

	// Reading a row's page before anything is written: empty, and no file.
	json read = t.ok("planner_read_page", json{ { "database", "Features" }, { "row", "Harbour" } });
	CHECK(read["page"]["blockCount"] == 0);
	CHECK(read["page"]["rowOf"]["row"] == rowId);
	CHECK_FALSE(fs::exists(pagePath(tp.root, rid)));

	// Writing it, and renaming through it renames the row.
	t.ok("planner_write_page", json{ { "database", "Features" }, { "row", rowId }, { "markdown", "- Fog\n" }, { "title", "Harbour level" } });
	CHECK(fs::exists(pagePath(tp.root, rid)));
	CHECK(t.ok("planner_read_database", json{ { "database", "Features" } })["rows"][0]["title"] == "Harbour level");
	CHECK(t.ok("planner_read_database", json{ { "database", "Features" } })["rows"][0]["pageBlocks"] == 1);

	// It is not a page to delete on its own.
	CHECK(t.call("planner_delete_page", json{ { "page", rowId } }).errorCode == "refused");
	// The overview says what it belongs to.
	bool listed = false;
	const json overview = t.ok("planner_overview");
	for (const json& p : overview["pages"])
		if (p["id"] == rowId) { listed = true; CHECK(p["rowOf"]["databaseName"] == "Features"); }
	CHECK(listed);
}

TEST_CASE("planner tools: Tasks is the Task Board — rows are tasks, status changes land in the history")
{
	TempProject tp;
	FakeBoard source;
	Tools t(tp.root);
	t.ws.setExternal(source.external());

	// The overview has it, its columns are fixed.
	bool sawTasks = false;
	const json overview = t.ok("planner_overview");
	for (const json& d : overview["databases"])
		if (d["name"] == "Tasks") sawTasks = true;
	CHECK(sawTasks);
	CHECK(t.call("planner_add_property", json{ { "database", "Tasks" }, { "name", "Estimate" }, { "type", "number" } }).isError);

	// A new row is a new task, with its status and people.
	const json row = t.ok("planner_add_row", json{ { "database", "Tasks" }, { "title", "Retexture the crates" },
		{ "values", json{ { "Status", "Blocked" }, { "Priority", "High" }, { "Assignees", "Anna" }, { "Description", "Rust and dirt" } } } });
	REQUIRE(source.board.tasks.size() == 1);
	const Task& task = source.board.tasks[0];
	CHECK(task.title == "Retexture the crates");
	CHECK(task.status == TaskStatus::Blocked);
	CHECK(task.priority == TaskPriority::High);
	CHECK(task.assignees == std::vector<std::string>{ "Anna" });
	CHECK(task.description == "Rust and dirt");
	CHECK(task.createdBy == "Claude");
	CHECK(row["row"]["id"] == uuidToHex(task.id));

	// Done: a status update in the history, which is what the team bridge announces.
	const std::size_t before = task.updates.size();
	t.ok("planner_update_row", json{ { "database", "Tasks" }, { "row", "Retexture the crates" }, { "values", json{ { "Status", "Done" } } } });
	CHECK(source.board.tasks[0].status == TaskStatus::Done);
	REQUIRE(source.board.tasks[0].updates.size() == before + 1);
	CHECK(source.board.tasks[0].updates.back().kind == TaskUpdateKind::Status);

	// A title change is no story: nothing is added to the history.
	t.ok("planner_update_row", json{ { "database", "Tasks" }, { "row", "Retexture the crates" }, { "title", "Retexture all crates" } });
	CHECK(source.board.tasks[0].title == "Retexture all crates");
	CHECK(source.board.tasks[0].updates.size() == before + 1);

	// A status word the board does not know is kept as it was; deleting deletes the task.
	t.ok("planner_update_row", json{ { "database", "Tasks" }, { "row", "Retexture all crates" }, { "values", json{ { "Status", "needs-art" } } } });
	CHECK(source.board.tasks[0].statusOther == "needs-art");
	t.ok("planner_delete_row", json{ { "database", "Tasks" }, { "row", "Retexture all crates" } });
	CHECK(source.board.tasks.empty());
}

TEST_CASE("planner tools: a whiteboard is made, filled with cards and arrows, changed and read back, all in its file")
{
	TempProject tp;
	Tools t(tp.root);
	const json made = t.ok("planner_create_board", json{ { "title", "Level 1" } });
	const std::string boardId = made["id"];
	CHECK(t.ok("planner_overview")["whiteboards"].size() == 1);

	// A page and a row to put on it.
	const json page = t.ok("planner_write_page", json{ { "title", "Design" }, { "markdown", "hello" } });
	const json db = t.ok("planner_create_database", json{ { "name", "Quests" } });
	const json row = t.ok("planner_add_row", json{ { "database", "Quests" }, { "title", "Find the key" } });

	const json a = t.ok("planner_add_board_card", json{ { "board", "Level 1" }, { "text", "Start" }, { "x", 10 }, { "y", 20 }, { "color", "Yellow" } });
	const json b = t.ok("planner_add_board_card", json{ { "board", boardId }, { "text", "End" }, { "x", 400 }, { "y", 20 } });
	const json frame = t.ok("planner_add_board_card", json{ { "board", boardId }, { "type", "frame" }, { "text", "Act 1" }, { "w", 600 }, { "h", 300 } });
	const json pc = t.ok("planner_add_board_card", json{ { "board", boardId }, { "type", "page" }, { "page", "Design" }, { "x", 0 }, { "y", 200 } });
	const json rc = t.ok("planner_add_board_card", json{ { "board", boardId }, { "type", "row" }, { "database", "Quests" }, { "row", "find the key" } });
	CHECK(a["color"] == "yellow");
	CHECK(pc["page"]["title"] == "Design");
	CHECK(rc["row"]["title"] == "Find the key");
	CHECK(frame["type"] == "frame");
	(void)page; (void)db; (void)row;

	const json arrow = t.ok("planner_connect_board_cards", json{ { "board", boardId }, { "from", a["id"] }, { "to", b["id"] }, { "label", "then" } });
	CHECK(arrow["arrow"] == true);
	// The same two again, and an arrow to itself, are refused.
	CHECK(t.call("planner_connect_board_cards", json{ { "board", boardId }, { "from", a["id"] }, { "to", b["id"] } }).isError);
	CHECK(t.call("planner_connect_board_cards", json{ { "board", boardId }, { "from", a["id"] }, { "to", a["id"] } }).isError);

	t.ok("planner_update_board_card", json{ { "board", boardId }, { "card", a["id"] }, { "text", "Begin" }, { "x", 50 }, { "color", "" } });
	const json read = t.ok("planner_read_board", json{ { "board", "Level 1" } });
	REQUIRE(read["nodes"].size() == 5);
	REQUIRE(read["edges"].size() == 1);
	bool found = false;
	for (const json& n : read["nodes"])
		if (n["id"] == a["id"])
		{
			found = true;
			CHECK(n["text"] == "Begin");
			CHECK(n["x"] == 50);
			CHECK(n["y"] == 20);
			CHECK_FALSE(n.contains("color"));
		}
	CHECK(found);

	// What the file holds is what a teammate pulls.
	{
		Board onDisk;
		UUID id;
		REQUIRE(uuidFromHex(boardId, id));
		REQUIRE(loadBoard(tp.root, id, onDisk).ok);
		CHECK(onDisk.nodes.size() == 5);
		CHECK(onDisk.edges.size() == 1);
	}

	t.ok("planner_delete_board_arrow", json{ { "board", boardId }, { "arrow", arrow["id"] } });
	CHECK(t.ok("planner_read_board", json{ { "board", boardId } })["edges"].empty());
	t.ok("planner_connect_board_cards", json{ { "board", boardId }, { "from", a["id"] }, { "to", b["id"] } });
	// Deleting a card takes its arrows along.
	t.ok("planner_delete_board_card", json{ { "board", boardId }, { "card", a["id"] } });
	const json after = t.ok("planner_read_board", json{ { "board", boardId } });
	CHECK(after["nodes"].size() == 4);
	CHECK(after["edges"].empty());

	// Refusals say what was wrong.
	CHECK(t.call("planner_read_board", json{ { "board", "nope" } }).errorCode == "not_found");
	CHECK(t.call("planner_add_board_card", json{ { "board", boardId }, { "type", "image" } }).errorCode == "invalid_payload");
	CHECK(t.call("planner_add_board_card", json{ { "board", boardId }, { "type", "page" }, { "page", "missing" } }).isError);
	CHECK(t.call("planner_update_board_card", json{ { "board", boardId }, { "card", a["id"] } }).errorCode == "not_found");
	CHECK(t.call("planner_create_board", json{ { "title", "" } }).errorCode == "invalid_payload");
}
