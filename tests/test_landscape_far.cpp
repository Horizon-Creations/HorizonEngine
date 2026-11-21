// The baked far image of a tile landscape (Thema 326): LandscapeFarMap.h (table, mips), LandscapeFarBake.h (plan, painter, the hash that
// decides which regions an export paints again) and its place in the pack (LandscapeSource.h, the tail sections).
#include "doctest.h"
#include <ContentManager/ContentManager.h>
#include <HorizonScene/Components/LandscapeWorldComponent.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LandscapeExport.h>
#include <HorizonScene/LandscapeRuntime.h>
#include <HorizonScene/LandscapeCellHlod.h>
#include <HorizonScene/LandscapeCellHlodBake.h>
#include <HorizonScene/LandscapeFarBake.h>
#include <HorizonScene/LandscapeFarMap.h>
#include <HorizonScene/LandscapeSource.h>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace
{
HE::ProceduralLandscapeParams smallWorld(int tiles)
{
	HE::ProceduralLandscapeParams p;
	p.geometry.hCells = 32;
	p.geometry.wTexels = 16;
	p.geometry.channels = 4;
	p.tilesX = p.tilesZ = tiles;
	return p;
}

HE::LandscapeFarPaint fourColours()
{
	HE::LandscapeFarPaint paint;
	paint.layerColor.fill(glm::vec3(0.0f));
	paint.layerAo.fill(1.0f);
	paint.layerColor[0] = { 0.20f, 0.40f, 0.10f };
	paint.layerColor[1] = { 0.40f, 0.40f, 0.40f };
	paint.layerColor[2] = { 0.80f, 0.70f, 0.40f };
	paint.layerColor[3] = { 0.90f, 0.90f, 0.90f };
	paint.layerAo[1] = 0.5f;
	return paint;
}

std::filesystem::path farPath(const char* name)
{
	return std::filesystem::temp_directory_path() / (std::string("he_landscape_far_") + name + "_" + std::to_string(std::random_device{}()) + ".bin");
}

HE::LandscapeFarTable bakedTable(const HE::ProceduralLandscapeSource& src, int regionTiles)
{
	HE::LandscapeFarBakeOptions o;
	o.regionTiles = regionTiles;
	o.painter = HE::landscapeFarWeightPainter(fourColours());
	o.painterHash = HE::landscapeFarPaintHash(fourColours());
	HE::LandscapeFarTable t;
	HE::LandscapeFarBakeStats s;
	HE::bakeLandscapeFar(src, [](HE::LandscapeTileKey k) { return static_cast<uint64_t>(k.x * 31 + k.z + 1); }, o, nullptr, t, s);
	REQUIRE(s.baked == s.regions);
	return t;
}

void writePack(const std::filesystem::path& path, const HE::ProceduralLandscapeSource& src, const HE::LandscapeHlodTable* hlod,
               const HE::LandscapeFarTable* farTable)
{
	HE::LandscapePackWriter w;
	REQUIRE(w.begin(path.string(), src.geometry(), src.tilesX(), src.tilesZ(), 3));
	if (hlod) w.setHlodTable(*hlod);
	if (farTable) w.setFarTable(*farTable);
	for (int z = 0; z < src.tilesZ(); ++z)
		for (int x = 0; x < src.tilesX(); ++x) REQUIRE(w.add({ x, z }, HE::encodeLandscapeTile(src.generate({ x, z }))));
	REQUIRE(w.finish());
}

HE::LandscapeHlodTable oneProxy()
{
	HE::LandscapeHlodSourceMesh tri;
	tri.positions = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };
	tri.indices = { 0, 1, 2 };
	HE::LandscapeHlodTable t;
	t.set(2, 5, HE::bakeCellHlod({ tri }));
	return t;
}
} // namespace

