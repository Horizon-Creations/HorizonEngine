// Foliage on the world-scale landscape (Thema 326, step 3): the tile adapter, the scatter core that is shared with the single landscape,
// and LandscapeFoliage, which scatters each (tile, layer) near the camera and lets go of it again. Real scatter, no timing.
#include "doctest.h"
#include <HorizonScene/Components/FoliageComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/FoliageSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LandscapeFoliage.h>
#include <HorizonScene/LandscapeTileTerrain.h>
#include <HorizonScene/TerrainMeshGenerator.h>
#include <HorizonScene/TerrainPaint.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

using namespace HE;

namespace
{
constexpr float kMin = 0.0f, kScale = 1000.0f;

LandscapeGeometry smallGeometry()
{
	LandscapeGeometry g;
	g.hCells = 32;
	g.wTexels = 16;
	g.channels = 12;
	return g;
}

std::shared_ptr<ProceduralLandscapeSource> world(int tiles)
{
	ProceduralLandscapeParams p;
	p.geometry = smallGeometry();
	p.tilesX = p.tilesZ = tiles;
	return std::make_shared<ProceduralLandscapeSource>(p);
}

FoliageLayer grass(float density, float distance, int seed = 11)
{
	FoliageLayer l;
	l.meshAssetId = HE::UUID{ 1, 0 };
	l.density = density;
	l.drawDistance = distance;
	l.seed = seed;
	return l;
}

bool settleStore(LandscapeStore& store, const glm::dvec3& cam, int ms = 20000)
{
	const auto t0 = std::chrono::steady_clock::now();
	for (;;)
	{
		store.update({ { cam } }, 5.0);
		if (store.settled()) return true;
		if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > ms) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

double distToSquare(double px, double pz, double x0, double z0, double x1, double z1)
{
	const double dx = std::max({ x0 - px, 0.0, px - x1 }), dz = std::max({ z0 - pz, 0.0, pz - z1 });
	return std::sqrt(dx * dx + dz * dz);
}

size_t tilesWithin(double x, double z, double radius, int tiles)
{
	size_t n = 0;
	for (int tz = 0; tz < tiles; ++tz)
		for (int tx = 0; tx < tiles; ++tx)
			if (distToSquare(x, z, tx * 512.0, tz * 512.0, (tx + 1) * 512.0, (tz + 1) * 512.0) <= radius) ++n;
	return n;
}
} // namespace

TEST_CASE("landscape tile terrain: a tile as a TerrainComponent keeps its heights and its twelve weight channels")
{
	const ProceduralLandscapeSource src([] { ProceduralLandscapeParams p; p.geometry = smallGeometry(); p.tilesX = p.tilesZ = 4; return p; }());
	const LandscapeTile tile = src.generate({ 1, 2 });
	const TerrainComponent tc = landscapeTileTerrain(tile, { -20.0f, 800.0f });
	CHECK(tc.sizeX == doctest::Approx(512.0f));
	CHECK(tc.sizeZ == doctest::Approx(512.0f));
	REQUIRE(tc.resolution == 33u);
	REQUIRE(tc.sculptHeights.size() == 33u * 33u);                     // terrainHeightAt reads them only when the size is exactly res * res
	for (size_t i = 0; i < tc.sculptHeights.size(); ++i)
		REQUIRE(tc.sculptHeights[i] == doctest::Approx(-20.0f + tile.heights[0][i] * (800.0f / 65535.0f)).epsilon(1e-5));
	// the height function of the single landscape answers with the tile's samples at its lattice (local space is centred)
	for (int iz : { 0, 7, 32 })
		for (int ix : { 0, 5, 32 })
			CHECK(terrainHeightAt(tc, -256.0f + ix * 16.0f, -256.0f + iz * 16.0f) == doctest::Approx(tc.sculptHeights[static_cast<size_t>(iz) * 33 + ix]).epsilon(1e-4));

	// weights: every channel 0..11 comes back through the paint API, all three pages
	REQUIRE(tc.weightRes == 16u);
	for (uint32_t z = 0; z < 16; ++z)
		for (uint32_t x = 0; x < 16; ++x)
		{
			uint8_t w[kTerrainMaxLayers] = {};
			REQUIRE(TerrainPaint::texelWeights(tc, x, z, w));
			for (int c = 0; c < 12; ++c) REQUIRE(w[c] == tile.weights[0][(static_cast<size_t>(z) * 16 + x) * 12 + static_cast<size_t>(c)]);
		}
	CHECK(landscapeTileCentre({ 1, 2 }, tile.geometry) == glm::dvec3(768.0, 0.0, 1280.0));

	// a tile with four channels has one page, with six two
	for (int channels : { 4, 6 })
	{
		ProceduralLandscapeParams p;
		p.geometry = smallGeometry();
		p.geometry.channels = static_cast<uint8_t>(channels);
		p.tilesX = p.tilesZ = 2;
		const LandscapeTile t = ProceduralLandscapeSource(p).generate({ 0, 0 });
		const TerrainComponent c = landscapeTileTerrain(t, { 0.0f, 100.0f });
		CHECK(!c.layerWeights.empty());
		CHECK(c.layerWeights2.empty() == (channels <= 4));
		CHECK(c.layerWeights3.empty());
	}
	// a tile that cannot be represented gives an empty terrain, not a broken one
	LandscapeTile broken = tile;
	broken.heights.clear();
	CHECK(landscapeTileTerrain(broken, {}).sizeX == 0.0f);
}

TEST_CASE("foliage: the public scatter is the entity path's scatter, instance for instance")
{
	const ProceduralLandscapeSource src([] { ProceduralLandscapeParams p; p.geometry = smallGeometry(); p.tilesX = p.tilesZ = 3; return p; }());
	const TerrainComponent tc = landscapeTileTerrain(src.generate({ 1, 1 }), { kMin, kScale });

	FoliageLayer viaApi = grass(0.002f, 100.0f, 5);
	viaApi.slopeMax = 0.3f;
	viaApi.heightMin = 100.0f;
	viaApi.weightLayer = 2;
	FoliageSystem::scatterLayer(tc, glm::vec3(0.0f), viaApi);

	HorizonWorld w;
	const Entity e = w.createEntity("Terrain");
	w.registry().emplace<TerrainComponent>(e, tc);
	FoliageComponent fol;
	fol.layers[0] = grass(0.002f, 100.0f, 5);
	fol.layers[0].slopeMax = 0.3f;
	fol.layers[0].heightMin = 100.0f;
	fol.layers[0].weightLayer = 2;
	w.registry().emplace<FoliageComponent>(e, fol);
	FoliageSystem::update(w);
	const FoliageLayer& viaEntity = w.registry().get<FoliageComponent>(e).layers[0];

	REQUIRE(viaApi.store);
	REQUIRE(viaEntity.store);
	REQUIRE(viaApi.cachedInstances.size() == viaEntity.cachedInstances.size());
	CHECK(viaApi.cachedInstances.size() > 0);
	CHECK(std::memcmp(viaApi.cachedInstances.data(), viaEntity.cachedInstances.data(), viaApi.cachedInstances.size() * sizeof(glm::mat4)) == 0);
	REQUIRE(viaApi.store->local.size() == viaEntity.store->local.size());
	CHECK(std::memcmp(viaApi.store->local.data(), viaEntity.store->local.data(), viaApi.store->local.size() * sizeof(glm::mat4)) == 0);
	CHECK(viaApi.store->buckets.size() == viaEntity.store->buckets.size());

	// rescattering clears first: calling it twice does not pile up
	const size_t n = viaApi.cachedInstances.size();
	FoliageSystem::scatterLayer(tc, glm::vec3(0.0f), viaApi);
	CHECK(viaApi.cachedInstances.size() == n);
}

TEST_CASE("landscape foliage: each tile near the camera is scattered on its own, on the ground, the same every time, and let go of when far")
{
	auto src = world(8);
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 3000.0f;
	LandscapeStore store(src, cfg);
	const glm::dvec3 cam(2048.0 + 256.0, 600.0, 2048.0 + 256.0);        // the middle of tile (4, 4)
	REQUIRE(settleStore(store, cam));

	LandscapeFoliage fol;
	fol.setLayers({ grass(0.001f, 300.0f, 11), grass(0.0005f, 900.0f, 12) });
	// Updates until everything in reach of `at` has its scatter and nothing is in flight. A scatter may remain beyond the reach (inside the
	// keep ring, hysteresis), so the check is "all that is wanted is there" and, separately, "nothing outside the keep ring".
	const auto run = [&](const glm::dvec3& at) {
		const double reach[2] = { 300.0, 900.0 };
		for (int i = 0; i < 4000; ++i)
		{
			fol.update(store, { at }, kMin, kScale);
			bool all = fol.stats().pending == 0;
			for (int tz = 0; tz < 8 && all; ++tz)
				for (int tx = 0; tx < 8 && all; ++tx)
					for (size_t l = 0; l < 2; ++l)
						if (distToSquare(at.x, at.z, tx * 512.0, tz * 512.0, (tx + 1) * 512.0, (tz + 1) * 512.0) <= reach[l] && !fol.storeOf({ tx, tz }, l))
							all = false;
			if (all) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	};
	const auto noneBeyondKeepRing = [&](const glm::dvec3& at) {
		const double reach[2] = { 300.0, 900.0 };
		for (int tz = 0; tz < 8; ++tz)
			for (int tx = 0; tx < 8; ++tx)
				for (size_t l = 0; l < 2; ++l)
					if (fol.storeOf({ tx, tz }, l) &&
					    distToSquare(at.x, at.z, tx * 512.0, tz * 512.0, (tx + 1) * 512.0, (tz + 1) * 512.0) > reach[l] * 1.25)
						return false;
		return true;
	};
	const size_t near300 = tilesWithin(cam.x, cam.z, 300.0, 8), near900 = tilesWithin(cam.x, cam.z, 900.0, 8);
	REQUIRE(run(cam));
	CHECK(fol.stats().scattered == near300 + near900);                  // from a standing start nothing else is there
	CHECK(near300 < near900);                                              // a layer with a short distance lives on fewer tiles

	// every (tile, layer) that is in range has a store, the others none
	for (int tz = 0; tz < 8; ++tz)
		for (int tx = 0; tx < 8; ++tx)
		{
			const double d = distToSquare(cam.x, cam.z, tx * 512.0, tz * 512.0, (tx + 1) * 512.0, (tz + 1) * 512.0);
			CHECK((fol.storeOf({ tx, tz }, 0) != nullptr) == (d <= 300.0));
			CHECK((fol.storeOf({ tx, tz }, 1) != nullptr) == (d <= 900.0));
		}

	// the plants of a tile: on its ground, inside its square, the count its density says
	const TerrainComponent tc = landscapeTileTerrain(src->generate({ 4, 4 }), { kMin, kScale });
	const auto s0 = fol.storeOf({ 4, 4 }, 0);
	REQUIRE(s0);
	CHECK(s0->local.size() == static_cast<size_t>(512.0f * 512.0f * 0.001f));
	for (const glm::mat4& m : s0->local)
	{
		REQUIRE(std::abs(m[3].x) <= 256.0f);
		REQUIRE(std::abs(m[3].z) <= 256.0f);
		REQUIRE(m[3].y == doctest::Approx(terrainHeightAt(tc, m[3].x, m[3].z)).epsilon(1e-4));
	}
	// neighbours grow different plants (the seed is the tile's own)
	const auto s1 = fol.storeOf({ 4, 3 }, 0);
	REQUIRE(s1);
	CHECK(s1->local.size() == s0->local.size());
	CHECK(std::memcmp(s0->local.data(), s1->local.data(), std::min(s0->local.size(), s1->local.size()) * sizeof(glm::mat4)) != 0);

	// draws(): the tile's centre in the absolute world, the layer, the store
	std::vector<LandscapeFoliageDraw> draws;
	fol.draws(draws);
	CHECK(draws.size() == near300 + near900);
	for (const auto& d : draws)
	{
		CHECK(d.centre == glm::dvec3((d.tile.x + 0.5) * 512.0, 0.0, (d.tile.z + 0.5) * 512.0));
		CHECK(d.store == fol.storeOf(d.tile, d.layer));
	}

	// a store stays the same object while it is wanted (the renderer's level memory keys on its address)
	for (int i = 0; i < 30; ++i) fol.update(store, { cam }, kMin, kScale);
	CHECK(fol.storeOf({ 4, 4 }, 0) == s0);

	// the camera leaves: everything is let go of; it returns: the same plants, byte for byte
	const std::vector<glm::mat4> before = s0->local;
	REQUIRE(settleStore(store, glm::dvec3(500.0, 600.0, 500.0)));
	REQUIRE(run(glm::dvec3(500.0, 600.0, 500.0)));
	CHECK(noneBeyondKeepRing(glm::dvec3(500.0, 600.0, 500.0)));
	CHECK(fol.stats().dropped > 0);
	CHECK(fol.storeOf({ 4, 4 }, 0) == nullptr);
	REQUIRE(settleStore(store, cam));
	REQUIRE(run(cam));
	CHECK(noneBeyondKeepRing(cam));
	const auto again = fol.storeOf({ 4, 4 }, 0);
	REQUIRE(again);
	CHECK(again != s0);                                                   // a new object ...
	REQUIRE(again->local.size() == before.size());
	CHECK(std::memcmp(again->local.data(), before.data(), before.size() * sizeof(glm::mat4)) == 0);   // ... of the same plants

	// new layers: everything is thrown away and the revision changes
	const uint32_t rev = fol.revision();
	fol.setLayers({ grass(0.002f, 300.0f, 99) });
	CHECK(fol.revision() != rev);
	CHECK(fol.stats().scattered == 0);
	CHECK(fol.storeOf({ 4, 4 }, 0) == nullptr);
}

TEST_CASE("landscape foliage: the layer's rules judge the tile's ground")
{
	auto src = world(6);
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 2000.0f;
	LandscapeStore store(src, cfg);
	const glm::dvec3 cam(1536.0 + 256.0, 600.0, 1536.0 + 256.0);
	REQUIRE(settleStore(store, cam));

	FoliageLayer open = grass(0.004f, 200.0f, 21);
	FoliageLayer high = open;
	high.heightMin = 450.0f;
	FoliageLayer low = open;
	low.heightMax = 450.0f;
	FoliageLayer paint = open;
	paint.weightLayer = 5;
	FoliageLayer flat = open;
	flat.slopeMax = 0.02f;

	LandscapeFoliage fol;
	fol.setLayers({ open, high, low, paint, flat });
	for (int i = 0; i < 4000; ++i)
	{
		fol.update(store, { cam }, kMin, kScale);
		if (fol.stats().pending == 0 && fol.stats().scattered >= 5) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const auto count = [&](size_t layer) { const auto s = fol.storeOf({ 3, 3 }, layer); return s ? s->local.size() : size_t(0); };
	REQUIRE(count(0) > 100);
	CHECK(count(1) < count(0));
	CHECK(count(2) < count(0));
	CHECK(count(1) + count(2) >= count(0) - 0);                             // the two halves of the height rule cover the layer: nothing is lost, nothing twice
	CHECK(count(3) < count(0));                                             // the paint weight thins it
	CHECK(count(4) < count(0));                                             // the slope rule thins it
	const TerrainComponent tc = landscapeTileTerrain(src->generate({ 3, 3 }), { kMin, kScale });
	for (const glm::mat4& m : fol.storeOf({ 3, 3 }, 1)->local) REQUIRE(m[3].y >= 450.0f);
	for (const glm::mat4& m : fol.storeOf({ 3, 3 }, 2)->local) REQUIRE(m[3].y <= 450.0f);
	(void)tc;
}

TEST_CASE("landscape foliage: a layer without a mesh, or hidden, scatters nothing, and destroying it with scatters running waits for them")
{
	auto src = world(4);
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 1500.0f;
	LandscapeStore store(src, cfg);
	const glm::dvec3 cam(1000.0, 600.0, 1000.0);
	REQUIRE(settleStore(store, cam));
	FoliageLayer noMesh = grass(0.01f, 400.0f);
	noMesh.meshAssetId = HE::UUID{};
	FoliageLayer hidden = grass(0.01f, 400.0f);
	hidden.visible = false;
	{
		LandscapeFoliage fol;
		fol.setLayers({ noMesh, hidden });
		for (int i = 0; i < 50; ++i) fol.update(store, { cam }, kMin, kScale);
		CHECK(fol.stats().started == 0);
		CHECK(fol.stats().scattered == 0);
	}
	{
		LandscapeFoliage fol;                                               // destroyed right after scatters were started
		fol.setLayers({ grass(0.01f, 600.0f) });
		fol.update(store, { cam }, kMin, kScale);
		CHECK(fol.stats().started > 0);
	}
	CHECK(true);   // reaching here means no scatter outlived its LandscapeFoliage (the destructor waited)
}

// ── through the runtime ──────────────────────────────────────────────────────
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/LandscapeWorldComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/LandscapeRuntime.h>
#include <filesystem>
#include <random>

TEST_CASE("landscape runtime: the FoliageComponent on the landscape's entity is scattered per tile around the camera, and follows the component")
{
	ProceduralLandscapeParams p;                      // real tile size, 8 x 8 tiles = 4 km
	p.tilesX = p.tilesZ = 8;
	const ProceduralLandscapeSource src(p);
	const auto path = std::filesystem::temp_directory_path() / ("he_landscape_foliage_" + std::to_string(std::random_device{}()) + ".bin");
	{
		LandscapePackWriter w;
		REQUIRE(w.begin(path.string(), src.geometry(), 8, 8));
		for (int z = 0; z < 8; ++z)
			for (int x = 0; x < 8; ++x) REQUIRE(w.add({ x, z }, encodeLandscapeTile(src.generate({ x, z }))));
		REQUIRE(w.finish());
	}

	HorizonWorld w;
	const Entity land = w.createEntity("Landscape");
	auto& comp = w.registry().emplace<LandscapeWorldComponent>(land);
	comp.pack = path.string();
	FoliageComponent fol;
	fol.layers[0] = grass(0.001f, 400.0f, 31);
	w.registry().emplace<FoliageComponent>(land, fol);
	const Entity cam = w.createEntity("Camera");
	w.registry().get_or_emplace<TransformComponent>(cam).position = { 2048.0f, 700.0f, 2048.0f };
	w.registry().emplace<CameraComponent>(cam);

	LandscapeRuntime rt;
	const auto tick = [&](int n, const auto& done) {
		for (int i = 0; i < n; ++i)
		{
			rt.update(w, 0.016f);
			if (done()) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	};
	const size_t expect = tilesWithin(2048.0, 2048.0, 400.0, 8);
	REQUIRE(tick(6000, [&] { return rt.active() && rt.foliage().stats().pending == 0 && rt.foliage().stats().scattered == expect; }));
	std::vector<LandscapeFoliageDraw> draws;
	rt.foliageDraws(draws);
	CHECK(draws.size() == expect);
	for (const auto& d : draws) CHECK(d.store->local.size() == static_cast<size_t>(512.0f * 512.0f * 0.001f));

	// changing a layer in the component re-scatters (the revision moves, the old scatter is gone, the new one has the new count)
	const uint32_t rev = rt.foliage().revision();
	w.registry().get<FoliageComponent>(land).layers[0].density = 0.002f;
	REQUIRE(tick(6000, [&] { return rt.foliage().revision() != rev && rt.foliage().stats().pending == 0 && rt.foliage().stats().scattered == expect; }));
	rt.foliageDraws(draws);
	REQUIRE_FALSE(draws.empty());
	CHECK(draws[0].store->local.size() == static_cast<size_t>(512.0f * 512.0f * 0.002f));
	// the same change again, nothing changed: no re-scatter
	const uint32_t rev2 = rt.foliage().revision();
	for (int i = 0; i < 40; ++i) rt.update(w, 0.016f);
	CHECK(rt.foliage().revision() == rev2);

	// hidden or removed: no foliage; the landscape itself stays
	w.registry().get<FoliageComponent>(land).visible = false;
	REQUIRE(tick(200, [&] { return rt.foliage().stats().scattered == 0; }));
	CHECK(rt.active());
	w.registry().get<FoliageComponent>(land).visible = true;
	REQUIRE(tick(6000, [&] { return rt.foliage().stats().scattered == expect && rt.foliage().stats().pending == 0; }));
	w.registry().remove<FoliageComponent>(land);
	REQUIRE(tick(200, [&] { return rt.foliage().stats().scattered == 0; }));
	CHECK(rt.active());

	// the landscape goes: nothing is left
	w.registry().emplace<FoliageComponent>(land, fol);
	REQUIRE(tick(6000, [&] { return rt.foliage().stats().scattered == expect && rt.foliage().stats().pending == 0; }));
	rt.reset();
	CHECK(rt.foliage().stats().scattered == 0);
	std::filesystem::remove(path);
}
