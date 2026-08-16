#include "doctest.h"
#include "TestFsUtil.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <HorizonScene/AnimatorHost.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/SceneSystems.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/AnimatorStateMachineComponent.h>
#include <AnimatorStateMachine/AnimatorStateMachineGraph.h>
#include <filesystem>
#include <string>

using namespace HorizonCode;

namespace fs = std::filesystem;

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

	// A sync graph that does the one thing a sync graph is for: on Update, read
	// its own owner and write a state-machine parameter.
	//
	//   Event "Update" ──exec──► animator.setParam( entity.self, "speed", 1.0 )
	//
	// `entity.self` is deliberately how it reaches the character. The asset is
	// shared by everything using this state machine, so a graph that asked for
	// "the player" would animate an NPC with the player's numbers.
	Graph syncGraphSettingSpeed(float value)
	{
		Graph g;

		Node ev; ev.type = NodeType::Event; ev.s = "Update";
		ev.hasArg = true; ev.propType = PinType::Float;   // dt
		const int e = g.addNode(std::move(ev));

		auto engineCall = [&g](const char* id)
		{
			const HE::api::ApiFn* fn = HE::api::find(id);
			REQUIRE(fn != nullptr);
			Node n; n.type = NodeType::EngineCall; n.s = fn->id;
			n.hasArg = fn->isExec;
			for (const auto& p : fn->params)  n.params.push_back({ p.name, p.type, p.isArray });
			for (const auto& r : fn->results) n.results.push_back({ r.name, r.type, r.isArray });
			return g.addNode(std::move(n));
		};

		const int self = engineCall("entity.self");
		const int set  = engineCall("animator.setParam");

		Node name; name.type = NodeType::ConstString; name.s = "speed";
		const int nm = g.addNode(std::move(name));
		Node val; val.type = NodeType::ConstFloat; val.f[0] = value;
		const int v = g.addNode(std::move(val));

		// Pins run exec-ins, exec-outs, then data. animator.setParam is an exec
		// call with one of each, so its three data inputs start at 2.
		REQUIRE(g.connect(e,    0, set, 0));   // Update's exec out → setParam's exec in
		REQUIRE(g.connect(self, 0, set, 2));   // entity
		REQUIRE(g.connect(nm,   0, set, 3));   // name
		REQUIRE(g.connect(v,    0, set, 4));   // value
		return g;
	}

	// An Idle→Walk machine that flips the moment speed clears 0.5, with no
	// crossfade so the state is observable in the same tick.
	std::string stateMachineJson()
	{
		HE::AnimatorStateMachineGraph g;
		g.states.push_back({ 1, "Idle", HE::UUID{}, true, 0.0f, 0.0f });
		g.states.push_back({ 2, "Walk", HE::UUID{}, true, 0.0f, 0.0f });
		g.transitions.push_back({ "Idle", "Walk", "speed",
		                          HE::TransitionOp::Greater, 0.5f, 0.0f });
		g.startState = "Idle";
		return HE::animatorStateMachineToJson(g);
	}

	// Wire the runtime's engine-call service the way both applications do, so an
	// EngineCall node in a graph actually reaches HE::api.
	void bindApi(Runtime& rt, HorizonWorld& world, ContentManager& cm)
	{
		Runtime::Services svc;
		svc.callApi = [&rt, &world, &cm](InstanceId self, const std::string& id,
		                                 const std::vector<Value>& args) -> std::vector<Value>
		{
			const HE::api::ApiFn* fn = HE::api::find(id);
			if (!fn) return {};
			HE::api::Ctx c{ &world, nullptr, &cm, nullptr, &rt, self };
			return fn->invoke(c, args);
		};
		rt.setServices(std::move(svc));
	}

	// A real one-joint skeleton. The state machine bails out early on an entity
	// whose skeletal mesh does not resolve — so without this the transitions
	// below are never even reached, and the test would pass or fail for the
	// wrong reason.
	HE::UUID registerSkeleton(ContentManager& cm)
	{
		SkeletalMeshAsset sma;
		sma.id   = HE::UUID::generate();
		sma.name = "testSkel";
		SkeletonJoint root;
		root.name   = "Root";
		root.parent = -1;
		root.inverseBindMatrix = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
		sma.skeleton.push_back(root);
		return cm.registerSkeletalMesh(std::move(sma));
	}

	Entity makeAnimatedEntity(HorizonWorld& world, ContentManager& cm, HE::UUID smAsset)
	{
		const Entity e = world.createEntity("Character");
		world.addComponent(e, TransformComponent{});
		SkeletalMeshComponent smc;
		smc.meshAssetId = registerSkeleton(cm);
		world.addComponent(e, smc);
		AnimatorStateMachineComponent sm;
		sm.stateMachineAssetId = smAsset;
		world.addComponent(e, sm);
		return e;
	}
}

