// Water on the world-scale landscape (Thema 326, step 3c): the tile's water streams, the pack's table of bodies, the procedural sea,
// and (further down) the region that turns tiles into one water::Field-carrying terrain. Real data, no timing.
#include "doctest.h"
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/LandscapeSource.h>
#include <HorizonScene/LandscapeTileTerrain.h>
#include <HorizonScene/LandscapeWater.h>
#include <HorizonScene/LandscapeStore.h>
#include <HorizonScene/LandscapeTile.h>
#include <HorizonScene/LandscapeWaterData.h>
#include <HorizonScene/TerrainMeshGenerator.h>
#include <HorizonScene/TerrainPaint.h>
#include <HorizonScene/WaterField.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <thread>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>

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

ProceduralLandscapeParams seaWorld(int tiles = 4, float sea = 1500.0f)  // the fBm heights sit around 1500 m: about half the cells are under it
{
	ProceduralLandscapeParams p;
	p.geometry = smallGeometry();
	p.tilesX = p.tilesZ = tiles;
	p.seaLevelM = sea;
	return p;
}

std::filesystem::path tempFile(const char* name)
{
	return std::filesystem::temp_directory_path() / (std::string("he_landscape_water_") + name + "_" + std::to_string(std::random_device{}()) + ".bin");
}
} // namespace

TEST_CASE("procedural sea: cells below the level are water of body 1, the rest is dry, a world without a sea has no water at all")
{
	const ProceduralLandscapeSource sea(seaWorld());
	REQUIRE(sea.water());
	REQUIRE(sea.water()->bodies.size() == 1);
	CHECK(sea.water()->bodies[0].id == 1);
	CHECK(sea.water()->bodies[0].level == doctest::Approx(1500.0f));

	int wet = 0, dry = 0;
	for (int tz = 0; tz < 4; ++tz)
		for (int tx = 0; tx < 4; ++tx)
		{
			const LandscapeTile t = sea.generate({ tx, tz });
			REQUIRE(t.hasWater());
			const int W = t.geometry.weightDim(0), n = t.geometry.heightDim(0), step = t.geometry.hCells / t.geometry.wTexels;
			REQUIRE(t.waterCoverage.size() == static_cast<size_t>(W) * W);
			for (int z = 0; z < W; ++z)
				for (int x = 0; x < W; ++x)
				{
					const size_t i = static_cast<size_t>(z) * W + x;
					const float metres = t.heights[0][static_cast<size_t>(z * step) * n + x * step] / 65535.0f * 3000.0f;
					REQUIRE((t.waterCoverage[i] == 255) == (metres < 1500.0f));
					REQUIRE(t.waterOwner[i] == (t.waterCoverage[i] ? 1 : 0));
					(t.waterCoverage[i] ? wet : dry)++;
				}
		}
	CHECK(wet > 0);
	CHECK(dry > 0);

	ProceduralLandscapeParams p = seaWorld();
	p.seaLevelM = -1.0e9f;
	const ProceduralLandscapeSource none(p);
	CHECK(none.water() == nullptr);
	CHECK_FALSE(none.generate({ 1, 1 }).hasWater());
}

