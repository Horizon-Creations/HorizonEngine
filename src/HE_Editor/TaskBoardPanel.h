#pragma once
#include <Types/UUID.h>

#include <functional>
#include <string>

struct AppContext;
namespace HE { class TaskBoard; }

// ── Task Board (Window ▸ Task Board) ──────────────────────────────────────────
// The project's to-do list (TaskBoard.h) as a panel: the tasks grouped by
// status, as a list or as four columns, a task's details underneath, and the
// things it is about as chips — an asset with its thumbnail, an entity, a
// scene, a panel. Drop an asset or a scene from the Content Browser, or an
// entity from the World Outliner, onto a task to link it; click a chip to go
// there.
//
// ── Who owns the board ────────────────────────────────────────────────────────
// This file does: one board per open project, loaded the first time anything
// asks (board()), reloaded when the project changes and when the file changes
// under it (a pull), saved after every edit (edit()). The footer widget, the
// MCP tools and the team bridge edit through the same two functions, so there
// is exactly one copy of the list in the editor and one writer of its file.
//
// ── Jumps ─────────────────────────────────────────────────────────────────────
// Selecting an entity is done here (the selection is on the AppContext). The
// rest — bringing the scene tab forward, opening a scene through the
// unsaved-changes prompt, revealing an asset in the Content Browser, raising a
// panel — belongs to EditorUI, whose statics and guards those are. A chip
// click leaves a one-shot Jump that EditorUI takes at the top of its frame,
// the way the Content Browser's own reveal works. That also keeps this panel
// free of EditorUI and the Content Browser, so the UI test can draw it alone.
namespace TaskBoardPanel
{
	// `open` is the Window-menu toggle; the window clears it when closed.
	// Drawn from the overlay pass like the other tool windows. Call it every
	// frame even while closed: a jump that had to open a scene first selects
	// its entity once the scene is there.
	void DrawTaskBoardWindow(AppContext& ctx, bool& open);

	// The open project's board, or nullptr with no project open. Loads on first
	// ask; reloads when the project root changes or the file on disk changed
	// since it was read or written (checked at most a few times a second).
	HE::TaskBoard* board(AppContext& ctx);

	// Apply `fn` to the board and save. When the save is refused because the
	// file changed under the board (a pull landed in between), the board is
	// reloaded from disk and `fn` applied again — once — so the edit lands on
	// what arrived instead of overwriting it. `fn` must therefore address tasks
	// by id, never by pointer or index. Returns false (and lastError() says
	// why) when the edit could not be saved; it is kept in memory either way.
	bool edit(AppContext& ctx, const std::function<bool(HE::TaskBoard&)>& fn);

	// Why the last load or save failed, for the banner; empty when it did not.
	const std::string& lastError();

	// The name edits are recorded under. Until the team bridge sets the team
	// member name, the operating system's user name.
	void setAuthor(std::string name);
	std::string author();

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

	// The task whose details are shown; a zero UUID for none.
	HE::UUID selectedTask();
	void     selectTask(const HE::UUID& id);

	enum class View { List, Columns };
	View view();
	void setView(View v);

	// Forget the board and every bit of panel state — the next board() loads
	// afresh. For tests; a project switch is noticed by itself.
	void reset();

	// ── What the last frame drew, for the UI test ────────────────────────────
	// Screen rectangles of the cards and of the chips on them; false for a
	// card that was not drawn (filtered out, folded away, scrolled off).
	struct Rect { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
	bool cardRect(const HE::UUID& task, Rect& out);
	bool chipRect(const HE::UUID& task, int link, Rect& out);
	// Cards drawn, and cards stepped over with a placeholder because they were
	// off screen — the row clipping, counted.
	struct DrawStats { int drawn = 0; int skipped = 0; };
	DrawStats lastDrawStats();
}

