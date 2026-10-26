#include "HorizonRendering/FrameCull.h"

void FrameCull::prepareShadowViews(FrustumCuller& culler, const RenderWorld& world, uint64_t walk,
                                   ShadowViewCpu* views, size_t n, const char* label) const
{
	const bool spans = RenderSorter::instanceSpansEnabled();
	for (size_t v = 0; v < n; ++v) views[v].batches.useSpans = spans;
	auto prepare = [&](size_t v)
	{
		ShadowViewCpu& sv = views[v];
		cull(culler, world, walk, sv.viewProj, sv.visible);
		sv.sorter.sortByMesh(world, sv.visible, sv.sorted);
		RenderSorter::batchDepthCasters(world, sv.sorted, sv.skip, sv.batches);
	};
	if (n > 1 && RenderSorter::parallelShadowViewsEnabled()) parallel_for(n, prepare, label, 1);
	else for (size_t v = 0; v < n; ++v) prepare(v);
}

HE::AABB FrameCull::sceneBox(const RenderWorld& world, uint64_t walk)
{
	if (!holds(world, walk))
	{
		HE::AABB box;
		for (const RenderObject& o : world.objects) box.expand(o.worldBounds);
		return box;
	}
	if (!m_sceneBoxValid)
	{
		m_sceneBox = HE::AABB{};
		for (const HE::AABB& b : m_bounds) m_sceneBox.expand(b);
		m_sceneBoxValid = true;
	}
	return m_sceneBox;
}

void FrameCull::cullCamera(FrustumCuller& culler, RenderSorter& sorter, const RenderWorld& world,
                           uint64_t walk, uint64_t frame, uint32_t variant, std::vector<uint8_t>& visible,
                           std::vector<uint32_t>& sorted)
{
	const glm::mat4 vp = world.camera.projection * world.camera.view;
	const bool refined = holds(world, walk);
	CameraAnswer& c = m_camera;
	if (refined && c.stamp == frame && c.variant == variant && c.viewProj == vp
	    && c.camPos == world.camera.position)
	{
		visible = c.visible;
		sorted  = c.sorted;
		return;
	}
	cull(culler, world, walk, vp, visible);
	sorter.sort(world, visible, sorted);
	if (!refined) { c.stamp = ~0ull; return; }
	c.stamp = frame; c.variant = variant; c.viewProj = vp; c.camPos = world.camera.position;
	c.visible = visible;
	c.sorted  = sorted;
}
