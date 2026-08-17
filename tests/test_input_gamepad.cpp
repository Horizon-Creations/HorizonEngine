#include "doctest.h"
#include <Application/Input.h>
#include <Application/InputMapping.h>
#include <Application/InputAssets.h>
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

// ─── InputMapping with gamepad sources (CP1) ─────────────────────────────────

namespace {
    Input inputWithFrame(const GamepadFrame& f)
    {
        Input input;
        input.SetGamepadFrame(f);
        return input;
    }
}

TEST_CASE("Mapping: gamepad button triggers an action with edges")
{
    InputMapping m;
    m.mapAction("Jump", { { SDL_SCANCODE_SPACE, SDL_GAMEPAD_BUTTON_SOUTH } });

    GamepadFrame f;
    f.connected = true;
    f.buttons[SDL_GAMEPAD_BUTTON_SOUTH] = true;
    Input input = inputWithFrame(f);

    m.tick(input);
    CHECK(m.isPressed("Jump"));
    CHECK(m.justPressed("Jump"));

    f.buttons[SDL_GAMEPAD_BUTTON_SOUTH] = false;
    input.SetGamepadFrame(f);
    m.tick(input);
    CHECK_FALSE(m.isPressed("Jump"));
    CHECK(m.justReleased("Jump"));
}

TEST_CASE("Mapping: key-only bindings ignore the gamepad, old spellings intact")
{
    InputMapping m;
    m.mapAction("Fire", { { SDL_SCANCODE_F } }); // pre-gamepad brace-init

    GamepadFrame f;
    f.connected = true;
    f.buttons[SDL_GAMEPAD_BUTTON_SOUTH] = true;
    Input input = inputWithFrame(f);

    m.tick(input);
    CHECK_FALSE(m.isPressed("Fire"));
}

TEST_CASE("Mapping: stick source feeds an axis, deadzone already applied")
{
    InputMapping m;
    AxisBinding stick;
    stick.source = AxisSource::GamepadLeftX;
    m.mapAxis("Steer", { stick });

    GamepadFrame f;
    f.connected = true;
    f.axes[SDL_GAMEPAD_AXIS_LEFTX] = 1.0f;
    Input input = inputWithFrame(f);

    m.tick(input);
    CHECK(m.axisValue("Steer") == doctest::Approx(1.0f));

    f.axes[SDL_GAMEPAD_AXIS_LEFTX] = 0.05f; // drift, inside deadzone
    input.SetGamepadFrame(f);
    m.tick(input);
    CHECK(m.axisValue("Steer") == 0.0f);
}

TEST_CASE("Mapping: stick plus key on the same axis clamps to full deflection")
{
    InputMapping m;
    AxisBinding keys{ SDL_SCANCODE_D, SDL_SCANCODE_A };
    AxisBinding stick;
    stick.source = AxisSource::GamepadLeftX;
    m.mapAxis("Move", { keys, stick });

    GamepadFrame f;
    f.connected = true;
    f.axes[SDL_GAMEPAD_AXIS_LEFTX] = 1.0f;
    Input input = inputWithFrame(f);
    SDL_Event evt{};
    evt.type         = SDL_EVENT_KEY_DOWN;
    evt.key.scancode = SDL_SCANCODE_D;
    input.ProcessEvent(evt);

    m.tick(input);
    CHECK(m.axisValue("Move") == doctest::Approx(1.0f)); // not 2.0
}

TEST_CASE("Mapping: negative scale flips SDL's downward-positive stick Y")
{
    InputMapping m;
    AxisBinding stickY;
    stickY.source = AxisSource::GamepadLeftY;
    stickY.scale  = -1.0f; // up-positive for gameplay
    m.mapAxis("MoveY", { stickY });

    GamepadFrame f;
    f.connected = true;
    f.axes[SDL_GAMEPAD_AXIS_LEFTY] = -1.0f; // stick pushed UP (SDL: negative)
    Input input = inputWithFrame(f);

    m.tick(input);
    CHECK(m.axisValue("MoveY") == doctest::Approx(1.0f));
}

