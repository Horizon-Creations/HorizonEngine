#include "doctest.h"
#include "EditorCamera.h"
#include <glm/glm.hpp>
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
