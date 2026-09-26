#pragma once
#include <cstdint>
#include <string>
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
// swapping controllers mid-session just works, no selection UI. It stays the
// DEFAULT; a local multiplayer session reads the pads one slot at a time
// instead (Input::gamepad(slot), InputDevices below).
HE_API void mergeGamepadFrame(GamepadFrame& into, const GamepadFrame& from);

// Which of Input's devices ONE player reads. The default is everything, merged
// — the single-player view every project had before there were slots.
// `gamepadSlot` -1 = the merged frame, 0.. = only that slot's pad (an empty or
// reserved slot reads as an idle pad). `keyboardMouse` false = keys and the
// handed-in MouseFrame are ignored: on one desk there is one keyboard, and it
// belongs to one player (PlayerHost gives it to player 0).
struct InputDevices
{
    int  gamepadSlot   = -1;
    bool keyboardMouse = true;
};

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

    // ── Pad slots (local multiplayer) ─────────────────────────────────────
    // Every pad that connects gets a SLOT, and the slot is the player: slot 0
    // is player 1's pad, slot 1 player 2's, and the pad's player LED says so
    // (SDL_SetGamepadPlayerIndex). A pad that is pulled keeps its slot
    // RESERVED, so plugging it back in — a loose cable, a flat battery —
    // returns it to the same player instead of shuffling everybody.
    //
    // Which slot a connecting pad gets:
    //   1. a reserved slot that remembers THIS pad (GUID + serial — SDL hands
    //      a reconnected pad a new joystick id, so the id cannot be the key),
    //   2. else the lowest free slot,
    //   3. else the lowest reserved one (its memory is dropped).
    // Two pads of the same model without a serial look alike: if both are
    // pulled and come back, they may come back swapped.
    //
    // The merged frame above is built from the connected slots and stays the
    // default; nothing reads a slot unless it asks.
    static constexpr int kMaxGamepadSlots = 8;
    enum class GamepadSlotState { Free, Connected, Reserved };

    // `slot` -1 = the merged frame (same as gamepad()). A slot out of range,
    // free or reserved reads as an idle, disconnected pad.
    const GamepadFrame& gamepad(int slot) const;
    bool  isGamepadButtonDown(SDL_GamepadButton b, int slot) const;
    float gamepadAxisFiltered(SDL_GamepadAxis axis, int slot) const;
    GamepadSlotState gamepadSlotState(int slot) const;
    // Forget a reserved slot (the player left for good), so the next new pad
    // may take it. False for a connected or free slot — a pad that is
    // plugged in keeps its slot.
    bool releaseGamepadSlot(int slot);

    // Default deadzones. Tunable later via settings; the accessors read these
    // members so a settings slider only has to write them.
    float stickDeadzone   = 0.15f;
    float triggerDeadzone = 0.05f;

    // ── Rumble ────────────────────────────────────────────────────────────
    // Sent to EVERY open pad (every slot, not per player yet), which is the
    // merge policy above read backwards:
    // all pads are one player, so all of them feel what that player feels.
    // Intensities 0..1 (clamped); `low` is the heavy motor, `high` the light
    // one. A pad has ONE effect at a time — a new call replaces the running
    // one, there is no mixing and no handle. `durationMs` 0 runs until
    // stopRumble() or the next call; SDL caps anything else at 65535 ms.
    // Returns true when at least one pad accepted it (false: no pad, or none
    // with motors). Trigger rumble exists on Xbox One/Series and DualSense
    // only; everywhere else it answers false and does nothing.
    bool rumble(float low, float high, uint32_t durationMs);
    bool rumbleTriggers(float left, float right, uint32_t durationMs);
    // Both motors AND both triggers to zero, on every pad.
    void stopRumble();

    // ── Internal — called by Application each frame ───────────────────────
    void ProcessEvent(const SDL_Event& event);
    // Mouse motion and wheel, fed SEPARATELY and ungated. Key events reach
    // ProcessEvent only when the application did not consume them (ImGui gets
    // first refusal); the mouse stream is raw device data and the decision of
    // who may act on it belongs to the consumer — the game always, the editor
    // only while play mode holds the mouse.
    void ProcessMouseEvent(const SDL_Event& event);
    // Hot-plug only (GAMEPAD_ADDED/_REMOVED): opens/closes the SDL handle and
    // hands out / reserves the slot (see "Pad slots" above).
    // Axis/button STATE is not event-accumulated — SDL already maintains it,
    // PollGamepads() reads it once per frame. Fed ungated like the mouse
    // stream: ImGui's "I want the keyboard" does not own a stick either.
    void ProcessGamepadEvent(const SDL_Event& event);
    // Snapshot all open pads into their slots and the merged frame. Called once per frame by
    // Application, right after event polling, so every consumer in the frame
    // sees the same values.
    void PollGamepads();
    // Overwrite the merged frame directly — the injection point for tests and
    // virtual devices. PollGamepads() would rebuild it next frame, so callers
    // that inject must not also poll (an Input with no open pads never does).
    void SetGamepadFrame(const GamepadFrame& frame) { m_gamepad = frame; }
    // The same for ONE slot, and the merged frame is rebuilt from the slots.
    // `frame.connected` decides whether the slot counts as connected, so a
    // test can plug and unplug pads without SDL. A slot holding a real pad is
    // overwritten by the next poll, like the merged frame.
    void SetGamepadFrame(int slot, const GamepadFrame& frame);
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

    struct PadSlot
    {
        GamepadSlotState state = GamepadSlotState::Free;
        SDL_JoystickID   id    = 0;         // while connected
        SDL_Gamepad*     pad   = nullptr;   // while connected to a real device
        std::string      identity;          // GUID/serial, kept while reserved
        GamepadFrame     frame;             // raw, zero unless connected
    };

    float filteredAxis(const GamepadFrame& f, SDL_GamepadAxis axis) const;
    bool  anyOpenPad() const;
    void  remerge();

    // Merged state of all connected slots + the slots with the open SDL
    // handles behind them. An Input constructed without SDL's gamepad
    // subsystem (unit tests, init failure) simply has no open pad and stays
    // all-zero — no SDL calls made.
    GamepadFrame m_gamepad;
    PadSlot      m_slots[kMaxGamepadSlots];
};
