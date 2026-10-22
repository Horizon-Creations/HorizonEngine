// Layer meshes on the instancing path (Thema 180, Schritt 5).
//
// The step-4 layer meshes (Tuft, Pebble, Rock A/B/C, Snow Drift, each with detail levels) are
// ordinary foliage layers, so they reach a backend the way every plant does: one cluster
// RenderObject per bucket and detail level (FoliageExtract.cpp), unfolded into instances by
// GeometryPass / RenderSorter::batchDepthRuns. What this file holds that to for the real
// preset meshes (engine files, not the cube the other foliage tests scatter):
//   * every cluster carries a real box. That needs the mesh resident in the ContentManager,
//     which an engine FILE is not until somebody loads it; without it the box is invalid and
//     the cluster is never culled;
//   * the draw count follows the meshes in play, not the plants: four times the instances
//     in the same buckets draw in the same number of calls, in the scene pass and in the
//     depth passes (shadow casters, AO contributors);
//   * the passes agree on who is there: rocks cast shadows and contribute AO, tufts and
//     pebbles do not, and what a shadow draws is a subset of what the scene draws;
//   * the frustum cull drops buckets of these meshes (their boxes are real), and a
//     preset's detail levels all show up at their distances.
//   * the extract inputs: the layer meshes add none to the FrameKey (a layer's rules and
//     levels are component values; the frame reuse's contract is an unchanged world), the
//     second extract of a frame is the first one's walk, and the next frame sees an edit;
//   * a reopened scene gets every level resident from the asset preload.
#include "doctest.h"
#include "../src/HE_Rendering/src/FoliageExtract.h"
#include <Renderer/IRenderer.h>
#include <HorizonRendering/CommandBuffer.h>
#include <HorizonRendering/FrustumCuller.h>
#include <HorizonRendering/RenderConstants.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderPass.h>
#include <HorizonRendering/RenderSorter.h>
#include <HorizonRendering/RenderWorld.h>
#include <HorizonScene/FoliageSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LayerMeshPresets.h>
#include <HorizonScene/SceneSystems.h>
#include <HorizonScene/Components/FoliageComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/DefaultAssets.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <numeric>
#include <set>
#include <string>
#include <vector>

namespace
{
	namespace fs = std::filesystem;

	struct UuidLess
	{
		bool operator()(const HE::UUID& a, const HE::UUID& b) const { return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo; }
	};
	using UuidSet = std::set<HE::UUID, UuidLess>;

	// The six kinds on one landscape: a flat plain (x < -24), a smoothstep ramp up to a
	// 40 m plateau, the plateau - the Auto Landscape witness's relief at y = 0, so every
	// preset's rule has ground it likes. 128 m square, one vertex per metre.
	struct Land
	{
		HorizonWorld   world;
		ContentManager cm;
		entt::entity   terrain = entt::null;
		std::vector<HE::LayerMeshKind> kinds;

		static std::string contentDir()
		{
			const fs::path p = fs::temp_directory_path() / "he_test_layer_mesh_instancing_content";
			fs::create_directories(p);
			return p.string();
		}

		explicit Land(float densityMul) : cm(contentDir())
		{
			cm.setEngineContentRoot((fs::path(HE_EDITOR_DEPS_DIR) / "EngineContent").string());
			auto& reg = world.registry();
			terrain = world.createEntity("Land");
			reg.emplace_or_replace<TransformComponent>(terrain);
			TerrainComponent tc;
			tc.sizeX = tc.sizeZ = 128.0f;
			tc.resolution = 129;
			tc.heightScale = 0.0f;
			tc.seed = 0;
			tc.sculptHeights.resize(129u * 129u);
			for (uint32_t z = 0; z < 129; ++z)
				for (uint32_t x = 0; x < 129; ++x)
				{
					const float t = std::clamp((-64.0f + static_cast<float>(x) + 24.0f) / 32.0f, 0.0f, 1.0f);
					tc.sculptHeights[z * 129 + x] = 40.0f * t * t * (3.0f - 2.0f * t);
				}
			reg.emplace<TerrainComponent>(terrain, tc);

			FoliageComponent fol;
			for (int k = 0; k < HE::kLayerMeshKindCount; ++k)
			{
				const auto kind = static_cast<HE::LayerMeshKind>(k);
				FoliageLayer* l = k == 0 ? &fol.layers[0] : fol.addLayer();
				REQUIRE(l);
				HE::applyLayerMeshPreset(*l, kind);
				l->density *= densityMul;
				if (kind == HE::LayerMeshKind::SnowDrift) l->heightMin = 30.0f;   // the plateau is 40 m
				kinds.push_back(kind);
				for (int n = 0; n < HE::layerMeshLevelCount(kind); ++n)
					REQUIRE(cm.loadAsset(HE::layerMeshLevelPath(kind, n)) == HE::layerMeshLevelId(kind, n));
			}
			reg.emplace<FoliageComponent>(terrain, fol);
			FoliageSystem::update(world);
		}

