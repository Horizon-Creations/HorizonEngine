#include "doctest.h"
#include "AssetThumbnailCache.h"
#include "TestFsUtil.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <MaterialGraph/MaterialGraph.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <Types/Enums.h>
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

// ── Material-function tiles ──────────────────────────────────────────────────
// A function is a sub-graph with no surface, so its tile is rendered through a
// scratch material that wraps it. The wrapping is the part that can silently go
// wrong (a function whose graph yields no shader would render as a blank sphere
// forever, cached to disk), and it needs no GPU to check.

TEST_CASE("material function wraps into a renderable scratch material")
{
	const fs::path dir = fs::temp_directory_path() / "he_thumb_fn_content";
	he_test::removeAllQuiet(dir);
	fs::create_directories(dir);

	ContentManager cm(dir.string());
	{
		MaterialFunctionAsset fn;
		fn.type = HE::AssetType::MaterialFunction;
		fn.name = "Tint";
		fn.path = "Tint.hasset";
		fn.nodeGraphJson = HE::materialGraphToJson(HE::MaterialGraph::makeDefaultFunction());
		REQUIRE(cm.saveAsset(fn));
	}

	AssetThumbnailCache::setContext(nullptr, &cm, dir.string());
	const HE::UUID scratch = AssetThumbnailCache::materialFunctionScratch("Tint.hasset");
	REQUIRE_FALSE(scratch == HE::UUID{});

	// The scratch must carry real generated GLSL — an empty shader is exactly the
	// failure that would bake a blank tile into the on-disk cache.
	const MaterialAsset* mat = cm.getMaterial(scratch);
	REQUIRE(mat != nullptr);
	CHECK_FALSE(mat->customShaderFragGlsl.empty());

	// Asking again reuses the same scratch rather than leaking one material per
	// function — tiles are rendered one at a time, so a second live one is waste.
	CHECK(AssetThumbnailCache::materialFunctionScratch("Tint.hasset") == scratch);

	AssetThumbnailCache::setContext(nullptr, nullptr, "");
	he_test::removeAllQuiet(dir);
}

TEST_CASE("a function with no output pin yields no scratch material")
{
	const fs::path dir = fs::temp_directory_path() / "he_thumb_fn_empty";
	he_test::removeAllQuiet(dir);
	fs::create_directories(dir);

	ContentManager cm(dir.string());
	{
		// A graph with nodes but no FnOutput: there is nothing to route into the
		// wrapper's BaseColor, so the honest answer is "no tile", not a black ball.
		HE::MaterialGraph g;
		g.addNode(HE::MatNodeType::ConstFloat);
		MaterialFunctionAsset fn;
		fn.type = HE::AssetType::MaterialFunction;
		fn.name = "NoOut";
		fn.path = "NoOut.hasset";
		fn.nodeGraphJson = HE::materialGraphToJson(g);
		REQUIRE(cm.saveAsset(fn));
	}

	AssetThumbnailCache::setContext(nullptr, &cm, dir.string());
	CHECK(AssetThumbnailCache::materialFunctionScratch("NoOut.hasset") == HE::UUID{});
	CHECK(AssetThumbnailCache::materialFunctionScratch("DoesNotExist.hasset") == HE::UUID{});

	AssetThumbnailCache::setContext(nullptr, nullptr, "");
	he_test::removeAllQuiet(dir);
}

// ── Texture tiles ────────────────────────────────────────────────────────────

namespace
{
	// Register a texture directly (no disk round-trip needed — the tile is built
	// from the in-memory asset).
	HE::UUID makeTexture(ContentManager& cm, uint32_t w, uint32_t h, uint32_t ch,
	                     std::vector<uint8_t> px)
	{
		TextureAsset t;
		t.type = HE::AssetType::Texture;
		t.name = "tex";
		t.width = w; t.height = h; t.channels = ch;
		t.data = std::move(px);
		return cm.registerTexture(std::move(t));
	}
}

TEST_CASE("texture tile letterboxes instead of stretching")
{
	const fs::path dir = fs::temp_directory_path() / "he_thumb_tex";
	he_test::removeAllQuiet(dir);
	fs::create_directories(dir);
	ContentManager cm(dir.string());
	AssetThumbnailCache::setContext(nullptr, &cm, dir.string());

	const uint32_t S = AssetThumbnailCache::thumbnailSize();
	// A 4:1 strip of solid opaque red.
	const uint32_t w = 64, h = 16;
	std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
	for (size_t i = 0; i < px.size(); i += 4) { px[i] = 255; px[i+1] = 0; px[i+2] = 0; px[i+3] = 255; }

	std::vector<uint8_t> out;
	REQUIRE(AssetThumbnailCache::textureThumbnail(makeTexture(cm, w, h, 4, px), out));
	REQUIRE(out.size() == static_cast<size_t>(S) * S * 4);

	auto at = [&](uint32_t x, uint32_t y) { return &out[(static_cast<size_t>(y) * S + x) * 4]; };
	// Centre row is the image: red and opaque.
	CHECK(at(S / 2, S / 2)[0] > 200);
	CHECK(at(S / 2, S / 2)[3] == 255);
	// Top and bottom are letterbox, NOT stretched image — a 4:1 source in a square
	// tile must leave those transparent rather than filling them.
	CHECK(at(S / 2, 2)[3] == 0);
	CHECK(at(S / 2, S - 3)[3] == 0);

	AssetThumbnailCache::setContext(nullptr, nullptr, "");
	he_test::removeAllQuiet(dir);
}

