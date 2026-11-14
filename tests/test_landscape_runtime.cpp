// HE::LandscapeRuntime (Thema 326, step 2 part A): the camera drives the store, the atlas follows it, buildFrame turns the
// camera's matrices into the patch list and the uploads a renderer needs. No GPU and no render extractor: what goes
// into the frame is checked as data, and the rules that tie the parts together are checked when the runtime starts.
#include "doctest.h"
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/LandscapeWorldComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LandscapeGpuLayout.h>
#include <HorizonScene/LandscapeRuntime.h>
#include <HorizonScene/SceneSerializer.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>
#include <set>
#include <thread>

using namespace HE;

namespace
{
// 12 x 12 tiles of the real geometry (512 m, 2 m / 4 m, 12 channels): 6.1 km, small enough to generate in a moment.
std::shared_ptr<ProceduralLandscapeSource> smallRealWorld(int tiles = 12)
{
	ProceduralLandscapeParams p;
	p.tilesX = p.tilesZ = tiles;
	return std::make_shared<ProceduralLandscapeSource>(p);
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

glm::dmat4 viewProjRelative(const glm::dvec3& camAbs, const glm::dvec3& origin, const glm::dvec3& dir)
{
	const glm::dvec3 eye = camAbs - origin;
	return glm::perspective(glm::radians(70.0), 16.0 / 9.0, 1.0, 40000.0) * glm::lookAt(eye, eye + dir, glm::dvec3(0.0, 1.0, 0.0));
}
} // namespace

TEST_CASE("landscape runtime: starts only when the parts fit together")
{
	auto src = smallRealWorld();
	LandscapeRuntime rt;
	std::string err;
	CHECK_FALSE(rt.active());

	LandscapeRuntimeConfig cfg;
	REQUIRE_MESSAGE(rt.begin(src, cfg, &err), err);
	CHECK(rt.active());
	CHECK(rt.quadtree().tileLevels == 3);                                     // the coarse world starts at mip 4
	CHECK(rt.quadtree().cellSize(4) == doctest::Approx(32.0f));
	CHECK(rt.quadtree().cellSize(rt.quadtree().tileLevels + 1) == doctest::Approx(src->coarse()->cellM(0)));
	CHECK(rt.atlas()->window() >= 2 * static_cast<int>(std::ceil(cfg.store.loadRadius * cfg.store.keepFactor / 512.0f)) + 2);

	// a store that would not load far enough for the fine levels
	LandscapeRuntimeConfig shortRadius;
	shortRadius.store.loadRadius = 4000.0f;
	CHECK_FALSE(rt.begin(src, shortRadius, &err));
	CHECK(err.find("loadRadius") != std::string::npos);
	CHECK_FALSE(rt.active());
	// a slot table narrower than the loaded disc
	LandscapeRuntimeConfig narrow;
	narrow.atlasWindow = 16;
	CHECK_FALSE(rt.begin(src, narrow, &err));
	CHECK(err.find("atlasWindow") != std::string::npos);
	// a tile the shaders have no room for: caught before anything is generated
	ProceduralLandscapeParams wide;
	wide.tilesX = wide.tilesZ = 2;
	wide.geometry.channels = 16;
	CHECK_FALSE(rt.begin(std::make_shared<ProceduralLandscapeSource>(wide), cfg, &err));
	CHECK(err.find("weight channels") != std::string::npos);
	ProceduralLandscapeParams fine;
	fine.tilesX = fine.tilesZ = 2;
	fine.geometry.hCells = 1024;                 // nine height mips
	CHECK_FALSE(rt.begin(std::make_shared<ProceduralLandscapeSource>(fine), cfg, &err));
	CHECK(err.find("height mips") != std::string::npos);
	// no source, a missing pack
	CHECK_FALSE(rt.begin(nullptr, cfg, &err));
	CHECK_FALSE(rt.beginFromPack((std::filesystem::temp_directory_path() / "he_landscape_no_such_pack.bin").string(), cfg, &err));
	CHECK_FALSE(rt.active());
}

TEST_CASE("landscape runtime: the frame covers the view, the fine levels sit on uploaded tiles, uploads come once")
{
	auto src = smallRealWorld();
	LandscapeRuntime rt;
	LandscapeRuntimeConfig cfg;
	REQUIRE(rt.begin(src, cfg));
	const glm::dvec3 cam(3000.0, 900.0, 3000.0);
	const glm::dvec3 origin(1024.0, 0.0, -512.0);

	// before any update there is nothing to draw
	LandscapeFrame none;
	rt.buildFrame(viewProjRelative(cam, origin, glm::dvec3(1, -0.1, 0.2)), origin, false, none);
	CHECK_FALSE(none.active);

	REQUIRE(settle(rt, cam));
	LandscapeFrame f1, f2;
	const glm::dmat4 vp = viewProjRelative(cam, origin, glm::dvec3(1, -0.1, 0.2));
	rt.buildFrame(vp, origin, false, f1);
	REQUIRE(f1.active);
	CHECK_FALSE(f1.patches.empty());
	CHECK(f1.uploads.size() == rt.atlas()->stats().uploads);          // everything the atlas took is handed over
	CHECK(f1.uploads.size() == rt.store()->residentKeys().size());
	rt.buildFrame(vp, origin, false, f2);
	CHECK(f2.uploads.empty());                                        // and only once
	CHECK(f2.tableUpdates.empty());
	CHECK(f2.patches.size() == f1.patches.size());

	// the slot table the renderer builds from the updates is the atlas's
	std::vector<uint16_t> table(static_cast<size_t>(f1.tableWindow) * f1.tableWindow, LandscapeAtlas::kNoSlot);
	for (const auto& u : f1.tableUpdates) table[u.index] = u.slot;
	CHECK(table == rt.atlas()->table());
	CHECK(f1.slotCapacity == rt.atlas()->capacity());

	// origin-relative positions, absolute extent, tile / coarse split
	int fine = 0, coarse = 0;
	for (const auto& p : f1.patches)
	{
		CHECK(p.morphStart < p.morphEnd);
		CHECK(p.size == doctest::Approx(rt.quadtree().patchSize(p.level)));
		CHECK(std::fmod(p.x0 + origin.x, p.size) == doctest::Approx(0.0).epsilon(1e-3).scale(1.0));   // a node sits on its level's grid
		CHECK(static_cast<double>(p.ix) * p.size == doctest::Approx(p.x0 + origin.x));            // the integer cell the shader uses: ix * 64 cells of size/64
		CHECK(static_cast<double>(p.iz) * p.size == doctest::Approx(p.z0 + origin.z));
		if (p.coarse)
		{
			++coarse;
			CHECK(p.level > f1.tileLevels);
			CHECK((p.gridIndex == p.level - (f1.tileLevels + 1) || p.gridIndex == static_cast<int>(f1.coarse->heights.size()) - 1));
			CHECK(p.gridIndex < static_cast<int>(f1.coarse->heights.size()));
		}
		else
		{
			++fine;
			CHECK(p.level <= f1.tileLevels);
			// every tile under a fine patch is in the atlas
			const double ax = p.x0 + origin.x, az = p.z0 + origin.z;
			for (int tz = static_cast<int>(std::floor(az / 512.0)); tz <= static_cast<int>(std::ceil((az + p.size) / 512.0)) - 1; ++tz)
				for (int tx = static_cast<int>(std::floor(ax / 512.0)); tx <= static_cast<int>(std::ceil((ax + p.size) / 512.0)) - 1; ++tx)
					if (tx < 12 && tz < 12) REQUIRE(rt.atlas()->contains({ tx, tz }));
		}
	}
	CHECK(fine > 0);
	MESSAGE("frame: " << f1.patches.size() << " patches (" << fine << " on tiles, " << coarse << " on the coarse world), " << f1.uploads.size() << " uploads");
	CHECK(f1.worldMaxX == doctest::Approx(12 * 512.0f));
	CHECK(f1.worldMaxZ == doctest::Approx(12 * 512.0f));
	CHECK(f1.slabDistance == doctest::Approx(0.0f));                  // y = 900 is inside the slab [0, 3000]
	CHECK(f1.cameraAbsolute.x == doctest::Approx(3000.0));
	CHECK(f1.coarse == src->coarse());

	// above the slab the vertical term appears, for the morph distance of every vertex
	const glm::dvec3 high(3000.0, 4200.0, 3000.0);
	REQUIRE(settle(rt, high));
	LandscapeFrame f3;
	rt.buildFrame(viewProjRelative(high, origin, glm::dvec3(1, -0.4, 0.2)), origin, false, f3);
	CHECK(f3.slabDistance == doctest::Approx(1200.0f));

	// Once the runtime lets go, nothing of it keeps the source alive: not a job, not a queue entry (a pack file stays open as long as
	// anything holds its source, and Windows cannot delete an open file).
	rt.reset();
	CHECK(src.use_count() == 1);
}

// Thema 326, step 14: LandscapeStore::invalidate's only wired consumer so far. tileChanged() must not touch m_generation
// (LandscapeFrame::coarseGeneration): every backend wipes the atlas's GPU arrays and slot table to empty on a generation
// change and only replays uploads/tableUpdates collected since the last buildFrame, so bumping it would blank every tile
// the edit did not itself touch (see the hive lesson on LandscapeRuntime.cpp). The tile's own slot comes back through the
// ordinary store/atlas sync instead: invalidate() makes it non-resident, sync() releases the slot, the store reloads it,
// sync() takes it up again.
TEST_CASE("landscape runtime: tileChanged drops a tile so the atlas gives its slot back and takes it up again, without touching the coarse generation")
{
	auto src = smallRealWorld();
	LandscapeRuntime rt;
	LandscapeRuntimeConfig cfg;
	REQUIRE(rt.begin(src, cfg));
	const glm::dvec3 cam(3000.0, 900.0, 3000.0);
	const glm::dvec3 origin(0.0);
	const glm::dmat4 vp = viewProjRelative(cam, origin, glm::dvec3(1, -0.1, 0.2));

	REQUIRE(settle(rt, cam));
	LandscapeFrame f0;
	rt.buildFrame(vp, origin, false, f0);                  // drains the initial uploads
	REQUIRE(f0.active);
	const uint64_t generationBefore = f0.coarseGeneration;

	const LandscapeTileKey key = rt.store()->residentKeys().front();
	REQUIRE(rt.atlas()->contains(key));
	const uint16_t oldSlot = rt.atlas()->slotOf(key);

	rt.tileChanged({ key });
	CHECK_FALSE(rt.store()->isResident(key));              // gone at once

	rt.updateAt(cam, glm::vec3(0.0f), 1.0);                // the atlas notices the store dropped it on the very next sync
	LandscapeFrame f1;
	rt.buildFrame(vp, origin, false, f1);
	bool releasedOld = false;
	for (const auto& r : f1.releases) if (r.key == key && r.slot == oldSlot) releasedOld = true;
	CHECK(releasedOld);
	CHECK_FALSE(rt.atlas()->contains(key));
	CHECK(f1.coarseGeneration == generationBefore);        // the trap: an edit must not look like "the runtime began again"

	REQUIRE(settle(rt, cam));
	LandscapeFrame f2;
	rt.buildFrame(vp, origin, false, f2);
	bool uploadedAgain = false;
	for (const auto& u : f2.uploads) if (u.key == key) uploadedAgain = true;
	CHECK(uploadedAgain);
	CHECK(rt.atlas()->contains(key));
	CHECK(f2.coarseGeneration == generationBefore);

	// a runtime that has not begun ignores tileChanged instead of crashing
	LandscapeRuntime idle;
	idle.tileChanged({ key });
}

TEST_CASE("landscape runtime: the same view gives the same patches whatever the floating origin is")
{
	auto src = smallRealWorld();
	LandscapeRuntime rt;
	REQUIRE(rt.begin(src, LandscapeRuntimeConfig{}));
	const glm::dvec3 cam(2500.0, 700.0, 3500.0), dir(0.7, -0.2, -0.5);
	REQUIRE(settle(rt, cam));
	LandscapeFrame a, b;
	const glm::dvec3 oa(0.0), ob(2048.0, 100.0, -4096.0);
	rt.buildFrame(viewProjRelative(cam, oa, dir), oa, false, a);
	rt.buildFrame(viewProjRelative(cam, ob, dir), ob, false, b);
	REQUIRE(a.patches.size() == b.patches.size());
	REQUIRE_FALSE(a.patches.empty());
	for (size_t i = 0; i < a.patches.size(); ++i)
	{
		CHECK(a.patches[i].level == b.patches[i].level);
		CHECK(a.patches[i].quadrants == b.patches[i].quadrants);
		CHECK(a.patches[i].x0 + oa.x == doctest::Approx(b.patches[i].x0 + ob.x));
		CHECK(a.patches[i].z0 + oa.z == doctest::Approx(b.patches[i].z0 + ob.z));
	}
	// the frustum culls: looking the other way gives other patches
	LandscapeFrame c;
	rt.buildFrame(viewProjRelative(cam, oa, -dir), oa, false, c);
	CHECK(c.patches.size() > 0);
}

TEST_CASE("landscape runtime: the render frame is the frame with its images packed: bytes as the layout says, one shared static part, uploads once")
{
	auto src = smallRealWorld();
	LandscapeRuntime rt;
	LandscapeRuntimeConfig cfg;
	REQUIRE(rt.begin(src, cfg));
	const glm::dvec3 cam(3000.0, 900.0, 3000.0);
	const glm::dvec3 origin(1024.0, 0.0, -512.0);
	const glm::dmat4 vp = viewProjRelative(cam, origin, glm::dvec3(1, -0.1, 0.2));

	LandscapeRenderFrame none;
	rt.buildRenderFrame(vp, origin, false, none);
	CHECK_FALSE(none.active);                                          // nothing before the first update

	REQUIRE(settle(rt, cam));
	LandscapeRenderFrame f1, f2;
	rt.buildRenderFrame(vp, origin, false, f1);
	REQUIRE(f1.active);
	REQUIRE(f1.world);
	const LandscapeRenderStatic& w = *f1.world;
	CHECK_FALSE(f1.patches.empty());
	CHECK(f1.uploads.size() == rt.atlas()->stats().uploads);           // everything the atlas took is handed over
	CHECK(f1.origin == origin);
	CHECK(f1.cameraRel.x == doctest::Approx(3000.0f - 1024.0f));
	CHECK(f1.slabDistance == doctest::Approx(0.0f));

	// the static part: numbers, the coarse images, the grid
	const LandscapeTileImageLayout layout = LandscapeTileImageLayout::forGeometry(src->geometry());
	CHECK(w.tilesX == 12);
	CHECK(w.tileM == doctest::Approx(512.0f));
	CHECK(w.worldMaxX == doctest::Approx(12 * 512.0f));
	CHECK(w.slotCapacity == static_cast<int>(rt.atlas()->capacity()));
	CHECK(w.tableWindow == rt.atlas()->window());
	CHECK(w.heightW == layout.heightW);
	CHECK(w.weightPages == layout.weightPages);
	CHECK(w.heightLevels == static_cast<int>(layout.heightRect.size()));
	CHECK(w.coarseHeights.size() == static_cast<size_t>(w.coarseW) * static_cast<size_t>(w.coarseH));
	REQUIRE(w.coarseWeights.size() == static_cast<size_t>(layout.weightPages));
	for (const auto& page : w.coarseWeights) CHECK(page.size() == static_cast<size_t>(w.coarseWeightW) * static_cast<size_t>(w.coarseWeightH) * 4);
	CHECK(w.patchIndices.size() == 24576);
	CHECK(w.quadrantFirst[3] + w.quadrantCount[3] == w.patchIndices.size());
	for (int i = 0; i < 12; ++i) CHECK(w.layerColor[i][3] == doctest::Approx(1.0f));

	// the uploads: sizes as the layout says, and the bytes are what landscapePackTile makes of that slot's tile
	REQUIRE_FALSE(f1.uploads.empty());
	std::set<uint16_t> slots;
	for (const LandscapeRenderUpload& u : f1.uploads)
	{
		CHECK(u.slot < w.slotCapacity);
		CHECK(slots.insert(u.slot).second);
		CHECK(u.heights.size() == static_cast<size_t>(w.heightW) * static_cast<size_t>(w.heightH));
		REQUIRE(u.pages.size() == static_cast<size_t>(layout.weightPages));
		for (const auto& page : u.pages) CHECK(page.size() == static_cast<size_t>(w.weightW) * static_cast<size_t>(w.weightH) * 4);
	}
	{
		const LandscapeRenderUpload& u = f1.uploads.front();
		LandscapeTileKey key{ -1, -1 };
		for (const LandscapeTileKey& k : rt.store()->residentKeys())
			if (rt.atlas()->slotOf(k) == u.slot) key = k;
		REQUIRE(key.x >= 0);
		const auto tile = rt.store()->tile(key);
		REQUIRE(tile);
		std::vector<uint16_t> heights;
		std::vector<std::vector<uint8_t>> pages;
		landscapePackTile(*tile, layout, heights, pages);
		CHECK(heights == u.heights);
		CHECK(pages == u.pages);
		CHECK(tile.use_count() == 2);                                  // the store's and ours: the frame does not keep a tile
	}

	// the table the renderer builds from the updates is the atlas's
	std::vector<uint16_t> table(static_cast<size_t>(w.tableWindow) * static_cast<size_t>(w.tableWindow), LandscapeAtlas::kNoSlot);
	for (const auto& t : f1.tableUpdates) table[t.index] = t.slot;
	CHECK(table == rt.atlas()->table());

	// patches: the integer corner cell the shader uses
	for (const LandscapeRenderPatch& p : f1.patches)
	{
		CHECK(p.cornerX * (static_cast<double>(p.size) / 64.0) == doctest::Approx(p.x0 + origin.x));
		CHECK(p.cornerZ * (static_cast<double>(p.size) / 64.0) == doctest::Approx(p.z0 + origin.z));
		CHECK(p.morphStart < p.morphEnd);
	}

	// the second frame: the same static part, nothing to upload
	rt.buildRenderFrame(vp, origin, false, f2);
	CHECK(f2.world == f1.world);
	CHECK(f2.uploads.empty());
	CHECK(f2.tableUpdates.empty());
	CHECK(f2.patches.size() == f1.patches.size());

	// the provider IRenderer takes is the same call, with the origin it is asked for each time
	glm::dvec3 askedOrigin = origin;
	const LandscapeProvider provider = rt.renderProvider([&askedOrigin]() { return askedOrigin; });
	LandscapeRenderFrame f4;
	provider(vp, false, f4);
	CHECK(f4.active);
	CHECK(f4.origin == origin);
	askedOrigin = glm::dvec3(2048.0, 0.0, 0.0);
	provider(viewProjRelative(cam, askedOrigin, glm::dvec3(1, -0.1, 0.2)), false, f4);
	CHECK(f4.origin == askedOrigin);

	// a landscape that begins again is a new one: the renderer is told by a new static part
	REQUIRE(rt.begin(src, cfg));
	REQUIRE(settle(rt, cam));
	LandscapeRenderFrame f3;
	rt.buildRenderFrame(vp, origin, false, f3);
	REQUIRE(f3.world);
	CHECK(f3.world != f1.world);
	CHECK(f3.world->generation != f1.world->generation);
	CHECK(f3.uploads.size() == rt.atlas()->stats().uploads);
}

TEST_CASE("landscape runtime: the scene's LandscapeWorldComponent starts it from a pack and takes it away again")
{
	const auto src = smallRealWorld(8);
	const auto path = std::filesystem::temp_directory_path() / ("he_landscape_runtime_" + std::to_string(std::random_device{}()) + ".bin");
	{
		LandscapePackWriter w;
		REQUIRE(w.begin(path.string(), src->geometry(), 8, 8));
		for (int z = 0; z < 8; ++z)
			for (int x = 0; x < 8; ++x) REQUIRE(w.add({ x, z }, encodeLandscapeTile(src->generate({ x, z }))));
		REQUIRE(w.finish());
	}

	HorizonWorld world;
	LandscapeRuntime rt;
	rt.update(world, 0.016f);                                     // no component, no camera: nothing happens
	CHECK_FALSE(rt.active());

	const Entity landscape = world.createEntity("Landscape");
	auto& comp = world.registry().emplace<LandscapeWorldComponent>(landscape);
	comp.pack = path.string();
	const Entity cam = world.createEntity("Camera");
	world.registry().get_or_emplace<TransformComponent>(cam).position = { 2000.0f, 600.0f, 2000.0f };
	world.registry().emplace<CameraComponent>(cam);

	rt.update(world, 0.016f);
	REQUIRE(rt.active());
	for (int i = 0; i < 3000 && !(rt.store()->settled() && rt.atlas()->stats().used == rt.store()->residentKeys().size()); ++i)
	{
		rt.update(world, 0.016f);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	CHECK(rt.store()->settled());
	CHECK(rt.store()->stats().resident == 64);                    // the whole 4 km world is inside the 8 km radius

	// a floating origin: the camera's absolute position is its transform plus the origin
	world.setOrigin({ 100000.0, 0.0, 0.0 });
	rt.update(world, 0.016f);
	LandscapeFrame f;
	rt.buildFrame(glm::dmat4(1.0), world.origin(), false, f);
	CHECK(f.cameraAbsolute.x == doctest::Approx(102000.0));
	world.setOrigin({ 0.0, 0.0, 0.0 });

	// disabled or removed: the runtime lets go of the pack
	comp.enabled = false;
	rt.update(world, 0.016f);
	CHECK_FALSE(rt.active());
	comp.enabled = true;
	rt.update(world, 0.016f);
	CHECK(rt.active());
	world.registry().remove<LandscapeWorldComponent>(landscape);
	rt.update(world, 0.016f);
	CHECK_FALSE(rt.active());
	rt.reset();
	std::filesystem::remove(path);

	// a component that names a pack that is not there: a warning once, no crash, no landscape
	world.registry().emplace<LandscapeWorldComponent>(landscape).pack = (path.string() + ".missing");
	const int attempts = rt.beginAttempts();
	rt.update(world, 0.016f);
	rt.update(world, 0.016f);
	rt.update(world, 0.016f);
	CHECK_FALSE(rt.active());
	CHECK(rt.beginAttempts() == attempts + 1);                    // tried once, warned once, not every frame
	world.registry().get<LandscapeWorldComponent>(landscape).pack = path.string() + ".other";
	rt.update(world, 0.016f);
	CHECK(rt.beginAttempts() == attempts + 2);                    // another path is a new try
}

TEST_CASE("landscape runtime: beyond the tile levels the patches read the coarse world's grids")
{
	auto src = smallRealWorld();                       // 6.1 km
	LandscapeRuntimeConfig cfg;
	cfg.rangeFactor = 3.0f;                            // level 3 only out to 3 km: the far corner of the world is coarse
	cfg.store.loadRadius = 6000.0f;                    // >= 3072 + sqrt(2) * 1024
	LandscapeRuntime rt;
	std::string err;
	REQUIRE_MESSAGE(rt.begin(src, cfg, &err), err);
	const glm::dvec3 cam(300.0, 900.0, 300.0);        // in a corner
	REQUIRE(settle(rt, cam));
	LandscapeFrame f;
	rt.buildFrame(viewProjRelative(cam, glm::dvec3(0.0), glm::dvec3(1, -0.05, 1)), glm::dvec3(0.0), false, f);
	int coarse = 0;
	std::set<int> grids;
	for (const auto& p : f.patches)
		if (p.coarse)
		{
			++coarse;
			grids.insert(p.gridIndex);
			CHECK(p.level > f.tileLevels);
			CHECK(p.gridIndex >= 0);
			CHECK(p.gridIndex < static_cast<int>(f.coarse->heights.size()));
			// the grid the patch reads has the cell size of its level
			CHECK(f.coarse->cellM(p.gridIndex) == doctest::Approx(rt.quadtree().cellSize(p.level)).epsilon(1e-4));
		}
	CHECK(coarse > 0);
	MESSAGE("coarse patches: " << coarse << " on grids " << grids.size());

	// a rangeFactor that cannot hide the seams is refused
	LandscapeRuntimeConfig bad;
	bad.rangeFactor = 2.0f;
	bad.morphStart = 0.5f;
	CHECK_FALSE(rt.begin(src, bad, &err));
	CHECK(err.find("sqrt(2)") != std::string::npos);
}

TEST_CASE("landscape runtime: the heights' metres come from the pack when it says them")
{
	const auto src = smallRealWorld(8);
	const auto path = std::filesystem::temp_directory_path() / ("he_landscape_range_" + std::to_string(std::random_device{}()) + ".bin");
	const auto writeWith = [&](float minM, float scaleM) {
		LandscapePackWriter w;
		REQUIRE(w.begin(path.string(), src->geometry(), 8, 8, 4, minM, scaleM));
		for (int z = 0; z < 8; ++z)
			for (int x = 0; x < 8; ++x) REQUIRE(w.add({ x, z }, encodeLandscapeTile(src->generate({ x, z }))));
		REQUIRE(w.finish());
	};
	LandscapeRuntime rt;
	LandscapeRuntimeConfig cfg;                         // 0 .. 3000 unless the pack says otherwise
	std::string err;

	writeWith(-50.0f, 1800.0f);
	REQUIRE_MESSAGE(rt.beginFromPack(path.string(), cfg, &err), err);
	CHECK(rt.quadtree().minY == doctest::Approx(-50.0f));
	CHECK(rt.quadtree().maxY == doctest::Approx(1750.0f));
	{
		const auto pack = LandscapePackSource::open(path.string());
		float mn = 0, sc = 0;
		REQUIRE(pack);
		CHECK(pack->heightRange(mn, sc));
		CHECK(mn == doctest::Approx(-50.0f));
		CHECK(sc == doctest::Approx(1800.0f));
	}
	rt.reset();

	writeWith(0.0f, 0.0f);                              // a pack that does not say: the config's numbers count
	cfg.heightMinM = 10.0f;
	cfg.heightScaleM = 900.0f;
	REQUIRE_MESSAGE(rt.beginFromPack(path.string(), cfg, &err), err);
	CHECK(rt.quadtree().minY == doctest::Approx(10.0f));
	CHECK(rt.quadtree().maxY == doctest::Approx(910.0f));
	rt.reset();
	std::filesystem::remove(path);
}

TEST_CASE("landscape world component: saved with the scene, loaded back, absent in a scene from before it")
{
	const auto file = std::filesystem::temp_directory_path() / ("he_landscape_scene_" + std::to_string(std::random_device{}()) + ".json");
	{
		HorizonWorld world;
		const Entity e = world.createEntity("World");
		auto& lw = world.registry().emplace<LandscapeWorldComponent>(e);
		lw.pack = "Landscape/world.hlp";
		lw.loadRadius = 6500.0f;
		lw.budgetMB = 200;
		lw.heightMinM = -20.0f;
		lw.heightScaleM = 2400.0f;
		lw.enabled = false;
		const Entity plain = world.createEntity("Plain");
		(void)plain;
		SceneSerializer ser;
		REQUIRE(ser.save(world, file, SerializeFormat::JSON));
	}
	HorizonWorld back;
	SceneSerializer ser;
	REQUIRE(ser.load(back, file, SerializeFormat::JSON));
	int found = 0;
	for (auto e : back.registry().view<LandscapeWorldComponent>())
	{
		const auto& lw = back.registry().get<LandscapeWorldComponent>(e);
		++found;
		CHECK(lw.pack == "Landscape/world.hlp");
		CHECK(lw.loadRadius == doctest::Approx(6500.0f));
		CHECK(lw.budgetMB == 200u);
		CHECK(lw.heightMinM == doctest::Approx(-20.0f));
		CHECK(lw.heightScaleM == doctest::Approx(2400.0f));
		CHECK_FALSE(lw.enabled);
	}
	CHECK(found == 1);

	// a scene without the key (every scene written before) loads without the component, and keys it does not know about stay harmless
	std::filesystem::remove(file);
	{
		HorizonWorld world;
		world.createEntity("Old");
		SceneSerializer s2;
		REQUIRE(s2.save(world, file, SerializeFormat::JSON));
	}
	HorizonWorld old;
	REQUIRE(ser.load(old, file, SerializeFormat::JSON));
	CHECK(old.registry().view<LandscapeWorldComponent>().size() == 0);
	std::filesystem::remove(file);
}

TEST_CASE("landscape runtime: the component's texture group and tile size survive a save and a load")
{
	HorizonWorld world;
	const Entity e = world.createEntity("Landscape");
	auto& c = world.registry().emplace<LandscapeWorldComponent>(e);
	c.pack = "world.bin";
	c.textureGroup = 2;
	c.textureMeters = 12.5f;

	SceneSerializer ser;
	std::vector<uint8_t> bytes;
	REQUIRE(ser.saveToMemory(world, bytes));
	HorizonWorld back;
	REQUIRE(ser.loadFromMemory(back, bytes));
	size_t n = 0;
	for (auto [be, lw] : back.registry().view<LandscapeWorldComponent>().each())
	{
		++n;
		CHECK(lw.pack == "world.bin");
		CHECK(lw.textureGroup == 2);
		CHECK(lw.textureMeters == doctest::Approx(12.5f));
	}
	CHECK(n == 1);
}
