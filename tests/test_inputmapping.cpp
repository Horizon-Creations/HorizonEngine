#include "doctest.h"
#include <Application/Input.h>
#include <Application/InputMapping.h>

// Helper: inject a synthetic key-down or key-up into an Input instance.
namespace {
    void pressKey(Input& input, SDL_Scancode sc)
    {
        SDL_Event evt{};
        evt.type          = SDL_EVENT_KEY_DOWN;
        evt.key.scancode  = sc;
        evt.key.key       = SDLK_UNKNOWN;
        evt.key.repeat    = 0;
        input.ProcessEvent(evt);
    }

    void releaseKey(Input& input, SDL_Scancode sc)
    {
        SDL_Event evt{};
        evt.type         = SDL_EVENT_KEY_UP;
        evt.key.scancode = sc;
        evt.key.key      = SDLK_UNKNOWN;
        evt.key.repeat   = 0;
        input.ProcessEvent(evt);
    }
}

// ─── mapAction / getAction ────────────────────────────────────────────────────

TEST_CASE("InputMapping: unknown action returns nullptr")
{
    InputMapping im;
    CHECK(im.getAction("Jump") == nullptr);
}

TEST_CASE("InputMapping: mapped action is initially not pressed")
{
    InputMapping im;
    im.mapAction("Jump", { { SDL_SCANCODE_SPACE } });

    Input input;
    im.tick(input);

    const auto* s = im.getAction("Jump");
    REQUIRE(s != nullptr);
    CHECK(!s->isPressed);
    CHECK(!s->justPressed);
    CHECK(!s->justReleased);
}

TEST_CASE("InputMapping: action registers justPressed on key-down frame")
{
    InputMapping im;
    im.mapAction("Jump", { { SDL_SCANCODE_SPACE } });

    Input input;
    im.tick(input); // frame 0: no key

    pressKey(input, SDL_SCANCODE_SPACE);
    im.tick(input); // frame 1: key pressed

    const auto* s = im.getAction("Jump");
    REQUIRE(s != nullptr);
    CHECK(s->isPressed);
    CHECK(s->justPressed);
    CHECK(!s->justReleased);
}

TEST_CASE("InputMapping: justPressed clears on the following frame while held")
{
    InputMapping im;
    im.mapAction("Jump", { { SDL_SCANCODE_SPACE } });

    Input input;
    pressKey(input, SDL_SCANCODE_SPACE);
    im.tick(input); // frame 1: justPressed

    im.tick(input); // frame 2: still held, justPressed clears
    const auto* s = im.getAction("Jump");
    REQUIRE(s != nullptr);
    CHECK(s->isPressed);
    CHECK(!s->justPressed);
    CHECK(!s->justReleased);
}

TEST_CASE("InputMapping: action registers justReleased on key-up frame")
{
    InputMapping im;
    im.mapAction("Jump", { { SDL_SCANCODE_SPACE } });

    Input input;
    pressKey(input, SDL_SCANCODE_SPACE);
    im.tick(input); // frame 1: pressed

    releaseKey(input, SDL_SCANCODE_SPACE);
    im.tick(input); // frame 2: released
    const auto* s = im.getAction("Jump");
    REQUIRE(s != nullptr);
    CHECK(!s->isPressed);
    CHECK(!s->justPressed);
    CHECK(s->justReleased);
}

TEST_CASE("InputMapping: action with multiple bindings triggers on any key")
{
    InputMapping im;
    im.mapAction("Attack", { { SDL_SCANCODE_Z }, { SDL_SCANCODE_X } });

    Input input;
    pressKey(input, SDL_SCANCODE_X);
    im.tick(input);
    CHECK(im.isPressed("Attack"));
}

TEST_CASE("InputMapping: convenience isPressed / justPressed / justReleased helpers")
{
    InputMapping im;
    im.mapAction("Fire", { { SDL_SCANCODE_F } });

    Input input;
    pressKey(input, SDL_SCANCODE_F);
    im.tick(input);
    CHECK(im.isPressed("Fire"));
    CHECK(im.justPressed("Fire"));
    CHECK(!im.justReleased("Fire"));
}

