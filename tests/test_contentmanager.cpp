#include "doctest.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/DefaultAssets.h>
#include <chrono>
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

// ── Default built-in assets ───────────────────────────────────────────────────

TEST_CASE("ContentManager pre-registers the default cube mesh")
{
	ContentManager cm;
	const StaticMeshAsset* cube = cm.getStaticMesh(HE::kDefaultCubeMeshId);
	REQUIRE(cube != nullptr);
	CHECK(cube->id == HE::kDefaultCubeMeshId);
	CHECK(cube->vertices.size() == 24 * 3); // 24 verts × 3 floats (pos)
	CHECK(cube->normals .size() == 24 * 3);
	CHECK(cube->indices .size() == 36);
}

TEST_CASE("ContentManager pre-registers the default white texture")
{
	ContentManager cm;
	const TextureAsset* tex = cm.getTexture(HE::kDefaultWhiteTextureId);
	REQUIRE(tex != nullptr);
	CHECK(tex->id      == HE::kDefaultWhiteTextureId);
	CHECK(tex->width   == 1);
	CHECK(tex->height  == 1);
	CHECK(tex->channels == 4);
	REQUIRE(tex->data.size() == 4);
	CHECK(tex->data[0] == 255);
	CHECK(tex->data[1] == 255);
	CHECK(tex->data[2] == 255);
	CHECK(tex->data[3] == 255);
}

TEST_CASE("ContentManager pre-registers the default material")
{
	ContentManager cm;
	const MaterialAsset* mat = cm.getMaterial(HE::kDefaultMaterialId);
	REQUIRE(mat != nullptr);
	CHECK(mat->id        == HE::kDefaultMaterialId);
	CHECK(mat->baseColor[0] == doctest::Approx(1.0f));
	CHECK(mat->metallic     == doctest::Approx(0.0f));
	CHECK(mat->roughness    == doctest::Approx(0.5f));
	CHECK(mat->opacity      == doctest::Approx(1.0f));
}

TEST_CASE("ContentManager default asset UUIDs are fixed and distinct")
{
	// Sentinel UUIDs must not collide with UUID::generate() output (version-4
	// requires hi & 0xF000 == 0x4000; all sentinels have hi < 0x10).
	CHECK_FALSE(HE::kDefaultCubeMeshId     == HE::UUID{});
	CHECK_FALSE(HE::kDefaultWhiteTextureId == HE::UUID{});
	CHECK_FALSE(HE::kDefaultMaterialId     == HE::UUID{});
	CHECK_FALSE(HE::kDefaultCubeMeshId     == HE::kDefaultWhiteTextureId);
	CHECK_FALSE(HE::kDefaultCubeMeshId     == HE::kDefaultMaterialId);
	CHECK_FALSE(HE::kDefaultWhiteTextureId == HE::kDefaultMaterialId);
	// Verify they cannot be produced by UUID::generate()
	CHECK((HE::kDefaultCubeMeshId.hi & 0x000000000000F000ULL) != 0x0000000000004000ULL);
}

// ── Asset enumeration ─────────────────────────────────────────────────────────

TEST_CASE("ContentManager enumerateIds returns all registered assets")
{
	ContentManager cm; // starts with 3 default assets

	const size_t defaultCount = cm.assetCount();
	REQUIRE(defaultCount == 3);

	StaticMeshAsset m; m.name = "extra";
	HE::UUID extraId = cm.registerStaticMesh(std::move(m));

	auto all = cm.enumerateIds();
	CHECK(all.size() == defaultCount + 1);

	bool found = false;
	for (auto id : all) if (id == extraId) { found = true; break; }
	CHECK(found);
}

