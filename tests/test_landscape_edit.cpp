// Sculpt and paint on a tile landscape that write back (Thema 326): LandscapeEdit.h. The overlay in front of the source, the way from an
// edited region terrain back into tiles, the session a tool drives (strokes, undo), the edits file beside the pack and baking it in.
#include "doctest.h"
#include <HorizonScene/LandscapeEdit.h>
#include <HorizonScene/LandscapeRuntime.h>
#include <HorizonScene/LandscapeExport.h>
#include <HorizonScene/LandscapeStamps.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/LandscapeStampComponent.h>
#include <HorizonScene/Components/LandscapeWorldComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/ContentManager.h>
#include <HorizonScene/LandscapeSource.h>
#include <HorizonScene/LandscapeTileTerrain.h>
#include <HorizonScene/TerrainPaint.h>
#include <HorizonScene/TerrainSculpt.h>
#include <HorizonScene/WaterBrush.h>
#include <HorizonScene/LandscapeFoliage.h>
#include <HorizonScene/Components/FoliageComponent.h>

#include <glm/gtc/matrix_transform.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <thread>

using namespace HE;

namespace
{
// The real geometry (512 m tiles, 257 heights, 128 weight texels, 12 channels): region sizes and shared borders are the real ones.
std::shared_ptr<ProceduralLandscapeSource> world(int tiles = 12)
{
	ProceduralLandscapeParams p;
	p.tilesX = p.tilesZ = tiles;
	return std::make_shared<ProceduralLandscapeSource>(p);
}

// Counts what is read from the base, so "an edited tile never reads the base" can be seen.
class CountingSource final : public LandscapeTileSource
{
public:
	explicit CountingSource(std::shared_ptr<const LandscapeTileSource> base) : m_base(std::move(base)) {}
	const LandscapeGeometry& geometry() const override { return m_base->geometry(); }
	int  tilesX() const override { return m_base->tilesX(); }
	int  tilesZ() const override { return m_base->tilesZ(); }
	bool load(LandscapeTileKey key, int maxLevels, LandscapeTile& out) const override
	{
		if (key.x == 1 && key.z == 1) ++loadsOfOneOne;
		return m_base->load(key, maxLevels, out);
	}
	bool heightRange(float& a, float& b) const override { return m_base->heightRange(a, b); }
	const LandscapeCoarseWorld* coarse() const override { return m_base->coarse(); }
	mutable std::atomic<int> loadsOfOneOne{ 0 };

private:
	std::shared_ptr<const LandscapeTileSource> m_base;
};

LandscapeTilePtr fullTile(const LandscapeTileSource& s, LandscapeTileKey k)
{
	auto t = std::make_shared<LandscapeTile>();
	REQUIRE(s.load(k, -1, *t));
	return t;
}

bool settle(LandscapeRuntime& rt, const glm::dvec3& cam, int ms = 30000)
{
	const auto t0 = std::chrono::steady_clock::now();
	for (;;)
	{
		rt.updateAt(cam, glm::vec3(0.0f), 5.0);
		if (rt.store()->settled() && rt.atlas()->stats().used == rt.store()->residentKeys().size()) return true;
		if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > ms) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

std::filesystem::path tempFile(const char* name)
{
	return std::filesystem::temp_directory_path() / (std::string("he_landscape_edit_") + name + "_" + std::to_string(std::random_device{}()));
}
} // namespace

TEST_CASE("TerrainSculpt::ramp: the corridor between two points becomes the straight line between their heights, nothing outside it moves")
{
	TerrainComponent tc;
	tc.sizeX = tc.sizeZ = 128.0f;
	tc.resolution = 129;   // a vertex every metre, local -64..64 (2^n + 1: ensureHeights keeps it)
	TerrainSculpt::ensureHeights(tc);
	const std::vector<float> before = tc.sculptHeights;
	// from (-30, 0) at 2 m to (30, 0) at 14 m, 5 m wide with no falloff, all the way
	const auto r = TerrainSculpt::ramp(tc, -30.0f, 0.0f, 2.0f, 30.0f, 0.0f, 14.0f, 5.0f, 0.0f, 1.0f);
	REQUIRE(r.ok);
	CHECK(r.changed > 0u);
	CHECK(tc.regionDirty);
	REQUIRE(tc.resolution == 129u);
	const auto at = [&](int x, int z) { return tc.sculptHeights[static_cast<size_t>(z + 64) * 129 + static_cast<size_t>(x + 64)]; };
	CHECK(at(-30, 0) == doctest::Approx(2.0f));
	CHECK(at(0, 0) == doctest::Approx(8.0f));     // half way
	CHECK(at(30, 0) == doctest::Approx(14.0f));
	CHECK(at(0, 3) == doctest::Approx(8.0f));     // inside the corridor's width
	CHECK(at(0, 10) == doctest::Approx(before[static_cast<size_t>(74) * 129 + 64]));   // beside it: untouched
	CHECK(at(-45, 0) == doctest::Approx(before[static_cast<size_t>(64) * 129 + 19]));  // before its start: untouched
	// a segment of no length is refused, and a half blend goes half way
	CHECK_FALSE(TerrainSculpt::ramp(tc, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 5.0f, 5.0f, 0.0f, 1.0f).ok);
	TerrainComponent half;
	half.sizeX = half.sizeZ = 128.0f;
	half.resolution = 129;
	TerrainSculpt::ensureHeights(half);
	const float h0 = half.sculptHeights[static_cast<size_t>(64) * 129 + 64];
	TerrainSculpt::ramp(half, -30.0f, 0.0f, 2.0f, 30.0f, 0.0f, 14.0f, 5.0f, 0.0f, 0.5f);
	CHECK(half.sculptHeights[static_cast<size_t>(64) * 129 + 64] == doctest::Approx(h0 + 0.5f * (8.0f - h0)));
}

TEST_CASE("landscapeWriteRegionTerrain: an untouched region changes no tile, a dab changes the tiles that hold its samples and keeps shared borders equal")
{
	const auto src = world();
	const LandscapeGeometry g = src->geometry();
	LandscapeTileTerrainParams params;
	REQUIRE(src->heightRange(params.heightMinM, params.heightScaleM));
	const auto current = [&](LandscapeTileKey k) { return fullTile(*src, k); };
	const LandscapeTileKey origin{ 3, 4 };
	LandscapeRegionTerrain region;
	REQUIRE(landscapeRegionTerrain(current, origin, 2, params, nullptr, region));
	const uint32_t res = region.terrain.resolution;
	REQUIRE(res == 2u * g.hCells + 1u);

	// The round trip is exact: 16 bit -> metres -> 16 bit.
	std::vector<std::pair<LandscapeTileKey, LandscapeTilePtr>> out;
	REQUIRE(landscapeWriteRegionTerrain(region.terrain, g, origin, 2, params, current, out));
	CHECK(out.empty());

	// One sample inside tile (3, 4): that tile alone.
	TerrainComponent edited = region.terrain;
	edited.sculptHeights[static_cast<size_t>(100) * res + 100] += 25.0f;
	REQUIRE(landscapeWriteRegionTerrain(edited, g, origin, 2, params, current, out));
	REQUIRE(out.size() == 1);
	CHECK(out[0].first == origin);
	CHECK(out[0].second->heights.size() == static_cast<size_t>(g.heightLevels()));   // every mip is there again
	const float step = params.heightScaleM / 65535.0f;
	const int dim = g.hCells + 1;
	const float got = params.heightMinM + static_cast<float>(out[0].second->heights[0][static_cast<size_t>(100) * dim + 100]) * step;
	CHECK(got == doctest::Approx(edited.sculptHeights[static_cast<size_t>(100) * res + 100]).epsilon(0.0).scale(1.0).epsilon(step));

	// The sample in the very middle is the corner four tiles share: all four change, and all four hold the same value there.
	edited = region.terrain;
	edited.sculptHeights[static_cast<size_t>(g.hCells) * res + g.hCells] += 40.0f;
	REQUIRE(landscapeWriteRegionTerrain(edited, g, origin, 2, params, current, out));
	REQUIRE(out.size() == 4);
	uint16_t corner[4] = {};
	for (const auto& [key, tile] : out)
	{
		const int cx = key.x == origin.x ? g.hCells : 0, cz = key.z == origin.z ? g.hCells : 0;
		corner[(key.z - origin.z) * 2 + (key.x - origin.x)] = tile->heights[0][static_cast<size_t>(cz) * dim + cx];
	}
	CHECK(corner[0] == corner[1]);
	CHECK(corner[0] == corner[2]);
	CHECK(corner[0] == corner[3]);
	CHECK(corner[0] != fullTile(*src, origin)->heights[0][static_cast<size_t>(g.hCells) * dim + g.hCells]);

	// Paint: one weight texel of page 2 (channel 9) in tile (4, 5).
	edited = region.terrain;
	const uint32_t wr = edited.weightRes;
	REQUIRE(wr == 2u * g.wTexels);
	uint8_t& texel = edited.layerWeights3[(static_cast<size_t>(g.wTexels + 7) * wr + g.wTexels + 5) * 4 + 1];
	texel = static_cast<uint8_t>(texel ^ 0x55);
	REQUIRE(landscapeWriteRegionTerrain(edited, g, origin, 2, params, current, out));
	REQUIRE(out.size() == 1);
	CHECK(out[0].first == LandscapeTileKey{ 4, 5 });
	CHECK(out[0].second->weights[0][(static_cast<size_t>(7) * g.wTexels + 5) * g.channels + 9] == texel);

	// A terrain that is not this region is refused.
	CHECK_FALSE(landscapeWriteRegionTerrain(edited, g, origin, 4, params, current, out));
	// A height outside the 16 bit range is clamped, not wrapped.
	edited = region.terrain;
	edited.sculptHeights[static_cast<size_t>(50) * res + 50] = params.heightMinM + params.heightScaleM * 2.0f;
	REQUIRE(landscapeWriteRegionTerrain(edited, g, origin, 2, params, current, out));
	REQUIRE(out.size() == 1);
	CHECK(out[0].second->heights[0][static_cast<size_t>(50) * dim + 50] == 65535);
}

TEST_CASE("LandscapeEditSource: an edited tile answers from the overlay and never reads the base, the coarse world follows, a removed edit gives the base back")
{
	const auto base = std::make_shared<CountingSource>(world());
	LandscapeEditSource edits(base);
	const LandscapeGeometry g = edits.geometry();
	const LandscapeTileKey key{ 1, 1 };
	CHECK(edits.coarse() == base->coarse());   // no edit: the base's coarse world, not a copy
	CHECK_FALSE(edits.dirty());

	auto tile = std::make_shared<LandscapeTile>(*fullTile(*base, key));
	for (uint16_t& h : tile->heights[0]) h = 40000;
	tile->buildMips();
	const int readsBefore = base->loadsOfOneOne.load();
	edits.set({ { key, tile } });
	CHECK(edits.dirty());
	LandscapeTile got;
	REQUIRE(edits.load(key, -1, got));
	CHECK(got.heights[0] == tile->heights[0]);
	REQUIRE(edits.load(key, 2, got));   // the store asks for the first levels only
	CHECK(got.heights.size() == 2);
	CHECK(base->loadsOfOneOne.load() == readsBefore);
	REQUIRE(edits.load({ 2, 2 }, -1, got));   // the others still come from the base
	CHECK(got.heights[0] == fullTile(*base, { 2, 2 })->heights[0]);
	CHECK(edits.editedKeys().size() == 1);

	// The coarse world: a copy, with the tile's share in it. The middle of the tile on grid 0.
	const LandscapeCoarseWorld* coarse = edits.coarse();
	REQUIRE(coarse);
	CHECK(coarse != base->coarse());
	const int cpt = coarse->cellsPerTile;
	CHECK(coarse->heights[0].at(key.x * cpt + cpt / 2, key.z * cpt + cpt / 2) == 40000);
	CHECK(base->coarse()->heights[0].at(key.x * cpt + cpt / 2, key.z * cpt + cpt / 2) != 40000);
	// ... and on the grids above, which are sampled from it.
	REQUIRE(coarse->heights.size() > 1);
	CHECK(coarse->heights[1].at((key.x * cpt + cpt / 2) / 2, (key.z * cpt + cpt / 2) / 2) == 40000);

	// The hash follows the content; an unedited tile asks the base's.
	const auto baseHash = [](LandscapeTileKey) { return uint64_t{ 7 }; };
	CHECK(edits.contentHash(key, baseHash) != 7);
	CHECK(edits.contentHash({ 2, 2 }, baseHash) == 7);

	// Taken away: the base's tile, and the base's share of the coarse world.
	edits.set({ { key, nullptr } });
	REQUIRE(edits.load(key, -1, got));
	CHECK(got.heights[0] == fullTile(*base, key)->heights[0]);
	CHECK(edits.coarse()->heights[0].at(key.x * cpt + cpt / 2, key.z * cpt + cpt / 2) == base->coarse()->heights[0].at(key.x * cpt + cpt / 2, key.z * cpt + cpt / 2));
	CHECK(edits.editedKeys().empty());
}

TEST_CASE("LandscapeEditSource: the edits file round-trips, a file cut short keeps its whole records, an empty overlay removes it")
{
	const auto base = world();
	const std::filesystem::path file = tempFile("sidecar");
	std::vector<uint8_t> bytes;
	{
		LandscapeEditSource edits(base);
		for (const LandscapeTileKey key : { LandscapeTileKey{ 0, 0 }, LandscapeTileKey{ 5, 7 } })
		{
			auto tile = std::make_shared<LandscapeTile>(*fullTile(*base, key));
			tile->heights[0][10] = static_cast<uint16_t>(tile->heights[0][10] ^ 0x1234);
			tile->buildMips();
			edits.set({ { key, tile } });
		}
		std::string err;
		REQUIRE_MESSAGE(edits.save(file, &err), err);
		CHECK_FALSE(edits.dirty());
		std::ifstream in(file, std::ios::binary);
		bytes.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

		LandscapeEditSource again(base);
		CHECK(again.loadFile(file) == 2);
		CHECK_FALSE(again.dirty());
		REQUIRE(again.edited({ 5, 7 }));
		CHECK(*again.edited({ 5, 7 }) == *edits.edited({ 5, 7 }));
		CHECK(*again.edited({ 0, 0 }) == *edits.edited({ 0, 0 }));
	}
	// Cut in the middle of the second record: the first one stands.
	{
		std::ofstream out(file, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size() - 100));
	}
	LandscapeEditSource cut(base);
	CHECK(cut.loadFile(file) == 1);
	CHECK(cut.edited({ 0, 0 }) != nullptr);
	// Not an edits file at all.
	{
		std::ofstream out(file, std::ios::binary | std::ios::trunc);
		out << "this is not a landscape edits file";
	}
	LandscapeEditSource junk(base);
	CHECK(junk.loadFile(file) == 0);
	// Saving an overlay with nothing in it removes the file.
	REQUIRE(junk.save(file));
	CHECK_FALSE(std::filesystem::exists(file));
}

