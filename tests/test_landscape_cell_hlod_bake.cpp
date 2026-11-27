// The cell HLOD baker (Thema 326, step 6): the static meshes of a cell file become one merged proxy in absolute metres, moving or skinned
// content is left out, the budgets cut the small instances first, and the table is appended to (and replaced in) an existing pack.
#include "doctest.h"
#include <ContentManager/ContentManager.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LandscapeCellHlodBake.h>
#include <HorizonScene/LandscapeSource.h>
#include <HorizonScene/SceneSerializer.h>

#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <thread>
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
	HE::UUID material{};   // an override on the entity: the instance's colour
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
		if (p.material != HE::UUID{})
		{
			MaterialComponent matc;
			matc.materialAssetId = p.material;
			w.registry().emplace<MaterialComponent>(e, matc);
		}
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

TEST_CASE("bakeCellProxies: every instance's uv is the palette texel of its material's colour, and the palette survives the table's bytes")
{
	ContentManager cm{ "." };
	const HE::UUID quad = registerQuad(cm, 10.0f);
	const auto material = [&](float r, float g, float b) {
		MaterialAsset a;
		a.type = HE::AssetType::Material;
		a.baseColor[0] = r; a.baseColor[1] = g; a.baseColor[2] = b;
		return cm.registerMaterial(std::move(a));
	};
	const HE::UUID red = material(1.0f, 0.0f, 0.0f), green = material(0.0f, 1.0f, 0.0f);

	HorizonWorld w;
	const HE::UUID mats[3] = { red, green, red };
	for (int i = 0; i < 3; ++i)
	{
		const Entity e = w.createEntity("M" + std::to_string(i));
		TransformComponent t;
		t.position = { 20.0f * static_cast<float>(i), 0.0f, 0.0f };
		w.addComponent(e, t);
		MeshComponent mc;
		mc.meshAssetId = quad;
		w.registry().emplace<MeshComponent>(e, mc);
		MaterialComponent matc;
		matc.materialAssetId = mats[i];
		w.registry().emplace<MaterialComponent>(e, matc);
	}
	Files files;
	REQUIRE(SceneSerializer().saveToMemory(w, files["H.cells/cell_0_0.hescene"]));

	const HE::LandscapeHlodTable table = HE::bakeCellProxies(manifestOf({ { 0, 0 } }), readerOver(files), cm, {}, nullptr);
	REQUIRE(table.find(0, 0));
	REQUIRE(table.palette.size() == 6);   // two colours, the second red shares the first's entry
	const auto entryIs = [&](size_t i, uint8_t r, uint8_t g, uint8_t b) {
		return table.palette[i * 3] == r && table.palette[i * 3 + 1] == g && table.palette[i * 3 + 2] == b;
	};
	const bool redFirst = entryIs(0, 0xF8, 0, 0);
	CHECK((redFirst ? entryIs(1, 0, 0xF8, 0) : (entryIs(0, 0, 0xF8, 0) && entryIs(1, 0xF8, 0, 0))));
	// each vertex's uv is its instance's texel: the quad at x 20..30 is the green one
	const glm::vec2 redUv = HE::landscapeHlodPaletteUv(redFirst ? 0 : 1), greenUv = HE::landscapeHlodPaletteUv(redFirst ? 1 : 0);
	for (const auto& v : table.find(0, 0)->vertices)
	{
		const bool isGreen = v.position.x >= 19.0f && v.position.x <= 31.0f;
		CHECK(v.uv.x == doctest::Approx((isGreen ? greenUv : redUv).x));
		CHECK(v.uv.y == doctest::Approx((isGreen ? greenUv : redUv).y));
	}

	HE::LandscapeHlodTable back;
	REQUIRE(HE::LandscapeHlodTable::decode(table.encode(), back));
	CHECK(back.palette == table.palette);
	// a table from before the palette (version 1: the same bytes without the palette's tail) still reads, without one
	std::vector<uint8_t> old = table.encode();
	old.resize(old.size() - 4 - table.palette.size());
	old[4] = 1;
	REQUIRE(HE::LandscapeHlodTable::decode(old, back));
	CHECK(back.palette.empty());
	CHECK(back.cellCount() == 1);
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

TEST_CASE("bakeCellProxies: a loose JSON cell file (what the editor's splitter writes) bakes like a CBOR one")
{
	ContentManager cm{ "." };
	const HE::UUID quad = registerQuad(cm, 10.0f);
	const std::vector<uint8_t> cbor = cellOf({ { quad, { 20, 0, 30 } } });
	const std::string text = nlohmann::json::from_cbor(cbor).dump();
	Files files;
	files["H.cells/cell_0_0.hescene"] = std::vector<uint8_t>(text.begin(), text.end());
	HE::CellHlodBakeStats st;
	const HE::LandscapeHlodTable table = HE::bakeCellProxies(manifestOf({ { 0, 0 } }), readerOver(files), cm, {}, &st);
	CHECK(st.failed == 0);
	REQUIRE(table.find(0, 0));
	CHECK(table.find(0, 0)->indices.size() == 6);
}

namespace
{
// The files of a test project with a stamp each: the size, and a version counted up by every edit (a real file's mtime).
struct Project
{
	Files files;
	std::map<std::string, int64_t> version;
	void put(const std::string& path, std::vector<uint8_t> bytes) { files[path] = std::move(bytes); ++version[path]; }
	HE::CellHlodStamper stamper() const
	{
		return [this](const std::string& path, HE::CellHlodStamp& out) {
			const auto it = files.find(path);
			if (it == files.end()) return false;
			out.size = it->second.size();
			out.modified = version.at(path);
			return true;
		};
	}
};

// Pumps a bake to its end (the pool does the rest), with a wall-clock cap.
void run(HE::CellHlodBakeJob& job, ContentManager& cm)
{
	for (int i = 0; i < 5000 && !job.finished(); ++i)
	{
		job.pump(cm, 4.0);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	REQUIRE(job.finished());
}
}

TEST_CASE("CellHlodBakeJob: an edit of one cell's file rebakes that cell only; the others keep their proxy and the palette keeps its entries")
{
	ContentManager cm{ "." };
	const HE::UUID quad = registerQuad(cm, 10.0f);
	const auto material = [&](float r, float g, float b) {
		MaterialAsset a;
		a.type = HE::AssetType::Material;
		a.baseColor[0] = r; a.baseColor[1] = g; a.baseColor[2] = b;
		return cm.registerMaterial(std::move(a));
	};
	const HE::UUID red = material(1, 0, 0), green = material(0, 1, 0), blue = material(0, 0, 1);

	Project p;
	p.put("H.cells/cell_0_0.hescene", cellOf({ { quad, { 20, 0, 30 }, false, red } }));
	p.put("H.cells/cell_1_0.hescene", cellOf({ { quad, { 120, 0, 30 }, false, green } }));
	p.put("H.cells/cell_2_0.hescene", cellOf({}));
	const HE::CellManifest m = manifestOf({ { 0, 0 }, { 1, 0 }, { 2, 0 } });
	HE::CellHlodCache cache;
	HE::CellHlodBakeJob job;

	// the first bake: everything
	job.start(m, readerOver(p.files), p.stamper(), {}, cache);
	run(job, cm);
	CHECK(job.stats().cells == 3);
	CHECK(job.stats().reused == 0);
	CHECK(job.stats().baked == 2);
	CHECK(job.stats().empty == 1);
	CHECK(cache.table.cellCount() == 2);
	CHECK(cache.stamps.size() == 3);   // the empty cell has its stamp too: it is not looked at again
	const std::vector<uint8_t> paletteBefore = cache.table.palette;
	REQUIRE(paletteBefore.size() == 6);
	const HE::LandscapeHlodMesh untouched = *cache.table.find(0, 0);

	// nothing changed: nothing baked
	job.start(m, readerOver(p.files), p.stamper(), {}, cache);
	run(job, cm);
	CHECK(job.stats().reused == 3);
	CHECK(job.stats().baked + job.stats().empty + job.stats().failed == 0);

	// one cell edited (a second quad, in a colour the palette does not have yet): that one cell is baked again, in the same palette
	p.put("H.cells/cell_1_0.hescene", cellOf({ { quad, { 120, 0, 30 }, false, green }, { quad, { 140, 0, 30 }, false, blue } }));
	job.start(m, readerOver(p.files), p.stamper(), {}, cache);
	run(job, cm);
	CHECK(job.stats().reused == 2);
	CHECK(job.stats().baked == 1);
	CHECK(job.stats().baked + job.stats().empty + job.stats().failed == 1);   // the rebuild count of the edit
	REQUIRE(cache.table.find(1, 0));
	CHECK(cache.table.find(1, 0)->indices.size() == 12);
	REQUIRE(cache.table.find(0, 0));
	CHECK(cache.table.find(0, 0)->vertices.size() == untouched.vertices.size());   // the other cell's proxy is the very one it had
	CHECK(cache.table.find(0, 0)->vertices[0].uv == untouched.vertices[0].uv);
	CHECK(cache.table.palette.size() == 9);
	CHECK(std::equal(paletteBefore.begin(), paletteBefore.end(), cache.table.palette.begin()));   // append only: the old uvs still mean what they did

	// a cell dropped from the manifest goes out of the cache; other budgets bake every cell again
	job.start(manifestOf({ { 0, 0 }, { 1, 0 } }), readerOver(p.files), p.stamper(), {}, cache);
	run(job, cm);
	CHECK(job.stats().reused == 2);
	CHECK(cache.stamps.size() == 2);
	HE::CellHlodBakeOptions fewer;
	fewer.maxTrianglesPerCell = 2;
	job.start(manifestOf({ { 0, 0 }, { 1, 0 } }), readerOver(p.files), p.stamper(), fewer, cache);
	run(job, cm);
	CHECK(job.stats().reused == 0);
	CHECK(job.stats().baked == 2);
	CHECK(cache.table.find(1, 0)->indices.size() == 6);

	// the cache survives its bytes (an editor restart)
	HE::CellHlodCache back;
	REQUIRE(HE::CellHlodCache::decode(cache.encode(), back));
	CHECK(back.optionsKey == cache.optionsKey);
	CHECK(back.stamps == cache.stamps);
	CHECK(back.table.cellCount() == cache.table.cellCount());
	CHECK(back.table.palette == cache.table.palette);
	job.start(manifestOf({ { 0, 0 }, { 1, 0 } }), readerOver(p.files), p.stamper(), fewer, back);
	run(job, cm);
	CHECK(job.stats().reused == 2);
}

TEST_CASE("CellHlodBakeJob: a missing or unreadable cell is counted once and not tried again until its file changes; cancel keeps what was done")
{
	ContentManager cm{ "." };
	const HE::UUID quad = registerQuad(cm, 10.0f);
	Project p;
	p.put("H.cells/cell_0_0.hescene", cellOf({ { quad, { 20, 0, 30 } } }));
	p.put("H.cells/cell_1_0.hescene", { 1, 2, 3 });   // not a scene
	const HE::CellManifest m = manifestOf({ { 0, 0 }, { 1, 0 }, { 2, 0 } });   // (2,0) has no file
	HE::CellHlodCache cache;
	HE::CellHlodBakeJob job;
	job.start(m, readerOver(p.files), p.stamper(), {}, cache);
	run(job, cm);
	CHECK(job.stats().baked == 1);
	CHECK(job.stats().failed == 2);
	job.start(m, readerOver(p.files), p.stamper(), {}, cache);
	run(job, cm);
	CHECK(job.stats().reused == 3);

	// a bake cancelled before it pumped does nothing to the cache
	HE::CellHlodCache other;
	job.start(m, readerOver(p.files), p.stamper(), {}, other);
	job.cancel();
	CHECK_FALSE(job.active());
	CHECK(other.table.cellCount() == 0);
	CHECK(other.stamps.empty());
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
