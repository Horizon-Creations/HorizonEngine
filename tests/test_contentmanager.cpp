#include "doctest.h"
#include "TestFsUtil.h"
#include <ContentManager/AssetRefRetarget.h>  // the rename half the editor does after fs::rename
#include <ContentManager/AssetRefScan.h>      // assetUuidOfFile — identity across a reimport
#include <ContentManager/ContentManager.h>
#include <ContentManager/DefaultAssets.h>
#include <ContentManager/HAsset.h>
#include "ImporterCommon.h"   // import provenance: writeAsset / sourceFileOf / reimport
#include <Diagnostics/GlobalState.h>
#include <MaterialGraph/MaterialGraph.h> // HE::MatParamKind
#include <Types/TypeRegistry.h>              // struct/enum defs mirror in on load
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
	struct TempContentDir
	{
		fs::path path;
		explicit TempContentDir(const char* name = "he_test_content")
		{
			path = fs::temp_directory_path() / name;
			he_test::removeAllQuiet(path);
			fs::create_directories(path);
		}
		~TempContentDir() { he_test::removeAllQuiet(path); }
	};

	// RAII env-var override for HE_ENGINE_CONTENT_EDITABLE — restores whatever
	// was there before (or unsets it) so this never leaks into another test.
	struct ScopedEnv
	{
		std::string name;
		bool hadPrev = false;
		std::string prev;
		ScopedEnv(const char* n, const char* value) : name(n)
		{
			if (const char* p = std::getenv(n)) { hadPrev = true; prev = p; }
#ifdef _WIN32
			_putenv_s(n, value);
#else
			setenv(n, value, 1);
#endif
		}
		~ScopedEnv()
		{
#ifdef _WIN32
			_putenv_s(name.c_str(), hadPrev ? prev.c_str() : "");
#else
			if (hadPrev) setenv(name.c_str(), prev.c_str(), 1);
			else         unsetenv(name.c_str());
#endif
		}
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

TEST_CASE("Struct/Enum assets register under their FILE name, not the stored one")
{
	TempContentDir dir;
	// Created as "NewStruct"/"NewEnum" (the panel's defaults) and renamed on
	// disk afterwards — which is all the Content Browser's rename does. The
	// name persisted in CHUNK_META keeps the creation-time value, so trusting it
	// showed every renamed type as "NewStruct"/"NewEnum" in the type dropdowns.
	{
		ContentManager cm(dir.path.string());
		StructTypeAsset a;
		a.type = HE::AssetType::StructType;
		a.name = "NewStruct";
		a.path = "PlayerStats.hasset";
		a.json = R"({"fields":[{"name":"hp","type":1}]})";
		REQUIRE(cm.saveAsset(a));

		EnumTypeAsset e;
		e.type = HE::AssetType::EnumType;
		e.name = "NewEnum";
		e.path = "Mood.hasset";
		e.json = R"({"entries":[{"name":"Calm","value":0}]})";
		REQUIRE(cm.saveAsset(e));
	}
	{
		HE::TypeRegistry::instance().clear();
		ContentManager cm(dir.path.string());
		REQUIRE_FALSE(cm.loadAsset("PlayerStats.hasset") == HE::UUID{});
		REQUIRE_FALSE(cm.loadAsset("Mood.hasset") == HE::UUID{});

		HE::StructDef sd;
		REQUIRE(HE::TypeRegistry::instance().getStruct("PlayerStats.hasset", sd));
		CHECK(sd.name == "PlayerStats");
		HE::EnumDef ed;
		REQUIRE(HE::TypeRegistry::instance().getEnum("Mood.hasset", ed));
		CHECK(ed.name == "Mood");
		HE::TypeRegistry::instance().clear();
	}
}

TEST_CASE("ContentManager HorizonCode class round-trip")
{
	TempContentDir dir;
	const std::string graph = R"({"nextId":3,"nodes":[],"links":[],"variables":[]})";
	HE::UUID savedId;
	{
		ContentManager cm(dir.path.string());
		HorizonCodeClassAsset a;
		a.type      = HE::AssetType::HorizonCodeClass;
		a.name      = "MyClass";
		a.path      = "MyClass.hasset";
		a.graphJson = graph;
		REQUIRE(cm.saveAsset(a));
		savedId = a.id;
		REQUIRE_FALSE(savedId == HE::UUID{});
	}
	{
		ContentManager cm(dir.path.string());
		HE::UUID loadedId = cm.loadAsset("MyClass.hasset");
		CHECK(loadedId == savedId);
		const HorizonCodeClassAsset* a = cm.getHorizonCodeClass(loadedId);
		REQUIRE(a != nullptr);
		CHECK(a->name == "MyClass");
		CHECK(a->graphJson == graph);
		CHECK(cm.getWidget(loadedId) == nullptr); // wrong-type lookup must not alias
	}
}

TEST_CASE("ContentManager HorizonCode class baseClass round-trip")
{
	TempContentDir dir;
	HE::UUID savedId;
	{
		ContentManager cm(dir.path.string());
		HorizonCodeClassAsset a;
		a.type      = HE::AssetType::HorizonCodeClass;
		a.name      = "PC";
		a.path      = "PC.hasset";
		a.graphJson = R"({"nextId":1,"nodes":[],"links":[],"variables":[]})";
		a.baseClass = "PlayerController";
		REQUIRE(cm.saveAsset(a));
		savedId = a.id;
	}
	{
		ContentManager cm(dir.path.string());
		const HorizonCodeClassAsset* a = cm.getHorizonCodeClass(cm.loadAsset("PC.hasset"));
		REQUIRE(a != nullptr);
		CHECK(a->baseClass == "PlayerController");
	}
}

TEST_CASE("ContentManager input action + mapping context round-trip")
{
	TempContentDir dir;
	const std::string actionJson  = R"({"valueType":"Axis"})";
	const std::string mappingJson =
		R"({"entries":[{"action":"IA_Move.hasset","axes":[{"positive":"W","negative":"S","scale":1.0}]}]})";
	HE::UUID actionId, mappingId;
	{
		ContentManager cm(dir.path.string());
		InputActionAsset ia;
		ia.type = HE::AssetType::InputAction;
		ia.name = "IA_Move";
		ia.path = "IA_Move.hasset";
		ia.json = actionJson;
		REQUIRE(cm.saveAsset(ia));
		actionId = ia.id;

		InputMappingContextAsset mc;
		mc.type = HE::AssetType::InputMappingContext;
		mc.name = "IMC_Default";
		mc.path = "IMC_Default.hasset";
		mc.json = mappingJson;
		REQUIRE(cm.saveAsset(mc));
		mappingId = mc.id;
	}
	{
		ContentManager cm(dir.path.string());
		const InputActionAsset* ia = cm.getInputAction(cm.loadAsset("IA_Move.hasset"));
		REQUIRE(ia != nullptr);
		CHECK(ia->id == actionId);
		CHECK(ia->json == actionJson);

		const InputMappingContextAsset* mc =
			cm.getInputMappingContext(cm.loadAsset("IMC_Default.hasset"));
		REQUIRE(mc != nullptr);
		CHECK(mc->id == mappingId);
		CHECK(mc->json == mappingJson);

		// Wrong-type lookups must not alias
		CHECK(cm.getInputAction(mappingId) == nullptr);
		CHECK(cm.getInputMappingContext(actionId) == nullptr);
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

// Regression: width/height/channels used to be written as size_t, so the TXMI
// chunk layout differed between 32- and 64-bit builds. They are uint32 now, but
// every .hasset a 64-bit build wrote before that change still carries the 8-byte
// fields and MUST keep loading — HAsset::readTextureHeader tells the two layouts
// apart by chunk size (legacy ≥ 24 bytes, current 18).
TEST_CASE("ContentManager loads the legacy (size_t) TXMI texture layout")
{
	auto legacyBlob = [](const HE::UUID& id, const char* name, bool withCookTail)
	{
		std::vector<uint8_t> meta;
		HAsset::Writer::appendPOD(meta, static_cast<uint16_t>(HE::AssetType::Texture));
		HAsset::Writer::appendPOD(meta, id.hi);
		HAsset::Writer::appendPOD(meta, id.lo);
		HAsset::Writer::appendString(meta, name);
		HAsset::Writer::appendString(meta, std::string("mem://") + name);

		std::vector<uint8_t> txmi;
		HAsset::Writer::appendPOD(txmi, static_cast<uint64_t>(2)); // width  (size_t on 64-bit)
		HAsset::Writer::appendPOD(txmi, static_cast<uint64_t>(2)); // height
		HAsset::Writer::appendPOD(txmi, static_cast<uint64_t>(4)); // channels
		if (withCookTail)
		{
			HAsset::Writer::appendPOD(txmi, static_cast<uint32_t>(1)); // mipLevels
			HAsset::Writer::appendPOD(txmi, static_cast<uint8_t>(0));  // format RGBA8
			HAsset::Writer::appendPOD(txmi, static_cast<uint8_t>(1));  // srgb
		}
		CHECK(txmi.size() == (withCookTail ? 30u : 24u));

		const std::vector<uint8_t> pixels(16, 0xAB);
		HAsset::Writer w;
		w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
		w.addChunk(HAsset::CHUNK_TXMI, txmi.data(), txmi.size());
		w.addChunk(HAsset::CHUNK_PIXL, pixels.data(), pixels.size());
		return w.toBytes(static_cast<uint16_t>(HE::AssetType::Texture));
	};

	const HE::UUID cooked{0x7E, 0x11}, preCook{0x7E, 0x12};

	ContentManager cm;
	REQUIRE(cm.loadAssetFromMemory(legacyBlob(cooked, "legacy_cooked", true)) == cooked);
	const TextureAsset* t = cm.getTexture(cooked);
	REQUIRE(t != nullptr);
	CHECK(t->width    == 2);
	CHECK(t->height   == 2);
	CHECK(t->channels == 4);
	CHECK(t->srgb);                    // cook tail still parsed at the right offset
	CHECK(t->data.size() == 16);

	// Pre-cook legacy files (24 bytes, no tail) keep their defaults.
	REQUIRE(cm.loadAssetFromMemory(legacyBlob(preCook, "legacy_precook", false)) == preCook);
	const TextureAsset* p = cm.getTexture(preCook);
	REQUIRE(p != nullptr);
	CHECK(p->width    == 2);
	CHECK(p->height   == 2);
	CHECK(p->channels == 4);
	CHECK(p->mipLevels == 1);
	CHECK_FALSE(p->srgb);

	// A re-save writes the CURRENT layout — 12 bytes of header + the 6-byte cook
	// tail. That 18 < 24 is what keeps the legacy discriminator working.
	TempContentDir dir;
	ContentManager saver(dir.path.string());
	TextureAsset out;
	out.type = HE::AssetType::Texture; out.name = "migrated"; out.path = "migrated.hasset";
	out.width = 2; out.height = 2; out.channels = 4; out.data = std::vector<uint8_t>(16, 0x01);
	REQUIRE(saver.saveAsset(out));
	HAsset::Reader r;
	REQUIRE(r.open((dir.path / "migrated.hasset").string()));
	const HAsset::Reader::Chunk* c = r.findChunk(HAsset::CHUNK_TXMI);
	REQUIRE(c != nullptr);
	CHECK(c->data.size() == 18);
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

// Regression: unloadAsset() only searched a subset of the SlotMaps. For
// InputAction / InputMappingContext / ParticleSystem / AnimatorStateMachine it
// found nothing, returned false and left the asset resident — which also killed
// hot reload for those types, because pollHotReload() unloads before reloading.
TEST_CASE("ContentManager unload works for every asset type it can load")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());

	InputActionAsset ia;
	ia.type = HE::AssetType::InputAction;
	ia.name = "IA_Fire";
	ia.path = "IA_Fire.hasset";
	ia.json = R"({"valueType":"Bool"})";
	REQUIRE(cm.saveAsset(ia));

	InputMappingContextAsset imc;
	imc.type = HE::AssetType::InputMappingContext;
	imc.name = "IMC_Unload";
	imc.path = "IMC_Unload.hasset";
	imc.json = R"({"entries":[]})";
	REQUIRE(cm.saveAsset(imc));

	ParticleGraphAsset pg;
	pg.type          = HE::AssetType::ParticleSystem;
	pg.name          = "PG_Sparks";
	pg.path          = "PG_Sparks.hasset";
	pg.nodeGraphJson = R"({"nextId":1,"nodes":[],"links":[]})";
	REQUIRE(cm.saveAsset(pg));

	AnimatorStateMachineAsset asm_;
	asm_.type      = HE::AssetType::AnimatorStateMachine;
	asm_.name      = "ASM_Locomotion";
	asm_.path      = "ASM_Locomotion.hasset";
	asm_.graphJson = R"({"states":[],"transitions":[]})";
	REQUIRE(cm.saveAsset(asm_));

	const HE::UUID actionId  = cm.loadAsset("IA_Fire.hasset");
	const HE::UUID mappingId = cm.loadAsset("IMC_Unload.hasset");
	const HE::UUID particleId = cm.loadAsset("PG_Sparks.hasset");
	const HE::UUID stateId   = cm.loadAsset("ASM_Locomotion.hasset");
	REQUIRE(cm.getInputAction(actionId)           != nullptr);
	REQUIRE(cm.getInputMappingContext(mappingId)  != nullptr);
	REQUIRE(cm.getParticleGraph(particleId)       != nullptr);
	REQUIRE(cm.getAnimatorStateMachine(stateId)   != nullptr);

	CHECK(cm.unloadAsset(actionId));
	CHECK_FALSE(cm.isLoaded(actionId));
	CHECK(cm.getInputAction(actionId) == nullptr);
	CHECK(cm.assetType(actionId) == HE::AssetType::Unknown);
	CHECK_FALSE(cm.isLoaded("IA_Fire.hasset")); // path→UUID entry gone too

	CHECK(cm.unloadAsset(mappingId));
	CHECK_FALSE(cm.isLoaded(mappingId));
	CHECK(cm.getInputMappingContext(mappingId) == nullptr);

	CHECK(cm.unloadAsset(particleId));
	CHECK_FALSE(cm.isLoaded(particleId));
	CHECK(cm.getParticleGraph(particleId) == nullptr);

	CHECK(cm.unloadAsset(stateId));
	CHECK_FALSE(cm.isLoaded(stateId));
	CHECK(cm.getAnimatorStateMachine(stateId) == nullptr);

	// A second unload of an already-evicted asset stays false.
	CHECK_FALSE(cm.unloadAsset(actionId));
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

// The ordering the other instance tests never produce, and the one every real
// project produces: the INSTANCE is registered first and its parent is loaded
// from disk during the sync. That load inserts into the very SlotMap the
// instance lives in — a dense vector — so the instance pointer syncMaterialInstance
// was holding pointed at the old buffer, and every value it copied from the
// parent went into freed memory. The visible symptom is an instance that opens
// with none of its parent's shading.
//
// Many pairs AND a varying number of unrelated materials between them, and both
// halves are load-bearing.
//
// A vector only moves when it crosses its capacity, so whether this bug shows up
// depends on which insert lands on a growth boundary. Loading pairs alone is not
// enough and the reason is worth writing down: instance-then-parent is a strict
// alternation, so with the pool starting at an odd size EVERY boundary falls on
// an instance insert and never on a parent load — the sync's pointer is taken
// after the move and survives by arithmetic. Forty pairs reproduced nothing.
// The fillers break the alternation, so the boundaries land on parent loads too.
//
// That is also the honest answer to why this bug lived so long: it needs the
// pool to grow at one specific moment, and most projects never line up.
//
// MUTATION: in ContentManager::syncMaterialInstance, take `inst` before the
// loadAsset again (move `getMaterialMutable(instanceId)` above the parent load).
// Opening a SECOND project has to forget the first one, and it did not.
//
// The path→UUID map is keyed by the CONTENT-RELATIVE path, and two projects made
// from the same template share every one of those keys. So after switching roots,
// loadAsset("mats/m.hasset") found the first project's entry still sitting there,
// returned its UUID, and handed back its data — with the new project's file right
// there on disk, unread. The content browser then listed the new project's files
// and showed the old project's contents, which is what "it stays in the old one"
// looks like from the outside.
//
// setContentRoot was a one-line setter, and scanContentDirectory cleared only the
// UUID→path index; nothing dropped the loaded assets, the path map, the type
// index or the mtimes.
//
// MUTATION: in ContentManager::setContentRoot, drop the forgetProjectContent()
// call — the second project reads back as the first.
TEST_CASE("ContentManager: opening another project does not serve the old one's assets")
{
	TempContentDir a("he_test_cm_projA");
	TempContentDir b("he_test_cm_projB");

	auto writeMaterial = [](const std::string& rootPath, float red)
	{
		ContentManager cm(rootPath);
		MaterialAsset m;
		m.type = HE::AssetType::Material;
		m.name = "Shared";
		m.path = "mats/m.hasset";     // the SAME relative path in both projects
		m.baseColor[0] = red;
		REQUIRE(cm.saveAsset(m));
	};
	writeMaterial(a.path.string(), 1.0f);   // project A: red
	writeMaterial(b.path.string(), 0.0f);   // project B: not red

	// ONE manager for the life of the editor, which is how the editor works: the
	// project changes underneath it.
	ContentManager cm(a.path.string());
	cm.scanContentDirectory();
	const HE::UUID inA = cm.loadAsset("mats/m.hasset");
	REQUIRE(cm.getMaterial(inA) != nullptr);
	CHECK(cm.getMaterial(inA)->baseColor[0] == doctest::Approx(1.0f));

	// …and now the other project, exactly as EditorApplication's
	// onProjectLoaded callback does it.
	cm.setContentRoot(b.path.string());
	cm.scanContentDirectory();
	const HE::UUID inB = cm.loadAsset("mats/m.hasset");
	REQUIRE(cm.getMaterial(inB) != nullptr);
	CHECK_MESSAGE(cm.getMaterial(inB)->baseColor[0] == doctest::Approx(0.0f),
	              "the second project's asset came back with the first project's data");

	// The built-ins survive the switch. They are registered at construction and
	// nothing on disk backs them, so a reset that forgot them would break every
	// scene referencing the default cube or material by UUID.
	CHECK(cm.getStaticMesh(HE::kDefaultCubeMeshId) != nullptr);
	CHECK(cm.getMaterial(HE::kDefaultMaterialId)   != nullptr);
}

TEST_CASE("Material instance: syncing survives the parent load that moves the pool")
{
	TempContentDir dir("he_test_cm_instance_reload");
	constexpr int kPairs = 40;

	// Author them on disk through one manager…
	{
		ContentManager cm(dir.path.string());
		for (int i = 0; i < kPairs; ++i)
		{
			const std::string n = std::to_string(i);
			MaterialAsset master;
			master.type = HE::AssetType::Material;
			master.name = "Master" + n;
			master.path = "mats/master" + n + ".hasset";
			// A value per pair, so a mixed-up parent is as visible as a missing one.
			master.baseColor[0] = 0.01f * static_cast<float>(i);
			master.metallic     = 0.5f;
			master.roughness    = 0.25f;
			REQUIRE(cm.saveAsset(master));

			MaterialAsset inst;
			inst.type = HE::AssetType::Material;
			inst.name = "Inst" + n;
			inst.path = "mats/inst" + n + ".hasset";
			inst.parentMaterialPath = master.path;
			REQUIRE(cm.saveAsset(inst));

			// Parentless fillers, loaded between the pairs below.
			for (int f = 0; f < 3; ++f)
			{
				MaterialAsset pad;
				pad.type = HE::AssetType::Material;
				pad.name = "Pad" + n + "_" + std::to_string(f);
				pad.path = "mats/pad" + n + "_" + std::to_string(f) + ".hasset";
				REQUIRE(cm.saveAsset(pad));
			}
		}
	}

	// …then load ONLY the instances through a fresh one, so every parent arrives
	// during its instance's sync rather than before it.
	ContentManager fresh(dir.path.string());
	for (int i = 0; i < kPairs; ++i)
	{
		const std::string n = std::to_string(i);
		for (int f = 0; f < i % 3; ++f)
			fresh.loadAsset("mats/pad" + n + "_" + std::to_string(f) + ".hasset");

		const HE::UUID id = fresh.loadAsset("mats/inst" + n + ".hasset");
		const MaterialAsset* inst = fresh.getMaterial(id);
		REQUIRE(inst != nullptr);
		// Base surface state follows the parent — what the sync is for, and what
		// silently did not happen for whichever instance's parent load moved the
		// pool.
		CHECK_MESSAGE(inst->baseColor[0] == doctest::Approx(0.01f * static_cast<float>(i)),
		              ("instance " + n + " did not take its parent's colour").c_str());
		CHECK(inst->metallic  == doctest::Approx(0.5f));
		CHECK(inst->roughness == doctest::Approx(0.25f));
	}
}

TEST_CASE("Material instance: param override survives sync; switch override re-permutes")
{
	ContentManager cm;

	// ── Master: graph with a named color param and a static switch. ──
	HE::MaterialGraph g;
	const int out = g.addNode(HE::MatNodeType::Output);
	const int pc  = g.addNode(HE::MatNodeType::ParamColor);
	g.findNode(pc)->s = "Tint";
	g.findNode(pc)->p[0] = 0.5f; g.findNode(pc)->p[1] = 0.5f; g.findNode(pc)->p[2] = 0.5f;
	const int sw  = g.addNode(HE::MatNodeType::StaticSwitch);
	g.findNode(sw)->s = "Fancy"; // default ON
	const int red = g.addNode(HE::MatNodeType::ConstColor);
	g.findNode(red)->p[0] = 0.93f;
	CHECK(g.connect(red, 0, sw, 0)); // True branch
	CHECK(g.connect(pc,  0, sw, 1)); // False branch = the param
	CHECK(g.connect(sw,  0, out, 0));
	// The param ALSO drives Emissive so it exists in every permutation (the switch's
	// default-ON culls the False branch — without this, the master would have 0 params).
	CHECK(g.connect(pc,  0, out, 3));

	MaterialAsset master;
	master.type = HE::AssetType::Material;
	master.name = "Master";
	master.path = "mats/master.hasset";
	master.nodeGraphJson = HE::materialGraphToJson(g);
	const HE::MatShaderGen gen = HE::generateFragment(g);
	master.customShaderFragGlsl = gen.glsl;
	for (const auto& slot : gen.params)
	{
		master.shaderParamData.insert(master.shaderParamData.end(), slot.value, slot.value + 4);
		master.graphParamNames.push_back(slot.name);
		master.graphParamTypes.push_back(static_cast<uint8_t>(slot.kind));
		master.graphParamMinMax.insert(master.graphParamMinMax.end(), { slot.minV, slot.maxV });
		master.graphParamGroups.push_back(slot.group);
		master.graphParamTooltips.push_back(slot.tooltip);
	}
	const HE::UUID masterId = cm.registerMaterial(std::move(master));
	REQUIRE(cm.getMaterial(masterId) != nullptr);

	// ── Instance: pure param override → byte-identical shader (same pipeline). ──
	MaterialAsset instA;
	instA.type = HE::AssetType::Material;
	instA.name = "InstA";
	instA.path = "mats/instA.hasset";
	instA.parentMaterialPath = "mats/master.hasset";
	const HE::UUID instAId = cm.registerMaterial(std::move(instA));
	cm.syncMaterialInstance(instAId);
	{
		MaterialAsset* ia = cm.getMaterialMutable(instAId);
		REQUIRE(ia != nullptr);
		const MaterialAsset* ma = cm.getMaterial(masterId);
		CHECK(ia->customShaderFragGlsl == ma->customShaderFragGlsl); // SAME source → same pipeline
		REQUIRE(ia->graphParamNames.size() == 1);

		// Override "Tint" on the instance, then re-sync: the override survives,
		// non-overridden state keeps following the parent.
		ia->instanceOverriddenParams.push_back("Tint");
		ia->shaderParamData[0] = 0.11f; ia->shaderParamData[1] = 0.22f; ia->shaderParamData[2] = 0.33f;
		cm.syncMaterialInstance(instAId);
		ia = cm.getMaterialMutable(instAId);
		CHECK(ia->shaderParamData[0] == doctest::Approx(0.11f)); // instance value kept
		CHECK(ia->customShaderFragGlsl == cm.getMaterial(masterId)->customShaderFragGlsl);
	}

	// ── Instance with a SWITCH override → different permutation (own source). ──
	MaterialAsset instB;
	instB.type = HE::AssetType::Material;
	instB.name = "InstB";
	instB.path = "mats/instB.hasset";
	instB.parentMaterialPath = "mats/master.hasset";
	instB.instanceSwitchNames.push_back("Fancy");
	instB.instanceSwitchValues.push_back(0); // force OFF → the param branch
	const HE::UUID instBId = cm.registerMaterial(std::move(instB));
	cm.syncMaterialInstance(instBId);
	{
		const MaterialAsset* ib = cm.getMaterial(instBId);
		REQUIRE(ib != nullptr);
		CHECK(ib->customShaderFragGlsl != cm.getMaterial(masterId)->customShaderFragGlsl);
		CHECK(ib->customShaderFragGlsl.find("0.930000") == std::string::npos); // red branch culled
		CHECK(ib->customShaderFragGlsl.find("param: Tint") != std::string::npos);
	}
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
	// 8 built-in defaults: cube, quad, snowflake, white tex, grid tex, layer-0
	// weightmap, default material, terrain material.
	ContentManager cm;

	const size_t defaultCount = cm.assetCount();
	REQUIRE(defaultCount == 8);

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
	// 3 meshes (cube + quad + snowflake), 3 textures (white + grid + layer-0
	// weightmap), 2 materials (default + terrain).
	ContentManager cm;

	auto meshes   = cm.enumerateIds(HE::AssetType::StaticMesh);
	auto textures = cm.enumerateIds(HE::AssetType::Texture);
	auto materials= cm.enumerateIds(HE::AssetType::Material);
	auto scripts  = cm.enumerateIds(HE::AssetType::Script);

	CHECK(meshes.size()    == 3);
	CHECK(textures.size()  == 3);
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
	CHECK(contains(textures,   HE::kDefaultLayer0WeightTextureId));
	CHECK(contains(materials,  HE::kDefaultMaterialId));
	CHECK(contains(materials,  HE::kDefaultTerrainMaterialId));

	// Add another mesh — only meshes count increases.
	StaticMeshAsset m2; m2.name = "m2";
	cm.registerStaticMesh(std::move(m2));
	CHECK(cm.enumerateIds(HE::AssetType::StaticMesh).size() == 4);
	CHECK(cm.enumerateIds(HE::AssetType::Texture).size()    == 3);
}

TEST_CASE("ContentManager enumerateIds unload removes entry")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());

	StaticMeshAsset m; m.name = "temp"; m.path = "mem://temp";
	HE::UUID id = cm.registerStaticMesh(std::move(m));

	auto before = cm.enumerateIds();
	REQUIRE(before.size() == 9); // 8 defaults + 1

	REQUIRE(cm.unloadAsset(id));

	auto after = cm.enumerateIds();
	CHECK(after.size() == 8);
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

// Regression: pollHotReload() unloads before reloading, so an asset type that
// unloadAsset() did not know stayed pinned at its old contents forever. Proven
// end-to-end here on the particle graph (same hole as InputAction /
// InputMappingContext / AnimatorStateMachine).
TEST_CASE("ContentManager pollHotReload reloads a changed particle graph")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());

	ParticleGraphAsset pg1;
	pg1.type          = HE::AssetType::ParticleSystem;
	pg1.name          = "hotgraph";
	pg1.path          = "hotgraph.hasset";
	pg1.nodeGraphJson = R"({"v":1})";
	REQUIRE(cm.saveAsset(pg1));
	const HE::UUID savedId = pg1.id;

	const HE::UUID loaded = cm.loadAsset("hotgraph.hasset");
	REQUIRE(loaded == savedId);
	REQUIRE(cm.getParticleGraph(loaded) != nullptr);
	CHECK(cm.getParticleGraph(loaded)->nodeGraphJson == R"({"v":1})");

	// Overwrite V2 — same path, same UUID, different graph.
	ParticleGraphAsset pg2 = *cm.getParticleGraph(loaded); // copies identity
	pg2.nodeGraphJson = R"({"v":2})";
	REQUIRE(cm.saveAsset(pg2));

	// Advance the stored mtime by 2 s so the poller detects same-second saves.
	const fs::path diskPath = dir.path / "hotgraph.hasset";
	auto cur = fs::last_write_time(diskPath);
	fs::last_write_time(diskPath, cur + std::chrono::seconds(2));

	auto changed = cm.pollHotReload();
	REQUIRE(changed.size() == 1);
	CHECK(changed[0] == savedId);

	const ParticleGraphAsset* reloaded = cm.getParticleGraph(savedId);
	REQUIRE(reloaded != nullptr);
	CHECK(reloaded->nodeGraphJson == R"({"v":2})");
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

	// Overwrite with garbage so sniffAssetTypeFromFile returns Unknown (mid-write).
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

// ─── Engine content root ("Engine/" reserved path prefix) ─────────────────────
// The Content Browser's new "Engine" tree (EditorDeps/EngineContent) is a
// SECOND filesystem root, addressed via the reserved "Engine/" path prefix —
// mirrors Unreal's /Engine/ vs /Game/. resolveAbsolutePath()/
// toContentRelativePath() are the one choke point deciding which root a path
// resolves against; these tests pin that behavior directly.

TEST_CASE("ContentManager resolveAbsolutePath routes \"Engine/\" to the engine root")
{
	TempContentDir dir;
	TempContentDir engineDir("he_test_engine_content");
	ContentManager cm(dir.path.string());
	cm.setEngineContentRoot(engineDir.path.string());

	CHECK(cm.resolveAbsolutePath("Materials/Foo.hasset") ==
	      dir.path.string() + "/Materials/Foo.hasset");
	CHECK(cm.resolveAbsolutePath("Engine/MaterialFunctions/Fresnel.hasset") ==
	      engineDir.path.string() + "/MaterialFunctions/Fresnel.hasset");
}

TEST_CASE("ContentManager resolveAbsolutePath falls back to content root when no engine root is set")
{
	TempContentDir dir;
	ContentManager cm(dir.path.string());
	// Never called setEngineContentRoot() — a literal "Engine/" segment in a
	// project's own Content folder must not be swallowed by an empty root.
	CHECK(cm.resolveAbsolutePath("Engine/Foo.hasset") ==
	      dir.path.string() + "/Engine/Foo.hasset");
}

TEST_CASE("ContentManager toContentRelativePath round-trips both roots")
{
	TempContentDir dir;
	TempContentDir engineDir("he_test_engine_content");
	ContentManager cm(dir.path.string());
	cm.setEngineContentRoot(engineDir.path.string());

	CHECK(cm.toContentRelativePath(dir.path.string() + "/Materials/Foo.hasset") ==
	      "Materials/Foo.hasset");
	CHECK(cm.toContentRelativePath(engineDir.path.string() + "/MaterialFunctions/Fresnel.hasset") ==
	      "Engine/MaterialFunctions/Fresnel.hasset");
	// Outside both roots entirely → empty, not a garbage "../.." path.
	CHECK(cm.toContentRelativePath("/some/unrelated/path.png") == "");
}

TEST_CASE("ContentManager saveAsset on an \"Engine/\" path writes a project override, not the shared default")
{
	TempContentDir dir;
	TempContentDir engineDir("he_test_engine_content");
	fs::create_directories(engineDir.path / "MaterialFunctions");
	// The shared default this override shadows — same UUID, same path identity.
	HE::UUID defaultId;
	{
		HAsset::Writer w;
		std::vector<uint8_t> meta;
		defaultId = HE::UUID::generate();
		HAsset::Writer::appendPOD(meta, static_cast<uint16_t>(HE::AssetType::MaterialFunction));
		HAsset::Writer::appendPOD(meta, defaultId.hi);
		HAsset::Writer::appendPOD(meta, defaultId.lo);
		HAsset::Writer::appendString(meta, "FresnelRimLight");
		HAsset::Writer::appendString(meta, "Engine/MaterialFunctions/FresnelRimLight.hasset");
		w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
		REQUIRE(w.write((engineDir.path / "MaterialFunctions" / "FresnelRimLight.hasset").string(),
		                 static_cast<uint16_t>(HE::AssetType::MaterialFunction)));
	}

	HE::UUID savedId;
	{
		ContentManager cm(dir.path.string());
		cm.setEngineContentRoot(engineDir.path.string());

		HE::UUID loadedDefault = cm.loadAsset("Engine/MaterialFunctions/FresnelRimLight.hasset");
		REQUIRE(loadedDefault == defaultId); // loaded from the shared default (no override yet)

		MaterialFunctionAsset fn;
		fn.id   = loadedDefault; // "editing" the already-loaded default in place
		fn.type = HE::AssetType::MaterialFunction;
		fn.name = "FresnelRimLight";
		fn.path = "Engine/MaterialFunctions/FresnelRimLight.hasset";
		REQUIRE(cm.saveAsset(fn));
		savedId = fn.id;
	}

	// Same identity (UUID) as the default — but written to the PROJECT's own
	// Content/Engine override location, never touching the shared default file.
	CHECK(savedId == defaultId);
	CHECK(fs::exists(dir.path / "Engine" / "MaterialFunctions" / "FresnelRimLight.hasset"));

	// Fresh manager (simulates reopening the project) — loadAsset() now prefers
	// the override over the shared default it shadows.
	ContentManager cm2(dir.path.string());
	cm2.setEngineContentRoot(engineDir.path.string());
	HE::UUID loadedId = cm2.loadAsset("Engine/MaterialFunctions/FresnelRimLight.hasset");
	CHECK(loadedId == savedId);
	const MaterialFunctionAsset* fn = cm2.getMaterialFunction(loadedId);
	REQUIRE(fn != nullptr);
	CHECK(fn->name == "FresnelRimLight");
}

TEST_CASE("ContentManager HE_ENGINE_CONTENT_EDITABLE=1 saves \"Engine/\" paths straight to the shared default")
{
	ScopedEnv devMode("HE_ENGINE_CONTENT_EDITABLE", "1");
	REQUIRE(ContentManager::isEngineContentDevMode());

	TempContentDir dir;
	TempContentDir engineDir("he_test_engine_content");
	fs::create_directories(engineDir.path / "MaterialFunctions");
	ContentManager cm(dir.path.string());
	cm.setEngineContentRoot(engineDir.path.string());

	MaterialFunctionAsset fn;
	fn.type = HE::AssetType::MaterialFunction;
	fn.name = "NewDefault";
	fn.path = "Engine/MaterialFunctions/NewDefault.hasset";
	REQUIRE(cm.saveAsset(fn));

	CHECK(fs::exists(engineDir.path / "MaterialFunctions" / "NewDefault.hasset"));
	CHECK_FALSE(fs::exists(dir.path / "Engine" / "MaterialFunctions" / "NewDefault.hasset"));
}

TEST_CASE("GlobalState::refreshEngineFolder merges project overrides into the displayed Engine tree")
{
	TempContentDir dir;                                  // project's Content/ root
	TempContentDir engineDir("he_test_engine_content");   // shared engine defaults

	fs::create_directories(engineDir.path / "MaterialFunctions");
	fs::create_directories(engineDir.path / "Meshes");
	std::ofstream(engineDir.path / "MaterialFunctions" / "Fresnel.hasset") << "default-bytes";
	std::ofstream(engineDir.path / "Meshes" / "Cube.hasset") << "default-bytes";

	// A project-level override of Fresnel.hasset, plus an override-only file
	// with no matching default at all.
	fs::create_directories(dir.path / "Engine" / "MaterialFunctions");
	std::ofstream(dir.path / "Engine" / "MaterialFunctions" / "Fresnel.hasset") << "override-bytes";
	std::ofstream(dir.path / "Engine" / "MaterialFunctions" / "OnlyInProject.hasset") << "override-only";

	GlobalState& gs = GlobalState::getInstance();
	REQUIRE(gs.refreshEngineFolder(engineDir.path.string(), dir.path.string()));

	auto [engineFolder, lock] = gs.lockEngineFolder();
	const HE::Folder* matFns = nullptr;
	const HE::Folder* meshes = nullptr;
	for (const HE::Folder* f : engineFolder.subfolders)
	{
		if (f->name == "MaterialFunctions") matFns = f;
		if (f->name == "Meshes")            meshes = f;
	}
	REQUIRE(matFns != nullptr);
	REQUIRE(meshes != nullptr);

	// Fresnel.hasset's node now points at the OVERRIDE file, not the default.
	const HE::File* fresnel = nullptr;
	const HE::File* onlyInProject = nullptr;
	for (const HE::File* f : matFns->files)
	{
		if (f->name == "Fresnel.hasset")       fresnel = f;
		if (f->name == "OnlyInProject.hasset") onlyInProject = f;
	}
	REQUIRE(fresnel != nullptr);
	CHECK(fresnel->fullPath == (dir.path / "Engine" / "MaterialFunctions" / "Fresnel.hasset").string());
	REQUIRE(onlyInProject != nullptr); // override-only file still shows up

	// Cube.hasset has no override — untouched, still points at the default.
	REQUIRE(meshes->files.size() == 1);
	CHECK(meshes->files[0]->fullPath == (engineDir.path / "Meshes" / "Cube.hasset").string());
}

// The remote catalogue (EngineContent SFTP sync) is tree STATE, not a per-call
// argument. It used to be a defaulted third parameter of refreshEngineFolder,
// which meant every caller that did not know about the feature — the periodic
// content refresh, project load, every create/rename refresh — silently rebuilt
// the Engine tree without it, and the remote assets vanished from the Content
// Browser roughly a minute after startup. These cases pin that down.
TEST_CASE("GlobalState remote EngineContent assets survive an unrelated refreshEngineFolder")
{
	TempContentDir dir;
	TempContentDir engineDir("he_test_engine_content");
	fs::create_directories(engineDir.path / "Meshes");
	std::ofstream(engineDir.path / "Meshes" / "Cube.hasset") << "default-bytes";

	GlobalState& gs = GlobalState::getInstance();

	// A remote-only asset in a folder that exists locally, and one in a folder
	// that does not exist locally at all.
	HE::RemoteEngineAsset remoteMesh;
	remoteMesh.relativePath = "Meshes/Sphere.hasset";
	remoteMesh.uuid         = HE::UUID{ 1, 2 };
	HE::RemoteEngineAsset remoteAudio;
	remoteAudio.relativePath = "Audio/Ambience/Rain.wav";
	remoteAudio.uuid         = HE::UUID{};   // raw file: no .hasset, hence no UUID
	gs.setEngineRemoteAssets({ remoteMesh, remoteAudio });

	auto findFile = [](const HE::Folder& root, const std::vector<std::string>& segments,
	                    const std::string& leaf) -> const HE::File*
	{
		const HE::Folder* cur = &root;
		for (const std::string& seg : segments)
		{
			const HE::Folder* next = nullptr;
			for (const HE::Folder* s : cur->subfolders)
				if (s->name == seg) { next = s; break; }
			if (!next) return nullptr;
			cur = next;
		}
		for (const HE::File* f : cur->files)
			if (f->name == leaf) return f;
		return nullptr;
	};

	SUBCASE("a refresh that knows nothing about the feature keeps the catalogue")
	{
		REQUIRE(gs.refreshEngineFolder(engineDir.path.string(), dir.path.string()));
		{
			auto [tree, lock] = gs.lockEngineFolder();
			const HE::File* sphere = findFile(tree, { "Meshes" }, "Sphere.hasset");
			REQUIRE(sphere != nullptr);
			CHECK(sphere->isRemoteOnly);
			CHECK(sphere->remoteUuid == HE::UUID{ 1, 2 });
			// A raw file in a folder that exists only remotely still appears.
			const HE::File* rain = findFile(tree, { "Audio", "Ambience" }, "Rain.wav");
			REQUIRE(rain != nullptr);
			CHECK(rain->isRemoteOnly);
		}

		// The exact call the periodic refresh makes — same two arguments. Before
		// the fix this wiped both entries.
		REQUIRE(gs.refreshEngineFolder(engineDir.path.string(), dir.path.string()));
		auto [tree, lock] = gs.lockEngineFolder();
		CHECK(findFile(tree, { "Meshes" }, "Sphere.hasset") != nullptr);
		CHECK(findFile(tree, { "Audio", "Ambience" }, "Rain.wav") != nullptr);
	}

	SUBCASE("an already-downloaded asset is a normal node, not a remote placeholder")
	{
		// Simulate a completed download landing in the shared cache.
		const fs::path cached = GlobalState::engineContentCacheDir() / "Meshes" / "Sphere.hasset";
		fs::create_directories(cached.parent_path());
		std::ofstream(cached) << "downloaded-bytes";

		REQUIRE(gs.refreshEngineFolder(engineDir.path.string(), dir.path.string()));
		{
			auto [tree, lock] = gs.lockEngineFolder();
			const HE::File* sphere = findFile(tree, { "Meshes" }, "Sphere.hasset");
			REQUIRE(sphere != nullptr);
			// Cached: no badge, and it points at the file actually on disk. Without
			// this the Content Browser would keep offering to download a file it
			// already has, on every single double-click.
			CHECK_FALSE(sphere->isRemoteOnly);
			CHECK(sphere->fullPath == cached.string());
		}
		std::error_code ec;
		fs::remove(cached, ec);
	}

	SUBCASE("a shipped default always wins over a remote entry of the same name")
	{
		HE::RemoteEngineAsset shadowing;
		shadowing.relativePath = "Meshes/Cube.hasset";   // exists locally already
		shadowing.uuid         = HE::UUID{ 9, 9 };
		gs.setEngineRemoteAssets({ shadowing });

		REQUIRE(gs.refreshEngineFolder(engineDir.path.string(), dir.path.string()));
		auto [tree, lock] = gs.lockEngineFolder();
		const HE::File* cube = findFile(tree, { "Meshes" }, "Cube.hasset");
		REQUIRE(cube != nullptr);
		CHECK_FALSE(cube->isRemoteOnly);
		CHECK(cube->fullPath == (engineDir.path / "Meshes" / "Cube.hasset").string());
	}

	// GlobalState is a singleton — leave no catalogue behind for later cases.
	gs.setEngineRemoteAssets({});
}

TEST_CASE("ContentManager isEngineDefaultPath / isEngineOverridePath classify absolute paths")
{
	TempContentDir dir;
	TempContentDir engineDir("he_test_engine_content");
	ContentManager cm(dir.path.string());
	cm.setEngineContentRoot(engineDir.path.string());

	const std::string defaultPath  = (engineDir.path / "MaterialFunctions" / "Fresnel.hasset").string();
	const std::string overridePath = (dir.path / "Engine" / "MaterialFunctions" / "Fresnel.hasset").string();
	const std::string unrelated    = (dir.path / "Materials" / "Foo.hasset").string();

	CHECK(cm.isEngineDefaultPath(defaultPath));
	CHECK_FALSE(cm.isEngineOverridePath(defaultPath));

	CHECK(cm.isEngineOverridePath(overridePath));
	CHECK_FALSE(cm.isEngineDefaultPath(overridePath));

	CHECK_FALSE(cm.isEngineDefaultPath(unrelated));
	CHECK_FALSE(cm.isEngineOverridePath(unrelated));
}

TEST_CASE("ContentManager scanContentDirectory indexes engine assets so UUID refs resolve")
{
	// Reproduces the "spawned sphere reloads as a cube" bug: a scene references
	// an engine mesh purely by UUID; on reload ensureResident(uuid) must find it
	// on disk. Before the engine root was scanned, only the default cube's UUID
	// resolved, so every engine mesh fell back to the cube.
	TempContentDir dir;
	TempContentDir engineDir("he_test_engine_content");
	fs::create_directories(engineDir.path / "Meshes");

	// A minimal engine-default StaticMesh, saved through the engine root.
	HE::UUID meshId;
	{
		ContentManager gen(engineDir.path.string());
		StaticMeshAsset mesh;
		mesh.type     = HE::AssetType::StaticMesh;
		mesh.name     = "Sphere";
		mesh.path     = "Meshes/Sphere.hasset"; // relative to the engine root here
		mesh.vertices = { 0,0,0, 1,0,0, 0,1,0 };
		mesh.indices  = { 0, 1, 2 };
		mesh.normals  = { 0,0,1, 0,0,1, 0,0,1 };
		REQUIRE(gen.saveAsset(mesh));
		meshId = mesh.id;
		REQUIRE_FALSE(meshId == HE::UUID{});
	}

	// A fresh manager pointed at the project (empty) + engine roots — exactly the
	// editor's setup after a restart, before any scene loads.
	ContentManager cm(dir.path.string());
	cm.setEngineContentRoot(engineDir.path.string());
	const size_t indexed = cm.scanContentDirectory();
	CHECK(indexed >= 1);

	// The UUID-only reference (what a MeshComponent stores) must now resolve from
	// disk, loading the real sphere — not fall through to nothing/the cube.
	CHECK(cm.ensureResident(meshId));
	const StaticMeshAsset* loaded = cm.getStaticMesh(meshId);
	REQUIRE(loaded != nullptr);
	CHECK(loaded->name == "Sphere");
	CHECK(loaded->indices.size() == 3);
}

TEST_CASE("Engine default mesh survives a scene save/reload round-trip")
{
	// End-to-end mirror of the editor flow that "loses" an engine primitive:
	//   session A: drag Engine/Meshes/Sphere.hasset in (loadAsset by the
	//              "Engine/"-prefixed content path) → MeshComponent UUID → save
	//   session B: fresh ContentManager (built-in defaults re-seeded) →
	//              scanContentDirectory() → the UUID must resolve back to the
	//              SAME engine mesh, not fall through to the default cube.
	TempContentDir dir;
	TempContentDir engineDir("he_test_engine_roundtrip");
	fs::create_directories(engineDir.path / "Meshes");

	// The shipped engine primitives are authored by mesh_gen with fixed UUIDs
	// (hi = 0x100 + index, lo = 1) — reproduce that shape, not a random UUID.
	const HE::UUID kSphereId{ 0x0000000000000101ULL, 0x0000000000000001ULL };
	{
		ContentManager gen(engineDir.path.string());
		StaticMeshAsset mesh;
		mesh.type     = HE::AssetType::StaticMesh;
		mesh.id       = kSphereId;
		mesh.name     = "Sphere";
		mesh.path     = "Meshes/Sphere.hasset";
		mesh.vertices = { 0,0,0, 1,0,0, 0,1,0, 1,1,0 };
		mesh.indices  = { 0, 1, 2, 1, 3, 2 };
		mesh.normals  = { 0,0,1, 0,0,1, 0,0,1, 0,0,1 };
		REQUIRE(gen.saveAsset(mesh));
	}

	// ── Session A: reference it the way the content browser does ──────────────
	HE::UUID authored;
	{
		ContentManager cm(dir.path.string());
		cm.setEngineContentRoot(engineDir.path.string());
		cm.scanContentDirectory();
		// toContentRelativePath() of a file under the engine root yields this.
		authored = cm.loadAsset("Engine/Meshes/Sphere.hasset");
		REQUIRE_FALSE(authored == HE::UUID{});
		// The identity written into the scene must be the mesh's OWN UUID.
		CHECK(authored == kSphereId);
		CHECK_FALSE(authored == HE::kDefaultCubeMeshId);
		const StaticMeshAsset* a = cm.getStaticMesh(authored);
		REQUIRE(a != nullptr);
		CHECK(a->name == "Sphere");
	}

	// ── Session B: restart — only the UUID from the scene file survives ───────
	{
		ContentManager cm(dir.path.string());   // re-seeds the built-in defaults
		cm.setEngineContentRoot(engineDir.path.string());
		CHECK(cm.scanContentDirectory() >= 1);
		// Nothing is loaded yet — this is the state a scene load starts from.
		CHECK(cm.getStaticMesh(authored) == nullptr);
		// SceneSystems::preloadAssetRefs does exactly this per referenced UUID.
		REQUIRE(cm.ensureResident(authored));
		const StaticMeshAsset* b = cm.getStaticMesh(authored);
		REQUIRE(b != nullptr);
		CHECK(b->name == "Sphere");
		CHECK(b->indices.size() == 6);          // the real mesh, not the 36-index cube
	}
}

TEST_CASE("ContentManager prefab round-trip preserves the CBOR payload and the UUID")
{
	// "Save as Prefab" used to only registerPrefab() — nothing hit disk, so the
	// asset was gone at shutdown. This is the contract that made it real: the
	// blob comes back byte-for-byte (SceneSerializer::instantiatePrefab reads it
	// verbatim, so a single dropped byte spawns nothing) under the same UUID a
	// scene reference would have stored.
	TempContentDir dir;

	// Stand-in for a serializeSubtree blob — HE_Core never decodes it, and the
	// test must not depend on HE_Scene to prove the bytes survive. Deliberately
	// includes 0x00 so a length-losing string round-trip would truncate it.
	const std::vector<uint8_t> payload = { 0xA1, 0x00, 0x64, 0x74, 0x65, 0x73, 0x74, 0xFF, 0x00, 0x01 };

	HE::UUID savedId;
	{
		ContentManager cm(dir.path.string());
		PrefabAsset p;
		p.type = HE::AssetType::Prefab;
		p.name = "Turret";
		p.path = "Prefabs/Turret.hasset";
		p.data = payload;
		REQUIRE(cm.saveAsset(p));
		savedId = p.id;
		REQUIRE_FALSE(savedId == HE::UUID{});
	}

	// Fresh manager — simulates an engine restart
	{
		ContentManager cm(dir.path.string());
		const HE::UUID loadedId = cm.loadAsset("Prefabs/Turret.hasset");
		CHECK(loadedId == savedId);

		const PrefabAsset* p = cm.getPrefab(loadedId);
		REQUIRE(p != nullptr);
		CHECK(p->name == "Turret");
		CHECK(p->path == "Prefabs/Turret.hasset");
		CHECK(p->type == HE::AssetType::Prefab);
		CHECK(p->data == payload);
		CHECK(cm.assetType(loadedId) == HE::AssetType::Prefab);

		// Wrong-type lookups must not alias (the SlotHandle can be valid in
		// another map — lookupAsset's stored-id check is what stops it).
		CHECK(cm.getStaticMesh(loadedId) == nullptr);
		CHECK(cm.getWidget(loadedId) == nullptr);

		// Prefabs must evict like any other type, or hot reload (unload, then
		// reload) silently does nothing for them.
		CHECK(cm.unloadAsset(loadedId));
		CHECK_FALSE(cm.isLoaded(loadedId));
		CHECK(cm.getPrefab(loadedId) == nullptr);
		CHECK_FALSE(cm.isLoaded("Prefabs/Turret.hasset"));

		// …and come straight back from the same file, under the same identity.
		CHECK(cm.loadAsset("Prefabs/Turret.hasset") == savedId);
	}
}

TEST_CASE("ContentManager prefab with an empty payload still round-trips")
{
	// A subtree that serialized to nothing must not read back as "no prefab at
	// all": saveAsset writes the PFAB chunk unconditionally, so the asset still
	// registers and the Content Browser still shows it.
	TempContentDir dir;
	HE::UUID savedId;
	{
		ContentManager cm(dir.path.string());
		PrefabAsset p;
		p.type = HE::AssetType::Prefab;
		p.name = "Empty";
		p.path = "Prefabs/Empty.hasset";
		REQUIRE(cm.saveAsset(p));
		savedId = p.id;
	}
	{
		ContentManager cm(dir.path.string());
		const PrefabAsset* p = cm.getPrefab(cm.loadAsset("Prefabs/Empty.hasset"));
		REQUIRE(p != nullptr);
		CHECK(p->id == savedId);
		CHECK(p->data.empty());
	}
}

// ─── Import provenance (META's optional source tail) ─────────────────────────

TEST_CASE("Importer::writeAsset records the source file and it round-trips through META")
{
	TempContentDir dir;
	TempContentDir srcDir("he_test_import_src");
	const fs::path srcFile = srcDir.path / "Rock.png";
	{ std::ofstream f(srcFile, std::ios::binary); f << "not really a png"; }

	// Handed a path with a "./" hop in it: the asset compiler is driven from a
	// command line whose working directory nothing later remembers, so what gets
	// stored has to be the resolved absolute path, not the caller's spelling.
	const fs::path asGiven  = srcDir.path / "." / "Rock.png";
	const std::string want  = fs::weakly_canonical(srcFile).generic_string();

	TextureAsset tex;
	tex.type   = HE::AssetType::Texture;
	tex.name   = "Rock";
	tex.path   = "Textures/Rock.hasset";
	tex.width  = 1;
	tex.height = 1;
	REQUIRE(Importer::writeAsset(tex, dir.path, asGiven));
	CHECK(tex.sourcePath == want);

	// Readable without loading the asset — this is what a Reimport menu item asks
	// to decide whether it can offer itself at all.
	CHECK(Importer::sourceFileOf(dir.path / "Textures" / "Rock.hasset") == want);

	// …and it survives the trip through the .hasset into a loaded asset.
	ContentManager cm(dir.path.string());
	const TextureAsset* loaded = cm.getTexture(cm.loadAsset("Textures/Rock.hasset"));
	REQUIRE(loaded != nullptr);
	CHECK(loaded->sourcePath == want);
	CHECK(loaded->id == tex.id);
}

TEST_CASE("Re-saving an imported asset from an editor keeps its source path")
{
	// saveAsset rebuilds META from the in-memory asset, so a material imported
	// from a .hmat and then edited in the Material Editor would lose the source it
	// came from unless the load path carries the field back in.
	TempContentDir dir;
	TempContentDir srcDir("he_test_import_src2");
	const fs::path srcFile = srcDir.path / "Brick.hmat";
	{ std::ofstream f(srcFile); f << "{}"; }
	const std::string want = fs::weakly_canonical(srcFile).generic_string();

	MaterialAsset mat;
	mat.type       = HE::AssetType::Material;
	mat.name       = "Brick";
	mat.path       = "Materials/Brick.hasset";
	mat.shaderPath = "builtin/unlit";
	REQUIRE(Importer::writeAsset(mat, dir.path, srcFile));

	{
		ContentManager cm(dir.path.string());
		const HE::UUID id = cm.loadAsset("Materials/Brick.hasset");
		const MaterialAsset* m = cm.getMaterial(id);
		REQUIRE(m != nullptr);
		REQUIRE(m->sourcePath == want);

		MaterialAsset edited = *m;   // what an editor panel writes back
		edited.roughness = 0.25f;
		REQUIRE(cm.saveAsset(edited));
	}

	ContentManager fresh(dir.path.string());
	const MaterialAsset* m = fresh.getMaterial(fresh.loadAsset("Materials/Brick.hasset"));
	REQUIRE(m != nullptr);
	CHECK(m->roughness == doctest::Approx(0.25f));
	CHECK(m->sourcePath == want);
}

TEST_CASE("An asset written before META had a source tail still loads")
{
	// Exactly the bytes an older build wrote: type, uuid, name, path — and then
	// the chunk ends. Treating the missing tail as a parse failure would make
	// every .hasset in every existing project unloadable.
	TempContentDir dir;
	fs::create_directories(dir.path / "Textures");

	const HE::UUID oldId{0x5011, 0x5012};
	{
		std::vector<uint8_t> meta;
		HAsset::Writer::appendPOD(meta, static_cast<uint16_t>(HE::AssetType::Texture));
		HAsset::Writer::appendPOD(meta, oldId.hi);
		HAsset::Writer::appendPOD(meta, oldId.lo);
		HAsset::Writer::appendString(meta, "Legacy");
		HAsset::Writer::appendString(meta, "Textures/Legacy.hasset");
		// No source string appended — that is the whole point of this case.

		std::vector<uint8_t> txmi;
		HAsset::Writer::appendPOD(txmi, static_cast<uint32_t>(2));
		HAsset::Writer::appendPOD(txmi, static_cast<uint32_t>(2));
		HAsset::Writer::appendPOD(txmi, static_cast<uint32_t>(4));

		HAsset::Writer w;
		w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
		w.addChunk(HAsset::CHUNK_TXMI, txmi.data(), txmi.size());
		REQUIRE(w.write((dir.path / "Textures" / "Legacy.hasset").string(),
		                static_cast<uint16_t>(HE::AssetType::Texture)));
	}

	ContentManager cm(dir.path.string());
	const HE::UUID id = cm.loadAsset("Textures/Legacy.hasset");
	CHECK(id == oldId);                       // identity intact, not regenerated
	const TextureAsset* t = cm.getTexture(id);
	REQUIRE(t != nullptr);
	CHECK(t->name == "Legacy");
	CHECK(t->width == 2);
	CHECK(t->sourcePath.empty());             // "no recorded source", not garbage
	CHECK(Importer::sourceFileOf(dir.path / "Textures" / "Legacy.hasset").empty());
}

TEST_CASE("A rewrite with no source of its own does not erase the recorded one")
{
	// Sidecar writes go through writeAsset too (a mesh's generated material, an
	// animation clip). Letting them pass "no source" as "clear the source" would
	// blank out the provenance of an asset the user imported directly.
	TempContentDir dir;
	TempContentDir srcDir("he_test_import_src3");
	const fs::path srcFile = srcDir.path / "Hit.wav";
	{ std::ofstream f(srcFile, std::ios::binary); f << "RIFF"; }
	const std::string want = fs::weakly_canonical(srcFile).generic_string();

	AudioAsset first;
	first.type = HE::AssetType::Audio;
	first.name = "Hit";
	first.path = "Audio/Hit.hasset";
	REQUIRE(Importer::writeAsset(first, dir.path, srcFile));

	AudioAsset again;                       // a fresh struct, as an importer builds
	again.type = HE::AssetType::Audio;
	again.name = "Hit";
	again.path = "Audio/Hit.hasset";
	REQUIRE(Importer::writeAsset(again, dir.path));
	CHECK(again.sourcePath == want);
	CHECK(again.id == first.id);            // same recovery as the UUID
	CHECK(Importer::sourceFileOf(dir.path / "Audio" / "Hit.hasset") == want);
}

TEST_CASE("Importer::reimport refuses what it cannot re-run")
{
	TempContentDir dir;

	// Authored in the editor: no source recorded, so there is nothing to re-run.
	{
		ContentManager cm(dir.path.string());
		ScriptAsset s;
		s.type = HE::AssetType::Script;
		s.name = "Spawner";
		s.path = "Scripts/Spawner.hasset";
		REQUIRE(cm.saveAsset(s));
	}
	CHECK_FALSE(Importer::reimport(dir.path / "Scripts" / "Spawner.hasset", dir.path));

	// Imported on another machine: the recorded absolute path resolves to nothing
	// here. Answering false (and logging) beats importing silence.
	{
		TextureAsset t;
		t.type = HE::AssetType::Texture;
		t.name = "Gone";
		t.path = "Textures/Gone.hasset";
		REQUIRE(Importer::writeAsset(t, dir.path, dir.path / "nowhere" / "Gone.png"));
	}
	CHECK_FALSE(Importer::reimport(dir.path / "Textures" / "Gone.hasset", dir.path));

	// Outside the content root: following the "../.." this yields would write the
	// re-imported asset outside the project entirely.
	{
		TempContentDir outside("he_test_reimport_outside");
		TextureAsset t;
		t.type = HE::AssetType::Texture;
		t.name = "Stray";
		t.path = "Stray.hasset";
		REQUIRE(Importer::writeAsset(t, outside.path, dir.path / "nowhere" / "Stray.png"));
		CHECK_FALSE(Importer::reimport(outside.path / "Stray.hasset", dir.path));
	}
}

// ─── Reimport lands on the asset that was clicked ────────────────────────────

namespace
{
	// A real RIFF/WAVE file: 44-byte header plus `frames` mono int16 samples. Hand
	// written rather than fixtured because the point of these cases is that the
	// SOURCE changes between the import and the re-import, which needs a decodable
	// file both times — and a WAV has no checksums to keep in step.
	bool writeWav(const fs::path& file, uint32_t frames, uint32_t sampleRate = 8000)
	{
		const uint16_t channels = 1, bits = 16;
		const uint32_t dataSize = frames * channels * (bits / 8);

		std::vector<uint8_t> buf;
		auto put32 = [&buf](uint32_t v) {
			buf.push_back(static_cast<uint8_t>(v));         buf.push_back(static_cast<uint8_t>(v >> 8));
			buf.push_back(static_cast<uint8_t>(v >> 16));   buf.push_back(static_cast<uint8_t>(v >> 24));
		};
		auto put16 = [&buf](uint16_t v) {
			buf.push_back(static_cast<uint8_t>(v));         buf.push_back(static_cast<uint8_t>(v >> 8));
		};
		auto putTag = [&buf](const char* t) { for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>(t[i])); };

		putTag("RIFF"); put32(36 + dataSize); putTag("WAVE");
		putTag("fmt "); put32(16); put16(1 /*PCM*/); put16(channels);
		put32(sampleRate); put32(sampleRate * channels * (bits / 8));
		put16(static_cast<uint16_t>(channels * (bits / 8))); put16(bits);
		putTag("data"); put32(dataSize);
		for (uint32_t i = 0; i < frames; ++i)
			put16(static_cast<uint16_t>(static_cast<int16_t>(i * 100)));

		std::error_code ec;
		fs::create_directories(file.parent_path(), ec);
		std::ofstream f(file, std::ios::binary | std::ios::trunc);
		if (!f) return false;
		f.write(reinterpret_cast<const char*>(buf.data()),
		        static_cast<std::streamsize>(buf.size()));
		return static_cast<bool>(f);
	}

	// Every file name directly in `dir`, sorted — the "did a second asset appear?"
	// probe. Comparing the whole listing catches a duplicate under ANY name, which
	// checking one expected path would not.
	std::vector<std::string> fileNamesIn(const fs::path& dir)
	{
		std::vector<std::string> names;
		std::error_code ec;
		for (const auto& e : fs::directory_iterator(dir, ec))
			names.push_back(e.path().filename().string());
		std::sort(names.begin(), names.end());
		return names;
	}

	// What the Content Browser's Rename does, both halves: move the file, then
	// carry every stored reference to it over to the new path. Renaming without
	// the retarget is a state the editor never leaves a project in, and testing
	// against it would test the wrong thing.
	void renameAssetLikeTheEditor(const fs::path& contentRoot,
	                              const std::string& oldRel, const std::string& newRel)
	{
		std::error_code ec;
		fs::rename(contentRoot / oldRel, contentRoot / newRel, ec);
		REQUIRE_FALSE(ec);
		HE::AssetRefs::retargetTree(
			contentRoot.string(),
			HE::AssetRefs::moveRules(oldRel, newRel, /*folder=*/false, "Content"));
	}
}

