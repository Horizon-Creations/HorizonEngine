// PlayerHost: who the engine creates at play start, and who receives input.
//
// This file exists because the answer CHANGED. The host used to instantiate
// every PlayerCharacter class it could find and auto-possess the unambiguous
// pair; now a character is spawned by the game (Create Object, at a transform
// it chooses) and possession is always a decision. Two things about that are
// invisible until a game is running, so they are pinned here instead:
//   * a PlayerCharacter class must NOT be instantiated by the host any more,
//   * a character the LEVEL placed must still be found, or a project without a
//     controller silently receives no input at all.
#include "doctest.h"
#include "TestFsUtil.h"
#include <HorizonScene/PlayerHost.h>
#include <HorizonScene/EntityHost.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/ScriptContext.h>
#include <HorizonScene/TimerSystem.h>
#include <Application/Input.h>
#include <Application/InputAssets.h>
#include <HorizonScene/Components/ScriptComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace HorizonCode;

namespace
{
	struct TempDir
	{
		fs::path path;
		explicit TempDir(const char* name)
		{
			path = fs::temp_directory_path() / name;
			he_test::removeAllQuiet(path);
			fs::create_directories(path);
		}
		~TempDir() { he_test::removeAllQuiet(path); }
	};

	// A graph that counts its BeginPlay into a variable. Enough to tell "this
	// class was instantiated" from "this class was only looked at".
	Graph beginPlayCounter()
	{
		Graph g;
		Variable v; v.name = "beginPlays"; v.type = PinType::Int;
		g.variables.push_back(v);

		Node ev; ev.type = NodeType::Event; ev.s = "BeginPlay";
		const int e = g.addNode(std::move(ev));
		Node get; get.type = NodeType::GetVariable; get.s = "beginPlays"; get.propType = PinType::Int;
		const int gv = g.addNode(std::move(get));
		Node one; one.type = NodeType::ConstInt; one.f[0] = 1.0f;
		const int k = g.addNode(std::move(one));
		Node add; add.type = NodeType::Add;
		const int a = g.addNode(std::move(add));
		Node set; set.type = NodeType::SetVariable; set.s = "beginPlays"; set.propType = PinType::Int;
		const int s = g.addNode(std::move(set));
		REQUIRE(g.connect(gv, 0, a, 0));
		REQUIRE(g.connect(k,  0, a, 1));
		REQUIRE(g.connect(e,  0, s, 0));
		REQUIRE(g.connect(a,  2, s, 2));   // Add has no exec pins: data-out is pin 2
		return g;
	}

	std::string writeClass(ContentManager& cm, const char* name, const char* baseClass)
	{
		HorizonCodeClassAsset a;
		a.type      = HE::AssetType::HorizonCodeClass;
		a.name      = name;
		a.path      = std::string(name) + ".hasset";
		a.baseClass = baseClass;
		a.graphJson = toJson(beginPlayCounter());
		REQUIRE(cm.saveAsset(a));
		return a.path;
	}
}

TEST_CASE("PlayerHost starts controllers and leaves PlayerCharacter classes alone")
{
	TempDir dir("he_test_playerhost_no_autospawn");
	ContentManager cm(dir.path.string());
	writeClass(cm, "Hero",    "PlayerCharacter");
	writeClass(cm, "Controls", "PlayerController");

	Runtime rt;
	PlayerHost host;
	host.begin(rt, cm);

	CHECK(host.controllerCount() == 1);
	// The character class was found (the log says so) but never instantiated, so
	// there is nothing to possess and nothing possessed it.
	CHECK(host.fallbackCharacterCount() == 0);
	CHECK(HE::api::player::character() == 0);
	CHECK(HE::api::player::possessed(HE::api::player::controller()) == 0);

	host.end();
	HE::api::player::clear();
}

