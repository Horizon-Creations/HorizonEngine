#include "doctest.h"
#include "TestFsUtil.h"
#include <ContentManager/AssetRefRetarget.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace
{
	struct TempContentDir
	{
		fs::path path;
		explicit TempContentDir(const char* name = "he_test_refretarget")
		{
			path = fs::temp_directory_path() / name / "Content";
			he_test::removeAllQuiet(path.parent_path());
			fs::create_directories(path);
		}
		~TempContentDir() { he_test::removeAllQuiet(path.parent_path()); }
	};

	// How often a byte sequence appears in a file — asserts on what actually
	// landed on disk, without going back through the loader (which regenerates a
	// material's graph-derived fields and would hide what the rewrite did).
	size_t countOccurrences(const fs::path& file, const std::string& needle)
	{
		std::ifstream f(file, std::ios::binary);
		const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		size_t n = 0;
		for (size_t p = bytes.find(needle); p != std::string::npos; p = bytes.find(needle, p + 1)) ++n;
		return n;
	}

	// A material that references the texture in every way one can: the flat
	// texture list, the node-graph texture slots, and the graph JSON itself
	// (which is stored as a string INSIDE the MTRL chunk — the awkward case).
	MaterialAsset makeMaterial(const std::string& path, const std::string& texRef)
	{
		MaterialAsset m;
		m.type              = HE::AssetType::Material;
		m.name              = fs::path(path).stem().string();
		m.path              = path;
		m.shaderPath        = "Shaders/Lit.hasset";
		m.texturePaths      = { texRef };
		m.graphTexturePaths = { texRef };
		m.nodeGraphJson     = R"({"nodes":[{"id":1,"type":"TextureSample","s":")" + texRef +
		                      R"("},{"id":2,"type":"Output"}],"links":[]})";
		m.customShaderFragGlsl = "void main() { }";
		m.shaderParamData      = { 1.0f, 0.5f, 0.25f, 0.0f };
		m.graphParamNames      = { "Tint" };
		return m;
	}
}

TEST_CASE("retargetValue rewrites whole values only")
{
	const std::vector<HE::AssetRefs::Rule> file =
		HE::AssetRefs::moveRules("Tex/Rock.hasset", "Tex/New/Rock.hasset", false, "Content");

	std::string v = "Tex/Rock.hasset";
	CHECK(HE::AssetRefs::retargetValue(v, file));
	CHECK(v == "Tex/New/Rock.hasset");

	// Project-relative form (what scene references store) is covered too.
	v = "Content/Tex/Rock.hasset";
	CHECK(HE::AssetRefs::retargetValue(v, file));
	CHECK(v == "Content/Tex/New/Rock.hasset");

	// A longer path that merely STARTS with the moved one is not a reference to it.
	v = "Tex/Rock.hasset.bak";
	CHECK_FALSE(HE::AssetRefs::retargetValue(v, file));
	v = "Other/Tex/Rock.hasset";
	CHECK_FALSE(HE::AssetRefs::retargetValue(v, file));

	// A folder move re-roots everything below it, and only below it.
	const std::vector<HE::AssetRefs::Rule> folder =
		HE::AssetRefs::moveRules("Tex", "Textures", true, "Content");
	v = "Tex/Rock.hasset";
	CHECK(HE::AssetRefs::retargetValue(v, folder));
	CHECK(v == "Textures/Rock.hasset");
	v = "TexOther/Rock.hasset";
	CHECK_FALSE(HE::AssetRefs::retargetValue(v, folder));
}