TEST_CASE("InputMapping: unknown action helpers return false/0")
{
    InputMapping im;
    CHECK(!im.isPressed("NoSuchAction"));
    CHECK(!im.justPressed("NoSuchAction"));
    CHECK(!im.justReleased("NoSuchAction"));
    CHECK(im.axisValue("NoSuchAxis") == doctest::Approx(0.0f));
}

// ─── mapAxis / getAxis ────────────────────────────────────────────────────────

TEST_CASE("InputMapping: axis is zero with no keys held")
{
    InputMapping im;
    im.mapAxis("MoveX", { { SDL_SCANCODE_D, SDL_SCANCODE_A } });

    Input input;
    im.tick(input);
    CHECK(im.axisValue("MoveX") == doctest::Approx(0.0f));
}

TEST_CASE("InputMapping: positive key produces +1 on axis")
{
    InputMapping im;
    im.mapAxis("MoveX", { { SDL_SCANCODE_D, SDL_SCANCODE_A } });

    Input input;
    pressKey(input, SDL_SCANCODE_D);
    im.tick(input);
    CHECK(im.axisValue("MoveX") == doctest::Approx(1.0f));
}

TEST_CASE("InputMapping: negative key produces -1 on axis")
{
    InputMapping im;
    im.mapAxis("MoveX", { { SDL_SCANCODE_D, SDL_SCANCODE_A } });

    Input input;
    pressKey(input, SDL_SCANCODE_A);
    im.tick(input);
    CHECK(im.axisValue("MoveX") == doctest::Approx(-1.0f));
}

TEST_CASE("InputMapping: both positive and negative keys cancel to 0")
{
    InputMapping im;
    im.mapAxis("MoveX", { { SDL_SCANCODE_D, SDL_SCANCODE_A } });

    Input input;
    pressKey(input, SDL_SCANCODE_D);
    pressKey(input, SDL_SCANCODE_A);
    im.tick(input);
    CHECK(im.axisValue("MoveX") == doctest::Approx(0.0f));
}

TEST_CASE("InputMapping: axis scale is applied")
{
    InputMapping im;
    im.mapAxis("Throttle", { { SDL_SCANCODE_UP, SDL_SCANCODE_UNKNOWN, 0.5f } });

    Input input;
    pressKey(input, SDL_SCANCODE_UP);
    im.tick(input);
    CHECK(im.axisValue("Throttle") == doctest::Approx(0.5f));
}

TEST_CASE("InputMapping: axis value is clamped to [-1, 1]")
{
    InputMapping im;
    // Two bindings both produce +1 → sum = 2, should clamp to 1
    im.mapAxis("MoveX", {
        { SDL_SCANCODE_D, SDL_SCANCODE_UNKNOWN },
        { SDL_SCANCODE_RIGHT, SDL_SCANCODE_UNKNOWN },
    });

    Input input;
    pressKey(input, SDL_SCANCODE_D);
    pressKey(input, SDL_SCANCODE_RIGHT);
    im.tick(input);
    CHECK(im.axisValue("MoveX") == doctest::Approx(1.0f));
}

// ─── clear / counts ──────────────────────────────────────────────────────────

TEST_CASE("InputMapping: actionCount and axisCount")
{
    InputMapping im;
    CHECK(im.actionCount() == 0);
    CHECK(im.axisCount()   == 0);

    im.mapAction("Jump",  { { SDL_SCANCODE_SPACE } });
    im.mapAction("Fire",  { { SDL_SCANCODE_F } });
    im.mapAxis("MoveX",   { { SDL_SCANCODE_D, SDL_SCANCODE_A } });

    CHECK(im.actionCount() == 2);
    CHECK(im.axisCount()   == 1);
}

TEST_CASE("InputMapping: clear removes all mappings")
{
    InputMapping im;
    im.mapAction("Jump", { { SDL_SCANCODE_SPACE } });
    im.mapAxis("MoveX",  { { SDL_SCANCODE_D, SDL_SCANCODE_A } });
    im.clear();

    CHECK(im.actionCount() == 0);
    CHECK(im.axisCount()   == 0);
    CHECK(im.getAction("Jump") == nullptr);
    CHECK(im.getAxis("MoveX")  == nullptr);
}

