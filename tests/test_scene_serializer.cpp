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
#include <HorizonScene/EnvironmentPush.h>   // HE::makeEnvironmentSettings
#include <HorizonScene/EngineApi.h>         // HE_ENV_FIELDS_* — the Environment field list
#include <HorizonScene/Components/AnimatorStateMachineComponent.h>
#include <HorizonScene/Components/AnimatorComponent.h>
#include <HorizonScene/Components/AnimatorBlendComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/PropertyAnimatorComponent.h>
#include <HorizonScene/Components/NavMeshComponent.h>
#include <HorizonScene/Components/NavAgentComponent.h>
// The remaining component types, for the every-component round-trip below.
#include <HorizonScene/Components/Transform2DComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/Components/CharacterControllerComponent.h>
#include <HorizonScene/Components/WeatherComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/AudioSourceComponent.h>
#include <HorizonScene/Components/AudioListenerComponent.h>
#include <HorizonScene/Components/ParticleSystemComponent.h>
#include <HorizonScene/Components/LODComponent.h>
#include <HorizonScene/Components/FoliageComponent.h>
#include <HorizonScene/Components/UICanvasComponent.h>
#include <HorizonScene/Components/UIElementComponent.h>
#include <HorizonScene/Components/UITextComponent.h>
#include <HorizonScene/Components/UIImageComponent.h>
#include <HorizonScene/Components/UIButtonComponent.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

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