		FoliageComponent& fol() { return world.registry().get<FoliageComponent>(terrain); }
	};

	EditorCameraOverride camAt(const glm::vec3& pos, const glm::vec3& target)
	{
		EditorCameraOverride c;
		c.active      = true;
		c.position    = pos;
		c.view        = glm::lookAt(pos, target, glm::vec3(0.0f, 1.0f, 0.0f));
		c.editorIcons = false;
		return c;
	}

	// Over the plain, looking across the ramp to the plateau.
	const EditorCameraOverride kAcross = camAt(glm::vec3(-50.0f, 6.0f, -30.0f), glm::vec3(10.0f, 10.0f, 10.0f));

	RenderWorld extract(Land& l, const EditorCameraOverride& cam)
	{
		RenderExtractor ex;
		ex.setContentManager(&l.cm);
		RenderWorld rw;
		ex.extract(l.world, rw, 16.0f / 9.0f, &cam);
		return rw;
	}

	// What a backend hands its passes: the frustum cull, then the sort by mesh and distance.
	// Runs of one mesh are only runs after the sort, so the draw counts below need it.
	std::vector<uint32_t> all(const RenderWorld& rw)
	{
		std::vector<uint8_t> visible;
		FrustumCuller().cull(rw, visible);
		std::vector<uint32_t> idx;
		RenderSorter().sort(rw, visible, idx);
		return idx;
	}

	struct Drawn
	{
		size_t draws = 0;       // calls a backend makes
		size_t instances = 0;   // plants they carry
		UuidSet meshes;
	};

	Drawn sceneDraws(const RenderWorld& rw)
	{
		CommandBuffer cmds;
		GeometryPass{}.execute(rw, all(rw), cmds);
		Drawn d;
		for (const DrawCall& dc : cmds.drawCalls())
		{
			++d.draws;
			d.instances += dc.instanceTransforms.empty() ? 1u : dc.instanceTransforms.size();
			d.meshes.insert(dc.meshAssetId);
		}
		return d;
	}

	Drawn depthDraws(const RenderWorld& rw, RenderSorter::DepthFilter filter)
	{
		RenderSorter::DepthBatchList list;
		RenderSorter::batchDepthRuns(rw, all(rw), filter, kNoOwnerEntity, list);
		Drawn d;
		for (const auto& b : list.batches)
		{
			++d.draws;
			d.instances += b.count;
			d.meshes.insert(b.meshAssetId);
		}
		return d;
	}

	// The mesh ids a kind draws with, every detail level.
	UuidSet idsOf(HE::LayerMeshKind k)
	{
		UuidSet s;
		for (int n = 0; n < HE::layerMeshLevelCount(k); ++n) s.insert(HE::layerMeshLevelId(k, n));
		return s;
	}
	bool anyOf(const UuidSet& drawn, HE::LayerMeshKind k)
	{
		for (const HE::UUID& id : idsOf(k)) if (drawn.count(id)) return true;
		return false;
	}
}

TEST_CASE("Layer meshes: every cluster is boxed, so the cull sees them")
{
	Land land(1.0f);
	const RenderWorld rw = extract(land, kAcross);
	size_t clusters = 0, unboxed = 0, foreign = 0;
	UuidSet known;
	for (int k = 0; k < HE::kLayerMeshKindCount; ++k)
	{
		const auto ids = idsOf(static_cast<HE::LayerMeshKind>(k));
		known.insert(ids.begin(), ids.end());
	}
	for (const RenderObject& o : rw.objects)
	{
		if (!o.isCluster()) continue;
		++clusters;
		if (!o.worldBounds.isValid()) ++unboxed;
		if (!known.count(o.meshAssetId)) ++foreign;
	}
	CHECK(clusters > 20u);
	CHECK(unboxed == 0u);     // a null box is "never culled"; the mesh files were loaded
	CHECK(foreign == 0u);

	// The same scene with the mesh files NOT resident: the boxes are invalid, the control
	// that shows the check above measures residency and not a default.
	ContentManager bare(Land::contentDir());
	RenderExtractor ex;
	ex.setContentManager(&bare);
	RenderWorld rwBare;
	ex.extract(land.world, rwBare, 16.0f / 9.0f, &kAcross);
	size_t invalid = 0, bareClusters = 0;
	for (const RenderObject& o : rwBare.objects)
		if (o.isCluster()) { ++bareClusters; if (!o.worldBounds.isValid()) ++invalid; }
	CHECK(bareClusters > 0u);
	CHECK(invalid == bareClusters);
}

