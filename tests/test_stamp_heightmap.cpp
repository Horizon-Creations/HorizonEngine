#include "doctest.h"

#include "StampHeightmap.h"

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
