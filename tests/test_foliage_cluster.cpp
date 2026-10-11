// Foliage as clusters (Thema 163, Schritt 3 / Teil 2a).
//
// A foliage layer reaches the renderer as ONE RenderObject per bucket instead of one per
// plant (FoliageExtract.cpp), and only GeometryPass / RenderSorter::batchDepthRuns turn a
// bucket back into instances. What these tests pin down, in the order of the plan's
// acceptance list:
//   * the store FoliageSystem builds is the scatter, sorted and terrain-local, and the
//     scatter itself is still bit for bit what it always was;
//   * "cluster unfolded == the same single objects": for every pose of the terrain, every
//     draw distance and both consumers, the instances that reach the draw are exactly the
//     ones the per-instance path emitted, which are exactly the cached instances in range;
//   * a cluster's box covers every plant of its bucket and survives the backends' refine;
//   * the instance cap, the counters, the revision, the flags, and the cost scaling with
//     buckets rather than instances.
#include "doctest.h"
#include "../src/HE_Rendering/src/FoliageExtract.h"
#include <Renderer/IRenderer.h>
#include <HorizonRendering/CommandBuffer.h>
#include <HorizonRendering/FrustumCuller.h>
#include <HorizonRendering/OcclusionCuller.h>
#include <HorizonRendering/RenderConstants.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderPass.h>
#include <HorizonRendering/RenderSorter.h>
#include <HorizonRendering/RenderWorld.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/FoliageSystem.h>
#include <HorizonScene/TerrainMeshGenerator.h>
#include <HorizonScene/Components/FoliageComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/DefaultAssets.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	namespace fs = std::filesystem;

	// The cluster switch is process-wide; a test that flips it puts it back.
	struct ClusterMode
	{
		explicit ClusterMode(HE::FoliageMode m) { HE::setFoliageModeOverride(static_cast<int>(m)); }
		explicit ClusterMode(bool on)
			: ClusterMode(on ? HE::FoliageMode::Clusters : HE::FoliageMode::PerInstance) {}
		~ClusterMode() { HE::setFoliageModeOverride(-1); }
	};

	// A flat or hilly terrain with one foliage layer of the default cube, scattered.
	struct Field
	{
		HorizonWorld  world;
		ContentManager cm;
		entt::entity  terrain = entt::null;
		entt::entity  parent  = entt::null;

		Field(float size, float density, float drawDistance, glm::vec3 pos = glm::vec3(0.0f),
		      glm::vec3 rotDeg = glm::vec3(0.0f), glm::vec3 scale = glm::vec3(1.0f),
		      float heightScale = 0.0f, bool parented = false)
		{
			auto& reg = world.registry();
			terrain = world.createEntity("Terrain");
			TransformComponent tf;
			tf.position = pos; tf.rotation = rotDeg; tf.scale = scale;
			reg.emplace_or_replace<TransformComponent>(terrain, tf);
			if (parented)
			{
				parent = world.createEntity("Parent");
				TransformComponent ptf;
				ptf.position = glm::vec3(100.0f, -3.0f, 40.0f);
				ptf.rotation = glm::vec3(0.0f, 70.0f, 0.0f);
				ptf.scale    = glm::vec3(2.0f);
				reg.emplace_or_replace<TransformComponent>(parent, ptf);
				REQUIRE(world.reparentEntity(terrain, parent));
			}
			TerrainComponent tc;
			tc.sizeX = tc.sizeZ = size;
			tc.heightScale = heightScale;
			tc.seed = heightScale > 0.0f ? 5 : 0;
			reg.emplace<TerrainComponent>(terrain, tc);
			FoliageComponent fol;
			fol.meshAssetId  = HE::kDefaultCubeMeshId;
			fol.density      = density;
			fol.seed         = 11;
			fol.minScale     = 0.5f;
			fol.maxScale     = 1.5f;
			fol.drawDistance = drawDistance;
			reg.emplace<FoliageComponent>(terrain, fol);
			FoliageSystem::update(world);
		}
		FoliageComponent& fol() { return world.registry().get<FoliageComponent>(terrain); }
		const glm::mat4& terrainWorld() { return world.registry().get<TransformComponent>(terrain).worldMatrix; }
	};

	EditorCameraOverride camAt(const glm::vec3& pos, const glm::vec3& target = glm::vec3(0.0f))
	{
		EditorCameraOverride c;
		c.active       = true;
		c.position     = pos;
		c.view         = glm::lookAt(pos, target, glm::vec3(0.0f, 1.0f, 0.0f));
		c.editorIcons  = false;
		return c;
	}

	RenderWorld extractField(Field& f, const EditorCameraOverride& cam, HE::FoliageMode how, bool withContent = true)
	{
		ClusterMode mode(how);
		RenderExtractor ex;
		if (withContent) ex.setContentManager(&f.cm);
		RenderWorld rw;
		ex.extract(f.world, rw, 16.0f / 9.0f, &cam);
		return rw;
	}
	RenderWorld extractField(Field& f, const EditorCameraOverride& cam, bool clusters, bool withContent = true)
	{
		return extractField(f, cam, clusters ? HE::FoliageMode::Clusters : HE::FoliageMode::PerInstance, withContent);
	}

	bool matLess(const glm::mat4& a, const glm::mat4& b)
	{
		return std::lexicographical_compare(glm::value_ptr(a), glm::value_ptr(a) + 16,
		                                    glm::value_ptr(b), glm::value_ptr(b) + 16);
	}
	void sorted(std::vector<glm::mat4>& v) { std::sort(v.begin(), v.end(), matLess); }

	// Every instance GeometryPass would draw for `rw`, the objects taken in extractor order
	// (no cull, no sort: this is about the unfolding, not about which objects survive).
	std::vector<glm::mat4> drawnInstances(const RenderWorld& rw)
	{
		std::vector<uint32_t> idx(rw.objects.size());
		std::iota(idx.begin(), idx.end(), 0u);
		CommandBuffer cmds;
		GeometryPass{}.execute(rw, idx, cmds);
		std::vector<glm::mat4> out;
		for (const DrawCall& dc : cmds.drawCalls())
		{
			if (dc.instanceTransforms.empty())
			{
				CHECK(dc.instanceCount == 1u);
				out.push_back(dc.transform);
			}
			else
			{
				CHECK(dc.instanceCount == dc.instanceTransforms.size());
				CHECK(dc.instanceTransforms.size() <= HE::kMaxInstancesPerDraw);
				out.insert(out.end(), dc.instanceTransforms.begin(), dc.instanceTransforms.end());
			}
		}
		sorted(out);
		return out;
	}

	// Every transform the shadow pass's depth batching would draw.
	std::vector<glm::mat4> depthInstances(const RenderWorld& rw, RenderSorter::DepthFilter filter)
	{
		std::vector<uint32_t> idx(rw.objects.size());
		std::iota(idx.begin(), idx.end(), 0u);
		RenderSorter::DepthBatchList list;
		RenderSorter::batchDepthRuns(rw, idx, filter, kNoOwnerEntity, list);
		uint32_t next = 0;
		for (const RenderSorter::DepthBatch& b : list.batches)
		{
			CHECK(b.first == next);                       // batches tile the transform array
			CHECK(b.count >= 1u);
			CHECK(b.count <= HE::kMaxInstancesPerDraw);
			next += b.count;
		}
		CHECK(next == list.transforms.size());
		std::vector<glm::mat4> out = list.transforms;
		sorted(out);
		return out;
	}

	// What the per-instance path always submitted: the cached (world-space) instances whose
	// ground-plane distance to the camera is within the layer's draw distance.
	std::vector<glm::mat4> cachedInRange(const FoliageComponent& fol, const glm::vec3& cam)
	{
		const float dd2 = fol.drawDistance * fol.drawDistance;
		std::vector<glm::mat4> v;
		for (const glm::mat4& m : fol.cachedInstances)
		{
			const float dx = m[3].x - cam.x, dz = m[3].z - cam.z;
			if (dx * dx + dz * dz <= dd2) v.push_back(m);
		}
		sorted(v);
		return v;
	}

	// The same for a terrain with any pose: the store's local matrices under the terrain's
	// world matrix, which is what the cluster path defines an instance's world pose as.
	std::vector<glm::mat4> storeInRange(Field& f, const glm::vec3& cam)
	{
		const FoliageComponent& fol = f.fol();
		const glm::mat4 W = f.terrainWorld();
		const float dd2 = fol.drawDistance * fol.drawDistance;
		std::vector<glm::mat4> v;
		for (const glm::mat4& l : fol.store->local)
		{
			const glm::mat4 w = W * l;
			const float dx = w[3].x - cam.x, dz = w[3].z - cam.z;
			if (dx * dx + dz * dz <= dd2) v.push_back(w);
		}
		sorted(v);
		return v;
	}

	size_t clusterCount(const RenderWorld& rw)
	{
		return static_cast<size_t>(std::count_if(rw.objects.begin(), rw.objects.end(),
			[](const RenderObject& o) { return o.isCluster(); }));
	}
}