TEST_CASE("InputMapping: re-mapping an action replaces its bindings")
{
    InputMapping im;
    im.mapAction("Jump", { { SDL_SCANCODE_SPACE } });
    im.mapAction("Jump", { { SDL_SCANCODE_RETURN } });

    Input input;
    pressKey(input, SDL_SCANCODE_RETURN);
    im.tick(input);
    CHECK(im.isPressed("Jump"));
}

// ─── Input asset glue (Application/InputAssets.h) ─────────────────────────────

#include <Application/InputAssets.h>

TEST_CASE("InputAssets: event names + action name from path")
{
    CHECK(HE::inputEventPressed("IA_Jump")  == "Input.IA_Jump.Pressed");
    CHECK(HE::inputEventReleased("IA_Jump") == "Input.IA_Jump.Released");
    CHECK(HE::inputEventAxis("IA_Move")     == "Input.IA_Move.Axis");
    CHECK(HE::inputActionNameFromPath("Content/Input/IA_Jump.hasset") == "IA_Jump");
}

TEST_CASE("InputAssets: action value type parse is tolerant")
{
    CHECK(HE::inputActionIsAxis(R"({"valueType":"Axis"})"));
    CHECK(!HE::inputActionIsAxis(R"({"valueType":"Button"})"));
    CHECK(!HE::inputActionIsAxis(R"({})"));        // missing → Button
    CHECK(!HE::inputActionIsAxis("not json"));     // malformed → Button
}

TEST_CASE("InputAssets: applyInputMappingContext binds keys and axes")
{
    InputMapping im;
    const std::string json = R"({"entries":[
        {"action":"Content/Input/IA_Jump.hasset","keys":["Space"]},
        {"action":"Content/Input/IA_Move.hasset",
         "axes":[{"positive":"W","negative":"S","scale":1.0}]}
    ]})";
    CHECK(HE::applyInputMappingContext(im, json) == 2);
    CHECK(im.actionCount() == 1);
    CHECK(im.axisCount()   == 1);

    // The bound names are the action-path stems; drive them via a real Input.
    Input input;
    pressKey(input, SDL_SCANCODE_SPACE);
    pressKey(input, SDL_SCANCODE_W);
    im.tick(input);
    CHECK(im.isPressed("IA_Jump"));
    CHECK(im.axisValue("IA_Move") == doctest::Approx(1.0f));
}

// ─── Mouse sources and 2D axes ────────────────────────────────────────────────

TEST_CASE("InputMapping: a mouse-sourced axis carries the frame's delta unclamped")
{
    InputMapping im;
    AxisBinding b; b.source = AxisSource::MouseX; b.scale = 1.0f;
    im.mapAxis("Look", { b });

    Input input;
    // A key axis clamps to ±1; a DELTA does not, or a fast flick would turn no
    // further than a held key — the whole reason mouse look could not be an
    // axis before.
    im.tick(input, { /*dx=*/40.0f, /*dy=*/0.0f, /*wheel=*/0.0f });
    CHECK(im.axisValue("Look") == doctest::Approx(40.0f));

    // It is per-FRAME: nothing carries over into a tick with no movement.
    im.tick(input, {});
    CHECK(im.axisValue("Look") == doctest::Approx(0.0f));

    // …and the scale applies, sign included.
    AxisBinding inv; inv.source = AxisSource::MouseY; inv.scale = -0.5f;
    im.mapAxis("LookY", { inv });
    im.tick(input, { 0.0f, 10.0f, 0.0f });
    CHECK(im.axisValue("LookY") == doctest::Approx(-5.0f));
}

TEST_CASE("InputMapping: keys clamp, the delta adds on top")
{
    // Two key bindings still cannot push a held direction past full deflection,
    // and a mouse source on the same axis is not capped by them.
    InputMapping im;
    AxisBinding k1; k1.positiveKey = SDL_SCANCODE_D;
    AxisBinding k2; k2.positiveKey = SDL_SCANCODE_RIGHT;
    AxisBinding m;  m.source = AxisSource::MouseX;
    im.mapAxis("Turn", { k1, k2, m });

    Input input;
    pressKey(input, SDL_SCANCODE_D);
    pressKey(input, SDL_SCANCODE_RIGHT);
    im.tick(input, {});
    CHECK(im.axisValue("Turn") == doctest::Approx(1.0f));      // 2 keys, still 1

    im.tick(input, { 12.0f, 0.0f, 0.0f });
    CHECK(im.axisValue("Turn") == doctest::Approx(13.0f));     // clamp(2) + 12
}

