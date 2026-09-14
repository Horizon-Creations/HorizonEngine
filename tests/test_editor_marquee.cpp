#include "doctest.h"
#include "EditorMarquee.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/vec4.hpp>

// The rubber-band rule the Scene viewport selects by. A fixed camera and a
// unit box, and the questions are the ones a user asks with the mouse: is a
// frame around the whole thing a hit, is a frame across half of it, is a frame
// over where it stands, and does something behind me ever count.

namespace
{
// 10 units back on +Z, looking at the origin; 4:3 perspective.
glm::mat4 testViewProj()
{
	const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f),
	                                   glm::vec3(0.0f, 1.0f, 0.0f));
	const glm::mat4 proj = glm::perspective(glm::radians(45.0f), 4.0f / 3.0f, 0.1f, 1000.0f);
	return proj * view;
}

HE::AABB unitBox()
{
	HE::AABB b;
	b.expand({ -0.5f, -0.5f, -0.5f });
	b.expand({  0.5f,  0.5f,  0.5f });
	return b;
}
} // namespace

TEST_CASE("EditorMarquee: projection lands the origin mid-screen and rejects what is behind")
{
	const glm::mat4 vp = testViewProj();
	glm::vec2 uv;
	REQUIRE(EditorMarquee::project(vp, glm::vec3(0.0f), uv));
	CHECK(uv.x == doctest::Approx(0.5f).epsilon(1e-4f));
	CHECK(uv.y == doctest::Approx(0.5f).epsilon(1e-4f));

	// Up in the world is up on screen (v shrinks), right is right.
	REQUIRE(EditorMarquee::project(vp, glm::vec3(1.0f, 1.0f, 0.0f), uv));
	CHECK(uv.x > 0.5f);
	CHECK(uv.y < 0.5f);

	// Behind the camera: not a point on screen at all.
	CHECK_FALSE(EditorMarquee::project(vp, glm::vec3(0.0f, 0.0f, 20.0f), uv));
}

TEST_CASE("EditorMarquee: a frame around the whole box is a hit, across half of it is not")
{
	const glm::mat4 vp = testViewProj();
	const HE::AABB box = unitBox();
	// The box is offset so its pivot is nowhere near the frames below — these
	// cases are about the box, not the pivot rule.
	const glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 0.0f));

	// The screen extent of all eight corners, which the frame then clears by a
	// hair. Off the view axis the far face shifts toward the screen centre, so
	// the near face alone does not bound the projection.
	glm::vec2 lo(1.0f), hi(0.0f);
	for (int i = 0; i < 8; ++i)
	{
		const glm::vec3 corner{ (i & 1) ? box.max.x : box.min.x,
		                        (i & 2) ? box.max.y : box.min.y,
		                        (i & 4) ? box.max.z : box.min.z };
		glm::vec2 uv;
		REQUIRE(EditorMarquee::project(vp, glm::vec3(model * glm::vec4(corner, 1.0f)), uv));
		lo = glm::min(lo, uv);
		hi = glm::max(hi, uv);
	}
	const EditorMarquee::Rect whole = EditorMarquee::Rect::fromCorners(
		{ lo.x - 0.01f, lo.y - 0.01f }, { hi.x + 0.01f, hi.y + 0.01f });
	CHECK(EditorMarquee::encloses(vp, whole, box, model));

	// Corner order does not matter to fromCorners.
	const EditorMarquee::Rect wholeFlipped = EditorMarquee::Rect::fromCorners(
		{ hi.x + 0.01f, hi.y + 0.01f }, { lo.x - 0.01f, lo.y - 0.01f });
	CHECK(EditorMarquee::encloses(vp, wholeFlipped, box, model));

	// Only the left half: the right-hand corners fall outside, and the frame's
	// right edge stops short of the pivot (the box's centre) so the pivot rule
	// cannot answer instead.
	glm::vec2 pivot;
	REQUIRE(EditorMarquee::project(vp, glm::vec3(2.0f, 0.0f, 0.0f), pivot));
	const EditorMarquee::Rect half = EditorMarquee::Rect::fromCorners(
		{ lo.x - 0.01f, lo.y - 0.01f }, { pivot.x - 0.02f, hi.y + 0.01f });
	CHECK_FALSE(EditorMarquee::encloses(vp, half, box, model));

	// A frame somewhere else entirely.
	const EditorMarquee::Rect elsewhere = EditorMarquee::Rect::fromCorners({ 0.0f, 0.0f }, { 0.2f, 0.2f });
	CHECK_FALSE(EditorMarquee::encloses(vp, elsewhere, box, model));
}

TEST_CASE("EditorMarquee: a frame over the pivot selects a box too big to enclose")
{
	const glm::mat4 vp = testViewProj();
	// A ground slab 100 units across: no frame inside the viewport can enclose
	// it, but a small frame over its origin still selects it.
	HE::AABB slab;
	slab.expand({ -50.0f, -0.1f, -50.0f });
	slab.expand({  50.0f,  0.1f,  50.0f });
	const glm::mat4 model(1.0f);

	const EditorMarquee::Rect overPivot = EditorMarquee::Rect::fromCorners({ 0.45f, 0.45f }, { 0.55f, 0.55f });
	CHECK(EditorMarquee::encloses(vp, overPivot, slab, model));
	const EditorMarquee::Rect beside = EditorMarquee::Rect::fromCorners({ 0.6f, 0.45f }, { 0.7f, 0.55f });
	CHECK_FALSE(EditorMarquee::encloses(vp, beside, slab, model));

	// An entity with no box at all goes by its pivot alone.
	CHECK(EditorMarquee::encloses(vp, overPivot, HE::AABB{}, model));
	CHECK_FALSE(EditorMarquee::encloses(vp, beside, HE::AABB{}, model));
}

TEST_CASE("EditorMarquee: a box straddling the camera plane is never enclosed")
{
	const glm::mat4 vp = testViewProj();
	// A long bar from in front of the camera to behind it. Its far-behind
	// corners would project to nonsense; the rule says no rather than guessing.
	HE::AABB bar;
	bar.expand({ -0.5f, -0.5f, -5.0f });
	bar.expand({  0.5f,  0.5f, 15.0f });
	// Pivot pushed off to the side so the pivot rule cannot answer instead.
	const glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 0.0f, 0.0f));
	const EditorMarquee::Rect everything = EditorMarquee::Rect::fromCorners({ 0.0f, 0.0f }, { 1.0f, 1.0f });
	// The pivot (3,0,0) IS on screen and inside a full-screen frame — so this
	// one is a hit through the pivot…
	CHECK(EditorMarquee::encloses(vp, everything, bar, model));
	// …and with the pivot excluded by a frame that misses it, the box alone
	// cannot make it a hit.
	glm::vec2 pivot;
	REQUIRE(EditorMarquee::project(vp, glm::vec3(3.0f, 0.0f, 0.0f), pivot));
	const EditorMarquee::Rect leftOfPivot = EditorMarquee::Rect::fromCorners({ 0.0f, 0.0f }, { pivot.x - 0.02f, 1.0f });
	CHECK_FALSE(EditorMarquee::encloses(vp, leftOfPivot, bar, model));
}