TEST_CASE("tile water streams: every codec round-trips them, maxLevels keeps them, a reader from before them still reads the rest")
{
	const ProceduralLandscapeSource sea(seaWorld());
	LandscapeTile t = sea.generate({ 2, 1 });
	REQUIRE(t.hasWater());
	CHECK(std::count(t.waterCoverage.begin(), t.waterCoverage.end(), uint8_t(255)) > 0);
	// a second body and a partly covered cell, to see more than two values
	t.waterOwner[3] = 7;
	t.waterCoverage[3] = 77;

	for (auto backend : { LandscapeCodec::Backend::Store, LandscapeCodec::Backend::Lz4, LandscapeCodec::Backend::Zstd })
	{
		if (!LandscapeCodec::available(backend)) continue;
		for (bool predictor : { false, true })
		{
			LandscapeCodec c;
			c.backend = backend;
			c.predictor = predictor;
			const auto blob = encodeLandscapeTile(t, c);
			REQUIRE_FALSE(blob.empty());
			CHECK((blob[6] & 2) != 0);                                 // the flag is set
			LandscapeTile back;
			std::string err;
			REQUIRE_MESSAGE(decodeLandscapeTile(blob.data(), blob.size(), back, -1, &err), err);
			CHECK(back == t);
			CHECK(back.waterCoverage == t.waterCoverage);
			CHECK(back.waterOwner == t.waterOwner);

			// maxLevels 1 keeps level 0 and the water (they are level-0 data)
			LandscapeTile one;
			REQUIRE(decodeLandscapeTile(blob.data(), blob.size(), one, 1));
			CHECK(one.heights.size() == 1);
			CHECK(one.waterCoverage == t.waterCoverage);
			CHECK(one.waterOwner == t.waterOwner);

			// a reader that knows nothing of the flag stops after the weights: clear the bit and the same blob reads as a tile without water
			auto old = blob;
			old[6] = static_cast<uint8_t>(old[6] & ~2);
			LandscapeTile legacy;
			REQUIRE_MESSAGE(decodeLandscapeTile(old.data(), old.size(), legacy, -1, &err), err);
			CHECK_FALSE(legacy.hasWater());
			CHECK(legacy.heights == t.heights);
			CHECK(legacy.weights == t.weights);

			// every truncation inside the water streams is refused; the flag on a blob that has no such streams too
			const auto without = encodeLandscapeTile(sea.generate({ 0, 0 }) , c);   // (also a water tile: strip its streams by size instead)
			(void)without;
			for (size_t cut = blob.size() - 1; cut + 40 > blob.size() && cut > 0; --cut)
			{
				LandscapeTile x;
				CHECK_FALSE(decodeLandscapeTile(blob.data(), cut, x));
			}
		}
	}

	// a tile without water writes no flag and no streams, and the golden fixture of the format is untouched (test_landscape_store.cpp)
	LandscapeTile dry = t;
	dry.waterCoverage.clear();
	dry.waterOwner.clear();
	const auto dryBlob = encodeLandscapeTile(dry);
	CHECK((dryBlob[6] & 2) == 0);
	CHECK(dryBlob.size() < encodeLandscapeTile(t).size());
	LandscapeTile dryBack;
	REQUIRE(decodeLandscapeTile(dryBlob.data(), dryBlob.size(), dryBack));
	CHECK_FALSE(dryBack.hasWater());
	// a water tile whose raster is not the weights' is not written with water (a wrong-sized stream would be refused by every reader)
	LandscapeTile odd = t;
	odd.waterCoverage.resize(5);
	odd.waterOwner.resize(5);
	CHECK((encodeLandscapeTile(odd)[6] & 2) == 0);
}

TEST_CASE("water table: round trip, the bounds a reader holds, damaged tables refused")
{
	LandscapeWaterTable t;
	t.clipToGround = false;
	t.shoreOvershoot = 0.4f;
	t.tessellation = true;
	LandscapeWaterBody a;
	a.id = 3;
	a.level = 123.5f;
	a.physics.density = 1025.0f;
	a.physics.flow = { 1.5f, -0.5f };
	a.physics.enabled = false;
	a.underwaterFog.density = 1.3f;
	a.underwaterFog.shallowColor = { 0.1f, 0.2f, 0.3f };
	a.underwaterCaustics.intensity = 0.7f;
	a.tessellation.height = 0.9f;
	LandscapeWaterBody b;
	b.id = 900;
	b.level = -4.25f;
	t.bodies = { a, b };

	const auto blob = encodeLandscapeWaterTable(t);
	CHECK(blob.size() == kLandscapeWaterHeaderBytes + 10 + 2 * kLandscapeWaterBodyBytes);   // header, the settings and the count, two bodies
	LandscapeWaterTable back;
	std::string err;
	REQUIRE_MESSAGE(decodeLandscapeWaterTable(blob.data(), blob.size(), back, &err), err);
	REQUIRE(back.bodies.size() == 2);
	CHECK(back.clipToGround == false);
	CHECK(back.shoreOvershoot == doctest::Approx(0.4f));
	CHECK(back.tessellation);
	CHECK(back.bodies[0].id == 3);
	CHECK(back.bodies[0].level == doctest::Approx(123.5f));
	CHECK(back.bodies[0].physics == a.physics);
	CHECK(back.bodies[0].underwaterFog == a.underwaterFog);
	CHECK(back.bodies[0].underwaterCaustics == a.underwaterCaustics);
	CHECK(back.bodies[0].tessellation == a.tessellation);
	CHECK(back.bodies[1].level == doctest::Approx(-4.25f));
	CHECK(back.find(900) != nullptr);
	CHECK(back.find(4) == nullptr);

	// damaged: every truncation, a wrong magic, a size that does not match, a count the blob cannot hold, id 0, an id twice, a level that is NaN
	LandscapeWaterTable x;
	for (size_t n = 0; n < blob.size(); ++n) CHECK_FALSE(decodeLandscapeWaterTable(blob.data(), n, x));
	auto bad = blob; bad[0] ^= 0xFF;                                  CHECK_FALSE(decodeLandscapeWaterTable(bad.data(), bad.size(), x));
	bad = blob; bad[4] = 9;                                            CHECK_FALSE(decodeLandscapeWaterTable(bad.data(), bad.size(), x));
	bad = blob; bad[8] ^= 1;                                           CHECK_FALSE(decodeLandscapeWaterTable(bad.data(), bad.size(), x));
	bad = blob; bad[18] = 0xFF; bad[19] = 0xFF;                        CHECK_FALSE(decodeLandscapeWaterTable(bad.data(), bad.size(), x));   // count
	bad = blob; bad[kLandscapeWaterHeaderBytes + 10] = 0; bad[kLandscapeWaterHeaderBytes + 11] = 0;   CHECK_FALSE(decodeLandscapeWaterTable(bad.data(), bad.size(), x));   // id 0
	LandscapeWaterTable dup = t;
	dup.bodies[1].id = 3;
	const auto dupBlob = encodeLandscapeWaterTable(dup);
	CHECK_FALSE(decodeLandscapeWaterTable(dupBlob.data(), dupBlob.size(), x));
	LandscapeWaterTable nan = t;
	nan.bodies[0].level = std::nanf("");
	const auto nanBlob = encodeLandscapeWaterTable(nan);
	CHECK_FALSE(decodeLandscapeWaterTable(nanBlob.data(), nanBlob.size(), x));

	// a stored value outside its bounds is held to them
	LandscapeWaterTable wild = t;
	wild.bodies[0].physics.density = 1.0e9f;
	wild.bodies[0].physics.linearDrag = -3.0f;
	wild.shoreOvershoot = 1000.0f;
	const auto wildBlob = encodeLandscapeWaterTable(wild);
	REQUIRE(decodeLandscapeWaterTable(wildBlob.data(), wildBlob.size(), x));
	CHECK(x.bodies[0].physics.density <= water::kMaxDensity);
	CHECK(x.bodies[0].physics.linearDrag >= 0.0f);
	CHECK(x.shoreOvershoot <= water::kMaxShoreOvershoot);
}

