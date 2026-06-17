#include "doctest.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/ScriptComponent.h>
#include <HorizonScene/Components/EnvironmentComponent.h>
#include <filesystem>

namespace fs = std::filesystem;

namespace
{
	// Builds a small scene with every serialised component type.
	HE::UUID populate(HorizonWorld& world)
	{
		Entity cube = world.createEntity("Cube");
		TransformComponent t;
		t.position = { 1.0f, 2.0f, 3.0f };
		t.rotation = { 0.0f, 45.0f, 0.0f };
		t.scale    = { 2.0f, 2.0f, 2.0f };
		world.addComponent(cube, t);

		MeshComponent m;
		m.meshAssetId = HE::UUID::generate();
		m.castsShadow = false;
		world.addComponent(cube, m);

		Entity cam = world.createEntity("Camera");
		world.addComponent(cam, TransformComponent{});
		CameraComponent c;
		c.fovDegrees = 75.0f;
		c.isMain     = true;
		world.addComponent(cam, c);

		Entity sun = world.createEntity("Sun");
		world.addComponent(sun, TransformComponent{});
		LightComponent l;
		l.type        = LightType::Directional;
		l.intensity   = 3.5f;
		l.castsShadow = true;
		world.addComponent(sun, l);

		RigidBodyComponent rb;
		rb.type = RigidBodyType::Dynamic;
		rb.mass = 12.5f;
		world.addComponent(cube, rb);

		ScriptComponent sc;
		sc.scriptAssetId = HE::UUID::generate();
		sc.moduleName    = "spinner";
		world.addComponent(cube, sc);

		return m.meshAssetId;
	}

	void verify(HorizonWorld& world, const HE::UUID& meshId)
	{
		auto& reg = world.registry();

		int cubes = 0, cams = 0, lights = 0;
		for (auto [e, t, m] : reg.view<TransformComponent, MeshComponent>().each())
		{
			++cubes;
			CHECK(t.position.x == doctest::Approx(1.0f));
			CHECK(t.rotation.y == doctest::Approx(45.0f));
			CHECK(t.scale.z    == doctest::Approx(2.0f));
			CHECK(m.meshAssetId == meshId);
			CHECK_FALSE(m.castsShadow);

			auto* rb = reg.try_get<RigidBodyComponent>(e);
			REQUIRE(rb != nullptr);
			CHECK(rb->type == RigidBodyType::Dynamic);
			CHECK(rb->mass == doctest::Approx(12.5f));

			auto* sc = reg.try_get<ScriptComponent>(e);
			REQUIRE(sc != nullptr);
			CHECK(sc->moduleName == "spinner");
		}
		for (auto [e, c] : reg.view<CameraComponent>().each())
		{
			++cams;
			CHECK(c.fovDegrees == doctest::Approx(75.0f));
			CHECK(c.isMain);
		}
		for (auto [e, l] : reg.view<LightComponent>().each())
		{
			++lights;
			CHECK(l.type == LightType::Directional);
			CHECK(l.intensity == doctest::Approx(3.5f));
			CHECK(l.castsShadow);
		}
		CHECK(cubes  == 1);
		CHECK(cams   == 1);
		CHECK(lights == 1);
	}
}

TEST_CASE("SceneSerializer JSON round-trip with components")
{
	const fs::path file = fs::temp_directory_path() / "he_test_scene.hescene";

	HorizonWorld world;
	const HE::UUID meshId = populate(world);

	SceneSerializer ser;
	REQUIRE(ser.save(world, file, SerializeFormat::JSON));

	HorizonWorld loaded;
	REQUIRE(ser.load(loaded, file, SerializeFormat::JSON));
	verify(loaded, meshId);

	fs::remove(file);
}

TEST_CASE("SceneSerializer binary round-trip with components")
{
	const fs::path file = fs::temp_directory_path() / "he_test_scene.hescene_bin";

	HorizonWorld world;
	const HE::UUID meshId = populate(world);

	SceneSerializer ser;
	REQUIRE(ser.save(world, file, SerializeFormat::Binary));

	HorizonWorld loaded;
	REQUIRE(ser.load(loaded, file, SerializeFormat::Binary));
	verify(loaded, meshId);

	fs::remove(file);
}

