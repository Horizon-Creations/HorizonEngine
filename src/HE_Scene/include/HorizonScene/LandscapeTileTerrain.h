#pragma once
#include "HorizonScene/Components/TerrainComponent.h"
#include "HorizonScene/LandscapeSource.h"
#include "HorizonScene/LandscapeTile.h"
#include "HorizonScene/LandscapeWaterData.h"

#include <functional>
#include <memory>

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

// ── A square of tiles as ONE TerrainComponent (water) ─────────────────────────────
// Water sheets are built from the lattice of one water::Field (WaterMesh.h): a shore that runs across the border of two Fields would be
// contoured twice, from different cells on each side, and the two sheets would not meet. So water is built over a REGION of n x n
// resident tiles, as one terrain, and seams can only be at the region's border. The region carries
//   * heights: the tiles' level 0 side by side, (n * hCells + 1) squared; the border samples the tiles share are identical, so the assembly
//     is exact. A TerrainComponent holds at most kTerrainMaxResolution (1025) samples a side: n * hCells + 1 <= 1025, which is n <= 4
//     for the 256-cell tile (2 km). A larger n is refused.
//   * the paint weights, de-interleaved into the three pages, at n * wTexels;
//   * a water::Field of n * wTexels cells with the tiles' coverage and owner rasters and the pack's bodies that occur in it (ids global),
//     with the world's clip settings.
// Space is the terrain's: centred, x and z in [-n * tileM / 2, n * tileM / 2]; the centre in the absolute world is `centre`. Heights and
// body levels are absolute metres (the terrain sits at y = 0).
struct LandscapeRegionTerrain
{
	TerrainComponent terrain;
	LandscapeTileKey origin;        // the region's lowest tile (x, z)
	int              n = 0;         // tiles a side
	glm::dvec3       centre{ 0.0 }; // absolute world position of the terrain's local origin
	bool             hasWater = false;
};

// `tileAt` hands out the resident tile of a key (null when it is not in memory). False when any tile of the region is missing, `n` is
// out of range, or the heights would not fit a TerrainComponent. `waterTable` null: no Field is built (the terrain is heights and weights).
bool landscapeRegionTerrain(const std::function<std::shared_ptr<const LandscapeTile>(LandscapeTileKey)>& tileAt, LandscapeTileKey origin, int n,
                            const LandscapeTileTerrainParams& params, const LandscapeWaterTable* waterTable, LandscapeRegionTerrain& out);

} // namespace HE
