#pragma once
#include <HorizonScene/HorizonWorld.h>   // Entity, HorizonWorld
#include <glm/mat4x4.hpp>

class EditorUndo;

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include "ViewportToolbar.h"   // State: which operation, which axes, which snap

// ── The move / rotate / scale gizmo ─────────────────────────────────────────
// Draws ImGuizmo over a viewport and writes the result back into the entity's
// TransformComponent, in its parent's space.
//
// A module of its own because more than one viewport shows a 3D world now — the
// Scene window and the class editor's — and both must manipulate identically.
// The two subtleties in here are the reason a second copy would be a bug rather
// than a duplication: the matrix is LATCHED for the duration of a drag (the
// TRS → matrix → decompose → TRS round trip is not an identity, so re-deriving
// it every frame made the values visibly jitter), and only the channels the
// current operation manipulates are written back (a scale drag used to overwrite
// rotation with an equivalent-but-different Euler triple).
//
// ImGuizmo is a single-global-state library, so at most one gizmo is live at a
// time — which is why the drag state here is file-static rather than per caller.
namespace EditorTransformGizmo
{
	// Manipulate `entity` in `world`. `view`/`proj` must be the matrices the
	// picture was drawn with; `rectMin`/`rectMax` its screen rectangle.
	// `enabled` is false while the camera is being navigated, so a fly drag
	// cannot grab a handle.
	//
	// `undo` may be null for a scratch world (a snapshot there would capture the
	// scene, which is not what is being edited). Returns true while the gizmo is
	// hovered or in use — the caller suppresses its own click handling then.
	//
	// `outChanged` (optional) reports whether the transform was actually written
	// this frame. A caller tracking unsaved changes needs that and not the
	// return value: merely hovering a handle changes nothing.
	bool manipulate(HorizonWorld& world, Entity entity,
	                const glm::mat4& view, const glm::mat4& proj,
	                const ImVec2& rectMin, const ImVec2& rectMax,
	                const ViewportToolbar::State& tb, bool enabled,
	                EditorUndo* undo, bool* outChanged = nullptr);

	// W/E/R switch the operation while `hovered` and not navigating — the same
	// keys the toolbar's Move/Rotate/Scale cells set. Split out so every viewport
	// answers those keys alike, including ones with no toolbar of their own.
	void handleOperationKeys(ViewportToolbar::State& tb, bool hovered, bool navigating);
}
#endif // HE_IMGUI_ENABLED
