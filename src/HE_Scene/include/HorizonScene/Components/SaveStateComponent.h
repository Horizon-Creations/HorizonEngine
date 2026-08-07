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
struct SaveStateComponent {
	bool enabled        = true;  // master switch (a disabled component fails loud too)
	bool saveTransform  = true;  // position / rotation / scale
	bool saveVisibility = true;  // the renderable-visibility toggle (entity.setVisible)
};
