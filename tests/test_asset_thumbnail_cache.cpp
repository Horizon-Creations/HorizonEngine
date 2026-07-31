#include "doctest.h"
#include "AssetThumbnailCache.h"
#include "TestFsUtil.h"
#include <filesystem>
#include <fstream>
#include <vector>

// The Content Browser's thumbnail cache. Its GPU half needs a renderer and a live
// ImGui frame, but the part that decides WHEN a tile has to be re-rendered — the
// on-disk file format, the source-file stamp and the collision guard — is plain
// logic, and it is exactly the part where a bug shows up as a stale or wrong
// picture rather than a crash. AssetThumbnailCache.cpp is compiled directly into
// the test target (no ImGui dependency, by design).

namespace fs = std::filesystem;

namespace
{
	fs::path scratchDir()
	{
		const fs::path d = fs::temp_directory_path() / "he_thumbcache_test";
		std::error_code ec;
		fs::create_directories(d, ec);
		return d;
	}

	std::vector<uint8_t> makePixels(uint32_t size, uint8_t seed)
	{
		std::vector<uint8_t> px(static_cast<size_t>(size) * size * 4);
		for (size_t i = 0; i < px.size(); ++i) px[i] = static_cast<uint8_t>((i + seed) & 0xFF);
		return px;
	}
}

TEST_CASE("thumbnail cache file round-trips pixels")
{
	const fs::path dir  = scratchDir();
	const fs::path file = dir / "roundtrip.hthumb";
	he_test::removeQuiet(file);

	const auto pixels = makePixels(8, 3);
	CHECK(AssetThumbnailCache::writeFile(file.string(), "Materials/Brick.hasset", 12345, 8, pixels));

	uint32_t size = 0;
	std::vector<uint8_t> read;
	CHECK(AssetThumbnailCache::readFile(file.string(), "Materials/Brick.hasset", 12345, size, read));
	CHECK(size == 8);
	CHECK(read == pixels);

	he_test::removeQuiet(file);
}

TEST_CASE("thumbnail cache misses when the source asset changed")
{
	const fs::path dir  = scratchDir();
	const fs::path file = dir / "stale.hthumb";
	he_test::removeQuiet(file);

	const auto pixels = makePixels(4, 0);
	CHECK(AssetThumbnailCache::writeFile(file.string(), "Meshes/Rock.hasset", 1000, 4, pixels));

	uint32_t size = 0;
	std::vector<uint8_t> read;
	// A different stamp means the .hasset was rewritten since this tile was made —
	// the caller must re-render rather than show the old picture.
	CHECK_FALSE(AssetThumbnailCache::readFile(file.string(), "Meshes/Rock.hasset", 1001, size, read));
	// Same stamp still hits.
	CHECK(AssetThumbnailCache::readFile(file.string(), "Meshes/Rock.hasset", 1000, size, read));

	he_test::removeQuiet(file);
}

TEST_CASE("thumbnail cache rejects a file written for another asset")
{
	const fs::path dir  = scratchDir();
	const fs::path file = dir / "collision.hthumb";
	he_test::removeQuiet(file);

	const auto pixels = makePixels(4, 7);
	CHECK(AssetThumbnailCache::writeFile(file.string(), "Materials/A.hasset", 99, 4, pixels));

	uint32_t size = 0;
	std::vector<uint8_t> read;
	// File names are a path hash; the stored path is what actually decides a hit,
	// so a collision can never surface another asset's picture. Same length as
	// "Materials/A.hasset" on purpose — the length check must not be what saves us.
	CHECK_FALSE(AssetThumbnailCache::readFile(file.string(), "Materials/B.hasset", 99, size, read));

	he_test::removeQuiet(file);
}

TEST_CASE("thumbnail cache rejects a truncated file")
{
	const fs::path dir  = scratchDir();
	const fs::path file = dir / "truncated.hthumb";
	he_test::removeQuiet(file);

	const auto pixels = makePixels(8, 1);
	CHECK(AssetThumbnailCache::writeFile(file.string(), "Meshes/Tree.hasset", 5, 8, pixels));

	// Chop the pixel block in half — what a crash mid-write used to leave behind
	// before the write went through a temp file + rename.
	const auto full = fs::file_size(file);
	fs::resize_file(file, full - (static_cast<size_t>(8) * 8 * 4) / 2);

	uint32_t size = 0;
	std::vector<uint8_t> read;
	CHECK_FALSE(AssetThumbnailCache::readFile(file.string(), "Meshes/Tree.hasset", 5, size, read));

	he_test::removeQuiet(file);
}

TEST_CASE("thumbnail cache rejects a foreign or empty file")
{
	const fs::path dir  = scratchDir();
	const fs::path file = dir / "garbage.hthumb";
	{
		std::ofstream f(file, std::ios::binary);
		f << "not a thumbnail at all, just some bytes";
	}
	uint32_t size = 0;
	std::vector<uint8_t> read;
	CHECK_FALSE(AssetThumbnailCache::readFile(file.string(), "X.hasset", 1, size, read));
	CHECK_FALSE(AssetThumbnailCache::readFile((dir / "missing.hthumb").string(), "X.hasset", 1, size, read));

	he_test::removeQuiet(file);
}

TEST_CASE("thumbnail file names are stable and per-path")
{
	const std::string a = AssetThumbnailCache::fileNameFor("Materials/Brick.hasset");
	const std::string b = AssetThumbnailCache::fileNameFor("Materials/Brick.hasset");
	const std::string c = AssetThumbnailCache::fileNameFor("Materials/Stone.hasset");
	CHECK(a == b);              // same path → same file across runs
	CHECK(a != c);
	CHECK(a.size() == 16 + 7);  // 16 hex digits + ".hthumb"
	CHECK(a.rfind(".hthumb") == 16);
}

TEST_CASE("source stamp tracks writes and reports missing files as zero")
{
	const fs::path dir  = scratchDir();
	const fs::path file = dir / "stamped.hasset";
	he_test::removeQuiet(file);

	CHECK(AssetThumbnailCache::sourceStampOf(file.string()) == 0); // not there yet

	{ std::ofstream f(file, std::ios::binary); f << "aaaa"; }
	const uint64_t first = AssetThumbnailCache::sourceStampOf(file.string());
	CHECK(first != 0);
	CHECK(AssetThumbnailCache::sourceStampOf(file.string()) == first); // stable while untouched

	// A rewrite that changes the length has to change the stamp even if the clock
	// has not ticked between the two writes — that is the case an mtime-only
	// fingerprint would miss, leaving the old tile on screen.
	{ std::ofstream f(file, std::ios::binary); f << "aaaaaaaaaaaaaaaa"; }
	CHECK(AssetThumbnailCache::sourceStampOf(file.string()) != first);

	he_test::removeQuiet(file);
}
