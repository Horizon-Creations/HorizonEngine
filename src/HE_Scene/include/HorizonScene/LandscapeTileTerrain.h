#pragma once
#include "HorizonScene/Components/TerrainComponent.h"
#include "HorizonScene/LandscapeSource.h"
#include "HorizonScene/LandscapeTile.h"

namespace HE
{

// ── A tile, seen as a TerrainComponent (Thema 326, step 3) ───────────────────────
// Foliage scatter, water fields (water::), shore clipping and every other piece of the single landscape take a TerrainComponent
// and read its size, heights and paint weights. A tile has exactly that data: a square of tileM metres, a height raster, up to
// twelve weight channels. So instead of teaching each of them about tiles, a tile is converted into a TerrainComponent that
// lives on the STACK of whoever works with it.
//
// NEVER put the result into a registry. A TerrainComponent in a registry is picked up by TerrainSystem (it builds chunk meshes), by
// the physics (an implicit height field body under the ground fields of LandscapePhysics) and by the render extractor's GI
// landscape table (four entries in all): all three would treat a tile as a landscape of its own.
//
// Local space of the result is the one of every TerrainComponent: centred, x and z in [-tileM / 2, tileM / 2]. `sculptHeights` hold
// absolute metres (heightMinM + stored * heightScaleM / 65535), so the terrain's world position has a y of 0 and height rules
// ("only above 300 m") judge the same numbers the renderer and the physics use. The weights are the tile's level 0, de-interleaved
// into the component's three pages (channels 0-3, 4-7, 8-11).
struct LandscapeTileTerrainParams
{
	float heightMinM = 0.0f;
	float heightScaleM = 3000.0f;
};

// An empty-sized terrain (sizeX == 0) for a tile that cannot be represented: no level 0, or more height samples than a TerrainComponent
// can hold (kTerrainMaxResolution).
TerrainComponent landscapeTileTerrain(const LandscapeTile& tile, const LandscapeTileTerrainParams& params);

// The tile's centre in the absolute world: the terrain's local origin.
inline glm::dvec3 landscapeTileCentre(LandscapeTileKey key, const LandscapeGeometry& g)
{
	return { (key.x + 0.5) * static_cast<double>(g.tileM), 0.0, (key.z + 0.5) * static_cast<double>(g.tileM) };
}

} // namespace HE