TEST_CASE("planLandscapeFar: a small world keeps the weights' raster, a 100 km one is scaled down to the budget and the texture limit")
{
	HE::LandscapeGeometry g;   // the default tile: 128 weight texels
	const HE::LandscapeFarLayout small = HE::planLandscapeFar(g, 20, 20, 128ull << 20, 8192, 8);
	CHECK(small.texelsPerTile == 128);   // 2560 px a side, 35 MB with mips
	const HE::LandscapeFarLayout world = HE::planLandscapeFar(g, 196, 196, 128ull << 20, 8192, 8);
	CHECK(world.texelsPerTile == 16);    // 32: 6272 px = 210 MB with mips, over the budget
	CHECK(world.width() == 196 * 16);
	CHECK(world.regionsX() == 25);       // 196 / 8 rounded up: the last region hangs over the edge
	// The texture limit bites before the budget does.
	CHECK(HE::planLandscapeFar(g, 196, 196, 1ull << 40, 4096, 8).texelsPerTile == 16);
	// Never below one texel a tile, however small the budget.
	CHECK(HE::planLandscapeFar(g, 196, 196, 1, 8192, 8).texelsPerTile == 1);
}

TEST_CASE("landscapeFarWeightPainter: a far texel is the tile shader's blend of the weights it covers")
{
	const HE::ProceduralLandscapeSource src(smallWorld(2));
	const HE::LandscapeFarPaint paint = fourColours();
	// 4 texels a tile from 16 weight texels: every far texel covers 4 x 4 weight texels.
	HE::LandscapeFarLayout layout{ 2, 2, 2, 4 };
	std::vector<uint8_t> rgba;
	REQUIRE(HE::landscapeFarWeightPainter(paint)(src, layout, 0, 0, rgba));
	REQUIRE(rgba.size() == 8u * 8u * 4u);

	int worst = 0;
	for (int tz = 0; tz < 2; ++tz)
		for (int tx = 0; tx < 2; ++tx)
		{
			const HE::LandscapeTile tile = src.generate({ tx, tz });
			const std::vector<uint8_t>& w0 = tile.weights[0];   // from level 0, not from the mip the painter reads
			for (int z = 0; z < 4; ++z)
				for (int x = 0; x < 4; ++x)
				{
					glm::vec3 albedo(0.0f);
					float ao = 1.0f;
					for (int dz = 0; dz < 4; ++dz)
						for (int dx = 0; dx < 4; ++dx)
							for (int c = 0; c < 4; ++c)
							{
								const float wt = static_cast<float>(w0[(static_cast<size_t>(z * 4 + dz) * 16 + static_cast<size_t>(x * 4 + dx)) * 4 + static_cast<size_t>(c)]) / 255.0f / 16.0f;
								albedo += wt * paint.layerColor[static_cast<size_t>(c)];
								ao += wt * (paint.layerAo[static_cast<size_t>(c)] - 1.0f);
							}
					const uint8_t* got = &rgba[(static_cast<size_t>(tz * 4 + z) * 8 + static_cast<size_t>(tx * 4 + x)) * 4];
					worst = std::max(worst, std::abs(static_cast<int>(got[0]) - static_cast<int>(HE::landscapeFarEncodeSrgb(albedo.r))));
					worst = std::max(worst, std::abs(static_cast<int>(got[1]) - static_cast<int>(HE::landscapeFarEncodeSrgb(albedo.g))));
					worst = std::max(worst, std::abs(static_cast<int>(got[2]) - static_cast<int>(HE::landscapeFarEncodeSrgb(albedo.b))));
					worst = std::max(worst, std::abs(static_cast<int>(got[3]) - static_cast<int>(std::lround(ao * 255.0f))));
				}
		}
	CHECK(worst <= 3);   // the mips are rounded 2 x 2 means of bytes: two levels of rounding, steep at the dark end of sRGB

	// Control: the image follows the colours it is given.
	HE::LandscapeFarPaint other = paint;
	for (glm::vec3& c : other.layerColor) c = glm::vec3(1.0f) - c;
	std::vector<uint8_t> rgba2;
	REQUIRE(HE::landscapeFarWeightPainter(other)(src, layout, 0, 0, rgba2));
	CHECK(rgba2 != rgba);
	CHECK(HE::landscapeFarPaintHash(other) != HE::landscapeFarPaintHash(paint));
}

