// Size and encode time of ONE baked landscape tile map per size and texture format
// (Thema 228 request for Thema 326, docs/landscape-hlod-baked-material-2026-11-02.md, section 3).
//
// A measurement, not a test: it does nothing (and passes) unless
//   HE_BENCH_TILE_PNG=<a top-down render of the landscape material>
// is set. The picture is cropped (HE_BENCH_TILE_CROP=x,y,w,h, default the whole image), box-filtered
// down to each tile side the HLOD planner can pick (256, 128, 64, 32) and cooked through the real
// pack pipeline (HpakWriter) once per format and codec. Per line: the encoded bytes (Store), the same
// entry compressed with zstd level 19 (what an export ships), and the wall time of the Store pack.
//   HE_BENCH_TILE_FORMATS=1,2,3   (HpakFormat.h: 1 ASTC, 2 BC7, 3 BC3, 4 BC1, 5 BC4, 6 BC5)
//   HE_BENCH_TILE_REPS=5          packs per cell, the median time is printed (a 32x32 tile is below timer noise)
#include "doctest.h"
#include "TestFsUtil.h"
#include "TextureImporter.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <Hpak/HpakFormat.h>
#include <Hpak/HpakWriter.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

// Box filter of the cropped RGBA8 image to side x side texels (the bake's own downsampling).
std::vector<uint8_t> boxResize(const std::vector<uint8_t>& src, int w, int h, int side)
{
	std::vector<uint8_t> out(static_cast<size_t>(side) * side * 4);
	for (int y = 0; y < side; ++y)
		for (int x = 0; x < side; ++x)
		{
			const int x0 = x * w / side, x1 = std::max(x0 + 1, (x + 1) * w / side);
			const int y0 = y * h / side, y1 = std::max(y0 + 1, (y + 1) * h / side);
			unsigned acc[4] = { 0, 0, 0, 0 };
			for (int sy = y0; sy < y1; ++sy)
				for (int sx = x0; sx < x1; ++sx)
					for (int c = 0; c < 4; ++c) acc[c] += src[(static_cast<size_t>(sy) * w + sx) * 4 + c];
			const unsigned n = static_cast<unsigned>((x1 - x0) * (y1 - y0));
			for (int c = 0; c < 4; ++c) out[(static_cast<size_t>(y) * side + x) * 4 + c] = static_cast<uint8_t>(acc[c] / n);
		}
	return out;
}
}

TEST_CASE("bench: one baked landscape tile map per size and format (only with HE_BENCH_TILE_PNG)")
{
	namespace fs = std::filesystem;
	const char* pngEnv = std::getenv("HE_BENCH_TILE_PNG");
	if (!pngEnv || !*pngEnv) return;

	std::ifstream f(pngEnv, std::ios::binary);
	REQUIRE(f.good());
	const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	TextureImporter::ImportSettings is; is.flipVertically = false;
	auto img = TextureImporter::decodeFromMemory(bytes.data(), bytes.size(), is);
	REQUIRE(img != nullptr);
	REQUIRE(img->width > 0);
	REQUIRE(img->channels >= 3);

	int cx = 0, cy = 0, cw = static_cast<int>(img->width), ch = static_cast<int>(img->height);
	if (const char* c = std::getenv("HE_BENCH_TILE_CROP"))
		std::sscanf(c, "%d,%d,%d,%d", &cx, &cy, &cw, &ch);
	REQUIRE(cx >= 0); REQUIRE(cy >= 0);
	REQUIRE(cx + cw <= static_cast<int>(img->width));
	REQUIRE(cy + ch <= static_cast<int>(img->height));
	std::vector<uint8_t> crop(static_cast<size_t>(cw) * ch * 4);
	for (int y = 0; y < ch; ++y)
		for (int x = 0; x < cw; ++x)
			for (int c = 0; c < 4; ++c)
			{
				const size_t si = (static_cast<size_t>(cy + y) * img->width + (cx + x)) * img->channels;
				crop[(static_cast<size_t>(y) * cw + x) * 4 + c] =
					c < static_cast<int>(img->channels) ? img->data[si + c] : 255;
			}

	std::vector<int> formats;
	for (const char* p = std::getenv("HE_BENCH_TILE_FORMATS") ? std::getenv("HE_BENCH_TILE_FORMATS") : "1,2,3"; *p;)
	{
		formats.push_back(std::atoi(p));
		while (*p && *p != ',') ++p;
		if (*p == ',') ++p;
	}
	const int reps = std::getenv("HE_BENCH_TILE_REPS") ? std::max(1, std::atoi(std::getenv("HE_BENCH_TILE_REPS"))) : 5;

	for (const int side : { 256, 128, 64, 32 })
	{
		const fs::path dir = fs::temp_directory_path() / ("he_bench_tile_" + std::to_string(side));
		he_test::removeAllQuiet(dir);
		fs::create_directories(dir);
		TextureAsset t;
		t.type = HE::AssetType::Texture; t.name = "tile"; t.path = "tile.hasset";
		t.width = t.height = static_cast<uint32_t>(side); t.channels = 4; t.srgb = true;
		t.data = boxResize(crop, cw, ch, side);
		{
			ContentManager cm(dir.string());
			REQUIRE(cm.saveAsset(t));
		}
		for (const int fmt : formats)
		{
			uintmax_t storeBytes = 0, zstdBytes = 0;
			std::vector<double> times;
			for (int rep = 0; rep < reps; ++rep)
				for (const Hpak::Codec codec : { Hpak::Codec::Store, Hpak::Codec::Zstd })
				{
					Hpak::PackSettings s; s.codec = codec; s.level = 19; s.cook = true;
					s.textureCompression = static_cast<uint8_t>(fmt);
					const fs::path pak = dir / ("t" + std::to_string(fmt) + ".hpak");
					const auto t0 = Clock::now();
					HpakWriter packer; packer.addDirectory(dir, s);
					REQUIRE(packer.write(pak.string()));
					const double ms = msSince(t0);
					const uintmax_t sz = fs::file_size(pak);
					if (codec == Hpak::Codec::Store) { storeBytes = sz; times.push_back(ms); }
					else zstdBytes = sz;
				}
			std::sort(times.begin(), times.end());
			std::printf("TILE side=%d format=%d texels=%d storeBytes=%ju zstdBytes=%ju zstdSaved=%.1f%% packMs=%.2f\n",
			            side, fmt, side * side, storeBytes, zstdBytes,
			            storeBytes ? 100.0 * (1.0 - static_cast<double>(zstdBytes) / static_cast<double>(storeBytes)) : 0.0,
			            times[times.size() / 2]);
			std::fflush(stdout);
		}
		he_test::removeAllQuiet(dir);
	}
}
