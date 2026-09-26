// Player rebinding, the session-free half (Application/InputRebind.h): the
// capture's edge detection, the override layer and the conflict search. The
// session half — PlayerHost, the script rows, prefs — is in test_player_host.
#include "doctest.h"
#include <Application/Input.h>
#include <Application/InputMapping.h>
#include <Application/InputAssets.h>
#include <Application/InputRebind.h>

namespace
{
	void key(Input& input, SDL_Scancode sc, bool down)
	{
		SDL_Event evt{};
		evt.type         = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
		evt.key.scancode = sc;
		evt.key.key      = SDLK_UNKNOWN;
		evt.key.repeat   = 0;
		input.ProcessEvent(evt);
	}

	void pad(Input& input, SDL_GamepadButton b, bool down)
	{
		GamepadFrame f = input.gamepad();
		f.connected  = true;
		f.buttons[b] = down;
		input.SetGamepadFrame(f);
	}

	using R = HE::BindingCapture::Result;
	using P = HE::BindingCapture::Phase;
}

// ─── Capture ─────────────────────────────────────────────────────────────────

TEST_CASE("BindingCapture: the press still held from the menu does not count, the next one does")
{
	Input input;
	HE::BindingCapture cap;
	CHECK_FALSE(cap.busy());

	// Enter opened the rebind and is still down when it arms.
	key(input, SDL_SCANCODE_RETURN, true);
	cap.arm(HE::BindingDevice::KeyboardMouse);
	CHECK(cap.busy());
	CHECK(cap.update(input, {}) == R::None);   // arming: only looks
	CHECK(cap.phase() == P::Listening);
	CHECK(cap.update(input, {}) == R::None);   // Enter still held — not an edge
	CHECK(cap.phase() == P::Listening);

	key(input, SDL_SCANCODE_RETURN, false);
	CHECK(cap.update(input, {}) == R::None);
	key(input, SDL_SCANCODE_K, true);
	CHECK(cap.update(input, {}) == R::None);   // caught, but K is still down
	CHECK(cap.phase() == P::Draining);
	CHECK(cap.captured().key == SDL_SCANCODE_K);
	CHECK(cap.update(input, {}) == R::None);

	key(input, SDL_SCANCODE_K, false);
	CHECK(cap.update(input, {}) == R::Captured);   // reported on the release
	CHECK_FALSE(cap.busy());
	CHECK(cap.captured().key == SDL_SCANCODE_K);
	CHECK(cap.update(input, {}) == R::None);
}

TEST_CASE("BindingCapture: a key stuck down from before never blocks it")
{
	Input input;
	key(input, SDL_SCANCODE_W, true);   // held the whole time, never released
	HE::BindingCapture cap;
	cap.arm(HE::BindingDevice::KeyboardMouse);
	cap.update(input, {});
	key(input, SDL_SCANCODE_J, true);
	cap.update(input, {});
	key(input, SDL_SCANCODE_J, false);
	CHECK(cap.update(input, {}) == R::Captured);
	CHECK(cap.captured().key == SDL_SCANCODE_J);
}

TEST_CASE("BindingCapture: a mouse button on the desk, and the device filter")
{
	Input input;
	HE::BindingCapture cap;
	cap.arm(HE::BindingDevice::KeyboardMouse);
	MouseFrame m;
	cap.update(input, m);

	// A pad press is not the desk's answer: ignored, still listening.
	pad(input, SDL_GAMEPAD_BUTTON_SOUTH, true);
	cap.update(input, m);
	CHECK(cap.phase() == P::Listening);

	m.buttons = 1u << kMouseButtonRight;
	cap.update(input, m);
	CHECK(cap.phase() == P::Draining);
	CHECK(cap.captured().mouseButton == kMouseButtonRight);
	m.buttons = 0;
	CHECK(cap.update(input, m) == R::Captured);
}

