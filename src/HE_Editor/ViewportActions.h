#pragma once
#include <HorizonScene/HorizonWorld.h>   // Entity
#include <vector>

class EditorSelection;

// ── What the viewport's context menu does to the scene ──────────────────────
// The menu that opens on a right-click over the picture — hide this, hide
// everything else, show it all again, put these under one parent — is a
// handful of scene edits that have nothing to do with ImGui. They live here,
// registry in and touched entities out, for the same reason EditorSelection
// and EditorMarquee do: "does Isolate leave the sun alone" and "does Group keep
// the pieces where they stand" are questions a test can ask without a window,
// and the menu in ViewportPanel is then only the wiring.
//
// None of these touch undo or collaboration themselves. The caller takes the
// snapshot BEFORE (one for the whole operation, the way the Outliner's eye and
// its context menu do) and reports the returned entities through
// AppContext::noteEntityEdited afterwards, so a prefab instance records the
// override — the visibility flags these flip are serialised component state,
// exactly what the Outliner's eye edits (HorizonScene/EntityVisibility.h).
namespace ViewportActions
{
	// Hide every selected entity with everything under it. Returns every
	// entity whose flags were touched (the subtrees, not just the roots).
	std::vector<Entity> hideSelected(HorizonWorld& world, const EditorSelection& selection);

	// Hide everything in the scene EXCEPT the selection and its subtrees — the
	// "let me look at this one thing" move. The built-ins (the world root, the
	// environment's sun and moon) are never touched: a hidden sun is a black
	// scene, which is not what anyone asking to isolate a prop meant. Returns
	// the entities hidden.
	std::vector<Entity> isolateSelected(HorizonWorld& world, const EditorSelection& selection);

	// Undo either: every entity carrying a hidden renderable is shown again.
	// Returns the entities shown.
	std::vector<Entity> showAll(HorizonWorld& world);

	// True while something in the scene is hidden — what enables "Show All".
	bool anyHidden(HorizonWorld& world);

	// Put the selection's roots under one new, empty "Group" entity. The group
	// is created under the roots' common parent (the world root when they do
	// not share one) at the centre of where the roots stand, and every root is
	// reparented under it WITHOUT moving on screen: reparentEntity keeps the
	// local transform as it is, so each root's local is recomputed against the
	// group's world matrix. With the group at identity rotation and unit scale
	// that is exact for roots that shared the group's parent; a root brought in
	// from elsewhere in the hierarchy is decomposed and may lose a shear it
	// could not hold anyway.
	//
	// Returns the group, which is also the new selection; entt::null when
	// there was nothing to group (no selection, or only built-ins).
	Entity groupSelected(HorizonWorld& world, EditorSelection& selection);

	// The reverse, for every selected entity that has children: the children
	// move up to its parent (again without moving on screen), the entity
	// itself is destroyed. Meant for the groups the call above makes, but it
	// does not check for a name — an author who wants a rig flattened gets it.
	// Built-ins and the world root are left alone. Returns the freed children,
	// which become the selection.
	std::vector<Entity> ungroupSelected(HorizonWorld& world, EditorSelection& selection);

	// Whether Ungroup has anything to do for this selection.
	bool canUngroup(HorizonWorld& world, const EditorSelection& selection);
}
