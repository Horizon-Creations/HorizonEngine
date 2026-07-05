#include "doctest.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/DefaultAssets.h>
#include <MaterialGraph/MaterialGraph.h> // HE::MatParamKind
#include <chrono>
#include <filesystem>
#include <fstream>

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

TEST_CASE("ContentManager round-trips a material's custom shader (and defaults empty)")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());

	// A material WITH a custom shader source survives save → load.
	MaterialAsset mat;
	mat.type       = HE::AssetType::Material;
	mat.name       = "custom";
	mat.path       = "custom.hasset";
	mat.baseColor[0] = 0.2f; mat.baseColor[1] = 0.4f; mat.baseColor[2] = 0.6f;
	mat.opacity    = 0.75f;
	mat.customShaderFragGlsl =
		"#version 450\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(1.0); }\n";
	REQUIRE(cm.saveAsset(mat));

	HE::UUID id = cm.loadAsset("custom.hasset");
	const MaterialAsset* loaded = cm.getMaterial(id);
	REQUIRE(loaded != nullptr);
	CHECK(loaded->customShaderFragGlsl == mat.customShaderFragGlsl);
	CHECK(loaded->opacity == doctest::Approx(0.75f)); // tail field before it still reads

	// A material WITHOUT one loads with the field empty (back-compatible default).
	MaterialAsset plain;
	plain.type = HE::AssetType::Material;
	plain.name = "plain";
	plain.path = "plain.hasset";
	REQUIRE(cm.saveAsset(plain));
	HE::UUID pid = cm.loadAsset("plain.hasset");
	const MaterialAsset* ploaded = cm.getMaterial(pid);
	REQUIRE(ploaded != nullptr);
	CHECK(ploaded->customShaderFragGlsl.empty());
}

TEST_CASE("ContentManager setMaterialParam sets node-graph params by name + round-trips")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());

	MaterialAsset mat;
	mat.type = HE::AssetType::Material;
	mat.name = "params"; mat.path = "params.hasset";
	// Two exposed params: 'K' (scalar, slot 0) and 'Tint' (vec4, slot 1).
	mat.graphParamNames = { "K", "Tint" };
	mat.graphParamTypes = { (uint8_t)HE::MatParamKind::Float, (uint8_t)HE::MatParamKind::Vec4 };
	mat.shaderParamData = { 1.0f, 0, 0, 0,   0.1f, 0.2f, 0.3f, 1.0f };
	REQUIRE(cm.saveAsset(mat));
	const HE::UUID id = cm.loadAsset("params.hasset");
	REQUIRE(cm.getMaterial(id) != nullptr);

	// Param names + widget kinds survive the MTRL-tail round-trip.
	CHECK(cm.getMaterial(id)->graphParamNames == std::vector<std::string>{ "K", "Tint" });
	CHECK(cm.getMaterial(id)->graphParamTypes == std::vector<uint8_t>{
		(uint8_t)HE::MatParamKind::Float, (uint8_t)HE::MatParamKind::Vec4 });

	// Set the scalar param by name → shaderParamData slot 0 updated.
	const float kv[1] = { 0.42f };
	CHECK(cm.setMaterialParam(id, "K", kv, 1));
	CHECK(cm.getMaterial(id)->shaderParamData[0] == doctest::Approx(0.42f));

	// Set the vec4 param → all four components of slot 1 updated.
	const float tint[4] = { 0.9f, 0.8f, 0.7f, 0.6f };
	CHECK(cm.setMaterialParam(id, "Tint", tint, 4));
	float out[4] = { 0, 0, 0, 0 };
	REQUIRE(cm.getMaterialParam(id, "Tint", out));
	CHECK(out[0] == doctest::Approx(0.9f));
	CHECK(out[3] == doctest::Approx(0.6f));

	// Unknown parameter / non-material → false, no crash.
	CHECK_FALSE(cm.setMaterialParam(id, "Nope", kv, 1));
	CHECK_FALSE(cm.getMaterialParam(id, "Nope", out));
	CHECK_FALSE(cm.setMaterialParam(HE::UUID::generate(), "K", kv, 1));
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
	// The cube ships without texture coords; ContentManager must box-project UVs so
	// UV-space material nodes (Noise/FBM/Checker/TextureSample) don't collapse to
	// vUV = (0,0) (which renders solid black). Each per-face UV lands in [0,1].
	REQUIRE(cube->uvs.size() == 24 * 2);
	bool anyNonZero = false, allInRange = true;
	for (size_t i = 0; i < cube->uvs.size(); ++i)
	{
		if (cube->uvs[i] != 0.0f) anyNonZero = true;
		if (cube->uvs[i] < -0.001f || cube->uvs[i] > 1.001f) allInRange = false;
	}
	CHECK(anyNonZero);  // not all (0,0) → the pattern actually varies across a face
	CHECK(allInRange);  // box projection of a unit cube maps into [0,1]
}