// ─── The store ────────────────────────────────────────────────────────────────

TEST_CASE("FoliageSystem: the store is the scatter, sorted into buckets and terrain-local")
{
	// 100 m square at an offset, hilly: 4 x 4 buckets of 32 m, instances at varied heights.
	Field f(100.0f, 0.25f, 1.0e6f, glm::vec3(10.0f, 5.0f, -20.0f), glm::vec3(0.0f), glm::vec3(1.0f), 6.0f);
	const FoliageComponent& fol = f.fol();
	REQUIRE(fol.store);
	const FoliageStore& st = *fol.store;
	REQUIRE(fol.cachedInstances.size() == 2500u);
	CHECK(st.local.size() == fol.cachedInstances.size());
	CHECK(st.gridX == 4);
	CHECK(st.gridZ == 4);
	CHECK(st.buckets.size() > 1u);

	// Terrain-local + the terrain's position == the world instance, bit for bit, and nothing
	// is lost or invented: the same multiset.
	const glm::vec3 origin(10.0f, 5.0f, -20.0f);
	std::vector<glm::mat4> fromStore;
	for (const glm::mat4& l : st.local)
	{
		glm::mat4 w = l;
		w[3] = glm::vec4(glm::vec3(l[3]) + origin, 1.0f);
		fromStore.push_back(w);
	}
	std::vector<glm::mat4> cached = fol.cachedInstances;
	sorted(fromStore);
	sorted(cached);
	CHECK(fromStore == cached);

	// The buckets tile the array, every instance sits in its own cell, the bounds hold
	// the origins, and inside a bucket the scatter's order survived (a prefix is a thinning).
	std::map<std::pair<float, float>, size_t> generation;   // (x, z) in scatter order -> index
	for (size_t i = 0; i < fol.cachedInstances.size(); ++i)
		generation[{ fol.cachedInstances[i][3].x, fol.cachedInstances[i][3].z }] = i;
	uint32_t next = 0;
	for (const FoliageBucket& b : st.buckets)
	{
		CHECK(b.first == next);
		CHECK(b.count > 0u);
		next += b.count;
		size_t prev = 0; bool havePrev = false;
		int cellX = -1, cellZ = -1;
		for (uint32_t k = b.first; k < b.first + b.count; ++k)
		{
			const glm::vec3 p(st.local[k][3]);
			CHECK(p.x >= b.localBounds.min.x);
			CHECK(p.x <= b.localBounds.max.x);
			CHECK(p.z >= b.localBounds.min.z);
			CHECK(p.z <= b.localBounds.max.z);
			const int cx = std::clamp(static_cast<int>((p.x + 50.0f) / 32.0f), 0, 3);
			const int cz = std::clamp(static_cast<int>((p.z + 50.0f) / 32.0f), 0, 3);
			if (cellX < 0) { cellX = cx; cellZ = cz; }
			CHECK(cx == cellX);
			CHECK(cz == cellZ);
			const size_t g = generation.at({ p.x + origin.x, p.z + origin.z });
			if (havePrev) CHECK(g > prev);
			prev = g; havePrev = true;
		}
	}
	CHECK(next == st.local.size());
}

namespace
{
	// The scatter as it was written before the store existed, copied here so a change to
	// the real one cannot quietly move every plant in every saved scene.
	uint32_t refWang(uint32_t n)
	{
		n = (n ^ 61u) ^ (n >> 16u); n += n << 3u; n ^= n >> 4u; n *= 0x27D4EB2Du; n ^= n >> 15u;
		return n;
	}
	float refRand(uint32_t& s) { s = refWang(s); return static_cast<float>(s & 0x00FFFFFFu) / static_cast<float>(0x01000000u); }
}

