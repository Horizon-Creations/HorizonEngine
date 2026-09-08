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
};
