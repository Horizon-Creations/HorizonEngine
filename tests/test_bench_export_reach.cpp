// Two real exports of a project, with and without "pack only referenced assets", for the
// size and the picture (Thema 228). A measurement, not a test: it does nothing (and passes)
// unless HE_REACH_PROJECT is set.
//
//   HE_REACH_PROJECT=<project root: the folder with the .heproj and Content/>
//   HE_REACH_RUNTIME=<a game export folder to take the executable + engine libraries from>
//   HE_REACH_OUT=<folder for the two exports: <out>/all and <out>/reached>
//   HE_REACH_ENGINE=<EditorDeps/EngineContent>   HE_REACH_CACHE=<EngineContentCache>
//   HE_REACH_SHADERS=<backend bitmask, 1u << RendererBackend>  bake the node-graph material
//                     shaders into the pak the way the editor's export does (the cross-compiler
//                     lives here, not in HE_Core); unset = none, the game cross-compiles at load
//
// Each export is a runnable game folder (project.hcfg, the pak, the runtime). Run both with
// HE_HIDDEN_WINDOW=1 HE_EXIT_AFTER_FRAMES=N HE_CAPTURE_FRAME=M HE_CAPTURE_PATH=x.ppm and compare
// the logs and the frames: that is the proof nothing the game needs was left out.

#include "doctest.h"
#include <Hpak/ProjectExporter.h>
#include <Hpak/HpakReader.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <MaterialGraph/MaterialGraph.h>
#include <material/MaterialShaderBake.h>
#include <material/MaterialShaderLibrary.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

TEST_CASE("bench: export a project with and without the reachability filter (only with HE_REACH_PROJECT)")
{
	namespace fs = std::filesystem;
	const char* proj = std::getenv("HE_REACH_PROJECT");
	if (!proj || !*proj) return;
	const char* rt  = std::getenv("HE_REACH_RUNTIME");
	const char* out = std::getenv("HE_REACH_OUT");
	const char* eng = std::getenv("HE_REACH_ENGINE");
	const char* cch = std::getenv("HE_REACH_CACHE");
	REQUIRE(out);

	const fs::path root(proj), content = root / "Content";
	std::string startupRel, projectName = "Game";
	for (const auto& e : fs::directory_iterator(root))
		if (e.path().extension() == ".heproj")
		{
			std::ifstream f(e.path());
			const auto j = nlohmann::json::parse(f, nullptr, false);
			if (j.is_object() && j.contains("startupScene")) startupRel = j["startupScene"].get<std::string>();
			projectName = e.path().stem().string();
		}
	REQUIRE_FALSE(startupRel.empty());

	std::vector<uint8_t> startup;
	std::vector<std::pair<std::string, std::vector<uint8_t>>> scenes;
	for (const auto& e : fs::recursive_directory_iterator(content))
	{
		if (!e.is_regular_file() || e.path().extension() != ".hescene") continue;
		HorizonWorld w;
		SceneSerializer ser;
		std::vector<uint8_t> bytes;
		REQUIRE(ser.load(w, e.path(), SerializeFormat::JSON));
		REQUIRE(ser.saveToMemory(w, bytes));
		const std::string rel = e.path().lexically_relative(root).generic_string();
		if (rel == startupRel) startup = bytes;
		scenes.emplace_back(rel, std::move(bytes));
	}
	REQUIRE_FALSE(startup.empty());

	const char* shaders = std::getenv("HE_REACH_SHADERS");
	const uint32_t shaderMask = shaders && *shaders ? static_cast<uint32_t>(std::strtoul(shaders, nullptr, 10)) : 0u;
	// The editor's CompileMaterialShaderVariants (ExportDialogPanel.cpp), minus its logging.
	auto bake = [](const std::string& frag, const std::string& vert, const std::string& gbuf, uint32_t backends)
	{
		std::vector<uint8_t> none;
		if (frag.empty() || backends == 0) return none;
		HE::MaterialShaderLibrary lib;
		std::vector<MaterialShaderVariant> variants;
		for (uint8_t v = 0; v <= static_cast<uint8_t>(HE::RendererBackend::Metal); ++v)
		{
			if ((backends & (1u << v)) == 0) continue;
			MaterialShaderVariant var; std::string error; std::vector<std::string> warnings;
			if (!HE::bakeMaterialShaderVariant(lib, static_cast<HE::RendererBackend>(v), frag, vert, var, error,
			                                   warnings, gbuf))
			{
				std::printf("REACH shader bake failed (backend %d): %s\n", int(v), error.c_str());
				continue;
			}
			for (const auto& w : warnings) std::printf("REACH shader bake warning: %s\n", w.c_str());
			variants.push_back(std::move(var));
		}
		return variants.empty() ? none : HE::encodeMaterialShaderVariants(variants);
	};

	for (const bool reached : { false, true })
	{
		ExportSettings s;
		s.compress = false;                       // Store: the entries stay readable
		s.textureCompression = 1;                 // ASTC, as the editor picks for Metal
		// HE_REACH_TEXCOMP=3 (BC3) for a game that runs on macOS OpenGL, which samples no ASTC.
		if (const char* tc = std::getenv("HE_REACH_TEXCOMP")) s.textureCompression = static_cast<uint8_t>(std::atoi(tc));
		s.cookTextureArrays = true;
		s.bakeMeshTextures = std::getenv("HE_REACH_NOBAKE") == nullptr;   // the mesh texture bake, on unless asked
		s.onlyReferenced = reached;
		if (std::getenv("HE_REACH_STEAMAUDIO")) s.audioBuses.spatialBackend = HE::AudioSpatialBackend::SteamAudio;
		if (rt && *rt) s.gameRuntimeDir = rt;
		if (eng && *eng) s.engineContentDir = eng;
		if (cch && *cch) s.engineContentCacheDir = cch;
		s.shaderBackends = shaderMask;
		if (shaderMask) s.compileShaderVariants = bake;
		s.onNotice = [](const std::string& m) { std::printf("REACH notice: %s\n", m.c_str()); };
		s.onWarning = [](const std::string& m) { std::printf("REACH warning: %s\n", m.c_str()); };
		const fs::path dst = fs::path(out) / (reached ? "reached" : "all");
		fs::remove_all(dst);
		const auto t0 = std::chrono::steady_clock::now();
		const auto res = ProjectExporter::exportProject(content, projectName, "", dst, s, startup, scenes);
		const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		REQUIRE(res.success);
		std::uintmax_t pakBytes = 0;
		for (const auto& e : fs::recursive_directory_iterator(dst))
			if (e.is_regular_file() && e.path().extension() == ".hpak") pakBytes = fs::file_size(e.path());
		std::printf("BENCH reach=%d assetsPacked=%d pakBytes=%ju seconds=%.1f dir=%s\n", reached ? 1 : 0,
		            res.assetsPacked, pakBytes, sec, dst.string().c_str());
	}
}