TEST_CASE("Importer::reimport writes the asset that was renamed, not one named after the source")
{
	// The bug this pins down: every importer derives its output name from the
	// SOURCE stem, and writeAsset only recovers a uuid when that path already holds
	// an asset. So re-importing an asset that had been renamed since wrote a SECOND
	// .hasset, under the source's name and with a fresh uuid, left the one every
	// scene references untouched — and still answered true, so nothing was logged
	// and the user saw a Reimport that had silently done nothing.
	TempContentDir dir;
	TempContentDir srcDir("he_test_reimport_src");
	const fs::path srcFile = srcDir.path / "hit.wav";
	REQUIRE(writeWav(srcFile, /*frames=*/4));

	REQUIRE(Importer::importSource(srcFile, dir.path, "Audio"));
	const fs::path imported = dir.path / "Audio" / "hit.hasset";
	REQUIRE(fs::exists(imported));
	const HE::UUID originalId = HE::AssetRefs::assetUuidOfFile(imported.string());
	REQUIRE(originalId != HE::UUID{});

	// The user renames it in the Content Browser: references retarget, uuid stays.
	renameAssetLikeTheEditor(dir.path, "Audio/hit.hasset", "Audio/Impact.hasset");
	const fs::path renamed = dir.path / "Audio" / "Impact.hasset";
	REQUIRE(fs::exists(renamed));
	const std::vector<std::string> before = fileNamesIn(dir.path / "Audio");

	// …then edits the source and hits Reimport on THAT asset.
	REQUIRE(writeWav(srcFile, /*frames=*/12));
	REQUIRE(Importer::reimport(renamed, dir.path));

	// The clicked file carries the new audio, under the identity it already had.
	CHECK(HE::AssetRefs::assetUuidOfFile(renamed.string()) == originalId);
	{
		ContentManager cm(dir.path.string());
		const AudioAsset* a = cm.getAudio(cm.loadAsset("Audio/Impact.hasset"));
		REQUIRE(a != nullptr);
		CHECK(a->audioData.size() == 12 * sizeof(int16_t));
		// The name follows the FILE — writing "hit" into a file called Impact is
		// how the asset ends up named one thing and displayed as another.
		CHECK(a->name == "Impact");
		CHECK(a->sourcePath == fs::weakly_canonical(srcFile).generic_string());
	}

	// And nothing else appeared: no second asset, under any name.
	CHECK(fileNamesIn(dir.path / "Audio") == before);
	CHECK_FALSE(fs::exists(dir.path / "Audio" / "hit.hasset"));
}

