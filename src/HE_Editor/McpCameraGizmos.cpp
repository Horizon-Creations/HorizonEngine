#include "McpCameraGizmos.h"

#include "CollabPresenceBar.h"   // PlaceMarker: on-screen / pinned-to-the-border placement

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace HE::Ed::McpCameraGizmos
{

glm::vec3 colorFor(McpClientId id)
{
	// Golden-ratio hue stepping, the collaboration fallback's recipe
	// (CollabController::participantColor) shifted by a third of the wheel:
	// client #1 and participant #1 must not look alike. Full saturation, high
	// value — these are overlay lines, not surfaces.
	const float hue = std::fmod(static_cast<float>(id) * 0.618033988f + 0.3333f, 1.0f);
	const float h6  = hue * 6.0f;
	const int   sector = static_cast<int>(h6) % 6;
	const float f   = h6 - std::floor(h6);
	const float q   = 1.0f - f;
	switch (sector)
	{
		case 0:  return { 1.0f, f,    0.0f };
		case 1:  return { q,    1.0f, 0.0f };
		case 2:  return { 0.0f, 1.0f, f    };
		case 3:  return { 0.0f, q,    1.0f };
		case 4:  return { f,    0.0f, 1.0f };
		default: return { 1.0f, 0.0f, q    };
	}
}

std::string labelFor(McpClientId id)
{
	char buf[32];
	std::snprintf(buf, sizeof(buf), "MCP #%u", static_cast<unsigned>(id));
	return buf;
}

void appendFrustum(const McpClientCamera& cam, const glm::vec3& color,
                   float length, DebugDrawBuffer& out)
{
	const glm::vec3 eye = cam.position;
	const glm::vec3 f   = cam.forward();
	const glm::vec3 r   = cam.right();
	const glm::vec3 u   = cam.up();

	// The far rectangle's half-extents follow the camera's real field of view
	// (vertical, like the tool's `fov`) and the drawn aspect — that is the
	// whole point of drawing a frustum rather than a box.
	const float halfH = length * std::tan(cam.fovDeg * McpClientCamera::kPi / 360.0f);
	const float halfW = halfH * kAspect;

	auto rect = [&](float dist, float scale) {
		const glm::vec3 c  = eye + f * dist;
		const glm::vec3 dx = r * (halfW * (dist / length) * scale);
		const glm::vec3 dy = u * (halfH * (dist / length) * scale);
		const glm::vec3 p[4] = { c - dx - dy, c + dx - dy, c + dx + dy, c - dx + dy };
		for (int i = 0; i < 4; ++i) out.line(p[i], p[(i + 1) % 4], color);
	};

	// Edges from the eye to the far corners.
	{
		const glm::vec3 c  = eye + f * length;
		const glm::vec3 dx = r * halfW, dy = u * halfH;
		out.line(eye, c - dx - dy, color);
		out.line(eye, c + dx - dy, color);
		out.line(eye, c + dx + dy, color);
		out.line(eye, c - dx + dy, color);
	}
	// The far rectangle, twice — the line renderer has no thickness, so a
	// second one a hair inside is how a stroke is made wide enough to survive
	// a busy background (the collaboration rings are nested for the same
	// reason). Then a near rectangle a third of the way, which is what makes
	// the shape read as a frustum and not as a pyramid of four hairs.
	rect(length, 1.0f);
	rect(length, 0.92f);
	rect(length / 3.0f, 1.0f);

	// The "up" triangle over the far rectangle's top edge: which way is up in
	// the picture. Without it a frustum rolled by 180° draws exactly the same.
	{
		const glm::vec3 top   = eye + f * length + u * halfH;
		const glm::vec3 apex  = top + u * (halfH * 0.45f);
		const glm::vec3 left  = top - r * (halfW * 0.35f);
		const glm::vec3 right = top + r * (halfW * 0.35f);
		out.line(left, apex, color);
		out.line(apex, right, color);
		out.line(right, left, color);
	}
}

void appendFrustums(const McpClientCameras& cameras, const glm::vec3& viewer,
                    DebugDrawBuffer& out)
{
	for (const auto& [id, cam] : cameras.all())
	{
		// Screen-constant size, the collaboration marker's rule: a fixed world
		// length is a speck at 80 m and a wall at 1 m, so it is scaled with the
		// distance to the local camera and clamped at both ends. The factor is
		// chosen so a 60° frustum's far edge is about the height of a peer's
		// ring.
		const float dist = glm::length(viewer - cam.position);
		if (dist < 0.001f) continue;   // standing inside their camera
		const float length = std::clamp(dist * 0.08f, 0.15f, 6.0f);
		appendFrustum(cam, colorFor(id), length, out);
	}
}

void drawViewportLabels(const McpClientCameras& cameras,
                        const glm::mat4& view, const glm::mat4& proj,
                        float rectMinX, float rectMinY, float rectMaxX, float rectMaxY)
{
#ifdef HE_IMGUI_ENABLED
	if (cameras.size() == 0) return;

	const float width  = rectMaxX - rectMinX;
	const float height = rectMaxY - rectMinY;
	if (width < 32.0f || height < 32.0f) return;

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->PushClipRect(ImVec2(rectMinX, rectMinY), ImVec2(rectMaxX, rectMaxY), true);

	constexpr float kDot   = 10.0f;   // marker diameter — smaller than a peer's face: no picture to show
	constexpr float kInset = 22.0f;   // how far inside the border a pinned marker sits

	for (const auto& [id, cam] : cameras.all())
	{
		const CollabPresenceBar::MarkerPlacement place = CollabPresenceBar::PlaceMarker(
			view, proj, cam.position, rectMinX, rectMinY, rectMaxX, rectMaxY, kInset);

		const float sx = place.x, sy = place.y;
		const bool  onScreen = place.onScreen;

		const glm::vec3 rgb = colorFor(id);
		const ImU32 col = ImGui::GetColorU32(ImVec4(rgb.r, rgb.g, rgb.b, 1.0f));

		const float  radius = kDot * 0.5f;
		const ImVec2 centre(sx, sy);

		// A dark halo under everything, a filled dot in the client's colour: the
		// viewport can be any colour at all, and the dot has to be legible on
		// all of them.
		dl->AddCircleFilled(centre, radius + 3.0f, IM_COL32(0, 0, 0, 110), 20);
		dl->AddCircleFilled(centre, radius, col, 20);

		if (!onScreen)
		{
			// An outward arrowhead just past the dot: which way to turn.
			const float ax = std::cos(place.arrowAngle), ay = std::sin(place.arrowAngle);
			const float px = -ay, py = ax;
			const ImVec2 tip(sx + ax * (radius + 12.0f), sy + ay * (radius + 12.0f));
			const ImVec2 b0(sx + ax * (radius + 3.0f) + px * 6.0f,
			                sy + ay * (radius + 3.0f) + py * 6.0f);
			const ImVec2 b1(sx + ax * (radius + 3.0f) - px * 6.0f,
			                sy + ay * (radius + 3.0f) - py * 6.0f);
			dl->AddTriangleFilled(tip, b0, b1, col);
		}

		// The tag, below an on-screen marker and above a pinned one — a pinned
		// marker sits at the border, where "below" would be off the image.
		// Same dark pill with a coloured edge as a peer's name tag, so the two
		// read as the same kind of thing.
		const std::string label = labelFor(id);
		const ImVec2 ts   = ImGui::CalcTextSize(label.c_str());
		const float  padX = 6.0f, padY = 2.0f;
		const float  labelY = onScreen ? sy + radius + 8.0f
		                               : sy - radius - 8.0f - (ts.y + padY * 2.0f);
		const ImVec2 lmin(sx - ts.x * 0.5f - padX, labelY);
		const ImVec2 lmax(sx + ts.x * 0.5f + padX, labelY + ts.y + padY * 2.0f);
		dl->AddRectFilled(lmin, lmax, IM_COL32(18, 18, 22, 220), 4.0f);
		dl->AddRect(lmin, lmax, col, 4.0f, 0, 1.5f);
		dl->AddText(ImVec2(lmin.x + padX, lmin.y + padY), IM_COL32(240, 240, 245, 255),
		            label.c_str());
	}

	dl->PopClipRect();
#else
	(void)cameras; (void)view; (void)proj;
	(void)rectMinX; (void)rectMinY; (void)rectMaxX; (void)rectMaxY;
#endif
}

} // namespace HE::Ed::McpCameraGizmos
