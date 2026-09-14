#include "ViewportPick.h"
#include "PreviewPick.h"                    // screenRay: the one unproject every pane shares
#include <HorizonRendering/RenderWorld.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TerrainChunkComponent.h>
#include <limits>

namespace ViewportPick
{

const HE::AABB& fallbackBox()
{
	static const HE::AABB box = []{
		HE::AABB b; b.expand({ -0.5f, -0.5f, -0.5f }); b.expand({ 0.5f, 0.5f, 0.5f }); return b;
	}();
	return box;
}

Entity pick(const RenderWorld& snapshot, entt::registry& reg, const BoxLookup& boxes,
            const glm::vec3& rayOrigin, const glm::vec3& rayDir)
{
	Entity meshHit    = entt::null; float meshDist    = std::numeric_limits<float>::max();
	Entity terrainHit = entt::null; float terrainDist = std::numeric_limits<float>::max();
	for (const RenderObject& obj : snapshot.objects)
	{
		const HE::AABB* local = (obj.meshAssetId != HE::UUID{} && boxes)
		                      ? boxes(obj.meshAssetId) : nullptr;
		const HE::AABB& box = local ? *local : fallbackBox();

		// Ray → object space (exact test for rotated objects). The direction
		// goes through as a VECTOR (w = 0), so `t` stays measured in units of
		// the world-space |rayDir| and is comparable between objects.
		const glm::mat4 invModel = glm::inverse(obj.transform);
		const glm::vec3 o = glm::vec3(invModel * glm::vec4(rayOrigin, 1.0f));
		const glm::vec3 d = glm::vec3(invModel * glm::vec4(rayDir,    0.0f));

		float t = 0.0f;
		if (!box.intersectRay(o, d, t)) continue;

		const Entity e = static_cast<Entity>(obj.entityId);
		Entity terrainOwner = entt::null;
		if (reg.valid(e))
		{
			if (auto* cc = reg.try_get<TerrainChunkComponent>(e)) terrainOwner = cc->terrain;
			else if (reg.all_of<TerrainComponent>(e))            terrainOwner = e;
		}

		if (terrainOwner != entt::null)
		{
			if (t < terrainDist) { terrainDist = t; terrainHit = terrainOwner; }
		}
		else if (t < meshDist)
		{
			meshDist = t; meshHit = e;
		}
	}
	return (meshHit != entt::null) ? meshHit : terrainHit;
}

Entity pickAtScreen(const RenderWorld& snapshot, entt::registry& reg, const BoxLookup& boxes,
                    const glm::mat4& viewProj, const glm::vec2& rectMin,
                    const glm::vec2& rectSize, const glm::vec2& mouse)
{
	glm::vec3 ro(0.0f), rd(0.0f);
	if (!PreviewPick::screenRay(viewProj, rectMin, rectSize, mouse, ro, rd)) return entt::null;
	return pick(snapshot, reg, boxes, ro, rd);
}

} // namespace ViewportPick
