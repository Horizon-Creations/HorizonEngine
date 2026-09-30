#pragma once

// ─── Bloom blur kernel: 9 taps on whole texels, folded into 5 bilinear taps ───
// Every backend's separable bloom blur uses the same 9-tap Gaussian (σ ≈ 1.9
// texel): a centre weight and four weights on ±1..±4 whole texels. A linear
// sampler can fetch the weighted sum of TWO neighbouring texels in one tap when
// it samples between them at the weight-proportional offset, so the pairs
// (1,2) and (3,4) collapse into one tap each per side:
//
//     w12 = w1 + w2,  o12 = (1·w1 + 2·w2) / w12
//     w34 = w3 + w4,  o34 = (3·w3 + 4·w4) / w34
//
// Same result, 5 fetches instead of 9 (perf audit 2026-09-27, B4). It only holds
// when the sampler filters linearly and the source and destination share one
// resolution, so each fragment's uv lands on a source texel centre, which the
// ping-pong bloom targets do. clamp_to_edge keeps it exact at the borders: both
// texels of a folded pair clamp to the edge texel, exactly as the two whole-
// texel taps did. The GPU quantises the bilinear fraction (8 bits on Apple
// GPUs), so the result matches to within that rounding, not bit for bit.
//
// The Metal shader (blurFragment in MetalRenderer.mm) carries these literals;
// tests/test_bloom_kernel.cpp proves the fold against the 9-tap reference.
// The other backends (OpenGL, postfx_bloom_blur.frag, D3D11, D3D12) still run
// the 9-tap loop.
namespace HE
{

// The 9-tap reference: centre, then ±1, ±2, ±3, ±4 texels.
inline constexpr float kBloomBlurWeights9[5] = { 0.227027f, 0.1945946f, 0.1216216f, 0.054054f, 0.016216f };

// The 5-tap bilinear fold: centre, then ±o12 and ±o34 texels.
inline constexpr float kBloomBlurWeight0  = kBloomBlurWeights9[0];
inline constexpr float kBloomBlurWeight12 = kBloomBlurWeights9[1] + kBloomBlurWeights9[2];
inline constexpr float kBloomBlurWeight34 = kBloomBlurWeights9[3] + kBloomBlurWeights9[4];
inline constexpr float kBloomBlurOffset12 =
	(1.0f * kBloomBlurWeights9[1] + 2.0f * kBloomBlurWeights9[2]) / kBloomBlurWeight12;
inline constexpr float kBloomBlurOffset34 =
	(3.0f * kBloomBlurWeights9[3] + 4.0f * kBloomBlurWeights9[4]) / kBloomBlurWeight34;

} // namespace HE
