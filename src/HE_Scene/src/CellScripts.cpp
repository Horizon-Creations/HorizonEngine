#include "HorizonScene/CellScripts.h"
#include "HorizonScene/EngineApi.h"
#include "HorizonScene/EntityHost.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/ScriptContext.h"
#include "HorizonScene/Components/HierarchyComponent.h"
#include <HorizonCode/HorizonCodeRuntime.h>

namespace HE
{

int startCellScripts(const CellScriptHosts& hosts, HorizonWorld& world, const std::vector<entt::entity>& created)
{
	const entt::registry& reg = world.registry();
	// Only what runs nothing yet. EntityHost::bind does not ask whether the entity
	// is bound already, and startScriptsFor writes over the map entry of one that
	// is: either would run the script twice and leak the first instance.
	std::vector<entt::entity> fresh;
	fresh.reserve(created.size());
	for (entt::entity e : created)
	{
		if (!reg.valid(e)) continue;
		if (hosts.entities && hosts.entities->instanceOf(e) != 0) continue;
		if (hosts.textInstances && hosts.textInstances->count(static_cast<uint32_t>(e))) continue;
		fresh.push_back(e);
	}
	if (fresh.empty()) return 0;
	// Both kinds, like a zone: missing the second is how a cell's Entity classes
	// would silently never run.
	int started = 0;
	if (hosts.entities) started += hosts.entities->bindFor(fresh);
	if (hosts.text && hosts.textInstances && hosts.content)
		started += hosts.text->startScriptsFor(fresh, *hosts.content, *hosts.textInstances);
	return started;
}

int stopCellScripts(const CellScriptHosts& hosts, HorizonWorld& world, entt::entity root)
{
	const entt::registry& reg = world.registry();
	if (!reg.valid(root)) return 0;
	// The subtree first, as it stands, then the scripts: Destruct is graph code and
	// may destroy or move entities of the cell while the instances go.
	std::vector<entt::entity> under;
	std::vector<entt::entity> stack{ root };
	while (!stack.empty())
	{
		const entt::entity e = stack.back();
		stack.pop_back();
		under.push_back(e);
		if (const auto* h = reg.try_get<HierarchyComponent>(e))
			for (entt::entity c : h->children)
				if (reg.valid(c)) stack.push_back(c);
	}

	int stopped = 0;
	if (hosts.entities)
	{
		std::vector<HorizonCode::InstanceId> classes;
		for (entt::entity e : under)
			if (const HorizonCode::InstanceId inst = hosts.entities->instanceOf(e)) classes.push_back(inst);
		for (const HorizonCode::InstanceId inst : classes) hosts.entities->unbind(inst);   // fires Destruct
		stopped += static_cast<int>(classes.size());
	}
	if (hosts.textInstances)
		for (entt::entity e : under)
		{
			const uint32_t id = static_cast<uint32_t>(e);
			const auto it = hosts.textInstances->find(id);
			if (it == hosts.textInstances->end()) continue;
			if (hosts.text) hosts.text->destroyInstance(it->second);
			hosts.textInstances->erase(it);
			// What it subscribed to (horizon.hc.watch) goes with it.
			if (hosts.runtime) hosts.runtime->unwatch(api::hc::scriptToken(id));
			++stopped;
		}
	return stopped;
}

} // namespace HE
