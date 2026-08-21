#include "doctest.h"
#include <Application/Input.h>
#include <Application/InputMapping.h>
#include "InputMappingModel.h"   // the editor-side model: where a pressed input lands

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

TEST_CASE("InputAssets: an action fires during a pause only when it says so")
{
    // Off unless explicitly true — an action authored before the switch existed
    // must go silent during a pause, which is the whole point of pausing.
    CHECK_FALSE(HE::inputActionRunsWhilePaused(R"({"valueType":"Button"})"));
    CHECK_FALSE(HE::inputActionRunsWhilePaused("{}"));
    CHECK_FALSE(HE::inputActionRunsWhilePaused("not json"));
    CHECK_FALSE(HE::inputActionRunsWhilePaused(R"({"runWhilePaused":false})"));
    CHECK(HE::inputActionRunsWhilePaused(R"({"runWhilePaused":true})"));
    // Only a real boolean counts: a stringly-typed "true" is not a yes.
    CHECK_FALSE(HE::inputActionRunsWhilePaused(R"({"runWhilePaused":"true"})"));
    CHECK_FALSE(HE::inputActionRunsWhilePaused(R"({"runWhilePaused":1})"));

    // The writer round-trips through both readers, so the editor cannot save a
    // payload the loader reads differently.
    const std::string j = HE::makeInputActionJson("Axis2D", true);
    CHECK(HE::inputActionIsAxis2D(j));
    CHECK(HE::inputActionRunsWhilePaused(j));
    const std::string b = HE::makeInputActionJson("Button", false);
    CHECK_FALSE(HE::inputActionIsAxis(b));
    CHECK_FALSE(HE::inputActionIsAxis2D(b));
    CHECK_FALSE(HE::inputActionRunsWhilePaused(b));
    // An empty type is a Button, not an unreadable asset.
    CHECK_FALSE(HE::inputActionIsAxis(HE::makeInputActionJson("", false)));
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

// ─── Editor: binding by pressing the thing ───────────────────────────────────
// The Input Mapping panel used to carry a single Bind button per key field and
// one card-level Auto Detect that could only ever APPEND. Every binding row has
// its own Bind now, and which row a press lands in is what these pin — the
// panel around it is ImGui and cannot be asserted headless.

using HE::Ed::AxisRow;
using HE::Ed::BindSlot;
using HE::Ed::DetectedBinding;
using HE::Ed::MapEntry;

namespace {
    DetectedBinding key(const char* n)   { DetectedBinding d; d.key = n; return d; }
    DetectedBinding pad(const char* n)   { DetectedBinding d; d.gamepadButton = n; return d; }
    DetectedBinding mouse(const char* n) { DetectedBinding d; d.mouseButton = n; return d; }
    DetectedBinding stick(AxisSource s)
    { DetectedBinding d; d.axisSource = static_cast<int>(s); return d; }

    // A Button entry with one of each kind of binding.
    MapEntry buttonEntry()
    {
        MapEntry e;
        e.actionPath = "Input/IA_Fire.hasset";
        e.valueType  = 0;
        e.keys           = { "Space", "F" };
        e.gamepadButtons = { "a", "x" };
        e.mouseButtons   = { "left" };
        return e;
    }
}

TEST_CASE("InputBinding: every row binds its own slot, not the first one")
{
    MapEntry e = buttonEntry();

    // Second key row, not the first: the whole point of a per-row Bind.
    CHECK(HE::Ed::applyDetected(e, BindSlot::Key, 1, key("G")));
    CHECK(e.keys[0] == "Space");
    CHECK(e.keys[1] == "G");
    CHECK(e.keys.size() == 2);        // replaced, not appended

    CHECK(HE::Ed::applyDetected(e, BindSlot::PadButton, 1, pad("dpup")));
    CHECK(e.gamepadButtons[0] == "a");
    CHECK(e.gamepadButtons[1] == "dpup");

    CHECK(HE::Ed::applyDetected(e, BindSlot::MouseButton, 0, mouse("right")));
    CHECK(e.mouseButtons[0] == "right");
    CHECK(e.mouseButtons.size() == 1);
}

TEST_CASE("InputBinding: a row only takes the device it can hold")
{
    MapEntry e = buttonEntry();

    // A key press at a pad row would have to be written as a pad-button name —
    // it is refused instead, and the row keeps waiting.
    CHECK_FALSE(HE::Ed::applyDetected(e, BindSlot::PadButton, 0, key("Space")));
    CHECK(e.gamepadButtons[0] == "a");
    CHECK_FALSE(HE::Ed::applyDetected(e, BindSlot::Key, 0, pad("b")));
    CHECK(e.keys[0] == "Space");
    CHECK_FALSE(HE::Ed::applyDetected(e, BindSlot::MouseButton, 0, key("Space")));
    CHECK(e.mouseButtons[0] == "left");

    // Nothing pressed at all is not a binding either.
    CHECK_FALSE(HE::Ed::applyDetected(e, BindSlot::Key, 0, DetectedBinding{}));
    CHECK_FALSE(HE::Ed::bindSlotAccepts(BindSlot::None, key("Space")));
}

TEST_CASE("InputBinding: a row index that no longer exists binds nothing")
{
    MapEntry e = buttonEntry();
    CHECK_FALSE(HE::Ed::applyDetected(e, BindSlot::Key, 7, key("G")));
    CHECK_FALSE(HE::Ed::applyDetected(e, BindSlot::Key, -1, key("G")));
    CHECK_FALSE(HE::Ed::applyDetected(e, BindSlot::AxisKeyPos, 0, key("G")));
    CHECK(e.keys.size() == 2);
}

TEST_CASE("InputBinding: the X and Y axis lists never answer for each other")
{
    MapEntry e;
    e.valueType = 2;
    AxisRow x; x.uiPadRow = true; x.positiveButton = "dpright"; x.negativeButton = "dpleft";
    AxisRow y; y.uiPadRow = true; y.positiveButton = "dpup";    y.negativeButton = "dpdown";
    e.axes.push_back(x);
    e.axesY.push_back(y);

    // Row indices are rowBase-encoded, exactly as the panel addresses them.
    CHECK(HE::Ed::applyDetected(e, BindSlot::AxisPadPos, HE::Ed::kAxisRowBaseX + 0, pad("b")));
    CHECK(e.axes[0].positiveButton == "b");
    CHECK(e.axesY[0].positiveButton == "dpup");     // the Y row is untouched

    CHECK(HE::Ed::applyDetected(e, BindSlot::AxisPadNeg, HE::Ed::kAxisRowBaseY + 0, pad("y")));
    CHECK(e.axesY[0].negativeButton == "y");
    CHECK(e.axes[0].negativeButton == "dpleft");

    // …and the key halves of a key-source pair.
    AxisRow k; e.axes.push_back(k);
    CHECK(HE::Ed::applyDetected(e, BindSlot::AxisKeyPos, HE::Ed::kAxisRowBaseX + 1, key("D")));
    CHECK(HE::Ed::applyDetected(e, BindSlot::AxisKeyNeg, HE::Ed::kAxisRowBaseX + 1, key("A")));
    CHECK(e.axes[1].positive == "D");
    CHECK(e.axes[1].negative == "A");
}

TEST_CASE("InputBinding: rebinding an axis row's device drops the pair it no longer reads")
{
    MapEntry e;
    e.valueType = 1;
    AxisRow r; r.positive = "D"; r.negative = "A"; r.positiveButton = "dpright";
    e.axes.push_back(r);

    // A stick press at the pair fields is not a pair binding…
    CHECK_FALSE(HE::Ed::applyDetected(e, BindSlot::AxisKeyPos, 0, stick(AxisSource::GamepadLeftX)));
    // …but at the row's source it rebinds which device the row reads. The
    // runtime reads a Key row's pairs whatever the source says, so they have to
    // go with it or they would keep binding invisibly.
    CHECK(HE::Ed::applyDetected(e, BindSlot::AxisSource, 0, stick(AxisSource::GamepadLeftX)));
    CHECK(e.axes[0].source == AxisSource::GamepadLeftX);
    CHECK(e.axes[0].positive.empty());
    CHECK(e.axes[0].negative.empty());
    CHECK(e.axes[0].positiveButton.empty());
    CHECK_FALSE(e.axes[0].uiPadRow);
}

TEST_CASE("InputBinding: the card's Auto Detect still appends, on any device")
{
    MapEntry b = buttonEntry();
    CHECK(HE::Ed::applyDetected(b, BindSlot::EntryAny, 0, key("G")));
    CHECK(b.keys.size() == 3);
    CHECK(b.keys[2] == "G");
    CHECK(HE::Ed::applyDetected(b, BindSlot::EntryAny, 0, mouse("middle")));
    CHECK(b.mouseButtons.size() == 2);
    CHECK(HE::Ed::applyDetected(b, BindSlot::EntryAny, 0, pad("dpdown")));
    CHECK(b.gamepadButtons.size() == 3);

    // An Axis entry turns a press into the + half of a NEW pair row, and a
    // moved stick into a source row.
    MapEntry a; a.valueType = 1;
    CHECK(HE::Ed::applyDetected(a, BindSlot::EntryAny, 0, key("W")));
    REQUIRE(a.axes.size() == 1);
    CHECK(a.axes[0].positive == "W");
    CHECK(HE::Ed::applyDetected(a, BindSlot::EntryAny, 0, pad("dpup")));
    REQUIRE(a.axes.size() == 2);
    CHECK(a.axes[1].uiPadRow);
    CHECK(a.axes[1].positiveButton == "dpup");
    CHECK(HE::Ed::applyDetected(a, BindSlot::EntryAny, 0, stick(AxisSource::GamepadRightY)));
    REQUIRE(a.axes.size() == 3);
    CHECK(a.axes[2].source == AxisSource::GamepadRightY);
    // A mouse click has no meaning on an axis entry — nothing is added.
    CHECK_FALSE(HE::Ed::applyDetected(a, BindSlot::EntryAny, 0, mouse("left")));
    CHECK(a.axes.size() == 3);
}

TEST_CASE("InputBinding: several entries bind independently and survive a save")
{
    std::vector<MapEntry> entries;
    MapEntry jump;  jump.actionPath = "Input/IA_Jump.hasset";  jump.valueType = 0;
    jump.keys = { "Space" };
    MapEntry fire;  fire.actionPath = "Input/IA_Fire.hasset";  fire.valueType = 0;
    fire.gamepadButtons = { "a" };
    fire.mouseButtons   = { "left" };
    MapEntry move;  move.actionPath = "Input/IA_Move.hasset";  move.valueType = 2;
    { AxisRow x; x.positive = "D"; x.negative = "A"; move.axes.push_back(x);
      AxisRow y; y.positive = "W"; y.negative = "S"; move.axesY.push_back(y); }
    entries = { jump, fire, move };

    // Three Bind buttons in three different cards, one after the other.
    CHECK(HE::Ed::applyDetected(entries[0], BindSlot::Key, 0, key("Return")));
    CHECK(HE::Ed::applyDetected(entries[1], BindSlot::PadButton, 0, pad("rightshoulder")));
    CHECK(HE::Ed::applyDetected(entries[2], BindSlot::AxisKeyNeg,
                                HE::Ed::kAxisRowBaseY + 0, key("X")));

    // Each landed in its own entry and nowhere else.
    CHECK(entries[0].keys[0] == "Return");
    CHECK(entries[1].gamepadButtons[0] == "rightshoulder");
    CHECK(entries[1].mouseButtons[0] == "left");
    CHECK(entries[2].axes[0].positive == "D");
    CHECK(entries[2].axesY[0].negative == "X");

    // Through the asset's JSON and back, which is what Save writes.
    std::vector<MapEntry> back;
    HE::Ed::decodeMapping(HE::Ed::encodeMapping(entries), back);
    REQUIRE(back.size() == 3);
    CHECK(back[0].keys[0] == "Return");
    CHECK(back[1].gamepadButtons[0] == "rightshoulder");
    CHECK(back[1].mouseButtons[0] == "left");
    CHECK(back[2].axes[0].negative == "A");
    CHECK(back[2].axesY[0].negative == "X");

    // …and the runtime binds what came back: a 2D entry has to encode as
    // axesX/axesY or axis2DValue() answers 0,0 for it.
    InputMapping im;
    CHECK(HE::applyInputMappingContext(im, HE::Ed::encodeMapping(entries)) == 3);
}

TEST_CASE("InputBinding: the mapping codec round-trips every binding kind")
{
    const std::string json =
        R"({"entries":[{"action":"Input/IA_Look.hasset",)"
        R"("keys":["Space"],"gamepadButtons":["a"],"mouseButtons":["right"],)"
        R"("axesX":[{"positive":"D","negative":"A","scale":1.0,"source":"Key"}],)"
        R"("axesY":[{"positive":"","negative":"","scale":-1.0,"source":"GamepadRightY"}]}]})";
    std::vector<MapEntry> e;
    HE::Ed::decodeMapping(json, e);
    REQUIRE(e.size() == 1);
    CHECK(e[0].keys[0] == "Space");
    CHECK(e[0].gamepadButtons[0] == "a");
    CHECK(e[0].mouseButtons[0] == "right");
    REQUIRE(e[0].axes.size() == 1);
    REQUIRE(e[0].axesY.size() == 1);
    CHECK(e[0].axesY[0].source == AxisSource::GamepadRightY);
    CHECK(e[0].axesY[0].scale == doctest::Approx(-1.0f));

    // The decoder cannot know the action's type; the 2D shape carries it here
    // (the panel resolves it from the asset), and re-encoding must keep the two
    // lists apart rather than collapsing them into a 1D "axes".
    e[0].valueType = 2;
    std::vector<MapEntry> back;
    HE::Ed::decodeMapping(HE::Ed::encodeMapping(e), back);
    REQUIRE(back.size() == 1);
    REQUIRE(back[0].axes.size() == 1);
    REQUIRE(back[0].axesY.size() == 1);
    CHECK(back[0].axes[0].positive == "D");
    CHECK(back[0].axesY[0].source == AxisSource::GamepadRightY);

    // Garbage decodes to nothing rather than to half an entry.
    std::vector<MapEntry> none;
    HE::Ed::decodeMapping("not json", none);
    CHECK(none.empty());
    HE::Ed::decodeMapping(R"({"entries":[1,2,3]})", none);
    CHECK(none.empty());
}
