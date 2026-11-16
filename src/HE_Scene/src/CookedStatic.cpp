#include "HorizonScene/CookedStatic.h"
#include "HorizonScene/CookAnalysis.h"
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include <algorithm>

namespace HE
{

void CookedStatic::syncContent(const void* manager, uint64_t epoch)
{
	if (manager == content && epoch == contentEpoch) return;
	content      = manager;
	contentEpoch = epoch;
	std::fill(state.begin(), state.end(), State::Pending);
}

CookedStatic& cookedStaticOf(entt::registry& reg)
{
	if (CookedStatic* c = reg.ctx().find<CookedStatic>()) return *c;
	return reg.ctx().emplace<CookedStatic>();
}

void refreshCookedStatic(entt::registry& reg, uint64_t structureEpoch)
{
	CookedStatic& c = cookedStaticOf(reg);
	if (c.structureEpoch == structureEpoch) return;
	c.structureEpoch = structureEpoch;

	const size_t slots = reg.storage<entt::entity>().size();
	std::vector<uint8_t> cookable(slots, 0);
	for (const CookEntry& ce : analyzeCookability(reg))
		if (ce.verdict.cookable()) cookable[static_cast<size_t>(entt::to_entity(ce.entity))] = 1;

	c.rowOfIndex.assign(slots, -1);
	c.entity.clear();

	const auto meshes = reg.view<TransformComponent, MeshComponent>();
	c.entity.reserve(meshes.size_hint());
	for (const entt::entity e : meshes)
	{
		const size_t i = static_cast<size_t>(entt::to_entity(e));
		if (!cookable[i]) continue;
		c.rowOfIndex[i] = static_cast<int32_t>(c.entity.size());
		c.entity.push_back(e);
	}
	const size_t rows = c.entity.size();
	// Rows are filled when the walk settles them; sized here so the extract never allocates.
	c.state.assign(rows, CookedStatic::State::Pending);
	c.world.assign(rows, glm::mat4(1.0f));
	c.worldBounds.assign(rows, HE::AABB{});
	c.meshId.assign(rows, HE::UUID{});
	c.materialId.assign(rows, HE::UUID{});
	c.lod.assign(rows, 0);
	c.castsShadow.assign(rows, 1);
	c.receivesShadow.assign(rows, 1);
}

}
