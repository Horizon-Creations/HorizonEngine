// BC1 / BC4 / BC5 next to BC7 / BC3 (Thema 180, Schritt 10): the format enum and its
// block-byte math, the pack-time encoders (2D and texture arrays), the CPU decoders the
// renderers fall back on, and the old on-disk forms. No GPU: the upload paths of the
// five backends are checked by their own builds and the CI image tests.
//
// What the round trip proves: pak -> cook -> load -> decode lands near the source.
// The tolerances below are for a smooth synthetic image (what BC1 and BC3's two
// endpoints per 4x4 block reproduce well); they pin "the encoder wrote valid blocks in
// the right channel order", not the encoders' rate-distortion.

#include "doctest.h"
#include "TestFsUtil.h"
#include <Hpak/HpakWriter.h>
#include <Hpak/HpakFormat.h>
#include <ContentManager/HAsset.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/TextureArrayBuild.h>
#include <ContentManager/TextureBlockDecode.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace
{

// A smooth image: every channel is a gentle ramp, the alpha one too.
TextureAsset rampTexture(uint32_t w, uint32_t h, int phase = 0)
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
			p[3] = static_cast<uint8_t>(255 - phase - x * 100 / w);
		}
	return t;
}

// Save `src` to a scratch dir, pack it with texture compression `fmt`, load the pak
// back and return the cooked asset (copied: the ContentManager goes out of scope).
bool cookViaPak(TextureAsset src, uint8_t fmt, uint8_t quality, const char* tag, TextureAsset& out)
{
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / (std::string("he_bcn_") + tag);
	he_test::removeAllQuiet(dir);
	fs::create_directories(dir);
	{
		ContentManager cmw(dir.string());
		if (!cmw.saveAsset(src)) return false;
	}
	Hpak::PackSettings s; s.codec = Hpak::Codec::Store; s.cook = true;
	s.textureCompression = fmt;
	s.textureQuality     = quality;
	HpakWriter packer; packer.addDirectory(dir, s);
	const fs::path pak = fs::temp_directory_path() / (std::string("he_bcn_") + tag + ".hpak");
	if (!packer.write(pak.string())) return false;

	ContentManager cm;
	if (!cm.loadPak(pak.string())) return false;
	const TextureAsset* t = cm.getTexture(src.id);
	if (!t) return false;
	out = *t;
	he_test::removeAllQuiet(dir);
	he_test::removeQuiet(pak);
	return true;
}

// The texel the hardware would hand a shader for `fmt`, from the RGBA8 source.
void expectedTexel(TextureFormat fmt, const uint8_t* src, int out[4])
{
	out[0] = src[0]; out[1] = src[1]; out[2] = src[2]; out[3] = src[3];
	switch (fmt)
	{
	case TextureFormat::BC1: out[3] = 255; break;
	case TextureFormat::BC4: out[1] = 0; out[2] = 0; out[3] = 255; break;
	case TextureFormat::BC5: out[2] = 0; out[3] = 255; break;
	default: break;
	}
}

struct Err { int maxAbs = 0; double mean = 0.0; };

// Compare a decoded RGBA8 level against the source level (both w*h*4).
Err compareLevel(TextureFormat fmt, const uint8_t* src, const uint8_t* dec, uint32_t w, uint32_t h)
{
	Err e; long long sum = 0, n = 0;
	for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
	{
		int want[4];
		expectedTexel(fmt, src + i * 4, want);
		for (int c = 0; c < 4; ++c)
		{
			const int d = std::abs(want[c] - int(dec[i * 4 + c]));
			e.maxAbs = std::max(e.maxAbs, d);
			sum += d; ++n;
		}
	}
	e.mean = n ? double(sum) / double(n) : 0.0;
	return e;
}

} // namespace