TEST_CASE("BindingCapture: a pad button for the pad; keys are ignored")
{
	Input input;
	HE::BindingCapture cap;
	// South opened it and is still held.
	pad(input, SDL_GAMEPAD_BUTTON_SOUTH, true);
	cap.arm(HE::BindingDevice::Gamepad);
	cap.update(input, {});
	cap.update(input, {});
	CHECK(cap.phase() == P::Listening);

	key(input, SDL_SCANCODE_K, true);
	cap.update(input, {});
	CHECK(cap.phase() == P::Listening);

	pad(input, SDL_GAMEPAD_BUTTON_SOUTH, false);
	cap.update(input, {});
	pad(input, SDL_GAMEPAD_BUTTON_SOUTH, true);   // pressed AGAIN: that is the answer
	cap.update(input, {});
	CHECK(cap.captured().gamepadButton == SDL_GAMEPAD_BUTTON_SOUTH);
	pad(input, SDL_GAMEPAD_BUTTON_SOUTH, false);
	CHECK(cap.update(input, {}) == R::Captured);
}

TEST_CASE("BindingCapture: Escape and Start cancel on either device")
{
	SUBCASE("Escape while binding the pad")
	{
		Input input;
		HE::BindingCapture cap;
		cap.arm(HE::BindingDevice::Gamepad);
		cap.update(input, {});
		key(input, SDL_SCANCODE_ESCAPE, true);
		CHECK(cap.update(input, {}) == R::None);
		key(input, SDL_SCANCODE_ESCAPE, false);
		CHECK(cap.update(input, {}) == R::Cancelled);
		CHECK_FALSE(cap.busy());
	}
	SUBCASE("Start while binding the keyboard")
	{
		Input input;
		HE::BindingCapture cap;
		cap.arm(HE::BindingDevice::KeyboardMouse);
		cap.update(input, {});
		pad(input, SDL_GAMEPAD_BUTTON_START, true);
		cap.update(input, {});
		pad(input, SDL_GAMEPAD_BUTTON_START, false);
		CHECK(cap.update(input, {}) == R::Cancelled);
	}
	SUBCASE("Start while binding the pad is a cancel, not a binding")
	{
		Input input;
		HE::BindingCapture cap;
		cap.arm(HE::BindingDevice::Gamepad);
		cap.update(input, {});
		pad(input, SDL_GAMEPAD_BUTTON_START, true);
		cap.update(input, {});
		pad(input, SDL_GAMEPAD_BUTTON_START, false);
		CHECK(cap.update(input, {}) == R::Cancelled);
	}
	SUBCASE("cancel() from outside ends it at once, reporting nothing")
	{
		Input input;
		HE::BindingCapture cap;
		cap.arm(HE::BindingDevice::KeyboardMouse);
		cap.update(input, {});
		cap.cancel();
		CHECK_FALSE(cap.busy());
		key(input, SDL_SCANCODE_K, true);
		CHECK(cap.update(input, {}) == R::None);
	}
}

// ─── Override layer ──────────────────────────────────────────────────────────

TEST_CASE("BindingOverrides: a desk rebind keeps the pad half, and the other way round")
{
	InputMapping base;
	ActionBinding pad; pad.gamepadButton = SDL_GAMEPAD_BUTTON_SOUTH;
	ActionBinding lmb; lmb.mouseButton = kMouseButtonLeft;
	base.mapAction("Jump", { { SDL_SCANCODE_SPACE }, pad, lmb });

	HE::BindingOverrides o;
	o.set("Jump", HE::BindingDevice::KeyboardMouse, { { SDL_SCANCODE_K } });
	InputMapping m = base;
	o.applyTo(m);
	const auto* rows = m.actionBindings("Jump");
	REQUIRE(rows);
	// Space AND the mouse button are gone (one desk class), South stays.
	REQUIRE(rows->size() == 2);
	CHECK((*rows)[0].gamepadButton == SDL_GAMEPAD_BUTTON_SOUTH);
	CHECK((*rows)[1].key == SDL_SCANCODE_K);

	ActionBinding north; north.gamepadButton = SDL_GAMEPAD_BUTTON_NORTH;
	o.set("Jump", HE::BindingDevice::Gamepad, { north });
	m = base;
	o.applyTo(m);
	rows = m.actionBindings("Jump");
	REQUIRE(rows->size() == 2);
	CHECK((*rows)[0].key == SDL_SCANCODE_K);
	CHECK((*rows)[1].gamepadButton == SDL_GAMEPAD_BUTTON_NORTH);

	// The layer is on top, not in: the pressed state follows the new key.
	Input input;
	key(input, SDL_SCANCODE_SPACE, true);
	m.tick(input);
	CHECK_FALSE(m.isPressed("Jump"));
	key(input, SDL_SCANCODE_K, true);
	m.tick(input);
	CHECK(m.isPressed("Jump"));
}