TEST_CASE("FoliageSystem: the cached layout is the one the scatter always produced")
{
	const glm::vec3 origin(37.5f, -4.25f, 120.125f);
	Field f(80.0f, 0.2f, 1.0e6f, origin, glm::vec3(0.0f), glm::vec3(1.0f), 5.0f);
	const FoliageComponent& fol = f.fol();
	const TerrainComponent& tc  = f.world.registry().get<TerrainComponent>(f.terrain);

	std::vector<glm::mat4> expected;
	const int   count = static_cast<int>(tc.sizeX * tc.sizeZ * fol.density);
	uint32_t    rng   = static_cast<uint32_t>(fol.seed) ^ 0xABCD1234u;
	for (int i = 0; i < count; ++i)
	{
		const float lx = refRand(rng) * tc.sizeX - tc.sizeX * 0.5f;
		const float lz = refRand(rng) * tc.sizeZ - tc.sizeZ * 0.5f;
		const float rotY  = refRand(rng) * 6.2831853f;
		const float scale = fol.minScale + refRand(rng) * (fol.maxScale - fol.minScale);
		const float ly = terrainHeightAt(tc, lx, lz);
		const glm::vec3 worldPos = origin + glm::vec3(lx, ly, lz);
		glm::mat4 m = glm::translate(glm::mat4(1.f), worldPos);
		m = glm::rotate(m, rotY, glm::vec3(0.f, 1.f, 0.f));
		m = glm::scale(m, glm::vec3(scale));
		expected.push_back(m);
	}
	REQUIRE(expected.size() == fol.cachedInstances.size());
	// Same random stream, same formulas: the same plants. Bit for bit on Apple clang (arm64); GCC 13 on x86-64
	// (the Linux CI) rounds the sin/cos of the rotation, or the height, differently in the library and in this
	// test's own copy of the loop for about one plant in eight, by an ulp or two. A change to the generator moves
	// plants by whole metres and turns them by whole radians, so a tolerance far above the rounding and far below
	// any real change still pins the layout.
	float worst3 = 0.0f, worstPos = 0.0f;
	for (size_t i = 0; i < expected.size(); ++i)
	{
		for (int c = 0; c < 3; ++c)
			for (int r = 0; r < 4; ++r)
				worst3 = std::max(worst3, std::abs(expected[i][c][r] - fol.cachedInstances[i][c][r]));
		for (int r = 0; r < 4; ++r)
			worstPos = std::max(worstPos, std::abs(expected[i][3][r] - fol.cachedInstances[i][3][r]));
	}
	MESSAGE("largest deviation: orientation/scale ", worst3, ", position ", worstPos);
	CHECK(worst3 <= 1.0e-5f);
	CHECK(worstPos <= 1.0e-4f);
}

TEST_CASE("FoliageComponent::revision counts re-scatters and settings the extraction reads")
{
	Field f(50.0f, 0.1f, 80.0f);
	auto& fol = f.fol();
	const uint32_t r0 = fol.revision;
	CHECK(r0 > 0u);                       // the scatter in the constructor

	FoliageSystem::update(f.world);       // nothing changed
	CHECK(fol.revision == r0);

	fol.drawDistance = 120.0f;            // a setting the extraction reads
	FoliageSystem::update(f.world);
	CHECK(fol.revision > r0);
	const uint32_t r1 = fol.revision;
	FoliageSystem::update(f.world);
	CHECK(fol.revision == r1);

	fol.castsShadow = false;
	FoliageSystem::update(f.world);
	CHECK(fol.revision > r1);
	const uint32_t r2 = fol.revision;

	fol.dirty = true;                     // a re-scatter
	FoliageSystem::update(f.world);
	CHECK(fol.revision > r2);
	CHECK(fol.store);

	// A different bucket grid is a different store: asked for, it is rebuilt.
	const uint32_t r3 = fol.revision;
	fol.bucketSize = 16.0f;
	FoliageSystem::update(f.world);
	CHECK(fol.revision > r3);
	REQUIRE(fol.store);
	CHECK(fol.store->bucketSize == doctest::Approx(16.0f));
	CHECK(fol.store->gridX == 4);         // 50 m / 16 m, rounded up

	// A layer with no mesh has nothing to extract.
	fol.meshAssetId = HE::UUID{};
	fol.dirty = true;
	FoliageSystem::update(f.world);
	CHECK_FALSE(fol.store);
	CHECK(fol.cachedInstances.empty());
}

// ─── Cluster unfolded == the same single objects ──────────────────────────────

TEST_CASE("Foliage clusters unfold into exactly the instances the per-instance path drew")
{
	struct Case { const char* name; float drawDistance; glm::vec3 cam; };
	// Everything in range; a circle that cuts through several buckets; a circle with the
	// camera outside the terrain; one with so small a radius that only a few buckets matter.
	const Case cases[] = {
		{ "all in range",            1.0e6f, glm::vec3(  5.0f, 30.0f,  -8.0f) },
		{ "the circle cuts buckets",    70.0f, glm::vec3( 20.0f, 30.0f, -40.0f) },
		{ "camera off the terrain",    90.0f, glm::vec3(190.0f, 12.0f,  60.0f) },
		{ "a handful of buckets",      25.0f, glm::vec3( 30.0f, 20.0f, -45.0f) },
	};
	for (const Case& c : cases)
	{
		INFO(c.name);
		// 200 m square at an offset, hilly, 0.25/m2 = 10 000 instances.
		Field f(200.0f, 0.25f, c.drawDistance, glm::vec3(30.0f, 12.0f, -45.0f),
		        glm::vec3(0.0f), glm::vec3(1.0f), 4.0f);
		const EditorCameraOverride cam = camAt(c.cam, glm::vec3(30.0f, 12.0f, -45.0f));

		const RenderWorld clustered = extractField(f, cam, true);
		const RenderWorld single    = extractField(f, cam, false);
		const std::vector<glm::mat4> expected = cachedInRange(f.fol(), clustered.camera.position);
		REQUIRE(expected.size() > 0u);

		// The per-instance path: one object per plant in range.
		CHECK(single.objects.size() == expected.size());
		CHECK(clusterCount(single) == 0u);
		// The cluster path: far fewer objects, none of them a lone plant that a cluster covers.
		CHECK(clusterCount(clustered) == clustered.objects.size());
		CHECK(clustered.objects.size() < expected.size());
		CHECK(clustered.instanceBlocks.size() == clustered.objects.size());

		// What reaches the draw is the same set either way, and it is the cached instances in range.
		CHECK(drawnInstances(single)    == expected);
		CHECK(drawnInstances(clustered) == expected);
		// The depth-only batching (shadow layers, SSAO) unfolds the same.
		CHECK(depthInstances(single,    RenderSorter::DepthFilter::ShadowCasters) == expected);
		CHECK(depthInstances(clustered, RenderSorter::DepthFilter::ShadowCasters) == expected);

		const HE::FoliageExtractStats stats = [&] { extractField(f, cam, true); return HE::lastFoliageExtractStats(); }();
		CHECK(stats.clusters == clustered.objects.size());
		CHECK(stats.clusterInstances == expected.size());
		CHECK(stats.totalInstances == f.fol().cachedInstances.size());
		if (c.drawDistance > 1.0e5f) CHECK(stats.straddlingBuckets == 0u);
		if (c.drawDistance < 100.0f) CHECK(stats.straddlingBuckets > 0u);   // the instance-by-instance path ran
	}
}