TEST_CASE("TextureFormat: new values are appended, the on-disk values of the old ones are unchanged")
{
	// The value is the TXMI format byte of every cooked texture ever written.
	CHECK(static_cast<int>(TextureFormat::RGBA8)    == 0);
	CHECK(static_cast<int>(TextureFormat::ASTC_4x4) == 1);
	CHECK(static_cast<int>(TextureFormat::BC7)      == 2);
	CHECK(static_cast<int>(TextureFormat::BC3)      == 3);
	CHECK(static_cast<int>(TextureFormat::BC1)      == 4);
	CHECK(static_cast<int>(TextureFormat::BC4)      == 5);
	CHECK(static_cast<int>(TextureFormat::BC5)      == 6);
}

TEST_CASE("TextureFormat: block bytes and level sizes")
{
	CHECK_FALSE(textureFormatIsBlock4x4(TextureFormat::RGBA8));
	for (TextureFormat f : { TextureFormat::ASTC_4x4, TextureFormat::BC7, TextureFormat::BC3,
	                         TextureFormat::BC1, TextureFormat::BC4, TextureFormat::BC5 })
		CHECK(textureFormatIsBlock4x4(f));

	CHECK(textureFormatBlockBytes(TextureFormat::RGBA8)    == 0);
	CHECK(textureFormatBlockBytes(TextureFormat::ASTC_4x4) == 16);
	CHECK(textureFormatBlockBytes(TextureFormat::BC7)      == 16);
	CHECK(textureFormatBlockBytes(TextureFormat::BC3)      == 16);
	CHECK(textureFormatBlockBytes(TextureFormat::BC5)      == 16);
	CHECK(textureFormatBlockBytes(TextureFormat::BC1)      == 8);
	CHECK(textureFormatBlockBytes(TextureFormat::BC4)      == 8);

	// 10x6 texels: 3 x 2 blocks (partial blocks at the right and bottom edge count whole).
	CHECK(textureLevelBytes(TextureFormat::RGBA8, 10, 6) == 10u * 6 * 4);
	CHECK(textureLevelBytes(TextureFormat::BC7,   10, 6) == 3u * 2 * 16);
	CHECK(textureLevelBytes(TextureFormat::BC1,   10, 6) == 3u * 2 * 8);
	CHECK(textureLevelBytes(TextureFormat::BC4,   1, 1)  == 8u);   // the 1x1 tail costs one block
	CHECK(textureLevelBytes(TextureFormat::BC5,   2, 2)  == 16u);
}

TEST_CASE("textureArraySliceBytes: the block math of every format, RGBA8 unchanged")
{
	// 16x8 with 5 levels: 16x8, 8x4, 4x2, 2x1, 1x1.
	CHECK(HE::textureArraySliceBytes(16, 8, 5) == (128 + 32 + 8 + 2 + 1) * 4u);                // default = RGBA8
	CHECK(HE::textureArraySliceBytes(16, 8, 5, TextureFormat::RGBA8) == (128 + 32 + 8 + 2 + 1) * 4u);
	// Blocks: 4x2=8, 2x1=2, 1x1=1 (4x2 texels = 1 x 1 block), 1, 1 -> 8+2+1+1+1 = 13 blocks.
	CHECK(HE::textureArraySliceBytes(16, 8, 5, TextureFormat::BC7) == 13u * 16);
	CHECK(HE::textureArraySliceBytes(16, 8, 5, TextureFormat::BC3) == 13u * 16);
	CHECK(HE::textureArraySliceBytes(16, 8, 5, TextureFormat::BC5) == 13u * 16);
	CHECK(HE::textureArraySliceBytes(16, 8, 5, TextureFormat::BC1) == 13u * 8);
	CHECK(HE::textureArraySliceBytes(16, 8, 5, TextureFormat::BC4) == 13u * 8);

	// Offsets walk the same math: slice 1, level 0 starts one whole slice in.
	CHECK(HE::textureArrayOffset(16, 8, 5, 1, 0, TextureFormat::BC1) == 13u * 8);
	CHECK(HE::textureArrayOffset(16, 8, 5, 2, 2, TextureFormat::BC4) == 2 * 13u * 8 + (8 + 2) * 8);
	CHECK(HE::textureArrayOffset(16, 8, 5, 1, 1) == (128 + 32 + 8 + 2 + 1) * 4u + 128 * 4u);

	// A payload is valid only with exactly layers x the format's slice size.
	TextureAsset a;
	a.width = 16; a.height = 8; a.channels = 4; a.mipLevels = 5; a.layers = 3;
	a.format = TextureFormat::BC1;
	a.data.assign(3 * 13u * 8, 0);
	CHECK(HE::textureArrayPayloadValid(a));
	a.data.push_back(0);
	CHECK_FALSE(HE::textureArrayPayloadValid(a));
	a.format = TextureFormat::BC7;           // right size for BC1, wrong for BC7
	a.data.assign(3 * 13u * 8, 0);
	CHECK_FALSE(HE::textureArrayPayloadValid(a));
	a.data.assign(3 * 13u * 16, 0);
	CHECK(HE::textureArrayPayloadValid(a));
	a.format = static_cast<TextureFormat>(99);   // an unknown format is not a payload
	CHECK_FALSE(HE::textureArrayPayloadValid(a));
}

