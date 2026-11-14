// A tile landscape in an export (Thema 326): the scene is pointed at a shipped copy of its pack, the cell proxies are baked into that copy and
// never into the project's own file, a pack two scenes share ships once, and the game finds a relative pack in its data folder.
#include "doctest.h"
#include <ContentManager/ContentManager.h>
#include <HorizonScene/Components/LandscapeWorldComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LandscapeExport.h>
#include <HorizonScene/LandscapeSource.h>
#include <HorizonScene/SceneSerializer.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;

fs::path tempDir(const char* tag)
{
	const fs::path p = fs::temp_directory_path() / (std::string("he_lsexport_") + tag + "_" + std::to_string(std::random_device{}()));
	fs::create_directories(p);
	return p;
}

void writePack(const fs::path& path)
{
	HE::ProceduralLandscapeParams p;
	p.geometry.hCells = 32;
	p.geometry.wTexels = 16;
	p.geometry.channels = 4;
	p.tilesX = p.tilesZ = 2;
	p.seaLevelM = 1500.0f;
	const HE::ProceduralLandscapeSource src(p);
	HE::LandscapePackWriter w;
	REQUIRE(w.begin(path.string(), src.geometry(), 2, 2, 3));
	w.setWaterTable(*src.water());
	for (int z = 0; z < 2; ++z)
		for (int x = 0; x < 2; ++x) REQUIRE(w.add({ x, z }, HE::encodeLandscapeTile(src.generate({ x, z }))));
	REQUIRE(w.finish());
}

