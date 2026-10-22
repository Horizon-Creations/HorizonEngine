// The Planner's workspace (the open project's databases, one writer for their
// files) and the import of the Task Board into the database "Tasks". ImGui-free:
// the tab on top is covered by test_planner_ui.cpp.
#include "doctest.h"
#include "TestFsUtil.h"

#include "PlannerImport.h"
#include "PlannerWorkspace.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace HE;
using namespace HE::Planner;

namespace
{

struct TempProject
{
	fs::path root;
	TempProject()
	{
		root = fs::temp_directory_path() / ("he_test_planner_ws_" + uuidToHex(UUID::generate()));
		fs::create_directories(root);
	}
	~TempProject() { he_test::removeAllQuiet(root); }
};

void writeBytes(const fs::path& p, const std::string& text)
{
	fs::create_directories(p.parent_path());
	std::ofstream out(p, std::ios::binary | std::ios::trunc);
	out << text;
}

constexpr std::int64_t kT0 = 1760000000000;

std::string readFileText(const fs::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

TaskBoard sampleBoard()
{
	TaskBoard b;
	Task* a = b.add("Retexture the crates", "Anna", kT0);
	const UUID aId = a->id;
	b.setPriority(aId, TaskPriority::High, kT0 + 10);
	b.setDescription(aId, "Rust and dirt", kT0 + 20);
	b.assign(aId, { "Bob", "Anna" }, "Anna", kT0 + 30);
	b.addLink(aId, TaskLink::sceneLink("Content/Levels/Harbour.hescene", "Harbour"), kT0 + 40);
	Task* c = b.add("Fix the bridge", "Bob", kT0 + 100);
	const UUID cId = c->id;
	b.setStatus(cId, TaskStatus::Blocked, "Bob", kT0 + 110);
	Task* d = b.add("Odd one", "Bob", kT0 + 200);
	d->status      = TaskStatus::Other;
	d->statusOther = "waiting-for-art";
	d->modifiedMs  = kT0 + 210;
	return b;
}

} // namespace

TEST_CASE("planner import: derived ids are the same everywhere")
{
	CHECK(derivedId("property", "Status") == derivedId("property", "Status"));
	CHECK(derivedId("property", "Status") != derivedId("view", "Status"));
	CHECK(derivedId("property", "Status") != derivedId("property", "Priority"));
	CHECK(tasksDatabaseId() == derivedId("database", "Tasks"));

	Database a, b;
	ensureTasksSchema(a);
	ensureTasksSchema(b);
	CHECK(a.toJson() == b.toJson());   // two machines, one file
	CHECK(a.id == tasksDatabaseId());
	CHECK(a.views.size() == 2);
	CHECK(a.views[1].kind == ViewKind::Board);
	CHECK(a.views[1].groupBy == derivedId("property", "Status"));

	// Running it again changes nothing and removes nothing the user added.
	a.addProperty("Estimate", PropertyType::Number);
	const std::string before = a.toJson();
	ensureTasksSchema(a);
	CHECK(a.toJson() == before);
}

TEST_CASE("planner import: tasks become rows with their status, people and links")
{
	const TaskBoard board = sampleBoard();
	Database db;
	const ImportResult r = importTaskBoard(board, db);
	CHECK(r.added == 3);
	CHECK(r.updated == 0);
	REQUIRE(db.rows.size() == 3);

	const UUID status = derivedId("property", "Status");
	const UUID priority = derivedId("property", "Priority");
	const Task& crates = board.tasks[0];
	const Row* row = db.findRow(crates.id);
	REQUIRE(row);
	CHECK(row->title == "Retexture the crates");
	CHECK(row->createdBy == "Anna");
	CHECK(row->createdMs == crates.createdMs);
	CHECK(db.findProperty(status)->findOption(db.select(crates.id, status))->name == "Open");
	CHECK(db.findProperty(priority)->findOption(db.select(crates.id, priority))->name == "High");
	CHECK(db.people(crates.id, derivedId("property", "Assignees")) == std::vector<std::string>{ "Bob", "Anna" });
	CHECK(db.text(crates.id, derivedId("property", "Description")) == "Rust and dirt");
	const auto links = db.links(crates.id, derivedId("property", "Links"));
	REQUIRE(links.size() == 1);
	CHECK(links[0].label == "Harbour");

	CHECK(db.findProperty(status)->findOption(db.select(board.tasks[1].id, status))->name == "Blocked");
	// A status word this build does not know becomes an option of its own.
	const SelectOption* odd = db.findProperty(status)->findOption(db.select(board.tasks[2].id, status));
	REQUIRE(odd);
	CHECK(odd->name == "waiting-for-art");

	// The board's times go onto the rows, so the next import compares like with like.
	CHECK(row->modifiedMs == crates.modifiedMs);
}

TEST_CASE("planner import: again adds what is new and never beats a newer Planner edit")
{
	TaskBoard board = sampleBoard();
	Database db;
	importTaskBoard(board, db);

	ImportResult again = importTaskBoard(board, db);
	CHECK(again.added == 0);
	CHECK(again.updated == 0);
	CHECK(again.unchanged == 3);

	const UUID status = derivedId("property", "Status");
	const UUID done = db.findProperty(status)->findOptionByName("Done")->id;
	const UUID id0 = board.tasks[0].id;
	const UUID id1 = board.tasks[1].id;

	// Edited in the Planner at kT0 + 1000; the board has not moved since.
	REQUIRE(db.setSelect(id0, status, done, kT0 + 1000));
	REQUIRE(db.setTitle(id0, "Renamed in the Planner", kT0 + 1000));
	// The board moved on for the second task, and has a new third one.
	board.setTitle(id1, "Fix the bridge, properly", kT0 + 2000);
	board.add("Brand new", "Anna", kT0 + 3000);

	again = importTaskBoard(board, db);
	CHECK(again.added == 1);
	CHECK(again.updated == 1);
	CHECK(again.unchanged == 2);
	CHECK(db.findRow(id0)->title == "Renamed in the Planner");
	CHECK(db.select(id0, status) == done);
	CHECK(db.findRow(id1)->title == "Fix the bridge, properly");
	CHECK(db.rows.size() == 4);
}

TEST_CASE("planner workspace: create, edit, reload from disk")
{
	TempProject tp;
	Workspace ws;
	ws.setProject(tp.root);
	CHECK(ws.databases().empty());

	Database* bugs = ws.create("Bugs");
	REQUIRE(bugs);
	const UUID id = bugs->id;
	CHECK(fs::exists(databasePath(tp.root, id)));

	const unsigned gen = ws.generation();
	UUID rowId;
	CHECK(ws.edit(id, [&](Database& db) { rowId = db.addRow("Crash on load", "Anna", kT0)->id; return true; }));
	CHECK(ws.generation() != gen);
	CHECK_FALSE(ws.edit(id, [](Database&) { return false; }));   // nothing changed: nothing saved
	CHECK_FALSE(ws.edit(UUID::generate(), [](Database&) { return true; }));

	// A second workspace over the same project (another editor start) sees it.
	Workspace other;
	other.setProject(tp.root);
	REQUIRE(other.databases().size() == 1);
	REQUIRE(other.find(id));
	CHECK(other.find(id)->findRow(rowId)->title == "Crash on load");

	// Changing the project drops the old databases.
	TempProject empty;
	other.setProject(empty.root);
	CHECK(other.databases().empty());
	other.setProject({});
	CHECK(other.create("x") == nullptr);   // no project, no database
}

TEST_CASE("planner workspace: a pull under the open database is read, the edit lands on top")
{
	TempProject tp;
	Workspace ws;
	ws.setProject(tp.root);
	const UUID id = ws.create("Bugs")->id;
	ws.edit(id, [&](Database& db) { db.addRow("Mine", "Anna", kT0); return true; });

	// A teammate's version arrives: same database with one more row.
	Workspace teammate;
	teammate.setProject(tp.root);
	REQUIRE(teammate.edit(id, [&](Database& db) { db.addRow("Theirs", "Bob", kT0 + 1); return true; }));

	// Our copy is stale; the edit finds the file changed, reloads, applies again.
	REQUIRE(ws.edit(id, [&](Database& db) { db.addRow("Mine too", "Anna", kT0 + 2); return true; }));
	std::vector<std::string> titles;
	for (const Row& r : ws.find(id)->rows) titles.push_back(r.title);
	CHECK(titles == std::vector<std::string>{ "Mine", "Theirs", "Mine too" });
	CHECK(ws.lastError().empty());
}

TEST_CASE("planner workspace: poll takes in new, changed and removed files")
{
	TempProject tp;
	Workspace ws;
	ws.setProject(tp.root);
	CHECK_FALSE(ws.poll(true));

	Workspace teammate;
	teammate.setProject(tp.root);
	const UUID id = teammate.create("From a pull")->id;
	CHECK(ws.poll(true));
	REQUIRE(ws.find(id));

	teammate.edit(id, [](Database& db) { db.name = "Renamed"; return true; });
	CHECK(ws.poll(true));
	CHECK(ws.find(id)->name == "Renamed");
	CHECK_FALSE(ws.poll(true));   // nothing new

	teammate.remove(id);
	CHECK(ws.poll(true));
	CHECK(ws.find(id) == nullptr);
}

TEST_CASE("planner workspace: an unreadable file is listed and never written over")
{
	TempProject tp;
	const UUID bad = UUID::generate();
	const std::string conflict = "<<<<<<< HEAD\n{}\n=======\n{}\n>>>>>>> x\n";
	writeBytes(databasePath(tp.root, bad), conflict);

	Workspace ws;
	ws.setProject(tp.root);
	CHECK(ws.databases().empty());
	REQUIRE(ws.unreadable().size() == 1);
	CHECK(ws.unreadable()[0].id == bad);
	CHECK(ws.unreadable()[0].error.find("merge-conflict") != std::string::npos);

	// ensure() of that very id must not make a fresh database over the damaged file.
	CHECK(ws.ensure(bad, "Tasks") == nullptr);
	CHECK_FALSE(ws.lastError().empty());
	std::ifstream in(databasePath(tp.root, bad), std::ios::binary);
	CHECK(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()) == conflict);