TEST_CASE("LandscapeEditSession: a stroke reaches the store in place, the far levels and the atlas follow, and undo and redo swap whole strokes")
{
	const auto base = world();
	const auto edits = std::make_shared<LandscapeEditSource>(base);
	LandscapeRuntime rt;
	LandscapeRuntimeConfig cfg;
	REQUIRE(rt.begin(edits, cfg));
	REQUIRE(rt.edits() == edits.get());   // an overlay handed in is the one that is edited
	const glm::dvec3 cam(3000.0, 900.0, 3000.0), origin(0.0);
	const glm::dmat4 vp = glm::perspective(glm::radians(70.0), 16.0 / 9.0, 1.0, 40000.0)
	                      * glm::lookAt(cam, cam + glm::dvec3(1, -0.1, 0.2), glm::dvec3(0.0, 1.0, 0.0));
	REQUIRE(settle(rt, cam));
	LandscapeRenderFrame f0;
	rt.buildRenderFrame(vp, origin, false, f0);
	REQUIRE(f0.active);
	const uint64_t generation = f0.world->generation;

	LandscapeEditSession session(rt, *edits);
	// A 30 m brush in the middle of tile (5, 5): one tile is enough.
	const double bx = 5.5 * 512.0, bz = 5.5 * 512.0;
	REQUIRE(session.focus(bx, bz, 30.0));
	CHECK(session.terrain().resolution == 257);
	float before = 0.0f;
	REQUIRE(session.heightAt(bx, bz, before));

	session.beginStroke();
	TerrainComponent& tc = session.terrain();
	const uint32_t mid = tc.resolution / 2;
	tc.sculptHeights[static_cast<size_t>(mid) * tc.resolution + mid] += 60.0f;
	CHECK(session.commit() == 1);
	// In place: the tile stayed resident, with the new content, and the atlas uploads it again into the slot it has.
	const LandscapeTileKey key{ 5, 5 };
	REQUIRE(rt.store()->isResident(key));
	const uint16_t slot = rt.atlas()->slotOf(key);
	rt.updateAt(cam, glm::vec3(0.0f), 1.0);
	LandscapeRenderFrame f1;
	rt.buildRenderFrame(vp, origin, false, f1);
	bool again = false;
	for (const LandscapeRenderUpload& u : f1.uploads) again = again || u.slot == slot;
	CHECK(again);
	CHECK(f1.tableUpdates.empty());                 // same slot: nothing in the table moves, nothing falls back to the far levels
	CHECK(f1.world->generation == generation);
	CHECK(f1.world->coarseRevision == f0.world->coarseRevision);   // the coarse world waits for the end of the stroke

	// A second dab of the same stroke, then the end.
	tc.sculptHeights[static_cast<size_t>(mid + 3) * tc.resolution + mid] += 60.0f;
	session.endStroke();
	CHECK(session.canUndo());
	CHECK_FALSE(session.canRedo());
	float after = 0.0f;
	REQUIRE(session.focus(bx, bz, 30.0));
	REQUIRE(session.heightAt(bx, bz, after));
	CHECK(after == doctest::Approx(before + 60.0f).epsilon(0.001));
	REQUIRE(settle(rt, cam));
	LandscapeRenderFrame f2;
	rt.buildRenderFrame(vp, origin, false, f2);
	CHECK(f2.world->generation == generation);                      // never "a new landscape": that would empty every backend's atlas
	CHECK(f2.world->coarseRevision != f0.world->coarseRevision);    // the far levels are uploaded again
	CHECK(f2.world->coarseHeights != f0.world->coarseHeights);
	REQUIRE(edits->edited(key));
	CHECK(edits->dirty());

	// Undo takes the whole stroke (both dabs) back, redo brings it again.
	REQUIRE(session.undo());
	CHECK(edits->edited(key) == nullptr);
	REQUIRE(session.focus(bx, bz, 30.0));
	float undone = 0.0f;
	REQUIRE(session.heightAt(bx, bz, undone));
	CHECK(undone == doctest::Approx(before).epsilon(0.0001));
	CHECK_FALSE(session.canUndo());
	REQUIRE(session.redo());
	REQUIRE(edits->edited(key));
	REQUIRE(session.focus(bx, bz, 30.0));
	REQUIRE(session.heightAt(bx, bz, after));
	CHECK(after == doctest::Approx(before + 60.0f).epsilon(0.001));
	CHECK_FALSE(session.redo());
}