TEST_CASE("Importer::reimport of an asset that was never renamed still updates it in place")
{
	TempContentDir dir;
	TempContentDir srcDir("he_test_reimport_src2");
	const fs::path srcFile = srcDir.path / "hit.wav";
	REQUIRE(writeWav(srcFile, /*frames=*/4));

	REQUIRE(Importer::importSource(srcFile, dir.path, "Audio"));
	const fs::path imported = dir.path / "Audio" / "hit.hasset";
	const HE::UUID originalId = HE::AssetRefs::assetUuidOfFile(imported.string());
	REQUIRE(originalId != HE::UUID{});
	const std::vector<std::string> before = fileNamesIn(dir.path / "Audio");

	REQUIRE(writeWav(srcFile, /*frames=*/20));
	REQUIRE(Importer::reimport(imported, dir.path));

	CHECK(HE::AssetRefs::assetUuidOfFile(imported.string()) == originalId);
	ContentManager cm(dir.path.string());
	const AudioAsset* a = cm.getAudio(cm.loadAsset("Audio/hit.hasset"));
	REQUIRE(a != nullptr);
	CHECK(a->audioData.size() == 20 * sizeof(int16_t));
	CHECK(a->name == "hit");
	CHECK(fileNamesIn(dir.path / "Audio") == before);
}

