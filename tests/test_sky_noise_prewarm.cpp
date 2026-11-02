#include "doctest.h"
#include <HorizonRendering/SkyNoise3D.h>

// A game bakes the sky noise while SDL and the window come up; the renderer then takes the
// finished volume. The bytes must not depend on who baked them, and a stray prewarm must go away.

TEST_CASE("Sky noise prewarm: the renderer gets exactly the bytes a plain bake gives")
{
	const int n = 16;
	const std::vector<uint16_t> plain = HE::BuildSkyNoise3D(n);
	REQUIRE(plain.size() == size_t(n) * n * n * 2);

	HE::PrewarmSkyNoise3D(n);
	HE::PrewarmSkyNoise3D(n);                       // one is pending: this one is ignored
	const std::vector<uint16_t> taken = HE::BuildSkyNoise3D(n);
	CHECK(taken == plain);

	// Taken: the next call bakes again and still gives the same volume.
	CHECK(HE::BuildSkyNoise3D(n) == plain);
}

TEST_CASE("Sky noise prewarm: another size is not handed over, and a discard clears the slot")
{
	HE::PrewarmSkyNoise3D(8);
	const std::vector<uint16_t> other = HE::BuildSkyNoise3D(16);      // not the prewarmed n
	CHECK(other.size() == size_t(16) * 16 * 16 * 2);
	HE::DiscardSkyNoise3DPrewarm();                                    // the 8³ one nobody took

	HE::DiscardSkyNoise3DPrewarm();                                    // nothing pending: a no-op
	HE::PrewarmSkyNoise3D(16);                                         // the slot is free again
	CHECK(HE::BuildSkyNoise3D(16) == other);
}

TEST_CASE("Sky noise resolution: the full tile in a release build, a small one in debug")
{
	const int n = HE::SkyNoise3DResolution();
#ifdef NDEBUG
	CHECK(n == 256);
#else
	CHECK(n == 64);
#endif
}
