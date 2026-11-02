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
int runEngineCheck(HE::RendererBackend backend, HE::RenderPath path, const std::string& packPath, const std::string& pngPath, const std::string& arrayRoot = {});
} // namespace gpucheck
