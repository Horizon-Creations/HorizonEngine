#include "doctest.h"
#include "EditorCamera.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

// The editor scene-view camera is pure math (no ImGui), so it is unit-testable.
// EditorCamera.cpp is compiled directly into the test target.

namespace
{
	// World-space point transformed into view space.
	glm::vec3 toViewSpace(const EditorCamera& cam, const glm::vec3& worldPos)
	{
		return glm::vec3(cam.viewMatrix() * glm::vec4(worldPos, 1.0f));
	}
}

TEST_CASE("EditorCamera starts framed on the world origin")
{
	EditorCamera cam;
	EditorCamera::Input in; in.dt = 0.016f;
	cam.update(in); // first update derives orientation toward the origin

	// The origin must sit dead-centre and in front of the camera.
	const glm::vec3 originVS = toViewSpace(cam, glm::vec3(0.0f));
	CHECK(std::abs(originVS.x) < 1e-3f);
	CHECK(std::abs(originVS.y) < 1e-3f);
	CHECK(originVS.z < 0.0f);

	// Distance to the origin equals how far in front it lands.
	CHECK(glm::length(cam.position()) == doctest::Approx(-originVS.z));

	// The override is active and mirrors the view matrix.
	const EditorCameraOverride ovr = cam.makeOverride();
	CHECK(ovr.active);
	CHECK(ovr.view == cam.viewMatrix());
}

TEST_CASE("EditorCamera dolly moves toward the pivot")
{
	EditorCamera cam;
	EditorCamera::Input in; in.dt = 0.016f;
	cam.update(in);
	const float before = glm::length(cam.position());

	EditorCamera::Input wheelIn; wheelIn.dt = 0.016f; wheelIn.wheel = 1.0f; // zoom in
	cam.update(wheelIn);
	CHECK(glm::length(cam.position()) < before); // closer to the origin
}

TEST_CASE("EditorCamera orbit preserves the pivot distance")
{
	EditorCamera cam;
	EditorCamera::Input init; init.dt = 0.016f;
	cam.update(init);
	const glm::vec3 before = cam.position();
	const float     dist   = glm::length(before); // pivot is the origin

	EditorCamera::Input orbit; orbit.dt = 0.016f;
	orbit.orbit = true;
	orbit.mouseDelta = glm::vec2(120.0f, 40.0f);
	cam.update(orbit);

	// Same radius around the origin, but a different position.
	CHECK(glm::length(cam.position()) == doctest::Approx(dist));
	CHECK(glm::length(cam.position() - before) > 0.1f);
	// Still looking at the origin.
	const glm::vec3 originVS = toViewSpace(cam, glm::vec3(0.0f));
	CHECK(std::abs(originVS.x) < 1e-3f);
	CHECK(std::abs(originVS.y) < 1e-3f);
}

TEST_CASE("EditorCamera focusOn frames a point")
{
	EditorCamera cam;
	EditorCamera::Input in; in.dt = 0.016f;
	cam.update(in);

	const glm::vec3 target(12.0f, 3.0f, -4.0f);
	cam.focusOn(target, 1.0f);

	// The target must be centred in the view after focusing.
	const glm::vec3 targetVS = toViewSpace(cam, target);
	CHECK(std::abs(targetVS.x) < 1e-3f);
	CHECK(std::abs(targetVS.y) < 1e-3f);
	CHECK(targetVS.z < 0.0f);
}

// ── Orthographic views and presets ───────────────────────────────────────────
namespace
{
	// The projection the RenderExtractor builds from an override, so a test
	// can check what lands on screen rather than only the flag.
	glm::mat4 projectionOf(const EditorCameraOverride& o, float aspect)
	{
		if (o.orthographic)   // near a whole far-distance behind the camera, as the extractor does
			return glm::ortho(-aspect * o.orthoHalfHeight, aspect * o.orthoHalfHeight,
			                  -o.orthoHalfHeight, o.orthoHalfHeight, -o.farPlane, o.farPlane);
		return glm::perspective(glm::radians(o.fovDegrees), aspect, o.nearPlane, o.farPlane);
	}

