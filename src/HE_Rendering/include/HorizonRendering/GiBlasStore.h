// The software GI's per-mesh BLAS cache, shared by the compute-GI backends
// (OpenGL, D3D11, D3D12, Vulkan; Metal keeps one MTLAccelerationStructure per
// mesh and needs none of this). Every mesh's GiBvh lives in two concatenated
// arrays, the layout the kernels read: an instance carries its BLAS's node and
// triangle offsets, and a BVH's own indices are relative to them.
//
// Why a store instead of the old map + two vectors: a single rebuilt mesh used
// to drop the WHOLE cache, so the next frame rebuilt every BVH in the scene.
// That was rare (a sculpt step) until the terrain seam stitching (Thema 190)
// re-stitched chunk copies on every LOD change: flying over a 1025² landscape
// with GI (or, on GL, GI reflections) on cost 100-200 ms per frame, 120 → 20
// FPS (Thema 221). erase() now forgets one mesh; its range turns into dead
// space, and compactIfWasteful() squeezes that out once it outweighs the live
// data, by MOVING ranges (offsets are relative, nothing is rebuilt).
//
// Upload contract: between two rewrites the arrays only grow at the tail, so
// [0, uploadFromNodes()) / [0, uploadFromTris()) are already on the GPU,
// unchanged, and only the rest has to be written. After a compaction or
// clear() both are 0: every offset moved, rewrite everything (into a fresh
// buffer if a frame in flight may still read the old layout). markUploaded()
// once the backend has written it.
#pragma once

#include "../HE_RENDERING_API.h"
#include "GiBvh.h"

#include <Types/UUID.h>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace HE
{
struct GiBlasRange
{
	int32_t nodeOffset = 0, nodeCount = 0;
	int32_t triOffset  = 0, triCount  = 0;
	bool    valid      = false;   // false = the mesh has no geometry (cached as such)
};

class HE_RENDERING_API GiBlasStore
{
public:
	// The range cached for `id`, nullptr when it was never built (or erased).
	// An invalid range IS cached: a mesh without geometry is not retried every frame.
	const GiBlasRange* find(const UUID& id) const;
	// Caches `bvh` as `id`'s BLAS, appended at the tail (an invalid bvh caches an
	// invalid range). Replaces an existing entry, whose old range turns dead.
	GiBlasRange add(const UUID& id, const GiBvh& bvh);
	// Forgets `id`; false when it was not cached. Its range turns into dead space.
	bool erase(const UUID& id);
	// Moves the live ranges together once the dead space outweighs the live data
	// (and is worth a pass at all). true = it did: every offset changed, so the
	// next upload is a full rewrite and this frame's instances must be built after.
	bool compactIfWasteful();
	void clear();

	const std::vector<GiBvhNode>&     nodes() const { return m_nodes; }
	const std::vector<GiBvhTriangle>& tris()  const { return m_tris; }
	size_t entries()   const { return m_ranges.size(); }
	size_t deadNodes() const { return m_deadNodes; }
	size_t deadTris()  const { return m_deadTris; }

	bool   dirty() const
	{ return m_rewrite || m_nodesUploaded != m_nodes.size() || m_trisUploaded != m_tris.size(); }
	size_t uploadFromNodes() const { return m_rewrite ? 0 : m_nodesUploaded; }
	size_t uploadFromTris()  const { return m_rewrite ? 0 : m_trisUploaded; }
	void   markUploaded();

private:
	void forget(const GiBlasRange& r);

	std::unordered_map<UUID, GiBlasRange> m_ranges;
	std::vector<GiBvhNode>     m_nodes;
	std::vector<GiBvhTriangle> m_tris;
	size_t m_deadNodes = 0, m_deadTris = 0;
	size_t m_nodesUploaded = 0, m_trisUploaded = 0;
	bool   m_rewrite = false;
};
} // namespace HE
