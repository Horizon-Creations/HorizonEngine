#pragma once
#include "TaskBoardPanel.h"          // Rect, and the one board this reads and edits
#include <Types/UUID.h>

#include <string>
#include <string_view>
#include <vector>

struct AppContext;
namespace HE { class TaskBoard; }

// ── The task board in the footer ──────────────────────────────────────────────
// "What is left to do here, and what is mine?" answered without a window open:
// a box, the number of open tasks (and how many of them are assigned to me),
// and the next one by name. Click it and a list of the open tasks opens above
// it, each with a box to tick it off on the spot, and a way into the Task Board
// panel for everything else.
//
// ── Where it sits, and how wide it may get ────────────────────────────────────
// Left of the footer, right after the source-control status — the two things
// about the PROJECT rather than the editor — and not in the right-anchored
// chain, whose every block would have to subtract it. The price of the left is
// a neighbour that grows towards it: the centred "Ready" line, which keeps to
// the middle third of the footer. budget() is the room up to that third, and
// the widget gives way inside it — the next task's title is cut with "...",
// then dropped, then the words of the count, then the whole widget — rather
// than ever running into the middle.
//
// ── One line tall, whatever it says ───────────────────────────────────────────
// The footer is a fixed 24px strip, and every item in it is one text line
// high. So is this, in every state (no tasks, a long title, a narrow window):
// the list it opens is a separate window drawn above the footer, never a
// second line inside it.
//
// ── The board ─────────────────────────────────────────────────────────────────
// It reads TaskBoardPanel::board() and changes it only through
// TaskBoardPanel::edit(), by task id — the one copy of the list and the one
// writer of its file (TaskBoardPanel.h, "Who owns the board").
//
// Split in two like the bell next door (NotificationBar.h): DrawFooter() is
// drawn inside the footer window, DrawOverlay() from the editor's overlay pass,
// after every panel, so nothing can cover the list.
namespace TaskBoardFooter
{
	// ── What it says (no ImGui) ──────────────────────────────────────────────
	// The open tasks (everything not Done, as TaskBoard::openCount() counts
	// them) in the order the footer offers them:
	//   1. assigned to `me` first;
	//   2. Blocked last — a blocked task is not the next thing anyone can do;
	//   3. then by priority, Urgent > High > Normal > Low (a priority word this
	//      build does not know counts as Normal);
	//   4. then In Progress before Open — what was started comes before what
	//      was not;
	//   5. then file order.
	std::vector<HE::UUID> rankedOpen(const HE::TaskBoard& board, std::string_view me);

	struct Summary
	{
		int         open = 0;    // TaskBoard::openCount()
		int         mine = 0;    // TaskBoard::openCountFor(me)
		HE::UUID    next;        // rankedOpen().front(); zero when nothing is open
		std::string nextTitle;   // its title ("(untitled)" for an empty one)
	};
	Summary summarize(const HE::TaskBoard& board, std::string_view me);

	// The width the widget may take when it starts at `cursorX` (window-local,
	// as GetCursorPosX() says) in a footer `windowWidth` wide: up to a gap short
	// of the middle third, where the centred status line lives. 0 when there is
	// no room at all.
	float budget(float cursorX, float windowWidth);

	// ── Drawing ──────────────────────────────────────────────────────────────
	// The widget, at the cursor, inside the footer window, no wider than
	// `maxWidth` and exactly one text line tall. Draws nothing (and returns 0)
	// with no project open, or when not even the box fits. Returns the width it
	// took. A click opens the list; a second click closes it.
	float DrawFooter(AppContext& ctx, float maxWidth);

	// The list. Call from the overlay pass. Draws nothing unless DrawFooter drew
	// the widget THIS frame (the project hub skips the footer). Returns true when
	// the user asked for the Task Board panel — "Open Task Board", or a task's
	// title, which also selects that task in it; the caller raises the window.
	bool DrawOverlay(AppContext& ctx);

	// ── For the UI test ──────────────────────────────────────────────────────
	using Rect = TaskBoardPanel::Rect;
	// Where the widget was drawn last frame; false when it was not.
	bool widgetRect(Rect& out);
	// Whether the list is open.
	bool isOpen();
	// The list's window, and in it a task's tick box and its title; false when
	// the list is closed or the task is not in it.
	bool listRect(Rect& out);
	bool tickRect(const HE::UUID& task, Rect& out);
	bool titleRect(const HE::UUID& task, Rect& out);
	// Close the list and forget everything. For tests.
	void reset();
}
