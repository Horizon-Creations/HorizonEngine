#pragma once
#include "../HE_RENDERING_API.h"
#include <Math/Math.h>
#include <Types/UUID.h>
#include <cstdint>
#include <functional>
#include <string>

struct MaterialAsset; // HE_Core ContentManager assets (global namespace)
struct TextureAsset;

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
//
// AUTO landscapes (Thema 173, Schritt 3). The engine's auto material
// (AutoLandscapeMaterial.h) has no weightmap and no layer blend: its BaseColor
// is a mix of texture-array slices driven per pixel by slope and world height,
// which the CPU fold cannot reach (it reflected plain white). Such a terrain
// gets an entry of its own kind, layerCount = kGiLandAuto: the kernels rebuild
// the graph's MASKS from the hit normal and height — the same thresholds, read
// from the material's live parameters — and mix the slices' MEAN colours
// (sRGB-decoded on the CPU from the albedo array's smallest mips). Left out on
// purpose, all of them texel-scale detail a reflection hit cannot show: the
// textures themselves, bombing, the Fbm dirt patches (their expected share
// instead), the height-map bias of every transition and the puddle noise (its
// expected share on flat ground instead). giAutoLandscapeAlbedo below is the
// reference; the three kernel copies (kGIReflMSL, kGISWMSL, kGiReflCS) mirror
// it line for line. Reflection-only like the painted path.
namespace HE
{

// Kernel-side cap: each landscape's weightmap occupies one texture binding in
// the reflection kernels. Scenes beyond this keep the flat per-instance colour
// (correct, just paint-agnostic) rather than losing a landscape entirely.
inline constexpr int kGiMaxLandscapes = 4;

// layerCount of an auto-landscape entry (cfg.w < 0 in the kernels).
inline constexpr int32_t kGiLandAuto = -1;

struct GiLandscape
{
	glm::mat4 worldToLocal{ 1.0f };  // landscape entity's inverse world matrix
	glm::vec2 invSize{ 0.01f };      // 1 / (sizeX, sizeZ) — local XZ → 0..1 across the terrain
	float     uvTiling  = 1.0f;      // TerrainComponent::uvTiling (the mesh UVs carry it too)
	int32_t   layerCount = 0;        // 0 = material is not layer-blended → use the flat colour; kGiLandAuto = auto
	glm::vec4 layerColor[4]{};       // per-layer folded colour (rgb; a unused). Auto: Grass, Dirt, Rock, Snow means
	HE::UUID  weightmapId{};         // painted RGBA8 weights; null = unpainted (layer 0). Auto: unused
	// Auto entries only (zero otherwise):
	glm::vec4 autoWet{ 0.0f };       // rgb = puddle colour (Wet Ground mean, water-darkened), a = puddle share of flat ground
	glm::vec4 autoSlope{ 0.0f };     // x Rock Slope, y Rock Blend, z Dirt Amount, w Puddle Max Slope
	glm::vec4 autoSnow{ 0.0f };      // x Snow Height (world Y), y Snow Blend, z Snow Max Slope, w unused
};

namespace giauto_detail
{
inline float sat(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
// GLSL/MSL smoothstep (e0 < e1 always holds here: every width is clamped > 0).
inline float smooth(float e0, float e1, float x)
{
	const float t = sat((x - e0) / (e1 - e0));
	return t * t * (3.0f - 2.0f * t);
}
}

// Reference albedo of an auto-landscape hit at world position `pos` with world
// normal `n` (unit, facing the ray). The order of the stages is the graph's:
// ground (grass ← dirt) → rock → snow → puddles. Kernel copies must match.
inline glm::vec3 giAutoLandscapeAlbedo(const GiLandscape& L, const glm::vec3& pos, const glm::vec3& n)
{
	using namespace giauto_detail;
	const float slope = sat(1.0f - n.y);
	const float rs = L.autoSlope.x, rb = L.autoSlope.y;
	// Dirt: the Fbm patches' expected share (Dirt Amount) plus the scree belt
	// just below the rock slope.
	const float dirt  = sat(L.autoSlope.z + smooth(rs - rb, rs, slope));
	const glm::vec3 ground = glm::mix(glm::vec3(L.layerColor[0]), glm::vec3(L.layerColor[1]), dirt);
	const float rock  = smooth(rs, rs + rb, slope);
	const glm::vec3 s1 = glm::mix(ground, glm::vec3(L.layerColor[2]), rock);
	const float snow  = smooth(L.autoSnow.x, L.autoSnow.x + L.autoSnow.y, pos.y)
	                  * (1.0f - smooth(L.autoSnow.z, L.autoSnow.z + 0.1f, slope));
	const glm::vec3 s2 = glm::mix(s1, glm::vec3(L.layerColor[3]), snow);
	const float pms   = L.autoSlope.w;
	const float flat  = (1.0f - smooth(0.5f * pms, pms, slope)) * (1.0f - snow);
	return glm::mix(s2, glm::vec3(L.autoWet), L.autoWet.w * flat);
}

// CPU side of an auto entry (GiAutoLandscape.cpp), used by the extractor.
//
// giAutoLandscapeParams: is `ma` an auto landscape material — recognised by
// its parameter NAMES (kAutoLandscapeParam*), not its id, so instances and
// copies of M_AutoLandscape count too — and if so, fill the auto parameters of
// `out` (layerCount = kGiLandAuto) from the live slot values, `overrideValue`
// first (a per-entity override; may be empty). Returns the graph-texture slot
// holding the albedo array (the one whose path is kAutoLandscapeAlbedoArray,
// slot 0 when the asset carries baked ids only), or -1 = not an auto material.
HE_RENDERING_API int giAutoLandscapeParams(
	const MaterialAsset& ma,
	const std::function<bool(const std::string& name, float& value)>& overrideValue,
	GiLandscape& out);

// giAutoLandscapeSliceMeans: the mean LINEAR colour of slices 0..4 (Grass, Dirt,
// Rock, Snow, Wet Ground) of the RGBA8 albedo array, read from a mip of at most
// 16×16 (a few hundred texels per slice — cheap enough to redo every frame,
// so a reimported array is never stale) and sRGB-decoded when the asset is
// sRGB. Fills layerColor[0..3] and autoWet.rgb (darkened for standing water).
// False = not a usable array of at least 5 slices.
HE_RENDERING_API bool giAutoLandscapeSliceMeans(const TextureAsset& albedoArray, GiLandscape& out);

} // namespace HE