TEST_CASE("Layer meshes: four times the plants draw in the same number of calls")
{
	Land one(1.0f), four(4.0f);
	size_t n1 = 0, n4 = 0;
	for (const FoliageLayer& l : one.fol().layers)  n1 += l.cachedInstances.size();
	for (const FoliageLayer& l : four.fol().layers) n4 += l.cachedInstances.size();
	REQUIRE(n1 > 1000u);
	CHECK(n4 > 3 * n1);           // the rules reject some candidates; most of the 4x survives

	const RenderWorld a = extract(one, kAcross);
	const RenderWorld b = extract(four, kAcross);

	const Drawn sa = sceneDraws(a), sb = sceneDraws(b);
	CHECK(sa.instances > 500u);
	CHECK(sb.instances > 3 * sa.instances);       // more plants ...
	CHECK(sb.draws == sa.draws);                   // ... the same calls
	CHECK(sb.meshes == sa.meshes);
	CHECK(sa.draws <= 20u);                        // meshes and levels in play, not plants
	CHECK(sa.draws * 20u < sa.instances);

	for (const auto filter : { RenderSorter::DepthFilter::ShadowCasters, RenderSorter::DepthFilter::AoContributors })
	{
		const Drawn da = depthDraws(a, filter), db = depthDraws(b, filter);
		CHECK(da.instances > 0u);
		CHECK(db.instances > da.instances);
		CHECK(db.draws == da.draws);
		CHECK(db.meshes == da.meshes);
	}
	// What a plant is in each pass is the layer's flag, not the instance count: the same
	// instances through the cluster path carry the preset's shadow / AO choice.
	CHECK(HE::lastFoliageExtractStats().clusters > 0u);
}

TEST_CASE("Layer meshes: the passes agree on who is there")
{
	Land land(1.0f);
	const RenderWorld rw = extract(land, kAcross);

	const Drawn scene  = sceneDraws(rw);
	const Drawn shadow = depthDraws(rw, RenderSorter::DepthFilter::ShadowCasters);
	const Drawn ao     = depthDraws(rw, RenderSorter::DepthFilter::AoContributors);

	// Rocks (preset: shadows and AO on) are in the depth passes, tufts and pebbles (off) are not.
	for (const auto rock : { HE::LayerMeshKind::RockA, HE::LayerMeshKind::RockB, HE::LayerMeshKind::RockC })
	{
		CAPTURE(HE::layerMeshKindName(rock));
		CHECK(anyOf(scene.meshes, rock));
		CHECK(anyOf(shadow.meshes, rock));
		CHECK(anyOf(ao.meshes, rock));
	}
	CHECK(anyOf(scene.meshes, HE::LayerMeshKind::Tuft));
	CHECK(anyOf(scene.meshes, HE::LayerMeshKind::Pebble));
	CHECK(!anyOf(shadow.meshes, HE::LayerMeshKind::Tuft));
	CHECK(!anyOf(shadow.meshes, HE::LayerMeshKind::Pebble));
	CHECK(!anyOf(ao.meshes, HE::LayerMeshKind::Tuft));
	CHECK(!anyOf(ao.meshes, HE::LayerMeshKind::Pebble));
	CHECK(anyOf(scene.meshes, HE::LayerMeshKind::SnowDrift));

	// A depth pass draws a subset of the scene's meshes and fewer plants.
	for (const HE::UUID& id : shadow.meshes) CHECK(scene.meshes.count(id) == 1u);
	CHECK(shadow.instances < scene.instances);
	CHECK(shadow.instances > 0u);
	CHECK(shadow.instances <= ao.instances);          // a rock's Shadow Distance (60 m) is shorter than its AO reach
}

TEST_CASE("Layer meshes: the cull drops the buckets behind the camera and a preset's levels appear by distance")
{
	Land land(1.0f);
	const RenderWorld rw = extract(land, kAcross);
	FrustumCuller culler;
	std::vector<uint8_t> visible;
	culler.cull(rw, visible);
	REQUIRE(visible.size() == rw.objects.size());
	const size_t kept = static_cast<size_t>(std::count(visible.begin(), visible.end(), uint8_t(1)));
	CHECK(kept > 0u);
	CHECK(kept < rw.objects.size());

	// Looking the other way sees a different, smaller set of buckets of the same scene.
	const RenderWorld away = extract(land, camAt(glm::vec3(-50.0f, 6.0f, -30.0f), glm::vec3(-120.0f, 6.0f, -60.0f)));
	std::vector<uint8_t> visAway;
	culler.cull(away, visAway);
	const size_t keptAway = static_cast<size_t>(std::count(visAway.begin(), visAway.end(), uint8_t(1)));
	CHECK(keptAway < kept);

	// Level 0 near, the coarser ones farther out: tufts have 3 meshes, all of them in a
	// 128 m field seen from its rim (levels start at 15 m and 35 m, the draw distance is 70 m).
	UuidSet tuftMeshes;
	for (const RenderObject& o : rw.objects)
		if (o.isCluster() && idsOf(HE::LayerMeshKind::Tuft).count(o.meshAssetId)) tuftMeshes.insert(o.meshAssetId);
	CHECK(tuftMeshes.size() == 3u);
}

