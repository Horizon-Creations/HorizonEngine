// The cell HLOD baker (Thema 326, step 6): the static meshes of a cell file become one merged proxy in absolute metres, moving or skinned
// content is left out, the budgets cut the small instances first, and the table is appended to (and replaced in) an existing pack.
#include "doctest.h"
#include <ContentManager/ContentManager.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LandscapeCellHlodBake.h>
#include <HorizonScene/LandscapeSource.h>
#include <HorizonScene/SceneSerializer.h>

#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace
{
using Files = std::map<std::string, std::vector<uint8_t>>;

// A unit quad (2 triangles) in the XZ plane, `size` metres a side.
HE::UUID registerQuad(ContentManager& cm, float size)
{
	StaticMeshAsset a;
	a.vertices = { 0, 0, 0, size, 0, 0, size, 0, size, 0, 0, size };
	a.normals = { 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0 };
	a.uvs = { 0, 0, 1, 0, 1, 1, 0, 1 };
	a.indices = { 0, 1, 2, 0, 2, 3 };
	return cm.registerStaticMesh(std::move(a));
}

struct Placed
{
	HE::UUID mesh;
	glm::vec3 at;
	bool dynamic = false;
};

std::vector<uint8_t> cellOf(const std::vector<Placed>& list)
{
	HorizonWorld w;
	int n = 0;
	for (const Placed& p : list)
	{
		const Entity e = w.createEntity("M" + std::to_string(n++));
		TransformComponent t;
		t.position = p.at;
		w.addComponent(e, t);
		MeshComponent mc;
		mc.meshAssetId = p.mesh;
		w.registry().emplace<MeshComponent>(e, mc);
		if (p.dynamic)
		{
			RigidBodyComponent rb;
			rb.type = HE::RigidBodyType::Dynamic;
			w.registry().emplace<RigidBodyComponent>(e, rb);
		}
	}
	SceneSerializer ser;
	std::vector<uint8_t> bytes;
	REQUIRE(ser.saveToMemory(w, bytes));
	return bytes;
}

HE::CellStreamer::Reader readerOver(const Files& files)
{
	return [&files](const std::string& path) -> std::function<bool(std::vector<uint8_t>&)> {
		const auto it = files.find(path);
		if (it == files.end()) return {};
		const std::vector<uint8_t> bytes = it->second;
		return [bytes](std::vector<uint8_t>& out) { out = bytes; return true; };
	};
}

HE::CellManifest manifestOf(std::initializer_list<std::pair<int, int>> cells)
{
	std::string list;
	for (auto c : cells) list += std::string(list.empty() ? "" : ",") + "[" + std::to_string(c.first) + "," + std::to_string(c.second) + ",1,0]";
	HE::CellManifest m;
	REQUIRE(HE::CellManifest::parse(R"({"version": 2, "cellSize": 100, "loadRadius": 40, "unloadRadius": 50, "lookaheadSec": 0, "dir": "H.cells", "list": [)" + list + "]}", m));
	return m;
}
} // namespace

TEST_CASE("bakeCellProxies: static meshes are merged in absolute metres, a moving body is left out, a missing cell and an empty cell are counted")
{
	ContentManager cm{ "." };
	const HE::UUID quad = registerQuad(cm, 10.0f);
	Files files;
	files["H.cells/cell_0_0.hescene"] = cellOf({ { quad, { 20, 0, 30 } }, { quad, { 60, 0, 30 } }, { quad, { 40, 0, 40 }, true } });
	files["H.cells/cell_1_0.hescene"] = cellOf({});
	const HE::CellManifest m = manifestOf({ { 0, 0 }, { 1, 0 }, { 2, 0 } });   // (2,0) has no file

	HE::CellHlodBakeStats st;
	const HE::LandscapeHlodTable table = HE::bakeCellProxies(m, readerOver(files), cm, {}, &st);
	CHECK(st.cells == 3);
	CHECK(st.baked == 1);
	CHECK(st.empty == 1);
	CHECK(st.failed == 1);
	CHECK(st.instances == 2);   // the dynamic one is not merged
	REQUIRE(table.find(0, 0));
	const HE::LandscapeHlodMesh& mesh = *table.find(0, 0);
	CHECK(mesh.indices.size() == 12);   // two quads
	float minX = 1e9f, maxX = -1e9f;
	for (const auto& v : mesh.vertices) { minX = std::min(minX, v.position.x); maxX = std::max(maxX, v.position.x); }
	CHECK(minX == doctest::Approx(20.0f));
	CHECK(maxX == doctest::Approx(70.0f));
	CHECK(table.find(1, 0) == nullptr);
}

