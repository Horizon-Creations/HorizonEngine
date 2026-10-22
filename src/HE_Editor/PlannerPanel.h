#pragma once
#include <imgui.h>

#include <functional>
#include <string>

#include <Types/UUID.h>

#include <string>

struct AppContext;
namespace HE::Planner { class Workspace; }

// ── Planner (Window ▸ Planner) ───────────────────────────────────────────────
// Project planning in the style of a Notion workspace, as an editor TAB (a
// virtual one, like Level Script and Game Instance) so the content gets the
// whole window instead of a dockable panel's corner. The data is Planner.h's
// (databases of rows with typed properties, saved as one file each under
// Config/Planner); this file is the window on top of it. See
// docs/planner-plan-2026-10-22.md.
//
// Left: the project's databases and pages ("New Database", "New Page"). Right: the selected database
// as a table (sort by a header, filter, search, hide columns, edit every cell in
// place) or as board lanes of a select property (drag a card to another lane to
// change its value), and, for the row you open, its details.
//
// ── Pages ─────────────────────────────────────────────────────────────────────
// A page is a tree of blocks (text, headings, lists, to-dos, quotes, callouts, code,
// dividers, an engine object, a database view), written like a document: Enter makes
// the next block, Markdown shortcuts turn a paragraph into a heading or a list, the
// "..." at a block's left turns it into another kind or moves it. Every database
// row is a page too ("Open Page" in its details), with its properties on top; the
// design document and level concepts are standalone pages in the sidebar.
//
// ── Whiteboards ───────────────────────────────────────────────────────────────
// "Boards" in the sidebar: cards and arrows on a free surface, with pan, zoom, move, resize,
// text edited in place, arrows dragged from a dot on a card, and undo (PlannerBoardView.h).
//
// ── Tasks replaces the Task Board window ──────────────────────────────────────
// The database "Tasks" is the project's task list (PlannerTasks.h): rows come
// from the Task Board's file and edits go back to it, so the footer, the MCP
// tools and the team bridge keep working on one list. Its row details have what
// the old window had: the description, the links as chips (click to jump, drop
// an asset, scene or entity to link), assignees from the team, the history and
// a comment box. Window ▸ Task Board and the footer open this tab.
//
// ── Who owns the databases ────────────────────────────────────────────────────
// This file does, through one HE::Planner::Workspace per editor: loaded for the
// open project, reloaded when files change under it (a pull), saved after every
// edit by the workspace. Edits that come out of the drawing code are POSTED and
// applied after the window has been drawn: an edit can remove a row or reload the
// whole database, and the loops that draw it hold references into it.
namespace PlannerPanel
{
	// Sentinel "asset path" identifying the Planner tab.
	constexpr const char* kTabPath = "::Planner::";

	// Fill the given tab rect with the planner (borderless).
	void render(AppContext& ctx, const ImVec2& pos, const ImVec2& size);

	// The editor's one workspace (for the tests and, later, the MCP tools).
	// Follows the open project; nullptr with none.
	HE::Planner::Workspace* workspace(AppContext& ctx);

	// Select a database / one of its views by id (the sidebar click, from code).
	void selectDatabase(const HE::UUID& id);
	HE::UUID selectedDatabase();
	HE::UUID selectedView();
	// Show Tasks with this task's details open (the footer's click, a "From the
	// Team" row, Window ▸ Task Board). The caller brings the tab forward.
	void openTask(const HE::UUID& id);
	// The row whose details are open; {} for none.
	HE::UUID selectedRow();
	// The month the calendar view shows (0 = not chosen yet: today's, once it is drawn).
	void showCalendarMonth(int year, int month);
	int  calendarYear();
	int  calendarMonth();
	// How wide a day is on a timeline: 0 days, 1 weeks, 2 months.
	void setTimelineZoom(int level);

	// Replace the system clipboard the whiteboard pastes from (tests); empty functions restore it.
	void setClipboardForTest(std::function<bool(std::string&, std::string&)> image, std::function<std::string()> text,
	                         std::function<void(const std::string&)> setText = {});

	// Show a standalone page, or the page of a row ("Open Page" in the row's details;
	// the page exists in memory until it has a block). {} when a database is shown.
	void selectPage(const HE::UUID& id);
	void openRowPage(const HE::UUID& databaseId, const HE::UUID& rowId);
	HE::UUID selectedPage();
	// Show a whiteboard (the sidebar click, from code); {} when a database or a page is shown.
	void selectBoard(const HE::UUID& id);
	HE::UUID selectedBoard();

	// What the last frame drew: rows the selected view has, and how many of them
	// were laid out (a long table only draws what is on screen).
	struct DrawStats { int rows = 0; int rowsDrawn = 0; };
	DrawStats lastDrawStats();

	// Where a named control was drawn last frame, in screen coordinates, so a
	// test can click it ("New Row", "Import Task Board", "database:<name>",
	// "title:<row title>", ...). {0,0,0,0} when it was not drawn.
	struct Rect { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
	Rect controlRect(const std::string& name);

	// Why the last load or save failed, empty when it did not.
	const std::string& lastError();

	// Forget everything (project closed, tests).
	void reset();
}
