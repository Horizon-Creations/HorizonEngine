// Thema 210: the auto landscape turned into hexagons when its texture arrays
// arrived AFTER the renderer had first asked for them (an EngineContent download
// landing in the per-user cache while the editor runs). Every backend cached the
// miss as null under the PATH key and never asked again, so the slots bound the
// white array for the rest of the run, and the material's texture bombing rotates
// that array's constant normal per hex. docs/landscape-hex-reload-ursache-2026-10-18.md
// has the pixel counter-probe (scripts/landscape-reload-repro/cap210.sh: "mid" was
// bit-equal to "absent", it has to be bit-equal to "present").
//
// No GPU under ctest, so the counter-probe is driven here through the two halves
// the fix consists of: the ContentManager moves contentEpoch() when a download
// lands, and a ResolveGraphTexture shaped exactly like the backends' (cache by
// path key, null on a miss, HE::forgetGraphTextureMisses at the top) then gets the
// real texture. A source pin holds all five backends to calling the helper.
#include "doctest.h"
#include "TestFsUtil.h"
#include <ContentManager/ContentManager.h>
#include <HorizonRendering/GraphTextureMisses.h>
#include <MaterialGraph/AutoLandscapeMaterial.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace
{
struct TempDir
{
	fs::path path;
	explicit TempDir(const char* name)
	{
		path = fs::temp_directory_path() / name;
		he_test::removeAllQuiet(path);
		fs::create_directories(path);
	}
	~TempDir() { he_test::removeAllQuiet(path); }
};

// The download "server" of test_contentmanager.cpp's remote tests: the test says
// when the file lands.
struct LateArray
{
	int                       calls = 0;
	std::function<void(bool)> done;
	fs::path                  from, to;
	void arrive()
	{
		fs::create_directories(to.parent_path());
		fs::copy_file(from, to, fs::copy_options::overwrite_existing);
		auto d = std::move(done);
		done = nullptr;
		if (d) d(true);
	}
};

// A 2-layer array asset like T_Landscape_*_Array (layers > 1, 1x1, one mip).
HE::UUID writeArray(const fs::path& dir, const std::string& rel)
{
	ContentManager src(dir.string());
	TextureAsset t;
	t.type = HE::AssetType::Texture; t.name = fs::path(rel).stem().string(); t.path = rel;
	t.width = t.height = 1; t.channels = 4; t.layers = 2; t.mipLevels = 1;
	t.data = { 10, 20, 30, 255, 40, 50, 60, 255 };
	t.id = HE::UUID::generate();
	REQUIRE(src.saveAsset(t));
	return t.id;
}

template<class Pred>
bool pumpUntil(ContentManager& cm, Pred pred)
{
	for (int i = 0; i < 400 && !pred(); ++i)
	{
		cm.pollAsyncResults(16);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return pred();
}

// MetalRenderer/OpenGLRenderer::ResolveGraphTexture without the upload: the
// "GPU texture" is the asset's id (never a TextureAsset* — the next loadAsset
// may move it), UUID{} is the miss, which the caller binds as the white array.
struct FakeBackend
{
	ContentManager*                            cm = nullptr;
	std::unordered_map<std::string, HE::UUID>  cache;
	uint64_t                                   missEpoch = 0;
	int                                        loads = 0;   // resolveTextureRef calls
	HE::UUID resolve(const std::string& path)
	{
		HE::forgetGraphTextureMisses(cache, missEpoch, cm->contentEpoch(),
		                             [](const HE::UUID& t) { return t == HE::UUID{}; });
		const std::string key = path + "#arr";
		if (auto it = cache.find(key); it != cache.end()) return it->second;
		++loads;
		const TextureAsset* t = cm->resolveTextureRef(HE::UUID{}, path);
		const HE::UUID gpu = t ? t->id : HE::UUID{};
		cache.emplace(key, gpu);
		return gpu;
	}
};
} // namespace

TEST_CASE("Landscape reload: a texture download landing moves the content epoch (Thema 210)")
{
	TempDir project("he_test_l210_project"), engine("he_test_l210_engine"), stage("he_test_l210_stage");
	const std::string rel = "Textures/Landscape/T_He210_Late_Array.hasset";
	const HE::UUID id = writeArray(stage.path, "T_He210_Late_Array.hasset");

	ContentManager cm(project.path.string());
	cm.setEngineContentRoot(engine.path.string());
	LateArray late;
	late.from = stage.path / "T_He210_Late_Array.hasset";
	late.to   = engine.path / rel;
	const std::string path = "Engine/" + rel;
	cm.registerRemoteAsset(id, path, [&late](std::function<void(bool)> done) {
		++late.calls; late.done = std::move(done); });

	CHECK(cm.loadAsset(path) == HE::UUID{});   // asks for the download
	REQUIRE(late.calls == 1);
	const uint64_t before = cm.contentEpoch();
	late.arrive();
	REQUIRE(pumpUntil(cm, [&] { return cm.isLoaded(id); }));
	// The file appeared under an EngineContent root: whoever remembered the path as
	// "not there" has to look again. Without this every renderer's miss stayed.
	CHECK(cm.contentEpoch() != before);
}

TEST_CASE("Landscape reload: an array that arrives after the first resolve renders like a first load (Thema 210)")
{
	const std::string rel = "Textures/Landscape/T_He210_Late_Array.hasset";
	const std::string path = "Engine/" + rel;

	// "present": the array is in the engine root before anything asks (first load).
	std::vector<uint8_t> presentPixels;
	uint32_t             presentLayers = 0;
	{
		TempDir project("he_test_l210_project"), engine("he_test_l210_engine");
		const HE::UUID id = writeArray(engine.path / "Textures" / "Landscape", "T_He210_Late_Array.hasset");
		ContentManager cm(project.path.string());
		cm.setEngineContentRoot(engine.path.string());
		FakeBackend be; be.cm = &cm;
		const HE::UUID presentGpu = be.resolve(path);
		REQUIRE(presentGpu == id);
		const TextureAsset* t = cm.getTexture(presentGpu);
		REQUIRE(t != nullptr);
		presentPixels = t->data;
		presentLayers = t->layers;
		// A loaded texture survives an epoch move untouched (the first-load picture
		// does not change, nothing is uploaded twice).
		cm.noteContentChanged();
		CHECK(be.resolve(path) == id);
		CHECK(be.loads == 1);
	}

	// "mid": the same array is remote, the backend asks (and caches the miss), then
	// the download lands. cap210.sh mid, without pixels.
	{
		TempDir project("he_test_l210_project"), engine("he_test_l210_engine"), stage("he_test_l210_stage");
		const HE::UUID id = writeArray(stage.path, "T_He210_Late_Array.hasset");
		ContentManager cm(project.path.string());
		cm.setEngineContentRoot(engine.path.string());
		LateArray late;
		late.from = stage.path / "T_He210_Late_Array.hasset";
		late.to   = engine.path / rel;
		cm.registerRemoteAsset(id, path, [&late](std::function<void(bool)> done) {
			++late.calls; late.done = std::move(done); });
		FakeBackend be; be.cm = &cm;

		CHECK(be.resolve(path) == HE::UUID{});   // white array this frame
		CHECK(be.resolve(path) == HE::UUID{});   // and no second load in the same epoch
		CHECK(be.loads == 1);
		CHECK(late.calls == 1);

		late.arrive();
		REQUIRE(pumpUntil(cm, [&] { return cm.isLoaded(id); }));
		// Was: the null stayed, the slot bound the white array = the hexagons.
		const HE::UUID midGpu = be.resolve(path);
		REQUIRE(midGpu == id);
		const TextureAsset* t = cm.getTexture(midGpu);   // fetched after the last load
		REQUIRE(t != nullptr);
		// What the slot samples is the first-load array, slice for slice.
		CHECK(t->layers == presentLayers);
		CHECK(t->data == presentPixels);
		CHECK(be.loads == 2);
	}
}

TEST_CASE("Landscape reload: the backend asking in the frame the download lands gets the array (Thema 210)")
{
	// The live editor's order: pollAsyncResults sees the download (epoch moves, the
	// async read of the file is only QUEUED), and the renderer resolves in the same
	// frame, so its loadAsset(path) opens the file while that read is in flight.
	const std::string rel = "Textures/Landscape/T_He210_Late_Array.hasset";
	const std::string path = "Engine/" + rel;
	TempDir project("he_test_l210_project"), engine("he_test_l210_engine"), stage("he_test_l210_stage");
	const HE::UUID id = writeArray(stage.path, "T_He210_Late_Array.hasset");
	ContentManager cm(project.path.string());
	cm.setEngineContentRoot(engine.path.string());
	LateArray late;
	late.from = stage.path / "T_He210_Late_Array.hasset";
	late.to   = engine.path / rel;
	cm.registerRemoteAsset(id, path, [&late](std::function<void(bool)> done) {
		++late.calls; late.done = std::move(done); });
	FakeBackend be; be.cm = &cm;

	CHECK(be.resolve(path) == HE::UUID{});
	late.arrive();
	cm.pollAsyncResults(16);                 // this frame's poll: the download is in
	REQUIRE_FALSE(cm.isLoaded(id));          // the async registration is still to come
	CHECK(be.resolve(path) == id);          // ... and this frame's draw already samples it
	CHECK(be.loads == 2);

	// The queued async read lands later on top of the asset the sync load registered:
	// still one asset under the manifest's id, same pixels, nothing re-requested.
	for (int i = 0; i < 50; ++i)
	{
		cm.pollAsyncResults(16);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	CHECK(cm.loadAsset(path) == id);
	const TextureAsset* t = cm.getTexture(id);
	REQUIRE(t != nullptr);
	CHECK(t->layers == 2);
	CHECK(t->data == std::vector<uint8_t>{ 10, 20, 30, 255, 40, 50, 60, 255 });
	CHECK(be.resolve(path) == id);
	CHECK(be.loads == 2);
	CHECK(late.calls == 1);
}

TEST_CASE("Landscape reload: a texture group's arrays arrive late like Mountains' (Thema 233)")
{
	// Forest and Grassland read their own arrays through the same path-keyed resolve, so a download that
	// lands after the first ask has to reach them too (the cache stores a miss as null per epoch). Stand-in
	// names, as in the Thema 210 cases: the per-user EngineContent cache of the machine running the test may
	// hold the real T_Landscape_*_Array, which would resolve before anything "arrives".
	for (int g = 0; g < HE::kAutoLandscapeGroupCount; ++g)
		for (int map = 0; map < 3; ++map)
		{
			static const char* const kMaps[3] = { "Albedo", "Normal", "Mask" };
			const std::string leaf = std::string("T_He233_") + HE::kAutoLandscapeGroups[g].name + "_" + kMaps[map] + "_Array.hasset";
			const std::string rel  = "Textures/Landscape/" + leaf;
			const std::string path = "Engine/" + rel;
			CAPTURE(path);
			TempDir project("he_test_l233_project"), engine("he_test_l233_engine"), stage("he_test_l233_stage");
			const HE::UUID id = writeArray(stage.path, leaf);
			ContentManager cm(project.path.string());
			cm.setEngineContentRoot(engine.path.string());
			LateArray late;
			late.from = stage.path / leaf;
			late.to   = engine.path / rel;
			cm.registerRemoteAsset(id, path, [&late](std::function<void(bool)> done) {
				++late.calls; late.done = std::move(done); });
			FakeBackend be; be.cm = &cm;
			CHECK(be.resolve(path) == HE::UUID{});
			late.arrive();
			REQUIRE(pumpUntil(cm, [&] { return cm.isLoaded(id); }));
			CHECK(be.resolve(path) == id);
		}
}

TEST_CASE("Landscape reload: a texture that never loads is not reopened every frame (Thema 210)")
{
	TempDir project("he_test_l210_project");
	ContentManager cm(project.path.string());
	FakeBackend be; be.cm = &cm;
	for (int frame = 0; frame < 16; ++frame)
		CHECK(be.resolve("Textures/T_He210_Missing.hasset") == HE::UUID{});
	CHECK(be.loads == 1);   // one ask, then the cached miss (step 1 measured 25/97 errors without it)
	cm.noteContentChanged();
	CHECK(be.resolve("Textures/T_He210_Missing.hasset") == HE::UUID{});
	CHECK(be.loads == 2);   // a moved epoch buys exactly one more look
}

namespace
{
fs::path findRepoRoot()
{
	std::error_code ec;
	std::vector<fs::path> seeds;
	fs::path self(__FILE__);
	if (!self.is_absolute()) self = fs::current_path(ec) / self;
	seeds.push_back(self.parent_path());
	seeds.push_back(fs::current_path(ec));
	for (fs::path seed : seeds)
		for (int up = 0; up < 8 && !seed.empty(); ++up, seed = seed.parent_path())
			if (fs::exists(seed / "src" / "HE_Rendering" / "src" / "Backends" / "Metal" / "MetalRenderer.mm", ec))
				return seed;
	return {};
}

std::string readSource(const fs::path& p)
{
	std::ifstream f(p, std::ios::binary);
	std::ostringstream ss;
	ss << f.rdbuf();
	std::string s = ss.str();
	s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
	return s;
}
} // namespace

TEST_CASE("Landscape reload: every backend's graph-texture resolve forgets its misses per epoch (Thema 210)")
{
	// The FakeBackend above is only as good as the five real ones: each must call
	// the helper inside its resolve, BEFORE it loads (or the miss is already back).
	const fs::path root = findRepoRoot();
	if (root.empty())
	{
		MESSAGE("renderer sources not found - graph-texture miss pin skipped");
		return;
	}
	const fs::path be = root / "src" / "HE_Rendering" / "src" / "Backends";
	struct Site { const char* file; const char* signature; };
	for (const Site& s : {
		Site{ "Metal/MetalRenderer.mm",     "void* MetalRenderer::ResolveGraphTexture(" },
		Site{ "OpenGL/OpenGLRenderer.cpp",  "unsigned int OpenGLRenderer::ResolveGraphTexture(" },
		Site{ "Vulkan/VulkanRenderer.cpp",  "VkImageView VulkanRenderer::resolveGraphTexture(" },
		Site{ "D3D11/D3D11Renderer.cpp",    "ID3D11ShaderResourceView* resolveGraphTexture(" },
		Site{ "D3D12/D3D12Renderer.cpp",    "ID3D12Resource* resolveGraphTexture(" } })
	{
		const std::string src = readSource(be / s.file);
		REQUIRE_MESSAGE(!src.empty(), s.file);
		const size_t at = src.find(s.signature);
		const std::string where = std::string(s.file) + ": " + s.signature;
		REQUIRE_MESSAGE(at != std::string::npos, where);
		const size_t load = src.find("resolveTextureRef(", at);
		REQUIRE_MESSAGE(load != std::string::npos, s.file);
		const size_t sweep = src.find("HE::forgetGraphTextureMisses(", at);
		const std::string why = std::string(s.file)
			+ ": ResolveGraphTexture must call HE::forgetGraphTextureMisses before it loads";
		CHECK_MESSAGE((sweep != std::string::npos && sweep < load), why);
	}
}
