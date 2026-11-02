// Foliage layers (Thema 182, Schritt 1 / plan 2d): several kinds of plant on one
// landscape, each a FoliageLayer of the terrain's FoliageComponent.
//
// What this file holds the layer list to: an old scene loads into layer 0 and
// saves byte for byte as before; layers scatter, paint and draw independently;
// every layer's assets reach an export. The collab half (one mask grid per
// layer) is in test_terrain_sync.cpp, the Landscape panel's brush in
// test_terrain_tools_ui.cpp.
#include "doctest.h"
#include "../src/HE_Rendering/src/FoliageExtract.h"
#include <HorizonRendering/RenderWorld.h>
#include <HorizonScene/FoliagePaint.h>
#include <HorizonScene/FoliageSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/SceneSystems.h>
#include <HorizonScene/Components/FoliageComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/DefaultAssets.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <set>
#include <unordered_set>
#include <string>
#include <vector>

namespace
{
	using json = nlohmann::json;

	// A flat 60 m landscape with whatever foliage the test hands it.
	Entity makeLand(HorizonWorld& world, FoliageComponent fol)
	{
		auto& reg = world.registry();
		const Entity e = world.createEntity("Land");
		reg.emplace<TransformComponent>(e);
		TerrainComponent tc;
		tc.sizeX = tc.sizeZ = 60.0f;
		tc.seed = 0;
		tc.heightScale = 0.0f;
		reg.emplace<TerrainComponent>(e, tc);
		reg.emplace<FoliageComponent>(e, std::move(fol));
		return e;
	}

	FoliageLayer grassLayer(int seed = 42)
	{
		FoliageLayer l;
		l.meshAssetId = HE::kDefaultCubeMeshId;
		l.density     = 0.2f;
		l.seed        = seed;
		return l;
	}

	// The "foliage" object the serializer writes for `e`.
	json foliageJson(HorizonWorld& world, Entity e)
	{
		const json comps = json::from_cbor(SceneSerializer().serializeEntityComponents(world, e));
		REQUIRE(comps.contains("foliage"));
		return comps["foliage"];
	}

	// Loads a hand-written "foliage" object onto a fresh landscape.
	FoliageComponent loadFoliage(const json& foliage)
	{
		HorizonWorld world;
		const Entity e = world.createEntity("Land");
		const json comps = { { "foliage", foliage } };
		REQUIRE(SceneSerializer().applyEntityComponents(world, e, json::to_cbor(comps)));
		REQUIRE(world.registry().all_of<FoliageComponent>(e));
		return world.registry().get<FoliageComponent>(e);
	}

	std::set<std::string> keysOf(const json& j)
	{
		std::set<std::string> k;
		for (auto it = j.begin(); it != j.end(); ++it) k.insert(it.key());
		return k;
	}
}

TEST_CASE("Foliage layers: a scene from before layers loads into layer 0")
{
	// The shape every scene saved before the layer list had, written by hand so it
	// keeps testing that shape whatever the writer does later.
	std::vector<uint8_t> mask(32u * 32u, 200);
	mask[5] = 0;
	const json old = {
		{ "visible",      false },
		{ "mesh",         json::array({ 11u, 22u }) },
		{ "material",     json::array({ 33u, 44u }) },
		{ "density",      0.35 },
		{ "seed",         7 },
		{ "minScale",     0.6 },
		{ "maxScale",     1.6 },
		{ "drawDistance", 95.0 },
		{ "maskRes",      32 },
		{ "densityMaskB64", SceneSerializer::encodeBase64(mask.data(), mask.size()) },
	};
	const FoliageComponent fol = loadFoliage(old);
	REQUIRE(fol.layers.size() == 1u);
	const FoliageLayer& l = fol.layers[0];
	CHECK_FALSE(fol.visible);            // "visible" always was the component's (zone hiding)
	CHECK(l.visible);                    // the layer's own switch is not that
	CHECK(l.meshAssetId == HE::UUID{ 11u, 22u });
	CHECK(l.materialAssetId == HE::UUID{ 33u, 44u });
	CHECK(l.density == doctest::Approx(0.35f));
	CHECK(l.seed == 7);
	CHECK(l.minScale == doctest::Approx(0.6f));
	CHECK(l.maxScale == doctest::Approx(1.6f));
	CHECK(l.drawDistance == doctest::Approx(95.0f));
	CHECK(l.maskRes == 32u);
	CHECK(l.densityMask == mask);
	CHECK(l.name.empty());
	CHECK(l.castsShadow);
	CHECK(l.contributesAO);
	CHECK(l.bucketSize == doctest::Approx(32.0f));
	CHECK(l.shadowDistance == doctest::Approx(0.0f));
	CHECK(fol.dirty);
	CHECK(l.dirty);
}