TEST_CASE("pack water: the table is written and read, a pack without one has no water, a damaged table refuses the pack")
{
	const ProceduralLandscapeSource sea(seaWorld(3));
	const auto writePack = [&](const std::filesystem::path& path, bool withTable) {
		LandscapePackWriter w;
		REQUIRE(w.begin(path.string(), sea.geometry(), 3, 3, 3));
		if (withTable) w.setWaterTable(*sea.water());
		for (int z = 0; z < 3; ++z)
			for (int x = 0; x < 3; ++x) REQUIRE(w.add({ x, z }, encodeLandscapeTile(sea.generate({ x, z }))));
		REQUIRE(w.finish());
	};

	const auto with = tempFile("with"), without = tempFile("without");
	writePack(with, true);
	writePack(without, false);
	{
		std::string err;
		auto pack = LandscapePackSource::open(with.string(), &err);
		REQUIRE_MESSAGE(pack, err);
		REQUIRE(pack->water());
		REQUIRE(pack->water()->bodies.size() == 1);
		CHECK(pack->water()->bodies[0].id == 1);
		CHECK(pack->water()->bodies[0].level == doctest::Approx(1500.0f));
		LandscapeTile t;
		REQUIRE(pack->load({ 1, 1 }, -1, t));
		CHECK(t == sea.generate({ 1, 1 }));                             // the tiles carry their water through the pack
		CHECK(pack->coarse() != nullptr);                               // and the coarse world is where it was
	}
	{
		auto pack = LandscapePackSource::open(without.string());
		REQUIRE(pack);
		CHECK(pack->water() == nullptr);
		CHECK(pack->coarse() != nullptr);
	}
	// a damaged table: flip a byte of its magic (the table is the last thing in the file)
	{
		const auto bad = tempFile("bad");
		std::filesystem::copy_file(with, bad);
		const uint64_t size = std::filesystem::file_size(bad);
		const uint64_t tableSize = encodeLandscapeWaterTable(*sea.water()).size();
		{
			std::fstream f(bad, std::ios::in | std::ios::out | std::ios::binary);
			f.seekp(static_cast<std::streamoff>(size - tableSize));
			f.write("XXXX", 4);
		}
		std::string err;
		CHECK_FALSE(LandscapePackSource::open(bad.string(), &err));
		std::filesystem::remove(bad);
		// and one cut inside the table
		const auto cut = tempFile("cut");
		std::filesystem::copy_file(with, cut);
		std::filesystem::resize_file(cut, size - 10);
		CHECK_FALSE(LandscapePackSource::open(cut.string(), &err));
		std::filesystem::remove(cut);
	}
	std::filesystem::remove(with);
	std::filesystem::remove(without);
}

TEST_CASE("landscape store: a world with water counts the water rasters in its tile size, a world without is as before")
{
	auto dry = std::make_shared<ProceduralLandscapeSource>([] { auto p = seaWorld(); p.seaLevelM = -1.0e9f; return p; }());
	auto wet = std::make_shared<ProceduralLandscapeSource>(seaWorld());
	LandscapeStore a(dry, {}), b(wet, {});
	const LandscapeGeometry g = dry->geometry();
	CHECK(b.tileBytes() == a.tileBytes() + static_cast<size_t>(g.weightDim(0)) * g.weightDim(0) * 3);
	CHECK(a.tileBytes() == g.decodedBytes());
}

