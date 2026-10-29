#include "doctest.h"
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <Trails/TrailClipmap.h>
#include <Trails/TrailField.h>
#include <HorizonScene/TerrainMeshGenerator.h>

#include <filesystem>

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

TEST_CASE("TrailClipmap: registers an untouched window, writes prints into the torus, rewrites only on change")
{
	const std::filesystem::path dir = std::filesystem::temp_directory_path() / "he_trail_clipmap_test";
	std::filesystem::create_directories(dir);
	ContentManager cm(dir.string());
	HE::TrailField field;
	HE::TrailClipmap map;
	const glm::vec3 cam(10.0f, 5.0f, 20.0f);

	// First call: the texture exists (so the material can resolve the path) and holds nothing.
	CHECK(map.update(cm, nullptr, field, cam));
	REQUIRE(map.textureId() != HE::UUID{});
	const TextureAsset* tex = cm.getTexture(map.textureId());
	REQUIRE(tex);
	CHECK(tex->width == 512u);
	CHECK(tex->height == 512u);
	CHECK(tex->channels == 4u);
	CHECK_FALSE(tex->srgb);
	CHECK(cm.idForPath(HE::kTrailTexturePath) == map.textureId());
	for (size_t i = 0; i < tex->data.size(); i += 4 * 997)
	{
		CHECK(tex->data[i + 0] == 128);                 // R = 128 - 127 v: untouched is the middle
		CHECK(tex->data[i + 1] == 128);                 // flat
		CHECK(tex->data[i + 2] == 128);
		CHECK(tex->data[i + 3] == 128);                 // the alpha marker
	}
	// Nothing changed, nothing near: no rewrite, also when the camera walks.
	CHECK_FALSE(map.update(cm, nullptr, field, cam));
	CHECK_FALSE(map.update(cm, nullptr, field, glm::vec3(30.0f, 5.0f, 20.0f)));

	// A print under the camera lands at the texel of its world position, modulo the window.
	field.stampEllipse(10.0f, 20.0f, 0.5f, 0.5f, 0.0f, 1.0f, HE::TrailSurface::Snow);
	CHECK(map.update(cm, nullptr, field, cam));
	tex = cm.getTexture(map.textureId());
	REQUIRE(tex);
	const auto px = [&](float x, float z) {
		const int ix = static_cast<int>(std::floor(x / 0.25f)), iz = static_cast<int>(std::floor(z / 0.25f));
		const int u = ((ix % 512) + 512) % 512, v = ((iz % 512) + 512) % 512;
		return &tex->data[(static_cast<size_t>(v) * 512 + u) * 4];
	};
	CHECK(px(10.1f, 20.1f)[0] < 20);                    // deep pit: R near 1
	CHECK(px(10.1f, 20.1f)[3] == 128);
	CHECK(px(40.0f, 20.0f)[0] == 128);                  // far from the print
	// The rim leans toward the pit: east of the centre the depth falls with x, so G < 128, west G > 128.
	CHECK(px(10.4f, 20.1f)[1] < 128);
	CHECK(px(9.6f, 20.1f)[1] > 128);
	CHECK(px(10.1f, 20.45f)[2] < 128);                  // south of it: B < 128
	// The same world texel one window (128 m) away shares the pixel: the wrap the shader relies on.
	CHECK(px(10.1f + 128.0f, 20.1f) == px(10.1f, 20.1f));

	// No new change: no rewrite while the camera stays; walking one texel rewrites (something is near).
	CHECK_FALSE(map.update(cm, nullptr, field, cam));
	CHECK(map.update(cm, nullptr, field, glm::vec3(10.5f, 5.0f, 20.0f)));
	// A healed field clears the picture once, then stays quiet.
	HE::TrailWeather snow;
	snow.precip = 1.0f; snow.snowing = true;
	field.tick(0.0, snow);
	field.tick(500.0, snow);
	CHECK(map.update(cm, nullptr, field, cam));
	CHECK(cm.getTexture(map.textureId())->data[(256 * 512 + 256) * 4] == 128);
	CHECK_FALSE(map.update(cm, nullptr, field, cam));
	std::filesystem::remove_all(dir);
}

