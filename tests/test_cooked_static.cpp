#include "doctest.h"
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderWorld.h>
#include <HorizonScene/CellSplit.h>
#include <HorizonScene/CellStreamer.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LODSystem.h>
#include <HorizonScene/CookedStatic.h>
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/SceneJsonParse.h>
#include <HorizonScene/ScriptApi.h>
#include <HorizonScene/ScriptContext.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <HorizonScene/Components/AnimationLayerComponent.h>
#include <HorizonScene/Components/AnimatorBlendComponent.h>
#include <HorizonScene/Components/AnimatorComponent.h>
#include <HorizonScene/Components/AnimatorStateMachineComponent.h>
#include <HorizonScene/Components/CharacterControllerComponent.h>
#include <HorizonScene/Components/InactiveComponent.h>
#include <HorizonScene/Components/IkComponent.h>
#include <HorizonScene/Components/LODComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/MobilityComponent.h>
#include <HorizonScene/Components/MovementComponent.h>
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
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

// Entity cooking, step 3: static meshes extracted from the flat block. The block may
// only ever repeat what the walk writes, so every test compares against an extractor
// that has cooking off.

namespace
{
	struct Scene
	{
		ContentManager cm;
		HorizonWorld   world;
		HE::UUID       meshId;
		std::vector<Entity> statics, movable, scripted, hidden;

		Scene()
		{
			StaticMeshAsset mesh; mesh.type = HE::AssetType::StaticMesh; mesh.name = "box";
			mesh.indices = { 0, 1, 2 };
			mesh.boundsMin[0] = mesh.boundsMin[1] = mesh.boundsMin[2] = -1.0f;
			mesh.boundsMax[0] = mesh.boundsMax[1] = mesh.boundsMax[2] =  1.0f;
			meshId = cm.registerStaticMesh(mesh);
		}

		Entity addMesh(const char* name, glm::vec3 pos)
		{
			const Entity e = world.createEntity(name);
			auto& reg = world.registry();
			TransformComponent t;
			t.position = pos;
			t.rotation = glm::vec3(0.0f, pos.x * 7.0f, 0.0f);
			reg.emplace_or_replace<TransformComponent>(e, t);
			MeshComponent m;
			m.meshAssetId = meshId;
			m.castsShadow = static_cast<int>(pos.x) % 2 == 0;
			reg.emplace_or_replace<MeshComponent>(e, m);
			return e;
		}

		// Cookable meshes interleaved with the three kinds that must stay on the walk.
		void build(int count)
		{
			for (int i = 0; i < count; ++i)
			{
				const glm::vec3 p(static_cast<float>(i), 0.5f, static_cast<float>(i % 3));
				const Entity e = addMesh("M", p);
				switch (i % 5)
				{
				case 1: world.registry().emplace<MobilityComponent>(e, MobilityComponent{ MobilityComponent::Mode::Movable }); movable.push_back(e); break;
				case 2: world.registry().emplace<ScriptComponent>(e); scripted.push_back(e); break;
				case 3: if (i % 2) { world.registry().get<MeshComponent>(e).visible = false; hidden.push_back(e); } else statics.push_back(e); break;
				default: statics.push_back(e); break;
				}
			}
		}
	};

