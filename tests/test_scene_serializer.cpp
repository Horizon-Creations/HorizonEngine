#include "doctest.h"
#include "TestFsUtil.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/ScriptComponent.h>
#include <HorizonScene/Components/EnvironmentComponent.h>
#include <HorizonScene/Components/EnvironmentLightComponent.h>
#include <HorizonScene/Components/AnimatorStateMachineComponent.h>
#include <HorizonScene/Components/AnimatorComponent.h>
#include <HorizonScene/Components/AnimatorBlendComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/PropertyAnimatorComponent.h>
#include <HorizonScene/Components/NavMeshComponent.h>
#include <HorizonScene/Components/NavAgentComponent.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>

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
		ScriptPropValue spd; spd.type = ScriptPropType::Float; spd.f = 3.5f;
		ScriptPropValue lv;  lv.type  = ScriptPropType::Int;   lv.i  = 5;
		ScriptPropValue vis; vis.type = ScriptPropType::Bool;   vis.b = true;
		ScriptPropValue tag; tag.type = ScriptPropType::String; tag.s = "hero";
		sc.properties["speed"]   = spd;
		sc.properties["lives"]   = lv;
		sc.properties["visible"] = vis;
		sc.properties["tag"]     = tag;
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
			REQUIRE(sc->properties.count("speed"));
			CHECK(sc->properties.at("speed").type == ScriptPropType::Float);
			CHECK(sc->properties.at("speed").f == doctest::Approx(3.5f));
			REQUIRE(sc->properties.count("lives"));
			CHECK(sc->properties.at("lives").type == ScriptPropType::Int);
			CHECK(sc->properties.at("lives").i == 5);
			REQUIRE(sc->properties.count("visible"));
			CHECK(sc->properties.at("visible").type == ScriptPropType::Bool);
			CHECK(sc->properties.at("visible").b == true);
			REQUIRE(sc->properties.count("tag"));
			CHECK(sc->properties.at("tag").type == ScriptPropType::String);
			CHECK(sc->properties.at("tag").s == "hero");
		}
		for (auto [e, c] : reg.view<CameraComponent>().each())
		{
			++cams;
			CHECK(c.fovDegrees == doctest::Approx(75.0f));
			CHECK(c.isMain);
		}
		for (auto [e, l] : reg.view<LightComponent>().each())
		{
			if (reg.all_of<EnvironmentLightComponent>(e)) continue; // skip built-in sun/moon
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

	he_test::removeQuiet(file);
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

	he_test::removeQuiet(file);
}

TEST_CASE("SceneSerializer round-trips per-entity material param overrides")
{
	for (SerializeFormat fmt : { SerializeFormat::JSON, SerializeFormat::Binary })
	{
		const fs::path file = fs::temp_directory_path() / "he_test_paramov.hescene";
		HorizonWorld world;
		auto e = world.createEntity("Overridden");
		MaterialComponent mc;
		mc.materialAssetId = HE::UUID::generate();
		MaterialParamOverride a; a.name = "K";    a.value[0] = 0.42f;
		MaterialParamOverride b; b.name = "Tint"; b.value[0] = 0.1f; b.value[1] = 0.2f; b.value[2] = 0.3f; b.value[3] = 1.0f;
		mc.paramOverrides = { a, b };
		world.registry().emplace<MaterialComponent>(e, mc);

		SceneSerializer ser;
		REQUIRE(ser.save(world, file, fmt));
		HorizonWorld loaded;
		REQUIRE(ser.load(loaded, file, fmt));

		// Find the sole entity with a MaterialComponent and check its overrides.
		bool found = false;
		for (auto [le, lm] : loaded.registry().view<MaterialComponent>().each())
		{
			found = true;
			REQUIRE(lm.paramOverrides.size() == 2);
			CHECK(lm.paramOverrides[0].name == "K");
			CHECK(lm.paramOverrides[0].value[0] == doctest::Approx(0.42f));
			CHECK(lm.paramOverrides[1].name == "Tint");
			CHECK(lm.paramOverrides[1].value[3] == doctest::Approx(1.0f));
		}
		CHECK(found);
		he_test::removeQuiet(file);
	}
}