TEST_CASE("A sync graph's palette is restricted by GROUP, not by wishful thinking")
{
	// The sync graph runs inside the animation phase, once per animated
	// character. Letting it switch scenes or write savegames from there is a
	// footgun the editor should not hand over, so its palette is an allow-list.
	//
	// This checks the RULE both menus share (HE::api::groupAllowed). It lives in
	// the API rather than the editor precisely so they cannot disagree:
	// filtering the add menu and forgetting the drag-off menu leaves a
	// restriction you can drag around.
	const std::vector<const char*> allowed = {
		"animator", "entity", "transform", "physics", "player", "math", "time",
	};

	CHECK(HE::api::groupAllowed("animator.setParam", allowed));
	CHECK(HE::api::groupAllowed("entity.self", allowed));
	CHECK(HE::api::groupAllowed("physics.isGrounded", allowed));
	CHECK(HE::api::groupAllowed("math.length3", allowed));

	// The ones a sync graph has no business doing from inside the animation pass.
	CHECK_FALSE(HE::api::groupAllowed("scene.load", allowed));
	CHECK_FALSE(HE::api::groupAllowed("save.setNumber", allowed));
	CHECK_FALSE(HE::api::groupAllowed("widget.create", allowed));
	CHECK_FALSE(HE::api::groupAllowed("audio.play", allowed));

	// A prefix that merely starts the same must not slip through — the group is
	// the id up to the first dot, not a string prefix.
	CHECK_FALSE(HE::api::groupAllowed("entityfoo.bar", allowed));
	CHECK_FALSE(HE::api::groupAllowed("ent.bar", allowed));

	// No list = the general-purpose editors, unchanged.
	CHECK(HE::api::groupAllowed("scene.load", {}));
}

TEST_CASE("AnimatorHost: a sync graph steers the state machine in the same frame")
{
	// The whole point of CP2. Before it, a parameter could only ever hold the
	// value its asset defaulted to, so an authored transition was unreachable.
	// The sync graph writes the parameter and the transition reads it — and
	// because the graph is fired from inside the animation phase, right before
	// the transitions, that happens within ONE tick rather than one frame later.
	TempDir dir("he_animator_host");
	ContentManager cm(dir.path.string());

	AnimatorStateMachineAsset asset;
	asset.type          = HE::AssetType::AnimatorStateMachine;
	asset.name          = "Locomotion";
	asset.path          = "Locomotion.hasset";
	asset.graphJson     = stateMachineJson();
	asset.syncGraphJson = toJson(syncGraphSettingSpeed(1.0f));
	REQUIRE(cm.saveAsset(asset));
	const HE::UUID smId = cm.loadAsset(asset.path);

	HorizonWorld world;
	const Entity e = makeAnimatedEntity(world, cm, smId);

	Runtime rt;
	bindApi(rt, world, cm);
	AnimatorHost host;
	host.begin(rt, world, cm);
	REQUIRE(host.count() == 1);

	// The start state is not set until the first update resolves the asset, so
	// before any tick the component has no state at all.
	CHECK(world.registry().get<AnimatorStateMachineComponent>(e).currentStateName == "");

	SceneSystems::tickAnimation(world, cm, 0.016f, &host);

	const auto& sm = world.registry().get<AnimatorStateMachineComponent>(e);
	CHECK(sm.params.at("speed") == doctest::Approx(1.0f));
	CHECK(sm.currentStateName == "Walk");
}

