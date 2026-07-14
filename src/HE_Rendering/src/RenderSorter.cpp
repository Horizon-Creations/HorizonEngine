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