TEST_CASE("SceneSerializer round-trips AnimatorStateMachineComponent (asset reference + per-entity runtime state)")
{
	// The graph itself (states/transitions/default params) lives in the
	// referenced AnimatorStateMachineAsset (see ContentManager tests for that
	// round-trip) — SceneSerializer only owns the per-entity runtime slice:
	// which asset, which state it's currently in, and live param overrides.
	for (SerializeFormat fmt : { SerializeFormat::JSON, SerializeFormat::Binary })
	{
		const fs::path file = fs::temp_directory_path() / "he_test_animsm.hescene";
		HorizonWorld world;
		auto e = world.createEntity("Character");

		AnimatorStateMachineComponent sm;
		sm.stateMachineAssetId = HE::UUID::generate();
		sm.params["speed"] = 1.5f;
		sm.currentStateName = "Idle";
		world.registry().emplace<AnimatorStateMachineComponent>(e, sm);

		SceneSerializer ser;
		REQUIRE(ser.save(world, file, fmt));
		HorizonWorld loaded;
		REQUIRE(ser.load(loaded, file, fmt));

		bool found = false;
		for (auto [le, lsm] : loaded.registry().view<AnimatorStateMachineComponent>().each())
		{
			found = true;
			CHECK(lsm.stateMachineAssetId == sm.stateMachineAssetId);
			REQUIRE(lsm.params.count("speed"));
			CHECK(lsm.params.at("speed") == doctest::Approx(1.5f));
			CHECK(lsm.currentStateName == "Idle");
			CHECK_FALSE(lsm.legacy.hasData);
		}
		CHECK(found);
		he_test::removeQuiet(file);
	}
}

TEST_CASE("SceneSerializer stages a legacy inline state machine (pre-asset format) for migration, auto-assigning ids")
{
	// Scenes saved before Forts. 71 (this asset conversion) had the whole graph
	// INLINE on the component, no "stateMachineAsset" key, states with no id/x/y
	// at all. There's no code path left that WRITES that shape any more (the
	// component doesn't have states/transitions fields to write), so build it by
	// hand-rewriting a freshly saved file's "animstatemachine" block — same
	// technique as testing any other hand-edited/ancient save file.
	const fs::path file = fs::temp_directory_path() / "he_test_animsm_legacy.hescene";
	{
		HorizonWorld world;
		auto e = world.createEntity("Character");
		world.registry().emplace<AnimatorStateMachineComponent>(e);
		SceneSerializer ser;
		REQUIRE(ser.save(world, file, SerializeFormat::JSON));
	}
	{
		std::ifstream in(file);
		nlohmann::json scene; in >> scene; in.close();
		REQUIRE(scene.contains("entities"));
		REQUIRE(!scene["entities"].empty());

		nlohmann::json legacyStates = nlohmann::json::array();
		for (const char* name : { "Idle", "Walk", "Run", "Jump", "Fall" }) // no "id"/"x"/"y" at all
			legacyStates.push_back({ { "name", name }, { "looping", true } });
		nlohmann::json legacyTransitions = nlohmann::json::array();
		legacyTransitions.push_back({ { "fromState", "Idle" }, { "toState", "Walk" },
		                              { "paramName", "speed" }, { "op", 0 },
		                              { "threshold", 0.1f }, { "duration", 0.25f } });

		scene["entities"][0]["components"]["animstatemachine"] = {
			{ "states",           legacyStates },
			{ "transitions",      legacyTransitions },
			{ "params",           { { "speed", 1.5f } } },
			{ "currentStateName", "Idle" },
		};
		std::ofstream out(file);
		out << scene.dump();
	}

	HorizonWorld loaded;
	SceneSerializer ser;
	REQUIRE(ser.load(loaded, file, SerializeFormat::JSON));

	bool found = false;
	for (auto [le, lsm] : loaded.registry().view<AnimatorStateMachineComponent>().each())
	{
		found = true;
		CHECK(lsm.stateMachineAssetId == HE::UUID{}); // not migrated yet — needs
		                                               // AnimationStateMachineSystem::update + a ContentManager
		REQUIRE(lsm.legacy.hasData);
		REQUIRE(lsm.legacy.states.size() == 5);
		// Every id must now be non-zero and unique.
		std::vector<int> ids;
		for (const auto& s : lsm.legacy.states) { CHECK(s.id != 0); ids.push_back(s.id); }
		std::sort(ids.begin(), ids.end());
		CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end()); // no duplicates
		// Simple grid auto-layout: 4 columns, 200/150-unit spacing, first 4 in row 0.
		CHECK(lsm.legacy.states[0].x == doctest::Approx(0.0f));
		CHECK(lsm.legacy.states[0].y == doctest::Approx(0.0f));
		CHECK(lsm.legacy.states[3].x == doctest::Approx(600.0f));
		CHECK(lsm.legacy.states[3].y == doctest::Approx(0.0f));
		CHECK(lsm.legacy.states[4].x == doctest::Approx(0.0f));
		CHECK(lsm.legacy.states[4].y == doctest::Approx(150.0f));

		REQUIRE(lsm.legacy.transitions.size() == 1);
		CHECK(lsm.legacy.transitions[0].fromState == "Idle");
		CHECK(lsm.legacy.transitions[0].toState   == "Walk");
		CHECK(lsm.legacy.transitions[0].paramName == "speed");
		CHECK(lsm.legacy.transitions[0].op == HE::TransitionOp::Greater);
		CHECK(lsm.legacy.transitions[0].threshold == doctest::Approx(0.1f));
		CHECK(lsm.legacy.transitions[0].duration  == doctest::Approx(0.25f));

		REQUIRE(lsm.legacy.params.count("speed"));
		CHECK(lsm.legacy.params.at("speed") == doctest::Approx(1.5f));
		CHECK(lsm.legacy.currentStateName == "Idle");
	}
	CHECK(found);
	he_test::removeQuiet(file);
}

