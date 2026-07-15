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

#include <cstddef>
#include <cstdint>
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
HE_RENDERING_API GiBvh buildGiBvh(const float* positions, size_t vertexCount, size_t strideFloats,
                 const uint32_t* indices, size_t indexCount, int leafSize = 4);

// CPU reference traversal, mirrored 1:1 by the GLSL compute kernels (same slab
// test, same Möller-Trumbore, same stack bound). Returns whether any triangle
// is hit with t in (tMin, tMax). anyHit == true → first accepted hit wins
// (occlusion/shadow rays); anyHit == false → nearest hit, tOut/triOut set to
// the closest intersection.
HE_RENDERING_API bool giBvhIntersect(const GiBvh& bvh, const glm::vec3& origin, const glm::vec3& dir,
                    float tMin, float tMax, bool anyHit, float& tOut, uint32_t& triOut);
} // namespace HE
