#pragma once
#include <HorizonCode/HorizonCodeRuntime.h>
#include <Application/InputMapping.h>
#include <string>
#include <vector>

class ContentManager;
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
	void begin(HorizonCode::Runtime& runtime, ContentManager& cm);

	// Per-frame pump: ticks the mapping against `input`, then fires Tick and
	// the per-action input events on every player instance. No-op when not
	// running.
	void tick(const Input& input, float dt);

	// Destroy the spawned instances (fires Destruct) and drop all state.
	// Idempotent; begin() may be called again for the next session.
	void end();

	bool running() const { return m_runtime != nullptr; }
	size_t playerCount() const { return m_players.size(); }

private:
	struct ActionInfo { std::string name; bool isAxis = false; };

	HorizonCode::Runtime*                m_runtime = nullptr;
	InputMapping                         m_mapping;
	std::vector<ActionInfo>              m_actions;
	std::vector<HorizonCode::InstanceId> m_players;
};