TEST_CASE("SceneSerializer round-trips SkeletalMeshComponent")
{
	for (SerializeFormat fmt : { SerializeFormat::JSON, SerializeFormat::Binary })
	{
		const fs::path file = fs::temp_directory_path() / "he_test_skeletalmesh.hescene";
		HorizonWorld world;
		auto e = world.createEntity("Character");

		SkeletalMeshComponent sk;
		sk.meshAssetId     = HE::UUID::generate();
		sk.visible         = false;
		sk.castsShadow     = false;
		sk.receivesShadow  = false;
		world.registry().emplace<SkeletalMeshComponent>(e, sk);

		SceneSerializer ser;
		REQUIRE(ser.save(world, file, fmt));
		HorizonWorld loaded;
		REQUIRE(ser.load(loaded, file, fmt));

		bool found = false;
		for (auto [le, lsk] : loaded.registry().view<SkeletalMeshComponent>().each())
		{
			found = true;
			CHECK(lsk.meshAssetId == sk.meshAssetId);
			CHECK(lsk.visible        == false);
			CHECK(lsk.castsShadow    == false);
			CHECK(lsk.receivesShadow == false);
		}
		CHECK(found);
		he_test::removeQuiet(file);
	}
}

TEST_CASE("SceneSerializer round-trips AnimatorComponent")
{
	for (SerializeFormat fmt : { SerializeFormat::JSON, SerializeFormat::Binary })
	{
		const fs::path file = fs::temp_directory_path() / "he_test_animator.hescene";
		HorizonWorld world;
		auto e = world.createEntity("Character");

		AnimatorComponent an;
		an.clipAssetId   = HE::UUID::generate();
		an.playbackTime  = 1.25f;
		an.playbackSpeed = 2.0f;
		an.looping       = false;
		an.playing       = false;
		world.registry().emplace<AnimatorComponent>(e, an);

		SceneSerializer ser;
		REQUIRE(ser.save(world, file, fmt));
		HorizonWorld loaded;
		REQUIRE(ser.load(loaded, file, fmt));

		bool found = false;
		for (auto [le, lan] : loaded.registry().view<AnimatorComponent>().each())
		{
			found = true;
			CHECK(lan.clipAssetId == an.clipAssetId);
			CHECK(lan.playbackTime  == doctest::Approx(1.25f));
			CHECK(lan.playbackSpeed == doctest::Approx(2.0f));
			CHECK(lan.looping == false);
			CHECK(lan.playing == false);
		}
		CHECK(found);
		he_test::removeQuiet(file);
	}
}