TEST_CASE("bakeLandscapeFar: only the regions whose tiles, painter or layout changed are painted again")
{
	const HE::ProceduralLandscapeSource src(smallWorld(5));   // 5 tiles, regions of 2: 3 x 3 regions, the last ones hang over
	std::map<std::pair<int, int>, uint64_t> hashes;
	const HE::LandscapeFarTileHash tileHash = [&](HE::LandscapeTileKey k) {
		const auto it = hashes.find({ k.x, k.z });
		return it != hashes.end() ? it->second : static_cast<uint64_t>(1000 + k.x + k.z * 5);
	};
	int painted = 0;
	const HE::LandscapeFarPainter inner = HE::landscapeFarWeightPainter(fourColours());
	HE::LandscapeFarBakeOptions o;
	o.regionTiles = 2;
	o.painterHash = 7;
	o.painter = [&](const HE::LandscapeTileSource& s, const HE::LandscapeFarLayout& l, int rx, int rz, std::vector<uint8_t>& rgba) {
		++painted;
		return inner(s, l, rx, rz, rgba);
	};

	HE::LandscapeFarTable first;
	HE::LandscapeFarBakeStats stats;
	HE::bakeLandscapeFar(src, tileHash, o, nullptr, first, stats);
	CHECK(stats.regions == 9);
	CHECK(stats.baked == 9);
	CHECK(stats.reused == 0);
	CHECK(painted == 9);
	CHECK_FALSE(first.empty());

	// Nothing changed: nothing is painted, and the table is the same.
	painted = 0;
	HE::LandscapeFarTable second;
	HE::bakeLandscapeFar(src, tileHash, o, &first, second, stats);
	CHECK(stats.reused == 9);
	CHECK(stats.baked == 0);
	CHECK(painted == 0);
	CHECK(second.encode() == first.encode());

	// One tile changed: its region and no other. Tile (3, 0) lies in region (1, 0).
	hashes[{ 3, 0 }] = 42;
	painted = 0;
	HE::LandscapeFarTable third;
	HE::bakeLandscapeFar(src, tileHash, o, &second, third, stats);
	CHECK(stats.baked == 1);
	CHECK(stats.reused == 8);
	CHECK(painted == 1);
	CHECK(third.regions[1].hash != second.regions[1].hash);
	CHECK(third.regions[0].hash == second.regions[0].hash);

	// The painter's settings are part of every hash.
	HE::LandscapeFarBakeOptions o2 = o;
	o2.painterHash = 8;
	HE::bakeLandscapeFar(src, tileHash, o2, &third, second, stats);
	CHECK(stats.baked == 9);

	// A previous table of another layout is no cache.
	HE::LandscapeFarBakeOptions o3 = o;
	o3.regionTiles = 4;
	HE::bakeLandscapeFar(src, tileHash, o3, &third, second, stats);
	CHECK(stats.regions == 4);
	CHECK(stats.baked == 4);
	CHECK(stats.reused == 0);

	// A cancel keeps what was painted and leaves the rest unbaked.
	HE::LandscapeFarBakeOptions o4 = o;
	o4.progress = [](int done, int) { return done < 3; };
	HE::bakeLandscapeFar(src, tileHash, o4, nullptr, second, stats);
	CHECK(stats.cancelled);
	CHECK(stats.baked == 3);
	CHECK_FALSE(second.regions[2].rgba.empty());
	CHECK(second.regions[3].rgba.empty());

	// A painter that fails leaves its region unbaked and is counted.
	HE::LandscapeFarBakeOptions o5 = o;
	o5.painter = [&](const HE::LandscapeTileSource& s, const HE::LandscapeFarLayout& l, int rx, int rz, std::vector<uint8_t>& rgba) {
		return rx == 1 && rz == 1 ? false : inner(s, l, rx, rz, rgba);
	};
	HE::bakeLandscapeFar(src, tileHash, o5, nullptr, second, stats);
	CHECK(stats.failed == 1);
	CHECK(stats.baked == 8);
	CHECK(second.regions[4].rgba.empty());
}

