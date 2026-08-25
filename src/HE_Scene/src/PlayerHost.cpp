#include "HorizonScene/PlayerHost.h"
#include <cstdint>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HcCompiledLoader.h>   // HorizonCode::compiledClasses()
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/HAsset.h>
#include <HorizonCode/HcClassResolve.h>
#include <HorizonScene/EngineApi.h>         // HE::api::player (the possession table)
#include <HorizonScene/EntityHost.h>        // the already-bound level characters
#include <Application/InputAssets.h>
#include <Diagnostics/Logger.h>
#include <algorithm>
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

void PlayerHost::begin(HorizonCode::Runtime& runtime, ContentManager& cm,
                       const EntityHost* entities)
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
			m_actions.push_back({ HE::inputActionNameFromPath(a->path), k,
			                      HE::inputActionRunsWhilePaused(a->json) });
		}

	// Bindings: union of every mapping context in the project.
	size_t bound = 0;
	for (const HE::UUID id : discoverAssets(cm, HE::AssetType::InputMappingContext))
		if (const InputMappingContextAsset* m = cm.getInputMappingContext(id))
			bound += HE::applyInputMappingContext(m_mapping, m->json);

	// Player classes: one instance per PlayerController asset. Characters are
	// only COUNTED — see the "What is NOT spawned here" note in the header.
	size_t characterClasses = 0;
	for (const HE::UUID id : discoverAssets(cm, HE::AssetType::HorizonCodeClass))
	{
		const HorizonCodeClassAsset* a = cm.getHorizonCodeClass(id);
		if (!a) continue;
		// The RESOLVED engine base, not the raw string: a class deriving from
		// another class that is a PlayerController is one too, and asking the
		// asset alone would miss every derived player in the project.
		HorizonCode::ResolvedClass rc = HorizonCode::resolveClassAsset(cm, a->path);
		if (HorizonCode::engineClassIsA(rc.engineBase, "PlayerCharacter"))
		{
			++characterClasses;
			continue;
		}
		if (!HorizonCode::engineClassIsA(rc.engineBase, "PlayerController")) continue;

		// A controller is not something you place in a level, so it stays a bare
		// instance with no entity of its own.
		//
		// Compiled class first (same per-asset hybrid as createObject), keyed by
		// the content-relative asset path; miss → the interpreted graph. Both
		// branches pass the identity from the ASSET rather than letting the
		// compiled one report its own: the asset is the authority on which base
		// class it derives from, and a generated library that predates a
		// baseClass edit would otherwise disagree with the editor.
		const HorizonCode::ClassIdentity cls{ a->path, rc.engineBase, rc.chain };
		HorizonCode::InstanceId inst = 0;
		if (auto compiled = HorizonCode::compiledClasses().create(a->path))
			inst = runtime.addCompiled(std::move(compiled), {}, cls);
		else
			inst = runtime.addLevels(std::move(rc.levels), {}, cls);
		if (!inst) continue;

		m_controllers.push_back(inst);
	}

	// Every controller is registered BEFORE any of them runs. A controller's
	// BeginPlay is now where the game spawns and possesses its character, so it
	// is graph code that asks player.controller() / player.possessed() — and it
	// must not get an answer that depends on which controller happens to start
	// first. That is why lifecycle firing is a second pass and not part of the
	// loop above.
	HE::api::player::setControllers(m_controllers);

	// The characters that were already in the level, bound by EntityHost::begin
	// before this host started. They are picked up BEFORE any BeginPlay runs, so
	// a controller that possesses a placed character in its BeginPlay finds the
	// list already complete — and so does the no-controller fallback, which is
	// the only thing that reads it. Asking the runtime for the base class rather
	// than re-resolving the asset: the instance already carries the identity the
	// asset gave it, and that identity is what possession and Cast agree on.
	if (entities)
		for (const auto& [entityId, inst] : entities->instances())
			if (HorizonCode::engineClassIsA(runtime.baseClassOf(inst), "PlayerCharacter"))
				addCharacter(inst);

	for (const HorizonCode::InstanceId inst : m_controllers)
	{
		runtime.fireConstruct(inst);
		runtime.fireBeginPlay(inst);
	}

	if (!m_controllers.empty() || characterClasses > 0)
		HE_LOG_INFO(Input, "%s",
			("PlayerHost: spawned " + std::to_string(m_controllers.size()) + " controller(s), " +
			 std::to_string(m_actions.size()) + " action(s), " +
			 std::to_string(bound) + " binding entrie(s)").c_str());
	// The one line that saves the half hour of "why is my character not there":
	// nothing is missing, nobody asked for it.
	if (characterClasses > 0)
		HE_LOG_INFO(Input, "%s",
			(std::to_string(characterClasses) + " PlayerCharacter classes found, none spawned "
			 "automatically - spawn one from your PlayerController with Create Object").c_str());
}

