#include "HorizonScene/EntityActive.h"
#include "HorizonScene/Components/InactiveComponent.h"
#include "HorizonScene/Components/HierarchyComponent.h"

namespace HE
{

bool isEntityActiveSelf(const entt::registry& reg, entt::entity e)
{
	return reg.valid(e) && !reg.all_of<InactiveComponent>(e);
}

bool isEntityActive(const entt::registry& reg, entt::entity e)
{
	if (!reg.valid(e)) return false;
	// The hierarchy is a tree (reparentEntity refuses cycles), so the walk
	// ends at the root; a guard against a corrupt parent chain still costs
	// nothing on the normal path.
	int hops = 0;
	for (entt::entity cur = e; cur != entt::null && reg.valid(cur) && hops < 256; ++hops)
	{
		if (reg.all_of<InactiveComponent>(cur)) return false;
		const auto* h = reg.try_get<HierarchyComponent>(cur);
		cur = h ? h->parent : entt::null;
	}
	return true;
}

bool anyEntityInactive(const entt::registry& reg)
{
	// The const overload hands back null rather than creating the pool — the
	// pool exists anyway (HorizonWorld::reserveComponentStorage), but a
	// registry that is not a HorizonWorld's must not gain one from a question.
	const auto* pool = reg.storage<InactiveComponent>();
	return pool && !pool->empty();
}

void setEntityActive(entt::registry& reg, entt::entity e, bool active)
{
	if (!reg.valid(e)) return;
	if (active) reg.remove<InactiveComponent>(e);
	else        reg.emplace_or_replace<InactiveComponent>(e);
}

} // namespace HE