TEST_CASE("Block decoders: hand-built blocks")
{
	uint8_t px[16 * 4];

	SUBCASE("BC4 in 8-value mode (a0 > a1) and in 6-value mode (a0 <= a1)")
	{
		// Indices 0..7 cycling over the 16 texels, packed 3 bits each, low bits first.
		uint8_t blk[8] = { 255, 0, 0, 0, 0, 0, 0, 0 };
		uint64_t bits = 0;
		for (int t = 0; t < 16; ++t) bits |= uint64_t(t & 7) << (3 * t);
		for (int i = 0; i < 6; ++i) blk[2 + i] = static_cast<uint8_t>(bits >> (8 * i));
		REQUIRE(HE::decodeBlockLevelToRGBA8(TextureFormat::BC4, blk, sizeof(blk), 4, 4, px));
		CHECK(px[0 * 4] == 255);                       // index 0 = a0
		CHECK(px[1 * 4] == 0);                         // index 1 = a1
		CHECK(px[2 * 4] == (6 * 255 + 1 * 0) / 7);     // index 2 = 6/7 a0 + 1/7 a1
		CHECK(px[7 * 4] == (1 * 255 + 6 * 0) / 7);     // index 7
		CHECK(px[0 * 4 + 1] == 0);                     // BC4 reads (r, 0, 0, 1)
		CHECK(px[0 * 4 + 3] == 255);

		blk[0] = 0; blk[1] = 255;                      // a0 <= a1: six interpolants + 0 + 255
		REQUIRE(HE::decodeBlockLevelToRGBA8(TextureFormat::BC4, blk, sizeof(blk), 4, 4, px));
		CHECK(px[2 * 4] == (4 * 0 + 1 * 255) / 5);
		CHECK(px[6 * 4] == 0);                         // index 6 = 0
		CHECK(px[7 * 4] == 255);                       // index 7 = 255
	}

	SUBCASE("BC1 four-colour and three-colour (transparent) mode")
	{
		// c0 = pure red 5:6:5 (0xF800), c1 = pure blue (0x001F); all texels index 3.
		uint8_t blk[8] = { 0x00, 0xF8, 0x1F, 0x00, 0xFF, 0xFF, 0xFF, 0xFF };
		REQUIRE(HE::decodeBlockLevelToRGBA8(TextureFormat::BC1, blk, sizeof(blk), 4, 4, px));
		CHECK(px[0] == (255 + 2 * 0) / 3);             // index 3 = (c0 + 2 c1) / 3
		CHECK(px[2] == (0 + 2 * 255) / 3);
		CHECK(px[3] == 255);

		// c0 <= c1: three-colour mode, index 3 is transparent black.
		std::swap(blk[0], blk[2]); std::swap(blk[1], blk[3]);
		REQUIRE(HE::decodeBlockLevelToRGBA8(TextureFormat::BC1, blk, sizeof(blk), 4, 4, px));
		CHECK(px[0] == 0);
		CHECK(px[1] == 0);
		CHECK(px[2] == 0);
		CHECK(px[3] == 0);
	}

	SUBCASE("a level with a partial edge block writes only inside the image")
	{
		uint8_t blk[8] = { 200, 200, 0, 0, 0, 0, 0, 0 };    // every texel index 0 -> 200
		std::vector<uint8_t> dst(2 * 3 * 4 + 4, 0x55);      // 2x3 texels + a guard texel
		REQUIRE(HE::decodeBlockLevelToRGBA8(TextureFormat::BC4, blk, sizeof(blk), 2, 3, dst.data()));
		for (int i = 0; i < 6; ++i) CHECK(dst[i * 4] == 200);
		CHECK(dst[6 * 4] == 0x55);                          // not touched
	}

	SUBCASE("refusals")
	{
		uint8_t blk[16] = {};
		CHECK_FALSE(HE::decodeBlockLevelToRGBA8(TextureFormat::BC7, blk, sizeof(blk), 4, 4, px));       // no decoder
		CHECK_FALSE(HE::decodeBlockLevelToRGBA8(TextureFormat::BC1, blk, 4, 4, 4, px));                 // too short
		CHECK_FALSE(HE::decodeBlockLevelToRGBA8(TextureFormat::BC5, blk, sizeof(blk), 0, 4, px));
		CHECK_FALSE(HE::textureFormatCpuDecodable(TextureFormat::BC7));
		CHECK_FALSE(HE::textureFormatCpuDecodable(TextureFormat::ASTC_4x4));
		CHECK(HE::textureFormatCpuDecodable(TextureFormat::BC5));
	}
}

