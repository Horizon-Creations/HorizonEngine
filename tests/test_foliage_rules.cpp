// Placement rules of a foliage layer (Thema 180, Schritt 4): slope, height and
// paint-layer weight decide where a layer's mesh (a pebble, a rock, a tuft) may
// stand. What this file holds the scatter to: a rule only REJECTS candidates, so
// the survivors are exactly a subset of the unruled layout (same pose); the same
// seed and rules give the same instances; a scene saved before the rules existed
// loads as "no rule" and an untouched layer saves none of the new keys; and the
// scatter writes instance data only, never an entity per instance.
#include "doctest.h"
#include <HorizonScene/FoliageSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/LayerMeshPresets.h>
#include <HorizonScene/TerrainPaint.h>
#include <HorizonScene/TerrainSystem.h>
#include <HorizonScene/TerrainMeshGenerator.h>
#include <HorizonScene/Components/FoliageComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/ContentManager.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace
{
	using json = nlohmann::json;

	// 64 m landscape, 2 m cells. Left half (x < 0) flat at 0, right half a ramp of
	// gradient `rise` per metre: steepness 1 - 1/sqrt(1 + rise^2), 0 on the flat side.
	Entity makeRampLand(HorizonWorld& world, float rise, float originY = 0.0f)
	{
		auto& reg = world.registry();
		const Entity e = world.createEntity("Land");
		auto& tf = reg.emplace<TransformComponent>(e);
		tf.position.y = originY;
		TerrainComponent tc;
		tc.sizeX = tc.sizeZ = 64.0f;
		tc.resolution = 33;
		tc.seed = 0;
		tc.heightScale = 0.0f;
		tc.sculptHeights.assign(33u * 33u, 0.0f);
		for (uint32_t z = 0; z < 33; ++z)
			for (uint32_t x = 0; x < 33; ++x)
				tc.sculptHeights[z * 33 + x] = rise * std::max(0.0f, -32.0f + 2.0f * static_cast<float>(x));
		reg.emplace<TerrainComponent>(e, tc);
		FoliageComponent fol;
		fol.layers[0].meshAssetId = HE::UUID{1, 0};
		fol.layers[0].density     = 0.5f;   // 64 * 64 * 0.5 = 2048 candidates
		fol.layers[0].seed        = 11;
		reg.emplace<FoliageComponent>(e, fol);
		return e;
	}

	FoliageLayer& layerOf(HorizonWorld& world, Entity e) { return world.registry().get<FoliageComponent>(e).layers[0]; }

	// Re-scatter layer 0 with whatever the test set on it.
	const std::vector<glm::mat4>& scatter(HorizonWorld& world, Entity e)
	{
		layerOf(world, e).dirty = true;
		FoliageSystem::update(world);
		return layerOf(world, e).cachedInstances;
	}

	// Steepness of the ramp (rise per metre), the landscape material's 1 - N.y.
	float steepness(float rise) { return 1.0f - 1.0f / std::sqrt(1.0f + rise * rise); }

	std::set<std::vector<float>> poses(const std::vector<glm::mat4>& v)
	{
		std::set<std::vector<float>> s;
		for (const glm::mat4& m : v) s.insert(std::vector<float>(&m[0][0], &m[0][0] + 16));
		return s;
	}
}

TEST_CASE("Foliage rules: a layer without rules scatters as it always did")
{
	HorizonWorld world;
	const Entity e = makeRampLand(world, 0.5f);
	const FoliageLayer def;
	CHECK(def.slopeMin == 0.0f);   CHECK(def.slopeMax == 1.0f);
	CHECK(def.heightMin == -FoliageLayer::kNoHeightLimit);
	CHECK(def.heightMax == FoliageLayer::kNoHeightLimit);
	CHECK(def.weightLayer == -1);
	CHECK(scatter(world, e).size() == 2048u);
}

