#pragma once
#include <Math/Math.h>
#include <cstdint>

// ─── Temporal AA, the host side (docs/anti-aliasing-plan.md A2/A3) ───────────
// What GL and Metal each spell inline, shared by D3D11, D3D12 and Vulkan so the
// three cannot drift apart from each other: the jitter sequence, how the jitter
// enters the rasterisation matrix, and the history weight. The shaders stay per
// backend (HLSL in D3D_Shared/HlslSources.h, GLSL in shaders/taa_*.frag).
//
// Header-only for the same reason ClipSpace.h is: the backend static libraries
// reuse this include path without linking HorizonRendering.
namespace HE
{

// Halton(2,3), 8 positions, in pixels around the pixel centre (-0.5..0.5). A
// low-discrepancy sequence covers the pixel evenly in few frames, where a random
// offset clumps and a regular grid re-aliases. `frameIndex` counts TAA frames;
// the sequence starts at index 1 (index 0 would be the exact centre, twice).
inline glm::vec2 taaJitter(uint32_t frameIndex)
{
	auto halton = [](uint32_t i, uint32_t base) {
		float f = 1.0f, r = 0.0f;
		while (i > 0) { f /= static_cast<float>(base); r += f * static_cast<float>(i % base); i /= base; }
		return r;
	};
	const uint32_t n = (frameIndex % 8u) + 1u;
	return glm::vec2(halton(n, 2) - 0.5f, halton(n, 3) - 0.5f);
}

// The rasterisation matrix. The offset is a clip-space translation of x/y by a
// fraction of a pixel — the same thing as shifting the sample grid — applied on
// the LEFT, so it works on any backend's final clip matrix (D3D's zero-to-one
// depth, Vulkan's flipped y: neither touches x/y translation). The caller's
// matrix stays untouched, so the unjittered one remains available for motion
// and reprojection. Identity for a zero jitter or a degenerate size.
inline glm::mat4 taaJitteredViewProj(const glm::mat4& viewProj, const glm::vec2& jitterPx,
                                     int width, int height)
{
	if (width <= 0 || height <= 0) return viewProj;
	glm::mat4 j(1.0f);
	j[3][0] = jitterPx.x * 2.0f / static_cast<float>(width);
	j[3][1] = jitterPx.y * 2.0f / static_cast<float>(height);
	return j * viewProj;
}

// History weight of the resolve. 0.9 keeps ~10 frames of samples: enough to
// converge on an edge, short enough that a mis-reprojected pixel does not
// linger. GL and Metal use the same number.
inline constexpr float kTaaHistoryBlend = 0.9f;

// Per-draw constants of the velocity pass, one layout for all three backends.
// mvpJitter rasterises; mvpNow / mvpPrev (both unjittered) measure. Column-major
// like every glm upload in the engine. 192 B is more than Vulkan's guaranteed
// 128 B of push constants, so no backend may take it as push/root constants
// without checking the device limit first.
struct TaaVelocityConstants
{
	glm::mat4 mvpJitter;
	glm::mat4 mvpNow;
	glm::mat4 mvpPrev;
};
static_assert(sizeof(TaaVelocityConstants) == 192, "TAA velocity constants are three mat4");

} // namespace HE
