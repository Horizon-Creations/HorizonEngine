#pragma once
#include <HorizonCode/HorizonCodeRuntime.h>
#include <Application/InputMapping.h>
#include <string>
#include <vector>

class ContentManager;
class EntityHost;
class Input;

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
// The PlayerController is the engine's central point of contact. Every input
// event reaches EVERY controller, always — a controller stays able to handle
// input whether or not it possesses anything. When a controller does possess a
// character, the same event is ALSO delivered to that character, which is what
// "the controller forwards input to the pawn" means here.
//
// A project with no PlayerController at all keeps the pre-possession behaviour:
// input goes straight to the characters registered via addCharacter(). Without
// that fallback every existing PlayerCharacter graph that handles its own input
// would go silent.
//
// Bindings are the union of every InputMappingContext asset in the project;
// action value types come from the InputAction assets. Discovery walks the
// loose content root (editor / dev builds) plus everything already registered
// in the ContentManager (loadPak'd builds). Assets living ONLY in a mounted,
// not-yet-streamed pak are not found — the pak path index carries no type
// information to sniff without loading (known limitation of the v1 pump).
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
	void tick(const Input& input, float dt, const MouseFrame& mouse = {});

	// Destroy the spawned instances (fires Destruct) and drop all state.
	// Idempotent; begin() may be called again for the next session.
	void end();

	// Register a character the GAME spawned mid-session (Create Object on a
	// PlayerCharacter class). The host does not create characters, so without
	// this it would not know any exist — and the no-controller input fallback
	// above would have nothing to deliver to. Ignores 0 and duplicates.
	//
	// Characters that were ALREADY in the level do not come through here: they
	// are bound before this host starts, and begin() scans them in. Calling this
	// before begin() would be pointless anyway — begin() starts with end().
	void addCharacter(HorizonCode::InstanceId instance);

	// The session's player controllers, in spawn order. Exposed so a caller can
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

	// Deliver one input event the way the routing note describes: to every
	// controller, and additionally to whatever each of them possesses.
	void fireInputEvent(const std::string& event, const HorizonCode::Value& arg);

	HorizonCode::Runtime*                m_runtime = nullptr;
	InputMapping                         m_mapping;
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
