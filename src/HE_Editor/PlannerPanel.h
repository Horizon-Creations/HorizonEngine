#pragma once
#include <imgui.h>

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
// Left: the project's databases, "New Database", "Import Task Board". Right: the
// selected database as a table (sort by a header, filter, search, hide columns,
// edit every cell in place) or as board lanes of a select property (drag a card
// to another lane to change its value). The Task Board stays where it is until
// the planner has taken it over (P1b); "Import Task Board" copies its tasks into
// the database "Tasks" (PlannerImport.h), as often as you like.
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
	// The line under the toolbar after an import ("Imported 3 tasks ...").
	const std::string& notice();

	// Forget everything (project closed, tests).
	void reset();
}
