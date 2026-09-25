#include "doctest.h"

#include <nlohmann/json.hpp>
#include <Backends/Software/SoftwareRaster.h>
#include <Renderer/UIRenderObject.h>
#include <UIWidget/UIElements.h>
#include <UIWidget/UIWidgetTree.h>

#include <cstdint>
#include <string>
#include <vector>

// ── Flip Horizontal / Flip Vertical on a UI Image (Thema 92, Schritt 3) ─────
// Flip mirrors the FINISHED picture inside the element's box. Every case here
// says so relative to the same image drawn unflipped — never "red is at the
// top-left" — because which way up the unflipped picture lands is a separate
// question (the orientation fix, Schritt 2). Whatever that fix does to the
// base UVs, a flipped image has to stay the mirror of an unflipped one.

namespace
{
	// 4x4 source, every texel its own colour, so a mirror along either axis
	// (or both) moves every sample to a different colour.
	std::vector<std::uint8_t> distinctTexels(int w, int h)
	{
		std::vector<std::uint8_t> px(static_cast<size_t>(w) * h * 4);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				std::uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
				p[0] = static_cast<std::uint8_t>(40 + 60 * x);
				p[1] = static_cast<std::uint8_t>(40 + 60 * y);
				p[2] = static_cast<std::uint8_t>((x + y) % 2 ? 200 : 20);
				p[3] = 255;
			}
		return px;
	}

	struct Src { const std::uint8_t* rgba; int w, h; };

	HE::sw::Image rasterise(const HE::UIImage& img, const HE::UIWidgetRect& r, const Src& src)
	{
		std::vector<UIRenderObject> out;
		img.render(r, HE::UIElementRenderState{}, HE::UUID{}, 1.0f, out);
		HE::sw::Image target;
		target.resize(64, 64);
		target.clear(0, 0, 0, 255);
		Src s = src;
		HE::sw::draw(target, out, glm::vec4(0.0f),
			[](const HE::UUID&, void* user) -> HE::sw::TextureView {
				const Src* s = static_cast<Src*>(user);
				return { s->rgba, s->w, s->h };
			}, &s);
		return target;
	}

	struct Rgb { int r, g, b; };
	Rgb px(const HE::sw::Image& im, int x, int y)
	{
		std::uint8_t r, g, b, a;
		im.pixel(x, y, r, g, b, a);
		return { r, g, b };
	}
	bool same(const Rgb& a, const Rgb& b)
	{
		auto near = [](int x, int y) { return x >= y - 2 && x <= y + 2; };
		return near(a.r, b.r) && near(a.g, b.g) && near(a.b, b.b);
	}
	bool same(const HE::sw::Image& a, int ax, int ay, const HE::sw::Image& b, int bx, int by)
	{
		return same(px(a, ax, ay), px(b, bx, by));
	}
}

TEST_CASE("UI Image flip: off by default, a property like any other, json only when on")
{
	HE::UIImage img;
	CHECK_FALSE(img.flipH);
	CHECK_FALSE(img.flipV);

	// Reachable by name — the Details panel and graphs both go through this.
	img.setProp("Flip Horizontal", HE::UIPropValue::ofBool(true));
	CHECK(img.flipH);
	CHECK(img.getProp("Flip Horizontal").b);
	CHECK_FALSE(img.getProp("Flip Vertical").b);
	img.setProp("Flip Vertical", HE::UIPropValue::ofBool(true));
	CHECK(img.flipV);
	CHECK(img.clone()->getProp("Flip Vertical").b);

	HE::UIWidgetTree t;
	const int id = t.add(HE::UIWidgetType::Image);
	// An image authored before Flip existed saves byte-identically.
	const std::string plain = HE::uiWidgetTreeToJson(t);
	CHECK(plain.find("flipH") == std::string::npos);
	CHECK(plain.find("flipV") == std::string::npos);

	auto* e = dynamic_cast<HE::UIImage*>(t.find(id));
	REQUIRE(e != nullptr);
	e->flipH = true;
	{
		HE::UIWidgetTree r;
		REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), r));
		auto* back = dynamic_cast<HE::UIImage*>(r.find(id));
		REQUIRE(back != nullptr);
		CHECK(back->flipH);
		CHECK_FALSE(back->flipV);
	}
	e->flipH = false; e->flipV = true;
	{
		HE::UIWidgetTree r;
		REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(t), r));
		auto* back = dynamic_cast<HE::UIImage*>(r.find(id));
		REQUIRE(back != nullptr);
		CHECK_FALSE(back->flipH);
		CHECK(back->flipV);
	}

	// An absent key means "not flipped", also when read over a flipped image.
	HE::UIImage over;
	over.flipH = over.flipV = true;
	over.readJson(nlohmann::json::object());
	CHECK_FALSE(over.flipH);
	CHECK_FALSE(over.flipV);
}

