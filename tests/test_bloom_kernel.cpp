#include "doctest.h"
#include <HorizonRendering/BloomKernel.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// The Metal bloom blur folds the 9-tap whole-texel Gaussian into 5 bilinear taps
// (perf audit B4). These tests replay both on the CPU with the sampler the shader
// uses (linear filter, clamp_to_edge, uv on texel centres) and require the same
// answer, including the texels whose taps run off the edge.
namespace
{
// A 1-D texture row read the way a linear clamp_to_edge sampler reads it:
// x is in texels, texel i's centre sits at i + 0.5.
float sampleLinearClamp(const std::vector<float>& row, float x)
{
	const int   n  = static_cast<int>(row.size());
	const float t  = x - 0.5f;
	const float fl = std::floor(t);
	const float f  = t - fl;
	auto at = [&](int i) { return row[static_cast<size_t>(std::clamp(i, 0, n - 1))]; };
	const int i0 = static_cast<int>(fl);
	return at(i0) * (1.0f - f) + at(i0 + 1) * f;
}

float blur9(const std::vector<float>& row, int i)
{
	const float c = static_cast<float>(i) + 0.5f;
	float r = sampleLinearClamp(row, c) * HE::kBloomBlurWeights9[0];
	for (int k = 1; k < 5; ++k)
		r += (sampleLinearClamp(row, c + k) + sampleLinearClamp(row, c - k)) * HE::kBloomBlurWeights9[k];
	return r;
}

float blur5(const std::vector<float>& row, int i)
{
	const float c = static_cast<float>(i) + 0.5f;
	float r = sampleLinearClamp(row, c) * HE::kBloomBlurWeight0;
	r += (sampleLinearClamp(row, c + HE::kBloomBlurOffset12) +
	      sampleLinearClamp(row, c - HE::kBloomBlurOffset12)) * HE::kBloomBlurWeight12;
	r += (sampleLinearClamp(row, c + HE::kBloomBlurOffset34) +
	      sampleLinearClamp(row, c - HE::kBloomBlurOffset34)) * HE::kBloomBlurWeight34;
	return r;
}
} // namespace

TEST_CASE("BloomKernel: the folded taps keep the 9-tap weight sum and sit between their texel pairs")
{
	const float sum9 = HE::kBloomBlurWeights9[0] +
	                   2.0f * (HE::kBloomBlurWeights9[1] + HE::kBloomBlurWeights9[2] +
	                           HE::kBloomBlurWeights9[3] + HE::kBloomBlurWeights9[4]);
	const float sum5 = HE::kBloomBlurWeight0 + 2.0f * (HE::kBloomBlurWeight12 + HE::kBloomBlurWeight34);
	CHECK(sum5 == doctest::Approx(sum9).epsilon(1e-6));
	CHECK(sum9 == doctest::Approx(1.0f).epsilon(1e-3));   // the Gaussian is normalised

	CHECK(HE::kBloomBlurOffset12 > 1.0f);
	CHECK(HE::kBloomBlurOffset12 < 2.0f);
	CHECK(HE::kBloomBlurOffset34 > 3.0f);
	CHECK(HE::kBloomBlurOffset34 < 4.0f);
	// The literals in MetalRenderer.mm's blurFragment.
	CHECK(HE::kBloomBlurWeight12 == doctest::Approx(0.3162162f).epsilon(1e-6));
	CHECK(HE::kBloomBlurWeight34 == doctest::Approx(0.07027f).epsilon(1e-6));
	CHECK(HE::kBloomBlurOffset12 == doctest::Approx(1.3846153f).epsilon(1e-6));
	CHECK(HE::kBloomBlurOffset34 == doctest::Approx(3.2307670f).epsilon(1e-6));
}

TEST_CASE("BloomKernel: 5 bilinear taps reproduce the 9 whole-texel taps, edges included")
{
	// A deterministic HDR-ish row: mostly dim, a few very bright texels (what the
	// bright pass leaves), two of them right at the borders.
	std::vector<float> row(64);
	uint32_t s = 12345u;
	for (float& v : row)
	{
		s = s * 1664525u + 1013904223u;
		v = static_cast<float>(s >> 8) * (1.0f / 16777216.0f);
	}
	row[0] = 40.0f; row[1] = 12.0f; row[30] = 25.0f; row[62] = 8.0f; row[63] = 60.0f;

	float maxErr = 0.0f;
	for (int i = 0; i < static_cast<int>(row.size()); ++i)
	{
		const float ref = blur9(row, i);
		const float got = blur5(row, i);
		maxErr = std::max(maxErr, std::fabs(ref - got) / std::max(1.0f, std::fabs(ref)));
	}
	CHECK(maxErr < 1e-5f);
}

TEST_CASE("BloomKernel: a single bright texel spreads into the 9-tap footprint and no further")
{
	// Impulse response: the fold must not widen or shift the kernel.
	std::vector<float> row(32, 0.0f);
	row[16] = 1.0f;
	for (int i = 0; i < 32; ++i)
	{
		const int   d   = std::abs(i - 16);
		const float ref = d <= 4 ? HE::kBloomBlurWeights9[d] : 0.0f;
		CHECK(blur5(row, i) == doctest::Approx(ref).epsilon(1e-5));
	}
}