TEST_CASE("Importer::reimport of a renamed mesh lands on its renamed sidecars too")
{
	// A mesh import writes three assets, all named after the source: the mesh, the
	// material it references and that material's base-colour texture. Only the mesh
	// can be redirected from the file that was clicked — the other two are read back
	// off the mesh's own MREF → MTRL chain, which the rename retarget keeps current.
	TempContentDir dir;
	TempContentDir srcDir("he_test_reimport_mesh_src");

	// A 1x1 red PNG (truecolor, 8 bit) — the smallest image stb_image decodes, so
	// the glTF below can reference a real base-colour texture on disk.
	static const uint8_t kPng1x1[] = {
		0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
		0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
		0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
		0x0C, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
		0x00, 0x03, 0x01, 0x01, 0x00, 0xF7, 0x03, 0x41, 0x43, 0x00, 0x00, 0x00,
		0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82
	};
	{
		std::ofstream png(srcDir.path / "tex1x1.png", std::ios::binary);
		REQUIRE(png);
		png.write(reinterpret_cast<const char*>(kPng1x1),
		          static_cast<std::streamsize>(sizeof(kPng1x1)));
	}
	// One triangle: POSITION + TEXCOORD_0 + indices, 66 bytes of buffer.
	{
		std::vector<uint8_t> buf(66, 0);
		const float pos[9] = { 0,0,0,  1,0,0,  0,1,0 };
		std::memcpy(buf.data() + 0, pos, sizeof(pos));
		const float uv[6] = { 0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f };
		std::memcpy(buf.data() + 36, uv, sizeof(uv));
		const uint16_t idx[3] = { 0, 1, 2 };
		std::memcpy(buf.data() + 60, idx, sizeof(idx));
		std::ofstream bin(srcDir.path / "uv.bin", std::ios::binary);
		REQUIRE(bin);
		bin.write(reinterpret_cast<const char*>(buf.data()),
		          static_cast<std::streamsize>(buf.size()));
	}
	{
		std::ofstream f(srcDir.path / "textured.gltf");
		REQUIRE(f);
		f << R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [0] } ],
  "nodes":  [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ {
      "attributes": { "POSITION": 0, "TEXCOORD_0": 1 },
      "indices": 2, "material": 0 } ] } ],
  "materials": [ { "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } } ],
  "textures":  [ { "source": 0 } ],
  "images":    [ { "uri": "tex1x1.png" } ],
  "buffers":    [ { "uri": "uv.bin", "byteLength": 66 } ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0,  "byteLength": 36 },
    { "buffer": 0, "byteOffset": 36, "byteLength": 24 },
    { "buffer": 0, "byteOffset": 60, "byteLength": 6 }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
      "min": [0,0,0], "max": [1,1,0] },
    { "bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2" },
    { "bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR" }
  ]
})";
	}

	const fs::path source = srcDir.path / "textured.gltf";
	REQUIRE(Importer::importSource(source, dir.path, "Meshes"));

	const fs::path meshFile = dir.path / "Meshes" / "textured.hasset";
	REQUIRE(fs::exists(meshFile));
	const auto sidecars = Importer::meshSidecarAssets(meshFile, dir.path);
	REQUIRE(sidecars.size() == 2);
	CHECK(sidecars[0] == "Meshes/textured_mat.hasset");
	CHECK(sidecars[1] == "Meshes/tex1x1.hasset");

	const HE::UUID meshId = HE::AssetRefs::assetUuidOfFile(meshFile.string());
	const HE::UUID matId  = HE::AssetRefs::assetUuidOfFile((dir.path / sidecars[0]).string());
	const HE::UUID texId  = HE::AssetRefs::assetUuidOfFile((dir.path / sidecars[1]).string());
	REQUIRE(meshId != HE::UUID{});
	REQUIRE(matId  != HE::UUID{});
	REQUIRE(texId  != HE::UUID{});

	// All three renamed — nothing in a project has to keep the name a DCC file
	// happened to have.
	renameAssetLikeTheEditor(dir.path, "Meshes/textured.hasset",     "Meshes/Rock.hasset");
	renameAssetLikeTheEditor(dir.path, "Meshes/textured_mat.hasset", "Meshes/RockMat.hasset");
	renameAssetLikeTheEditor(dir.path, "Meshes/tex1x1.hasset",       "Meshes/RockTex.hasset");

	const fs::path renamedMesh = dir.path / "Meshes" / "Rock.hasset";
	const std::vector<std::string> before = fileNamesIn(dir.path / "Meshes");

	// The artist moves a vertex and hits Reimport — something that has to show up
	// in the RENAMED file, not in a fresh textured.hasset next to it.
	{
		std::vector<uint8_t> buf(66, 0);
		const float pos[9] = { 0,0,0,  2,0,0,  0,1,0 };   // v1.x was 1
		std::memcpy(buf.data() + 0, pos, sizeof(pos));
		const float uv[6] = { 0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f };
		std::memcpy(buf.data() + 36, uv, sizeof(uv));
		const uint16_t idx[3] = { 0, 1, 2 };
		std::memcpy(buf.data() + 60, idx, sizeof(idx));
		std::ofstream bin(srcDir.path / "uv.bin", std::ios::binary | std::ios::trunc);
		REQUIRE(bin);
		bin.write(reinterpret_cast<const char*>(buf.data()),
		          static_cast<std::streamsize>(buf.size()));
	}
	REQUIRE(Importer::reimport(renamedMesh, dir.path));

	// Every one of the three kept its identity, and no fourth/fifth/sixth file
	// under the source's names appeared beside them.
	CHECK(HE::AssetRefs::assetUuidOfFile(renamedMesh.string()) == meshId);
	CHECK(HE::AssetRefs::assetUuidOfFile((dir.path / "Meshes" / "RockMat.hasset").string()) == matId);
	CHECK(HE::AssetRefs::assetUuidOfFile((dir.path / "Meshes" / "RockTex.hasset").string()) == texId);
	CHECK(fileNamesIn(dir.path / "Meshes") == before);

	// …and the mesh still points at the renamed pair rather than at the two files
	// a source-derived re-import would have written.
	const auto after = Importer::meshSidecarAssets(renamedMesh, dir.path);
	REQUIRE(after.size() == 2);
	CHECK(after[0] == "Meshes/RockMat.hasset");
	CHECK(after[1] == "Meshes/RockTex.hasset");

	ContentManager cm(dir.path.string());
	const StaticMeshAsset* m = cm.getStaticMesh(cm.loadAsset("Meshes/Rock.hasset"));
	REQUIRE(m != nullptr);
	CHECK(m->name == "Rock");
	CHECK(m->materialPath == "Meshes/RockMat.hasset");
	CHECK(m->vertices.size() == 9);
}

