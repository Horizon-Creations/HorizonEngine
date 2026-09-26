// Texture colour space at import time (Thema 82, Schritt 2).
//
// Every manual texture import used to come out linear: Importer::importSource
// passed TextureImporter::ImportSettings{} (srgb = false), so an albedo PNG was
// sampled as if its sRGB-encoded bytes were linear light and rendered washed out.
// Pinned here: the name guess, the explicit override, that a reimport keeps the
// flag the asset carries, and the in-place rewrite the editor's batch action uses.
#include "doctest.h"
#include "TestFsUtil.h"
#include <ContentManager/AssetRefScan.h>   // assetUuidOfFile — identity across a rewrite
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>
#include "ImporterCommon.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
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

	// A valid 1x1 RGB PNG, the same bytes the reimport tests in
	// test_contentmanager use.
	void writePng1x1(const fs::path& file)
	{
		static const unsigned char kPng1x1[] = {
			0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
			0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
			0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
			0x0C, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
			0x00, 0x03, 0x01, 0x01, 0x00, 0xF7, 0x03, 0x41, 0x43, 0x00, 0x00, 0x00,
			0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82
		};
		std::ofstream png(file, std::ios::binary | std::ios::trunc);
		REQUIRE(png);
		png.write(reinterpret_cast<const char*>(kPng1x1),
		          static_cast<std::streamsize>(sizeof(kPng1x1)));
	}

	// A texture .hasset exactly as a build from before the cook tail wrote it:
	// TXMI is width/height/channels only, so the file carries no sRGB answer.
	void writePreTailTexture(const fs::path& file, const std::string& relPath,
	                         const fs::path& source)
	{
		std::vector<uint8_t> meta;
		HAsset::Writer::appendPOD(meta, static_cast<uint16_t>(HE::AssetType::Texture));
		HAsset::Writer::appendPOD(meta, uint64_t{0x5A6B});
		HAsset::Writer::appendPOD(meta, uint64_t{0x7C8D});
		HAsset::Writer::appendString(meta, fs::path(relPath).stem().string());
		HAsset::Writer::appendString(meta, relPath);
		HAsset::Writer::appendString(meta, source.generic_string());

		std::vector<uint8_t> txmi;
		HAsset::Writer::appendPOD(txmi, uint32_t{1});   // width
		HAsset::Writer::appendPOD(txmi, uint32_t{1});   // height
		HAsset::Writer::appendPOD(txmi, uint32_t{4});   // channels

		const std::vector<uint8_t> pixels = { 0x10, 0x20, 0x30, 0xFF };
		HAsset::Writer w;
		w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
		w.addChunk(HAsset::CHUNK_TXMI, txmi.data(), txmi.size());
		w.addChunk(HAsset::CHUNK_PIXL, pixels.data(), pixels.size());
		const std::vector<uint8_t> bytes = w.toBytes(static_cast<uint16_t>(HE::AssetType::Texture));

		fs::create_directories(file.parent_path());
		std::ofstream out(file, std::ios::binary | std::ios::trunc);
		REQUIRE(out);
		out.write(reinterpret_cast<const char*>(bytes.data()),
		          static_cast<std::streamsize>(bytes.size()));
	}
}

TEST_CASE("suggestTextureSrgb: colour by default, data maps by name")
{
	using Importer::suggestTextureSrgb;

	// Colour: plain names and the usual colour suffixes.
	CHECK(suggestTextureSrgb("Rock.png"));
	CHECK(suggestTextureSrgb("rock_albedo.png"));
	CHECK(suggestTextureSrgb("T_Rock_BaseColor.tga"));
	CHECK(suggestTextureSrgb("wood_diffuse_2k.jpg"));
	CHECK(suggestTextureSrgb("ui-icon.png"));
	CHECK(suggestTextureSrgb("Brick_Emissive.png"));

	// Data: the hint anywhere after the first token, any case, any separator.
	CHECK_FALSE(suggestTextureSrgb("rock_normal.png"));
	CHECK_FALSE(suggestTextureSrgb("rock_normal_2k.png"));
	CHECK_FALSE(suggestTextureSrgb("T_Rock_N.png"));
	CHECK_FALSE(suggestTextureSrgb("rock_nrm.png"));
	CHECK_FALSE(suggestTextureSrgb("Brick_ORM.tga"));
	CHECK_FALSE(suggestTextureSrgb("wood-rough.jpg"));
	CHECK_FALSE(suggestTextureSrgb("wood_Roughness.png"));
	CHECK_FALSE(suggestTextureSrgb("panel_metallic.png"));
	CHECK_FALSE(suggestTextureSrgb("cliff_AO.png"));
	CHECK_FALSE(suggestTextureSrgb("ground_height.png"));
	CHECK_FALSE(suggestTextureSrgb("ground_disp.png"));
	CHECK_FALSE(suggestTextureSrgb("decal mask.png"));
	CHECK_FALSE(suggestTextureSrgb("leaf_opacity.png"));
	CHECK_FALSE(suggestTextureSrgb("Content/Textures/rock_normal.hasset"));   // an asset path reads the same

	// Separator-less: only long, unambiguous words at the end.
	CHECK_FALSE(suggestTextureSrgb("BrickNormal.png"));
	CHECK_FALSE(suggestTextureSrgb("wallRoughness.png"));
	CHECK(suggestTextureSrgb("Lion.png"));      // ends in "n", is not a normal map
	CHECK(suggestTextureSrgb("Cacao.png"));     // ends in "ao", is not an AO map

	// The first token is the subject, not a map type.
	CHECK(suggestTextureSrgb("normal_test.png"));
	CHECK(suggestTextureSrgb("mask_of_zorro.png"));

	// HDR is radiance, linear by definition, whatever it is called.
	CHECK_FALSE(suggestTextureSrgb("sky_colour.hdr"));
	CHECK_FALSE(suggestTextureSrgb("Studio.HDR"));
}

