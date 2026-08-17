#pragma once
#include <HorizonScene/HorizonWorld.h>   // Entity, HorizonWorld
#include <string>
#include <vector>

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
	//
	// `onlyComponent` (optional) restricts the output to the ONE section with
	// that label — what the class tab's component tree wants when a single
	// component is selected. The entity name row and the Add Component button
	// belong to the entity and are left out then.
	//
	// Returns true when a component was added or removed. Neither is visible
	// from the outside — the add menu is a popup (its own ImGui window) and a
	// removal leaves no active item behind — so a caller that tracks unsaved
	// changes by watching for active widgets would miss both.
	bool renderFor(AppContext& ctx, HorizonWorld& world, Entity entity, EditorUndo* undo,
	               const char* onlyComponent = nullptr);

	// The labels of the sections renderFor WOULD draw for this entity, drawing
	// nothing. It runs the very same body in a collect mode, so a caller listing
	// components (the class tab's tree) can never fall behind what this panel
	// actually edits — a hand-written second list would drift the first time a
	// component was added here. Safe to call from inside another window: it
	// emits no ImGui calls.
	void listComponents(AppContext& ctx, HorizonWorld& world, Entity entity,
	                    std::vector<std::string>& out);

	// Remove the component with that Details-panel label. Goes through the very
	// same section body the header's right-click "Remove Component" uses, so
	// there is no second label→type map to keep in step. Draws nothing.
	void removeComponent(AppContext& ctx, HorizonWorld& world, Entity entity,
	                     const char* label, EditorUndo* undo = nullptr);

	// The Add Component menu's items, inside a popup the CALLER has opened
	// (BeginPopup/BeginPopupContextItem … EndPopup). Shared by the Details
	// panel's button and the class tab's component tree.
	//
	// Returns true when something was added — a popup is its own ImGui window,
	// so a caller tracking unsaved changes cannot detect the edit from the
	// outside, and an untracked add is a component lost when the tab closes.
	bool addComponentMenu(HorizonWorld& world, Entity entity, EditorUndo* undo);
}