TEST_CASE("Foliage rules: slope - Slope Min keeps a rock off the flat, Slope Max keeps grass off the hill")
{
	HorizonWorld world;
	const Entity e = makeRampLand(world, 0.5f);   // steepness 0.1056 on the right half
	const std::vector<glm::mat4> all = scatter(world, e);
	REQUIRE(all.size() == 2048u);

	// Rocks: only on the ramp. The ground a cell (2 m) either side of x = 0 mixes both
	// slopes, so the far side of the rule is judged beyond that margin.
	layerOf(world, e).slopeMin = 0.05f;
	const std::vector<glm::mat4> steep = scatter(world, e);
	CHECK(steep.size() > 200u);
	CHECK(steep.size() < all.size());
	for (const glm::mat4& m : steep) CHECK(m[3].x > -2.5f);
	// ...and the ones that remain are not moved: a subset of the unruled layout.
	const auto allPoses = poses(all);
	for (const std::vector<float>& p : poses(steep)) CHECK(allPoses.count(p) == 1u);

	// A rule nothing satisfies (steeper than the ramp) leaves nothing.
	layerOf(world, e).slopeMin = steepness(0.5f) + 0.05f;
	CHECK(scatter(world, e).empty());

	// Grass: only on the flat.
	layerOf(world, e).slopeMin = 0.0f;
	layerOf(world, e).slopeMax = 0.05f;
	const std::vector<glm::mat4> flat = scatter(world, e);
	CHECK(flat.size() > 200u);
	CHECK(flat.size() < all.size());
	for (const glm::mat4& m : flat) CHECK(m[3].x < 2.5f);

	// The two halves together are the whole landscape (nothing is lost between them).
	layerOf(world, e).slopeMax = 1.0f;
	CHECK(scatter(world, e).size() == all.size());
}

TEST_CASE("Foliage rules: height - the limits are in world metres, the entity's own height included")
{
	HorizonWorld world;
	const Entity e = makeRampLand(world, 0.5f);   // ground y = 0.5 * x on the right half
	const size_t all = scatter(world, e).size();

	layerOf(world, e).heightMin = 5.0f;           // snow on the peaks
	const std::vector<glm::mat4> high = scatter(world, e);
	CHECK(!high.empty());
	CHECK(high.size() < all);
	for (const glm::mat4& m : high) CHECK(m[3].y >= 5.0f);

	layerOf(world, e).heightMin = -FoliageLayer::kNoHeightLimit;
	layerOf(world, e).heightMax = 5.0f;           // grass below the snow line
	const std::vector<glm::mat4> low = scatter(world, e);
	CHECK(!low.empty());
	for (const glm::mat4& m : low) CHECK(m[3].y <= 5.0f);
	CHECK(low.size() + high.size() >= all);       // a window of one value counts twice at most

	// A window above everything.
	layerOf(world, e).heightMin = 50.0f;
	layerOf(world, e).heightMax = 60.0f;
	CHECK(scatter(world, e).empty());

	// The same landscape standing 100 m higher: the limit follows the world height.
	HorizonWorld world2;
	const Entity e2 = makeRampLand(world2, 0.5f, 100.0f);
	layerOf(world2, e2).heightMin = 105.0f;
	const std::vector<glm::mat4> raised = scatter(world2, e2);
	CHECK(raised.size() == high.size());
	for (const glm::mat4& m : raised) CHECK(m[3].y >= 105.0f);
}

TEST_CASE("Foliage rules: paint weight - the layer grows where its paint layer is, and only there")
{
	HorizonWorld world;
	const Entity e = makeRampLand(world, 0.0f);   // flat: only the paint decides
	auto& tc = world.registry().get<TerrainComponent>(e);
	const size_t all = scatter(world, e).size();
	REQUIRE(all == 2048u);

	// Nobody painted: the shader shows layer 0 everywhere, so layer 0 grows everywhere.
	layerOf(world, e).weightLayer = 0;
	CHECK(scatter(world, e).size() == all);
	layerOf(world, e).weightLayer = 1;
	CHECK(scatter(world, e).empty());

	// Everything painted layer 1.
	REQUIRE(TerrainPaint::fillLayer(tc, 1));
	CHECK(scatter(world, e).size() == all);
	layerOf(world, e).weightLayer = 0;
	CHECK(scatter(world, e).empty());

	// A solid disc of layer 0 on layer 1: layer 0's plants stand inside it.
	REQUIRE(TerrainPaint::paint(tc, 0.0f, 0.0f, 0, 12.0f, 0.0f, 1.0f));
	const std::vector<glm::mat4> inDisc = scatter(world, e);
	CHECK(inDisc.size() > 50u);
	CHECK(inDisc.size() < all / 4u);
	for (const glm::mat4& m : inDisc) CHECK(std::hypot(m[3].x, m[3].z) <= 12.5f);
	// Layer 1 holds the rest, so the two together stay near the whole landscape.
	layerOf(world, e).weightLayer = 1;
	const size_t outside = scatter(world, e).size();
	CHECK(outside + inDisc.size() >= all - all / 50u);

	// Layers 4..7 live on the second page: filling layer 5 moves the growth there.
	REQUIRE(TerrainPaint::fillLayer(tc, 5));
	layerOf(world, e).weightLayer = 5;
	CHECK(scatter(world, e).size() == all);
	layerOf(world, e).weightLayer = 1;
	CHECK(scatter(world, e).empty());

	// An index past the last paint layer is no rule rather than a crash.
	layerOf(world, e).weightLayer = 99;
	CHECK(scatter(world, e).size() == all);
}