TEST_CASE("Moving an asset carries every path reference to it")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());

	fs::create_directories(dir.path / "Tex");
	fs::create_directories(dir.path / "Tex" / "Sub");

	TextureAsset tex;
	tex.type = HE::AssetType::Texture;
	tex.name = "Rock"; tex.path = "Tex/Rock.hasset";
	tex.width = 1; tex.height = 1; tex.channels = 4;
	tex.data = { 255, 255, 255, 255 };
	REQUIRE(cm.saveAsset(tex));

	MaterialAsset mat = makeMaterial("Mat.hasset", "Tex/Rock.hasset");
	REQUIRE(cm.saveAsset(mat));

	StaticMeshAsset mesh;
	mesh.type = HE::AssetType::StaticMesh;
	mesh.name = "Cube"; mesh.path = "Cube.hasset";
	mesh.vertices = { 0,0,0, 1,0,0, 0,1,0 };
	mesh.indices  = { 0, 1, 2 };
	mesh.materialPath = "Mat.hasset";
	REQUIRE(cm.saveAsset(mesh));

	// A graph that names the texture as a plain JSON string value.
	HorizonCodeClassAsset hc;
	hc.type = HE::AssetType::HorizonCodeClass;
	hc.name = "Logic"; hc.path = "Logic.hasset";
	hc.graphJson = R"({"nextId":2,"nodes":[{"id":1,"s":"Tex/Rock.hasset"}],"links":[],"variables":[]})";
	REQUIRE(cm.saveAsset(hc));

	// An unrelated asset whose text merely CONTAINS the moved name.
	HorizonCodeClassAsset other;
	other.type = HE::AssetType::HorizonCodeClass;
	other.name = "Other"; other.path = "Other.hasset";
	other.graphJson = R"({"nextId":2,"nodes":[{"id":1,"s":"Tex/Rock.hasset.bak"}],"links":[],"variables":[]})";
	REQUIRE(cm.saveAsset(other));

	// The move itself (the Content Browser renames the file, then calls this).
	fs::rename(dir.path / "Tex" / "Rock.hasset", dir.path / "Tex" / "Sub" / "Rock.hasset");
	const size_t rewritten =
		cm.retargetAssetReferences("Tex/Rock.hasset", "Tex/Sub/Rock.hasset");
	CHECK(rewritten == 3); // material, HorizonCode class, and the texture's own META

	// On disk: all THREE places the material stored the texture were rewritten —
	// the flat texture list, the node-graph slot list, and the graph JSON that
	// sits inside the same chunk as a length-prefixed string.
	CHECK(countOccurrences(dir.path / "Mat.hasset", "Tex/Sub/Rock.hasset") == 3);
	CHECK(countOccurrences(dir.path / "Mat.hasset", "\"Tex/Rock.hasset\"") == 0);

	ContentManager fresh(dir.path.string());
	const MaterialAsset* m = fresh.getMaterial(fresh.loadAsset("Mat.hasset"));
	REQUIRE(m != nullptr);
	CHECK(m->texturePaths == std::vector<std::string>{ "Tex/Sub/Rock.hasset" });
	CHECK(m->nodeGraphJson.find("Tex/Sub/Rock.hasset") != std::string::npos);
	CHECK(m->nodeGraphJson.find("\"Tex/Rock.hasset\"") == std::string::npos);
	// A neighbouring field of the rewritten ones survives the re-serialization.
	CHECK(m->shaderPath == "Shaders/Lit.hasset");

	const HorizonCodeClassAsset* g = fresh.getHorizonCodeClass(fresh.loadAsset("Logic.hasset"));
	REQUIRE(g != nullptr);
	CHECK(g->graphJson.find("\"Tex/Sub/Rock.hasset\"") != std::string::npos);

	const HorizonCodeClassAsset* o = fresh.getHorizonCodeClass(fresh.loadAsset("Other.hasset"));
	REQUIRE(o != nullptr);
	CHECK(o->graphJson.find("\"Tex/Rock.hasset.bak\"") != std::string::npos);

	// The moved asset's OWN embedded path — what the packer's path→UUID map is
	// built from — moved with it.
	{
		HAsset::Reader r;
		REQUIRE(r.open((dir.path / "Tex" / "Sub" / "Rock.hasset").string()));
		const auto* meta = r.findChunk(HAsset::CHUNK_META);
		REQUIRE(meta != nullptr);
		size_t off = sizeof(uint16_t);
		HE::UUID id; std::string name, path;
		REQUIRE(HAsset::Reader::readPOD(meta->data, off, id.hi));
		REQUIRE(HAsset::Reader::readPOD(meta->data, off, id.lo));
		REQUIRE(HAsset::Reader::readString(meta->data, off, name));
		REQUIRE(HAsset::Reader::readString(meta->data, off, path));
		CHECK(path == "Tex/Sub/Rock.hasset");
		CHECK(id == tex.id);
	}

	// The mesh referenced the material, not the texture — nothing to change.
	const StaticMeshAsset* mm = fresh.getStaticMesh(fresh.loadAsset("Cube.hasset"));
	REQUIRE(mm != nullptr);
	CHECK(mm->materialPath == "Mat.hasset");
	CHECK(mm->vertices.size() == 9);
}

TEST_CASE("Renaming a folder re-roots every reference under it")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());
	fs::create_directories(dir.path / "Tex");

	TextureAsset tex;
	tex.type = HE::AssetType::Texture;
	tex.name = "Rock"; tex.path = "Tex/Rock.hasset";
	tex.width = 1; tex.height = 1; tex.channels = 4;
	tex.data = { 1, 2, 3, 4 };
	REQUIRE(cm.saveAsset(tex));

	MaterialAsset mat = makeMaterial("Mat.hasset", "Tex/Rock.hasset");
	REQUIRE(cm.saveAsset(mat));
	const HE::UUID matId = cm.loadAsset("Mat.hasset");

	fs::rename(dir.path / "Tex", dir.path / "Textures");
	CHECK(cm.retargetAssetReferences("Tex", "Textures", /*folder=*/true) == 2);

	ContentManager fresh(dir.path.string());
	const MaterialAsset* m = fresh.getMaterial(fresh.loadAsset("Mat.hasset"));
	REQUIRE(m != nullptr);
	CHECK(m->texturePaths == std::vector<std::string>{ "Textures/Rock.hasset" });
	CHECK(m->nodeGraphJson.find("Textures/Rock.hasset") != std::string::npos);

	// The already-loaded copy in the manager that performed the move knows its
	// new home, so saving it does not resurrect the old location.
	const MaterialAsset* live = cm.getMaterial(matId);
	REQUIRE(live != nullptr);
	CHECK(live->path == "Mat.hasset");
	CHECK(cm.getTexture(cm.loadAsset("Textures/Rock.hasset")) != nullptr);
}

TEST_CASE("Retargeting leaves an unaffected project alone")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());

	MaterialAsset mat = makeMaterial("Mat.hasset", "Tex/Rock.hasset");
	REQUIRE(cm.saveAsset(mat));
	const auto before = fs::last_write_time(dir.path / "Mat.hasset");

	CHECK(cm.retargetAssetReferences("Tex/Other.hasset", "Moved/Other.hasset") == 0);
	CHECK(fs::last_write_time(dir.path / "Mat.hasset") == before);
}