	bool sameBits(const glm::mat4& a, const glm::mat4& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

	// Every field the walk writes for a plain static mesh.
	void checkSameObjects(const RenderWorld& a, const RenderWorld& b)
	{
		REQUIRE(a.objects.size() == b.objects.size());
		for (size_t i = 0; i < a.objects.size(); ++i)
		{
			const RenderObject& x = a.objects[i];
			const RenderObject& y = b.objects[i];
			CHECK(x.entityId == y.entityId);
			CHECK(x.meshAssetId == y.meshAssetId);
			CHECK(x.materialAssetId == y.materialAssetId);
			CHECK(sameBits(x.transform, y.transform));
			CHECK(x.worldBounds.isValid() == y.worldBounds.isValid());
			CHECK(x.worldBounds.min == y.worldBounds.min);
			CHECK(x.worldBounds.max == y.worldBounds.max);
			CHECK(x.lod == y.lod);
			CHECK(x.castsShadow == y.castsShadow);
			CHECK(x.receivesShadow == y.receivesShadow);
			CHECK(x.sections.size() == y.sections.size());
			CHECK(x.paramOverride == y.paramOverride);
		}
	}

	void setUp(RenderExtractor& ex, ContentManager& cm, bool cook)
	{
		ex.setContentManager(&cm);
		ex.setCookStatic(cook);
	}
}

TEST_CASE("CookedStatic: the block holds the cookable meshes, not the movable, scripted or hidden ones")
{
	Scene s;
	s.build(40);
	REQUIRE(s.statics.size() > 10);
	REQUIRE_FALSE(s.movable.empty());
	REQUIRE_FALSE(s.scripted.empty());
	REQUIRE_FALSE(s.hidden.empty());

	HE::refreshCookedStatic(s.world.registry(), s.world.structureEpoch());
	const HE::CookedStatic& c = HE::cookedStaticOf(s.world.registry());
	CHECK(c.size() == s.statics.size());
	for (const Entity e : s.statics)  CHECK(c.rowOf(e) >= 0);
	for (const Entity e : s.movable)  CHECK(c.rowOf(e) < 0);
	for (const Entity e : s.scripted) CHECK(c.rowOf(e) < 0);
	for (const Entity e : s.hidden)   CHECK(c.rowOf(e) < 0);
}

TEST_CASE("CookedStatic: the cooked extract equals the walk, and a second frame copies from the rows")
{
	Scene s;
	s.build(60);

	RenderExtractor walk; setUp(walk, s.cm, false);
	RenderExtractor cook; setUp(cook, s.cm, true);
	RenderWorld ref, rw;
	walk.extract(s.world, ref, 1.5f);

	cook.extract(s.world, rw, 1.5f);   // first walk after the rebuild: rows are Pending, settled on the way
	checkSameObjects(rw, ref);
	const HE::CookedStatic& c = HE::cookedStaticOf(s.world.registry());
	size_t ready = 0;
	for (const auto st : c.state) ready += st == HE::CookedStatic::State::Ready;
	CHECK(ready == s.statics.size());   // positive control: the second extract really runs the fast path

	cook.extract(s.world, rw, 1.5f);   // now served from the rows
	checkSameObjects(rw, ref);
}

TEST_CASE("CookedStatic: visible is read live, a hidden cooked entity vanishes without a rebuild")
{
	Scene s;
	s.build(30);
	RenderExtractor cook; setUp(cook, s.cm, true);
	RenderWorld rw;
	cook.extract(s.world, rw, 1.0f);
	const size_t before = rw.objects.size();
	const uint64_t epoch = s.world.structureEpoch();

	s.world.registry().get<MeshComponent>(s.statics[0]).visible = false;   // a raw write, as several call sites do
	cook.extract(s.world, rw, 1.0f);
	CHECK(s.world.structureEpoch() == epoch);
	CHECK(rw.objects.size() == before - 1);
	for (const RenderObject& o : rw.objects) CHECK(o.entityId != static_cast<uint32_t>(s.statics[0]));
}

TEST_CASE("CookedStatic: a structure change rebuilds the block and a framed extract re-walks")
{
	Scene s;
	s.build(30);
	RenderExtractor cook; setUp(cook, s.cm, true);
	RenderWorld rw;
	{
		RenderExtractor::FrameScope frame(cook);
		cook.extract(s.world, rw, 1.0f);
		cook.extract(s.world, rw, 1.0f);
		CHECK(cook.fullExtractCount() == 1u);    // unchanged structure: the second call reuses the walk
		CHECK(cook.reusedExtractCount() == 1u);

		s.addMesh("Late", glm::vec3(50.0f, 0.5f, 0.0f));   // createEntity moves the epoch
		const size_t had = rw.objects.size();
		cook.extract(s.world, rw, 1.0f);
		CHECK(cook.fullExtractCount() == 2u);    // the key saw it: a new walk, not a reuse
		CHECK(rw.objects.size() == had + 1);
	}

	const HE::CookedStatic& c = HE::cookedStaticOf(s.world.registry());
	CHECK(c.structureEpoch == s.world.structureEpoch());
	CHECK(c.size() == s.statics.size() + 1);

	RenderExtractor walk; setUp(walk, s.cm, false);
	RenderWorld ref;
	walk.extract(s.world, ref, 1.0f);
	cook.extract(s.world, rw, 1.0f);
	checkSameObjects(rw, ref);
}

TEST_CASE("CookedStatic: a mesh that becomes resident later, and a content change, are picked up")
{
	Scene s;
	const Entity e = s.addMesh("Late", glm::vec3(1.0f, 0.5f, 0.0f));
	const HE::UUID missing = HE::UUID::generate();
	s.world.registry().get<MeshComponent>(e).meshAssetId = missing;

	RenderExtractor cook; setUp(cook, s.cm, true);
	RenderWorld rw;
	cook.extract(s.world, rw, 1.0f);
	REQUIRE(rw.objects.size() == 1);
	CHECK_FALSE(rw.objects[0].worldBounds.isValid());   // not resident: bounds unknown, never culled
	const HE::CookedStatic& c = HE::cookedStaticOf(s.world.registry());
	CHECK(c.state[static_cast<size_t>(c.rowOf(e))] == HE::CookedStatic::State::Pending);   // not settled on an unknown mesh

	// The mesh the entity names arrives: the next walk has bounds for it.
	s.world.registry().get<MeshComponent>(e).meshAssetId = s.meshId;
	cook.extract(s.world, rw, 1.0f);
	CHECK(rw.objects[0].worldBounds.isValid());
	CHECK(c.state[static_cast<size_t>(c.rowOf(e))] == HE::CookedStatic::State::Ready);

	// A content change (a mesh edited or imported) sends every row back through the walk.
	HE::CookedStatic& block = HE::cookedStaticOf(s.world.registry());
	block.syncContent(&s.cm, s.cm.contentEpoch());
	CHECK(c.state[static_cast<size_t>(c.rowOf(e))] == HE::CookedStatic::State::Ready);   // same epoch: untouched
	s.cm.noteContentChanged();
	cook.setContentManager(&s.cm);
	block.syncContent(&s.cm, s.cm.contentEpoch());
	CHECK(c.state[static_cast<size_t>(c.rowOf(e))] == HE::CookedStatic::State::Pending);
	cook.extract(s.world, rw, 1.0f);
	CHECK(c.state[static_cast<size_t>(c.rowOf(e))] == HE::CookedStatic::State::Ready);   // settled again, by that walk
}

// ── Step 4: every mutation takes the entity out of the block ─────────────────
// A row is Ready (the extract copies it) before each mutation; the first extract after it must show
// the new value, with no frame in between, and must agree with an extractor that has cooking off.

namespace
{
	const HE::CookedStatic& block(Scene& s) { return HE::cookedStaticOf(s.world.registry()); }

	HE::CookedStatic::State stateOf(Scene& s, Entity e)
	{
		const HE::CookedStatic& c = block(s);
		const int32_t r = c.rowOf(e);
		REQUIRE(r >= 0);
		return c.state[static_cast<size_t>(r)];
	}

	// Two extracts: the first settles the rows, the second runs on them.
	void warm(Scene& s, RenderExtractor& cook, RenderWorld& rw)
	{
		cook.extract(s.world, rw, 1.0f);
		cook.extract(s.world, rw, 1.0f);
		const HE::CookedStatic& c = block(s);
		size_t ready = 0;
		for (const auto st : c.state) ready += st == HE::CookedStatic::State::Ready;
		REQUIRE(ready == c.size());
		REQUIRE(ready > 5);
	}

	const RenderObject* objectOf(const RenderWorld& rw, Entity e)
	{
		for (const RenderObject& o : rw.objects)
			if (o.entityId == static_cast<uint32_t>(e)) return &o;
		return nullptr;
	}

