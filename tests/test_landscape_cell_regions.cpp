// The region gate for the entity cells of a tile landscape (Thema 326): the policy numbers, and that a manifest wider than the landscape is
// capped by it while one inside the landscape is not.
#include "doctest.h"
#include <HorizonScene/CellStreamer.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LandscapeCellRegions.h>
#include <HorizonScene/SceneJsonParse.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <map>
#include <thread>

using json = nlohmann::json;

namespace
{
HE::CellManifest manifestOf(float cellSize, float load, float unload, const char* list)
{
	HE::CellManifest m;
	REQUIRE(HE::CellManifest::parse(std::string(R"({"version": 2, "cellSize": )") + std::to_string(cellSize) + R"(, "loadRadius": )"
	                                + std::to_string(load) + R"(, "unloadRadius": )" + std::to_string(unload)
	                                + R"(, "lookaheadSec": 0, "dir": "L.cells", "list": )" + list + "}", m));
	return m;
}

std::vector<uint8_t> cellBytes(float x0)
{
	HorizonWorld cell;
	const Entity b = cell.createEntity("Box");
	TransformComponent t;
	t.position = { x0 + 50.0f, 0.0f, 50.0f };
	cell.addComponent(b, t);
	cell.registry().emplace<MeshComponent>(b);
	SceneSerializer ser;
	std::vector<uint8_t> bytes;
	REQUIRE(ser.saveToMemory(cell, bytes));
	json j = HE::parseSceneCbor(bytes);
	j["streaming"] = { { "version", 2 } };
	return json::to_cbor(j);
}
} // namespace

TEST_CASE("cellRegionConfigForLandscape: 4 tiles a side rounded to whole cells, the store's radii, off without cells or tiles")
{
	const HE::CellManifest m = manifestOf(100.0f, 150.0f, 190.0f, "[[0,0,1,0]]");
	const HE::CellRegionConfig rc = HE::cellRegionConfigForLandscape(m, 512.0f, 8000.0f, 1.25f);
	CHECK(rc.regionSize == doctest::Approx(2000.0f));   // 2048 m rounded to 20 cells of 100 m
	CHECK(rc.loadRadius == doctest::Approx(8000.0f));
	CHECK(rc.keepFactor == doctest::Approx(1.25f));

	// a cell bigger than the region is the region
	const HE::CellManifest big = manifestOf(4096.0f, 4000.0f, 5000.0f, "[[0,0,1,0]]");
	CHECK(HE::cellRegionConfigForLandscape(big, 512.0f, 8000.0f).regionSize == doctest::Approx(4096.0f));

	// a radius at a region's half width is nudged off the tie
	const HE::CellRegionConfig tie = HE::cellRegionConfigForLandscape(m, 512.0f, 1000.0f);
	CHECK(tie.loadRadius == doctest::Approx(1001.0f));

	CHECK(HE::cellRegionConfigForLandscape(HE::CellManifest{}, 512.0f, 8000.0f).regionSize == 0.0f);
	CHECK(HE::cellRegionConfigForLandscape(m, 0.0f, 8000.0f).regionSize == 0.0f);
	CHECK(HE::cellRegionConfigForLandscape(m, 512.0f, 0.0f).regionSize == 0.0f);
	CHECK(HE::sameRegionConfig(rc, rc));
	CHECK_FALSE(HE::sameRegionConfig(rc, HE::CellRegionConfig{}));
}

TEST_CASE("cellRegionConfigForLandscape: a manifest wider than the landscape is capped, one inside it is not")
{
	// Cell 0 at [0,100), cell 25 at [2500,2600): a second region (2000 m regions). The manifest takes 4 km; the landscape holds 600 m.
	const char* list = "[[0,0,1,0],[25,0,1,0]]";
	std::map<std::string, std::vector<uint8_t>> files;
	files["L.cells/cell_0_0.hescene"] = cellBytes(0.0f);
	files["L.cells/cell_25_0.hescene"] = cellBytes(2500.0f);
	const auto reader = [&files](const std::string& path) -> std::function<bool(std::vector<uint8_t>&)> {
		const auto it = files.find(path);
		if (it == files.end()) return {};
		const std::vector<uint8_t> bytes = it->second;
		return [bytes](std::vector<uint8_t>& out) { out = bytes; return true; };
	};
	const auto run = [&](float landscapeRadius) {
		HorizonWorld world;
		HE::CellStreamer s;
		const HE::CellManifest m = manifestOf(100.0f, 4000.0f, 5000.0f, list);
		s.begin(m, reader, {});
		s.setRegionConfig(HE::cellRegionConfigForLandscape(m, 512.0f, landscapeRadius));
		for (int i = 0; i < 1500 && !(s.isLoaded(0, 0) && (landscapeRadius < 3000.0f || s.isLoaded(25, 0))); ++i)
		{
			s.update(world, { 50.0, 0.0, 50.0 }, glm::vec3(0.0f), 4.0);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		const std::pair<bool, bool> r{ s.isLoaded(0, 0), s.isLoaded(25, 0) };
		s.clear(world);
		return r;
	};
	const auto capped = run(600.0f);    // region 1 starts 2000 m from the anchor: past the landscape
	CHECK(capped.first);
	CHECK_FALSE(capped.second);
	const auto open = run(8000.0f);     // the landscape reaches it: the manifest's own radius decides, and it takes both
	CHECK(open.first);
	CHECK(open.second);
}
