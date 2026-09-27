#include "doctest.h"
#include "TextureImporter.h"
#include "AssetThumbnailCache.h"
#include "TestFsUtil.h"

#include <Backends/Software/SoftwareRaster.h>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <Renderer/UIRenderObject.h>
#include <UIWidget/UIElements.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// ── Which way up an imported picture lands, end to end ───────────────────────
// Reported (Thema 92): a picture put on a UI Image came out mirrored — upside
// down, top and bottom swapped; left and right were always right. The chain:
//
//   1. TextureImporter stores the rows BOTTOM-UP (flipVertically defaults to
//      true) — the engine's GL-style convention, v = 0 at the bottom. The mesh
//      importers flip their V to match (ImporterCommon.cpp, the glTF path), so
//      this is load-bearing for every textured mesh and stays.
//   2. Every backend uploads TextureAsset::data as-is, row 0 first, so texel
//      row 0 (v = 0) is the picture's BOTTOM row on all five of them.
//   3. A UI quad has its uvMin at its TOP-left corner — the UI's own y-down
//      convention — and the UI shaders pass it through unchanged.
//
// The fix sits between 2 and 3: the UI's quad helper (UIElement.cpp) turns v
// around for every textured quad, and the thumbnail cache reads the stored
// rows from the bottom. These cases pin the whole chain with a picture whose
// four corners tell every mirroring apart, so a regression in any link shows
// up as a wrong colour in a named corner.

namespace fs = std::filesystem;

namespace
{
	// A picture of four solid quadrants, each `half` pixels square:
	//   top-left red,     top-right green,
	//   bottom-left blue, bottom-right white.
	// Binary PPM rather than PNG so the fixture needs no encoder; stb_image
	// reads it through the same stbi_load_from_memory the PNG path takes.
	std::vector<uint8_t> quadrantPpm(int half = 1)
	{
		const int n = half * 2;
		const std::string header = "P6\n" + std::to_string(n) + " " + std::to_string(n) + "\n255\n";
		std::vector<uint8_t> b(header.begin(), header.end());
		for (int y = 0; y < n; ++y)          // file order: top row first
			for (int x = 0; x < n; ++x)
			{
				const bool right = x >= half, bottom = y >= half;
				const uint8_t r = (!bottom && !right) || (bottom && right) ? 255 : 0;
				const uint8_t g = (!bottom &&  right) || (bottom && right) ? 255 : 0;
				const uint8_t bl = bottom ? 255 : 0;
				b.push_back(r); b.push_back(g); b.push_back(bl);
			}
		return b;
	}

	struct Rgb { int r, g, b; };
	Rgb at(const TextureAsset& t, int x, int y)
	{
		const size_t o = (static_cast<size_t>(y) * t.width + x) * 4;
		return { t.data[o], t.data[o + 1], t.data[o + 2] };
	}
	// A few steps of slack: the rasteriser's filter and blend may round 255 to 254.
	bool is(const Rgb& c, int r, int g, int b)
	{
		auto near = [](int a, int e) { return a >= e - 8 && a <= e + 8; };
		return near(c.r, r) && near(c.g, g) && near(c.b, b);
	}

	// Draw one element's quads with the software rasteriser — a port of
	// uiFragment / kUIFS that samples the same way the Metal and GL UI shaders
	// do (texel row 0 at v = 0) — and hand back the 64x64 target.
	HE::sw::Image draw(const HE::UIElement& e, const TextureAsset& tex)
	{
		std::vector<UIRenderObject> out;
		e.render(HE::UIWidgetRect{ 0.0f, 0.0f, 64.0f, 64.0f }, HE::UIElementRenderState{},
		         HE::UUID{}, 1.0f, out);
		HE::sw::Image target;
		target.resize(64, 64);
		target.clear(0, 0, 0, 255);
		struct Src { const TextureAsset* t; } src{ &tex };
		HE::sw::draw(target, out, glm::vec4(0.0f),
			[](const HE::UUID&, void* user) -> HE::sw::TextureView {
				const TextureAsset* t = static_cast<Src*>(user)->t;
				return { t->data.data(), static_cast<int>(t->width), static_cast<int>(t->height) };
			}, &src);
		return target;
	}
	Rgb px(const HE::sw::Image& img, int x, int y)
	{
		std::uint8_t r, g, b, a;
		img.pixel(x, y, r, g, b, a);
		return { r, g, b };
	}
	// The picture the right way up: sampled well inside each quadrant, away
	// from the bilinear seam.
	void checkUpright(const HE::sw::Image& img)
	{
		CHECK(is(px(img,  8,  8), 255,   0,   0));   // top-left     red
		CHECK(is(px(img, 56,  8),   0, 255,   0));   // top-right    green
		CHECK(is(px(img,  8, 56),   0,   0, 255));   // bottom-left  blue
		CHECK(is(px(img, 56, 56), 255, 255, 255));   // bottom-right white
	}
}

