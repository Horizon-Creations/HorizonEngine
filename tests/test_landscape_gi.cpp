// GI height clipmap on the world-scale landscape (Thema 326, step 4, redone over the tile store): LandscapeGi builds the
// window LandscapeRuntime hands the GI shadow kernel through RenderExtractor. Real data, no GPU.
#include "doctest.h"
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LandscapeGi.h>
#include <HorizonScene/LandscapeRuntime.h>
#include <HorizonScene/LandscapeSource.h>
#include <HorizonScene/LandscapeStore.h>
#include <HorizonScene/LandscapeTile.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderWorld.h>

#include <chrono>
#include <thread>

using namespace HE;

namespace
{
LandscapeGeometry smallGeometry()
{
	LandscapeGeometry g;
	g.hCells = 32;
	g.wTexels = 16;
	g.channels = 4;
	return g;
}

ProceduralLandscapeParams testWorld(int tiles)
{
	ProceduralLandscapeParams p;
	p.geometry = smallGeometry();
	p.tilesX = p.tilesZ = tiles;
	return p;
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

// Updates until an active clipmap exists at `version`, and nothing is in flight.
bool settleGi(LandscapeGi& gi, const LandscapeStore& store, const glm::dvec3& cam, uint64_t version, int ms = 20000)
{
	const auto t0 = std::chrono::steady_clock::now();
	for (;;)
	{
		gi.update(store, cam, { 0.0f, 3000.0f });
		if (gi.heightfield().active() && gi.heightfield().version == version && !gi.stats().building) return true;
		if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > ms) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}
} // namespace

TEST_CASE("landscape gi: an unbuilt clipmap is inactive")
{
	LandscapeGiHeightfield hf;
	CHECK_FALSE(hf.active());
	hf.res = 5;
	CHECK_FALSE(hf.active());   // res without data is still not active
	hf.data = std::make_shared<const std::vector<float>>(static_cast<size_t>(hf.res) * hf.res, 0.0f);
	CHECK(hf.active());
}

TEST_CASE("landscape gi: the window follows the camera in whole tiles, stays put while the camera stays in its inner tiles, "
          "and the version grows only when it actually rebuilds")
{
	auto src = std::make_shared<ProceduralLandscapeSource>(testWorld(10));
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 6000.0f;
	LandscapeStore store(src, cfg);
	const glm::dvec3 cam(5 * 512.0 + 100.0, 600.0, 5 * 512.0 + 100.0);   // tile (5, 5)
	REQUIRE(settleStore(store, cam));

	LandscapeGi gi;
	REQUIRE(settleGi(gi, store, cam, 1));   // first build, version 1
	const LandscapeGiHeightfield first = gi.heightfield();
	CHECK(first.res == 4 * 32 + 1);
	CHECK(first.sizeX == doctest::Approx(4 * 512.0f));
	CHECK(first.centreAbsolute == glm::dvec3(5 * 512.0, 0.0, 5 * 512.0));   // 5 - 4/2 .. +4/2, centred on tile (5,5)
	const uint64_t started = gi.stats().started;
	CHECK(started == 1);

	// inside the inner 2 x 2 tiles of the window (tiles 4 and 5 of 3..6): nothing is rebuilt, the version does not move
	for (const glm::dvec3& c : { glm::dvec3(4 * 512.0 + 10.0, 600.0, 4 * 512.0 + 10.0), glm::dvec3(6 * 512.0 - 10.0, 600.0, 6 * 512.0 - 10.0),
	                             glm::dvec3(5 * 512.0, 600.0, 4 * 512.0 + 300.0) })
	{
		for (int i = 0; i < 20; ++i) gi.update(store, c, { 0.0f, 3000.0f });
		CHECK(gi.stats().started == started);
		CHECK(gi.heightfield().version == first.version);
		CHECK(gi.heightfield().centreAbsolute == first.centreAbsolute);
	}

	// one tile out of the inner range: a new window, one tile on, a new (higher) version
	const glm::dvec3 east(6 * 512.0 + 10.0, 600.0, 5 * 512.0 + 100.0);
	REQUIRE(settleStore(store, east));
	REQUIRE(settleGi(gi, store, east, 2));
	CHECK(gi.stats().started == started + 1);
	CHECK(gi.heightfield().version > first.version);
	CHECK(gi.heightfield().centreAbsolute == glm::dvec3(6 * 512.0, 0.0, 5 * 512.0));

	// at the world's edge the window does not leave the world
	const glm::dvec3 corner(20.0, 600.0, 20.0);
	REQUIRE(settleStore(store, corner));
	REQUIRE(settleGi(gi, store, corner, 3));
	CHECK(gi.heightfield().centreAbsolute == glm::dvec3(2 * 512.0, 0.0, 2 * 512.0));   // window [0,4): centre at tile 2
}

TEST_CASE("landscape gi: a window waits for tiles that are not in memory yet, and a world smaller than the window still gets one")
{
	auto src = std::make_shared<ProceduralLandscapeSource>(testWorld(10));
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 1500.0f;
	LandscapeStore store(src, cfg);
	LandscapeGi gi;
	// the store has not loaded anything yet: nothing can be built, and nothing crashes
	for (int i = 0; i < 10; ++i) gi.update(store, glm::dvec3(2560.0, 600.0, 2560.0), { 0.0f, 3000.0f });
	CHECK_FALSE(gi.heightfield().active());
	CHECK(gi.stats().started == 0);
	CHECK(gi.stats().deferred > 0);

	auto tiny = std::make_shared<ProceduralLandscapeSource>(testWorld(2));   // two tiles a side: the window is 2 x 2, not 4 x 4
	LandscapeStore tinyStore(tiny, cfg);
	REQUIRE(settleStore(tinyStore, glm::dvec3(500.0, 600.0, 500.0)));
	LandscapeGi g2;
	REQUIRE(settleGi(g2, tinyStore, glm::dvec3(500.0, 600.0, 500.0), 1));
	CHECK(g2.heightfield().res == 2 * 32 + 1);
	CHECK(g2.heightfield().sizeX == doctest::Approx(2 * 512.0f));
}

TEST_CASE("landscape gi: more tiles a side than a TerrainComponent holds falls back to what fits")
{
	ProceduralLandscapeParams p;   // the real tile size, 256 cells: 5 tiles a side would need 1281 samples
	p.tilesX = p.tilesZ = 8;
	auto src = std::make_shared<ProceduralLandscapeSource>(p);
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 2000.0f;   // just the window's tiles, not the whole (real-size, so not cheap) world
	LandscapeStore store(src, cfg);
	const glm::dvec3 cam(4 * 512.0, 600.0, 4 * 512.0);
	REQUIRE(settleStore(store, cam));

	LandscapeGiConfig wc;
	wc.tilesPerSide = 5;   // asked for; 5 * 256 + 1 = 1281 > kTerrainMaxResolution (1025)
	LandscapeGi gi(wc);
	REQUIRE(settleGi(gi, store, cam, 1));
	CHECK(gi.heightfield().res == 4 * 256 + 1);   // clamped to 4, like LandscapeWater's region
	CHECK(gi.heightfield().res <= kTerrainMaxResolution);
}

TEST_CASE("landscape runtime: a tile landscape gets a GI height clipmap around the camera, reset clears it")
{
	ProceduralLandscapeParams p;
	p.tilesX = p.tilesZ = 8;
	auto source = std::make_shared<ProceduralLandscapeSource>(p);
	LandscapeRuntime rt;
	std::string err;
	REQUIRE_MESSAGE(rt.begin(source, LandscapeRuntimeConfig{}, &err), err);
	const glm::dvec3 cam(2048.0, 700.0, 2048.0);
	for (int i = 0; i < 6000 && !(rt.giHeightfield().active() && !rt.giStats().building); ++i)
	{
		rt.updateAt(cam, glm::vec3(0.0f), 5.0);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	REQUIRE(rt.giHeightfield().active());
	CHECK(rt.giHeightfield().centreAbsolute == glm::dvec3(2048.0, 0.0, 2048.0));   // tile 4 is the camera's; window [2,6)
	CHECK(rt.giHeightfield().res == 4 * 256 + 1);

	rt.reset();
	CHECK_FALSE(rt.giHeightfield().active());
	CHECK_FALSE(rt.active());
}

TEST_CASE("render extractor: a tile landscape's GI clipmap lands in RenderWorld::heightfields, centred on its window")
{
	ProceduralLandscapeParams p;   // the real tile size: smallGeometry's tiny tiles do not fit the quadtree's default range/load radius
	p.tilesX = p.tilesZ = 8;
	auto source = std::make_shared<ProceduralLandscapeSource>(p);
	LandscapeRuntime rt;
	std::string err;
	REQUIRE_MESSAGE(rt.begin(source, LandscapeRuntimeConfig{}, &err), err);
	const glm::dvec3 cam(4 * 512.0, 700.0, 4 * 512.0);
	for (int i = 0; i < 6000 && !(rt.giHeightfield().active() && !rt.giStats().building); ++i)
	{
		rt.updateAt(cam, glm::vec3(0.0f), 5.0);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	REQUIRE(rt.giHeightfield().active());

	HorizonWorld world;
	world.setLandscapeRuntime(&rt);
	RenderWorld rw;
	RenderExtractor extractor;
	extractor.extract(world, rw, 16.0f / 9.0f);

	REQUIRE(rw.heightfields.size() == 1);
	const GiHeightfield& hf = rw.heightfields[0];
	CHECK(hf.res == rt.giHeightfield().res);
	CHECK(hf.version == rt.giHeightfield().version);
	CHECK(hf.data == rt.giHeightfield().data);
	CHECK(hf.size.x == doctest::Approx(rt.giHeightfield().sizeX));
	CHECK(hf.size.y == doctest::Approx(rt.giHeightfield().sizeZ));
	// worldToLocal maps the window's own centre (world.origin() == 0 here) to local (0, 0, 0): the
	// shader's "+0.5" centred convention (kGiHeightfieldGLSL/HLSL) needs exactly that.
	const glm::vec3 centreRel(rt.giHeightfield().centreAbsolute.x, 0.0, rt.giHeightfield().centreAbsolute.z);
	const glm::vec3 local = glm::vec3(hf.worldToLocal * glm::vec4(centreRel, 1.0f));
	CHECK(local.x == doctest::Approx(0.0f).epsilon(1e-4));
	CHECK(local.z == doctest::Approx(0.0f).epsilon(1e-4));

	// without a landscape runtime attached, extract() leaves heightfields empty (no TerrainComponent either)
	HorizonWorld bare;
	RenderWorld rwBare;
	extractor.extract(bare, rwBare, 16.0f / 9.0f);
	CHECK(rwBare.heightfields.empty());
}