	// Fixed by hand: the next poll takes it in.
	writeBytes(databasePath(tp.root, bad), "{\"name\": \"Fixed\"}");
	CHECK(ws.poll(true));
	CHECK(ws.unreadable().empty());
	REQUIRE(ws.find(bad));
	CHECK(ws.find(bad)->name == "Fixed");
}

TEST_CASE("planner workspace: ensure creates the Tasks database once, and import fills it")
{
	TempProject tp;
	Workspace ws;
	ws.setProject(tp.root);
	const TaskBoard board = sampleBoard();

	Database* tasks = ws.ensure(tasksDatabaseId(), "Tasks", [](Database& db) { ensureTasksSchema(db); });
	REQUIRE(tasks);
	CHECK(tasks->views.size() == 2);   // from the schema, not an extra default table
	CHECK(ws.ensure(tasksDatabaseId(), "Tasks") == ws.find(tasksDatabaseId()));
	CHECK(ws.databases().size() == 1);

	ImportResult r;
	CHECK(ws.edit(tasksDatabaseId(), [&](Database& db) { r = importTaskBoard(board, db); return r.added + r.updated > 0; }));
	CHECK(r.added == 3);
	CHECK(ws.edit(tasksDatabaseId(), [&](Database& db) { r = importTaskBoard(board, db); return r.added + r.updated > 0; }) == false);

	Workspace again;
	again.setProject(tp.root);
	REQUIRE(again.find(tasksDatabaseId()));
	CHECK(again.find(tasksDatabaseId())->rows.size() == 3);
}

