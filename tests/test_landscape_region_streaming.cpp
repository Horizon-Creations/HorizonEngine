// CellStreamer's region gate (Thema 326, step 6): a coarser hysteresis layered on
// top of the per-cell one, so that entity cells unload together by the region
// their landscape tile belongs to, not only by their own (often much larger)
// manifest radius. CellRegionConfig::regionSize <= 0 (the default, tested
// elsewhere by every existing cell-streaming test, unchanged) turns the gate off;
// here it is always on, with regionSize == cellSize so a region is exactly one
// cell and the maths stays easy to check by hand.
#include "doctest.h"
#include <HorizonScene/CellStreamer.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneJsonParse.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace
{
// One cell's worth of bytes (CBOR, version-2 head): a single box mesh well inside
// the cell square that starts at (x0, z0), so the cell's root is unambiguous.
std::vector<uint8_t> cellBytes(float x0, float z0)
{
	HorizonWorld cell;
	const Entity b = cell.createEntity("Box");
	TransformComponent t;
	t.position = { x0 + 50.0f, 0.0f, z0 + 50.0f };
	cell.addComponent(b, t);
	cell.registry().emplace<MeshComponent>(b);
	SceneSerializer ser;
	std::vector<uint8_t> bytes;
	REQUIRE(ser.saveToMemory(cell, bytes));
	json j = HE::parseSceneCbor(bytes);
	j["streaming"] = { { "version", 2 } };
	return json::to_cbor(j);
}

using Files = std::map<std::string, std::vector<uint8_t>>;

HE::CellStreamer::Reader readerOver(const Files& files)
{
	return [&files](const std::string& path) -> std::function<bool(std::vector<uint8_t>&)>
	{
		const auto it = files.find(path);
		if (it == files.end()) return {};
		const std::vector<uint8_t> bytes = it->second;
		return [bytes](std::vector<uint8_t>& out) { out = bytes; return true; };
	};
}

// Pumps the streamer from one standpoint until `done`, or a couple of seconds have
// passed (the read and parse happen on the job pool).
template <class Done>
void streamAt(HE::CellStreamer& s, HorizonWorld& world, const glm::dvec3& at, Done done)
{
	for (int i = 0; i < 3000 && !done(); ++i)
	{
		s.update(world, at, glm::vec3(0.0f), 4.0);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

// Two 100 m cells along +X ((0,0) at [0,100), (1,0) at [100,200)); the manifest's
// OWN radii are huge, so nothing in this test is ever gated by them — anything
// that loads or unloads here does so because of the region gate alone.
constexpr const char* kManifest =
	R"({"version": 2, "cellSize": 100, "loadRadius": 1000, "unloadRadius": 1250,
	    "lookaheadSec": 0, "dir": "R.cells", "list": [[0,0,1,0],[1,0,1,0]]})";

struct Fixture
{
	HorizonWorld     world;
	HE::CellStreamer streamer;
	Files            files;
	int              regionLoaded = 0, regionUnloaded = 0;
	std::vector<std::pair<int, int>> loadedOrder, unloadedOrder;

	Fixture()
	{
		files["R.cells/cell_0_0.hescene"] = cellBytes(0.0f, 0.0f);
		files["R.cells/cell_1_0.hescene"] = cellBytes(100.0f, 0.0f);
		HE::CellManifest m;
		REQUIRE(HE::CellManifest::parse(kManifest, m));
		HE::CellStreamer::Hooks hooks;
		hooks.regionLoaded = [this](int x, int z) { ++regionLoaded; loadedOrder.emplace_back(x, z); };
		hooks.regionUnloaded = [this](int x, int z) { ++regionUnloaded; unloadedOrder.emplace_back(x, z); };
		streamer.begin(m, readerOver(files), hooks);
		// Region == cell here: load within 40 m, keep until 50 m (the default 1.25x).
		// A region is 100 m square, so its own centre is 50 m from every one of its
		// four edges; keeping the load radius under that (40, not 50) means a point
		// dead centre in a region is never ALSO within reach of a neighbour — no ties.
		HE::CellRegionConfig rc;
		rc.regionSize = 100.0f;
		rc.loadRadius = 40.0f;
		streamer.setRegionConfig(rc);
	}
	~Fixture() { streamer.clear(world); }
};
} // namespace