TEST_CASE("ContentManager box-projects UVs for a registered mesh that has none")
{
	ContentManager cm;
	StaticMeshAsset m;
	m.type = HE::AssetType::StaticMesh;
	m.name = "NoUVQuad";
	// A single +Z-facing quad, no uvs supplied.
	m.vertices = { -0.5f,-0.5f,0.0f,  0.5f,-0.5f,0.0f,  0.5f,0.5f,0.0f,  -0.5f,0.5f,0.0f };
	m.normals  = {  0,0,1,            0,0,1,            0,0,1,             0,0,1 };
	m.indices  = { 0,1,2, 0,2,3 };
	const HE::UUID id = cm.registerStaticMesh(std::move(m));
	const StaticMeshAsset* got = cm.getStaticMesh(id);
	REQUIRE(got != nullptr);
	REQUIRE(got->uvs.size() == 4 * 2); // one uv per vertex, generated
	// +Z face → uv = (px, py) + 0.5 → the four corners span the full [0,1] square.
	CHECK(got->uvs[0] == doctest::Approx(0.0f)); CHECK(got->uvs[1] == doctest::Approx(0.0f));
	CHECK(got->uvs[4] == doctest::Approx(1.0f)); CHECK(got->uvs[5] == doctest::Approx(1.0f));
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
	ContentManager cm; // 7 default assets (cube, quad, snowflake, white tex, material, grid tex, terrain material)

	const size_t defaultCount = cm.assetCount();
	REQUIRE(defaultCount == 7);

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
	ContentManager cm; // 3 meshes (cube + quad + snowflake), 2 textures (white + grid), 2 materials (default + terrain)

	auto meshes   = cm.enumerateIds(HE::AssetType::StaticMesh);
	auto textures = cm.enumerateIds(HE::AssetType::Texture);
	auto materials= cm.enumerateIds(HE::AssetType::Material);
	auto scripts  = cm.enumerateIds(HE::AssetType::Script);

	CHECK(meshes.size()    == 3);
	CHECK(textures.size()  == 2);
	CHECK(materials.size() == 2);
	CHECK(scripts.size()   == 0);

	// Unordered_map iteration order is not guaranteed — use containment checks.
	auto contains = [](const std::vector<HE::UUID>& v, HE::UUID id) {
		return std::find(v.begin(), v.end(), id) != v.end();
	};
	CHECK(contains(meshes,     HE::kDefaultCubeMeshId));
	CHECK(contains(meshes,     HE::kDefaultQuadMeshId));
	CHECK(contains(textures,   HE::kDefaultWhiteTextureId));
	CHECK(contains(textures,   HE::kDefaultGridTextureId));
	CHECK(contains(materials,  HE::kDefaultMaterialId));
	CHECK(contains(materials,  HE::kDefaultTerrainMaterialId));

	// Add another mesh — only meshes count increases.
	StaticMeshAsset m2; m2.name = "m2";
	cm.registerStaticMesh(std::move(m2));
	CHECK(cm.enumerateIds(HE::AssetType::StaticMesh).size() == 4);
	CHECK(cm.enumerateIds(HE::AssetType::Texture).size()    == 2);
}

TEST_CASE("ContentManager enumerateIds unload removes entry")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());

	StaticMeshAsset m; m.name = "temp"; m.path = "mem://temp";
	HE::UUID id = cm.registerStaticMesh(std::move(m));

	auto before = cm.enumerateIds();
	REQUIRE(before.size() == 8); // 7 defaults + 1

	REQUIRE(cm.unloadAsset(id));

	auto after = cm.enumerateIds();
	CHECK(after.size() == 7);
	for (auto uid : after) CHECK_FALSE(uid == id);

	// Type-filtered enumeration also must not contain it
	auto meshes = cm.enumerateIds(HE::AssetType::StaticMesh);
	CHECK(meshes.size() == 3); // default cube + quad + snowflake
	for (auto uid : meshes) CHECK_FALSE(uid == id);
}