TEST_CASE("UI Image flip: the drawn picture is the unflipped one mirrored")
{
	const int tw = 4, th = 4;
	const std::vector<std::uint8_t> tex = distinctTexels(tw, th);
	const Src src{ tex.data(), tw, th };
	const HE::UIWidgetRect box{ 0.0f, 0.0f, 64.0f, 64.0f };

	HE::UIImage img;
	img.textureAssetId = HE::UUID::generate();
	const HE::sw::Image base = rasterise(img, box, src);

	// The centre of each of the 16 texel cells: well away from the bilinear
	// seams, so a sample and its mirror read one texel each.
	const int c[4] = { 8, 24, 40, 56 };

	SUBCASE("Flip Horizontal: left and right trade places, rows stay")
	{
		img.flipH = true;
		const HE::sw::Image f = rasterise(img, box, src);
		for (int y : c) for (int x : c)
			CHECK(same(f, x, y, base, 63 - x, y));
		CHECK_FALSE(same(f, 8, 8, base, 8, 8));   // it did change something
	}
	SUBCASE("Flip Vertical: top and bottom trade places, columns stay")
	{
		img.flipV = true;
		const HE::sw::Image f = rasterise(img, box, src);
		for (int y : c) for (int x : c)
			CHECK(same(f, x, y, base, x, 63 - y));
		CHECK_FALSE(same(f, 8, 8, base, 8, 8));
	}
	SUBCASE("both: the picture turned half a circle")
	{
		img.flipH = img.flipV = true;
		const HE::sw::Image f = rasterise(img, box, src);
		for (int y : c) for (int x : c)
			CHECK(same(f, x, y, base, 63 - x, 63 - y));
	}
}

TEST_CASE("UI Image flip on a 9-slice: the whole frame is mirrored, margins keep their size")
{
	// Asymmetric on purpose: a mirror that re-cut the frame (Slice Left on the
	// left again) would put a 4-wide piece where the 12-wide one belongs.
	HE::UIImage img;
	img.textureAssetId = HE::UUID{ 1, 2 };
	img.textureW = img.textureH = 32;
	img.sliceLeft = 4.0f;  img.sliceRight  = 12.0f;
	img.sliceTop  = 6.0f;  img.sliceBottom = 10.0f;
	const HE::UIWidgetRect box{ 10.0f, 20.0f, 200.0f, 100.0f };

	std::vector<UIRenderObject> base;
	img.render(box, {}, HE::UUID{}, 1.0f, base);
	REQUIRE(base.size() == 9);

	img.flipH = true;
	img.flipV = true;
	std::vector<UIRenderObject> f;
	img.render(box, {}, HE::UUID{}, 1.0f, f);
	REQUIRE(f.size() == base.size());

	for (size_t i = 0; i < base.size(); ++i)
	{
		CAPTURE(i);
		// Same piece, same size, mirrored place in the box …
		CHECK(f[i].size.x == doctest::Approx(base[i].size.x));
		CHECK(f[i].size.y == doctest::Approx(base[i].size.y));
		CHECK(f[i].position.x == doctest::Approx(
			box.x + box.x + box.w - (base[i].position.x + base[i].size.x)));
		CHECK(f[i].position.y == doctest::Approx(
			box.y + box.y + box.h - (base[i].position.y + base[i].size.y)));
		// … reading its source rect backwards.
		CHECK(f[i].uvMin.x == doctest::Approx(base[i].uvMax.x));
		CHECK(f[i].uvMax.x == doctest::Approx(base[i].uvMin.x));
		CHECK(f[i].uvMin.y == doctest::Approx(base[i].uvMax.y));
		CHECK(f[i].uvMax.y == doctest::Approx(base[i].uvMin.y));
	}

	// Concretely: the Slice Left piece (4 source px) is the one on the right
	// edge now, and the 12-wide Slice Right piece is on the left.
	CHECK(f[0].uvMax.x == doctest::Approx(0.0f));
	CHECK(f[0].size.x == doctest::Approx(4.0f));
	CHECK(f[0].position.x + f[0].size.x == doctest::Approx(box.x + box.w));
	CHECK(f[2].size.x == doctest::Approx(12.0f));
	CHECK(f[2].position.x == doctest::Approx(box.x));
	// And the Slice Top piece (6 px) is at the bottom.
	CHECK(f[0].size.y == doctest::Approx(6.0f));
	CHECK(f[0].position.y + f[0].size.y == doctest::Approx(box.y + box.h));
}

TEST_CASE("UI Image flip on a 9-slice, rasterised: the left border ends up on the right")
{
	// 8x2 source: the two left columns red (the border), the rest grey.
	const int tw = 8, th = 2;
	std::vector<std::uint8_t> tex(static_cast<size_t>(tw) * th * 4);
	for (int y = 0; y < th; ++y)
		for (int x = 0; x < tw; ++x)
		{
			std::uint8_t* p = &tex[(static_cast<size_t>(y) * tw + x) * 4];
			const bool border = x < 2;
			p[0] = border ? 255 : 128; p[1] = border ? 0 : 128; p[2] = border ? 0 : 128;
			p[3] = 255;
		}
	const Src src{ tex.data(), tw, th };

	HE::UIImage img;
	img.textureAssetId = HE::UUID::generate();
	img.textureW = tw; img.textureH = th;
	img.sliceLeft = 2.0f;
	const HE::UIWidgetRect box{ 0.0f, 0.0f, 64.0f, 64.0f };

	const HE::sw::Image base = rasterise(img, box, src);
	CHECK(same(px(base, 0, 32), { 255, 0, 0 }));
	CHECK(same(px(base, 63, 32), { 128, 128, 128 }));

	img.flipH = true;
	const HE::sw::Image f = rasterise(img, box, src);
	CHECK(same(px(f, 63, 32), { 255, 0, 0 }));
	CHECK(same(px(f, 62, 32), { 255, 0, 0 }));
	CHECK(same(px(f, 0, 32), { 128, 128, 128 }));
}
