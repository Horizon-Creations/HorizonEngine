// LandscapeHlod (Thema 326, step 6): the data model and CPU selector for entity-
// cell HLOD proxies. What the baker actually reads from (a cell's placed static
// meshes) and what draws the proxy (a hidden MeshComponent entity, mirroring
// LandscapeWorldProxies) are both future work — see LandscapeHlod.h's file
// comment; this pins the merge, the round-trip and the selector's rule.
#include "doctest.h"
#include <HorizonScene/CellStreamer.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LandscapeHlod.h>
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

TEST_CASE("bakeCellHlod: merges sources, numbers indices across them, flat-shades a source without normals")
{
	HE::LandscapeHlodSourceMesh tri;   // one triangle in the XZ plane, no normals given
	tri.positions = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };
	tri.indices   = { 0, 1, 2 };

	HE::LandscapeHlodSourceMesh quad;   // two triangles, its own (upward) normals
	quad.positions = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f } };
	quad.normals   = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 } };
	quad.indices   = { 0, 1, 2, 0, 2, 3 };

	const HE::LandscapeHlodMesh merged = HE::bakeCellHlod({ tri, quad });
	CHECK(merged.vertices.size() == 7);   // 3 + 4, no welding
	CHECK(merged.indices.size() == 9);    // 3 + 6
	// quad's indices are offset by tri's vertex count (3), so its first triangle is 3,4,5.
	CHECK(merged.indices[3] == 3);
	CHECK(merged.indices[4] == 4);
	CHECK(merged.indices[5] == 5);
	// tri had no normals: every one of its three vertices got the flat cross-product
	// normal of cross(b - a, c - a) — (0, 0, 0) → (1, 0, 0) → (0, 0, 1) winds to -Y,
	// not +Y (the same "clockwise in x,z is a downward normal" rule WaterMesh.h
	// documents), so this pins the actual convention, not an assumed one.
	for (int i = 0; i < 3; ++i)
	{
		CHECK(merged.vertices[i].normal.y == doctest::Approx(-1.0f));
		CHECK(merged.vertices[i].normal.x == doctest::Approx(0.0f));
	}
	// quad kept its own normals, untouched.
	CHECK(merged.vertices[4].normal.y == doctest::Approx(1.0f));
}

TEST_CASE("bakeCellHlod: skips a malformed source instead of producing garbage")
{
	HE::LandscapeHlodSourceMesh notTriangles;
	notTriangles.positions = { { 0, 0, 0 }, { 1, 0, 0 } };
	notTriangles.indices   = { 0, 1 };   // not a multiple of 3

	HE::LandscapeHlodSourceMesh outOfBounds;
	outOfBounds.positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 0, 1 } };
	outOfBounds.indices   = { 0, 1, 5 };   // 5 has no position

	HE::LandscapeHlodSourceMesh good;
	good.positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 0, 1 } };
	good.indices   = { 0, 1, 2 };

	const HE::LandscapeHlodMesh merged = HE::bakeCellHlod({ notTriangles, outOfBounds, good });
	CHECK(merged.vertices.size() == 3);
	CHECK(merged.indices.size() == 3);
}

