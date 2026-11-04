// The editor reads big loose RGBA8 texture arrays cooked for the GPU (ContentManager/GpuArrayCook.h):
// the cook runs once per source file, the result is kept in a cache directory keyed by path/size/mtime,
// the file on disk is never touched. No GPU involved: what is checked is the bytes the loader gets.

#include "doctest.h"
#include "TestFsUtil.h"
#include <ContentManager/GpuArrayCook.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/TextureArrayBuild.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;

TextureAsset ramp(uint32_t w, uint32_t h, int phase)
{
	TextureAsset t;
	t.type = HE::AssetType::Texture; t.name = "ramp"; t.path = "ramp.hasset";
	t.width = w; t.height = h; t.channels = 4;
	t.data.resize(static_cast<size_t>(w) * h * 4);
	for (uint32_t y = 0; y < h; ++y)
		for (uint32_t x = 0; x < w; ++x)
		{
			uint8_t* p = &t.data[(static_cast<size_t>(y) * w + x) * 4];
			p[0] = static_cast<uint8_t>(40 + phase + x * 120 / w);
			p[1] = static_cast<uint8_t>(30 + y * 140 / h);
			p[2] = static_cast<uint8_t>(60 + (x + y) * 60 / (w + h));
			p[3] = 255;
		}
	return t;
}

// Writes a `slices`-slice 64x64 array as <dir>/gc_arr.hasset.
void writeArray(const fs::path& dir, int slices, int phaseStep)
{
	std::vector<TextureAsset> src;
	for (int s = 0; s < slices; ++s) src.push_back(ramp(64, 64, s * phaseStep));
	std::vector<const TextureAsset*> ptrs;
	for (const auto& t : src) ptrs.push_back(&t);
	TextureAsset arr;
	REQUIRE(HE::buildTextureArray(ptrs, arr, /*bakeMips=*/true));
	arr.type = HE::AssetType::Texture; arr.name = "gc_arr"; arr.path = "gc_arr.hasset"; arr.srgb = true;
	ContentManager cm(dir.string());
	REQUIRE(cm.saveAsset(arr));
}

size_t countCached(const fs::path& cache)
{
	size_t n = 0;
	std::error_code ec;
	if (!fs::exists(cache, ec)) return 0;
	for (const auto& e : fs::recursive_directory_iterator(cache, ec))
		if (e.is_regular_file() && e.path().extension() == ".hasset") ++n;
	return n;
}

// Leaves the process-wide cook off whatever a test did.
struct CookGuard { ~CookGuard() { HE::setGpuArrayCook(HE::GpuArrayCookSettings{}); } };

} // namespace

TEST_CASE("GpuArrayCook: the format each backend asks for")
{
	using B = HE::RendererBackend;
	CHECK(HE::gpuArrayCookFormatFor(static_cast<int>(B::Metal)) == 1);       // ASTC 4x4
	CHECK(HE::gpuArrayCookFormatFor(static_cast<int>(B::Software)) == 0);    // draws only UI
	CHECK(HE::gpuArrayCookFormatFor(static_cast<int>(B::D3D12)) == 2);       // BC7
	CHECK(HE::gpuArrayCookFormatFor(static_cast<int>(B::D3D11)) == 2);
}

TEST_CASE("GpuArrayCook: off by default, small files and non-arrays are left alone")
{
	CookGuard guard;
	const fs::path dir = fs::temp_directory_path() / "he_gpucook_off";
	he_test::removeAllQuiet(dir);
	fs::create_directories(dir / "src");
	writeArray(dir / "src", 4, 20);
	std::vector<uint8_t> out;
	CHECK_FALSE(HE::readGpuCookedArray((dir / "src" / "gc_arr.hasset").string(), out));   // nothing set

	HE::GpuArrayCookSettings s; s.format = 3; s.cacheDir = (dir / "cache").string();      // 8 MB minimum
	HE::setGpuArrayCook(s);
	CHECK(HE::gpuArrayCookEnabled());
	CHECK_FALSE(HE::readGpuCookedArray((dir / "src" / "gc_arr.hasset").string(), out));   // this file is tiny
	CHECK_FALSE(HE::readGpuCookedArray((dir / "src" / "missing.hasset").string(), out));
	HE::setGpuArrayCook(HE::GpuArrayCookSettings{});
	he_test::removeAllQuiet(dir);
}

#ifdef HE_HAVE_STB_DXT
TEST_CASE("GpuArrayCook: an RGBA8 array is cooked once, cached by source, and loads compressed")
{
	CookGuard guard;
	const fs::path dir = fs::temp_directory_path() / "he_gpucook_on";
	he_test::removeAllQuiet(dir);
	const fs::path src = dir / "src", cache = dir / "cache";
	fs::create_directories(src);
	writeArray(src, 4, 20);
	const std::string file = (src / "gc_arr.hasset").string();
	const auto slurp = [](const std::string& p) {
		std::ifstream in(p, std::ios::binary);
		return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {}); };
	const std::vector<uint8_t> original = slurp(file);

	HE::GpuArrayCookSettings s; s.format = 3; s.quality = 0; s.cacheDir = cache.string(); s.minSourceBytes = 0;
	HE::setGpuArrayCook(s);

	std::vector<uint8_t> first;
	REQUIRE(HE::readGpuCookedArray(file, first));
	CHECK(first != original);
	CHECK(first.size() < original.size());
	CHECK(countCached(cache) == 1);
	CHECK(slurp(file) == original);                                   // the source is never rewritten

	std::vector<uint8_t> second;                                      // a hit: same bytes, no new file
	REQUIRE(HE::readGpuCookedArray(file, second));
	CHECK(second == first);
	CHECK(countCached(cache) == 1);

	{   // the cooked bytes are a valid compressed array with its slice means
		ContentManager cm;
		const TextureAsset* t = cm.getTexture(cm.loadAssetFromMemory(first));
		REQUIRE(t);
		CHECK(t->format == TextureFormat::BC3);
		CHECK(t->layers == 4);
		CHECK(t->srgb);
		CHECK(t->sliceMeans.size() == 4 * 3);
		CHECK(HE::textureArrayPayloadValid(*t));
	}
	{   // ... and the loader that reads the file by path gets it compressed too
		ContentManager cm(src.string());
		const TextureAsset* t = cm.getTexture(cm.loadAsset("gc_arr.hasset"));
		REQUIRE(t);
		CHECK(t->format == TextureFormat::BC3);
	}

	// A changed source (other size) is a different cache key: the old entry goes, a new one is made.
	writeArray(src, 5, 12);
	std::vector<uint8_t> third;
	REQUIRE(HE::readGpuCookedArray(file, third));
	CHECK(third != first);
	CHECK(countCached(cache) == 1);

	// An array that is already compressed is not cooked again.
	{
		std::ofstream f(file, std::ios::binary | std::ios::trunc);
		f.write(reinterpret_cast<const char*>(third.data()), static_cast<std::streamsize>(third.size()));
	}
	std::vector<uint8_t> none;
	CHECK_FALSE(HE::readGpuCookedArray(file, none));

	HE::setGpuArrayCook(HE::GpuArrayCookSettings{});
	CHECK_FALSE(HE::gpuArrayCookEnabled());
	he_test::removeAllQuiet(dir);
}
#endif // HE_HAVE_STB_DXT