	// The cooked extract right after a mutation: `e` sits where the hierarchy says, and the whole
	// result equals the walk's.
	void checkShowsNow(Scene& s, RenderExtractor& cook, RenderWorld& rw, Entity e)
	{
		RenderExtractor walk; setUp(walk, s.cm, false);
		RenderWorld ref;
		walk.extract(s.world, ref, 1.0f);
		cook.extract(s.world, rw, 1.0f);
		checkSameObjects(rw, ref);
		const RenderObject* o = objectOf(rw, e);
		REQUIRE(o != nullptr);
		const glm::vec3 where = HE::worldPositionOf(s.world, e);
		CHECK(o->transform[3].x == doctest::Approx(where.x));
		CHECK(o->transform[3].y == doctest::Approx(where.y));
		CHECK(o->transform[3].z == doctest::Approx(where.z));
	}

	HE::api::Ctx apiCtx(Scene& s) { HE::api::Ctx c; c.world = &s.world; c.content = &s.cm; return c; }
}

TEST_CASE("CookedStatic un-cook: a transform write, by whichever way, shows in the next extract")
{
	Scene s;
	s.build(40);
	RenderExtractor cook; setUp(cook, s.cm, true);
	RenderWorld rw;
	warm(s, cook, rw);
	const Entity e     = s.statics[2];
	const Entity other = s.statics[3];
	HE::api::Ctx c = apiCtx(s);

	SUBCASE("a raw write through the component pointer (inspector, gizmo, collaboration)")
	{
		s.world.registry().get<TransformComponent>(e).position = { 40.0f, 1.0f, 2.0f };
	}
	SUBCASE("ScriptApi, the one writer under Lua, Python and HorizonCode")
	{
		ScriptApi::setPosition(s.world, static_cast<uint32_t>(e), { 41.0f, 1.0f, 2.0f });
	}
	SUBCASE("HE::api::transform::setPosition")
	{
		HE::api::transform::setPosition(c, static_cast<uint32_t>(e), { 42.0f, 1.0f, 2.0f });
	}
	SUBCASE("HE::api::transform::setWorldPosition")
	{
		HE::api::transform::setWorldPosition(c, static_cast<uint32_t>(e), { 43.0f, 1.0f, 2.0f });
	}
	SUBCASE("HE::api::transform::setRotation")
	{
		HE::api::transform::setRotation(c, static_cast<uint32_t>(e), { 0.0f, 90.0f, 0.0f });
	}
	SUBCASE("HE::api::transform::setScale")
	{
		HE::api::transform::setScale(c, static_cast<uint32_t>(e), { 2.0f, 3.0f, 4.0f });
	}
	SUBCASE("the registry row a HorizonCode node calls")
	{
		const HE::api::ApiFn* fn = HE::api::find("transform.setPosition");
		REQUIRE(fn != nullptr);
		fn->invoke(c, { HorizonCode::Value::ofInt(static_cast<int>(e)), HorizonCode::Value::ofVec3({ 44.0f, 1.0f, 2.0f }) });
	}
	SUBCASE("a Lua script")
	{
		ScriptContext lua(s.world);
		REQUIRE(lua.engine().exec("horizon.setPosition(" + std::to_string(static_cast<uint32_t>(e)) + ", 45, 1, 2)"));
	}

	CHECK(stateOf(s, other) == HE::CookedStatic::State::Ready);   // the rows of other entities are not touched
	checkShowsNow(s, cook, rw, e);
	CHECK(stateOf(s, e) == HE::CookedStatic::State::Ready);       // and the moved one is a row again, settled on the new value
	CHECK(block(s).world[static_cast<size_t>(block(s).rowOf(e))][3].x ==
	      doctest::Approx(HE::worldPositionOf(s.world, e).x));
}

TEST_CASE("CookedStatic un-cook: a write of the value the row already has leaves it cooked")
{
	Scene s;
	s.build(30);
	RenderExtractor cook; setUp(cook, s.cm, true);
	RenderWorld rw;
	warm(s, cook, rw);
	const Entity e = s.statics[1];
	auto& t = s.world.registry().get<TransformComponent>(e);
	const glm::vec3 same = t.position;
	t.position = { 9.0f, 9.0f, 9.0f };
	t.position = same;   // moved and put back between two extracts
	cook.extract(s.world, rw, 1.0f);
	CHECK(stateOf(s, e) == HE::CookedStatic::State::Ready);
}

TEST_CASE("CookedStatic un-cook: moving a transform back and forth keeps the rows equal to the registry")
{
	Scene s;
	s.build(40);
	RenderExtractor cook; setUp(cook, s.cm, true);
	RenderWorld rw;
	warm(s, cook, rw);
	const Entity a = s.statics[0];
	const Entity b = s.statics[4];
	const glm::vec3 homeA = s.world.registry().get<TransformComponent>(a).position;
	const glm::vec3 homeB = s.world.registry().get<TransformComponent>(b).position;

	for (int i = 0; i < 8; ++i)
	{
		const bool away = (i % 2) == 0;
		s.world.registry().get<TransformComponent>(a).position = away ? glm::vec3(100.0f + i, 3.0f, 0.0f) : homeA;
		// b through the API, on the odd rounds only: a is moved every frame, b only now and then
		if (i % 3 == 0)
		{
			HE::api::Ctx c = apiCtx(s);
			HE::api::transform::setPosition(c, static_cast<uint32_t>(b), away ? glm::vec3(-50.0f, 1.0f, 1.0f) : homeB);
		}
		checkShowsNow(s, cook, rw, a);
		checkShowsNow(s, cook, rw, b);

		// Every row agrees with the hierarchy, not only the two that moved.
		const HE::CookedStatic& blk = block(s);
		for (size_t r = 0; r < blk.size(); ++r)
			if (blk.state[r] == HE::CookedStatic::State::Ready)
			{
				const glm::mat4 w = HE::worldMatrixOf(s.world, blk.entity[r]);
				CHECK(std::memcmp(&blk.world[r], &w, sizeof(w)) == 0);
			}
		CHECK(stateOf(s, a) == HE::CookedStatic::State::Ready);
	}
}

TEST_CASE("CookedStatic un-cook: moving a group moves the cooked meshes under it in the same frame")
{
	Scene s;
	s.build(20);
	const Entity group = s.world.createEntity("Group");
	s.world.registry().emplace<TransformComponent>(group);
	const Entity child = s.statics[1];
	REQUIRE(s.world.reparentEntity(child, group));   // epoch moves: the block rebuilds under it

	RenderExtractor cook; setUp(cook, s.cm, true);
	RenderWorld rw;
	warm(s, cook, rw);

	for (int i = 0; i < 4; ++i)
	{
		s.world.registry().get<TransformComponent>(group).position = { 10.0f * (i % 2), 5.0f, 0.0f };   // back and forth
		checkShowsNow(s, cook, rw, child);
		CHECK(stateOf(s, child) == HE::CookedStatic::State::Ready);
	}
}

TEST_CASE("CookedStatic un-cook: a parent change takes effect in the next extract")
{
	Scene s;
	s.build(20);
	const Entity group = s.world.createEntity("Group");
	s.world.registry().emplace<TransformComponent>(group).position = { 30.0f, 0.0f, 0.0f };
	const Entity e = s.statics[2];

	RenderExtractor cook; setUp(cook, s.cm, true);
	RenderWorld rw;
	warm(s, cook, rw);

	REQUIRE(s.world.reparentEntity(e, group));
	checkShowsNow(s, cook, rw, e);
	REQUIRE(s.world.reparentEntity(e, s.world.rootEntity()));
	checkShowsNow(s, cook, rw, e);
	REQUIRE(s.world.reparentEntity(e, group));
	checkShowsNow(s, cook, rw, e);
}

namespace
{
	// Adds a default T to a cooked entity and reports whether the block noticed.
	template<typename T>
	void addAndCheck(const char* what, bool blocks)
	{
		INFO(what);
		Scene s;
		s.build(20);
		RenderExtractor cook; setUp(cook, s.cm, true);
		RenderWorld rw;
		warm(s, cook, rw);
		const Entity e = s.statics[1];
		REQUIRE_FALSE(block(s).stale);
		const uint64_t epoch = s.world.structureEpoch();

		s.world.registry().emplace<T>(e);                       // what a script / the editor does: no epoch change
		CHECK(s.world.structureEpoch() == epoch);
		CHECK(block(s).stale);                                  // the block knows
		cook.extract(s.world, rw, 1.0f);
		CHECK_FALSE(block(s).stale);                            // and rebuilt before this extract
		CHECK((block(s).rowOf(e) < 0) == blocks);
		RenderExtractor walk; setUp(walk, s.cm, false);
		RenderWorld ref;
		walk.extract(s.world, ref, 1.0f);
		checkSameObjects(rw, ref);                              // an uncooked entity renders as any entity does

		// Taking it off again: an entity that has left the block stays out until the structure changes
		// (always correct, never stale); a row that stayed is looked at again.
		s.world.registry().remove<T>(e);
		CHECK(block(s).stale == !blocks);
		cook.extract(s.world, rw, 1.0f);
		CHECK((block(s).rowOf(e) >= 0) == !blocks);
		walk.extract(s.world, ref, 1.0f);
		checkSameObjects(rw, ref);
	}
}

TEST_CASE("CookedStatic un-cook: a component added to a live cooked entity un-cooks it")
{
	addAndCheck<ScriptComponent>("Script", true);
	addAndCheck<AnimatorComponent>("Animator", true);
	addAndCheck<AnimationLayerComponent>("AnimationLayer", true);
	addAndCheck<AnimatorBlendComponent>("AnimatorBlend", true);
	addAndCheck<AnimatorStateMachineComponent>("AnimatorStateMachine", true);
	addAndCheck<RootMotionComponent>("RootMotion", true);
	addAndCheck<IkComponent>("Ik", true);
	addAndCheck<PropertyAnimatorComponent>("PropertyAnimator", true);
	addAndCheck<SequencePlayerComponent>("SequencePlayer", true);
	addAndCheck<CharacterControllerComponent>("CharacterController", true);
	addAndCheck<NavAgentComponent>("NavAgent", true);
	addAndCheck<MovementComponent>("Movement", true);
	addAndCheck<ParticleSystemComponent>("ParticleSystem", true);
	addAndCheck<SaveStateComponent>("SaveState", true);
	addAndCheck<TrailComponent>("Trail", true);
	addAndCheck<RopeComponent>("Rope", true);
	addAndCheck<InactiveComponent>("Inactive", true);
	addAndCheck<MobilityComponent>("Mobility (default Movable)", true);
	addAndCheck<NetworkComponent>("Network (replicates by default)", true);
	// Present but harmless by default (a static body, no LOD levels, no declared vars): the block
	// still looks again, and the verdict keeps the row.
	addAndCheck<RigidBodyComponent>("RigidBody (Static)", false);
	addAndCheck<LODComponent>("LOD (no levels)", false);
	addAndCheck<ReplicatedVarsComponent>("ReplicatedVars (empty)", false);
}

TEST_CASE("CookedStatic un-cook: a blocker on a parent takes the cooked children with it")
{
	Scene s;
	s.build(20);
	const Entity group = s.world.createEntity("Group");
	s.world.registry().emplace<TransformComponent>(group);
	const Entity child = s.statics[1];
	REQUIRE(s.world.reparentEntity(child, group));

	RenderExtractor cook; setUp(cook, s.cm, true);
	RenderWorld rw;
	warm(s, cook, rw);
	REQUIRE(block(s).rowOf(child) >= 0);

	s.world.registry().emplace<ScriptComponent>(group);          // the parent now has a script: it may move
	cook.extract(s.world, rw, 1.0f);
	CHECK(block(s).rowOf(child) < 0);
	s.world.registry().get<TransformComponent>(group).position = { 0.0f, 7.0f, 0.0f };
	checkShowsNow(s, cook, rw, child);
}

TEST_CASE("CookedStatic un-cook: a material or mesh swap on a cooked entity shows in the next extract")
{
	Scene s;
	s.build(20);
	RenderExtractor cook; setUp(cook, s.cm, true);
	RenderWorld rw;
	warm(s, cook, rw);
	const Entity e = s.statics[2];
	const HE::UUID mat = HE::UUID::generate();

	MaterialComponent mc;
	mc.materialAssetId = mat;
	s.world.registry().emplace_or_replace<MaterialComponent>(e, mc);   // added: a signal, no raw write
	CHECK(stateOf(s, e) == HE::CookedStatic::State::Pending);
	checkShowsNow(s, cook, rw, e);
	CHECK(objectOf(rw, e)->materialAssetId == mat);

	const HE::UUID mat2 = HE::UUID::generate();
	mc.materialAssetId = mat2;
	s.world.registry().emplace_or_replace<MaterialComponent>(e, mc);   // replaced: on_update
	checkShowsNow(s, cook, rw, e);
	CHECK(objectOf(rw, e)->materialAssetId == mat2);

	MeshComponent m = s.world.registry().get<MeshComponent>(e);
	m.castsShadow = !m.castsShadow;
	s.world.registry().emplace_or_replace<MeshComponent>(e, m);
	checkShowsNow(s, cook, rw, e);
}

TEST_CASE("CookedStatic un-cook: uncookEntity covers a raw field write nothing can observe")
{
	Scene s;
	s.build(20);
	RenderExtractor cook; setUp(cook, s.cm, true);
	RenderWorld rw;
	warm(s, cook, rw);
	const Entity e = s.statics[2];
	const bool shadow = s.world.registry().get<MeshComponent>(e).castsShadow;

	s.world.registry().get<MeshComponent>(e).castsShadow = !shadow;   // an inspector checkbox
	cook.extract(s.world, rw, 1.0f);
	CHECK(objectOf(rw, e)->castsShadow == shadow);                    // positive control: a raw write alone is not seen

	HE::uncookEntity(s.world.registry(), e);                          // what the inspector does for the entity it shows
	checkShowsNow(s, cook, rw, e);
	CHECK(objectOf(rw, e)->castsShadow == !shadow);
	HE::uncookEntity(s.world.registry(), entt::null);                 // a null or unknown entity is fine
}

// ── Export: the packed verdict (project setting "Cook static entities") ──────────────────────────────────
namespace
{
	// Saved the way the export saves a scene, with or without the verdict.
	std::vector<uint8_t> pack(Scene& s, bool withVerdict)
	{
		std::vector<uint8_t> bytes;
		REQUIRE(SceneSerializer().saveToMemory(s.world, bytes, withVerdict));
		return bytes;
	}