	glm::vec2 toNdc(const EditorCamera& cam, const glm::vec3& worldPos, float aspect = 16.0f / 9.0f)
	{
		const EditorCameraOverride o = cam.makeOverride();
		const glm::vec4 clip = projectionOf(o, aspect) * o.view * glm::vec4(worldPos, 1.0f);
		return glm::vec2(clip) / clip.w;
	}

	bool finite(const glm::mat4& m)
	{
		for (int c = 0; c < 4; ++c)
			for (int r = 0; r < 4; ++r)
				if (!std::isfinite(m[c][r])) return false;
		return true;
	}
}

TEST_CASE("EditorCamera axis presets aim along the axis, orthographic, pivot kept")
{
	EditorCamera cam;
	EditorCamera::Input in; in.dt = 0.016f;
	cam.update(in);   // pivot = the origin

	struct Case { EditorCamera::ViewPreset preset; glm::vec3 forward; };
	const Case cases[] = {
		{ EditorCamera::ViewPreset::Top,    {  0.0f, -1.0f,  0.0f } },
		{ EditorCamera::ViewPreset::Bottom, {  0.0f,  1.0f,  0.0f } },
		{ EditorCamera::ViewPreset::Front,  {  0.0f,  0.0f, -1.0f } },
		{ EditorCamera::ViewPreset::Back,   {  0.0f,  0.0f,  1.0f } },
		{ EditorCamera::ViewPreset::Right,  { -1.0f,  0.0f,  0.0f } },
		{ EditorCamera::ViewPreset::Left,   {  1.0f,  0.0f,  0.0f } },
	};
	for (const Case& c : cases)
	{
		CAPTURE(EditorCamera::presetName(c.preset));
		cam.applyPreset(c.preset);
		CHECK(cam.orthographic());
		CHECK(cam.currentPreset() == c.preset);
		CHECK(finite(cam.viewMatrix()));
		// The camera sits on the opposite side of the pivot, looking back at it.
		const glm::vec3 expectPos = -c.forward * cam.pivotDistance();
		CHECK(glm::length(cam.position() - expectPos) < 1e-3f);
		const glm::vec3 originVS = toViewSpace(cam, glm::vec3(0.0f));
		CHECK(std::abs(originVS.x) < 1e-3f);
		CHECK(std::abs(originVS.y) < 1e-3f);
		CHECK(originVS.z == doctest::Approx(-cam.pivotDistance()));
	}

	// Perspective puts the lens back and keeps the heading.
	cam.applyPreset(EditorCamera::ViewPreset::Left);
	cam.applyPreset(EditorCamera::ViewPreset::Perspective);
	CHECK_FALSE(cam.orthographic());
	CHECK(cam.currentPreset() == EditorCamera::ViewPreset::Perspective);
	CHECK(glm::length(cam.position() - glm::vec3(-cam.pivotDistance(), 0.0f, 0.0f)) < 1e-3f);
}

