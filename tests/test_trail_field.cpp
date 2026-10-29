#include "doctest.h"
#include <Trails/TrailField.h>

#include <cmath>

using HE::TrailField;
using HE::TrailSettings;
using HE::TrailSurface;
using HE::TrailWeather;

TEST_CASE("TrailField: an ellipse stamp presses a soft print and nothing outside it")
{
	TrailField f;
	CHECK(f.pageCount() == 0u);
	CHECK(f.sample(3.0f, 4.0f) == 0.0f);          // no page, no depth

	f.stampEllipse(10.0f, 20.0f, 0.5f, 0.25f, 0.0f, 0.8f, TrailSurface::Snow);
	CHECK(f.pageCount() == 1u);
	CHECK(f.sample(10.0f, 20.0f) == doctest::Approx(0.8f).epsilon(0.02));   // the centre: the full depth
	CHECK(f.sample(10.0f, 20.0f) > f.sample(10.4f, 20.0f));                  // falls off along the long axis
	CHECK(f.sample(10.0f, 20.6f) == 0.0f);                                   // beyond rz = 0.25 across it
	CHECK(f.sample(11.0f, 20.0f) == 0.0f);                                   // beyond rx = 0.5 along it
	// The press is a maximum: a shallower print over a deeper one changes nothing.
	f.stampEllipse(10.0f, 20.0f, 0.5f, 0.25f, 0.0f, 0.3f, TrailSurface::Snow);
	CHECK(f.sample(10.0f, 20.0f) == doctest::Approx(0.8f).epsilon(0.02));
}

TEST_CASE("TrailField: the yaw turns the ellipse")
{
	TrailField f;
	f.stampEllipse(0.0f, 0.0f, 0.6f, 0.2f, 1.5707963f, 1.0f, TrailSurface::Sand);   // long axis along +Z
	CHECK(f.sample(0.0f, 0.4f) > 0.1f);
	CHECK(f.sample(0.4f, 0.0f) == 0.0f);
}

TEST_CASE("TrailField: a capsule leaves a continuous line, and a stamp may cross page borders")
{
	TrailField f;
	// 300 m long: more than two pages, along the page border z = 0 (texels on both sides).
	f.stampCapsule(-100.0f, 0.0f, 200.0f, 0.0f, 0.3f, 0.6f, TrailSurface::Sand);
	CHECK(f.pageCount() >= 4u);                    // x in [-100, 200] spans 3 page columns, z = 0 is a row border
	for (float x = -99.0f; x < 199.0f; x += 7.3f)
		CHECK(f.sample(x, 0.0f) > 0.2f);
	CHECK(f.sample(50.0f, 1.0f) == 0.0f);
	// Degenerate segment = a disc.
	TrailField g;
	g.stampCapsule(5.0f, 5.0f, 5.0f, 5.0f, 0.4f, 1.0f, TrailSurface::Snow);
	CHECK(g.sample(5.0f, 5.0f) > 0.5f);
	// Bad input stamps nothing.
	TrailField h;
	h.stampEllipse(0, 0, 0.0f, 1.0f, 0.0f, 1.0f, TrailSurface::Snow);
	h.stampEllipse(0, 0, 1.0f, 1.0f, 0.0f, 0.0f, TrailSurface::Snow);
	h.stampCapsule(0, 0, 1, 1, -1.0f, 1.0f, TrailSurface::Snow);
	CHECK(h.pageCount() == 0u);
}

TEST_CASE("TrailField: the gradient leans toward the pit")
{
	TrailField f;
	f.stampEllipse(0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, TrailSurface::Snow);
	const glm::vec2 right = f.gradient(0.8f, 0.0f);   // east of the centre: depth falls with x
	const glm::vec2 left  = f.gradient(-0.8f, 0.0f);
	const glm::vec2 south = f.gradient(0.0f, 0.8f);
	CHECK(right.x < -0.1f);
	CHECK(left.x > 0.1f);
	CHECK(south.y < -0.1f);
	CHECK(std::fabs(right.y) < 0.05f);
	CHECK(f.gradient(0.0f, 0.0f).x == doctest::Approx(0.0f).epsilon(0.05));   // the pit floor is flat
}