TEST_CASE("PlayerHost finds the PlayerCharacter the level already placed")
{
	TempDir dir("he_test_playerhost_placed");
	ContentManager cm(dir.path.string());
	const std::string hero = writeClass(cm, "Hero", "PlayerCharacter");

	HorizonWorld world;
	const Entity e = world.createEntity("Hero1");
	world.addComponent(e, TransformComponent{});
	{
		ScriptComponent sc;
		sc.scriptAssetId = cm.loadAsset(hero);
		world.addComponent(e, sc);
	}

	Runtime rt;
	// The real order: entities are bound first, the player host reads them after.
	EntityHost entities;
	entities.begin(rt, world, cm);
	REQUIRE(entities.instanceOf(e) != 0);

	PlayerHost host;
	host.begin(rt, cm, &entities);

	// Without this the no-controller input fallback has an empty list, and a
	// project that places its character in the scene gets no input whatsoever.
	CHECK(host.fallbackCharacterCount() == 1);

	host.end();
	entities.end();
	HE::api::player::clear();
}

TEST_CASE("PlayerHost: a character registered twice is still one character")
{
	TempDir dir("he_test_playerhost_addcharacter");
	ContentManager cm(dir.path.string());

	Runtime rt;
	PlayerHost host;
	host.begin(rt, cm);

	Graph g = beginPlayCounter();
	const InstanceId inst = rt.add(std::move(g));
	REQUIRE(inst != 0);

	host.addCharacter(inst);
	host.addCharacter(inst);       // the same instance, not a second character
	host.addCharacter(0);          // nothing at all
	CHECK(host.fallbackCharacterCount() == 1);

	host.end();
	HE::api::player::clear();
}

// ─── Several mapping contexts: a union, in path order ────────────────────────
// Two contexts naming the same action used to be last-writer-wins, and "last"
// was whatever ContentManager::discoverAssets handed back first — an
// unordered_map, then a directory walk. Pinned here: the contexts are applied
// sorted by path, the same action in both keeps BOTH bindings, a binding both
// contexts carry counts once (a doubled MouseX row would double look speed),
// and none of it depends on which asset happened to be saved first.
namespace
{
	void writeMappingContext(ContentManager& cm, const char* path, const char* json)
	{
		InputMappingContextAsset mc;
		mc.type = HE::AssetType::InputMappingContext;
		mc.name = fs::path(path).stem().string();
		mc.path = path;
		mc.json = json;
		REQUIRE(cm.saveAsset(mc));
	}

