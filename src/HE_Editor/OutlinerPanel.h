#pragma once

struct AppContext;

// ── World Outliner ───────────────────────────────────────────────────────────
// The scene hierarchy tree on the right: the cached hierarchy snapshot, the
// selection, drag & drop reparenting, the per-entity and background context
// menus (create/duplicate/delete) and the entity rename popup.
// Split out of EditorUI.cpp; all of its state is file-static in the .cpp.
namespace OutlinerPanel
{
	void render(AppContext& ctx);
}