TEST_CASE("Foliage rules: a weight-gated layer re-scatters after a paint stroke, the others do not")
{
	HorizonWorld world;
	ContentManager cm(".");
	const Entity e = makeRampLand(world, 0.0f);
	auto& reg = world.registry();
	auto& fol = reg.get<FoliageComponent>(e);
	fol.layers.push_back(fol.layers[0]);
	fol.layers[0].weightLayer = 0;      // gated
	fol.layers[1].seed        = 12;     // not
	TerrainSystem::updateTerrains(world, cm);
	FoliageSystem::update(world);
	REQUIRE(fol.layers[0].cachedInstances.size() == 2048u);

	auto& tc = reg.get<TerrainComponent>(e);
	REQUIRE(TerrainPaint::fillLayer(tc, 1));
	TerrainSystem::updateTerrains(world, cm);
	CHECK(fol.layers[0].dirty);
	CHECK(!fol.layers[1].dirty);
	FoliageSystem::update(world);
	CHECK(fol.layers[0].cachedInstances.empty());
	CHECK(fol.layers[1].cachedInstances.size() == 2048u);
}

TEST_CASE("Foliage rules: the same seed gives the same instances, another seed does not")
{
	HorizonWorld world;
	const Entity e = makeRampLand(world, 0.5f);
	auto& l = layerOf(world, e);
	l.slopeMin = 0.05f; l.heightMin = 2.0f; l.heightMax = 12.0f;
	l.weightLayer = 0;

	const std::vector<glm::mat4> first = scatter(world, e);
	REQUIRE(first.size() > 50u);
	const std::vector<glm::mat4> second = scatter(world, e);
	REQUIRE(first.size() == second.size());
	for (size_t i = 0; i < first.size(); ++i) CHECK(first[i] == second[i]);   // bit for bit, same order

	// The bucketed store the renderer reads is the same scatter.
	REQUIRE(layerOf(world, e).store != nullptr);
	CHECK(layerOf(world, e).store->local.size() == first.size());

	layerOf(world, e).seed = 12;
	const std::vector<glm::mat4> other = scatter(world, e);
	CHECK(poses(other) != poses(first));
	// The rules hold for the other seed too.
	for (const glm::mat4& m : other) { CHECK(m[3].y >= 2.0f); CHECK(m[3].y <= 12.0f); }
}

TEST_CASE("Foliage rules: two layers with one mesh and different seeds do not grow in the same spots")
{
	HorizonWorld world;
	const Entity e = makeRampLand(world, 0.5f);
	auto& fol = world.registry().get<FoliageComponent>(e);
	fol.layers[0].slopeMin = 0.05f;
	FoliageLayer* second = fol.addLayer();   // a seed no other layer uses
	REQUIRE(second);
	second->meshAssetId = fol.layers[0].meshAssetId;
	second->density     = fol.layers[0].density;
	second->slopeMin    = 0.05f;
	FoliageSystem::update(world);
	REQUIRE(fol.layers.size() == 2u);
	CHECK(fol.layers[0].seed != fol.layers[1].seed);
	std::vector<std::vector<float>> shared;
	const auto a = poses(fol.layers[0].cachedInstances), b = poses(fol.layers[1].cachedInstances);
	std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(shared));
	CHECK(shared.empty());
}

TEST_CASE("Foliage rules: a scatter creates instance data, never an entity per instance")
{
	HorizonWorld world;
	const Entity e = makeRampLand(world, 0.5f);
	layerOf(world, e).slopeMin = 0.05f;
	const size_t aliveBefore = world.registry().storage<entt::entity>().in_use();
	const size_t n = scatter(world, e).size();
	REQUIRE(n > 200u);
	CHECK(world.registry().storage<entt::entity>().in_use() == aliveBefore);
	// What the renderer streams is the bucket store: one array of matrices plus a few buckets.
	const auto& store = *layerOf(world, e).store;
	CHECK(store.local.size() == n);
	CHECK(store.buckets.size() < n / 4u);
}

