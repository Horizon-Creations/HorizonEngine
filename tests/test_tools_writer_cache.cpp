#include "doctest.h"

#include <ContentManager/AssetFile.h>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <Hpak/HpakReader.h>
#include <Hpak/HpakWriter.h>
#include <Hpak/PackManifest.h>
#include <Platform/ProjectWriteGate.h>

#include "TestFsUtil.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

// ── The offline writers' fast paths (topic 219 step 2) ───────────────────────
// TLS-101: the importers write through HE::saveAssetFile instead of building a whole
// ContentManager per asset; it has to write the very file saveAsset would.
// TLS-203: the hpak_packer CLI shares the editor export's incremental cache
// (Hpak/PackManifest.h); the gate must take a matching manifest and nothing else.

namespace fs = std::filesystem;

namespace
{
	struct TempDir
	{
		fs::path path;
		explicit TempDir(const char* name)
		{
			path = fs::temp_directory_path() / (std::string(name) + "_" + std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
			he_test::removeAllQuiet(path);
			fs::create_directories(path);
		}
		~TempDir() { he_test::removeAllQuiet(path); }
	};

	std::vector<char> fileBytes(const fs::path& p)
	{
		std::ifstream f(p, std::ios::binary);
		return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
	}

	TextureAsset makeTexture(const std::string& path, uint8_t seed)
	{
		TextureAsset t;
		t.id       = HE::UUID{ 0x1234000000000000ull + seed, 0x5678ull + seed };
		t.name     = fs::path(path).stem().string();
		t.path     = path;
		t.type     = HE::AssetType::Texture;
		t.width    = 4;
		t.height   = 4;
		t.channels = 4;
		t.data.resize(4 * 4 * 4);
		for (size_t i = 0; i < t.data.size(); ++i)
			t.data[i] = static_cast<uint8_t>(i * 7 + seed);
		t.sourcePath = "/abs/source/" + t.name + ".png";
		return t;
	}

	bool gExporting = false;
	bool probe() { return gExporting; }
	void handler(const char*) {}
}

TEST_CASE("saveAssetFile writes the bytes a fresh ContentManager's saveAsset writes")
{
	TempDir a("he_assetfile_cm"), b("he_assetfile_free");

	TextureAsset viaManager = makeTexture("Textures/Deep/T_Rock.hasset", 3);
	TextureAsset viaFree    = viaManager;
	{
		ContentManager cm(a.path.string());
		REQUIRE(cm.saveAsset(viaManager));
	}
	// The folder does not exist yet: saveAssetFile has to create it, as saveAsset does.
	REQUIRE(HE::saveAssetFile(viaFree, b.path.string()));

	const auto expect = fileBytes(a.path / "Textures/Deep/T_Rock.hasset");
	const auto got    = fileBytes(b.path / "Textures/Deep/T_Rock.hasset");
	REQUIRE_FALSE(expect.empty());
	CHECK(got == expect);
}

TEST_CASE("saveAssetFile mints a UUID for an asset that has none")
{
	TempDir dir("he_assetfile_uuid");
	TextureAsset t = makeTexture("T_New.hasset", 5);
	t.id = HE::UUID{};
	REQUIRE(HE::saveAssetFile(t, dir.path.string()));
	CHECK(t.id != HE::UUID{});

	// …and it is the id the file carries.
	ContentManager cm(dir.path.string());
	const TextureAsset* loaded = cm.getTexture(cm.loadAsset("T_New.hasset"));
	REQUIRE(loaded != nullptr);
	CHECK(loaded->id == t.id);
}

TEST_CASE("saveAssetFile is refused while an export reads the project")
{
	TempDir dir("he_assetfile_gate");
	gExporting = true;
	HE::ProjectWriteGate::install(&probe, &handler);
	TextureAsset t = makeTexture("T_Locked.hasset", 7);
	const bool saved = HE::saveAssetFile(t, dir.path.string());
	HE::ProjectWriteGate::install(nullptr, nullptr);
	gExporting = false;

	CHECK_FALSE(saved);
	CHECK_FALSE(fs::exists(dir.path / "T_Locked.hasset"));
}

TEST_CASE("Pack manifest: a re-pack with the manifest reuses every unchanged entry, byte for byte")
{
	TempDir dir("he_packmanifest");
	const fs::path content = dir.path / "Content";
	for (uint8_t i = 0; i < 3; ++i)
	{
		TextureAsset t = makeTexture("Tex/T_" + std::to_string(i) + ".hasset", i);
		REQUIRE(HE::saveAssetFile(t, content.string()));
	}
	const fs::path pak      = dir.path / "Game.hpak";
	const fs::path manifest = dir.path / "Game.hpak.manifest";
	const std::vector<HpakWriter::SourceRoot> roots{ { content, {} } };

	Hpak::PackSettings s; // Store, unencrypted: the same input gives the same bytes
	{
		HpakWriter first;
		REQUIRE(first.addDirectories(roots, s) == 3);
		REQUIRE(first.write(pak.string()));
		REQUIRE(HE::writeIncrementalManifest(pak, manifest, s, first.entryInfos()));
	}

	const fs::path pak2 = dir.path / "Game2.hpak";
	{
		auto prev = std::make_unique<HpakReader>();
		Hpak::IncrementalCache cache;
		REQUIRE(HE::openIncrementalCache(pak, manifest, s, *prev, cache));
		CHECK(cache.previousPak == prev.get());

		HpakWriter second;
		REQUIRE(second.addDirectories(roots, s, {}, &cache) == 3);
		CHECK(second.reusedCount() == 3);
		prev.reset();
		REQUIRE(second.write(pak2.string()));
	}
	CHECK(fileBytes(pak2) == fileBytes(pak));

	SUBCASE("other settings do not get the cache")
	{
		Hpak::PackSettings codec = s;
		codec.codec = Hpak::Codec::Zstd;
		Hpak::PackSettings keyed = s;
		keyed.encrypt = true;
		keyed.key[0]  = 1;
		Hpak::PackSettings cooked = s;
		cooked.cook = true;
		for (const Hpak::PackSettings* other : { &codec, &keyed, &cooked })
		{
			HpakReader prev;
			Hpak::IncrementalCache cache;
			CHECK_FALSE(HE::openIncrementalCache(pak, manifest, *other, prev, cache));
		}
		CHECK(HE::packSettingsFingerprint(codec) != HE::packSettingsFingerprint(s));
	}

	SUBCASE("a manifest describing a different pak is not trusted")
	{
		// Re-pack with one asset changed but keep the OLD manifest: its tocHash no
		// longer matches the pak on disk.
		TextureAsset t = makeTexture("Tex/T_1.hasset", 1);
		t.data[0] ^= 0xFF;
		REQUIRE(HE::saveAssetFile(t, content.string()));
		HpakWriter repack;
		REQUIRE(repack.addDirectories(roots, s) == 3);
		REQUIRE(repack.write(pak.string()));

		HpakReader prev;
		Hpak::IncrementalCache cache;
		CHECK_FALSE(HE::openIncrementalCache(pak, manifest, s, prev, cache));
	}

	SUBCASE("no manifest, or no previous pak, means a full pack")
	{
		HpakReader prev;
		Hpak::IncrementalCache cache;
		CHECK_FALSE(HE::openIncrementalCache(pak, dir.path / "missing.manifest", s, prev, cache));
		HpakReader prev2;
		Hpak::IncrementalCache cache2;
		CHECK_FALSE(HE::openIncrementalCache(dir.path / "missing.hpak", manifest, s, prev2, cache2));
	}
}
