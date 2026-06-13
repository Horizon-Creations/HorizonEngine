#include "HorizonRendering/RenderSorter.h"
#include <algorithm>

// Sort key: group by mesh asset (minimises GPU state changes), then
// front-to-back within a group (early-z friendliness).
void RenderSorter::sort(const RenderWorld&       world,
                        const std::vector<bool>& visible,
                        std::vector<uint32_t>&   outSortedIndices)
{
	outSortedIndices.clear();
	outSortedIndices.reserve(world.objects.size());
	for (uint32_t i = 0; i < world.objects.size(); ++i)
		if (i >= visible.size() || visible[i])
			outSortedIndices.push_back(i);

	const glm::vec3 camPos = world.camera.position;
	std::sort(outSortedIndices.begin(), outSortedIndices.end(),
		[&](uint32_t a, uint32_t b)
		{
			const RenderObject& oa = world.objects[a];
			const RenderObject& ob = world.objects[b];
			if (!(oa.meshAssetId == ob.meshAssetId))
			{
				if (oa.meshAssetId.hi != ob.meshAssetId.hi)
					return oa.meshAssetId.hi < ob.meshAssetId.hi;
				return oa.meshAssetId.lo < ob.meshAssetId.lo;
			}
			const float da = glm::dot(glm::vec3(oa.transform[3]) - camPos,
			                          glm::vec3(oa.transform[3]) - camPos);
			const float db = glm::dot(glm::vec3(ob.transform[3]) - camPos,
			                          glm::vec3(ob.transform[3]) - camPos);
			return da < db;
		});
}
