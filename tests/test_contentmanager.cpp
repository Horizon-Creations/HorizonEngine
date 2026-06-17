#include "doctest.h"
#include <ContentManager/ContentManager.h>
#include <filesystem>

namespace fs = std::filesystem;

namespace
{
	struct TempContentDir
	{
		fs::path path;
		TempContentDir()
		{
			path = fs::temp_directory_path() / "he_test_content";
			fs::remove_all(path);
			fs::create_directories(path);
		}
		~TempContentDir() { fs::remove_all(path); }
	};
}

TEST_CASE("ContentManager static mesh save/load round-trip preserves UUID")
{
	TempContentDir dir;

	HE::UUID savedId;
	{
		ContentManager cm(dir.path.string());
		StaticMeshAsset mesh;
		mesh.type         = HE::AssetType::StaticMesh;
		mesh.name         = "tri";
		mesh.path         = "tri.hasset";
		mesh.vertices     = { 0,0,0,  1,0,0,  0,1,0 };
		mesh.indices      = { 0, 1, 2 };
		mesh.normals      = { 0,0,1,  0,0,1,  0,0,1 };
		mesh.uvs          = { 0,0,  1,0,  0,1 };
		mesh.materialPath = "mat.hasset";
		REQUIRE(cm.saveAsset(mesh));
		savedId = mesh.id;
		REQUIRE_FALSE(savedId == HE::UUID{});
	}

	// Fresh manager — simulates an engine restart
	{
		ContentManager cm(dir.path.string());
		HE::UUID loadedId = cm.loadAsset("tri.hasset");
		CHECK(loadedId == savedId);

		const StaticMeshAsset* mesh = cm.getStaticMesh(loadedId);
		REQUIRE(mesh != nullptr);
		CHECK(mesh->name == "tri");
		CHECK(mesh->vertices.size() == 9);
		CHECK(mesh->indices.size() == 3);
		CHECK(mesh->normals.size() == 9);
		CHECK(mesh->uvs.size() == 6);
		CHECK(mesh->materialPath == "mat.hasset");

		// Wrong-type lookup must not alias
		CHECK(cm.getTexture(loadedId) == nullptr);
	}
}

TEST_CASE("ContentManager texture round-trip")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());

	TextureAsset tex;
	tex.type     = HE::AssetType::Texture;
	tex.name     = "checker";
	tex.path     = "checker.hasset";
	tex.width    = 2;
	tex.height   = 2;
	tex.channels = 4;
	tex.data     = { 255,0,0,255,  0,255,0,255,  0,0,255,255,  255,255,255,255 };
	REQUIRE(cm.saveAsset(tex));

	ContentManager cm2(dir.path.string());
	HE::UUID id = cm2.loadAsset("checker.hasset");
	const TextureAsset* loaded = cm2.getTexture(id);
	REQUIRE(loaded != nullptr);
	CHECK(loaded->width == 2);
	CHECK(loaded->height == 2);
	CHECK(loaded->channels == 4);
	CHECK(loaded->data == tex.data);
}

TEST_CASE("ContentManager unload removes asset")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());

	MaterialAsset mat;
	mat.type       = HE::AssetType::Material;
	mat.name       = "m";
	mat.path       = "m.hasset";
	mat.shaderPath = "builtin/unlit";
	mat.texturePaths = { "a.hasset", "b.hasset" };
	REQUIRE(cm.saveAsset(mat));

	HE::UUID id = cm.loadAsset("m.hasset");
	REQUIRE(cm.isLoaded(id));
	const MaterialAsset* loaded = cm.getMaterial(id);
	REQUIRE(loaded != nullptr);
	CHECK(loaded->shaderPath == "builtin/unlit");
	REQUIRE(loaded->texturePaths.size() == 2);
	CHECK(loaded->texturePaths[1] == "b.hasset");

	CHECK(cm.unloadAsset(id));
	CHECK_FALSE(cm.isLoaded(id));
	CHECK(cm.getMaterial(id) == nullptr);
}

TEST_CASE("ContentManager registers a runtime mesh without a disk file")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());

	StaticMeshAsset mesh;
	mesh.name     = "procedural";
	mesh.vertices = { 0,0,0,  1,0,0,  0,1,0 };
	mesh.indices  = { 0, 1, 2 };

	HE::UUID id = cm.registerStaticMesh(std::move(mesh));
	REQUIRE_FALSE(id == HE::UUID{});          // a UUID was minted
	REQUIRE(cm.isLoaded(id));

	const StaticMeshAsset* got = cm.getStaticMesh(id);
	REQUIRE(got != nullptr);
	CHECK(got->name == "procedural");
	CHECK(got->vertices.size() == 9);
	CHECK(got->type == HE::AssetType::StaticMesh);
	CHECK(got->id == id);

	// No file was written, and wrong-type lookups still don't alias.
	CHECK_FALSE(fs::exists(dir.path / "procedural.hasset"));
	CHECK(cm.getMaterial(id) == nullptr);
}

TEST_CASE("ContentManager replaceStaticMesh keeps the UUID, swaps the payload")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());

	StaticMeshAsset a;
	a.name    = "terrain";
	a.path    = "mem://terrain";
	a.indices = { 0, 1, 2 };
	HE::UUID id = cm.registerStaticMesh(std::move(a));

	// Regenerate with denser geometry — same identity.
	StaticMeshAsset b;
	b.indices = { 0,1,2,  2,3,0 };
	REQUIRE(cm.replaceStaticMesh(id, std::move(b)));

	const StaticMeshAsset* got = cm.getStaticMesh(id);
	REQUIRE(got != nullptr);
	CHECK(got->indices.size() == 6);   // new payload
	CHECK(got->id == id);              // identity preserved
	CHECK(got->name == "terrain");     // name preserved
	CHECK(got->path == "mem://terrain");

	// Replacing a UUID of the wrong type fails and changes nothing.
	CHECK_FALSE(cm.replaceMaterial(id, MaterialAsset{}));
	CHECK_FALSE(cm.replaceStaticMesh(HE::UUID::generate(), StaticMeshAsset{}));
	CHECK(cm.getStaticMesh(id)->indices.size() == 6);
}

TEST_CASE("ContentManager registers a runtime material and mesh distinctly")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());

	MaterialAsset mat;
	mat.name      = "rtMat";
	mat.baseColor[0] = 0.25f;
	HE::UUID matId = cm.registerMaterial(std::move(mat));

	StaticMeshAsset mesh;
	HE::UUID meshId = cm.registerStaticMesh(std::move(mesh));

	CHECK_FALSE(matId == meshId);
	const MaterialAsset* m = cm.getMaterial(matId);
	REQUIRE(m != nullptr);
	CHECK(m->baseColor[0] == doctest::Approx(0.25f));
	CHECK(cm.getStaticMesh(matId) == nullptr); // material id ≠ mesh
	CHECK(cm.getMaterial(meshId)  == nullptr); // mesh id ≠ material
}

TEST_CASE("ContentManager loading same path twice returns same UUID")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());

	ScriptAsset s;
	s.type       = HE::AssetType::Script;
	s.name       = "boot";
	s.path       = "boot.hasset";
	s.sourceCode = "print('hi')";
	REQUIRE(cm.saveAsset(s));

	HE::UUID a = cm.loadAsset("boot.hasset");
	HE::UUID b = cm.loadAsset("boot.hasset");
	CHECK(a == b);
}
