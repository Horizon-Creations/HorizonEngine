#pragma once
#include <Types/UUID.h>
#include <HorizonScene/CameraPose.h>
#include <HorizonScene/CameraShake.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <algorithm>
#include <array>

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
    //
    // FollowSmoothed: the same coupling, but the target turns TOWARDS the
    // camera's yaw at targetTurnRate instead of being set to it. A body that
    // snaps around with the mouse reads as weightless; one that swings after it
    // reads as a body. Not the default, because it puts the character's facing
    // and the player's aim a few degrees apart, and a shooter wants them equal.
    //
    // Appended, never reordered: the serializer stores this as a uint8_t.
    enum class TargetYaw {
        Free,
        Follow,
        FollowSmoothed,
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

    // Degrees per second the target turns towards the camera's yaw, and read by
    // FollowSmoothed alone — Free does not touch the target's rotation and
    // Follow sets it outright. A rate, so the turn takes the same time at any
    // frame rate; the step is clamped to it, never overshoots, and takes the
    // short way round ±180.
    float targetTurnRate = 720.0f;   // degrees/second

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

    // ── Lag ──────────────────────────────────────────────────────────────────
    // Two smoothings that do two different things (see below). Knobs: scene
    // data, serialised. The smoothed values themselves are runtime state and
    // are NOT — see the block after this one.
    //
    // Off by default, unlike collision. Collision has an explicit argument
    // behind it (no physics world → no collision) and its absence reads as a
    // defect; lag has no such argument, and switching it on would shift every
    // existing scene and every existing assertion by a frame. Off means the
    // rig is bit-for-bit what it was before lag existed.
    struct Lag {
        bool  enabled       = false;
        float positionSpeed = 10.0f;  // 1/s, on the pivot; large = tight
        float rotationSpeed = 15.0f;  // 1/s, on armYaw/armPitch only
        float maxDistance   = 2.0f;   // m, cap on how far the pivot may trail
        float snapDistance  = 5.0f;   // m, beyond this it is set, not smoothed
    };
    Lag lag;

    // ── Runtime lag state, NOT serialised ────────────────────────────────────
    // A saved lag pose would become authored content the moment PIE writes its
    // stop snapshot, and a scene would then load with the camera parked
    // wherever a play session happened to leave it.
    //
    // pivotLagged trails the real pivot; the boom is built from IT, and the
    // sphere sweep starts there — smoothing the finished camera pose instead
    // would fight the shortening, with the camera creeping into a wall and back
    // out again while the arm snaps.
    //
    // armYaw/armPitch are the smoothed BOOM direction, and only that. yaw/pitch
    // above stay the raw input state: they are the look direction that goes into
    // the transform and the value the Follow coupling writes to the target. A
    // smoothed look direction would make the mouse feel indirect and would send
    // the character walking somewhere the player is not looking.
    //
    // hasLagState == false means "set, do not smooth, on the next frame". That
    // is the whole snap mechanism: the first frame of a rig, and any explicit
    // snap after a teleport or a cut. Beyond that, a jump larger than
    // snapDistance sets by itself.
    bool      hasLagState = false;
    glm::vec3 pivotLagged{ 0.0f };
    float     armYaw     = 0.0f;
    float     armPitch   = 0.0f;

    // Set the rig's pose hard on the next frame instead of easing into it.
    // For teleports, respawns and cuts, where the script knows before the rig
    // could possibly tell.
    void snap() { hasLagState = false; }

    // ── Camera shake — runtime, NOT serialised ───────────────────────────────
    // A shake is an additive pose offset with an envelope, and it belongs to
    // this play session only. A half-decayed shake saved into a scene by a PIE
    // stop snapshot would be authored content from then on.
    //
    // Fixed capacity so the component stays copyable and allocation-free. A
    // ninth shake replaces the one with the least energy left in it, which for
    // an endless shake is never — those only go away when someone stops them.
    std::array<HE::ShakeInstance, HE::kMaxCameraShakes> shakes{};

    // Handles are dealt out from here. 0 is reserved: it is what a free slot
    // carries, so it can never be a valid handle.
    uint32_t nextShakeId = 1;

    // Start a shake and return its handle. `s.elapsed` and `s.id` are the rig's
    // to fill in; everything else is the caller's description of the shake.
    uint32_t playShake(HE::ShakeInstance s)
    {
        s.elapsed = 0.0f;
        s.id      = nextShakeId++;
        if (nextShakeId == 0) nextShakeId = 1;
        // A caller who did not pick a seed gets one from the handle, so two
        // shakes fired in the same frame do not move in lockstep.
        if (s.seed == 0) s.seed = s.id * 2654435761u + 1u;

        HE::ShakeInstance* slot = nullptr;
        for (auto& e : shakes)
            if (e.id == 0) { slot = &e; break; }

        if (!slot)
        {
            slot = &shakes[0];
            float least = HE::shakeRemainingEnergy(shakes[0]);
            for (auto& e : shakes)
            {
                const float energy = HE::shakeRemainingEnergy(e);
                if (energy < least) { least = energy; slot = &e; }
            }
        }

        *slot = s;
        return s.id;
    }

    // Stop one shake. An unknown or already-finished handle is a no-op, which is
    // what a script holding a handle across a level load needs it to be.
    void stopShake(uint32_t handle)
    {
        if (handle == 0) return;
        for (auto& e : shakes)
            if (e.id == handle) e = HE::ShakeInstance{};
    }

    void stopAllShakes()
    {
        for (auto& e : shakes) e = HE::ShakeInstance{};
    }

    // ── FOV kick — runtime, NOT serialised ───────────────────────────────────
    // CameraComponent::fovDegrees stays the value the author set; the kick lands
    // in CameraComponent::fovOffset, which nothing saves. One slot, because
    // several kicks at once add up to nonsense.
    HE::FovKick fovKick;

    void kickFov(float degrees, float attack, float hold, float decay)
    {
        fovKick           = HE::FovKick{};
        fovKick.amplitude = degrees;
        fovKick.attack    = std::max(0.0f, attack);
        fovKick.hold      = std::max(0.0f, hold);
        fovKick.decay     = std::max(0.0f, decay);
        fovKick.active    = true;
    }

    // ── Blending — runtime, NOT serialised ───────────────────────────────────
    // Lives on the INCOMING rig: it is the one that is driving, and the one that
    // has to know where it is coming from.
    struct Blend {
        // The source camera, re-solved every frame so the blend goes from where
        // the old camera WOULD BE, not from where it stood when the switch
        // happened. entt::null with `useFromPose` set means the source is not an
        // entity at all — see below.
        entt::entity   from      = entt::null;

        // A blend started while another was running has to begin at the pose
        // currently on screen, and that pose is an interpolation between two
        // rigs — not something any entity holds. So it is frozen here. Same for
        // a source camera that carries no rig of its own.
        bool           useFromPose = false;
        HE::SolvedPose fromPose{};

        float          remaining = 0.0f;   // s
        float          duration  = 0.0f;   // s; 0 = not blending
        HE::BlendCurve curve     = HE::BlendCurve::SmoothStep;
    };
    Blend blend;

    bool isBlending() const { return blend.duration > 0.0f && blend.remaining > 0.0f; }

    // The pose this rig last put on screen, in WORLD space — the only output the
    // rig keeps, and deliberately a copy taken before the transform write rather
    // than a read-back of the transform. It is what a second blendTo starts
    // from.
    HE::SolvedPose lastWritten{};
    bool           hasLastWritten = false;

    // Runtime only, not serialised: WHICH entity the rig is currently holding
    // hidden, entt::null for none. Two things depend on it being an entity
    // rather than a flag — telling "I hid this" apart from "the author hid
    // this", and noticing that the target changed. A bool would say "already
    // hiding" when the rig retargets, leaving the old target invisible forever
    // and the new one drawn straight through the camera.
    entt::entity meshHiddenEntity = entt::null;
};
