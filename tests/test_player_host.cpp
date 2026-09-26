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

// A cutscene's Lock Player Input (SequenceSystem::locksPlayerInput, handed to
// tick by both applications) is the third reason for silence beside a pause and
// UI-only mode, with the same exception — or the key that skips the cutscene
// would be silenced along with the rest.
TEST_CASE("PlayerHost: a locked frame is silenced like a pause, run-while-paused actions still arrive")
{
	TempDir dir("he_test_playerhost_locked");
	ContentManager cm(dir.path.string());
	writeInputAssets(cm);
	InputActionAsset skip;
	skip.type = HE::AssetType::InputAction;
	skip.name = "Skip";
	skip.path = "Input/Skip.hasset";
	skip.json = HE::makeInputActionJson("Button", true);
	REQUIRE(cm.saveAsset(skip));
	InputMappingContextAsset mc;
	mc.type = HE::AssetType::InputMappingContext;
	mc.name = "IMC_Cutscene";
	mc.path = "Input/IMC_Cutscene.hasset";
	mc.json = R"({"entries":[{"action":"Input/Skip.hasset","keys":["E"]}]})";
	REQUIRE(cm.saveAsset(mc));

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
	host.begin(rt, cm);
	host.setTextScripts(&scripts, &instances);
	HE::api::time::resume();
	HE::api::input::setModeGameAndUI();

	Input input;
	host.tick(input, 1.0f / 60.0f, {}, true);

	// Locked: the gameplay action and the axis are silent, the poll agrees; the
	// run-while-paused one comes through.
	keyEvent(input, SDL_SCANCODE_SPACE, true);
	keyEvent(input, SDL_SCANCODE_D, true);
	keyEvent(input, SDL_SCANCODE_E, true);
	host.tick(input, 1.0f / 60.0f, {}, true);
	CHECK(scripts.engine().getGlobalString("_heIn") == "+Skip");
	CHECK(scripts.engine().getGlobalNumber("_heAxis") == doctest::Approx(0.0));
	CHECK_FALSE(HE::api::input::actionDown("Jump"));
	CHECK(HE::api::input::actionAxis("Move") == 0.0f);
	CHECK(HE::api::input::actionDown("Skip"));

	// The same held keys unlocked: the axis is heard at once. The press edge
	// fell inside the lock and is gone — dropped, never queued, as in a pause.
	host.tick(input, 1.0f / 60.0f, {}, false);
	CHECK(scripts.engine().getGlobalNumber("_heAxis") == doctest::Approx(1.0));
	CHECK(HE::api::input::actionDown("Jump"));
	CHECK(scripts.engine().getGlobalString("_heIn") == "+Skip");

	host.end();
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
