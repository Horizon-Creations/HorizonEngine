#pragma once

struct AppContext;

// ── World Outliner ───────────────────────────────────────────────────────────
// The scene hierarchy tree on the right: the cached hierarchy snapshot, the
// selection, drag & drop reparenting, the per-entity and background context
// menus (create/duplicate/delete/sibling order/lock), the search + type
// filter header (OutlinerFilter.h), the eye and padlock on every row
// (EntityVisibility.h, EditorLockComponent.h) and the entity rename popup.
// Split out of EditorUI.cpp; all of its state is file-static in the .cpp.
namespace OutlinerPanel
{
	void render(AppContext& ctx);
}
