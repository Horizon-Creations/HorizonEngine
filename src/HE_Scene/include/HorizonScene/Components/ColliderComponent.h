#pragma once
#include <Types/Enums.h>
#include <Math/Math.h>

using ColliderShape = HE::ColliderShape;

// Explicit collision shape. When present on an entity alongside a
// RigidBodyComponent, the PhysicsWorld uses this shape instead of the
// transform-derived default box. The shape is axis-aligned in the body's
// local frame (Y-up for Capsule).
struct ColliderComponent {
    ColliderShape shape       = ColliderShape::Box;
    glm::vec3     halfExtents = { 0.5f, 0.5f, 0.5f }; // Box: half-size in each axis
    float         radius      = 0.5f;                  // Sphere / Capsule radius
    float         height      = 2.0f;                  // Capsule total height (incl. hemispheres)
    bool          isTrigger   = false;                  // sensor — no collision response
};
