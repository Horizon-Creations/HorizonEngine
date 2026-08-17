#include "doctest.h"
#include <Application/Input.h>
#include <cmath>

// CP0 of gamepad support: the pure device-layer math. No SDL gamepad subsystem
// is initialised here — GamepadFrame is a POD, the deadzone/merge helpers are
// free functions, and SetGamepadFrame injects synthetic state into Input.

// ─── applyRadialDeadzone ─────────────────────────────────────────────────────

TEST_CASE("Radial deadzone: resting stick is exactly zero")
{
    float x = 1.0f, y = 1.0f;
    applyRadialDeadzone(0.0f, 0.0f, 0.15f, x, y);
    CHECK(x == 0.0f);
    CHECK(y == 0.0f);
}

TEST_CASE("Radial deadzone: drift inside the deadzone is exactly zero")
{
    // 0.1 on one axis, 0.1 on the other: length ~0.141 < 0.15. This is the
    // case that matters — PlayerHost fires Axis events every frame, so drift
    // must produce a hard 0.0, not a small value.
    float x = 99.0f, y = 99.0f;
    applyRadialDeadzone(0.1f, 0.1f, 0.15f, x, y);
    CHECK(x == 0.0f);
    CHECK(y == 0.0f);
}

TEST_CASE("Radial deadzone: full deflection stays full")
{
    float x = 0.0f, y = 0.0f;
    applyRadialDeadzone(1.0f, 0.0f, 0.15f, x, y);
    CHECK(x == doctest::Approx(1.0f));
    CHECK(y == doctest::Approx(0.0f));
}

TEST_CASE("Radial deadzone: rescales from the deadzone edge")
{
    // Just past the edge: intensity should be barely above zero, not 0.16.
    float x = 0.0f, y = 0.0f;
    applyRadialDeadzone(0.16f, 0.0f, 0.15f, x, y);
    CHECK(x > 0.0f);
    CHECK(x < 0.02f);
}

TEST_CASE("Radial deadzone: direction is preserved, no diagonal snapping")
{
    // A per-axis deadzone would zero the small component; radial must not.
    float x = 0.0f, y = 0.0f;
    applyRadialDeadzone(0.6f, 0.2f, 0.15f, x, y);
    CHECK(y > 0.0f);
    CHECK(x / y == doctest::Approx(3.0f)); // direction ratio unchanged
}

TEST_CASE("Radial deadzone: over-unit diagonals clamp to unit intensity")
{
    // Square-gated sticks report diagonals slightly above unit length.
    float x = 0.0f, y = 0.0f;
    applyRadialDeadzone(0.9f, 0.9f, 0.15f, x, y);
    const float len = std::sqrt(x * x + y * y);
    CHECK(len == doctest::Approx(1.0f));
}

TEST_CASE("Radial deadzone: negative values keep their sign")
{
    float x = 0.0f, y = 0.0f;
    applyRadialDeadzone(-1.0f, 0.0f, 0.15f, x, y);
    CHECK(x == doctest::Approx(-1.0f));
}

// ─── applyTriggerDeadzone ────────────────────────────────────────────────────

TEST_CASE("Trigger deadzone: resting and inside are zero, full is full")
{
    CHECK(applyTriggerDeadzone(0.0f, 0.05f) == 0.0f);
    CHECK(applyTriggerDeadzone(0.04f, 0.05f) == 0.0f);
    CHECK(applyTriggerDeadzone(1.0f, 0.05f) == doctest::Approx(1.0f));
    CHECK(applyTriggerDeadzone(0.06f, 0.05f) > 0.0f);
    CHECK(applyTriggerDeadzone(0.06f, 0.05f) < 0.02f);
}

// ─── mergeGamepadFrame ───────────────────────────────────────────────────────

TEST_CASE("Merge: larger magnitude wins per axis, sign preserved")
{
    GamepadFrame a, b;
    a.connected = true;
    a.axes[SDL_GAMEPAD_AXIS_LEFTX] = 0.3f;
    b.connected = true;
    b.axes[SDL_GAMEPAD_AXIS_LEFTX] = -0.8f;

    GamepadFrame merged;
    mergeGamepadFrame(merged, a);
    mergeGamepadFrame(merged, b);
    CHECK(merged.axes[SDL_GAMEPAD_AXIS_LEFTX] == doctest::Approx(-0.8f));
    CHECK(merged.connected);
}

