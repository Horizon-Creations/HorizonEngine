// EntityHost: HorizonCode classes that live ON scene entities — the Entity
// branch of the class taxonomy. What matters here is the BINDING (which
// instance belongs to which entity, and how a graph gets from one to the other)
// and its LIFETIME in both directions, which nothing else in the suite covers:
// a leaked instance keeps ticking, keeps answering a Cast, and hands out a
// dangling entity id, and only a running game would ever notice.
#include "doctest.h"
#include "TestFsUtil.h"
#include <HorizonScene/EntityHost.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/Components/ScriptComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/Components/CharacterControllerComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/SceneSerializer.h>
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

	// A class graph that records each lifecycle event in its own Int variable,
	// so the test can tell Construct from BeginPlay from Tick from Destruct.
	Graph lifecycleGraph()
	{
		Graph g;
		for (const char* n : { "constructs", "beginPlays", "ticks", "destructs" })
		{
			Variable v; v.name = n; v.type = PinType::Int;
			g.variables.push_back(v);
		}
		auto count = [&g](const char* event, const char* var)
		{
			Node ev; ev.type = NodeType::Event; ev.s = event;
			const int e = g.addNode(std::move(ev));
			Node get; get.type = NodeType::GetVariable; get.s = var; get.propType = PinType::Int;
			const int gv = g.addNode(std::move(get));
			Node one; one.type = NodeType::ConstInt; one.f[0] = 1.0f;
			const int k = g.addNode(std::move(one));
			Node add; add.type = NodeType::Add;
			const int a = g.addNode(std::move(add));
			Node set; set.type = NodeType::SetVariable; set.s = var; set.propType = PinType::Int;
			const int s = g.addNode(std::move(set));
			// Add carries no exec pins, so its data-out is pin 2 (after A and B).
			REQUIRE(g.connect(gv, 0, a, 0));
			REQUIRE(g.connect(k,  0, a, 1));
			REQUIRE(g.connect(e,  0, s, 0));
			REQUIRE(g.connect(a,  2, s, 2));
		};
		count("Construct", "constructs");
		count("BeginPlay", "beginPlays");
		count("Tick",      "ticks");
		count("Destruct",  "destructs");
		return g;
	}

	// Write a HorizonCode class asset carrying `graph` and return its path.
	std::string writeClass(ContentManager& cm, const char* name,
	                       const char* baseClass, const Graph& g,
	                       const std::vector<uint8_t>& components = {})
	{
		HorizonCodeClassAsset a;
		a.type          = HE::AssetType::HorizonCodeClass;
		a.name          = name;
		a.path          = std::string(name) + ".hasset";
		a.baseClass     = baseClass;
		a.graphJson     = toJson(g);
		a.componentBlob = components;
		REQUIRE(cm.saveAsset(a));
		return a.path;
	}
}

TEST_CASE("EntityHost binds the classes named by ScriptComponent and runs their lifecycle")
{
	TempDir dir("he_test_entityhost_bind");
	ContentManager cm(dir.path.string());
	const std::string cls = writeClass(cm, "Door", "Entity", lifecycleGraph());

	HorizonWorld world;
	const Entity e = world.createEntity("Door1");
	world.addComponent(e, TransformComponent{});
	{
		ScriptComponent sc;
		sc.scriptAssetId = cm.loadAsset(cls);
		world.addComponent(e, sc);
	}
	// An entity with no code at all must simply be skipped, not warned about.
	world.createEntity("JustGeometry");

	Runtime rt;
	EntityHost host;
	host.begin(rt, world, cm);

	REQUIRE(host.count() == 1);
	const InstanceId inst = host.instanceOf(e);
	REQUIRE(inst != 0);
	CHECK(host.entityOf(inst) == e);

	// Construct AND BeginPlay fire on bind — an Entity class is something the
	// world runs, not something a reference-holder merely created.
	CHECK(rt.getVariable(inst, "constructs").i == 1);
	CHECK(rt.getVariable(inst, "beginPlays").i == 1);
	CHECK(rt.getVariable(inst, "ticks").i == 0);

	host.tick(0.016f);
	host.tick(0.016f);
	CHECK(rt.getVariable(inst, "ticks").i == 2);

	// The class identity came from the ASSET, so the instance is castable.
	CHECK(rt.instanceIsA(inst, cls));
	CHECK(rt.instanceIsA(inst, "Entity"));
	CHECK_FALSE(rt.instanceIsA(inst, "PlayerCharacter"));

	host.end();
	CHECK(host.count() == 0);
	CHECK_FALSE(rt.alive(inst));
}