TEST_CASE("LandscapeHlodTable: encode and decode round-trip every cell's mesh exactly")
{
	HE::LandscapeHlodSourceMesh tri;
	tri.positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 0, 1 } };
	tri.uvs       = { { 0, 0 }, { 1, 0 }, { 0, 1 } };
	tri.indices   = { 0, 1, 2 };
	const HE::LandscapeHlodMesh mesh = HE::bakeCellHlod({ tri });
	REQUIRE_FALSE(mesh.empty());

	HE::LandscapeHlodTable table;
	table.set(0, 0, mesh);
	table.set(-3, 7, mesh);
	REQUIRE(table.cellCount() == 2);

	const std::vector<uint8_t> bytes = table.encode();
	HE::LandscapeHlodTable round;
	REQUIRE(HE::LandscapeHlodTable::decode(bytes, round));
	REQUIRE(round.cellCount() == 2);
	for (auto [x, z] : std::vector<std::pair<int, int>>{ { 0, 0 }, { -3, 7 } })
	{
		const HE::LandscapeHlodMesh* m = round.find(x, z);
		REQUIRE(m != nullptr);
		REQUIRE(m->vertices.size() == mesh.vertices.size());
		REQUIRE(m->indices == mesh.indices);
		for (size_t i = 0; i < m->vertices.size(); ++i)
		{
			CHECK(m->vertices[i].position == mesh.vertices[i].position);
			CHECK(m->vertices[i].uv == mesh.vertices[i].uv);
		}
	}

	// A buffer that is not this table's own: decode refuses it rather than reading
	// whatever garbage the counts would otherwise walk off into.
	HE::LandscapeHlodTable broken;
	CHECK_FALSE(HE::LandscapeHlodTable::decode({ 1, 2, 3 }, broken));
}

TEST_CASE("LandscapeHlodTable: an empty mesh is not stored, same as never having been set")
{
	HE::LandscapeHlodTable table;
	table.set(0, 0, HE::LandscapeHlodMesh{});
	CHECK(table.cellCount() == 0);
	CHECK_FALSE(table.has(0, 0));
}

namespace
{
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

template <class Done>
void streamAt(HE::CellStreamer& s, HorizonWorld& world, const glm::dvec3& at, Done done)
{
	for (int i = 0; i < 3000 && !done(); ++i)
	{
		s.update(world, at, glm::vec3(0.0f), 4.0);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

// loadRadius 40 m: standing dead centre of cell (0,0) (distance 0) is well inside
// it, while cell (1,0)'s square starts 50 m away — just outside it — so only (0,0)
// loads from there, with no region gate needed to keep (1,0) out.
constexpr const char* kManifest =
	R"({"version": 2, "cellSize": 100, "loadRadius": 40, "unloadRadius": 50,
	    "lookaheadSec": 0, "dir": "H.cells", "list": [[0,0,1,0],[1,0,1,0]]})";

HE::LandscapeHlodMesh oneTriangle()
{
	HE::LandscapeHlodSourceMesh tri;
	tri.positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 0, 1 } };
	tri.indices   = { 0, 1, 2 };
	return HE::bakeCellHlod({ tri });
}
} // namespace

TEST_CASE("hlodCellsToDraw: only the cell that is NOT fully loaded is selected")
{
	HorizonWorld world;
	HE::CellStreamer streamer;
	Files files;
	files["H.cells/cell_0_0.hescene"] = cellBytes(0.0f, 0.0f);
	files["H.cells/cell_1_0.hescene"] = cellBytes(100.0f, 0.0f);
	HE::CellManifest m;
	REQUIRE(HE::CellManifest::parse(kManifest, m));
	streamer.begin(m, readerOver(files), {});

	HE::LandscapeHlodTable table;
	table.set(0, 0, oneTriangle());
	table.set(1, 0, oneTriangle());

	// Anchor dead centre of cell (0,0): cell (1,0) is 50 m past the 40 m load
	// radius, so it never loads here — no region gate needed for this selector.
	streamer.setRegionConfig({});   // explicit: this selector does not need the gate
	streamAt(streamer, world, { 50.0, 0.0, 50.0 }, [&] { return streamer.isLoaded(0, 0); });
	REQUIRE(streamer.isLoaded(0, 0));
	REQUIRE_FALSE(streamer.isLoaded(1, 0));

	const std::vector<HE::LandscapeHlodCellKey> drawn = HE::hlodCellsToDraw(table, streamer);
	REQUIRE(drawn.size() == 1);
	CHECK(drawn[0].x == 1);
	CHECK(drawn[0].z == 0);

	streamer.clear(world);
}