TEST_CASE("TrailField: pressing leaves a pit and nothing else, plowing drags a groove with a heap on both sides")
{
	// Pressed: a capsule of pits, no heap anywhere (the field never goes negative).
	TrailField press;
	press.stampCapsule(0.0f, 10.0f, 20.0f, 10.0f, 0.4f, 0.8f, TrailSurface::Snow);
	for (float z = 8.0f; z <= 12.0f; z += 0.2f)
		for (float x = -1.0f; x <= 21.0f; x += 1.3f)
			CHECK(press.sample(x, z) >= 0.0f);

	// Plowed along +X at z = 10: groove radius 0.4, berms 0.5 m wide and 0.5 tall beside it.
	TrailField plow;
	plow.plowCapsule(0.0f, 10.0f, 20.0f, 10.0f, 0.4f, 0.8f, 0.5f, 0.5f, TrailSurface::Snow);
	CHECK(plow.sample(10.0f, 10.0f) == doctest::Approx(0.8f).epsilon(0.03));       // the groove: a pit
	CHECK(plow.sample(10.0f, 10.65f) < -0.2f);                                       // a heap on the +Z side ...
	CHECK(plow.sample(10.0f, 9.35f) < -0.2f);                                        // ... and on the -Z side
	CHECK(plow.sample(10.0f, 11.5f) == 0.0f);                                        // none farther out
	CHECK(plow.sample(10.0f, 10.65f) == doctest::Approx(plow.sample(10.0f, 9.35f)).epsilon(0.1));   // symmetric
	// Continuous: no gap along the groove, and the heap runs the whole length.
	for (float x = 1.0f; x < 19.0f; x += 0.9f)
	{
		CHECK(plow.sample(x, 10.0f) > 0.5f);
		CHECK(plow.sample(x, 10.65f) < -0.1f);
	}
	// The gradient across a heap points up its far flank: the value falls (more negative) toward the crest.
	CHECK(plow.gradient(10.0f, 10.5f).y < 0.0f);                                     // groove side of the heap: v falls with z
	CHECK(plow.gradient(10.0f, 10.9f).y > 0.0f);                                     // outer side: v rises back to 0

	// Dragging in many small steps (one per frame) makes the same track as one long segment, and the next
	// stretch clears the heap at the front of the previous one (a pit wins).
	TrailField steps;
	for (float x = 0.0f; x < 20.0f; x += 0.3f)
		steps.plowCapsule(x, 10.0f, x + 0.3f, 10.0f, 0.4f, 0.8f, 0.5f, 0.5f, TrailSurface::Snow);
	CHECK(steps.sample(10.0f, 10.0f) == doctest::Approx(0.8f).epsilon(0.03));
	CHECK(steps.sample(10.0f, 10.65f) < -0.2f);
	CHECK(steps.sample(10.0f, 10.0f) > 0.0f);                                        // not buried by the berm of a later step

	// A pit overwrites a heap; a heap never grows on a pit.
	TrailField mix;
	mix.plowCapsule(0.0f, 10.0f, 20.0f, 10.0f, 0.4f, 0.8f, 0.5f, 0.5f, TrailSurface::Snow);
	CHECK(mix.sample(10.0f, 10.65f) < -0.2f);
	mix.stampEllipse(10.0f, 10.65f, 0.3f, 0.3f, 0.0f, 0.6f, TrailSurface::Snow);    // a foot in the heap
	CHECK(mix.sample(10.0f, 10.65f) > 0.3f);
	const float pit = mix.sample(10.0f, 10.0f);
	mix.plowCapsule(0.0f, 10.65f, 20.0f, 10.65f, 0.2f, 0.2f, 0.5f, 0.5f, TrailSurface::Snow);
	CHECK(mix.sample(10.0f, 10.0f) >= pit * 0.9f);                                   // the old groove stays a pit

	// A degenerate plow (no berm width) is a plain groove; bad input does nothing.
	TrailField bare;
	bare.plowCapsule(0, 0, 5, 0, 0.4f, 0.8f, 0.0f, 0.5f, TrailSurface::Sand);
	CHECK(bare.sample(2.0f, 0.65f) == 0.0f);
	bare.plowCapsule(0, 0, 5, 0, 0.0f, 0.8f, 0.5f, 0.5f, TrailSurface::Sand);
	CHECK(bare.activeTexels() > 0u);
}