TEST_CASE("EntityHost: destroying the entity takes its instance with it")
{
	// Without this an instance whose entity is gone keeps ticking, keeps
	// answering a Cast, and entityOf hands out a dangling id. Watching the
	// registry in tick() covers every path that can remove an entity —
	// entity.destroy from a graph, the outliner, a scene edit — without any of
	// them having to know this host exists.
	TempDir dir("he_test_entityhost_reap");
	ContentManager cm(dir.path.string());
	const std::string cls = writeClass(cm, "Crate", "Entity", lifecycleGraph());

	HorizonWorld world;
	const Entity e = world.createEntity("Crate1");
	world.addComponent(e, TransformComponent{});
	{
		ScriptComponent sc; sc.scriptAssetId = cm.loadAsset(cls);
		world.addComponent(e, sc);
	}

	Runtime rt;
	EntityHost host;
	host.begin(rt, world, cm);
	const InstanceId inst = host.instanceOf(e);
	REQUIRE(inst != 0);

	world.destroyEntity(e);
	host.tick(0.016f);

	CHECK(host.count() == 0);
	CHECK(host.instanceOf(e) == 0);
	CHECK((host.entityOf(inst) == entt::null));   // parens: doctest cannot decompose entt::null
	CHECK_FALSE(rt.alive(inst));
	// A dead instance is an instance of nothing — which is what makes a Cast on
	// a stale reference take its Failure branch.
	CHECK_FALSE(rt.instanceIsA(inst, "Entity"));
}

TEST_CASE("EntityHost::spawn brings its own entity, and takes it back on failure")
{
	TempDir dir("he_test_entityhost_spawn");
	ContentManager cm(dir.path.string());
	const std::string cls = writeClass(cm, "Bullet", "Entity", lifecycleGraph());

	HorizonWorld world;
	Runtime rt;
	EntityHost host;
	host.begin(rt, world, cm);

	const EntityHost::Spawned s = host.spawn(cls);
	REQUIRE(s.instance != 0);
	REQUIRE((s.entity != entt::null));
	CHECK(world.registry().valid(s.entity));
	CHECK(host.entityOf(s.instance) == s.entity);
	CHECK(rt.getVariable(s.instance, "beginPlays").i == 1);

	// A class that does not exist must not leave a body without logic standing
	// in the scene.
	const size_t before = world.registry().storage<entt::entity>().in_use();
	const EntityHost::Spawned bad = host.spawn("Nope.hasset");
	CHECK(bad.instance == 0);
	CHECK((bad.entity == entt::null));
	CHECK(world.registry().storage<entt::entity>().in_use() == before);
}

