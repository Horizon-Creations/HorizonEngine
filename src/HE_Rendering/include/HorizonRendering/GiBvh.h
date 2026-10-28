// CPU-built BVH for the compute-shader GI backends (OpenGL 4.3+ on Windows/
// Linux — macOS GL is capped at 4.1/no compute and never takes this path).
// This is the software counterpart of Metal's MTLAccelerationStructure: the
// BLAS (per-mesh triangle BVH) is built here on the CPU and uploaded to SSBOs;
// the GLSL compute kernels traverse it with the exact algorithm mirrored by
// giBvhIntersect() below, so the unit tests validate the same logic the shader
// runs (the shader itself can only be offline-validated on this machine).
// The TLAS analogue stays renderer-side: a flat instance array (world/inverse
// transform + node/tri offsets), traversed linearly per ray in v1.
#pragma once

#include "../HE_RENDERING_API.h"

#include <Types/UUID.h>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <glm/glm.hpp>

namespace HE
{
// GPU-facing node, std430-compatible (32 bytes, two vec4-sized rows). Internal
// nodes: triCount == 0, children are adjacent at leftFirst / leftFirst + 1.
// Leaves: triCount > 0, triangles [leftFirst, leftFirst + triCount).
struct GiBvhNode
{
	glm::vec3 boundsMin{0.0f}; int32_t leftFirst = 0;
	glm::vec3 boundsMax{0.0f}; int32_t triCount  = 0;
};
static_assert(sizeof(GiBvhNode) == 32, "GiBvhNode must stay std430-compatible (2 x 16 bytes)");

// One triangle, de-indexed at build time into leaf order (no index indirection
// in the kernel's inner loop). vec4 rows keep std430 alignment trivial; .w is
// padding.
struct GiBvhTriangle
{
	glm::vec4 v0{0.0f}, v1{0.0f}, v2{0.0f};
};
static_assert(sizeof(GiBvhTriangle) == 48, "GiBvhTriangle must stay std430-compatible (3 x 16 bytes)");

struct GiBvh
{
	std::vector<GiBvhNode>     nodes;     // nodes[0] = root; empty = no geometry
	std::vector<GiBvhTriangle> triangles; // permuted into leaf order
	bool valid() const { return !nodes.empty(); }
};

// Builds a median-split BVH (longest centroid axis, leafSize cap). `positions`
// points at the first vertex's x; `strideFloats` is the float distance between
// consecutive vertices — 8 for the engine's interleaved pos3+normal3+uv2
// layout (position at offset 0, matching Metal's BLAS vertex descriptor), 3
// for tightly packed test data. Degenerate input (no triangles) → invalid BVH.
// `sourceTriangles`, when given, receives for every triangle in leaf order the number of its triangle in the
// index buffer (indexBuffer[3n .. 3n+2]); refitGiBvh() needs it to redo the vertices of the same topology.
HE_RENDERING_API GiBvh buildGiBvh(const float* positions, size_t vertexCount, size_t strideFloats,
                 const uint32_t* indices, size_t indexCount, int leafSize = 4,
                 std::vector<uint32_t>* sourceTriangles = nullptr);

// Refit: the SAME tree (`nodes`, from an earlier buildGiBvh of a mesh with this very index buffer) over moved
// vertices. Triangles are re-read in the old leaf order and every node's bounds are recomputed bottom-up, no
// split is redone — about a tenth of the cost of a build. The tree gets looser the further the vertices move
// from where it was built, so callers rebuild now and then (GiBvhRefitCache does). false = the input does not
// fit the tree (an index out of range, a triangle number past the buffer); `out` is untouched then.
HE_RENDERING_API bool refitGiBvh(const std::vector<GiBvhNode>& nodes, const std::vector<uint32_t>& sourceTriangles,
                 const float* positions, size_t vertexCount, size_t strideFloats,
                 const uint32_t* indices, size_t indexCount, GiBvh& out);

// A mesh that is rebuilt over and over with the same triangles and different vertices — a re-stitched terrain
// chunk, a sculpted one — only needs its first rebuild to be a build. The cache remembers the tree of meshes
// that were built a second time (first-time builds are the vast majority and are not worth the memory), checks
// the index buffer's hash, and refits instead of rebuilding while it matches. One per renderer, main thread only.
class HE_RENDERING_API GiBvhRefitCache
{
public:
	GiBvh build(const UUID& meshId, const float* positions, size_t vertexCount, size_t strideFloats,
	            const uint32_t* indices, size_t indexCount, int leafSize = 4);
	void   clear() { m_entries.clear(); m_seen.clear(); }
	size_t refits()   const { return m_refits; }     // builds answered by a refit (tests, counters)
	size_t rebuilds() const { return m_rebuilds; }   // builds answered by a full build

private:
	struct Entry
	{
		uint64_t              topologyKey = 0;
		std::vector<GiBvhNode>  nodes;
		std::vector<uint32_t> sourceTriangles;
		int                   refits  = 0;   // since the last full build
		uint64_t              lastUse = 0;
	};
	std::unordered_map<UUID, Entry> m_entries;
	std::unordered_set<UUID>        m_seen;      // built once already: its next build is worth remembering
	uint64_t m_tick = 0;
	size_t   m_refits = 0, m_rebuilds = 0;
};

// CPU reference traversal, mirrored 1:1 by the GLSL compute kernels (same slab
// test, same Möller-Trumbore, same stack bound). Returns whether any triangle
// is hit with t in (tMin, tMax). anyHit == true → first accepted hit wins
// (occlusion/shadow rays); anyHit == false → nearest hit, tOut/triOut set to
// the closest intersection.
HE_RENDERING_API bool giBvhIntersect(const GiBvh& bvh, const glm::vec3& origin, const glm::vec3& dir,
                    float tMin, float tMax, bool anyHit, float& tOut, uint32_t& triOut);
} // namespace HE