TEST_CASE("Foliage clusters follow a terrain that is rotated, scaled and parented")
{
	// The instances are stored relative to the terrain; the world pose is taken from the
	// terrain's world matrix at extraction, so the plants stay on a terrain that was
	// scattered where it no longer stands. (The old path baked in the position alone.)
	Field f(120.0f, 0.2f, 85.0f, glm::vec3(-20.0f, 3.0f, 15.0f), glm::vec3(0.0f, 30.0f, 0.0f),
	        glm::vec3(1.5f, 1.0f, 1.5f), 3.0f, /*parented=*/true);
	const EditorCameraOverride cam = camAt(glm::vec3(90.0f, 25.0f, 60.0f), glm::vec3(100.0f, 0.0f, 40.0f));

	const RenderWorld clustered = extractField(f, cam, true);
	const RenderWorld single    = extractField(f, cam, false);
	const std::vector<glm::mat4> expected = storeInRange(f, clustered.camera.position);
	REQUIRE(expected.size() > 0u);
	REQUIRE(expected.size() < f.fol().store->local.size());   // a real subset: the circle matters
	CHECK(drawnInstances(clustered) == expected);
	CHECK(drawnInstances(single)    == expected);
	CHECK(depthInstances(clustered, RenderSorter::DepthFilter::ShadowCasters) == expected);

	// And the world pose really is the parent chain's: the first plant of a cluster,
	// the transform a cluster-unaware reader sees, is parent * local.
	const glm::mat4& W = f.terrainWorld();
	CHECK(W != glm::mat4(1.0f));
	for (const RenderObject& o : clustered.objects)
	{
		const InstanceBlock& b = clustered.instanceBlocks[static_cast<size_t>(o.instanceBlock)];
		REQUIRE(b.valid());
		CHECK(o.transform == b.world(0));
	}

	// Moving the terrain moves the plants, with no re-scatter and no flag.
	f.world.registry().get<TransformComponent>(f.terrain).position += glm::vec3(10.0f, 0.0f, 0.0f);
	const RenderWorld moved = extractField(f, cam, true);
	const std::vector<glm::mat4> expectedMoved = storeInRange(f, moved.camera.position);
	CHECK(drawnInstances(moved) == expectedMoved);
	CHECK(expectedMoved != expected);
}

TEST_CASE("Foliage clusters: a terrain at the origin is the cheap path and still the same plants")
{
	// Identity and pure translation unfold with copies and adds; the numbers must not
	// differ from the cached instances by so much as a bit.
	for (glm::vec3 pos : { glm::vec3(0.0f), glm::vec3(1000.5f, 7.0f, -333.25f) })
	{
		Field f(160.0f, 0.2f, 60.0f, pos);
		const EditorCameraOverride cam = camAt(pos + glm::vec3(12.0f, 20.0f, -9.0f), pos);
		const RenderWorld clustered = extractField(f, cam, true);
		CHECK(drawnInstances(clustered) == cachedInRange(f.fol(), clustered.camera.position));
		for (const InstanceBlock& b : clustered.instanceBlocks)
			CHECK(b.kind != InstanceBlock::Parent::General);
	}
}

TEST_CASE("Ordered mode draws the instances in exactly the order the per-instance path does")
{
	// Opaque geometry that intersects resolves depth ties by draw order, so the default cluster
	// path (buckets, then the scatter's order inside them) and the per-instance path (every plant
	// sorted front to back) can differ in a pixel along a cube-meets-cube line. The verification
	// mode removes the difference at its source: one cluster, in the per-instance path's order.
	// Metal shows the picture bit for bit equal; this pins the order itself, element by element.
	Field f(200.0f, 0.25f, 90.0f, glm::vec3(30.0f, 12.0f, -45.0f), glm::vec3(0.0f), glm::vec3(1.0f), 4.0f);
	const EditorCameraOverride cam = camAt(glm::vec3(20.0f, 30.0f, -40.0f), glm::vec3(30.0f, 12.0f, -45.0f));
	glm::vec3 camPos(0.0f);
	const auto sequence = [&](HE::FoliageMode how)
	{
		const RenderWorld rw = extractField(f, cam, how);
		camPos = rw.camera.position;
		RenderSorter sorter;
		std::vector<uint8_t>  visible(rw.objects.size(), 1u);
		std::vector<uint32_t> idx;
		sorter.sort(rw, visible, idx);
		CommandBuffer cmds;
		GeometryPass{}.execute(rw, idx, cmds);
		std::vector<glm::mat4> seq;
		for (const DrawCall& dc : cmds.drawCalls())
		{
			if (dc.instanceTransforms.empty()) seq.push_back(dc.transform);
			else seq.insert(seq.end(), dc.instanceTransforms.begin(), dc.instanceTransforms.end());
		}
		return seq;
	};
	const std::vector<glm::mat4> perInstance = sequence(HE::FoliageMode::PerInstance);
	const std::vector<glm::mat4> ordered     = sequence(HE::FoliageMode::Ordered);
	REQUIRE(perInstance.size() > 100u);
	REQUIRE(ordered.size() == perInstance.size());

	// The same instances in the same order. Two instances at exactly the same squared distance
	// have no order of their own (std::sort is not stable), so the order is compared by what it
	// is made of: position by position the squared distance is equal, and it never decreases.
	const auto distSq = [&](const glm::mat4& m) { const glm::vec3 d = glm::vec3(m[3]) - camPos; return glm::dot(d, d); };
	size_t mismatches = 0, decreasing = 0;
	for (size_t i = 0; i < perInstance.size(); ++i)
	{
		if (distSq(ordered[i]) != distSq(perInstance[i])) ++mismatches;
		if (i > 0 && distSq(ordered[i]) < distSq(ordered[i - 1])) ++decreasing;
	}
	CHECK(mismatches == 0u);
	CHECK(decreasing == 0u);
	std::vector<glm::mat4> a = ordered, b = perInstance;
	sorted(a); sorted(b);
	CHECK(a == b);                                      // and nothing was added or lost

	// And it is one cluster that culls nothing.
	const RenderWorld rw = extractField(f, cam, HE::FoliageMode::Ordered);
	REQUIRE(rw.objects.size() == 1u);
	CHECK(rw.objects[0].isCluster());
	CHECK_FALSE(rw.objects[0].worldBounds.isValid());
}

// ─── Bounds ───────────────────────────────────────────────────────────────────