TEST_CASE("SceneSerializer round-trips AnimatorBlendComponent")
{
	for (SerializeFormat fmt : { SerializeFormat::JSON, SerializeFormat::Binary })
	{
		const fs::path file = fs::temp_directory_path() / "he_test_animatorblend.hescene";
		HorizonWorld world;
		auto e = world.createEntity("Character");

		AnimatorBlendComponent ab;
		ab.clipAId       = HE::UUID::generate();
		ab.clipBId       = HE::UUID::generate();
		ab.blendAlpha    = 0.75f;
		ab.playbackTime  = 3.5f;
		ab.playbackSpeed = 0.5f;
		ab.looping       = false;
		ab.playing       = false;
		world.registry().emplace<AnimatorBlendComponent>(e, ab);

		SceneSerializer ser;
		REQUIRE(ser.save(world, file, fmt));
		HorizonWorld loaded;
		REQUIRE(ser.load(loaded, file, fmt));

		bool found = false;
		for (auto [le, lab] : loaded.registry().view<AnimatorBlendComponent>().each())
		{
			found = true;
			CHECK(lab.clipAId == ab.clipAId);
			CHECK(lab.clipBId == ab.clipBId);
			CHECK(lab.blendAlpha    == doctest::Approx(0.75f));
			CHECK(lab.playbackTime  == doctest::Approx(3.5f));
			CHECK(lab.playbackSpeed == doctest::Approx(0.5f));
			CHECK(lab.looping == false);
			CHECK(lab.playing == false);
		}
		CHECK(found);
		he_test::removeQuiet(file);
	}
}

TEST_CASE("SceneSerializer round-trips PropertyAnimatorComponent")
{
	for (SerializeFormat fmt : { SerializeFormat::JSON, SerializeFormat::Binary })
	{
		const fs::path file = fs::temp_directory_path() / "he_test_propertyanimator.hescene";
		HorizonWorld world;
		auto e = world.createEntity("Door");

		PropertyAnimatorComponent pa;
		pa.clipId        = HE::UUID::generate();
		pa.playbackTime  = 0.4f;
		pa.playbackSpeed = 1.5f;
		pa.looping       = false;
		pa.playing       = false;
		world.registry().emplace<PropertyAnimatorComponent>(e, pa);

		SceneSerializer ser;
		REQUIRE(ser.save(world, file, fmt));
		HorizonWorld loaded;
		REQUIRE(ser.load(loaded, file, fmt));

		bool found = false;
		for (auto [le, lpa] : loaded.registry().view<PropertyAnimatorComponent>().each())
		{
			found = true;
			CHECK(lpa.clipId == pa.clipId);
			CHECK(lpa.playbackTime  == doctest::Approx(0.4f));
			CHECK(lpa.playbackSpeed == doctest::Approx(1.5f));
			CHECK(lpa.looping == false);
			CHECK(lpa.playing == false);
		}
		CHECK(found);
		he_test::removeQuiet(file);
	}
}

