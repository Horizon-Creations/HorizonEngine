#include "doctest.h"
#include "TestFsUtil.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <HorizonScene/AnimatorHost.h>
#include <HorizonScene/EntityHost.h>
#include <HorizonScene/Components/ScriptComponent.h>
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

TEST_CASE("entity.instance answers with the CLASS on an entity, not the sync graph")
{
	// Cast takes an object reference, and Get Owning Entity gives an entity —
	// so a sync graph could not reach the character's own class at all until
	// this row existed.
	//
	// It cannot come from the runtime: TWO instances own the animated entity —
	// the character's class and the sync graph itself, because both call
	// setOwnedEntity. A reverse lookup there would be ambiguous by
	// construction, so the question goes to EntityHost, which holds exactly the
	// class bindings.
	TempDir dir("he_entity_instance");
	ContentManager cm(dir.path.string());

	// A minimal Entity class to bind to the character.
	HorizonCodeClassAsset cls;
	cls.type      = HE::AssetType::HorizonCodeClass;
	cls.name      = "Hero";
	cls.path      = "Hero.hasset";
	cls.baseClass = "Entity";
	{ Graph g; Node ev; ev.type = NodeType::Event; ev.s = "BeginPlay"; g.addNode(std::move(ev));
	  cls.graphJson = toJson(g); }
	REQUIRE(cm.saveAsset(cls));

	AnimatorStateMachineAsset sm;
	sm.type          = HE::AssetType::AnimatorStateMachine;
	sm.name          = "Locomotion";
	sm.path          = "Locomotion.hasset";
	sm.graphJson     = stateMachineJson();
	sm.syncGraphJson = toJson(syncGraphSettingSpeed(1.0f));
	REQUIRE(cm.saveAsset(sm));

	HorizonWorld world;
	const Entity e = makeAnimatedEntity(world, cm, cm.loadAsset(sm.path));
	{
		ScriptComponent sc;
		sc.scriptAssetId = cm.loadAsset(cls.path);
		world.addComponent(e, sc);
	}

	Runtime rt;
	bindApi(rt, world, cm);
	EntityHost entities;
	entities.begin(rt, world, cm);
	AnimatorHost animators;
	animators.begin(rt, world, cm);

	// Both really do own the same entity — that is the ambiguity this row steps
	// around, so it is worth stating rather than assuming.
	const InstanceId classInst = entities.instanceOf(e);
	REQUIRE(classInst != 0);
	REQUIRE(animators.count() == 1);

	HE::api::Ctx c{ &world, nullptr, &cm, nullptr, &rt, 0, &entities };
	CHECK(HE::api::entity::instance(c, (HE::api::Entity)e) == classInst);

	// …and the one-step version, asked from the vantage point that matters: the
	// SYNC instance. Both own the same entity, so "my owner's object" has to
	// answer with the character's class and not with the asker itself.
	const InstanceId syncInst = animators.instanceOf(e);
	REQUIRE(syncInst != 0);
	REQUIRE(syncInst != classInst);   // genuinely two instances on one entity

	HE::api::Ctx fromSync{ &world, nullptr, &cm, nullptr, &rt, syncInst, &entities };
	CHECK(HE::api::entity::selfObject(fromSync) == classInst);

	// No EntityHost in the context (edit mode, a bare script call): 0, not a
	// guess.
	HE::api::Ctx bare{ &world, nullptr, &cm, nullptr, &rt, 0 };
	CHECK(HE::api::entity::instance(bare, (HE::api::Entity)e) == 0u);
}

