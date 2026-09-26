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
#include <HorizonScene/ScriptContext.h>     // the Lua/Python instances the events also reach
#include <Application/InputAssets.h>
#include <Diagnostics/Logger.h>
#include <algorithm>
#include <filesystem>
#include <type_traits>
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

	// Bindings: the union of every mapping context in the project. An action
	// named in two contexts keeps the bindings of both (a duplicate counts once),
	// rather than whichever context happened to be applied last — and "last"
	// was never defined: discoverAssets hands back an unordered_map first and a
	// directory walk after it. The contexts are applied sorted by path, so the
	// order of the merged bindings (what a binding UI lists, and what a shape
	// conflict between two contexts falls back to) is the same on every machine.
	// Path and JSON are COPIED out before sorting: the getter's pointer is into
	// the asset pool, which the next load may move.
	std::vector<std::pair<std::string, std::string>> contexts;
	for (const HE::UUID id : discoverAssets(cm, HE::AssetType::InputMappingContext))
		if (const InputMappingContextAsset* m = cm.getInputMappingContext(id))
			contexts.emplace_back(m->path, m->json);
	std::sort(contexts.begin(), contexts.end(),
	          [](const auto& a, const auto& b) { return a.first < b.first; });
	size_t bound = 0;
	for (auto& [path, json] : contexts)
	{
		bound += HE::applyInputMappingContext(m_mapping, json);
		m_contextPaths.push_back(std::move(path));
	}

	// The player's own bindings, over the contexts (see "Rebinding" in the
	// header). The base is kept apart so a rebind or a reset can rebuild from
	// it without going back to the content manager.
	m_baseMapping = m_mapping;
	{
		HE::api::Ctx ctx;
		const std::string saved = HE::api::prefs::getString(ctx, kBindingsPrefsKey, "");
		if (!saved.empty() && m_overrides.fromJson(saved) && !m_overrides.empty())
		{
			m_overrides.applyTo(m_mapping);
			HE_LOG_INFO(Input, "%s", ("PlayerHost: " + std::to_string(m_overrides.size()) +
			                          " action(s) with the player's own bindings").c_str());
		}
	}
	HE::api::input::setBindingService({
		[this](const std::string& a, const std::string& d) { return rebindBegin(a, d); },
		[this]()                                            { rebindCancel(); },
		[this]()                                            { return isRebinding(); },
		[this]()                                            { return rebindConflict(); },
		[this](const std::string& a, const std::string& d) { return bindingName(a, d); },
		[this]()                                            { resetBindings(); },
		[this]()                                            { return saveBindings(); } });
	m_bindingServiceInstalled = true;

	// Player classes: one instance per PlayerController asset. Characters are
	// only COUNTED — see the "What is NOT spawned here" note in the header.
	size_t characterClasses = 0;
	for (const HE::UUID id : discoverAssets(cm, HE::AssetType::HorizonCodeClass))
	{
		const HorizonCodeClassAsset* a = cm.getHorizonCodeClass(id);
		if (!a) continue;
		// Copied before the resolve below, because the resolve LOADS: every
		// ancestor of this class goes through the content manager, and its asset
		// pool is a dense vector that moves everything in it when it grows. `a`
		// is dead from the next line on, and the string it owns with it — which
		// matters most here, where it is the argument being passed.
		const std::string assetPath = a->path;
		// The RESOLVED engine base, not the raw string: a class deriving from
		// another class that is a PlayerController is one too, and asking the
		// asset alone would miss every derived player in the project.
		HorizonCode::ResolvedClass rc = HorizonCode::resolveClassAsset(cm, assetPath);
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
		const HorizonCode::ClassIdentity cls{ assetPath, rc.engineBase, rc.chain };
		HorizonCode::InstanceId inst = 0;
		if (auto compiled = HorizonCode::compiledClasses().create(assetPath))
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
	// Neither a controller nor a character class anywhere: the session starts
	// with no player in it and, until this line existed, said nothing at all.
	// Everything a beginner does next depends on there being one, so the silence
	// was the single most expensive one in the engine — you press Play, nothing
	// moves, and the log is empty.
	//
	// A menu or cutscene scene legitimately has no player, which is why this
	// says what is missing rather than claiming something is broken. It fires
	// once per play session.
	// A Lua or Python project has neither and is not broken: its entity
	// scripts receive the same actions through onInputPressed and friends. So
	// this says what is missing for a HorizonCode player, and what the other
	// languages have instead, rather than declaring the session deaf.
	if (m_controllers.empty() && characterClasses == 0)
		HE_LOG_WARN(Input, "%s",
			"PlayerHost: no PlayerController and no PlayerCharacter class in this project - "
			"no HorizonCode player will respond to input (Lua/Python entity scripts still "
			"receive onInputPressed/onInputReleased/onInputAxis). For a HorizonCode player "
			"create them in the Content Browser under Gameplay, then spawn the character "
			"from the controller's Begin Play with Create Object and take it over with Possess");
}

