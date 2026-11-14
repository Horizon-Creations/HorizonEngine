#include "doctest.h"
#include "TestFsUtil.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/CookAnalysis.h>
#include <HorizonScene/Components/AnimationLayerComponent.h>
#include <HorizonScene/Components/AnimatorBlendComponent.h>
#include <HorizonScene/Components/AnimatorComponent.h>
#include <HorizonScene/Components/AnimatorStateMachineComponent.h>
#include <HorizonScene/Components/CharacterControllerComponent.h>
#include <HorizonScene/Components/InactiveComponent.h>
#include <HorizonScene/Components/IkComponent.h>
#include <HorizonScene/Components/LODComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/MobilityComponent.h>
#include <HorizonScene/Components/MovementComponent.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/NavAgentComponent.h>
#include <HorizonScene/Components/NetworkComponent.h>
#include <HorizonScene/Components/ParticleSystemComponent.h>
#include <HorizonScene/Components/PropertyAnimatorComponent.h>
#include <HorizonScene/Components/ReplicatedVarsComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/RootMotionComponent.h>
#include <HorizonScene/Components/RopeComponent.h>
#include <HorizonScene/Components/SaveStateComponent.h>
#include <HorizonScene/Components/ScriptComponent.h>
#include <HorizonScene/Components/SequencePlayerComponent.h>
#include <HorizonScene/Components/StreamingComponent.h>
#include <HorizonScene/Components/TrailComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using HE::CookBlocker;

namespace
{
	// A plain placed mesh, the thing cooking is for.
	Entity makeMesh(HorizonWorld& world, const char* name)
	{
		const Entity e = world.createEntity(name);
		auto& reg = world.registry();
		reg.emplace_or_replace<TransformComponent>(e, TransformComponent{});
		reg.emplace_or_replace<MeshComponent>(e, MeshComponent{});
		return e;
	}

	// A plain mesh plus one component; the verdict says which rule fired.
	template<typename T>
	HE::CookVerdict withComponent(T comp = T{})
	{
		HorizonWorld world;
		const Entity e = makeMesh(world, "E");
		world.registry().emplace<T>(e, std::move(comp));
		return HE::analyzeCookability(world.registry(), e);
	}

	template<typename T>
	void blockedBy(CookBlocker b, T comp = T{})
	{
		const HE::CookVerdict v = withComponent<T>(std::move(comp));
		CHECK_FALSE(v.cookable());
		CHECK(v.has(b));
	}

	Entity findByName(HorizonWorld& world, const char* name)
	{
		for (const Entity e : world.registry().view<NameComponent>())
			if (world.registry().get<NameComponent>(e).name == name) return e;
		return entt::null;
	}

	MobilityComponent mobility(MobilityComponent::Mode m) { MobilityComponent c; c.mode = m; return c; }
}

TEST_CASE("cook analysis: a plain Transform + Mesh entity is cookable")
{
	HorizonWorld world;
	const Entity e = makeMesh(world, "Rock");
	CHECK(HE::analyzeCookability(world.registry(), e).cookable());
}

TEST_CASE("cook analysis: every survey rule makes an entity uncookable")
{
	SUBCASE("1 script") { blockedBy<ScriptComponent>(CookBlocker::Script); }
	SUBCASE("2 animation")
	{
		blockedBy<AnimatorComponent>(CookBlocker::Animation);
		blockedBy<AnimationLayerComponent>(CookBlocker::Animation);
		blockedBy<AnimatorBlendComponent>(CookBlocker::Animation);
		blockedBy<AnimatorStateMachineComponent>(CookBlocker::Animation);
		blockedBy<RootMotionComponent>(CookBlocker::Animation);
		blockedBy<IkComponent>(CookBlocker::Animation);
	}
	SUBCASE("3 property playback")
	{
		blockedBy<PropertyAnimatorComponent>(CookBlocker::PropertyPlayback);
		blockedBy<SequencePlayerComponent>(CookBlocker::PropertyPlayback);
	}
	SUBCASE("4 rigid body that is not Static")
	{
		RigidBodyComponent rb;
		rb.type = RigidBodyType::Dynamic;
		blockedBy<RigidBodyComponent>(CookBlocker::Physics, rb);
		rb.type = RigidBodyType::Kinematic;
		blockedBy<RigidBodyComponent>(CookBlocker::Physics, rb);
		// A static body is exactly what a cooked entity may keep.
		rb.type = RigidBodyType::Static;
		CHECK(withComponent<RigidBodyComponent>(rb).cookable());
	}
	SUBCASE("5 locomotion and particles")
	{
		blockedBy<CharacterControllerComponent>(CookBlocker::Locomotion);
		blockedBy<NavAgentComponent>(CookBlocker::Locomotion);
		blockedBy<MovementComponent>(CookBlocker::Locomotion);
		blockedBy<ParticleSystemComponent>(CookBlocker::Locomotion);
	}
	SUBCASE("6 replication")
	{
		NetworkComponent n;
		n.replicates = true;
		blockedBy<NetworkComponent>(CookBlocker::Replication, n);
		// The component alone is not the switch.
		n.replicates = false;
		CHECK(withComponent<NetworkComponent>(n).cookable());

		ReplicatedVarsComponent vars;
		CHECK(withComponent<ReplicatedVarsComponent>(vars).cookable());
		vars.entries.push_back({});
		blockedBy<ReplicatedVarsComponent>(CookBlocker::Replication, vars);
	}
	SUBCASE("7 save state") { blockedBy<SaveStateComponent>(CookBlocker::SaveState); }
	SUBCASE("8 LOD with several levels or a refined mesh")
	{
		LODComponent lod;
		lod.levels.resize(1);
		CHECK(withComponent<LODComponent>(lod).cookable());
		lod.levels.resize(2);
		blockedBy<LODComponent>(CookBlocker::Lod, lod);

		LODComponent refined;
		refined.levels.resize(1);
		refined.refinedMeshId = HE::UUID::generate();
		blockedBy<LODComponent>(CookBlocker::Lod, refined);
	}
	SUBCASE("9 trail and rope")
	{
		blockedBy<TrailComponent>(CookBlocker::Geometry);
		blockedBy<RopeComponent>(CookBlocker::Geometry);
	}
	SUBCASE("11 streaming Auto")
	{
		StreamingComponent s;
		s.mode = StreamingComponent::Mode::Auto;
		blockedBy<StreamingComponent>(CookBlocker::Streaming, s);
		// Resident stays in the base for good.
		s.mode = StreamingComponent::Mode::Resident;
		CHECK(withComponent<StreamingComponent>(s).cookable());
	}
	SUBCASE("12 switched off or hidden")
	{
		blockedBy<InactiveComponent>(CookBlocker::Switchable);

		HorizonWorld world;
		const Entity e = makeMesh(world, "Hidden");
		world.registry().get<MeshComponent>(e).visible = false;
		const HE::CookVerdict v = HE::analyzeCookability(world.registry(), e);
		CHECK(v.has(CookBlocker::Switchable));
	}
}