TEST_CASE("Merge: buttons OR together")
{
    GamepadFrame a, b;
    a.buttons[SDL_GAMEPAD_BUTTON_SOUTH] = true;
    b.buttons[SDL_GAMEPAD_BUTTON_EAST]  = true;

    GamepadFrame merged;
    mergeGamepadFrame(merged, a);
    mergeGamepadFrame(merged, b);
    CHECK(merged.buttons[SDL_GAMEPAD_BUTTON_SOUTH]);
    CHECK(merged.buttons[SDL_GAMEPAD_BUTTON_EAST]);
    CHECK_FALSE(merged.buttons[SDL_GAMEPAD_BUTTON_WEST]);
}

TEST_CASE("Merge: idle pad does not erase an active one")
{
    GamepadFrame active, idle;
    active.connected = true;
    active.axes[SDL_GAMEPAD_AXIS_RIGHTY] = 0.5f;
    idle.connected = true;

    GamepadFrame merged;
    mergeGamepadFrame(merged, active);
    mergeGamepadFrame(merged, idle);
    CHECK(merged.axes[SDL_GAMEPAD_AXIS_RIGHTY] == doctest::Approx(0.5f));
}

// ─── Input integration (synthetic injection) ─────────────────────────────────

TEST_CASE("Input: default state reports no gamepad")
{
    Input input;
    CHECK_FALSE(input.gamepad().connected);
    CHECK_FALSE(input.isGamepadButtonDown(SDL_GAMEPAD_BUTTON_SOUTH));
    CHECK(input.gamepadAxisFiltered(SDL_GAMEPAD_AXIS_LEFTX) == 0.0f);
}

TEST_CASE("Input: injected frame is visible raw and filtered")
{
    Input input;
    GamepadFrame f;
    f.connected = true;
    f.axes[SDL_GAMEPAD_AXIS_LEFTX] = 1.0f;
    f.buttons[SDL_GAMEPAD_BUTTON_SOUTH] = true;
    input.SetGamepadFrame(f);

    CHECK(input.gamepad().connected);
    CHECK(input.isGamepadButtonDown(SDL_GAMEPAD_BUTTON_SOUTH));
    CHECK(input.gamepadAxisFiltered(SDL_GAMEPAD_AXIS_LEFTX) == doctest::Approx(1.0f));
}

TEST_CASE("Input: raw axis keeps drift, filtered axis removes it")
{
    Input input;
    GamepadFrame f;
    f.connected = true;
    f.axes[SDL_GAMEPAD_AXIS_LEFTX] = 0.08f; // typical resting drift
    input.SetGamepadFrame(f);

    CHECK(input.gamepad().axes[SDL_GAMEPAD_AXIS_LEFTX] == doctest::Approx(0.08f));
    CHECK(input.gamepadAxisFiltered(SDL_GAMEPAD_AXIS_LEFTX) == 0.0f);
}

TEST_CASE("Input: stick components are filtered as a pair")
{
    // X alone is under the deadzone, but the VECTOR (0.12, 0.12) is over it —
    // both components must survive, or diagonals die close to the deadzone.
    Input input;
    GamepadFrame f;
    f.connected = true;
    f.axes[SDL_GAMEPAD_AXIS_LEFTX] = 0.12f;
    f.axes[SDL_GAMEPAD_AXIS_LEFTY] = 0.12f;
    input.SetGamepadFrame(f);

    CHECK(input.gamepadAxisFiltered(SDL_GAMEPAD_AXIS_LEFTX) > 0.0f);
    CHECK(input.gamepadAxisFiltered(SDL_GAMEPAD_AXIS_LEFTY) > 0.0f);
}

TEST_CASE("Input: PollGamepads without pads leaves an injected frame alone")
{
    Input input;
    GamepadFrame f;
    f.connected = true;
    f.axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = 1.0f;
    input.SetGamepadFrame(f);

    input.PollGamepads(); // no SDL pads open → must not wipe the injection
    CHECK(input.gamepadAxisFiltered(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) == doctest::Approx(1.0f));
}

TEST_CASE("Input: EndFrame clears mouse but not gamepad state")
{
    Input input;
    GamepadFrame f;
    f.connected = true;
    f.buttons[SDL_GAMEPAD_BUTTON_START] = true;
    input.SetGamepadFrame(f);

    input.EndFrame();
    CHECK(input.isGamepadButtonDown(SDL_GAMEPAD_BUTTON_START)); // held state, not a displacement
}
