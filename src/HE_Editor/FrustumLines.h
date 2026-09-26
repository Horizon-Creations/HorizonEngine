#pragma once

// ─── A camera's frustum as debug lines ───────────────────────────────────────
// The one drawing both kinds of camera in the viewport share: the MCP clients'
// screenshot cameras (McpCameraGizmos) and a selected scene camera
// (ViewportOverlays). Pure geometry into a DebugDrawBuffer, header-only so
// either can use it without the other's translation unit (the tests link them
// separately).
//
// The shape: the four edges from the eye to a rectangle `length` ahead, that
// rectangle twice (nested — the line renderer has no thickness, and a doubled
// edge is what survives a busy background), a smaller one a third of the way
// (what makes it read as a frustum and not as a pyramid of four hairs), and an
// "up" triangle on the far rectangle's top edge so the roll reads: a frustum
// rolled by 180° draws the same without it.

#include <DebugDraw/DebugDraw.h>

#include <glm/glm.hpp>

#include <cmath>

namespace HE::Ed::FrustumLines
{
	// Deterministic, so a frame that draws N cameras draws N × this.
	constexpr int kLinesPerFrustum = 4 + 4 + 4 + 4 + 3;

	// `forward`, `right`, `up` are unit vectors of the camera's frame.
	// `halfW` / `halfH` are the far rectangle's half extents at `length`; the
	// rectangle a third of the way is that one scaled by `nearScale` (1/3 for a
	// perspective frustum, whose sides converge on the eye; 1 for an
	// orthographic box, whose sides stay parallel). The four side edges start
	// at a rectangle of half extents `eyeHalfW` × `eyeHalfH` around the eye:
	// zero for a perspective frustum (they meet in the eye), the box's own
	// extents for an orthographic one.
	inline void appendShape(const glm::vec3& eye, const glm::vec3& forward,
	                        const glm::vec3& right, const glm::vec3& up,
	                        float halfW, float halfH, float length, float nearScale,
	                        float eyeHalfW, float eyeHalfH,
	                        const glm::vec3& color, DebugDrawBuffer& out)
	{
		auto rect = [&](float dist, float sx, float sy) {
			const glm::vec3 c  = eye + forward * dist;
			const glm::vec3 dx = right * sx;
			const glm::vec3 dy = up * sy;
			const glm::vec3 p[4] = { c - dx - dy, c + dx - dy, c + dx + dy, c - dx + dy };
			for (int i = 0; i < 4; ++i) out.line(p[i], p[(i + 1) % 4], color);
		};

		// Edges from the eye (or the eye plane's corners) to the far corners.
		{
			const glm::vec3 c  = eye + forward * length;
			const glm::vec3 dx = right * halfW, dy = up * halfH;
			const glm::vec3 ex = right * eyeHalfW, ey = up * eyeHalfH;
			out.line(eye - ex - ey, c - dx - dy, color);
			out.line(eye + ex - ey, c + dx - dy, color);
			out.line(eye + ex + ey, c + dx + dy, color);
			out.line(eye - ex + ey, c - dx + dy, color);
		}
		rect(length, halfW, halfH);
		rect(length, halfW * 0.92f, halfH * 0.92f);
		rect(length / 3.0f, halfW * nearScale, halfH * nearScale);

		// The "up" triangle over the far rectangle's top edge.
		{
			const glm::vec3 top    = eye + forward * length + up * halfH;
			const glm::vec3 apex   = top + up * (halfH * 0.45f);
			const glm::vec3 left   = top - right * (halfW * 0.35f);
			const glm::vec3 rightP = top + right * (halfW * 0.35f);
			out.line(left, apex, color);
			out.line(apex, rightP, color);
			out.line(rightP, left, color);
		}
	}

	// A perspective frustum: vertical field of view `fovDeg`, width/height
	// `aspect`, drawn `length` world units deep.
	inline void appendPerspective(const glm::vec3& eye, const glm::vec3& forward,
	                              const glm::vec3& right, const glm::vec3& up,
	                              float fovDeg, float aspect, float length,
	                              const glm::vec3& color, DebugDrawBuffer& out)
	{
		const float halfH = length * std::tan(fovDeg * 3.14159265358979f / 360.0f);
		const float halfW = halfH * aspect;
		appendShape(eye, forward, right, up, halfW, halfH, length, 1.0f / 3.0f,
		            0.0f, 0.0f, color, out);
	}

	// An orthographic view volume: a box of half extents `halfW` × `halfH`
	// starting at the eye plane, `length` deep. Same line count as the
	// perspective shape, so callers need not tell the two apart.
	inline void appendOrtho(const glm::vec3& eye, const glm::vec3& forward,
	                        const glm::vec3& right, const glm::vec3& up,
	                        float halfW, float halfH, float length,
	                        const glm::vec3& color, DebugDrawBuffer& out)
	{
		appendShape(eye, forward, right, up, halfW, halfH, length, 1.0f,
		            halfW, halfH, color, out);
	}
}
