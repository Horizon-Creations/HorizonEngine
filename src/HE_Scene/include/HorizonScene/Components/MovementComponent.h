#pragma once
#include <Math/Math.h>
#include <cstdint>

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
    // ── What frame the move input is given in ────────────────────────────────
    // The question every third-person game has to answer and this component had
    // no way to: when the player pushes forward, forward according to WHOM?
    //
    // World  — the direction is taken as-is. Right for a fixed camera, a
    //          top-down game, or anything that computes its own facing.
    // Camera — the direction is turned by the main camera rig's yaw before it is
    //          applied, so "forward" is where the camera looks. This is what a
    //          player expects from a third- or first-person game, and there was
    //          previously no setting, node or trick that produced it.
    //
    // orientToMovement is NOT this. That one turns the character to FACE where
    // it is already travelling; it does not change which way "forward" is. The
    // two are usually on together — camera-relative input decides where you go,
    // orientToMovement decides which way you look while going there — and
    // mistaking one for the other is easy, so they say so here.
    //
    // Defaults to World: a MovementComponent added by hand is a bare mover, and
    // a silent frame change would move every existing character differently.
    // A PLAYER character does not get this default — EntityHost::
    // defaultComponents gives PlayerCharacter Camera, because that is what a
    // player is.
    enum class Space : uint8_t { World, Camera };
    Space moveSpace = Space::World;

    // ── Intent, written by gameplay code via locomotion.move / .look ─────────
    // Cleared to zero at the end of each frame, like an input axis: "no call
    // this frame" has to read as "not moving", not as "still moving where it
    // was told an hour ago".
    //
    // Interpreted in `moveSpace`, NOT always world — the name predates the
    // setting and the comment is the correction.
    glm::vec3 moveInput{ 0.0f };   // direction in moveSpace, length 0..1
    float     lookYaw   = 0.0f;    // degrees applied this frame
    float     lookPitch = 0.0f;

    // ── Config ──────────────────────────────────────────────────────────────
    float maxSpeed = 5.0f;         // metres/second at full input
    float turnRate = 720.0f;       // degrees/second when facing the move direction
    // Face the direction of travel automatically. Off when something else owns
    // the facing — a camera rig with coupled rotation, for instance.
    bool  orientToMovement = false;
};