#ifdef HE_HAVE_STB_DXT

// Round trip through the packer: size, mip chain and decoded closeness, per format.
// Tolerances (max abs error / mean abs error over all four channels, level 0 of a
// 16x16 smooth ramp; measured, then rounded up): BC1 and BC3 quantise colour to 5:6:5
// endpoints plus four palette steps; BC4/BC5 use eight steps between two 8-bit
// endpoints, so a ramp that spans < 120 values is reproduced to about 1-2.
TEST_CASE("Cook: BC1/BC3/BC4/BC5 round trip through the pak, decode lands near the source")
{
	struct Case { TextureFormat fmt; uint8_t id; int maxAbs; double mean; const char* tag; };
	const Case cases[] = {
		{ TextureFormat::BC1, 4, 20, 4.5, "bc1" },
		{ TextureFormat::BC3, 3, 20, 4.5, "bc3" },
		{ TextureFormat::BC4, 5,  4, 1.0, "bc4" },
		{ TextureFormat::BC5, 6,  4, 1.0, "bc5" },
	};
	for (const Case& c : cases)
	{
		for (uint8_t quality : { uint8_t(0), uint8_t(2) })
		{
			CAPTURE(std::string(c.tag)); CAPTURE(int(quality));
			const TextureAsset src = rampTexture(16, 16);
			TextureAsset cooked;
			REQUIRE(cookViaPak(src, c.id, quality, c.tag, cooked));
			CHECK(cooked.format == c.fmt);
			CHECK(cooked.mipLevels == 5);                         // 16,8,4,2,1
			CHECK(cooked.layers == 1);
			CHECK(cooked.width == 16);
			CHECK(cooked.height == 16);
			CHECK(cooked.data.size() == HE::textureArraySliceBytes(16, 16, 5, c.fmt));

			TextureAsset dec;
			REQUIRE(HE::decodeTextureToRGBA8(cooked, dec));
			REQUIRE(dec.data.size() == HE::textureArraySliceBytes(16, 16, 5));
			const Err e = compareLevel(c.fmt, src.data.data(), dec.data.data(), 16, 16);
			CHECK(e.maxAbs <= c.maxAbs);
			CHECK(e.mean <= c.mean);
		}
	}
}

