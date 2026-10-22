#pragma once
#include "Planner.h"

#include <string_view>

// ── The Task Board as a Planner database ─────────────────────────────────────
// The Planner replaces the Task Board (docs/planner-plan-2026-10-22.md). Until
// the board's panel, footer and MCP tools move over (P1b), the board stays the
// place tasks are kept and this copies them into the database "Tasks", where
// the Planner shows them as a table and as lanes.
//
// ── Fixed ids ─────────────────────────────────────────────────────────────────
// Every id the import creates — the database, its properties, their options and
// the two views — is derived from a NAME, not generated. Two teammates who each
// press "Import Task Board" get the same ids, so the two files git sees are one
// file with two sets of rows to merge, not two unrelated "Tasks" databases. A
// row's id is the task's id, so importing again finds the row again.
//
// ── Importing again ───────────────────────────────────────────────────────────
// Adds the tasks that are not in the database and refreshes a row only when its
// task changed AFTER the row did (task.modifiedMs > row.modifiedMs). An edit
// made in the Planner stamps the row with the time of the edit, so it wins over
// an older board and is not overwritten by pressing Import twice.
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
