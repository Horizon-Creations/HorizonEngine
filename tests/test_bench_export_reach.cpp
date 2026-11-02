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