TEST_CASE("Cook: BC4 keeps R, BC5 keeps R and G, and the dropped channels decode to 0 / 255")
{
	// A source whose channels are all different, so a channel mix-up cannot pass.
	TextureAsset src = rampTexture(8, 8);
	for (size_t i = 0; i < 64; ++i) { src.data[i * 4] = 220; src.data[i * 4 + 1] = 90; src.data[i * 4 + 2] = 33; src.data[i * 4 + 3] = 128; }

	TextureAsset c4, c5, d4, d5;
	REQUIRE(cookViaPak(src, 5, 0, "bc4solid", c4));
	REQUIRE(cookViaPak(src, 6, 0, "bc5solid", c5));
	REQUIRE(HE::decodeTextureToRGBA8(c4, d4));
	REQUIRE(HE::decodeTextureToRGBA8(c5, d5));
	CHECK(std::abs(int(d4.data[0]) - 220) <= 1);
	CHECK(d4.data[1] == 0);   CHECK(d4.data[2] == 0);   CHECK(d4.data[3] == 255);
	CHECK(std::abs(int(d5.data[0]) - 220) <= 1);
	CHECK(std::abs(int(d5.data[1]) - 90)  <= 1);
	CHECK(d5.data[2] == 0);   CHECK(d5.data[3] == 255);   // no Z: a normal read as .xyz needs it rebuilt
}

TEST_CASE("Cook: BC1 byte size is half of BC3, the srgb flag survives")
{
	TextureAsset src = rampTexture(8, 8);
	src.srgb = true;
	TextureAsset b1, b3;
	REQUIRE(cookViaPak(src, 4, 0, "bc1size", b1));
	REQUIRE(cookViaPak(src, 3, 0, "bc3size", b3));
	CHECK(b1.srgb);
	CHECK(b3.srgb);
	CHECK(b1.data.size() * 2 == b3.data.size());
	CHECK(b1.data.size() == (4 + 1 + 1 + 1) * 8);         // 8x8, 4x4, 2x2, 1x1
}

TEST_CASE("Cook: an unknown target format keeps RGBA8 with the baked mip chain")
{
	// 7+ is no TextureFormat: the cook has no encoder for it and must fall back, not fail.
	const TextureAsset src = rampTexture(8, 8);
	TextureAsset cooked;
	REQUIRE(cookViaPak(src, 7, 0, "unknownfmt", cooked));
	CHECK(cooked.format == TextureFormat::RGBA8);
	CHECK(cooked.mipLevels == 4);
	CHECK(cooked.data.size() == HE::textureArraySliceBytes(8, 8, 4));
	CHECK(std::equal(src.data.begin(), src.data.end(), cooked.data.begin()));   // level 0 untouched
}

