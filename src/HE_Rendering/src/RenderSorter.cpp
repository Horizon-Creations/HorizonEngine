#include "HorizonRendering/RenderSorter.h"
#include <cstdint>
#include <algorithm>

// Sort key: group by mesh asset (minimises GPU state changes), then
// front-to-back within a group (early-z friendliness). The distance and mesh
// id are computed once per object into a key array, so the std::sort comparator
// — invoked O(n log n) times — only compares cheap precomputed scalars instead
// of re-extracting matrix columns and recomputing squared distances each call.
void RenderSorter::sort(const RenderWorld&          world,
                        const std::vector<uint8_t>& visible,
                        std::vector<uint32_t>&       outSortedIndices)
{
	const glm::vec3 camPos = world.camera.position;

	m_keys.clear();
	m_keys.reserve(world.objects.size());
	for (uint32_t i = 0; i < world.objects.size(); ++i)
	{
		if (i < visible.size() && !visible[i])
			continue;
		const RenderObject& o = world.objects[i];
		const glm::vec3     d = glm::vec3(o.transform[3]) - camPos;
		m_keys.push_back(SortKey{ o.meshAssetId.hi, o.meshAssetId.lo,
		                          glm::dot(d, d), i });
	}

	std::sort(m_keys.begin(), m_keys.end(),
		[](const SortKey& a, const SortKey& b)
		{
			if (a.meshHi != b.meshHi) return a.meshHi < b.meshHi;
			if (a.meshLo != b.meshLo) return a.meshLo < b.meshLo;
			return a.distSq < b.distSq;
		});

	outSortedIndices.clear();
	outSortedIndices.reserve(m_keys.size());
	for (const SortKey& k : m_keys)
		outSortedIndices.push_back(k.index);
}

void RenderSorter::partitionByOpacity(const std::vector<DrawCall>&  drawCalls,
                                      std::vector<const DrawCall*>& outOpaque,
                                      std::vector<const DrawCall*>& outTransparent,
                                      bool                          sectionAware)
{
	outOpaque.clear();
	outTransparent.clear();
	outOpaque.reserve(drawCalls.size());
	for (const DrawCall& dc : drawCalls)
	{
		// GL and Metal classify inline and honour DrawCall::indexOffset/
		// indexCount; the backends collecting through here (D3D11, D3D12,
		// Vulkan) do too and pass sectionAware. The default guards a caller that
		// draws the whole index buffer per DrawCall: for it a multi-section
		// mesh's second, third… draw would repaint the entire mesh in another
		// material, so it gets slot 0 alone — one draw, the mesh's own material,
		// exactly where they were before sections existed. A one-section mesh
		// never carries sectionIndex > 0, so nothing changes for it either way.
		if (!sectionAware && dc.sectionIndex > 0) continue;
		(isTransparent(dc) ? outTransparent : outOpaque).push_back(&dc);
	}
}

void RenderSorter::sortBackToFront(std::vector<const DrawCall*>& transparent,
                                   const glm::vec3&              camPos)
{
	std::sort(transparent.begin(), transparent.end(),
		[&camPos](const DrawCall* a, const DrawCall* b)
		{
			return backToFrontKey(a->transform, camPos)
			     > backToFrontKey(b->transform, camPos);
		});
}

void RenderSorter::batchDepthRuns(const RenderWorld&           world,
                                  const std::vector<uint32_t>& sortedIndices,
                                  DepthFilter                  filter,
                                  uint32_t                     skipEntity,
                                  DepthBatchList&              out)
{
	out.clear();
	out.transforms.reserve(sortedIndices.size());
	for (uint32_t idx : sortedIndices)
	{
		if (idx >= world.objects.size()) continue;
		const RenderObject& obj = world.objects[idx];
		// Billboards (precip/particles) opt out of both depth maps and AO.
		if (filter == DepthFilter::ShadowCasters  && !obj.castsShadow)   continue;
		if (filter == DepthFilter::AoContributors && !obj.contributesAO) continue;
		if (obj.entityId == skipEntity) continue; // the light's own mesh
		// Extend the current run when the mesh matches; the sorter grouped by
		// mesh id, so a change here means a genuinely new mesh.
		if (!out.batches.empty() && out.batches.back().meshAssetId == obj.meshAssetId)
			++out.batches.back().count;
		else
			out.batches.push_back(DepthBatch{ obj.meshAssetId,
			                                  static_cast<uint32_t>(out.transforms.size()), 1u });
		out.transforms.push_back(obj.transform);
	}
}