TEST_CASE("Importer::isImportableSource covers every extension the editor offers")
{
	// The menu's file-dialog filter, the Content Browser's right-click Import and
	// the asset compiler each used to carry their own copy of this list, and they
	// had drifted: fonts were importable from the browser but absent from the
	// dialog. This is the list they now share.
	for (const char* ext : { ".gltf", ".glb", ".png", ".jpg", ".jpeg", ".tga",
	                         ".bmp", ".hdr", ".wav", ".hmat", ".ttf", ".otf" })
		CHECK(Importer::isImportableSource(fs::path("Some/File") += ext));

	// Case is not part of the answer — Windows hands back "TEXTURE.PNG".
	CHECK(Importer::isImportableSource("Art/TEXTURE.PNG"));
	CHECK(Importer::isImportableSource("Art/Hero.GLB"));

	CHECK_FALSE(Importer::isImportableSource("Art/notes.txt"));
	CHECK_FALSE(Importer::isImportableSource("Art/Rock.hasset")); // already an asset
	CHECK_FALSE(Importer::isImportableSource("Art/Rock"));        // no extension at all
}

// ─── Reimport keeps what the import cannot rebuild ───────────────────────────

namespace
{
	// The three files a textured mesh import reads: a 1x1 PNG, the 66-byte vertex
	// buffer and the .gltf tying them together. `v1x` is the second vertex's x, so
	// "the artist moved a vertex and re-exported" is one more call; with
	// `withBaseColorTexture` false the SAME mesh is written without material,
	// texture and image — the re-export that loses its texture, which is what makes
	// an import produce no material at all.
	void writeGltfSource(const fs::path& dir, const std::string& stem,
	                     float v1x = 1.0f, bool withBaseColorTexture = true)
	{
		std::error_code ec;
		fs::create_directories(dir, ec);

		// 1x1 truecolor red — the smallest image stb_image decodes.
		static const uint8_t kPng1x1[] = {
			0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
			0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
			0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
			0x0C, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
			0x00, 0x03, 0x01, 0x01, 0x00, 0xF7, 0x03, 0x41, 0x43, 0x00, 0x00, 0x00,
			0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82
		};
		if (withBaseColorTexture)
		{
			std::ofstream png(dir / (stem + "_tex.png"), std::ios::binary | std::ios::trunc);
			REQUIRE(png);
			png.write(reinterpret_cast<const char*>(kPng1x1),
			          static_cast<std::streamsize>(sizeof(kPng1x1)));
		}
		{
			std::vector<uint8_t> buf(66, 0);
			const float pos[9] = { 0,0,0,  v1x,0,0,  0,1,0 };
			std::memcpy(buf.data() + 0, pos, sizeof(pos));
			const float uv[6] = { 0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f };
			std::memcpy(buf.data() + 36, uv, sizeof(uv));
			const uint16_t idx[3] = { 0, 1, 2 };
			std::memcpy(buf.data() + 60, idx, sizeof(idx));
			std::ofstream bin(dir / (stem + ".bin"), std::ios::binary | std::ios::trunc);
			REQUIRE(bin);
			bin.write(reinterpret_cast<const char*>(buf.data()),
			          static_cast<std::streamsize>(buf.size()));
		}
		{
			std::ofstream f(dir / (stem + ".gltf"), std::ios::trunc);
			REQUIRE(f);
			f << R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [0] } ],
  "nodes":  [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ {
      "attributes": { "POSITION": 0, "TEXCOORD_0": 1 },
      "indices": 2)" << (withBaseColorTexture ? ", \"material\": 0" : "") << " } ] } ],\n";
			if (withBaseColorTexture)
				f << "  \"materials\": [ { \"pbrMetallicRoughness\": "
				     "{ \"baseColorTexture\": { \"index\": 0 } } } ],\n"
				     "  \"textures\":  [ { \"source\": 0 } ],\n"
				     "  \"images\":    [ { \"uri\": \"" << stem << "_tex.png\" } ],\n";
			f << "  \"buffers\": [ { \"uri\": \"" << stem << ".bin\", \"byteLength\": 66 } ],"
			  << R"(
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0,  "byteLength": 36 },
    { "buffer": 0, "byteOffset": 36, "byteLength": 24 },
    { "buffer": 0, "byteOffset": 60, "byteLength": 6 }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
      "min": [0,0,0], "max": [1,1,0] },
    { "bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2" },
    { "bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR" }
  ]
})";
		}
	}

	// The whole file as bytes — the only probe that answers "was this rewritten?"
	// rather than "does it still parse the same": a rewrite that happens to
	// reproduce the fields one thinks to compare passes every field-wise check.
	std::vector<uint8_t> fileBytes(const fs::path& file)
	{
		std::ifstream f(file, std::ios::binary | std::ios::ate);
		if (!f) return {};
		const std::streamoff size = f.tellg();
		f.seekg(0, std::ios::beg);
		std::vector<uint8_t> bytes(static_cast<size_t>(size));
		if (size > 0)
			f.read(reinterpret_cast<char*>(bytes.data()),
			       static_cast<std::streamsize>(size));
		return f ? bytes : std::vector<uint8_t>{};
	}
}

