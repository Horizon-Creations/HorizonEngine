// HE::LandscapePackBuild (Thema 326, step 6): the editor's three ways to make a tile landscape. A migrated TerrainComponent comes back out of
// the pack as the same heights and weights (the check that "the landscape is in the tile store"), a procedural pack serves what the source
// generates, a tiled height map import puts every pixel where the grid says, and a cancelled or failed build leaves the old pack alone.
#include "doctest.h"
#include "TestFsUtil.h"
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/LandscapePackBuild.h>
#include <HorizonScene/LandscapeSource.h>
#include <HorizonScene/LandscapeStore.h>
#include <HorizonScene/LandscapeTileTerrain.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace HE;
namespace fs = std::filesystem;

namespace
{
fs::path tempDir(const char* tag)
{
	static int n = 0;
	const fs::path p = fs::temp_directory_path() / ("he_packbuild_" + std::string(tag) + "_" + std::to_string(++n) + "_" +
	                                                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	he_test::removeAllQuiet(p);
	fs::create_directories(p);
	return p;
}

LandscapeGeometry smallGeometry()
{
	LandscapeGeometry g;
	g.tileM = 512.0f;
	g.hCells = 32;
	g.wTexels = 16;
	g.channels = 12;
	return g;
}

// A terrain of 4 x 4 small tiles whose cell IS the tile's raster: 2048 m over 129 samples = 16 m = 512 / 32.
TerrainComponent alignedTerrain(bool paint)
{
	TerrainComponent tc;
	tc.sizeX = tc.sizeZ = 2048.0f;
	tc.resolution = 129;
	tc.sculptHeights.resize(129u * 129u);
	for (uint32_t z = 0; z < 129; ++z)
		for (uint32_t x = 0; x < 129; ++x)
			tc.sculptHeights[z * 129 + x] = 100.0f + 3.0f * static_cast<float>(x) + 1.5f * static_cast<float>(z) + ((x * 7 + z * 13) % 5);
	if (paint)
	{
		tc.weightRes = 128;   // 2048 m / 128 = 16 m a texel; the small tile's texel is 512 / 16 = 32 m: 2 x 2 terrain texels
		tc.layerWeights.assign(128u * 128u * 4, 0);
		tc.layerWeights2.assign(128u * 128u * 4, 0);
		for (uint32_t z = 0; z < 128; ++z)
			for (uint32_t x = 0; x < 128; ++x)
			{
				// Constant over 2 x 2 blocks, so the tile texel (the mean of the four) is exact.
				const uint32_t bx = x / 2, bz = z / 2;
				uint8_t* a = &tc.layerWeights[(z * 128 + x) * 4];
				uint8_t* b = &tc.layerWeights2[(z * 128 + x) * 4];
				a[0] = static_cast<uint8_t>(100 + (bx % 4) * 10);   // layer 0
				a[1] = static_cast<uint8_t>(60 + (bz % 3) * 10);    // layer 1
				b[2] = static_cast<uint8_t>(255 - a[0] - a[1]);     // layer 6
			}
	}
	return tc;
}
} // namespace

TEST_CASE("pack build: a migrated terrain comes back out of the store as the same landscape")
{
	const fs::path dir = tempDir("migrate");
	const TerrainComponent tc = alignedTerrain(true);
	LandscapeMigrateParams mp;
	mp.geometry = smallGeometry();
	const auto r = migrateTerrainToPack(tc, (dir / "world.hlp").string(), mp);
	REQUIRE_MESSAGE(r.ok, r.error);
	CHECK(r.tilesX == 4);
	CHECK(r.tilesZ == 4);
	CHECK(r.tilesWritten == 16);
	CHECK(r.heightScaleM > 0.0f);
	CHECK_FALSE(fs::exists(dir / "world.hlp.part"));

	std::string err;
	const auto pack = LandscapePackSource::open((dir / "world.hlp").string(), &err);
	REQUIRE_MESSAGE(pack, err);
	float mn = 0.0f, sc = 0.0f;
	REQUIRE(pack->heightRange(mn, sc));
	CHECK(mn == r.heightMinM);
	CHECK(sc == r.heightScaleM);
	const double quantum = static_cast<double>(sc) / 65535.0;

	// Every tile, every height sample within half a quantum of the terrain, shared edges identical on both sides.
	for (int tz = 0; tz < 4; ++tz)
		for (int tx = 0; tx < 4; ++tx)
		{
			LandscapeTile t;
			REQUIRE(pack->load({ tx, tz }, -1, t));
			REQUIRE(t.heights.size() == static_cast<size_t>(t.geometry.heightLevels()));
			for (int z = 0; z <= 32; ++z)
				for (int x = 0; x <= 32; ++x)
				{
					const double metres = mn + t.heights[0][static_cast<size_t>(z) * 33 + x] * quantum;
					const double want = tc.sculptHeights[static_cast<size_t>(tz * 32 + z) * 129 + static_cast<size_t>(tx * 32 + x)];
					REQUIRE(std::abs(metres - want) <= quantum * 0.51);
				}
		}

	// The weights: 12 channels, layers 0, 1 (page 0) and 6 (page 1), summing to 255.
	LandscapeTile t;
	REQUIRE(pack->load({ 2, 1 }, 1, t));
	for (int z = 0; z < 16; ++z)
		for (int x = 0; x < 16; ++x)
		{
			const uint8_t* p = &t.weights[0][(static_cast<size_t>(z) * 16 + x) * 12];
			// Global terrain texel of this tile texel: (2*16 + x) * 2 ...: the block index is (tile texel) itself.
			const uint32_t bx = static_cast<uint32_t>(2 * 16 + x), bz = static_cast<uint32_t>(1 * 16 + z);
			CHECK(p[0] == 100 + (bx % 4) * 10);
			CHECK(p[1] == 60 + (bz % 3) * 10);
			CHECK(p[6] == 255 - p[0] - p[1]);
			int sum = 0;
			for (int k = 0; k < 12; ++k) sum += p[k];
			CHECK(sum == 255);
		}

	// And the store, which is what the editor and the game read, holds the same tile.
	LandscapeStoreConfig sc2;
	sc2.loadRadius = 600.0f;
	sc2.byteBudget = 64ull * 1024 * 1024;
	LandscapeStore store(pack, sc2);
	const LandscapeAnchor anchor{ glm::dvec3(2.5 * 512.0, 0.0, 1.5 * 512.0) };
	for (int i = 0; i < 400 && !store.settled(); ++i)
	{
		store.update({ anchor }, 5.0);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	REQUIRE(store.settled());
	const auto resident = store.tile({ 2, 1 });
	REQUIRE(resident);
	CHECK(resident->heights[0] == t.heights[0]);

	// The inverse view: the tile as a TerrainComponent is the piece of the original it was cut from.
	LandscapeTileTerrainParams tp;
	tp.heightMinM = mn;
	tp.heightScaleM = sc;
	const TerrainComponent back = landscapeTileTerrain(*resident, tp);
	REQUIRE(back.sculptHeights.size() == 33u * 33u);
	for (int z = 0; z <= 32; ++z)
		for (int x = 0; x <= 32; ++x)
			REQUIRE(std::abs(static_cast<double>(back.sculptHeights[static_cast<size_t>(z) * 33 + x]) - tc.sculptHeights[static_cast<size_t>(32 + z) * 129 + static_cast<size_t>(64 + x)]) <= quantum * 0.51);
	he_test::removeAllQuiet(dir);
}

TEST_CASE("pack build: an unpainted terrain is layer 0 everywhere; a given range is kept; a terrain that is no multiple of a tile is clamped at its edge")
{
	const fs::path dir = tempDir("odd");
	TerrainComponent tc;
	tc.sizeX = 1000.0f;
	tc.sizeZ = 700.0f;
	tc.resolution = 65;
	tc.sculptHeights.assign(65u * 65u, 0.0f);
	for (uint32_t z = 0; z < 65; ++z)
		for (uint32_t x = 0; x < 65; ++x) tc.sculptHeights[z * 65 + x] = 20.0f + static_cast<float>(x) * 2.0f;   // 20 .. 148 m along x

	LandscapeMigrateParams mp;
	mp.geometry = smallGeometry();
	mp.heightMinM = -10.0f;
	mp.heightScaleM = 500.0f;
	mp.yOffsetM = 5.0f;
	const auto r = migrateTerrainToPack(tc, (dir / "odd.hlp").string(), mp);
	REQUIRE_MESSAGE(r.ok, r.error);
	CHECK(r.tilesX == 2);   // 1000 m = 2 tiles of 512
	CHECK(r.tilesZ == 2);   // 700 m = 2 tiles
	CHECK(r.heightMinM == -10.0f);
	CHECK(r.heightScaleM == 500.0f);

	const auto pack = LandscapePackSource::open((dir / "odd.hlp").string());
	REQUIRE(pack);
	LandscapeTile t;
	REQUIRE(pack->load({ 0, 0 }, 1, t));
	const auto metres = [&](int x, int z) { return -10.0 + t.heights[0][static_cast<size_t>(z) * 33 + x] * 500.0 / 65535.0; };
	// x = 16 samples = 256 m: the terrain's x index 256 / 1000 * 64 = 16.384 -> 20 + 2 * 16.384 = 52.77, plus the 5 m offset.
	CHECK(metres(16, 0) == doctest::Approx(52.768 + 5.0).epsilon(0.001));
	CHECK(metres(0, 0) == doctest::Approx(25.0).epsilon(0.001));
	LandscapeTile edge;
	REQUIRE(pack->load({ 1, 0 }, 1, edge));
	// Past the terrain's east edge (x > 1000 m) the height repeats the edge: 148 + 5.
	CHECK(-10.0 + edge.heights[0][32] * 500.0 / 65535.0 == doctest::Approx(153.0).epsilon(0.001));
	for (size_t i = 0; i < 16u * 16u; ++i) CHECK(t.weights[0][i * 12] == 255);   // no paint: layer 0

	// A fitted range: min = floor(lowest), scale = the span above it.
	mp.heightScaleM = 0.0f;
	mp.yOffsetM = 0.0f;
	const auto fit = migrateTerrainToPack(tc, (dir / "fit.hlp").string(), mp);
	REQUIRE(fit.ok);
	CHECK(fit.heightMinM == 20.0f);
	CHECK(fit.heightScaleM == 128.0f);

	TerrainComponent empty;
	empty.sizeX = 0.0f;
	CHECK_FALSE(migrateTerrainToPack(empty, (dir / "no.hlp").string(), mp).ok);
	CHECK_FALSE(fs::exists(dir / "no.hlp"));
	he_test::removeAllQuiet(dir);
}

TEST_CASE("pack build: a procedural pack serves what the source generates, and replaces an older pack without a trace")
{
	const fs::path dir = tempDir("proc");
	ProceduralLandscapeParams p;
	p.geometry = smallGeometry();
	p.tilesX = 5;
	p.tilesZ = 3;
	p.seed = 77;
	const std::string path = (dir / "p.hlp").string();
	const auto r = buildProceduralPack(path, p);
	REQUIRE_MESSAGE(r.ok, r.error);
	CHECK(r.tilesWritten == 15);
	CHECK(r.heightScaleM == p.heightScale);
	CHECK_FALSE(r.summary().empty());

	const auto pack = LandscapePackSource::open(path);
	REQUIRE(pack);
	CHECK(pack->tilesX() == 5);
	CHECK(pack->tilesZ() == 3);
	CHECK(pack->coarse() != nullptr);
	const ProceduralLandscapeSource source(p);
	for (const LandscapeTileKey k : { LandscapeTileKey{ 0, 0 }, LandscapeTileKey{ 4, 2 }, LandscapeTileKey{ 2, 1 } })
	{
		LandscapeTile a;
		REQUIRE(pack->load(k, -1, a));
		CHECK(a == source.generate(k));
	}

	// Again with another seed over the same path: the old pack is replaced, no `.part` is left, the new pack is the new world.
	p.seed = 78;
	const auto again = buildProceduralPack(path, p);
	REQUIRE(again.ok);
	CHECK_FALSE(fs::exists(path + ".part"));
	const auto pack2 = LandscapePackSource::open(path);
	REQUIRE(pack2);
	LandscapeTile b;
	REQUIRE(pack2->load({ 2, 1 }, -1, b));
	CHECK(b == ProceduralLandscapeSource(p).generate({ 2, 1 }));
	CHECK_FALSE(b == source.generate({ 2, 1 }));
	he_test::removeAllQuiet(dir);
}

TEST_CASE("pack build: a cancelled build keeps the old pack; a bad geometry or path says so")
{
	const fs::path dir = tempDir("cancel");
	ProceduralLandscapeParams p;
	p.geometry = smallGeometry();
	p.tilesX = p.tilesZ = 4;
	const std::string path = (dir / "keep.hlp").string();
	REQUIRE(buildProceduralPack(path, p).ok);
	const auto before = fs::file_size(path);

	LandscapeBuildOptions opt;
	opt.threads = 1;
	opt.progress = [](size_t, size_t) { return false; };
	p.seed = 5;
	const auto r = buildProceduralPack(path, p, opt);
	CHECK_FALSE(r.ok);
	CHECK(r.cancelled);
	CHECK(r.summary() == "Cancelled.");
	CHECK(fs::file_size(path) == before);
	CHECK_FALSE(fs::exists(path + ".part"));

	p.geometry.hCells = 30;   // not a power of two
	const auto bad = buildProceduralPack(path, p);
	CHECK_FALSE(bad.ok);
	CHECK_FALSE(bad.error.empty());
	CHECK_FALSE(buildProceduralPack("", ProceduralLandscapeParams{}).ok);
	he_test::removeAllQuiet(dir);
}

namespace
{
// Writes a headerless 16-bit little-endian square (.r16) whose pixel (x, y) of the whole grid is f(gx, gy).
template <class F>
void writeR16(const fs::path& file, int side, int fx, int fz, F f)
{
	std::vector<uint16_t> px(static_cast<size_t>(side) * side);
	for (int y = 0; y < side; ++y)
		for (int x = 0; x < side; ++x) px[static_cast<size_t>(y) * side + x] = f(fx * side + x, fz * side + y);
	std::ofstream o(file, std::ios::binary);
	o.write(reinterpret_cast<const char*>(px.data()), static_cast<std::streamsize>(px.size() * 2));
}
uint16_t gradient(int gx, int gy) { return static_cast<uint16_t>(gx * 700 + gy * 90); }
} // namespace

TEST_CASE("pack build: a tiled height map set is found, ordered, and every pixel lands where the grid says")
{
	const fs::path dir = tempDir("hm");
	// 2 x 2 files of 17 x 17: a 34 x 34 grid. Names as World Machine writes them; a stray file and another set's file are ignored.
	for (int fz = 0; fz < 2; ++fz)
		for (int fx = 0; fx < 2; ++fx) writeR16(dir / ("Terrain_x" + std::to_string(fx) + "_y" + std::to_string(fz) + ".r16"), 17, fx, fz, gradient);
	{ std::ofstream o(dir / "notes.txt"); o << "x"; }
	writeR16(dir / "Other_x0_y0.r16", 17, 0, 0, gradient);

	const LandscapeHeightmapFiles found = findHeightmapTileFiles(dir.string());
	REQUIRE_MESSAGE(found.error.empty(), found.error);
	CHECK(found.filesX == 2);
	CHECK(found.filesZ == 2);
	REQUIRE(found.files.size() == 4);
	CHECK(fs::path(found.files[1]).filename() == "Terrain_x1_y0.r16");
	CHECK(fs::path(found.files[2]).filename() == "Terrain_x0_y1.r16");

	LandscapeHeightmapImportParams ip;
	ip.geometry = smallGeometry();
	ip.metersPerPixel = 16.0f;   // = the tile raster (512 / 32): a sample is a pixel
	ip.heightMinM = 0.0f;
	ip.heightScaleM = 1000.0f;
	const std::string path = (dir / "world.hlp").string();
	const auto r = importHeightmapToPack(found, path, ip);
	REQUIRE_MESSAGE(r.ok, r.error);
	CHECK(r.tilesX == 2);   // 33 pixel gaps * 16 m = 528 m: two tiles
	CHECK(r.tilesZ == 2);
	CHECK(r.heightScaleM == 1000.0f);

	const auto pack = LandscapePackSource::open(path);
	REQUIRE(pack);
	LandscapeTile t00, t11;
	REQUIRE(pack->load({ 0, 0 }, -1, t00));
	REQUIRE(pack->load({ 1, 1 }, -1, t11));
	for (int z = 0; z <= 32; ++z)
		for (int x = 0; x <= 32; ++x) CHECK(t00.heights[0][static_cast<size_t>(z) * 33 + x] == gradient(x, z));
	// The second tile starts at pixel 32 and the grid ends at pixel 33: the rest repeats the last column/row.
	CHECK(t11.heights[0][0] == gradient(32, 32));
	CHECK(t11.heights[0][1] == gradient(33, 32));
	CHECK(t11.heights[0][2] == gradient(33, 32));
	CHECK(t11.heights[0][33 * 2] == gradient(32, 33));
	// autoWeights: 12 channels, each texel sums to 255 (the ramp is gentle: mostly layer 0).
	for (size_t i = 0; i < 16u * 16u; ++i)
	{
		int sum = 0;
		for (int k = 0; k < 12; ++k) sum += t00.weights[0][i * 12 + static_cast<size_t>(k)];
		CHECK(sum == 255);
	}

	// flipY reverses the file rows: file row 1 is then the northern one.
	ip.flipY = true;
	ip.autoWeights = false;
	const auto fr = importHeightmapToPack(found, path, ip);
	REQUIRE(fr.ok);
	const auto flipped = LandscapePackSource::open(path);
	REQUIRE(flipped);
	LandscapeTile f00;
	REQUIRE(flipped->load({ 0, 0 }, -1, f00));
	CHECK(f00.heights[0][0] == gradient(0, 17));        // the first row of file y1
	CHECK(f00.heights[0][33 * 16] == gradient(0, 33));  // z = 16 is still file row 1 (its last picture row) ...
	CHECK(f00.heights[0][33 * 17] == gradient(0, 0));   // ... and z = 17 is the first row of file row 0
	CHECK(f00.weights[0][0] == 255);                    // autoWeights off: layer 0
	he_test::removeAllQuiet(dir);
}

TEST_CASE("pack build: holes read as the lowest ground, a mismatched or unreadable file fails the import and leaves no pack")
{
	const fs::path dir = tempDir("hmbad");
	writeR16(dir / "T_x0_y0.r16", 17, 0, 0, gradient);
	writeR16(dir / "T_x1_y1.r16", 17, 1, 1, gradient);   // x1_y0 and x0_y1 are holes
	const LandscapeHeightmapFiles found = findHeightmapTileFiles(dir.string());
	REQUIRE(found.error.empty());
	CHECK(found.files[1].empty());
	CHECK(found.files[2].empty());

	LandscapeHeightmapImportParams ip;
	ip.geometry = smallGeometry();
	ip.metersPerPixel = 16.0f;
	const std::string path = (dir / "w.hlp").string();
	REQUIRE(importHeightmapToPack(found, path, ip).ok);
	const auto pack = LandscapePackSource::open(path);
	REQUIRE(pack);
	LandscapeTile t;
	REQUIRE(pack->load({ 1, 0 }, -1, t));
	// Tile (1,0) starts at pixel 32 of file row 0, whose east file (x1_y0) is a hole.
	CHECK(t.heights[0][0] == 0);

	// A file of another size.
	writeR16(dir / "T_x1_y0.r16", 9, 1, 0, gradient);
	const LandscapeHeightmapFiles mixed = findHeightmapTileFiles(dir.string());
	const std::string path2 = (dir / "m.hlp").string();
	const auto bad = importHeightmapToPack(mixed, path2, ip);
	CHECK_FALSE(bad.ok);
	CHECK(bad.error.find("pixels") != std::string::npos);
	CHECK_FALSE(fs::exists(path2));
	CHECK_FALSE(fs::exists(path2 + ".part"));

	// No files, no folder, wrong parameters.
	const LandscapeHeightmapFiles none = findHeightmapTileFiles((dir / "nope").string());
	CHECK_FALSE(none.error.empty());
	CHECK_FALSE(importHeightmapToPack(none, path2, ip).ok);
	LandscapeHeightmapImportParams bp = ip;
	bp.metersPerPixel = 0.0f;
	CHECK_FALSE(importHeightmapToPack(found, path2, bp).ok);
	// One picture is a set of one; a missing one is an error in words.
	const auto one = importHeightmapToPack(singleHeightmapFile((dir / "missing.r16").string()), path2, ip);
	CHECK_FALSE(one.ok);
	CHECK_FALSE(one.error.empty());
	he_test::removeAllQuiet(dir);
}

TEST_CASE("pack build: a build job runs on its own thread, reports progress, and hands out its result once")
{
	const fs::path dir = tempDir("job");
	ProceduralLandscapeParams p;
	p.geometry = smallGeometry();
	p.tilesX = p.tilesZ = 4;
	const std::string path = (dir / "j.hlp").string();
	LandscapeBuildJob job;
	REQUIRE(job.start("Procedural", [&](const LandscapeBuildOptions& o) { return buildProceduralPack(path, p, o); }));
	CHECK_FALSE(job.start("again", [](const LandscapeBuildOptions&) { return LandscapeBuildResult{}; }));   // one at a time
	LandscapeBuildResult r;
	bool got = false;
	for (int i = 0; i < 2000 && !got; ++i)
	{
		got = job.poll(r);
		if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	REQUIRE(got);
	CHECK(r.ok);
	CHECK(job.total() == 16);
	CHECK(job.done() == 16);
	CHECK_FALSE(job.running());
	CHECK_FALSE(job.poll(r));   // handed out once
	he_test::removeAllQuiet(dir);
}
