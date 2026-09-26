#pragma once

// ─── Opt-in savegame serialization for one entity ────────────────────────────
// An entity carrying this component can write its state into the ACTIVE save
// (HE::api::save) and re-apply it later: entity.saveState stores the flagged
// attributes under the entity's stable UUID (EntityIdComponent) in the save's
// entities section; entity.applySavedState applies every attribute PRESENT in
// the save back onto the instance (partial by design — what the save doesn't
// carry stays untouched). entity.hasSavedState answers whether the active save
// knows this entity at all.
//
// The API is gated to play mode (PIE + packaged game): in edit mode the
// SceneSerializer owns persistence, and a save call fails loud instead.
//
// Identity is the SCENE-AUTHORED UUID — an entity spawned at runtime mints a
// fresh UUID every run, so its saved state can never re-apply; this component
// is for scene-authored entities.
//
// Script variables (saveScriptVars): WHICH ones is decided in the script, not
// here — a HorizonCode class ticks "Save Game" on a variable
// (Variable::saveGame), and those, and only those, travel under "vars",
// name-keyed, through the same value codec the save's own fields use. Restoring
// is name-keyed and partial like the rest: a saved name the class no longer
// declares is skipped, a declared one the save lacks keeps its current value.
// Object (Ref) variables are never saved — a handle names nothing next run.
// Lua and Python scripts are NOT captured: their backends can inject
// properties before onStart but cannot read them back, so their state goes
// through the save's own fields (save.set / save.get) instead.
struct SaveStateComponent {
	bool enabled        = true;  // master switch (a disabled component fails loud too)
	bool saveTransform  = true;  // position / rotation / scale
	bool saveVisibility = true;  // the renderable-visibility toggle (entity.setVisible)
	bool saveScriptVars = true;  // the class's Save Game variables (HorizonCode only)
};