TEST_CASE("InputMapping: a 2D axis keeps its components apart")
{
    InputMapping im;
    AxisBinding mx; mx.source = AxisSource::MouseX;
    AxisBinding my; my.source = AxisSource::MouseY;
    im.mapAxis2D("Look", { mx }, { my });

    Input input;
    im.tick(input, { 3.0f, -7.0f, 0.0f });
    float x = 0.0f, y = 0.0f;
    im.axis2DValue("Look", x, y);
    CHECK(x == doctest::Approx(3.0f));
    CHECK(y == doctest::Approx(-7.0f));
    // Asking a 2D axis for a single value is a mistake, and reads as zero
    // rather than as one of the two components.
    CHECK(im.axisValue("Look") == doctest::Approx(0.0f));

    // Re-mapping the same name as 1D drops the second component with it.
    AxisBinding k; k.positiveKey = SDL_SCANCODE_D;
    im.mapAxis("Look", { k });
    pressKey(input, SDL_SCANCODE_D);
    im.tick(input, { 3.0f, -7.0f, 0.0f });
    im.axis2DValue("Look", x, y);
    CHECK(x == doctest::Approx(0.0f));
    CHECK(y == doctest::Approx(0.0f));
    CHECK(im.axisValue("Look") == doctest::Approx(1.0f));
}

TEST_CASE("InputAssets: axis sources and 2D axes come out of a mapping context")
{
    InputMapping im;
    // An "axes" row with no "source" is a KEY row — every context written
    // before mouse sources existed says exactly that by leaving it out.
    const size_t bound = HE::applyInputMappingContext(im, R"({"entries":[
        { "action":"IA_Move.hasset", "axes":[{"positive":"W","negative":"S"}] },
        { "action":"IA_Zoom.hasset", "axes":[{"source":"MouseWheel","scale":2.0}] },
        { "action":"IA_Look.hasset",
          "axesX":[{"source":"MouseX"}],
          "axesY":[{"source":"MouseY","scale":-1.0}] }
    ]})");
    CHECK(bound == 3);

    Input input;
    pressKey(input, SDL_SCANCODE_W);
    im.tick(input, { 5.0f, 8.0f, 3.0f });
    CHECK(im.axisValue("IA_Move") == doctest::Approx(1.0f));
    CHECK(im.axisValue("IA_Zoom") == doctest::Approx(6.0f));
    float x = 0.0f, y = 0.0f;
    im.axis2DValue("IA_Look", x, y);
    CHECK(x == doctest::Approx(5.0f));
    CHECK(y == doctest::Approx(-8.0f));
}

TEST_CASE("InputAssets: the three value types are told apart")
{
    CHECK_FALSE(HE::inputActionIsAxis(R"({"valueType":"Button"})"));
    CHECK(HE::inputActionIsAxis(R"({"valueType":"Axis"})"));
    // An Axis2D is NOT an Axis: callers key on that to mean "one float".
    CHECK_FALSE(HE::inputActionIsAxis(R"({"valueType":"Axis2D"})"));
    CHECK(HE::inputActionIsAxis2D(R"({"valueType":"Axis2D"})"));
    // Missing or malformed reads as Button, never as an error.
    CHECK_FALSE(HE::inputActionIsAxis("{}"));
    CHECK_FALSE(HE::inputActionIsAxis2D("nonsense"));
    // Each type has its own event name — a retyped action stops firing the old
    // handler instead of handing it a value of the wrong shape.
    CHECK(HE::inputEventAxis("Look")   == "Input.Look.Axis");
    CHECK(HE::inputEventAxis2D("Look") == "Input.Look.Axis2D");
}

TEST_CASE("InputAssets: malformed mapping JSON binds nothing")
{
    InputMapping im;
    CHECK(HE::applyInputMappingContext(im, "nope") == 0);
    CHECK(HE::applyInputMappingContext(im, R"({"entries":"x"})") == 0);
    // Unknown key names are skipped; an entry with no valid binding counts 0.
    CHECK(HE::applyInputMappingContext(im,
        R"({"entries":[{"action":"IA_X.hasset","keys":["NoSuchKey_123"]}]})") == 0);
    CHECK(im.actionCount() == 0);
}