TEST_CASE("Texture import stores rows bottom-up and leaves columns alone")
{
	const std::vector<uint8_t> ppm = quadrantPpm();

	SUBCASE("default settings: the picture's bottom row is data row 0")
	{
		auto t = TextureImporter::decodeFromMemory(ppm.data(), ppm.size());
		REQUIRE(t);
		REQUIRE(t->width == 2);
		REQUIRE(t->height == 2);
		// Row 0 is the SOURCE bottom row (blue, white) …
		CHECK(is(at(*t, 0, 0),   0,   0, 255));
		CHECK(is(at(*t, 1, 0), 255, 255, 255));
		// … row 1 the source top row (red, green).
		CHECK(is(at(*t, 0, 1), 255,   0,   0));
		CHECK(is(at(*t, 1, 1),   0, 255,   0));
		// Left stays left: the import mirrors vertically only, never along X.
	}

	SUBCASE("flipVertically off: rows in file order, top first")
	{
		TextureImporter::ImportSettings s;
		s.flipVertically = false;
		auto t = TextureImporter::decodeFromMemory(ppm.data(), ppm.size(), s);
		REQUIRE(t);
		CHECK(is(at(*t, 0, 0), 255,   0,   0));
		CHECK(is(at(*t, 1, 0),   0, 255,   0));
		CHECK(is(at(*t, 0, 1),   0,   0, 255));
		CHECK(is(at(*t, 1, 1), 255, 255, 255));
	}
}

TEST_CASE("UI Image: a textured quad reads the picture's top row at its top edge")
{
	// With link 1 above, v = 1 is the picture's top row — so that is what the
	// quad's top edge (uvMin) has to read. Columns are untouched.
	HE::UIImage img;
	img.textureAssetId = HE::UUID::generate();
	std::vector<UIRenderObject> out;
	img.render(HE::UIWidgetRect{ 10.0f, 20.0f, 64.0f, 32.0f }, HE::UIElementRenderState{},
	           HE::UUID{}, 1.0f, out);
	REQUIRE(out.size() == 1);
	CHECK(out[0].uvMin.x == doctest::Approx(0.0f));
	CHECK(out[0].uvMin.y == doctest::Approx(1.0f));   // top edge samples v = 1
	CHECK(out[0].uvMax.x == doctest::Approx(1.0f));
	CHECK(out[0].uvMax.y == doctest::Approx(0.0f));

	SUBCASE("an untextured quad keeps its plain 0..1 (nothing to turn around)")
	{
		HE::UIImage bare;
		out.clear();
		bare.render(HE::UIWidgetRect{ 0.0f, 0.0f, 8.0f, 8.0f }, HE::UIElementRenderState{},
		            HE::UUID{}, 1.0f, out);
		REQUIRE(out.size() == 1);
		CHECK(out[0].uvMin.y == doctest::Approx(0.0f));
		CHECK(out[0].uvMax.y == doctest::Approx(1.0f));
	}
}

TEST_CASE("UI Image end to end: an imported picture is drawn the right way up")
{
	// The whole chain in one go, on the CPU: import with the settings the
	// editor's Import Asset uses, the Image's own quads, the software rasteriser.
	const std::vector<uint8_t> ppm = quadrantPpm();
	auto tex = TextureImporter::decodeFromMemory(ppm.data(), ppm.size());
	REQUIRE(tex);

	HE::UIImage img;
	img.textureAssetId = HE::UUID::generate();
	checkUpright(draw(img, *tex));

	SUBCASE("Flip Vertical turns the now upright picture upside down, and only that")
	{
		img.flipV = true;
		const HE::sw::Image f = draw(img, *tex);
		CHECK(is(px(f,  8,  8),   0,   0, 255));   // blue on top
		CHECK(is(px(f, 56,  8), 255, 255, 255));
		CHECK(is(px(f,  8, 56), 255,   0,   0));   // red below
		CHECK(is(px(f, 56, 56),   0, 255,   0));
	}
	SUBCASE("Flip Horizontal swaps left and right, top stays top")
	{
		img.flipH = true;
		const HE::sw::Image f = draw(img, *tex);
		CHECK(is(px(f,  8,  8),   0, 255,   0));   // green top-left
		CHECK(is(px(f, 56,  8), 255,   0,   0));   // red top-right
		CHECK(is(px(f,  8, 56), 255, 255, 255));
		CHECK(is(px(f, 56, 56),   0,   0, 255));
	}
}

