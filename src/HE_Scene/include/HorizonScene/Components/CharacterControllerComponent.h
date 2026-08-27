#pragma once
#include <Math/Math.h>

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
