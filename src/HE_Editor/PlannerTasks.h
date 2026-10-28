#pragma once
#include "Planner.h"
#include "PlannerImport.h"

#include <string>

// ── The Tasks database: the Task Board, seen through the Planner ─────────────
// The Planner has replaced the Task Board's window (P1b), not its data: tasks
// are still kept in Config/TaskBoard.json by TaskBoard, because the footer, the
// MCP tools and the team bridge (assignment and status notices, built on each
// task's update history) all work on that model, and a task's history has no
// place in a row. So "Tasks" in the Planner is a PROJECTION:
//
//   board -> database   refreshTasks(): rows, properties and options are rebuilt
//                       from the board; the database file keeps only the views
//                       (Database::persistRows is false for it);
//   database -> board   applyTasksEdit(): an edit made on the database is turned
//                       into the TaskBoard calls that make the same change, so
//                       status and assignment changes append the updates the
//                       team bridge announces, and a title change appends none.
//
// There is exactly one writer of the tasks, TaskBoardPanel::edit. The Tasks
// database has a fixed set of columns (Status, Priority, Assignees, Description,
// Links); adding or deleting one is refused. Options of Status and Priority may
// be added: a word this build does not know is a status/priority "Other", which
// the board keeps as the word it was.
namespace HE::Planner
{

// Rebuilds `db` from `board`: sets id and name when empty, drops properties that
// are not part of the Tasks schema, and replaces every row. The views stay.
void refreshTasks(Database& db, const TaskBoard& board);

// Applies the difference between `before` and `after` (the Tasks database
// either side of one edit) to `board`, by row id, recording `author` and `nowMs`.
// Returns false with `error` set when the edit cannot be expressed on the board
// (the columns changed); the board is then untouched. True otherwise, also when
// nothing differed. `changed` says whether the board was modified.
bool applyTasksEdit(const Database& before, const Database& after, TaskBoard& board,
                    const std::string& author, std::int64_t nowMs, bool& changed,
                    std::string& error);

} // namespace HE::Planner