TEST_CASE("BindingOverrides: JSON round trip through the mapping-context loader")
{
	HE::BindingOverrides o;
	ActionBinding rmb;  rmb.mouseButton = kMouseButtonRight;
	ActionBinding west; west.gamepadButton = SDL_GAMEPAD_BUTTON_WEST;
	o.set("Fire", HE::BindingDevice::KeyboardMouse, { rmb });
	o.set("Jump", HE::BindingDevice::KeyboardMouse, { { SDL_SCANCODE_K } });
	o.set("Jump", HE::BindingDevice::Gamepad, { west });
	const std::string json = o.toJson();
	// Mapping-context shape: a context holding the same text binds the same.
	InputMapping direct;
	HE::applyInputMappingContext(direct, json, HE::MappingMerge::Replace);
	REQUIRE(direct.actionBindings("Jump"));
	CHECK(direct.actionBindings("Jump")->size() == 2);

	HE::BindingOverrides back;
	CHECK(back.fromJson(json));
	CHECK(back.size() == 2);
	CHECK(back.toJson() == json);   // deterministic: same layer, same text

	InputMapping base;
	base.mapAction("Fire", { { SDL_SCANCODE_F } });
	back.applyTo(base);
	REQUIRE(base.actionBindings("Fire")->size() == 1);
	CHECK(base.actionBindings("Fire")->front().mouseButton == kMouseButtonRight);
	// An action the base never bound gets the override alone.
	REQUIRE(base.actionBindings("Jump"));
	CHECK(base.actionBindings("Jump")->size() == 2);

	// Empty text is an empty layer; garbage is refused and leaves it empty.
	CHECK(back.fromJson(""));
	CHECK(back.empty());
	back.set("Jump", HE::BindingDevice::KeyboardMouse, { { SDL_SCANCODE_K } });
	CHECK_FALSE(back.fromJson("{not json"));
	CHECK(back.empty());
}

// ─── Conflicts, names, devices ───────────────────────────────────────────────

TEST_CASE("bindingConflicts: other actions and key-pair axes, never the action itself")
{
	InputMapping m;
	m.mapAction("Jump",   { { SDL_SCANCODE_SPACE } });
	m.mapAction("Crouch", { { SDL_SCANCODE_C } });
	m.mapAction("Use",    { { SDL_SCANCODE_C } });
	m.mapAxis("Move",     { { SDL_SCANCODE_D, SDL_SCANCODE_A } });
	AxisBinding mouse; mouse.source = AxisSource::MouseX;
	m.mapAxis("Look", { mouse });

	CHECK(HE::bindingConflicts(m, "Jump", { SDL_SCANCODE_C }) ==
	      std::vector<std::string>{ "Crouch", "Use" });
	CHECK(HE::bindingConflicts(m, "Jump", { SDL_SCANCODE_A }) ==
	      std::vector<std::string>{ "Move" });
	CHECK(HE::bindingConflicts(m, "Jump", { SDL_SCANCODE_SPACE }).empty());   // itself
	CHECK(HE::bindingConflicts(m, "Jump", { SDL_SCANCODE_Q }).empty());
}

TEST_CASE("Rebind helpers: device names and display names")
{
	HE::BindingDevice d;
	CHECK(HE::bindingDeviceFromName("Keyboard", d));
	CHECK(d == HE::BindingDevice::KeyboardMouse);
	CHECK(HE::bindingDeviceFromName("mouse", d));
	CHECK(d == HE::BindingDevice::KeyboardMouse);
	CHECK(HE::bindingDeviceFromName("GAMEPAD", d));
	CHECK(d == HE::BindingDevice::Gamepad);
	CHECK_FALSE(HE::bindingDeviceFromName("joystick", d));

	CHECK(HE::bindingDisplayName({ SDL_SCANCODE_SPACE }) == "Space");
	ActionBinding south; south.gamepadButton = SDL_GAMEPAD_BUTTON_SOUTH;
	CHECK(HE::bindingDisplayName(south) == "A (South)");
	ActionBinding lmb; lmb.mouseButton = kMouseButtonLeft;
	CHECK(HE::bindingDisplayName(lmb) == "Left Mouse Button");
	CHECK(HE::bindingDisplayName(ActionBinding{}).empty());
}