TEST_CASE("TrailField: no weather, no healing; the clock starts at the first call and never runs back")
{
	TrailField f;
	f.stampEllipse(0, 0, 0.5f, 0.5f, 0.0f, 0.8f, TrailSurface::Snow);
	const float before = f.sample(0, 0);
	TrailWeather calm;                       // no precip, windSpeed 1, dry
	f.tick(100.0, calm);                     // starts the clock
	f.tick(100000.0, calm);
	CHECK(f.sample(0, 0) == before);
	f.tick(5.0, TrailWeather{ 1.0f, true, false, 1.0f, 0.0f });   // back in time: nothing
	CHECK(f.sample(0, 0) == before);
}

TEST_CASE("TrailField: snowfall fills a print at the authored rate, and an empty page is freed")
{
	TrailField f;
	f.stampEllipse(10, 10, 0.5f, 0.5f, 0.0f, 0.6f, TrailSurface::Snow);
	TrailWeather snow;
	snow.precip = 1.0f; snow.snowing = true;
	TrailSettings s;                         // hSnow = 1/120 per second
	f.tick(0.0, snow, s);
	f.tick(30.0, snow, s);                   // a quarter of the way: -0.25 depth
	CHECK(f.sample(10, 10) == doctest::Approx(0.35f).epsilon(0.02));
	CHECK(f.pageCount() == 1u);
	f.tick(200.0, snow, s);                  // long past 0.6 * 120 = 72 s in total
	CHECK(f.sample(10, 10) == 0.0f);
	CHECK(f.pageCount() == 0u);
	CHECK(f.activeTexels() == 0u);
}

TEST_CASE("TrailField: slow healing does not truncate away (16-bit depth, carried remainder)")
{
	TrailField f;
	f.stampEllipse(0, 0, 0.5f, 0.5f, 0.0f, 0.5f, TrailSurface::Snow);
	TrailWeather drift;                      // a gale on dry snow: 1/1800 per second
	drift.windSpeed = 5.0f;
	f.tick(0.0, drift);
	f.tick(900.0, drift);                    // half an hour's worth is 0.5: here a quarter of an hour
	CHECK(f.sample(0, 0) == doctest::Approx(0.0f).epsilon(0.05));   // 0.5 - 900/1800 = 0
	// A breeze: a tenth of the gale's rate must still move the print after a long while.
	TrailField g;
	g.stampEllipse(0, 0, 0.5f, 0.5f, 0.0f, 0.5f, TrailSurface::Snow);
	TrailWeather breeze;
	breeze.windSpeed = 1.4f;                 // wind = 0.1
	g.tick(0.0, breeze);
	g.tick(600.0, breeze);
	CHECK(g.sample(0, 0) < 0.5f - 0.02f);
}

TEST_CASE("TrailField: sand follows the wind squared, and wet or rained-on sand keeps its prints")
{
	const auto remaining = [](const TrailWeather& w) {
		TrailField f;
		f.stampEllipse(0, 0, 0.5f, 0.5f, 0.0f, 0.5f, TrailSurface::Sand);
		f.tick(0.0, w);
		f.tick(300.0, w);
		return f.sample(0, 0);
	};
	TrailWeather gale;   gale.windSpeed = 5.0f;                     // wind 1
	TrailWeather half;   half.windSpeed = 3.0f;                     // wind 0.5
	TrailWeather wet = gale;   wet.wetness = 1.0f;
	TrailWeather rain = gale;  rain.precip = 1.0f; rain.raining = true;
	const float start = 0.5f;
	const float lostGale = start - remaining(gale);
	const float lostHalf = start - remaining(half);
	CHECK(lostGale > 0.1f);
	CHECK(lostHalf == doctest::Approx(lostGale * 0.25f).epsilon(0.1));   // (1/2)^2
	CHECK(start - remaining(wet)  < lostGale * 0.2f);
	CHECK(start - remaining(rain) < lostGale * 0.2f);
	// Snow does not heal from sand's wind: a snow print under the same gale uses hDrift (1/1800), sand hWind (1/900).
	TrailField s;
	s.stampEllipse(0, 0, 0.5f, 0.5f, 0.0f, 0.5f, TrailSurface::Snow);
	s.tick(0.0, gale);
	s.tick(300.0, gale);
	CHECK(start - s.sample(0, 0) == doctest::Approx(300.0f / 1800.0f).epsilon(0.05));
}

TEST_CASE("TrailField: a long pause heals in one go and stays bounded")
{
	TrailField f;
	f.stampEllipse(0, 0, 0.5f, 0.5f, 0.0f, 1.0f, TrailSurface::Snow);
	TrailWeather snow;
	snow.precip = 1.0f; snow.snowing = true;
	f.tick(0.0, snow);
	f.tick(1.0e7, snow);                     // 115 days
	CHECK(f.pageCount() == 0u);
}
