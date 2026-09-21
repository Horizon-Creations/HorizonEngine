#include "doctest.h"

#include "../src/HE_Editor/McpCameraGizmos.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

// ─── The MCP clients' cameras, as lines ──────────────────────────────────────
// The frustum half of the gizmo is geometry into a DebugDrawBuffer, so the
// questions are geometric: does it open the way the camera looks (every
// point but the eye lies ahead of the eye, and none behind it), does it turn
// with yaw and pitch the way McpClientCamera says those turn, does its opening
// follow the field of view, is the up-triangle actually up, and is the line
// count what the header promises so a frame with N cameras is N × that and
// the range EditorApplication remembers for the screenshot strip is right.
// What the lines look like on a real frame is not this binary's question.

using HE::Ed::McpClientCamera;
using HE::Ed::McpClientCameras;
namespace G = HE::Ed::McpCameraGizmos;

namespace {

// Every endpoint of every line, minus the eye — the eye is on four lines and
// says nothing about direction.
std::vector<glm::vec3> pointsAhead(const DebugDrawBuffer& buf, const glm::vec3& eye)
{
	std::vector<glm::vec3> pts;
	for (const DebugLine& l : buf.lines())
		for (const glm::vec3& p : { l.start, l.end })
			if (glm::length(p - eye) > 1e-5f) pts.push_back(p);
	return pts;
}

McpClientCamera at(glm::vec3 pos, float yaw, float pitch, float fov = 60.0f)
{
	McpClientCamera c;
	c.position = pos;
	c.yawDeg   = yaw;
	c.pitchDeg = pitch;
	c.fovDeg   = fov;
	return c;
}

} // namespace

TEST_CASE("McpCameraGizmos: the frustum opens the way the camera looks")
{
	// yaw 0 / pitch 0 looks down -Z (McpClientCamera, EditorCamera): every
	// point of the gizmo is ahead of the eye along -Z, none behind it.
	SUBCASE("default heading: -Z")
	{
		DebugDrawBuffer buf;
		const glm::vec3 eye(1.0f, 2.0f, 3.0f);
		G::appendFrustum(at(eye, 0.0f, 0.0f), glm::vec3(1.0f), 2.0f, buf);
		CHECK(static_cast<int>(buf.lines().size()) == G::kLinesPerFrustum);
		const auto pts = pointsAhead(buf, eye);
		REQUIRE(!pts.empty());
		for (const glm::vec3& p : pts) CHECK(p.z < eye.z);
		// The far rectangle is exactly `length` ahead; nothing is further.
		float maxAhead = 0.0f;
		for (const glm::vec3& p : pts) maxAhead = std::max(maxAhead, eye.z - p.z);
		CHECK(maxAhead == doctest::Approx(2.0f).epsilon(0.001));
	}
	// +yaw turns right: yaw 90 looks down +X.
	SUBCASE("yaw 90: +X")
	{
		DebugDrawBuffer buf;
		const glm::vec3 eye(0.0f);
		G::appendFrustum(at(eye, 90.0f, 0.0f), glm::vec3(1.0f), 2.0f, buf);
		for (const glm::vec3& p : pointsAhead(buf, eye)) CHECK(p.x > 0.0f);
	}
	// +pitch looks up; -90 is straight down.
	SUBCASE("pitch -90: -Y")
	{
		DebugDrawBuffer buf;
		const glm::vec3 eye(0.0f, 10.0f, 0.0f);
		G::appendFrustum(at(eye, 0.0f, -90.0f), glm::vec3(1.0f), 2.0f, buf);
		for (const glm::vec3& p : pointsAhead(buf, eye)) CHECK(p.y < eye.y);
	}
}

