#pragma once
#include <HorizonCode/HorizonCodeRuntime.h>
#include <Application/InputMapping.h>
#include <Application/InputRebind.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class ContentManager;
class EntityHost;
class Input;
class ScriptContext;

// ── PlayerHost ───────────────────────────────────────────────────────────────
// Spawns the project's PlayerController HorizonCode classes (HorizonCodeClass
// assets whose resolved base is PlayerController) on the shared app runtime and
// pumps input into them:
//   Construct + BeginPlay (once at begin), Tick (per frame, Float dt),
//   Input.<Action>.Pressed / .Released  (Button actions, edge-triggered),
//   Input.<Action>.Axis                 (Axis actions, per frame, Float value).
// Event names come from HE::inputEvent* (Application/InputAssets.h) — the same
// helpers the editor's event catalog uses.
//
// ── What is NOT spawned here: characters ─────────────────────────────────────
// A PlayerCharacter is a body in a level, and where a body stands is the game's
// decision, not the engine's. So the host instantiates controllers only — enough
// for something to be running — and the game spawns its character itself, from
// the controller's BeginPlay, with Create Object at a Location/Rotation of its
// choosing, then takes it with player.possess. Possession is therefore always
// authored: there is no auto-possess any more, because with nothing spawned
// automatically there is nothing unambiguous left to guess at.
//
// ── Where input goes ─────────────────────────────────────────────────────────
// The PlayerController is the engine's central point of contact. Each
// controller is a LOCAL PLAYER, and a player's input events reach that
// player's controller, always — a controller stays able to handle input
// whether or not it possesses anything. When a controller does possess a
// character, the same event is ALSO delivered to that character, which is what
// "the controller forwards input to the pawn" means here.
//
// ── Local players (couch co-op) ──────────────────────────────────────────────
// Controllers are spawned sorted by asset path, so "player 1" is the same
// class on every machine; HE::api::player::controllerAt(i) names them.
//   * ONE controller (every single-player project): it reads everything — the
//     keyboard, the mouse and ALL pads merged — exactly as before there were
//     pad slots.
//   * TWO or more: controller i reads pad slot i (Input's slot table, the pad
//     whose LED shows player i+1) and nothing else; controller 0 also owns the
//     keyboard and mouse. A player without a pad in their slot hears nothing
//     until one is plugged in.
// Each player has its own mapping state, binding layer and capture. Several
// players come from several PlayerController classes (P1Controller and
// P2Controller deriving from one shared class); spawning a second instance of
// ONE class at runtime is not there yet.
//
// What stays one-player, on purpose: the text-script events and the polling
// rows (HE::api::input::setActions) carry player 0's actions — they have no
// player parameter, and handing them everybody's would make player 2's jump
// look like player 1's. The script rebinding rows act on player 0 as well.
//
// A project with no PlayerController at all keeps the pre-possession behaviour:
// input goes straight to the characters registered via addCharacter(). Without
// that fallback every existing PlayerCharacter graph that handles its own input
// would go silent.
//
// ── Text scripts hear it too ─────────────────────────────────────────────────
// A Lua or Python script on an entity has no controller and possesses nothing,
// so the routing above says nothing about it — and until setTextScripts existed
// it simply never heard an action: the pump ran in a Lua project like in any
// other, with nobody listening. Now every text-script instance of the session
// receives every action event (onInputPressed/… in Lua, on_input_pressed/… in
// Python), under the same pause and UI-only silence the graphs get. Which of
// them cares is the script's business: a handler that is not defined costs a
// table lookup and nothing else. The same tick also publishes the frame's
// action states to HE::api::input::setActions, the polling twin of the events.
//
// Bindings are the union of every InputMappingContext asset in the project:
// the contexts are applied sorted by path, and an action named in several
// keeps the bindings of all of them (duplicates once) — not only the last
// context's. Replacing an action's bindings is kept for a layer on top of this
// base (HE::MappingMerge::Replace). That layer is the PLAYER's: see
// "Rebinding" below. Action value types come from the
// InputAction assets. Discovery walks the loose content root (editor / dev
// builds) plus everything already registered in the ContentManager (loadPak'd
// builds). Assets living ONLY in a mounted,
// not-yet-streamed pak are not found — the pak path index carries no type
// information to sniff without loading (known limitation of the v1 pump).
//
// ── Rebinding ────────────────────────────────────────────────────────────────
// The player's own bindings are a layer (HE::BindingOverrides) applied over the
// merged contexts, per action and device class — one layer per local player.
// begin() loads each from prefs (bindingsPrefsKey(player)) once the players
// are known and before any BeginPlay runs; the script rows
// (HE::api::input::rebindBegin …) reach this host through the BindingService it
// installs for the length of the session. The capture (HE::BindingCapture)
// reads Input in tick() BEFORE the mapping, so a menu in UI-only mode — where
// every gameplay action is silent — can still hear the press. While it runs,
// all actions are silent, the "run while paused" ones included: the Escape or
// Start that cancels it must not also open or close the pause menu.
//
// The host does NOT own the runtime: the application passes its
// GameInstanceHost runtime so player instances share its services (widgets,
// createObject, engine calls) and its latent-node update.
class PlayerHost
{
public:
	// Discover input/player assets and spawn one instance per PlayerController
	// class (compiled backend first, interpreted graph fallback); fires Construct
	// and BeginPlay on each. Call once per play session, after content + the
	// runtime's services are set up. `runtime` must outlive this host's session.
	// PlayerCharacter classes are counted and reported, never instantiated — see
	// the note above.
	//
	// `entities` is not for spawning: it is READ, once, to pick up the
	// PlayerCharacters that are already in the level. Those are bound by
	// EntityHost::begin (a Script component naming the class), which runs BEFORE
	// this — so without the scan the no-controller fallback below would have an
	// empty list and a project that places its character in the scene and has no
	// controller would receive no input at all. Null is allowed: a session with
	// no entity host has no placed characters to find.
	void begin(HorizonCode::Runtime& runtime, ContentManager& cm,
	           const EntityHost* entities = nullptr);