TEST_CASE("LandscapeFarTable: round trip, damaged bytes refused, the assembled image puts every region where its tiles are")
{
	const HE::ProceduralLandscapeSource src(smallWorld(5));
	const HE::LandscapeFarTable table = bakedTable(src, 2);
	const std::vector<uint8_t> bytes = table.encode();

	HE::LandscapeFarTable back;
	REQUIRE(HE::LandscapeFarTable::decode(bytes, back));
	CHECK(back.layout == table.layout);
	REQUIRE(back.regions.size() == table.regions.size());
	for (size_t i = 0; i < table.regions.size(); ++i)
	{
		CHECK(back.regions[i].hash == table.regions[i].hash);
		CHECK(back.regions[i].rgba == table.regions[i].rgba);
	}
	// Every truncation and a wrong magic are refused, and leave nothing behind.
	for (size_t cut = 0; cut < bytes.size(); cut += std::max<size_t>(1, bytes.size() / 97))
	{
		HE::LandscapeFarTable t;
		CHECK_FALSE(HE::LandscapeFarTable::decode(std::vector<uint8_t>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut)), t));
		CHECK(t.regions.empty());
	}
	std::vector<uint8_t> wrong = bytes;
	wrong[0] ^= 0xFF;
	CHECK_FALSE(HE::LandscapeFarTable::decode(wrong, back));
	std::vector<uint8_t> longer = bytes;
	longer.push_back(0);
	CHECK_FALSE(HE::LandscapeFarTable::decode(longer, back));

	// Assembled: 5 tiles x 16 texels. Tile (3, 4) sits in region (1, 2) at tile (1, 0) of it.
	int w = 0, h = 0;
	std::vector<std::vector<uint8_t>> mips;
	table.assemble(w, h, mips);
	CHECK(w == 80);
	CHECK(h == 80);
	REQUIRE(mips.size() == 7);   // 80, 40, 20, 10, 5, 2, 1
	CHECK(mips[4].size() == 5u * 5u * 4u);
	CHECK(mips[6].size() == 4u);
	const int side = table.layout.regionSide();
	const HE::LandscapeFarRegion& r = table.regions[2 * 3 + 1];
	for (const int px : { 0, 7, 15 })
	{
		const uint8_t* fromWorld = &mips[0][(static_cast<size_t>(4 * 16 + px) * 80 + static_cast<size_t>(3 * 16 + px)) * 4];
		const uint8_t* fromRegion = &r.rgba[(static_cast<size_t>(0 * 16 + px) * static_cast<size_t>(side) + static_cast<size_t>(1 * 16 + px)) * 4];
		for (int c = 0; c < 4; ++c) CHECK(fromWorld[c] == fromRegion[c]);
	}
}

TEST_CASE("LandscapeFarTable: mips are the mean in linear light, not of the sRGB bytes")
{
	HE::LandscapeFarTable t;
	t.layout = { 1, 1, 1, 2 };
	t.regions.resize(1);
	// Two black and two white texels: half the light is sRGB 188, the byte mean would be 128.
	t.regions[0].rgba = { 0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 255 };
	t.regions[0].hash = 1;
	int w = 0, h = 0;
	std::vector<std::vector<uint8_t>> mips;
	t.assemble(w, h, mips);
	REQUIRE(mips.size() == 2);
	CHECK(mips[1][0] == 188);
	CHECK(mips[1][3] == 255);
	// An unbaked table assembles to nothing.
	HE::LandscapeFarTable none;
	none.layout = t.layout;
	none.regions.resize(1);
	CHECK(none.empty());
	none.assemble(w, h, mips);
	CHECK(mips.empty());
}