	void checkContextUnionInPathOrder(bool saveZFirst)
	{
		TempDir dir(saveZFirst ? "he_test_playerhost_ctx_z_first"
		                       : "he_test_playerhost_ctx_a_first");
		ContentManager cm(dir.path.string());

		InputActionAsset jump;
		jump.type = HE::AssetType::InputAction;
		jump.name = "Jump";
		jump.path = "Input/Jump.hasset";
		jump.json = HE::makeInputActionJson("Button", false);
		REQUIRE(cm.saveAsset(jump));
		InputActionAsset look;
		look.type = HE::AssetType::InputAction;
		look.name = "Look";
		look.path = "Input/Look.hasset";
		look.json = HE::makeInputActionJson("Axis", false);
		REQUIRE(cm.saveAsset(look));

		// Each context has a binding the other lacks (K / J, H / L), so no
		// single context on its own — which is all last-writer-wins keeps —
		// can pass the counts below.
		const char* aKeys = R"({"entries":[
			{"action":"Input/Jump.hasset","keys":["Space","K"]},
			{"action":"Input/Look.hasset","axes":[{"source":"MouseX","scale":1.0},
			                                     {"positive":"H","scale":1.0}]}
		]})";
		const char* zMore = R"({"entries":[
			{"action":"Input/Jump.hasset","keys":["J","Space"]},
			{"action":"Input/Look.hasset","axes":[{"source":"MouseX","scale":1.0},
			                                     {"positive":"L","scale":1.0}]}
		]})";
		if (saveZFirst)
		{
			writeMappingContext(cm, "Input/Z_More.hasset", zMore);
			writeMappingContext(cm, "Input/A_Keys.hasset", aKeys);
		}
		else
		{
			writeMappingContext(cm, "Input/A_Keys.hasset", aKeys);
			writeMappingContext(cm, "Input/Z_More.hasset", zMore);
		}

		Runtime rt;
		PlayerHost host;
		host.begin(rt, cm);

		const std::vector<std::string> order{ "Input/A_Keys.hasset", "Input/Z_More.hasset" };
		CHECK(host.mappingContexts() == order);

		// Jump: A's Space and K, then Z's J; Z's Space is the same binding again.
		const std::vector<ActionBinding>* jb = host.mapping().actionBindings("Jump");
		REQUIRE(jb != nullptr);
		REQUIRE(jb->size() == 3);
		CHECK((*jb)[0].key == SDL_SCANCODE_SPACE);
		CHECK((*jb)[1].key == SDL_SCANCODE_K);
		CHECK((*jb)[2].key == SDL_SCANCODE_J);

		// Look: A's MouseX and H, then Z's L; Z's MouseX is not a second one.
		const std::vector<AxisBinding>* lb = host.mapping().axisBindings("Look");
		REQUIRE(lb != nullptr);
		REQUIRE(lb->size() == 3);
		CHECK((*lb)[0].source == AxisSource::MouseX);
		CHECK((*lb)[1].positiveKey == SDL_SCANCODE_H);
		CHECK((*lb)[2].positiveKey == SDL_SCANCODE_L);

		// And the frame agrees: J alone is a jump (Z's binding survived A), and
		// ten pixels of mouse are ten units of look, not twenty.
		HE::api::time::resume();
		HE::api::input::setModeGameAndUI();
		Input input;
		SDL_Event down{};
		down.type         = SDL_EVENT_KEY_DOWN;
		down.key.scancode = SDL_SCANCODE_J;
		down.key.key      = SDLK_UNKNOWN;
		input.ProcessEvent(down);
		MouseFrame mouse;
		mouse.dx = 10.0f;
		host.tick(input, 1.0f / 60.0f, mouse);
		CHECK(HE::api::input::actionDown("Jump"));
		CHECK(host.mapping().axisValue("Look") == doctest::Approx(10.0f));

		host.end();
		CHECK(host.mappingContexts().empty());
		HE::api::player::clear();
	}
}

TEST_CASE("PlayerHost: mapping contexts are a union applied in path order")
{
	SUBCASE("A saved first") { checkContextUnionInPathOrder(false); }
	SUBCASE("Z saved first") { checkContextUnionInPathOrder(true); }
}

// ─── Text scripts hear the actions ───────────────────────────────────────────
// The pump ran in a Lua project exactly as in a HorizonCode one, and nobody
// listened: PlayerHost fired Input.<Action>.* at controllers and characters
// only. These pin the new door — every Lua/Python instance of the session gets
// the same events, under the same silence — and the polling twin beside it.
namespace
{
	void writeInputAssets(ContentManager& cm)
	{
		InputActionAsset jump;
		jump.type = HE::AssetType::InputAction;
		jump.name = "Jump";
		jump.path = "Input/Jump.hasset";
		jump.json = HE::makeInputActionJson("Button", false);
		REQUIRE(cm.saveAsset(jump));

		InputActionAsset move;
		move.type = HE::AssetType::InputAction;
		move.name = "Move";
		move.path = "Input/Move.hasset";
		move.json = HE::makeInputActionJson("Axis", false);
		REQUIRE(cm.saveAsset(move));

		InputMappingContextAsset mc;
		mc.type = HE::AssetType::InputMappingContext;
		mc.name = "IMC_Default";
		mc.path = "Input/IMC_Default.hasset";
		mc.json = R"({"entries":[
			{"action":"Input/Jump.hasset","keys":["Space"]},
			{"action":"Input/Move.hasset","axes":[{"positive":"D","negative":"A","scale":1.0}]}
		]})";
		REQUIRE(cm.saveAsset(mc));
	}

	void keyEvent(Input& input, SDL_Scancode sc, bool down)
	{
		SDL_Event evt{};
		evt.type         = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
		evt.key.scancode = sc;
		evt.key.key      = SDLK_UNKNOWN;
		evt.key.repeat   = 0;
		input.ProcessEvent(evt);
	}

	// A script that writes every event it hears into globals the test reads.
	const char* kListener = R"lua(
