// Foliage extraction on the pool (Thema 238, Schritt 2): the buckets the draw distance or a detail
// level cuts through are tested plant by plant as pool jobs, and must give EXACTLY what the same
// walk gives on one thread: the same objects in the same order, the same instance blocks over the
// same matrices, the same level memory (so the next frame's hysteresis agrees), the same counters.
//
// Two RenderExtractors, so two level memories: one extracts with the straddlers run in order on
// the calling thread (setFoliageParallelOverride(0)), one with the pool. Both look from the same
// camera path, which crosses level boundaries and moves back over them, so the hysteresis has
// something to remember.
#include "doctest.h"
#include "../src/HE_Rendering/src/FoliageExtract.h"
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderWorld.h>
#include <HorizonRendering/SkyNoise3D.h>
#include <HorizonScene/FoliageSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/FoliageComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/DefaultAssets.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

namespace
{
	struct Parallel
	{
		explicit Parallel(int p) { HE::setFoliageParallelOverride(p); }
		~Parallel() { HE::setFoliageParallelOverride(-1); }
	};

	struct Landscape
	{
		HorizonWorld   world;
		ContentManager cm;
		entt::entity   terrain = entt::null;

		explicit Landscape(bool rotated)
		{
			auto& reg = world.registry();
			terrain = world.createEntity("Terrain");
			TransformComponent tf;
			if (rotated) { tf.position = { 30.0f, 2.0f, -10.0f }; tf.rotation = { 0.0f, 25.0f, 0.0f }; }
			reg.emplace_or_replace<TransformComponent>(terrain, tf);
			TerrainComponent tc;
			tc.sizeX = tc.sizeZ = 400.0f;
			tc.heightScale = 0.0f;
			tc.seed = 0;
			reg.emplace<TerrainComponent>(terrain, tc);
			FoliageComponent fol;
			fol.layers.resize(3);
			for (int i = 0; i < 3; ++i)
			{
				FoliageLayer& l = fol.layers[i];
				l.meshAssetId  = HE::kDefaultCubeMeshId;
				l.density      = 0.03f + 0.01f * i;
				l.seed         = 11 + 7 * i;
				l.drawDistance = 90.0f + 25.0f * i;
				l.bucketSize   = 32.0f;
				if (i >= 1)
				{
					FoliageLod a; a.distance = 30.0f; a.density = 0.6f;
					FoliageLod b; b.distance = 60.0f; b.density = 0.3f;
					l.lods = { a, b };
					l.lodHysteresis = 3.0f;
				}
			}
			reg.emplace<FoliageComponent>(terrain, fol);
			FoliageSystem::update(world);
			for (const FoliageLayer& l : reg.get<FoliageComponent>(terrain).layers) REQUIRE(l.store);
		}
	};

