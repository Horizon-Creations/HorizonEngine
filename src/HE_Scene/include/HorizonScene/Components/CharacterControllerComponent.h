#pragma once
#include <Math/Math.h>

struct CharacterControllerComponent {
    float     slopeLimit  = 45.0f;   // max walkable slope in degrees
    float     stepHeight  = 0.4f;    // max step-up height (m)
    float     skinWidth   = 0.02f;   // character padding (m)
    float     mass        = 70.0f;   // character mass (kg)
    float     gravity     = 9.81f;   // gravity scale (m/s²)

    // Runtime state — written back by PhysicsWorld::step()
    glm::vec3 velocity    = {};      // current velocity (m/s), writable
    bool      isGrounded  = false;
};