local M = {}
function M.onStart(self) _heIn = "" _heAxis = 0 end
function M.onInputPressed(self, action)  _heIn = _heIn .. "+" .. action end
function M.onInputReleased(self, action) _heIn = _heIn .. "-" .. action end
function M.onInputAxis(self, action, v)  _heAxis = v end
function M.onTimer(self, handle)         _heTimer = handle end
return M
)lua";
}

TEST_CASE("PlayerHost: Lua instances receive the action events and the poll agrees")
{
	TempDir dir("he_test_playerhost_lua_input");
	ContentManager cm(dir.path.string());
	writeInputAssets(cm);

	HorizonWorld world;
	const Entity e = world.createEntity("Listener");
	ScriptContext scripts(world);
	REQUIRE(scripts.engine().loadScript("listener", kListener));
	const auto lua = scripts.engine().createInstance("listener", static_cast<uint32_t>(e));
	REQUIRE(lua != ScriptEngine::kInvalidInstance);
	scripts.engine().callOnStart(lua);
	ScriptContext::InstanceMap instances{ { static_cast<uint32_t>(e), lua } };

	Runtime rt;
	PlayerHost host;
	// No controller and no character class anywhere: a plain Lua project.
	host.begin(rt, cm);
	host.setTextScripts(&scripts, &instances);
	HE::api::time::resume();
	HE::api::input::setModeGameAndUI();

	Input input;
	host.tick(input, 1.0f / 60.0f);
	CHECK(scripts.engine().getGlobalString("_heIn") == "");
	CHECK_FALSE(HE::api::input::actionDown("Jump"));

	// Press: the edge fires once, the poll says held and pressed.
	keyEvent(input, SDL_SCANCODE_SPACE, true);
	keyEvent(input, SDL_SCANCODE_D, true);
	host.tick(input, 1.0f / 60.0f);
	CHECK(scripts.engine().getGlobalString("_heIn") == "+Jump");
	CHECK(scripts.engine().getGlobalNumber("_heAxis") == doctest::Approx(1.0));
	CHECK(HE::api::input::actionDown("Jump"));
	CHECK(HE::api::input::actionPressed("Jump"));
	CHECK_FALSE(HE::api::input::actionReleased("Jump"));
	CHECK(HE::api::input::actionAxis("Move") == doctest::Approx(1.0f));

	// Held: no second edge, still down.
	host.tick(input, 1.0f / 60.0f);
	CHECK(scripts.engine().getGlobalString("_heIn") == "+Jump");
	CHECK(HE::api::input::actionDown("Jump"));
	CHECK_FALSE(HE::api::input::actionPressed("Jump"));

	// Release.
	keyEvent(input, SDL_SCANCODE_SPACE, false);
	keyEvent(input, SDL_SCANCODE_D, false);
	host.tick(input, 1.0f / 60.0f);
	CHECK(scripts.engine().getGlobalString("_heIn") == "+Jump-Jump");
	CHECK(scripts.engine().getGlobalNumber("_heAxis") == doctest::Approx(0.0));
	CHECK(HE::api::input::actionReleased("Jump"));
	CHECK_FALSE(HE::api::input::actionDown("Jump"));

	// A pause silences the script exactly as it silences a graph: no press
	// reaches it, and the poll reads released rather than what the keyboard
	// says — a handler and a poll must never disagree.
	HE::api::time::pause();
	keyEvent(input, SDL_SCANCODE_SPACE, true);
	host.tick(input, 1.0f / 60.0f);
	CHECK(scripts.engine().getGlobalString("_heIn") == "+Jump-Jump");
	CHECK_FALSE(HE::api::input::actionDown("Jump"));
	HE::api::time::resume();

	// Unknown action: false and zero, never a throw.
	CHECK_FALSE(HE::api::input::actionDown("NoSuchAction"));
	CHECK(HE::api::input::actionAxis("NoSuchAction") == 0.0f);

	// end() withdraws the sink and the snapshot: outside a session the poll
	// answers nothing, not the last frame of the previous session.
	host.end();
	CHECK_FALSE(HE::api::input::actionReleased("Jump"));
	HE::api::player::clear();
}

