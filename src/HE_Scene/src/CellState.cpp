#include "HorizonScene/CellState.h"
#include "HorizonScene/EngineApi.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/EntityIdComponent.h"
#include "HorizonScene/Components/HierarchyComponent.h"
#include <Diagnostics/Log.h>

namespace HE
{

std::string CellState::restoreText(const HE::UUID& id) const
{
	if (const auto s = m_states.find(id); s != m_states.end()) return s->second;
	// Not seen go this session: what the active save holds for it, if anything —
	// what a script's entity.applySavedState would have put on it at the load of the
	// game, had the cell been there then. Under the save's own gates.
	if (!api::save::inPlayMode() || api::save::activeId().empty()) return {};
	return api::save::entityState(std::to_string(id.hi) + ":" + std::to_string(id.lo));
}

void CellState::sliceBuilt(api::Ctx& ctx, entt::entity root, const std::vector<entt::entity>& created,
                           bool stableIds)
{
	if (!ctx.world) return;
	if (!stableIds)
	{
		if (!m_warnedFreshIds)
		{
			HE_LOG_WARN(World, "%s", "Cell state: a cell without stable ids (no version-2 head) keeps nothing "
			                         "of what changed in it over an unload; split the scene again to fix it");
			m_warnedFreshIds = true;
		}
		return;
	}
	entt::registry& reg = ctx.world->registry();
	std::vector<HE::UUID>& built = m_built[static_cast<uint32_t>(root)];
	for (entt::entity e : created)
	{
		// The root is the cell, not a thing in it; the later slices hang under it.
		if (e == root || !reg.valid(e)) continue;
		const auto* idc = reg.try_get<EntityIdComponent>(e);
		if (!idc) continue;
		built.push_back(idc->id);
		if (const std::string text = restoreText(idc->id); !text.empty())
			api::entity::applyState(ctx, static_cast<api::Entity>(e), text, api::entity::kStateWorld);
	}
}

void CellState::cellBuilt(api::Ctx& ctx, entt::entity root)
{
	const auto it = m_built.find(static_cast<uint32_t>(root));
	if (!ctx.world || it == m_built.end()) return;
	if (m_states.empty() && (!api::save::inPlayMode() || api::save::activeId().empty())) return;
	for (const HE::UUID& id : it->second)
	{
		const std::string text = restoreText(id);
		if (text.empty()) continue;
		const entt::entity e = ctx.world->findByEntityId(id);
		if (e == entt::null) continue;
		api::entity::applyState(ctx, static_cast<api::Entity>(e), text, api::entity::kStateScript);
	}
}

void CellState::cellUnloading(api::Ctx& ctx, entt::entity root)
{
	const auto it = m_built.find(static_cast<uint32_t>(root));
	if (it == m_built.end()) return;
	if (!ctx.world || !ctx.world->registry().valid(root))
	{
		m_built.erase(it);
		return;
	}
	entt::registry& reg = ctx.world->registry();
	// What is under the root now, by id. Not "is the id anywhere in the world": a
	// thing hung out of the cell survives the cell's destruction, and building it
	// again would put a second one into the world.
	std::unordered_map<HE::UUID, entt::entity> under;
	under.reserve(it->second.size());
	std::vector<entt::entity> stack{ root };
	while (!stack.empty())
	{
		const entt::entity e = stack.back();
		stack.pop_back();
		if (e != root)
			if (const auto* idc = reg.try_get<EntityIdComponent>(e)) under.emplace(idc->id, e);
		if (const auto* h = reg.try_get<HierarchyComponent>(e))
			for (entt::entity c : h->children)
				if (reg.valid(c)) stack.push_back(c);
	}
	for (const HE::UUID& id : it->second)
	{
		const auto u = under.find(id);
		if (u == under.end())
		{
			m_tombstones.insert(id);
			m_states.erase(id);
			continue;
		}
		std::string state = api::entity::stateOf(ctx, static_cast<api::Entity>(u->second));
		if (!state.empty()) m_states[id] = std::move(state);
	}
	m_built.erase(it);
}

void CellState::forgetCell(entt::entity root)
{
	m_built.erase(static_cast<uint32_t>(root));
}

void CellState::reset()
{
	m_built.clear();
	m_tombstones.clear();
	m_states.clear();
	m_warnedFreshIds = false;
}

const std::string* CellState::stateOf(const HE::UUID& id) const
{
	const auto it = m_states.find(id);
	return it != m_states.end() ? &it->second : nullptr;
}

} // namespace HE
