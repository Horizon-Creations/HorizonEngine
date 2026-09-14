#pragma once

// ── Locked in the editor ─────────────────────────────────────────────────────
// A tag, not data: an entity carrying it is one the user has pinned down in
// the World Outliner. The viewport leaves it alone — a click passes through it
// to whatever is behind, the rubber band does not frame it, the gizmo does
// not move it — so the floor, the walls and the big set pieces stop catching
// every stray click meant for the prop in front of them. The Outliner still
// selects it and the Details panel still edits it: the lock is about the
// mouse in the picture, not about the data.
//
// Editor state, kept on the entity so it survives an undo (which rebuilds the
// world from a snapshot) and a save: SceneSerializer writes it as a field of
// the entity record, beside its name, NOT in the components block. The
// components block is what the prefab machinery diffs and syncs, and a lock
// on a placed prefab's child is not a change to the prefab — it is where the
// user parked the mouse. Prefab assets and the clipboard therefore never
// carry it, and a peer in a collaboration session does not see it either:
// EditorCommands replicate components, and this is deliberately not one of
// the keys they know.
struct EditorLockComponent
{
};
