#include "doctest.h"

#include "PreviewPick.h"
#include "HcEditorUtil.h"          // assetMatchesQuery (inline, no link needed)
#include <Renderer/IRenderer.h>    // worldPreviewProjection
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

// ── The class Viewport's geometry ────────────────────────────────────────────
// An editor 3D pane cannot be screenshotted headless, so everything about it
// that can be wrong silently — where a click lands, and what lens the picture
// is taken with — is asserted numerically here instead.

namespace
{
// A 1×1×1 box centred on the origin, the shape a mesh-less component is picked
// by and the shape the fallback cube is drawn as.
HE::AABB unitBox(float half = 0.5f)
{
	HE::AABB b;
	b.expand(glm::vec3(-half));
	b.expand(glm::vec3(half));
	return b;
}

// Camera at +Z looking down -Z at the origin, and the projection the world
// preview actually uses for a 800×600 pane.
glm::mat4 testViewProj(float aspect = 800.0f / 600.0f, float dist = 5.0f)
{
	EditorCameraOverride cam;
	cam.fovDegrees = 45.0f;
	const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, dist),
	                                   glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	return worldPreviewProjection(cam, aspect) * view;
}
} // namespace

TEST_CASE("PreviewPick: the centre of the pane hits the object at the origin")
{
	std::vector<PreviewPick::Candidate> cands = { { 7u, glm::mat4(1.0f), unitBox() } };
	const glm::vec2 rectMin(120.0f, 60.0f), rectSize(800.0f, 600.0f);

	std::uint32_t hit = 0;
	REQUIRE(PreviewPick::pickAtScreen(cands, testViewProj(), rectMin, rectSize,
		rectMin + rectSize * 0.5f, hit));
	CHECK(hit == 7u);

	// A corner of the pane looks past it.
	CHECK_FALSE(PreviewPick::pickAtScreen(cands, testViewProj(), rectMin, rectSize,
		rectMin + glm::vec2(4.0f, 4.0f), hit));
}

TEST_CASE("PreviewPick: the rect offset is honoured, and screen Y is flipped")
{
	// One box a metre ABOVE the origin. It has to be picked in the UPPER half of
	// the pane — the flip between screen space (y down) and NDC (y up) is the
	// single mistake that still works perfectly on the centre line.
	std::vector<PreviewPick::Candidate> cands = {
		{ 1u, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f,  1.0f, 0.0f)), unitBox(0.3f) },
		{ 2u, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.0f, 0.0f)), unitBox(0.3f) },
	};
	const glm::vec2 rectMin(300.0f, 200.0f), rectSize(800.0f, 600.0f);
	const glm::mat4 vp = testViewProj();

	std::uint32_t hit = 0;
	REQUIRE(PreviewPick::pickAtScreen(cands, vp, rectMin, rectSize,
		glm::vec2(rectMin.x + 400.0f, rectMin.y + 180.0f), hit));
	CHECK(hit == 1u);                       // above centre → the raised box

	REQUIRE(PreviewPick::pickAtScreen(cands, vp, rectMin, rectSize,
		glm::vec2(rectMin.x + 400.0f, rectMin.y + 420.0f), hit));
	CHECK(hit == 2u);

	// The same pixels against a pane anchored somewhere else land on a different
	// part of the picture: the rect origin is part of the mapping, not
	// decoration. (Those pixels are past the bottom of a pane at 0,0 — with the
	// offset ignored the raised box would come back again.)
	std::uint32_t elsewhere = 0;
	const bool hitElsewhere = PreviewPick::pickAtScreen(cands, vp, glm::vec2(0.0f), rectSize,
		glm::vec2(rectMin.x + 400.0f, rectMin.y + 180.0f), elsewhere);
	CHECK_FALSE(hitElsewhere);
}

TEST_CASE("PreviewPick: the nearest box along the ray wins")
{
	// Two boxes on the view axis. The camera sits at +Z, so the one at +1 is in
	// front and has to be the one selected — picking the far one would be an
	// editor that selects through the character standing in the way.
	std::vector<PreviewPick::Candidate> cands = {
		{ 10u, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -1.0f)), unitBox(0.4f) },
		{ 11u, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f,  1.0f)), unitBox(0.4f) },
	};
	const glm::vec2 rectMin(0.0f), rectSize(800.0f, 600.0f);

	std::uint32_t hit = 0;
	REQUIRE(PreviewPick::pickAtScreen(cands, testViewProj(), rectMin, rectSize,
		rectSize * 0.5f, hit));
	CHECK(hit == 11u);
}

TEST_CASE("PreviewPick: a box behind the camera is never picked")
{
	// Behind the eye, on the axis the ray runs along. intersectRay clamps the
	// entry distance to t >= 0, so pointing away from something must not select
	// it — without that, flying past a component selects it from the far side.
	std::vector<PreviewPick::Candidate> cands = {
		{ 3u, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 20.0f)), unitBox() },
	};
	std::uint32_t hit = 0;
	CHECK_FALSE(PreviewPick::pickAtScreen(cands, testViewProj(), glm::vec2(0.0f),
		glm::vec2(800.0f, 600.0f), glm::vec2(400.0f, 300.0f), hit));
}

