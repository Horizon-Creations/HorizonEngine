#pragma once
#include <HorizonCode/HorizonCodeRuntime.h>
#include <Application/InputMapping.h>
#include <string>
#include <vector>

class ContentManager;
class EntityHost;
class Input;

// ── PlayerHost ───────────────────────────────────────────────────────────────
// Spawns the project's player HorizonCode classes (HorizonCodeClassAsset with
// baseClass "PlayerController" / "PlayerCharacter") on the shared app runtime
// and pumps input into them:
//   Construct + BeginPlay (once at begin), Tick (per frame, Float dt),
//   Input.<Action>.Pressed / .Released  (Button actions, edge-triggered),
//   Input.<Action>.Axis                 (Axis actions, per frame, Float value).
// Event names come from HE::inputEvent* (Application/InputAssets.h) — the same
// helpers the editor's event catalog uses.
//
// ── Where input goes ─────────────────────────────────────────────────────────
// The PlayerController is the engine's central point of contact. Every input
// event reaches EVERY controller, always — a controller stays able to handle
// input whether or not it possesses anything. When a controller does possess a
// character, the same event is ALSO delivered to that character, which is what
// "the controller forwards input to the pawn" means here.
//
// A project with no PlayerController at all keeps the pre-possession behaviour:
// input goes straight to the characters. Without that fallback every existing
// PlayerCharacter graph that handles its own input would go silent.
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
	// Discover input/player assets and spawn one instance per player class
	// (compiled backend first, interpreted graph fallback); fires Construct and
	// BeginPlay on each. Call once per play session, after content + the
	// runtime's services are set up. `runtime` must outlive this host's session.
	//
	// `entities` (optional) gives PlayerCharacters a scene entity: the character
	// is spawned through it, so it arrives with the components its class carries
	// instead of being a bodiless runtime instance. Null = no entity, which is
	// what a project that drives an already-placed entity itself wants.
	void begin(HorizonCode::Runtime& runtime, ContentManager& cm,
	           EntityHost* entities = nullptr);

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

	bool running() const { return m_runtime != nullptr; }
	size_t playerCount() const { return m_controllers.size() + m_characters.size(); }
	size_t controllerCount() const { return m_controllers.size(); }

private:
	// An action is a button, a one-dimensional axis or a two-dimensional one —
	// three shapes, three event names, so one bool no longer says it.
	enum class ActionKind { Button, Axis, Axis2D };
	struct ActionInfo { std::string name; ActionKind kind = ActionKind::Button; };

	// Deliver one input event the way the routing note describes: to every
	// controller, and additionally to whatever each of them possesses.
	void fireInputEvent(const std::string& event, const HorizonCode::Value& arg);

	HorizonCode::Runtime*                m_runtime = nullptr;
	InputMapping                         m_mapping;
	std::vector<ActionInfo>              m_actions;
	// Routing lists — EVERY player instance, wherever it came from.
	std::vector<HorizonCode::InstanceId> m_controllers;
	std::vector<HorizonCode::InstanceId> m_characters;
	// The subset this host CREATED, and therefore the only ones it ticks and
	// destroys. A character spawned through the EntityHost belongs to that host:
	// ticking it here as well would fire Tick twice a frame, and destroying it
	// here would leave a stale entry in the other host's map.
	std::vector<HorizonCode::InstanceId> m_owned;
};
