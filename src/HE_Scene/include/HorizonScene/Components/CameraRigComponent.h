#pragma once
#include <Types/UUID.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

// ── Camera rig ───────────────────────────────────────────────────────────────
// Puts a camera on a target entity: first person (in its head) or third person
// (on a boom behind it). Sits on the CAMERA entity, not on the target — the
// camera is what it configures, and a scene can carry several rigs and switch
// isMain between them.
//
// One component for both modes, because they are one calculation with a
// different arm length rather than two behaviours:
//
//     pivot  = targetWorldPosition + pivotOffset
//     rot    = quat(pitch, yaw)
//     camPos = pivot + rot * (armOffset - forward * armLength)
//
// First person is armLength == 0. There is no second branch to keep in sync.
struct CameraRigComponent {

    enum class Mode {
        FirstPerson,
        ThirdPerson,
    };

    // What the rig does with the TARGET's rotation.
    //
    // Free: it does not touch it. The camera orbits, the character turns on its
    // own — the usual third-person feel, where the character faces the way it
    // walks and the camera is free to look elsewhere.
    //
    // Follow: the target's yaw is the camera's yaw, so the character always
    // faces where the camera looks. That is what makes strafing and walking
    // backwards possible, and it is the normal case for first person, which is
    // why this is not a third-person-only switch.
    enum class TargetYaw {
        Free,
        Follow,
    };

    Mode      mode = Mode::ThirdPerson;

    // The entity to follow, by stable id — an entt handle would not survive a
    // save/load round trip, which is what EntityIdComponent exists for.
    // Empty = follow the player character the PlayerHost possesses, so a normal
    // project never has to assign anything.
    HE::UUID  target;

    // Eye / shoulder height above the target's origin, in WORLD axes — it is
    // deliberately not rotated by the target. With Follow coupling the rig
    // writes the target's yaw, so a pivot that rotated with the target would
    // feed the rig's own output back into its input.
    glm::vec3 pivotOffset = { 0.0f, 1.6f, 0.0f };

    // Third person only. Shoulder offset in CAMERA space (x = right), and how
    // far back the boom sits.
    glm::vec3 armOffset = { 0.4f, 0.0f, 0.0f };
    float     armLength = 4.0f;

    // Where the rig is looking. Runtime state, but serialised: it is the
    // camera's starting direction on load, and losing it would snap every
    // reloaded scene back to due north.
    float yaw   = 0.0f;
    float pitch = -10.0f;

    float sensitivity = 0.12f;    // degrees per pixel of mouse motion
    // Right-stick look: degrees per SECOND at full deflection — a rate, where
    // the mouse sensitivity above is per pixel of displacement. The two are
    // separate knobs because they are separate units; one number could not be
    // right for both. 180 = a half turn per second at full tilt.
    float stickSensitivity = 180.0f;
    bool  stickInvertY     = false;
    float pitchMin    = -80.0f;
    float pitchMax    =  75.0f;

    TargetYaw targetYaw = TargetYaw::Free;

    // Third person only. Sweep a sphere from the pivot out to where the camera
    // wants to be and stop it at the first solid thing, so the boom does not put
    // the view inside a wall. On by default: a camera that drives through
    // geometry does not read as "collision is off", it reads as broken.
    //
    // The sphere's radius IS the clearance the camera keeps from surfaces —
    // sweeping stops the sphere's centre one radius short. Too small and the
    // near plane still clips; too large and the camera shoves forward in
    // corridors it would have fit through.
    bool  collision       = true;
    float collisionRadius = 0.2f;

    // First person: hide the target's own mesh so the player is not looking at
    // the inside of their own head. Shadow casting is left on — the character
    // still has a shadow, it just is not drawn.
    bool hideTargetMesh = true;

    // Runtime only, not serialised: WHICH entity the rig is currently holding
    // hidden, entt::null for none. Two things depend on it being an entity
    // rather than a flag — telling "I hid this" apart from "the author hid
    // this", and noticing that the target changed. A bool would say "already
    // hiding" when the rig retargets, leaving the old target invisible forever
    // and the new one drawn straight through the camera.
    entt::entity meshHiddenEntity = entt::null;
};
