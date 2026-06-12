#pragma once
#include "Types/Defines.h"
#include <SDL3/SDL.h>
#include <functional>
#include <vector>

// ── Event structs ─────────────────────────────────────────────────────────────
// These are the types passed to registered callbacks.
// SDL scancodes / keycodes are used directly because SDL is already a
// public dependency of HorizonCore.

struct HE_API KeyEvent
{
    SDL_Keycode  key;        // logical key (e.g. SDLK_W)
    SDL_Scancode scancode;   // physical location
    bool         repeat;     // true for held-key repeats
};

struct HE_API MouseButtonEvent
{
    uint8_t button;          // SDL_BUTTON_LEFT / _RIGHT / _MIDDLE …
    float   x, y;            // cursor position at the time of the event
};

struct HE_API MouseMoveEvent
{
    float x,  y;             // absolute cursor position
    float dx, dy;            // relative motion this event
};

struct HE_API MouseScrollEvent
{
    float dx, dy;            // scroll delta (dy > 0 = scroll up)
};

// ── Input class ───────────────────────────────────────────────────────────────

class HE_API Input
{
public:
    // ── Callback registration ─────────────────────────────────────────────
    // Multiple callbacks can be registered per event type.

    using KeyCallback         = std::function<void(const KeyEvent&)>;
    using MouseButtonCallback = std::function<void(const MouseButtonEvent&)>;
    using MouseMoveCallback   = std::function<void(const MouseMoveEvent&)>;
    using MouseScrollCallback = std::function<void(const MouseScrollEvent&)>;

    void OnKeyDown        (KeyCallback         cb) { m_keyDownCbs.push_back(std::move(cb)); }
    void OnKeyUp          (KeyCallback         cb) { m_keyUpCbs.push_back(std::move(cb)); }
    void OnMouseButtonDown(MouseButtonCallback cb) { m_mbDownCbs.push_back(std::move(cb)); }
    void OnMouseButtonUp  (MouseButtonCallback cb) { m_mbUpCbs.push_back(std::move(cb)); }
    void OnMouseMove      (MouseMoveCallback   cb) { m_mmoveCbs.push_back(std::move(cb)); }
    void OnMouseScroll    (MouseScrollCallback cb) { m_mscrollCbs.push_back(std::move(cb)); }

    // ── Polling ───────────────────────────────────────────────────────────
    // Useful for per-frame checks without registering a callback.

    bool IsKeyDown        (SDL_Scancode sc)      const { return sc < SDL_SCANCODE_COUNT && m_keys[sc]; }
    bool IsMouseButtonDown(uint8_t button)       const { return button < k_maxButtons && m_mouseButtons[button]; }
    void GetMousePosition (float& x, float& y)  const { x = m_mouseX; y = m_mouseY; }

    // ── Internal — called by Application each frame ───────────────────────
    void ProcessEvent(const SDL_Event& event);

private:
    static constexpr int k_maxButtons = 6;

    // Per-frame state
    bool  m_keys[SDL_SCANCODE_COUNT]{};
    bool  m_mouseButtons[k_maxButtons]{};
    float m_mouseX = 0.0f;
    float m_mouseY = 0.0f;

    // Registered callbacks
    std::vector<KeyCallback>         m_keyDownCbs;
    std::vector<KeyCallback>         m_keyUpCbs;
    std::vector<MouseButtonCallback> m_mbDownCbs;
    std::vector<MouseButtonCallback> m_mbUpCbs;
    std::vector<MouseMoveCallback>   m_mmoveCbs;
    std::vector<MouseScrollCallback> m_mscrollCbs;
};