TEST_CASE("ContentManager default assets are addressable by virtual path")
{
	ContentManager cm;
	CHECK(cm.isLoaded("mem://default_cube"));
	CHECK(cm.isLoaded("mem://default_white"));
	CHECK(cm.isLoaded("mem://default_material"));
	CHECK(cm.isLoaded("mem://default_grid_tex"));
	CHECK(cm.isLoaded("mem://default_terrain_material"));
}

TEST_CASE("ContentManager default grid texture has correct dimensions and grid pattern")
{
	ContentManager cm;
	HE::UUID id = cm.loadAsset("mem://default_grid_tex");
	REQUIRE_FALSE(id == HE::UUID{});
	auto* tex = cm.getTexture(id);
	REQUIRE(tex != nullptr);
	CHECK(tex->width    == 128);
	CHECK(tex->height   == 128);
	CHECK(tex->channels == 4);
	// pixel (0,0) is a corner accent dot (lighter than the line: R=148)
	CHECK(tex->data[0] == 148);
	// pixel (2,2) is in the background cell (light cool-grey R=228)
	CHECK(tex->data[(2 * 128 + 2) * 4] == 228);
}

TEST_CASE("ContentManager default terrain material is flat grey with no texture")
{
	ContentManager cm;
	HE::UUID id = cm.loadAsset("mem://default_terrain_material");
	REQUIRE_FALSE(id == HE::UUID{});
	auto* mat = cm.getMaterial(id);
	REQUIRE(mat != nullptr);
	CHECK(mat->texturePaths.empty());
	CHECK(mat->roughness == doctest::Approx(0.8f));
	// neutral grey base colour
	CHECK(mat->baseColor[0] == doctest::Approx(0.50f));
	CHECK(mat->baseColor[1] == doctest::Approx(0.52f));
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

// ── AssetRef (pin-based lifetime) ─────────────────────────────────────────────

TEST_CASE("AssetRef acquireStaticMesh returns valid handle for existing asset")
{
	ContentManager cm;
	auto ref = cm.acquireStaticMesh(HE::kDefaultCubeMeshId);
	REQUIRE(ref);
	CHECK(ref.id() == HE::kDefaultCubeMeshId);
	CHECK(ref.get() != nullptr);
	CHECK(ref->id == HE::kDefaultCubeMeshId);
}

TEST_CASE("AssetRef acquireStaticMesh returns null handle for unknown UUID")
{
	ContentManager cm;
	auto ref = cm.acquireStaticMesh(HE::UUID::generate());
	CHECK_FALSE(ref);
	CHECK(ref.get() == nullptr);
}

TEST_CASE("AssetRef blocks unloadAsset while alive")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());

	StaticMeshAsset mesh;
	mesh.name     = "pinned";
	mesh.vertices = { 0,0,0, 1,0,0, 0,1,0 };
	mesh.indices  = { 0, 1, 2 };
	HE::UUID id = cm.registerStaticMesh(std::move(mesh));
	REQUIRE(cm.isLoaded(id));

	{
		auto pin = cm.acquireStaticMesh(id);
		REQUIRE(pin);
		CHECK(cm.isPinned(id));
		// unload must fail while the pin is alive
		CHECK_FALSE(cm.unloadAsset(id));
		CHECK(cm.isLoaded(id));
	}

	// pin went out of scope — should succeed now
	CHECK_FALSE(cm.isPinned(id));
	CHECK(cm.unloadAsset(id));
	CHECK_FALSE(cm.isLoaded(id));
}

TEST_CASE("AssetRef copy shares the pin; both must be released")
{
	ContentManager cm;
	REQUIRE_FALSE(cm.isPinned(HE::kDefaultCubeMeshId));

	auto a = cm.acquireStaticMesh(HE::kDefaultCubeMeshId);
	CHECK(cm.isPinned(HE::kDefaultCubeMeshId));

	{
		auto b = a; // copy → pin count 2
		CHECK(cm.isPinned(HE::kDefaultCubeMeshId));
		CHECK(b.get() == a.get());
	} // b destroyed → pin count 1, still pinned

	CHECK(cm.isPinned(HE::kDefaultCubeMeshId));
} // a destroyed → pin count 0