TEST_CASE("Foliage rules: the keys are saved only when set, and a scene from before them loads as no rule")
{
	HorizonWorld world;
	const Entity e = makeRampLand(world, 0.5f);
	const auto toJson = [&](const Entity ent) {
		const json comps = json::from_cbor(SceneSerializer().serializeEntityComponents(world, ent));
		REQUIRE(comps.contains("foliage"));
		return comps["foliage"];
	};
	const auto load = [](const json& foliage) {
		HorizonWorld w;
		const Entity ent = w.createEntity("Land");
		REQUIRE(SceneSerializer().applyEntityComponents(w, ent, json::to_cbor(json{ { "foliage", foliage } })));
		return w.registry().get<FoliageComponent>(ent);
	};

	// Untouched: none of the five keys, so an old scene saves as the file it was.
	const json plain = toJson(e);
	for (const char* k : { "slopeMin", "slopeMax", "heightMin", "heightMax", "weightLayer" })
		CHECK(!plain.contains(k));
	// ...and the old form (what the writer produced before) loads with defaults.
	const FoliageComponent old = load(plain);
	REQUIRE(old.layers.size() == 1u);
	CHECK(old.layers[0].slopeMin == 0.0f);   CHECK(old.layers[0].slopeMax == 1.0f);
	CHECK(old.layers[0].heightMin == -FoliageLayer::kNoHeightLimit);
	CHECK(old.layers[0].heightMax == FoliageLayer::kNoHeightLimit);
	CHECK(old.layers[0].weightLayer == -1);

	// Set: written, and read back as written.
	auto& l = layerOf(world, e);
	l.slopeMin = 0.1f; l.slopeMax = 0.6f; l.heightMin = 3.5f; l.heightMax = 80.0f; l.weightLayer = 6;
	const FoliageComponent back = load(toJson(e));
	REQUIRE(back.layers.size() == 1u);
	CHECK(back.layers[0].slopeMin == doctest::Approx(0.1f));
	CHECK(back.layers[0].slopeMax == doctest::Approx(0.6f));
	CHECK(back.layers[0].heightMin == doctest::Approx(3.5f));
	CHECK(back.layers[0].heightMax == doctest::Approx(80.0f));
	CHECK(back.layers[0].weightLayer == 6);

	// A hand-edited file cannot break the scatter: out of range clamps or switches off.
	json bad = toJson(e);
	bad["slopeMin"] = -4.0f; bad["slopeMax"] = 9.0f; bad["heightMin"] = -1e9f; bad["weightLayer"] = 99;
	const FoliageComponent clamped = load(bad);
	CHECK(clamped.layers[0].slopeMin == 0.0f);
	CHECK(clamped.layers[0].slopeMax == 1.0f);
	CHECK(clamped.layers[0].heightMin == -FoliageLayer::kNoHeightLimit);
	CHECK(clamped.layers[0].weightLayer == -1);
}

// ── The layer meshes themselves (mesh_gen) and the presets that use them ──────

namespace
{
	struct Bounds { glm::vec3 lo{ 1e9f }, hi{ -1e9f }; };

	Bounds boundsOf(const StaticMeshAsset& m)
	{
		Bounds b;
		for (size_t i = 0; i + 2 < m.vertices.size(); i += 3)
		{
			const glm::vec3 p(m.vertices[i], m.vertices[i + 1], m.vertices[i + 2]);
			b.lo = glm::min(b.lo, p); b.hi = glm::max(b.hi, p);
		}
		return b;
	}
}

