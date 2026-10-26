#pragma once
#include <algorithm>
#include <cmath>

// ─── GI-reflection quality tiers, shared by every backend ───────────────────
// docs/gi-reflections-cross-backend-plan-2026-10-20.md. The tier means exactly
// two things — RAYS per pixel and trace RESOLUTION — plus the blur that only
// stands in for what the trace did not sample. It used to be spelled out inline
// in MetalRenderer.mm and again in OpenGLRenderer.cpp, and the copies had
// already drifted (GL hard-coded resDiv = 2). D3D11/D3D12/Vulkan take it from
// here so a tier reads the same on all of them.
//
// Pure arithmetic, no GPU types: header-only and inline, so there is nothing to
// export (no HE_API) and tests/test_gi_reflection_tier.cpp pins the values
// without linking a renderer. The setting itself is IRenderer::
// GIReflectionSettings (quality 0 Low / 1 Medium / 2 High, blur on/off).
namespace HE
{

// How far, in SCREEN pixels, the widest allowed lobe (roughness = maxRoughness)
// scatters when ONE ray samples it. Divided by the tier's ray count at use: the
// blur only stands in for the part of the lobe the rays missed. Absent entirely,
// every higher tier was noisier than the one below; held constant across tiers,
// every higher tier was blurrier. One definition replaces the two copies
// (Metal / GL) that carried a "keep them together" comment.
inline constexpr float kGIReflLobeScreenPx = 24.0f;

struct GIReflTier
{
	int   rays      = 1;    // specular rays per pixel (1 / 2 / 4)
	int   resDiv    = 4;    // trace resolution = screen / resDiv (4 / 2 / 1)
	float blurFloor = 0.0f; // blur even a MIRROR gets, to hide the resolution
	                        // (0.75 / 0.35 / 0); 0 when the blur is switched off
	bool  jitter    = false; // rays > 1: the kernel jitters inside the lobe
};

// Out-of-range quality clamps (SetGIReflectionSettings clamps too). A near-
// mirror still traces one ray whatever the tier: its cone is narrower than a
// pixel, so mirrors cost the same at every tier — that is the kernels' job, not
// part of this table.
inline GIReflTier giReflTierFor(int quality, bool blurEnabled)
{
	GIReflTier t;
	t.rays      = quality >= 2 ? 4 : (quality >= 1 ? 2 : 1);
	t.resDiv    = quality >= 2 ? 1 : (quality >= 1 ? 2 : 4);
	t.blurFloor = blurEnabled ? (t.resDiv >= 4 ? 0.75f : (t.resDiv >= 2 ? 0.35f : 0.0f)) : 0.0f;
	t.jitter    = t.rays > 1;
	return t;
}

struct GIReflBlurReach
{
	float reach  = 1.0f; // total blur reach in reflection texels
	int   levels = 1;    // separable H+V passes (strides unit * 2^level)
	float unit   = 1.0f; // stride of level 0, scaled so the levels sum to `reach`
};

// `resDivUsed` is the resolution the backend ACTUALLY traces at, which is what
// the blur must be measured against: Metal passes tier.resDiv, GL passes 2
// (its reflection targets live at the shared GI prepass resolution, a
// documented gap). Doubling raw strides and stopping past the target snaps the
// total to 2^n - 1, which quantised the "tier-independent" screen reach and let
// a better tier blur wider — the strides are scaled to land on `reach` exactly.
inline GIReflBlurReach giReflBlurReachFor(float maxRoughness, int rays, int resDivUsed)
{
	GIReflBlurReach r;
	const float lobeScreen = kGIReflLobeScreenPx * std::clamp(maxRoughness, 0.0f, 1.0f)
	                       / static_cast<float>(std::max(rays, 1));
	r.reach  = std::clamp(1.0f + lobeScreen / static_cast<float>(std::max(resDivUsed, 1)),
	                      1.0f, 512.0f);
	r.levels = std::clamp(static_cast<int>(std::ceil(std::log2(r.reach + 1.0f))), 1, 6);
	r.unit   = r.reach / (std::exp2(static_cast<float>(r.levels)) - 1.0f);
	return r;
}

} // namespace HE