TEST_CASE("AssetRef move transfers the pin without doubling it")
{
	ContentManager cm;

	auto a = cm.acquireStaticMesh(HE::kDefaultCubeMeshId);
	CHECK(a);
	{
		auto b = std::move(a); // move — a is now null
		CHECK_FALSE(a);
		CHECK(b);
		CHECK(cm.isPinned(HE::kDefaultCubeMeshId));
	} // b destroyed → pin released

	CHECK_FALSE(cm.isPinned(HE::kDefaultCubeMeshId));
}

TEST_CASE("AssetRef reset releases the pin early")
{
	ContentManager cm;
	auto ref = cm.acquireStaticMesh(HE::kDefaultCubeMeshId);
	REQUIRE(ref);
	ref.reset();
	CHECK_FALSE(ref);
	CHECK_FALSE(cm.isPinned(HE::kDefaultCubeMeshId));
}

TEST_CASE("AssetRef acquireTexture and acquireMaterial work on default assets")
{
	ContentManager cm;
	auto tex = cm.acquireTexture(HE::kDefaultWhiteTextureId);
	auto mat = cm.acquireMaterial(HE::kDefaultMaterialId);
	REQUIRE(tex);
	REQUIRE(mat);
	CHECK(tex->width   == 1);
	CHECK(mat->roughness == doctest::Approx(0.5f));
}

TEST_CASE("ContentManager pollHotReload ignores virtual mem:// paths")
{
	// Default manager has only mem:// assets — poll must not crash or signal any.
	ContentManager cm;
	CHECK(cm.pollHotReload().empty());
}

TEST_CASE("ContentManager pollHotReload skips mid-write (invalid) files")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());

	MaterialAsset mat;
	mat.type = HE::AssetType::Material;
	mat.name = "guarded";
	mat.path = "guarded.hasset";
	REQUIRE(cm.saveAsset(mat));
	const HE::UUID id = mat.id;
	REQUIRE(cm.loadAsset("guarded.hasset") == id);

	// Overwrite with garbage so getAssetType returns Unknown (simulates mid-write).
	const fs::path diskPath = dir.path / "guarded.hasset";
	{ std::ofstream f(diskPath, std::ios::binary); f << "GARBAGE_NOT_A_VALID_ASSET"; }
	auto t = fs::last_write_time(diskPath);
	fs::last_write_time(diskPath, t + std::chrono::seconds(2));

	// Poll must skip the file and keep the old asset alive.
	auto changed = cm.pollHotReload();
	CHECK(changed.empty());
	CHECK(cm.getMaterial(id) != nullptr);
}

// ─── SkeletalMeshAsset / CHUNK_SKEL round-trip ──────────────────────────────

TEST_CASE("ContentManager skeletal mesh round-trip (no skeleton)")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());

	SkeletalMeshAsset mesh;
	mesh.type     = HE::AssetType::SkeletalMesh;
	mesh.name     = "sm_noskel";
	mesh.path     = "sm_noskel.hasset";
	mesh.vertices = { 0,0,0, 1,0,0, 0,1,0 };
	mesh.indices  = { 0,1,2 };
	mesh.normals  = { 0,0,1, 0,0,1, 0,0,1 };
	mesh.uvs      = { 0,0, 1,0, 0,1 };
	mesh.boneIDs     = { 0,0,0,0, 0,0,0,0, 0,0,0,0 };
	mesh.boneWeights = { 1,0,0,0, 1,0,0,0, 1,0,0,0 };
	REQUIRE(cm.saveAsset(mesh));

	ContentManager cm2(dir.path.string());
	HE::UUID id = cm2.loadAsset("sm_noskel.hasset");
	const SkeletalMeshAsset* loaded = cm2.getSkeletalMesh(id);
	REQUIRE(loaded != nullptr);
	CHECK(loaded->name == "sm_noskel");
	CHECK(loaded->vertices.size() == 9);
	CHECK(loaded->indices.size() == 3);
	CHECK(loaded->normals.size() == 9);
	CHECK(loaded->uvs.size() == 6);
	CHECK(loaded->boneIDs.size() == 12);
	CHECK(loaded->boneWeights.size() == 12);
	CHECK(loaded->skeleton.empty());
}