TEST_CASE("A legacy transition with an out-of-range op loads as a valid enumerator")
{
	// Regression: the legacy inline-component path cast `op` straight to
	// TransitionOp. A scene from a newer editor — or a hand-edit — could therefore
	// put a value with NO enumerator into the component, which
	// AnimationStateMachineSystem then switch()es on. Both readers now go through
	// HE::transitionOpFromInt (AnimatorStateMachineGraph.h), which clamps unknown
	// ops to the field's default (Greater), the same value an absent key yields.
	const fs::path file = fs::temp_directory_path() / "he_test_animsm_badop.hescene";
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

		nlohmann::json transitions = nlohmann::json::array();
		// 3 is one past Equal; 99 and -1 are the hand-edit / corruption cases.
		for (int badOp : { 3, 99, -1 })
			transitions.push_back({ { "fromState", "Idle" }, { "toState", "Walk" },
			                        { "paramName", "speed" }, { "op", badOp },
			                        { "threshold", 0.1f }, { "duration", 0.25f } });
		// One valid op alongside them, so the guard is shown to pass those through.
		transitions.push_back({ { "fromState", "Walk" }, { "toState", "Idle" },
		                        { "paramName", "speed" }, { "op", (int)HE::TransitionOp::Equal },
		                        { "threshold", 0.0f }, { "duration", 0.1f } });

		scene["entities"][0]["components"]["animstatemachine"] = {
			{ "states",      nlohmann::json::array({ { { "name", "Idle" } }, { { "name", "Walk" } } }) },
			{ "transitions", transitions },
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
		REQUIRE(lsm.legacy.transitions.size() == 4);
		for (size_t i = 0; i < 3; ++i)
		{
			// Not merely "some value" — every op must be a real enumerator, and the
			// out-of-range ones specifically fall back to the default.
			const HE::TransitionOp op = lsm.legacy.transitions[i].op;
			CHECK((op == HE::TransitionOp::Greater || op == HE::TransitionOp::Less ||
			       op == HE::TransitionOp::Equal));
			CHECK(op == HE::TransitionOp::Greater);
		}
		CHECK(lsm.legacy.transitions[3].op == HE::TransitionOp::Equal); // valid op survives
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

TEST_CASE("EnvironmentComponent round-trips on a dedicated Sky entity")
{
	const fs::path file = fs::temp_directory_path() / "he_test_env.hescene";

	HorizonWorld world;
	world.createEntity("Decoy"); // ensure the root is not the only/first entity
	const Entity sky = world.addSky(); // Sky is its own entity now, not a root component
	auto& env = world.registry().get<EnvironmentComponent>(sky);
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
	const Entity lsky = loaded.environmentEntity();
	REQUIRE((lsky != entt::null));
	CHECK(lsky != loaded.rootEntity()); // dedicated entity, not the World root
	const auto& le = loaded.registry().get<EnvironmentComponent>(lsky);
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

namespace
{
	// Every field HE::makeEnvironmentSettings maps, compared one by one. Used to assert
	// that what a saved scene pushes to the renderer is what the reloaded scene pushes:
	// a sky knob added to the component + the push but forgotten in the SceneSerializer
	// shows up here instead of as "the sky looks different after reopening the level".
	void checkSameEnvironmentSettings(const IRenderer::EnvironmentSettings& a,
	                                  const IRenderer::EnvironmentSettings& b)
	{
		CHECK(a.skyEnabled          == b.skyEnabled);
		CHECK(a.dayNightCycle       == b.dayNightCycle);
		CHECK(a.timeOfDay           == doctest::Approx(b.timeOfDay));
		CHECK(a.sunColor            == b.sunColor);
		CHECK(a.sunIntensity        == doctest::Approx(b.sunIntensity));
		CHECK(a.moonColor           == b.moonColor);
		CHECK(a.moonIntensity       == doctest::Approx(b.moonIntensity));
		CHECK(a.moonPhase           == doctest::Approx(b.moonPhase));
		CHECK(a.cloudCoverage       == doctest::Approx(b.cloudCoverage));
		CHECK(a.fogDensity          == doctest::Approx(b.fogDensity));
		CHECK(a.fogHeightFalloff    == doctest::Approx(b.fogHeightFalloff));
		CHECK(a.auroraIntensity     == doctest::Approx(b.auroraIntensity));
		CHECK(a.milkyWayIntensity   == doctest::Approx(b.milkyWayIntensity));
		CHECK(a.nebulaIntensity     == doctest::Approx(b.nebulaIntensity));
		CHECK(a.nebulaColor         == b.nebulaColor);
		CHECK(a.nebulaColor2        == b.nebulaColor2);
		CHECK(a.nebulaColor3        == b.nebulaColor3);
		CHECK(a.nebulaSeed          == doctest::Approx(b.nebulaSeed));
		CHECK(a.nebulaCoverage      == doctest::Approx(b.nebulaCoverage));
		CHECK(a.nebulaQuality       == b.nebulaQuality);
		CHECK(a.auroraColor         == b.auroraColor);
		CHECK(a.auroraColorTop      == b.auroraColorTop);
		CHECK(a.auroraHeight        == doctest::Approx(b.auroraHeight));
		CHECK(a.auroraFragmentation == doctest::Approx(b.auroraFragmentation));
		CHECK(a.windDirection       == doctest::Approx(b.windDirection));
		CHECK(a.windSpeed           == doctest::Approx(b.windSpeed));
		CHECK(a.wetness             == doctest::Approx(b.wetness));
		CHECK(a.snowAmount          == doctest::Approx(b.snowAmount));
		CHECK(a.rainAmount          == doctest::Approx(b.rainAmount));
		CHECK(a.cloudMode           == b.cloudMode);
		CHECK(a.cloudHeight         == doctest::Approx(b.cloudHeight));
		CHECK(a.cloudQuality        == b.cloudQuality);
		CHECK(a.lowResClouds        == b.lowResClouds);
		CHECK(a.cloudDensity        == doctest::Approx(b.cloudDensity));
		CHECK(a.cloudFluffiness     == doctest::Approx(b.cloudFluffiness));
		CHECK(a.cloudTint           == b.cloudTint);
		CHECK(a.contrailAmount      == doctest::Approx(b.contrailAmount));
		CHECK(a.cirrusAmount        == doctest::Approx(b.cirrusAmount));
		CHECK(a.cirrusSeed          == doctest::Approx(b.cirrusSeed));
		CHECK(a.godRays             == doctest::Approx(b.godRays));
		CHECK(a.shootingStars       == doctest::Approx(b.shootingStars));
		CHECK(a.lensFlare           == doctest::Approx(b.lensFlare));
		CHECK(a.starBrightness      == doctest::Approx(b.starBrightness));
		CHECK(a.starColor           == b.starColor);
		CHECK(a.starSize            == doctest::Approx(b.starSize));
		CHECK(a.starSizeVariation   == doctest::Approx(b.starSizeVariation));
		CHECK(a.starGlow            == doctest::Approx(b.starGlow));
		CHECK(a.starTwinkle         == doctest::Approx(b.starTwinkle));
		CHECK(a.starDensity         == doctest::Approx(b.starDensity));
	}
}

TEST_CASE("A round-tripped Sky pushes identical EnvironmentSettings to the renderer")
{
	const fs::path file = fs::temp_directory_path() / "he_test_env_push.hescene";

	HorizonWorld world;
	const Entity sky = world.addSky();
	auto& env = world.registry().get<EnvironmentComponent>(sky);
	// Non-default in every persisted field, so a dropped one changes the pushed value.
	env.dayNightCycle = true;  env.timeOfDay = 0.37f;
	env.autoAdvance = false;   env.cycleSeconds = 55.0f;      // no auto-advance: pure mapping
	env.sunColor = glm::vec3(0.11f, 0.12f, 0.13f);   env.sunIntensity  = 3.1f;
	env.moonColor = glm::vec3(0.21f, 0.22f, 0.23f);  env.moonIntensity = 0.42f;
	env.moonPhase = 0.23f;     env.moonPhaseAuto = false;     env.moonCycleDays = 12.5f;
	env.cloudCoverage = 0.71f; env.windDirection = 210.0f;    env.windSpeed = 2.2f;
	env.cloudMode = 1;         env.cloudHeight = 275.0f;      env.cloudQuality = 2;
	env.lowResClouds = true;   env.cloudDensity = 1.6f;       env.cloudFluffiness = 0.34f;
	env.cloudTint = glm::vec3(0.31f, 0.32f, 0.33f);
	env.contrailAmount = 0.18f; env.cirrusAmount = 0.29f;     env.cirrusSeed = 7.0f;
	env.godRays = 0.41f;       env.shootingStars = 0.52f;     env.lensFlare = 0.63f;
	env.fogDensity = 0.045f;   env.fogHeightFalloff = 0.22f;
	env.rainAmount = 0.64f;    env.snowAmount = 0.15f;        env.wetness = 0.72f;
	env.auroraIntensity = 0.81f; env.milkyWayIntensity = 0.44f; env.nebulaIntensity = 0.55f;
	env.nebulaColor  = glm::vec3(0.41f, 0.42f, 0.43f);
	env.nebulaColor2 = glm::vec3(0.51f, 0.52f, 0.53f);
	env.nebulaColor3 = glm::vec3(0.61f, 0.62f, 0.63f);
	env.nebulaSeed = 13.0f;    env.nebulaCoverage = 0.66f;    env.nebulaQuality = 2;
	env.auroraColor    = glm::vec3(0.71f, 0.72f, 0.73f);
	env.auroraColorTop = glm::vec3(0.81f, 0.82f, 0.83f);
	env.auroraHeight = 0.27f;  env.auroraFragmentation = 0.38f;
	env.starBrightness = 1.25f; env.starColor = glm::vec3(0.91f, 0.92f, 0.93f);
	env.starSize = 1.45f;      env.starSizeVariation = 0.26f;
	env.starGlow = 1.65f;      env.starTwinkle = 0.47f;       env.starDensity = 0.58f;

	const IRenderer::EnvironmentSettings before = HE::makeEnvironmentSettings(env, 0.0f);

	SceneSerializer ser;
	REQUIRE(ser.save(world, file, SerializeFormat::JSON));

	HorizonWorld loaded;
	REQUIRE(ser.load(loaded, file, SerializeFormat::JSON));
	const Entity lsky = loaded.environmentEntity();
	REQUIRE((lsky != entt::null));
	auto& lenv = loaded.registry().get<EnvironmentComponent>(lsky);
	const IRenderer::EnvironmentSettings after = HE::makeEnvironmentSettings(lenv, 0.0f);

	checkSameEnvironmentSettings(before, after);

	he_test::removeQuiet(file);
}

TEST_CASE("Lightning flash is deliberately runtime-only and is not persisted")
{
	const fs::path file = fs::temp_directory_path() / "he_test_env_flash.hescene";

	HorizonWorld world;
	const Entity sky = world.addSky();
	auto& env = world.registry().get<EnvironmentComponent>(sky);
	env.flash = 0.9f;   // set by the WeatherSystem during a strike, never serialized

	SceneSerializer ser;
	REQUIRE(ser.save(world, file, SerializeFormat::JSON));

	HorizonWorld loaded;
	REQUIRE(ser.load(loaded, file, SerializeFormat::JSON));
	auto& lenv = loaded.registry().get<EnvironmentComponent>(loaded.environmentEntity());
	CHECK(lenv.flash == doctest::Approx(0.0f));
	// …so a reloaded scene starts un-flashed rather than frozen mid-strike.
	CHECK(HE::makeEnvironmentSettings(lenv, 0.0f).flash == doctest::Approx(0.0f));

	he_test::removeQuiet(file);
}

namespace
{
	// Fill EVERY field in the HE_ENV_FIELDS_* lists with a distinct non-default
	// value. Driven by the same X-lists the serializer is, so a field added to the
	// component is covered here the moment it is added there — no hand-kept copy of
	// the field list to forget (which is exactly the drift this test guards).
	// `n` walks upward so no two floats share a value: a swapped pair of keys in the
	// serializer would otherwise round-trip cleanly.
	void fillEveryEnvironmentField(EnvironmentComponent& e)
	{
		int n = 0;
#define HE_TEST_ENV_FLOAT(m, Name, disp) e.m = 1.0f + 0.125f * static_cast<float>(++n);
#define HE_TEST_ENV_BOOL(m, Name, disp)  e.m = !e.m;
		// Every int knob accepts 0/1 (cloudMode has only those two), so flipping
		// inside that range gives a non-default value that is still legal.
#define HE_TEST_ENV_INT(m, Name, disp)   e.m = (e.m > 0) ? 0 : 1;
#define HE_TEST_ENV_COLOR(m, Name, disp) ++n; e.m = glm::vec3(0.01f * static_cast<float>(n), \
                                                              0.02f * static_cast<float>(n), \
                                                              0.03f * static_cast<float>(n));
		HE_ENV_FIELDS_FLOAT(HE_TEST_ENV_FLOAT)
		HE_ENV_FIELDS_BOOL (HE_TEST_ENV_BOOL)
		HE_ENV_FIELDS_INT  (HE_TEST_ENV_INT)
		HE_ENV_FIELDS_COLOR(HE_TEST_ENV_COLOR)
#undef HE_TEST_ENV_FLOAT
#undef HE_TEST_ENV_BOOL
#undef HE_TEST_ENV_INT
#undef HE_TEST_ENV_COLOR
	}

	// Compare every field EXCEPT flash (runtime-only, asserted separately above).
	void checkSameEnvironmentComponent(const EnvironmentComponent& a,
	                                   const EnvironmentComponent& b)
	{
#define HE_TEST_ENV_CMP_FLOAT(m, Name, disp) if (std::string_view(#m) != "flash") \
		CHECK(a.m == doctest::Approx(b.m));
#define HE_TEST_ENV_CMP_EQ(m, Name, disp)    CHECK(a.m == b.m);
#define HE_TEST_ENV_CMP_COLOR(m, Name, disp) CHECK(a.m.x == doctest::Approx(b.m.x)); \
		CHECK(a.m.y == doctest::Approx(b.m.y)); CHECK(a.m.z == doctest::Approx(b.m.z));
		HE_ENV_FIELDS_FLOAT(HE_TEST_ENV_CMP_FLOAT)
		HE_ENV_FIELDS_BOOL (HE_TEST_ENV_CMP_EQ)
		HE_ENV_FIELDS_INT  (HE_TEST_ENV_CMP_EQ)
		HE_ENV_FIELDS_COLOR(HE_TEST_ENV_CMP_COLOR)
#undef HE_TEST_ENV_CMP_FLOAT
#undef HE_TEST_ENV_CMP_EQ
#undef HE_TEST_ENV_CMP_COLOR
	}
}

TEST_CASE("A fully-populated Environment component round-trips field for field (JSON + binary)")
{
	HorizonWorld world;
	world.createEntity("Decoy");
	const Entity sky = world.addSky();
	auto& env = world.registry().get<EnvironmentComponent>(sky);
	fillEveryEnvironmentField(env);
	const EnvironmentComponent authored = env;

	SceneSerializer ser;

	SUBCASE("JSON")
	{
		const fs::path file = fs::temp_directory_path() / "he_test_env_full.hescene";
		REQUIRE(ser.save(world, file, SerializeFormat::JSON));

		HorizonWorld loaded;
		REQUIRE(ser.load(loaded, file, SerializeFormat::JSON));
		const Entity lsky = loaded.environmentEntity();
		REQUIRE((lsky != entt::null));
		checkSameEnvironmentComponent(loaded.registry().get<EnvironmentComponent>(lsky), authored);

		he_test::removeQuiet(file);
	}

	SUBCASE("binary / CBOR")
	{
		std::vector<uint8_t> blob;
		REQUIRE(ser.saveToMemory(world, blob));

		HorizonWorld loaded;
		REQUIRE(ser.loadFromMemory(loaded, blob));
		const Entity lsky = loaded.environmentEntity();
		REQUIRE((lsky != entt::null));
		checkSameEnvironmentComponent(loaded.registry().get<EnvironmentComponent>(lsky), authored);
	}
}

TEST_CASE("The Environment component's on-disk keys are exactly the persisted field list")
{
	// Scene files are user data: the key set is the format. Pinning it here means a
	// change to how the serializer is written (e.g. generating the two halves from
	// the HE_ENV_FIELDS_* X-lists instead of typing them twice) cannot silently add,
	// drop or rename a key on disk.
	const fs::path file = fs::temp_directory_path() / "he_test_env_keys.hescene";

	HorizonWorld world;
	const Entity sky = world.addSky();
	fillEveryEnvironmentField(world.registry().get<EnvironmentComponent>(sky));

	SceneSerializer ser;
	REQUIRE(ser.save(world, file, SerializeFormat::JSON));

	std::ifstream in(file);
	REQUIRE(in.is_open());
	nlohmann::json scene = nlohmann::json::parse(in, nullptr, false);
	REQUIRE(!scene.is_discarded());

	const nlohmann::json* envJson = nullptr;
	for (const auto& e : scene["entities"])
		if (e.contains("components") && e["components"].contains("environment"))
			envJson = &e["components"]["environment"];
	REQUIRE(envJson != nullptr);

	std::vector<std::string> expected;
#define HE_TEST_ENV_KEY(m, Name, disp) if (std::string_view(#m) != "flash") expected.push_back(#m);
	HE_ENV_FIELDS_FLOAT(HE_TEST_ENV_KEY)
	HE_ENV_FIELDS_BOOL (HE_TEST_ENV_KEY)
	HE_ENV_FIELDS_INT  (HE_TEST_ENV_KEY)
	HE_ENV_FIELDS_COLOR(HE_TEST_ENV_KEY)
#undef HE_TEST_ENV_KEY
	std::sort(expected.begin(), expected.end());

	std::vector<std::string> actual;
	for (auto it = envJson->begin(); it != envJson->end(); ++it) actual.push_back(it.key());
	std::sort(actual.begin(), actual.end());

	CHECK(actual == expected);
	CHECK(envJson->count("flash") == 0);   // runtime-only, must never reach the file

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

	// Only the root survives the clear; no authored scene entity may remain. (A
	// cleared world is bare — no Sky/Weather and hence no built-in env lights.)
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

// ── Every-component round-trip ───────────────────────────────────────────────
// WHY this exists: `environment` and `navmesh.config` generate both halves of
// their (de)serialisation from one X-macro field list, so a field cannot be added
// to the writer and forgotten in the reader. The other 28 component blocks in
// SceneSerializer.cpp still type every field out TWICE, by hand — and converting
// all 28 was judged out of proportion (see docs/rework-2026-07-deferrals.md).
// This fixture is the substitute protection: it authors a non-default value in
// every persisted field of all 28, round-trips the scene through both formats and
// compares. A field serialised but not applied (or applied but not serialised)
// turns from silent data loss on the user's next save into a red test here.
//
// NOT covered here, on purpose: `environment` and `navmesh` (own field-for-field
// test cases above), and fields the components document as runtime-only — those
// are asserted to be absent/reset by their own dedicated tests.
namespace
{
	// The authored values, kept so the verification compares against what was
	// written rather than against a second copy of the same literals (UUIDs are
	// generated per run, so they have to be carried anyway).
	struct AuthoredComponents
	{
		TransformComponent             transform;
		Transform2DComponent           transform2d;
		MeshComponent                  mesh;
		MaterialComponent              material;
		CameraComponent                camera;
		LightComponent                 light;
		RigidBodyComponent             rigidbody;
		ColliderComponent              collider;
		CharacterControllerComponent   characterController;
		ScriptComponent                script;
		AnimatorComponent              animator;
		AnimatorBlendComponent         animatorBlend;
		AnimatorStateMachineComponent  stateMachine;
		PropertyAnimatorComponent      propertyAnimator;
		SkeletalMeshComponent          skeletalMesh;
		AudioSourceComponent           audioSource;
		AudioListenerComponent         audioListener;
		ParticleSystemComponent        particleSystem;
		LODComponent                   lod;
		NavAgentComponent              navAgent;
		TerrainComponent               terrain;
		FoliageComponent               foliage;
		WeatherComponent               weather;
		UICanvasComponent              uiCanvas;
		UIElementComponent             uiElement;
		UITextComponent                uiText;
		UIImageComponent               uiImage;
		UIButtonComponent              uiButton;
	};

	void checkVec2(const glm::vec2& a, const glm::vec2& b)
	{
		CHECK(a.x == doctest::Approx(b.x));
		CHECK(a.y == doctest::Approx(b.y));
	}
	void checkVec3(const glm::vec3& a, const glm::vec3& b)
	{
		CHECK(a.x == doctest::Approx(b.x));
		CHECK(a.y == doctest::Approx(b.y));
		CHECK(a.z == doctest::Approx(b.z));
	}
	void checkVec4(const glm::vec4& a, const glm::vec4& b)
	{
		CHECK(a.x == doctest::Approx(b.x));
		CHECK(a.y == doctest::Approx(b.y));
		CHECK(a.z == doctest::Approx(b.z));
		CHECK(a.w == doctest::Approx(b.w));
	}

	Entity findEntityByName(HorizonWorld& world, std::string_view name)
	{
		for (auto [e, n] : world.registry().view<NameComponent>().each())
			if (n.name == name) return e;
		return entt::null;
	}

	AuthoredComponents populateEveryComponent(HorizonWorld& world)
	{
		AuthoredComponents a;
		auto& reg = world.registry();

		// ── "Actor": everything that can legally share one entity ────────────
		const Entity actor = world.createEntity("Actor");

		a.transform.position = { 1.5f, -2.25f, 3.75f };
		a.transform.rotation = { 11.0f, 22.0f, 33.0f };
		a.transform.scale    = { 2.0f, 3.0f, 4.0f };
		reg.emplace<TransformComponent>(actor, a.transform);

		a.transform2d.position = { 5.5f, 6.5f };
		a.transform2d.rotation = 47.0f;
		a.transform2d.scale    = { 7.5f, 8.5f };
		reg.emplace<Transform2DComponent>(actor, a.transform2d);

		a.mesh.meshAssetId    = HE::UUID::generate();
		a.mesh.lodBias        = 3;
		a.mesh.visible        = false;
		a.mesh.castsShadow    = false;
		a.mesh.receivesShadow = false;
		reg.emplace<MeshComponent>(actor, a.mesh);

		a.material.materialAssetId = HE::UUID::generate();
		{
			MaterialParamOverride ov; ov.name = "Roughness";
			ov.value[0] = 0.25f; ov.value[1] = 0.5f; ov.value[2] = 0.75f; ov.value[3] = 1.0f;
			a.material.paramOverrides = { ov };
		}
		reg.emplace<MaterialComponent>(actor, a.material);

		a.camera.fovDegrees   = 75.0f;
		a.camera.nearPlane    = 0.05f;
		a.camera.farPlane     = 2500.0f;
		a.camera.isMain       = true;
		a.camera.orthographic = true;
		reg.emplace<CameraComponent>(actor, a.camera);

		a.light.type         = LightType::Spot;
		a.light.color        = { 0.15f, 0.25f, 0.35f };
		a.light.intensity    = 7.5f;
		a.light.range        = 25.0f;
		a.light.spotAngle    = 33.0f;
		a.light.cullDistance = 90.0f;
		a.light.castsShadow  = true;
		a.light.visible      = false;
		reg.emplace<LightComponent>(actor, a.light);

		a.rigidbody.type        = RigidBodyType::Kinematic;
		a.rigidbody.mass        = 12.5f;
		a.rigidbody.friction    = 0.9f;
		a.rigidbody.restitution = 0.15f;
		a.rigidbody.is2D        = true;
		reg.emplace<RigidBodyComponent>(actor, a.rigidbody);

		a.collider.shape       = ColliderShape::Capsule;
		a.collider.halfExtents = { 1.5f, 2.5f, 3.5f };
		a.collider.radius      = 0.75f;
		a.collider.height      = 3.25f;
		a.collider.isTrigger   = true;
		reg.emplace<ColliderComponent>(actor, a.collider);

		a.characterController.slopeLimit = 55.0f;
		a.characterController.stepHeight = 0.6f;
		a.characterController.skinWidth  = 0.05f;
		a.characterController.mass       = 82.0f;
		a.characterController.gravity    = 12.5f;
		reg.emplace<CharacterControllerComponent>(actor, a.characterController);

		a.script.scriptAssetId = HE::UUID::generate();
		a.script.moduleName    = "combat";
		a.script.enabled       = false;
		{
			ScriptPropValue f; f.type = ScriptPropType::Float;  f.f = 2.75f;
			ScriptPropValue i; i.type = ScriptPropType::Int;    i.i = 9;
			ScriptPropValue b; b.type = ScriptPropType::Bool;   b.b = true;
			ScriptPropValue s; s.type = ScriptPropType::String; s.s = "boss";
			a.script.properties["damage"]  = f;
			a.script.properties["ammo"]    = i;
			a.script.properties["hostile"] = b;
			a.script.properties["archetype"] = s;
		}
		reg.emplace<ScriptComponent>(actor, a.script);

		a.animator.clipAssetId   = HE::UUID::generate();
		a.animator.playbackTime  = 1.25f;
		a.animator.playbackSpeed = 2.5f;
		a.animator.looping       = false;
		a.animator.playing       = false;
		reg.emplace<AnimatorComponent>(actor, a.animator);

		a.animatorBlend.clipAId        = HE::UUID::generate();
		a.animatorBlend.clipBId        = HE::UUID::generate();
		a.animatorBlend.blendAlpha     = 0.35f;
		a.animatorBlend.playbackTime   = 0.75f;
		a.animatorBlend.playbackSpeed  = 1.75f;
		a.animatorBlend.looping        = false;
		a.animatorBlend.playing        = false;
		reg.emplace<AnimatorBlendComponent>(actor, a.animatorBlend);

		a.stateMachine.stateMachineAssetId = HE::UUID::generate();
		a.stateMachine.params["speed"]  = 2.25f;
		a.stateMachine.params["health"] = 0.5f;
		a.stateMachine.currentStateName = "Run";
		reg.emplace<AnimatorStateMachineComponent>(actor, a.stateMachine);

		a.propertyAnimator.clipId        = HE::UUID::generate();
		a.propertyAnimator.playbackTime  = 3.5f;
		a.propertyAnimator.playbackSpeed = 0.25f;
		a.propertyAnimator.looping       = false;
		a.propertyAnimator.playing       = false;
		reg.emplace<PropertyAnimatorComponent>(actor, a.propertyAnimator);

		a.skeletalMesh.meshAssetId    = HE::UUID::generate();
		a.skeletalMesh.visible        = false;
		a.skeletalMesh.castsShadow    = false;
		a.skeletalMesh.receivesShadow = false;
		reg.emplace<SkeletalMeshComponent>(actor, a.skeletalMesh);

		a.audioSource.assetId       = HE::UUID::generate();
		a.audioSource.busName       = "sfx";
		a.audioSource.volume        = 0.65f;
		a.audioSource.pitch         = 1.4f;
		a.audioSource.range         = 45.0f;
		a.audioSource.innerRange    = 2.5f;
		a.audioSource.rolloffFactor = 0.35f;
		a.audioSource.loop          = true;
		a.audioSource.playOnStart   = true;
		a.audioSource.spatial       = true;
		reg.emplace<AudioSourceComponent>(actor, a.audioSource);

		a.audioListener.masterVolume = 0.42f;
		reg.emplace<AudioListenerComponent>(actor, a.audioListener);

		a.particleSystem.particleAssetId = HE::UUID::generate();
		a.particleSystem.visible = false;
		a.particleSystem.playing = false;
		reg.emplace<ParticleSystemComponent>(actor, a.particleSystem);

		{
			// Not named near/far: those are macros in the Windows headers this
			// test also builds under.
			LODLevel lodNear; lodNear.meshId = HE::UUID::generate(); lodNear.maxDistance = 15.0f;
			LODLevel lodFar;  lodFar.meshId  = HE::UUID::generate(); lodFar.maxDistance  = 120.0f;
			a.lod.levels = { lodNear, lodFar };
		}
		reg.emplace<LODComponent>(actor, a.lod);

		a.navAgent.targetPos    = { 4.0f, 1.0f, -2.0f };
		a.navAgent.speed        = 6.0f;
		a.navAgent.stoppingDist = 0.5f;
		reg.emplace<NavAgentComponent>(actor, a.navAgent);

		// ── "Land": terrain + foliage. MeshComponent is deliberately NOT here —
		// the serializer skips it on terrain entities (regenerated on load).
		const Entity land = world.createEntity("Land");

		a.terrain.sizeX            = 250.0f;
		a.terrain.sizeZ            = 175.0f;
		a.terrain.resolution       = 4;
		a.terrain.heightScale      = 35.0f;
		a.terrain.seed             = 4711;
		a.terrain.lodDistanceScale = 2.5f;
		a.terrain.octaves          = 6;
		a.terrain.frequency        = 1.75f;
		a.terrain.lacunarity       = 2.25f;
		a.terrain.gain             = 0.65f;
		a.terrain.uvTiling         = 12.0f;
		a.terrain.sculptHeights.resize(a.terrain.resolution * a.terrain.resolution);
		for (size_t i = 0; i < a.terrain.sculptHeights.size(); ++i)
			a.terrain.sculptHeights[i] = 0.5f * static_cast<float>(i);
		// weightRes² × 4 bytes, or the loader drops the blob as truncated.
		a.terrain.weightRes = 2;
		a.terrain.layerWeights.resize(a.terrain.weightRes * a.terrain.weightRes * 4);
		for (size_t i = 0; i < a.terrain.layerWeights.size(); ++i)
			a.terrain.layerWeights[i] = static_cast<uint8_t>(i * 7 + 1);
		reg.emplace<TerrainComponent>(land, a.terrain);

		a.foliage.visible         = false;
		a.foliage.meshAssetId     = HE::UUID::generate();
		a.foliage.materialAssetId = HE::UUID::generate();
		a.foliage.density         = 0.45f;
		a.foliage.seed            = 1234;
		a.foliage.minScale        = 0.55f;
		a.foliage.maxScale        = 1.85f;
		a.foliage.drawDistance    = 140.0f;
		reg.emplace<FoliageComponent>(land, a.foliage);

		// ── "Weather": its own entity since Forts. (addWeather), like Sky ─────
		const Entity weather = world.addWeather();
		a.weather.currentKind        = WeatherKind::Rain;
		a.weather.targetKind         = WeatherKind::Storm;
		a.weather.intensity          = 0.85f;
		a.weather.transitionDuration = 14.0f;
		a.weather.autoCycle          = true;
		a.weather.cycleSeconds       = 95.0f;
		a.weather.thunderSound       = HE::UUID::generate();
		a.weather.maxRainParticles   = 1500;
		a.weather.maxSnowParticles   = 900;
		a.weather.groundLevel        = -3.5f;
		reg.get<WeatherComponent>(weather) = a.weather;

		// ── "Panel": the UI component family ─────────────────────────────────
		const Entity panel = world.createEntity("Panel");

		a.uiCanvas.width      = 1280.0f;
		a.uiCanvas.height     = 720.0f;
		a.uiCanvas.renderMode = UIRenderMode::WorldSpace;
		a.uiCanvas.active     = false;
		reg.emplace<UICanvasComponent>(panel, a.uiCanvas);

		a.uiElement.position = { 12.0f, 34.0f };
		a.uiElement.size     = { 240.0f, 60.0f };
		a.uiElement.pivot    = { 0.25f, 0.75f };
		a.uiElement.rotation = 15.0f;
		a.uiElement.anchor   = UIAnchor::BottomRight;
		a.uiElement.layer    = 7;
		a.uiElement.active   = false;
		reg.emplace<UIElementComponent>(panel, a.uiElement);

		a.uiText.text     = "Press Start";
		a.uiText.fontSize = 28.0f;
		a.uiText.color    = { 0.1f, 0.2f, 0.3f, 0.4f };
		reg.emplace<UITextComponent>(panel, a.uiText);

		a.uiImage.materialAssetId = HE::UUID::generate();
		a.uiImage.tint            = { 0.5f, 0.6f, 0.7f, 0.8f };
		reg.emplace<UIImageComponent>(panel, a.uiImage);

		a.uiButton.normalColor     = { 0.11f, 0.12f, 0.13f, 0.14f };
		a.uiButton.hoveredColor    = { 0.21f, 0.22f, 0.23f, 0.24f };
		a.uiButton.pressedColor    = { 0.31f, 0.32f, 0.33f, 0.34f };
		a.uiButton.onClickFunction = "OnStartClicked";
		reg.emplace<UIButtonComponent>(panel, a.uiButton);

		return a;
	}

	void verifyEveryComponent(HorizonWorld& world, const AuthoredComponents& a)
	{
		auto& reg = world.registry();

		const Entity actor = findEntityByName(world, "Actor");
		REQUIRE((actor != entt::null));

		{
			const auto* t = reg.try_get<TransformComponent>(actor);
			REQUIRE(t != nullptr);
			checkVec3(t->position, a.transform.position);
			checkVec3(t->rotation, a.transform.rotation);
			checkVec3(t->scale,    a.transform.scale);
		}
		{
			const auto* t = reg.try_get<Transform2DComponent>(actor);
			REQUIRE(t != nullptr);
			checkVec2(t->position, a.transform2d.position);
			CHECK(t->rotation == doctest::Approx(a.transform2d.rotation));
			checkVec2(t->scale, a.transform2d.scale);
		}
		{
			const auto* m = reg.try_get<MeshComponent>(actor);
			REQUIRE(m != nullptr);
			CHECK(m->meshAssetId    == a.mesh.meshAssetId);
			CHECK(m->lodBias        == a.mesh.lodBias);
			CHECK(m->visible        == a.mesh.visible);
			CHECK(m->castsShadow    == a.mesh.castsShadow);
			CHECK(m->receivesShadow == a.mesh.receivesShadow);
		}
		{
			const auto* m = reg.try_get<MaterialComponent>(actor);
			REQUIRE(m != nullptr);
			CHECK(m->materialAssetId == a.material.materialAssetId);
			REQUIRE(m->paramOverrides.size() == a.material.paramOverrides.size());
			CHECK(m->paramOverrides[0].name == a.material.paramOverrides[0].name);
			for (int k = 0; k < 4; ++k)
				CHECK(m->paramOverrides[0].value[k] ==
				      doctest::Approx(a.material.paramOverrides[0].value[k]));
		}
		{
			const auto* c = reg.try_get<CameraComponent>(actor);
			REQUIRE(c != nullptr);
			CHECK(c->fovDegrees   == doctest::Approx(a.camera.fovDegrees));
			CHECK(c->nearPlane    == doctest::Approx(a.camera.nearPlane));
			CHECK(c->farPlane     == doctest::Approx(a.camera.farPlane));
			CHECK(c->isMain       == a.camera.isMain);
			CHECK(c->orthographic == a.camera.orthographic);
		}
		{
			const auto* l = reg.try_get<LightComponent>(actor);
			REQUIRE(l != nullptr);
			CHECK(l->type == a.light.type);
			checkVec3(l->color, a.light.color);
			CHECK(l->intensity    == doctest::Approx(a.light.intensity));
			CHECK(l->range        == doctest::Approx(a.light.range));
			CHECK(l->spotAngle    == doctest::Approx(a.light.spotAngle));
			CHECK(l->cullDistance == doctest::Approx(a.light.cullDistance));
			CHECK(l->castsShadow  == a.light.castsShadow);
			CHECK(l->visible      == a.light.visible);
		}
		{
			const auto* r = reg.try_get<RigidBodyComponent>(actor);
			REQUIRE(r != nullptr);
			CHECK(r->type        == a.rigidbody.type);
			CHECK(r->mass        == doctest::Approx(a.rigidbody.mass));
			CHECK(r->friction    == doctest::Approx(a.rigidbody.friction));
			CHECK(r->restitution == doctest::Approx(a.rigidbody.restitution));
			CHECK(r->is2D        == a.rigidbody.is2D);
		}
		{
			const auto* col = reg.try_get<ColliderComponent>(actor);
			REQUIRE(col != nullptr);
			CHECK(col->shape == a.collider.shape);
			checkVec3(col->halfExtents, a.collider.halfExtents);
			CHECK(col->radius    == doctest::Approx(a.collider.radius));
			CHECK(col->height    == doctest::Approx(a.collider.height));
			CHECK(col->isTrigger == a.collider.isTrigger);
		}
		{
			const auto* cc = reg.try_get<CharacterControllerComponent>(actor);
			REQUIRE(cc != nullptr);
			CHECK(cc->slopeLimit == doctest::Approx(a.characterController.slopeLimit));
			CHECK(cc->stepHeight == doctest::Approx(a.characterController.stepHeight));
			CHECK(cc->skinWidth  == doctest::Approx(a.characterController.skinWidth));
			CHECK(cc->mass       == doctest::Approx(a.characterController.mass));
			CHECK(cc->gravity    == doctest::Approx(a.characterController.gravity));
		}
		{
			const auto* s = reg.try_get<ScriptComponent>(actor);
			REQUIRE(s != nullptr);
			CHECK(s->scriptAssetId == a.script.scriptAssetId);
			CHECK(s->moduleName    == a.script.moduleName);
			CHECK(s->enabled       == a.script.enabled);
			REQUIRE(s->properties.size() == a.script.properties.size());
			for (const auto& [key, want] : a.script.properties)
			{
				REQUIRE(s->properties.count(key));
				const ScriptPropValue& got = s->properties.at(key);
				CHECK(got.type == want.type);
				switch (want.type)
				{
				case ScriptPropType::Float:  CHECK(got.f == doctest::Approx(want.f)); break;
				case ScriptPropType::Int:    CHECK(got.i == want.i);                  break;
				case ScriptPropType::Bool:   CHECK(got.b == want.b);                  break;
				case ScriptPropType::String: CHECK(got.s == want.s);                  break;
				}
			}
		}
		{
			const auto* an = reg.try_get<AnimatorComponent>(actor);
			REQUIRE(an != nullptr);
			CHECK(an->clipAssetId   == a.animator.clipAssetId);
			CHECK(an->playbackTime  == doctest::Approx(a.animator.playbackTime));
			CHECK(an->playbackSpeed == doctest::Approx(a.animator.playbackSpeed));
			CHECK(an->looping       == a.animator.looping);
			CHECK(an->playing       == a.animator.playing);
		}
		{
			const auto* ab = reg.try_get<AnimatorBlendComponent>(actor);
			REQUIRE(ab != nullptr);
			CHECK(ab->clipAId       == a.animatorBlend.clipAId);
			CHECK(ab->clipBId       == a.animatorBlend.clipBId);
			CHECK(ab->blendAlpha    == doctest::Approx(a.animatorBlend.blendAlpha));
			CHECK(ab->playbackTime  == doctest::Approx(a.animatorBlend.playbackTime));
			CHECK(ab->playbackSpeed == doctest::Approx(a.animatorBlend.playbackSpeed));
			CHECK(ab->looping       == a.animatorBlend.looping);
			CHECK(ab->playing       == a.animatorBlend.playing);
		}
		{
			const auto* sm = reg.try_get<AnimatorStateMachineComponent>(actor);
			REQUIRE(sm != nullptr);
			CHECK(sm->stateMachineAssetId == a.stateMachine.stateMachineAssetId);
			CHECK(sm->currentStateName    == a.stateMachine.currentStateName);
			REQUIRE(sm->params.size() == a.stateMachine.params.size());
			for (const auto& [key, want] : a.stateMachine.params)
			{
				REQUIRE(sm->params.count(key));
				CHECK(sm->params.at(key) == doctest::Approx(want));
			}
			CHECK_FALSE(sm->legacy.hasData); // current format, no migration staging
		}
		{
			const auto* pa = reg.try_get<PropertyAnimatorComponent>(actor);
			REQUIRE(pa != nullptr);
			CHECK(pa->clipId        == a.propertyAnimator.clipId);
			CHECK(pa->playbackTime  == doctest::Approx(a.propertyAnimator.playbackTime));
			CHECK(pa->playbackSpeed == doctest::Approx(a.propertyAnimator.playbackSpeed));
			CHECK(pa->looping       == a.propertyAnimator.looping);
			CHECK(pa->playing       == a.propertyAnimator.playing);
		}
		{
			const auto* sk = reg.try_get<SkeletalMeshComponent>(actor);
			REQUIRE(sk != nullptr);
			CHECK(sk->meshAssetId    == a.skeletalMesh.meshAssetId);
			CHECK(sk->visible        == a.skeletalMesh.visible);
			CHECK(sk->castsShadow    == a.skeletalMesh.castsShadow);
			CHECK(sk->receivesShadow == a.skeletalMesh.receivesShadow);
		}
		{
			const auto* as = reg.try_get<AudioSourceComponent>(actor);
			REQUIRE(as != nullptr);
			CHECK(as->assetId       == a.audioSource.assetId);
			CHECK(as->busName       == a.audioSource.busName);
			CHECK(as->volume        == doctest::Approx(a.audioSource.volume));
			CHECK(as->pitch         == doctest::Approx(a.audioSource.pitch));
			CHECK(as->range         == doctest::Approx(a.audioSource.range));
			CHECK(as->innerRange    == doctest::Approx(a.audioSource.innerRange));
			CHECK(as->rolloffFactor == doctest::Approx(a.audioSource.rolloffFactor));
			CHECK(as->loop          == a.audioSource.loop);
			CHECK(as->playOnStart   == a.audioSource.playOnStart);
			CHECK(as->spatial       == a.audioSource.spatial);
		}
		{
			const auto* al = reg.try_get<AudioListenerComponent>(actor);
			REQUIRE(al != nullptr);
			CHECK(al->masterVolume == doctest::Approx(a.audioListener.masterVolume));
		}
		{
			const auto* ps = reg.try_get<ParticleSystemComponent>(actor);
			REQUIRE(ps != nullptr);
			CHECK(ps->particleAssetId == a.particleSystem.particleAssetId);
			CHECK(ps->visible         == a.particleSystem.visible);
			CHECK(ps->playing         == a.particleSystem.playing);
			CHECK_FALSE(ps->legacy.hasData); // current format, no migration staging
		}
		{
			const auto* lod = reg.try_get<LODComponent>(actor);
			REQUIRE(lod != nullptr);
			REQUIRE(lod->levels.size() == a.lod.levels.size());
			for (size_t i = 0; i < a.lod.levels.size(); ++i)
			{
				CHECK(lod->levels[i].meshId == a.lod.levels[i].meshId);
				CHECK(lod->levels[i].maxDistance ==
				      doctest::Approx(a.lod.levels[i].maxDistance));
			}
		}
		{
			const auto* na = reg.try_get<NavAgentComponent>(actor);
			REQUIRE(na != nullptr);
			checkVec3(na->targetPos, a.navAgent.targetPos);
			CHECK(na->speed        == doctest::Approx(a.navAgent.speed));
			CHECK(na->stoppingDist == doctest::Approx(a.navAgent.stoppingDist));
		}

		const Entity land = findEntityByName(world, "Land");
		REQUIRE((land != entt::null));
		{
			const auto* t = reg.try_get<TerrainComponent>(land);
			REQUIRE(t != nullptr);
			CHECK(t->sizeX            == doctest::Approx(a.terrain.sizeX));
			CHECK(t->sizeZ            == doctest::Approx(a.terrain.sizeZ));
			CHECK(t->resolution       == a.terrain.resolution);
			CHECK(t->heightScale      == doctest::Approx(a.terrain.heightScale));
			CHECK(t->seed             == a.terrain.seed);
			CHECK(t->lodDistanceScale == doctest::Approx(a.terrain.lodDistanceScale));
			CHECK(t->octaves          == a.terrain.octaves);
			CHECK(t->frequency        == doctest::Approx(a.terrain.frequency));
			CHECK(t->lacunarity       == doctest::Approx(a.terrain.lacunarity));
			CHECK(t->gain             == doctest::Approx(a.terrain.gain));
			CHECK(t->uvTiling         == doctest::Approx(a.terrain.uvTiling));
			CHECK(t->weightRes        == a.terrain.weightRes);
			// The blobs matter most: a wrong size makes the loader drop them silently.
			REQUIRE(t->layerWeights.size() == a.terrain.layerWeights.size());
			CHECK(t->layerWeights == a.terrain.layerWeights);
			REQUIRE(t->sculptHeights.size() == a.terrain.sculptHeights.size());
			for (size_t i = 0; i < a.terrain.sculptHeights.size(); ++i)
				CHECK(t->sculptHeights[i] == doctest::Approx(a.terrain.sculptHeights[i]));
		}
		{
			const auto* f = reg.try_get<FoliageComponent>(land);
			REQUIRE(f != nullptr);
			CHECK(f->visible         == a.foliage.visible);
			CHECK(f->meshAssetId     == a.foliage.meshAssetId);
			CHECK(f->materialAssetId == a.foliage.materialAssetId);
			CHECK(f->density         == doctest::Approx(a.foliage.density));
			CHECK(f->seed            == a.foliage.seed);
			CHECK(f->minScale        == doctest::Approx(a.foliage.minScale));
			CHECK(f->maxScale        == doctest::Approx(a.foliage.maxScale));
			CHECK(f->drawDistance    == doctest::Approx(a.foliage.drawDistance));
		}

		const Entity weather = world.weatherEntity();
		REQUIRE((weather != entt::null));
		{
			const auto* w = reg.try_get<WeatherComponent>(weather);
			REQUIRE(w != nullptr);
			CHECK(w->currentKind        == a.weather.currentKind);
			CHECK(w->targetKind         == a.weather.targetKind);
			CHECK(w->intensity          == doctest::Approx(a.weather.intensity));
			CHECK(w->transitionDuration == doctest::Approx(a.weather.transitionDuration));
			CHECK(w->autoCycle          == a.weather.autoCycle);
			CHECK(w->cycleSeconds       == doctest::Approx(a.weather.cycleSeconds));
			CHECK(w->thunderSound       == a.weather.thunderSound);
			CHECK(w->maxRainParticles   == a.weather.maxRainParticles);
			CHECK(w->maxSnowParticles   == a.weather.maxSnowParticles);
			CHECK(w->groundLevel        == doctest::Approx(a.weather.groundLevel));
			// Deliberately reset on load so a reloaded scene doesn't reclaim the
			// authored env values (see the `prevTarget` line in SceneSerializer).
			CHECK(w->prevTarget == a.weather.targetKind);
		}

		const Entity panel = findEntityByName(world, "Panel");
		REQUIRE((panel != entt::null));
		{
			const auto* c = reg.try_get<UICanvasComponent>(panel);
			REQUIRE(c != nullptr);
			CHECK(c->width      == doctest::Approx(a.uiCanvas.width));
			CHECK(c->height     == doctest::Approx(a.uiCanvas.height));
			CHECK(c->renderMode == a.uiCanvas.renderMode);
			CHECK(c->active     == a.uiCanvas.active);
		}
		{
			const auto* e = reg.try_get<UIElementComponent>(panel);
			REQUIRE(e != nullptr);
			checkVec2(e->position, a.uiElement.position);
			checkVec2(e->size,     a.uiElement.size);
			checkVec2(e->pivot,    a.uiElement.pivot);
			CHECK(e->rotation == doctest::Approx(a.uiElement.rotation));
			CHECK(e->anchor   == a.uiElement.anchor);
			CHECK(e->layer    == a.uiElement.layer);
			CHECK(e->active   == a.uiElement.active);
		}
		{
			const auto* t = reg.try_get<UITextComponent>(panel);
			REQUIRE(t != nullptr);
			CHECK(t->text     == a.uiText.text);
			CHECK(t->fontSize == doctest::Approx(a.uiText.fontSize));
			checkVec4(t->color, a.uiText.color);
		}
		{
			const auto* img = reg.try_get<UIImageComponent>(panel);
			REQUIRE(img != nullptr);
			CHECK(img->materialAssetId == a.uiImage.materialAssetId);
			checkVec4(img->tint, a.uiImage.tint);
		}
		{
			const auto* b = reg.try_get<UIButtonComponent>(panel);
			REQUIRE(b != nullptr);
			checkVec4(b->normalColor,  a.uiButton.normalColor);
			checkVec4(b->hoveredColor, a.uiButton.hoveredColor);
			checkVec4(b->pressedColor, a.uiButton.pressedColor);
			CHECK(b->onClickFunction == a.uiButton.onClickFunction);
		}
	}
}

TEST_CASE("Every component survives a round-trip with non-default values in every persisted field")
{
	SUBCASE("JSON")
	{
		const fs::path file = fs::temp_directory_path() / "he_test_all_components.hescene";
		HorizonWorld world;
		const AuthoredComponents authored = populateEveryComponent(world);

		SceneSerializer ser;
		REQUIRE(ser.save(world, file, SerializeFormat::JSON));
		HorizonWorld loaded;
		REQUIRE(ser.load(loaded, file, SerializeFormat::JSON));
		verifyEveryComponent(loaded, authored);

		he_test::removeQuiet(file);
	}

	SUBCASE("binary / CBOR")
	{
		// The undo snapshot path (saveToMemory/loadFromMemory) shares buildSceneJson
		// with the file path, so this mainly guards the CBOR encoding of the base64
		// blobs and the numeric types.
		HorizonWorld world;
		const AuthoredComponents authored = populateEveryComponent(world);

		SceneSerializer ser;
		std::vector<uint8_t> blob;
		REQUIRE(ser.saveToMemory(world, blob));
		HorizonWorld loaded;
		REQUIRE(ser.loadFromMemory(loaded, blob));
		verifyEveryComponent(loaded, authored);
	}
}
