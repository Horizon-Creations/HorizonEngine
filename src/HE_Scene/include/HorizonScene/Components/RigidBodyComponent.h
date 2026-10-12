#pragma once
#include <Math/Math.h>
#include <Types/Enums.h>
#include <cstdint>

using RigidBodyType = HE::RigidBodyType;

struct RigidBodyComponent {
    RigidBodyType type        = RigidBodyType::Static;
    float         mass        = 1.0f;
    float         friction    = 0.5f;
    float         restitution = 0.3f;
    bool          is2D        = false;   // use 2D physics solver if true

    // Which collision channel this body belongs to — an index into the project's
    // HE::CollisionLayerConfig, whose matrix decides which pairs of channels may
    // touch at all.
    //
    // WHY HERE AND NOT ON ColliderComponent: a body exists exactly when this
    // component does. ColliderComponent is optional — without it the physics
    // falls back to a box from the transform scale — so a layer field over there
    // would be a field an entity with a body sometimes has and sometimes does
    // not. It also sits beside mass/friction/restitution because that is the set
    // Jolt's BodyCreationSettings is filled from, and the layer is one of them.
    //
    // 0 is `Default` for EVERY body, trigger volumes included. The `Trigger`
    // preset is a name offered to whoever wants it, not an automatism: a sensor
    // is still a body built from this component, so forcing its layer would
    // overrule a layer the author explicitly picked. Out-of-range values are
    // clamped to 0 with a log when the body is built.
    uint8_t       collisionLayer = 0;

    // ── Water (Thema 179) ────────────────────────────────────────────────────
    // Both are read once per physics step, not when the body is built, so changing
    // one while the game runs takes effect at the next step without a rebuild of
    // the body (the editor's PlayPhysicsInputs fingerprint does not include them).
    // Neither is written to a scene while it has its default, so a scene without
    // water saves byte for byte as before.
    //
    // false: the water in the scene does nothing to this body — it falls through a
    // lake to the ground like it did before water had physics. For projectiles,
    // debris, anything that should ignore the water.
    bool          affectedByWater = true;
    // kg/m³ for the buoyancy: a body floats when this is below the density of the
    // water (1000 for fresh water) and sinks when it is above, and floats with the
    // share of its volume under water that this is of the water's. 0 = take it from
    // the mass and the volume of the collider. Set it when the mass is chosen for
    // how the body hits things and not for how it swims (wood 500, ice 920, stone
    // 2600, iron 7800): the mass decides collisions, this decides buoyancy.
    float         density = 0.0f;
};
