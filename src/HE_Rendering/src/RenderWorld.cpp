#include "HorizonRendering/RenderWorld.h"
#include <cstring>
#include <mutex>
#include <unordered_map>

void RenderWorld::clear()
{
	objects.clear();
	instanceBlocks.clear();   // the clusters in `objects` index into it: the two live and die together
	skinnedObjects.clear();
	lights.clear();
	decals.clear();
	uiObjects.clear();
	particleBatches.clear();
	ribbonBatches.clear();
	landscapes.clear();
	camera = CameraData{};
	shadow = ShadowData{};
	sunDirection = glm::vec3(0.45f, 0.80f, 0.55f);
	ambient      = glm::vec3(0.03f, 0.035f, 0.05f);
}

// See the header for the full "why" — this pick is load-bearing for GI shadows,
// the probe bounce and every backend's material sun uniform.
bool RenderWorld::dominantDirectionalLight(glm::vec3& towardOut,
                                           glm::vec3& colorIntensityOut) const
{
	const LightData* best = nullptr;
	for (const LightData& l : lights)
		if (l.type == 0 && l.intensity > 0.0f && (!best || l.intensity > best->intensity))
			best = &l;
	if (!best || glm::dot(best->direction, best->direction) < 1e-8f)
	{
		towardOut         = glm::normalize(sunDirection);
		colorIntensityOut = glm::vec3(0.0f);
		return false;
	}
	towardOut         = -glm::normalize(best->direction); // LightData.direction = light travel direction
	colorIntensityOut = best->color * best->intensity;
	return true;
}

// ── Instance world matrices, once per block ─────────────────────────────────
// Keyed on what decides the matrices: the storage (pointer, held), the range and the
// parent's bits. A new scatter is new storage, a moved terrain a new parent: both are
// new keys, and the old entry ages out.
namespace
{
	struct WorldKey
	{
		const void* storage;
		uint32_t    first, count;
		glm::mat4   parent;
		bool operator==(const WorldKey& o) const
		{
			return storage == o.storage && first == o.first && count == o.count
			    && std::memcmp(&parent, &o.parent, sizeof(parent)) == 0;
		}
	};
	struct WorldKeyHash
	{
		size_t operator()(const WorldKey& k) const
		{
			uint64_t h = reinterpret_cast<uintptr_t>(k.storage) * 0x9E3779B97F4A7C15ull;
			h ^= (static_cast<uint64_t>(k.first) << 32 | k.count) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
			const uint32_t* w = reinterpret_cast<const uint32_t*>(&k.parent);
			for (int i = 12; i < 15; ++i) h ^= w[i] + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);   // the translation
			return static_cast<size_t>(h);
		}
	};
	struct WorldEntry
	{
		std::shared_ptr<const std::vector<glm::mat4>> pin;   // keeps `storage` from being reused
		std::vector<glm::mat4>                        world;
		uint64_t                                      lastFrame = 0;
	};
	struct WorldCache
	{
		std::mutex mutex;
		std::unordered_map<WorldKey, WorldEntry, WorldKeyHash> entries;   // node-based: `world` never moves
		uint64_t frame = 0;
	};
	WorldCache& worldCache()
	{
		static WorldCache c;
		return c;
	}
	constexpr uint64_t kWorldCacheKeepFrames = 8;
}

const glm::mat4* instanceWorldMatrices(const InstanceBlock& b)
{
	if (b.kind == InstanceBlock::Parent::Identity) return b.matrices->data() + b.first;
	WorldCache& c = worldCache();
	const WorldKey key{ b.matrices.get(), b.first, b.count, b.parent };
	std::lock_guard<std::mutex> lock(c.mutex);
	WorldEntry& e = c.entries[key];
	e.lastFrame = c.frame;
	if (!e.pin)
	{
		e.pin = b.matrices;
		e.world.resize(b.count);
		glm::mat4* dst = e.world.data();
		const uint32_t n = b.count;
		auto fill = [&b, dst](size_t i) { dst[i] = b.world(static_cast<uint32_t>(i)); };
		if (n >= RenderWorld::kParallelUnfoldMin && RenderWorld::parallelUnfoldEnabled())
			parallel_for(n, fill, "InstanceWorldCache", 1024);
		else
			for (uint32_t i = 0; i < n; ++i) fill(i);
	}
	return e.world.data();
}

void instanceWorldCacheNextFrame()
{
	WorldCache& c = worldCache();
	std::lock_guard<std::mutex> lock(c.mutex);
	++c.frame;
	for (auto it = c.entries.begin(); it != c.entries.end();)
		if (c.frame - it->second.lastFrame > kWorldCacheKeepFrames) it = c.entries.erase(it);
		else ++it;
}

size_t instanceWorldCacheEntries()
{
	WorldCache& c = worldCache();
	std::lock_guard<std::mutex> lock(c.mutex);
	return c.entries.size();
}
