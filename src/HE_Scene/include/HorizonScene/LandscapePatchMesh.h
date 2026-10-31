#pragma once
#include <cstdint>
#include <vector>

namespace HE
{

// ── The one grid every landscape patch is drawn with (Thema 326, step 2, part A) ──
// (cells + 1)^2 vertices, addressed by their grid position (i, j) in 0..cells; a vertex shader turns that into a world
// position from the patch's origin and size (x = x0 + i / cells * size) and fetches the height there. One vertex buffer
// and one index buffer serve every patch of every level on all backends; nothing is built per patch.
//
// The index buffer holds the four quadrants one after the other, so a patch that is drawn only in part (the quadrant
// bitmask of LandscapePatch) draws the ranges of the quadrants it has: quadrantFirst[q] / quadrantCount[q] in indices.
// A whole node is one draw of [0, indices.size()) because the quadrants follow each other.
//
// Triangulation and morph belong together. Every quad is split along the same diagonal, from (i, j) to (i + 1, j + 1)
// ("\" seen with x to the right and z down). The vertex shader's morph (landscapeMorphSource below) moves each vertex
// with an odd index onto the line between its even neighbours; with that diagonal, every fine triangle then lies in the
// plane of one coarse triangle, so a fully morphed patch is exactly the patch of the next level.
struct LandscapePatchMesh
{
	int                   cells = 64;
	std::vector<uint16_t> indices;          // triangles whose winding gives a +y normal (x right, z toward the bottom of a top view)
	uint32_t              quadrantFirst[4] = { 0, 0, 0, 0 };   // q = dx + 2 * dz
	uint32_t              quadrantCount[4] = { 0, 0, 0, 0 };

	int vertexCount() const { return (cells + 1) * (cells + 1); }
	// `cells` must be even and at most 255 (uint16 indices and the vertex position in bytes).
	static LandscapePatchMesh build(int cells = 64);
};

// Which vertices the morph blends between: a vertex (i, j) with no odd index keeps its height; odd i, even j sits at the
// middle of (i - 1, j) and (i + 1, j); even i, odd j at the middle of (i, j - 1) and (i, j + 1); both odd on the diagonal of
// the triangulation: the middle of (i - 1, j - 1) and (i + 1, j + 1). `n` is 0 for no morph, else 2 and a/b are the two sources.
struct LandscapeMorphSource
{
	int n = 0;
	int aI = 0, aJ = 0, bI = 0, bJ = 0;
};
LandscapeMorphSource landscapeMorphSource(int i, int j);

} // namespace HE
