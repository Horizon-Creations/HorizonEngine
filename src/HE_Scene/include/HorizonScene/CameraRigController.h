#pragma once
#include <entt/entt.hpp>
#include <Application/Input.h>   // MouseFrame — embedded in CameraLookInput

class HorizonWorld;
class PhysicsWorld;

// Drives a camera that carries a CameraRigComponent: first person or third
// person, following a target entity.
//
// Sibling of FlyCameraController and shaped like it — static, no state of its
// own, shared by the packaged game and play-in-editor so PIE behaves like a
// shipped game. What state there is lives in the component, on the camera.
//
// ── Where this runs in the frame ─────────────────────────────────────────────
// AFTER physics, not before. A follow camera that runs before the step reads
// the position its target had last frame, and that lag is visible. The fly
// camera does not care because nothing else moves it.
//
// ── Mouse input ──────────────────────────────────────────────────────────────
// It reads Input::mouse(), the event-accumulated stream, NOT
// SDL_GetRelativeMouseState — that one drains on read and the fly camera
// already consumes it. The two are independent, so both can exist.
// The delta is a DISPLACEMENT: never scale it by delta time.
namespace HE {

// What one update() did, for callers that want to log or gate on it.
struct CameraRigFrame
{
    entt::entity camera = entt::null;  // rig camera found (null = scene has none)
    entt::entity target = entt::null;  // resolved target (null = nothing to follow)
    bool         driven = false;       // the camera transform was actually written
    bool         occluded = false;     // the boom was shortened by geometry
};

// One frame of look input, both device families in their own units — because
// they ARE different units and conflating them is the classic bug:
//   mouse  = a DISPLACEMENT (degrees per pixel via `sensitivity`); scaling it
//            by dt would make the same flick turn further at a lower framerate.
//   stick  = a RATE (deflection -1..+1, degrees per second via
//            `stickSensitivity`); NOT scaling it by dt would make the turn
//            speed framerate-dependent. The rig applies dt itself — that is
//            what `dt` is here for.
// Stick values are expected deadzone-filtered (Input::gamepadAxisFiltered);
// SDL convention: stickY positive is DOWNWARD, matching mouse dy.
struct CameraLookInput
{
    MouseFrame mouse;
    float stickX = 0.0f;
    float stickY = 0.0f;
    float dt     = 0.0f;
};

class CameraRigController
{
public:
    using Frame = CameraRigFrame;

    // One frame of rig update. Returns driven == false when there is no rig
    // camera or its target cannot be resolved — that is the caller's signal to
    // fall back to the fly camera.
    //
    // `fallbackTarget` is used when the component's target id is empty; pass the
    // player character the PlayerHost possesses so a default project needs no
    // assignment. entt::null = no fallback.
    //
    // Propagates world transforms itself, so the target's world position is this
    // frame's regardless of where the caller sits in the frame.
    //
    // `physics` is what the third-person boom sweeps against so it does not put
    // the camera inside a wall. Null (edit mode, tests, a scene without physics)
    // simply means no collision — the boom keeps its full length.
    static Frame update(HorizonWorld& world, const CameraLookInput& look,
                        entt::entity fallbackTarget = entt::null,
                        const PhysicsWorld* physics = nullptr);

    // Mouse-only convenience — the pre-gamepad signature, kept so tests and
    // callers without a pad don't have to spell out a CameraLookInput.
    static Frame update(HorizonWorld& world, const MouseFrame& mouse,
                        entt::entity fallbackTarget = entt::null,
                        const PhysicsWorld* physics = nullptr)
    {
        CameraLookInput look;
        look.mouse = mouse;
        return update(world, look, fallbackTarget, physics);
    }

    // The camera this controller drives: an entity with both CameraComponent and
    // CameraRigComponent, preferring isMain. entt::null when there is none.
    static entt::entity findRigCamera(entt::registry& reg);
};

} // namespace HE