TEST_CASE("Importer::reimport leaves an authored material sidecar alone")
{
	// The generated sidecar is where an artist STARTS a material: they open it in the
	// Material Editor and give it a node graph, params, a parent. A re-import of the
	// mesh then rewrote that file with a freshly built MaterialAsset — shader path,
	// one texture, defaults for everything else — and looked healthy doing it:
	// writeAsset keeps the file's uuid, so nothing dangles, and the editor never
	// reloads an already resident material, so the viewport kept rendering the
	// authored graph from memory. The loss only surfaced on the next project load.
	TempContentDir dir;
	TempContentDir srcDir("he_test_reimport_authored_src");
	writeGltfSource(srcDir.path, "prop");

	const fs::path source = srcDir.path / "prop.gltf";
	REQUIRE(Importer::importSource(source, dir.path, "Meshes"));

	const fs::path meshFile = dir.path / "Meshes" / "prop.hasset";
	const fs::path matFile  = dir.path / "Meshes" / "prop_mat.hasset";
	REQUIRE(fs::exists(meshFile));
	REQUIRE(fs::exists(matFile));

	// The material this instance points at — a real file, so the load below takes
	// the same instance path the editor does instead of a missing-parent shortcut.
	{
		MaterialAsset master;
		master.type = HE::AssetType::Material;
		master.name = "Master";
		master.path = "Meshes/Master.hasset";
		master.shaderPath = "builtin/unlit";
		REQUIRE(Importer::writeAsset(master, dir.path));
	}

	// …and now the authoring itself, over the sidecar the import wrote.
	HE::UUID matId;
	{
		MaterialAsset mat;
		mat.type       = HE::AssetType::Material;
		mat.name       = "prop_mat";
		mat.path       = "Meshes/prop_mat.hasset";
		mat.shaderPath = "builtin/unlit";
		mat.texturePaths.push_back("Meshes/prop_tex.hasset");
		mat.nodeGraphJson      = R"({"nodes":[{"id":1,"type":"Output"}]})";
		mat.parentMaterialPath = "Meshes/Master.hasset";
		mat.instanceOverriddenParams = { "Tint" };
		mat.shaderParamData = { 0.25f, 0.5f, 0.75f, 1.0f };
		mat.graphParamNames = { "Tint" };
		mat.graphParamTypes = { static_cast<uint8_t>(HE::MatParamKind::Color) };
		mat.graphParamGroups = { "Surface" };
		mat.blendMode = 2;                 // translucent
		mat.roughness = 0.125f;
		// No id of its own: writeAsset recovers the sidecar's, exactly as the editor's
		// save does — so this is the same file, not a replacement of it.
		REQUIRE(Importer::writeAsset(mat, dir.path));
		matId = mat.id;
	}
	REQUIRE(matId != HE::UUID{});
	const std::vector<uint8_t> authored = fileBytes(matFile);
	REQUIRE_FALSE(authored.empty());

	// The artist moves a vertex in the DCC and hits Reimport on the mesh.
	writeGltfSource(srcDir.path, "prop", /*v1x=*/2.0f);
	REQUIRE(Importer::reimport(meshFile, dir.path));

	// Byte for byte the same file: the material was not rewritten at all.
	CHECK(fileBytes(matFile) == authored);

	ContentManager cm(dir.path.string());
	const MaterialAsset* m = cm.getMaterial(cm.loadAsset("Meshes/prop_mat.hasset"));
	REQUIRE(m != nullptr);
	CHECK(m->id == matId);
	CHECK(m->nodeGraphJson == R"({"nodes":[{"id":1,"type":"Output"}]})");
	CHECK(m->parentMaterialPath == "Meshes/Master.hasset");   // still an INSTANCE
	// Only the two fields load reads back verbatim are checked here: an instance's
	// param table is re-derived from its parent on every load (syncMaterialInstance),
	// so a loaded value would say nothing about what is in the file. The byte compare
	// above is the assertion of record for everything else the editor authored.

	// …while the geometry edit the re-import was FOR did land, and the mesh still
	// names the sidecar.
	const StaticMeshAsset* mesh = cm.getStaticMesh(cm.loadAsset("Meshes/prop.hasset"));
	REQUIRE(mesh != nullptr);
	CHECK(mesh->materialPath == "Meshes/prop_mat.hasset");
	REQUIRE(mesh->vertices.size() == 9);
	CHECK(mesh->vertices[3] == doctest::Approx(2.0f));        // v1.x was 1
}