TEST_CASE("A cluster's box covers every plant of its bucket, and the backends' refine leaves it alone")
{
	Field f(200.0f, 0.25f, 1.0e6f, glm::vec3(30.0f, 12.0f, -45.0f), glm::vec3(0.0f, 20.0f, 0.0f),
	        glm::vec3(1.0f), 4.0f);
	const EditorCameraOverride cam = camAt(glm::vec3(0.0f, 60.0f, 0.0f));
	RenderWorld rw = extractField(f, cam, true);
	REQUIRE(rw.objects.size() > 4u);

	// The built-in cube registers no box of its own: the extractor reads its vertices.
	HE::AABB cube;
	cube.expand(glm::vec3(-0.5f)); cube.expand(glm::vec3(0.5f));

	for (const RenderObject& o : rw.objects)
	{
		REQUIRE(o.isCluster());
		REQUIRE(o.worldBounds.isValid());
		rw.forEachInstance(o, [&](const glm::mat4& m)
		{
			const HE::AABB plant = cube.transformed(m);
			CHECK(plant.min.x >= o.worldBounds.min.x);
			CHECK(plant.min.y >= o.worldBounds.min.y);
			CHECK(plant.min.z >= o.worldBounds.min.z);
			CHECK(plant.max.x <= o.worldBounds.max.x);
			CHECK(plant.max.y <= o.worldBounds.max.y);
			CHECK(plant.max.z <= o.worldBounds.max.z);
		});
	}

	// The refine every backend runs before culling: a cluster keeps its box (shrinking it to
	// the one plant `transform` names would make the whole bucket vanish), an ordinary object
	// is refined as ever.
	std::vector<HE::AABB> before;
	for (const RenderObject& o : rw.objects) before.push_back(o.worldBounds);
	RenderObject lone;
	lone.transform   = glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 6.0f, 7.0f));
	lone.worldBounds = HE::AABB{};
	rw.objects.push_back(lone);
	for (RenderObject& o : rw.objects) o.refineWorldBounds(cube);
	for (size_t i = 0; i < before.size(); ++i)
	{
		CHECK(rw.objects[i].worldBounds.min == before[i].min);
		CHECK(rw.objects[i].worldBounds.max == before[i].max);
	}
	CHECK(rw.objects.back().worldBounds.min == glm::vec3(4.5f, 5.5f, 6.5f));
	CHECK(rw.objects.back().worldBounds.max == glm::vec3(5.5f, 6.5f, 7.5f));

	// Without a ContentManager the mesh's box is unknown: the cluster is not culled, it is drawn.
	const RenderWorld blind = extractField(f, cam, true, /*withContent=*/false);
	REQUIRE(!blind.objects.empty());
	for (const RenderObject& o : blind.objects) CHECK_FALSE(o.worldBounds.isValid());
}

TEST_CASE("Frustum culling over foliage clusters never loses a plant that is in view")
{
	Field f(200.0f, 0.25f, 1.0e6f, glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), 4.0f);
	// Standing in the middle, looking along +X: everything behind the camera and out to the
	// sides is outside the frustum.
	const EditorCameraOverride cam = camAt(glm::vec3(0.0f, 25.0f, 0.0f), glm::vec3(100.0f, 0.0f, 0.0f));
	const RenderWorld rw = extractField(f, cam, true);

	FrustumCuller culler;
	std::vector<uint8_t> visible;
	culler.cull(rw, visible);
	REQUIRE(visible.size() == rw.objects.size());
	const size_t visibleClusters = static_cast<size_t>(std::count(visible.begin(), visible.end(), uint8_t(1)));
	CHECK(visibleClusters > 0u);
	CHECK(visibleClusters < rw.objects.size());               // the cull actually removed buckets

	// Every plant whose own box touches the frustum belongs to a bucket that survived.
	const Frustum frustum = Frustum::fromViewProj(rw.camera.projection * rw.camera.view);
	HE::AABB cube;
	cube.expand(glm::vec3(-0.5f)); cube.expand(glm::vec3(0.5f));
	size_t inView = 0;
	for (size_t i = 0; i < rw.objects.size(); ++i)
		rw.forEachInstance(rw.objects[i], [&](const glm::mat4& m)
		{
			if (!frustum.intersects(cube.transformed(m))) return;
			++inView;
			CHECK(visible[i] != 0);
		});
	CHECK(inView > 0u);
}

// ─── Cap, depth batches, flags ────────────────────────────────────────────────

namespace
{
	RenderWorld syntheticRun(uint32_t clusterInstances, uint32_t plainObjects)
	{
		RenderWorld rw;
		const HE::UUID mesh{ 7, 7 };
		for (uint32_t i = 0; i < plainObjects; ++i)
		{
			RenderObject o;
			o.meshAssetId = mesh;
			o.transform   = glm::translate(glm::mat4(1.0f), glm::vec3(-float(i) - 1.0f, 0.0f, 0.0f));
			rw.objects.push_back(o);
		}
		auto mats = std::make_shared<std::vector<glm::mat4>>();
		mats->reserve(clusterInstances);
		for (uint32_t i = 0; i < clusterInstances; ++i)
			mats->push_back(glm::translate(glm::mat4(1.0f), glm::vec3(float(i), 1.0f, 2.0f)));
		InstanceBlock b;
		b.matrices = mats;
		b.first    = 0;
		b.count    = clusterInstances;
		rw.instanceBlocks.push_back(b);
		RenderObject c;
		c.meshAssetId   = mesh;
		c.transform     = (*mats)[0];
		c.instanceBlock = 0;
		rw.objects.push_back(c);
		return rw;
	}

	std::vector<size_t> drawSizes(const RenderWorld& rw)
	{
		std::vector<uint32_t> idx(rw.objects.size());
		std::iota(idx.begin(), idx.end(), 0u);
		CommandBuffer cmds;
		GeometryPass{}.execute(rw, idx, cmds);
		std::vector<size_t> sizes;
		for (const DrawCall& dc : cmds.drawCalls())
			sizes.push_back(dc.instanceTransforms.empty() ? 1u : dc.instanceTransforms.size());
		return sizes;
	}
}

TEST_CASE("GeometryPass cuts a run longer than one instanced draw may carry")
{
	const uint32_t cap = HE::kMaxInstancesPerDraw;

	// 150 000 plants of one mesh: three draws that each fit a backend's instance ring,
	// not one draw that every backend answers with a draw per instance.
	{
		const RenderWorld rw = syntheticRun(150000, 0);
		CHECK(drawSizes(rw) == std::vector<size_t>{ cap, cap, 150000u - 2u * cap });
		CHECK(drawnInstances(rw).size() == 150000u);
	}
	// One over the cap: the tail is a SINGLE instance and keeps the contract that a
	// lone instance is a plain draw (empty list, instanceCount 1, its own transform).
	{
		const RenderWorld rw = syntheticRun(cap + 1, 0);
		std::vector<uint32_t> idx(rw.objects.size());
		std::iota(idx.begin(), idx.end(), 0u);
		CommandBuffer cmds;
		GeometryPass{}.execute(rw, idx, cmds);
		REQUIRE(cmds.drawCalls().size() == 2u);
		CHECK(cmds.drawCalls()[0].instanceTransforms.size() == cap);
		CHECK(cmds.drawCalls()[1].instanceTransforms.empty());
		CHECK(cmds.drawCalls()[1].instanceCount == 1u);
		CHECK(cmds.drawCalls()[1].transform == (*rw.instanceBlocks[0].matrices)[cap]);
	}
	// A cluster of exactly the cap, or of one, and ordinary objects beside it, join one run.
	CHECK(drawSizes(syntheticRun(cap, 0)) == std::vector<size_t>{ cap });
	CHECK(drawSizes(syntheticRun(1, 0)) == std::vector<size_t>{ 1u });
	CHECK(drawSizes(syntheticRun(100, 3)) == std::vector<size_t>{ 103u });
	CHECK(drawnInstances(syntheticRun(100, 3)).size() == 103u);
}

