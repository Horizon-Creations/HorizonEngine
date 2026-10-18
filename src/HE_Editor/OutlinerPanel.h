#pragma once

struct AppContext;

// ── World Outliner ───────────────────────────────────────────────────────────
// The scene hierarchy tree on the right: the cached hierarchy snapshot, the
// selection, drag & drop reparenting, the per-entity and background context
// menus (create/duplicate/delete/sibling order/lock), the search box and type
// chips (OutlinerFilter.h), per row a type icon, badges, the eye and padlock
// (EntityVisibility.h, EditorLockComponent.h), in-place rename (F2 or a
// double-click) and drops on a row's edge to reorder.
// Split out of EditorUI.cpp; all of its state is file-static in the .cpp.
namespace OutlinerPanel
{
	void render(AppContext& ctx);

	// Rows scrolled out of the panel are skipped, not submitted (render's
	// "Rows scrolled out of the panel" note). On by default; off draws every
	// row as before, which is what the test compares the clipped panel with.
	void setRowClipping(bool on);

#ifdef HE_IMGUI_ENABLED
	// The "New Entity" list (Empty, Cube, Camera ▸, Light ▸, Rope, Trail) as
	// menu rows, creating the pick at the world root with an undo step — the
	// same list the Outliner's background menu and "Create Child" draw. Public
	// so the main bar's Entity ▸ Create submenu is that list and not a copy of
	// it. Call inside an open menu; true when something was created.
	bool drawCreateEntityMenu(AppContext& ctx);
	// The same list as a table, for a menu that is built rather than drawn (the
	// native macOS bar): `group` is the submenu the row sits in ("" = top
	// level, "Camera", "Light"), `index` is what createEntityPreset takes.
	struct EntityPresetRow { const char* label; const char* group; };
	const EntityPresetRow* entityPresetTable(int& outCount);
	void createEntityPreset(AppContext& ctx, int index);

	// Save the primary selection's subtree as a prefab under Content/Prefabs
	// (unique name, announced to a collaboration session) — what the row's
	// context menu does, callable from the Entity menu. Refuses the root and
	// an empty selection; the world is not changed.
	void saveSelectionAsPrefab(AppContext& ctx);
#endif
}
