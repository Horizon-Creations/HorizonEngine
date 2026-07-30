#pragma once

struct AppContext;

// ── Environment window (View ▸ Environment) ──────────────────────────────────
// Adds / removes the scene's Sky and Weather entities. Their *settings* are
// edited in the Details panel; this window only manages their presence.
// Split out of EditorUI.cpp.
namespace EnvironmentPanel
{
	void DrawEnvironmentWindow(AppContext& ctx, bool& open);
}
