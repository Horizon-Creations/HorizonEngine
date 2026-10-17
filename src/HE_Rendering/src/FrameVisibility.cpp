#include "HorizonRendering/FrameVisibility.h"
#include <Diagnostics/Profiler.h>
#include <algorithm>

namespace HE {

FrameVisibility::WorldKey FrameVisibility::makeKey(const RenderWorld& world, const Key& key)
{
	WorldKey k;
	k.key      = key;
	k.world    = &world;
	k.objects  = world.objects.data();
	k.count    = world.objects.size();
	k.camPos   = world.camera.position;
	k.viewProj = world.camera.projection * world.camera.view;
	return k;
}

bool FrameVisibility::sameKey(const WorldKey& a, const WorldKey& b)
{
	// The matrix compares exactly: a reuse at another aspect (the SSAO pass at half
	// resolution) rewrites the projection in place, and any change at all must miss.
	return a.key.walk == b.key.walk && a.key.boundsStamp == b.key.boundsStamp
	    && a.key.variant == b.key.variant
	    && a.world == b.world && a.objects == b.objects && a.count == b.count
	    && a.camPos == b.camPos && a.viewProj == b.viewProj;
}

bool FrameVisibility::findCamera(const RenderWorld& world, const Key& key,
                                 std::vector<uint8_t>& outVisible, std::vector<uint32_t>& outSorted)
{
	if (!m_cameraValid || !sameKey(m_cameraKey, makeKey(world, key))) return false;
	outVisible = m_cameraVisible;
	outSorted  = m_cameraSorted;
	++m_cameraReuses;
	return true;
}

void FrameVisibility::storeCamera(const RenderWorld& world, const Key& key,
                                  const std::vector<uint8_t>& visible, const std::vector<uint32_t>& sorted)
{
	m_cameraKey     = makeKey(world, key);
	m_cameraVisible = visible;
	m_cameraSorted  = sorted;
	m_cameraValid   = true;
	++m_cameraComputes;
}

bool FrameVisibility::cullAndSortCamera(const RenderWorld& world, const Key& key,
                                        std::vector<uint8_t>& outVisible, std::vector<uint32_t>& outSorted)
{
	if (findCamera(world, key, outVisible, outSorted)) return false;
	HE_PROFILE_SCOPE_N("FrameVisibility::camera");
	m_culler.cull(world, outVisible);
	m_sorter.sort(world, outVisible, outSorted);
	storeCamera(world, key, outVisible, outSorted);
	return true;
}

void FrameVisibility::shadowLayer(const RenderWorld& world, const Key& key, const glm::mat4& viewProj,
                                  uint32_t skipEntity, RenderSorter::DepthBatchList& out)
{
	HE_PROFILE_SCOPE_N("FrameVisibility::shadowLayer");
	// The views the pass renders this walk. The backends clamp the cascade count to at
	// least one and the local count to their own atlas; a layer outside this list just
	// takes the fallback below, so the list only has to be right, not complete.
	const ShadowData& sh = world.shadow;
	m_viewScratch.clear();
	if (sh.enabled)
		for (int c = 0; c < std::clamp(sh.cascadeCount, 1, ShadowData::kMaxCascades); ++c)
			m_viewScratch.push_back(sh.cascadeViewProj[c]);
	for (int v = 0; v < std::clamp(sh.localLayerCount, 0, ShadowData::kMaxLocalShadowLayers); ++v)
		m_viewScratch.push_back(sh.localViewProj[v]);

	// The presort depends on the walk's objects, the bounds the culls read, the camera
	// position (the sort key) and the views (the union); not on the camera projection.
	// The views are compared by value: the extractor's aspect tail refits the cascades in
	// place without a new walk, and a union from before would drop casters.
	const WorldKey k = makeKey(world, key);
	const bool same = m_shadowValid
	               && m_shadowKey.key.walk == k.key.walk && m_shadowKey.key.boundsStamp == k.key.boundsStamp
	               && m_shadowKey.world == k.world && m_shadowKey.objects == k.objects
	               && m_shadowKey.count == k.count && m_shadowKey.camPos == k.camPos
	               && m_shadowViews == m_viewScratch;
	if (!same)
	{
		HE_PROFILE_SCOPE_N("FrameVisibility::shadowPresort");
		m_shadowViews.swap(m_viewScratch);
		m_shadowViewVisible.resize(m_shadowViews.size());
		const size_t n = world.objects.size();
		m_shadowUnion.assign(n, 0u);
		uint8_t* const any = m_shadowUnion.data();
		for (size_t v = 0; v < m_shadowViews.size(); ++v)
		{
			m_culler.cull(world, m_shadowViews[v], m_shadowViewVisible[v]);
			// Raw pointers: a uint8_t store may alias the vectors themselves, and indexing
			// them reloads data() and size() per byte (1.5 ms for 11 layers at 50k).
			const uint8_t* const vis = m_shadowViewVisible[v].data();
			for (size_t i = 0; i < n; ++i) any[i] |= vis[i];
		}
		m_sorter.sortShadowCasters(world, m_shadowUnion, m_shadowPresorted);
		m_shadowKey   = k;
		m_shadowValid = true;
		++m_shadowPresorts;
	}

	const std::vector<uint8_t>* layerVisible = nullptr;
	for (size_t v = 0; v < m_shadowViews.size() && !layerVisible; ++v)
		if (m_shadowViews[v] == viewProj) layerVisible = &m_shadowViewVisible[v];
	if (layerVisible)
	{
		RenderSorter::keepVisible(m_shadowPresorted, *layerVisible, m_layerSorted);
	}
	else
	{
		// Not one of world.shadow's views: the old per-layer cull + sort, always right.
		m_culler.cull(world, viewProj, m_layerVisible);
		m_sorter.sort(world, m_layerVisible, m_layerSorted);
		++m_shadowFallbacks;
	}
	RenderSorter::batchDepthCasters(world, m_layerSorted, skipEntity, out);
	++m_shadowLayers;
}

} // namespace HE
