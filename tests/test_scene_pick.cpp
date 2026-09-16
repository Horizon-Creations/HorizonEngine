#include "doctest.h"
#include <HorizonRendering/ScenePick.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

// The collision probe behind dropping an asset into the viewport: a ray from the
// camera through the cursor has to come back with the nearest SURFACE, not the
// nearest bounding box, or the asset floats above whatever it was dropped on.

namespace
{
	// A 2x2 quad in the XZ plane at y = 0, facing up. Two triangles, so the
	// winding/normal handling is exercised on a real (non-degenerate) surface.
	struct Quad
	{
		std::vector<float>    positions = { -1.0f, 0.0f, -1.0f,
		                                     1.0f, 0.0f, -1.0f,
		                                     1.0f, 0.0f,  1.0f,
		                                    -1.0f, 0.0f,  1.0f };
		std::vector<uint32_t> indices   = { 0, 1, 2,  0, 2, 3 };
		HE::AABB              bounds    = [] {
			HE::AABB b; b.expand({ -1.0f, 0.0f, -1.0f }); b.expand({ 1.0f, 0.0f, 1.0f }); return b;
		}();
	};

	Quad         g_quad;
	const HE::UUID kQuadId{ 1, 1 };

	HE::ScenePick::MeshLookup quadLookup()
	{
		return [](const HE::UUID& id, HE::ScenePick::MeshGeometry& out)
		{
			if (!(id == kQuadId)) return false;
			out.positions   = g_quad.positions.data();
			out.stride      = 3;
			out.vertexCount = g_quad.positions.size() / 3;
			out.indices     = g_quad.indices.data();
			out.indexCount  = g_quad.indices.size();
			out.bounds      = &g_quad.bounds;
			return true;
		};
	}

	RenderObject quadAt(const glm::vec3& pos, uint32_t entityId, float scale = 1.0f)
	{
		RenderObject o;
		o.meshAssetId = kQuadId;
		o.entityId    = entityId;
		o.transform   = glm::scale(glm::translate(glm::mat4(1.0f), pos), glm::vec3(scale));
		return o;
	}
}

TEST_CASE("ScenePick returns the surface point straight down onto a quad")
{
	RenderWorld w;
	w.objects.push_back(quadAt({ 0.0f, 0.0f, 0.0f }, 7));

	const auto hit = HE::ScenePick::raycast(w, quadLookup(),
		glm::vec3(0.25f, 5.0f, -0.25f), glm::vec3(0.0f, -1.0f, 0.0f));

	REQUIRE(hit.hit);
	CHECK(hit.entityId == 7);
	CHECK(hit.point.x == doctest::Approx(0.25f));
	CHECK(hit.point.y == doctest::Approx(0.0f));
	CHECK(hit.point.z == doctest::Approx(-0.25f));
	CHECK(hit.distance == doctest::Approx(5.0f));
	// Normal faces back along the ray, whichever way the triangle was wound.
	CHECK(hit.normal.y == doctest::Approx(1.0f));
}

TEST_CASE("ScenePick misses past the edge of the geometry")
{
	RenderWorld w;
	w.objects.push_back(quadAt({ 0.0f, 0.0f, 0.0f }, 7));

	// Beside the surface entirely.
	CHECK_FALSE(HE::ScenePick::raycast(w, quadLookup(),
		glm::vec3(1.5f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)).hit);
	// Pointing away from the surface never hits it either.
	CHECK_FALSE(HE::ScenePick::raycast(w, quadLookup(),
		glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)).hit);
}

TEST_CASE("ScenePick is triangle-exact, not box-exact")
{
	// One triangle covering the half of its bounding box where x + z <= 0. A
	// probe over the OTHER half is inside the box and still has to miss — this
	// is the whole reason the probe traces triangles: dropping an asset next to
	// a sloped roof or a terrain chunk must not place it on the box's face.
	static const std::vector<float>    tri = { -1.0f, 0.0f, -1.0f,
	                                            1.0f, 0.0f, -1.0f,
	                                           -1.0f, 0.0f,  1.0f };
	static const std::vector<uint32_t> idx = { 0, 1, 2 };
	static const HE::AABB box = [] {
		HE::AABB b; b.expand({ -1.0f, 0.0f, -1.0f }); b.expand({ 1.0f, 0.0f, 1.0f }); return b;
	}();
	const HE::ScenePick::MeshLookup lookup = [](const HE::UUID&, HE::ScenePick::MeshGeometry& out)
	{
		out.positions = tri.data(); out.stride = 3; out.vertexCount = 3;
		out.indices = idx.data();   out.indexCount = idx.size(); out.bounds = &box;
		return true;
	};

	RenderWorld w;
	w.objects.push_back(quadAt({ 0.0f, 0.0f, 0.0f }, 1));

	CHECK(HE::ScenePick::raycast(w, lookup, glm::vec3(-0.5f, 5.0f, -0.5f),
		glm::vec3(0.0f, -1.0f, 0.0f)).hit);                        // on the triangle
	CHECK_FALSE(HE::ScenePick::raycast(w, lookup, glm::vec3(0.5f, 5.0f, 0.5f),
		glm::vec3(0.0f, -1.0f, 0.0f)).hit);                        // in the box, off the triangle
}

