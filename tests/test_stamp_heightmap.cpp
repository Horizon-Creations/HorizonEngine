#include "doctest.h"

#include "StampHeightmap.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

namespace SH = HE::StampHeightmap;

namespace
{
float at(const SH::Map& m, int x, int y) { return m.v[static_cast<size_t>(y) * m.w + x]; }
}

TEST_CASE("stamp heightmap: a line runs from its start height to its end height and fades to the sides")
{
	SH::Map m;
	SH::fill(m, 64, 64, 0.0f);
	SH::line(m, 8.0f, 32.0f, 56.0f, 32.0f, 6.0f, 0.2f, 1.0f, 1.0f);
	CHECK(at(m, 8, 32) == doctest::Approx(0.2f));
	CHECK(at(m, 56, 32) == doctest::Approx(1.0f));
	CHECK(at(m, 32, 32) == doctest::Approx(0.6f));
	CHECK(at(m, 32, 35) > 0.0f);
	CHECK(at(m, 32, 35) < at(m, 32, 32));
	CHECK(at(m, 32, 39) == 0.0f);   // beyond the radius: untouched
	CHECK(at(m, 0, 0) == 0.0f);
}

TEST_CASE("stamp heightmap: a transition covers the whole picture; the brush raises, lowers and smooths")
{
	SH::Map m;
	SH::fill(m, 32, 32, 0.5f);
	SH::gradient(m, 8.0f, 0.0f, 24.0f, 0.0f, 0.0f, 1.0f, 1.0f);
	CHECK(at(m, 0, 20) == 0.0f);    // before the start
	CHECK(at(m, 31, 5) == 1.0f);    // beyond the end
	CHECK(at(m, 16, 9) == doctest::Approx(0.5f));

	SH::fill(m, 32, 32, 0.5f);
	SH::dab(m, 16.0f, 16.0f, 5.0f, 0.25f);
	CHECK(at(m, 16, 16) == doctest::Approx(0.75f));
	SH::dab(m, 16.0f, 16.0f, 5.0f, -2.0f);
	CHECK(at(m, 16, 16) == 0.0f);   // clamped
	CHECK(at(m, 25, 16) == 0.5f);
	SH::dab(m, -40.0f, 500.0f, 5.0f, 1.0f);   // off the picture: nothing, and no write outside it

	SH::fill(m, 32, 32, 0.5f);
	m.v[16 * 32 + 16] = 0.0f;       // a one-pixel pit
	SH::smooth(m, 16.0f, 16.0f, 5.0f, 1.0f);
	CHECK(at(m, 16, 16) == doctest::Approx(4.0f / 9.0f));   // the mean of its neighbourhood
	CHECK(at(m, 25, 16) == 0.5f);
}

TEST_CASE("stamp heightmap: a landform is the same for a seed, another for another, and stays in 0..1")
{
	for (int shape = 0; shape < static_cast<int>(SH::Shape::Count); ++shape)
	{
		SH::GenParams p;
		p.shape = static_cast<SH::Shape>(shape);
		SH::Map a, b, c;
		SH::fill(a, 64, 64, 0.0f);
		b = a; c = a;
		SH::generate(a, p);
		SH::generate(b, p);
		p.seed = 2;
		SH::generate(c, p);
		CAPTURE(shape);
		CHECK(a.v == b.v);
		CHECK(a.v != c.v);
		float lo = 1.0f, hi = 0.0f;
		for (float v : a.v) { REQUIRE(std::isfinite(v)); lo = std::min(lo, v); hi = std::max(hi, v); }
		CHECK(lo >= 0.0f);
		CHECK(hi <= 1.0f);
		CHECK(hi - lo > 0.3f);   // a form, not a flat sheet
	}
	// A mountain stands in the middle and is gone at the border.
	SH::Map m;
	SH::fill(m, 64, 64, 0.0f);
	SH::generate(m, {});
	CHECK(at(m, 32, 32) > 0.3f);
	CHECK(at(m, 0, 32) < 0.02f);
}

TEST_CASE("stamp heightmap: a selection limits generators and filters to its region")
{
	SH::Map m, mask;
	SH::fill(m, 64, 64, 0.25f);
	SH::fill(mask, 64, 64, 0.0f);
	SH::dab(mask, 16.0f, 16.0f, 10.0f, 1.0f);
	const SH::Map before = m;
	SH::GenParams p;
	p.shape = SH::Shape::Hills;
	SH::generate(m, p, &mask);
	CHECK(at(m, 16, 16) != at(before, 16, 16));   // made again inside
	CHECK(at(m, 48, 48) == 0.25f);                 // untouched outside
	SH::erode(m, 0.5f, 4, 3, &mask);
	SH::blur(m, 4.0f, &mask);
	SH::terrace(m, 4, 1.0f, &mask);
	SH::invert(m, &mask);
	SH::refine(m, {}, &mask);
	CHECK(at(m, 48, 48) == 0.25f);
	CHECK(at(m, 40, 16) == 0.25f);
}

TEST_CASE("stamp heightmap: Naturalize turns a flat-topped sketch into relief and leaves empty ground empty")
{
	SH::Map m;
	SH::fill(m, 96, 96, 0.0f);
	SH::line(m, 24.0f, 48.0f, 72.0f, 48.0f, 14.0f, 0.8f, 0.8f, 1.0f);   // a sausage: what a hand draws
	const SH::Map sketch = m;
	SH::RefineParams p;
	SH::refine(m, p);
	SH::Map again = sketch;
	SH::refine(again, p);
	CHECK(m.v == again.v);   // the same for a seed
	// Along the sketch's level crest the ground now varies ...
	float lo = 1.0f, hi = 0.0f;
	for (int x = 30; x <= 66; ++x) { lo = std::min(lo, at(m, x, 48)); hi = std::max(hi, at(m, x, 48)); }
	CHECK(hi - lo > 0.02f);
	CHECK(lo > 0.2f);        // ... but it is still the ridge that was drawn
	// ... and far from it nothing was raised (erosion's settling blur leaves less than a 16-bit step).
	CHECK(at(m, 4, 4) < 1.0f / 65535.0f);
	CHECK(at(m, 90, 90) < 1.0f / 65535.0f);
	for (float v : m.v) { REQUIRE(std::isfinite(v)); REQUIRE(v >= 0.0f); REQUIRE(v <= 1.0f); }
}

