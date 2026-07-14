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
// Load every asset of `type` the manager can currently discover and return the
// UUIDs: already-registered assets plus a loose-content walk (header sniff →
// loadAsset). The walk covers the editor and dev builds; pak-only assets are
// found only once registered (see the header's known limitation).
std::vector<HE::UUID> discoverAssets(ContentManager& cm, HE::AssetType type)
{
	std::vector<HE::UUID>        out = cm.enumerateIds(type);
	std::unordered_set<HE::UUID> seen(out.begin(), out.end());

	const std::string root = cm.contentRoot();
	std::error_code ec;
	if (root.empty() || !std::filesystem::is_directory(root, ec)) return out;

	std::filesystem::recursive_directory_iterator it(root, ec), end;
	for (; it != end; it.increment(ec))
	{
		if (ec) break;
		if (!it->is_regular_file(ec) || it->path().extension() != ".hasset") continue;
		HAsset::Reader r;
		if (!r.open(it->path().string()) ||
		    r.assetType() != static_cast<uint16_t>(type)) continue;
		const std::string rel =
			std::filesystem::relative(it->path(), root, ec).generic_string();
		const HE::UUID id = cm.loadAsset(rel);
		if (!(id == HE::UUID{}) && seen.insert(id).second) out.push_back(id);
	}
	return out;
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
		if (auto compiled = HorizonCode::compiledClasses().create(a->path))
			inst = runtime.addCompiled(std::move(compiled));
		else
		{
			HorizonCode::Graph g;
			if (!a->graphJson.empty()) HorizonCode::fromJson(a->graphJson, g);
			inst = runtime.add(std::move(g));
		}
		runtime.fireEvent(inst, "Construct", 0);
		runtime.fireEvent(inst, "BeginPlay", 0);
		m_players.push_back(inst);
	}

	if (!m_players.empty())
		Logger::Log(Logger::LogLevel::Info,
			("PlayerHost: spawned " + std::to_string(m_players.size()) +
			 " player instance(s), " + std::to_string(m_actions.size()) +
			 " action(s), " + std::to_string(bound) + " binding entrie(s)").c_str());
}

void PlayerHost::tick(const Input& input, float dt)
{
	if (!m_runtime) return;
	m_mapping.tick(input);

	for (const HorizonCode::InstanceId inst : m_players)
		m_runtime->fireEvent(inst, "Tick", 0, HorizonCode::Value::ofFloat(dt));

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