TEST_CASE("Mapping: D-pad button pair drives an axis like keys")
{
    InputMapping m;
    AxisBinding dpad;
    dpad.positiveButton = SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    dpad.negativeButton = SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    m.mapAxis("Move", { dpad });

    GamepadFrame f;
    f.connected = true;
    f.buttons[SDL_GAMEPAD_BUTTON_DPAD_LEFT] = true;
    Input input = inputWithFrame(f);

    m.tick(input);
    CHECK(m.axisValue("Move") == doctest::Approx(-1.0f));
}

TEST_CASE("Mapping: trigger source spans 0..1 on an axis")
{
    InputMapping m;
    AxisBinding trig;
    trig.source = AxisSource::GamepadRightTrigger;
    m.mapAxis("Throttle", { trig });

    GamepadFrame f;
    f.connected = true;
    f.axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = 0.5f;
    Input input = inputWithFrame(f);

    m.tick(input);
    const float v = m.axisValue("Throttle");
    CHECK(v > 0.4f);
    CHECK(v < 0.6f);
}

TEST_CASE("Mapping: Axis2D from both stick components")
{
    InputMapping m;
    AxisBinding sx, sy;
    sx.source = AxisSource::GamepadLeftX;
    sy.source = AxisSource::GamepadLeftY;
    sy.scale  = -1.0f;
    m.mapAxis2D("Move", { sx }, { sy });

    GamepadFrame f;
    f.connected = true;
    f.axes[SDL_GAMEPAD_AXIS_LEFTX] = 0.7f;
    f.axes[SDL_GAMEPAD_AXIS_LEFTY] = -0.7f; // up
    Input input = inputWithFrame(f);

    m.tick(input);
    float x = 0.0f, y = 0.0f;
    m.axis2DValue("Move", x, y);
    CHECK(x > 0.5f);
    CHECK(y > 0.5f);
}

TEST_CASE("Assets: gamepadButtons array binds an action, pad-only entries work")
{
    InputMapping m;
    const char* json = R"({"entries":[
        {"action":"Input/Jump.hasset","keys":["Space"],"gamepadButtons":["a"]},
        {"action":"Input/Dash.hasset","gamepadButtons":["leftshoulder"]}
    ]})";
    CHECK(HE::applyInputMappingContext(m, json) == 2);

    GamepadFrame f;
    f.connected = true;
    f.buttons[SDL_GAMEPAD_BUTTON_SOUTH] = true;
    f.buttons[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = true;
    Input input = inputWithFrame(f);

    m.tick(input);
    CHECK(m.isPressed("Jump"));
    CHECK(m.isPressed("Dash"));
}

TEST_CASE("Assets: stick source and pad-button axis rows parse")
{
    InputMapping m;
    const char* json = R"({"entries":[
        {"action":"Input/Move.hasset","axesX":[
            {"source":"GamepadLeftX","positive":"","negative":"","scale":1.0},
            {"source":"Key","positiveButton":"dpright","negativeButton":"dpleft","scale":1.0}
        ],"axesY":[
            {"source":"GamepadLeftY","positive":"","negative":"","scale":-1.0}
        ]}
    ]})";
    CHECK(HE::applyInputMappingContext(m, json) == 1);

    GamepadFrame f;
    f.connected = true;
    f.axes[SDL_GAMEPAD_AXIS_LEFTY] = -1.0f; // up
    f.buttons[SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = true;
    Input input = inputWithFrame(f);

    m.tick(input);
    float x = 0.0f, y = 0.0f;
    m.axis2DValue("Move", x, y);
    CHECK(x == doctest::Approx(1.0f));  // D-pad right
    CHECK(y == doctest::Approx(1.0f));  // stick up, scale -1 flips SDL's sign
}

TEST_CASE("Assets: unknown gamepad names are skipped, not errors")
{
    InputMapping m;
    const char* json = R"({"entries":[
        {"action":"Input/Jump.hasset","gamepadButtons":["not_a_button","a"]},
        {"action":"Input/Aim.hasset","axes":[{"source":"GamepadNoSuchAxis","positive":"E"}]}
    ]})";
    // Both entries still bind: the bad button is dropped, the unknown source
    // falls back to Key (existing convention) and keeps its key.
    CHECK(HE::applyInputMappingContext(m, json) == 2);

    GamepadFrame f;
    f.connected = true;
    f.buttons[SDL_GAMEPAD_BUTTON_SOUTH] = true;
    Input input = inputWithFrame(f);
    m.tick(input);
    CHECK(m.isPressed("Jump"));
}

