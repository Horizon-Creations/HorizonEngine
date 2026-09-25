#include "doctest.h"
#include "TextureImporter.h"

#include <Backends/Software/SoftwareRaster.h>
#include <ContentManager/Assets.h>
#include <Renderer/UIRenderObject.h>
#include <UIWidget/UIElements.h>

#include <cstdint>
#include <string>
#include <vector>

// ── Which way up an imported picture lands, end to end ───────────────────────
// Reported (Thema 92): a picture put on a UI Image comes out mirrored. The
// chain behind it is three links that are each right on their own:
//
//   1. TextureImporter stores the rows BOTTOM-UP (flipVertically defaults to
//      true) — the engine's GL-style convention, v = 0 at the bottom. The mesh
//      importers flip their V to match (ImporterCommon.cpp, the glTF path), so
//      this is load-bearing for every textured mesh and must stay.
//   2. Every backend uploads TextureAsset::data as-is, row 0 first, so texel
//      row 0 (v = 0) is the picture's BOTTOM row on all five of them.
//   3. A plain UIImage emits uv (0,0) at its TOP-left corner — the UI's own
//      y-down convention — and the UI shaders pass it through unchanged.
//
// Put together, the top edge of the widget samples the bottom row of the
// picture: it is drawn upside down (mirrored across the horizontal axis). The
// designer preview inherits the same thing through the thumbnail cache, which
// reads the stored rows top to bottom as well.
//
// These cases pin the links as they are TODAY, so the fix has something to
// turn around rather than a description to believe. Link 1 is meant to stay;
// the UIImage case is the one the fix is expected to change.

namespace
{
	// A 2x2 picture that tells all four mirrorings apart:
	//   top-left red,    top-right green,
	//   bottom-left blue, bottom-right white.
	// Binary PPM rather than PNG so the fixture needs no encoder; stb_image
	// reads it through the same stbi_load_from_memory the PNG path takes.
	std::vector<uint8_t> quadrantPpm()
	{
		const std::string header = "P6\n2 2\n255\n";
		std::vector<uint8_t> b(header.begin(), header.end());
		const uint8_t px[] = {
			255,   0,   0,     0, 255,   0,    // top row:    red, green
			  0,   0, 255,   255, 255, 255,    // bottom row: blue, white
		};
		b.insert(b.end(), std::begin(px), std::end(px));
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

TEST_CASE("UI Image: a plain quad puts v = 0 on its top edge (Ist-Zustand, Thema 92)")
{
	// With link 1 above, v = 0 is the picture's bottom row — so this is the
	// half of the chain that draws it upside down. When the fix lands, this
	// expectation turns around (uvMin.y == 1, uvMax.y == 0) or moves into the
	// UI shaders; either way this case is where it shows.
	HE::UIImage img;
	img.textureAssetId = HE::UUID::generate();
	std::vector<UIRenderObject> out;
	img.render(HE::UIWidgetRect{ 10.0f, 20.0f, 64.0f, 32.0f }, HE::UIElementRenderState{},
	           HE::UUID{}, 1.0f, out);
	REQUIRE(out.size() == 1);
	CHECK(out[0].uvMin.x == doctest::Approx(0.0f));
	CHECK(out[0].uvMin.y == doctest::Approx(0.0f));   // top edge samples v = 0
	CHECK(out[0].uvMax.x == doctest::Approx(1.0f));
	CHECK(out[0].uvMax.y == doctest::Approx(1.0f));
}

TEST_CASE("UI Image end to end: an imported picture is drawn upside down (Ist-Zustand, Thema 92)")
{
	// The whole chain in one go, on the CPU: import with the settings the
	// editor's Import Asset uses, the Image's own quads, and the software
	// rasteriser — a port of uiFragment / kUIFS that samples the same way the
	// Metal and GL UI shaders do (texel row 0 at v = 0).
	const std::vector<uint8_t> ppm = quadrantPpm();
	auto tex = TextureImporter::decodeFromMemory(ppm.data(), ppm.size());
	REQUIRE(tex);

	HE::UIImage img;
	img.textureAssetId = HE::UUID::generate();
	std::vector<UIRenderObject> out;
	img.render(HE::UIWidgetRect{ 0.0f, 0.0f, 64.0f, 64.0f }, HE::UIElementRenderState{},
	           HE::UUID{}, 1.0f, out);
	REQUIRE(out.size() == 1);

	HE::sw::Image target;
	target.resize(64, 64);
	target.clear(0, 0, 0, 255);
	struct Src { const TextureAsset* t; } src{ tex.get() };
	HE::sw::draw(target, out, glm::vec4(0.0f),
		[](const HE::UUID&, void* user) -> HE::sw::TextureView {
			const TextureAsset* t = static_cast<Src*>(user)->t;
			return { t->data.data(), static_cast<int>(t->width), static_cast<int>(t->height) };
		}, &src);

	// Sampled well inside each quadrant, away from the bilinear seam.
	auto px = [&](int x, int y) { std::uint8_t r, g, b, a; target.pixel(x, y, r, g, b, a);
	                              return Rgb{ r, g, b }; };
	// The widget's TOP-left shows the picture's BOTTOM-left (blue), its
	// bottom-left the picture's top-left (red): mirrored across the horizontal
	// axis. Left and right are where they belong.
	CHECK(is(px( 8,  8),   0,   0, 255));   // should be red
	CHECK(is(px(56,  8), 255, 255, 255));   // should be green
	CHECK(is(px( 8, 56), 255,   0,   0));   // should be blue
	CHECK(is(px(56, 56),   0, 255,   0));   // should be white
}
