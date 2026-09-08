#pragma once
#include <Math/Math.h>
#include <cstdint>

struct CharacterControllerComponent {
    float     slopeLimit  = 45.0f;   // max walkable slope in degrees
    float     stepHeight  = 0.4f;    // max step-up height (m)
    float     skinWidth   = 0.02f;   // character padding (m)
    float     mass        = 70.0f;   // character mass (kg)
    float     gravity     = 9.81f;   // gravity scale (m/s²)
    // Upward speed a jump starts with (m/s). Peak height is jumpSpeed² / (2·gravity),
    // so the default pair (5, 9.81) clears ~1.27 m — a bit over head height.
    //
    // Read fresh by PhysicsWorld::jumpCharacter() at the moment of the jump, so
    // retuning it during play takes effect on the next jump. That is also why it
    // must stay OUT of the editor's character-rebuild fingerprint (see
    // EditorApplication's PhysInputs): rebuilding the CharacterVirtual for it
    // would throw the player's velocity away for nothing, exactly as noted there
    // for gravity.
    float     jumpSpeed   = 5.0f;

    // The collision channel the CHARACTER walks in — an index into the project's
    // HE::CollisionLayerConfig. It decides what the character is BLOCKED BY:
    // every query the CharacterVirtual makes (its ground check, its slide, its
    // step-up) is filtered through the matrix row of this layer.
    //
    // It is a second field rather than a reuse of RigidBodyComponent's because a
    // character controller does not require a rigid body, and because the two
    // answer different questions on an entity that has both — the normal case,
    // since EntityHost gives every PlayerCharacter a kinematic collision proxy.
    // There, THIS layer is what the player is blocked by, and the proxy body's
    // RigidBodyComponent::collisionLayer is what everything else SEES the player
    // as. Set both when you want the player fully out of a channel.
    //
    // Defaults to CollisionLayerConfig::kCharacter (3) — a name for a thing that
    // otherwise has none. With the default all-true matrix that is the same
    // simulation as before layers existed. Out-of-range values are clamped to 0
    // with a log when the character is built.
    uint8_t   collisionLayer = 3;

    // Runtime state — written back by PhysicsWorld::step()
    glm::vec3 velocity    = {};      // current velocity (m/s), writable
    bool      isGrounded  = false;
    // Seconds of simulated time since the feet last left the ground, and at the
    // same time the COYOTE CREDIT a jump spends: PhysicsWorld::jumpCharacter()
    // allows a jump while this is still inside its grace window, and sets it past
    // the window so the same credit cannot be spent twice. step() advances it per
    // fixed step and zeroes it on landing; a teleport spends it too, because the
    // ground the character was standing on is not under it any more.
    float     airTime     = 0.0f;
};