TEST_CASE("Assets: round-trip through name tables covers every source")
{
    using HE::axisSourceName;
    using HE::axisSourceFromName;
    for (AxisSource s : { AxisSource::Key, AxisSource::MouseX, AxisSource::MouseY,
                          AxisSource::MouseWheel,
                          AxisSource::GamepadLeftX, AxisSource::GamepadLeftY,
                          AxisSource::GamepadRightX, AxisSource::GamepadRightY,
                          AxisSource::GamepadLeftTrigger, AxisSource::GamepadRightTrigger })
        CHECK(axisSourceFromName(axisSourceName(s)) == s);
}

TEST_CASE("Mapping: gamepad sources are not delta sources")
{
    CHECK_FALSE(axisSourceIsDelta(AxisSource::GamepadLeftX));
    CHECK_FALSE(axisSourceIsDelta(AxisSource::GamepadRightY));
    CHECK_FALSE(axisSourceIsDelta(AxisSource::GamepadLeftTrigger));
    CHECK(axisSourceIsDelta(AxisSource::MouseX));
    CHECK(axisSourceIsDelta(AxisSource::MouseWheel));
    CHECK_FALSE(axisSourceIsDelta(AxisSource::Key));
}

// ─── Mouse buttons as action bindings ────────────────────────────────────────

TEST_CASE("Mouse: button events fill the held mask in OUR order, not SDL's")
{
    Input input;
    auto press = [&](Uint8 sdlButton, bool down){
        SDL_Event evt{};
        evt.type          = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        evt.button.button = sdlButton;
        input.ProcessMouseEvent(evt);
    };
    // SDL numbers LEFT=1, MIDDLE=2, RIGHT=3 — our mask is left/RIGHT/middle.
    press(SDL_BUTTON_RIGHT, true);
    CHECK((input.mouse().buttons & (1u << kMouseButtonRight)) != 0);
    CHECK((input.mouse().buttons & (1u << kMouseButtonMiddle)) == 0);
    press(SDL_BUTTON_MIDDLE, true);
    CHECK((input.mouse().buttons & (1u << kMouseButtonMiddle)) != 0);
    press(SDL_BUTTON_RIGHT, false);
    CHECK((input.mouse().buttons & (1u << kMouseButtonRight)) == 0);

    // EndFrame clears the movement, keeps the held mask — a button is a held
    // state, not a displacement.
    input.EndFrame();
    CHECK((input.mouse().buttons & (1u << kMouseButtonMiddle)) != 0);
}

TEST_CASE("Mapping: mouse-button binding obeys the handed-in frame (ownership gate)")
{
    InputMapping m;
    ActionBinding mb;
    mb.mouseButton = kMouseButtonLeft;
    m.mapAction("Fire", { mb });

    Input input;
    MouseFrame held;
    held.buttons = 1u << kMouseButtonLeft;

    // Same Input, same click — with the frame handed in it fires, with a
    // blank frame (the editor outside play capture) it must not.
    m.tick(input, held);
    CHECK(m.isPressed("Fire"));
    m.tick(input, MouseFrame{});
    CHECK_FALSE(m.isPressed("Fire"));
    CHECK(m.justReleased("Fire"));
}

TEST_CASE("Assets: mouseButtons array parses, unknown names skipped")
{
    InputMapping m;
    const char* json = R"({"entries":[
        {"action":"Input/Fire.hasset","mouseButtons":["left","nope","x2"]}
    ]})";
    CHECK(HE::applyInputMappingContext(m, json) == 1);

    Input input;
    MouseFrame frame;
    frame.buttons = 1u << kMouseButtonX2;
    m.tick(input, frame);
    CHECK(m.isPressed("Fire"));
}

