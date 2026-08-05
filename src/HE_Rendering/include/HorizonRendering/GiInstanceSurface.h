#pragma once
#include "../HE_RENDERING_API.h"
#include "RenderObject.h"
#include <Math/Math.h>

class ContentManager; // global namespace (HE_Core's ContentManager is not namespaced)

// ─── GI-hit surface approximation ─────────────────────────────────────────────
// The ray kernels shade a hit per INSTANCE, not per texel: there is no UV, no
// material evaluation and no texture fetch at a BVH/TLAS hit, so every instance
// carries one flat surface description. Resolving it is NOT a one-liner —
// `RenderObject::baseColor` is only the mesh's own colour, a material override
// has to be looked up, and a NODE-GRAPH material has no scalar colour at all
// (its BaseColor/Emissive pins are folded on the CPU into
// MaterialAsset::approx*, with a HeParams slot index when the pin is
// param-driven so the LIVE value — including a per-entity override — wins).
//
// Getting this wrong is invisible in the diffuse GI (a slightly-off bounce
// tint) and glaring in reflections: before the approx fold existed, EVERY
// graph material reflected plain white.
//
// SYNC: MetalRenderer.mm's static giInstanceShading() is the same resolution,
// written before this shared copy existed. The two must agree — a divergence
// shows up as the same scene reflecting different colours per backend.
namespace HE
{

struct GiInstanceSurface
{
	glm::vec3 albedo    = { 1.0f, 1.0f, 1.0f }; // flat bounce/reflection colour
	glm::vec3 emissive  = { 0.0f, 0.0f, 0.0f }; // self-lit term (reflections only — see below)
	float     metallic  = 0.0f;                 // mirror-ness for a reflection bounce loop
	float     roughness = 0.5f;
};

// `cm` may be null (no content manager yet) — the object's own scalars are then
// the whole answer. Emissive is deliberately NOT fed back into the DDGI probe
// field: that would change the scene's global light balance, not just what a
// mirror shows (docs/gi-reflections-plan.md P4).
HE_RENDERING_API GiInstanceSurface giInstanceSurface(const RenderObject&  obj,
                                                     const ContentManager* cm);

} // namespace HE
