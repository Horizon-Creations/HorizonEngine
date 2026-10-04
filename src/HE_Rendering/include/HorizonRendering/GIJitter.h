#pragma once
#include <cstdint>

// ─── GI ray-jitter frame index, shared by all five backends ─────────────────
// The GI shadow and reflection kernels pick their per-pixel cone sample from a
// hash of (pixel, frame index). That index used to be a float that grew by one
// every GI frame and was never reset, fed into fract(sin(dot(p, k)) * 43758).
// Once it passed ~1e5 (≈12 min of editor at 144 fps) the GPU's sin() lost the
// argument and the hash went constant: every pixel aimed at the same point of
// the sun's disk, the penumbra vanished and the shadow went hard and shifted
// (Thema 131, docs/gi-shadow-edge-noise-analysis-2026-10-02.md §3 B).
//
// Now the kernels take a per-pixel offset from an integer PCG hash plus an R2
// low-discrepancy step per frame (each pixel covers the sun disk evenly over
// the frames the temporal pass averages), and the host keeps the index in
// [0, kGIJitterPeriod), so the float uniform always carries a small exact
// integer. Reflection kernels give each glossy sample its own stream through
// the hashed pixel id (gid.y + sampleIndex * 65536), not through the seed: a
// seed offset only shifts the R2 sequence by a near-constant and bunches the
// samples of one frame together.
namespace HE
{

inline constexpr uint32_t kGIJitterPeriod = 1024;

inline float NextGIJitterSeed(float seed)
{
	const float next = seed + 1.0f;
	return next >= static_cast<float>(kGIJitterPeriod) ? 0.0f : next;
}

// ─── Shadow-mask spatial filter (Thema 134 §4.3) ─────────────────────────────
// The a-trous value stop scales with the Bernoulli sigma sqrt(c(1-c)/N_eff) of
// the accumulated visibility c: an exponential moving average with weight a
// over `rays` binary samples per frame has the variance of (1+a)/(1-a) * rays
// independent samples. Two iterations, holes 1 and 2 texels.
inline constexpr int kGIShadowAtrousIterations = 2;

inline float GIShadowEffectiveSamples(float historyWeight, int rays)
{
	const float a = historyWeight < 0.0f ? 0.0f : historyWeight > 0.98f ? 0.98f : historyWeight;
	return (1.0f + a) / (1.0f - a) * static_cast<float>(rays < 1 ? 1 : rays);
}

// Per-iteration parameters of the shared a-trous shader (all four copies):
// x = hole step in texels (0 = plain copy, the filter switched off),
// y = 1 when reading the shadow scalar from the temporal history's alpha
//     (first iteration), 0 when reading the previous iteration's red,
// z = N_eff, w unused.
struct GIShadowAtrousStep { float step, fromHistory, effectiveSamples, unused; };

inline GIShadowAtrousStep GIShadowAtrousParams(int iteration, bool filterOn, float historyWeight, int rays)
{
	return { filterOn ? static_cast<float>(1 << iteration) : 0.0f,
	         iteration == 0 ? 1.0f : 0.0f,
	         GIShadowEffectiveSamples(historyWeight, rays), 0.0f };
}

} // namespace HE
