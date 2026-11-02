#include "doctest.h"
#include "TestFsUtil.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <Hpak/HpakWriter.h>
#include <Hpak/MeshBakePass.h>
#include <set>
#include <Hpak/SimpleTextureMaterial.h>

// The export pass end to end on real files: textures, materials and a two-material mesh are
// saved the way the editor saves them, the pass bakes them, and what it hands back loads in
// a fresh content manager as a one-section mesh on a graph material over atlas textures.

namespace fs = std::filesystem;

namespace
{

std::vector<uint8_t> bytesOf(const fs::path& p)
{
	std::ifstream f(p, std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void writeBytes(const fs::path& p, const std::vector<uint8_t>& b)
{
	std::error_code ec;
	fs::create_directories(p.parent_path(), ec);
	std::ofstream f(p, std::ios::binary);
	f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

struct Project
{
	fs::path dir;
	explicit Project(const char* name) : dir(fs::temp_directory_path() / name)
	{
		he_test::removeAllQuiet(dir);
		fs::create_directories(dir);
	}
	~Project() { he_test::removeAllQuiet(dir); }

	std::vector<HE::MeshBake::PassEntry> entries;
	std::vector<std::vector<uint8_t>>    storage;    // keeps the bytes the entries point at

	void add(const RuntimeAsset& a, uint16_t type)
	{
		storage.push_back(bytesOf(dir / a.path));
		HE::MeshBake::PassEntry e;
		e.id = a.id; e.relPath = a.path; e.type = type;
		entries.push_back(e);
	}
	void link()   // pointers last, once the vector stopped growing
	{
		for (size_t i = 0; i < entries.size(); ++i) entries[i].bytes = &storage[i];
	}
};

// Builds: Tex/A, Tex/B (8x8 RGBA8), Mats/A, Mats/B (uber shader), Meshes/Two (two quads, two slots).
void buildProject(Project& pr, bool secondMaterialTranslucent = false, uint8_t meshFlagsOff = 0)
{
	ContentManager cm(pr.dir.string());
	const char* names[2] = { "A", "B" };
	std::vector<TextureAsset> textures(2);
	std::vector<MaterialAsset> materials(2);
	for (int i = 0; i < 2; ++i)
	{
		TextureAsset& t = textures[static_cast<size_t>(i)];
		t.type = HE::AssetType::Texture; t.name = names[i]; t.path = std::string("Tex/") + names[i] + ".hasset";
		t.width = t.height = 8; t.channels = 4; t.srgb = false;
		t.data.resize(8 * 8 * 4);
		for (int p = 0; p < 64; ++p)
		{
			t.data[static_cast<size_t>(p) * 4 + 0] = static_cast<uint8_t>(30 * i + 3 * (p % 8));
			t.data[static_cast<size_t>(p) * 4 + 1] = static_cast<uint8_t>(60 + 3 * (p / 8));
			t.data[static_cast<size_t>(p) * 4 + 2] = static_cast<uint8_t>(90 * i);
			t.data[static_cast<size_t>(p) * 4 + 3] = 255;
		}
		REQUIRE(cm.saveAsset(t));
		MaterialAsset& m = materials[static_cast<size_t>(i)];
		m.type = HE::AssetType::Material; m.name = names[i]; m.path = std::string("Mats/") + names[i] + ".hasset";
		m.texturePaths = { t.path };
		if (i == 1 && secondMaterialTranslucent) m.opacity = 0.5f;
		REQUIRE(cm.saveAsset(m));
	}
	StaticMeshAsset mesh;
	mesh.type = HE::AssetType::StaticMesh; mesh.name = "Two"; mesh.path = "Meshes/Two.hasset";
	mesh.vertices = { 0,0,0,  1,0,0,  1,1,0,  0,1,0,   2,0,0,  3,0,0,  3,1,0,  2,1,0 };
	mesh.normals  = { 0,0,1,  0,0,1,  0,0,1,  0,0,1,   0,0,1,  0,0,1,  0,0,1,  0,0,1 };
	mesh.uvs      = { 0,0,  1,0,  1,1,  0,1,   0,0,  1,0,  1,1,  0,1 };
	mesh.indices  = { 0,1,2, 0,2,3,   4,5,6, 4,6,7 };
	mesh.materialPath = materials[0].path;
	mesh.sections = { { 0, 6, materials[0].path, {} }, { 6, 6, materials[1].path, {} } };
	if (meshFlagsOff & 1) mesh.bake.mergeTextures = false;
	if (meshFlagsOff & 2) mesh.bake.uvTexture = false;
	REQUIRE(cm.saveAsset(mesh));

	pr.add(textures[0], static_cast<uint16_t>(HE::AssetType::Texture));
	pr.add(textures[1], static_cast<uint16_t>(HE::AssetType::Texture));
	pr.add(materials[0], static_cast<uint16_t>(HE::AssetType::Material));
	pr.add(materials[1], static_cast<uint16_t>(HE::AssetType::Material));
	pr.add(mesh, static_cast<uint16_t>(HE::AssetType::StaticMesh));
	pr.link();
}

} // namespace

TEST_CASE("Mesh bake pass: a two-material mesh comes back as one section on a baked material")
{
	Project pr("he_test_meshbake_pass");
	buildProject(pr);
	const HE::MeshBake::PassResult r = HE::MeshBake::runPass(pr.entries, {});
	REQUIRE(r.baked == 1);
	CHECK(r.declined == 0);
	REQUIRE(r.replaced.size() == 1);
	REQUIRE(r.added.size() == 2);            // the baked material and its one atlas (base only)
	CHECK_FALSE(r.notices.empty());

	// Load the result in a fresh project, the way the game's pack reader would see the assets.
	Project out("he_test_meshbake_out");
	writeBytes(out.dir / "Meshes/Two.hasset", r.replaced[0].bytes);
	std::string matPath, texPath;
	for (const HE::MeshBake::NewAsset& a : r.added)
	{
		writeBytes(out.dir / a.relPath, a.bytes);
		(a.type == static_cast<uint16_t>(HE::AssetType::Material) ? matPath : texPath) = a.relPath;
	}
	REQUIRE_FALSE(matPath.empty());
	REQUIRE_FALSE(texPath.empty());
	CHECK(matPath.rfind("Baked/", 0) == 0);

	ContentManager cm(out.dir.string());
	const StaticMeshAsset* mesh = cm.getStaticMesh(cm.loadAsset("Meshes/Two.hasset"));
	REQUIRE(mesh != nullptr);
	REQUIRE(mesh->sections.size() == 1);
	CHECK(mesh->sections[0].materialPath == matPath);
	CHECK(mesh->sections[0].indexCount == 12);
	CHECK(mesh->indices.size() == 12);
	CHECK(mesh->vertices.size() / 3 == 8);
	CHECK(mesh->id == pr.entries[4].id);                    // the mesh keeps its identity
	for (float uv : mesh->uvs) { CHECK(uv >= 0.0f); CHECK(uv <= 1.0f); }

	const MaterialAsset* mat = cm.getMaterial(cm.loadAsset(matPath));
	REQUIRE(mat != nullptr);
	CHECK_FALSE(mat->nodeGraphJson.empty());
	CHECK_FALSE(mat->customShaderFragGlsl.empty());
	CHECK_FALSE(mat->customShaderGBufGlsl.empty());
	REQUIRE(mat->graphTexturePaths.size() == 1);
	CHECK(mat->graphTexturePaths[0] == texPath);
	HE::SimpleTextureMaterial simple;
	std::string why;
	CHECK_MESSAGE(HE::classifySimpleTextureMaterial(*mat, simple, &why), why);   // still a plain PBR set

	const TextureAsset* tex = cm.getTexture(cm.loadAsset(texPath));
	REQUIRE(tex != nullptr);
	CHECK(tex->width >= 64);
	CHECK(tex->width == tex->height);
	CHECK(tex->channels == 4);
	CHECK(tex->data.size() == size_t(tex->width) * tex->height * 4);
}

TEST_CASE("Mesh bake pass: the same project bakes to the same bytes (the incremental cache needs it)")
{
	Project a("he_test_meshbake_det");
	buildProject(a);
	const HE::MeshBake::PassResult ra = HE::MeshBake::runPass(a.entries, {});
	const HE::MeshBake::PassResult rb = HE::MeshBake::runPass(a.entries, {});
	REQUIRE(ra.added.size() == rb.added.size());
	REQUIRE(ra.replaced.size() == rb.replaced.size());
	for (size_t i = 0; i < ra.added.size(); ++i)
	{
		CHECK(ra.added[i].id == rb.added[i].id);
		CHECK(ra.added[i].relPath == rb.added[i].relPath);
		CHECK(ra.added[i].bytes == rb.added[i].bytes);
	}
	for (size_t i = 0; i < ra.replaced.size(); ++i)
		CHECK(ra.replaced[i].bytes == rb.replaced[i].bytes);
}

TEST_CASE("Mesh bake in the pack: the sources drop out of the pak, the baked assets are in it")
{
	Project pr("he_test_meshbake_pack");
	buildProject(pr);
	const HE::UUID meshId = pr.entries[4].id;
	const std::string root = "{\"entities\":[{\"mesh\":[" + std::to_string(meshId.hi) + ", "
	                       + std::to_string(meshId.lo) + "]}]}";

	const auto packed = [&](bool bake) {
		Hpak::PackSettings s;
		s.codec = Hpak::Codec::Store;
		s.onlyReferenced = true;
		s.bakeMeshTextures = bake;
		s.referenceRoots.push_back(std::vector<uint8_t>(root.begin(), root.end()));
		HpakWriter w;
		w.addDirectory(pr.dir, s);
		std::set<std::string> names;
		for (const auto& [path, id] : w.packedPaths()) names.insert(path);
		return names;
	};

	const std::set<std::string> plain = packed(false);
	CHECK(plain.count("Meshes/Two.hasset"));
	CHECK(plain.count("Mats/A.hasset")); CHECK(plain.count("Mats/B.hasset"));
	CHECK(plain.count("Tex/A.hasset"));  CHECK(plain.count("Tex/B.hasset"));
	CHECK(plain.size() == 5);

	const std::set<std::string> baked = packed(true);
	CHECK(baked.count("Meshes/Two.hasset"));
	CHECK_FALSE(baked.count("Mats/A.hasset")); CHECK_FALSE(baked.count("Mats/B.hasset"));
	CHECK_FALSE(baked.count("Tex/A.hasset"));  CHECK_FALSE(baked.count("Tex/B.hasset"));
	size_t derived = 0;
	for (const std::string& n : baked) if (n.rfind("Baked/", 0) == 0) ++derived;
	CHECK(derived == 2);                       // the merged material and its one atlas
	CHECK(baked.size() == 3);                  // mesh + 2: three assets instead of five
}

TEST_CASE("Mesh bake pass: switched off, or with a section that cannot be baked, the mesh is left alone")
{
	{   // both bake forms off
		Project pr("he_test_meshbake_off");
		buildProject(pr, false, 3);
		const HE::MeshBake::PassResult r = HE::MeshBake::runPass(pr.entries, {});
		CHECK(r.baked == 0);
		CHECK(r.replaced.empty());
		CHECK(r.added.empty());
	}
	{   // the second material is translucent: fewer than two simple sections
		Project pr("he_test_meshbake_glass");
		buildProject(pr, true);
		const HE::MeshBake::PassResult r = HE::MeshBake::runPass(pr.entries, {});
		CHECK(r.baked == 0);
		CHECK(r.replaced.empty());
		CHECK(r.added.empty());
		CHECK(r.declined == 1);
		REQUIRE_FALSE(r.notices.empty());
		CHECK(r.notices[0].find("fewer than two simple sections") != std::string::npos);
	}
}
