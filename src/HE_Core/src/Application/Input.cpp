#include "Application/Input.h"
#include <cstdint>

void Input::ProcessEvent(const SDL_Event& event)
{
    switch (event.type)
    {
    // ── Keyboard ─────────────────────────────────────────────────────────────
    case SDL_EVENT_KEY_DOWN:
    {
        const SDL_Scancode sc = event.key.scancode;
        if (sc < SDL_SCANCODE_COUNT) m_keys[sc] = true;

        KeyEvent e{ event.key.key, sc, event.key.repeat };
        for (auto& cb : m_keyDownCbs) cb(e);
        break;
    }
    case SDL_EVENT_KEY_UP:
    {
        const SDL_Scancode sc = event.key.scancode;
        if (sc < SDL_SCANCODE_COUNT) m_keys[sc] = false;

        KeyEvent e{ event.key.key, sc, false };
        for (auto& cb : m_keyUpCbs) cb(e);
        break;
    }

    // ── Mouse buttons ─────────────────────────────────────────────────────────
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    {
        const uint8_t btn = event.button.button;
        if (btn < k_maxButtons) m_mouseButtons[btn] = true;

        MouseButtonEvent e{ btn, event.button.x, event.button.y };
        for (auto& cb : m_mbDownCbs) cb(e);
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
    {
        const uint8_t btn = event.button.button;
        if (btn < k_maxButtons) m_mouseButtons[btn] = false;

        MouseButtonEvent e{ btn, event.button.x, event.button.y };
        for (auto& cb : m_mbUpCbs) cb(e);
        break;
    }

    // ── Mouse motion ──────────────────────────────────────────────────────────
    case SDL_EVENT_MOUSE_MOTION:
    {
        m_mouseX = event.motion.x;
        m_mouseY = event.motion.y;

        MouseMoveEvent e{ event.motion.x, event.motion.y, event.motion.xrel, event.motion.yrel };
        for (auto& cb : m_mmoveCbs) cb(e);
        break;
    }

    // ── Mouse wheel ───────────────────────────────────────────────────────────
    case SDL_EVENT_MOUSE_WHEEL:
    {
        MouseScrollEvent e{ event.wheel.x, event.wheel.y };
        for (auto& cb : m_mscrollCbs) cb(e);
        break;
    }

    default:
        break;
    }
}
