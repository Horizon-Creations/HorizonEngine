#include "HorizonScene/PlayerHost.h"
#include <cstdint>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HcCompiledLoader.h>   // HorizonCode::compiledClasses()
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/HAsset.h>
#include <HorizonCode/HcClassResolve.h>
#include <HorizonScene/EntityHost.h>
#include <HorizonScene/EngineApi.h>         // HE::api::player (the possession table)
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

void PlayerHost::begin(HorizonCode::Runtime& runtime, ContentManager& cm, EntityHost* entities)
{
	end();
	m_runtime = &runtime;

	// Actions: logical name (asset stem — what mappings and events key on) + kind.
	for (const HE::UUID id : discoverAssets(cm, HE::AssetType::InputAction))
		if (const InputActionAsset* a = cm.getInputAction(id))
		{
			const ActionKind k = HE::inputActionIsAxis2D(a->json) ? ActionKind::Axis2D
			                   : HE::inputActionIsAxis(a->json)   ? ActionKind::Axis
			                                                      : ActionKind::Button;
			m_actions.push_back({ HE::inputActionNameFromPath(a->path), k });
		}

	// Bindings: union of every mapping context in the project.
	size_t bound = 0;
	for (const HE::UUID id : discoverAssets(cm, HE::AssetType::InputMappingContext))
		if (const InputMappingContextAsset* m = cm.getInputMappingContext(id))
			bound += HE::applyInputMappingContext(m_mapping, m->json);

	// Player classes: one instance per PlayerController/PlayerCharacter asset.
	for (const HE::UUID id : discoverAssets(cm, HE::AssetType::HorizonCodeClass))
	{
		const HorizonCodeClassAsset* a = cm.getHorizonCodeClass(id);
		if (!a) continue;
		// The RESOLVED engine base, not the raw string: a class deriving from
		// another class that is a PlayerController is one too, and asking the
		// asset alone would miss every derived player in the project.
		HorizonCode::ResolvedClass rc = HorizonCode::resolveClassAsset(cm, a->path);
		const bool isController = HorizonCode::engineClassIsA(rc.engineBase, "PlayerController");
		const bool isCharacter  = HorizonCode::engineClassIsA(rc.engineBase, "PlayerCharacter");
		if (!isController && !isCharacter) continue;

		HorizonCode::InstanceId inst = 0;
		// A character gets a scene entity — it is an Entity class, and a player
		// without a body cannot be moved, collided with or rendered. A controller
		// is not something you place in a level, so it stays a bare instance.
		bool owned = true;
		if (isCharacter && entities)
		{
			inst  = entities->spawn(a->path).instance;
			owned = false;   // that host ticks and destroys it
		}
		else
		{
			// Compiled class first (same per-asset hybrid as createObject), keyed
			// by the content-relative asset path; miss → the interpreted graph.
			// Both branches pass the identity from the ASSET rather than letting
			// the compiled one report its own: the asset is the authority on
			// which base class it derives from, and a generated library that
			// predates a baseClass edit would otherwise disagree with the editor.
			const HorizonCode::ClassIdentity cls{ a->path, rc.engineBase, rc.chain };
			if (auto compiled = HorizonCode::compiledClasses().create(a->path))
				inst = runtime.addCompiled(std::move(compiled), {}, cls);
			else
				inst = runtime.addLevels(std::move(rc.levels), {}, cls);
			runtime.fireConstruct(inst);
			runtime.fireBeginPlay(inst);
		}
		if (!inst) continue;

		if (owned) m_owned.push_back(inst);
		(isController ? m_controllers : m_characters).push_back(inst);
	}

	HE::api::player::setControllers(m_controllers);
	// Auto-possess the unambiguous case, so the common single-player project
	// needs no wiring at all. Anything else is the game's decision and belongs
	// in the controller's BeginPlay (player.possess) — guessing there would be
	// worse than doing nothing.
	if (m_controllers.size() == 1 && m_characters.size() == 1)
		HE::api::player::possess(m_controllers.front(), m_characters.front());

	if (!m_controllers.empty() || !m_characters.empty())
		HE_LOG_INFO(Input, "%s",
			("PlayerHost: spawned " + std::to_string(m_controllers.size()) + " controller(s) and " +
			 std::to_string(m_characters.size()) + " character(s), " +
			 std::to_string(m_actions.size()) + " action(s), " +
			 std::to_string(bound) + " binding entrie(s)").c_str());
}

void PlayerHost::fireInputEvent(const std::string& event, const HorizonCode::Value& arg)
{
	// No controller in the project: the pre-possession behaviour, so a project
	// whose characters handle their own input keeps working unchanged.
	if (m_controllers.empty())
	{
		for (const HorizonCode::InstanceId inst : m_characters)
			m_runtime->fireEvent(inst, event, 0, arg);
		return;
	}
	for (const HorizonCode::InstanceId ctrl : m_controllers)
	{
		// The controller ALWAYS gets it — possessing a character makes the
		// controller forward input, it does not make the controller passive.
		m_runtime->fireEvent(ctrl, event, 0, arg);
		const HorizonCode::InstanceId pawn = HE::api::player::possessed(ctrl);
		if (pawn != 0 && m_runtime->alive(pawn))
			m_runtime->fireEvent(pawn, event, 0, arg);
	}
}

void PlayerHost::tick(const Input& input, float dt, const MouseFrame& mouse)
{
	if (!m_runtime) return;
	m_mapping.tick(input, mouse);

	for (const HorizonCode::InstanceId inst : m_owned)
		m_runtime->fireTick(inst, dt);

	for (const ActionInfo& a : m_actions)
	{
		switch (a.kind)
		{
		case ActionKind::Axis:
			// Per-frame, like the Tick event — graphs use it as their movement
			// pump. The value is NOT scaled by dt here: a key axis is a held
			// state the graph integrates itself, and a mouse-sourced one is
			// already a displacement. Doing it here would be wrong for both.
			fireInputEvent(HE::inputEventAxis(a.name),
			               HorizonCode::Value::ofFloat(m_mapping.axisValue(a.name)));
			break;
		case ActionKind::Axis2D:
		{
			float x = 0.0f, y = 0.0f;
			m_mapping.axis2DValue(a.name, x, y);
			fireInputEvent(HE::inputEventAxis2D(a.name),
			               HorizonCode::Value::ofVec2(glm::vec2(x, y)));
			break;
		}
		case ActionKind::Button:
			if (m_mapping.justPressed(a.name))
				fireInputEvent(HE::inputEventPressed(a.name), {});
			if (m_mapping.justReleased(a.name))
				fireInputEvent(HE::inputEventReleased(a.name), {});
			break;
		}
	}
}

void PlayerHost::end()
{
	HE::api::player::clear();
	if (m_runtime)
		for (const HorizonCode::InstanceId inst : m_owned)
			m_runtime->destroy(inst); // fires "Destruct"
	m_owned.clear();
	m_controllers.clear();
	m_characters.clear();
	m_actions.clear();
	m_mapping.clear();
	m_runtime = nullptr;
}