TEST_CASE("cook analysis: rule 10, a blocked ancestor blocks the whole subtree")
{
	HorizonWorld world;
	auto& reg = world.registry();
	const Entity room  = makeMesh(world, "Room");
	const Entity chair = makeMesh(world, "Chair");
	const Entity leg   = makeMesh(world, "Leg");
	const Entity other = makeMesh(world, "Other");
	world.reparentEntity(chair, room);
	world.reparentEntity(leg, chair);

	CHECK(HE::analyzeCookability(reg, leg).cookable());

	reg.emplace<ScriptComponent>(room, ScriptComponent{});
	// The grandchild is blocked by the room alone, with no blocker of its own.
	const HE::CookVerdict legV = HE::analyzeCookability(reg, leg);
	CHECK(legV.blockers == static_cast<uint16_t>(CookBlocker::Ancestor));
	CHECK(HE::analyzeCookability(reg, chair).has(CookBlocker::Ancestor));
	CHECK(HE::analyzeCookability(reg, other).cookable());

	// An explicit Movable parent moves its children too.
	reg.remove<ScriptComponent>(room);
	CHECK(HE::analyzeCookability(reg, leg).cookable());
	reg.emplace<MobilityComponent>(room, mobility(MobilityComponent::Mode::Movable));
	CHECK(HE::analyzeCookability(reg, leg).has(CookBlocker::Ancestor));

	// The whole-world pass agrees with the single query for every entity.
	for (const HE::CookEntry& entry : HE::analyzeCookability(reg))
		CHECK(entry.verdict.blockers == HE::analyzeCookability(reg, entry.entity).blockers);
}

TEST_CASE("cook analysis: Mobility is the author's say, and the rules stay the veto")
{
	HorizonWorld world;
	auto& reg = world.registry();
	const Entity undecided = makeMesh(world, "Undecided");
	const Entity movable   = makeMesh(world, "Movable");
	const Entity stat      = makeMesh(world, "Static");
	const Entity scripted  = makeMesh(world, "StaticButScripted");
	reg.emplace<MobilityComponent>(movable, mobility(MobilityComponent::Mode::Movable));
	reg.emplace<MobilityComponent>(stat, mobility(MobilityComponent::Mode::Static));
	reg.emplace<MobilityComponent>(scripted, mobility(MobilityComponent::Mode::Static));
	reg.emplace<ScriptComponent>(scripted, ScriptComponent{});

	CHECK(HE::analyzeCookability(reg, undecided).cookable());
	// Movable keeps an otherwise cookable entity out.
	const HE::CookVerdict m = HE::analyzeCookability(reg, movable);
	CHECK_FALSE(m.cookable());
	CHECK(m.has(CookBlocker::MarkedMovable));
	CHECK(HE::analyzeCookability(reg, stat).cookable());
	// Static does not lift a rule: the entity is not cooked (and the whole-world
	// pass logs the contradiction).
	const HE::CookVerdict s = HE::analyzeCookability(reg, scripted);
	CHECK_FALSE(s.cookable());
	CHECK(s.has(CookBlocker::Script));
	CHECK_FALSE(s.has(CookBlocker::MarkedMovable));

	size_t cookable = 0;
	for (const HE::CookEntry& entry : HE::analyzeCookability(reg))
		if (entry.verdict.cookable()) ++cookable;
	// Undecided and Static (the world's root has no mesh but also no blocker).
	CHECK(cookable >= 2);
}

