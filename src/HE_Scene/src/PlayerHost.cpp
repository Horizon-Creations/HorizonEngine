#include "HorizonScene/PlayerHost.h"
#include <cstdint>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HcCompiledLoader.h>   // HorizonCode::compiledClasses()
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/HAsset.h>
#include <Application/InputAssets.h>
#include <Diagnostics/Logger.h>
#include <filesystem>
#include <unordered_set>

namespace
{
// Every asset of `type` the manager can currently discover — moved into
// ContentManager::discoverAssets (the TypeRegistry refresh shares it); this
// thin wrapper keeps the call sites below unchanged.
std::vector<HE::UUID> discoverAssets(ContentManager& cm, HE::AssetType type)
{
	return cm.discoverAssets(type);
}
} // namespace

void PlayerHost::begin(HorizonCode::Runtime& runtime, ContentManager& cm)
{
	end();
	m_runtime = &runtime;

	// Actions: logical name (asset stem — what mappings and events key on) + kind.
	for (const HE::UUID id : discoverAssets(cm, HE::AssetType::InputAction))
		if (const InputActionAsset* a = cm.getInputAction(id))
			m_actions.push_back({ HE::inputActionNameFromPath(a->path),
			                      HE::inputActionIsAxis(a->json) });

	// Bindings: union of every mapping context in the project.
	size_t bound = 0;
	for (const HE::UUID id : discoverAssets(cm, HE::AssetType::InputMappingContext))
		if (const InputMappingContextAsset* m = cm.getInputMappingContext(id))
			bound += HE::applyInputMappingContext(m_mapping, m->json);

	// Player classes: one instance per PlayerController/PlayerCharacter asset.
	for (const HE::UUID id : discoverAssets(cm, HE::AssetType::HorizonCodeClass))
	{
		const HorizonCodeClassAsset* a = cm.getHorizonCodeClass(id);
		if (!a || (a->baseClass != "PlayerController" && a->baseClass != "PlayerCharacter"))
			continue;

		HorizonCode::InstanceId inst = 0;
		// Compiled class first (same per-asset hybrid as createObject), keyed by
		// the content-relative asset path; miss → the interpreted graph.
		// Both branches pass the identity from the ASSET rather than letting the
		// compiled one report its own: the asset is the authority on which base
		// class it derives from, and a generated library that predates a
		// baseClass edit would otherwise disagree with the editor.
		const HorizonCode::ClassIdentity cls{ a->path, a->baseClass };
		if (auto compiled = HorizonCode::compiledClasses().create(a->path))
			inst = runtime.addCompiled(std::move(compiled), {}, cls);
		else
		{
			HorizonCode::Graph g;
			if (!a->graphJson.empty()) HorizonCode::fromJson(a->graphJson, g);
			inst = runtime.add(std::move(g), {}, cls);
		}
		runtime.fireConstruct(inst);
		runtime.fireBeginPlay(inst);
		m_players.push_back(inst);
	}

	if (!m_players.empty())
		HE_LOG_INFO(Input, "%s",
			("PlayerHost: spawned " + std::to_string(m_players.size()) +
			 " player instance(s), " + std::to_string(m_actions.size()) +
			 " action(s), " + std::to_string(bound) + " binding entrie(s)").c_str());
}

void PlayerHost::tick(const Input& input, float dt)
{
	if (!m_runtime) return;
	m_mapping.tick(input);

	for (const HorizonCode::InstanceId inst : m_players)
		m_runtime->fireTick(inst, dt);

	for (const ActionInfo& a : m_actions)
	{
		if (a.isAxis)
		{
			// Per-frame, like the Tick event — graphs use it as their movement pump.
			const float v = m_mapping.axisValue(a.name);
			const std::string ev = HE::inputEventAxis(a.name);
			for (const HorizonCode::InstanceId inst : m_players)
				m_runtime->fireEvent(inst, ev, 0, HorizonCode::Value::ofFloat(v));
		}
		else
		{
			if (m_mapping.justPressed(a.name))
			{
				const std::string ev = HE::inputEventPressed(a.name);
				for (const HorizonCode::InstanceId inst : m_players)
					m_runtime->fireEvent(inst, ev, 0);
			}
			if (m_mapping.justReleased(a.name))
			{
				const std::string ev = HE::inputEventReleased(a.name);
				for (const HorizonCode::InstanceId inst : m_players)
					m_runtime->fireEvent(inst, ev, 0);
			}
		}
	}
}

void PlayerHost::end()
{
	if (m_runtime)
		for (const HorizonCode::InstanceId inst : m_players)
			m_runtime->destroy(inst); // fires "Destruct"
	m_players.clear();
	m_actions.clear();
	m_mapping.clear();
	m_runtime = nullptr;
}
