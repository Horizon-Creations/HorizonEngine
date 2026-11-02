#pragma once
#include "Planner.h"

#include <string_view>

// ── The Task Board as a Planner database ─────────────────────────────────────
// The schema and the rows of the database "Tasks": what a task looks like as a
// row. PlannerTasks.h keeps that database in step with the Task Board (the
// Planner replaced the Task Board's window, P1b); this is the part that builds
// it: the properties and views, and a board's tasks as rows.
//
// ── Fixed ids ─────────────────────────────────────────────────────────────────
// Every id here — the database, its properties, their options and the two views —
// is derived from a NAME, not generated. Two teammates who each open the Planner
// get the same ids, so the two view files git sees are one file to merge, not two
// unrelated "Tasks" databases. A row's id is the task's id.
//
// ── Importing into a database that has rows ───────────────────────────────────
// Adds the tasks that are not in the database and refreshes a row only when its
// task changed AFTER the row did (task.modifiedMs > row.modifiedMs). The Tasks
// database is rebuilt from an empty row list, so every task is new to it; the
// rule is for a database that kept rows (a P1-era file).
//
// What does not come across: a task's history (comments, status and assignment
// updates) stays in Config/TaskBoard.json, which the import never touches.
namespace HE::Planner
{

struct ImportResult
{
	int added = 0;
	int updated = 0;
	int unchanged = 0;
};

// The ids derived from names: stable across machines and across versions, so a
// name once used here never changes meaning. `scope` keeps one name in two
// places apart ("Open" the status vs "Open" some other select).
UUID derivedId(std::string_view kind, std::string_view name);

UUID tasksDatabaseId();

// Gives `db` the Tasks schema: the properties Status, Priority, Assignees,
// Description and Links with their options, and the views "All tasks" (table)
// and "By status" (board). What is there already is kept, matched by id;
// nothing the user added is removed. Sets db.id and db.name when they are empty.
void ensureTasksSchema(Database& db);

// Copies the board's tasks into `db` (see above). `db` gets the schema first.
ImportResult importTaskBoard(const TaskBoard& board, Database& db);

} // namespace HE::Planner
