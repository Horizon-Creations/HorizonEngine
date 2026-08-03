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
}
