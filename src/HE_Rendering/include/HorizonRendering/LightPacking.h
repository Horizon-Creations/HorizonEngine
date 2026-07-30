#pragma once
#include "../HE_RENDERING_API.h"
#include "RenderWorld.h"
#include <Math/Math.h>

// ─── Shared GPU light packing ─────────────────────────────────────────────────
// Both packings below were duplicated verbatim in all five backends. Their OUTPUT
// IS OBSERVABLE: the shaders index the arrays positionally, so the order in which
// lights are written, the clamp, and the attenuation encoding are part of the
// contract with the GLSL/MSL/HLSL — not implementation detail.
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

} // namespace HE