// A texture-heavy project for the RAM measurement of the post-upload CPU pixel release, for a
// machine without the foliage test project: HE_GEN_PROJECT=<empty folder> writes a project of
// HE_GEN_TEXTURES (default 20) noise textures of HE_GEN_SIZE (default 2048)^2, each on its own
// uber-shader material on its own cube, plus a camera. Point HE_REACH_PROJECT at it. Not a test.
TEST_CASE("bench: generate a texture-heavy project (only with HE_GEN_PROJECT)")
{
	namespace fs = std::filesystem;
	const char* dirEnv = std::getenv("HE_GEN_PROJECT");
	if (!dirEnv || !*dirEnv) return;
	const int count = std::getenv("HE_GEN_TEXTURES") ? std::atoi(std::getenv("HE_GEN_TEXTURES")) : 20;
	const int size  = std::getenv("HE_GEN_SIZE") ? std::atoi(std::getenv("HE_GEN_SIZE")) : 2048;
	const fs::path root(dirEnv), content = root / "Content";
	fs::create_directories(content);

	using nlohmann::json;
	auto uuidJ = [](const HE::UUID& id) { return json::array({ id.hi, id.lo }); };
	json ents = json::array(), kids = json::array();
	const json rootId = json::array({ 7700228, 1 });
	uint64_t next = 1000;
	auto entity = [&](const std::string& name, json comps)
	{
		const json id = json::array({ 7700228, ++next });
		kids.push_back(id);
		ents.push_back({ { "children", json::array() }, { "components", std::move(comps) }, { "name", name },
		                 { "parent", rootId }, { "uuid", id } });
	};
	{
		ContentManager cm(content.string());
		uint32_t rng = 12345;
		for (int i = 0; i < count; ++i)
		{
			TextureAsset t;
			t.type = HE::AssetType::Texture; t.name = "T" + std::to_string(i);
			t.path = "Textures/T" + std::to_string(i) + ".hasset";
			t.width = t.height = size; t.channels = 4; t.srgb = true;
			t.data.resize(static_cast<size_t>(size) * size * 4);
			for (int y = 0; y < size; ++y)
				for (int x = 0; x < size; ++x)
				{
					uint8_t* p = &t.data[(static_cast<size_t>(y) * size + x) * 4];
					rng = rng * 1664525u + 1013904223u;
					const int n = static_cast<int>(rng >> 29);   // 0..7 of noise on a gradient
					p[0] = static_cast<uint8_t>((x * 255 / size + i * 20 + n) & 255);
					p[1] = static_cast<uint8_t>((y * 255 / size + n) & 255);
					p[2] = static_cast<uint8_t>(((x ^ y) * 255 / size + n) & 255);
					p[3] = 255;
				}
			REQUIRE(cm.saveAsset(t));
			MaterialAsset m;
			m.type = HE::AssetType::Material; m.name = "M" + std::to_string(i);
			m.path = "Materials/M" + std::to_string(i) + ".hasset";
			m.texturePaths = { t.path };
			if (std::getenv("HE_GEN_GRAPH"))
			{
				// A node-graph material (a coloured Base Color), so the export bakes per-backend shader
				// variants for it and the game has to take them from the pak (HE_REACH_SHADERS).
				HE::MaterialGraph g = HE::MaterialGraph::makeDefault();
				int out = 0;
				for (auto& n : g.nodes) if (n.type == HE::MatNodeType::Output) out = n.id;
				const int col = g.addNode(HE::MatNodeType::ConstColor);
				g.findNode(col)->p[0] = 0.2f + 0.8f * static_cast<float>(i % 5) / 4.0f;
				g.findNode(col)->p[1] = 0.2f + 0.8f * static_cast<float>(i / 5) / 3.0f;
				g.findNode(col)->p[2] = 0.3f;
				REQUIRE(g.connect(col, 0, out, 0));
				m.nodeGraphJson = HE::materialGraphToJson(g);
				const HE::MatShaderGen gen = HE::generateFragment(g);
				m.customShaderFragGlsl = gen.glsl;
				m.customShaderGBufGlsl = gen.glslGBuffer;
				m.texturePaths.clear();
			}
			REQUIRE(cm.saveAsset(m));
			const float gx = static_cast<float>(i % 5) * 3.0f - 6.0f, gz = static_cast<float>(i / 5) * -3.0f;
			entity("Cube" + std::to_string(i), {
				{ "mesh", { { "asset", json::array({ 1, 1 }) }, { "castsShadow", true }, { "receivesShadow", true },
				            { "visible", true }, { "lodBias", 0 } } },
				{ "material", { { "asset", uuidJ(m.id) } } },
				{ "transform", { { "position", { gx, 1.0f, gz } }, { "rotation", { 0, 20, 0 } }, { "scale", { 2, 2, 2 } } } } });
		}
	}
	entity("MainCamera", { { "camera", { { "fovDegrees", 60.0 }, { "nearPlane", 0.1 }, { "farPlane", 200.0 },
	                                     { "isMain", true }, { "orthographic", false } } },
	                       { "transform", { { "position", { 0, 6, 12 } }, { "rotation", { -20, 0, 0 } }, { "scale", { 1, 1, 1 } } } } });
	entity("Sky", { { "environment", { { "timeOfDay", 0.38 }, { "sunIntensity", 2.2 }, { "dayNightCycle", false },
	                                   { "autoAdvance", false }, { "cloudCoverage", 0.3 } } } });
	ents.push_back({ { "children", kids }, { "name", "World" }, { "parent", nullptr }, { "uuid", rootId } });
	{
		std::ofstream f(content / "StartupScene.hescene", std::ios::trunc);
		f << json({ { "entities", ents }, { "version", "1.1" } }).dump(1);
	}
	{
		std::ofstream f(root / "TexHeavy.heproj", std::ios::trunc);
		f << json({ { "name", "TexHeavy" }, { "startupScene", "Content/StartupScene.hescene" } }).dump(1);
	}
	std::printf("GEN project: %d textures of %dx%d in %s\n", count, size, size, root.string().c_str());
}

