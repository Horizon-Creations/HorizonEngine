#pragma once
#include <Renderer/LandscapeRenderFrame.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <vector>

// ─── The tile landscape's ground under a Runtime Virtual Texture: which patches read pages ───────
// IRenderer::SetRvtTileGround names a consumer material and the absolute XZ rectangle its pages cover. A patch that lies
// WHOLLY inside that rectangle is drawn with the consumer (it reads the pool), every other one with the ground's own
// material; and with a native near distance the patches within reach of the camera keep the material itself too (up close
// it is sharper than any page). One function for the backends that draw the ground through a material's landscape variant
// (D3D11, D3D12, Vulkan, OpenGL); MetalRenderer::TakeRvtTilePatches is the reference and does the same on its own members.
namespace HE
{

// Moves the patches the consumer serves out of `frame.patches` into `inside` (both keep their order).
inline void splitRvtTilePatches(LandscapeRenderFrame& frame, const glm::dvec2& minXZ, const glm::dvec2& maxXZ, float nativeNearM,
                                std::vector<LandscapeRenderPatch>& inside)
{
	const glm::dvec2 o(frame.uvOrigin.x, frame.uvOrigin.z);   // patch corners are relative to the floating origin
	const auto mid = std::stable_partition(frame.patches.begin(), frame.patches.end(), [&](const LandscapeRenderPatch& p) {
		if (nativeNearM > 0.0f)
		{
			const float dx = std::max({ p.x0 - frame.cameraRel.x, 0.0f, frame.cameraRel.x - (p.x0 + p.size) });
			const float dz = std::max({ p.z0 - frame.cameraRel.z, 0.0f, frame.cameraRel.z - (p.z0 + p.size) });
			if (dx * dx + dz * dz < nativeNearM * nativeNearM) return true;
		}
		const double x0 = double(p.x0) + o.x, z0 = double(p.z0) + o.y;
		return !(x0 >= minXZ.x && z0 >= minXZ.y && x0 + double(p.size) <= maxXZ.x && z0 + double(p.size) <= maxXZ.y);
	});
	inside.assign(mid, frame.patches.end());
	frame.patches.erase(mid, frame.patches.end());
}

} // namespace HE