TEST_CASE("Cook: a texture ARRAY is compressed slice by slice and level by level")
{
	// Slices that differ strongly in R and alpha (phase 0 / 45 / 90), so a slice or level
	// offset that is off by one cannot hide inside the tolerance.
	const TextureAsset s0 = rampTexture(32, 16, 0), s1 = rampTexture(32, 16, 45), s2 = rampTexture(32, 16, 90);
	TextureAsset arr;
	std::string err;
	REQUIRE_MESSAGE(HE::buildTextureArray({ &s0, &s1, &s2 }, arr, /*bakeMips=*/true, &err), err);
	arr.type = HE::AssetType::Texture; arr.name = "ramp_arr"; arr.path = "ramp_arr.hasset"; arr.srgb = true;
	REQUIRE(arr.layers == 3);
	REQUIRE(arr.mipLevels == 6);                         // 32x16, 16x8, 8x4, 4x2, 2x1, 1x1

	// Tolerance per level. BC1/BC3 keep two colour endpoints and two interpolants per 4x4
	// block: down to 16x8 a smooth ramp stays within ~20 (measured 26 at 8x4), but the
	// 4x2 / 2x1 tail holds a whole gradient in ONE block (a plane in RGB, not a line),
	// where a 4-colour palette cannot follow it (measured 58). That is the format, so the
	// tail gets a loose bound; the 1x1 level is a single colour again. Slice and level
	// addressing is pinned by BC4/BC5 at +-6 over the very same loop.
	auto tolerance = [](TextureFormat f, uint32_t lw, uint32_t lh) {
		const bool oneChannelRamp = f == TextureFormat::BC4 || f == TextureFormat::BC5;
		if (oneChannelRamp) return 6;
		if (lw == 1 && lh == 1) return 12;
		return lw >= 8 ? 28 : 64;
	};

	struct Case { TextureFormat fmt; uint8_t id; const char* tag; };
	const Case cases[] = {
		{ TextureFormat::BC1, 4, "arrbc1" }, { TextureFormat::BC3, 3, "arrbc3" },
		{ TextureFormat::BC4, 5, "arrbc4" }, { TextureFormat::BC5, 6, "arrbc5" },
	};
	for (const Case& c : cases)
	{
		CAPTURE(std::string(c.tag));
		TextureAsset cooked;
		REQUIRE(cookViaPak(arr, c.id, 1, c.tag, cooked));
		CHECK(cooked.format == c.fmt);
		CHECK(cooked.layers == 3);                       // the slice count rides in the TXMI tail
		CHECK(cooked.mipLevels == 6);                    // not rebuilt, not dropped
		CHECK(cooked.srgb);
		CHECK(cooked.data.size() == 3 * HE::textureArraySliceBytes(32, 16, 6, c.fmt));
		CHECK(HE::textureArrayPayloadValid(cooked));

		TextureAsset dec;
		REQUIRE(HE::decodeTextureToRGBA8(cooked, dec));
		CHECK(dec.layers == 3);
		CHECK(HE::textureArrayPayloadValid(dec));
		// Every slice and every level, against the array that went in (its levels were
		// baked by buildTextureArray; the cook must encode them, not rebuild them).
		for (uint32_t s = 0; s < 3; ++s)
		{
			uint32_t lw = 32, lh = 16;
			for (uint32_t l = 0; l < 6; ++l)
			{
				const Err e = compareLevel(c.fmt,
					arr.data.data() + HE::textureArrayOffset(32, 16, 6, s, l),
					dec.data.data() + HE::textureArrayOffset(32, 16, 6, s, l), lw, lh);
				CAPTURE(s); CAPTURE(l);
				CHECK(e.maxAbs <= tolerance(c.fmt, lw, lh));
				lw = std::max<uint32_t>(1, lw >> 1); lh = std::max<uint32_t>(1, lh >> 1);
			}
		}
	}
}

TEST_CASE("Cook: an array that is already compressed, or has no target, ships unchanged")
{
	const TextureAsset s0 = rampTexture(8, 8, 0), s1 = rampTexture(8, 8, 30);
	TextureAsset arr;
	REQUIRE(HE::buildTextureArray({ &s0, &s1 }, arr, true));
	arr.type = HE::AssetType::Texture; arr.name = "keep_arr"; arr.path = "keep_arr.hasset";

	// No compression asked: the RGBA8 array is not rewritten.
	TextureAsset none;
	REQUIRE(cookViaPak(arr, 0, 0, "arrnone", none));
	CHECK(none.format == TextureFormat::RGBA8);
	CHECK(none.layers == 2);
	CHECK(none.data == arr.data);

	// Packing the BC array again must not encode blocks as if they were RGBA8.
	TextureAsset bc, again;
	REQUIRE(cookViaPak(arr, 2, 0, "arrfirst", bc));           // BC7 where built, else RGBA8: either way valid
	REQUIRE(cookViaPak(bc, 4, 0, "arrsecond", again));
	CHECK(again.format == bc.format);
	CHECK(again.data == bc.data);
}

#endif // HE_HAVE_STB_DXT