TEST_CASE("batchDepthRuns unfolds a cluster and cuts the run at the instance cap")
{
	const uint32_t cap = HE::kMaxInstancesPerDraw;
	const RenderWorld rw = syntheticRun(150000, 4);
	std::vector<uint32_t> idx(rw.objects.size());
	std::iota(idx.begin(), idx.end(), 0u);
	RenderSorter::DepthBatchList list;
	RenderSorter::batchDepthCasters(rw, idx, kNoOwnerEntity, list);
	CHECK(list.transforms.size() == 150004u);
	std::vector<uint32_t> counts;
	for (const auto& b : list.batches) counts.push_back(b.count);
	CHECK(counts == std::vector<uint32_t>{ cap, cap, 150004u - 2u * cap });

	// The opt-outs still apply to a cluster as a whole.
	RenderWorld noShadow = rw;
	noShadow.objects.back().castsShadow = false;
	RenderSorter::batchDepthCasters(noShadow, idx, kNoOwnerEntity, list);
	CHECK(list.transforms.size() == 4u);
	RenderSorter::batchDepthRuns(noShadow, idx, RenderSorter::DepthFilter::All, kNoOwnerEntity, list);
	CHECK(list.transforms.size() == 150004u);
	noShadow.objects.back().contributesAO = false;
	RenderSorter::batchDepthRuns(noShadow, idx, RenderSorter::DepthFilter::AoContributors, kNoOwnerEntity, list);
	CHECK(list.transforms.size() == 4u);
}

TEST_CASE("InstanceBlock::position is exactly world()[3] for every kind of parent")
{
	// The draw-distance test reads the position alone and builds the matrix only for what passes;
	// the two must never round differently.
	auto mats = std::make_shared<std::vector<glm::mat4>>();
	for (int i = 0; i < 50; ++i)
	{
		glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(0.37f * i - 9.0f, 0.113f * i, 7.7f - 0.291f * i));
		m = glm::rotate(m, 0.41f * i, glm::vec3(0.0f, 1.0f, 0.0f));
		mats->push_back(glm::scale(m, glm::vec3(0.5f + 0.02f * i)));
	}
	const glm::mat4 parents[] = {
		glm::mat4(1.0f),
		glm::translate(glm::mat4(1.0f), glm::vec3(1000.5f, -7.25f, 333.125f)),
		glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(12.0f, 3.0f, -4.0f)), 0.7f, glm::vec3(0.0f, 1.0f, 0.0f)) };
	const InstanceBlock::Parent kinds[] = { InstanceBlock::Parent::Identity, InstanceBlock::Parent::Translation,
	                                        InstanceBlock::Parent::General };
	for (int k = 0; k < 3; ++k)
	{
		InstanceBlock b;
		b.matrices = mats;
		b.first    = 5;
		b.count    = 40;
		b.parent   = parents[k];
		b.kind     = InstanceBlock::classify(parents[k]);
		CHECK(b.kind == kinds[k]);
		for (uint32_t i = 0; i < b.count; ++i)
			CHECK(b.position(i) == glm::vec3(b.world(i)[3]));
	}
}

TEST_CASE("A broken block reads as the one plant its transform names")
{
	// A cluster whose block is missing or does not fit its storage must never be read past
	// the end: the readers fall back to the single object (fail-soft, as RenderObject says).
	RenderWorld rw = syntheticRun(10, 0);
	rw.objects[0].instanceBlock = 5;                         // no such block
	CHECK(rw.instanceCountOf(rw.objects[0]) == 1u);
	CHECK(drawnInstances(rw).size() == 1u);
	rw.objects[0].instanceBlock = 0;
	rw.instanceBlocks[0].count = 11;                         // one more than the storage holds
	CHECK_FALSE(rw.instanceBlocks[0].valid());
	CHECK(rw.instanceCountOf(rw.objects[0]) == 1u);
	CHECK(drawnInstances(rw).size() == 1u);
	rw.clear();
	CHECK(rw.instanceBlocks.empty());
}

TEST_CASE("A RenderWorld copy keeps the instances it was extracted from across a re-scatter")
{
	// RenderExtractor::beginFrame keeps a copy of the world for the frame's later extracts,
	// and a persistent world may outlive the scatter it came from: the blocks own the storage.
	Field f(100.0f, 0.2f, 1.0e6f);
	const EditorCameraOverride cam = camAt(glm::vec3(0.0f, 30.0f, 0.0f));
	RenderWorld kept = extractField(f, cam, true);
	const std::vector<glm::mat4> first = drawnInstances(kept);
	REQUIRE(!first.empty());

	f.fol().seed  = 99;               // a different scatter replaces the store
	f.fol().dirty = true;
	FoliageSystem::update(f.world);
	const RenderWorld fresh = extractField(f, cam, true);
	CHECK(drawnInstances(fresh) != first);
	CHECK(drawnInstances(kept) == first);
	const RenderWorld copy = kept;
	CHECK(drawnInstances(copy) == first);
}

TEST_CASE("Foliage flags: casting, ambient occlusion and the shadow distance reach the clusters")
{
	Field f(160.0f, 0.2f, 120.0f);
	const EditorCameraOverride cam = camAt(glm::vec3(0.0f, 30.0f, 0.0f));
	const auto count = [](const RenderWorld& rw, auto pred) {
		return std::count_if(rw.objects.begin(), rw.objects.end(), pred); };

	// Defaults are "as before": every plant casts and occludes.
	RenderWorld rw = extractField(f, cam, true);
	REQUIRE(!rw.objects.empty());
	CHECK(count(rw, [](const RenderObject& o) { return o.castsShadow && o.contributesAO; }) == static_cast<long>(rw.objects.size()));

	// Grass: no ambient occlusion.
	f.fol().contributesAO = false;
	rw = extractField(f, cam, true);
	CHECK(count(rw, [](const RenderObject& o) { return o.contributesAO; }) == 0);
	CHECK(depthInstances(rw, RenderSorter::DepthFilter::AoContributors).empty());
	CHECK_FALSE(depthInstances(rw, RenderSorter::DepthFilter::ShadowCasters).empty());
	f.fol().contributesAO = true;

	// A shadow distance shorter than the draw distance: the near buckets cast, the far ones do not.
	f.fol().shadowDistance = 40.0f;
	rw = extractField(f, cam, true);
	const long casters = count(rw, [](const RenderObject& o) { return o.castsShadow; });
	CHECK(casters > 0);
	CHECK(casters < static_cast<long>(rw.objects.size()));
	for (const RenderObject& o : rw.objects)
	{
		// A bucket casts while any part of it is within reach: the ones that do not hold
		// only plants farther than 40 m from the camera (ground plane).
		if (o.castsShadow) continue;
		rw.forEachInstance(o, [&](const glm::mat4& m)
		{
			const float dx = m[3].x - cam.position.x, dz = m[3].z - cam.position.z;
			CHECK(std::sqrt(dx * dx + dz * dz) > 39.99f);
		});
	}
	f.fol().shadowDistance = 0.0f;

	// Not a caster at all.
	f.fol().castsShadow = false;
	rw = extractField(f, cam, true);
	CHECK(count(rw, [](const RenderObject& o) { return o.castsShadow; }) == 0);
	CHECK(depthInstances(rw, RenderSorter::DepthFilter::ShadowCasters).empty());
}

