#include "doctest.h"
#include <HorizonRendering/WorldPreviewGrid.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderWorld.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <cmath>

// The backdrop of IRenderer::RenderWorldPreview. It lives in a shared header
// precisely so OpenGL and Metal cannot end up with differently-sized grids, and
// these checks are what makes that promise testable at all — neither backend's
// draw path can run in CI.

namespace
{
struct Vert { glm::vec3 pos; glm::vec3 color; };

std::vector<Vert> unpack(const std::vector<float>& raw, size_t fromVertex = 0)
{
	std::vector<Vert> out;
	for (size_t i = fromVertex * 6; i + 5 < raw.size(); i += 6)
		out.push_back({ { raw[i], raw[i + 1], raw[i + 2] }, { raw[i + 3], raw[i + 4], raw[i + 5] } });
	return out;
}
} // namespace

TEST_CASE("buildPreviewGround is two triangles just below the grid plane")
{
	std::vector<float> raw;
	HE::buildPreviewGround(10.0f, raw);
	REQUIRE(raw.size() == 6 * 6); // 6 vertices × (pos3 + color3)

	for (const Vert& v : unpack(raw))
	{
		// Below y = 0, or the grid lines lying exactly on the plane z-fight.
		CHECK(v.pos.y < 0.0f);
		CHECK(std::abs(v.pos.x) == doctest::Approx(10.0f));
		CHECK(std::abs(v.pos.z) == doctest::Approx(10.0f));
	}
}

TEST_CASE("buildPreviewGrid lays whole line pairs on the ground plane")
{
	std::vector<float> raw;
	HE::buildPreviewGrid(5.0f, 1.0f, raw);
	REQUIRE(raw.size() % 6 == 0);
	const std::vector<Vert> verts = unpack(raw);
	CHECK(verts.size() % 2 == 0); // GL_LINES / MTLPrimitiveTypeLine: pairs

	// 11 lines each way (−5..5), 2 vertices apiece, plus the origin marker.
	CHECK(verts.size() >= 11u * 2u * 2u);

	int aboveGround = 0;
	for (const Vert& v : verts)
	{
		CHECK(v.pos.y >= 0.0f);
		if (v.pos.y > 0.01f) ++aboveGround;
	}
	// The origin marker's up axis is the only thing standing off the plane.
	CHECK(aboveGround == 1);
}

TEST_CASE("buildPreviewGrid tints the two world axes apart from the plain lines")
{
	std::vector<float> raw;
	HE::buildPreviewGrid(3.0f, 1.0f, raw);
	const std::vector<Vert> verts = unpack(raw);

	// The line running along Z at x = 0 is the Z axis and must be blue-dominant;
	// the one running along X at z = 0 red-dominant. Anything else is neutral.
	bool sawBlueZ = false, sawRedX = false;
	for (size_t i = 0; i + 1 < verts.size(); i += 2)
	{
		const Vert& a = verts[i];
		const Vert& b = verts[i + 1];
		const bool alongZ = (a.pos.x == b.pos.x) && (a.pos.z != b.pos.z);
		const bool alongX = (a.pos.z == b.pos.z) && (a.pos.x != b.pos.x);
		if (alongZ && a.pos.x == doctest::Approx(0.0f) && a.color.b > a.color.r) sawBlueZ = true;
		if (alongX && a.pos.z == doctest::Approx(0.0f) && a.color.r > a.color.b) sawRedX = true;
		if (alongZ && std::abs(a.pos.x) > 0.5f) CHECK(a.color.r == doctest::Approx(a.color.b).epsilon(0.2));
	}
	CHECK(sawBlueZ);
	CHECK(sawRedX);
}

TEST_CASE("buildPreviewGrid marks the origin with an up axis")
{
	std::vector<float> raw;
	HE::buildPreviewGrid(4.0f, 2.0f, raw);
	const std::vector<Vert> verts = unpack(raw);

	bool sawUpAxis = false;
	for (size_t i = 0; i + 1 < verts.size(); i += 2)
		if (verts[i].pos == glm::vec3(0.0f) && verts[i + 1].pos.y == doctest::Approx(2.0f))
			sawUpAxis = true; // scales with `step`, so it reads at any grid density
	CHECK(sawUpAxis);
}