TEST_CASE("Layer meshes: one walk per frame, and the next frame sees a changed rule")
{
	// The layer meshes add no extract input of their own: a layer's rules, levels and
	// scatter are component values the walk reads, and the frame reuse's contract is that
	// the world does not change between two extracts of one frame (RenderExtractor.h,
	// beginFrame). So the FrameKey is unchanged - what has to hold is that the second
	// extract of a frame is the first one's walk, in place, and that the next frame, with
	// a layer edited in between, walks again and sees the edit.
	Land land(1.0f);
	RenderExtractor ex;
	ex.setContentManager(&land.cm);
	RenderWorld rw;

	size_t clusters = 0, instances = 0;
	const RenderObject* walked = nullptr;
	{
		RenderExtractor::FrameScope frame(ex);
		ex.extract(land.world, rw, 16.0f / 9.0f, &kAcross);
		walked = rw.objects.data();
		for (const RenderObject& o : rw.objects) clusters += o.isCluster();
		instances = sceneDraws(rw).instances;
		ex.extract(land.world, rw, 16.0f / 9.0f, &kAcross);   // the shadow / SSAO pass of the same frame
	}
	CHECK(ex.fullExtractCount() == 1u);
	CHECK(ex.reusedExtractCount() == 1u);
	CHECK(rw.objects.data() == walked);                       // found in place, nothing rebuilt
	size_t clustersAgain = 0;
	for (const RenderObject& o : rw.objects) clustersAgain += o.isCluster();
	CHECK(clustersAgain == clusters);
	CHECK(sceneDraws(rw).instances == instances);
	REQUIRE(instances > 500u);

	// Outside the frame, an edit and a re-scatter: the next frame is a new walk with fewer plants.
	for (FoliageLayer& l : land.fol().layers) { l.slopeMax = 0.0f; l.dirty = true; }
	FoliageSystem::update(land.world);
	{
		RenderExtractor::FrameScope frame(ex);
		ex.extract(land.world, rw, 16.0f / 9.0f, &kAcross);
		ex.extract(land.world, rw, 16.0f / 9.0f, &kAcross);
	}
	CHECK(ex.fullExtractCount() == 2u);
	CHECK(ex.reusedExtractCount() == 2u);
	CHECK(sceneDraws(rw).instances < instances);

	// A second view (the preview next to the scene view) has its own extractor, hence its
	// own level memory, and the same mesh levels are chosen from its own camera.
	RenderExtractor other;
	other.setContentManager(&land.cm);
	RenderWorld rw2;
	other.extract(land.world, rw2, 16.0f / 9.0f, &kAcross);
	CHECK(sceneDraws(rw2).instances == sceneDraws(rw).instances);
}

TEST_CASE("Layer meshes: a scene that names them by UUID gets them resident from the preload")
{
	// A reopened scene stores the layer's mesh and level ids, nobody loads the paths. Once
	// the engine folder is scanned the preload must make every level resident: otherwise the
	// extractor reads no box (the cluster is never culled) and the backend draws a fallback.
	ContentManager fresh(Land::contentDir());
	fresh.setEngineContentRoot((fs::path(HE_EDITOR_DEPS_DIR) / "EngineContent").string());
	fresh.scanContentDirectory();
	HorizonWorld world;
	const Entity e = world.createEntity("Land");
	FoliageComponent fol;
	for (int k = 0; k < HE::kLayerMeshKindCount; ++k)
	{
		FoliageLayer* l = k == 0 ? &fol.layers[0] : fol.addLayer();
		REQUIRE(l);
		HE::applyLayerMeshPreset(*l, static_cast<HE::LayerMeshKind>(k));
	}
	for (int k = 0; k < HE::kLayerMeshKindCount; ++k)
		for (const HE::UUID& id : idsOf(static_cast<HE::LayerMeshKind>(k)))
			REQUIRE(fresh.getStaticMesh(id) == nullptr);
	world.registry().emplace<FoliageComponent>(e, fol);
	SceneSystems::preloadAssetRefs(world, fresh);
	for (int k = 0; k < HE::kLayerMeshKindCount; ++k)
		for (const HE::UUID& id : idsOf(static_cast<HE::LayerMeshKind>(k)))
		{
			CAPTURE(HE::layerMeshKindName(static_cast<HE::LayerMeshKind>(k)));
			CHECK(fresh.getStaticMesh(id) != nullptr);
		}
}