TEST_CASE("LandscapeEditSession: the region follows the brush, takes 2 or 4 tiles near a border, and refuses a brush no region holds")
{
	const auto base = world();
	const auto edits = std::make_shared<LandscapeEditSource>(base);
	LandscapeRuntime rt;
	REQUIRE(rt.begin(edits, LandscapeRuntimeConfig{}));
	LandscapeEditSession session(rt, *edits);

	// On the border between tiles 4 and 5 in x: two tiles a side (never three).
	REQUIRE(session.focus(5.0 * 512.0, 5.5 * 512.0, 40.0));
	CHECK(session.terrain().resolution == 513);
	// A large brush across a corner: four.
	REQUIRE(session.focus(5.0 * 512.0, 5.0 * 512.0, 600.0));
	CHECK(session.terrain().resolution == 1025);
	// More than a 4 x 4 region can hold with a sample to spare.
	CHECK_FALSE(session.focus(5.0 * 512.0, 5.0 * 512.0, 1100.0));
	CHECK_FALSE(session.hasRegion());
	// Outside the world.
	CHECK_FALSE(session.focus(-50.0, 100.0, 10.0));
	// At the world's own edge the region ends with the world.
	REQUIRE(session.focus(10.0, 10.0, 60.0));
	CHECK(session.terrain().resolution == 257);

	// A dab that crosses a tile border is written to both tiles, and their shared samples agree.
	REQUIRE(session.focus(5.0 * 512.0, 5.5 * 512.0, 40.0));
	TerrainComponent& tc = session.terrain();
	const uint32_t res = tc.resolution;
	for (uint32_t x = res / 2 - 3; x <= res / 2 + 3; ++x) tc.sculptHeights[static_cast<size_t>(100) * res + x] += 30.0f;
	CHECK(session.commit() == 2);
	const LandscapeTilePtr left = edits->edited({ 4, 5 }), right = edits->edited({ 5, 5 });
	REQUIRE(left);
	REQUIRE(right);
	const int dim = edits->geometry().hCells + 1;
	for (int z = 0; z < dim; ++z) CHECK(left->heights[0][static_cast<size_t>(z) * dim + dim - 1] == right->heights[0][static_cast<size_t>(z) * dim]);
}

