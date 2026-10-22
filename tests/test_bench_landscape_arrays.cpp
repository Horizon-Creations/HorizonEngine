// Size, encode time and load time of the Auto Landscape texture arrays per texture format
// (Thema 180, Schritt 6, docs/perf-audit/auto-landscape-material-2-audit-2026-10-21.md).
//
// This is a measurement, not a test: it does nothing (and passes) unless
//   HE_BENCH_ARRAYS_IN=<folder with T_Landscape_{Albedo,Normal,Mask}_Array.hasset>
// is set. Each array goes through the real pack cook (HpakWriter, PackSettings::cookTextureArrays) once per
// format, and the cooked asset is loaded back from the pak. With
//   HE_BENCH_ARRAYS_OUT=<folder>
// the cooked arrays are also written as LOOSE .hasset files, one sub-folder per format
// (<folder>/ASTC4x4/T_Landscape_Albedo_Array.hasset ...), so the live editor can open them through a project's
// Content/Engine/Textures/Landscape override (the audit's runtime memory / startup runs).
// HE_BENCH_ARRAYS_FORMATS=1,2,3 picks the cook targets (HpakFormat.h: 1 ASTC, 2 BC7, 3 BC3, 4 BC1, 5 BC4, 6 BC5);
// HE_BENCH_ARRAYS_QUALITY=0..2 the encoder effort (default 0, what every export did until Schritt 10).
// One "BENCH ..." line per (map, format) on stdout; the audit doc quotes them.

#include "doctest.h"
#include "TestFsUtil.h"
#include <Hpak/HpakWriter.h>
#include <Hpak/HpakFormat.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0)
{
	return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}
}

TEST_CASE("bench: Auto Landscape arrays per texture format (only with HE_BENCH_ARRAYS_IN)")
{
	namespace fs = std::filesystem;
	const char* in = std::getenv("HE_BENCH_ARRAYS_IN");
	if (!in || !*in) return;
	const char* outEnv = std::getenv("HE_BENCH_ARRAYS_OUT");
	const char* fmtEnv = std::getenv("HE_BENCH_ARRAYS_FORMATS");
	const char* qEnv   = std::getenv("HE_BENCH_ARRAYS_QUALITY");
	const uint8_t quality = qEnv ? static_cast<uint8_t>(std::atoi(qEnv)) : 0;

	std::vector<int> formats;
	for (const char* p = fmtEnv && *fmtEnv ? fmtEnv : "1,2,3"; *p;)
	{
		formats.push_back(std::atoi(p));
		while (*p && *p != ',') ++p;
		if (*p == ',') ++p;
	}

	for (const char* map : { "Albedo", "Normal", "Mask" })
	{
		const std::string file = std::string("T_Landscape_") + map + "_Array.hasset";

		// RGBA8 loose, as the editor loads it today.
		TextureAsset src;
		{
			ContentManager cm(in);
			const auto t0 = Clock::now();
			const HE::UUID id = cm.loadAsset(file);
			const double ms = msSince(t0);
			const TextureAsset* t = cm.getTexture(id);
			REQUIRE(t != nullptr);
			src = *t;
			std::printf("BENCH map=%s format=RGBA8 layers=%u size=%zux%zu mips=%u bytes=%zu encodeMs=0 pakBytes=0 "
			            "loadLooseMs=%.1f loadPakMs=0\n",
			            map, src.layers, src.width, src.height, src.mipLevels, src.data.size(), ms);
			std::fflush(stdout);
		}

		for (int fmt : formats)
		{
			const fs::path dir = fs::temp_directory_path() / (std::string("he_bench_arr_") + map + std::to_string(fmt));
			he_test::removeAllQuiet(dir);
			fs::create_directories(dir);
			{
				ContentManager cmw(dir.string());
				TextureAsset copy = src;
				REQUIRE(cmw.saveAsset(copy));
			}
			Hpak::PackSettings s; s.codec = Hpak::Codec::Store; s.cook = true;
			s.textureCompression = static_cast<uint8_t>(fmt);
			s.textureQuality     = quality;
			s.cookTextureArrays  = true;
			const fs::path pak = fs::temp_directory_path() / (std::string("he_bench_arr_") + map + std::to_string(fmt) + ".hpak");
			const auto t0 = Clock::now();
			HpakWriter packer; packer.addDirectory(dir, s);
			REQUIRE(packer.write(pak.string()));
			const double encodeMs = msSince(t0);
			const uintmax_t pakBytes = fs::file_size(pak);

			ContentManager cm;
			const auto t1 = Clock::now();
			REQUIRE(cm.loadPak(pak.string()));
			const TextureAsset* t = cm.getTexture(src.id);
			const double loadPakMs = msSince(t1);
			REQUIRE(t != nullptr);

			double loadLooseMs = 0.0;
			if (outEnv && *outEnv)
			{
				const fs::path od = fs::path(outEnv) / ::textureFormatName(t->format);
				fs::create_directories(od);
				TextureAsset cooked = *t;
				{
					ContentManager cmo(od.string());
					REQUIRE(cmo.saveAsset(cooked));
				}
				ContentManager cml(od.string());
				const auto t2 = Clock::now();
				const HE::UUID lid = cml.loadAsset(file);
				loadLooseMs = msSince(t2);
				const TextureAsset* lt = cml.getTexture(lid);
				REQUIRE(lt != nullptr);
				CHECK(lt->format == t->format);
				CHECK(lt->data == t->data);
				CHECK(lt->layers == t->layers);
			}
			std::printf("BENCH map=%s format=%s layers=%u size=%zux%zu mips=%u bytes=%zu encodeMs=%.0f pakBytes=%ju "
			            "loadLooseMs=%.1f loadPakMs=%.1f\n",
			            map, ::textureFormatName(t->format), t->layers, t->width, t->height, t->mipLevels,
			            t->data.size(), encodeMs, pakBytes, loadLooseMs, loadPakMs);
			std::fflush(stdout);
			he_test::removeAllQuiet(dir);
			he_test::removeQuiet(pak);
		}
	}
}