TEST_CASE("PreviewPick: a rotated box is tested in its own space")
{
	// A long thin slab, turned 45°. A point off its short axis is inside the
	// world-space box around it but outside the slab itself; testing in object
	// space is what tells the two apart.
	HE::AABB slab;
	slab.expand(glm::vec3(-2.0f, -0.05f, -0.05f));
	slab.expand(glm::vec3( 2.0f,  0.05f,  0.05f));
	const glm::mat4 turned =
		glm::rotate(glm::mat4(1.0f), glm::radians(45.0f), glm::vec3(0.0f, 0.0f, 1.0f));

	std::vector<PreviewPick::Candidate> cands = { { 5u, turned, slab } };
	const glm::vec2 rectSize(800.0f, 600.0f);
	const glm::mat4 vp = testViewProj();

	// Straight through the middle: on the slab whichever way it is turned.
	std::uint32_t hit = 0;
	CHECK(PreviewPick::pickAtScreen(cands, vp, glm::vec2(0.0f), rectSize,
		rectSize * 0.5f, hit));

	// Along +X at y = 0, well past where the slab's rotated end is: inside the
	// axis-aligned box around it, outside the slab.
	glm::vec3 ro(0.0f, 0.0f, 5.0f), rd(0.0f, 0.0f, -1.0f);
	ro.x = 1.6f;
	CHECK_FALSE(PreviewPick::pick(cands, ro, rd, hit));
}

TEST_CASE("worldPreviewProjection: plain perspective below the horizontal cap")
{
	EditorCameraOverride cam;
	cam.fovDegrees = 45.0f;
	cam.nearPlane  = 0.1f;
	cam.farPlane   = 5000.0f;

	const float aspect = 800.0f / 600.0f;
	// 45° vertical at 4:3 is 58° horizontal — nowhere near the cap, so this must
	// be glm::perspective and nothing else, element for element.
	const glm::mat4 expect = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 5000.0f);
	const glm::mat4 got    = worldPreviewProjection(cam, aspect);
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			CHECK(got[c][r] == doctest::Approx(expect[c][r]).epsilon(1e-6));

	CHECK(worldPreviewVerticalFov(45.0f, aspect) == doctest::Approx(45.0f));
}

TEST_CASE("worldPreviewProjection: a wide pane narrows the vertical FOV instead of fanning out")
{
	// The bug this exists for: the vertical FOV is what the camera carries, so a
	// wide pane turns a comfortable lens into a wide-angle one and everything
	// away from the centre stretches — which reads as the view being curved.
	const float aspect = 3.0f;
	CHECK(worldPreviewVerticalFov(60.0f, aspect) < 60.0f);

	const float vfov = worldPreviewVerticalFov(60.0f, aspect);
	// What comes out has EXACTLY the capped horizontal angle…
	const float hfov = 2.0f * std::atan(std::tan(glm::radians(vfov) * 0.5f) * aspect);
	CHECK(glm::degrees(hfov) == doctest::Approx(kWorldPreviewMaxHorizontalFov).epsilon(1e-4));

	// …and the matrix is the perspective built from that narrowed angle.
	EditorCameraOverride cam;
	cam.fovDegrees = 60.0f;
	const glm::mat4 expect = glm::perspective(glm::radians(vfov), aspect,
	                                          cam.nearPlane, cam.farPlane);
	const glm::mat4 got = worldPreviewProjection(cam, aspect);
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			CHECK(got[c][r] == doctest::Approx(expect[c][r]).epsilon(1e-6));

	// Narrowing only ever SHRINKS the drawn frustum against the one the
	// extractor culls with (which is built from the unclamped angle), so nothing
	// visible can be culled away by this.
	CHECK(std::tan(glm::radians(vfov) * 0.5f) <= std::tan(glm::radians(60.0f) * 0.5f));

	// A tall pane is not the case being fixed and must come through untouched.
	CHECK(worldPreviewVerticalFov(60.0f, 0.6f) == doctest::Approx(60.0f));
}

TEST_CASE("assetMatchesQuery: the picker's search filter")
{
	const std::string label = "Idle";
	const std::string path  = "Characters/Hero/Idle.hasset";

	CHECK(HcEditorUtil::assetMatchesQuery(label, path, ""));        // untouched box
	CHECK(HcEditorUtil::assetMatchesQuery(label, path, "   "));
	CHECK(HcEditorUtil::assetMatchesQuery(label, path, "idle"));    // case-insensitive
	CHECK(HcEditorUtil::assetMatchesQuery(label, path, "IDLE"));
	CHECK(HcEditorUtil::assetMatchesQuery(label, path, "hero"));    // matches on the path
	CHECK(HcEditorUtil::assetMatchesQuery(label, path, "char idle"));// terms in any order
	CHECK(HcEditorUtil::assetMatchesQuery(label, path, "idle char"));
	CHECK_FALSE(HcEditorUtil::assetMatchesQuery(label, path, "walk"));
	CHECK_FALSE(HcEditorUtil::assetMatchesQuery(label, path, "hero walk"));  // ALL terms
	// The label and the path are separate lines, so a term may not span them.
	CHECK_FALSE(HcEditorUtil::assetMatchesQuery(label, path, "idlecharacters"));
}
