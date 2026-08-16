#pragma once

class HorizonWorld;
class PhysicsWorld;

// Turns MovementComponent's intent into character-controller motion, once per
// frame, in the GAMEPLAY phase — before physics steps and well before the
// animation phase reads the result.
//
// It is deliberately thin. It does not hold state, does not decide what a
// character wants, and does not touch the animator: it takes the move/look a
// graph asked for this frame, hands the velocity to the character controller,
// turns the character if it is set to face its travel, and clears the intent so
// the next frame starts from silence.
namespace MovementSystem {
    // `physics` may be null (no physics world yet, or edit mode) — the intent is
    // still consumed and the facing still applies, so a character does not
    // snap-turn when physics arrives.
    void update(HorizonWorld& world, PhysicsWorld* physics, float dt);
}