// ── a region of tiles as one terrain ────────────────────────────────────────

namespace
{
using TileFn = std::function<std::shared_ptr<const LandscapeTile>(LandscapeTileKey)>;

// Tiles of a procedural world, generated once each.
struct Tiles
{
    const ProceduralLandscapeSource& src;
    mutable std::map<std::pair<int, int>, std::shared_ptr<const LandscapeTile>> cache;
    std::shared_ptr<const LandscapeTile> operator()(LandscapeTileKey k) const
    {
        if (k.x < 0 || k.z < 0 || k.x >= src.tilesX() || k.z >= src.tilesZ()) return nullptr;
        auto& slot = cache[{ k.x, k.z }];
        if (!slot) slot = std::make_shared<LandscapeTile>(src.generate(k));
        return slot;
    }
};
} // namespace

TEST_CASE("landscape region terrain: n x n tiles are one terrain with the tiles' heights, weights and water, exactly")
{
	const ProceduralLandscapeSource sea(seaWorld(4));
	Tiles tiles{ sea };
	const TileFn fn = [&](LandscapeTileKey k) { return tiles(k); };
	LandscapeRegionTerrain r;
	REQUIRE(landscapeRegionTerrain(fn, { 1, 1 }, 2, { 0.0f, 3000.0f }, sea.water(), r));
	const TerrainComponent& tc = r.terrain;
	CHECK(r.n == 2);
	CHECK(r.centre == glm::dvec3(1024.0, 0.0, 1024.0));              // origin tile 1 + half of 2 tiles, 512 m each
	CHECK(tc.sizeX == doctest::Approx(1024.0f));
	REQUIRE(tc.resolution == 65u);                                    // 2 * 32 + 1
	REQUIRE(tc.sculptHeights.size() == 65u * 65u);

	// every sample of every tile is where it belongs; the samples two tiles share agree from both sides
	for (int tj = 0; tj < 2; ++tj)
		for (int ti = 0; ti < 2; ++ti)
		{
			const auto t = tiles({ 1 + ti, 1 + tj });
			for (int z = 0; z < 33; ++z)
				for (int x = 0; x < 33; ++x)
					REQUIRE(tc.sculptHeights[static_cast<size_t>(tj * 32 + z) * 65 + static_cast<size_t>(ti * 32 + x)] ==
					        doctest::Approx(t->heights[0][static_cast<size_t>(z) * 33 + x] * (3000.0f / 65535.0f)).epsilon(1e-5));
		}
	for (int z = 0; z <= 32; ++z)                                     // the border column between the two tiles of the first row
		REQUIRE(tiles({ 1, 1 })->heights[0][static_cast<size_t>(z) * 33 + 32] == tiles({ 2, 1 })->heights[0][static_cast<size_t>(z) * 33]);

	// weights through the paint API at region texels
	REQUIRE(tc.weightRes == 32u);
	for (int z = 0; z < 32; z += 3)
		for (int x = 0; x < 32; x += 3)
		{
			uint8_t w[kTerrainMaxLayers] = {};
			REQUIRE(TerrainPaint::texelWeights(tc, static_cast<uint32_t>(x), static_cast<uint32_t>(z), w));
			const auto t = tiles({ 1 + x / 16, 1 + z / 16 });
			for (int c = 0; c < 4; ++c) REQUIRE(w[c] == t->weights[0][(static_cast<size_t>(z % 16) * 16 + static_cast<size_t>(x % 16)) * 4 + static_cast<size_t>(c)]);
		}

	// the water field: allocated, at the weights' raster, with the pack's body, and its answers are the streams'
	REQUIRE(r.hasWater);
	const water::Field& f = tc.water;
	CHECK(f.allocated());
	CHECK(f.res == 32u);
	REQUIRE(f.bodies.size() == 1);
	CHECK(f.bodies[0].id == 1);
	CHECK(f.bodies[0].level == doctest::Approx(1500.0f));
	int wet = 0, dry = 0;
	for (int z = 0; z < 32; ++z)
		for (int x = 0; x < 32; ++x)
		{
			const auto t = tiles({ 1 + x / 16, 1 + z / 16 });
			const size_t src = static_cast<size_t>(z % 16) * 16 + static_cast<size_t>(x % 16);
			REQUIRE(f.coverage[static_cast<size_t>(z) * 32 + x] == t->waterCoverage[src]);
			REQUIRE(f.owner[static_cast<size_t>(z) * 32 + x] == t->waterOwner[src]);
			const float lx = (static_cast<float>(x) + 0.5f) / 32.0f * 1024.0f - 512.0f, lz = (static_cast<float>(z) + 0.5f) / 32.0f * 1024.0f - 512.0f;
			float level = -1.0f;
			if (t->waterCoverage[src] == 255)
			{
				++wet;
				REQUIRE(water::levelAt(tc, lx, lz, level));
				REQUIRE(level == doctest::Approx(1500.0f));
				REQUIRE(water::bodyAt(tc, lx, lz) == 1);
				REQUIRE(water::coverageAt(tc, lx, lz) >= 250.0f / 255.0f);
			}
			else
			{
				++dry;
				REQUIRE_FALSE(water::levelAt(tc, lx, lz, level));
				REQUIRE(water::bodyAt(tc, lx, lz) == water::kNoBody);
			}
		}
	CHECK(wet > 0);
	CHECK(dry > 0);
	CHECK(f.clipToGround);

	// a tile without water streams in the region (an authored world has dry tiles beside a lake): the Field is still built, dry there
	{
		const TileFn partly = [&](LandscapeTileKey k) {
			auto t = std::make_shared<LandscapeTile>(*tiles(k));
			if (k.x == 2 && k.z == 1) { t->waterCoverage.clear(); t->waterOwner.clear(); }
			return std::shared_ptr<const LandscapeTile>(t);
		};
		LandscapeRegionTerrain p;
		REQUIRE(landscapeRegionTerrain(partly, { 1, 1 }, 2, { 0.0f, 3000.0f }, sea.water(), p));
		REQUIRE(p.hasWater);
		REQUIRE(p.terrain.water.allocated());
		for (int z = 0; z < 16; ++z)
			for (int x = 16; x < 32; ++x)                            // tile (2, 1) covers region texels x 16..31, z 0..15
			{
				REQUIRE(p.terrain.water.coverage[static_cast<size_t>(z) * 32 + x] == 0);
				REQUIRE(p.terrain.water.owner[static_cast<size_t>(z) * 32 + x] == 0);
			}
		bool wetElsewhere = false;
		for (int x = 0; x < 16 && !wetElsewhere; ++x) wetElsewhere = p.terrain.water.coverage[static_cast<size_t>(x)] != 0 || p.terrain.water.coverage[static_cast<size_t>(16 * 32 + x)] != 0;
		CHECK(wetElsewhere);                                          // the other tiles' water is still there
	}
	// no table: heights and weights only
	LandscapeRegionTerrain bare;
	REQUIRE(landscapeRegionTerrain(fn, { 0, 0 }, 2, { 0.0f, 3000.0f }, nullptr, bare));
	CHECK_FALSE(bare.hasWater);
	CHECK_FALSE(bare.terrain.water.allocated());
	// a region that reaches outside the world, or one with a missing tile, is refused
	LandscapeRegionTerrain bad;
	CHECK_FALSE(landscapeRegionTerrain(fn, { 3, 3 }, 2, { 0.0f, 3000.0f }, sea.water(), bad));
	CHECK_FALSE(landscapeRegionTerrain(fn, { 0, 0 }, 0, { 0.0f, 3000.0f }, sea.water(), bad));
	CHECK_FALSE(landscapeRegionTerrain(TileFn{}, { 0, 0 }, 2, { 0.0f, 3000.0f }, sea.water(), bad));
}

