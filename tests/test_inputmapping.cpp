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
