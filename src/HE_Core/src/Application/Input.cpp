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

namespace
{
// What recognises a pad when it comes back: SDL gives a reconnected device a
// NEW joystick id, so the id is useless for that. The GUID names the model
// (bus, vendor, product, version); the serial, where the driver reports one,
// tells two pads of the same model apart.
std::string padIdentity(SDL_JoystickID id, SDL_Gamepad* pad)
{
    char guid[64] = {};
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), guid, sizeof(guid));
    const char* serial = pad ? SDL_GetGamepadSerial(pad) : nullptr;
    return std::string(guid) + "/" + (serial ? serial : "");
}
}

void Input::ProcessGamepadEvent(const SDL_Event& event)
{
    switch (event.type)
    {
    case SDL_EVENT_GAMEPAD_ADDED:
    {
        const SDL_JoystickID id = event.gdevice.which;
        for (const PadSlot& s : m_slots)
            if (s.state == GamepadSlotState::Connected && s.pad && s.id == id) return;

        SDL_Gamepad* pad = SDL_OpenGamepad(id);
        if (!pad)
        {
            HE_LOG_WARN(Input, "SDL_OpenGamepad(%d) failed: %s", (int)id, SDL_GetError());
            break;
        }
        const std::string identity = padIdentity(id, pad);

        // The slot order from the header: its own reserved slot, then the
        // lowest free one, then the lowest reserved one.
        int slot = -1;
        for (int i = 0; i < kMaxGamepadSlots && slot < 0; ++i)
            if (m_slots[i].state == GamepadSlotState::Reserved && m_slots[i].identity == identity)
                slot = i;
        for (int i = 0; i < kMaxGamepadSlots && slot < 0; ++i)
            if (m_slots[i].state == GamepadSlotState::Free) slot = i;
        for (int i = 0; i < kMaxGamepadSlots && slot < 0; ++i)
            if (m_slots[i].state == GamepadSlotState::Reserved) slot = i;
        if (slot < 0)
        {
            HE_LOG_WARN(Input, "Gamepad %d ignored: all %d pad slots are in use",
                        (int)id, kMaxGamepadSlots);
            SDL_CloseGamepad(pad);
            break;
        }

        PadSlot& s = m_slots[slot];
        const bool returning = s.state == GamepadSlotState::Reserved && s.identity == identity;
        s.state    = GamepadSlotState::Connected;
        s.id       = id;
        s.pad      = pad;
        s.identity = identity;
        s.frame    = GamepadFrame{};
        s.frame.connected = true;
        // The LED/light bar shows the player number where the pad has one.
        // Not every pad (or driver) does; that is not an error.
        SDL_SetGamepadPlayerIndex(pad, slot);
        remerge();

        const char* name = SDL_GetGamepadName(pad);
        HE_LOG_INFO(Input, "Gamepad connected: %s (id %d) -> slot %d%s",
                    name ? name : "?", (int)id, slot, returning ? " (returned)" : "");
        break;
    }
    case SDL_EVENT_GAMEPAD_REMOVED:
    {
        const SDL_JoystickID id = event.gdevice.which;
        for (int i = 0; i < kMaxGamepadSlots; ++i)
        {
            PadSlot& s = m_slots[i];
            if (s.state != GamepadSlotState::Connected || !s.pad || s.id != id) continue;
            const char* name = SDL_GetGamepadName(s.pad);
            HE_LOG_INFO(Input, "Gamepad disconnected: %s (id %d), slot %d reserved",
                        name ? name : "?", (int)id, i);
            SDL_CloseGamepad(s.pad);
            s.state = GamepadSlotState::Reserved;
            s.pad   = nullptr;
            s.id    = 0;
            // Zeroed HERE: the slot is no longer polled, so the last polled
            // state would stick forever — a stick held while yanking the cable
            // would keep that player moving. The merged frame is rebuilt from
            // what is left for the same reason (PollGamepads() early-outs
            // once no pad is open).
            s.frame = GamepadFrame{};
            remerge();
            break;
        }
        break;
    }
    default:
        break;
    }
}

bool Input::anyOpenPad() const
{
    for (const PadSlot& s : m_slots)
        if (s.pad) return true;
    return false;
}

void Input::remerge()
{
    m_gamepad = GamepadFrame{};
    for (const PadSlot& s : m_slots)
        if (s.state == GamepadSlotState::Connected) mergeGamepadFrame(m_gamepad, s.frame);
}

void Input::PollGamepads()
{
    // No open pads: leave the frames alone instead of zeroing them. This is
    // what lets SetGamepadFrame() injection (tests, virtual devices) coexist
    // with the per-frame poll — with real pads present, polling owns them.
    if (!anyOpenPad()) return;
    for (PadSlot& s : m_slots)
    {
        if (!s.pad) continue;
        GamepadFrame& one = s.frame;
        one.connected = true;
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a)
        {
            const Sint16 raw = SDL_GetGamepadAxis(s.pad, static_cast<SDL_GamepadAxis>(a));
            // 32767 (not 32768) so full positive deflection reaches exactly
            // 1.0; the asymmetric negative end then lands at -1.00003, which
            // the merge/deadzone path never lets escape past clamping.
            one.axes[a] = static_cast<float>(raw) / 32767.0f;
        }
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
            one.buttons[b] = SDL_GetGamepadButton(s.pad, static_cast<SDL_GamepadButton>(b));
    }
    remerge();
}