TEST_CASE("importSource flags a texture by its name unless told otherwise")
{
	TempDir dir("he_test_srgb_import");
	const fs::path src  = dir.path / "src";
	const fs::path root = dir.path / "Content";
	fs::create_directories(src);
	fs::create_directories(root);
	writePng1x1(src / "rock_albedo.png");
	writePng1x1(src / "rock_normal.png");

	// The guess, which is what File > Import Asset and a bare importSource get.
	// Before the flag was threaded through, both of these came out linear.
	REQUIRE(Importer::importSource(src / "rock_albedo.png", root, "Textures"));
	REQUIRE(Importer::importSource(src / "rock_normal.png", root, "Textures"));
	CHECK(Importer::textureSrgbOf(root / "Textures" / "rock_albedo.hasset") == true);
	CHECK(Importer::textureSrgbOf(root / "Textures" / "rock_normal.hasset") == false);

	// And the loader sees the same byte the streaming read does.
	{
		ContentManager cm(root.string());
		const TextureAsset* t = cm.getTexture(cm.loadAsset("Textures/rock_albedo.hasset"));
		REQUIRE(t != nullptr);
		CHECK(t->srgb);
	}

	// The import dialog's choice wins over the name, in both directions.
	Importer::ImportOptions asData;   asData.textureSrgb   = false;
	Importer::ImportOptions asColour; asColour.textureSrgb = true;
	REQUIRE(Importer::importSource(src / "rock_albedo.png", root, "Override", {}, asData));
	REQUIRE(Importer::importSource(src / "rock_normal.png", root, "Override", {}, asColour));
	CHECK(Importer::textureSrgbOf(root / "Override" / "rock_albedo.hasset") == false);
	CHECK(Importer::textureSrgbOf(root / "Override" / "rock_normal.hasset") == true);

	// A non-texture answers nothing rather than "linear".
	CHECK_FALSE(Importer::textureSrgbOf(src / "rock_albedo.png").has_value());
	CHECK(Importer::isTextureSource(src / "rock_albedo.png"));
	CHECK_FALSE(Importer::isTextureSource(src / "rock.glb"));
}

TEST_CASE("Importer::reimport keeps the texture's sRGB flag instead of re-guessing it")
{
	TempDir dir("he_test_srgb_reimport");
	const fs::path src  = dir.path / "src";
	const fs::path root = dir.path / "Content";
	fs::create_directories(src);
	fs::create_directories(root);
	writePng1x1(src / "rock_albedo.png");
	writePng1x1(src / "rock_normal.png");

	// Both imported AGAINST their names, so a reimport that fell back to the
	// guess (or to the old linear default, for the albedo) would flip them.
	Importer::ImportOptions asData;   asData.textureSrgb   = false;
	Importer::ImportOptions asColour; asColour.textureSrgb = true;
	REQUIRE(Importer::importSource(src / "rock_albedo.png", root, "Textures", {}, asData));
	REQUIRE(Importer::importSource(src / "rock_normal.png", root, "Textures", {}, asColour));
	const fs::path albedo = root / "Textures" / "rock_albedo.hasset";
	const fs::path normal = root / "Textures" / "rock_normal.hasset";

	REQUIRE(Importer::reimport(albedo, root));
	REQUIRE(Importer::reimport(normal, root));
	CHECK(Importer::textureSrgbOf(albedo) == false);
	CHECK(Importer::textureSrgbOf(normal) == true);

	// Changed in place afterwards (the editor's colour-space menu) — the next
	// reimport keeps THAT.
	REQUIRE(Importer::setTextureSrgb(albedo, root, true));
	REQUIRE(Importer::reimport(albedo, root));
	CHECK(Importer::textureSrgbOf(albedo) == true);
}