// ── The Tasks database as a projection of the Task Board (PlannerTasks.h) ────

#include "PlannerTasks.h"

namespace
{

// A board in memory with a change counter, standing in for TaskBoardPanel: the
// workspace reads rows from it and writes edits to it through two functions.
struct FakeSource
{
	TaskBoard board;
	unsigned  gen = 1;
	std::string author = "Tester";
	std::int64_t now = kT0 + 5000;
	bool refuse = false;

	Workspace::External external()
	{
		Workspace::External e;
		e.id   = tasksDatabaseId();
		e.name = "Tasks";
		e.refresh    = [this](Database& db) { refreshTasks(db, board); };
		e.generation = [this] { return gen; };
		e.apply = [this](const Database& before, const Database& after, std::string& err) {
			if (refuse) { err = "refused"; return false; }
			bool changed = false;
			const bool ok = applyTasksEdit(before, after, board, author, now++, changed, err);
			if (changed) ++gen;
			return ok;
		};
		return e;
	}
};

UUID prop(const char* name) { return derivedId("property", name); }
const Task* byTitle(const TaskBoard& b, const char* title)
{
	for (const Task& t : b.tasks) if (t.title == title) return &t;
	return nullptr;
}

} // namespace

TEST_CASE("planner tasks: the board is shown as rows, the schema is fixed")
{
	FakeSource src;
	src.board = sampleBoard();
	Database db;
	refreshTasks(db, src.board);
	CHECK(db.id == tasksDatabaseId());
	CHECK(db.rows.size() == 3);
	CHECK_FALSE(db.persistRows);

	// A column somebody added is not kept (there is nowhere to put its cells).
	db.addProperty("Estimate", PropertyType::Number);
	refreshTasks(db, src.board);
	CHECK(db.findPropertyByName("Estimate") == nullptr);
	CHECK(db.properties.size() == 5);

	// The file holds the schema and the views, never the rows.
	CHECK(db.toJson().find("Retexture the crates") == std::string::npos);
	CHECK(db.toJson().find("\"rows\": []") != std::string::npos);
}