TEST_CASE("stamp heightmap: erosion moves ground downhill, the filters do what they say")
{
	SH::Map m;
	SH::fill(m, 96, 96, 0.0f);
	SH::GenParams p;
	SH::generate(m, p);
	const SH::Map before = m;
	double sumBefore = 0.0, sumAfter = 0.0;
	SH::erode(m, 1.0f, 8, 7);
	for (size_t i = 0; i < m.v.size(); ++i) { REQUIRE(std::isfinite(m.v[i])); sumBefore += before.v[i]; sumAfter += m.v[i]; }
	CHECK(m.v != before.v);
	CHECK(at(m, 48, 48) <= at(before, 48, 48));                   // the peak is worn, never built up
	CHECK(sumAfter <= sumBefore * 1.001);                           // nothing comes from nowhere
	CHECK(sumAfter >= sumBefore * 0.7);                             // ... and the mountain is still there
	SH::Map again = before;
	SH::erode(again, 1.0f, 8, 7);
	CHECK(again.v == m.v);

	SH::Map t = before;
	SH::terrace(t, 4, 1.0f);
	int onStep = 0;
	for (float v : t.v) if (std::abs(v * 4.0f - std::round(v * 4.0f)) < 1e-4f) ++onStep;
	CHECK(onStep > static_cast<int>(t.v.size()) / 2);              // most of the ground lies on a tread

	SH::Map n = before;
	for (float& v : n.v) v = 0.2f + v * 0.3f;
	SH::normalize(n);
	CHECK(*std::min_element(n.v.begin(), n.v.end()) == doctest::Approx(0.0f));
	CHECK(*std::max_element(n.v.begin(), n.v.end()) == doctest::Approx(1.0f));

	SH::Map f;
	SH::fill(f, 32, 32, 1.0f);
	SH::fadeEdges(f, 0.3f);
	CHECK(at(f, 0, 16) == 0.0f);
	CHECK(at(f, 16, 16) == 1.0f);

	SH::Map fl;
	SH::fill(fl, 32, 32, 0.0f);
	SH::flatten(fl, 16.0f, 16.0f, 6.0f, 0.6f, 1.0f);
	CHECK(at(fl, 16, 16) == doctest::Approx(0.6f));
}

TEST_CASE("stamp heightmap: the ground colours show rock on steep ground, snow on top, and never a white sheet")
{
	SH::Map m;
	SH::fill(m, 64, 64, 0.0f);
	SH::gradient(m, 20.0f, 0.0f, 44.0f, 0.0f, 0.0f, 1.0f, 1.0f);   // flat low, a steep face, flat high
	std::vector<uint8_t> rgba;
	SH::groundColours(m, 64.0f, 80.0f, true, rgba);
	REQUIRE(rgba.size() == m.v.size() * 4);
	const auto px = [&](int x, int y) { return &rgba[(static_cast<size_t>(y) * 64 + x) * 4]; };
	CHECK(px(2, 32)[1] > px(2, 32)[0]);                // low and flat: green leads
	CHECK(px(62, 32)[0] == 255);                       // high and flat: snow
	const uint8_t* face = px(32, 32);                  // the steep face: grey rock, neither green nor white
	CHECK(std::abs(face[0] - face[1]) < 12);
	CHECK(face[0] < 200);
	CHECK(px(2, 32)[3] == 255);

	SH::groundColours(m, 64.0f, 80.0f, false, rgba);  // off: one even grey
	CHECK(px(2, 32)[0] == px(62, 32)[0]);
	CHECK(px(2, 32)[0] == px(2, 32)[2]);
	CHECK(px(2, 32)[0] < 200);
}

TEST_CASE("stamp heightmap: a request to edit a file is taken once")
{
	std::string file;
	CHECK_FALSE(SH::takeEditRequest(file));
	SH::requestEdit("Heightmaps/A.pgm");
	REQUIRE(SH::takeEditRequest(file));
	CHECK(file == "Heightmaps/A.pgm");
	CHECK_FALSE(SH::takeEditRequest(file));
}

TEST_CASE("stamp heightmap: the 16-bit PGM it writes is what a stamp's heightmap file reads")
{
	SH::Map m;
	SH::fill(m, 5, 3, 0.0f);
	for (size_t i = 0; i < m.v.size(); ++i) m.v[i] = static_cast<float>(i) / static_cast<float>(m.v.size() - 1);
	const std::string path = (std::filesystem::temp_directory_path() / "he_test_stamp_heightmap" / "ramp.pgm").string();
	std::string error;
	REQUIRE_MESSAGE(SH::writePgm16(m, path, error), error);
	SH::Map back;
	REQUIRE_MESSAGE(SH::read(back, path, error), error);
	REQUIRE(back.w == 5);
	REQUIRE(back.h == 3);
	for (size_t i = 0; i < m.v.size(); ++i) CHECK(std::abs(back.v[i] - m.v[i]) < 1.0f / 65535.0f);   // 16 bits: no 8-bit steps
	std::filesystem::remove_all(std::filesystem::path(path).parent_path());
}
