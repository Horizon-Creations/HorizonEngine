#include "HorizonScene/FlyCameraController.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/Components/CameraComponent.h"
#include <Application/Input.h>
#include <algorithm>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace HE {

entt::entity FlyCameraController::findCamera(entt::registry& reg)
{
    // The scene's main camera (prefer isMain; else the first camera found).
    entt::entity cam = entt::null;
    for (auto [e, t, c] : reg.view<TransformComponent, CameraComponent>().each())
    {
        if (cam == entt::null) cam = e;
        if (c.isMain) { cam = e; break; }
    }
    return cam;
}

FlyCameraController::Frame FlyCameraController::update(entt::registry& reg, Input& in, float dt,
                                                      SDL_Window* warpWindow, const Config& cfg)
{
    Frame f;
    if (dt <= 0.0f) return f;

    f.camera = findCamera(reg);

    if (cfg.reassertCapture)
    {
        if (warpWindow && !SDL_GetWindowRelativeMouseMode(warpWindow))
            SDL_SetWindowRelativeMouseMode(warpWindow, true);
        if (SDL_CursorVisible())
            SDL_HideCursor();
    }

    // Nothing to drive (the extractor's fallback camera is fixed).
    if (f.camera == entt::null && !cfg.runWithoutCamera) return f;

    // Mouse look from the relative motion accumulated since last frame. Read exactly
    // once — a second SDL_GetRelativeMouseState() would return zero because reading
    // drains it.
    SDL_GetRelativeMouseState(&f.dx, &f.dy);

    // Park the cursor back at the window centre after each frame's relative motion.
    // With relative mode engaged this is a pure internal position update (SDL generates
    // no motion events for it); when it is NOT engaged (focus transition, platform
    // quirk) the OS cursor physically drifts and would stall the look at the screen
    // edge — the warp keeps it centred either way. SDL pre-sets last_x/last_y to the
    // warp target, so the warp never pollutes the relative accumulator. The caller
    // passes nullptr while unfocused (alt-tabbed) so we never yank the cursor away
    // from another app.
    if (warpWindow)
    {
        int ww = 0, wh = 0;
        SDL_GetWindowSize(warpWindow, &ww, &wh);
        SDL_WarpMouseInWindow(warpWindow, ww * 0.5f, wh * 0.5f);
    }

    if (f.camera == entt::null) return f;
    auto& t = reg.get<TransformComponent>(f.camera);

    t.rotation.y -= f.dx * cfg.sensitivity;  // yaw
    t.rotation.x -= f.dy * cfg.sensitivity;  // pitch
    t.rotation.x = std::clamp(t.rotation.x, -cfg.pitchLimit, cfg.pitchLimit);

    // Movement along the camera's own axes (rotation matches the transform
    // propagation in RenderExtractor: worldMatrix is built from
    // glm::quat(radians(rotation))).
    const glm::quat q = glm::quat(glm::radians(t.rotation));
    const glm::vec3 forward = q * glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 right   = q * glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);

    glm::vec3 move(0.0f);
    if (in.IsKeyDown(SDL_SCANCODE_W)) move += forward;
    if (in.IsKeyDown(SDL_SCANCODE_S)) move -= forward;
    if (in.IsKeyDown(SDL_SCANCODE_D)) move += right;
    if (in.IsKeyDown(SDL_SCANCODE_A)) move -= right;
    if (in.IsKeyDown(SDL_SCANCODE_E) || in.IsKeyDown(SDL_SCANCODE_SPACE)) move += worldUp;
    if (in.IsKeyDown(SDL_SCANCODE_Q) || in.IsKeyDown(SDL_SCANCODE_LCTRL)) move -= worldUp;

    if (glm::dot(move, move) > 0.0f)
    {
        float speed = cfg.moveSpeed;
        if (in.IsKeyDown(SDL_SCANCODE_LSHIFT) || in.IsKeyDown(SDL_SCANCODE_RSHIFT))
            speed *= cfg.sprintMul;
        t.position += glm::normalize(move) * speed * dt;
    }
    t.dirty = true;

    f.driven = true;
    return f;
}

} // namespace HE
