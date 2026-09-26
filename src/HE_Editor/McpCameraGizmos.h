#pragma once

// ─── The MCP clients' cameras, drawn in the viewport ─────────────────────────
// Every connected MCP client that has described a screenshot camera keeps one
// in McpClientCameras (EditorApplication::m_mcpCameras). This is what makes
// them visible to the human at the editor: a frustum with the client's number,
// so "which way is Claude looking right now" is answered by glancing at the
// scene rather than by reading a log.
//
// Split in two the way the collaboration presence marker is split, and for
// the same reason (CollabPresenceBar.h):
//
//   • appendFrustums — the DEPTH-AWARE half. Lines into the editor's per-frame
//     debug buffer, occluded by the scene like any geometry, so a camera behind
//     a wall reads as behind that wall. Pure geometry, no ImGui; the tests
//     build it.
//   • drawViewportLabels — the LEGIBLE half. A tag with the client's number
//     drawn over the rendered frame, pinned to the border with an arrow when
//     the camera is outside the view, so a camera is never simply invisible.
//     ImGui, hence compiled away without HE_IMGUI_ENABLED.
//
// Collaboration peers deliberately gave up their wire frustum for rings (the
// comment in EditorApplication's presence block says why: eight hairlines seen
// edge-on vanish against a detailed scene). The client cameras keep a frustum
// anyway, because the thing worth reading off them is not "where" but "what
// will the picture contain" — and a frustum's opening angle IS the field of
// view the next screenshot renders with. Its far rectangle is doubled so it
// survives a busy background, and the whole shape is scaled with the distance
// to the viewer, like the rings, so it is neither a speck at 80 m nor a wall
// at 1 m.
//
// Both halves are gated on the same viewport show flag as the collaborators'
// markers (ShowFlags::collaborators): a remote camera is a remote camera,
// whether a person or a model is behind it, and a second switch for the
// difference would be a switch nobody understands.

#include "FrustumLines.h"
#include "McpClientCameras.h"

#include <DebugDraw/DebugDraw.h>

#include <glm/glm.hpp>

#include <string>

namespace HE::Ed::McpCameraGizmos
{
	// The aspect the frustum is drawn with. McpClientCamera does not store one
	// (the tool takes width and height per call), so the gizmo shows the
	// tool's default size — kScreenshotDefaultWidth × kScreenshotDefaultHeight,
	// 16:9 — and a client asking for another shape sees a picture that is a
	// little wider or narrower than the gizmo.
	constexpr float kAspect = 16.0f / 9.0f;

	// The colour a client is drawn in, frustum and tag alike. Derived from the
	// id the way collaboration peers' fallback colour is (golden-ratio hue
	// stepping), but from a different starting point: connection numbers and
	// participant ids both count from one, and a client and a peer with the
	// same small number must not wear the same colour.
	glm::vec3 colorFor(McpClientId id);

	// The tag text: "MCP #<id>". The connection number is the only identity the
	// bridge has for a client (McpBridge names no client; the shim's
	// `params.client` is one string for all of them), and it is the number the
	// tool's own answers carry, so a human can match tag and log.
	std::string labelFor(McpClientId id);

	// One camera's frustum into `out`: the four edges from the eye to a
	// rectangle `length` ahead, that rectangle (twice, nested), a smaller one a
	// third of the way, and an "up" triangle on the far rectangle's top edge so
	// the roll reads — FrustumLines' shape, shared with the selected scene
	// camera. `length` is in world units; callers scale it with the distance to
	// the viewer (see appendFrustums). Deterministic line count —
	// kLinesPerFrustum — so a frame that draws N cameras draws N × that.
	constexpr int kLinesPerFrustum = FrustumLines::kLinesPerFrustum;
	void appendFrustum(const McpClientCamera& cam, const glm::vec3& color,
	                   float length, DebugDrawBuffer& out);

	// Every camera in the table, in id order, each scaled to read the same size
	// on screen from where `viewer` (the editor camera) stands. A camera the
	// viewer is standing inside is skipped: there is nothing to draw around
	// yourself, and the collaboration rings make the same call.
	void appendFrustums(const McpClientCameras& cameras, const glm::vec3& viewer,
	                    DebugDrawBuffer& out);

	// The tags, over the rendered viewport image. Call from inside the viewport
	// window after the ImGui::Image, with THIS frame's camera matrices and the
	// rectangle the image occupies on screen — the same contract as
	// CollabPresenceBar::DrawViewportMarkers, whose placement it shares.
	// Draws nothing without HE_IMGUI_ENABLED.
	void drawViewportLabels(const McpClientCameras& cameras,
	                        const glm::mat4& view, const glm::mat4& proj,
	                        float rectMinX, float rectMinY,
	                        float rectMaxX, float rectMaxY);
}