TEST_CASE("Foliage layers: one untouched layer saves exactly the keys it always had")
{
	HorizonWorld world;
	FoliageComponent fol;
	fol.layers[0] = grassLayer();
	fol.layers[0].maskRes = 32;
	const Entity e = makeLand(world, fol);
	auto& live = world.registry().get<FoliageComponent>(e);
	REQUIRE(FoliagePaint::paint(live.layers[0], world.registry().get<TerrainComponent>(e),
	                            0.0f, 0.0f, 8.0f, 2.0f, 1.0f, 0.0f));

	const json fj = foliageJson(world, e);
	// The pre-layer writer's key set, no more: an old scene that is opened and saved
	// again is the same file.
	const std::set<std::string> oldKeys = { "visible", "mesh", "material", "density", "seed",
	                                        "minScale", "maxScale", "drawDistance", "maskRes",
	                                        "densityMaskB64" };
	CHECK(keysOf(fj) == oldKeys);

	// And it reads back as it was.
	const FoliageComponent back = loadFoliage(fj);
	REQUIRE(back.layers.size() == 1u);
	CHECK(back.layers[0].densityMask == live.layers[0].densityMask);
	CHECK(back.layers[0].seed == live.layers[0].seed);
}

TEST_CASE("Foliage layers: a single layer's own settings stay in the flat form")
{
	HorizonWorld world;
	FoliageComponent fol;
	fol.layers[0] = grassLayer();
	fol.layers[0].name           = "Grass";
	fol.layers[0].visible        = false;
	fol.layers[0].castsShadow    = false;
	fol.layers[0].contributesAO  = false;
	fol.layers[0].shadowDistance = 25.0f;
	fol.layers[0].bucketSize     = 16.0f;
	const Entity e = makeLand(world, fol);

	const json fj = foliageJson(world, e);
	CHECK_FALSE(fj.contains("layers"));
	CHECK(fj.value("visible", false));                 // the component is still shown
	CHECK_FALSE(fj.value("layerVisible", true));       // the layer is not

	const FoliageComponent back = loadFoliage(fj);
	REQUIRE(back.layers.size() == 1u);
	CHECK(back.visible);
	const FoliageLayer& l = back.layers[0];
	CHECK(l.name == "Grass");
	CHECK_FALSE(l.visible);
	CHECK_FALSE(l.castsShadow);
	CHECK_FALSE(l.contributesAO);
	CHECK(l.shadowDistance == doctest::Approx(25.0f));
	CHECK(l.bucketSize == doctest::Approx(16.0f));
}

TEST_CASE("Foliage layers: two layers round-trip, each with its own mask")
{
	HorizonWorld world;
	FoliageComponent fol;
	fol.layers[0] = grassLayer(42);
	fol.layers[0].maskRes = 32;
	fol.layers.push_back(grassLayer(9001));
	fol.layers[1].name            = "Trees";
	fol.layers[1].meshAssetId     = HE::UUID{ 0xAB, 0xCD };
	fol.layers[1].materialAssetId = HE::UUID{ 0x12, 0x34 };
	fol.layers[1].maskRes         = 64;
	fol.layers[1].castsShadow     = true;
	fol.layers[1].shadowDistance  = 150.0f;
	fol.layers[1].visible         = false;
	fol.visible = false;
	const Entity e = makeLand(world, fol);
	auto& reg  = world.registry();
	auto& live = reg.get<FoliageComponent>(e);
	const auto& tc = reg.get<TerrainComponent>(e);
	REQUIRE(FoliagePaint::paint(live.layers[0], tc, -10.0f, 0.0f, 6.0f, 0.0f, 1.0f, 0.0f));
	REQUIRE(FoliagePaint::paint(live.layers[1], tc,  15.0f, 5.0f, 9.0f, 3.0f, 1.0f, 0.0f));
	REQUIRE(live.layers[0].densityMask != std::vector<uint8_t>(live.layers[0].densityMask.size(), 255));

	json fj = foliageJson(world, e);
	REQUIRE(fj.contains("layers"));
	REQUIRE(fj["layers"].size() == 2u);
	CHECK_FALSE(fj.contains("mesh"));   // nothing flat beside the list

	{
		const FoliageComponent back = loadFoliage(fj);
		CHECK_FALSE(back.visible);
		REQUIRE(back.layers.size() == 2u);
		for (size_t i = 0; i < 2; ++i)
		{
			CAPTURE(i);
			const FoliageLayer& a = live.layers[i];
			const FoliageLayer& b = back.layers[i];
			CHECK(b.name == a.name);
			CHECK(b.visible == a.visible);
			CHECK(b.meshAssetId == a.meshAssetId);
			CHECK(b.materialAssetId == a.materialAssetId);
			CHECK(b.seed == a.seed);
			CHECK(b.maskRes == a.maskRes);
			CHECK(b.densityMask == a.densityMask);
			CHECK(b.castsShadow == a.castsShadow);
			CHECK(b.shadowDistance == doctest::Approx(a.shadowDistance));
		}
	}

	// A mask that does not fit its resolution is dropped for that layer only.
	fj["layers"][1]["maskRes"] = 128;
	{
		const FoliageComponent back = loadFoliage(fj);
		REQUIRE(back.layers.size() == 2u);
		CHECK(back.layers[0].densityMask == live.layers[0].densityMask);
		CHECK(back.layers[1].densityMask.empty());
	}

	// A hand-edited empty list is one default layer, never none.
	fj["layers"] = json::array();
	CHECK(loadFoliage(fj).layers.size() == 1u);

	// More layers than a landscape may carry: the rest are dropped.
	json many = json::array();
	for (size_t i = 0; i < FoliageComponent::kMaxLayers + 3; ++i) many.push_back(json::object());
	fj["layers"] = many;
	CHECK(loadFoliage(fj).layers.size() == FoliageComponent::kMaxLayers);
}

