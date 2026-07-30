#pragma once
#include "../HE_RENDERING_API.h"
#include "RenderWorld.h"
#include <material/MaterialShaderLibrary.h> // MaterialShaderLibrary::Lighting (the graph-material light ABI)
#include <Math/Math.h>

// ─── Shared GPU light packing ─────────────────────────────────────────────────
// All three packings below were duplicated verbatim in the backends — the first
// two once per backend, FillMaterialLightWindow seven times (OpenGL and Metal
// each had a scene copy and a UI copy). Their OUTPUT IS OBSERVABLE: the shaders
// index the arrays positionally, so the order in which lights are written, the
// clamp, and the attenuation encoding are part of the contract with the
// GLSL/MSL/HLSL — not implementation detail.
namespace HE
{

// The engine's per-frame light window. Everything downstream (direct shading, GI
// probe bounce, local shadow-mask channels) works on the FIRST kMaxLightWindow
// lights of RenderWorld::lights, in extractor order.
inline constexpr int kMaxLightWindow = 8;

// Local (point/spot) lights that get a ray-traced shadow mask channel. The mask
// texture is RGBA, hence four.
inline constexpr int kMaxMaskedLocalLights = 4;

// ── GI probe-update local lights ──────────────────────────────────────────────
// Point/spot lights feeding the one-bounce probe estimate — a scene keyed by point
// lights otherwise converges to pitch-black probes. Same 8-light window the scene
// pass binds for direct shading.
//
// Note the two different scans: this one SKIPS directional lights and stops after
// 8 ACCEPTED lights, whereas BuildMaskedLocalLights (below) walks the first 8
// lights of the window and counts within them. They are deliberately different and
// were identical in all five backends.
struct PackedLightArray
{
	glm::vec4 posRange  [kMaxLightWindow] = {}; // xyz = world position, w = max(range, 1e-4)
	glm::vec4 colorType [kMaxLightWindow] = {}; // xyz = color * intensity, w = LightType (1 point, 2 spot)
	glm::vec4 dirCos    [kMaxLightWindow] = {}; // xyz = light travel direction, w = cos(half angle)
	int       count = 0;                        // populated entries; shaders read it as sunDirRadius.w
};

HE_RENDERING_API PackedLightArray BuildPackedLightArray(const RenderWorld& rw);

// ── Ray-traced local shadow-mask lights ───────────────────────────────────────
// First 4 local (point/spot) lights of the same 8-light window the scene shader
// iterates — the fragment shader's channel index is a plain counter over
// type != 0 in the SAME order, so count every non-directional light exactly like
// that loop does and fill the first 4 slots. (Hence `count` can exceed 4 during
// the scan; it is clamped only when written out.)
struct PackedLocalShadowLights
{
	glm::vec4 posRange[kMaxMaskedLocalLights] = {}; // xyz = position, w = max(range, 1e-4)
	int       count = 0;                            // already clamped to kMaxMaskedLocalLights
};

HE_RENDERING_API PackedLocalShadowLights BuildMaskedLocalLights(const RenderWorld& rw);

// ── Graph-material (heLitP) light window ──────────────────────────────────────
// Fills the light half of MaterialShaderLibrary::Lighting — the block every
// backend binds for node-graph materials. This is a THIRD, deliberately
// different scan from the two above: it walks the first kMaxLightWindow lights
// of RenderWorld::lights and writes ALL of them, directional included and
// without an intensity filter, because heLitP()'s loop is a plain
// `for (i < counts.x)` over that same window and its light-type branch expects
// slot i to be light i. Do not "fix" it to match BuildPackedLightArray.
//
// localShadowsActive = the local (point/spot) shadow atlas is bound and valid
// this frame. Only then is lightParams[i].y written as shadowLayer + 1 (0 = the
// light casts no local shadow); every pass that has no atlas — UI quads,
// previews, and the backends with no local-shadow support at all — passes false
// and leaves the field zeroed, which is what the preamble reads as "none".
//
// Fields OTHER than the light window (sun, ambient, camPos, GI, fog, CSM,
// localShadowVP, weather) stay at the call sites: they differ per backend and
// per pass, and several are clip-space-convention-baked.
HE_RENDERING_API void FillMaterialLightWindow(const RenderWorld&               rw,
                                              MaterialShaderLibrary::Lighting& out,
                                              bool                             localShadowsActive);

} // namespace HE
