#pragma once

struct AppContext;

// ── Source Control panel ─────────────────────────────────────────────────────
// Same shape as CollabPanel: a floating tool window whose open flag lives in
// EditorUI, so the View menu and the window's own close button stay in step.
//
// Drawn from renderOverlays, NOT renderEditor — the latter returns early while
// an asset tab is active, which is exactly when someone wants to look at what
// they have changed.
//
// Read-only for now: branch, ahead/behind and the list of changes. Staging,
// committing and pushing arrive in a later checkpoint, along with the host-only
// restriction the panel already explains.
namespace SourceControlPanel
{
	void DrawSourceControlWindow(AppContext& ctx, bool& open);

	// One line of repository status for the editor's footer bar: the branch and
	// how many files have changed. The point is ambient awareness — "I have
	// eleven files open that I have not committed" is the sort of thing you want
	// to notice, not to go looking for.
	//
	// Returns true when it was clicked, which the caller turns into "reveal the
	// panel". The reveal itself lives with the panel's open flag in EditorUI
	// rather than here, so there is exactly one place that decides what showing a
	// tool window means.
	//
	// Call from inside the footer window; draws nothing without a project.
	bool DrawFooterStatus(AppContext& ctx);
}
