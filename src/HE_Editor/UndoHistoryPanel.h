#pragma once

struct AppContext;

// ── Undo History (Window ▸ Undo History) ───────────────────────────────────────
// The scene's undo stack as a list: every step that can still be taken back,
// oldest at the top, the current state marked, and below it everything that
// was taken back and can be brought back. Clicking a row jumps there in ONE
// restore (EditorUndo::undoSteps / redoSteps), so a row far up costs what a
// single Ctrl+Z costs.
//
// Reads the same stack the footer buttons, the Edit menu and Ctrl+Z drive.
// In a collaboration session that stack is not in use (CollabUndo replaces it
// with inverse operations on your own changes), and during play the editor
// hands out no undo at all — both states are said in the window rather than
// shown as an empty list.
namespace UndoHistoryPanel
{
	// `open` is the View-menu toggle; the window clears it when closed. Drawn
	// from the editor's overlay pass like the other tool windows.
	void DrawUndoHistoryWindow(AppContext& ctx, bool& open);
}
