#pragma once

// ─── Selection markers and collider wireframes, as lines ─────────────────────
// Overlays the editor's per-frame debug block draws into the viewport (the
// toolbar's Show popup switches each one): the amber box around every selected
// entity, the reach of a selected light or camera, and the cyan/magenta
// outline of every collider.
//
// Out of EditorApplication::OnRender for two reasons. The headless dump never
// runs that block — it renders straight through r->Render() — so a witness
// scene that wants these lines in its picture has to be able to build them by
// the same code, not by a copy of it (HE_DUMP_SELBOXTEST does). And they are
// pure geometry into a DebugDrawBuffer, so the tests can ask where the box
// actually lands without an editor.
//
// Both stand at the WORLD pose, parents included, composed on the spot
// (HE::worldMatrixOf): the icons and meshes they belong to are drawn there,
// and PhysicsWorld builds the bodies there. Reading TransformComponent's own
// (local) position put them beside their entity under any moved parent.

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

	// What a selected light or camera reaches, drawn only while it is selected
	// (a scene full of lights would otherwise be a scene full of spheres):
	//   • point light — a sphere of its `range`;
	//   • spot light — its cone: `range` long along the light's -Z (the
	//     extractor's direction), opening at HALF of `spotAngle` (which is the
	//     full angle, RenderExtractor takes cos(spotAngle/2)), closed by a ring
	//     and two arcs on the sphere of `range`, the edge the light falls off at;
	//   • camera — its view volume: the frustum of `fovDegrees` at
	//     `viewportAspect`, or the orthographic box the extractor builds
	//     (±5 × aspect by ±5). The depth is not the far plane (1000 m would be
	//     a wall across the scene) but screen-constant like the MCP clients'
	//     frustums, never deeper than the far plane itself. `viewer` is the
	//     editor camera's position; a camera the viewer stands in is skipped.
	// Lights are drawn in lightDisplayColor, the colour their icon wears;
	// cameras in kCameraFrustumColor. Directional lights get nothing (their
	// position means nothing), nor do the environment's Sun and Moon, which
	// have no icon either.
	void appendSelectedLightAndCameraShapes(HorizonWorld& world, const EditorSelection& selection,
	                                        const glm::vec3& viewer, float viewportAspect,
	                                        DebugDrawBuffer& out);

	// Segment counts of the shapes above, so a test can count what it asks for.
	constexpr int kRangeSphereSegments = 32;   // per great circle, three circles
	constexpr int kSpotRingSegments    = 32;   // the ring closing the cone
	constexpr int kSpotSideLines       = 8;    // apex to ring
	constexpr int kSpotArcSegments     = 12;   // per arc over the cap, two arcs

	// A mid blue: a paler one vanished against a sunlit floor in the
	// HE_DUMP_LIGHTGIZMOTEST picture, and it must not read as collider cyan.
	constexpr glm::vec3 kCameraFrustumColor     { 0.25f, 0.5f, 1.0f };
	constexpr glm::vec3 kPrimarySelectionColor  { 1.0f, 0.8f, 0.0f };
	constexpr glm::vec3 kSecondarySelectionColor{ 0.8f, 0.6f, 0.05f };
	constexpr glm::vec3 kColliderColor          { 0.0f, 1.0f, 1.0f };
	constexpr glm::vec3 kTriggerColor           { 1.0f, 0.0f, 1.0f };
}