TEST_CASE("planner tasks: edits become the board calls the team bridge listens to")
{
	FakeSource src;
	src.board = sampleBoard();
	const UUID crates = src.board.tasks[0].id;
	const std::size_t updatesBefore = src.board.tasks[0].updates.size();
	Database before;
	refreshTasks(before, src.board);

	auto edit = [&](const std::function<void(Database&)>& fn) {
		Database after = before;
		fn(after);
		bool changed = false;
		std::string err;
		REQUIRE(applyTasksEdit(before, after, src.board, "Anna", kT0 + 9000, changed, err));
		const bool did = changed;
		refreshTasks(before, src.board);
		return did;
	};

	// A new title is not a story: the history gets nothing, the task is stamped.
	CHECK(edit([&](Database& d) { d.setTitle(crates, "Retexture the wooden crates", kT0 + 9000); }));
	CHECK(src.board.find(crates)->title == "Retexture the wooden crates");
	CHECK(src.board.find(crates)->updates.size() == updatesBefore);
	CHECK(src.board.find(crates)->modifiedMs == kT0 + 9000);

	// A status change appends the update the bridge keys on, under the author.
	const Property* st = before.findProperty(prop("Status"));
	const UUID done = st->findOptionByName("Done")->id;
	CHECK(edit([&](Database& d) { d.setSelect(crates, prop("Status"), done, kT0 + 9000); }));
	const Task* t = src.board.find(crates);
	CHECK(t->status == TaskStatus::Done);
	REQUIRE(t->updates.size() == updatesBefore + 1);
	CHECK(t->updates.back().kind == TaskUpdateKind::Status);
	CHECK(t->updates.back().author == "Anna");

	// Assignees: an Assignment update with the whole new list.
	CHECK(edit([&](Database& d) { d.setPeople(crates, prop("Assignees"), { "Carol" }, kT0 + 9000); }));
	t = src.board.find(crates);
	CHECK(t->assignees == std::vector<std::string>{ "Carol" });
	CHECK(t->updates.back().kind == TaskUpdateKind::Assignment);
	CHECK(t->assignedBy == "Anna");

	// Priority, description, links in and out.
	const UUID urgent = before.findProperty(prop("Priority"))->findOptionByName("Urgent")->id;
	CHECK(edit([&](Database& d) {
		d.setSelect(crates, prop("Priority"), urgent, kT0 + 9000);
		d.setText(crates, prop("Description"), "Now with moss", kT0 + 9000);
		d.setLinks(crates, prop("Links"), { TaskLink::panelLink("Console", "Console") }, kT0 + 9000);
	}));
	t = src.board.find(crates);
	CHECK(t->priority == TaskPriority::Urgent);
	CHECK(t->description == "Now with moss");
	REQUIRE(t->links.size() == 1);   // the scene link went, the panel link came
	CHECK(t->links[0].kind == TaskLinkKind::Panel);

	// Nothing changed: nothing written.
	CHECK_FALSE(edit([&](Database&) {}));

	// A status word this build does not know is kept as the word it was.
	CHECK(edit([&](Database& d) {
		const SelectOption* o = d.addOption(prop("Status"), "needs-art");
		d.setSelect(crates, prop("Status"), o->id, kT0 + 9000);
	}));
	CHECK(src.board.find(crates)->status == TaskStatus::Other);
	CHECK(src.board.find(crates)->statusOther == "needs-art");
}

