#include "Application/Input.h"
#include "Diagnostics/Log.h"
#include <cmath>

// ── Deadzone / merge helpers ─────────────────────────────────────────────────

void applyRadialDeadzone(float x, float y, float deadzone, float& outX, float& outY)
{
    const float len = std::sqrt(x * x + y * y);
    if (len <= deadzone || deadzone >= 1.0f)
    {
        outX = 0.0f;
        outY = 0.0f;
        return;
    }
    // Rescale so intensity spans 0..1 from the deadzone edge to full
    // deflection; clamp the length to 1 first — diagonals on square-gated
    // sticks report slightly above unit length.
    const float clamped = len > 1.0f ? 1.0f : len;
    const float scaled  = (clamped - deadzone) / (1.0f - deadzone);
    outX = x / len * scaled;
    outY = y / len * scaled;
}

float applyTriggerDeadzone(float value, float deadzone)
{
    if (value <= deadzone || deadzone >= 1.0f) return 0.0f;
    const float clamped = value > 1.0f ? 1.0f : value;
    return (clamped - deadzone) / (1.0f - deadzone);
}

void mergeGamepadFrame(GamepadFrame& into, const GamepadFrame& from)
{
    into.connected = into.connected || from.connected;
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a)
    {
        if (std::fabs(from.axes[a]) > std::fabs(into.axes[a]))
            into.axes[a] = from.axes[a];
    }
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
        into.buttons[b] = into.buttons[b] || from.buttons[b];
}

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
    // Held mask, assigned not accumulated. The bit index is OUR MouseButton
    // order (left, right, middle — the order input.mouseButton has always
    // used), NOT SDL's button numbering, which puts MIDDLE second. A plain
    // `button - 1` would swap right and middle silently.
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    {
        int bit = -1;
        switch (event.button.button)
        {
        case SDL_BUTTON_LEFT:   bit = kMouseButtonLeft;   break;
        case SDL_BUTTON_RIGHT:  bit = kMouseButtonRight;  break;
        case SDL_BUTTON_MIDDLE: bit = kMouseButtonMiddle; break;
        case SDL_BUTTON_X1:     bit = kMouseButtonX1;     break;
        case SDL_BUTTON_X2:     bit = kMouseButtonX2;     break;
        default: break;
        }
        if (bit >= 0)
        {
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) m_mouse.buttons |= 1u << bit;
            else                                           m_mouse.buttons &= ~(1u << bit);
        }
        break;
    }
    default:
        break;
    }
}

void Input::ProcessGamepadEvent(const SDL_Event& event)
{
    switch (event.type)
    {
    case SDL_EVENT_GAMEPAD_ADDED:
    {
        const SDL_JoystickID id = event.gdevice.which;
        if (m_pads.count(id)) break;
        if (SDL_Gamepad* pad = SDL_OpenGamepad(id))
        {
            m_pads[id] = pad;
            const char* name = SDL_GetGamepadName(pad);
            HE_LOG_INFO(Input, "Gamepad connected: %s (id %d, %d total)",
                        name ? name : "?", (int)id, (int)m_pads.size());
        }
        else
        {
            HE_LOG_WARN(Input, "SDL_OpenGamepad(%d) failed: %s", (int)id, SDL_GetError());
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_REMOVED:
    {
        const SDL_JoystickID id = event.gdevice.which;
        auto it = m_pads.find(id);
        if (it == m_pads.end()) break;
        const char* name = SDL_GetGamepadName(it->second);
        HE_LOG_INFO(Input, "Gamepad disconnected: %s (id %d, %d remaining)",
                    name ? name : "?", (int)id, (int)m_pads.size() - 1);
        SDL_CloseGamepad(it->second);
        m_pads.erase(it);
        // With the last pad gone PollGamepads() early-outs, so the frame must
        // be cleared here or the final polled state would stick forever —
        // a stick held while yanking the cable would keep the character moving.
        if (m_pads.empty()) m_gamepad = GamepadFrame{};
        break;
    }
    default:
        break;
    }
}

void Input::PollGamepads()
{
    // No open pads: leave the frame alone instead of zeroing it. This is what
    // lets SetGamepadFrame() injection (tests, virtual devices) coexist with
    // the per-frame poll — with real pads present, polling owns the frame.
    if (m_pads.empty()) return;
    m_gamepad = GamepadFrame{};
    for (auto& [id, pad] : m_pads)
    {
        GamepadFrame one;
        one.connected = true;
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a)
        {
            const Sint16 raw = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(a));
            // 32767 (not 32768) so full positive deflection reaches exactly
            // 1.0; the asymmetric negative end then lands at -1.00003, which
            // the merge/deadzone path never lets escape past clamping.
            one.axes[a] = static_cast<float>(raw) / 32767.0f;
        }
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
            one.buttons[b] = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b));
        mergeGamepadFrame(m_gamepad, one);
    }
}

float Input::gamepadAxisFiltered(SDL_GamepadAxis axis) const
{
    float x = 0.0f, y = 0.0f;
    switch (axis)
    {
    // Sticks are filtered as PAIRS (radial); asking for one component runs the
    // pair through the deadzone and returns the requested half, so X and Y of
    // the same stick always agree on whether the stick counts as deflected.
    case SDL_GAMEPAD_AXIS_LEFTX:
    case SDL_GAMEPAD_AXIS_LEFTY:
        applyRadialDeadzone(m_gamepad.axes[SDL_GAMEPAD_AXIS_LEFTX],
                            m_gamepad.axes[SDL_GAMEPAD_AXIS_LEFTY],
                            stickDeadzone, x, y);
        return axis == SDL_GAMEPAD_AXIS_LEFTX ? x : y;
    case SDL_GAMEPAD_AXIS_RIGHTX:
    case SDL_GAMEPAD_AXIS_RIGHTY:
        applyRadialDeadzone(m_gamepad.axes[SDL_GAMEPAD_AXIS_RIGHTX],
                            m_gamepad.axes[SDL_GAMEPAD_AXIS_RIGHTY],
                            stickDeadzone, x, y);
        return axis == SDL_GAMEPAD_AXIS_RIGHTX ? x : y;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
        return applyTriggerDeadzone(m_gamepad.axes[axis], triggerDeadzone);
    default:
        return 0.0f;
    }
}
