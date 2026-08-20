#pragma once
#include <Math/AABB.h>
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

// ─── PreviewPick ─────────────────────────────────────────────────────────────
// "Which of these boxes did the user click?" for the editor's offscreen 3D
// panes — the HorizonCode class Viewport today, any other RenderWorldPreview
// pane tomorrow.
//
// The Scene window answers the same question against a RenderWorld snapshot
// (HE::ScenePick). A preview pane has no snapshot of its own: the renderer
// extracts the world INSIDE RenderWorldPreview and hands back only a picture
// and the matrix it was drawn with. So the panel builds the candidate list
// itself and this is the geometry underneath it.
//
// Deliberately free of ImGui, entt and ContentManager: it is pure projection +
// slab math, which is the part that is worth asserting rather than trusting.
// A viewport cannot be screenshotted headless, but "a click at these pixels
// means that box" can be checked to the pixel.
namespace PreviewPick
{

// One pickable thing: an opaque caller id, its local→world transform, and its
// bounds in LOCAL space (so a rotated object is tested exactly, not through a
// loose world-space box).
struct Candidate
{
	std::uint32_t id = 0;
	glm::mat4     world{ 1.0f };
	HE::AABB      box;
};

// Screen point → world-space ray, through the view-projection the pane was
// drawn with. `rectMin` is the image's top-left in the same coordinates as
// `mouse` (ImGui screen space, y down); `rectSize` its size in those units.
// Returns false for a degenerate rect or a non-invertible matrix.
bool screenRay(const glm::mat4& viewProj, const glm::vec2& rectMin, const glm::vec2& rectSize,
               const glm::vec2& mouse, glm::vec3& outOrigin, glm::vec3& outDir);

// Nearest candidate along the ray. `outT` receives the entry distance in units
// of |dir|. Boxes behind the ray origin never hit: intersectRay clamps the
// entry to t >= 0, so something the camera has already passed cannot be picked
// by pointing away from it.
bool pick(const std::vector<Candidate>& candidates,
          const glm::vec3& rayOrigin, const glm::vec3& rayDir,
          std::uint32_t& outId, float* outT = nullptr);

// The two above in one call — what a panel actually wants.
bool pickAtScreen(const std::vector<Candidate>& candidates, const glm::mat4& viewProj,
                  const glm::vec2& rectMin, const glm::vec2& rectSize, const glm::vec2& mouse,
                  std::uint32_t& outId);

} // namespace PreviewPick