TEST_CASE("Layer meshes: every kind and detail level is a committed engine file with the id the code names")
{
	namespace fs = std::filesystem;
	const fs::path content = fs::temp_directory_path() / "he_test_foliage_rules_content";
	fs::create_directories(content);
	ContentManager cm(content.string());
	cm.setEngineContentRoot((fs::path(HE_EDITOR_DEPS_DIR) / "EngineContent").string());

	for (int k = 0; k < HE::kLayerMeshKindCount; ++k)
	{
		const auto kind = static_cast<HE::LayerMeshKind>(k);
		size_t prevTris = ~size_t(0);
		Bounds first;
		for (int lv = 0; lv < HE::layerMeshLevelCount(kind); ++lv)
		{
			CAPTURE(HE::layerMeshLevelPath(kind, lv));
			const HE::UUID id = cm.loadAsset(HE::layerMeshLevelPath(kind, lv));
			REQUIRE(id == HE::layerMeshLevelId(kind, lv));
			const StaticMeshAsset* m = cm.getStaticMesh(id);
			REQUIRE(m);
			REQUIRE(!m->vertices.empty());
			CHECK(m->normals.size() == m->vertices.size());
			CHECK(m->uvs.size() == m->vertices.size() / 3 * 2);
			const size_t tris = m->indices.size() / 3;
			CHECK(tris > 0u);
			CHECK(tris < prevTris);              // each level is cheaper than the one before
			prevTris = tris;

			const Bounds b = boundsOf(*m);
			// The pivot is at the ground and the foot a little below it: a mesh that began
			// at y = 0 would hover on a slope, one far below would be buried.
			CHECK(b.lo.y > -0.3f);
			CHECK(b.hi.y > 0.05f);
			CHECK(b.hi.y < 1.2f);
			if (kind != HE::LayerMeshKind::Tuft) CHECK(b.lo.y < 0.0f);
			// The same shape from farther away: a level may not change size by more than a quarter.
			if (lv == 0) first = b;
			else
			{
				CHECK(std::abs((b.hi.y - b.lo.y) - (first.hi.y - first.lo.y)) < 0.25f * (first.hi.y - first.lo.y) + 0.02f);
				CHECK(std::abs((b.hi.x - b.lo.x) - (first.hi.x - first.lo.x)) < 0.30f * (first.hi.x - first.lo.x) + 0.02f);
			}
		}
	}
}

TEST_CASE("Layer meshes: a preset fills the layer and its rules put it on the ground that suits it")
{
	for (int k = 0; k < HE::kLayerMeshKindCount; ++k)
	{
		const auto kind = static_cast<HE::LayerMeshKind>(k);
		CAPTURE(HE::layerMeshKindName(kind));
		FoliageLayer l;
		l.seed = 777;
		l.densityMask.assign(l.maskRes * l.maskRes, 200);
		HE::applyLayerMeshPreset(l, kind);
		CHECK(l.meshAssetId == HE::layerMeshLevel0(kind));
		CHECK(l.name == HE::layerMeshKindName(kind));
		CHECK(l.dirty);
		CHECK(l.seed == 777);                        // its own random stream stays
		CHECK(!l.densityMask.empty());               // so does a painted mask
		REQUIRE(l.lods.size() + 1u == static_cast<size_t>(HE::layerMeshLevelCount(kind)));
		float prev = 0.0f;
		for (size_t i = 0; i < l.lods.size(); ++i)
		{
			CHECK(l.lods[i].meshAssetId == HE::layerMeshLevelId(kind, static_cast<int>(i) + 1));
			CHECK(l.lods[i].distance > prev);
			CHECK(l.lods[i].distance < l.drawDistance);
			prev = l.lods[i].distance;
		}
		CHECK(l.density > 0.0f);
		CHECK(l.minScale < l.maxScale);
		const bool anyRule = l.slopeMin > 0.0f || l.slopeMax < 1.0f || l.heightMin > -FoliageLayer::kNoHeightLimit;
		CHECK(anyRule);
	}

	// Rocks stand on the slope, tufts on the flat: the same preset rules, scattered over
	// the ramp landscape (flat on the left, a 0.5 rise on the right: steepness 0.106 is
	// flatter than the rock rule, 0.5 here steeper).
	HorizonWorld world;
	const Entity e = makeRampLand(world, 1.0f);      // steepness 0.293
	auto& fol = world.registry().get<FoliageComponent>(e);
	HE::applyLayerMeshPreset(fol.layers[0], HE::LayerMeshKind::RockA);
	fol.layers[0].density = 0.2f;
	FoliageSystem::update(world);
	REQUIRE(!fol.layers[0].cachedInstances.empty());
	for (const glm::mat4& m : fol.layers[0].cachedInstances) CHECK(m[3].x > -2.5f);   // none on the flat half
	HE::applyLayerMeshPreset(fol.layers[0], HE::LayerMeshKind::Tuft);
	fol.layers[0].density = 0.2f;
	FoliageSystem::update(world);
	REQUIRE(!fol.layers[0].cachedInstances.empty());
	for (const glm::mat4& m : fol.layers[0].cachedInstances) CHECK(m[3].x < 2.5f);    // none on the slope
}