TEST_CASE("UI Image 9-slice end to end: Slice Top is the top of the picture")
{
	// 32x32 picture, 16-px quadrants. Top margin 16, bottom margin 8: the top
	// row of pieces must come from the picture's top half (red/green) at full
	// 16 px, the bottom row from the picture's bottom quarter (blue/white). Were
	// the rows read the wrong way round, the top corners would show blue/white.
	const std::vector<uint8_t> ppm = quadrantPpm(16);
	auto tex = TextureImporter::decodeFromMemory(ppm.data(), ppm.size());
	REQUIRE(tex);

	HE::UIImage img;
	img.textureAssetId = HE::UUID::generate();
	img.textureW = img.textureH = 32;
	img.sliceLeft = img.sliceRight = img.sliceTop = 16.0f;
	img.sliceBottom = 8.0f;

	std::vector<UIRenderObject> out;
	img.render(HE::UIWidgetRect{ 0.0f, 0.0f, 64.0f, 64.0f }, HE::UIElementRenderState{},
	           HE::UUID{}, 1.0f, out);
	REQUIRE_FALSE(out.empty());
	// Top-left piece: picture rows 0..16 from the top = stored v 1..0.5.
	CHECK(out[0].uvMin.y == doctest::Approx(1.0f));
	CHECK(out[0].uvMax.y == doctest::Approx(0.5f));

	const HE::sw::Image f = draw(img, *tex);
	CHECK(is(px(f,  8,  8), 255,   0,   0));   // top-left corner     red
	CHECK(is(px(f, 56,  8),   0, 255,   0));   // top-right corner    green
	CHECK(is(px(f,  4, 60),   0,   0, 255));   // bottom-left corner  blue
	CHECK(is(px(f, 60, 60), 255, 255, 255));   // bottom-right corner white
}

TEST_CASE("UI Panel with a texture goes through the same turn")
{
	// Every element with a Texture slot emits through the one quad helper, so
	// Panel (and Button, List, …) are upright too — not only Image.
	const std::vector<uint8_t> ppm = quadrantPpm();
	auto tex = TextureImporter::decodeFromMemory(ppm.data(), ppm.size());
	REQUIRE(tex);

	HE::UIPanel panel;
	panel.color = glm::vec4(1.0f);   // untinted, so the corners read as-is
	panel.textureAssetId = HE::UUID::generate();
	checkUpright(draw(panel, *tex));
}

TEST_CASE("Texture tile (Content Browser, designer preview) is the right way up")
{
	// The thumbnail cache is where the designer preview and the Content Browser
	// get their picture from; it reads TextureAsset rows itself, not through the
	// UI quad, so it needs its own turn — and its own pin.
	const fs::path dir = fs::temp_directory_path() / "he_tex_orientation_tile";
	he_test::removeAllQuiet(dir);
	fs::create_directories(dir);
	ContentManager cm(dir.string());
	AssetThumbnailCache::setContext(nullptr, &cm, dir.string());

	const std::vector<uint8_t> ppm = quadrantPpm(16);
	auto tex = TextureImporter::decodeFromMemory(ppm.data(), ppm.size());
	REQUIRE(tex);
	tex->type = HE::AssetType::Texture;
	tex->name = "quadrants";
	const HE::UUID id = cm.registerTexture(std::move(*tex));

	std::vector<uint8_t> out;
	REQUIRE(AssetThumbnailCache::textureThumbnail(id, out));
	const int S = static_cast<int>(AssetThumbnailCache::thumbnailSize());
	REQUIRE(out.size() == static_cast<size_t>(S) * S * 4);
	auto tile = [&](int x, int y) {
		const uint8_t* p = &out[(static_cast<size_t>(y) * S + x) * 4];
		return Rgb{ p[0], p[1], p[2] };
	};
	// Square source: fills the whole tile, no letterbox.
	CHECK(is(tile(S / 4,     S / 4),     255,   0,   0));   // top-left     red
	CHECK(is(tile(3 * S / 4, S / 4),       0, 255,   0));   // top-right    green
	CHECK(is(tile(S / 4,     3 * S / 4),   0,   0, 255));   // bottom-left  blue
	CHECK(is(tile(3 * S / 4, 3 * S / 4), 255, 255, 255));   // bottom-right white

	AssetThumbnailCache::setContext(nullptr, nullptr, "");
	he_test::removeAllQuiet(dir);
}
