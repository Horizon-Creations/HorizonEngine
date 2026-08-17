#pragma once
#include <cstdint>
#include <unordered_map>
#include "Types/Defines.h"
#include <SDL3/SDL.h>

// What the mouse did during one frame: movement since the last frame and wheel
// travel, both as DISPLACEMENTS. Which is the whole point — they are "how far",
// not "how fast", so game code must never multiply them by delta time the way it
// correctly does for a held key or a stick.
//
// `buttons` is the exception in here: a HELD-state mask (bit = MouseButton),
// not a displacement — EndFrame clears the movement and keeps the mask. It
// rides in the frame rather than on Input because the frame is what call sites
// already gate by ownership (the editor hands PlayerHost a blank MouseFrame
// outside capture), and a mouse-button action binding must obey the same gate
// as mouse movement — otherwise clicking editor UI would fire game actions.
struct MouseFrame
{
    float dx = 0.0f, dy = 0.0f;
    float wheel = 0.0f;
    uint32_t buttons = 0;
};

// Bit indices for MouseFrame::buttons, in the order every consumer already
// numbers them (EngineApi's input.mouseButton, UI hit-testing): 0 = left.
enum MouseButton : int
{
    kMouseButtonLeft   = 0,
    kMouseButtonRight  = 1,
    kMouseButtonMiddle = 2,
    kMouseButtonX1     = 3,
    kMouseButtonX2     = 4,
    kMouseButtonCount  = 5,
};

// What every connected gamepad amounts to this frame, merged into one virtual
// pad: sticks in [-1,1], triggers in [0,1], both RAW — no deadzone applied, so
// calibration UI and script queries can see the true hardware values. Consumers
// that feed gameplay go through Input::gamepadAxisFiltered() instead, which is
// where the deadzone lives. Unlike MouseFrame these are held STATES, not
// displacements — a stick is dt-scaled by whoever turns it into motion, exactly
// like a held key.
struct GamepadFrame
{
    bool  connected = false;
    float axes[SDL_GAMEPAD_AXIS_COUNT]{};
    bool  buttons[SDL_GAMEPAD_BUTTON_COUNT]{};
};

// ── Deadzone / merge helpers ──────────────────────────────────────────────────
// Free functions so the math is unit-testable without SDL devices.

// Radial deadzone for a stick PAIR. Per-axis deadzones produce cross-shaped
// dead regions and snap diagonals to the axes; treating the vector's length is
// what every shipped controller scheme does. Inside the deadzone both axes
// become exactly 0.0 (PlayerHost fires Axis events every frame — resting drift
// must not keep graphs "alive"); outside, the remaining range is rescaled so
// intensity still spans 0..1 from the deadzone edge to full deflection.
HE_API void applyRadialDeadzone(float x, float y, float deadzone, float& outX, float& outY);

// Scalar deadzone for a trigger: 0 below, rescaled to 0..1 above.
HE_API float applyTriggerDeadzone(float value, float deadzone);

// Merge one pad's state into the combined frame: per axis the larger magnitude
// wins, buttons OR together. This is the "all pads are one player" policy —
// swapping controllers mid-session just works, no selection UI. Per-player
// assignment (splitscreen) is a later, separate step.
HE_API void mergeGamepadFrame(GamepadFrame& into, const GamepadFrame& from);

// ── Input class ───────────────────────────────────────────────────────────────
// Keyboard state plus the frame's raw mouse movement. SDL scancodes are used
// directly because SDL is already a public dependency of HorizonCore.
//
// The mouse part is deliberately only the DEVICE stream — no buttons, no
// position, no notion of who is allowed to act on it. Absolute position and
// buttons are read where they are needed (ImGui in the editor, SDL_GetMouseState
// in the game); what could not be read anywhere was the movement, because
// SDL_GetRelativeMouseState DRAINS on read and the fly camera already consumes
// it. Accumulating from motion EVENTS is a second, independent stream, so this
// takes nothing away from that one.
class HE_API Input
{
public:
    // ── Polling ───────────────────────────────────────────────────────────
    // Per-frame check of the key state maintained by ProcessEvent.

    bool IsKeyDown(SDL_Scancode sc) const { return sc < SDL_SCANCODE_COUNT && m_keys[sc]; }

    // This frame's mouse movement. Readable as often as wanted — it is cleared
    // once, at the frame boundary, not by reading it.
    const MouseFrame& mouse() const { return m_mouse; }

    // The merged, RAW gamepad state (see GamepadFrame). Gameplay consumers use
    // the filtered accessors below; raw is for calibration UI and scripts.
    const GamepadFrame& gamepad() const { return m_gamepad; }

    bool isGamepadButtonDown(SDL_GamepadButton b) const
    {
        return b >= 0 && b < SDL_GAMEPAD_BUTTON_COUNT && m_gamepad.buttons[b];
    }

    // Deadzone-filtered axis value: sticks radially (as pairs), triggers
    // scalar. This is what InputMapping consumes.
    float gamepadAxisFiltered(SDL_GamepadAxis axis) const;

    // Default deadzones. Tunable later via settings; the accessors read these
    // members so a settings slider only has to write them.
    float stickDeadzone   = 0.15f;
    float triggerDeadzone = 0.05f;

    // ── Internal — called by Application each frame ───────────────────────
    void ProcessEvent(const SDL_Event& event);
    // Mouse motion and wheel, fed SEPARATELY and ungated. Key events reach
    // ProcessEvent only when the application did not consume them (ImGui gets
    // first refusal); the mouse stream is raw device data and the decision of
    // who may act on it belongs to the consumer — the game always, the editor
    // only while play mode holds the mouse.
    void ProcessMouseEvent(const SDL_Event& event);
    // Hot-plug only (GAMEPAD_ADDED/_REMOVED): opens/closes the SDL handle.
    // Axis/button STATE is not event-accumulated — SDL already maintains it,
    // PollGamepads() reads it once per frame. Fed ungated like the mouse
    // stream: ImGui's "I want the keyboard" does not own a stick either.
    void ProcessGamepadEvent(const SDL_Event& event);
    // Snapshot all open pads into the merged frame. Called once per frame by
    // Application, right after event polling, so every consumer in the frame
    // sees the same values.
    void PollGamepads();
    // Overwrite the merged frame directly — the injection point for tests and
    // virtual devices. PollGamepads() would rebuild it next frame, so callers
    // that inject must not also poll (an Input with no open pads never does).
    void SetGamepadFrame(const GamepadFrame& frame) { m_gamepad = frame; }
    // Clear the frame's movement. Called by Application after the frame is
    // rendered, so everything drawing that frame sees the same numbers.
    // The button mask survives — it is a held state like a key, not a
    // displacement; the gamepad frame is NOT cleared either (rebuilt from
    // device state by PollGamepads(), not accumulated from events).
    void EndFrame() { const uint32_t held = m_mouse.buttons; m_mouse = MouseFrame{}; m_mouse.buttons = held; }

private:
    // Per-frame state
    bool       m_keys[SDL_SCANCODE_COUNT]{};
    MouseFrame m_mouse;

    // Merged state of all connected pads + the open SDL handles behind it.
    // An Input constructed without SDL's gamepad subsystem (unit tests, init
    // failure) simply has an empty map and stays all-zero — no SDL calls made.
    GamepadFrame m_gamepad;
    std::unordered_map<SDL_JoystickID, SDL_Gamepad*> m_pads;
};