TEST_CASE("Foliage layers: each layer scatters on its own, and layer 0 as it always did")
{
	// The same layer alone, for the reference layout.
	std::vector<glm::mat4> alone;
	{
		HorizonWorld w;
		FoliageComponent f;
		f.layers[0] = grassLayer(42);
		const Entity e = makeLand(w, f);
		FoliageSystem::update(w);
		alone = w.registry().get<FoliageComponent>(e).layers[0].cachedInstances;
		REQUIRE(alone.size() == 720u);   // 60 * 60 * 0.2
	}

	HorizonWorld world;
	FoliageComponent fol;
	fol.layers[0] = grassLayer(42);
	fol.layers.push_back(grassLayer(43));
	fol.layers[1].density = 0.05f;
	const Entity e = makeLand(world, fol);
	FoliageSystem::update(world);
	auto& live = world.registry().get<FoliageComponent>(e);
	REQUIRE(live.layers[0].cachedInstances.size() == alone.size());
	CHECK(std::equal(alone.begin(), alone.end(), live.layers[0].cachedInstances.begin()));
	CHECK(live.layers[1].cachedInstances.size() == 180u);
	CHECK(live.layers[1].cachedInstances[0] != live.layers[0].cachedInstances[0]);

	// Painting the second layer re-scatters it, and only it.
	const auto store0 = live.layers[0].store;
	const auto store1 = live.layers[1].store;
	const uint32_t rev = live.revision;
	REQUIRE(FoliagePaint::paint(live.layers[1], world.registry().get<TerrainComponent>(e),
	                            0.0f, 0.0f, 20.0f, 0.0f, 1.0f, 0.0f));
	CHECK_FALSE(live.layers[0].dirty);
	FoliageSystem::update(world);
	CHECK(live.layers[0].store == store0);
	CHECK(live.layers[1].store != store1);
	CHECK(live.layers[1].cachedInstances.size() < 180u);
	CHECK(live.revision > rev);

	// The ground moving (the component's own flag) re-scatters every layer.
	live.dirty = true;
	FoliageSystem::update(world);
	CHECK(live.layers[0].store != store0);
	CHECK(std::equal(alone.begin(), alone.end(), live.layers[0].cachedInstances.begin()));

	// Adding or removing a layer is a change a retained RenderWorld must hear of.
	const uint32_t rev2 = live.revision;
	live.layers.pop_back();
	FoliageSystem::update(world);
	CHECK(live.revision > rev2);
}

