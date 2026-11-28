#pragma once

// The native near view of the Runtime Virtual Texture (IRenderer::SetRvtNearChunks): per landscape entity, the chunk
// entities that keep the landscape's own material although the swap to the consumer is on. Every backend holds one and asks
// it in its swap loop, so the rule lives here once.

#include <HorizonRendering/RenderObject.h>

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace HE
{
struct RvtNearChunks
{
	std::unordered_map<uint32_t, std::unordered_set<uint32_t>> byLandscape;

	// Replaces the set of one landscape; empty = every chunk of it swaps.
	void set(uint32_t terrainEntity, const std::vector<uint32_t>& chunkEntities)
	{
		if (chunkEntities.empty()) byLandscape.erase(terrainEntity);
		else                       byLandscape[terrainEntity] = std::unordered_set<uint32_t>(chunkEntities.begin(), chunkEntities.end());
	}
	// The chunk keeps its own material (the swap loop skips it).
	bool keeps(const RenderObject& o) const
	{
		const auto it = byLandscape.find(o.terrainEntity);
		return it != byLandscape.end() && it->second.count(o.entityId) != 0;
	}
};
} // namespace HE