TEST_CASE("landscape region terrain: four tiles of the real size fit a TerrainComponent, five do not")
{
	ProceduralLandscapeParams p;                  // 512 m, 256 cells
	p.tilesX = p.tilesZ = 6;
	p.seaLevelM = 1500.0f;
	const ProceduralLandscapeSource src(p);
	const auto one = std::make_shared<LandscapeTile>(src.generate({ 0, 0 }));
	const TileFn same = [&](LandscapeTileKey) { return one; };     // the same tile everywhere is enough to ask the size question
	LandscapeRegionTerrain r;
	REQUIRE(landscapeRegionTerrain(same, { 0, 0 }, 4, { 0.0f, 3000.0f }, src.water(), r));
	CHECK(r.terrain.resolution == 1025u);
	CHECK(r.terrain.sizeX == doctest::Approx(2048.0f));
	CHECK(r.terrain.water.res == 512u);               // 4 * 128 cells, 4 m each
	CHECK(r.terrain.water.allocated());
	LandscapeRegionTerrain too;
	CHECK_FALSE(landscapeRegionTerrain(same, { 0, 0 }, 5, { 0.0f, 3000.0f }, src.water(), too));   // 5 * 256 + 1 samples do not fit
}

// ── water around the camera ──────────────────────────────────────────────────
namespace
{
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

// Updates until a region exists whose origin is `origin`, and nothing is in flight.
bool settleWater(LandscapeWater& w, const LandscapeStore& store, const glm::dvec3& cam, const LandscapeWaterTable* table, LandscapeTileKey origin, int ms = 20000)
{
	const auto t0 = std::chrono::steady_clock::now();
	for (;;)
	{
		w.update(store, cam, { 0.0f, 3000.0f }, table);
		const auto r = w.region();
		if (r && r->origin == origin && !w.stats().building) return true;
		if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > ms) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}
} // namespace