TEST_CASE("AnimatorHost: a sync graph's variables ARE the machine's parameters")
{
	// Declaring what the machine reacts to, in the same place it is computed.
	// The States side's default params stay as defaults and as what the editor
	// shows, but a variable declared here is a parameter without anyone saying
	// so twice.
	TempDir dir("he_animator_host_vars");
	ContentManager cm(dir.path.string());

	// A graph with a Float variable `speed`, set to 1.5 on Update. No
	// animator.setParam call anywhere.
	Graph g;
	{
		Variable v; v.name = "speed"; v.type = PinType::Float; v.f[0] = 0.0f;
		g.variables.push_back(v);

		Node ev; ev.type = NodeType::Event; ev.s = "Update";
		ev.hasArg = true; ev.propType = PinType::Float;
		const int e = g.addNode(std::move(ev));

		Node k; k.type = NodeType::ConstFloat; k.f[0] = 1.5f;
		const int c = g.addNode(std::move(k));

		Node set; set.type = NodeType::SetVariable; set.s = "speed"; set.propType = PinType::Float;
		const int s = g.addNode(std::move(set));

		REQUIRE(g.connect(e, 0, s, 0));   // exec
		REQUIRE(g.connect(c, 0, s, 2));   // value (exec-in, exec-out, then data)
	}

	AnimatorStateMachineAsset asset;
	asset.type          = HE::AssetType::AnimatorStateMachine;
	asset.name          = "Locomotion";
	asset.path          = "Locomotion.hasset";
	asset.graphJson     = stateMachineJson();
	asset.syncGraphJson = toJson(g);
	REQUIRE(cm.saveAsset(asset));

	HorizonWorld world;
	const Entity e = makeAnimatedEntity(world, cm, cm.loadAsset(asset.path));

	Runtime rt;
	bindApi(rt, world, cm);
	AnimatorHost host;
	host.begin(rt, world, cm);
	REQUIRE(host.count() == 1);

	SceneSystems::tickAnimation(world, cm, 0.016f, &host);

	const auto& sm = world.registry().get<AnimatorStateMachineComponent>(e);
	CHECK(sm.params.at("speed") == doctest::Approx(1.5f));
	CHECK(sm.currentStateName == "Walk");   // 1.5 > 0.5, so the transition fired
}

TEST_CASE("AnimatorHost: a variable beats a setParam call for the same name")
{
	// The copy happens after the graph ran, so the variable is the later write.
	// Worth pinning down rather than leaving to whoever reads the code next:
	// the two paths exist for different callers (graph vs Lua/Python) and a
	// graph that used both would otherwise be a coin flip.
	TempDir dir("he_animator_host_precedence");
	ContentManager cm(dir.path.string());

	Graph g;
	{
		Variable v; v.name = "speed"; v.type = PinType::Float;
		g.variables.push_back(v);

		Node ev; ev.type = NodeType::Event; ev.s = "Update";
		ev.hasArg = true; ev.propType = PinType::Float;
		const int e = g.addNode(std::move(ev));

		// setParam("speed", 9) first…
		const HE::api::ApiFn* fn = HE::api::find("animator.setParam");
		REQUIRE(fn != nullptr);
		Node call; call.type = NodeType::EngineCall; call.s = fn->id; call.hasArg = fn->isExec;
		for (const auto& p : fn->params)  call.params.push_back({ p.name, p.type, p.isArray });
		for (const auto& r : fn->results) call.results.push_back({ r.name, r.type, r.isArray });
		const int set = g.addNode(std::move(call));
		{
			Node* n = g.findNode(set);
			n->pinDefaults[1] = Value::ofString("speed");
			n->pinDefaults[2] = Value::ofFloat(9.0f);
		}
		const HE::api::ApiFn* selfFn = HE::api::find("entity.self");
		Node selfCall; selfCall.type = NodeType::EngineCall; selfCall.s = selfFn->id;
		selfCall.hasArg = selfFn->isExec;
		for (const auto& r : selfFn->results) selfCall.results.push_back({ r.name, r.type, r.isArray });
		const int self = g.addNode(std::move(selfCall));
		REQUIRE(g.connect(self, 0, set, 2));

		// …then the variable, set to 2.
		Node k; k.type = NodeType::ConstFloat; k.f[0] = 2.0f;
		const int c = g.addNode(std::move(k));
		Node sv; sv.type = NodeType::SetVariable; sv.s = "speed"; sv.propType = PinType::Float;
		const int s = g.addNode(std::move(sv));

		REQUIRE(g.connect(e,   0, set, 0));
		REQUIRE(g.connect(set, 1, s,   0));   // setParam's exec-out → SetVariable
		REQUIRE(g.connect(c,   0, s,   2));
	}

	AnimatorStateMachineAsset asset;
	asset.type          = HE::AssetType::AnimatorStateMachine;
	asset.name          = "Locomotion";
	asset.path          = "Locomotion.hasset";
	asset.graphJson     = stateMachineJson();
	asset.syncGraphJson = toJson(g);
	REQUIRE(cm.saveAsset(asset));

	HorizonWorld world;
	const Entity e = makeAnimatedEntity(world, cm, cm.loadAsset(asset.path));

	Runtime rt;
	bindApi(rt, world, cm);
	AnimatorHost host;
	host.begin(rt, world, cm);

	SceneSystems::tickAnimation(world, cm, 0.016f, &host);
	CHECK(world.registry().get<AnimatorStateMachineComponent>(e).params.at("speed")
	      == doctest::Approx(2.0f));   // the variable, not the 9 from setParam
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