TEST_CASE("a graph reaches its own entity through the engine API")
{
	// entity.self is the one row that cannot be answered from world state: it
	// depends on WHO is calling, which is why the calling instance rides in the
	// Ctx alongside the world.
	TempDir dir("he_test_entityhost_self");
	ContentManager cm(dir.path.string());
	const std::string cls = writeClass(cm, "Lamp", "Entity", lifecycleGraph());

	HorizonWorld world;
	const Entity e = world.createEntity("Lamp1");
	world.addComponent(e, TransformComponent{});
	{
		ScriptComponent sc; sc.scriptAssetId = cm.loadAsset(cls);
		world.addComponent(e, sc);
	}

	Runtime rt;
	EntityHost host;
	host.begin(rt, world, cm);
	const InstanceId inst = host.instanceOf(e);
	REQUIRE(inst != 0);

	HE::api::Ctx c{ &world, nullptr, &cm, nullptr, &rt, inst };
	CHECK(HE::api::entity::self(c) == static_cast<uint32_t>(e));
	CHECK(HE::api::entity::owned(c, inst) == static_cast<uint32_t>(e));

	// A caller that is not a HorizonCode instance, and an object that owns no
	// entity, both answer 0 rather than something arbitrary.
	HE::api::Ctx outside{ &world, nullptr, &cm, nullptr, &rt, 0 };
	CHECK(HE::api::entity::self(outside) == 0u);
	const InstanceId plain = rt.add(Graph{});
	CHECK(HE::api::entity::owned(c, plain) == 0u);
}

// ── Input routing: the controller is the central point of contact ───────────

namespace
{
	// A graph that counts one named event into an Int variable.
	Graph eventCounter(const char* event, const char* var)
	{
		Graph g;
		Variable v; v.name = var; v.type = PinType::Int; g.variables.push_back(v);
		Node ev; ev.type = NodeType::Event; ev.s = event;
		const int e = g.addNode(std::move(ev));
		Node get; get.type = NodeType::GetVariable; get.s = var; get.propType = PinType::Int;
		const int gv = g.addNode(std::move(get));
		Node one; one.type = NodeType::ConstInt; one.f[0] = 1.0f;
		const int k = g.addNode(std::move(one));
		Node add; add.type = NodeType::Add;
		const int a = g.addNode(std::move(add));
		Node set; set.type = NodeType::SetVariable; set.s = var; set.propType = PinType::Int;
		const int s = g.addNode(std::move(set));
		REQUIRE(g.connect(gv, 0, a, 0));
		REQUIRE(g.connect(k,  0, a, 1));
		REQUIRE(g.connect(e,  0, s, 0));
		REQUIRE(g.connect(a,  2, s, 2));   // Add's data-out is pin 2 (no exec pins)
		return g;
	}
}

TEST_CASE("input reaches the controller always, and the possessed character too")
{
	// The routing rule, in one place: a PlayerController is the engine's central
	// point of contact and handles input whether or not it possesses anything;
	// possessing a character makes the controller FORWARD the same event, it does
	// not make the controller passive.
	Runtime rt;
	const InstanceId ctrl = rt.add(eventCounter("Input.Jump.Pressed", "got"), {},
	                               { "Content/PC.hasset", "PlayerController" });
	const InstanceId pawn = rt.add(eventCounter("Input.Jump.Pressed", "got"), {},
	                               { "Content/Hero.hasset", "PlayerCharacter" });

	HE::api::player::clear();
	HE::api::player::setControllers({ ctrl });

	// Unpossessed: the controller still handles its own input.
	rt.fireEvent(ctrl, "Input.Jump.Pressed");
	CHECK(rt.getVariable(ctrl, "got").i == 1);
	CHECK(rt.getVariable(pawn, "got").i == 0);

	// Possessed: BOTH get it — the controller does not go quiet.
	HE::api::player::possess(ctrl, pawn);
	CHECK(HE::api::player::possessed(ctrl) == pawn);
	rt.fireEvent(ctrl, "Input.Jump.Pressed");
	rt.fireEvent(HE::api::player::possessed(ctrl), "Input.Jump.Pressed");
	CHECK(rt.getVariable(ctrl, "got").i == 2);
	CHECK(rt.getVariable(pawn, "got").i == 1);

	// A destroyed character must not be fired at — the handle stays in the table
	// until someone unpossesses, so the liveness check is what covers it.
	rt.destroy(pawn);
	CHECK_FALSE(rt.alive(HE::api::player::possessed(ctrl)));
	HE::api::player::clear();
}

// ── The class's component list ──────────────────────────────────────────────