TEST_CASE("landscape water: the region follows the camera in whole tiles and stays put while the camera stays in its inner tiles")
{
	auto src = std::make_shared<ProceduralLandscapeSource>(seaWorld(10));
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 6000.0f;
	LandscapeStore store(src, cfg);
	const glm::dvec3 cam(5 * 512.0 + 100.0, 600.0, 5 * 512.0 + 100.0);           // tile (5, 5)
	REQUIRE(settleStore(store, cam));

	LandscapeWater water;
	REQUIRE(settleWater(water, store, cam, src->water(), { 3, 3 }));              // 5 - 4/2
	const auto first = water.region();
	REQUIRE(first);
	CHECK(first->n == 4);
	CHECK(first->centre == glm::dvec3(5 * 512.0, 0.0, 5 * 512.0));
	const uint64_t started = water.stats().started;
	CHECK(started == 1);

	// inside the inner 2 x 2 tiles of the region (tiles 4 and 5 of 3..6): nothing is rebuilt, the region is the same object
	for (const glm::dvec3& c : { glm::dvec3(4 * 512.0 + 10.0, 600.0, 4 * 512.0 + 10.0), glm::dvec3(6 * 512.0 - 10.0, 600.0, 6 * 512.0 - 10.0),
	                              glm::dvec3(5 * 512.0, 600.0, 4 * 512.0 + 300.0) })
	{
		for (int i = 0; i < 20; ++i) water.update(store, c, { 0.0f, 3000.0f }, src->water());
		CHECK(water.stats().started == started);
		CHECK(water.region() == first);
	}

	// one tile out of the inner range: a new region, one tile on
	const glm::dvec3 east(6 * 512.0 + 10.0, 600.0, 5 * 512.0 + 100.0);
	REQUIRE(settleStore(store, east));
	REQUIRE(settleWater(water, store, east, src->water(), { 4, 3 }));
	CHECK(water.stats().started == started + 1);
	CHECK(water.region() != first);
	CHECK(water.region()->revision > first->revision);

	// at the world's edge the region does not leave the world
	const glm::dvec3 corner(20.0, 600.0, 20.0);
	REQUIRE(settleStore(store, corner));
	REQUIRE(settleWater(water, store, corner, src->water(), { 0, 0 }));
	const glm::dvec3 far(10 * 512.0 - 20.0, 600.0, 10 * 512.0 - 20.0);
	REQUIRE(settleStore(store, far));
	REQUIRE(settleWater(water, store, far, src->water(), { 6, 6 }));              // 10 - 4
	CHECK(water.region()->origin == LandscapeTileKey{ 6, 6 });

	// a world without water has no region, and a table that lists nothing is the same
	LandscapeWater none;
	for (int i = 0; i < 20; ++i) none.update(store, far, { 0.0f, 3000.0f }, nullptr);
	CHECK(none.region() == nullptr);
	LandscapeWaterTable empty;
	for (int i = 0; i < 20; ++i) none.update(store, far, { 0.0f, 3000.0f }, &empty);
	CHECK(none.region() == nullptr);
	CHECK(none.stats().started == 0);
}

TEST_CASE("landscape water: a region waits for tiles that are not in memory yet, and a world smaller than the region still gets one")
{
	auto src = std::make_shared<ProceduralLandscapeSource>(seaWorld(10));
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 1500.0f;
	LandscapeStore store(src, cfg);
	LandscapeWater water;
	// the store has not loaded anything: nothing can be built, and nothing crashes
	for (int i = 0; i < 10; ++i) water.update(store, glm::dvec3(2560.0, 600.0, 2560.0), { 0.0f, 3000.0f }, src->water());
	CHECK(water.region() == nullptr);
	CHECK(water.stats().started == 0);
	CHECK(water.stats().deferred > 0);

	auto tiny = std::make_shared<ProceduralLandscapeSource>(seaWorld(2));        // two tiles a side: the region is 2 x 2, not 4 x 4
	LandscapeStore tinyStore(tiny, cfg);
	REQUIRE(settleStore(tinyStore, glm::dvec3(500.0, 600.0, 500.0)));
	LandscapeWater w2;
	REQUIRE(settleWater(w2, tinyStore, glm::dvec3(500.0, 600.0, 500.0), tiny->water(), { 0, 0 }));
	CHECK(w2.region()->n == 2);
}

