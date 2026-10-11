#pragma once
#include <Math/Math.h>

// A bool that a COPY does not carry over: the copy reads false, a move keeps the
// value. Used for TransformComponent::localCacheValid, see there.
struct TransformCacheFlag {
    bool value = false;

    constexpr TransformCacheFlag() = default;
    constexpr TransformCacheFlag(const TransformCacheFlag&) noexcept {}   // a copy starts out "not valid"
    constexpr TransformCacheFlag& operator=(const TransformCacheFlag&) noexcept { value = false; return *this; }
    constexpr TransformCacheFlag(TransformCacheFlag&&) noexcept = default;
    constexpr TransformCacheFlag& operator=(TransformCacheFlag&&) noexcept = default;

    constexpr TransformCacheFlag& operator=(bool v) noexcept { value = v; return *this; }
    constexpr operator bool() const noexcept { return value; }
};

struct TransformComponent {
    glm::vec3 position    = glm::vec3(0.0f);
    glm::vec3 rotation    = glm::vec3(0.0f);   // Euler angles in degrees
    glm::vec3 scale       = glm::vec3(1.0f);
    // Set by writers that remember to (scripts, physics sync, animation, the sequencer ...),
    // cleared by HE::propagateTransforms when it has recomputed the world matrix. A hint only:
    // propagateTransforms honours it, but never relies on it (see localCache below, which is
    // what actually catches a write that did not set it).
    bool      dirty       = true;

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
    //
    // The same comparison is how propagateTransforms finds the entities whose
    // world matrix has to be recomputed (docs/render-extractor-shadow-pass-plan.md
    // section 7): "the cache does not match" means "this entity changed since the last
    // pass", and everything below it follows.
    //
    // localCacheValid does not survive a copy (TransformCacheFlag). A copy of a whole
    // component (a saved pose written back with `*tc = saved`) carries a worldMatrix
    // from wherever it was taken, and an entity whose cache still matched would be
    // skipped with it; a copy that starts out invalid is always picked up.
    glm::mat4 localCache         = glm::mat4(1.0f);
    glm::vec3 localCachePosition = glm::vec3(0.0f);
    glm::vec3 localCacheRotation = glm::vec3(0.0f);
    glm::vec3 localCacheScale    = glm::vec3(1.0f);
    TransformCacheFlag localCacheValid;
};