	RenderWorld extractFrom(RenderExtractor& ex, Landscape& f, const glm::vec3& cam)
	{
		ex.setContentManager(&f.cm);
		RenderWorld rw;
		EditorCameraOverride c;
		c.active      = true;
		c.position    = cam;
		c.view        = glm::lookAt(cam, cam + glm::vec3(0.0f, -0.3f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		c.editorIcons = false;
		ex.extract(f.world, rw, 16.0f / 9.0f, &c);
		return rw;
	}

	bool sameBytes(const void* a, const void* b, size_t n) { return std::memcmp(a, b, n) == 0; }

	// Everything the draw passes read of the foliage in a RenderWorld, compared bit for bit.
	void requireIdentical(const RenderWorld& a, const RenderWorld& b)
	{
		REQUIRE(a.objects.size() == b.objects.size());
		REQUIRE(a.instanceBlocks.size() == b.instanceBlocks.size());
		for (size_t i = 0; i < a.objects.size(); ++i)
		{
			const RenderObject& x = a.objects[i];
			const RenderObject& y = b.objects[i];
			CHECK(x.meshAssetId == y.meshAssetId);
			CHECK(x.materialAssetId == y.materialAssetId);
			CHECK(x.entityId == y.entityId);
			CHECK(x.castsShadow == y.castsShadow);
			CHECK(x.contributesAO == y.contributesAO);
			CHECK(x.instanceBlock == y.instanceBlock);
			CHECK(sameBytes(&x.transform, &y.transform, sizeof(x.transform)));
			CHECK(sameBytes(&x.worldBounds, &y.worldBounds, sizeof(x.worldBounds)));
		}
		for (size_t i = 0; i < a.instanceBlocks.size(); ++i)
		{
			const InstanceBlock& x = a.instanceBlocks[i];
			const InstanceBlock& y = b.instanceBlocks[i];
			REQUIRE(x.count == y.count);
			CHECK(x.kind == y.kind);
			CHECK(sameBytes(&x.parent, &y.parent, sizeof(x.parent)));
			REQUIRE(x.valid());
			REQUIRE(y.valid());
			CHECK(sameBytes(x.matrices->data() + x.first, y.matrices->data() + y.first, sizeof(glm::mat4) * x.count));
		}
	}

	std::vector<glm::vec3> cameraPath()
	{
		// Over the level boundaries (30 m, 60 m of each layer) and back, and out to the edge
		// where nearly every bucket is a straddler.
		return { { 0, 5, 0 }, { 4, 5, 3 }, { 29, 5, 2 }, { 31, 5, 2 }, { 33, 5, 2 }, { 31, 5, 2 }, { 60, 5, 20 },
		         { 58, 5, 21 }, { 120, 5, -40 }, { 119, 5, -40 }, { -80, 5, 80 }, { 0, 5, 0 } };
	}

	void compareOverPath(bool rotated)
	{
		Landscape f(rotated);
		RenderExtractor serialEx, poolEx;
		serialEx.setContentManager(&f.cm);
		poolEx.setContentManager(&f.cm);
		uint32_t mostStraddlers = 0;
		uint64_t held = 0;
		for (const glm::vec3& cam : cameraPath())
		{
			RenderWorld a, b;
			{ Parallel p(0); a = extractFrom(serialEx, f, cam); }
			const HE::FoliageExtractStats sa = HE::lastFoliageExtractStats();
			{ Parallel p(-1); b = extractFrom(poolEx, f, cam); }
			const HE::FoliageExtractStats sb = HE::lastFoliageExtractStats();
			CHECK_MESSAGE(a.objects.size() > 0u, "the camera sees foliage");
			requireIdentical(a, b);
			CHECK(sa.clusters == sb.clusters);
			CHECK(sa.straddlingBuckets == sb.straddlingBuckets);
			CHECK(sa.instanceTests == sb.instanceTests);
			CHECK(sa.clusterInstances == sb.clusterInstances);
			CHECK(sa.thinnedInstances == sb.thinnedInstances);
			CHECK(sa.heldInstances == sb.heldInstances);
			for (int k = 0; k < HE::FoliageExtractStats::kLevels; ++k) CHECK(sa.levelInstances[k] == sb.levelInstances[k]);
			mostStraddlers = std::max(mostStraddlers, sb.straddlingBuckets);
			held += sb.heldInstances;
		}
		// The path must actually exercise what is compared: enough straddlers for the pool to split
		// them (parallel_for with grain 4 needs 8), and plants that kept a level through hysteresis.
		CHECK(mostStraddlers >= 8u);
		CHECK(held > 0u);
	}
}

TEST_CASE("Foliage extract: straddlers on the pool give the single-threaded result (landscape at the origin)")
{
	compareOverPath(false);
}

TEST_CASE("Foliage extract: straddlers on the pool give the single-threaded result (rotated, moved landscape)")
{
	compareOverPath(true);
}

// ── A standing camera replays the extract before ──────────────────────────────
namespace
{
	struct Replay
	{
		explicit Replay(int r) { HE::setFoliageReplayOverride(r); }
		~Replay() { HE::setFoliageReplayOverride(-1); }
	};
}

TEST_CASE("Foliage extract: a standing camera replays the previous extract and nothing else changes")
{
	Landscape f(true);
	RenderExtractor walkEx, replayEx;   // two views: two level memories
	walkEx.setContentManager(&f.cm);
	replayEx.setContentManager(&f.cm);
	auto& reg = f.world.registry();

	// Each stop is extracted four times: the first walks, the second walks and records, the others replay.
	struct Stop { glm::vec3 cam; int change; };
	enum { None, DrawDistance, Moved, LayerHidden, LayerShown, EntityFlag, Lods, Mesh };
	const std::vector<Stop> stops = {
		{ { 0, 5, 0 }, None }, { { 31, 5, 2 }, None }, { { 33, 5, 2 }, None }, { { 31, 5, 2 }, None },
		{ { 31, 5, 2 }, DrawDistance }, { { 31, 5, 2 }, Moved }, { { 31, 5, 2 }, LayerHidden },
		{ { 31, 5, 2 }, LayerShown }, { { 31, 5, 2 }, Lods }, { { 31, 5, 2 }, Mesh }, { { 60, 5, 20 }, None },
	};
	uint32_t replays = 0;
	for (const Stop& stop : stops)
	{
		FoliageComponent& fol = reg.get<FoliageComponent>(f.terrain);
		switch (stop.change)
		{
		case DrawDistance: fol.layers[1].drawDistance = 70.0f; break;
		case Moved:        reg.get<TransformComponent>(f.terrain).position += glm::vec3(4.0f, 0.0f, -3.0f); break;
		case LayerHidden:  fol.layers[2].visible = false; break;
		case LayerShown:   fol.layers[2].visible = true; break;
		case Lods:         fol.layers[1].lods[0].distance = 40.0f; break;
		case Mesh:         fol.layers[0].materialAssetId = HE::kDefaultMaterialId; break;
		default: break;
		}
		for (int again = 0; again < 4; ++again)
		{
			RenderWorld a, b;
			{ Replay r(0); a = extractFrom(walkEx, f, stop.cam); }
			CHECK_FALSE(HE::lastFoliageExtractStats().replayed);
			{ Replay r(-1); b = extractFrom(replayEx, f, stop.cam); }
			const HE::FoliageExtractStats sb = HE::lastFoliageExtractStats();
			requireIdentical(a, b);
			if (sb.replayed) ++replays;
			if (again >= 2) CHECK_MESSAGE(sb.replayed, "the third extract with the same inputs replays (change " << stop.change << ")");
			else CHECK_MESSAGE(!sb.replayed, "the first two extracts walk (change " << stop.change << ")");
		}
	}
	CHECK(replays > 0u);
}

// ── Bench (not run by default): the extract's cost with a MOVING camera ──────────
// The game harness of scripts/perf has a standing camera, which the replay answers; this walks one
// across a 512 m landscape with five layers (the s512_x4 scene's order of magnitude) and prints the
// median ExtractFoliage per call, one thread against the pool.
//   he_tests --no-skip -tc="*Foliage extract bench*"
namespace
{
	double timeExtractMs(RenderExtractor& ex, Landscape& f, const glm::vec3& cam)
	{
		RenderWorld rw;
		ex.setContentManager(&f.cm);
		EditorCameraOverride c;
		c.active = true; c.position = cam; c.editorIcons = false;
		c.view = glm::lookAt(cam, cam + glm::vec3(0.0f, -0.3f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		const auto t0 = std::chrono::steady_clock::now();
		ex.extract(f.world, rw, 16.0f / 9.0f, &c);
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	}
	// CPU time of the whole process, ms (std::clock: CPU time on POSIX, wall time since start on Windows;
	// only the skipped benches read it).
	double cpuMs() { return 1e3 * static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC); }
	double median(std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; }
}

TEST_CASE("Foliage extract bench: moving camera, one thread against the pool" * doctest::skip())
{
	Landscape f(false);
	auto& reg = f.world.registry();
	reg.get<TerrainComponent>(f.terrain).sizeX = reg.get<TerrainComponent>(f.terrain).sizeZ = 512.0f;
	FoliageComponent& fol = reg.get<FoliageComponent>(f.terrain);
	fol.layers.resize(5);
	for (size_t i = 0; i < fol.layers.size(); ++i)
	{
		FoliageLayer& l = fol.layers[i];
		l.meshAssetId = HE::kDefaultCubeMeshId;
		l.density = 0.4f; l.seed = 5 + static_cast<int>(i) * 13; l.drawDistance = 100.0f;
		l.dirty = true;
	}
	fol.dirty = true;
	FoliageSystem::update(f.world);
	size_t total = 0;
	for (const FoliageLayer& l : fol.layers) total += l.store ? l.store->local.size() : 0;
	std::printf("[bench] %zu stored instances\n", total);
	// CPU time per extract, one thread then the pool, each over the whole path.
	auto cpuPerExtract = [&](int parallel) {
		RenderExtractor ex;
		Parallel pa(parallel); Replay re(0);
		const double c0 = cpuMs();
		for (int i = 0; i < 600; ++i)
			timeExtractMs(ex, f, glm::vec3(-60.0f + 0.2f * static_cast<float>(i), 15.0f, -30.0f + 0.1f * static_cast<float>(i)));
		return (cpuMs() - c0) / 600.0;
	};
	std::printf("[bench] CPU per extract (all threads): one thread %.3f ms, pool %.3f ms\n", cpuPerExtract(0), cpuPerExtract(-1));
	// Alternating per step, so whatever else the machine is doing lands on both.
	RenderExtractor serialEx, poolEx;
	std::vector<double> serial, pool;
	for (int i = 0; i < 600; ++i)
	{
		const glm::vec3 cam(-60.0f + 0.2f * static_cast<float>(i), 15.0f, -30.0f + 0.1f * static_cast<float>(i));
		{ Parallel pa(0); Replay re(0); serial.push_back(timeExtractMs(serialEx, f, cam)); }
		{ Parallel pa(-1); Replay re(0); pool.push_back(timeExtractMs(poolEx, f, cam)); }
	}
	std::printf("[bench] whole extract, moving camera, median of 600: one thread %.3f ms, pool %.3f ms\n",
	            median(serial), median(pool));
}

// ── The scatter: layers as pool jobs ──────────────────────────────────────────
TEST_CASE("Foliage scatter: layers scattered together are the layers scattered alone")
{
	auto makeLayer = [](int i) {
		FoliageLayer l;
		l.meshAssetId  = HE::kDefaultCubeMeshId;
		l.density      = 0.02f + 0.01f * static_cast<float>(i);
		l.seed         = 100 + 31 * i;
		l.minScale     = 0.5f + 0.1f * static_cast<float>(i);
		l.bucketSize   = (i % 2) ? 32.0f : 64.0f;
		return l;
	};
	auto makeWorld = [](HorizonWorld& w, const std::vector<FoliageLayer>& layers) {
		auto& reg = w.registry();
		const entt::entity t = w.createEntity("Terrain");
		TransformComponent tf; tf.position = { 3.0f, 1.0f, -2.0f };
		reg.emplace_or_replace<TransformComponent>(t, tf);
		TerrainComponent tc;
		tc.sizeX = tc.sizeZ = 300.0f; tc.heightScale = 4.0f; tc.seed = 9;
		reg.emplace<TerrainComponent>(t, tc);
		FoliageComponent fol;
		fol.layers = layers;
		reg.emplace<FoliageComponent>(t, fol);
		FoliageSystem::update(w);
		return t;
	};

	std::vector<FoliageLayer> layers;
	for (int i = 0; i < 5; ++i) layers.push_back(makeLayer(i));
	HorizonWorld together;
	const entt::entity tt = makeWorld(together, layers);
	const FoliageComponent& all = together.registry().get<FoliageComponent>(tt);
	REQUIRE(all.layers.size() == 5u);

	for (int i = 0; i < 5; ++i)
	{
		HorizonWorld alone;
		const entt::entity ta = makeWorld(alone, { layers[static_cast<size_t>(i)] });
		const FoliageLayer& a = alone.registry().get<FoliageComponent>(ta).layers[0];
		const FoliageLayer& b = all.layers[static_cast<size_t>(i)];
		REQUIRE(a.store);
		REQUIRE(b.store);
		REQUIRE(a.cachedInstances.size() == b.cachedInstances.size());
		CHECK(a.cachedInstances.size() > 100u);
		CHECK(sameBytes(a.cachedInstances.data(), b.cachedInstances.data(), a.cachedInstances.size() * sizeof(glm::mat4)));
		REQUIRE(a.store->local.size() == b.store->local.size());
		CHECK(sameBytes(a.store->local.data(), b.store->local.data(), a.store->local.size() * sizeof(glm::mat4)));
		REQUIRE(a.store->buckets.size() == b.store->buckets.size());
	}
	// Dirtying one layer afterwards re-scatters that layer alone and leaves the others' stores untouched.
	FoliageComponent& mut = together.registry().get<FoliageComponent>(tt);
	const auto keep = mut.layers[3].store;
	const uint32_t rev = mut.revision;
	mut.layers[1].dirty = true;
	FoliageSystem::update(together);
	CHECK(mut.layers[3].store == keep);
	CHECK(mut.revision == rev + 1);
	CHECK(mut.layers[1].store);
}

// ── Bench (not run by default): the sky noise volume's cost (Thema 238) ───────────
// he_tests --no-skip -tc="*SkyNoise3D bench*"   (the bytes are pinned in test_culling.cpp)
TEST_CASE("SkyNoise3D bench: wall and CPU time of the 256^3 volume" * doctest::skip())
{
	for (int round = 0; round < 3; ++round)
	{
		const double c0 = cpuMs();
		const auto t0 = std::chrono::steady_clock::now();
		const std::vector<uint16_t> v = HE::BuildSkyNoise3D(256);
		const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		std::printf("[bench] BuildSkyNoise3D(256): wall %.0f ms, CPU %.0f ms (%zu values)\n", wall, cpuMs() - c0, v.size());
	}
}
