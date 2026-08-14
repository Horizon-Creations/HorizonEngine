#pragma once
#include <HorizonScene/HorizonWorld.h>   // Entity, HorizonWorld

struct AppContext;
class EditorUndo;

// ── Details panel (Inspector) ────────────────────────────────────────────────
// The per-entity property editor on the right: name, every component's fields,
// the asset slots that reference other .hassets, "Add Component" and the
// remove-component menus. Every widget routes its edits through the editor undo
// system (capturePre on the mouse press, stash/commit around the widget).
// Split out of EditorUI.cpp; it keeps no file-static state of its own beyond a
// couple of function-local caches and talks to the editor only via AppContext.
namespace InspectorPanel
{
	// The docked "Details" window over the editor's current selection.
	void render(AppContext& ctx);

	// Just the component rows, over an EXPLICIT world + entity and inside
	// whatever window the caller has already opened. This is what lets a panel
	// that edits a template subtree in its own scratch world — the HorizonCode
	// class tab's component list — use the real component editor instead of a
	// second copy that would drift the first time a component gained a field.
	//
	// `undo` is the editor's SCENE undo system, or null for a world that is not
	// the scene (a snapshot there would capture the wrong thing).
	void renderFor(AppContext& ctx, HorizonWorld& world, Entity entity, EditorUndo* undo);
}