TEST_CASE("Importer::reimport of a texture from before the flag takes the name guess")
{
	TempDir dir("he_test_srgb_reimport_pretail");
	const fs::path src  = dir.path / "src";
	const fs::path root = dir.path / "Content";
	fs::create_directories(src);
	writePng1x1(src / "rock_albedo.png");

	const fs::path asset = root / "Textures" / "rock_albedo.hasset";
	writePreTailTexture(asset, "Textures/rock_albedo.hasset", src / "rock_albedo.png");
	REQUIRE(Importer::sourceFileOf(asset) == (src / "rock_albedo.png").generic_string());
	CHECK_FALSE(Importer::textureSrgbOf(asset).has_value());   // nothing to keep

	REQUIRE(Importer::reimport(asset, root));
	CHECK(Importer::textureSrgbOf(asset) == true);              // guessed, not linear
	CHECK(HE::AssetRefs::assetUuidOfFile(asset.string()) == HE::UUID{0x5A6B, 0x7C8D});
}

TEST_CASE("Importer::setTextureSrgb rewrites only the flag")
{
	TempDir dir("he_test_srgb_set");
	const fs::path root = dir.path / "Content";
	fs::create_directories(root);

	// A texture with no recorded source — the kind the batch action is for, since
	// a reimport cannot reach it.
	{
		ContentManager cm(root.string());
		TextureAsset t;
		t.type     = HE::AssetType::Texture;
		t.name     = "Old";
		t.path     = "Textures/Old.hasset";
		t.width    = 2; t.height = 1; t.channels = 4;
		t.data     = { 1, 2, 3, 4, 5, 6, 7, 8 };
		REQUIRE(cm.saveAsset(t));
	}
	const fs::path file = root / "Textures" / "Old.hasset";
	const HE::UUID before = HE::AssetRefs::assetUuidOfFile(file.string());
	REQUIRE(before != HE::UUID{});
	CHECK(Importer::textureSrgbOf(file) == false);

	REQUIRE(Importer::setTextureSrgb(file, root, true));
	CHECK(Importer::textureSrgbOf(file) == true);
	CHECK(HE::AssetRefs::assetUuidOfFile(file.string()) == before);
	{
		ContentManager cm(root.string());
		const TextureAsset* t = cm.getTexture(cm.loadAsset("Textures/Old.hasset"));
		REQUIRE(t != nullptr);
		CHECK(t->srgb);
		CHECK(t->width == 2);
		CHECK(t->height == 1);
		CHECK(t->data == std::vector<uint8_t>{ 1, 2, 3, 4, 5, 6, 7, 8 });
	}
	REQUIRE(Importer::setTextureSrgb(file, root, true));   // already so: fine, no-op
	REQUIRE(Importer::setTextureSrgb(file, root, false));
	CHECK(Importer::textureSrgbOf(file) == false);

	// A pre-tail file has no byte to flip; the rewrite gives it the current layout.
	const fs::path legacy = root / "Textures" / "Legacy.hasset";
	writePreTailTexture(legacy, "Textures/Legacy.hasset", dir.path / "gone.png");
	REQUIRE(Importer::setTextureSrgb(legacy, root, true));
	CHECK(Importer::textureSrgbOf(legacy) == true);
	CHECK(Importer::sourceFileOf(legacy) == (dir.path / "gone.png").generic_string());

	// Refused: not a texture, and not under the content root.
	{
		ContentManager cm(root.string());
		ScriptAsset s;
		s.type = HE::AssetType::Script;
		s.name = "Spawner";
		s.path = "Scripts/Spawner.hasset";
		REQUIRE(cm.saveAsset(s));
	}
	CHECK_FALSE(Importer::setTextureSrgb(root / "Scripts" / "Spawner.hasset", root, true));
	CHECK_FALSE(Importer::textureSrgbOf(root / "Scripts" / "Spawner.hasset").has_value());
	TempDir outside("he_test_srgb_set_outside");
	CHECK_FALSE(Importer::setTextureSrgb(outside.path / "Stray.hasset", root, true));
}
