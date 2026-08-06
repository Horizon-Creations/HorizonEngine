#pragma once
#include "../HE_RENDERING_API.h"
#include <Math/Math.h>
#include <Types/UUID.h>
#include <cstdint>

// ─── Painted landscapes, as the GI ray kernels see them ──────────────────────
// A ray hit carries no UV and no material evaluation, so every other surface is
// shaded from ONE flat colour per instance (HE::giInstanceSurface). For a
// landscape that is not enough: its whole point is that the paint VARIES across
// it, and a per-instance colour flattens a red ridge on a green hillside into
// one averaged tint — the mirror then shows a single colour where the terrain
// plainly has two.
//
// A landscape is the one surface where the missing UV can be RECONSTRUCTED. It
// is a heightfield over an axis-aligned local XZ rect and its mesh UVs are a
// linear function of that rect (TerrainMeshGenerator writes global, not
// per-chunk, UVs), so world hit position → local XZ → UV is exact — no
// per-vertex UV in the acceleration structure, no vertex-format change, and it
// works identically in the hardware, software-BVH and OpenGL kernels.
//
// The kernels then sample the painted weightmap at that UV and blend these
// per-layer colours (MaterialAsset::approxLayerColor, the CPU fold of each
// layer input). That is per-TEXEL, i.e. the same weights the rasterizer reads.
//
// Deliberately reflection-only: the DDGI probe bounce keeps the flat
// per-instance tint (RenderObject::landscapeLayerWeights). Its rays are a
// low-frequency diffuse estimate spread over an octahedral probe texel — paint
// detail is invisible there, and the extra sample per bounce ray is not.
namespace HE
{

// Kernel-side cap: each landscape's weightmap occupies one texture binding in
// the reflection kernels. Scenes beyond this keep the flat per-instance colour
// (correct, just paint-agnostic) rather than losing a landscape entirely.
inline constexpr int kGiMaxLandscapes = 4;

struct GiLandscape
{
	glm::mat4 worldToLocal{ 1.0f };  // landscape entity's inverse world matrix
	glm::vec2 invSize{ 0.01f };      // 1 / (sizeX, sizeZ) — local XZ → 0..1 across the terrain
	float     uvTiling  = 1.0f;      // TerrainComponent::uvTiling (the mesh UVs carry it too)
	int32_t   layerCount = 0;        // 0 = material is not layer-blended → use the flat colour
	glm::vec4 layerColor[4]{};       // per-layer folded colour (rgb; a unused)
	HE::UUID  weightmapId{};         // painted RGBA8 weights; null = unpainted (layer 0)
};

} // namespace HE