TEST_CASE("PlayerHost: without a sink the pump still runs and nothing crashes")
{
	TempDir dir("he_test_playerhost_no_sink");
	ContentManager cm(dir.path.string());
	writeInputAssets(cm);

	Runtime rt;
	PlayerHost host;
	host.begin(rt, cm);
	HE::api::time::resume();

	Input input;
	keyEvent(input, SDL_SCANCODE_SPACE, true);
	host.tick(input, 1.0f / 60.0f);
	// The polling rows are fed either way — they do not depend on a script
	// context existing, only on the host ticking.
	CHECK(HE::api::input::actionPressed("Jump"));
	host.end();
	HE::api::player::clear();
}

// ─── Rebinding: the player's layer, through the script rows ──────────────────
// The whole path a settings menu takes: rebindBegin from a menu in UI-only mode
// (where gameplay is silent and the capture must still hear), the conflict
// report, the rebuilt mapping, prefs across two sessions, reset and cancel.
namespace
{
	void writeRebindAssets(ContentManager& cm)
	{
		writeInputAssets(cm);   // Jump (Space), Move (D/A)
		// A pause-menu action: runs while paused / in UI-only mode, which is
		// exactly the kind the capture has to silence as well.
		InputActionAsset menu;
		menu.type = HE::AssetType::InputAction;
		menu.name = "Menu";
		menu.path = "Input/Menu.hasset";
		menu.json = HE::makeInputActionJson("Button", true);
		REQUIRE(cm.saveAsset(menu));

		InputMappingContextAsset mc;
		mc.type = HE::AssetType::InputMappingContext;
		mc.name = "IMC_Menu";
		mc.path = "Input/IMC_Menu.hasset";
		mc.json = R"({"entries":[
			{"action":"Input/Menu.hasset","keys":["C"]},
			{"action":"Input/Jump.hasset","gamepadButtons":["a"]}
		]})";
		REQUIRE(cm.saveAsset(mc));
	}

	void padButton(Input& input, SDL_GamepadButton b, bool down)
	{
		GamepadFrame f = input.gamepad();
		f.connected  = true;
		f.buttons[b] = down;
		input.SetGamepadFrame(f);
	}
}

