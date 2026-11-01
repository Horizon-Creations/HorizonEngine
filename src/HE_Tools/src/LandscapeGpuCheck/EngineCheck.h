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

int runEngineCheck(HE::RendererBackend backend, HE::RenderPath path, const std::string& packPath, const std::string& pngPath);
} // namespace gpucheck
