#include "doctest.h"
#include <HorizonRendering/GIReflectionTier.h>

#include <cmath>
#include <limits>

// The tier table used to live inline in MetalRenderer.mm and OpenGLRenderer.cpp.
// These numbers are what Metal computed BEFORE it was lifted into
// GIReflectionTier.h, so a pass means "the lift changed nothing" — there is no
// way to run a pixel A/B for it off the Mac. D3D11/D3D12/Vulkan read the same
// table (docs/gi-reflections-cross-backend-plan-2026-10-20.md), which is the
// point of pinning it here.

TEST_CASE("GIReflTier: rays and resolution climb together, one table for all backends")
{
	const HE::GIReflTier low  = HE::giReflTierFor(0, true);
	const HE::GIReflTier med  = HE::giReflTierFor(1, true);
	const HE::GIReflTier high = HE::giReflTierFor(2, true);

	CHECK(low.rays == 1);  CHECK(low.resDiv == 4);  CHECK(low.blurFloor == doctest::Approx(0.75f));
	CHECK(med.rays == 2);  CHECK(med.resDiv == 2);  CHECK(med.blurFloor == doctest::Approx(0.35f));
	CHECK(high.rays == 4); CHECK(high.resDiv == 1); CHECK(high.blurFloor == doctest::Approx(0.0f));

	// The kernels jitter inside the lobe exactly when they trace more than one ray.
	CHECK_FALSE(low.jitter);
	CHECK(med.jitter);
	CHECK(high.jitter);
}

TEST_CASE("GIReflTier: the blur switch zeroes only the resolution floor")
{
	for (int q = 0; q <= 2; ++q)
	{
		const HE::GIReflTier on  = HE::giReflTierFor(q, true);
		const HE::GIReflTier off = HE::giReflTierFor(q, false);
		CHECK(off.blurFloor == 0.0f);
		CHECK(off.rays == on.rays);
		CHECK(off.resDiv == on.resDiv);
		CHECK(off.jitter == on.jitter);
	}
}

TEST_CASE("GIReflTier: out-of-range quality clamps to the nearest tier")
{
	CHECK(HE::giReflTierFor(-3, true).rays == 1);
	CHECK(HE::giReflTierFor(-3, true).resDiv == 4);
	CHECK(HE::giReflTierFor(7, true).rays == 4);
	CHECK(HE::giReflTierFor(7, true).resDiv == 1);
}

TEST_CASE("GIReflBlurReach: Metal's tiers land on one screen reach, GL's fixed resDiv does not")
{
	const float maxRough = 0.6f; // the default GIReflMaxRoughness

	// Metal traces at the tier's own resolution, so rays x resolution cancel and
	// the blur covers the same span in SCREEN pixels at every tier.
	for (int q = 0; q <= 2; ++q)
	{
		const HE::GIReflTier t = HE::giReflTierFor(q, true);
		const HE::GIReflBlurReach r = HE::giReflBlurReachFor(maxRough, t.rays, t.resDiv);
		CHECK(r.reach == doctest::Approx(4.6f));
		CHECK(r.levels == 3);
		CHECK(r.unit == doctest::Approx(4.6f / 7.0f));
		// The scaled strides unit * (1 + 2 + 4) sum to the reach exactly.
		CHECK(r.unit * (std::exp2(static_cast<float>(r.levels)) - 1.0f) == doctest::Approx(r.reach));
	}

	// GL passes resDivUsed = 2 for every tier (its targets live at the GI prepass
	// resolution — the documented gap), so ITS reach shrinks with the ray count.
	// Pinned so a change to that behaviour is a decision, not a side effect.
	CHECK(HE::giReflBlurReachFor(maxRough, 1, 2).reach == doctest::Approx(8.2f));
	CHECK(HE::giReflBlurReachFor(maxRough, 2, 2).reach == doctest::Approx(4.6f));
	CHECK(HE::giReflBlurReachFor(maxRough, 4, 2).reach == doctest::Approx(2.8f));
	CHECK(HE::giReflBlurReachFor(maxRough, 1, 2).levels == 4);
	CHECK(HE::giReflBlurReachFor(maxRough, 4, 2).levels == 2);
}

TEST_CASE("GIReflBlurReach: degenerate inputs stay in range")
{
	// Roughness 0 = pure mirror: no lobe, one level, reach 1.
	const HE::GIReflBlurReach mirror = HE::giReflBlurReachFor(0.0f, 4, 1);
	CHECK(mirror.reach == doctest::Approx(1.0f));
	CHECK(mirror.levels == 1);

	// Roughness above 1 clamps; zero or negative divisors do not divide by zero.
	const HE::GIReflBlurReach wild = HE::giReflBlurReachFor(50.0f, 0, 0);
	CHECK(wild.reach == doctest::Approx(25.0f)); // 1 + 24 / (max(0,1) * max(0,1))
	CHECK(wild.levels >= 1);
	CHECK(wild.levels <= 6);
	CHECK(std::isfinite(wild.unit));
}