TEST_CASE("TrailField: heaps slump back to nothing like pits fill, and the clipmap shows them above the middle")
{
	TrailField f;
	f.plowCapsule(0.0f, 10.0f, 10.0f, 10.0f, 0.4f, 0.8f, 0.5f, 0.5f, TrailSurface::Snow);
	CHECK(f.sample(5.0f, 10.65f) < -0.2f);
	TrailWeather snow;
	snow.precip = 1.0f; snow.snowing = true;
	f.tick(0.0, snow);
	f.tick(60.0, snow);                       // half a minute: -0.5 at 1/120 per second
	CHECK(f.sample(5.0f, 10.65f) > -0.01f);   // the heap (<= 0.5 tall) is gone
	CHECK(f.sample(5.0f, 10.0f) == doctest::Approx(0.3f).epsilon(0.1));   // the pit only lost 0.5 of its 0.8
	f.tick(500.0, snow);
	CHECK(f.pageCount() == 0u);               // everything healed, the page is freed

	// Clipmap: R above 128 = heap, below = pit.
	const std::filesystem::path dir = std::filesystem::temp_directory_path() / "he_trail_clipmap_heap_test";
	std::filesystem::create_directories(dir);
	ContentManager cm(dir.string());
	TrailField g;
	g.plowCapsule(0.0f, 10.0f, 10.0f, 10.0f, 0.4f, 0.8f, 0.5f, 0.5f, TrailSurface::Snow);
	HE::TrailClipmap map;
	map.update(cm, nullptr, g, glm::vec3(5.0f, 5.0f, 10.0f));
	const TextureAsset* tex = cm.getTexture(map.textureId());
	REQUIRE(tex);
	const auto px = [&](float x, float z) {
		const int ix = static_cast<int>(std::floor(x / 0.25f)), iz = static_cast<int>(std::floor(z / 0.25f));
		return &tex->data[(static_cast<size_t>(((iz % 512) + 512) % 512) * 512 + ((ix % 512) + 512) % 512) * 4];
	};
	CHECK(px(5.1f, 10.1f)[0] < 40);           // the groove: a pit
	CHECK(px(5.1f, 10.65f)[0] > 150);         // the heap: above the middle
	CHECK(px(5.1f, 10.65f)[3] == 128);
	std::filesystem::remove_all(dir);
}

TEST_CASE("TrailField: region versions say where the ground changed, also after a page healed away")
{
	TrailField f;
	CHECK(f.regionVersion(0.0f, 0.0f, 50.0f, 50.0f) == 0u);
	f.stampEllipse(10.0f, 10.0f, 0.5f, 0.5f, 0.0f, 0.8f, TrailSurface::Snow);
	const uint64_t a = f.regionVersion(0.0f, 0.0f, 50.0f, 50.0f);
	CHECK(a > 0u);
	CHECK(f.regionVersion(300.0f, 300.0f, 350.0f, 350.0f) == 0u);        // another page: untouched
	f.stampEllipse(300.0f, 300.0f, 0.5f, 0.5f, 0.0f, 0.8f, TrailSurface::Snow);
	CHECK(f.regionVersion(0.0f, 0.0f, 50.0f, 50.0f) == a);               // the first rectangle did not change
	CHECK(f.regionVersion(300.0f, 300.0f, 350.0f, 350.0f) > a);
	TrailWeather snow;
	snow.precip = 1.0f; snow.snowing = true;
	f.tick(0.0, snow);
	f.tick(500.0, snow);                                                   // everything healed, both pages freed
	CHECK(f.pageCount() == 0u);
	CHECK(f.regionVersion(0.0f, 0.0f, 50.0f, 50.0f) > a);                 // a mesh built with the pit is told it is gone
}

