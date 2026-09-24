#pragma once

// ─── Selection markers and collider wireframes, as lines ─────────────────────
// Two of the overlays the editor's per-frame debug block draws into the
// viewport (the toolbar's Show popup switches each one): the amber box around
// every selected entity and the cyan/magenta outline of every collider.
//
// Out of EditorApplication::OnRender for two reasons. The headless dump never
// runs that block — it renders straight through r->Render() — so a witness
// scene that wants these lines in its picture has to be able to build them by
// the same code, not by a copy of it (HE_DUMP_SELBOXTEST does). And they are
// pure geometry into a DebugDrawBuffer, so the tests can ask where the box
// actually lands without an editor.

#include <DebugDraw/DebugDraw.h>

class ContentManager;
class EditorSelection;
class HorizonWorld;

namespace HE::Ed::ViewportOverlays
{
	// Unit box around each member of the selection. The primary is the bright
	// one; the rest of a multi-selection get the same amber a shade dimmer, so
	// which one the gizmo will move is visible without reading the outliner.
	void appendSelectionMarkers(HorizonWorld& world, const EditorSelection& selection,
	                            DebugDrawBuffer& out);

	// Every collider in the world: cyan for solid, magenta for triggers.
	// `content` resolves the mesh the Mesh / Convex Hull shapes are built from;
	// a mesh that is not loaded yet is simply skipped this frame.
	void appendColliderWireframes(HorizonWorld& world, ContentManager& content,
	                              DebugDrawBuffer& out);

	constexpr glm::vec3 kPrimarySelectionColor  { 1.0f, 0.8f, 0.0f };
	constexpr glm::vec3 kSecondarySelectionColor{ 0.8f, 0.6f, 0.05f };
	constexpr glm::vec3 kColliderColor          { 0.0f, 1.0f, 1.0f };
	constexpr glm::vec3 kTriggerColor           { 1.0f, 0.0f, 1.0f };
}