TEST_CASE("landscape water: the sheet of the sea lies at its level, inside the region, never over ground that stands above it, and crosses the tile borders inside")
{
	auto src = std::make_shared<ProceduralLandscapeSource>(seaWorld(8));
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 6000.0f;
	LandscapeStore store(src, cfg);
	const glm::dvec3 cam(4 * 512.0, 600.0, 4 * 512.0);
	REQUIRE(settleStore(store, cam));
	LandscapeWater water;
	REQUIRE(settleWater(water, store, cam, src->water(), { 2, 2 }));
	const auto r = water.region();
	REQUIRE(r);
	REQUIRE(r->terrain);
	REQUIRE(r->sheets.size() == 1);
	const LandscapeWaterSheet& sheet = r->sheets[0];
	CHECK(sheet.body == 1);
	CHECK(sheet.level == doctest::Approx(1500.0f));
	REQUIRE_FALSE(sheet.surface.empty());
	CHECK(sheet.params.id == 1);
	CHECK(sheet.surface.triangles > 0);

	const auto& mesh = sheet.surface.mesh;
	REQUIRE(mesh.vertices.size() % 3 == 0);
	const TerrainComponent& tc = *r->terrain;
	const double half = tc.sizeX * 0.5;
	// the steepest slope of the region's ground (per height cell) times a water cell (the lattice is one cell wide), both axes
	double maxGrad = 0.0;
	const float cellH = tc.sizeX / static_cast<float>(tc.resolution - 1), cellW = tc.sizeX / static_cast<float>(tc.water.res);
	for (uint32_t z = 0; z + 1 < tc.resolution; ++z)
		for (uint32_t x = 0; x + 1 < tc.resolution; ++x)
		{
			const float h = tc.sculptHeights[static_cast<size_t>(z) * tc.resolution + x];
			maxGrad = std::max({ maxGrad, std::abs(static_cast<double>(tc.sculptHeights[static_cast<size_t>(z) * tc.resolution + x + 1] - h)) / cellH,
			                     std::abs(static_cast<double>(tc.sculptHeights[static_cast<size_t>(z + 1) * tc.resolution + x] - h)) / cellH });
		}
	const float chordBound = static_cast<float>(maxGrad * cellW * 1.5);
	MESSAGE("steepest ground " << maxGrad << " m/m, water cell " << cellW << " m: the shore may be " << chordBound << " m off in height");
	size_t belowZero = 0, aboveZero = 0, zBelow = 0, zAbove = 0;
	for (size_t i = 0; i + 2 < mesh.vertices.size(); i += 3)
	{
		const double lx = sheet.surface.center.x + mesh.vertices[i], lz = sheet.surface.center.y + mesh.vertices[i + 2];
		REQUIRE(mesh.vertices[i + 1] == doctest::Approx(0.0f));            // the mesh sits at y = 0 under an entity at the level
		REQUIRE(std::abs(lx) <= half + 1e-3);
		REQUIRE(std::abs(lz) <= half + 1e-3);
		// where the sheet is, the ground does not stand above the water by more than the overshoot, give or take the contour's chord across a
		// cell (the shore is only as fine as the water grid): at most the steepest slope times the cell, taken both ways across it
		REQUIRE(terrainHeightAt(tc, static_cast<float>(lx), static_cast<float>(lz)) <= sheet.level + r->shoreOvershoot + chordBound);
		(lx < -1.0 ? belowZero : aboveZero) += (std::abs(lx) > 1.0);
		(lz < -1.0 ? zBelow : zAbove) += (std::abs(lz) > 1.0);
	}
	// The region's middle (local 0) is the border between its tiles 1 and 2: the sea passes across it, in both directions
	CHECK(belowZero > 0);
	CHECK(aboveZero > 0);
	CHECK(zBelow > 0);
	CHECK(zAbove > 0);

	// surfaceAt agrees with the field and the ground
	int wet = 0, dry = 0, outside = 0;
	for (double x = r->centre.x - half + 3.0; x < r->centre.x + half; x += 67.0)
		for (double z = r->centre.z - half + 3.0; z < r->centre.z + half; z += 67.0)
		{
			LandscapeWaterQuery q;
			const bool w = water.surfaceAt(x, z, q);
			const float lx = static_cast<float>(x - r->centre.x), lz = static_cast<float>(z - r->centre.z);
			float level = 0.0f;
			const bool cellWet = water::levelAt(tc, lx, lz, level);
			const float ground = terrainHeightAt(tc, lx, lz);
			REQUIRE(w == (cellWet && ground <= level + r->shoreOvershoot));
			REQUIRE(q.wet == w);
			if (w)
			{
				++wet;
				REQUIRE(q.body == 1);
				REQUIRE(q.level == doctest::Approx(1500.0f));
				REQUIRE(q.depth == doctest::Approx(std::max(0.0f, 1500.0f - ground)));
				REQUIRE(q.params.id == 1);
			}
			else ++dry;
		}
	CHECK(wet > 0);
	CHECK(dry > 0);
	LandscapeWaterQuery q;
	CHECK_FALSE(water.surfaceAt(r->centre.x + half + 50.0, r->centre.z, q));           // outside the region: no water here
	CHECK_FALSE(water.surfaceAt(-100.0, -100.0, q));
	(void)outside;
}

