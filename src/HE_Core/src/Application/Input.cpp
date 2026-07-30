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
