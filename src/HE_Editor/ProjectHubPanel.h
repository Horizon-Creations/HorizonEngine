#pragma once

struct AppContext;

// ── Project Hub ──────────────────────────────────────────────────────────────
// The full-window start screen shown while no project is open: the header bar,
// the "New Project" form (template preset + script language + target folder),
// the recent-project list and the async open-project / choose-folder dialogs.
// Split out of EditorUI.cpp; all of its state is file-static in the .cpp.
namespace ProjectHubPanel
{
	void render(AppContext& ctx);
}