	void load(HorizonWorld& w, const std::vector<uint8_t>& bytes)
	{
		REQUIRE(SceneSerializer().loadFromMemory(w, bytes));
	}

	// The rows a cooked extract of `w` holds, and the output of an extractor that never cooks one
	// loaded from the same scene without the verdict (the pack an older export wrote).
	void checkDrawsLikeOldPack(Scene& s, HorizonWorld& packed, RenderWorld& got)
	{
		HorizonWorld old;
		load(old, pack(s, false));
		RenderExtractor walk; setUp(walk, s.cm, false);
		RenderWorld ref;
		walk.extract(old, ref, 1.0f);
		checkSameObjects(got, ref);
	}
}

TEST_CASE("CookedStatic pack: a scene saved with the verdict loads it, skips the analysis and draws like an old pack")
{
	Scene s;
	s.build(60);
	const std::vector<uint8_t> bytes = pack(s, true);

	HorizonWorld w;
	load(w, bytes);
	const HE::CookedStatic* c = w.registry().ctx().find<HE::CookedStatic>();
	REQUIRE(c != nullptr);
	CHECK(c->enabled);                          // a pack exported with the setting on is cooked without HE_COOK_STATIC
	CHECK(c->seed.size() == s.statics.size());
	CHECK(c->seededBuilds == 0u);               // nothing has asked for the block yet
	CHECK(c->analysedBuilds == 0u);

	RenderExtractor ex; setUp(ex, s.cm, false);   // the env switch is off: the world's own flag does it
	RenderWorld rw;
	ex.extract(w, rw, 1.0f);
	CHECK(c->seededBuilds == 1u);
	CHECK(c->analysedBuilds == 0u);             // analyzeCookability never ran for this load
	CHECK(c->size() == s.statics.size());
	CHECK(c->seed.empty());                     // one shot
	for (const Entity e : s.statics) CHECK(c->rowOf(w.findByEntityId(s.world.registry().get<EntityIdComponent>(e).id)) >= 0);
	size_t ready = 0;
	for (const auto st : c->state) ready += st == HE::CookedStatic::State::Ready;
	CHECK(ready == s.statics.size());           // positive control: the rows really serve the draw

	checkDrawsLikeOldPack(s, w, rw);
	ex.extract(w, rw, 1.0f);                    // second frame, from the rows
	checkDrawsLikeOldPack(s, w, rw);
}

TEST_CASE("CookedStatic pack: a scene without the verdict (an older export) loads and cooks nothing")
{
	Scene s;
	s.build(30);
	HorizonWorld w;
	load(w, pack(s, false));
	CHECK(w.registry().ctx().find<HE::CookedStatic>() == nullptr);

	RenderExtractor ex; setUp(ex, s.cm, false);
	RenderWorld rw;
	ex.extract(w, rw, 1.0f);
	CHECK(w.registry().ctx().find<HE::CookedStatic>() == nullptr);   // no block was even made
	RenderExtractor walk; setUp(walk, s.cm, false);
	RenderWorld ref;
	walk.extract(s.world, ref, 1.0f);
	CHECK(rw.objects.size() == ref.objects.size());   // every visible mesh, as the original world draws it
}

TEST_CASE("CookedStatic pack: a verdict component added before the first extract puts the analysis back")
{
	Scene s;
	s.build(40);
	HorizonWorld w;
	load(w, pack(s, true));
	const Entity victim = w.findByEntityId(s.world.registry().get<EntityIdComponent>(s.statics[3]).id);
	REQUIRE((victim != entt::null));
	w.registry().emplace<ScriptComponent>(victim);   // a Start() script attaching one, on the entity the pack called static

	RenderExtractor ex; setUp(ex, s.cm, false);
	RenderWorld rw;
	ex.extract(w, rw, 1.0f);
	const HE::CookedStatic& c = HE::cookedStaticOf(w.registry());
	CHECK(c.seededBuilds == 0u);
	CHECK(c.analysedBuilds == 1u);
	CHECK(c.rowOf(victim) < 0);                       // not cooked: the verdict the pack carried is stale
	CHECK(c.size() == s.statics.size() - 1);
}

TEST_CASE("CookedStatic pack: a change of structure after the load, or a verdict that names nothing, is analysed")
{
	Scene s;
	s.build(40);
	const std::vector<uint8_t> bytes = pack(s, true);
	RenderExtractor ex; setUp(ex, s.cm, false);
	RenderWorld rw;

	{   // an entity spawned between the load and the first extract moves the epoch
		HorizonWorld w;
		load(w, bytes);
		w.createEntity("Spawned");
		ex.extract(w, rw, 1.0f);
		const HE::CookedStatic& c = HE::cookedStaticOf(w.registry());
		CHECK(c.seededBuilds == 0u);
		CHECK(c.analysedBuilds == 1u);
		CHECK(c.size() == s.statics.size());
	}
	{   // a scene whose entity the verdict lists was edited away by something that did not know it
		nlohmann::json j = HE::parseSceneCbor(bytes);
		REQUIRE(j.contains("cookedStatic"));
		j["entities"].erase(j["entities"].begin());
		HorizonWorld w;
		load(w, nlohmann::json::to_cbor(j));
		const HE::CookedStatic* c = w.registry().ctx().find<HE::CookedStatic>();
		CHECK((c == nullptr || !c->enabled));         // dropped as a whole: no seed, no cooking asked for
		CHECK((c == nullptr || c->seed.empty()));
	}
}

// ─── Streamed cells (step 7) ──────────────────────────────────────────────────
// A cell's entities are destroyed with it and built again from its file on the next load: new
// entt handles, the same UUIDs. The block holds handles, and a structure change rebuilds it from
// the live registry, so a cell needs no cook data of its own. These tests run the real splitter
// and streamer, with the Auto streaming mark (the one rule 11 used to refuse) on part of the cell.

namespace
{
	using CellFiles = std::map<std::string, std::string>;

