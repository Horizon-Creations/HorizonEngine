#pragma once
#include <HorizonRendering/ClipSpace.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderWorld.h>
#include <Renderer/IRenderer.h>
#include <cmath>

class ContentManager;
class HorizonWorld;

// ─── World-preview frame setup ──────────────────────────────────────────────
// Everything IRenderer::RenderWorldPreview decides before its first draw —
// camera, snapshot, sky settings, the light the preview shaders take — for the
// D3D11, D3D12 and Vulkan backends, which would otherwise carry three copies of
// the same sixty lines. The steps are GL's and Metal's (OpenGLRenderer.cpp,
// RenderWorldPreview), which predate this header and keep their own copy.
//
// Header-inline for the same reason as ClipSpace.h: the backend static
// libraries reuse this include path but are compiled with different glm flags.

namespace HE
{

struct WorldPreviewFrame
{
	RenderWorld                    snapshot;
	IRenderer::EnvironmentSettings sky{};           // the preview sky (only set with env.sky)
	glm::mat4                      viewProj{1.0f};  // GL clip, depth -1..1 — the caller's space
	glm::vec3                      camPos{0.0f};
	glm::vec4                      sun{0.0f};       // xyz toward the light, w > 0 = armed
	glm::vec3                      sunColor{1.0f};
	glm::vec3                      ambient{0.0f};
};

inline void buildWorldPreviewFrame(ContentManager* cm, HorizonWorld& world,
                                   const EditorCameraOverride& camera, const WorldPreviewEnv& env,
                                   float aspect, WorldPreviewFrame& out)
{
	// The caller's pose; the projection is the shared rule's, and it reaches
	// the extractor through the override — the Hor+ narrowing goes into the fov
	// BEFORE the extract, so the extractor culls with exactly the frustum that
	// is drawn (for an orthographic camera its matrix already is the rule's).
	EditorCameraOverride previewCam = camera;
	previewCam.active = true;
	if (!camera.orthographic)
		previewCam.fovDegrees = worldPreviewVerticalFov(camera.fovDegrees, aspect);

	// Its OWN extractor: the renderer's carries the scene's day-night state,
	// and a preview inheriting the scene's sunset tint would be a surprise. The
	// sky is built the way the editor and the packaged game build theirs
	// (makeWorldPreviewEnvironment), and setDayNight puts the sun where the
	// scene would have it at this hour.
	RenderExtractor extractor;
	extractor.setContentManager(cm);
	out.sky = IRenderer::EnvironmentSettings{};
	if (env.sky)
	{
		out.sky = makeWorldPreviewEnvironment(env.timeOfDay, env.cloudCoverage);
		extractor.setDayNight(true, env.timeOfDay,
		                      out.sky.sunColor, out.sky.sunIntensity,
		                      out.sky.moonColor, out.sky.moonIntensity,
		                      out.sky.cloudCoverage);
	}
	out.snapshot = RenderWorld{};
	extractor.extract(world, out.snapshot, aspect, &previewCam);

	// The projection comes out of the snapshot, brought to GL depth explicitly:
	// the three backends compile with GLM_FORCE_DEPTH_ZERO_TO_ONE and the
	// extractor does not, so neither convention may be assumed. Each backend
	// applies its own clip fix to this exactly once.
	const float nearViewZ = camera.orthographic ? camera.farPlane : -camera.nearPlane;
	out.viewProj = toNegOneToOneDepth(out.snapshot.camera.projection, nearViewZ)
	             * out.snapshot.camera.view;
	out.camPos   = camera.position;

	// Lighting from the extracted sun: dominantDirectionalLight, NOT
	// sunDirection — the sky-dome sun sits below the horizon at night and would
	// light the mesh from underneath. Without a sky: w == 0, the studio light.
	out.sun      = glm::vec4(0.0f);
	out.sunColor = glm::vec3(1.0f);
	out.ambient  = glm::vec3(0.0f);
	if (env.sky)
	{
		glm::vec3 toward(0.0f, 1.0f, 0.0f), colorIntensity(0.0f);
		if (out.snapshot.dominantDirectionalLight(toward, colorIntensity))
		{
			out.sun      = glm::vec4(toward, 1.0f);
			out.sunColor = colorIntensity;
		}
		else
		{
			// Night with nothing shining: armed anyway, so the ambient floor
			// lights the mesh instead of the studio light snapping back on.
			out.sun      = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
			out.sunColor = glm::vec3(0.0f);
		}
		// A floor under the extractor's ambient: a viewer that goes black at
		// midnight is a viewer nobody can use at midnight.
		out.ambient = glm::max(out.snapshot.ambient, glm::vec3(0.10f, 0.11f, 0.13f));
	}
}

// The grid's half extent: follows the camera's distance from the origin so it
// never runs out, capped because the line count grows with it.
inline float worldPreviewGridExtent(const glm::vec3& camPos, const glm::vec3& origin)
{
	const float e = std::ceil(glm::length(camPos - origin) * 2.0f);
	return e < 10.0f ? 10.0f : (e > 200.0f ? 200.0f : e);
}

} // namespace HE
