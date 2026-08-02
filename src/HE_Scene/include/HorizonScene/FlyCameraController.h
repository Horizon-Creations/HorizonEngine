#pragma once
#include <entt/entt.hpp>
#include <SDL3/SDL.h>

class Input;

// Built-in free-fly camera, shared by the packaged game (GameApplication) and
// play-in-editor (EditorApplication) so PIE navigates exactly like a shipped game:
// mouse look from SDL's relative motion + WASD/QE/Space/Ctrl movement along the
// camera's own axes, driving the scene's main camera (isMain, else the first one
// found). Both used to carry their own copy; the look/move maths, the pitch clamp
// and the cursor warp live here now, and the two genuine differences (the editor's
// per-frame capture re-assert and its PIE self-diagnostic) are Config knobs.
namespace HE {

struct FlyCameraConfig
{
    float sensitivity = 0.12f;  // degrees per pixel
    float moveSpeed   = 6.0f;   // units/sec
    float sprintMul   = 3.0f;   // Shift multiplier
    float pitchLimit  = 89.0f;  // pitch clamp, degrees

    // Editor only. Re-assert the capture every frame: SDL engages relative mode only
    // while the *flagged* window holds keyboard focus, and with multi-viewport panels
    // the focus can move between OS windows mid-play — the flag set on one window
    // silently stops engaging when another gains focus. Also re-hides the cursor in
    // case anything slipped past ImGuiConfigFlags_NoMouseCursorChange and re-showed
    // it. The packaged game has a single window and leaves this off.
    bool reassertCapture = false;

    // Editor only. Still drain the relative-motion accumulator and warp when the scene
    // has no camera, so the PIE self-diagnostic sees the raw mouse motion and can
    // report "no camera to drive" *and* whether input arrives at all. The game returns
    // immediately instead — nothing to drive, its render fallback camera is fixed.
    bool runWithoutCamera = false;
};

// What one update() did. The editor's throttled PIE self-diagnostic logs it.
struct FlyCameraFrame
{
    entt::entity camera = entt::null; // camera found (null = the scene has none)
    float dx = 0.0f, dy = 0.0f;       // relative mouse motion consumed this frame
    bool  driven = false;             // the camera transform was actually updated
};

class FlyCameraController
{
public:
    using Config = FlyCameraConfig;
    using Frame  = FlyCameraFrame;

    // One frame of fly-camera input. Callers gate this on their own "is the mouse
    // captured / are we playing" state; `warpWindow` is the window whose centre the
    // cursor is parked at after the delta is read (nullptr = don't warp).
    static Frame update(entt::registry& reg, Input& in, float dt,
                        SDL_Window* warpWindow, const Config& cfg = {});

    // The camera the controller drives: the entity flagged isMain, else the first
    // camera found. entt::null when the scene has none.
    static entt::entity findCamera(entt::registry& reg);
};

} // namespace HE