TEST_CASE("TrailWindow reads like the field")
{
	TrailField f;
	f.plowCapsule(0.0f, 10.0f, 10.0f, 10.0f, 0.4f, 0.8f, 0.5f, 0.5f, TrailSurface::Snow);
	const HE::TrailWindow w = f.window(-1.0f, 8.0f, 11.0f, 12.0f);
	CHECK(w.any());
	for (float x = 0.5f; x < 10.0f; x += 1.1f)
		for (float z = 9.0f; z < 11.5f; z += 0.13f)
			CHECK(w.sample(x, z) == doctest::Approx(f.sample(x, z)).epsilon(1e-4));
	CHECK(w.sample(500.0f, 500.0f) == 0.0f);                              // outside the copy
	CHECK_FALSE(f.window(100.0f, 100.0f, 110.0f, 110.0f).any());
}

TEST_CASE("Refined terrain chunk: a height offset lowers pits and raises heaps, tilts the normals, deepens the skirt")
{
	const std::vector<float> flat(33u * 33u, 0.0f);
	TerrainDisplacementMap none;
	// A pit of 0.3 m around the chunk centre and a 0.2 m ridge along u = 0.75.
	TerrainHeightOffset off;
	off.maxAbs = 0.3f;
	off.fn = [](float u, float v) {
		const float du = u - 0.5f, dv = v - 0.5f;
		float h = -0.3f * std::max(0.0f, 1.0f - std::sqrt(du * du + dv * dv) / 0.1f);
		h += 0.2f * std::max(0.0f, 1.0f - std::fabs(u - 0.75f) / 0.05f);
		return h;
	};
	const StaticMeshAsset plain = generateTerrainChunkMeshTessellated(flat, 33, 32.0f, 32.0f, 0, 0, 1, 1, 65, 1.0f, none, nullptr);
	const StaticMeshAsset shown = generateTerrainChunkMeshTessellated(flat, 33, 32.0f, 32.0f, 0, 0, 1, 1, 65, 1.0f, none, &off);
	REQUIRE(plain.vertices.size() == shown.vertices.size());
	const size_t N = 65;
	auto y = [&](const StaticMeshAsset& m, size_t i, size_t j) { return m.vertices[(j * N + i) * 3 + 1]; };
	CHECK(y(plain, 32, 32) == 0.0f);
	CHECK(y(shown, 32, 32) == doctest::Approx(-0.3f).epsilon(0.02));    // the pit's floor
	CHECK(y(shown, 48, 32) == doctest::Approx(0.2f).epsilon(0.05));     // the ridge's crest (u = 0.75)
	CHECK(y(shown, 4, 4) == 0.0f);                                       // untouched ground stays
	// The wall of the pit leans toward its floor: a vertex east of the centre has a normal with x < 0 ... in the
	// engine's convention n = (-gx, 1, -gz), the height rises toward +x there, so nx < 0.
	const size_t k = (32 * N + 34) * 3;
	CHECK(shown.normals[k + 0] < -0.05f);
	CHECK(plain.normals[k + 0] == doctest::Approx(0.0f).epsilon(1e-3));
	// No offset, an empty one or maxAbs 0: the plain mesh, bit for bit.
	TerrainHeightOffset inert;
	const StaticMeshAsset same = generateTerrainChunkMeshTessellated(flat, 33, 32.0f, 32.0f, 0, 0, 1, 1, 65, 1.0f, none, &inert);
	CHECK(same.vertices == plain.vertices);
	CHECK(same.normals == plain.normals);
}