	// Per-frame pump: ticks the mapping against `input`, then fires Tick and
	// the per-action input events (see the routing note above). No-op when not
	// running.
	// `mouse` is the frame's mouse movement, for axes bound to a mouse source.
	// Passed in rather than read off `input` because who may act on the mouse is
	// the caller's decision: a running game always may, the editor only while
	// play mode holds it. Default {} = no mouse this frame.
	// `locked` silences gameplay input for this frame exactly the way a pause
	// does — the actions marked "run while paused" still arrive. A cutscene with
	// Lock Player Input passes true (SequenceSystem::locksPlayerInput), so the
	// player cannot walk out of their own cutscene but can still skip it or
	// open the pause menu.
	void tick(const Input& input, float dt, const MouseFrame& mouse = {}, bool locked = false);

	// Destroy the spawned instances (fires Destruct) and drop all state.
	// Idempotent; begin() may be called again for the next session.
	void end();

	// The session's Lua/Python instances, so tick() can deliver the action
	// events to them as well (see the note above). `instances` is the host
	// application's entity → instance map, the same one CollisionSystem and
	// AnimationNotifySystem are handed; it is READ every tick, never copied, so
	// a script that starts or dies mid-session is seen the next frame. Either
	// null = no text scripts (tests, a context-less session). Cleared by end().
	// The map type is spelled out rather than taken from ScriptContext so this
	// header does not pull ScriptContext.h (and entt with it) into both apps.
	using TextScriptInstances = std::unordered_map<uint32_t, uint64_t>;
	void setTextScripts(ScriptContext* scripts, const TextScriptInstances* instances);

	// Register a character the GAME spawned mid-session (Create Object on a
	// PlayerCharacter class). The host does not create characters, so without
	// this it would not know any exist — and the no-controller input fallback
	// above would have nothing to deliver to. Ignores 0 and duplicates.
	//
	// Characters that were ALREADY in the level do not come through here: they
	// are bound before this host starts, and begin() scans them in. Calling this
	// before begin() would be pointless anyway — begin() starts with end().
	void addCharacter(HorizonCode::InstanceId instance);

	// The session's player controllers, in spawn order — sorted by asset path,
	// and index i is local player i. Exposed so a caller can
	// ask each one what it possesses (HE::api::player::possessed) and map that
	// instance to its scene entity via EntityHost::entityOf — which is what lets
	// a camera rig default to "follow the player" without the project having to
	// name an entity. Possession, not spawn order: the character the player
	// steers is the one a controller took, and a level may hold several.
	const std::vector<HorizonCode::InstanceId>& controllers() const { return m_controllers; }

	bool running() const { return m_runtime != nullptr; }
	size_t controllerCount() const { return m_controllers.size(); }
	// Only what the no-controller fallback would deliver to. Not "the players":
	// with a controller in the project this list is beside the point, and the
	// character the player actually steers is whatever a controller possesses.
	size_t fallbackCharacterCount() const { return m_characters.size(); }

