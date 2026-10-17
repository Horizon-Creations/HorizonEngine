#pragma once
#include "HorizonScene/CellState.h"
#include "HorizonScene/CellStreamer.h"
#include <JobSystem/JobSystem.h>
#include <entt/entt.hpp>
#include <glm/vec3.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class HorizonWorld;
class PhysicsWorld;

namespace HE::api { struct Ctx; }

namespace HE
{

// ── Cell streaming for an application (Thema 164, step 2c) ──────────────────
// A CellStreamer (CellStreamer.h) knows cells; it does not know where a cell's
// file comes from, which physics world takes its bodies or who loads its meshes.
// Those answers belong to the application that runs it — the packaged game, or
// the editor while it plays — and differ between them only there: the game reads
// a mounted pak or the loose file, the editor the file in the project. Everything
// else (following the world's manifest, one cancel token per cell, the bodies in
// a charge per slice, the camera as the anchor and its motion as the lookahead)
// is the same in both, and lives here once, so that Play streams what the game
// streams and in the same way.
//
// The application implements ICellHost; CellRuntime calls it from inside its own
// update() (and clear()), on the main thread.
class ICellHost
{
public:
	virtual ~ICellHost() = default;

	// A cell's file (project-relative path, as the manifest names it) as something a
	// worker can call to get its bytes (JSON or CBOR). Empty: the file is not there,
	// the cell fails for this session. CellStreamer::Reader's contract.
	virtual std::function<bool(std::vector<uint8_t>&)> openCellFile(const std::string& path) = 0;

	// The physics world the cells' bodies go into, asked at every call (an
	// application replaces it on a scene switch). Null: no bodies and no limit.
	virtual PhysicsWorld* cellPhysics() = 0;

	// Start loading the assets these entities (of one slice) use, under `token`. The
	// token is the cell's: cancelled when the cell is unloaded or the runtime reset,
	// so a cell that went before its meshes arrived does not leave loads behind.
	virtual void streamCellAssets(HorizonWorld& world, const CancelToken& token,
	                              const std::vector<uint32_t>& entities) = 0;

	// The cell is built: its root and every entity the load created, root included.
	// For what needs the whole cell at once (step 3b: scripts). Default: nothing.
	virtual void cellLoaded(HorizonWorld& world, entt::entity root, const std::vector<entt::entity>& created)
	{
		(void)world; (void)root; (void)created;
	}
	// The cell is about to be destroyed, built in full or in part: called BEFORE its
	// bodies leave the physics world and its loads are cancelled, so that whatever
	// stops here (step 3b: scripts) still sees all of it. Default: nothing.
	virtual void cellUnloading(HorizonWorld& world, entt::entity root)
	{
		(void)world; (void)root;
	}

	// The script API context the cells' state is read and written through
	// (CellState, entity.saveState's code): `ctx` comes with the world and
	// cellPhysics() filled in; the host adds what only it has, the HorizonCode
	// runtime and its EntityHost, without which a class's Save Game variables are
	// neither written out nor put back. Default: nothing added (transform and
	// visibility only).
	virtual void cellApiCtx(api::Ctx& ctx) { (void)ctx; }
};

class CellRuntime
{
public:
	// The frame budget the game has always given the streamer.
	static constexpr double kDefaultBudgetMs = 4.0;

	explicit CellRuntime(ICellHost& host);
	~CellRuntime();
	CellRuntime(const CellRuntime&)            = delete;
	CellRuntime& operator=(const CellRuntime&) = delete;

	// Once a frame, after physics and the camera. Follows the world's manifest
	// (HorizonWorld::cellManifestJson): starts streaming when one appears, starts
	// over when it or the world changes (the old cells are forgotten, not unloaded:
	// a world that changed under the runtime has dropped them already), and stops
	// for a scene without cells or with a malformed list (logged once). Then streams
	// around the world's camera (cameraAnchor) with its motion as the lookahead. No
	// camera: nothing changes this frame.
	void update(HorizonWorld& world, float dt, double budgetMs = kDefaultBudgetMs);

	// Every cell unloaded from `world` through the hooks — the host's cellUnloading,
	// the bodies out of the physics world, the loads cancelled — and the runtime
	// inactive until the next update(). For a world that lives on after the cells
	// go (the editor, before it puts the edited scene back after Play).
	void clear(HorizonWorld& world);
	// Forgets the cells without touching any world, and cancels their loads. For a
	// world that is about to be cleared or destroyed anyway (a scene switch).
	void reset();

	bool                active() const { return m_streamer.active(); }
	const CellStreamer& streamer() const { return m_streamer; }
	CellStreamer&       streamer() { return m_streamer; }
	// Cells with a live cancel token, i.e. that started building and have not gone.
	size_t              cellsWithLoads() const { return m_tokens.size(); }
	// What the session keeps of the cells that went (tombstones, SaveState): kept
	// across unloads and loads, dropped with the session — by clear(), reset(), and a
	// start over for another manifest or world.
	const CellState&    state() const { return m_state; }

	// The anchor update() streams around: the first entity with a transform and a
	// camera, absolute (world origin added). False when the world has none.
	static bool cameraAnchor(HorizonWorld& world, glm::dvec3& absolute);

private:
	void begin(HorizonWorld& world, const std::string& manifestJson);
	void dropTokens();

	ICellHost&    m_host;
	CellStreamer  m_streamer;
	HorizonWorld* m_world = nullptr;   // the world it streams into; null = start over
	std::string   m_manifestJson;      // and the manifest it started from
	glm::dvec3    m_lastCamera{ 0.0 };
	bool          m_hasLastCamera = false;
	std::unordered_map<uint32_t, CancelToken> m_tokens;   // cell root → its asset loads
	CellState     m_state;
	// clear() is under way: the cells go with the session, so their state is not
	// written out (nothing would read it, and on the editor's Stop the script hosts
	// it would be read through have ended already).
	bool          m_clearing = false;
};

} // namespace HE