TEST_CASE("EditorCamera Top view is a map: X right, -Z up; Bottom mirrors it")
{
	EditorCamera cam;
	EditorCamera::Input in; in.dt = 0.016f;
	cam.update(in);
	cam.applyPreset(EditorCamera::ViewPreset::Top);

	const glm::vec2 px = toNdc(cam, glm::vec3(1.0f, 0.0f, 0.0f));
	const glm::vec2 pz = toNdc(cam, glm::vec3(0.0f, 0.0f, -1.0f));
	CHECK(px.x > 0.0f);  CHECK(std::abs(px.y) < 1e-4f);
	CHECK(pz.y > 0.0f);  CHECK(std::abs(pz.x) < 1e-4f);

	cam.applyPreset(EditorCamera::ViewPreset::Bottom);
	const glm::vec2 bx = toNdc(cam, glm::vec3(1.0f, 0.0f, 0.0f));
	const glm::vec2 bz = toNdc(cam, glm::vec3(0.0f, 0.0f, -1.0f));
	CHECK(bx.x < 0.0f);
	CHECK(bz.y > 0.0f);

	// Front: X right, Y up.
	cam.applyPreset(EditorCamera::ViewPreset::Front);
	CHECK(toNdc(cam, glm::vec3(1.0f, 0.0f, 0.0f)).x > 0.0f);
	CHECK(toNdc(cam, glm::vec3(0.0f, 1.0f, 0.0f)).y > 0.0f);
	// Right (on +X looking toward -X): -Z to the right.
	cam.applyPreset(EditorCamera::ViewPreset::Right);
	CHECK(toNdc(cam, glm::vec3(0.0f, 0.0f, -1.0f)).x > 0.0f);
}

TEST_CASE("EditorCamera ortho toggle keeps the pivot plane framed at the same size")
{
	EditorCamera cam;
	EditorCamera::Input in; in.dt = 0.016f;
	cam.update(in);
	// A point on the pivot plane (perpendicular to the view through the pivot),
	// one unit up in view space.
	const glm::mat4 camWorld = glm::inverse(cam.viewMatrix());
	const glm::vec3 pivot    = cam.position() + glm::vec3(-camWorld[2]) * cam.pivotDistance();
	const glm::vec3 p        = pivot + glm::vec3(camWorld[1]) * 1.0f;

	const glm::vec2 persp = toNdc(cam, p);
	cam.setOrthographic(true);
	const glm::vec2 ortho = toNdc(cam, p);
	CHECK(persp.x == doctest::Approx(ortho.x).epsilon(1e-3));
	CHECK(persp.y == doctest::Approx(ortho.y).epsilon(1e-3));

	const EditorCameraOverride o = cam.makeOverride();
	CHECK(o.orthographic);
	CHECK(o.orthoHalfHeight == doctest::Approx(cam.orthoHalfHeight()));
	CHECK(projectionOf(o, 1.5f)[3][3] == 1.0f);
}

TEST_CASE("EditorCamera wheel zooms an ortho view (smaller visible height)")
{
	EditorCamera cam;
	EditorCamera::Input in; in.dt = 0.016f;
	cam.update(in);
	cam.applyPreset(EditorCamera::ViewPreset::Top);
	const float before = cam.orthoHalfHeight();

	EditorCamera::Input wheelIn; wheelIn.dt = 0.016f; wheelIn.wheel = 1.0f;
	cam.update(wheelIn);
	CHECK(cam.orthoHalfHeight() < before);
	CHECK(cam.orthographic());                       // zooming never drops the lens choice
	// Still exactly Top: the wheel dollies along the axis, not off it.
	CHECK(cam.currentPreset() == EditorCamera::ViewPreset::Top);
}

TEST_CASE("EditorCamera orbit out of a preset keeps ortho but is no longer that preset")
{
	EditorCamera cam;
	EditorCamera::Input in; in.dt = 0.016f;
	cam.update(in);
	cam.applyPreset(EditorCamera::ViewPreset::Front);

	EditorCamera::Input orbit; orbit.dt = 0.016f;
	orbit.orbit = true;
	orbit.mouseDelta = glm::vec2(80.0f, 30.0f);
	cam.update(orbit);
	CHECK(cam.orthographic());
	CHECK(cam.currentPreset() == EditorCamera::ViewPreset::Perspective);
	CHECK(finite(cam.viewMatrix()));
	// Orbiting off the Top pole must not leave a NaN behind either.
	cam.applyPreset(EditorCamera::ViewPreset::Top);
	cam.update(orbit);
	CHECK(finite(cam.viewMatrix()));
	CHECK(finite(cam.makeOverride().view));
}
