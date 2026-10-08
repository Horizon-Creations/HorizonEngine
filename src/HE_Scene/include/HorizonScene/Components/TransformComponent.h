#pragma once
#include <Math/Math.h>

struct TransformComponent {
    glm::vec3 position    = glm::vec3(0.0f);
    glm::vec3 rotation    = glm::vec3(0.0f);   // Euler angles in degrees
    glm::vec3 scale       = glm::vec3(1.0f);
    bool      dirty       = true;              // set when changed, cleared by RenderExtractor

    glm::mat4 worldMatrix = glm::mat4(1.0f);   // computed, not serialized

    // ── Local-matrix cache (computed, not serialized) ────────────────────────
    // localMatrix() is a quaternion from Euler degrees — three sin/cos pairs —
    // and propagateTransforms used to pay it for every entity on every call,
    // several times a frame (docs/world-streaming-baseline-2026-10-06.md §3.3).
    // Most entities do not move, so the matrix is kept together with the exact
    // position/rotation/scale it was built from and rebuilt only when one of
    // them differs. Compared by VALUE on purpose, not via `dirty`: plenty of
    // writers (an Inspector drag, the gizmo) change position without setting
    // the flag, and a cache that trusted it would draw them where they were.
    glm::mat4 localCache         = glm::mat4(1.0f);
    glm::vec3 localCachePosition = glm::vec3(0.0f);
    glm::vec3 localCacheRotation = glm::vec3(0.0f);
    glm::vec3 localCacheScale    = glm::vec3(1.0f);
    bool      localCacheValid    = false;
};
