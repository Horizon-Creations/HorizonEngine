#pragma once
#include <HorizonScene/RootMotion.h>
#include <glm/glm.hpp>
#include <cstdint>

// ── RootMotionComponent ──────────────────────────────────────────────────────
// Whether THIS entity applies the root motion its clips carry, and how.
//
// Two switches, on purpose: AnimationClipAsset::hasRootMotion says whether a clip
// carries any, this says whether the entity acts on it. Without the component
// nothing happens at all, even for a clip that carries motion — which is the
// compatible direction: no existing project changes behaviour by upgrading.
struct RootMotionComponent
{
    enum class Mode : uint8_t
    {
        // Extract and lock still happen (the pose is the same either way), but the
        // component does nothing. Default, so adding the component is not itself a
        // change of behaviour.
        Off = 0,
        // The delta lands on TransformComponent. Props, camera moves, anything
        // that is not a physical figure.
        Transform,
        // The delta becomes a velocity for the next physics step, via
        // PhysicsWorld::setCharacterVelocity — the same call and the same one-frame
        // latency MovementSystem already has, and the LAST writer before the step,
        // so root motion wins over walk input while it is running.
        CharacterController,
    };

    Mode mode = Mode::Off;
    HE::RootMotionOptions options;

    // ── Runtime state, not serialized ────────────────────────────────────────
    // Cleared by rootMotionBeginFrame. An entity may carry more than one pose
    // driver (nothing stops it today); the pose of the last one wins, but two
    // deltas would ADD, which is worse than an overwritten pose. So: one delta per
    // entity per frame, and the second driver says so in the log.
    bool appliedThisFrame = false;

    // A horizontal zero is owed to the character controller. Jolt HOLDS the last
    // velocity until someone overwrites it, MovementSystem only overwrites for
    // entities that have a MovementComponent, and an animator stops writing the
    // moment a non-looping clip clamps — the three together are a figure that
    // glides on for ever. rootMotionEndFrame settles the debt.
    bool wroteVelocity = false;

    // Last applied delta, for the inspector readout and for tests.
    glm::vec3 lastDelta{0.0f};
    float     lastYawDelta = 0.0f;
};