TEST_CASE("Textures written before BC1/BC4/BC5 existed still load")
{
	// The TXMI byte layout of a cooked texture, current form: width, height, channels
	// (u32 each), mipLevels u32, format u8, srgb u8 - 18 bytes, no layers tail.
	auto blob = [](const HE::UUID& id, const char* name, uint8_t fmt, size_t pixelBytes)
	{
		std::vector<uint8_t> meta;
		HAsset::Writer::appendPOD(meta, static_cast<uint16_t>(HE::AssetType::Texture));
		HAsset::Writer::appendPOD(meta, id.hi);
		HAsset::Writer::appendPOD(meta, id.lo);
		HAsset::Writer::appendString(meta, name);
		HAsset::Writer::appendString(meta, std::string("mem://") + name);
		std::vector<uint8_t> txmi;
		HAsset::Writer::appendPOD(txmi, uint32_t(8));
		HAsset::Writer::appendPOD(txmi, uint32_t(8));
		HAsset::Writer::appendPOD(txmi, uint32_t(4));
		HAsset::Writer::appendPOD(txmi, uint32_t(1));
		HAsset::Writer::appendPOD(txmi, fmt);
		HAsset::Writer::appendPOD(txmi, uint8_t(1));
		CHECK(txmi.size() == 18);
		const std::vector<uint8_t> pixels(pixelBytes, 0x7F);
		HAsset::Writer w;
		w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
		w.addChunk(HAsset::CHUNK_TXMI, txmi.data(), txmi.size());
		w.addChunk(HAsset::CHUNK_PIXL, pixels.data(), pixels.size());
		return w.toBytes(static_cast<uint16_t>(HE::AssetType::Texture));
	};

	ContentManager cm;
	const HE::UUID bc3{0x7E57, 1}, bc7{0x7E57, 2}, astc{0x7E57, 3}, rgba{0x7E57, 4};
	REQUIRE(cm.loadAssetFromMemory(blob(bc3,  "old_bc3",  3, 4 * 16)) == bc3);
	REQUIRE(cm.loadAssetFromMemory(blob(bc7,  "old_bc7",  2, 4 * 16)) == bc7);
	REQUIRE(cm.loadAssetFromMemory(blob(astc, "old_astc", 1, 4 * 16)) == astc);
	REQUIRE(cm.loadAssetFromMemory(blob(rgba, "old_rgba", 0, 8 * 8 * 4)) == rgba);
	CHECK(cm.getTexture(bc3)->format  == TextureFormat::BC3);
	CHECK(cm.getTexture(bc7)->format  == TextureFormat::BC7);
	CHECK(cm.getTexture(astc)->format == TextureFormat::ASTC_4x4);
	CHECK(cm.getTexture(rgba)->format == TextureFormat::RGBA8);
	CHECK(cm.getTexture(bc3)->layers == 1);
	CHECK(cm.getTexture(bc3)->srgb);
	// 8x8 BC3 = 2 x 2 blocks x 16 B: the block math agrees with the stored payload.
	CHECK(cm.getTexture(bc3)->data.size() == textureLevelBytes(TextureFormat::BC3, 8, 8));

	// And the new values travel the same chunk: a format byte 4/5/6 loads as BC1/BC4/BC5.
	const HE::UUID b1{0x7E57, 5}, b4{0x7E57, 6}, b5{0x7E57, 7};
	REQUIRE(cm.loadAssetFromMemory(blob(b1, "new_bc1", 4, 4 * 8)) == b1);
	REQUIRE(cm.loadAssetFromMemory(blob(b4, "new_bc4", 5, 4 * 8)) == b4);
	REQUIRE(cm.loadAssetFromMemory(blob(b5, "new_bc5", 6, 4 * 16)) == b5);
	CHECK(cm.getTexture(b1)->format == TextureFormat::BC1);
	CHECK(cm.getTexture(b4)->format == TextureFormat::BC4);
	CHECK(cm.getTexture(b5)->format == TextureFormat::BC5);
	CHECK(cm.getTexture(b1)->data.size() == textureLevelBytes(TextureFormat::BC1, 8, 8));
}