TEST_CASE("pack far image: written by the writer or set afterwards, beside the cell HLOD table in either order, and a pack without one reads as before")
{
	const HE::ProceduralLandscapeSource src(smallWorld(2));
	const HE::LandscapeFarTable farTable = bakedTable(src, 2);
	const HE::LandscapeHlodTable hlod = oneProxy();
	std::string err;

	// The writer: far alone, HLOD alone, both, neither.
	for (int mask = 0; mask < 4; ++mask)
	{
		const auto path = farPath("writer");
		writePack(path, src, (mask & 1) ? &hlod : nullptr, (mask & 2) ? &farTable : nullptr);
		auto p = HE::LandscapePackSource::open(path.string(), &err);
		REQUIRE_MESSAGE(p, err);
		CHECK((p->hlod() != nullptr) == ((mask & 1) != 0));
		CHECK((p->farImage() != nullptr) == ((mask & 2) != 0));
		if (p->farImage()) CHECK(p->farImage()->encode() == farTable.encode());
		if (p->hlod()) CHECK(p->hlod()->find(2, 5) != nullptr);
		HE::LandscapeTile t;
		CHECK(p->load({ 1, 1 }, -1, t));
		CHECK(p->coarse() != nullptr);
		p.reset();
		std::filesystem::remove(path);
	}

	// Set afterwards, in both orders: each call keeps the other section.
	for (const bool farFirst : { true, false })
	{
		const auto path = farPath("set");
		writePack(path, src, nullptr, nullptr);
		const auto plain = std::filesystem::file_size(path);
		if (farFirst)
		{
			REQUIRE_MESSAGE(HE::landscapePackSetTails(path.string(), nullptr, &farTable, &err), err);
			REQUIRE_MESSAGE(HE::landscapePackSetHlod(path.string(), hlod, &err), err);
		}
		else
		{
			REQUIRE_MESSAGE(HE::landscapePackSetHlod(path.string(), hlod, &err), err);
			REQUIRE_MESSAGE(HE::landscapePackSetTails(path.string(), nullptr, &farTable, &err), err);
		}
		{
			auto p = HE::LandscapePackSource::open(path.string(), &err);
			REQUIRE_MESSAGE(p, err);
			REQUIRE(p->farImage());
			REQUIRE(p->hlod());
			CHECK(p->farImage()->encode() == farTable.encode());
			CHECK(p->hlod()->cellCount() == 1);
		}
		// Replacing one does not grow the file by a second copy, and removing both gives the plain pack back.
		const auto both = std::filesystem::file_size(path);
		REQUIRE(HE::landscapePackSetTails(path.string(), nullptr, &farTable, &err));
		CHECK(std::filesystem::file_size(path) == both);
		const HE::LandscapeFarTable noFar;
		const HE::LandscapeHlodTable noHlod;
		REQUIRE(HE::landscapePackSetTails(path.string(), &noHlod, &noFar, &err));
		CHECK(std::filesystem::file_size(path) == plain);
		auto p = HE::LandscapePackSource::open(path.string(), &err);
		REQUIRE_MESSAGE(p, err);
		CHECK(p->farImage() == nullptr);
		CHECK(p->hlod() == nullptr);
		p.reset();
		std::filesystem::remove(path);
	}
}

TEST_CASE("pack far image: a damaged table refuses the pack, and a tile's stored hash follows its bytes")
{
	const HE::ProceduralLandscapeSource src(smallWorld(2));
	const HE::LandscapeFarTable farTable = bakedTable(src, 2);
	const auto path = farPath("damaged");
	writePack(path, src, nullptr, &farTable);
	std::string err;
	{
		auto p = HE::LandscapePackSource::open(path.string(), &err);
		REQUIRE_MESSAGE(p, err);
		// Two tiles of the fBm world differ, and the same tile hashes the same twice.
		CHECK(p->storedHash({ 0, 0 }) != p->storedHash({ 1, 0 }));
		CHECK(p->storedHash({ 0, 0 }) == p->storedHash({ 0, 0 }));
		CHECK(p->storedHash({ 0, 0 }) != 0);
		CHECK(p->storedHash({ 9, 9 }) == 0);   // no such tile
	}
	// Flip a byte inside the far table (its magic, right where the trailer says it starts).
	const auto size = std::filesystem::file_size(path);
	std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
	uint8_t tr[8];
	f.seekg(static_cast<std::streamoff>(size - 20));
	f.read(reinterpret_cast<char*>(tr), 8);
	uint64_t off = 0;
	for (int i = 7; i >= 0; --i) off = (off << 8) | tr[i];
	f.seekp(static_cast<std::streamoff>(off));
	const char bad = 0x7f;
	f.write(&bad, 1);
	f.close();
	CHECK(HE::LandscapePackSource::open(path.string(), &err) == nullptr);
	std::filesystem::remove(path);
}