TEST_CASE("McpCameraGizmos: the opening follows the field of view and the up-triangle is up")
{
	const glm::vec3 eye(0.0f);
	auto extentY = [&](float fov) {
		DebugDrawBuffer buf;
		G::appendFrustum(at(eye, 0.0f, 0.0f, fov), glm::vec3(1.0f), 2.0f, buf);
		float maxY = -1e9f, minY = 1e9f;
		for (const DebugLine& l : buf.lines())
			for (const glm::vec3& p : { l.start, l.end })
			{
				maxY = std::max(maxY, p.y);
				minY = std::min(minY, p.y);
			}
		return std::make_pair(minY, maxY);
	};
	const auto [lo30, hi30] = extentY(30.0f);
	const auto [lo90, hi90] = extentY(90.0f);
	// A wider field of view is a wider frustum, top and bottom alike.
	CHECK(hi90 > hi30);
	CHECK(lo90 < lo30);
	// The far rectangle's bottom edge is at -length·tan(fov/2); the top of the
	// gizmo is higher than that by the up-triangle, so the shape is not
	// symmetric about the axis — which is what makes the roll readable.
	const float halfH30 = 2.0f * std::tan(glm::radians(15.0f));
	CHECK(lo30 == doctest::Approx(-halfH30).epsilon(0.001));
	CHECK(hi30 > halfH30 + 1e-4f);
}

TEST_CASE("McpCameraGizmos: every client in the table, each in its own colour")
{
	McpClientCameras table;
	table.set(3, at({ 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f));
	table.set(7, at({ 10.0f, 0.0f, 0.0f }, 0.0f, 0.0f));
	table.set(9, at({ 0.0f, 5.0f, 0.0f }, 0.0f, 0.0f));

	DebugDrawBuffer buf;
	G::appendFrustums(table, glm::vec3(0.0f, 0.0f, 20.0f), buf);
	CHECK(static_cast<int>(buf.lines().size()) == 3 * G::kLinesPerFrustum);

	// Three cameras, three colours — and the colour of each is colorFor(id),
	// so the tag over the frame and the frustum in it agree.
	std::set<std::tuple<float, float, float>> colours;
	for (const DebugLine& l : buf.lines())
		colours.insert({ l.color.r, l.color.g, l.color.b });
	CHECK(colours.size() == 3);
	const glm::vec3 c3 = G::colorFor(3);
	CHECK(colours.count({ c3.r, c3.g, c3.b }) == 1);

	// A camera the viewer stands inside draws nothing (nothing to draw around
	// yourself); the others still do.
	DebugDrawBuffer inside;
	G::appendFrustums(table, glm::vec3(10.0f, 0.0f, 0.0f), inside);
	CHECK(static_cast<int>(inside.lines().size()) == 2 * G::kLinesPerFrustum);

	// Empty table, empty buffer — the range EditorApplication remembers for the
	// screenshot strip is then empty too.
	DebugDrawBuffer none;
	G::appendFrustums(McpClientCameras{}, glm::vec3(0.0f), none);
	CHECK(none.lines().empty());
}

TEST_CASE("McpCameraGizmos: the size on screen is held, not the size in the world")
{
	// The same camera seen from twice as far is drawn twice as long, up to the
	// clamp — so it reads the same size from anywhere, like the peers' rings.
	McpClientCameras table;
	table.set(1, at(glm::vec3(0.0f), 0.0f, 0.0f));
	auto lengthFrom = [&](float viewerZ) {
		DebugDrawBuffer buf;
		G::appendFrustums(table, glm::vec3(0.0f, 0.0f, viewerZ), buf);
		float maxAhead = 0.0f;
		for (const DebugLine& l : buf.lines())
			maxAhead = std::max({ maxAhead, -l.start.z, -l.end.z });
		return maxAhead;
	};
	CHECK(lengthFrom(20.0f) == doctest::Approx(2.0f * lengthFrom(10.0f)).epsilon(0.001));
	CHECK(lengthFrom(1000.0f) == doctest::Approx(6.0f).epsilon(0.001));   // the far clamp
	CHECK(lengthFrom(0.5f)    == doctest::Approx(0.15f).epsilon(0.001));  // the near clamp
}

TEST_CASE("McpCameraGizmos: the tag names the connection")
{
	CHECK(G::labelFor(1) == "MCP #1");
	CHECK(G::labelFor(42) == "MCP #42");
	// Neighbouring ids land far apart on the wheel, and a client does not
	// wear the colour of the collaboration peer with the same number (whose
	// hue is id·φ without the offset) — checked through the offset itself:
	// no two of the first twenty clients share a colour.
	std::set<std::tuple<float, float, float>> colours;
	for (unsigned id = 1; id <= 20; ++id)
	{
		const glm::vec3 c = G::colorFor(id);
		colours.insert({ c.r, c.g, c.b });
	}
	CHECK(colours.size() == 20);
}