TEST_CASE("SceneSerializer round-trips NavMeshComponent (config + geometry, re-bakes on load)")
{
	// Flat 10x10 floor in the XZ plane — same shape used by test_navigation.cpp's
	// makeFlatFloor, small enough to bake instantly and exercise a real re-bake.
	NavMeshGeometry geo;
	geo.verts = {
		-5.0f, 0.0f,  5.0f,
		 5.0f, 0.0f,  5.0f,
		 5.0f, 0.0f, -5.0f,
		-5.0f, 0.0f, -5.0f,
	};
	geo.tris = { 0, 1, 2, 0, 2, 3 };

	for (SerializeFormat fmt : { SerializeFormat::JSON, SerializeFormat::Binary })
	{
		const fs::path file = fs::temp_directory_path() / "he_test_navmesh.hescene";
		HorizonWorld world;
		auto e = world.createEntity("Ground");

		NavMeshComponent nm;
		nm.config.cellSize          = 0.5f;
		nm.config.cellHeight        = 0.25f;
		nm.config.walkableHeight    = 1.8f;
		nm.config.walkableClimb     = 0.5f;
		nm.config.walkableRadius    = 0.4f;
		nm.config.maxSlope          = 30.0f;
		nm.config.maxEdgeLen        = 10.0f;
		nm.config.maxSimplification = 1.0f;
		nm.config.minRegionArea     = 6.0f;
		nm.config.mergeRegionArea   = 15.0f;
		nm.config.detailSampleDist  = 5.0f;
		nm.config.detailMaxError    = 0.8f;
		nm.geometry = geo;
		world.registry().emplace<NavMeshComponent>(e, nm);

		SceneSerializer ser;
		REQUIRE(ser.save(world, file, fmt));
		HorizonWorld loaded;
		REQUIRE(ser.load(loaded, file, fmt));

		bool found = false;
		for (auto [le, lnm] : loaded.registry().view<NavMeshComponent>().each())
		{
			found = true;
			CHECK(lnm.config.cellSize          == doctest::Approx(0.5f));
			CHECK(lnm.config.cellHeight        == doctest::Approx(0.25f));
			CHECK(lnm.config.walkableHeight    == doctest::Approx(1.8f));
			CHECK(lnm.config.walkableClimb     == doctest::Approx(0.5f));
			CHECK(lnm.config.walkableRadius    == doctest::Approx(0.4f));
			CHECK(lnm.config.maxSlope          == doctest::Approx(30.0f));
			CHECK(lnm.config.maxEdgeLen        == doctest::Approx(10.0f));
			CHECK(lnm.config.maxSimplification == doctest::Approx(1.0f));
			CHECK(lnm.config.minRegionArea     == doctest::Approx(6.0f));
			CHECK(lnm.config.mergeRegionArea   == doctest::Approx(15.0f));
			CHECK(lnm.config.detailSampleDist  == doctest::Approx(5.0f));
			CHECK(lnm.config.detailMaxError    == doctest::Approx(0.8f));

			REQUIRE(lnm.geometry.verts.size() == geo.verts.size());
			for (size_t i = 0; i < geo.verts.size(); ++i)
				CHECK(lnm.geometry.verts[i] == doctest::Approx(geo.verts[i]));
			REQUIRE(lnm.geometry.tris == geo.tris);

			// navMesh/navQuery aren't persisted — SceneSerializer re-bakes from the
			// restored geometry on load, so a loaded scene has a working NavMesh.
			CHECK((bool)lnm.navMesh);
			CHECK(!lnm.isDirty);
		}
		CHECK(found);
		he_test::removeQuiet(file);
	}
}

TEST_CASE("SceneSerializer round-trips NavAgentComponent")
{
	for (SerializeFormat fmt : { SerializeFormat::JSON, SerializeFormat::Binary })
	{
		const fs::path file = fs::temp_directory_path() / "he_test_navagent.hescene";
		HorizonWorld world;
		auto e = world.createEntity("Enemy");

		NavAgentComponent na;
		na.targetPos    = { 4.0f, 0.0f, -2.0f };
		na.speed        = 6.0f;
		na.stoppingDist = 0.5f;
		world.registry().emplace<NavAgentComponent>(e, na);

		SceneSerializer ser;
		REQUIRE(ser.save(world, file, fmt));
		HorizonWorld loaded;
		REQUIRE(ser.load(loaded, file, fmt));

		bool found = false;
		for (auto [le, lna] : loaded.registry().view<NavAgentComponent>().each())
		{
			found = true;
			CHECK(lna.targetPos.x == doctest::Approx(4.0f));
			CHECK(lna.targetPos.y == doctest::Approx(0.0f));
			CHECK(lna.targetPos.z == doctest::Approx(-2.0f));
			CHECK(lna.speed        == doctest::Approx(6.0f));
			CHECK(lna.stoppingDist == doctest::Approx(0.5f));
			// Runtime path state is not persisted — always reset on load.
			CHECK(lna.path.empty());
			CHECK(!lna.hasPath);
			CHECK(!lna.moving);
		}
		CHECK(found);
		he_test::removeQuiet(file);
	}
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
	int sceneChildren = 0;
	for (Entity c : rHier.children)
	{
		CHECK(lreg.get<HierarchyComponent>(c).parent == root);
		if (!lreg.all_of<EnvironmentLightComponent>(c)) ++sceneChildren; // exclude built-in sun/moon
	}
	CHECK(sceneChildren == 3); // Alpha/Beta/Gamma reparented to root

	he_test::removeQuiet(file);
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

	he_test::removeQuiet(file);
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

	// Only the root + the two built-in environment lights survive the clear; no
	// authored scene entity may remain.
	auto& creg = world.registry();
	int sceneEntities = 0;
	for (auto e : creg.view<entt::entity>())
		if (e != world.rootEntity() && !creg.all_of<EnvironmentLightComponent>(e))
			++sceneEntities;
	CHECK(sceneEntities == 0);

	REQUIRE(ser.load(world, file, SerializeFormat::Binary));
	verify(world, meshId);

	// The play-time entity must be gone
	for (auto [e, name] : world.registry().view<NameComponent>().each())
		CHECK(name.name != "SpawnedDuringPlay");

	he_test::removeQuiet(file);
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

	he_test::removeQuiet(file);
}