	// One gameplay frame of streaming at `at`, until `done` holds or a few seconds have passed.
	template<class Done>
	void streamUntil(HE::CellStreamer& st, HorizonWorld& w, double x, Done done)
	{
		for (int i = 0; i < 3000 && !done(); ++i)
		{
			st.update(w, glm::dvec3(x, 1.7, 0.0), glm::vec3(0.0f), 4.0);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	void checkCookedEqualsWalk(HorizonWorld& w, ContentManager& cm, RenderExtractor& cook, RenderWorld& rw)
	{
		cook.extract(w, rw, 1.0f);
		cook.extract(w, rw, 1.0f);   // the second frame runs on settled rows
		RenderExtractor walk; setUp(walk, cm, false);
		RenderWorld ref;
		walk.extract(w, ref, 1.0f);
		checkSameObjects(rw, ref);
	}

	std::vector<Entity> autoStreamed(HorizonWorld& w)
	{
		std::vector<Entity> out;
		for (auto [e, st] : w.registry().view<StreamingComponent>().each())
			if (st.mode == StreamingComponent::Mode::Auto) out.push_back(e);
		return out;
	}
}

TEST_CASE("CookedStatic cells: a streamed cell is cooked, leaves with its cell and comes back cooked")
{
	Scene ed;
	ed.build(30);   // x = 0..29 along a row, a cell is 10 m wide: three cells
	for (size_t i = 0; i < 4; ++i)
		ed.world.registry().emplace<StreamingComponent>(ed.statics[i], StreamingComponent{ StreamingComponent::Mode::Auto });

	CellFiles files;
	HE::CellSplitOptions opt;
	opt.cellSize = 10.0f;
	opt.dir      = "Content/T.cells";
	const HE::CellSplitResult split = HE::splitWorldIntoCells(ed.world, opt,
		[&](const std::string& path, const std::string& text) { files[path] = text; return true; });
	REQUIRE(split.error.empty());
	REQUIRE(split.moved > 10);

	HorizonWorld game;
	{
		std::vector<uint8_t> bytes;
		REQUIRE(SceneSerializer().saveToMemory(ed.world, bytes));
		REQUIRE(SceneSerializer().loadFromMemory(game, bytes));
	}
	HE::CellManifest manifest;
	REQUIRE(HE::CellManifest::parse(game.cellManifestJson(), manifest));
	HE::CellStreamer streamer;
	streamer.begin(manifest, [&files](const std::string& path) -> std::function<bool(std::vector<uint8_t>&)>
	{
		const auto it = files.find(path);
		if (it == files.end()) return {};
		const std::string bytes = it->second;
		return [bytes](std::vector<uint8_t>& out) { out.assign(bytes.begin(), bytes.end()); return true; };
	}, {});

	RenderExtractor cook; setUp(cook, ed.cm, true);
	RenderWorld rw;
	const HE::CookedStatic& c = HE::cookedStaticOf(game.registry());

	// Loaded: the cell's meshes, the Auto-marked ones among them, are rows and draw as the walk does.
	// Until the neighbouring cells in range are in as well (they stream on the pool: counting rows while
	// one is still on its way made this test race).
	const auto streamIdle = [&] {
		const HE::CellStreamer::Stats& st = streamer.stats();
		return streamer.isLoaded(0, 0) && st.inFlight + st.ready + st.building == 0;
	};
	streamUntil(streamer, game, 5.0, streamIdle);
	REQUIRE(streamer.isLoaded(0, 0));
	checkCookedEqualsWalk(game, ed.cm, cook, rw);
	const std::vector<Entity> marked = autoStreamed(game);
	REQUIRE_FALSE(marked.empty());
	for (const Entity e : marked) CHECK(c.rowOf(e) >= 0);
	const size_t rowsLoaded = c.size();
	REQUIRE(rowsLoaded > 0);
	const uint32_t analysedLoaded = c.analysedBuilds;

	// Unloaded: the entities are gone, so are their rows, and no row points at a dead entity.
	streamUntil(streamer, game, 500.0, [&] { return !streamer.isLoaded(0, 0); });
	REQUIRE_FALSE(streamer.isLoaded(0, 0));
	checkCookedEqualsWalk(game, ed.cm, cook, rw);
	for (const Entity e : marked) CHECK(c.rowOf(e) < 0);
	for (const Entity e : c.entity) CHECK(game.registry().valid(e));
	CHECK(c.size() < rowsLoaded);
	CHECK(c.analysedBuilds > analysedLoaded);   // a rebuild followed the unload

	// Loaded again: new handles, the same ids, the Auto-marked meshes cooked again.
	streamUntil(streamer, game, 5.0, streamIdle);
	REQUIRE(streamer.isLoaded(0, 0));
	checkCookedEqualsWalk(game, ed.cm, cook, rw);
	const std::vector<Entity> markedAgain = autoStreamed(game);
	CHECK(markedAgain.size() == marked.size());
	for (const Entity e : markedAgain)
	{
		REQUIRE(c.rowOf(e) >= 0);
		CHECK(c.state[static_cast<size_t>(c.rowOf(e))] == HE::CookedStatic::State::Ready);
	}
	CHECK(c.size() == rowsLoaded);
	streamer.clear(game);
}

// ── LOD rows (survey rule 8) ─────────────────────────────────────────────────
// A mesh with several levels is cooked: the row holds what the walk builds for EACH level and the fast path
// indexes it by LODSystem's live choice. Every frame is compared with an extractor that has cooking off,
// while the camera sweeps across the level boundaries, hysteresis band included.

namespace
{
	// A box mesh of `half` extent, optionally with two sections (a multi-section level is never fast).
	HE::UUID registerBox(Scene& s, float half, bool twoSections = false)
	{
		StaticMeshAsset mesh; mesh.type = HE::AssetType::StaticMesh; mesh.name = "lodbox";
		mesh.indices = { 0, 1, 2, 0, 2, 3 };
		mesh.boundsMin[0] = mesh.boundsMin[1] = mesh.boundsMin[2] = -half;
		mesh.boundsMax[0] = mesh.boundsMax[1] = mesh.boundsMax[2] =  half;
		if (twoSections)
		{
			MeshSection a; a.indexOffset = 0; a.indexCount = 3;
			MeshSection b; b.indexOffset = 3; b.indexCount = 3;
			mesh.sections = { a, b };
		}
		return s.cm.registerStaticMesh(mesh);
	}

	struct LodScene
	{
		Scene              s;
		HE::UUID           level[3];
		std::vector<Entity> ents;

		explicit LodScene(int count, bool secondLevelMultiSection = false)
		{
			level[0] = registerBox(s, 1.0f);
			level[1] = registerBox(s, 2.0f, secondLevelMultiSection);
			level[2] = registerBox(s, 4.0f);
			for (int i = 0; i < count; ++i)
			{
				const Entity e = s.addMesh("L", { 3.0f * static_cast<float>(i), 0.5f, 1.0f });
				LODComponent lod;
				lod.levels = { { level[0], 15.0f }, { level[1], 40.0f }, { level[2], 1e9f } };
				s.world.registry().emplace<LODComponent>(e, lod);
				s.world.registry().get<MeshComponent>(e).meshAssetId = level[0];
				ents.push_back(e);
			}
		}
	};

	// One frame of a sweep: the tick's LOD pick for `cam`, then both extractors.
	void lodFrame(LodScene& l, RenderExtractor& walk, RenderExtractor& cook, const glm::vec3& cam,
	              RenderWorld& rwWalk, RenderWorld& rwCook)
	{
		LODSystem::update(l.s.world, cam);
		walk.extract(l.s.world, rwWalk, 1.0f);
		cook.extract(l.s.world, rwCook, 1.0f);
		checkSameObjects(rwCook, rwWalk);
	}

	size_t readyRows(Scene& s)
	{
		size_t ready = 0;
		for (const auto st : block(s).state) ready += st == HE::CookedStatic::State::Ready;
		return ready;
	}
}

TEST_CASE("CookedStatic LOD: a cooked LOD row draws the level LODSystem picked, to the bit, across a camera sweep")
{
	LodScene l(12);
	RenderExtractor walk; setUp(walk, l.s.cm, false);
	RenderExtractor cook; setUp(cook, l.s.cm, true);
	RenderWorld rwWalk, rwCook;

	lodFrame(l, walk, cook, { 0.0f, 0.5f, 0.0f }, rwWalk, rwCook);   // settles the rows
	lodFrame(l, walk, cook, { 0.0f, 0.5f, 0.0f }, rwWalk, rwCook);   // runs on them
	REQUIRE(block(l.s).size() == 12);
	CHECK(readyRows(l.s) == 12);   // positive control: the sweep below is served from the rows
	for (const Entity e : l.ents) CHECK(block(l.s).lodFirst[static_cast<size_t>(block(l.s).rowOf(e))] >= 0);

	// Out and back through every boundary (15 and 40, and the 5 % band under them), in steps small
	// enough to land inside the band.
	bool seen[3] = { false, false, false };
	for (int pass = 0; pass < 2; ++pass)
		for (int i = 0; i <= 90; ++i)
		{
			const float x = pass == 0 ? 1.7f * static_cast<float>(i) - 40.0f : 113.0f - 1.7f * static_cast<float>(i);
			lodFrame(l, walk, cook, { x, 0.5f, 0.0f }, rwWalk, rwCook);
			for (const RenderObject& o : rwCook.objects)
				for (int k = 0; k < 3; ++k) seen[k] |= o.meshAssetId == l.level[k];
		}
	CHECK(seen[0]);
	CHECK(seen[1]);
	CHECK(seen[2]);
	CHECK(readyRows(l.s) == 12);   // no frame of the sweep sent a row back to the walk
}

TEST_CASE("CookedStatic LOD: a level that is not resident keeps the row pending, a multi-section level walks it")
{
	SUBCASE("a level that is not resident yet")
	{
		LodScene l(6);
		l.s.world.registry().get<LODComponent>(l.ents[2]).levels[2].meshId = HE::UUID::generate();
		RenderExtractor walk; setUp(walk, l.s.cm, false);
		RenderExtractor cook; setUp(cook, l.s.cm, true);
		RenderWorld rwWalk, rwCook;
		for (int i = 0; i < 3; ++i) lodFrame(l, walk, cook, { 0.0f, 0.5f, 0.0f }, rwWalk, rwCook);
		CHECK(block(l.s).state[static_cast<size_t>(block(l.s).rowOf(l.ents[2]))] == HE::CookedStatic::State::Pending);
		CHECK(readyRows(l.s) == 5);
	}
	SUBCASE("a level with several sections")
	{
		LodScene l(6, true);
		RenderExtractor walk; setUp(walk, l.s.cm, false);
		RenderExtractor cook; setUp(cook, l.s.cm, true);
		RenderWorld rwWalk, rwCook;
		for (int i = 0; i < 3; ++i) lodFrame(l, walk, cook, { 0.0f, 0.5f, 0.0f }, rwWalk, rwCook);
		CHECK(readyRows(l.s) == 0);
		for (const auto st : block(l.s).state) CHECK(st == HE::CookedStatic::State::Walk);
		lodFrame(l, walk, cook, { 30.0f, 0.5f, 0.0f }, rwWalk, rwCook);   // the multi-section level is drawn
	}
}

TEST_CASE("CookedStatic LOD: a row settled to Walk is not settled again every frame, an un-cook settles it anew")
{
	LodScene l(4);
	RenderExtractor walk; setUp(walk, l.s.cm, false);
	RenderExtractor cook; setUp(cook, l.s.cm, true);
	RenderWorld rwWalk, rwCook;
	const Entity e = l.ents[1];
	auto& reg = l.s.world.registry();
	const auto stateOf = [&] { return block(l.s).state[static_cast<size_t>(block(l.s).rowOf(e))]; };

	// A drawn mesh that is none of the levels settles Walk (see settleLodRow).
	reg.get<MeshComponent>(e).meshAssetId = l.s.meshId;
	walk.extract(l.s.world, rwWalk, 1.0f);
	cook.extract(l.s.world, rwCook, 1.0f);
	REQUIRE(stateOf() == HE::CookedStatic::State::Walk);

	// The mesh is a level again, as LODSystem would write it. A settle would make the row Ready; Walk stays.
	reg.get<MeshComponent>(e).meshAssetId = l.level[0];
	for (int i = 0; i < 3; ++i)
	{
		walk.extract(l.s.world, rwWalk, 1.0f);
		cook.extract(l.s.world, rwCook, 1.0f);
		checkSameObjects(rwCook, rwWalk);
		CHECK(stateOf() == HE::CookedStatic::State::Walk);
	}

	// An un-cook sends it back to Pending, and the next walk settles it again.
	HE::uncookEntity(reg, e);
	cook.extract(l.s.world, rwCook, 1.0f);
	CHECK(stateOf() == HE::CookedStatic::State::Ready);
	CHECK(readyRows(l.s) == 4);
}

TEST_CASE("CookedStatic LOD: a refined mesh is not cooked, a moved LOD entity and a swapped mesh show at once")
{
	LodScene l(8);
	RenderExtractor walk; setUp(walk, l.s.cm, false);
	RenderExtractor cook; setUp(cook, l.s.cm, true);
	RenderWorld rwWalk, rwCook;

	const Entity refined = l.ents[0];
	l.s.world.registry().get<LODComponent>(refined).refinedMeshId = l.level[2];
	lodFrame(l, walk, cook, { 0.0f, 0.5f, 0.0f }, rwWalk, rwCook);
	lodFrame(l, walk, cook, { 0.0f, 0.5f, 0.0f }, rwWalk, rwCook);
	CHECK(block(l.s).rowOf(refined) < 0);   // rule 8: the terrain's refined mesh comes and goes at runtime
	CHECK(readyRows(l.s) == 7);

	const Entity moved = l.ents[3];
	l.s.world.registry().get<TransformComponent>(moved).position = { 90.0f, 0.5f, 1.0f };   // out to the far level
	lodFrame(l, walk, cook, { 0.0f, 0.5f, 0.0f }, rwWalk, rwCook);
	CHECK(objectOf(rwCook, moved)->meshAssetId == l.level[2]);
	lodFrame(l, walk, cook, { 0.0f, 0.5f, 0.0f }, rwWalk, rwCook);

	// A mesh id that none of the levels holds (written raw, nothing reported): the guard sends the row to the
	// walk, which draws what the entity says.
	const Entity swapped = l.ents[5];
	l.s.world.registry().get<MeshComponent>(swapped).meshAssetId = l.s.meshId;
	walk.extract(l.s.world, rwWalk, 1.0f);
	cook.extract(l.s.world, rwCook, 1.0f);
	checkSameObjects(rwCook, rwWalk);
	CHECK(objectOf(rwCook, swapped)->meshAssetId == l.s.meshId);
	cook.extract(l.s.world, rwCook, 1.0f);
	CHECK(block(l.s).state[static_cast<size_t>(block(l.s).rowOf(swapped))] == HE::CookedStatic::State::Walk);
}