TEST_CASE("AnimatorHost: without it the same machine never leaves its start state")
{
	// The other half of the claim: it is the sync graph doing this, not the
	// state machine drifting on its own.
	TempDir dir("he_animator_host_off");
	ContentManager cm(dir.path.string());

	AnimatorStateMachineAsset asset;
	asset.type          = HE::AssetType::AnimatorStateMachine;
	asset.name          = "Locomotion";
	asset.path          = "Locomotion.hasset";
	asset.graphJson     = stateMachineJson();
	asset.syncGraphJson = toJson(syncGraphSettingSpeed(1.0f));
	REQUIRE(cm.saveAsset(asset));

	HorizonWorld world;
	const Entity e = makeAnimatedEntity(world, cm, cm.loadAsset(asset.path));

	// No host: this is edit mode, and also every scene before sync graphs existed.
	SceneSystems::tickAnimation(world, cm, 0.016f, nullptr);
	CHECK(world.registry().get<AnimatorStateMachineComponent>(e).currentStateName == "Idle");
}

TEST_CASE("AnimatorHost: an asset without a sync graph binds nothing")
{
	TempDir dir("he_animator_host_nosync");
	ContentManager cm(dir.path.string());

	AnimatorStateMachineAsset asset;
	asset.type      = HE::AssetType::AnimatorStateMachine;
	asset.name      = "Plain";
	asset.path      = "Plain.hasset";
	asset.graphJson = stateMachineJson();
	REQUIRE(cm.saveAsset(asset));

	HorizonWorld world;
	makeAnimatedEntity(world, cm, cm.loadAsset(asset.path));

	Runtime rt;
	bindApi(rt, world, cm);
	AnimatorHost host;
	host.begin(rt, world, cm);
	CHECK(host.count() == 0);   // nothing to run, and nothing complained
}

TEST_CASE("AnimatorHost: the sync graph survives a save/load round trip")
{
	// CHUNK_ASSY is new, and an asset written before it existed has no such
	// chunk — which must keep loading as "no sync graph" rather than failing.
	TempDir dir("he_animator_host_io");
	ContentManager cm(dir.path.string());

	AnimatorStateMachineAsset asset;
	asset.type          = HE::AssetType::AnimatorStateMachine;
	asset.name          = "Locomotion";
	asset.path          = "Locomotion.hasset";
	asset.graphJson     = stateMachineJson();
	asset.syncGraphJson = toJson(syncGraphSettingSpeed(2.5f));
	REQUIRE(cm.saveAsset(asset));

	ContentManager fresh(dir.path.string());
	const AnimatorStateMachineAsset* back = fresh.getAnimatorStateMachine(fresh.loadAsset(asset.path));
	REQUIRE(back != nullptr);
	CHECK(back->syncGraphJson == asset.syncGraphJson);

	Graph g;
	REQUIRE(fromJson(back->syncGraphJson, g));
	CHECK(g.nodes.size() == 5);
}

TEST_CASE("AnimatorHost: binding is idempotent, and a swapped asset rebinds")
{
	// bind() is called every frame from the animation phase — that is how an
	// entity which gains a state machine mid-session gets picked up — so calling
	// it repeatedly must not pile up instances.
	TempDir dir("he_animator_host_rebind");
	ContentManager cm(dir.path.string());

	auto write = [&](const char* name, float speed)
	{
		AnimatorStateMachineAsset a;
		a.type          = HE::AssetType::AnimatorStateMachine;
		a.name          = name;
		a.path          = std::string(name) + ".hasset";
		a.graphJson     = stateMachineJson();
		a.syncGraphJson = toJson(syncGraphSettingSpeed(speed));
		REQUIRE(cm.saveAsset(a));
		return cm.loadAsset(a.path);
	};
	const HE::UUID first  = write("First", 1.0f);
	const HE::UUID second = write("Second", 3.0f);

	HorizonWorld world;
	const Entity e = makeAnimatedEntity(world, cm, first);

	Runtime rt;
	bindApi(rt, world, cm);
	AnimatorHost host;
	host.begin(rt, world, cm);
	REQUIRE(host.count() == 1);

	for (int i = 0; i < 5; ++i) host.bind(e);
	CHECK(host.count() == 1);

	// Point the component at a different state machine: the old sync instance
	// must go, or the entity keeps running the previous asset's logic.
	world.registry().get<AnimatorStateMachineComponent>(e).stateMachineAssetId = second;
	host.bind(e);
	CHECK(host.count() == 1);

	SceneSystems::tickAnimation(world, cm, 0.016f, &host);
	CHECK(world.registry().get<AnimatorStateMachineComponent>(e).params.at("speed")
	      == doctest::Approx(3.0f));
}
