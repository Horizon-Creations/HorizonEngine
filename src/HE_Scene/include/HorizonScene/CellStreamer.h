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
// runs on the main thread, within a time budget. Positions in the files are
// absolute: a loaded cell's root sits at -origin, so cells land in the right place
// under a floating origin (FloatingOrigin.h) too.
//
// Anchors and slices (Thema 164): what the streamer keeps around is decided by a
// LIST of anchors (the camera, a player, a script's pin), not by one camera. A cell
// is wanted as soon as any anchor wants it, and it is dropped only when every
// anchor is beyond the unload radius. And a cell is not built in one piece: the
// worker cuts the parsed file into slices of whole subtrees (a few hundred entities
// at most), the main thread builds them one after the other while the frame's budget
// lasts, and the frame that is over budget is over by the largest slice, not by the
// largest cell.
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

// Something the streamer keeps cells around: a camera, a player, a pin a script set.
// Positions are absolute (world position + world origin).
struct CellAnchor
{
	glm::dvec3 position{ 0.0 };
	// m/s. The anchor also wants what it will be near in `lookaheadSec`.
	glm::vec3  velocity{ 0.0f };
	// Stretches both radii for this anchor alone: 2 loads and keeps cells twice as far
	// out as the manifest says, 0.5 half as far. Not positive counts as 1.
	float      radiusScale = 1.0f;
};

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
	// The same for a list of anchors, as CellStreamer measures it: the nearest of
	// them, where an anchor is as near as the closer of its position and where it
	// will be in `lookaheadSec`, and its radiusScale divides the distance (an anchor
	// with scale 2 is as good as one half as far away). Infinite for no anchor.
	double      distanceTo(const std::vector<CellAnchor>& anchors, int x, int z) const;

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
		// About how many slices the game builds the cell in: one for a cell of at most
		// kDefaultCellSliceEntities, else the slice with the cell's root plus the entity
		// count over the slice size, rounded up. From the count alone, so a lower bound:
		// the real cut follows the subtrees in the file, and a subtree that does not fit
		// what is packed so far starts the next slice.
		uint32_t slicesEstimate = 0;
	};
	// The cells within `range` of `p` (absolute), nearest first, with what the
	// game would do with each from there — CellStreamer's rule without the
	// lookahead (a viewpoint has no velocity). For the editor's cell view.
	std::vector<View> around(const glm::dvec3& p, double range) const;
	// The same for several viewpoints at once (the camera and the pins): each cell
	// is judged by the nearest of them, as the streamer does.
	std::vector<View> around(const std::vector<CellAnchor>& anchors, double range) const;
};

// How many entities the streamer puts in one slice unless told otherwise.
constexpr size_t kDefaultCellSliceEntities = 128;

class CellStreamer
{
public:
	// Turns a project-relative cell path into something a worker can call to get
	// the file's bytes: the loose file (JSON), or the scene's entry in a mounted
	// pak (CBOR, as the export writes scenes — told apart by the first byte).
	// Called on the main thread; the returned function runs on a worker. An empty
	// function means the cell cannot be found.
	using Reader = std::function<std::function<bool(std::vector<uint8_t>&)>(const std::string& path)>;

	using Anchor = CellAnchor;

	struct Hooks
	{
		// A slice of a cell was built: the cell's root (a child of the world root) and
		// the entities this slice created. The first slice of a cell is the root alone
		// (a cell of at most the slice size is ONE slice, root and all); the others are
		// whole subtrees, already hung under the root. Physics bodies, asset streaming:
		// what can be done a piece at a time is done here, inside the frame's budget.
		std::function<void(entt::entity root, const std::vector<entt::entity>& created)> loadedSlice;
		// The cell is built: its root and EVERY entity the load created, root included
		// — the call after the last loadedSlice, for what needs the whole cell (a
		// script's BeginPlay seeing all of it).
		std::function<void(entt::entity root, const std::vector<entt::entity>& created)> loaded;
		// A cell is about to be destroyed: its root. Also for a cell that was only
		// built in part (loadedSlice was called, loaded was not) when its anchors left.
		std::function<void(entt::entity root)> unloading;
		// Asked before the FIRST slice of a cell is built, with the number of bodies the
		// manifest says the cell can own (its "bodies" column, an upper bound; the cell is
		// never asked about when the column says nothing, 0): does the physics world have
		// room for them? False puts the whole load off: the cell stays wanted, is not
		// started, and neither is any cell behind it in the build order, so that a nearer
		// cell does not wait while a farther one takes its room. It is asked again every
		// update, and the cells the anchors have left behind are unloaded at the end of
		// every update, which is what makes room. One exception, so that a world whose base
		// alone fills the reserve is not frozen: with no cell built and none under
		// construction the load goes ahead (the physics world logs what it cannot hold).
		// Empty: no limit.
		std::function<bool(uint32_t bodies)> bodiesFit;
	};

