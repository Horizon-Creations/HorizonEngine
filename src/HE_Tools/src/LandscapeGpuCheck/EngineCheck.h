#pragma once
#include <Types/Enums.h>

#include <glm/glm.hpp>

#include <string>

namespace gpucheck
{
// The REAL renderer (RendererFactory, a hidden window, the backend's own scene pass) draws the ramp world through IRenderer::SetLandscapeProvider
// and RenderSceneImage, and the picture is compared with where the plane is. Returns the number of failed checks. The image is tone-mapped,
// so this checks WHERE the ground is and that it is ground, not exact colours (the offscreen checks do that, on the same shaders).
struct EngineCamera
{
	glm::dvec3 pos;      // absolute
	glm::dvec3 dir;
	glm::dvec3 origin;   // the world's floating origin
};

// `arrayRoot` (optional): an EngineContent folder that holds Textures/Landscape/T_Landscape_Forest_Albedo_Array.hasset. Then the renderer draws the ground in
// world coordinates from that array (texture group Forest) and the check is that the ground is NOT the flat colour any more but has texture in it.
// `farMarker`: the pack carries a far image of a colour no layer has (magenta). A backend that takes the far image (Metal, OpenGL) must then show
// it on the ground beyond the atlas and keep the ground near the camera green; the others must show none of it.
int runEngineCheck(HE::RendererBackend backend, HE::RenderPath path, const std::string& packPath, const std::string& pngPath, const std::string& arrayRoot = {},
                   bool farMarker = false);

// The ground drawn with a material (LandscapeRenderFrame::materialId) by a backend that says it can (Capabilities::landscapeMaterial):
// a marker material's layer 0 is on the ground, a box's shadow darkens it, fog moves it. See the comment at the function.
// `arrayRoot` (optional): an EngineContent folder; then the engine's own Auto Landscape material (Forest) is drawn on the ground as well.
int runMaterialCheck(HE::RendererBackend backend, HE::RenderPath path, const std::string& packPath, const std::string& pngPath, const std::string& arrayRoot = {});

// Frame times through the real renderer: the tile landscape with its fixed blend, with its material, with its material in the scene
// pass alone, and a TerrainComponent landscape with the same material (--timing). Prints a table; a measurement, not a check.
int runMaterialTiming(HE::RendererBackend backend, HE::RenderPath path, const std::string& packPath, const std::string& arrayRoot);

// GL-only: a mirror standing on the tile landscape with GI reflections on, checking that the GI
// pipelines link (the 'GI pipelines built' log line) and that the mirror's pixel differs from the
// same render with the landscape's GI heightfield removed (reflections stay on in both — a plain
// reflections-on/off toggle can pass on the baked sky alone). See
// docs/landscape-reflections-design-2026-11-19.md §7.
int runReflectionCheck(const std::string& packPath, const std::string& pngPath);
} // namespace gpucheck