TEST_CASE("the backdrop follows the origin it is given")
{
	// A class whose ROOT carries an offset must still see its own origin marked
	// and the grid laid out around it — the alternative frames the character
	// against an empty patch of grid, which reads as a broken marker rather
	// than a moved root.
	const glm::vec3 o(3.0f, 1.5f, -2.0f);
	std::vector<float> raw;
	HE::buildPreviewGround(6.0f, raw, o);
	for (const Vert& v : unpack(raw))
	{
		CHECK(v.pos.y == doctest::Approx(o.y - HE::kPreviewGroundDrop));
		CHECK(std::abs(v.pos.x - o.x) == doctest::Approx(6.0f));
		CHECK(std::abs(v.pos.z - o.z) == doctest::Approx(6.0f));
	}

	std::vector<float> grid;
	HE::buildPreviewGrid(6.0f, 1.0f, grid, o);
	const std::vector<Vert> verts = unpack(grid);

	// The marker's up axis starts exactly on the origin.
	bool sawUpAxis = false;
	for (size_t i = 0; i + 1 < verts.size(); i += 2)
		if (verts[i].pos == o && verts[i + 1].pos == o + glm::vec3(0.0f, 1.0f, 0.0f))
			sawUpAxis = true;
	CHECK(sawUpAxis);

	// And the two tinted axes run THROUGH it, which only holds because the
	// lines are laid out from the origin rather than from world zero.
	bool sawBlueZ = false, sawRedX = false;
	for (size_t i = 0; i + 1 < verts.size(); i += 2)
	{
		const Vert& a = verts[i];
		const Vert& b = verts[i + 1];
		if (a.pos.x == doctest::Approx(o.x) && b.pos.x == doctest::Approx(o.x) &&
		    a.pos.z != b.pos.z && a.color.b > a.color.r) sawBlueZ = true;
		if (a.pos.z == doctest::Approx(o.z) && b.pos.z == doctest::Approx(o.z) &&
		    a.pos.x != b.pos.x && a.color.r > a.color.b) sawRedX = true;
	}
	CHECK(sawBlueZ);
	CHECK(sawRedX);
}

TEST_CASE("the sky dome surrounds the camera and carries the sky's own colours")
{
	const glm::vec3 cam(5.0f, 2.0f, -3.0f);
	const glm::vec3 sun = glm::normalize(glm::vec3(0.3f, 0.8f, 0.5f));
	std::vector<float> raw;
	HE::buildPreviewSkyDome(cam, 100.0f, sun, raw);

	REQUIRE(raw.size() % 6 == 0);
	const std::vector<Vert> verts = unpack(raw);
	CHECK(verts.size() % 3 == 0);   // triangles
	REQUIRE(!verts.empty());

	bool above = false, below = false;
	for (const Vert& v : verts)
	{
		// Centred on the CAMERA, at the given radius — that is what makes it
		// impossible to fly out of the sky.
		CHECK(glm::length(v.pos - cam) == doctest::Approx(100.0f).epsilon(0.001));
		// Colours come from the scattering model, so they must at least be
		// finite and non-negative; a NaN here paints black holes in the sky.
		CHECK(std::isfinite(v.color.r));
		CHECK(std::isfinite(v.color.g));
		CHECK(std::isfinite(v.color.b));
		CHECK(v.color.r >= 0.0f);
		if (v.pos.y > cam.y + 50.0f) above = true;
		if (v.pos.y < cam.y - 50.0f) below = true;
	}
	CHECK(above);   // a full sphere, so looking down still finds sky
	CHECK(below);
}

TEST_CASE("the sky dome answers different sun positions differently")
{
	// The one thing that makes a time-of-day slider worth having: moving the sun
	// has to change the picture. A dome that ignored `sunDir` would still pass
	// every check above.
	const glm::vec3 cam(0.0f);
	std::vector<float> noon, dusk;
	HE::buildPreviewSkyDome(cam, 50.0f, glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f)), noon);
	HE::buildPreviewSkyDome(cam, 50.0f, glm::normalize(glm::vec3(0.98f, 0.05f, 0.0f)), dusk);
	REQUIRE(noon.size() == dusk.size());

	int differing = 0;
	for (size_t i = 0; i < noon.size(); i += 6)
		for (int c = 3; c < 6; ++c)
			if (std::abs(noon[i + c] - dusk[i + c]) > 0.01f) { ++differing; break; }
	CHECK(differing > 0);
}