TEST_CASE("planner tasks: a new row is a new task with the row's id, a deleted row is a deleted task")
{
	FakeSource src;
	src.board = sampleBoard();
	Database before;
	refreshTasks(before, src.board);

	Database after = before;
	const UUID fresh = after.addRow("Paint the lighthouse", "Anna", kT0 + 9000)->id;
	after.removeRow(src.board.tasks[1].id);
	bool changed = false;
	std::string err;
	REQUIRE(applyTasksEdit(before, after, src.board, "Anna", kT0 + 9000, changed, err));
	CHECK(changed);
	CHECK(src.board.tasks.size() == 3);
	const Task* t = byTitle(src.board, "Paint the lighthouse");
	REQUIRE(t);
	CHECK(t->id == fresh);
	CHECK(t->createdBy == "Anna");
	CHECK(t->status == TaskStatus::Open);
	CHECK(t->updates.empty());   // a new task has no history yet
	CHECK(byTitle(src.board, "Fix the bridge") == nullptr);

	// Columns added or deleted: refused, the board untouched.
	Database wider = before;
	wider.addProperty("Estimate", PropertyType::Number);
	const std::string snapshot = src.board.toJson();
	CHECK_FALSE(applyTasksEdit(before, wider, src.board, "Anna", kT0, changed, err));
	CHECK(err.find("fixed set of columns") != std::string::npos);
	CHECK(src.board.toJson() == snapshot);
	Database narrower = before;
	narrower.removeProperty(prop("Links"));
	CHECK_FALSE(applyTasksEdit(before, narrower, src.board, "Anna", kT0, changed, err));
}

TEST_CASE("planner tasks: the workspace writes edits to the board and keeps only the views")
{
	TempProject tp;
	FakeSource src;
	src.board = sampleBoard();
	Workspace ws;
	ws.setProject(tp.root);
	ws.setExternal(src.external());

	Database* tasks = ws.find(tasksDatabaseId());
	REQUIRE(tasks);
	CHECK(tasks->rows.size() == 3);
	CHECK_FALSE(fs::exists(databasePath(tp.root, tasksDatabaseId())));   // nothing to save yet
	CHECK_FALSE(ws.remove(tasksDatabaseId()));
	CHECK_FALSE(ws.lastError().empty());

	// An edit on a row reaches the board, and what the board made of it comes back.
	const UUID bridge = src.board.tasks[1].id;
	const UUID done = tasks->findProperty(prop("Status"))->findOptionByName("Done")->id;
	REQUIRE(ws.edit(tasksDatabaseId(), [&](Database& db) { return db.setSelect(bridge, prop("Status"), done, kT0 + 1); }));
	CHECK(src.board.find(bridge)->status == TaskStatus::Done);
	CHECK(ws.find(tasksDatabaseId())->select(bridge, prop("Status")) == done);
	CHECK_FALSE(fs::exists(databasePath(tp.root, tasksDatabaseId())));   // a row edit writes no database file

	// A view edit is saved, without rows.
	REQUIRE(ws.edit(tasksDatabaseId(), [&](Database& db) {
		db.views[0].sorts = { { UUID{}, true } };
		return true;
	}));
	REQUIRE(fs::exists(databasePath(tp.root, tasksDatabaseId())));
	const std::string file = readFileText(databasePath(tp.root, tasksDatabaseId()));
	CHECK(file.find("Fix the bridge") == std::string::npos);
	CHECK(file.find("\"descending\": true") != std::string::npos);

	// A refused change leaves the database as it was.
	const std::size_t props = ws.find(tasksDatabaseId())->properties.size();
	CHECK_FALSE(ws.edit(tasksDatabaseId(), [](Database& db) { db.addProperty("Estimate", PropertyType::Number); return true; }));
	CHECK(ws.find(tasksDatabaseId())->properties.size() == props);
	CHECK(ws.lastError().find("fixed set of columns") != std::string::npos);
	src.refuse = true;
	CHECK_FALSE(ws.edit(tasksDatabaseId(), [&](Database& db) { return db.setTitle(bridge, "x", kT0 + 2); }));
	CHECK(ws.find(tasksDatabaseId())->findRow(bridge)->title == "Fix the bridge");

	// The board changes elsewhere (the footer, an MCP tool, a pull): poll reads it.
	src.refuse = false;
	src.board.setTitle(src.board.tasks[0].id, "Changed on the board", kT0 + 7000);
	++src.gen;
	CHECK(ws.poll(true));
	CHECK(ws.find(tasksDatabaseId())->findRow(src.board.tasks[0].id)->title == "Changed on the board");

	// A new session: the rows come from the board again, the views from the file.
	Workspace again;
	again.setProject(tp.root);
	again.setExternal(src.external());
	REQUIRE(again.find(tasksDatabaseId()));
	CHECK(again.find(tasksDatabaseId())->rows.size() == 3);
	CHECK(again.find(tasksDatabaseId())->views[0].sorts.size() == 1);
}