TEST_CASE("PlayerHost: rebinding in a UI-only menu, the conflict, prefs, reset and cancel")
{
	using namespace HE::api;
	TempDir dir("he_test_playerhost_rebind");
	ContentManager cm(dir.path.string());
	writeRebindAssets(cm);

	const auto sandbox = std::filesystem::temp_directory_path() / "he_test_playerhost_rebind_prefs";
	he_test::removeAllQuiet(sandbox);
	HE::api::fs::setSandboxRoot(sandbox.string());
	Ctx ctx;
	prefs::clear(ctx);

	// No session, no service: every row answers "nothing".
	CHECK_FALSE(input::rebindBegin("Jump", "keyboard"));
	CHECK_FALSE(input::isRebinding());
	CHECK(input::bindingName("Jump", "keyboard").empty());

	Runtime rt;
	PlayerHost host;
	host.begin(rt, cm);
	time::resume();
	const float dt = 1.0f / 60.0f;
	Input input;

	CHECK(input::bindingName("Jump", "keyboard") == "Space");
	CHECK(input::bindingName("Jump", "gamepad") == "A (South)");
	CHECK_FALSE(input::rebindBegin("Move", "keyboard"));    // an axis: not yet
	CHECK_FALSE(input::rebindBegin("Nope", "keyboard"));    // no such action
	CHECK_FALSE(input::rebindBegin("Jump", "joystick"));    // no such device
	CHECK_FALSE(input::isRebinding());

	SUBCASE("capture, conflict, save, next session, reset")
	{
		input::setMode(input::Mode::UIOnly);
		// Enter pressed the menu's "Rebind Jump" button and is still down.
		keyEvent(input, SDL_SCANCODE_RETURN, true);
		REQUIRE(input::rebindBegin("Jump", "keyboard"));
		CHECK(input::isRebinding());
		host.tick(input, dt);                       // arms
		keyEvent(input, SDL_SCANCODE_RETURN, false);
		host.tick(input, dt);
		CHECK(input::isRebinding());                // Enter was not the answer

		// C: the Menu action's key. Menu runs in UI-only mode, but not while a
		// rebind listens — the press is the capture's alone.
		keyEvent(input, SDL_SCANCODE_C, true);
		host.tick(input, dt);
		CHECK(input::isRebinding());                // caught, waiting for the release
		CHECK_FALSE(input::actionPressed("Menu"));
		keyEvent(input, SDL_SCANCODE_C, false);
		host.tick(input, dt);
		CHECK_FALSE(input::isRebinding());
		CHECK_FALSE(input::actionPressed("Menu"));
		CHECK(input::rebindConflict() == "Menu");
		CHECK(input::bindingName("Jump", "keyboard") == "C");
		CHECK(input::bindingName("Jump", "gamepad") == "A (South)");   // other half kept

		// Back in the game: C is Jump now (and still Menu), Space is nothing.
		input::setMode(input::Mode::GameAndUI);
		keyEvent(input, SDL_SCANCODE_C, true);
		host.tick(input, dt);
		CHECK(input::actionPressed("Jump"));
		CHECK(input::actionPressed("Menu"));
		keyEvent(input, SDL_SCANCODE_C, false);
		host.tick(input, dt);
		keyEvent(input, SDL_SCANCODE_SPACE, true);
		host.tick(input, dt);
		CHECK_FALSE(input::actionDown("Jump"));
		keyEvent(input, SDL_SCANCODE_SPACE, false);
		host.tick(input, dt);

		// Saved under the slot-0 key; a new session picks it up.
		CHECK(input::saveBindings());
		CHECK(prefs::has(ctx, PlayerHost::kBindingsPrefsKey));
		host.end();
		CHECK_FALSE(input::rebindBegin("Jump", "keyboard"));   // service gone with the session
		host.begin(rt, cm);
		CHECK(input::bindingName("Jump", "keyboard") == "C");

		// Reset while Space is HELD: after the rebuild Space is Jump again and
		// down — held, not a fresh press.
		keyEvent(input, SDL_SCANCODE_SPACE, true);
		host.tick(input, dt);
		input::resetBindings();
		host.tick(input, dt);
		CHECK(input::bindingName("Jump", "keyboard") == "Space");
		CHECK(input::actionDown("Jump"));
		CHECK_FALSE(input::actionPressed("Jump"));
		keyEvent(input, SDL_SCANCODE_SPACE, false);
		host.tick(input, dt);

		// Reset is not saved until saveBindings — which then removes the key.
		CHECK(prefs::has(ctx, PlayerHost::kBindingsPrefsKey));
		CHECK(input::saveBindings());
		CHECK_FALSE(prefs::has(ctx, PlayerHost::kBindingsPrefsKey));
	}

	SUBCASE("a pad rebind, cancelled by Start and by the row")
	{
		REQUIRE(input::rebindBegin("Jump", "gamepad"));
		host.tick(input, dt);
		padButton(input, SDL_GAMEPAD_BUTTON_START, true);
		host.tick(input, dt);
		CHECK(input::isRebinding());
		padButton(input, SDL_GAMEPAD_BUTTON_START, false);
		host.tick(input, dt);
		CHECK_FALSE(input::isRebinding());
		CHECK(input::bindingName("Jump", "gamepad") == "A (South)");

		REQUIRE(input::rebindBegin("Jump", "gamepad"));
		host.tick(input, dt);
		input::rebindCancel();   // what the apps do on Escape
		CHECK_FALSE(input::isRebinding());
		padButton(input, SDL_GAMEPAD_BUTTON_NORTH, true);
		host.tick(input, dt);
		padButton(input, SDL_GAMEPAD_BUTTON_NORTH, false);
		host.tick(input, dt);
		CHECK(input::bindingName("Jump", "gamepad") == "A (South)");
		CHECK(host.bindingOverrides().empty());

		// And one that goes through: West replaces South, Space stays.
		REQUIRE(input::rebindBegin("Jump", "gamepad"));
		host.tick(input, dt);
		padButton(input, SDL_GAMEPAD_BUTTON_WEST, true);
		host.tick(input, dt);
		padButton(input, SDL_GAMEPAD_BUTTON_WEST, false);
		host.tick(input, dt);
		CHECK(input::bindingName("Jump", "gamepad") == "X (West)");
		CHECK(input::bindingName("Jump", "keyboard") == "Space");
		CHECK(input::rebindConflict().empty());
	}

	host.end();
	input::setMode(input::Mode::GameAndUI);
	player::clear();
	prefs::clear(ctx);
	he_test::removeAllQuiet(sandbox);
}