std::vector<uint8_t> bytesOf(const fs::path& f)
{
	std::ifstream in(f, std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// One cell file with one 10 m quad in it, under <root>/H.cells/cell_0_0.hescene.
void writeCell(const fs::path& root, ContentManager& cm, HE::UUID& quadOut)
{
	StaticMeshAsset a;
	a.vertices = { 0, 0, 0, 10, 0, 0, 10, 0, 10, 0, 0, 10 };
	a.normals = { 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0 };
	a.uvs = { 0, 0, 1, 0, 1, 1, 0, 1 };
	a.indices = { 0, 1, 2, 0, 2, 3 };
	quadOut = cm.registerStaticMesh(std::move(a));

	HorizonWorld w;
	const Entity e = w.createEntity("Quad");
	TransformComponent t;
	t.position = { 20, 0, 30 };
	w.addComponent(e, t);
	MeshComponent mc;
	mc.meshAssetId = quadOut;
	w.registry().emplace<MeshComponent>(e, mc);
	SceneSerializer ser;
	std::vector<uint8_t> bytes;
	REQUIRE(ser.saveToMemory(w, bytes));
	fs::create_directories(root / "H.cells");
	std::ofstream out(root / "H.cells" / "cell_0_0.hescene", std::ios::binary);
	out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

const char* kManifest = R"({"version": 2, "cellSize": 100, "loadRadius": 40, "unloadRadius": 50, "lookaheadSec": 0, "dir": "H.cells", "list": [[0,0,1,0]]})";

void addLandscape(HorizonWorld& w, const std::string& pack)
{
	const Entity e = w.createEntity("Landscape");
	LandscapeWorldComponent lw;
	lw.pack = pack;
	w.registry().emplace<LandscapeWorldComponent>(e, lw);
}

std::string packOf(HorizonWorld& w)
{
	for (auto e : w.registry().view<LandscapeWorldComponent>()) return w.registry().get<LandscapeWorldComponent>(e).pack;
	return {};
}
} // namespace

TEST_CASE("prepareLandscapePacksForExport: the scene names the shipped copy, the proxies go into the copy and the project's pack stays as it was")
{
	const fs::path root = tempDir("prep");
	ContentManager cm{ "." };
	HE::UUID quad;
	writeCell(root, cm, quad);
	writePack(root / "world.pack");
	const std::vector<uint8_t> before = bytesOf(root / "world.pack");

	HorizonWorld world;
	addLandscape(world, "world.pack");   // project-relative
	world.setCellManifestJson(kManifest);

	HE::LandscapeExportPlan plan;
	std::vector<std::string> notes;
	const size_t n = HE::prepareLandscapePacksForExport(world, root, cm, {}, plan, [&](int, const std::string& s) { notes.push_back(s); });
	CHECK(n == 1);
	CHECK(packOf(world) == "Landscape/world.pack");
	REQUIRE(plan.packs.size() == 1);
	CHECK(plan.packs[0].dest == "Landscape/world.pack");
	REQUIRE(plan.packs[0].hlod);
	CHECK(plan.packs[0].hlod->cellCount() == 1);
	CHECK(plan.bake.baked == 1);
	CHECK(plan.bake.instances == 1);
	CHECK(!notes.empty());

	// The exporter's half: a copy, finished.
	const fs::path copy = root / "shipped.pack";
	fs::copy_file(root / "world.pack", copy);
	std::string err;
	REQUIRE_MESSAGE(HE::finishExportedPack(plan.packs[0], copy, &err), err);
	{
		auto a = HE::LandscapePackSource::open(copy.string(), &err);
		REQUIRE_MESSAGE(a, err);
		REQUIRE(a->hlod());
		CHECK(a->hlod()->find(0, 0) != nullptr);
		CHECK(a->water() != nullptr);
	}
	CHECK(bytesOf(root / "world.pack") == before);   // the author's file is untouched
	fs::remove_all(root);
}

TEST_CASE("prepareLandscapePacksForExport: a pack two scenes share ships and bakes once, switched off bakes nothing, a missing pack is left alone")
{
	const fs::path root = tempDir("share");
	ContentManager cm{ "." };
	HE::UUID quad;
	writeCell(root, cm, quad);
	writePack(root / "world.pack");

	HE::LandscapeExportPlan plan;
	HorizonWorld a, b;
	addLandscape(a, (root / "world.pack").string());   // absolute
	addLandscape(b, "world.pack");                      // the same file, written the other way
	a.setCellManifestJson(kManifest);
	b.setCellManifestJson(kManifest);
	HE::prepareLandscapePacksForExport(a, root, cm, {}, plan);
	HE::prepareLandscapePacksForExport(b, root, cm, {}, plan);
	CHECK(plan.packs.size() == 1);
	CHECK(plan.bake.cells == 1);   // one bake, not two
	CHECK(packOf(a) == packOf(b));

	HE::LandscapeExportPlan off;
	HorizonWorld c;
	addLandscape(c, "world.pack");
	c.setCellManifestJson(kManifest);
	HE::LandscapeExportOptions opt;
	opt.bakeHlod = false;
	HE::prepareLandscapePacksForExport(c, root, cm, opt, off);
	REQUIRE(off.packs.size() == 1);
	CHECK(!off.packs[0].hlod);
	CHECK(packOf(c) == "Landscape/world.pack");   // still shipped

	HE::LandscapeExportPlan none;
	HorizonWorld d;
	addLandscape(d, "nowhere.pack");
	int warnings = 0;
	CHECK(HE::prepareLandscapePacksForExport(d, root, cm, {}, none, [&](int level, const std::string&) { warnings += level; }) == 0);
	CHECK(none.packs.empty());
	CHECK(packOf(d) == "nowhere.pack");
	CHECK(warnings == 1);

	// Two different files with one name get two destinations.
	fs::create_directories(root / "other");
	writePack(root / "other" / "world.pack");
	HE::LandscapeExportPlan two;
	HorizonWorld e1, e2;
	addLandscape(e1, "world.pack");
	addLandscape(e2, "other/world.pack");
	HE::prepareLandscapePacksForExport(e1, root, cm, opt, two);
	HE::prepareLandscapePacksForExport(e2, root, cm, opt, two);
	REQUIRE(two.packs.size() == 2);
	CHECK(two.packs[0].dest != two.packs[1].dest);
	fs::remove_all(root);
}

TEST_CASE("resolveLandscapePacks: a relative pack that is in the data folder becomes its absolute path, anything else stays")
{
	const fs::path data = tempDir("data");
	fs::create_directories(data / "Landscape");
	writePack(data / "Landscape" / "world.pack");

	HorizonWorld w;
	addLandscape(w, "Landscape/world.pack");
	CHECK(HE::resolveLandscapePacks(w, data) == 1);
	CHECK(fs::path(packOf(w)).is_absolute());
	CHECK(fs::exists(packOf(w)));
	CHECK(HE::resolveLandscapePacks(w, data) == 0);   // already absolute

	HorizonWorld missing;
	addLandscape(missing, "Landscape/gone.pack");
	CHECK(HE::resolveLandscapePacks(missing, data) == 0);
	CHECK(packOf(missing) == "Landscape/gone.pack");
	CHECK(HE::resolveLandscapePacks(w, fs::path{}) == 0);   // no data folder known (the editor)
	fs::remove_all(data);
}
