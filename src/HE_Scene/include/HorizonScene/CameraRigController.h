#pragma once
#include <entt/entt.hpp>

class HorizonWorld;
class PhysicsWorld;
struct MouseFrame;

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
    static Frame update(HorizonWorld& world, const MouseFrame& mouse,
                        entt::entity fallbackTarget = entt::null,
                        const PhysicsWorld* physics = nullptr);

    // The camera this controller drives: an entity with both CameraComponent and
    // CameraRigComponent, preferring isMain. entt::null when there is none.
    static entt::entity findRigCamera(entt::registry& reg);
};

} // namespace HE