TEST_CASE("texture tile composites alpha over a checkerboard")
{
	const fs::path dir = fs::temp_directory_path() / "he_thumb_tex_alpha";
	he_test::removeAllQuiet(dir);
	fs::create_directories(dir);
	ContentManager cm(dir.string());
	AssetThumbnailCache::setContext(nullptr, &cm, dir.string());

	const uint32_t S = AssetThumbnailCache::thumbnailSize();
	// Fully TRANSPARENT square. Drawn straight onto the dark tile this would be
	// invisible; the checkerboard is what makes "this texture has alpha" legible.
	const uint32_t w = 32, h = 32;
	std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, 0);

	std::vector<uint8_t> out;
	REQUIRE(AssetThumbnailCache::textureThumbnail(makeTexture(cm, w, h, 4, px), out));

	bool light = false, dark = false;
	for (uint32_t y = 4; y < S - 4; ++y)
		for (uint32_t x = 4; x < S - 4; ++x)
		{
			const uint8_t v = out[(static_cast<size_t>(y) * S + x) * 4];
			if (v > 110) light = true;
			if (v > 0 && v < 110) dark = true;
		}
	CHECK(light);   // both checker shades present → the pattern really is drawn
	CHECK(dark);

	AssetThumbnailCache::setContext(nullptr, nullptr, "");
	he_test::removeAllQuiet(dir);
}

TEST_CASE("texture tile handles greyscale and RGB sources, rejects nonsense")
{
	const fs::path dir = fs::temp_directory_path() / "he_thumb_tex_ch";
	he_test::removeAllQuiet(dir);
	fs::create_directories(dir);
	ContentManager cm(dir.string());
	AssetThumbnailCache::setContext(nullptr, &cm, dir.string());

	std::vector<uint8_t> out;
	CHECK(AssetThumbnailCache::textureThumbnail(
		makeTexture(cm, 8, 8, 1, std::vector<uint8_t>(64, 200)), out));   // grey
	CHECK(AssetThumbnailCache::textureThumbnail(
		makeTexture(cm, 8, 8, 3, std::vector<uint8_t>(8 * 8 * 3, 90)), out)); // RGB
	// A header claiming more pixels than the asset carries must be refused, not
	// read past the end of the buffer.
	CHECK_FALSE(AssetThumbnailCache::textureThumbnail(
		makeTexture(cm, 256, 256, 4, std::vector<uint8_t>(16, 0)), out));
	CHECK_FALSE(AssetThumbnailCache::textureThumbnail(
		makeTexture(cm, 0, 0, 4, {}), out));
	CHECK_FALSE(AssetThumbnailCache::textureThumbnail(HE::UUID{}, out));

	AssetThumbnailCache::setContext(nullptr, nullptr, "");
	he_test::removeAllQuiet(dir);
}

// ── Prefab tiles ─────────────────────────────────────────────────────────────

TEST_CASE("prefab tile resolves to the mesh inside the blob")
{
	const fs::path dir = fs::temp_directory_path() / "he_thumb_prefab";
	he_test::removeAllQuiet(dir);
	fs::create_directories(dir);
	ContentManager cm(dir.string());
	AssetThumbnailCache::setContext(nullptr, &cm, dir.string());

	const HE::UUID meshId{ 0x1234ULL, 0x5678ULL };

	// Author a one-entity subtree carrying that mesh and capture it as a prefab —
	// the same path the editor takes when you make a prefab from a selection.
	HorizonWorld world;
	SceneSerializer ser;
	const Entity e = world.createEntity("Prop");
	world.registry().emplace<MeshComponent>(e, MeshComponent{ meshId });
	PrefabAsset pf;
	pf.type = HE::AssetType::Prefab;
	pf.name = "Prop";
	pf.data = ser.serializeSubtree(world, e);
	REQUIRE_FALSE(pf.data.empty());
	const HE::UUID prefabId = cm.registerPrefab(std::move(pf));

	HE::UUID found{}; bool skeletal = true;
	REQUIRE(AssetThumbnailCache::prefabMesh(prefabId, found, skeletal));
	CHECK(found == meshId);
	CHECK_FALSE(skeletal);

	AssetThumbnailCache::setContext(nullptr, nullptr, "");
	he_test::removeAllQuiet(dir);
}

TEST_CASE("a prefab without any mesh reports no tile")
{
	const fs::path dir = fs::temp_directory_path() / "he_thumb_prefab_empty";
	he_test::removeAllQuiet(dir);
	fs::create_directories(dir);
	ContentManager cm(dir.string());
	AssetThumbnailCache::setContext(nullptr, &cm, dir.string());

	HorizonWorld world;
	SceneSerializer ser;
	const Entity e = world.createEntity("LogicOnly");   // no MeshComponent
	PrefabAsset pf;
	pf.type = HE::AssetType::Prefab;
	pf.name = "LogicOnly";
	pf.data = ser.serializeSubtree(world, e);
	const HE::UUID prefabId = cm.registerPrefab(std::move(pf));

	HE::UUID found{}; bool skeletal = false;
	// Nothing to draw — the caller must fall back to the glyph rather than render
	// an empty sphere and cache it.
	CHECK_FALSE(AssetThumbnailCache::prefabMesh(prefabId, found, skeletal));
	CHECK_FALSE(AssetThumbnailCache::prefabMesh(HE::UUID{}, found, skeletal));

	AssetThumbnailCache::setContext(nullptr, nullptr, "");
	he_test::removeAllQuiet(dir);
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
