#include "Application/Input.h"

void Input::ProcessEvent(const SDL_Event& event)
{
    switch (event.type)
    {
    // ── Keyboard ─────────────────────────────────────────────────────────────
    case SDL_EVENT_KEY_DOWN:
    {
        const SDL_Scancode sc = event.key.scancode;
        if (sc < SDL_SCANCODE_COUNT) m_keys[sc] = true;
        break;
    }
    case SDL_EVENT_KEY_UP:
    {
        const SDL_Scancode sc = event.key.scancode;
        if (sc < SDL_SCANCODE_COUNT) m_keys[sc] = false;
        break;
    }

    default:
        break;
    }
}

void Input::ProcessMouseEvent(const SDL_Event& event)
{
    switch (event.type)
    {
    // Accumulated, not assigned: one frame can carry several motion events, and
    // a fast movement is exactly the case where it does. Taking only the last
    // one would quietly throw away most of a flick.
    case SDL_EVENT_MOUSE_MOTION:
        m_mouse.dx += event.motion.xrel;
        m_mouse.dy += event.motion.yrel;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        m_mouse.wheel += event.wheel.y;
        break;
    default:
        break;
    }
}
