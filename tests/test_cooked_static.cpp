#include "doctest.h"
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderWorld.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/CookedStatic.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/MobilityComponent.h>
#include <HorizonScene/Components/ScriptComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <cstring>
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