TEST_CASE("ContentManager enumerateIds(type) filters by asset type")
{
	ContentManager cm; // 1 cube mesh, 1 white texture, 1 material

	auto meshes   = cm.enumerateIds(HE::AssetType::StaticMesh);
	auto textures = cm.enumerateIds(HE::AssetType::Texture);
	auto materials= cm.enumerateIds(HE::AssetType::Material);
	auto scripts  = cm.enumerateIds(HE::AssetType::Script);

	CHECK(meshes.size()    == 1);
	CHECK(textures.size()  == 1);
	CHECK(materials.size() == 1);
	CHECK(scripts.size()   == 0);

	CHECK(meshes[0]    == HE::kDefaultCubeMeshId);
	CHECK(textures[0]  == HE::kDefaultWhiteTextureId);
	CHECK(materials[0] == HE::kDefaultMaterialId);

	// Add another mesh — only meshes count increases.
	StaticMeshAsset m2; m2.name = "m2";
	cm.registerStaticMesh(std::move(m2));
	CHECK(cm.enumerateIds(HE::AssetType::StaticMesh).size() == 2);
	CHECK(cm.enumerateIds(HE::AssetType::Texture).size()    == 1);
}

TEST_CASE("ContentManager enumerateIds unload removes entry")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());

	StaticMeshAsset m; m.name = "temp"; m.path = "mem://temp";
	HE::UUID id = cm.registerStaticMesh(std::move(m));

	auto before = cm.enumerateIds();
	REQUIRE(before.size() == 4); // 3 defaults + 1

	REQUIRE(cm.unloadAsset(id));

	auto after = cm.enumerateIds();
	CHECK(after.size() == 3);
	for (auto uid : after) CHECK_FALSE(uid == id);

	// Type-filtered enumeration also must not contain it
	auto meshes = cm.enumerateIds(HE::AssetType::StaticMesh);
	CHECK(meshes.size() == 1); // only default cube
	for (auto uid : meshes) CHECK_FALSE(uid == id);
}

TEST_CASE("ContentManager default assets are addressable by virtual path")
{
	ContentManager cm;
	CHECK(cm.isLoaded("mem://default_cube"));
	CHECK(cm.isLoaded("mem://default_white"));
	CHECK(cm.isLoaded("mem://default_material"));
}

// ── Hot-reload ────────────────────────────────────────────────────────────────

TEST_CASE("ContentManager pollHotReload detects a changed file and reloads it")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());

	// Save V1 — metallic = 0.1
	MaterialAsset mat1;
	mat1.type     = HE::AssetType::Material;
	mat1.name     = "hotmat";
	mat1.path     = "hotmat.hasset";
	mat1.metallic = 0.1f;
	REQUIRE(cm.saveAsset(mat1));
	const HE::UUID savedId = mat1.id;

	HE::UUID loaded = cm.loadAsset("hotmat.hasset");
	REQUIRE(loaded == savedId);
	REQUIRE(cm.getMaterial(loaded) != nullptr);
	CHECK(cm.getMaterial(loaded)->metallic == doctest::Approx(0.1f));

	// No changes yet — poll is quiet.
	CHECK(cm.pollHotReload().empty());

	// Overwrite V2 — same path, same UUID, different content.
	MaterialAsset mat2 = *cm.getMaterial(loaded); // copies identity
	mat2.metallic = 0.9f;
	REQUIRE(cm.saveAsset(mat2));

	// Advance the stored mtime by 2 s so the poller detects it on same-second saves.
	const fs::path diskPath = dir.path / "hotmat.hasset";
	auto cur = fs::last_write_time(diskPath);
	fs::last_write_time(diskPath, cur + std::chrono::seconds(2));

	// Poll — one asset changed, UUID is preserved (persisted in the v2 file).
	auto changed = cm.pollHotReload();
	REQUIRE(changed.size() == 1);
	CHECK(changed[0] == savedId);

	// Reloaded payload reflects V2.
	const MaterialAsset* reloaded = cm.getMaterial(savedId);
	REQUIRE(reloaded != nullptr);
	CHECK(reloaded->metallic == doctest::Approx(0.9f));

	// Second poll with no further changes is quiet.
	CHECK(cm.pollHotReload().empty());
}

TEST_CASE("ContentManager pollHotReload ignores virtual mem:// paths")
{
	// Default manager has only mem:// assets — poll must not crash or signal any.
	ContentManager cm;
	CHECK(cm.pollHotReload().empty());
}