TEST_CASE("the dome's sky is the same gradient WITHOUT the sun's glare lobes")
{
	// The disc is a fraction of a degree wide; interpolated across triangles it
	// does not read as a sun but as a huge faceted white polygon — which is what
	// the first version of the dome looked like. So the dome asks for the
	// gradient only, and that has to be a real difference near the sun.
	const glm::vec3 sun = glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f));
	const glm::vec3 atSun = HE::SkyColorCPU(sun, sun, /*withSunDisc=*/false);
	const glm::vec3 withDisc = HE::SkyColorCPU(sun, sun, /*withSunDisc=*/true);
	CHECK(withDisc.r > atSun.r + 1.0f);   // the disc is worth many units of radiance
	CHECK(atSun.r < 4.0f);                // …and without it nothing blows out

	// Away from the sun the two must agree — the disc is the ONLY difference,
	// so the IBL bake and the dome still describe the same sky.
	const glm::vec3 away(0.0f, 0.2f, 1.0f);
	const glm::vec3 a = HE::SkyColorCPU(away, sun, false);
	const glm::vec3 b = HE::SkyColorCPU(away, sun, true);
	CHECK(a.r == doctest::Approx(b.r).epsilon(0.05));
	CHECK(a.b == doctest::Approx(b.b).epsilon(0.05));
}

TEST_CASE("day-night lights a world that brought no light of its own")
{
	// What a preview hands over: geometry, no lights. The moon has always been
	// synthesised in that case; the sun was not, so such a world was lit by
	// moonlight at noon — visible as a mesh that the time-of-day slider moved
	// the sky of but never the shading.
	HorizonWorld world;
	const Entity e = world.createEntity("Cube");
	world.addComponent(e, TransformComponent{});
	world.addComponent(e, MeshComponent{});

	RenderExtractor ex;
	ex.setDayNight(true, /*timeOfDay=*/0.5f,          // noon
	               glm::vec3(1.0f, 0.97f, 0.90f), 2.2f,
	               glm::vec3(0.55f, 0.65f, 0.95f), 0.66f, /*cloudCoverage=*/0.2f);
	RenderWorld rw;
	ex.extract(world, rw, 1.0f);

	glm::vec3 toward(0.0f), colorIntensity(0.0f);
	REQUIRE(rw.dominantDirectionalLight(toward, colorIntensity));
	CHECK(toward.y > 0.5f);                       // the sun is up at noon…
	CHECK(glm::length(colorIntensity) > 0.5f);    // …and actually shining

	// And at midnight the dominant light is the moon's — dimmer, and from the
	// other side. If this stopped holding, "noon" and "midnight" would look the
	// same and the slider would be decoration.
	RenderExtractor night;
	night.setDayNight(true, 0.0f, glm::vec3(1.0f, 0.97f, 0.90f), 2.2f,
	                  glm::vec3(0.55f, 0.65f, 0.95f), 0.66f, 0.2f);
	RenderWorld rwNight;
	night.extract(world, rwNight, 1.0f);
	glm::vec3 nToward(0.0f), nColor(0.0f);
	if (rwNight.dominantDirectionalLight(nToward, nColor))
		CHECK(glm::length(nColor) < glm::length(colorIntensity));
}

TEST_CASE("buildPreviewGrid survives a zero step instead of looping forever")
{
	std::vector<float> raw;
	HE::buildPreviewGrid(2.0f, 0.0f, raw);
	CHECK(raw.size() % 6 == 0);
	CHECK(!raw.empty());
}

TEST_CASE("ground and grid append, so one buffer can hold both")
{
	std::vector<float> raw;
	HE::buildPreviewGround(8.0f, raw);
	const size_t groundVerts = raw.size() / 6;
	HE::buildPreviewGrid(8.0f, 1.0f, raw);

	// The backends draw [0, groundVerts) as triangles and the rest as lines —
	// that split only works if the grid strictly appends.
	CHECK(groundVerts == 6);
	CHECK(raw.size() / 6 > groundVerts);
	for (const Vert& v : unpack(raw, groundVerts)) CHECK(v.pos.y >= 0.0f);
}
