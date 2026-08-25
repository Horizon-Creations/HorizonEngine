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
