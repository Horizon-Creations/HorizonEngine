#include "doctest.h"
#include <HorizonScene/LandscapeHlodPlan.h>

// The planner behind the baked far-landscape material: what a pixel at the switch distance wants,
// the per-tile cap, and the halving that fits a world into a byte budget.

using namespace HE::landscape;

TEST_CASE("HLOD plan: the screen decides, one texel per pixel at the near distance of each level")
{
	const HlodBakePlan plan = planHlodBake(HlodBakeInput{});   // 512 m tiles, 60 deg, 1080p, near 2 km
	REQUIRE(plan.levels.size() == 6);
	// A pixel at 2 km spans 2*2000*tan(30deg)/1080 = 2.138 m, so a 512 m tile wants 240 texels.
	CHECK(plan.levels[0].nearM == doctest::Approx(2000.0f));
	CHECK(plan.levels[0].neededRaster == doctest::Approx(2.1383f).epsilon(0.001));
	CHECK(plan.levels[0].neededTexels == 240);
	// Every coarser level starts twice as far out and wants half the texels.
	CHECK(plan.levels[1].neededTexels == 120);
	CHECK(plan.levels[2].neededTexels == 60);
	CHECK(plan.levels[3].neededTexels == 30);
	CHECK(plan.levels[4].neededTexels == 15);
	// Planned sides are powers of two (block formats, mips), never below the floor.
	const int expected[6] = { 256, 128, 64, 32, 16, 16 };
	for (int i = 0; i < 6; ++i) CHECK(plan.levels[static_cast<size_t>(i)].texels == expected[i]);
	for (const HlodLevelPlan& l : plan.levels) CHECK_FALSE(l.downscaled == (l.texels >= l.neededTexels));
	CHECK(plan.levels[0].rasterM == doctest::Approx(2.0f));
	// 3 maps x side^2 x 1 byte x 4/3 for the mips = 4 x side^2 bytes per level.
	CHECK(plan.levels[0].bytesPerTile == 4u * 256u * 256u);
	CHECK(plan.bytesPerTile == 4u * (65536u + 16384u + 4096u + 1024u + 256u + 256u));
	CHECK(plan.bytesPerTile == 350208u);
}

TEST_CASE("HLOD plan: a need above the cap is downscaled and says so")
{
	HlodBakeInput in;
	in.nearM = 500.0f;                       // a pixel is 0.53 m here: 958 texels wanted
	const HlodBakePlan plan = planHlodBake(in);
	CHECK(plan.levels[0].neededTexels > 256);
	CHECK(plan.levels[0].texels == 256);
	CHECK(plan.levels[0].downscaled);
	CHECK(plan.levels[1].texels == 128);     // never more than half the finer level
	CHECK(plan.levels[1].downscaled);        // it wanted 479
}

TEST_CASE("HLOD plan: a world that does not fit is halved from the finest level until it does")
{
	HlodBakeInput in;
	in.tileCount = 195u * 195u;              // 100 km of 512 m tiles
	const HlodBakePlan open = planHlodBake(in);
	CHECK(open.totalBytes == 350208ull * 38025ull);       // 13.3 GB: no budget, no halving
	CHECK(open.budgetSteps == 0);
	CHECK(open.withinBudget);

	struct Case { uint64_t budget; int side0; int steps; };
	const Case cases[] = {
		{ 14'000'000'000ull, 256, 0 },
		{  4'000'000'000ull, 128, 1 },
		{  2'000'000'000ull,  64, 2 },
		{    500'000'000ull,  32, 3 },
	};
	for (const Case& c : cases)
	{
		in.budgetBytes = c.budget;
		const HlodBakePlan p = planHlodBake(in);
		CHECK(p.withinBudget);
		CHECK(p.totalBytes <= c.budget);
		CHECK(p.levels[0].texels == c.side0);
		CHECK(p.budgetSteps == c.steps);
		for (size_t i = 1; i < p.levels.size(); ++i)      // a coarser level never has more texels
			CHECK(p.levels[i].texels <= p.levels[i - 1].texels);
	}

	// A budget below the floor cannot be met, and the plan says so instead of lying.
	in.budgetBytes = 1'000'000ull;
	const HlodBakePlan tiny = planHlodBake(in);
	CHECK_FALSE(tiny.withinBudget);
	CHECK(tiny.levels[0].texels == 16);
}

TEST_CASE("HLOD plan: a small world needs no downscaling at all")
{
	HlodBakeInput in;
	in.tileCount = 8u * 8u;                  // a 4 km landscape
	in.budgetBytes = 64ull * 1024 * 1024;
	const HlodBakePlan p = planHlodBake(in);
	CHECK(p.totalBytes == 350208ull * 64ull);              // 21 MB
	CHECK(p.budgetSteps == 0);
	CHECK(p.levels[0].texels == 256);
	CHECK_FALSE(p.levels[0].downscaled);
}