	// Local players of this session: one per controller, and one (player 0)
	// when the project has no controller — the fallback characters and the text
	// scripts are that player's. 0 when not running.
	size_t       playerCount() const { return m_players.size(); }
	// Whose hands player `player` reads (see "Local players" above).
	InputDevices devicesOf(size_t player) const
	{ return player < m_players.size() ? m_players[player].devices : InputDevices{}; }

	// A player's bindings: the merged contexts (see the note above for how they
	// combine) plus that player's layer; and the content-relative paths of the
	// contexts that went into the base, in the order they were applied. An
	// unknown player reads as an empty mapping.
	const InputMapping&             mapping(size_t player = 0) const;
	const std::vector<std::string>& mappingContexts() const { return m_contextPaths; }

	// ── Rebinding (see the note above; the script rows call these) ───────────
	// Where a player's layer is kept: "input.overrides.<player>". Player 0's is
	// named here because it is the one a single-player project ever writes.
	static constexpr const char* kBindingsPrefsKey = "input.overrides.0";
	static std::string bindingsPrefsKey(size_t player);

	// `device` "keyboard" (keys + mouse buttons) or "gamepad". False when not
	// running, for an unknown or non-Button action, a bad device name, an
	// unknown player, or "keyboard" for a player who does not own the desk.
	// A second call for the same player while one listens replaces it. The
	// script rows always mean player 0.
	bool        rebindBegin(const std::string& action, const std::string& device,
	                        size_t player = 0);
	// Every player's capture.
	void        rebindCancel();
	// True while any player's capture listens (or drains its button).
	bool        isRebinding() const;
	// The other actions/axes the last captured input also triggers, ", "-joined.
	const std::string& rebindConflict(size_t player = 0) const;
	std::string bindingName(const std::string& action, const std::string& device,
	                        size_t player = 0) const;
	// Drop a player's layer. Applied on the next tick (the mapping is rebuilt
	// against that frame's input, so a held key does not read as a fresh press).
	void        resetBindings(size_t player = 0);
	// Write every player's layer to prefs (an empty layer removes its key).
	bool        saveBindings();
	const HE::BindingOverrides& bindingOverrides(size_t player = 0) const;

	PlayerHost() = default;
	PlayerHost(const PlayerHost&) = delete;
	PlayerHost& operator=(const PlayerHost&) = delete;
	// Only takes the BindingService down (it points at this host); end() is
	// still the caller's, since it needs a runtime that may already be gone.
	~PlayerHost();

private:
	// An action is a button, a one-dimensional axis or a two-dimensional one —
	// three shapes, three event names, so one bool no longer says it.
	enum class ActionKind { Button, Axis, Axis2D };
	struct ActionInfo
	{
		std::string name;
		ActionKind  kind = ActionKind::Button;
		// The action's "Fires while the game is paused" switch. Off by default,
		// so a pause silences everything unless the author said otherwise.
		bool        runWhilePaused = false;
	};

	// Everything input-side that is ONE player's.
	struct LocalPlayer
	{
		InputDevices         devices;
		InputMapping         mapping;     // base + this player's layer: what ticks
		HE::BindingOverrides overrides;
		HE::BindingCapture   capture;
		std::string          rebindAction;
		std::string          rebindConflict;
		bool                 mappingDirty = false;
	};

	// Deliver one of `player`'s input events the way the routing note
	// describes: to that player's controller, and additionally to whatever it
	// possesses (no controller at all: to the fallback characters).
	void fireInputEvent(size_t player, const std::string& event, const HorizonCode::Value& arg);

	// base + layer, then one tick against this frame's input so whatever is
	// held reads as held, not as just pressed.
	void rebuildMapping(LocalPlayer& p, const Input& input, const MouseFrame& mouse);

	HorizonCode::Runtime*                m_runtime = nullptr;
	ScriptContext*                       m_scripts = nullptr;
	const TextScriptInstances*           m_scriptInstances = nullptr;
	InputMapping                         m_baseMapping;   // the contexts alone
	std::vector<std::string>             m_contextPaths;
	std::vector<LocalPlayer>             m_players;
	bool                                 m_bindingServiceInstalled = false;
	std::vector<ActionInfo>              m_actions;
	// The instances this host CREATED, and therefore the only ones it ticks and
	// destroys.
	std::vector<HorizonCode::InstanceId> m_controllers;
	// Only ever filled by addCharacter(), and only read by the no-controller
	// fallback. These instances belong to the EntityHost: ticking them here as
	// well would fire Tick twice a frame, and destroying them here would leave a
	// stale entry in the other host's map.
	std::vector<HorizonCode::InstanceId> m_characters;
};
