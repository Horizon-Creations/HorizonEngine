#pragma once
#include <Math/Math.h>

// ── MovementComponent ────────────────────────────────────────────────────────
// What a character is DOING, in the form an animator wants to read it.
//
// The point is the seam: an animation state machine needs "how fast", "on the
// ground?", "which way relative to where I face" — and those are questions about
// locomotion, not about physics bodies. Without this, every project rederives
// them from the character controller by hand, differently each time, and an
// animator graph ends up knowing about Jolt.
//
// ── Not a second copy of the character controller ────────────────────────────
// CharacterControllerComponent already owns `velocity` and `isGrounded`; the
// physics step writes them back every tick. Storing them again here would be two
// components claiming one truth, and which one is stale would depend on tick
// order. So this holds only what the controller does NOT: the INTENT (what the
// character was told to do this frame) and the CONFIG (how fast it may go).
//
// Everything derived — speed, grounded, forward/right amounts — is computed on
// read from the controller, by the movement.* API rows. The values below are the
// ones nothing else knows.
struct MovementComponent {
    // ── Intent, written by gameplay code via locomotion.move / .look ─────────
    // Cleared to zero at the end of each frame, like an input axis: "no call
    // this frame" has to read as "not moving", not as "still moving where it
    // was told an hour ago".
    glm::vec3 moveInput{ 0.0f };   // world-space direction, length 0..1
    float     lookYaw   = 0.0f;    // degrees applied this frame
    float     lookPitch = 0.0f;

    // ── Config ──────────────────────────────────────────────────────────────
    float maxSpeed = 5.0f;         // metres/second at full input
    float turnRate = 720.0f;       // degrees/second when facing the move direction
    // Face the direction of travel automatically. Off when something else owns
    // the facing — a camera rig with coupled rotation, for instance.
    bool  orientToMovement = false;
};
