#pragma once
#include <JobSystem/JobSystem.h>
#include <entt/entt.hpp>
#include <glm/vec3.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class HorizonWorld;

namespace HE
{

// ── Cell streaming (Thema 153) ───────────────────────────────────────────────
// A large scene split into a square grid of cells (scripts/split_scene_cells.py):
// the base scene keeps everything that has to be there all the time (sky,
// terrain, cameras, scripts, players) and a "cells" object naming the cell
// files; each cell file holds the placed things inside one grid square (meshes,
// lights, static bodies). The game loads a cell once the camera, or where the
// camera will be in `lookaheadSec`, comes within `loadRadius` of the cell's
// square, and unloads it once both are further than `unloadRadius` (the gap
// stops a camera on the border from loading and unloading every frame).
//
// Reading and parsing a cell runs on the job pool; only building its entities
// runs on the main thread, cell by cell, within a time budget. Positions in the
// files are absolute: a loaded cell's root sits at -origin, so cells land in the
// right place under a floating origin (FloatingOrigin.h) too.
//
// Identity (Thema 164): a cell the C++ splitter wrote (CellSplit.h) carries a
// "streaming" head with its format version, and from version 2 on its entities are
// loaded with the ids they were saved with, not fresh ones — so whatever refers to
// one of them by id (a prefab placement's bindings, a joint, a rig) finds it again
// after the cell was unloaded and loaded. An id the world already holds is not
// taken over: that entity gets a fresh one, and Stats::idCollisions and the log say
// so. A cell without a head (scripts/split_scene_cells.py, or an editor from before
// the head existed) is version 1 and loads as it always did, with fresh ids.

// The format the C++ splitter writes. Version 2 added the manifest's "version" and
// the fourth column of its list ("bodies"), and the "streaming" head of every cell.
constexpr int kCellFormatVersion = 2;

struct CellManifest
{
	struct Cell
	{
		int      x = 0;
		int      z = 0;
		uint32_t entities = 0;   // as the splitter counted them; informational
		// Of those, the ones that can own a physics body: a rigid body or a collider
		// (a collider alone gets none today, so this is an upper bound). Lets a loader
		// see what a cell costs the physics world before it builds it. 0 when the
		// manifest does not say (version 1).
		uint32_t bodies = 0;
	};
	int               version      = 1;      // of the format, see kCellFormatVersion; missing = 1
	float             cellSize     = 0.0f;   // m, edge of a grid square
	float             loadRadius   = 0.0f;   // m, from the camera to the square
	float             unloadRadius = 0.0f;   // m, >= loadRadius
	float             lookaheadSec = 2.0f;   // s, how far ahead the camera is extrapolated
	std::string       dir;                   // project-relative folder of the cell files
	std::vector<Cell> cells;

	// Reads the scene's "cells" object (HorizonWorld::cellManifestJson). False,
	// and an empty manifest, for anything malformed — a scene that cannot stream
	// its cells then loads only its base, and says so in the log.
	static bool parse(const std::string& json, CellManifest& out);
	bool        empty() const { return cells.empty() || !(cellSize > 0.0f); }
	// Project-relative path of one cell's file.
	std::string cellPath(int x, int z) const;
	// The grid square a world position (absolute) falls into.
	static int  cellIndex(double coord, float cellSize);
	// Ground-plane distance from `p` (absolute) to the square of cell (x, z),
	// 0 inside it — the distance CellStreamer loads and unloads by, so the
	// editor's cell view can say what the game would hold from a viewpoint.
	double      distanceTo(const glm::dvec3& p, int x, int z) const;

	// One cell as seen from a viewpoint.
	struct View
	{
		enum class Reach : uint8_t
		{
			Load,   // within loadRadius: the game builds it from here
			Keep,   // up to unloadRadius: built cells stay, nothing new loads
			Out,    // beyond: the game drops it
		};
		int      x = 0, z = 0;
		uint32_t entities = 0;
		double   distance = 0.0;   // distanceTo(viewpoint)
		Reach    reach = Reach::Out;
	};
	// The cells within `range` of `p` (absolute), nearest first, with what the
	// game would do with each from there — CellStreamer's rule without the
	// lookahead (a viewpoint has no velocity). For the editor's cell view.
	std::vector<View> around(const glm::dvec3& p, double range) const;
};

class CellStreamer
{
public:
	// Turns a project-relative cell path into something a worker can call to get
	// the file's bytes: the loose file (JSON), or the scene's entry in a mounted
	// pak (CBOR, as the export writes scenes — told apart by the first byte).
	// Called on the main thread; the returned function runs on a worker. An empty
	// function means the cell cannot be found.
	using Reader = std::function<std::function<bool(std::vector<uint8_t>&)>(const std::string& path)>;

	struct Hooks
	{
		// A cell was built: its root (a child of the world root) and every entity
		// the load created, root included. Physics bodies, asset streaming.
		std::function<void(entt::entity root, const std::vector<entt::entity>& created)> loaded;
		// A cell is about to be destroyed: its root.
		std::function<void(entt::entity root)> unloading;
	};

	struct Stats
	{
		size_t loaded    = 0;   // cells built right now
		size_t inFlight  = 0;   // reading or parsing on the pool
		size_t ready     = 0;   // parsed, waiting for the main thread
		size_t loadsDone = 0;   // cells built since begin()
		size_t unloads   = 0;   // cells destroyed since begin()
		size_t failed    = 0;   // cells that could not be read or parsed
		size_t cancelled = 0;   // reads dropped because the camera turned away
		// Entities that kept a fresh id because the one stored in their (version 2)
		// cell was already in the world: a copied cell file, or two copies of the
		// scene in one project. Cumulative since begin().
		size_t idCollisions = 0;
	};

	CellStreamer();
	~CellStreamer();
	CellStreamer(const CellStreamer&)            = delete;
	CellStreamer& operator=(const CellStreamer&) = delete;

	// Starts streaming `manifest`. Unloads nothing: call clear() first when the
	// world still holds cells of an earlier scene.
	void begin(const CellManifest& manifest, Reader reader, Hooks hooks);
	bool active() const { return !m_manifest.empty(); }

	// Once a frame: which cells should be there for this camera (absolute
	// position, and its velocity for the lookahead), start reading the missing
	// ones, drop reads nobody wants any more, build what has been parsed — at
	// least one cell, then more while `budgetMs` lasts — and destroy cells
	// beyond the unload radius.
	void update(HorizonWorld& world, const glm::dvec3& cameraAbsolute, const glm::vec3& cameraVelocity,
	            double budgetMs);

	// Unloads every cell and cancels every read. The streamer is inactive after.
	void clear(HorizonWorld& world);
	// Forgets the cells without touching the world — for a world that was cleared
	// or destroyed already.
	void reset();

	const Stats& stats() const { return m_stats; }
	bool         isLoaded(int x, int z) const;
	const CellManifest& manifest() const { return m_manifest; }

private:
	struct Pending;
	using Key = int64_t;
	static Key key(int x, int z) { return (static_cast<int64_t>(x) << 32) ^ static_cast<uint32_t>(z); }

	void unloadCell(HorizonWorld& world, Key k);

	CellManifest m_manifest;
	Reader       m_reader;
	Hooks        m_hooks;
	std::unordered_map<Key, size_t>                   m_index;    // cell → manifest.cells slot
	std::unordered_map<Key, entt::entity>             m_loaded;   // cell → its root
	std::unordered_map<Key, std::shared_ptr<Pending>> m_pending;  // reading/parsing/ready
	std::unordered_map<Key, bool>                     m_failed;   // not tried again this session
	Stats        m_stats;
};

} // namespace HE
