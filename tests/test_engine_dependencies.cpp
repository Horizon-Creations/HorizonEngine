#include "doctest.h"
#include "TestFsUtil.h"
#include <ContentManager/EngineDependencies.h>
#include <ContentManager/HAsset.h>
#include <Types/Enums.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

// What a project needs from the EngineContent library, answered from the files alone,
// before the project is opened (EngineDependencies.h). A fake server stands in for the
// network: a catalogue plus a fetch that copies a staged file into the download cache.

namespace fs = std::filesystem;
using namespace HE::EngineDeps;

namespace
{
struct Dir
{
	fs::path path;
	explicit Dir(const char* name) { path = fs::temp_directory_path() / name; he_test::removeAllQuiet(path); fs::create_directories(path); }
	~Dir() { he_test::removeAllQuiet(path); }
};

void write(const fs::path& p, const std::string& bytes)
{
	fs::create_directories(p.parent_path());
	std::ofstream f(p, std::ios::binary);
	f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// A .hasset-shaped file: the 32-byte header (type picks whether it is read) + a payload.
std::string hasset(HE::AssetType type, const std::string& payload)
{
	HAsset::FileHeader h{};
	std::memcpy(h.magic, HAsset::k_magic, 4);
	h.version = HAsset::k_version;
	h.asset_type = static_cast<uint16_t>(type);
	std::string out(reinterpret_cast<const char*>(&h), sizeof(h));
	return out + payload;
}

// A string as the engine stores it: u32 length, then the characters.
std::string lp(const std::string& s)
{
	const uint32_t n = static_cast<uint32_t>(s.size());
	return std::string(reinterpret_cast<const char*>(&n), 4) + s;
}

struct Fixture
{
	Dir project{ "he_dep_project" }, engine{ "he_dep_engine" }, cache{ "he_dep_cache" }, server{ "he_dep_server" };
	Options opt;
	std::map<std::string, int> fetchCalls;
	std::vector<std::string>   failing;      // catalogue entries whose download fails

	Fixture()
	{
		write(project.path / "Game.heproj", "{}");
		opt.projectFile        = (project.path / "Game.heproj").string();
		opt.projectContentRoot = (project.path / "Content").string();
		opt.engineRoot         = engine.path.string();
		opt.cacheRoot          = cache.path.string();
		opt.catalogueKnown     = true;
		opt.fetch = [this](const std::string& path, std::function<void(bool)> done)
		{
			++fetchCalls[path];
			if (std::find(failing.begin(), failing.end(), path) != failing.end()) { done(false); return; }
			const fs::path from = server.path / path;
			if (!fs::exists(from)) { done(false); return; }
			fs::create_directories((cache.path / path).parent_path());
			fs::copy_file(from, cache.path / path, fs::copy_options::overwrite_existing);
			done(true);
		};
	}
	// Puts an asset on the "server" and into the catalogue.
	void serve(const std::string& path, const std::string& bytes, HE::UUID id = {})
	{
		write(server.path / path, bytes);
		opt.catalogue.push_back({ path, id });
	}
};
} // namespace

TEST_CASE("findEngineReferences: stored paths, however they are wrapped, and nothing else")
{
	// As a chunk stores them (length-prefixed), inside JSON, in a script, beside noise.
	const std::string data =
		lp("Engine/Textures/Rock.hasset") + std::string("\0\x01\x02", 3) +
		"{\"tex\":\"Engine/Textures/Landscape/T_Albedo_Array.hasset\",\"x\":1}" +
		" load('Engine/MaterialFunctions/Weather/MF_WeatherSnow.hasset') " +
		lp("Engine/Textures/Rock.hasset") +                 // twice: reported once
		lp("MyEngine/Textures/Nope.hasset") +                // not the library
		lp("Content/Engine/Meshes/Cube.hasset") +             // a project override's own path: "Engine/…" inside it
		lp("Engine/My Folder/Spaced Name.hasset") +           // a space is a legal path byte
		lp("Engine/Textures/NoExtension") +                   // no .hasset, not an asset path
		"Engine/";                                            // a bare prefix at the very end
	const auto refs = findEngineReferences(data.data(), data.size());
	const std::vector<std::string> want = {
		"Engine/Textures/Rock.hasset",
		"Engine/Textures/Landscape/T_Albedo_Array.hasset",
		"Engine/MaterialFunctions/Weather/MF_WeatherSnow.hasset",
		"Engine/Meshes/Cube.hasset",
		"Engine/My Folder/Spaced Name.hasset",
	};
	CHECK(refs == want);
	CHECK(findEngineReferences(nullptr, 0).empty());
	CHECK(findEngineReferences("Engine/a", 8).empty());
}

TEST_CASE("resolve: a project that needs nothing from the library is fine, and reads quickly")
{
	Fixture f;
	write(f.project.path / "Content" / "Scenes" / "Main.hescene", "{\"entities\":[]}");
	write(f.project.path / "Content" / "Mat.hasset", hasset(HE::AssetType::Material, lp("Textures/Mine.hasset")));
	const Result r = resolve(f.opt);
	CHECK(r.ok());
	CHECK(r.referenced == 0u);
	CHECK(r.filesScanned >= 3u);   // .heproj, the scene, the material
}

TEST_CASE("resolve: references are followed through the library and fetched on the way")
{
	Fixture f;
	// project material → Engine/Materials/Ground (server) → Engine/Textures/Rock (server) + a function (local)
	write(f.project.path / "Content" / "Mat.hasset",
	      hasset(HE::AssetType::Material, lp("Engine/Materials/Ground.hasset")));
	f.serve("Materials/Ground.hasset",
	        hasset(HE::AssetType::Material, lp("Engine/Textures/Rock.hasset") + lp("Engine/MaterialFunctions/Fn.hasset")));
	f.serve("Textures/Rock.hasset", hasset(HE::AssetType::Texture, lp("Engine/Textures/NotRead.hasset")));
	f.serve("MaterialFunctions/Fn.hasset", hasset(HE::AssetType::MaterialFunction, ""));
	// …already on this machine, so not asked for:
	write(f.engine.path / "MaterialFunctions" / "Fn.hasset", hasset(HE::AssetType::MaterialFunction, ""));

	const Result r = resolve(f.opt);
	CHECK(r.ok());
	CHECK(r.referenced == 3u);       // Ground, Rock, Fn — NotRead is inside a texture, which is not read
	CHECK(r.downloaded == 2u);       // Ground and Rock
	CHECK(r.alreadyHere == 1u);      // Fn
	CHECK(f.fetchCalls.count("Materials/Ground.hasset") == 1u);
	CHECK(f.fetchCalls.count("Textures/Rock.hasset") == 1u);
	CHECK(f.fetchCalls.count("MaterialFunctions/Fn.hasset") == 0u);
	CHECK(fs::exists(f.cache.path / "Textures" / "Rock.hasset"));
}

TEST_CASE("resolve: a project override and a cached copy both count as 'here'")
{
	Fixture f;
	write(f.project.path / "Content" / "Mat.hasset",
	      hasset(HE::AssetType::Material, lp("Engine/Textures/A.hasset") + lp("Engine/Textures/B.hasset")));
	write(f.project.path / "Content" / "Engine" / "Textures" / "A.hasset", hasset(HE::AssetType::Texture, ""));
	write(f.cache.path / "Textures" / "B.hasset", hasset(HE::AssetType::Texture, ""));
	const Result r = resolve(f.opt);
	CHECK(r.ok());
	CHECK(r.alreadyHere == 2u);
	CHECK(f.fetchCalls.empty());
}

TEST_CASE("resolve: a scene's asset ids are matched against the catalogue")
{
	Fixture f;
	write(f.project.path / "Content" / "Scenes" / "Main.hescene",
	      "{\"entities\":[{\"mesh\":[11,22],\"material\":{\"hi\":33,\"lo\":44},\"other\":[5,6]}]}");
	f.serve("Meshes/Cube.hasset", hasset(HE::AssetType::StaticMesh, ""), HE::UUID{ 11, 22 });
	f.serve("Materials/Grey.hasset", hasset(HE::AssetType::Material, ""), HE::UUID{ 33, 44 });
	f.serve("Meshes/Unused.hasset", hasset(HE::AssetType::StaticMesh, ""), HE::UUID{ 7, 8 });
	const Result r = resolve(f.opt);
	CHECK(r.ok());
	CHECK(r.referenced == 2u);      // [5,6] matches nothing, Unused is never asked for
	CHECK(f.fetchCalls.count("Meshes/Cube.hasset") == 1u);
	CHECK(f.fetchCalls.count("Materials/Grey.hasset") == 1u);
	CHECK(f.fetchCalls.count("Meshes/Unused.hasset") == 0u);
}

TEST_CASE("resolve: what cannot be had is reported with its reason and who asked for it")
{
	Fixture f;
	write(f.project.path / "Content" / "Mat.hasset",
	      hasset(HE::AssetType::Material,
	             lp("Engine/Textures/Gone.hasset") + lp("Engine/Textures/Broken.hasset") + lp("Engine/Textures/Fine.hasset")));
	f.serve("Textures/Broken.hasset", hasset(HE::AssetType::Texture, ""));
	f.failing.push_back("Textures/Broken.hasset");
	f.serve("Textures/Fine.hasset", hasset(HE::AssetType::Texture, ""));

	const Result r = resolve(f.opt);
	CHECK_FALSE(r.ok());
	REQUIRE(r.missing.size() == 2u);
	std::map<std::string, Missing> by;
	for (const Missing& m : r.missing) by[m.path] = m;
	CHECK(by.at("Engine/Textures/Gone.hasset").reason == MissingReason::NotOnServer);
	CHECK(by.at("Engine/Textures/Broken.hasset").reason == MissingReason::DownloadFailed);
	CHECK(by.at("Engine/Textures/Gone.hasset").neededBy == "Mat.hasset");
	CHECK(r.downloaded == 1u);       // Fine
}

TEST_CASE("resolve: without a catalogue or a way to download, the answer says so")
{
	Fixture f;
	write(f.project.path / "Content" / "Mat.hasset", hasset(HE::AssetType::Material, lp("Engine/Textures/A.hasset")));
	f.opt.catalogueKnown = false;
	{
		const Result r = resolve(f.opt);
		REQUIRE(r.missing.size() == 1u);
		CHECK(r.missing[0].reason == MissingReason::NoCatalogue);
	}
	f.opt.catalogueKnown = true;
	f.serve("Textures/A.hasset", hasset(HE::AssetType::Texture, ""));
	f.opt.fetch = nullptr;
	{
		const Result r = resolve(f.opt);
		REQUIRE(r.missing.size() == 1u);
		CHECK(r.missing[0].reason == MissingReason::NoFetcher);
	}
}

TEST_CASE("resolve: cancelling stops the walk and says so")
{
	Fixture f;
	write(f.project.path / "Content" / "Mat.hasset", hasset(HE::AssetType::Material, lp("Engine/Textures/A.hasset")));
	f.serve("Textures/A.hasset", hasset(HE::AssetType::Texture, ""));
	f.opt.cancelled = [] { return true; };
	const Result r = resolve(f.opt);
	CHECK(r.cancelled);
	CHECK_FALSE(r.ok());
	CHECK(f.fetchCalls.empty());
}

TEST_CASE("locateEngineFile: the project's override beats the library, which beats the cache")
{
	Fixture f;
	write(f.cache.path / "X.hasset", "cache");
	CHECK(fs::path(locateEngineFile("Engine/X.hasset", f.opt)) == f.cache.path / "X.hasset");
	write(f.engine.path / "X.hasset", "library");
	CHECK(fs::path(locateEngineFile("Engine/X.hasset", f.opt)) == f.engine.path / "X.hasset");
	write(f.project.path / "Content" / "Engine" / "X.hasset", "override");
	CHECK(fs::path(locateEngineFile("Engine/X.hasset", f.opt)) == f.project.path / "Content" / "Engine" / "X.hasset");
	CHECK(locateEngineFile("Engine/Nope.hasset", f.opt).empty());
	CHECK(locateEngineFile("Textures/X.hasset", f.opt).empty());   // not an engine path at all
}
