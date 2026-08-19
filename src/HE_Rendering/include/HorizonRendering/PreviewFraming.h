#pragma once
#include <Math/Math.h>
#include <Math/AABB.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>

// ─── Preview / thumbnail framing ────────────────────────────────────────────
// The camera every Content-Browser tile and every asset preview is shot with.
//
// Free functions in a header for the same reason as WorldPreviewGrid.h: five
// backends have to frame the SAME asset the same way. A tile that sits a shade
// closer on D3D than on OpenGL is not a bug anybody reports — it is a grid of
// assets that stops reading as one set, and it is only visible when two people
// on two machines compare screenshots.
//
// WHY THE PROJECTION IS BUILT HERE AND NOT AT THE CALL SITE:
// `GLM_FORCE_DEPTH_ZERO_TO_ONE` is a PRIVATE compile definition on the Vulkan,
// D3D11 and D3D12 targets and absent on OpenGL and Metal (see
// src/HE_Rendering/CMakeLists.txt). So `glm::perspective` compiles to two
// different matrices in two different translation units. `meshOrbit` therefore
// calls `glm::perspectiveRH_NO` explicitly — the -1..1 depth form, which
// ignores the force macros — and returns an OpenGL-convention matrix. The
// caller then pre-multiplies exactly ONE ClipSpace.h fix-up:
//
//   OpenGL / Metal … proj                        (Metal: kMetalClipFix)
//   D3D11 / D3D12  … HE::kD3DClipFix    * proj
//   Vulkan         … HE::kVulkanClipFix * proj   (also flips clip-space Y)
//
// Getting that wrong is quiet rather than loud: a double depth remap is still
// monotonic, so a single-object preview keeps looking plausible and only
// misbehaves near the near plane. Hence the one place to get it right.
//
// Header-only, all `inline`/`constexpr`, no export macro — the backend static
// libraries reuse HorizonRendering's include path but do not link the shared
// library (the same reasoning ClipSpace.h:11-13 spells out).

namespace HE
{

// ─── The fixed three-quarter tile shot ──────────────────────────────────────
// A thumbnail has no orbit interaction, so one shared angle is what makes a
// grid of tiles comparable at a glance.
inline constexpr float kThumbYaw   = 0.7f;
inline constexpr float kThumbPitch = 0.45f;

// The three framing distances all target the same ~90 %-of-half-frame fill, so
// mesh and material tiles sit equally in their cells. They differ because the
// paths differ in FOV and in what the distance is measured against:
//   • kMeshFrameDist scales the bounds' half-DIAGONAL, and fitting that into
//     the 35° FOV takes ≥ extent / tan(17.5°) ≈ 3.17 · extent.
//   • the two sphere distances are absolute (unit radius). A sphere's
//     silhouette fills tan(asin(r/D)) / tan(fov/2) of the half-frame, NOT r/D —
//     the naive form is what once left the material tiles cropped.
inline constexpr float kMeshFrameDist   = 3.6f;  // × extent, 35° FOV
inline constexpr float kMatGraphDist    = 4.0f;  // unit sphere, 32° FOV → 90.0 %
inline constexpr float kMatFallbackDist = 3.66f; // unit sphere, 35° FOV → 90.1 %
inline constexpr float kParticleDist    = 2.9f;  // particle cloud, 35° FOV

// What an untextured mesh tile is shaded as, so a mesh with no material still
// produces a tile that reads as geometry rather than as a silhouette.
inline constexpr float kMeshTileBaseColor = 0.78f;
inline constexpr float kMeshTileMetallic  = 0.0f;
inline constexpr float kMeshTileRoughness = 0.55f;

// The preview FOV. Kept next to the distances above because the two are only
// meaningful together — change one and the fill fraction moves.
inline constexpr float kPreviewFovDegrees = 35.0f;

// ─── Bounds → framing ───────────────────────────────────────────────────────
// Meshes vary wildly in size AND in where their pivot sits, so a preview frames
// on the bounds rather than on the origin. An invalid AABB (an asset that never
// reported bounds) falls back to a unit box at the origin instead of collapsing
// the camera onto a zero-extent point.

inline glm::vec3 boundsCenter(const HE::AABB& b)
{
	return b.isValid() ? (b.min + b.max) * 0.5f : glm::vec3(0.0f);
}

// HALF-DIAGONAL, not half-extent: fitting the diagonal is what guarantees the
// mesh stays inside the tile at every yaw, not just the one it was tuned at.
inline float boundsExtent(const HE::AABB& b)
{
	return b.isValid() ? glm::length(b.max - b.min) * 0.5f : 1.0f;
}

// ─── The orbit camera ───────────────────────────────────────────────────────
struct PreviewCamera
{
	glm::vec3 position{0.0f};
	glm::mat4 view{1.0f};
	glm::mat4 proj{1.0f}; // OpenGL convention (depth −1..1) — see the header note
};

// `dist` SCALES the extent rather than being an absolute distance, so one
// constant frames a 2 cm bolt and a 200 m terrain chunk alike.
inline PreviewCamera meshOrbit(const glm::vec3& center, float extent,
                               float yaw, float pitch, float dist,
                               float aspect = 1.0f,
                               float fovDegrees = kPreviewFovDegrees)
{
	PreviewCamera c;
	const float camDist = std::max(0.05f, dist) * std::max(extent, 0.05f);
	const float cp = std::cos(pitch), sp = std::sin(pitch);
	c.position = center + glm::vec3(std::sin(yaw) * cp, sp, std::cos(yaw) * cp) * camDist;
	c.view     = glm::lookAt(c.position, center, glm::vec3(0.0f, 1.0f, 0.0f));
	// perspectiveRH_NO, never glm::perspective — see the header note.
	c.proj     = glm::perspectiveRH_NO(glm::radians(fovDegrees), aspect,
	                                   0.01f, camDist * 20.0f + 10.0f);
	return c;
}

// ─── Thumbnail size contract ────────────────────────────────────────────────
// IRenderer::RenderAssetThumbnail takes a uint32_t the caller may have computed
// from a UI scale factor. Every backend clamps it the same way, so the clamp
// lives here rather than in five copies that could drift apart.
inline int clampThumbnailSize(uint32_t size)
{
	return std::clamp(static_cast<int>(size), 16, 512);
}

inline int clampPreviewSize(uint32_t size)
{
	return std::clamp(static_cast<int>(size), 32, 1024);
}

// Die Skelett-Vorschau hat bewusst eine höhere Obergrenze als die anderen beiden:
// sie füllt einen ganzen Editor-Tab, nicht ein kleines Feld neben Parametern, und
// ein maximiertes Fenster ist auf heutigen Bildschirmen regelmäßig breiter als
// 1024. OpenGL klemmt genau diesen einen Einstiegspunkt auf 32..2048
// (OpenGLRenderer.cpp:8588) — hier steht dieselbe Zahl, damit die vier Backends
// nicht wieder auseinanderlaufen.
inline int clampSkeletalPreviewSize(uint32_t size)
{
	return std::clamp(static_cast<int>(size), 32, 2048);
}

} // namespace HE