TEST_CASE("bakeCellProxies: the budgets drop the small instances, not the big ones")
{
	ContentManager cm{ "." };
	const HE::UUID big = registerQuad(cm, 50.0f), small = registerQuad(cm, 1.0f);
	Files files;
	files["H.cells/cell_0_0.hescene"] = cellOf({ { small, { 1, 0, 1 } }, { big, { 30, 0, 30 } }, { small, { 5, 0, 5 } } });
	const HE::CellManifest m = manifestOf({ { 0, 0 } });
	HE::CellHlodBakeOptions opt;
	opt.maxTrianglesPerCell = 2;   // room for one quad
	HE::CellHlodBakeStats st;
	const HE::LandscapeHlodTable table = HE::bakeCellProxies(m, readerOver(files), cm, opt, &st);
	REQUIRE(table.find(0, 0));
	CHECK(st.instances == 1);
	CHECK(st.instancesDropped == 2);
	float maxX = -1e9f;
	for (const auto& v : table.find(0, 0)->vertices) maxX = std::max(maxX, v.position.x);
	CHECK(maxX == doctest::Approx(80.0f));   // the 50 m quad at x = 30
}

TEST_CASE("landscapePackSetHlod: appends to a pack without a table, replaces one that has it, removes it with an empty table, refuses a non-pack")
{
	HE::ProceduralLandscapeParams p;
	p.geometry.hCells = 32;
	p.geometry.wTexels = 16;
	p.geometry.channels = 4;
	p.tilesX = p.tilesZ = 2;
	p.seaLevelM = 1500.0f;
	const HE::ProceduralLandscapeSource src(p);
	const auto path = std::filesystem::temp_directory_path() / ("he_setHlod_" + std::to_string(std::random_device{}()) + ".bin");
	{
		HE::LandscapePackWriter w;
		REQUIRE(w.begin(path.string(), src.geometry(), 2, 2, 3));
		w.setWaterTable(*src.water());
		for (int z = 0; z < 2; ++z)
			for (int x = 0; x < 2; ++x) REQUIRE(w.add({ x, z }, HE::encodeLandscapeTile(src.generate({ x, z }))));
		REQUIRE(w.finish());
	}
	const auto tri = [](float s) {
		HE::LandscapeHlodSourceMesh t;
		t.positions = { { 0, 0, 0 }, { s, 0, 0 }, { 0, 0, s } };
		t.indices = { 0, 1, 2 };
		return HE::bakeCellHlod({ t });
	};
	const auto sizeOf = [&] { return std::filesystem::file_size(path); };
	const uintmax_t plain = sizeOf();

	HE::LandscapeHlodTable one;
	one.set(1, 2, tri(1.0f));
	std::string err;
	REQUIRE_MESSAGE(HE::landscapePackSetHlod(path.string(), one, &err), err);
	{
		auto a = HE::LandscapePackSource::open(path.string(), &err);
		REQUIRE_MESSAGE(a, err);
		REQUIRE(a->hlod());
		CHECK(a->hlod()->cellCount() == 1);
		CHECK(a->water() != nullptr);
		HE::LandscapeTile t;
		CHECK(a->load({ 1, 0 }, -1, t));
	}
	HE::LandscapeHlodTable two;
	two.set(5, 5, tri(2.0f));
	two.set(6, 5, tri(3.0f));
	REQUIRE_MESSAGE(HE::landscapePackSetHlod(path.string(), two, &err), err);
	{
		auto a = HE::LandscapePackSource::open(path.string(), &err);
		REQUIRE_MESSAGE(a, err);
		REQUIRE(a->hlod());
		CHECK(a->hlod()->cellCount() == 2);
		CHECK(a->hlod()->find(1, 2) == nullptr);   // replaced, not added to
		CHECK(a->hlod()->find(6, 5) != nullptr);
	}
	REQUIRE(HE::landscapePackSetHlod(path.string(), HE::LandscapeHlodTable{}, &err));
	{
		auto a = HE::LandscapePackSource::open(path.string(), &err);
		REQUIRE_MESSAGE(a, err);
		CHECK(a->hlod() == nullptr);
		CHECK(sizeOf() == plain);   // back to the file it was
	}
	const auto junk = std::filesystem::temp_directory_path() / ("he_setHlod_junk_" + std::to_string(std::random_device{}()) + ".bin");
	{ std::ofstream o(junk, std::ios::binary); o << "not a pack at all, not a pack at all, not a pack"; }
	CHECK_FALSE(HE::landscapePackSetHlod(junk.string(), one, &err));
	std::filesystem::remove(junk);
	std::filesystem::remove(path);
}