void PlayerHost::addCharacter(HorizonCode::InstanceId instance)
{
	if (instance == 0) return;
	// Reap on the way in. A project WITH controllers never reaches the fallback
	// in fireInputEvent, so pruning only there would let this list grow for every
	// character the level ever spawned — and characters are spawned and destroyed
	// all game long now. The runtime may not be up yet: an entity class's
	// BeginPlay can Create Object before begin() ever runs.
	if (m_runtime)
		std::erase_if(m_characters,
		              [this](HorizonCode::InstanceId i){ return !m_runtime->alive(i); });
	// A class spawned twice is two characters, but the SAME instance registered
	// twice would be fired at twice per event.
	if (std::find(m_characters.begin(), m_characters.end(), instance) != m_characters.end()) return;
	m_characters.push_back(instance);
}

void PlayerHost::fireInputEvent(const std::string& event, const HorizonCode::Value& arg)
{
	// No controller in the project: the pre-possession behaviour, so a project
	// whose characters handle their own input keeps working unchanged.
	if (m_controllers.empty())
	{
		// Reap first. This list is fed by begin()'s scan and by addCharacter(),
		// and nothing else ever removes from it — a level that spawns and
		// destroys characters would otherwise grow it forever and fire at dead
		// instances. Guarded like addCharacter's copy: the two must not disagree
		// about whether a runtime is required to prune.
		if (m_runtime)
			std::erase_if(m_characters,
			              [this](HorizonCode::InstanceId i){ return !m_runtime->alive(i); });
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

	// Tick still fires while paused — with dt 0, so anything integrating against
	// it stands still. That is what lets a controller keep driving a pause menu
	// without a second, pause-only tick channel.
	for (const HorizonCode::InstanceId inst : m_controllers)
		m_runtime->fireTick(inst, dt);

	// A pause silences input by DEFAULT: without that the player keeps shooting
	// through the pause menu, because the mapping below never stopped reading
	// the keyboard. The mapping is still ticked (above) either way — dropping
	// its tick would make a key held across the pause look like a fresh press on
	// resume. Events that fall in a pause are dropped, never queued.
	const bool paused = HE::api::time::isPaused();

	// UI-only routing silences gameplay input for the same reason a pause does,
	// and lets the same actions through: the one the author marked "run while
	// paused". Without that exception the key that opened a menu could not close
	// it, because in UI-only mode nothing else reaches the controller. Both
	// conditions ask "is the game being played right now", so they share the
	// author's answer rather than growing a second flag that can contradict it.
	const bool uiOnly = HE::api::input::mode() == HE::api::input::Mode::UIOnly;
	const bool silenced = paused || uiOnly;

	for (const ActionInfo& a : m_actions)
	{
		if (silenced && !a.runWhilePaused) continue;
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
		for (const HorizonCode::InstanceId inst : m_controllers)
			m_runtime->destroy(inst); // fires "Destruct"
	m_controllers.clear();
	m_characters.clear();
	m_actions.clear();
	m_mapping.clear();
	m_runtime = nullptr;
}