TEST_CASE("landscapePackBakeEdits: the pack takes the edited tiles and keeps its other sections, and a runtime shows an edits file beside a pack")
{
	ProceduralLandscapeParams p;
	p.geometry.hCells = 32;
	p.geometry.wTexels = 16;
	p.geometry.channels = 4;
	p.tilesX = p.tilesZ = 3;
	p.seaLevelM = 1500.0f;
	const ProceduralLandscapeSource src(p);
	const std::filesystem::path packFile = tempFile("pack.bin");
	{
		LandscapePackWriter w;
		REQUIRE(w.begin(packFile.string(), src.geometry(), 3, 3, 3, 0.0f, 3000.0f));
		w.setWaterTable(*src.water());
		for (int z = 0; z < 3; ++z)
			for (int x = 0; x < 3; ++x) REQUIRE(w.add({ x, z }, encodeLandscapeTile(src.generate({ x, z }))));
		REQUIRE(w.finish());
	}
	std::string err;
	LandscapeTile editedTile;
	{
		auto pack = LandscapePackSource::open(packFile.string(), &err);
		REQUIRE_MESSAGE(pack, err);
		LandscapeEditSource edits(pack);
		auto tile = std::make_shared<LandscapeTile>(*fullTile(*pack, { 1, 2 }));
		for (uint16_t& h : tile->heights[0]) h = 12345;
		tile->buildMips();
		editedTile = *tile;
		edits.set({ { { 1, 2 }, tile } });
		REQUIRE_MESSAGE(edits.save(landscapeEditsFile(packFile), &err), err);

		// A copy, as the export makes one, baked from an overlay of the ORIGINAL.
		const std::filesystem::path copy = tempFile("copy.bin");
		std::filesystem::copy_file(packFile, copy);
		REQUIRE_MESSAGE(landscapePackBakeEdits(copy.string(), edits, &err), err);
		auto baked = LandscapePackSource::open(copy.string(), &err);
		REQUIRE_MESSAGE(baked, err);
		LandscapeTile t;
		REQUIRE(baked->load({ 1, 2 }, -1, t));
		CHECK(t.heights[0] == editedTile.heights[0]);
		REQUIRE(baked->load({ 0, 0 }, -1, t));
		CHECK(t == *fullTile(*pack, { 0, 0 }));          // the others byte for byte
		CHECK(baked->water() != nullptr);                // the water table came along
		float minM = 0.0f, scaleM = 0.0f;
		CHECK(baked->heightRange(minM, scaleM));
		CHECK(scaleM == 3000.0f);
		REQUIRE(baked->coarse());
		const int cpt = baked->coarse()->cellsPerTile;
		CHECK(baked->coarse()->heights[0].at(1 * cpt + cpt / 2, 2 * cpt + cpt / 2) == 12345);   // and its coarse world is the edited one
		baked.reset();
		std::filesystem::remove(copy);
	}

	std::filesystem::remove(landscapeEditsFile(packFile));
	std::filesystem::remove(packFile);
}

TEST_CASE("landscape runtime: a pack with an edits file beside it is shown edited, and an editable runtime has an overlay from the start")
{
	const auto src = world(4);   // the real geometry: a runtime needs it for its load radius
	const std::filesystem::path packFile = tempFile("rt.bin");
	{
		LandscapePackWriter w;
		REQUIRE(w.begin(packFile.string(), src->geometry(), 4, 4, 4, 0.0f, 3000.0f));
		for (int z = 0; z < 4; ++z)
			for (int x = 0; x < 4; ++x) REQUIRE(w.add({ x, z }, encodeLandscapeTile(src->generate({ x, z }))));
		REQUIRE(w.finish());
	}
	std::string err;
	// No edits file: a plain runtime reads the pack itself, an editable one gets an (empty) overlay.
	{
		LandscapeRuntime plain;
		REQUIRE_MESSAGE(plain.beginFromPack(packFile.string(), LandscapeRuntimeConfig{}, &err), err);
		CHECK(plain.edits() == nullptr);
		LandscapeRuntime editable;
		editable.setEditable(true);
		REQUIRE_MESSAGE(editable.beginFromPack(packFile.string(), LandscapeRuntimeConfig{}, &err), err);
		REQUIRE(editable.edits());
		CHECK(editable.edits()->editedKeys().empty());
		CHECK(editable.packFile() == packFile);

		// An edit through that runtime, saved where the next one finds it.
		auto tile = std::make_shared<LandscapeTile>(*fullTile(*src, { 1, 2 }));
		for (uint16_t& h : tile->heights[0]) h = 22222;
		tile->buildMips();
		editable.edits()->set({ { { 1, 2 }, tile } });
		REQUIRE_MESSAGE(editable.edits()->save(landscapeEditsFile(editable.packFile()), &err), err);
	}
	// Now any runtime, editable or not, shows the edited tile.
	{
		LandscapeRuntime rt;
		REQUIRE_MESSAGE(rt.beginFromPack(packFile.string(), LandscapeRuntimeConfig{}, &err), err);
		REQUIRE(rt.edits());
		CHECK(rt.edits()->edited({ 1, 2 }) != nullptr);
		CHECK_FALSE(rt.edits()->dirty());
		LandscapeTile t;
		REQUIRE(rt.store()->source().load({ 1, 2 }, -1, t));
		CHECK(t.heights[0][0] == 22222);
	}
	// The world is built again under the same path (Generate, Migrate, Import): the edits file beside it belongs to the OLD world and
	// must not put its tiles onto the new one.
	{
		ProceduralLandscapeParams p;
		p.tilesX = p.tilesZ = 4;
		p.seed = 4242;
		const ProceduralLandscapeSource other(p);
		LandscapePackWriter w;
		REQUIRE(w.begin(packFile.string(), other.geometry(), 4, 4, 4, 0.0f, 3000.0f));
		for (int z = 0; z < 4; ++z)
			for (int x = 0; x < 4; ++x) REQUIRE(w.add({ x, z }, encodeLandscapeTile(other.generate({ x, z }))));
		REQUIRE(w.finish());
		REQUIRE(std::filesystem::exists(landscapeEditsFile(packFile)));

		LandscapeRuntime rt;
		REQUIRE_MESSAGE(rt.beginFromPack(packFile.string(), LandscapeRuntimeConfig{}, &err), err);
		REQUIRE(rt.edits());                                   // the file is there, so there is an overlay ...
		CHECK(rt.edits()->editedKeys().empty());               // ... with nothing of the old world in it
		LandscapeTile t;
		REQUIRE(rt.store()->source().load({ 1, 2 }, -1, t));
		CHECK(t.heights[0] == other.generate({ 1, 2 }).heights[0]);
	}
	std::filesystem::remove(landscapeEditsFile(packFile));
	std::filesystem::remove(packFile);
}

