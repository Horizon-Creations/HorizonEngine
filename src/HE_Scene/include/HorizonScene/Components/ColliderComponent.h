#pragma once
#include <Types/Enums.h>
#include <Math/Math.h>

using ColliderShape = HE::ColliderShape;

// Explicit collision shape. When present on an entity alongside a
// RigidBodyComponent, the PhysicsWorld uses this shape instead of the
// transform-derived default box. The shape is axis-aligned in the body's
// local frame (Y-up for Capsule).
//
// Only some of the numbers below apply to any one shape — the rest are simply
// unread, which is why the Details panel shows a different set per shape:
//   Box          halfExtents
//   Sphere       radius
//   Capsule      radius + height
//   Mesh         none — the geometry IS the entity's mesh (LOD0)
//   ConvexHull   none — the hull of that same mesh
//   HeightField  none — the entity's TerrainComponent height field
//
// The last three deliberately carry NO source field of their own. Pointing a
// collider at some other asset (the low-poly collision mesh an artist ships
// beside the render mesh) needs a UUID here, a serializer field, a picker row
// and a manual entry to go with it; until all four exist a field would be data
// that silently resets on save. "The entity's own geometry" covers the case the
// readiness audit named — the imported glTF house that collides as a crate.
struct ColliderComponent {
    ColliderShape shape       = ColliderShape::Box;
    glm::vec3     halfExtents = { 0.5f, 0.5f, 0.5f }; // Box: half-size in each axis
    float         radius      = 0.5f;                  // Sphere / Capsule radius
    float         height      = 2.0f;                  // Capsule total height (incl. hemispheres)
    bool          isTrigger   = false;                  // sensor — no collision response
};
