#pragma once
#include "HorizonScene/CellStreamer.h"

namespace HE
{

// ── The region gate for the entity cells of a tile landscape (Thema 326) ───────────────────────────────────────────────────────────────────
// CellStreamer can admit and drop its cells by coarse regions (CellRegionConfig); nothing set it. This is the policy for a scene that has a tile
// landscape and streaming cells: the entity cells follow the landscape's own residency, so a cell is never built where the tiles under it have
// been given back, and a whole region of cells goes together.
//
//  * regionSize: `tilesPerRegion` landscape tiles a side (4 x 512 m = 2 km, the block the editor's region tools use), rounded to a whole
//    number of cells (at least one), so a cell never straddles two regions.
//  * loadRadius, keepFactor: the landscape store's own. The gate is a CAP: it only bites where the manifest (or a pin with a big radius) would
//    build cells farther out than the landscape holds tiles. A manifest whose radii are inside the landscape's is not changed by it.
//
// A manifest without cells, or a landscape without tiles, gives the disabled config (regionSize 0).
CellRegionConfig cellRegionConfigForLandscape(const CellManifest& manifest, float tileM, float landscapeLoadRadius, float landscapeKeepFactor = 1.25f,
                                              int tilesPerRegion = 4);

inline bool sameRegionConfig(const CellRegionConfig& a, const CellRegionConfig& b)
{
	return a.regionSize == b.regionSize && a.loadRadius == b.loadRadius && a.keepFactor == b.keepFactor;
}

} // namespace HE