TEST_CASE("Assets: mouse button name table round-trips, display names exist")
{
    for (int b = 0; b < kMouseButtonCount; ++b)
    {
        CHECK(HE::mouseButtonFromName(HE::mouseButtonName(b)) == b);
        CHECK(HE::mouseButtonDisplayName(b) != "?");
    }
    CHECK(HE::mouseButtonFromName("bogus") == -1);
    // Every SDL pad button has a readable label (worst case its SDL name).
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
        CHECK_FALSE(HE::gamepadButtonDisplayName(static_cast<SDL_GamepadButton>(b)).empty());
}

TEST_CASE("Assets: an axesX-only entry still registers as a 2D axis")
{
    // The editor writes axesX (no axesY yet) for an entry whose ACTION is
    // declared Axis2D — the loader must register it 2D, or axis2DValue()
    // answers 0,0 for a mapping that clearly has an X binding.
    InputMapping m;
    const char* json = R"({"entries":[
        {"action":"Input/Move.hasset","axesX":[{"source":"GamepadLeftX","scale":1.0}]}
    ]})";
    CHECK(HE::applyInputMappingContext(m, json) == 1);

    GamepadFrame f;
    f.connected = true;
    f.axes[SDL_GAMEPAD_AXIS_LEFTX] = 1.0f;
    Input input = inputWithFrame(f);
    m.tick(input);

    float x = 0.0f, y = 0.0f;
    m.axis2DValue("Move", x, y);
    CHECK(x == doctest::Approx(1.0f));
    CHECK(m.axisValue("Move") == 0.0f); // and NOT as a 1D axis
}

// ─── End to end: SDL virtual gamepad ─────────────────────────────────────────
// A real SDL device (no hardware, works headless — verified on macOS) through
// the REAL plumbing: hot-plug event → Input opens the pad → PollGamepads reads
// it → InputMapping maps it. This is the test that would catch a broken event
// route or axis normalisation, which frame injection by design cannot.

TEST_CASE("End-to-end: virtual pad drives Input and InputMapping via hot-plug")
{
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
    {
        // Headless CI without device support: not a failure of OUR code.
        MESSAGE("SDL gamepad subsystem unavailable here (", SDL_GetError(),
                ") — end-to-end covered by frame injection only");
        return;
    }

    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type     = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes    = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    const SDL_JoystickID vid = SDL_AttachVirtualJoystick(&desc);
    REQUIRE(vid != 0);

    Input input;
    // Route the queued events the way Application's callback does. The ADDED
    // event is what makes Input open the pad — nothing is opened by hand here.
    auto pump = [&]{
        SDL_UpdateJoysticks();
        SDL_Event e;
        while (SDL_PollEvent(&e)) input.ProcessGamepadEvent(e);
    };
    pump();
    input.PollGamepads();
    REQUIRE(input.gamepad().connected);

    // Drive the virtual hardware and read it through the whole stack.
    SDL_Gamepad*  pad = SDL_GetGamepadFromID(vid);
    REQUIRE(pad != nullptr);
    SDL_Joystick* js  = SDL_GetGamepadJoystick(pad);
    SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_SOUTH, true);
    pump();
    input.PollGamepads();

    CHECK(input.gamepad().axes[SDL_GAMEPAD_AXIS_LEFTX] == doctest::Approx(1.0f));
    CHECK(input.isGamepadButtonDown(SDL_GAMEPAD_BUTTON_SOUTH));

    InputMapping m;
    m.mapAction("Jump", { { SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_SOUTH } });
    AxisBinding stick;
    stick.source = AxisSource::GamepadLeftX;
    m.mapAxis("Move", { stick });
    m.tick(input);
    CHECK(m.isPressed("Jump"));
    CHECK(m.axisValue("Move") == doctest::Approx(1.0f));

    // Unplug: the REMOVED event must close the pad AND zero the frame — a
    // stick held while the cable is yanked must not keep the character moving.
    SDL_DetachVirtualJoystick(vid);
    pump();
    input.PollGamepads();
    CHECK_FALSE(input.gamepad().connected);
    CHECK(input.gamepad().axes[SDL_GAMEPAD_AXIS_LEFTX] == 0.0f);

    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}