TEST_CASE("The extraction costs buckets, not plants")
{
	// The same ground and the same view, eight times the plants: the same number of buckets
	// looked at and the same number of clusters. (The per-instance path emitted one object per
	// plant, so its cost, and the memory it wrote, grew with the density.)
	size_t clusters[2] = {}, buckets[2] = {}, objectsLegacy[2] = {};
	uint64_t total[2] = {};
	const float density[2] = { 0.1f, 0.8f };
	for (int i = 0; i < 2; ++i)
	{
		Field f(256.0f, density[i], 1.0e6f);
		const EditorCameraOverride cam = camAt(glm::vec3(0.0f, 40.0f, 0.0f));
		const RenderWorld rw = extractField(f, cam, true);
		clusters[i] = rw.objects.size();
		buckets[i]  = HE::lastFoliageExtractStats().buckets;
		total[i]    = HE::lastFoliageExtractStats().totalInstances;
		objectsLegacy[i] = extractField(f, cam, false).objects.size();
	}
	CHECK(total[1] > 7u * total[0]);
	CHECK(clusters[0] == clusters[1]);
	CHECK(buckets[0] == buckets[1]);
	CHECK(objectsLegacy[1] > 7u * objectsLegacy[0]);
	CHECK(clusters[1] * 100 < objectsLegacy[1]);
}

// ─── Occlusion ────────────────────────────────────────────────────────────────

TEST_CASE("OcclusionCuller: a foliage cluster is never an occluder, but it is occluded as a whole")
{
	// A wall 5 m ahead of a camera at the origin, a box 10 m ahead behind it.
	ContentManager cm;
	const glm::vec3 half(4.0f, 2.0f, 0.1f);
	StaticMeshAsset wallMesh; wallMesh.type = HE::AssetType::StaticMesh; wallMesh.name = "wall";
	const glm::vec3 c[8] = { { -half.x, -half.y, -half.z }, {  half.x, -half.y, -half.z },
	                         {  half.x,  half.y, -half.z }, { -half.x,  half.y, -half.z },
	                         { -half.x, -half.y,  half.z }, {  half.x, -half.y,  half.z },
	                         {  half.x,  half.y,  half.z }, { -half.x,  half.y,  half.z } };
	for (const glm::vec3& p : c) wallMesh.vertices.insert(wallMesh.vertices.end(), { p.x, p.y, p.z });
	wallMesh.indices = { 0,1,2, 0,2,3,  4,6,5, 4,7,6,  0,4,5, 0,5,1,  3,2,6, 3,6,7,  1,5,6, 1,6,2,  0,3,7, 0,7,4 };
	wallMesh.boundsMin[0] = -half.x; wallMesh.boundsMin[1] = -half.y; wallMesh.boundsMin[2] = -half.z;
	wallMesh.boundsMax[0] =  half.x; wallMesh.boundsMax[1] =  half.y; wallMesh.boundsMax[2] =  half.z;
	const HE::UUID mesh = cm.registerStaticMesh(wallMesh);

	const auto build = [&](bool wallIsCluster, bool boxIsCluster)
	{
		RenderWorld rw;
		rw.camera.position   = glm::vec3(0.0f);
		rw.camera.view       = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		rw.camera.projection = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 200.0f);
		auto mats = std::make_shared<std::vector<glm::mat4>>(1, glm::mat4(1.0f));
		InstanceBlock block; block.matrices = mats; block.first = 0; block.count = 1;
		rw.instanceBlocks.push_back(block);
		const auto add = [&](glm::vec3 pos, glm::vec3 h, bool cluster)
		{
			RenderObject o;
			o.meshAssetId     = mesh;
			o.transform       = glm::translate(glm::mat4(1.0f), pos);
			o.worldBounds.min = pos - h;
			o.worldBounds.max = pos + h;
			if (cluster) o.instanceBlock = 0;
			rw.objects.push_back(o);
		};
		add(glm::vec3(0.0f, 0.0f, -5.0f), half, wallIsCluster);
		add(glm::vec3(0.0f, 0.0f, -10.0f), glm::vec3(0.5f), boxIsCluster);
		return rw;
	};
	const auto run = [&](const RenderWorld& rw, uint32_t& occluders)
	{
		OcclusionCuller oc;
		OcclusionCuller::Settings st; st.enabled = true;
		oc.setSettings(st);
		std::vector<uint8_t> vis;
		FrustumCuller().cull(rw, vis);
		oc.refine(rw, &cm, vis);
		occluders = oc.stats().occluders;
		return vis;
	};

	uint32_t occluders = 0;
	// Control: an ordinary wall hides the box behind it.
	auto vis = run(build(false, false), occluders);
	CHECK(occluders == 1u);
	CHECK(vis[0] == 1);
	CHECK(vis[1] == 0);
	// The same wall as a cluster (one plant's mesh, a bucket's box) is not an occluder: nothing hides.
	vis = run(build(true, false), occluders);
	CHECK(occluders == 0u);
	CHECK(vis[0] == 1);
	CHECK(vis[1] == 1);
	// A cluster BEHIND an ordinary wall is occluded: its whole box is hidden, so it goes.
	vis = run(build(false, true), occluders);
	CHECK(occluders == 1u);
	CHECK(vis[1] == 0);
}

// ─── The cascade fit does not see clusters ────────────────────────────────────

TEST_CASE("The shadow cascade fit is the same whether the foliage arrives as clusters or as plants")
{
	// The fit is bounded by the objects' boxes. A foliage object's box was invalid at that point
	// (the backends refine it later), so foliage never moved the cascades; a cluster has a real
	// box and must not start to, or every scene with foliage would shift its shadows.
	Field f(400.0f, 0.05f, 1.0e6f, glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), 8.0f);
	{
		auto& reg = f.world.registry();
		auto sun = f.world.createEntity("sun");
		TransformComponent stf; stf.rotation = glm::vec3(-50.0f, 30.0f, 0.0f);
		reg.emplace_or_replace<TransformComponent>(sun, stf);
		LightComponent l; l.type = HE::LightType::Directional; l.intensity = 2.0f;
		reg.emplace<LightComponent>(sun, l);
		// One ordinary caster WITH a known box (a mesh that carries one), so the fit is bounded
		// by it and by nothing else: far smaller than the foliage a cluster's box would add.
		StaticMeshAsset box; box.type = HE::AssetType::StaticMesh; box.name = "box";
		box.indices = { 0, 1, 2 };
		box.boundsMin[0] = box.boundsMin[1] = box.boundsMin[2] = -1.0f;
		box.boundsMax[0] = box.boundsMax[1] = box.boundsMax[2] =  1.0f;
		const HE::UUID boxId = f.cm.registerStaticMesh(box);
		auto cubeEntity = f.world.createEntity("cube");
		TransformComponent ctf; ctf.position = glm::vec3(2.0f, 1.0f, 2.0f);
		reg.emplace_or_replace<TransformComponent>(cubeEntity, ctf);
		MeshComponent mc; mc.meshAssetId = boxId;
		reg.emplace<MeshComponent>(cubeEntity, mc);
	}
	const EditorCameraOverride cam = camAt(glm::vec3(0.0f, 30.0f, -60.0f), glm::vec3(0.0f));
	const RenderWorld a = extractField(f, cam, true);
	const RenderWorld b = extractField(f, cam, false);
	REQUIRE(a.shadow.enabled);
	REQUIRE(b.shadow.enabled);
	CHECK(a.shadow.viewProj == b.shadow.viewProj);
	REQUIRE(a.shadow.cascadeCount == b.shadow.cascadeCount);
	for (int c = 0; c < a.shadow.cascadeCount; ++c)
		CHECK(a.shadow.cascadeViewProj[c] == b.shadow.cascadeViewProj[c]);
}