TEST_CASE("ScenePick takes the nearest of several surfaces")
{
	RenderWorld w;
	w.objects.push_back(quadAt({ 0.0f, 0.0f, 0.0f }, 1));   // ground
	w.objects.push_back(quadAt({ 0.0f, 2.0f, 0.0f }, 2));   // table on top of it
	w.objects.push_back(quadAt({ 0.0f, -3.0f, 0.0f }, 3));  // basement below

	const auto hit = HE::ScenePick::raycast(w, quadLookup(),
		glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
	REQUIRE(hit.hit);
	CHECK(hit.entityId == 2);
	CHECK(hit.point.y == doctest::Approx(2.0f));

	// Order in the snapshot must not decide it.
	std::swap(w.objects[0], w.objects[2]);
	const auto again = HE::ScenePick::raycast(w, quadLookup(),
		glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
	REQUIRE(again.hit);
	CHECK(again.entityId == 2);
}

TEST_CASE("ScenePick respects each object's transform")
{
	RenderWorld w;
	// Scaled 4x, so its surface reaches out to x = ±4 — a point the unscaled
	// quad's local geometry would miss.
	w.objects.push_back(quadAt({ 0.0f, 1.0f, 0.0f }, 9, 4.0f));

	const auto hit = HE::ScenePick::raycast(w, quadLookup(),
		glm::vec3(3.0f, 6.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
	REQUIRE(hit.hit);
	CHECK(hit.point.y == doctest::Approx(1.0f));
	CHECK(hit.distance == doctest::Approx(5.0f));

	// A tilted surface reports both the contact point and its normal in WORLD space.
	RenderObject tilted = quadAt({ 0.0f, 0.0f, 0.0f }, 10);
	tilted.transform = glm::rotate(glm::mat4(1.0f), glm::radians(45.0f), glm::vec3(0, 0, 1));
	RenderWorld slope;
	slope.objects.push_back(tilted);
	const auto slopeHit = HE::ScenePick::raycast(slope, quadLookup(),
		glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
	REQUIRE(slopeHit.hit);
	CHECK(slopeHit.point.y == doctest::Approx(0.0f).epsilon(0.001));
	CHECK(slopeHit.normal.y == doctest::Approx(std::sqrt(0.5f)).epsilon(0.001));
	CHECK(slopeHit.normal.x == doctest::Approx(-std::sqrt(0.5f)).epsilon(0.001));
}

TEST_CASE("ScenePick tolerates objects it cannot resolve")
{
	RenderWorld w;
	RenderObject unknown = quadAt({ 0.0f, 4.0f, 0.0f }, 1);
	unknown.meshAssetId = HE::UUID{ 99, 99 };   // lookup says no
	w.objects.push_back(unknown);
	RenderObject noMesh = quadAt({ 0.0f, 3.0f, 0.0f }, 2);
	noMesh.meshAssetId = HE::UUID{};            // no mesh at all
	w.objects.push_back(noMesh);
	w.objects.push_back(quadAt({ 0.0f, 0.0f, 0.0f }, 3));

	const auto hit = HE::ScenePick::raycast(w, quadLookup(),
		glm::vec3(0.0f, 8.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
	REQUIRE(hit.hit);
	CHECK(hit.entityId == 3);

	// No lookup at all is a miss, not a crash.
	CHECK_FALSE(HE::ScenePick::raycast(w, {}, glm::vec3(0.0f, 8.0f, 0.0f),
		glm::vec3(0.0f, -1.0f, 0.0f)).hit);
}

// ── Snapping: the filter and the vertex probe ────────────────────────────────
// Surface snapping drags an object over the scene and asks what is under it;
// without a filter the answer is the object itself, every frame. Vertex
// snapping asks which corner of another mesh is nearest on screen.

TEST_CASE("ScenePick: a filtered-out object is looked through")
{
	RenderWorld w;
	w.objects.push_back(quadAt({ 0.0f, 2.0f, 0.0f }, 5));   // the one being dragged
	w.objects.push_back(quadAt({ 0.0f, 0.0f, 0.0f }, 7));   // the floor under it

	const glm::vec3 o(0.0f, 5.0f, 0.0f), d(0.0f, -1.0f, 0.0f);
	// No filter: the upper quad is nearer and wins.
	CHECK(HE::ScenePick::raycast(w, quadLookup(), o, d).entityId == 5);
	// Filtering 5 out: the ray goes through to the floor.
	const auto hit = HE::ScenePick::raycast(w, quadLookup(), o, d,
		[](const RenderObject& obj) { return obj.entityId != 5; });
	REQUIRE(hit.hit);
	CHECK(hit.entityId == 7);
	CHECK(hit.point.y == doctest::Approx(0.0f));
	// Filtering everything out is a clean miss.
	CHECK_FALSE(HE::ScenePick::raycast(w, quadLookup(), o, d,
		[](const RenderObject&) { return false; }).hit);
}

namespace
{
	// A camera looking straight down the -Z axis from z = 5 at a 200×100 picture.
	struct TopCamera
	{
		glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		glm::mat4 proj = glm::perspective(glm::radians(60.0f), 2.0f, 0.1f, 100.0f);
		glm::mat4 viewProj() const { return proj * view; }
		glm::vec2 rectMin{ 10.0f, 20.0f };
		glm::vec2 rectSize{ 200.0f, 100.0f };

		glm::vec2 screenOf(const glm::vec3& world) const
		{
			const glm::vec4 c = viewProj() * glm::vec4(world, 1.0f);
			return { rectMin.x + (c.x / c.w * 0.5f + 0.5f) * rectSize.x,
			         rectMin.y + (0.5f - c.y / c.w * 0.5f) * rectSize.y };
		}
	};
}

TEST_CASE("ScenePick: the nearest vertex on screen, within the radius, not the object itself")
{
	TopCamera cam;
	RenderWorld w;
	// The quad's corners are at (±1, 0, ±1); seen from +Z they project to four
	// points, two of which (z = +1) sit nearer the camera than the other two.
	w.objects.push_back(quadAt({ 0.0f, 0.0f, 0.0f }, 7));
	w.objects.push_back(quadAt({ 0.3f, 0.0f, 0.0f }, 5));   // the dragged one, filtered out

	const glm::vec3 corner(1.0f, 0.0f, 1.0f);
	const glm::vec2 at = cam.screenOf(corner) + glm::vec2(3.0f, -2.0f);   // a few px off

	const auto hit = HE::ScenePick::nearestVertex(w, quadLookup(), cam.viewProj(),
		cam.rectMin, cam.rectSize, at, 24.0f,
		[](const RenderObject& obj) { return obj.entityId != 5; });
	REQUIRE(hit.hit);
	CHECK(hit.entityId == 7);
	CHECK(hit.point.x == doctest::Approx(1.0f));
	CHECK(hit.point.z == doctest::Approx(1.0f));
	CHECK(hit.distancePx == doctest::Approx(std::sqrt(13.0f)).epsilon(0.01));

	// Outside the radius there is nothing to snap to — a free move.
	CHECK_FALSE(HE::ScenePick::nearestVertex(w, quadLookup(), cam.viewProj(),
		cam.rectMin, cam.rectSize, at + glm::vec2(60.0f, 0.0f), 24.0f).hit);

	// Without the filter the dragged object's own corner, 0.3 m nearer the
	// cursor's world line, would be picked — the filter is what stops that.
	const auto self = HE::ScenePick::nearestVertex(w, quadLookup(), cam.viewProj(),
		cam.rectMin, cam.rectSize, cam.screenOf({ 1.3f, 0.0f, 1.0f }), 24.0f);
	REQUIRE(self.hit);
	CHECK(self.entityId == 5);
}

TEST_CASE("ScenePick: vertices behind the camera are never candidates")
{
	TopCamera cam;
	RenderWorld w;
	w.objects.push_back(quadAt({ 0.0f, 0.0f, 12.0f }, 9));   // wholly behind z = 5
	CHECK_FALSE(HE::ScenePick::nearestVertex(w, quadLookup(), cam.viewProj(),
		cam.rectMin, cam.rectSize, cam.rectMin + cam.rectSize * 0.5f, 1e6f).hit);
}
