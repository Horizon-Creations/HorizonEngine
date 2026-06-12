#pragma once
#include <Math/Math.h>
#include <Types/Enums.h>

using RigidBodyType = HE::RigidBodyType;

struct RigidBodyComponent {
    RigidBodyType type        = RigidBodyType::Static;
    float         mass        = 1.0f;
    float         friction    = 0.5f;
    float         restitution = 0.3f;
    bool          is2D        = false;   // use 2D physics solver if true
};