TEST_CASE("World root identity survives round-trip with children")
{
	// Regression: entt's view iterates in reverse-creation order, so the root is
	// serialised LAST. The loader must map the root by parent==null, not by
	// position — otherwise it renamed the root to the first child and shredded the
	// hierarchy on every save/load and undo.
	const fs::path file = fs::temp_directory_path() / "he_test_root.hescene";

	HorizonWorld world;
	world.createEntity("Alpha");
	world.createEntity("Beta");
	world.createEntity("Gamma"); // root ("World", id 0) is now created first → serialised last

	SceneSerializer ser;
	REQUIRE(ser.save(world, file, SerializeFormat::JSON));

	HorizonWorld loaded;
	REQUIRE(ser.load(loaded, file, SerializeFormat::JSON));

	auto& lreg = loaded.registry();
	Entity root = loaded.rootEntity();
	CHECK(lreg.get<NameComponent>(root).name == "World"); // not renamed to a child
	auto& rHier = lreg.get<HierarchyComponent>(root);
	CHECK(rHier.children.size() == 3);                    // Alpha/Beta/Gamma reparented to root
	for (Entity c : rHier.children)
		CHECK(lreg.get<HierarchyComponent>(c).parent == root);

	fs::remove(file);
}

TEST_CASE("EnvironmentComponent on the World root round-trips")
{
	const fs::path file = fs::temp_directory_path() / "he_test_env.hescene";

	HorizonWorld world;
	world.createEntity("Decoy"); // ensure the root is not the only/first entity
	auto& env = world.registry().get<EnvironmentComponent>(world.rootEntity());
	env.dayNightCycle  = true;
	env.timeOfDay      = 0.73f;
	env.autoAdvance    = true;
	env.cycleSeconds   = 42.0f;
	env.sunIntensity   = 3.5f;
	env.cloudCoverage  = 0.8f;
	env.fogDensity     = 0.05f;
	env.auroraIntensity = 0.6f;
	env.nebulaColor    = glm::vec3(0.1f, 0.2f, 0.3f);

	SceneSerializer ser;
	REQUIRE(ser.save(world, file, SerializeFormat::JSON));

	HorizonWorld loaded;
	REQUIRE(ser.load(loaded, file, SerializeFormat::JSON));
	const auto& le = loaded.registry().get<EnvironmentComponent>(loaded.rootEntity());
	CHECK(le.dayNightCycle  == true);
	CHECK(le.timeOfDay      == doctest::Approx(0.73f));
	CHECK(le.autoAdvance    == true);
	CHECK(le.cycleSeconds   == doctest::Approx(42.0f));
	CHECK(le.sunIntensity   == doctest::Approx(3.5f));
	CHECK(le.cloudCoverage  == doctest::Approx(0.8f));
	CHECK(le.fogDensity     == doctest::Approx(0.05f));
	CHECK(le.auroraIntensity == doctest::Approx(0.6f));
	CHECK(le.nebulaColor.b  == doctest::Approx(0.3f));

	fs::remove(file);
}

TEST_CASE("Play-mode cycle: snapshot, clear, restore")
{
	const fs::path file = fs::temp_directory_path() / "he_test_playmode.hescene_bin";

	HorizonWorld world;
	const HE::UUID meshId = populate(world);

	SceneSerializer ser;
	REQUIRE(ser.save(world, file, SerializeFormat::Binary));

	// "Play" mutates the world, "Stop" clears and restores
	world.createEntity("SpawnedDuringPlay");
	world.clear();

	// Only the root must survive the clear
	size_t count = 0;
	for (auto e : world.registry().view<entt::entity>()) { (void)e; ++count; }
	CHECK(count == 1);

	REQUIRE(ser.load(world, file, SerializeFormat::Binary));
	verify(world, meshId);

	// The play-time entity must be gone
	for (auto [e, name] : world.registry().view<NameComponent>().each())
		CHECK(name.name != "SpawnedDuringPlay");

	fs::remove(file);
}

TEST_CASE("SceneSerializer hierarchy survives round-trip")
{
	const fs::path file = fs::temp_directory_path() / "he_test_hier.hescene";

	HorizonWorld world;
	Entity parent = world.createEntity("Parent");
	Entity child  = world.createEntity("Child");
	auto& reg = world.registry();

	// Reparent child under parent
	auto& pHier = reg.get<HierarchyComponent>(parent);
	auto& cHier = reg.get<HierarchyComponent>(child);
	auto& rHier = reg.get<HierarchyComponent>(world.rootEntity());
	std::erase(rHier.children, child);
	pHier.children.push_back(child);
	cHier.parent = parent;

	SceneSerializer ser;
	REQUIRE(ser.save(world, file, SerializeFormat::JSON));

	HorizonWorld loaded;
	REQUIRE(ser.load(loaded, file, SerializeFormat::JSON));

	auto& lreg = loaded.registry();
	Entity lParent = entt::null;
	for (auto [e, name] : lreg.view<NameComponent>().each())
		if (name.name == "Parent") lParent = e;
	REQUIRE((lParent != entt::null));

	auto* lHier = lreg.try_get<HierarchyComponent>(lParent);
	REQUIRE(lHier != nullptr);
	REQUIRE(lHier->children.size() == 1);
	CHECK(lreg.get<NameComponent>(lHier->children[0]).name == "Child");

	fs::remove(file);
}
