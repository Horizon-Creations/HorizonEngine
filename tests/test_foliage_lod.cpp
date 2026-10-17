// Foliage detail levels (Thema 182, Schritt 3 / plan 3a): FoliageLayer::lods.
//
// What this file holds the levels to: every plant in range is drawn exactly once,
// with the mesh of the level its own distance picks, in all three extraction modes;
// a level's thinning keeps the same plants whether its bucket is drawn whole or split
// by a boundary; a plant near a boundary keeps the level it had (hysteresis), per view,
// and the memory behind that does not outlive its store; the levels save, load and
// reach an export; the cross card is a committed file with the UUID the code names.
// A layer WITHOUT levels is held to the old behaviour by test_foliage_cluster.cpp.
#include "doctest.h"
#include "../src/HE_Rendering/src/FoliageExtract.h"
#include <Renderer/IRenderer.h>
#include <HorizonRendering/CommandBuffer.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderPass.h>
#include <HorizonRendering/RenderWorld.h>
#include <HorizonScene/FoliageSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/SceneSystems.h>
#include <HorizonScene/Components/FoliageComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/DefaultAssets.h>
#include <nlohmann/json.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using json = nlohmann::json;

	// The extraction mode is process-wide; a test that flips it puts it back.
	struct Mode
	{
		explicit Mode(HE::FoliageMode m) { HE::setFoliageModeOverride(static_cast<int>(m)); }
		~Mode() { HE::setFoliageModeOverride(-1); }
	};

	// A flat 200 m landscape at the origin with one cube layer: ~2000 plants in 32 m
	// buckets, drawn out to 80 m, plus whatever levels the test gives it.
	struct Field
	{
		HorizonWorld   world;
		ContentManager cm;
		entt::entity   terrain = entt::null;

		explicit Field(std::vector<FoliageLod> lods, float hysteresis = 2.0f)
		{
			auto& reg = world.registry();
			terrain = world.createEntity("Terrain");
			reg.emplace_or_replace<TransformComponent>(terrain);
			TerrainComponent tc;
			tc.sizeX = tc.sizeZ = 200.0f;
			tc.heightScale = 0.0f;
			tc.seed = 0;
			reg.emplace<TerrainComponent>(terrain, tc);
			FoliageComponent fol;
			FoliageLayer& l = fol.layers[0];
			l.meshAssetId   = HE::kDefaultCubeMeshId;
			l.density       = 0.05f;
			l.seed          = 23;
			l.drawDistance  = 80.0f;
			l.lods          = std::move(lods);
			l.lodHysteresis = hysteresis;
			reg.emplace<FoliageComponent>(terrain, fol);
			FoliageSystem::update(world);
			REQUIRE(layer().store);
			REQUIRE(layer().store->local.size() > 1000u);
		}
		FoliageLayer& layer() { return world.registry().get<FoliageComponent>(terrain).layers[0]; }
	};

	FoliageLod lod(float distance, HE::UUID mesh = {}, float density = 1.0f)
	{
		FoliageLod d;
		d.meshAssetId = mesh;
		d.distance    = distance;
		d.density     = density;
		return d;
	}

	EditorCameraOverride camAt(const glm::vec3& pos)
	{
		EditorCameraOverride c;
		c.active      = true;
		c.position    = pos;
		c.view        = glm::lookAt(pos, pos + glm::vec3(0.0f, -0.3f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		c.editorIcons = false;
		return c;
	}

	RenderWorld extractWith(RenderExtractor& ex, Field& f, const glm::vec3& cam)
	{
		ex.setContentManager(&f.cm);
		RenderWorld rw;
		const EditorCameraOverride c = camAt(cam);
		ex.extract(f.world, rw, 16.0f / 9.0f, &c);
		return rw;
	}

	bool matLess(const glm::mat4& a, const glm::mat4& b)
	{
		return std::lexicographical_compare(glm::value_ptr(a), glm::value_ptr(a) + 16,
		                                    glm::value_ptr(b), glm::value_ptr(b) + 16);
	}

	// HE::UUID has no ordering of its own.
	struct UuidLess
	{
		bool operator()(const HE::UUID& a, const HE::UUID& b) const { return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo; }
	};
	using ByMesh = std::map<HE::UUID, std::vector<glm::mat4>, UuidLess>;
	void sortAll(ByMesh& m) { for (auto& [id, v] : m) std::sort(v.begin(), v.end(), matLess); }

	// Every instance GeometryPass draws for `rw`, by mesh.
	ByMesh drawn(const RenderWorld& rw)
	{
		std::vector<uint32_t> idx(rw.objects.size());
		std::iota(idx.begin(), idx.end(), 0u);
		CommandBuffer cmds;
		GeometryPass{}.execute(rw, idx, cmds);
		ByMesh out;
		for (const DrawCall& dc : cmds.drawCalls())
		{
			auto& v = out[dc.meshAssetId];
			if (dc.instanceTransforms.empty()) v.push_back(dc.transform);
			else v.insert(v.end(), dc.instanceTransforms.begin(), dc.instanceTransforms.end());
		}
		sortAll(out);
		return out;
	}

	// The test's own copy of the rule: level by ground-plane distance (the last level
	// that has started), then the level's density keeps the first plants of each bucket.
	struct Expected
	{
		ByMesh   byMesh;
		std::map<std::pair<float, float>, int> level;   // plant (x, z) -> level, drawn ones only
		uint64_t thinned = 0;
	};
	Expected expected(Field& f, const glm::vec3& cam)
	{
		const FoliageLayer& l = f.layer();
		const FoliageStore& s = *l.store;
		std::vector<FoliageLod> lods = l.lods;
		std::stable_sort(lods.begin(), lods.end(),
		                 [](const FoliageLod& a, const FoliageLod& b) { return a.distance < b.distance; });
		Expected e;
		const float dd2 = l.drawDistance * l.drawDistance;
		for (const FoliageBucket& b : s.buckets)
			for (uint32_t j = 0; j < b.count; ++j)
			{
				const glm::mat4& m = s.local[b.first + j];   // terrain at the origin: local == world
				const float dx = m[3].x - cam.x, dz = m[3].z - cam.z;
				const float d2 = dx * dx + dz * dz;
				if (d2 > dd2) continue;
				const float d = std::sqrt(d2);
				int level = 0;
				for (size_t k = 0; k < lods.size(); ++k)
					if (lods[k].distance <= d) level = static_cast<int>(k + 1);
				const float density = level == 0 ? 1.0f : lods[level - 1].density;
				const uint32_t kept = density >= 1.0f ? b.count
				                    : static_cast<uint32_t>(static_cast<double>(density) * b.count + 0.5);
				if (j >= kept) { ++e.thinned; continue; }
				const HE::UUID mesh = level == 0 || lods[level - 1].meshAssetId == HE::UUID{}
				                    ? l.meshAssetId : lods[level - 1].meshAssetId;
				e.byMesh[mesh].push_back(m);
				e.level[{ m[3].x, m[3].z }] = level;
			}
		sortAll(e.byMesh);
		return e;
	}

	// The level of every drawn plant, read back from which mesh drew it (each level of
	// the hysteresis test has a mesh of its own).
	std::map<std::pair<float, float>, int> levelsOf(const ByMesh& m, const std::vector<HE::UUID>& meshOfLevel)
	{
		std::map<std::pair<float, float>, int> out;
		for (const auto& [id, v] : m)
		{
			const auto it = std::find(meshOfLevel.begin(), meshOfLevel.end(), id);
			REQUIRE(it != meshOfLevel.end());
			for (const glm::mat4& x : v)
			{
				const bool fresh = out.emplace(std::make_pair(x[3].x, x[3].z),
				                               static_cast<int>(it - meshOfLevel.begin())).second;
				CHECK(fresh);   // no plant is drawn twice
			}
		}
		return out;
	}

	HE::AABB meshBox(const ContentManager& cm, const HE::UUID& id)
	{
		const StaticMeshAsset* m = cm.getStaticMesh(id);
		REQUIRE(m);
		REQUIRE(m->vertices.size() >= 3);
		return HE::AABB::fromPositions(m->vertices.data(), m->vertices.size() / 3);
	}
}

// ─── Which plant draws with which mesh ───────────────────────────────────────

TEST_CASE("Foliage levels: every plant in range is drawn once, with the mesh its distance picks, in every mode")
{
	// Level 1 a quad from 25 m, level 2 the snowflake from 50 m at half density;
	// the camera in the middle, so buckets lie wholly in each level and others are
	// cut by both boundaries and by the draw distance.
	Field f({ lod(25.0f, HE::kDefaultQuadMeshId), lod(50.0f, HE::kDefaultSnowflakeMeshId, 0.5f) });
	const glm::vec3 cam(3.0f, 4.0f, -2.0f);
	const Expected want = expected(f, cam);
	REQUIRE(want.byMesh.size() == 3u);
	REQUIRE(want.thinned > 0u);

	for (const HE::FoliageMode m : { HE::FoliageMode::Clusters, HE::FoliageMode::PerInstance, HE::FoliageMode::Ordered })
	{
		CAPTURE(static_cast<int>(m));
		Mode mode(m);
		RenderExtractor ex;
		const RenderWorld rw = extractWith(ex, f, cam);
		const ByMesh got = drawn(rw);
		REQUIRE(got.size() == want.byMesh.size());
		for (const auto& [id, v] : want.byMesh)
		{
			REQUIRE(got.count(id) == 1u);
			CHECK(got.at(id).size() == v.size());
			CHECK(got.at(id) == v);
		}
		const HE::FoliageExtractStats& st = HE::lastFoliageExtractStats();
		CHECK(st.thinnedInstances == want.thinned);
		CHECK(st.levelInstances[0] == want.byMesh.at(HE::kDefaultCubeMeshId).size());
		CHECK(st.levelInstances[1] == want.byMesh.at(HE::kDefaultQuadMeshId).size());
		CHECK(st.levelInstances[2] == want.byMesh.at(HE::kDefaultSnowflakeMeshId).size());
		CHECK(st.heldInstances == 0u);   // a first frame has nothing to hold
		if (m == HE::FoliageMode::Clusters)
		{
			CHECK(st.straddlingBuckets > 0u);
			CHECK(st.clusters > st.straddlingBuckets);   // whole buckets are one cluster each, no copy
		}
	}
}

TEST_CASE("Foliage levels: a level without a mesh thins the layer's own, evenly")
{
	// From 20 m on, three plants in ten.
	Field f({ lod(20.0f, {}, 0.3f) });
	const glm::vec3 cam(0.0f, 2.0f, 0.0f);
	const Expected want = expected(f, cam);
	RenderExtractor ex;
	const ByMesh got = drawn(extractWith(ex, f, cam));
	REQUIRE(got.size() == 1u);
	CHECK(got.at(HE::kDefaultCubeMeshId) == want.byMesh.at(HE::kDefaultCubeMeshId));

	// The share actually kept beyond 20 m is the one asked for: a bucket prefix of a random
	// scatter is an even thinning, not a corner of the bucket.
	const HE::FoliageExtractStats& st = HE::lastFoliageExtractStats();
	const double far = static_cast<double>(st.levelInstances[1]) / static_cast<double>(st.levelInstances[1] + st.thinnedInstances);
	CHECK(far == doctest::Approx(0.3).epsilon(0.15));
	// And they are spread: each quadrant of the ring beyond 20 m keeps roughly its share.
	int quadrant[4] = {};
	for (const glm::mat4& m : got.at(HE::kDefaultCubeMeshId))
	{
		if (std::hypot(m[3].x - cam.x, m[3].z - cam.z) < 20.0f) continue;
		++quadrant[(m[3].x >= 0.0f ? 1 : 0) + (m[3].z >= 0.0f ? 2 : 0)];
	}
	const int sum = quadrant[0] + quadrant[1] + quadrant[2] + quadrant[3];
	for (int q : quadrant) CHECK(q == doctest::Approx(sum / 4.0).epsilon(0.35));
}

TEST_CASE("Foliage levels: a bucket keeps the same thinned plants whether it is drawn whole or split")
{
	// One bucket's plants at two camera positions: first the bucket lies wholly in the
	// thinned level, then the draw distance cuts through it (plant-by-plant path). The
	// plants drawn the second time are exactly those of the first that are still in range.
	Field f({ lod(5.0f, {}, 0.4f) }, 0.0f);
	const FoliageStore& s = *f.layer().store;
	// A bucket with plenty of plants, away from the terrain's edge.
	const FoliageBucket* bucket = nullptr;
	for (const FoliageBucket& b : s.buckets)
		if (b.count > 30 && b.localBounds.min.x > -40.0f && b.localBounds.max.x < 40.0f
		    && b.localBounds.min.z > -40.0f && b.localBounds.max.z < 40.0f) { bucket = &b; break; }
	REQUIRE(bucket);
	const glm::vec3 centre = (bucket->localBounds.min + bucket->localBounds.max) * 0.5f;
	auto plantsOfBucket = [&](const ByMesh& m)
	{
		std::vector<glm::mat4> v;
		for (const glm::mat4& x : m.at(HE::kDefaultCubeMeshId))
			if (x[3].x >= bucket->localBounds.min.x && x[3].x <= bucket->localBounds.max.x
			    && x[3].z >= bucket->localBounds.min.z && x[3].z <= bucket->localBounds.max.z)
				v.push_back(x);
		return v;
	};

	// 40 m beside the bucket: all of it within 80 m and beyond 5 m + the margin.
	const glm::vec3 camWhole = centre + glm::vec3(40.0f, 2.0f, 0.0f);
	RenderExtractor ex;
	const std::vector<glm::mat4> whole = plantsOfBucket(drawn(extractWith(ex, f, camWhole)));
	REQUIRE(!whole.empty());
	CHECK(whole.size() == static_cast<size_t>(0.4 * bucket->count + 0.5));

	// Then the draw distance through the bucket, at several depths: from cutting off its
	// far strip to leaving only its near one.
	const float dd2 = f.layer().drawDistance * f.layer().drawDistance;
	for (const float offset : { 66.0f, 72.0f, 80.0f, 88.0f })
	{
		CAPTURE(offset);
		const glm::vec3 camSplit = centre + glm::vec3(offset, 2.0f, 0.0f);
		const std::vector<glm::mat4> split = plantsOfBucket(drawn(extractWith(ex, f, camSplit)));
		CHECK(HE::lastFoliageExtractStats().straddlingBuckets > 0u);
		CHECK(split.size() < whole.size());
		std::vector<glm::mat4> wholeInRange;
		for (const glm::mat4& x : whole)
		{
			const float dx = x[3].x - camSplit.x, dz = x[3].z - camSplit.z;
			if (dx * dx + dz * dz <= dd2) wholeInRange.push_back(x);
		}
		CHECK(split == wholeInRange);
	}
}

// ─── Hysteresis ─────────────────────────────────────────────────────────────

TEST_CASE("Foliage levels: a plant near a boundary keeps its level until it is the hysteresis past it")
{
	const std::vector<HE::UUID> meshOfLevel = { HE::kDefaultCubeMeshId, HE::kDefaultQuadMeshId };
	const float boundary = 30.0f, hysteresis = 5.0f;
	Field f({ lod(boundary, HE::kDefaultQuadMeshId) }, hysteresis);

	RenderExtractor ex;
	const glm::vec3 cam0(0.0f, 2.0f, 0.0f);
	const auto before = levelsOf(drawn(extractWith(ex, f, cam0)), meshOfLevel);
	CHECK(before == expected(f, cam0).level);   // the first frame: by distance alone

	// Three metres along x: plants on both sides of the boundary move by up to 3 m,
	// so every one that crossed it is still within the 5 m band and keeps its level.
	const glm::vec3 cam1(3.0f, 2.0f, 0.0f);
	const auto after = levelsOf(drawn(extractWith(ex, f, cam1)), meshOfLevel);
	const auto plain = expected(f, cam1).level;
	REQUIRE(after.size() == plain.size());
	uint64_t held = 0;
	for (const auto& [xz, level] : after)
	{
		const float d = std::hypot(xz.first - cam1.x, xz.second - cam1.z);
		const auto was = before.find(xz);
		int want = plain.at(xz);
		if (was != before.end() && was->second != want && std::abs(d - boundary) <= hysteresis)
		{
			want = was->second;
			++held;
		}
		CHECK(level == want);
	}
	CHECK(held > 5u);
	CHECK(HE::lastFoliageExtractStats().heldInstances == held);

	// Another view looking from the same spot has no history: it chooses by distance alone.
	RenderExtractor other;
	CHECK(levelsOf(drawn(extractWith(other, f, cam1)), meshOfLevel) == plain);
	CHECK(HE::lastFoliageExtractStats().heldInstances == 0u);

	// Far past the band the levels are by distance again, history or not.
	const glm::vec3 cam2(12.0f, 2.0f, 0.0f);
	const auto moved = levelsOf(drawn(extractWith(ex, f, cam2)), meshOfLevel);
	const auto plain2 = expected(f, cam2).level;
	for (const auto& [xz, level] : moved)
	{
		const float d = std::hypot(xz.first - cam2.x, xz.second - cam2.z);
		if (std::abs(d - boundary) > hysteresis) CHECK(level == plain2.at(xz));
	}
}

TEST_CASE("Foliage levels: the level memory follows the stores, so a re-scatter every frame cannot grow it")
{
	Field f({ lod(30.0f, HE::kDefaultQuadMeshId) });
	HE::FoliageLodMemory mem;
	auto extractOnce = [&]
	{
		RenderWorld rw;
		rw.camera.position = glm::vec3(0.0f, 2.0f, 0.0f);
		HE::extractFoliage(f.world.registry(), rw, &f.cm, &mem);
	};
	extractOnce();
	REQUIRE(mem.layers.size() == 1u);
	CHECK(mem.layers.begin()->first == f.layer().store.get());

	for (int i = 0; i < 5; ++i)   // what a held sculpt brush does: a new store every frame
	{
		f.layer().dirty = true;
		FoliageSystem::update(f.world);
		extractOnce();
		REQUIRE(mem.layers.size() == 1u);
		CHECK(mem.layers.begin()->first == f.layer().store.get());
	}

	f.layer().visible = false;     // a hidden layer is not extracted, and forgotten
	extractOnce();
	CHECK(mem.layers.empty());

	f.layer().visible = true;      // a layer without levels needs no memory at all
	f.layer().lods.clear();
	extractOnce();
	CHECK(mem.layers.empty());
}

// ─── Bounds ──────────────────────────────────────────────────────────────────

TEST_CASE("Foliage levels: each level's cluster box holds that level's mesh at every plant")
{
	// The snowflake (radius ~0.5 in XY) and the quad stand differently from the cube; a box
	// computed from the layer's mesh alone would cut them.
	Field f({ lod(25.0f, HE::kDefaultQuadMeshId), lod(50.0f, HE::kDefaultSnowflakeMeshId) });
	RenderExtractor ex;
	const RenderWorld rw = extractWith(ex, f, glm::vec3(5.0f, 3.0f, 5.0f));
	std::map<HE::UUID, HE::AABB, UuidLess> boxes;
	size_t checked = 0;
	for (const RenderObject& o : rw.objects)
	{
		if (!o.isCluster()) continue;
		if (!boxes.count(o.meshAssetId)) boxes[o.meshAssetId] = meshBox(f.cm, o.meshAssetId);
		REQUIRE(o.worldBounds.isValid());
		const InstanceBlock& b = rw.instanceBlocks[static_cast<size_t>(o.instanceBlock)];
		for (uint32_t i = 0; i < b.count; ++i)
		{
			const HE::AABB w = boxes[o.meshAssetId].transformed(b.world(i));
			const float eps = 1e-3f;
			CHECK(w.min.x >= o.worldBounds.min.x - eps);
			CHECK(w.min.y >= o.worldBounds.min.y - eps);
			CHECK(w.min.z >= o.worldBounds.min.z - eps);
			CHECK(w.max.x <= o.worldBounds.max.x + eps);
			CHECK(w.max.y <= o.worldBounds.max.y + eps);
			CHECK(w.max.z <= o.worldBounds.max.z + eps);
			++checked;
		}
	}
	CHECK(boxes.size() == 3u);
	CHECK(checked > 500u);
}

// ─── Settings, file, export ─────────────────────────────────────────────────

TEST_CASE("Foliage levels: a change to a level bumps the revision, without a re-scatter")
{
	Field f({ lod(30.0f, HE::kDefaultQuadMeshId) });
	auto& comp = f.world.registry().get<FoliageComponent>(f.terrain);
	const auto store = f.layer().store;
	const uint32_t r0 = comp.revision;
	f.layer().lods[0].distance = 35.0f;
	FoliageSystem::update(f.world);
	CHECK(comp.revision == r0 + 1);
	f.layer().lods[0].density = 0.5f;
	FoliageSystem::update(f.world);
	CHECK(comp.revision == r0 + 2);
	f.layer().lods.push_back(lod(60.0f));
	FoliageSystem::update(f.world);
	CHECK(comp.revision == r0 + 3);
	f.layer().lodHysteresis = 4.0f;
	FoliageSystem::update(f.world);
	CHECK(comp.revision == r0 + 4);
	CHECK(f.layer().store == store);   // the same scatter all along
}

TEST_CASE("Foliage levels: they save and load in both forms, sorted and clamped, and a layer without them writes no key")
{
	auto foliageJson = [](HorizonWorld& world, Entity e)
	{
		const json comps = json::from_cbor(SceneSerializer().serializeEntityComponents(world, e));
		REQUIRE(comps.contains("foliage"));
		return comps["foliage"];
	};
	auto load = [](const json& foliage)
	{
		HorizonWorld world;
		const Entity e = world.createEntity("Land");
		REQUIRE(SceneSerializer().applyEntityComponents(world, e, json::to_cbor(json{ { "foliage", foliage } })));
		return world.registry().get<FoliageComponent>(e);
	};

	HorizonWorld world;
	const Entity e = world.createEntity("Land");
	FoliageComponent fol;
	fol.layers[0].meshAssetId = HE::kDefaultCubeMeshId;
	world.registry().emplace<FoliageComponent>(e, fol);
	const json plain = foliageJson(world, e);
	CHECK_FALSE(plain.contains("lods"));
	CHECK_FALSE(plain.contains("lodHysteresis"));

	// One layer: the flat form carries them.
	FoliageLod a = lod(40.0f, HE::kEngineCrossCardMeshId, 0.25f);
	a.materialAssetId = HE::UUID{ 7u, 9u };
	auto& c = world.registry().get<FoliageComponent>(e);
	c.layers[0].lods = { lod(20.0f), a };
	c.layers[0].lodHysteresis = 3.5f;
	const json flat = foliageJson(world, e);
	CHECK_FALSE(flat.contains("layers"));
	REQUIRE(flat.contains("lods"));
	const FoliageComponent back = load(flat);
	REQUIRE(back.layers[0].lods.size() == 2u);
	CHECK(back.layers[0].lods[1].meshAssetId == HE::kEngineCrossCardMeshId);
	CHECK(back.layers[0].lods[1].materialAssetId == (HE::UUID{ 7u, 9u }));
	CHECK(back.layers[0].lods[1].distance == doctest::Approx(40.0f));
	CHECK(back.layers[0].lods[1].density == doctest::Approx(0.25f));
	CHECK(back.layers[0].lods[0].meshAssetId == HE::UUID{});
	CHECK(back.layers[0].lodHysteresis == doctest::Approx(3.5f));

	// Two layers: inside the "layers" array, each layer its own.
	c.addLayer();
	c.layers[1].meshAssetId = HE::kDefaultQuadMeshId;
	c.layers[1].lods = { lod(60.0f, HE::kDefaultSnowflakeMeshId) };
	const json two = foliageJson(world, e);
	REQUIRE(two.contains("layers"));
	const FoliageComponent back2 = load(two);
	REQUIRE(back2.layers.size() == 2u);
	CHECK(back2.layers[0].lods.size() == 2u);
	REQUIRE(back2.layers[1].lods.size() == 1u);
	CHECK(back2.layers[1].lods[0].meshAssetId == HE::kDefaultSnowflakeMeshId);

	// A hand-edited file: out of order, a density out of range, a negative distance,
	// more levels than a layer holds.
	json hand = flat;
	hand["lods"] = json::array({ json{ { "distance", -5.0 } } });
	for (int i = 0; i < 10; ++i)
		hand["lods"].push_back({ { "distance", 100.0 - i * 10.0 }, { "density", i == 0 ? 3.0 : -1.0 } });
	const FoliageComponent odd = load(hand);
	REQUIRE(odd.layers[0].lods.size() == FoliageLayer::kMaxLods);
	for (size_t i = 1; i < odd.layers[0].lods.size(); ++i)
		CHECK(odd.layers[0].lods[i - 1].distance <= odd.layers[0].lods[i].distance);
	for (const FoliageLod& d : odd.layers[0].lods)
	{
		CHECK(d.density >= 0.0f);
		CHECK(d.density <= 1.0f);
		CHECK(d.distance >= 0.0f);
	}
	CHECK(odd.layers[0].lods.front().distance == doctest::Approx(0.0f));   // from -5
	CHECK(odd.layers[0].lods.back().distance == doctest::Approx(100.0f));
	CHECK(odd.layers[0].lods.back().density == doctest::Approx(1.0f));    // clamped from 3
}

TEST_CASE("Foliage levels: an export ships every level's mesh and material")
{
	HorizonWorld world;
	const Entity e = world.createEntity("Land");
	FoliageComponent fol;
	fol.layers[0].meshAssetId = HE::kDefaultCubeMeshId;
	FoliageLod far = lod(50.0f, HE::kEngineCrossCardMeshId);
	far.materialAssetId = HE::UUID{ 0x4000ull, 77u };
	fol.layers[0].lods = { far };
	world.registry().emplace<FoliageComponent>(e, fol);
	const std::vector<HE::UUID> refs = SceneSystems::collectAssetRefs(world);
	CHECK(std::count(refs.begin(), refs.end(), HE::kEngineCrossCardMeshId) == 1);
	CHECK(std::count(refs.begin(), refs.end(), far.materialAssetId) == 1);
}

#ifdef HE_EDITOR_DEPS_DIR
TEST_CASE("Foliage levels: the cross card is a committed engine mesh with the UUID the code names")
{
	namespace fs = std::filesystem;
	const fs::path content = fs::temp_directory_path() / "he_test_foliage_lod_content";
	fs::create_directories(content);
	ContentManager cm(content.string());
	cm.setEngineContentRoot((fs::path(HE_EDITOR_DEPS_DIR) / "EngineContent").string());
	const HE::UUID id = cm.loadAsset("Engine/Meshes/CrossCard.hasset");
	REQUIRE(id == HE::kEngineCrossCardMeshId);
	const StaticMeshAsset* m = cm.getStaticMesh(id);
	REQUIRE(m);
	REQUIRE(m->vertices.size() == 16u * 3u);
	REQUIRE(m->normals.size() == m->vertices.size());
	REQUIRE(m->uvs.size() == 16u * 2u);
	REQUIRE(m->indices.size() == 8u * 3u);

	// Pivot at the base, one metre across and up.
	const HE::AABB box = HE::AABB::fromPositions(m->vertices.data(), 16);
	CHECK(box.min.x == doctest::Approx(-0.5f)); CHECK(box.max.x == doctest::Approx(0.5f));
	CHECK(box.min.y == doctest::Approx(0.0f));  CHECK(box.max.y == doctest::Approx(1.0f));
	CHECK(box.min.z == doctest::Approx(-0.5f)); CHECK(box.max.z == doctest::Approx(0.5f));

	// Two upright planes, both faces each: every triangle winds along its normal, the
	// normals are horizontal, and each of the four directions is faced by two triangles.
	int facing[4] = {};   // +X, -X, +Z, -Z
	for (size_t t = 0; t < m->indices.size(); t += 3)
	{
		const auto p = [&](uint32_t i) { return glm::vec3(m->vertices[i * 3], m->vertices[i * 3 + 1], m->vertices[i * 3 + 2]); };
		const auto n = [&](uint32_t i) { return glm::vec3(m->normals[i * 3], m->normals[i * 3 + 1], m->normals[i * 3 + 2]); };
		const uint32_t a = m->indices[t], b = m->indices[t + 1], c = m->indices[t + 2];
		const glm::vec3 g = glm::cross(p(b) - p(a), p(c) - p(a));
		const glm::vec3 nn = n(a);
		CHECK(glm::dot(g, nn) > 0.0f);
		CHECK(nn.y == doctest::Approx(0.0f));
		if (nn.x > 0.5f) ++facing[0];
		else if (nn.x < -0.5f) ++facing[1];
		else if (nn.z > 0.5f) ++facing[2];
		else if (nn.z < -0.5f) ++facing[3];
	}
	for (int k = 0; k < 4; ++k) CHECK(facing[k] == 2);
	// The whole picture on every face.
	for (size_t i = 0; i < m->uvs.size(); ++i)
		CHECK((m->uvs[i] == 0.0f || m->uvs[i] == 1.0f));
}
#endif