// ── the export: LandscapeExport.h ────────────────────────────────────────────
TEST_CASE("export far image: baked into the shipped copy, a second export paints nothing, a changed world is painted again, off and cancelled ship none")
{
	namespace fs = std::filesystem;
	const fs::path root = fs::temp_directory_path() / ("he_landscape_far_export_" + std::to_string(std::random_device{}()));
	fs::create_directories(root);
	const fs::path packFile = root / "world.pack";
	{
		const HE::ProceduralLandscapeSource src(smallWorld(3));
		writePack(packFile, src, nullptr, nullptr);
	}
	const auto bytesOf = [](const fs::path& f) {
		std::ifstream in(f, std::ios::binary);
		return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	};
	const std::vector<uint8_t> before = bytesOf(packFile);

	ContentManager cm{ "." };   // no texture group arrays here: the painter gets the flat layer colours
	HE::LandscapeExportOptions opt;
	opt.farCacheDir = root / "Saved" / "LandscapeFar";
	const auto run = [&](const HE::LandscapeExportOptions& o, HE::LandscapeExportPlan& plan, std::vector<std::string>& notes) {
		HorizonWorld world;
		const Entity e = world.createEntity("Landscape");
		LandscapeWorldComponent lw;
		lw.pack = "world.pack";
		world.registry().emplace<LandscapeWorldComponent>(e, lw);
		HE::prepareLandscapePacksForExport(world, root, cm, o, plan, [&](int, const std::string& s) { notes.push_back(s); });
	};

	// First export: every region is painted, the table is in the plan and the cache is written.
	HE::LandscapeExportPlan first;
	std::vector<std::string> notes;
	run(opt, first, notes);
	REQUIRE(first.packs.size() == 1);
	CHECK(first.farBake.regions == 1);   // 3 x 3 tiles in one region of 8 x 8
	CHECK(first.farBake.baked == 1);
	CHECK(first.farBake.reused == 0);
	REQUIRE(first.packs[0].farTable);
	CHECK(first.packs[0].farTable->layout.texelsPerTile == 16);   // the weights' own raster: a small world is not scaled down
	CHECK(fs::is_regular_file(opt.farCacheDir / "world.far"));
	bool said = false;
	for (const std::string& n : notes) said = said || n.find("far image") != std::string::npos;
	CHECK(said);

	// The exporter's half on a copy: the copy has the image, the author's pack is as it was.
	const fs::path copy = root / "shipped.pack";
	fs::copy_file(packFile, copy);
	std::string err;
	REQUIRE_MESSAGE(HE::finishExportedPack(first.packs[0], copy, &err), err);
	{
		auto p = HE::LandscapePackSource::open(copy.string(), &err);
		REQUIRE_MESSAGE(p, err);
		REQUIRE(p->farImage());
		CHECK(p->farImage()->encode() == first.packs[0].farTable->encode());
	}
	CHECK(bytesOf(packFile) == before);

	// Second export of the same world: nothing is painted, and the same image ships.
	HE::LandscapeExportPlan second;
	run(opt, second, notes);
	CHECK(second.farBake.baked == 0);
	CHECK(second.farBake.reused == 1);
	REQUIRE(second.packs[0].farTable);
	CHECK(second.packs[0].farTable->encode() == first.packs[0].farTable->encode());

	// The world changed (other tiles in the same file): painted again.
	{
		HE::ProceduralLandscapeParams p = smallWorld(3);
		p.seed = 99;
		const HE::ProceduralLandscapeSource src(p);
		writePack(packFile, src, nullptr, nullptr);
	}
	HE::LandscapeExportPlan third;
	run(opt, third, notes);
	CHECK(third.farBake.baked == 1);
	CHECK(third.farBake.reused == 0);
	REQUIRE(third.packs[0].farTable);
	CHECK(third.packs[0].farTable->encode() != first.packs[0].farTable->encode());

	// Switched off: no image, and finishing the copy leaves it alone.
	HE::LandscapeExportOptions off = opt;
	off.bakeFar = false;
	HE::LandscapeExportPlan none;
	run(off, none, notes);
	CHECK(none.packs[0].farTable == nullptr);
	CHECK(none.farBake.regions == 0);

	// Cancelled: the pack ships without one rather than with half of one.
	HE::LandscapeExportOptions stop = opt;
	stop.farCacheDir.clear();
	stop.farProgress = [](int, int) { return false; };
	HE::LandscapeExportPlan cancelled;
	run(stop, cancelled, notes);
	CHECK(cancelled.farBake.cancelled);
	CHECK(cancelled.packs[0].farTable == nullptr);

	fs::remove_all(root);
}

TEST_CASE("landscapeFarPaintFor: without the group's arrays every layer keeps the runtime's flat colour and no occlusion")
{
	ContentManager cm{ "." };
	const HE::LandscapeFarPaint flat = HE::landscapeFarPaintFor(-1, cm), group = HE::landscapeFarPaintFor(0, cm);
	const auto colours = HE::LandscapeRuntimeConfig::defaultLayerColors();
	for (size_t i = 0; i < 12; ++i)
	{
		CHECK(flat.layerColor[i] == glm::vec3(colours[i]));
		CHECK(group.layerColor[i] == glm::vec3(colours[i]));   // the arrays are not in this content root
		CHECK(flat.layerAo[i] == 1.0f);
	}
}
