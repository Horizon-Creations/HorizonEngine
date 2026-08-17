#include "doctest.h"
#include <HorizonRendering/WorldPreviewGrid.h>
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
