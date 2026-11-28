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

// The painted, non-square world of the material check (main.cpp's PaintedSource writes it, EngineCheck.cpp looks for it): 16 x 8
// tiles of the ramp, layer 1 in one square of ground, layer 5 (second weight page) in another, layer 0 elsewhere.
inline constexpr int kPaintTilesX = 16, kPaintTilesZ = 8;
inline constexpr double kPaintA[4] = { 2944.0, 3040.0, 3008.0, 3104.0 };   // x0, z0, x1, z1 of layer 1
inline constexpr double kPaintB[4] = { 3040.0, 3040.0, 3104.0, 3104.0 };   // ... of layer 5
inline int paintLayerAt(double x, double z)
{
	if (x >= kPaintA[0] && x < kPaintA[2] && z >= kPaintA[1] && z < kPaintA[3]) return 1;
	if (x >= kPaintB[0] && x < kPaintB[2] && z >= kPaintB[1] && z < kPaintB[3]) return 5;
	return 0;
}

// The ground drawn with a material (LandscapeRenderFrame::materialId) by a backend that says it can (Capabilities::landscapeMaterial):
// a marker material's layer 0 is on the ground, a box's shadow darkens it, fog moves it. See the comment at the function.
// `arrayRoot` (optional): an EngineContent folder; then the engine's own Auto Landscape material (Forest) is drawn on the ground as well.
// `paintedPack` (optional): the pack of PaintedSource; then the painted squares are looked for at their pixels.
int runMaterialCheck(HE::RendererBackend backend, HE::RenderPath path, const std::string& packPath, const std::string& pngPath, const std::string& arrayRoot = {},
                     const std::string& paintedPack = {});

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
