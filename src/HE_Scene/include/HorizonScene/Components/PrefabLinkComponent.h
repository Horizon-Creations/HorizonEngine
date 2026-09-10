#pragma once
#include <Types/UUID.h>

// ─── Where this entity came from ─────────────────────────────────────────────
// Placing a prefab is a COPY: `SceneSerializer::instantiatePrefab` mints fresh
// identities for every entity in the blob, which is what makes the same prefab
// placed twice two things rather than one thing claiming two places. What the
// copy never carried was any memory of the file it was copied from — so
// "which of these lamp posts came from Prefabs/Lamp.hasset" was a question
// nobody in the editor could answer, including the delete dialog that is
// supposed to warn what a prefab is still used by.
//
// This component is that memory and nothing more: the uuid of the Prefab asset
// the entity was instantiated from, written on the ROOT of the placement (the
// children are part of the instance, not instances of their own).
//
// ── What it deliberately is NOT ──────────────────────────────────────────────
// It is not prefab inheritance. Editing Prefabs/Lamp.hasset does not reach the
// lamp posts already standing in a scene, there is no override tracking, and
// nothing re-applies anything on load. A placement is still a copy; it now
// knows where it came from. Anything more has to answer what happens to an
// entity a human edited after placing it, and that is a design question with
// several defensible answers rather than a field.
//
// ── Why a uuid and not a path ────────────────────────────────────────────────
// The same reason every other asset reference in a scene is one: a path in a
// scene file breaks the moment the asset is moved, and `AssetRefScan` finds
// asset references in a `.hescene` by walking for [hi, lo] pairs INSIDE a
// "components" block — so an instance is found by the delete/move dialogs for
// free, and only because the id sits where every other asset id sits.
struct PrefabLinkComponent {
	HE::UUID asset;   // the Prefab asset this entity was instantiated from
};
