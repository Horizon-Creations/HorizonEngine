#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace HE::landscape
{

// ── Baked far-landscape material: how big, and what to give up when it is too big ────────────
// Design: docs/landscape-hlod-baked-material-2026-11-02.md (Thema 228 request, for Thema 326).
//
// Beyond the radius where the real landscape material is worth its shader (layer blend, auto
// rules, snow/sand), a tile is drawn from three baked maps (BaseColor, Normal, Roughness)
// instead. This is the planner for their resolution. Two rules, both pure arithmetic:
//
//  * NEEDED resolution: a texel needs to be no coarser than one screen pixel at the distance
//    where its level takes over. A pixel at distance d spans 2·d·tan(fovY/2) / screenHeight
//    metres, so level i (which starts at nearM · growth^i) wants a raster of that size, i.e.
//    tileM / raster texels per tile side.
//  * AUTOMATIC DOWNSCALING: the needed size is capped per tile (maxTexels) and then, if all
//    tiles of the world together exceed the byte budget, the finest level is halved (and the
//    coarser ones follow, a coarser level never has more than half the texels of the finer
//    one) until it fits or the floor (minTexels) is reached.
struct HlodBakeInput
{
	float    tileM            = 512.0f;   // tile edge in metres (LandscapeGeometry::tileM)
	int      levels           = 6;        // baked levels, finest first
	float    nearM            = 2000.0f;  // distance where the finest baked level takes over
	float    growth           = 2.0f;     // each coarser level starts this many times further out
	float    fovYDeg          = 60.0f;    // reference camera
	int      screenHeightPx   = 1080;     // reference screen
	float    texelsPerPixel   = 1.0f;     // 1: a texel is one pixel at the near distance; 0.5: twice as sharp
	int      maxTexels        = 256;      // cap per tile side (the downscaling when the need is higher)
	int      minTexels        = 16;       // floor per tile side
	int      maps             = 3;        // BaseColor, Normal, Roughness
	float    bytesPerTexel    = 1.0f;     // one map in ASTC 4x4 / BC7 (8 bits per texel)
	bool     mips             = true;     // each map stores its mip chain (+1/3)
	uint64_t tileCount        = 0;        // tiles of the world; with budgetBytes > 0 the planner fits them
	uint64_t budgetBytes      = 0;        // 0 = no budget, only the per-tile cap
};

struct HlodLevelPlan
{
	int      level        = 0;
	float    nearM        = 0.0f;   // where this level takes over
	float    neededRaster = 0.0f;   // metres per texel the screen wants there
	int      neededTexels = 0;      // tile side that would give it, before any cap (not rounded)
	int      texels       = 0;      // tile side actually planned (power of two)
	float    rasterM      = 0.0f;   // metres per texel actually planned
	bool     downscaled   = false;  // planned below what the screen wants
	uint64_t bytesPerTile = 0;      // all maps, with mips
};

struct HlodBakePlan
{
	std::vector<HlodLevelPlan> levels;
	uint64_t bytesPerTile   = 0;
	uint64_t totalBytes     = 0;    // bytesPerTile x tileCount
	int      budgetSteps    = 0;    // how often the finest level was halved to fit the budget
	bool     withinBudget   = true; // false: even the floor does not fit
};

inline int hlodPow2Ceil(int v)
{
	int p = 1;
	while (p < v) p <<= 1;
	return p;
}

inline HlodBakePlan planHlodBake(const HlodBakeInput& in)
{
	HlodBakePlan plan;
	const int count = std::max(1, in.levels);
	const float tanHalf = std::tan(in.fovYDeg * 0.5f * 3.14159265358979f / 180.0f);
	const int   minT = std::max(1, hlodPow2Ceil(std::max(1, in.minTexels)));
	const int   maxT = std::max(minT, hlodPow2Ceil(std::max(1, in.maxTexels)));
	const double mipFactor = in.mips ? 4.0 / 3.0 : 1.0;

	plan.levels.resize(static_cast<size_t>(count));
	for (int i = 0; i < count; ++i)
	{
		HlodLevelPlan& l = plan.levels[static_cast<size_t>(i)];
		l.level        = i;
		l.nearM        = in.nearM * std::pow(in.growth, static_cast<float>(i));
		l.neededRaster = in.texelsPerPixel * 2.0f * l.nearM * tanHalf / static_cast<float>(std::max(1, in.screenHeightPx));
		const float needed = l.neededRaster > 0.0f ? in.tileM / l.neededRaster : static_cast<float>(maxT);
		l.neededTexels = static_cast<int>(std::ceil(needed));
	}

	// The side of level 0 is what the cap and the budget move; the others follow it.
	int side0 = std::clamp(hlodPow2Ceil(plan.levels[0].neededTexels), minT, maxT);
	const auto fill = [&](int s0) {
		plan.bytesPerTile = 0;
		int prev = 0;
		for (int i = 0; i < count; ++i)
		{
			HlodLevelPlan& l = plan.levels[static_cast<size_t>(i)];
			int s = std::clamp(hlodPow2Ceil(l.neededTexels), minT, maxT);
			s = (i == 0) ? std::min(s, s0) : std::min(s, std::max(minT, prev / 2));
			l.texels       = s;
			l.rasterM      = in.tileM / static_cast<float>(s);
			l.downscaled   = s < l.neededTexels;
			l.bytesPerTile = static_cast<uint64_t>(std::llround(
				static_cast<double>(in.maps) * s * s * in.bytesPerTexel * mipFactor));
			plan.bytesPerTile += l.bytesPerTile;
			prev = s;
		}
		plan.totalBytes = plan.bytesPerTile * in.tileCount;
	};
	fill(side0);
	if (in.budgetBytes > 0)
	{
		while (plan.totalBytes > in.budgetBytes && side0 > minT)
		{
			side0 /= 2;
			++plan.budgetSteps;
			fill(side0);
		}
		plan.withinBudget = plan.totalBytes <= in.budgetBytes;
	}
	return plan;
}

} // namespace HE::landscape
