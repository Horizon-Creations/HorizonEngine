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
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/CameraRigComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/PhysicsWorld.h>
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

	// …and a camera to see it with, as a CHILD. Without it, "how do I attach a
	// camera to my player" is a question the tool creates: the author would have
	// to know that a camera is its own entity, that a rig aims it, and that the
	// rig's target defaults to the possessed player. Shipping it wired up answers
	// all three, and the Outliner then reads "Player → Camera".
	{
		REQUIRE(w.registry().all_of<HierarchyComponent>(root));
		Entity camera = entt::null;
		for (Entity child : w.registry().get<HierarchyComponent>(root).children)
			if (w.registry().all_of<CameraComponent>(child)) { camera = child; break; }
		REQUIRE((camera != entt::null));
		CHECK(w.registry().all_of<CameraRigComponent>(camera));
		CHECK(w.registry().all_of<TransformComponent>(camera));
		// It has to be the camera the game renders through, or the fallback wins.
		CHECK(w.registry().get<CameraComponent>(camera).isMain);
		const auto& rig = w.registry().get<CameraRigComponent>(camera);
		CHECK(rig.mode == CameraRigComponent::Mode::ThirdPerson);
		// Empty target = "the possessed player", which is this very class.
		CHECK(rig.target == HE::UUID{});
	}

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

TEST_CASE("destroying an Entity-class object takes its body with it")
{
	// The other direction of the lifetime rule. The reaper covers "entity gone →
	// instance goes"; this is "object gone → entity goes", and without it a mesh
	// with no logic on it stays standing in the scene forever.
	//
	// The apps bind this as Runtime::Services::destroyObject; the ORDER is what
	// matters and is asserted here: the entity id has to be read BEFORE the
	// instance is destroyed (afterwards there is nothing left to ask) and the
	// entity destroyed AFTER, so Destruct can still reach its own entity.
	TempDir dir("he_test_entityhost_destroy");
	ContentManager cm(dir.path.string());
	const std::string cls = writeClass(cm, "Barrel", "Entity", lifecycleGraph());

	HorizonWorld world;
	Runtime rt;
	EntityHost host;
	host.begin(rt, world, cm);

	const EntityHost::Spawned s = host.spawn(cls);
	REQUIRE(s.instance != 0);
	REQUIRE(world.registry().valid(s.entity));

	// Exactly what the apps' destroyObject service does.
	const uint32_t owned = rt.ownedEntity(s.instance);
	CHECK(owned == static_cast<uint32_t>(s.entity));
	rt.destroy(s.instance);
	CHECK(world.registry().valid(static_cast<Entity>(owned)));   // Destruct still had it
	world.destroyEntity(static_cast<Entity>(owned));

	CHECK_FALSE(world.registry().valid(s.entity));
	CHECK_FALSE(rt.alive(s.instance));
	// The host notices on its next tick and lets go of both map directions.
	host.tick(0.016f);
	CHECK(host.count() == 0);
	CHECK(host.instanceOf(s.entity) == 0);
}

TEST_CASE("a streamed-in zone's entity classes are bound too")
{
	// begin() walks the world exactly once, so an entity that arrives LATER —
	// an additively loaded zone — would never have its class started. That is
	// what bindFor is for; the game's startScriptsFor calls both hosts with the
	// zone's new entities.
	TempDir dir("he_test_entityhost_zone");
	ContentManager cm(dir.path.string());
	const std::string cls = writeClass(cm, "Torch", "Entity", lifecycleGraph());

	HorizonWorld world;
	Runtime rt;
	EntityHost host;
	host.begin(rt, world, cm);
	REQUIRE(host.count() == 0);

	// The zone arrives.
	std::vector<Entity> created;
	for (int i = 0; i < 2; ++i)
	{
		const Entity e = world.createEntity("Torch");
		world.addComponent(e, TransformComponent{});
		ScriptComponent sc; sc.scriptAssetId = cm.loadAsset(cls);
		world.addComponent(e, sc);
		created.push_back(e);
	}
	created.push_back(world.createEntity("PlainProp"));   // no code: skipped, not an error

	CHECK(host.bindFor(created) == 2);
	CHECK(host.count() == 2);
	for (int i = 0; i < 2; ++i)
	{
		const InstanceId inst = host.instanceOf(created[i]);
		REQUIRE(inst != 0);
		CHECK(rt.getVariable(inst, "beginPlays").i == 1);
	}
	host.tick(0.016f);
	CHECK(rt.getVariable(host.instanceOf(created[0]), "ticks").i == 1);
}

TEST_CASE("a Tick that spawns and destroys entity classes does not walk a mutating map")
{
	// The regression: tick() used to iterate the live entity→instance map while
	// firing Tick, and Tick is GRAPH code — it can Create Object (inserting, and
	// possibly rehashing) or destroy one (erasing). Iterating across either is
	// undefined behaviour of the kind that survives every test and then crashes
	// in a shipped game, so it is driven here rather than reasoned about.
	TempDir dir("he_test_entityhost_reentrant");
	ContentManager cm(dir.path.string());
	const std::string cls = writeClass(cm, "Spawner", "Entity", lifecycleGraph());

	HorizonWorld world;
	Runtime rt;
	EntityHost host;
	host.begin(rt, world, cm);

	// Enough instances that the map has to rehash while the pass is running.
	for (int i = 0; i < 8; ++i) REQUIRE(host.spawn(cls).instance != 0);
	REQUIRE(host.count() == 8);

	// A host binding that spawns and destroys DURING the tick, which is exactly
	// what a graph doing Create Object / Destroy Object in its Tick amounts to.
	Runtime::Services svc;
	// Placement is not what this test is about: it hands the spawn nothing, which
	// is the "leave it where the class authored it" case.
	svc.createObject = [&](const std::string& p, const float*, const float*) -> uint32_t
	{ return host.spawn(p).instance; };
	svc.destroyObject = [&](uint32_t ref) { rt.destroy(ref); };
	rt.setServices(std::move(svc));

	for (int frame = 0; frame < 4; ++frame)
	{
		// Spawn from inside the pass…
		const EntityHost::Spawned s = host.spawn(cls);
		REQUIRE(s.instance != 0);
		// …and take one away, so both directions are exercised.
		world.destroyEntity(s.entity);
		CHECK_NOTHROW(host.tick(0.016f));
	}

	// Everything the reaper let go of is really gone in both directions.
	for (const auto& [raw, inst] : host.instances())
	{
		CHECK(rt.alive(inst));
		CHECK(world.registry().valid(static_cast<Entity>(raw)));
	}
}

