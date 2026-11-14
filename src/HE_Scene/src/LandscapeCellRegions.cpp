#include "HorizonScene/LandscapeCellRegions.h"

#include <algorithm>
#include <cmath>

namespace HE
{

CellRegionConfig cellRegionConfigForLandscape(const CellManifest& manifest, float tileM, float landscapeLoadRadius, float landscapeKeepFactor,
                                              int tilesPerRegion)
{
	CellRegionConfig rc;   // disabled
	if (manifest.empty() || !(tileM > 0.0f) || !(landscapeLoadRadius > 0.0f)) return rc;

	const float wanted = tileM * static_cast<float>(std::max(1, tilesPerRegion));
	const float cells = std::max(1.0f, std::round(wanted / manifest.cellSize));
	rc.regionSize = cells * manifest.cellSize;
	rc.loadRadius = landscapeLoadRadius;
	// A radius exactly at a region's half width puts the anchor's neighbours in reach as well (CellStreamer's squareDistance gives 0 on a
	// shared edge): one metre either way is the same landscape and no extra regions.
	if (std::fabs(rc.loadRadius - rc.regionSize * 0.5f) < 0.5f) rc.loadRadius += 1.0f;
	rc.keepFactor = std::max(landscapeKeepFactor, 1.0f);
	return rc;
}

} // namespace HE