TEST_CASE("LandscapeEditSession: the single landscape's own brushes work on the region and reach the tiles (sculpt and paint)")
{
	const auto base = world();
	const auto edits = std::make_shared<LandscapeEditSource>(base);
	LandscapeRuntime rt;
	REQUIRE(rt.begin(edits, LandscapeRuntimeConfig{}));
	LandscapeEditSession session(rt, *edits);
	const double bx = 6.5 * 512.0, bz = 6.5 * 512.0;
	REQUIRE(session.focus(bx, bz, 60.0));
	float before = 0.0f;
	REQUIRE(session.heightAt(bx, bz, before));
	const float lx = static_cast<float>(bx - session.centre().x), lz = static_cast<float>(bz - session.centre().z);

	// Raise by 12 m at the centre: the brush must not have resized the terrain behind the session's back (ensureHeights snaps to 2^k + 1).
	const uint32_t res = session.terrain().resolution, wres = session.terrain().weightRes;
	const TerrainSculpt::Result r = TerrainSculpt::apply(session.terrain(), lx, lz, TerrainSculpt::Op::Raise, 40.0f, 20.0f, 12.0f);
	REQUIRE(r.ok);
	CHECK(r.changed > 0);
	CHECK(session.terrain().resolution == res);
	CHECK(session.commit() == 1);
	float after = 0.0f;
	REQUIRE(session.heightAt(bx, bz, after));
	CHECK(after == doctest::Approx(before + 12.0f).epsilon(0.001));
	const LandscapeTilePtr sculpted = edits->edited({ 6, 6 });
	REQUIRE(sculpted);
	const LandscapeTilePtr was = fullTile(*base, { 6, 6 });
	CHECK(sculpted->heights[0] != was->heights[0]);
	CHECK(sculpted->weights[0] == was->weights[0]);   // a sculpt dab leaves the paint alone

	// Paint layer 2 at full strength: the weights of the tile change, the heights do not change again.
	REQUIRE(TerrainPaint::paint(session.terrain(), lx, lz, 2, 40.0f, 20.0f, 1.0f));
	CHECK(session.terrain().weightRes == wres);
	CHECK(session.commit() == 1);
	const LandscapeTilePtr painted = edits->edited({ 6, 6 });
	REQUIRE(painted);
	CHECK(painted->heights[0] == sculpted->heights[0]);
	CHECK(painted->weights[0] != sculpted->weights[0]);
	const int wt = edits->geometry().wTexels, C = edits->geometry().channels;
	CHECK(painted->weights[0][(static_cast<size_t>(wt / 2) * wt + wt / 2) * C + 2] > 200);   // the middle of the tile is layer 2 now
}

TEST_CASE("LandscapeEditSession: the water brush makes a lake on a dry tile world: cells in the tile, a body in the overlay's table, "
          "both in the edits file and in a baked pack")
{
	const auto base = world(4);   // no sea: the source has no water table at all
	REQUIRE(base->water() == nullptr);
	const std::filesystem::path packFile = tempFile("water_pack.bin");
	{
		LandscapePackWriter w;
		REQUIRE(w.begin(packFile.string(), base->geometry(), 4, 4, 4, 0.0f, 3000.0f));
		for (int z = 0; z < 4; ++z)
			for (int x = 0; x < 4; ++x) REQUIRE(w.add({ x, z }, encodeLandscapeTile(base->generate({ x, z }))));
		REQUIRE(w.finish());
	}
	std::string err;
	const std::filesystem::path sidecar = tempFile("water_sidecar");
	uint16_t bodyId = 0;
	float level = 0.0f;
	LandscapeTile wetTile;
	{
		auto pack = LandscapePackSource::open(packFile.string(), &err);
		REQUIRE_MESSAGE(pack, err);
		const auto edits = std::make_shared<LandscapeEditSource>(pack);
		LandscapeRuntime rt;
		REQUIRE(rt.begin(edits, LandscapeRuntimeConfig{}));
		LandscapeEditSession session(rt, *edits);
		const double bx = 1.5 * 512.0, bz = 1.5 * 512.0;
		REQUIRE(session.focus(bx, bz, 60.0));
		const float lx = static_cast<float>(bx - session.centre().x), lz = static_cast<float>(bz - session.centre().z);
		CHECK(session.terrain().water.res == session.foliageRes());   // the field is at the tiles' raster before the first cell

		namespace WB = HE::water::brush;
		WB::Params wp;
		wp.radius = 40.0f;
		wp.falloff = 10.0f;
		wp.amount = 1.0f;
		wp.levelOffset = 5.0f;   // the ground here is not flat: a lake 5 m above the point under the brush covers its surroundings
		WB::Stroke stroke;
		session.beginStroke();
		WB::begin(stroke, session.terrain(), lx, lz, wp);
		REQUIRE(stroke.active);
		for (int i = 0; i < 4; ++i) CHECK(WB::dab(stroke, session.terrain(), lx, lz, wp).ok);
		WB::end(stroke, session.terrain());
		session.endStroke();

		const LandscapeTilePtr wet = edits->edited({ 1, 1 });
		REQUIRE(wet);
		REQUIRE(wet->hasWater());
		const size_t wt = static_cast<size_t>(edits->geometry().weightDim(0));
		CHECK(wet->waterCoverage[(wt / 2) * wt + wt / 2] > 0);
		bodyId = wet->waterOwner[(wt / 2) * wt + wt / 2];
		CHECK(bodyId != 0);
		REQUIRE(edits->waterEdited());
		const LandscapeWaterBody* body = edits->water()->find(bodyId);
		REQUIRE(body);
		level = body->level;
		float ground = 0.0f;
		REQUIRE(session.heightAt(bx, bz, ground));
		CHECK(level == doctest::Approx(ground + 5.0f).epsilon(0.01));
		wetTile = *wet;
		CHECK(fullTile(*pack, { 2, 2 })->hasWater() == false);
		CHECK(edits->edited({ 3, 3 }) == nullptr);   // nothing else was touched

		REQUIRE_MESSAGE(edits->save(sidecar, &err), err);
		// Baked: the pack has the table and the wet tile.
		const std::filesystem::path copy = tempFile("water_copy.bin");
		std::filesystem::copy_file(packFile, copy);
		REQUIRE_MESSAGE(landscapePackBakeEdits(copy.string(), *edits, &err), err);
		{
			auto baked = LandscapePackSource::open(copy.string(), &err);
			REQUIRE_MESSAGE(baked, err);
			REQUIRE(baked->water());
			REQUIRE(baked->water()->find(bodyId));
			CHECK(baked->water()->find(bodyId)->level == level);
			LandscapeTile t;
			REQUIRE(baked->load({ 1, 1 }, -1, t));
			CHECK(t.waterCoverage == wetTile.waterCoverage);
			CHECK(t.waterOwner == wetTile.waterOwner);
		}
		std::filesystem::remove(copy);
	}
	// The edits file brings the lake back: tile and table.
	{
		auto pack = LandscapePackSource::open(packFile.string(), &err);
		REQUIRE_MESSAGE(pack, err);
		LandscapeEditSource again(pack);
		CHECK(again.loadFile(sidecar) == 1);
		REQUIRE(again.water());
		REQUIRE(again.water()->find(bodyId));
		CHECK(again.water()->find(bodyId)->level == level);
		REQUIRE(again.edited({ 1, 1 }));
		CHECK(again.edited({ 1, 1 })->waterCoverage == wetTile.waterCoverage);
		CHECK_FALSE(again.dirty());
	}
	std::filesystem::remove(sidecar);
	std::filesystem::remove(packFile);
}