TEST_CASE("tearing the host down while Destruct spawns does not corrupt its maps")
{
	TempDir dir("he_test_entityhost_teardown");
	ContentManager cm(dir.path.string());
	const std::string cls = writeClass(cm, "Ghost", "Entity", lifecycleGraph());

	HorizonWorld world;
	Runtime rt;
	EntityHost host;
	host.begin(rt, world, cm);
	for (int i = 0; i < 8; ++i) REQUIRE(host.spawn(cls).instance != 0);

	CHECK_NOTHROW(host.end());
	CHECK(host.count() == 0);
}

// Red under two independent source mutations, both applied, built and observed:
//   - deleting the `m_physics->addEntityTree(...)` call in EntityHost::spawn:
//     both hasPhysics checks go false and the ray finds nothing.
//   - making PhysicsWorld::buildBodyFor use the entity's LOCAL transform as its
//     pose instead of worldPoseOf(): the bodies exist but stand at the authored
//     origin, so the gun is not at the spawn point.
TEST_CASE("EntityHost::spawn gives the whole spawned subtree its physics, at the spawn point")
{
	// The ANSCHLUSS, not the class: PhysicsWorld::addEntityTree having a test of
	// its own says nothing about whether a spawn ever calls it. Everything here
	// goes through the real path — setPhysicsWorld, then spawn — so that removing
	// the one line in EntityHost::spawn that reaches for physics fails a test
	// instead of quietly shipping bodiless prefabs.
	TempDir dir("he_test_entityhost_physics");
	ContentManager cm(dir.path.string());

	// A turret-shaped prefab: a base, and a gun mounted 3 m above it. Both carry
	// bodies, which is what makes the SUBTREE part of the contract testable —
	// addEntity alone would build the base and leave the gun bodiless.
	std::vector<uint8_t> blob;
	{
		HorizonWorld scratch;
		const Entity base = scratch.createEntity("TurretBase");
		TransformComponent bt; bt.position = { 0.0f, 0.0f, 0.0f }; bt.scale = { 1.0f, 1.0f, 1.0f };
		scratch.addComponent(base, bt);
		RigidBodyComponent brb; brb.type = RigidBodyType::Static;
		scratch.addComponent(base, brb);

		const Entity gun = scratch.createEntity("TurretGun");
		TransformComponent gt; gt.position = { 0.0f, 3.0f, 0.0f }; gt.scale = { 1.0f, 1.0f, 1.0f };
		scratch.addComponent(gun, gt);
		RigidBodyComponent grb; grb.type = RigidBodyType::Static;
		scratch.addComponent(gun, grb);
		REQUIRE(scratch.reparentEntity(gun, base));

		SceneSerializer ser;
		blob = ser.serializeSubtree(scratch, base);
	}
	REQUIRE_FALSE(blob.empty());
	const std::string cls = writeClass(cm, "Turret", "Entity", lifecycleGraph(), blob);

	HorizonWorld world;
	Runtime      rt;
	EntityHost   host;
	PhysicsWorld phys;
	phys.initialize(world);          // empty world: only the spawn can add bodies
	host.setPhysicsWorld(&phys);
	host.begin(rt, world, cm);

	// Spawned far from the origin, because that is where the authored-origin bug
	// hides: at (0,0,0) a collider built from the local transform is right by
	// accident.
	const float where[3] = { 50.0f, 0.0f, -25.0f };
	const EntityHost::Spawned s = host.spawn(cls, entt::null, where);
	REQUIRE(s.instance != 0);
	REQUIRE((s.entity != entt::null));

	auto& reg = world.registry();
	REQUIRE(reg.all_of<HierarchyComponent>(s.entity));
	const auto& kids = reg.get<HierarchyComponent>(s.entity).children;
	REQUIRE(kids.size() == 1);
	const Entity gun = kids.front();

	// BEFORE THE CHANGE: spawn() never touched physics, so both of these were
	// false and a spawned turret stood in the level with nothing to shoot at it.
	CHECK(phys.hasPhysics(static_cast<uint32_t>(s.entity)));
	CHECK(phys.hasPhysics(static_cast<uint32_t>(gun)));

	const glm::vec3 down{ 0.0f, -1.0f, 0.0f };

	// The gun sits 3 m up at the SPAWN POINT — root placement composed with the
	// child's local offset. A ray from above finds the gun first, being higher.
	const auto onGun = phys.raycast({ 50.0f, 60.0f, -25.0f }, down, 200.0f);
	REQUIRE(onGun.hit);
	CHECK(onGun.entityId == static_cast<uint32_t>(gun));
	CHECK(onGun.point.y == doctest::Approx(3.5f).epsilon(0.02));

	// Nothing was left behind at the coordinates the prefab was authored around.
	CHECK_FALSE(phys.raycast({ 0.0f, 60.0f, 0.0f }, down, 200.0f).hit);
}
