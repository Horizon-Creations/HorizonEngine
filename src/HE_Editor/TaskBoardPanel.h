#pragma once
#include "TaskBoard.h"

#include <Types/UUID.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct AppContext;
namespace HE { class TaskBoardTeam; }

// ── The Task Board's data core (the window is the Planner's Tasks database) ───
// The project's to-do list (TaskBoard.h). Its WINDOW went when the Planner took
// over (PlannerPanel.h, docs/planner-plan-2026-10-22.md); what stays is the part
// everybody else stands on, and the pieces of link UI the Planner reuses.
//
// ── Who owns the board ────────────────────────────────────────────────────────
// This file does: one board per open project, loaded the first time anything
// asks (board()), reloaded when the project changes and when the file changes
// under it (a pull), saved after every edit (edit()). The footer widget, the
// MCP tools, the team bridge and the Planner's Tasks database edit through the
// same two functions, so there is exactly one copy of the list in the editor and
// one writer of its file.
//
// ── Jumps ─────────────────────────────────────────────────────────────────────
// Selecting an entity is done here (the selection is on the AppContext). The
// rest — bringing the scene tab forward, opening a scene through the
// unsaved-changes prompt, revealing an asset in the Content Browser, raising a
// panel — belongs to EditorUI, whose statics and guards those are. A chip click
// leaves a one-shot Jump that EditorUI takes at the top of its frame, the way
// the Content Browser's own reveal works.
namespace TaskBoardPanel
{
	// Once per frame, wherever the editor draws: pumps the team bridge (notices
	// arrive while no window is open) and finishes an entity jump that had to
	// open its scene first.
	void tick(AppContext& ctx);

	// The open project's board, or nullptr with no project open. Loads on first
	// ask; reloads when the project root changes or the file on disk changed
	// since it was read or written (checked at most a few times a second).
	HE::TaskBoard* board(AppContext& ctx);
	// Bumped whenever the board changes: replaced from disk (a pull, Reload) or
	// edited through edit(). The Planner compares it to know when to re-read.
	unsigned generation();

	// Apply `fn` to the board and save. When the save is refused because the
	// file changed under the board (a pull landed in between), the board is
	// reloaded from disk and `fn` applied again — once — so the edit lands on
	// what arrived instead of overwriting it. `fn` must therefore address tasks
	// by id, never by pointer or index. Returns false (and lastError() says
	// why) when the edit could not be saved; it is kept in memory either way.
	bool edit(AppContext& ctx, const std::function<bool(HE::TaskBoard&)>& fn);

	// Why the last load or save failed, for the banner; empty when it did not.
	const std::string& lastError();
	// The banner's two answers: read the file again (for a file that could not be
	// read, or to drop an edit that could not be saved), and try the save again.
	void reloadBoard(AppContext& ctx);
	bool retrySave();
	// An edit is in memory that the file does not have (a save failed).
	bool hasUnsavedEdit();

	// The name edits are recorded under. Until the team bridge sets the team
	// member name, the operating system's user name.
	void setAuthor(std::string name);
	std::string author();

	// ── Team (TaskBoardTeam.h) ────────────────────────────────────────────────
	// The bridge to the team server, or null (the default): the board is local.
	// Not owned. While its link is signed in, tick() pumps it every frame,
	// records edits under the member name, and posts every saved edit's new
	// updates to the team.
	// The From the Team list: notices that arrived after `seen` were not looked at.
	std::uint64_t teamSeen();
	void          markTeamSeen(std::uint64_t atMs);
	// The open project's folder name, which a notice names its project by.
	std::string   projectName();
	void               setTeam(HE::TaskBoardTeam* team);
	HE::TaskBoardTeam* team();
	// One frame of the team: send and receive (TaskBoardTeam::update), and keep
	// author() on the member name while signed in. tick() calls it; a test
	// without ImGui calls it itself.
	void pumpTeam();
	// The steady milliseconds the bridge is pumped and told about edits with.
	// Empty (the default): NotificationStore::nowMs(). A test passes the one
	// fake clock all its pumps share.
	void setTeamClock(std::function<std::uint64_t()> now);

	// What a chip click asked EditorUI to do. One-shot: takeJump() returns it
	// once and clears it.
	struct Jump
	{
		enum class Kind { None, Asset, Scene, Entity, Panel };
		Kind        kind = Kind::None;
		// Asset / Scene: the absolute path. Panel: the ImGui window name.
		// Entity: empty — the entity is already selected; EditorUI brings the
		// scene tab and the World Outliner forward.
		std::string target;
	};
	Jump takeJump();

	// The task the Planner shows details for; a zero UUID for none.
	HE::UUID selectedTask();
	void     selectTask(const HE::UUID& id);

	// A screen rectangle, for the footer's test hooks (TaskBoardFooter.h).
	struct Rect { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; };

	// Forget the board and every bit of panel state, the team bridge included —
	// the next board() loads afresh. For tests; a project switch is noticed by itself.
	void reset();

	// ── Links ────────────────────────────────────────────────────────────────
	// Marks every link of the board orphaned or not (its target is gone), a
	// few times a second, not per frame. Cheap to call every frame.
	void refreshOrphans(AppContext& ctx);
	// Ask EditorUI to go to the link's target (leaves a Jump; an entity is
	// selected here, in a scene that has to be opened first, once it is).
	void jumpToLink(AppContext& ctx, const HE::TaskLink& l);
	// The windows a Panel link can name.
	std::vector<const char*> linkablePanels();
	// What "Link Selection" and "Link Scene" would add: the selected entities,
	// and the open scene (false when it has not been saved).
	std::vector<HE::TaskLink> selectionLinks(AppContext& ctx);
	bool currentSceneLink(AppContext& ctx, HE::TaskLink& out);

	// ── Chips and drops (ImGui) ──────────────────────────────────────────────
	// A chip: icon or thumbnail, the name, clipped at its edge. An orphaned link
	// is dimmed and struck through. Returns true when a live link was clicked
	// (the caller then calls jumpToLink).
	float linkChipWidth(const HE::TaskLink& l, float maxW, float h);
	bool  linkChip(AppContext& ctx, const HE::TaskLink& l, int index, float maxW, float h);
	// The item submitted just above is the target: the links an asset or scene
	// from the Content Browser, or an entity from the World Outliner, dropped on
	// it stand for. Empty when nothing was dropped.
	std::vector<HE::TaskLink> acceptDroppedLinks(AppContext& ctx);
}