void Input::SetGamepadFrame(int slot, const GamepadFrame& frame)
{
    if (slot < 0 || slot >= kMaxGamepadSlots) return;
    PadSlot& s = m_slots[slot];
    s.frame = frame;
    // A slot with a real pad stays that pad's; an injected one follows the
    // frame's connected flag, and "unplugging" it reserves it like a real one.
    if (!s.pad)
        s.state = frame.connected ? GamepadSlotState::Connected
                : s.state == GamepadSlotState::Free ? GamepadSlotState::Free
                                                     : GamepadSlotState::Reserved;
    if (!frame.connected && !s.pad) s.frame = GamepadFrame{};
    remerge();
}

const GamepadFrame& Input::gamepad(int slot) const
{
    static const GamepadFrame kIdle{};
    if (slot < 0) return m_gamepad;
    if (slot >= kMaxGamepadSlots || m_slots[slot].state != GamepadSlotState::Connected) return kIdle;
    return m_slots[slot].frame;
}

bool Input::isGamepadButtonDown(SDL_GamepadButton b, int slot) const
{
    return b >= 0 && b < SDL_GAMEPAD_BUTTON_COUNT && gamepad(slot).buttons[b];
}

Input::GamepadSlotState Input::gamepadSlotState(int slot) const
{
    if (slot < 0 || slot >= kMaxGamepadSlots) return GamepadSlotState::Free;
    return m_slots[slot].state;
}

bool Input::releaseGamepadSlot(int slot)
{
    if (slot < 0 || slot >= kMaxGamepadSlots) return false;
    PadSlot& s = m_slots[slot];
    if (s.state != GamepadSlotState::Reserved) return false;
    s = PadSlot{};
    return true;
}

namespace
{
Uint16 rumbleLevel(float v)
{
    // NaN fails both comparisons and lands on 0 — a garbage intensity from a
    // script must not become full power.
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f)   return 0xFFFF;
    return static_cast<Uint16>(v * 65535.0f + 0.5f);
}
}

bool Input::rumble(float low, float high, uint32_t durationMs)
{
    bool any = false;
    for (const PadSlot& s : m_slots)
        if (s.pad) any |= SDL_RumbleGamepad(s.pad, rumbleLevel(low), rumbleLevel(high), durationMs);
    return any;
}

bool Input::rumbleTriggers(float left, float right, uint32_t durationMs)
{
    bool any = false;
    for (const PadSlot& s : m_slots)
        if (s.pad)
            any |= SDL_RumbleGamepadTriggers(s.pad, rumbleLevel(left), rumbleLevel(right), durationMs);
    return any;
}

void Input::stopRumble()
{
    // Triggers too, unconditionally: on a pad without them SDL just says no,
    // and asking is cheaper than remembering which pads were told to buzz.
    for (const PadSlot& s : m_slots)
    {
        if (!s.pad) continue;
        SDL_RumbleGamepad(s.pad, 0, 0, 0);
        SDL_RumbleGamepadTriggers(s.pad, 0, 0, 0);
    }
}

float Input::gamepadAxisFiltered(SDL_GamepadAxis axis) const
{
    return filteredAxis(m_gamepad, axis);
}

float Input::gamepadAxisFiltered(SDL_GamepadAxis axis, int slot) const
{
    return filteredAxis(gamepad(slot), axis);
}

float Input::filteredAxis(const GamepadFrame& f, SDL_GamepadAxis axis) const
{
    float x = 0.0f, y = 0.0f;
    switch (axis)
    {
    // Sticks are filtered as PAIRS (radial); asking for one component runs the
    // pair through the deadzone and returns the requested half, so X and Y of
    // the same stick always agree on whether the stick counts as deflected.
    case SDL_GAMEPAD_AXIS_LEFTX:
    case SDL_GAMEPAD_AXIS_LEFTY:
        applyRadialDeadzone(f.axes[SDL_GAMEPAD_AXIS_LEFTX],
                            f.axes[SDL_GAMEPAD_AXIS_LEFTY],
                            stickDeadzone, x, y);
        return axis == SDL_GAMEPAD_AXIS_LEFTX ? x : y;
    case SDL_GAMEPAD_AXIS_RIGHTX:
    case SDL_GAMEPAD_AXIS_RIGHTY:
        applyRadialDeadzone(f.axes[SDL_GAMEPAD_AXIS_RIGHTX],
                            f.axes[SDL_GAMEPAD_AXIS_RIGHTY],
                            stickDeadzone, x, y);
        return axis == SDL_GAMEPAD_AXIS_RIGHTX ? x : y;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
        return applyTriggerDeadzone(f.axes[axis], triggerDeadzone);
    default:
        return 0.0f;
    }
}