void PlayerHost::setTextScripts(ScriptContext* scripts, const TextScriptInstances* instances)
{
	// The alias in the header has to BE ScriptContext::InstanceMap, or the
	// pointer the apps hand over would point at a map of some other shape.
	static_assert(std::is_same_v<TextScriptInstances, ScriptContext::InstanceMap>,
	              "PlayerHost::TextScriptInstances must match ScriptContext::InstanceMap");
	m_scripts         = scripts;
	m_scriptInstances = instances;
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

void PlayerHost::tick(const Input& input, float dt, const MouseFrame& mouse, bool locked)
{
	if (!m_runtime) return;

	// The capture first, on the raw device state: the mapping below is exactly
	// what a menu has silenced. A capture that finishes this frame keeps the
	// frame silent too (wasRebinding) — its release is not a gameplay event.
	const bool wasRebinding = m_capture.busy();
	switch (m_capture.update(input, mouse))
	{
	case HE::BindingCapture::Result::Captured:
	{
		const ActionBinding b = m_capture.captured();
		const std::vector<std::string> others = HE::bindingConflicts(m_mapping, m_rebindAction, b);
		m_rebindConflict.clear();
		for (const std::string& o : others)
			m_rebindConflict += (m_rebindConflict.empty() ? "" : ", ") + o;
		m_overrides.set(m_rebindAction, m_capture.device(), { b });
		m_mappingDirty = true;
		HE_LOG_INFO(Input, "%s", ("Rebound " + m_rebindAction + " to " +
		                          HE::bindingDisplayName(b)).c_str());
		if (!others.empty())
			HE_LOG_WARN(Input, "%s", (HE::bindingDisplayName(b) + " is now bound to " +
			                          m_rebindAction + " and also to " + m_rebindConflict).c_str());
		break;
	}
	case HE::BindingCapture::Result::Cancelled:
		HE_LOG_INFO(Input, "%s", ("Rebinding " + m_rebindAction + " cancelled").c_str());
		break;
	case HE::BindingCapture::Result::None:
		break;
	}
	if (m_mappingDirty) rebuildMapping(input, mouse);
	const bool rebinding = wasRebinding || m_capture.busy();

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
	//
	// This is the reason isPaused() asks about PAUSE REASONS rather than about
	// the clock standing still: a hit-stop also holds the clock at zero, and a
	// 100 ms freeze that swallows the player's button press is a bug. A freeze
	// stops the world; it does not stop the player being heard.
	const bool paused = HE::api::time::isPaused();

	// UI-only routing silences gameplay input for the same reason a pause does,
	// and lets the same actions through: the one the author marked "run while
	// paused". Without that exception the key that opened a menu could not close
	// it, because in UI-only mode nothing else reaches the controller. Both
	// conditions ask "is the game being played right now", so they share the
	// author's answer rather than growing a second flag that can contradict it.
	const bool uiOnly = HE::api::input::mode() == HE::api::input::Mode::UIOnly;
	// A cutscene's lock is the third such reason, with the same exception: the
	// key that skips the cutscene is an action the author marked the same way.
	const bool silenced = paused || uiOnly || locked;

	// The text-script side of every event below: the same event, to every
	// Lua/Python instance of the session (see the header). Written once as a
	// lambda so the three shapes cannot drift apart in who they reach.
	const bool textScripts = m_scripts && m_scriptInstances && !m_scriptInstances->empty();
	auto eachScript = [&](auto&& fn)
	{
		if (!textScripts) return;
		for (const auto& [entityId, inst] : *m_scriptInstances) fn(inst);
	};

	// The frame's action states, published for the polling rows
	// (HE::api::input::actionDown/…) once the loop has decided what fired.
	// A silenced action is published as released with a zero axis — the same
	// answer the events give, so a poll and a handler never disagree.
	std::vector<HE::api::input::ActionState> states;
	states.reserve(m_actions.size());

	for (const ActionInfo& a : m_actions)
	{
		HE::api::input::ActionState st;
		st.name = a.name;
		// A rebind in progress silences EVERYTHING, the run-while-paused actions
		// included: the press being captured, or the Start that cancels it, must
		// not also fire the pause menu's own action.
		if ((silenced && !a.runWhilePaused) || rebinding) { states.push_back(std::move(st)); continue; }
		switch (a.kind)
		{
		case ActionKind::Axis:
		{
			// Per-frame, like the Tick event — graphs use it as their movement
			// pump. The value is NOT scaled by dt here: a key axis is a held
			// state the graph integrates itself, and a mouse-sourced one is
			// already a displacement. Doing it here would be wrong for both.
			const float v = m_mapping.axisValue(a.name);
			fireInputEvent(HE::inputEventAxis(a.name), HorizonCode::Value::ofFloat(v));
			eachScript([&](uint64_t inst){ m_scripts->callOnInputAxis(inst, a.name, v); });
			st.x = v;
			break;
		}
		case ActionKind::Axis2D:
		{
			float x = 0.0f, y = 0.0f;
			m_mapping.axis2DValue(a.name, x, y);
			fireInputEvent(HE::inputEventAxis2D(a.name),
			               HorizonCode::Value::ofVec2(glm::vec2(x, y)));
			eachScript([&](uint64_t inst){ m_scripts->callOnInputAxis2D(inst, a.name, x, y); });
			st.x = x; st.y = y;
			break;
		}
		case ActionKind::Button:
			st.down     = m_mapping.isPressed(a.name);
			st.pressed  = m_mapping.justPressed(a.name);
			st.released = m_mapping.justReleased(a.name);
			if (st.pressed)
			{
				fireInputEvent(HE::inputEventPressed(a.name), {});
				eachScript([&](uint64_t inst){ m_scripts->callOnInputPressed(inst, a.name); });
			}
			if (st.released)
			{
				fireInputEvent(HE::inputEventReleased(a.name), {});
				eachScript([&](uint64_t inst){ m_scripts->callOnInputReleased(inst, a.name); });
			}
			break;
		}
		states.push_back(std::move(st));
	}
	HE::api::input::setActions(std::move(states));
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
	m_baseMapping.clear();
	m_contextPaths.clear();
	m_overrides.clear();
	m_capture.cancel();
	m_rebindAction.clear();
	m_rebindConflict.clear();
	m_mappingDirty = false;
	if (m_bindingServiceInstalled)
	{
		HE::api::input::setBindingService({});
		m_bindingServiceInstalled = false;
	}
	m_runtime = nullptr;
	m_scripts         = nullptr;
	m_scriptInstances = nullptr;
	// The polling rows answer "nothing" outside a session, not "whatever the
	// last frame of the previous one held".
	HE::api::input::clearActions();
}

PlayerHost::~PlayerHost()
{
	// The service's functions capture `this`; a host that dies mid-session
	// (a test without end(), an app torn down early) must not leave the rows
	// calling into freed memory.
	if (m_bindingServiceInstalled) HE::api::input::setBindingService({});
}

// ── Rebinding ────────────────────────────────────────────────────────────────

bool PlayerHost::rebindBegin(const std::string& action, const std::string& device)
{
	if (!m_runtime) return false;
	HE::BindingDevice dev;
	if (!HE::bindingDeviceFromName(device, dev))
	{
		HE_LOG_WARN(Input, "%s", ("Rebind: unknown device \"" + device +
		                          "\" - use \"keyboard\" or \"gamepad\"").c_str());
		return false;
	}
	const auto it = std::find_if(m_actions.begin(), m_actions.end(),
	                             [&](const ActionInfo& a) { return a.name == action; });
	if (it == m_actions.end())
	{
		HE_LOG_WARN(Input, "%s", ("Rebind: no input action named \"" + action + "\"").c_str());
		return false;
	}
	// An axis needs a direction per key (which of W/S is "up"?) — a question
	// this row has no parameter for yet, so it says no rather than guess.
	if (it->kind != ActionKind::Button)
	{
		HE_LOG_WARN(Input, "%s", ("Rebind: \"" + action + "\" is an axis - only button "
		                          "actions can be rebound so far").c_str());
		return false;
	}
	m_rebindAction = action;
	m_rebindConflict.clear();
	m_capture.arm(dev);
	return true;
}

void PlayerHost::rebindCancel()
{
	if (m_capture.busy())
		HE_LOG_INFO(Input, "%s", ("Rebinding " + m_rebindAction + " cancelled").c_str());
	m_capture.cancel();
}

std::string PlayerHost::bindingName(const std::string& action, const std::string& device) const
{
	HE::BindingDevice dev;
	if (!HE::bindingDeviceFromName(device, dev)) return {};
	const auto* rows = m_mapping.actionBindings(action);
	if (!rows) return {};
	std::string out;
	for (const ActionBinding& b : *rows)
		if (HE::bindingBelongsTo(b, dev))
			out += (out.empty() ? "" : " / ") + HE::bindingDisplayName(b);
	return out;
}

void PlayerHost::resetBindings()
{
	m_capture.cancel();
	m_overrides.clear();
	m_rebindConflict.clear();
	// Not rebuilt here: this is called from a script, mid-frame, with no input
	// in hand. The next tick rebuilds against that frame's.
	m_mappingDirty = true;
}

bool PlayerHost::saveBindings()
{
	HE::api::Ctx ctx;
	if (m_overrides.empty())
		HE::api::prefs::remove(ctx, kBindingsPrefsKey);
	else
		HE::api::prefs::setString(ctx, kBindingsPrefsKey, m_overrides.toJson());
	return true;
}

void PlayerHost::rebuildMapping(const Input& input, const MouseFrame& mouse)
{
	m_mapping = m_baseMapping;
	m_overrides.applyTo(m_mapping);
	// A fresh mapping has never seen a key: without this tick, whatever the
	// player is holding would read as pressed THIS frame and fire again.
	m_mapping.tick(input, mouse);
	m_mappingDirty = false;
}