// ─── Timers reach both frontends ─────────────────────────────────────────────
TEST_CASE("TimerSystem: a due timer reaches the GameInstance and every Lua instance")
{
	HE::api::timer::cancelAll();

	HorizonWorld world;
	const Entity e = world.createEntity("Clock");
	ScriptContext scripts(world);
	REQUIRE(scripts.engine().loadScript("listener", kListener));
	const auto lua = scripts.engine().createInstance("listener", static_cast<uint32_t>(e));
	REQUIRE(lua != ScriptEngine::kInvalidInstance);
	scripts.engine().callOnStart(lua);
	ScriptContext::InstanceMap instances{ { static_cast<uint32_t>(e), lua } };

	// A GameInstance graph that stores the handle OnTimer carries.
	Graph g;
	Variable v; v.name = "got"; v.type = PinType::Int;
	g.variables.push_back(v);
	Node ev; ev.type = NodeType::Event; ev.s = "OnTimer";
	ev.hasArg = true; ev.propType = PinType::Int;
	const int en = g.addNode(std::move(ev));
	Node set; set.type = NodeType::SetVariable; set.s = "got"; set.propType = PinType::Int;
	const int sn = g.addNode(std::move(set));
	REQUIRE(g.connect(en, 0, sn, 0));
	REQUIRE(g.connect(en, 1, sn, 2));
	Runtime rt;
	const InstanceId gi = rt.setGameInstance(g);
	REQUIRE(gi != 0);

	const int h = HE::api::timer::after(0.05);
	REQUIRE(h != 0);

	// Not yet due: nothing fires, nothing is delivered.
	CHECK(TimerSystem::dispatch(0.01, &rt, &scripts, &instances) == 0);
	CHECK(rt.getVariable(gi, "got").i == 0);

	// Due: one timer, two receivers, the same handle at both.
	CHECK(TimerSystem::dispatch(0.1, &rt, &scripts, &instances) == 1);
	CHECK(rt.getVariable(gi, "got").i == h);
	CHECK(static_cast<int>(scripts.engine().getGlobalNumber("_heTimer")) == h);

	// A one-shot is gone afterwards.
	CHECK_FALSE(HE::api::timer::active(h));
	CHECK(TimerSystem::dispatch(1.0, &rt, &scripts, &instances) == 0);

	// Null receivers are a valid configuration (a session with no scripts),
	// and the timer is still consumed rather than left standing.
	const int h2 = HE::api::timer::after(0.01);
	REQUIRE(h2 != 0);
	CHECK(TimerSystem::dispatch(0.1, nullptr, nullptr, nullptr) == 1);
	CHECK_FALSE(HE::api::timer::active(h2));
	HE::api::timer::cancelAll();
}
