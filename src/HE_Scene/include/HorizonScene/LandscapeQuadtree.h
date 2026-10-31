#pragma once
#include "HorizonScene/LandscapeTile.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace HE
{

// ── World-scale landscape: which patches to draw (Thema 326, step 2, part A) ──
// A CDLOD-style quadtree over the whole world. Every node is drawn with the SAME 64 x 64 cell grid, so a node
// of level L is 64 * cell0 * 2^L metres wide (128 m for level 0 at the default 2 m, 8 km for level 6) and its
// cell size doubles with each level. The vertex shader gives each vertex the height of its own grid point and
// then, by its distance to the camera, morphs the odd vertices toward the average of their even neighbours
// (landscapeMorph below), so a node meets a coarser neighbour without a crack and without stitching meshes.
//
// A node is drawn as a whole when it lies beyond the finer level's range; otherwise it is split. A child that
// is out of its own range is drawn by the PARENT, as a quarter of the parent's grid (quadrants bitmask), which
// is what keeps the cell size right at the border between two levels.
//
// This is the selection only: pure CPU, no renderer, no textures. The backends take the patch list; the data
// behind a patch comes from the store's tiles for levels 0..tileLevels and from the always-resident coarse
// world pyramid for the levels above (a node of level >= 4 spans 16 or more tiles, which are never all in memory).
struct LandscapeQuadtreeConfig
{
	// The world: tilesX x tilesZ tiles of tileM metres, origin at the absolute origin. Same as the store's source.
	float tileM  = 512.0f;
	int   tilesX = 196;
	int   tilesZ = 196;

	int   patchCells = 64;       // cells per patch side (the shared grid)
	float cell0M     = 2.0f;     // cell size of level 0 (the tile's level-0 raster)
	// Level L is drawn from distance range(L-1) to range(L), range(L) = rangeFactor * patchSize(L). A node must
	// have rangeFactor * morphStart >= sqrt(2) for the morph to hide every seam (checked by the tests).
	float rangeFactor = 6.0f;
	float morphStart  = 0.6f;    // the morph begins this far into a level's distance band, ends at its far edge
	int   tileLevels  = 3;       // levels 0..tileLevels need the store's tiles, higher ones the coarse pyramid
	// Heights of the slab a node is tested with (frustum, and the vertical term of the distance, see landscapeMorphDistance).
	float minY = 0.0f;
	float maxY = 3000.0f;

	float patchSize(int level) const { return static_cast<float>(patchCells) * cell0M * static_cast<float>(1 << level); }
	float cellSize(int level) const { return cell0M * static_cast<float>(1 << level); }
	float rangeOf(int level) const { return level < 0 ? 0.0f : rangeFactor * patchSize(level); }
	// First level whose single node covers the whole world.
	int   topLevel() const;

	// The tile geometry and the world size of a source, so the two cannot drift apart.
	static LandscapeQuadtreeConfig fromGeometry(const LandscapeGeometry& g, int tilesX, int tilesZ);
	// The store has to keep every tile under a node of level <= tileLevels whenever such a node is in range: a node of level
	// tileLevels is drawn up to rangeOf(tileLevels) away and reaches sqrt(2) patch sizes beyond that. A smaller load radius
	// leaves holes in the fine levels at the edge of the loaded disc, drawn by a coarser parent next to finer siblings: a step.
	float minLoadRadius() const;
	bool  fitsStore(float loadRadius) const { return loadRadius >= minLoadRadius(); }
};

// One patch to draw: a node of the quadtree, or some quadrants of it.
struct LandscapePatch
{
	int      level = 0;
	int      ix = 0, iz = 0;     // node index at this level; the node covers [ix, ix+1) * patchSize
	uint8_t  quadrants = 15;     // bit q = dx + 2 * dz of the quarter that is drawn (15 = the whole node)
	double   x0 = 0.0, z0 = 0.0; // world position of the node's corner
	float    size = 0.0f;        // edge of the node, metres
};

// Frustum as six inward-facing planes (a, b, c, d): a*x + b*y + c*z + d >= 0 inside. Invalid = no culling.
struct LandscapeFrustum
{
	glm::dvec4 planes[6];
	bool       valid = false;
	// From a combined view-projection matrix (Gribb/Hartmann). Depth range -1..1 or 0..1: pass zeroToOne.
	static LandscapeFrustum fromViewProjection(const glm::dmat4& viewProj, bool zeroToOne = false);
};

// Is the tile (tx, tz) in memory? Called for the tiles a node of level <= tileLevels covers.
using LandscapeTileResident = std::function<bool(int tx, int tz)>;

// The patches to draw from `camera` (absolute world position). A tile that is not resident keeps its area at the
// coarsest level that has data, so the result always covers the whole world inside the frustum without a hole.
std::vector<LandscapePatch> selectLandscapePatches(const LandscapeQuadtreeConfig& cfg, const glm::dvec3& camera,
                                                   const LandscapeFrustum* frustum = nullptr,
                                                   const LandscapeTileResident& resident = {});

// The distance both the selection and the morph work with: horizontal distance from the camera to (x, z), plus the
// camera's distance to the height slab [minY, maxY] (0 while the camera is inside it). It does NOT include the height
// of the vertex: the node test (box distance to the slab) and the per-vertex morph then see the same metric, which
// is what makes the seams exact. A tighter per-patch height range would lose that unless the shader got it too.
float landscapeMorphDistance(const LandscapeQuadtreeConfig& cfg, const glm::dvec3& camera, double x, double z);

// 0 = the vertex keeps its own height, 1 = the odd vertices sit on the line between their even neighbours.
// `distance` is landscapeMorphDistance; the vertex shader computes the same with the slab distance as a uniform.
float landscapeMorph(const LandscapeQuadtreeConfig& cfg, int level, float distance);

} // namespace HE
