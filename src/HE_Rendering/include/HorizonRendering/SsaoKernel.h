#pragma once
#include "../HE_RENDERING_API.h"
#include <Math/Math.h>
#include <cstdint>
#include <vector>

// ─── SSAO kernel + rotation noise (shared by every backend) ───────────────────
// The five backends used to carry a private, byte-identical copy of this (OpenGL,
// Metal, Vulkan, D3D11, D3D12). Both generators are pure functions of a fixed
// seed, so every backend gets THE SAME samples — a prerequisite for
// GL == Metal == Vulkan == D3D visual parity, which is why the copies carried
// "keep in sync" comments. There is nothing handedness-dependent here: the kernel
// is expressed in the fragment's TANGENT space (+Z = surface normal) and the noise
// lies in the tangent plane, so no backend has to flip a sign for its clip-space
// convention.
namespace HE
{

// Deterministic [0,1) RNG. A plain LCG, NOT <random>: std::mt19937 +
// uniform_real_distribution is only reproducible for the engine, not across
// standard libraries, and the whole point is that all five backends bake the
// identical numbers.
struct SsaoRng
{
	uint32_t s;
	float next() { s = s * 1664525u + 1013904223u; return float(s >> 8) * (1.0f / 16777216.0f); }
};

// Sample counts every backend agrees on: a 32-tap kernel and a 4x4 noise tile
// (16 rotation vectors) repeated across the screen.
inline constexpr int kSsaoKernelSize    = 32;
inline constexpr int kSsaoNoiseTileSize = 4;
inline constexpr int kSsaoNoiseCount    = kSsaoNoiseTileSize * kSsaoNoiseTileSize;

// Cosine-ish hemisphere kernel oriented to +Z, packed toward the origin so close
// occluders dominate.
HE_RENDERING_API std::vector<glm::vec3> BuildSSAOKernel(int n);

// 4x4 tile of random rotation vectors in the tangent plane (z = 0). Returned as
// vec3 so a GL_RGB / vec3-array upload matches directly.
HE_RENDERING_API std::vector<glm::vec3> BuildSSAONoise(int n);

// Same RNG stream, same numbers, pre-padded to vec4 — for backends that upload
// the tile straight into an RGBA32F image (Vulkan) instead of expanding vec3
// element-by-element. BuildSSAONoiseRGBA(n)[i] == vec4(BuildSSAONoise(n)[i], 0).
HE_RENDERING_API std::vector<glm::vec4> BuildSSAONoiseRGBA(int n);

} // namespace HE
