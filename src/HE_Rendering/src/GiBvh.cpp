#include <HorizonRendering/GiBvh.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace HE
{
namespace
{
struct BuildTri
{
	glm::vec3 v0, v1, v2;
	glm::vec3 centroid;
	glm::vec3 bmin, bmax;
};

void growNodeBounds(GiBvhNode& node, const std::vector<BuildTri>& tris,
                    const std::vector<uint32_t>& order, int first, int count)
{
	glm::vec3 bmin(std::numeric_limits<float>::max());
	glm::vec3 bmax(-std::numeric_limits<float>::max());
	for (int i = first; i < first + count; ++i)
	{
		const BuildTri& t = tris[order[static_cast<size_t>(i)]];
		bmin = glm::min(bmin, t.bmin);
		bmax = glm::max(bmax, t.bmax);
	}
	node.boundsMin = bmin;
	node.boundsMax = bmax;
}

// Recursive median split over `order[first..first+count)`. Nodes are appended
// depth-first with both children adjacent (left = leftFirst, right = +1), the
// layout giBvhIntersect() and the GLSL kernel expect.
void subdivide(std::vector<GiBvhNode>& nodes, uint32_t nodeIdx,
               const std::vector<BuildTri>& tris, std::vector<uint32_t>& order,
               int leafSize)
{
	GiBvhNode& node = nodes[nodeIdx];
	if (node.triCount <= leafSize) return; // small enough → leaf

	// Longest centroid-extent axis; a degenerate spread (all centroids equal)
	// cannot be split meaningfully → keep as (possibly oversized) leaf.
	glm::vec3 cmin(std::numeric_limits<float>::max());
	glm::vec3 cmax(-std::numeric_limits<float>::max());
	for (int i = node.leftFirst; i < node.leftFirst + node.triCount; ++i)
	{
		const glm::vec3& c = tris[order[static_cast<size_t>(i)]].centroid;
		cmin = glm::min(cmin, c);
		cmax = glm::max(cmax, c);
	}
	const glm::vec3 extent = cmax - cmin;
	int axis = 0;
	if (extent.y > extent.x) axis = 1;
	if (extent.z > extent[static_cast<glm::length_t>(axis)]) axis = 2;
	if (extent[static_cast<glm::length_t>(axis)] <= 1e-12f) return;

	const int first = node.leftFirst, count = node.triCount;
	const int mid   = count / 2;
	std::nth_element(order.begin() + first, order.begin() + first + mid,
	                 order.begin() + first + count,
	                 [&](uint32_t a, uint32_t b)
	                 { return tris[a].centroid[static_cast<glm::length_t>(axis)]
	                        < tris[b].centroid[static_cast<glm::length_t>(axis)]; });

	const uint32_t leftIdx = static_cast<uint32_t>(nodes.size());
	// Repurpose this node as internal: children adjacent, triCount = 0.
	nodes[nodeIdx].leftFirst = static_cast<int32_t>(leftIdx);
	nodes[nodeIdx].triCount  = 0;

	GiBvhNode left{};  left.leftFirst  = first;       left.triCount  = mid;
	GiBvhNode right{}; right.leftFirst = first + mid; right.triCount = count - mid;
	nodes.push_back(left);
	nodes.push_back(right);
	growNodeBounds(nodes[leftIdx],     tris, order, first,       mid);
	growNodeBounds(nodes[leftIdx + 1], tris, order, first + mid, count - mid);

	subdivide(nodes, leftIdx,     tris, order, leafSize);
	subdivide(nodes, leftIdx + 1, tris, order, leafSize);
}
} // namespace

GiBvh buildGiBvh(const float* positions, size_t vertexCount, size_t strideFloats,
                 const uint32_t* indices, size_t indexCount, int leafSize)
{
	GiBvh bvh;
	if (!positions || !indices || vertexCount == 0 || indexCount < 3) return bvh;
	leafSize = std::max(1, leafSize);

	const size_t triCount = indexCount / 3;
	std::vector<BuildTri> tris;
	tris.reserve(triCount);
	for (size_t t = 0; t < triCount; ++t)
	{
		const uint32_t i0 = indices[t * 3 + 0], i1 = indices[t * 3 + 1], i2 = indices[t * 3 + 2];
		if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) continue; // corrupt index → skip
		BuildTri bt;
		bt.v0 = glm::vec3(positions[i0 * strideFloats], positions[i0 * strideFloats + 1], positions[i0 * strideFloats + 2]);
		bt.v1 = glm::vec3(positions[i1 * strideFloats], positions[i1 * strideFloats + 1], positions[i1 * strideFloats + 2]);
		bt.v2 = glm::vec3(positions[i2 * strideFloats], positions[i2 * strideFloats + 1], positions[i2 * strideFloats + 2]);
		bt.centroid = (bt.v0 + bt.v1 + bt.v2) * (1.0f / 3.0f);
		bt.bmin = glm::min(bt.v0, glm::min(bt.v1, bt.v2));
		bt.bmax = glm::max(bt.v0, glm::max(bt.v1, bt.v2));
		tris.push_back(bt);
	}
	if (tris.empty()) return bvh;

	std::vector<uint32_t> order(tris.size());
	for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;

	GiBvhNode root{};
	root.leftFirst = 0;
	root.triCount  = static_cast<int32_t>(tris.size());
	bvh.nodes.push_back(root);
	growNodeBounds(bvh.nodes[0], tris, order, 0, root.triCount);
	subdivide(bvh.nodes, 0, tris, order, leafSize);

	// De-index into leaf order: leaves reference [leftFirst, leftFirst+triCount)
	// directly in this array — no second indirection in the kernel.
	bvh.triangles.reserve(tris.size());
	for (uint32_t src : order)
	{
		GiBvhTriangle out;
		out.v0 = glm::vec4(tris[src].v0, 0.0f);
		out.v1 = glm::vec4(tris[src].v1, 0.0f);
		out.v2 = glm::vec4(tris[src].v2, 0.0f);
		bvh.triangles.push_back(out);
	}
	return bvh;
}

