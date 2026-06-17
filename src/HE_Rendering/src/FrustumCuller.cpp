#include "HorizonRendering/FrustumCuller.h"
#include <Diagnostics/Profiler.h>
#include <JobSystem/JobSystem.h>

Frustum Frustum::fromViewProj(const glm::mat4& vp)
{
	// Rows of the transposed matrix combine into the clip planes
	const glm::vec4 r0(vp[0][0], vp[1][0], vp[2][0], vp[3][0]);
	const glm::vec4 r1(vp[0][1], vp[1][1], vp[2][1], vp[3][1]);
	const glm::vec4 r2(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
	const glm::vec4 r3(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);

	Frustum f;
	f.planes[0] = r3 + r0; // left
	f.planes[1] = r3 - r0; // right
	f.planes[2] = r3 + r1; // bottom
	f.planes[3] = r3 - r1; // top
	f.planes[4] = r3 + r2; // near
	f.planes[5] = r3 - r2; // far

	for (auto& p : f.planes)
	{
		const float len = glm::length(glm::vec3(p));
		if (len > 1e-9f) p /= len;
	}
	return f;
}

bool Frustum::intersects(const HE::AABB& box) const
{
	const glm::vec3 center  = box.center();
	const glm::vec3 extents = box.extents();

	for (const glm::vec4& p : planes)
	{
		const glm::vec3 n(p);
		// Projected box radius onto the plane normal
		const float r = extents.x * std::abs(n.x)
		              + extents.y * std::abs(n.y)
		              + extents.z * std::abs(n.z);
		if (glm::dot(n, center) + p.w < -r)
			return false; // fully behind one plane
	}
	return true;
}

void FrustumCuller::cull(const RenderWorld& world, std::vector<uint8_t>& outVisible)
{
	cull(world, world.camera.projection * world.camera.view, outVisible);
}

void FrustumCuller::cull(const RenderWorld& world, const glm::mat4& viewProj,
                         std::vector<uint8_t>& outVisible)
{
	HE_PROFILE_SCOPE_N("FrustumCuller::cull");
	const Frustum frustum = Frustum::fromViewProj(viewProj);
	const size_t  count   = world.objects.size();

	outVisible.assign(count, 1u);
	parallel_for(count, [&](size_t i) {
		const HE::AABB& bounds = world.objects[i].worldBounds;
		if (bounds.isValid())
			outVisible[i] = frustum.intersects(bounds) ? 1u : 0u;
	});
}