TEST_CASE("LandscapeEditSession on a world with a sea: a commit without a change writes no tile, a sculpt dab changes its own tile and "
          "leaves the water rasters and the water table as they were")
{
	ProceduralLandscapeParams p;
	p.tilesX = p.tilesZ = 6;
	p.seaLevelM = 1500.0f;   // half of the height range: most tiles are partly under water
	const auto base = std::make_shared<ProceduralLandscapeSource>(p);
	REQUIRE(base->water());
	const auto edits = std::make_shared<LandscapeEditSource>(base);
	LandscapeRuntime rt;
	REQUIRE(rt.begin(edits, LandscapeRuntimeConfig{}));
	LandscapeEditSession session(rt, *edits);
	const double bx = 2.5 * 512.0, bz = 2.5 * 512.0;
	REQUIRE(session.focus(bx, bz, 300.0));   // 2 x 2 tiles
	size_t wetTiles = 0;
	for (int z = 0; z < 6; ++z)
		for (int x = 0; x < 6; ++x) wetTiles += fullTile(*base, { x, z })->hasWater() ? 1 : 0;
	REQUIRE(wetTiles > 0);

	CHECK(session.commit() == 0);
	CHECK(edits->editedKeys().empty());
	CHECK_FALSE(edits->waterEdited());

	const float lx = static_cast<float>(bx - session.centre().x), lz = static_cast<float>(bz - session.centre().z);
	REQUIRE(TerrainSculpt::apply(session.terrain(), lx, lz, TerrainSculpt::Op::Raise, 20.0f, 10.0f, 12.0f).ok);
	CHECK(session.commit() == 1);
	REQUIRE(edits->editedKeys().size() == 1);
	const LandscapeTileKey key = edits->editedKeys()[0];
	const LandscapeTilePtr was = fullTile(*base, key), now = edits->edited(key);
	CHECK(now->heights[0] != was->heights[0]);
	CHECK(now->waterCoverage == was->waterCoverage);
	CHECK(now->waterOwner == was->waterOwner);
	CHECK_FALSE(edits->waterEdited());   // the sea is the base's: no body moved, no table of the overlay's own
}

TEST_CASE("LandscapeStamp: a stamp raises the tiles under it when they are loaded, lies over a brush edit without changing it, moves by "
          "naming old and new footprint, and is baked into a pack")
{
	ProceduralLandscapeParams p;
	p.geometry.hCells = 32;      // 16 m samples on 512 m tiles
	p.geometry.wTexels = 16;
	p.geometry.channels = 4;
	p.tilesX = p.tilesZ = 3;
	const ProceduralLandscapeSource src(p);
	const std::filesystem::path packFile = tempFile("stamp_pack.bin");
	{
		LandscapePackWriter w;
		REQUIRE(w.begin(packFile.string(), src.geometry(), 3, 3, 3, 0.0f, 3000.0f));
		for (int z = 0; z < 3; ++z)
			for (int x = 0; x < 3; ++x) REQUIRE(w.add({ x, z }, encodeLandscapeTile(src.generate({ x, z }))));
		REQUIRE(w.finish());
	}
	std::string err;
	{
		auto pack = LandscapePackSource::open(packFile.string(), &err);
		REQUIRE_MESSAGE(pack, err);
		LandscapeEditSource edits(pack);
		const int dim = 33;
		const auto at = [&](const LandscapeTile& t, int x, int z) { return static_cast<int>(t.heights[0][static_cast<size_t>(z) * dim + x]); };
		const LandscapeTilePtr base11 = fullTile(*pack, { 1, 1 });

		// A flat picture of 1, 256 m a side, 300 m high, in the middle of tile (1, 1): 300 / 3000 * 65535 = 6553.5 on every sample inside.
		LandscapeStamp s;
		s.image = std::make_shared<std::vector<float>>(4, 1.0f);
		s.w = s.h = 2;
		s.x = s.z = 768.0;
		s.sizeX = s.sizeZ = 256.0f;
		s.heightM = 300.0f;
		s.falloff = 0.0f;
		std::vector<LandscapeTileKey> changed = edits.setStamps({ s }, 0.0f);
		REQUIRE(changed.size() == 1);
		CHECK(changed[0] == LandscapeTileKey{ 1, 1 });
		CHECK(edits.stamped({ 1, 1 }));
		CHECK_FALSE(edits.stamped({ 0, 0 }));
		LandscapeTile t;
		REQUIRE(edits.load({ 1, 1 }, -1, t));
		CHECK(std::abs(at(t, 16, 16) - at(*base11, 16, 16) - 6554) <= 1);   // the centre, inside
		CHECK(at(t, 2, 2) == at(*base11, 2, 2));                            // 32 m from the tile's corner, outside
		CHECK(t.heights.size() == base11->heights.size());                  // the mips were made again
		CHECK(t.heights[1] != base11->heights[1]);
		REQUIRE(edits.load({ 1, 1 }, 1, t));
		CHECK(t.heights.size() == 1);                                       // and fewer levels are still what was asked for
		REQUIRE(edits.load({ 0, 0 }, -1, t));
		CHECK(t == *fullTile(*pack, { 0, 0 }));
		REQUIRE(edits.coarse());
		{
			const int cpt = edits.coarse()->cellsPerTile;
			CHECK(edits.coarse()->heights[0].at(1 * cpt + cpt / 2, 1 * cpt + cpt / 2) > pack->coarse()->heights[0].at(1 * cpt + cpt / 2, 1 * cpt + cpt / 2) + 6000);
		}

		// A brush edit under the stamp: the overlay keeps the edit as it was made, and shows the stamp on top of it.
		auto flat = std::make_shared<LandscapeTile>(*base11);
		for (uint16_t& h : flat->heights[0]) h = 12345;
		flat->buildMips();
		edits.set({ { { 1, 1 }, flat } });
		CHECK(edits.edited({ 1, 1 })->heights[0][0] == 12345);
		REQUIRE(edits.load({ 1, 1 }, -1, t));
		CHECK(std::abs(at(t, 16, 16) - 12345 - 6554) <= 1);
		CHECK(at(t, 2, 2) == 12345);
		CHECK(std::abs(at(*edits.shown({ 1, 1 }, flat), 16, 16) - 12345 - 6554) <= 1);
		CHECK(edits.shown({ 0, 0 }, flat) == flat);   // no stamp there: the same tile

		// Moved onto the border between tiles (0, 1) and (1, 1), turned by 45 degrees: old and new footprint are named, and the two
		// tiles agree on the samples they share.
		LandscapeStamp moved = s;
		moved.x = 512.0;
		moved.yaw = 0.7853982f;
		changed = edits.setStamps({ moved }, 0.0f);
		const auto has = [&](LandscapeTileKey k) { return std::find(changed.begin(), changed.end(), k) != changed.end(); };
		CHECK(has({ 1, 1 }));
		CHECK(has({ 0, 1 }));
		LandscapeTile left, right;
		REQUIRE(edits.load({ 0, 1 }, -1, left));
		REQUIRE(edits.load({ 1, 1 }, -1, right));
		const LandscapeTilePtr base01 = fullTile(*pack, { 0, 1 });
		for (int z = 0; z < dim; ++z) CHECK(at(left, 32, z) - at(*base01, 32, z) == at(right, 0, z) - 12345);
		CHECK(at(left, 32, 16) - at(*base01, 32, 16) > 6000);
		CHECK(edits.setStamps({ moved }, 0.0f).empty());   // the same stamps again: nothing changed

		// Baked, as the export does it: the copy holds the tiles as they are shown.
		const std::filesystem::path copy = tempFile("stamp_copy.bin");
		std::filesystem::copy_file(packFile, copy);
		REQUIRE_MESSAGE(landscapePackBakeEdits(copy.string(), edits, &err), err);
		{
			auto baked = LandscapePackSource::open(copy.string(), &err);
			REQUIRE_MESSAGE(baked, err);
			LandscapeTile b;
			REQUIRE(baked->load({ 0, 1 }, -1, b));
			CHECK(b.heights[0] == left.heights[0]);
			REQUIRE(baked->load({ 1, 1 }, -1, b));
			CHECK(b.heights[0] == right.heights[0]);
			REQUIRE(baked->load({ 2, 2 }, -1, b));
			CHECK(b == *fullTile(*pack, { 2, 2 }));
		}
		std::filesystem::remove(copy);

		// Taken away: the ground under it is back, the brush edit is still there.
		changed = edits.setStamps({}, 0.0f);
		CHECK(changed.size() >= 2);
		CHECK(edits.stampCount() == 0);
		REQUIRE(edits.load({ 0, 1 }, -1, t));
		CHECK(t == *base01);
		REQUIRE(edits.load({ 1, 1 }, -1, t));
		CHECK(at(t, 16, 16) == 12345);
	}
	std::filesystem::remove(packFile);
}