TEST_CASE("a base class hands a new class the components it cannot work without")
{
	// "Erben" made concrete: a PlayerCharacter arrives with what a player needs
	// to be moved and hit, rather than as a bare transform. Built from the live
	// component types (not a checked-in blob), so a field added to one of them
	// follows automatically.
	CHECK(EntityHost::defaultComponents("").empty());                 // Object has no body
	CHECK(EntityHost::defaultComponents("PlayerController").empty()); // nor does a controller
	CHECK_FALSE(EntityHost::defaultComponents("Entity").empty());

	HorizonWorld w;
	SceneSerializer ser;
	const Entity root = ser.instantiatePrefab(w, EntityHost::defaultComponents("PlayerCharacter"));
	REQUIRE((root != entt::null));
	CHECK(w.registry().all_of<TransformComponent>(root));
	CHECK(w.registry().all_of<CharacterControllerComponent>(root));
	CHECK(w.registry().all_of<ColliderComponent>(root));
	// The mesh SLOT is there but unassigned: which mesh it is is the whole point
	// of the class, so a guess would only be something to delete.
	REQUIRE(w.registry().all_of<SkeletalMeshComponent>(root));
	CHECK(w.registry().get<SkeletalMeshComponent>(root).meshAssetId == HE::UUID{});

	// A plain Entity gets a place in the world and nothing more.
	HorizonWorld w2;
	const Entity e2 = ser.instantiatePrefab(w2, EntityHost::defaultComponents("Entity"));
	REQUIRE((e2 != entt::null));
	CHECK(w2.registry().all_of<TransformComponent>(e2));
	CHECK_FALSE(w2.registry().all_of<CharacterControllerComponent>(e2));
}

TEST_CASE("a spawned class brings its authored components, and survives a save/load")
{
	TempDir dir("he_test_entityhost_components");
	ContentManager cm(dir.path.string());

	// Author a body: a named root with a transform and a collider.
	std::vector<uint8_t> blob;
	{
		HorizonWorld scratch;
		const Entity r = scratch.createEntity("CrateBody");
		TransformComponent t; t.position = { 3.0f, 4.0f, 5.0f };
		scratch.addComponent(r, t);
		ColliderComponent col; col.shape = ColliderShape::Sphere; col.radius = 2.5f;
		scratch.addComponent(r, col);
		SceneSerializer ser;
		blob = ser.serializeSubtree(scratch, r);
	}
	REQUIRE_FALSE(blob.empty());
	const std::string cls = writeClass(cm, "Crate", "Entity", lifecycleGraph(), blob);

	// The blob is a chunk on the asset, so it has to come back off disk intact —
	// a fresh ContentManager reads the file, not the in-memory copy.
	{
		ContentManager reload(dir.path.string());
		const HorizonCodeClassAsset* a = reload.getHorizonCodeClass(reload.loadAsset(cls));
		REQUIRE(a != nullptr);
		CHECK(a->componentBlob == blob);
		CHECK(a->baseClass == "Entity");
	}

	HorizonWorld world;
	Runtime rt;
	EntityHost host;
	host.begin(rt, world, cm);

	const EntityHost::Spawned s = host.spawn(cls);
	REQUIRE(s.instance != 0);
	REQUIRE((s.entity != entt::null));
	// The spawned entity IS the authored subtree, not a bare stand-in.
	auto& reg = world.registry();
	REQUIRE(reg.all_of<TransformComponent>(s.entity));
	CHECK(reg.get<TransformComponent>(s.entity).position.x == doctest::Approx(3.0f));
	REQUIRE(reg.all_of<ColliderComponent>(s.entity));
	CHECK(reg.get<ColliderComponent>(s.entity).radius == doctest::Approx(2.5f));
	// …and it is bound to the instance, so the graph can reach it.
	CHECK(host.entityOf(s.instance) == s.entity);
	HE::api::Ctx c{ &world, nullptr, &cm, nullptr, &rt, s.instance };
	CHECK(HE::api::entity::self(c) == static_cast<uint32_t>(s.entity));
}
