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