TEST_CASE("tile foliage density: an unpainted tile is stored as before, a painted one round-trips, the region carries it to the tiles "
          "it covers, and the scatter leaves the erased ground bare")
{
	const auto src = world(4);
	const LandscapeGeometry g = src->geometry();
	const size_t wt = static_cast<size_t>(g.weightDim(0));
	const LandscapeTilePtr plain = fullTile(*src, { 1, 1 });

	// The blob: no mask, no flag (byte for byte what was written before the mask existed); with one, it comes back.
	const std::vector<uint8_t> blob = encodeLandscapeTile(*plain);
	REQUIRE(blob.size() > 7);
	CHECK((blob[6] & 4) == 0);
	LandscapeTile painted = *plain;
	painted.foliageDensity.assign(wt * wt, 255);
	for (size_t z = 0; z < wt; ++z)
		for (size_t x = 0; x < wt / 2; ++x) painted.foliageDensity[z * wt + x] = 0;   // the west half is bare
	const std::vector<uint8_t> blob2 = encodeLandscapeTile(painted);
	CHECK((blob2[6] & 4) != 0);
	LandscapeTile back;
	REQUIRE(decodeLandscapeTile(blob2.data(), blob2.size(), back));
	CHECK(back == painted);
	REQUIRE(decodeLandscapeTile(blob.data(), blob.size(), back));
	CHECK(back.foliageDensity.empty());

	// The region: 2 x 2 tiles, all 255; a square erased inside tile (1, 1) changes that tile alone.
	const auto tileAt = [&](LandscapeTileKey k) { return fullTile(*src, k); };
	LandscapeRegionTerrain region;
	REQUIRE(landscapeRegionTerrain(tileAt, { 0, 0 }, 2, {}, nullptr, region));
	REQUIRE(region.foliageRes == 2 * wt);
	REQUIRE(region.foliageDensity.size() == 4 * wt * wt);
	std::vector<std::pair<LandscapeTileKey, LandscapeTilePtr>> out;
	REQUIRE(landscapeWriteRegionTerrain(region.terrain, g, { 0, 0 }, 2, {}, tileAt, out, &region.foliageDensity));
	CHECK(out.empty());   // untouched: nothing to write
	for (size_t z = wt + 10; z < wt + 20; ++z)
		for (size_t x = wt + 10; x < wt + 20; ++x) region.foliageDensity[z * 2 * wt + x] = 0;
	REQUIRE(landscapeWriteRegionTerrain(region.terrain, g, { 0, 0 }, 2, {}, tileAt, out, &region.foliageDensity));
	REQUIRE(out.size() == 1);
	CHECK(out[0].first == LandscapeTileKey{ 1, 1 });
	CHECK(out[0].second->foliageDensity.size() == wt * wt);
	CHECK(out[0].second->foliageDensity[15 * wt + 15] == 0);
	CHECK(out[0].second->foliageDensity[0] == 255);
	CHECK(out[0].second->heights == plain->heights);

	// The scatter: with the west half erased, nothing grows there and the east half is as it was.
	auto edits = std::make_shared<LandscapeEditSource>(src);
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 1500.0f;
	const glm::dvec3 cam(768.0, 600.0, 768.0);
	const auto scatter = [&](std::vector<glm::mat4>& local) {
		LandscapeStore store(edits, cfg);
		for (int i = 0; i < 20000 && !(store.update({ { cam } }, 5.0), store.settled()); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		FoliageLayer l;
		l.meshAssetId = HE::UUID{ 1, 0 };
		l.density = 0.004f;
		l.drawDistance = 200.0f;
		l.seed = 21;
		LandscapeFoliage fol;
		fol.setLayers({ l });
		for (int i = 0; i < 4000; ++i)
		{
			fol.update(store, { cam }, 0.0f, 3000.0f);
			if (fol.stats().pending == 0 && fol.storeOf({ 1, 1 }, 0)) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		const auto s = fol.storeOf({ 1, 1 }, 0);
		local = s ? s->local : std::vector<glm::mat4>{};
	};
	std::vector<glm::mat4> all, masked;
	scatter(all);
	edits->set({ { { 1, 1 }, std::make_shared<LandscapeTile>(painted) } });
	scatter(masked);
	REQUIRE(all.size() > 100);
	size_t east = 0;
	for (const glm::mat4& m : all) east += m[3].x > 8.0f ? 1 : 0;   // local: the tile's centre is 0; two texels of soft edge
	CHECK(masked.size() < all.size());
	CHECK(masked.size() >= east * 9 / 10);
	for (const glm::mat4& m : masked) CHECK(m[3].x > -8.0f);
}

TEST_CASE("LandscapeStampSync: a stamp entity reaches the runtime's overlay, an idle frame does nothing, moving it names both footprints, "
          "the component round-trips, and an export bakes the stamp and takes it out of the shipped scene")
{
	const auto src = world(4);
	const std::filesystem::path packFile = tempFile("stamp_rt.bin");
	{
		LandscapePackWriter w;
		REQUIRE(w.begin(packFile.string(), src->geometry(), 4, 4, 4, 0.0f, 3000.0f));
		for (int z = 0; z < 4; ++z)
			for (int x = 0; x < 4; ++x) REQUIRE(w.add({ x, z }, encodeLandscapeTile(src->generate({ x, z }))));
		REQUIRE(w.finish());
	}
	ContentManager cm{ "." };
	TextureAsset tex;
	tex.width = tex.height = 4;
	tex.channels = 4;
	tex.data.assign(4 * 4 * 4, 255);   // white: the stamp is its full height everywhere
	const HE::UUID texId = cm.registerTexture(std::move(tex));

	HorizonWorld scene;
	const auto lsEnt = scene.createEntity("Landscape");
	LandscapeWorldComponent lw;
	lw.pack = packFile.string();
	scene.registry().emplace<LandscapeWorldComponent>(lsEnt, lw);
	const auto stampEnt = scene.createEntity("Stamp");
	auto& tf = scene.registry().get_or_emplace<TransformComponent>(stampEnt);
	tf.position = glm::vec3(768.0f, 0.0f, 768.0f);   // the middle of tile (1, 1)
	tf.scale = glm::vec3(1.0f, 2.0f, 1.0f);          // twice the height
	tf.dirty = true;
	LandscapeStampComponent sc;
	sc.textureId = texId;
	sc.sizeM = 200.0f;
	sc.heightM = 100.0f;
	sc.falloff = 0.0f;
	scene.registry().emplace<LandscapeStampComponent>(stampEnt, sc);

	std::string err;
	{
		LandscapeRuntime rt;
		rt.setEditable(true);
		REQUIRE_MESSAGE(rt.beginFromPack(packFile.string(), LandscapeRuntimeConfig{}, &err), err);
		const glm::dvec3 cam(768.0, 400.0, 768.0);
		REQUIRE(settle(rt, cam));
		LandscapeStampSync sync;
		CHECK(sync.sync(scene, rt, cm) == 1);            // tile (1, 1) is read again
		CHECK(rt.edits()->stamped({ 1, 1 }));
		CHECK(sync.sync(scene, rt, cm) == 0);            // idle: nothing handed over, nothing read
		REQUIRE(settle(rt, cam));
		const LandscapeStore::TilePtr shown = rt.store()->tile({ 1, 1 });
		REQUIRE(shown);
		const LandscapeTilePtr base = fullTile(*src, { 1, 1 });
		const int dim = src->geometry().heightDim(0), mid = dim / 2;
		// 100 m x scale 2 = 200 m of 3000: 4369 steps
		CHECK(std::abs(static_cast<int>(shown->heights[0][static_cast<size_t>(mid) * dim + mid]) - static_cast<int>(base->heights[0][static_cast<size_t>(mid) * dim + mid]) - 4369) <= 1);

		// Moved to tile (2, 1): both tiles are read again, once the rate limit of a drag lets it through.
		scene.registry().get<TransformComponent>(stampEnt).position.x = 1280.0f;
		scene.registry().get<TransformComponent>(stampEnt).dirty = true;
		std::this_thread::sleep_for(std::chrono::milliseconds(120));
		CHECK(sync.sync(scene, rt, cm) == 2);
		CHECK(rt.edits()->stamped({ 2, 1 }));
		CHECK_FALSE(rt.edits()->stamped({ 1, 1 }));
		CHECK_FALSE(rt.edits()->dirty());                // stamps are the scene's, not the edits file's

		// Switched off: gone from the ground.
		scene.registry().get<LandscapeStampComponent>(stampEnt).enabled = false;
		std::this_thread::sleep_for(std::chrono::milliseconds(120));
		CHECK(sync.sync(scene, rt, cm) == 1);
		CHECK(rt.edits()->stampCount() == 0);
		scene.registry().get<LandscapeStampComponent>(stampEnt).enabled = true;
	}

	// A heightmap FILE keeps 16 bits: a .r16 ramp of 512 values a row arrives as 512 different heights (a texture asset has 256
	// at most), and the file wins over the texture.
	{
		const std::filesystem::path r16 = tempFile("stamp_ramp").string() + ".r16";   // the reader goes by the extension
		{
			std::vector<uint16_t> ramp(512 * 512);
			for (size_t y = 0; y < 512; ++y)
				for (size_t x = 0; x < 512; ++x) ramp[y * 512 + x] = static_cast<uint16_t>(x * 128);   // 0, 128, 256 ...: steps of half an 8-bit level
			std::ofstream out(r16, std::ios::binary);
			out.write(reinterpret_cast<const char*>(ramp.data()), static_cast<std::streamsize>(ramp.size() * 2));
		}
		scene.registry().get<LandscapeStampComponent>(stampEnt).heightmapFile = r16.string();
		LandscapeStampImages images;
		const std::vector<LandscapeStamp> stamps = landscapeStampsOfWorld(scene, cm, images);
		REQUIRE(stamps.size() == 1);
		CHECK(stamps[0].w == 512);
		CHECK(stamps[0].h == 512);
		const std::vector<float>& img = *stamps[0].image;
		CHECK(img[1] == doctest::Approx(128.0 / 65535.0).epsilon(1e-4));   // half an 8-bit step: lost in a texture asset
		std::set<float> row(img.begin(), img.begin() + 512);
		CHECK(row.size() == 512);
		// A file that is not there: the stamp does nothing, and says so once (not a crash, not the texture behind its back).
		scene.registry().get<LandscapeStampComponent>(stampEnt).heightmapFile = (r16.string() + ".missing");
		CHECK(landscapeStampsOfWorld(scene, cm, images).empty());
		scene.registry().get<LandscapeStampComponent>(stampEnt).heightmapFile = "maps/ramp.r16";   // for the round trip below
		std::filesystem::remove(r16);
	}

	// The component is saved with its entity.
	{
		SceneSerializer ser;
		const std::vector<uint8_t> blob = ser.serializeSubtree(scene, stampEnt);
		HorizonWorld other;
		const auto e = ser.instantiatePrefab(other, blob);
		const auto* back = other.registry().try_get<LandscapeStampComponent>(e);
		REQUIRE(back);
		CHECK(back->textureId == texId);
		CHECK(back->sizeM == 200.0f);
		CHECK(back->heightM == 100.0f);
		CHECK(back->falloff == 0.0f);
		CHECK(back->heightmapFile == "maps/ramp.r16");
		scene.registry().get<LandscapeStampComponent>(stampEnt).heightmapFile.clear();   // the export below bakes the texture's stamp
		CHECK(SceneSerializer::isKnownComponentKey("landscapeStamp"));
	}

	// Export: the plan carries the stamp, the scene that ships has none, and the copy of the pack holds the raised ground.
	{
		LandscapeExportPlan plan;
		LandscapeExportOptions options;
		options.bakeHlod = false;
		options.bakeFar = true;   // painted from the world with its stamps: they are in the plan before the far image is made
		prepareLandscapePacksForExport(scene, packFile.parent_path(), cm, options, plan, {});
		REQUIRE(plan.packs.size() == 1);
		CHECK(plan.packs[0].stamps.size() == 1);
		CHECK(plan.packs[0].farDone);
		CHECK(plan.farBake.baked > 0);
		CHECK(plan.farBake.failed == 0);
		CHECK(scene.registry().view<LandscapeStampComponent>().empty());
		const std::filesystem::path copy = tempFile("stamp_export.bin");
		std::filesystem::copy_file(packFile, copy);
		REQUIRE_MESSAGE(finishExportedPack(plan.packs[0], copy, &err), err);
		auto baked = LandscapePackSource::open(copy.string(), &err);
		REQUIRE_MESSAGE(baked, err);
		LandscapeTile t;
		REQUIRE(baked->load({ 2, 1 }, -1, t));
		const LandscapeTilePtr base = fullTile(*src, { 2, 1 });
		const int dim = src->geometry().heightDim(0), mid = dim / 2;
		CHECK(std::abs(static_cast<int>(t.heights[0][static_cast<size_t>(mid) * dim + mid]) - static_cast<int>(base->heights[0][static_cast<size_t>(mid) * dim + mid]) - 4369) <= 1);
		baked.reset();
		std::filesystem::remove(copy);
	}
	std::filesystem::remove(packFile);
}