TEST_CASE("ContentManager skeletal mesh CHUNK_SKEL round-trip")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());

	SkeletalMeshAsset mesh;
	mesh.type = HE::AssetType::SkeletalMesh;
	mesh.name = "sm_skel";
	mesh.path = "sm_skel.hasset";

	// Minimal geometry
	mesh.vertices = { 0,0,0, 1,0,0 };
	mesh.indices  = { 0,1 };
	mesh.normals  = { 0,1,0, 0,1,0 };
	mesh.uvs      = { 0,0, 1,0 };
	mesh.boneIDs     = { 0,0,0,0, 1,0,0,0 };
	mesh.boneWeights = { 1,0,0,0, 0.5f,0.5f,0,0 };

	// Two-joint skeleton: root (index 0) and child (index 1)
	SkeletonJoint root;
	root.name   = "root";
	root.parent = -1;
	root.inverseBindMatrix = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

	SkeletonJoint child;
	child.name   = "hip";
	child.parent = 0;
	child.inverseBindMatrix = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 1,2,3,1 };

	mesh.skeleton = { root, child };
	REQUIRE(cm.saveAsset(mesh));
	const HE::UUID savedId = mesh.id;

	// Reload in a fresh ContentManager (simulates engine restart)
	ContentManager cm2(dir.path.string());
	HE::UUID loadedId = cm2.loadAsset("sm_skel.hasset");
	CHECK(loadedId == savedId);

	const SkeletalMeshAsset* loaded = cm2.getSkeletalMesh(loadedId);
	REQUIRE(loaded != nullptr);
	REQUIRE(loaded->skeleton.size() == 2);

	CHECK(loaded->skeleton[0].name   == "root");
	CHECK(loaded->skeleton[0].parent == -1);
	CHECK(loaded->skeleton[0].inverseBindMatrix[0] == 1.0f);
	CHECK(loaded->skeleton[0].inverseBindMatrix[15] == 1.0f);

	CHECK(loaded->skeleton[1].name   == "hip");
	CHECK(loaded->skeleton[1].parent == 0);
	CHECK(loaded->skeleton[1].inverseBindMatrix[12] == 1.0f);
	CHECK(loaded->skeleton[1].inverseBindMatrix[13] == 2.0f);
	CHECK(loaded->skeleton[1].inverseBindMatrix[14] == 3.0f);
}

TEST_CASE("ContentManager SkeletonJoint default values")
{
	SkeletonJoint j;
	CHECK(j.parent == -1);
	CHECK(j.name.empty());
	// All floats default to zero
	for (float f : j.inverseBindMatrix)
		CHECK(f == 0.0f);
}

TEST_CASE("ContentManager skeletal mesh preserves UUID across save/load")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());

	SkeletalMeshAsset mesh;
	mesh.type = HE::AssetType::SkeletalMesh;
	mesh.name = "sm_uuid";
	mesh.path = "sm_uuid.hasset";
	mesh.vertices = { 0,0,0 };
	mesh.indices  = { 0 };
	REQUIRE(cm.saveAsset(mesh));
	const HE::UUID original = mesh.id;
	REQUIRE_FALSE(original == HE::UUID{});

	ContentManager cm2(dir.path.string());
	HE::UUID reloaded = cm2.loadAsset("sm_uuid.hasset");
	CHECK(reloaded == original);
}

TEST_CASE("ContentManager skeletal mesh wrong-type lookup returns null")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());

	SkeletalMeshAsset mesh;
	mesh.type = HE::AssetType::SkeletalMesh;
	mesh.name = "sm_wrongtype";
	mesh.path = "sm_wrongtype.hasset";
	REQUIRE(cm.saveAsset(mesh));
	HE::UUID id = cm.loadAsset("sm_wrongtype.hasset");
	CHECK(cm.getSkeletalMesh(id) != nullptr);
	CHECK(cm.getStaticMesh(id)   == nullptr);
	CHECK(cm.getTexture(id)      == nullptr);
}