// One mesh of two quads on two different simple textured materials (the case the exporter's mesh
// texture bake merges onto one material): HE_GEN_MESHBAKE_PROJECT=<empty folder>. The scene shows it
// head on. Export it with and without HE_REACH_NOBAKE=1 and compare the game's frames. Not a test.
TEST_CASE("bench: generate a two-material mesh project (only with HE_GEN_MESHBAKE_PROJECT)")
{
	namespace fs = std::filesystem;
	const char* dirEnv = std::getenv("HE_GEN_MESHBAKE_PROJECT");
	if (!dirEnv || !*dirEnv) return;
	const fs::path root(dirEnv), content = root / "Content";
	fs::create_directories(content);
	using nlohmann::json;
	const json rootId = json::array({ 7700229, 1 });
	json ents = json::array(), kids = json::array();
	uint64_t next = 1000;
	auto entity = [&](const std::string& name, json comps)
	{
		const json id = json::array({ 7700229, ++next });
		kids.push_back(id);
		ents.push_back({ { "children", json::array() }, { "components", std::move(comps) }, { "name", name },
		                 { "parent", rootId }, { "uuid", id } });
	};
	HE::UUID meshId;
	{
		ContentManager cm(content.string());
		const char* names[2] = { "A", "B" };
		std::vector<MaterialAsset> mats(2);
		for (int i = 0; i < 2; ++i)
		{
			TextureAsset t;
			t.type = HE::AssetType::Texture; t.name = names[i]; t.path = std::string("Tex/") + names[i] + ".hasset";
			t.width = t.height = 128; t.channels = 4; t.srgb = true; t.data.resize(128 * 128 * 4);
			for (int y = 0; y < 128; ++y)
				for (int x = 0; x < 128; ++x)
				{
					uint8_t* p = &t.data[(static_cast<size_t>(y) * 128 + x) * 4];
					const bool chk = ((x / 16) + (y / 16)) & 1;
					p[0] = static_cast<uint8_t>(i == 0 ? (chk ? 230 : 40) : (chk ? 30 : 90));
					p[1] = static_cast<uint8_t>(i == 0 ? (chk ? 120 : 30) : (chk ? 200 : 60));
					p[2] = static_cast<uint8_t>(i == 0 ? 40 : (chk ? 230 : 120));
					p[3] = 255;
				}
			REQUIRE(cm.saveAsset(t));
			MaterialAsset& m = mats[static_cast<size_t>(i)];
			m.type = HE::AssetType::Material; m.name = names[i]; m.path = std::string("Mats/") + names[i] + ".hasset";
			m.texturePaths = { t.path };
			REQUIRE(cm.saveAsset(m));
		}
		StaticMeshAsset mesh;
		mesh.type = HE::AssetType::StaticMesh; mesh.name = "Two"; mesh.path = "Meshes/Two.hasset";
		mesh.vertices = { 0,0,0,  1,0,0,  1,1,0,  0,1,0,   1.1f,0,0,  2.1f,0,0,  2.1f,1,0,  1.1f,1,0 };
		mesh.normals  = { 0,0,1,  0,0,1,  0,0,1,  0,0,1,   0,0,1,  0,0,1,  0,0,1,  0,0,1 };
		mesh.uvs      = { 0,0,  1,0,  1,1,  0,1,   0,0,  1,0,  1,1,  0,1 };
		mesh.indices  = { 0,1,2, 0,2,3,   4,5,6, 4,6,7 };
		mesh.materialPath = mats[0].path;
		mesh.sections = { { 0, 6, mats[0].path, {} }, { 6, 6, mats[1].path, {} } };
		REQUIRE(cm.saveAsset(mesh));
		meshId = mesh.id;
	}
	entity("Two", { { "mesh", { { "asset", json::array({ meshId.hi, meshId.lo }) }, { "castsShadow", true },
	                            { "receivesShadow", true }, { "visible", true }, { "lodBias", 0 } } },
	                { "transform", { { "position", { -2.0, 0.0, 0.0 } }, { "rotation", { 0, 0, 0 } }, { "scale", { 2, 2, 2 } } } } });
	entity("MainCamera", { { "camera", { { "fovDegrees", 60.0 }, { "nearPlane", 0.1 }, { "farPlane", 200.0 },
	                                     { "isMain", true }, { "orthographic", false } } },
	                       { "transform", { { "position", { 0, 1, 5 } }, { "rotation", { 0, 0, 0 } }, { "scale", { 1, 1, 1 } } } } });
	entity("Sky", { { "environment", { { "timeOfDay", 0.38 }, { "sunIntensity", 2.2 }, { "dayNightCycle", false },
	                                   { "autoAdvance", false }, { "cloudCoverage", 0.3 } } } });
	ents.push_back({ { "children", kids }, { "name", "World" }, { "parent", nullptr }, { "uuid", rootId } });
	{ std::ofstream f(content / "StartupScene.hescene", std::ios::trunc);
	  f << json({ { "entities", ents }, { "version", "1.1" } }).dump(1); }
	{ std::ofstream f(root / "MeshBake.heproj", std::ios::trunc);
	  f << json({ { "name", "MeshBake" }, { "startupScene", "Content/StartupScene.hescene" } }).dump(1); }
	std::printf("GEN mesh bake project in %s\n", root.string().c_str());
}