namespace
{
	nlohmann::json readJson(const fs::path& p)
	{
		std::ifstream in(p);
		REQUIRE(in.good());
		nlohmann::json j;
		in >> j;
		return j;
	}

	const nlohmann::json* componentsOf(const nlohmann::json& scene, const std::string& name)
	{
		for (const auto& e : scene["entities"])
			if (e.value("name", "") == name && e.contains("components"))
				return &e["components"];
		return nullptr;
	}
}

TEST_CASE("mobility round-trips through .hescene, JSON and binary")
{
	HorizonWorld world;
	auto& reg = world.registry();
	const Entity stat    = makeMesh(world, "S");
	const Entity movable = makeMesh(world, "M");
	const Entity none    = makeMesh(world, "N");
	reg.emplace<MobilityComponent>(stat, mobility(MobilityComponent::Mode::Static));
	reg.emplace<MobilityComponent>(movable, mobility(MobilityComponent::Mode::Movable));
	(void)none;

	for (const SerializeFormat fmt : { SerializeFormat::JSON, SerializeFormat::Binary })
	{
		const fs::path file = fs::temp_directory_path() / "he_test_mobility.hescene";
		SceneSerializer ser;
		REQUIRE(ser.save(world, file, fmt));
		if (fmt == SerializeFormat::JSON)
		{
			// Written as the readable word, and only for the entities that have one.
			const nlohmann::json scene = readJson(file);
			REQUIRE(componentsOf(scene, "S"));
			CHECK((*componentsOf(scene, "S"))["mobility"] == "static");
			CHECK((*componentsOf(scene, "M"))["mobility"] == "movable");
			CHECK_FALSE(componentsOf(scene, "N")->contains("mobility"));
		}

		HorizonWorld loaded;
		REQUIRE(ser.load(loaded, file, fmt));
		he_test::removeQuiet(file);
		auto& lreg = loaded.registry();
		const Entity ls = findByName(loaded, "S");
		const Entity lm = findByName(loaded, "M");
		const Entity ln = findByName(loaded, "N");
		REQUIRE(lreg.valid(ls));
		REQUIRE(lreg.all_of<MobilityComponent>(ls));
		CHECK(lreg.get<MobilityComponent>(ls).mode == MobilityComponent::Mode::Static);
		REQUIRE(lreg.all_of<MobilityComponent>(lm));
		CHECK(lreg.get<MobilityComponent>(lm).mode == MobilityComponent::Mode::Movable);
		// "Nobody decided" stays that way: no component, not a Movable one.
		CHECK_FALSE(lreg.all_of<MobilityComponent>(ln));
	}
}

TEST_CASE("a scene written before MobilityComponent loads with the derived default")
{
	// The old form: a scene whose entities carry no "mobility" key at all (the
	// save path writes none for an entity without the component), plus the
	// repository's own reference scene.
	HorizonWorld world;
	const Entity rock   = makeMesh(world, "Rock");
	const Entity guard  = makeMesh(world, "Guard");
	world.registry().emplace<ScriptComponent>(guard, ScriptComponent{});

	const fs::path file = fs::temp_directory_path() / "he_test_mobility_old.hescene";
	SceneSerializer ser;
	REQUIRE(ser.save(world, file, SerializeFormat::JSON));
	{
		const nlohmann::json scene = readJson(file);
		for (const auto& e : scene["entities"])
			if (e.contains("components"))
				CHECK_FALSE(e["components"].contains("mobility"));
	}
	HorizonWorld loaded;
	REQUIRE(ser.load(loaded, file, SerializeFormat::JSON));
	he_test::removeQuiet(file);
	(void)rock;
	auto& reg = loaded.registry();
	CHECK(reg.view<MobilityComponent>().empty());
	// Derived by the analysis: the plain mesh cooks, the scripted one does not.
	CHECK(HE::analyzeCookability(reg, findByName(loaded, "Rock")).cookable());
	CHECK(HE::analyzeCookability(reg, findByName(loaded, "Guard")).has(CookBlocker::Script));

	// __FILE__ is relative to the build folder under a compiler cache's base_dir,
	// so look for the repository from the working directory upwards.
	fs::path reference;
	for (fs::path dir = fs::current_path(); !dir.empty(); dir = dir.parent_path())
	{
		const fs::path candidate = dir / "docs" / "perf-audit" / "scenes" / "landscape_noclouds.hescene";
		if (fs::exists(candidate)) { reference = candidate; break; }
		if (dir == dir.parent_path()) break;
	}
	if (!reference.empty())
	{
		HorizonWorld ref;
		REQUIRE(ser.load(ref, reference, SerializeFormat::JSON));
		CHECK(ref.registry().view<MobilityComponent>().empty());
		CHECK_FALSE(HE::analyzeCookability(ref.registry()).empty());
	}
}
