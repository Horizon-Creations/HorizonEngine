#pragma once
#include "PlannerBoard.h"

#include <imgui.h>

#include <functional>
#include <string>
#include <vector>

struct AppContext;
namespace HE::Planner { class Workspace; }

// ── The Whiteboard's canvas (inside the Planner tab) ─────────────────────────
// A toolbar and an unbounded plane to draw the board's cards and arrows on: pan and
// zoom, select (click, shift, a rectangle), move, resize, edit a card's text in place,
// connect cards by dragging from a dot on their edge, delete, colour, undo and redo.
// The data and its file are PlannerBoard.h's; every change is made through the editor's one
// Workspace (editBoard), so a pull under the open board is read and the change lands on top.
//
// Gestures
//   scroll / drag with the middle button / Space + drag    pan        Ctrl (Cmd) + scroll    zoom at the cursor
//   click a card, Shift+click, drag a rectangle              select
//   drag a selected card                                     move (a frame takes what is inside it)
//   drag a corner handle of a single selected card           resize
//   drag from a dot on a selected or hovered card to another connect them
//   double-click empty space / a card                        new card there / edit its text
//   Delete, Cmd/Ctrl+Z, Shift+Cmd/Ctrl+Z (or Y), Cmd/Ctrl+A, Esc
//
// Edits that come out of the drawing code are POSTED and applied after the window has been
// drawn (PlannerPanel.h says why).
namespace PlannerBoardView
{

// What the panel lends the canvas.
struct Services
{
	HE::Planner::Workspace*                        ws = nullptr;
	AppContext*                                    ctx = nullptr;
	// Run after this frame's drawing.
	std::function<void(std::function<void()>)>     post;
	// Remember where the item just submitted is, under a name a test can ask for.
	std::function<void(const std::string&)>        note;
	std::function<void(const std::string&, ImVec2, ImVec2)> noteRect;
};

// Toolbar and canvas for the board; call inside a window, with the panel's help scope open.
void draw(const Services& services, const HE::UUID& boardId);

struct ViewState { double x = -40, y = -40, zoom = 1; };   // the world point at the canvas's top left, and the scale
ViewState viewOf(const HE::UUID& board);
void      setView(const HE::UUID& board, ViewState v);

std::vector<HE::UUID> selection();
HE::UUID              selectedEdge();

struct Stats { int nodes = 0; int drawn = 0; };   // the board's cards, and how many were drawn (the rest are off screen)
Stats lastStats();

bool canUndo(const HE::UUID& board);
bool canRedo(const HE::UUID& board);

// Forget a board's view, history and selection (it was deleted), or everything (project closed, tests).
void forget(const HE::UUID& board);
void reset();

} // namespace PlannerBoardView