TEST_CASE("Region gate: a cell only loads while its region is resident")
{
	Fixture f;
	// (50, 50): dead centre of region (0,0)'s square ([0,100) x [0,100)), distance
	// 0 — resident. Region (1,0)'s square ([100,200) x [0,100)) is 50 m away, past
	// the 40 m region load radius, so cell (1,0) must not even start reading.
	streamAt(f.streamer, f.world, { 50.0, 0.0, 50.0 }, [&] { return f.streamer.isLoaded(0, 0); });
	CHECK(f.streamer.isLoaded(0, 0));
	CHECK_FALSE(f.streamer.isLoaded(1, 0));
	CHECK_FALSE(f.streamer.isBuilding(1, 0));
	CHECK(f.streamer.isRegionResident(0, 0));
	CHECK_FALSE(f.streamer.isRegionResident(1, 0));
	CHECK(f.streamer.stats().regionsResident == 1);
	CHECK(f.regionLoaded == 1);
	CHECK(f.loadedOrder == std::vector<std::pair<int, int>>{ { 0, 0 } });
}

TEST_CASE("Region gate: border wobble inside the keep band does not thrash")
{
	Fixture f;
	streamAt(f.streamer, f.world, { 50.0, 0.0, 50.0 }, [&] { return f.streamer.isLoaded(0, 0); });

	// Region (1,0)'s square starts at x = 100: at x = 65 the distance is 35 m (under
	// the 40 m load radius) so it becomes resident; at x = 55 the distance is 45 m —
	// past "load" but still under the 50 m "keep" radius, so it must stay resident,
	// not drop and reload. Oscillate across that band several times.
	for (int i = 0; i < 12; ++i)
	{
		const double x = (i % 2 == 0) ? 65.0 : 55.0;
		streamAt(f.streamer, f.world, { x, 0.0, 50.0 }, [&] { return f.streamer.isLoaded(1, 0); });
	}
	CHECK(f.streamer.isLoaded(1, 0));
	CHECK(f.streamer.stats().loadsDone == 2);   // (0,0) once, (1,0) once
	CHECK(f.streamer.stats().unloads == 0);     // no thrash: never dropped in between
	CHECK(f.regionLoaded == 2);                 // each region announced exactly once
	CHECK(f.regionUnloaded == 0);
}

TEST_CASE("Region gate: unloads a region's cells even though the manifest's own radius would have kept them")
{
	Fixture f;
	streamAt(f.streamer, f.world, { 50.0, 0.0, 50.0 }, [&] { return f.streamer.isLoaded(0, 0); });
	streamAt(f.streamer, f.world, { 65.0, 0.0, 50.0 }, [&] { return f.streamer.isLoaded(1, 0); });
	REQUIRE(f.streamer.isLoaded(0, 0));
	REQUIRE(f.streamer.isLoaded(1, 0));

	// x = 1050 (dead centre of region (10,0), nowhere near a region boundary): 950 m
	// from cell (0,0)'s square, 850 m from cell (1,0)'s — both well inside the
	// manifest's own 1250 m unload radius, so without the region gate neither cell
	// would ever unload from here. Both their regions are past the 50 m keep
	// radius, though, so the gate must drop both anyway — the camera's OWN region
	// (10,0) becomes resident instead, which is the one region left afterwards.
	streamAt(f.streamer, f.world, { 1050.0, 0.0, 50.0 },
	         [&] { return !f.streamer.isLoaded(0, 0) && !f.streamer.isLoaded(1, 0); });
	CHECK_FALSE(f.streamer.isLoaded(0, 0));
	CHECK_FALSE(f.streamer.isLoaded(1, 0));
	CHECK_FALSE(f.streamer.isRegionResident(0, 0));
	CHECK_FALSE(f.streamer.isRegionResident(1, 0));
	CHECK(f.streamer.isRegionResident(10, 0));
	CHECK(f.streamer.stats().regionsResident == 1);
	CHECK(f.regionUnloaded == 2);
}

TEST_CASE("Region gate off (regionSize 0, the default): every region call reports nothing tracked")
{
	HorizonWorld     world;
	HE::CellStreamer streamer;
	Files            files;
	files["R.cells/cell_0_0.hescene"] = cellBytes(0.0f, 0.0f);
	HE::CellManifest m;
	REQUIRE(HE::CellManifest::parse(kManifest, m));
	streamer.begin(m, readerOver(files), {});
	CHECK(streamer.regionConfig().regionSize == 0.0f);
	streamAt(streamer, world, { 10.0, 0.0, 50.0 }, [&] { return streamer.isLoaded(0, 0); });
	CHECK(streamer.isLoaded(0, 0));
	CHECK_FALSE(streamer.isRegionResident(0, 0));
	CHECK(streamer.stats().regionsResident == 0);
	streamer.clear(world);
}
