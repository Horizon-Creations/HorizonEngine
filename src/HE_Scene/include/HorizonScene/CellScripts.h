#pragma once
#include <Scripting/ScriptEngine.h>
#include <entt/entt.hpp>
#include <cstdint>
#include <unordered_map>
#include <vector>

class ContentManager;
class EntityHost;
class HorizonWorld;
class ScriptContext;
namespace HorizonCode { class Runtime; }

namespace HE
{

// ── The scripts of a streamed cell (Thema 183, step 3) ──────────────────────
// A cell brings entities with a ScriptComponent like any scene does: a
// HorizonCode class (EntityHost) or a Lua/Python script (ScriptContext). At a
// scene start the application starts them all once; what a cell brings later
// is started here, when the cell is built (ICellHost::cellLoaded), and stopped
// here, when it is about to go (ICellHost::cellUnloading) — the same two calls
// in the game and in the editor's Play, so that both hosts start and stop a
// cell's scripts the same way. The zones' unload in the two apps had already
// drifted apart (the editor's dropped neither the instance map entry nor the
// watches, the game's never destroyed the backend instance); this is the one
// copy for cells.
//
// What the application has, borrowed for the call. Any of them may be null, or
// not running (the editor's Stop tears the hosts down before the cells): that
// half is then left alone.
struct CellScriptHosts
{
	EntityHost*           entities = nullptr;   // HorizonCode classes on entities
	ScriptContext*        text     = nullptr;   // Lua / Python
	std::unordered_map<uint32_t, ScriptEngine::InstanceId>* textInstances = nullptr;   // the app's entity → instance
	HorizonCode::Runtime* runtime  = nullptr;   // where a text script's horizon.hc.watch subscriptions live
	ContentManager*       content  = nullptr;   // a text script's source
};

// Starts the scripts of `created` (a cell just built, its root included): binds
// the HorizonCode classes (Construct + BeginPlay) and starts the Lua/Python
// scripts (onStart). An entity that already runs a script is left alone — it is
// not started twice, and its instance is not overwritten and leaked. Returns how
// many started.
int startCellScripts(const CellScriptHosts& hosts, HorizonWorld& world, const std::vector<entt::entity>& created);

// Stops the scripts of everything under `root` NOW (the cell's root included):
// what a script hung out of the cell keeps its script and lives on, and what a
// script spawned under the root goes with the cell. A class's Destruct fires
// while the whole cell is still there; a Lua/Python instance is destroyed, its
// map entry and its watches dropped. Returns how many stopped.
int stopCellScripts(const CellScriptHosts& hosts, HorizonWorld& world, entt::entity root);

} // namespace HE