namespace
{
// Slab test — identical formulation to the GLSL kernel (inf-safe: IEEE inf
// from 1/0 component dirs resolves correctly through min/max).
inline bool aabbHit(const glm::vec3& bmin, const glm::vec3& bmax,
                    const glm::vec3& origin, const glm::vec3& invDir,
                    float tMin, float tMax)
{
	const glm::vec3 t0 = (bmin - origin) * invDir;
	const glm::vec3 t1 = (bmax - origin) * invDir;
	const glm::vec3 lo = glm::min(t0, t1);
	const glm::vec3 hi = glm::max(t0, t1);
	const float tNear = std::max(std::max(lo.x, lo.y), std::max(lo.z, tMin));
	const float tFar  = std::min(std::min(hi.x, hi.y), std::min(hi.z, tMax));
	return tNear <= tFar;
}

// Möller-Trumbore, both faces (occluders are two-sided for shadow/GI rays —
// matches Metal, which builds BLASes without any culling flags).
inline bool triHit(const GiBvhTriangle& tri, const glm::vec3& origin, const glm::vec3& dir,
                   float tMin, float tMax, float& tOut)
{
	const glm::vec3 e1 = glm::vec3(tri.v1) - glm::vec3(tri.v0);
	const glm::vec3 e2 = glm::vec3(tri.v2) - glm::vec3(tri.v0);
	const glm::vec3 p  = glm::cross(dir, e2);
	const float det    = glm::dot(e1, p);
	if (std::fabs(det) < 1e-9f) return false; // parallel
	const float invDet = 1.0f / det;
	const glm::vec3 s  = origin - glm::vec3(tri.v0);
	const float u = glm::dot(s, p) * invDet;
	if (u < 0.0f || u > 1.0f) return false;
	const glm::vec3 q = glm::cross(s, e1);
	const float v = glm::dot(dir, q) * invDet;
	if (v < 0.0f || u + v > 1.0f) return false;
	const float t = glm::dot(e2, q) * invDet;
	if (t <= tMin || t >= tMax) return false;
	tOut = t;
	return true;
}
} // namespace

bool giBvhIntersect(const GiBvh& bvh, const glm::vec3& origin, const glm::vec3& dir,
                    float tMin, float tMax, bool anyHit, float& tOut, uint32_t& triOut)
{
	if (!bvh.valid()) return false;
	const glm::vec3 invDir(1.0f / dir.x, 1.0f / dir.y, 1.0f / dir.z);

	// Fixed-depth stack, same bound the GLSL kernel uses. Median-split depth is
	// ~log2(N)+leaf slack; 64 covers far beyond any real mesh in this engine.
	uint32_t stack[64];
	int      sp = 0;
	stack[sp++] = 0;

	bool  hit   = false;
	float bestT = tMax;

	while (sp > 0)
	{
		const GiBvhNode& node = bvh.nodes[stack[--sp]];
		if (!aabbHit(node.boundsMin, node.boundsMax, origin, invDir, tMin, bestT))
			continue;
		if (node.triCount > 0) // leaf
		{
			for (int i = 0; i < node.triCount; ++i)
			{
				const uint32_t triIdx = static_cast<uint32_t>(node.leftFirst + i);
				float t;
				if (triHit(bvh.triangles[triIdx], origin, dir, tMin, bestT, t))
				{
					hit    = true;
					bestT  = t;
					tOut   = t;
					triOut = triIdx;
					if (anyHit) return true;
				}
			}
		}
		else if (sp + 2 <= static_cast<int>(sizeof(stack) / sizeof(stack[0])))
		{
			stack[sp++] = static_cast<uint32_t>(node.leftFirst);
			stack[sp++] = static_cast<uint32_t>(node.leftFirst + 1);
		}
	}
	return hit;
}
} // namespace HE
