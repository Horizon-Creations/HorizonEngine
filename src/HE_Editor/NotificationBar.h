#pragma once

// ── The bell in the footer, and what it opens ────────────────────────────────
// The reading end of NotificationStore. Everything the editor does behind the
// user's back — a peer that could not apply a delete, a scan that could not read
// a file, an import that half-worked — is posted into the store from whatever
// thread noticed it; this is the one place it becomes visible.
//
// The footer is the right home for the same reason the presence cluster lives
// there: it is ambient. "Three things went wrong while you were modelling" is
// something to notice in passing, not something to go looking for in a menu, and
// there is nowhere else to put it — the editor has no console panel, and the
// collab activity line next to it holds exactly one string and destroys it the
// moment you click.
//
// Split in two for the same reason CollabPresenceBar is (read its header): the
// bell has to be drawn INSIDE the footer window, which is rendered before the
// dockspace so docked panels cannot cover it, while the flyout has to be drawn
// LAST of all or a docked panel would sit on top of it. So DrawFooter() records
// where the bell landed and whether it is hovered, and DrawOverlay() — called
// from the editor's overlay pass — draws the list on top of everything.
//
// One rule the implementation exists to protect: OPENING the flyout does not
// mark anything seen. A notification centre that clears twelve entries because
// you opened it to read one is a notification centre nobody trusts, so the
// unseen flag only ever falls on the explicit "Mark all as seen".

struct AppContext;

namespace NotificationBar
{
	// How wide the bell cluster will be, in pixels; 0 when there is nothing at
	// all to show. Separate from DrawFooter because the footer right-aligns its
	// contents and therefore has to know the width BEFORE it places the cursor —
	// and the two must agree exactly, which is why both go through the same
	// private layout helper.
	float FooterWidth(AppContext& ctx);

	// The bell and its count. Call from inside the footer window, at the position
	// FooterWidth() was used to compute. Draws nothing when the store is empty:
	// the footer is not a place to park a dead icon.
	void DrawFooter(AppContext& ctx);

	// The flyout. Call from the editor's overlay pass, after every panel, so
	// nothing can cover it. Draws nothing unless DrawFooter ran THIS frame — the
	// editor has paths (the project hub) where the footer never draws at all, and
	// a flyout anchored to last frame's rectangle would hang in empty space.
	void DrawOverlay(AppContext& ctx);
}