TEST_CASE("Foliage layers: the extraction draws every visible layer with its own mesh")
{
	HorizonWorld world;
	FoliageComponent fol;
	fol.layers[0] = grassLayer(42);
	fol.layers.push_back(grassLayer(43));
	fol.layers[1].meshAssetId     = HE::UUID{ 257u, 1u };   // the default sphere
	fol.layers[1].materialAssetId = HE::UUID{ 0x77, 0x88 };
	fol.layers[1].castsShadow     = false;
	const Entity e = makeLand(world, fol);
	FoliageSystem::update(world);

	auto meshesDrawn = [&](RenderWorld& out) {
		std::unordered_set<HE::UUID> meshes;
		size_t instances1 = 0;
		for (const RenderObject& o : out.objects)
		{
			meshes.insert(o.meshAssetId);
			if (o.meshAssetId == HE::UUID{ 257u, 1u })
			{
				CHECK(o.materialAssetId == HE::UUID{ 0x77, 0x88 });
				CHECK_FALSE(o.castsShadow);
				REQUIRE(o.instanceBlock >= 0);
				instances1 += out.instanceBlocks[static_cast<size_t>(o.instanceBlock)].count;
			}
		}
		return std::make_pair(meshes, instances1);
	};

	{
		RenderWorld out;
		out.camera.position = glm::vec3(0.0f, 10.0f, 0.0f);
		HE::extractFoliage(world.registry(), out, nullptr);
		const auto [meshes, n1] = meshesDrawn(out);
		CHECK(meshes.count(HE::kDefaultCubeMeshId) == 1u);
		CHECK(meshes.count(HE::UUID{ 257u, 1u }) == 1u);
		CHECK(n1 == world.registry().get<FoliageComponent>(e).layers[1].cachedInstances.size());
		CHECK(HE::lastFoliageExtractStats().layers == 2u);
	}

	// A hidden layer is not drawn; its neighbour still is.
	world.registry().get<FoliageComponent>(e).layers[1].visible = false;
	{
		RenderWorld out;
		out.camera.position = glm::vec3(0.0f, 10.0f, 0.0f);
		HE::extractFoliage(world.registry(), out, nullptr);
		const auto [meshes, n1] = meshesDrawn(out);
		CHECK(meshes.count(HE::kDefaultCubeMeshId) == 1u);
		CHECK(meshes.count(HE::UUID{ 257u, 1u }) == 0u);
		CHECK(n1 == 0u);
	}

	// The component's switch (zone hiding) still hides them all.
	world.registry().get<FoliageComponent>(e).visible = false;
	{
		RenderWorld out;
		out.camera.position = glm::vec3(0.0f, 10.0f, 0.0f);
		HE::extractFoliage(world.registry(), out, nullptr);
		CHECK(out.objects.empty());
	}
}

TEST_CASE("Foliage layers: every layer's mesh and material reach the asset references")
{
	// collectAssetRefs is what an export packs and a game preloads: a layer it
	// missed would be an invisible layer in the shipped game.
	HorizonWorld world;
	FoliageComponent fol;
	fol.layers[0] = grassLayer();
	fol.layers.push_back(grassLayer(43));
	fol.layers[1].meshAssetId     = HE::UUID{ 0x51, 0x52 };
	fol.layers[1].materialAssetId = HE::UUID{ 0x53, 0x54 };
	makeLand(world, fol);
	const std::vector<HE::UUID> refs = SceneSystems::collectAssetRefs(world);
	auto has = [&](const HE::UUID& id) { return std::find(refs.begin(), refs.end(), id) != refs.end(); };
	CHECK(has(HE::kDefaultCubeMeshId));
	CHECK(has(HE::UUID{ 0x51, 0x52 }));
	CHECK(has(HE::UUID{ 0x53, 0x54 }));
}

TEST_CASE("Foliage layers: a new layer gets a seed of its own and becomes the active one")
{
	FoliageComponent fol;
	REQUIRE(fol.layers.size() == 1u);
	CHECK(fol.activeIndex() == 0u);
	FoliageLayer* l = fol.addLayer();
	REQUIRE(l != nullptr);
	CHECK(fol.layers.size() == 2u);
	CHECK(fol.activeIndex() == 1u);
	CHECK(fol.layers[1].name == "Layer 2");
	CHECK(fol.layers[1].seed != fol.layers[0].seed);
	CHECK(fol.layers[1].meshAssetId == HE::UUID{});
	CHECK(foliageLayerLabel(fol.layers[0], 0) == "Layer 1");

	while (fol.addLayer()) {}
	CHECK(fol.layers.size() == FoliageComponent::kMaxLayers);
	std::set<int> seeds;
	for (const FoliageLayer& x : fol.layers) seeds.insert(x.seed);
	CHECK(seeds.size() == fol.layers.size());

	// An index a removal left behind is clamped, not trusted.
	fol.layers.resize(3);
	CHECK(fol.activeIndex() == 2u);
	fol.activeLayer = -4;
	CHECK(fol.activeIndex() == 0u);
}