	struct Stats
	{
		size_t loaded    = 0;   // cells built right now
		size_t inFlight  = 0;   // reading or parsing on the pool
		size_t ready     = 0;   // parsed, waiting for the main thread to start on them
		size_t building  = 0;   // started, some slices built, the rest still to come
		size_t loadsDone = 0;   // cells built since begin()
		size_t unloads   = 0;   // cells destroyed since begin()
		size_t failed    = 0;   // cells that could not be read or parsed
		size_t cancelled = 0;   // reads dropped because the camera turned away
		size_t abandoned = 0;   // cells dropped half built, because every anchor left
		// Slices built since begin(), and the entities of the largest one: the most a
		// single step of update() builds, and so how far a frame can be over its budget.
		size_t slicesDone   = 0;
		size_t largestSlice = 0;
		// Anchors update() was given last time.
		size_t anchors   = 0;
		// Entities that kept a fresh id because the one stored in their (version 2)
		// cell was already in the world: a copied cell file, or two copies of the
		// scene in one project. Cumulative since begin().
		size_t idCollisions = 0;
		// Updates in which a cell that was ready to be built waited because the physics
		// world had no room for its bodies (Hooks::bodiesFit). Cumulative since begin();
		// a number that keeps growing is a world with too many bodies for the reserve.
		size_t deferredForBodies = 0;
	};

	CellStreamer();
	~CellStreamer();
	CellStreamer(const CellStreamer&)            = delete;
	CellStreamer& operator=(const CellStreamer&) = delete;

	// Starts streaming `manifest`. Unloads nothing: call clear() first when the
	// world still holds cells of an earlier scene.
	void begin(const CellManifest& manifest, Reader reader, Hooks hooks);
	bool active() const { return !m_manifest.empty(); }

	// Once a frame: which cells should be there for these anchors, start reading the
	// missing ones, drop reads nobody wants any more, build what has been parsed — at
	// least one slice, then more while `budgetMs` lasts, nearest cell first and a
	// cell's slices in order — and destroy cells every anchor has left behind (and
	// drop a cell half built the same way).
	//
	// A cell is wanted when ANY anchor is within the load radius of its square (or of
	// where that anchor will be in lookaheadSec), kept while ANY is within the unload
	// radius. With no anchor nothing changes: a server that has nobody on it keeps
	// the cells it has and starts no new read, but the slices already on their way
	// are not built either.
	void update(HorizonWorld& world, const std::vector<Anchor>& anchors, double budgetMs);
	// One anchor with a lookahead: the camera.
	void update(HorizonWorld& world, const glm::dvec3& cameraAbsolute, const glm::vec3& cameraVelocity,
	            double budgetMs);

	// Entities per slice (kDefaultCellSliceEntities when never set). Takes effect for
	// the cells read after the call; 0 builds every cell in one piece, as before the
	// slices — for comparing, not for a game.
	void   setSliceEntities(size_t n) { m_sliceEntities = n; }
	size_t sliceEntities() const { return m_sliceEntities; }

	// True when every cell of the manifest within `radius` of `position` (absolute) is
	// built — or has failed for good, so that nothing more is coming — and so is
	// there for whoever is about to stand or fall there. A teleport waits for this
	// before it lets the player go. The bodies of the cell's entities are in the
	// physics world once the cell is built, because the slice hook is what puts them
	// there synchronously (a cell put off for lack of room for its bodies is not
	// built, so it is not settled either); the body of something that moves is held
	// out of the simulation until its own cell is built (holdsAt), which is what
	// makes the floor really be under it when it is let go (Thema 164 step 3a).
	bool isSettled(const glm::dvec3& position, double radius) const;

	// True when the manifest has a cell at `position` (absolute) that is not built
	// and is not going to be: its file is still being read, parsed, built in part,
	// put off, or not wanted at all yet. What stands there — the floor — is not there
	// (yet), so a body that would fall or walk there has to be held
	// (PhysicsWorld::setRegionHold). False where the manifest has no cell (nothing was
	// moved out of that square), where the cell is built, and where it failed for good:
	// a hold that waits for a file that cannot come would freeze whatever stands there.
	// Inactive streamer: false.
	bool holdsAt(const glm::dvec3& position) const;

	// Unloads every cell and cancels every read. The streamer is inactive after.
	void clear(HorizonWorld& world);
	// Forgets the cells without touching the world — for a world that was cleared
	// or destroyed already.
	void reset();

	const Stats& stats() const { return m_stats; }
	// Built completely; a cell with slices still to come is not loaded yet.
	bool         isLoaded(int x, int z) const;
	// Its root exists and some slices are built, but not all.
	bool         isBuilding(int x, int z) const;
	const CellManifest& manifest() const { return m_manifest; }

private:
	struct Pending;
	using Key = int64_t;
	static Key key(int x, int z) { return (static_cast<int64_t>(x) << 32) ^ static_cast<uint32_t>(z); }

	void unloadCell(HorizonWorld& world, Key k);
	// Takes a cell with some slices built out of the world again.
	void abandonCell(HorizonWorld& world, Key k);

	size_t       m_sliceEntities = kDefaultCellSliceEntities;
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
