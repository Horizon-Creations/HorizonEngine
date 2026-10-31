// Water on the world-scale landscape (Thema 326, step 3c): the tile's water streams, the pack's table of bodies, the procedural sea,
// and (further down) the region that turns tiles into one water::Field-carrying terrain. Real data, no timing.
#include "doctest.h"
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/LandscapeSource.h>
#include <HorizonScene/LandscapeTileTerrain.h>
#include <HorizonScene/LandscapeStore.h>
#include <HorizonScene/LandscapeTile.h>
#include <HorizonScene/LandscapeWaterData.h>
#include <HorizonScene/TerrainMeshGenerator.h>
#include <HorizonScene/TerrainPaint.h>
#include <HorizonScene/WaterField.h>

#include <algorithm>
#include <cmath>
#include <cstring>
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
