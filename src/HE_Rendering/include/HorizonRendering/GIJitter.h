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

} // namespace HE