// ─────────────────────────────────────────────────────────────────────────────
//  loadAdditive: merges entities without clearing
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("SceneSerializer loadAdditive preserves existing entities")
{
    namespace fs = std::filesystem;
    const fs::path file = fs::temp_directory_path() / "he_test_additive.hescene";

    // Save a small scene to disk
    {
        HorizonWorld src;
        Entity child = src.createEntity("AdditiveChild");
        src.addComponent(child, TransformComponent{ .position = {5.0f, 0.0f, 0.0f},
                                                     .rotation = {},
                                                     .scale    = glm::vec3(1.0f) });
        SceneSerializer ser;
        REQUIRE(ser.save(src, file, SerializeFormat::JSON));
    }

    // Load additively into a world that already has an entity
    HorizonWorld world;
    Entity existing = world.createEntity("Existing");
    world.addComponent(existing, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });

    SceneSerializer ser;
    REQUIRE(ser.loadAdditive(world, file, SerializeFormat::JSON));

    // Both existing and loaded entities must be present
    auto& reg = world.registry();
    bool foundExisting = false, foundAdditive = false;
    for (auto [e, name] : reg.view<NameComponent>().each())
    {
        if (name.name == "Existing")      foundExisting = true;
        if (name.name == "AdditiveChild") foundAdditive = true;
    }
    CHECK(foundExisting);
    CHECK(foundAdditive);

    he_test::removeQuiet(file);
}

TEST_CASE("SceneSerializer loadAdditive does not clear the world")
{
    namespace fs = std::filesystem;
    const fs::path file = fs::temp_directory_path() / "he_test_additive2.hescene";

    // Scene to merge: single entity with a known component value
    {
        HorizonWorld src;
        Entity e = src.createEntity("MergedEntity");
        src.addComponent(e, TransformComponent{ .position = {3.0f, 0.0f, 0.0f},
                                                 .rotation = {},
                                                 .scale    = glm::vec3(1.0f) });
        SceneSerializer ser;
        REQUIRE(ser.save(src, file, SerializeFormat::JSON));
    }

    HorizonWorld world;
    // Add 3 entities to the base world
    world.createEntity("A");
    world.createEntity("B");
    world.createEntity("C");

    size_t before = 0;
    for (auto [e, n] : world.registry().view<NameComponent>().each()) ++before;

    SceneSerializer ser;
    REQUIRE(ser.loadAdditive(world, file, SerializeFormat::JSON));

    size_t after = 0;
    for (auto [e, n] : world.registry().view<NameComponent>().each()) ++after;

    // After additive load there must be more entities than before
    CHECK(after > before);

    // Verify the merged entity's transform
    const auto& reg = world.registry();
    Entity merged = entt::null;
    for (auto [e, n] : reg.view<NameComponent>().each())
        if (n.name == "MergedEntity") { merged = e; break; }
    REQUIRE((merged != entt::null));

    const auto* tc = reg.try_get<TransformComponent>(merged);
    REQUIRE(tc != nullptr);
    CHECK(tc->position.x == doctest::Approx(3.0f));

    he_test::removeQuiet(file);
}