// ─── The backends ─────────────────────────────────────────────────────────────

namespace
{
	fs::path findRepoRoot()
	{
		std::error_code ec;
		std::vector<fs::path> seeds;
		fs::path self(__FILE__);
		if (!self.is_absolute()) self = fs::current_path(ec) / self;
		seeds.push_back(self.parent_path());
		seeds.push_back(fs::current_path(ec));
		for (fs::path seed : seeds)
			for (int up = 0; up < 8 && !seed.empty(); ++up, seed = seed.parent_path())
				if (fs::exists(seed / "src" / "HE_Rendering" / "src" / "Backends" / "Metal" / "MetalRenderer.mm", ec))
					return seed;
		return {};
	}

	std::string readFile(const fs::path& p)
	{
		std::ifstream f(p, std::ios::binary);
		std::ostringstream ss;
		ss << f.rdbuf();
		std::string s = ss.str();
		s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
		return s;
	}

	size_t countOf(const std::string& hay, const std::string& needle)
	{
		size_t n = 0;
		for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size())) ++n;
		return n;
	}
}

TEST_CASE("Every backend refreshes bounds through RenderObject::refineWorldBounds and keeps clusters out of GI")
{
	// Five backends rebuild each object's box from the real mesh before they cull, and six
	// GI instance builds walk the objects: the places a cluster (one plant's transform, a
	// whole bucket's box) goes wrong without a guard. D3D11, D3D12 and Vulkan cannot run on
	// every machine, so this reads the sources the way the shader drift guards do: a
	// hand-written refine, or a GI loop that does not skip clusters, fails here on every platform.
	const fs::path root = findRepoRoot();
	if (root.empty())
	{
		MESSAGE("backend sources not found - cluster guard check skipped");
		return;
	}
	const fs::path dir = root / "src" / "HE_Rendering" / "src" / "Backends";
	// Metal and Vulkan refine once per walk (Thema 162, step 4): one helper holds the loop and
	// the five passes that need the bounds call it. The other three still loop in place.
	struct RefineSite
	{
		const char* file;
		size_t      refines;      // .refineWorldBounds( sites
		const char* helper;       // the call the passes make, when there is a helper
		size_t      helperCalls;
	};
	const RefineSite refineSites[] = {
		{ "Metal/MetalRenderer.mm", 1, "RefineObjectBounds();", 5 }, { "OpenGL/OpenGLRenderer.cpp", 2, nullptr, 0 },
		{ "D3D11/D3D11Renderer.cpp", 1, nullptr, 0 }, { "D3D12/D3D12Renderer.cpp", 1, nullptr, 0 },
		{ "Vulkan/VulkanRenderer.cpp", 1, "refineObjectBounds();", 5 } };
	for (const auto& site : refineSites)
	{
		const char* rel     = site.file;
		const size_t atLeast = site.refines;
		INFO(rel);
		const std::string text = readFile(dir / rel);
		REQUIRE(!text.empty());
		CHECK(text.find("localBounds.transformed(") == std::string::npos);   // no hand-written refine
		CHECK(countOf(text, ".refineWorldBounds(") >= atLeast);
		if (site.helper)
		{
			// One loop, the passes go through it: a pass with a loop of its own would refine
			// again on every walk, and one without a call would cull with unrefined bounds.
			CHECK(countOf(text, ".refineWorldBounds(") == atLeast);
			CHECK_MESSAGE(countOf(text, site.helper) >= site.helperCalls, std::string(site.helper));
		}
		// Every GI / caster instance build skips clusters on the line that tests castsShadow.
		std::istringstream lines(text);
		for (std::string line; std::getline(lines, line);)
			if (line.find("!obj.castsShadow") != std::string::npos)
				CHECK_MESSAGE(line.find("isCluster()") != std::string::npos, line);
	}
}

TEST_CASE("OpenGL: a graph material keeps its own program, only built-in PBR takes the instanced one")
{
	// The color passes (G-buffer and forward) used to send EVERY instanced batch to the
	// built-in instanced program, so a foliage layer with a graph material was drawn in flat
	// grey PBR: no wind, no alpha discard, not its colours. The batch must reach the material's
	// program (and draw per instance), exactly as Metal does with cMaterialPipeline == nullptr.
	// A live GL context is not available on every CI machine, so this reads the source: each
	// branch that sends a batch to a built-in instanced program must name the material program
	// on its condition, and the material branches must loop over the instances.
	const fs::path root = findRepoRoot();
	if (root.empty())
	{
		MESSAGE("backend sources not found - GL gate check skipped");
		return;
	}
	const std::string text = readFile(root / "src" / "HE_Rendering" / "src" / "Backends" / "OpenGL" / "OpenGLRenderer.cpp");
	REQUIRE(!text.empty());

	// The forward loop's condition sits on one line, the G-buffer loop's wraps onto the next.
	std::vector<std::string> rows;
	{
		std::istringstream lines(text);
		for (std::string line; std::getline(lines, line);) rows.push_back(line);
	}
	size_t gated = 0;
	for (size_t i = 0; i < rows.size(); ++i)
	{
		if (rows[i].find("dc.instanceTransforms.empty()") == std::string::npos) continue;
		const std::string both = rows[i] + "\n" + (i + 1 < rows.size() ? rows[i + 1] : std::string());
		const bool builtInInstanced = both.find("m_gbufferInstancedProgram") != std::string::npos
		                           || both.find("m_instancedProgram") != std::string::npos;
		if (!builtInInstanced) continue;
		++gated;
		CHECK_MESSAGE((both.find("matProg") != std::string::npos), both);
	}
	CHECK(gated == 2);   // G-buffer loop + forward loop; the depth passes carry no material
	// Both material branches draw every instance with the material's program.
	CHECK(countOf(text, "drawGraphInstance(t)") >= 2);
	CHECK(countOf(text, "pushForward(t)") >= 1);
}