TEST_CASE("landscape water: destroying it with a build running waits for the build")
{
	auto src = std::make_shared<ProceduralLandscapeSource>(seaWorld(8));
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 6000.0f;
	LandscapeStore store(src, cfg);
	REQUIRE(settleStore(store, glm::dvec3(2048.0, 600.0, 2048.0)));
	{
		LandscapeWater water;
		water.update(store, glm::dvec3(2048.0, 600.0, 2048.0), { 0.0f, 3000.0f }, src->water());
		CHECK(water.stats().started == 1);
	}   // the build was started a moment ago and is waited for here
	const auto held = store.tile({ 4, 4 });
	REQUIRE(held);
	// the tiles of the region are the store's and ours; nothing of the finished (or cancelled) build still holds one
	for (int i = 0; i < 100 && held.use_count() > 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	CHECK(held.use_count() == 2);
}

TEST_CASE("landscape water: a straight bank across a tile border gives a straight shore, exactly where the ground meets the level")
{
	// Four tiles whose ground is a plane rising to the east (0.2 m per metre), crossing the sea level at x = 2048 (the border between tile
	// 3 and 4); every cell is wet (coverage 255) and the ground decides, so the sheet is the west half and its shore is the line x = 2048.
	const ProceduralLandscapeSource base(seaWorld(8));
	const auto tileAt = [&](LandscapeTileKey k) -> std::shared_ptr<const LandscapeTile> {
		if (k.x < 0 || k.z < 0 || k.x >= 8 || k.z >= 8) return nullptr;
		auto t = std::make_shared<LandscapeTile>(base.generate(k));
		const int n = t->geometry.heightDim(0);
		for (int z = 0; z < n; ++z)
			for (int x = 0; x < n; ++x)
			{
				const double xAbs = k.x * 512.0 + x * (512.0 / (n - 1));
				const double metres = 1500.0 + 0.2 * (xAbs - 2048.0);
				t->heights[0][static_cast<size_t>(z) * n + x] = static_cast<uint16_t>(std::clamp(std::lround(metres / 3000.0 * 65535.0), 0l, 65535l));
			}
		t->heights.resize(1);
		t->buildMips();
		std::fill(t->waterCoverage.begin(), t->waterCoverage.end(), uint8_t(255));
		std::fill(t->waterOwner.begin(), t->waterOwner.end(), uint16_t(1));
		return t;
	};
	LandscapeRegionTerrain rt;
	REQUIRE(landscapeRegionTerrain(tileAt, { 2, 2 }, 4, { 0.0f, 3000.0f }, base.water(), rt));
	water::SurfaceOptions opt;
	opt.clipToGround = rt.terrain.water.clipToGround;
	opt.shoreOvershoot = rt.terrain.water.shoreOvershoot;
	water::SurfaceMesh sm;
	REQUIRE(water::buildSurface(rt.terrain, 1, opt, sm));
	REQUIRE_FALSE(sm.empty());
	CHECK(rt.centre.x == 2048.0);                                              // the shore is the middle of the region
	size_t onShore = 0, west = 0;
	double minX = 1e9, maxX = -1e9;
	for (size_t i = 0; i + 2 < sm.mesh.vertices.size(); i += 3)
	{
		const double lx = sm.center.x + sm.mesh.vertices[i];
		minX = std::min(minX, lx);
		maxX = std::max(maxX, lx);
		// overshoot 0.05 m / slope 0.2 = 0.25 m east of the middle; the heights are 16 bit (4.6 cm a step, 0.23 m along this slope), so within that
		if (std::abs(lx) < 2.0) { ++onShore; REQUIRE(std::abs(lx - 0.25) < 0.3); }
		else REQUIRE(lx < 0.0);                                                // every other vertex is on the water's side
		if (lx < -1000.0) ++west;
	}
	CHECK(onShore >= 2);                                                       // the shore line has its ends (and the sheet is a half of the region)
	CHECK(west >= 2);                                                          // and the far side reaches the region's border
	CHECK(std::abs(minX + 1024.0) < 0.01);                                     // the far side is the region's border
	CHECK(maxX < 1.0);
	// The area of the sheet is the water half: 1024 x 2048 m (plus a sliver for the overshoot)
	double area = 0.0;
	for (size_t t = 0; t + 2 < sm.mesh.indices.size(); t += 3)
	{
		const auto vtx = [&](uint32_t i) { return glm::dvec2(sm.mesh.vertices[i * 3], sm.mesh.vertices[i * 3 + 2]); };
		const glm::dvec2 a = vtx(sm.mesh.indices[t]), b = vtx(sm.mesh.indices[t + 1]), c = vtx(sm.mesh.indices[t + 2]);
		area += std::abs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) * 0.5;
	}
	CHECK(area == doctest::Approx(1024.0 * 2048.0 + 0.25 * 2048.0).epsilon(1e-3));
}