TEST_CASE("Importer::reimport without a resolvable material keeps the mesh's material link")
{
	// saveAsset writes chunk MREF unconditionally out of the freshly built mesh, so
	// an import that produces no material blanked the reference every scene resolves
	// its material through — and meshSidecarAssets then found nothing, so the next
	// re-import had nothing left to redirect either: the link was gone for good.
	TempContentDir dir;
	TempContentDir srcDir("he_test_reimport_nomat_src");
	writeGltfSource(srcDir.path, "crate");

	const fs::path source   = srcDir.path / "crate.gltf";
	const fs::path meshFile = dir.path / "Meshes" / "crate.hasset";
	REQUIRE(Importer::importSource(source, dir.path, "Meshes"));
	{
		const auto sidecars = Importer::meshSidecarAssets(meshFile, dir.path);
		REQUIRE(sidecars.size() == 2);
		REQUIRE(sidecars[0] == "Meshes/crate_mat.hasset");
	}

	// The base-colour image is gone from next to the .gltf — the usual cause is a
	// DCC that wrote an absolute texture path only its own machine ever resolved.
	// The glTF itself still parses, so the re-import runs; it just cannot produce a
	// material this time.
	std::error_code ec;
	fs::remove(srcDir.path / "crate_tex.png", ec);
	REQUIRE_FALSE(ec);
	REQUIRE(Importer::reimport(meshFile, dir.path));

	{
		ContentManager cm(dir.path.string());
		const StaticMeshAsset* mesh = cm.getStaticMesh(cm.loadAsset("Meshes/crate.hasset"));
		REQUIRE(mesh != nullptr);
		CHECK(mesh->materialPath == "Meshes/crate_mat.hasset");
	}
	// Still findable off the mesh, which is what lets the NEXT re-import redirect
	// onto it rather than writing a second material beside it.
	const auto after = Importer::meshSidecarAssets(meshFile, dir.path);
	REQUIRE_FALSE(after.empty());
	CHECK(after[0] == "Meshes/crate_mat.hasset");

	// The other way to end up here: the source is re-exported without its material
	// at all, so there is not even a texture to look for.
	writeGltfSource(srcDir.path, "crate", /*v1x=*/3.0f, /*withBaseColorTexture=*/false);
	REQUIRE(Importer::reimport(meshFile, dir.path));

	ContentManager cm(dir.path.string());
	const StaticMeshAsset* mesh = cm.getStaticMesh(cm.loadAsset("Meshes/crate.hasset"));
	REQUIRE(mesh != nullptr);
	CHECK(mesh->materialPath == "Meshes/crate_mat.hasset");
	REQUIRE(mesh->vertices.size() == 9);
	CHECK(mesh->vertices[3] == doctest::Approx(3.0f));   // the re-import did run
}
