#pragma once
#include "HorizonScene/Components/NavMeshComponent.h"
#include "HorizonScene/LandscapeStore.h"

#include <JobSystem/JobSystem.h>
#include <glm/vec3.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace HE
{

// ── Navigation on a tile landscape (Thema 326, step 3) ─────────────────────────────
// The single navmesh (NavigationSystem::bake) is one Recast build over all the geometry of a scene. A 100 km world cannot be: at 0.5 m
// cells that is 200 000 x 200 000 cells. Here the mesh is TILED, in Detour's own sense: one Detour tile per landscape tile, built on the
// job pool from the tile's own height raster (and a strip of its neighbours' heights for the border), added to one dtNavMesh when it is
// finished and taken out again when no anchor is near. Detour links the borders of tiles that sit next to each other by itself, so a path
// runs across tile borders like across any polygon edge.
//
// Space: ABSOLUTE metres, like the single navmesh ("baked in absolute coordinates and queried with the origin added", FloatingOrigin.h):
// tile (x, z) is Detour tile (x, z) with the world's corner at the Detour origin and tileM as tile size. NavigationSystem reads the
// navMesh / navQuery pair of the scene's NavMeshComponent, so a landscape world needs nothing new there: attach() puts this mesh into one.
//
// What is a floor: the ground of the tiles, by slope (NavMeshConfig::maxSlope), eroded by the agent radius. NOT considered: water (an
// agent wades through a lake like through a meadow, as with the single navmesh), anything standing on the ground (no entity geometry; props,
// buildings and bridges are the entity cells' business, step 5), and off-mesh links.
//
// A tile is built only when the tiles around it are in memory too (their heights are what the border needs); a world edge needs none. So the
// store has to hold at least one tile more than the navigation radius (the default store reaches 8 km, the radius is 600 m).
//
// The last limit is Detour's polygon reference (32 bit): 8 bits of tile slot, 14 bits of polygon, 10 bits of salt. At most kMaxTiles tiles
// with data are held at once (no new build starts while they are all used) and a tile of kMaxPolysPerTile polygons or more is refused (counted in `failed`). findPath keeps its 256
// polygons (NavigationSystem), so a path longer than that stops short, the same on any navmesh.
struct LandscapeNavConfig
{
	// Landscape defaults, coarser than the single navmesh's (0.3 m / 0.2 m): the grid of one tile is (tileM / cellSize)^2.
	NavMeshConfig nav = defaultNav();
	float  radius = 600.0f;          // tiles within this many metres of an anchor are wanted
	float  keepFactor = 1.25f;       // and kept until the nearest anchor is beyond radius * keepFactor
	size_t maxInFlight = 2;          // builds at once
	size_t maxStartsPerUpdate = 1;

	static NavMeshConfig defaultNav()
	{
		NavMeshConfig c;
		c.cellSize = 0.5f;
		c.cellHeight = 0.25f;
		// Recast's ledge filter drops a span whose neighbours on the two sides together differ from it by more than the climb, so on a ramp
		// the climb has to be twice the rise of one cell: 0.5 m of rise per 0.5 m cell is 45 degrees (maxSlope) at a climb of 1.0 m = 4 height
		// cells. At the single navmesh's 0.9 m the effective limit would be 37 degrees. In general: min(maxSlope, atan(climb cells * cellHeight / (2 * cellSize))).
		c.walkableClimb = 1.0f;
		c.detailSampleDist = 6.0f;
		return c;
	}
};

struct LandscapeNavStats
{
	size_t   tiles = 0;              // tiles with polygons, in the mesh now
	size_t   empty = 0;              // tiles built that have no walkable ground (sea, cliffs): done, nothing in the mesh
	size_t   pending = 0;            // builds in flight
	size_t   polygons = 0;           // in the mesh now
	uint64_t started = 0;
	uint64_t finished = 0;
	uint64_t dropped = 0;            // tiles taken out because the anchors left
	uint64_t discarded = 0;          // finished after nobody wanted them
	uint64_t failed = 0;             // a build or addTile that gave up
	uint64_t deferred = 0;           // times a wanted tile waited for a neighbour that was not in memory
};

enum class LandscapeNavBuild { Ok, Empty, Failed };

// The Detour tile data of one landscape tile. `tileAt` hands out the resident tiles (null when not in memory; a missing tile inside the world
// is a failure, outside it the border repeats the edge). Ok: `data` (allocated with dtAlloc, the caller frees it with dtFree or hands it to
// dtNavMesh::addTile with DT_TILE_FREE_DATA) and `size`, `polygons`. Empty: the tile has no walkable ground. Pure and thread-safe.
LandscapeNavBuild buildLandscapeNavTile(const std::function<std::shared_ptr<const LandscapeTile>(LandscapeTileKey)>& tileAt, LandscapeTileKey key,
                                        int tilesX, int tilesZ, float heightMinM, float heightScaleM, const NavMeshConfig& cfg,
                                        unsigned char*& data, int& size, int& polygons);

class LandscapeNavigation
{
public:
	static constexpr int kMaxTiles = 256;
	static constexpr int kMaxPolysPerTile = 16384;

	explicit LandscapeNavigation(LandscapeNavConfig config = {}, ThreadPool* pool = nullptr);
	~LandscapeNavigation();
	LandscapeNavigation(const LandscapeNavigation&) = delete;
	LandscapeNavigation& operator=(const LandscapeNavigation&) = delete;

	// Main thread, once a frame. `anchors`: absolute positions of whatever walks (agents, the player), nearest tiles first. Takes in the builds
	// that finished, drops tiles out of range and starts new builds for tiles the store (and their neighbours) hold.
	void update(const LandscapeStore& store, const std::vector<glm::dvec3>& anchors, float heightMinM, float heightScaleM);

	// The mesh and its query, shared with whoever uses them (a NavMeshComponent). Null before the first update(). They stay the same objects
	// until clear() ends the landscape.
	std::shared_ptr<dtNavMesh>      navMesh() const { return m_mesh; }
	std::shared_ptr<dtNavMeshQuery> navQuery() const { return m_query; }
	// Puts the mesh into `nmc` (what NavigationSystem reads). The component's own baked mesh is replaced; its geometry is left alone.
	void attach(NavMeshComponent& nmc) const;

	bool tileBuilt(LandscapeTileKey tile) const;   // in the mesh with polygons
	bool tileDone(LandscapeTileKey tile) const;    // built (with or without polygons)
	const LandscapeNavStats& stats() const { return m_stats; }
	const LandscapeNavConfig& config() const { return m_config; }
	void clear();

private:
	struct Entry
	{
		bool        done = false;
		bool        inMesh = false;
		uint32_t    ref = 0;      // dtTileRef, valid when inMesh
		int         polygons = 0;
		uint64_t    ticket = 0;
		CancelToken cancel;
		JobHandle   job;
	};
	struct Result
	{
		uint64_t        key = 0;
		uint64_t        ticket = 0;
		LandscapeNavBuild status = LandscapeNavBuild::Failed;
		unsigned char*  data = nullptr;
		int             size = 0;
		int             polygons = 0;
	};
	struct Shared
	{
		std::mutex mutex;
		std::vector<Result> done;
		std::atomic<int> running{ 0 };
		std::atomic<bool> closed{ false };
	};

	void quiesce();
	bool ensureMesh(float tileM);
	void removeTile(Entry& e, uint64_t key);
	static uint64_t pack(LandscapeTileKey k) { return (static_cast<uint64_t>(static_cast<uint32_t>(k.z)) << 32) | static_cast<uint32_t>(k.x); }

	LandscapeNavConfig m_config;
	ThreadPool* m_pool;
	std::shared_ptr<Shared> m_shared;
	CancelToken m_token;
	std::shared_ptr<dtNavMesh>      m_mesh;
	std::shared_ptr<dtNavMeshQuery> m_query;
	float m_tileM = 0.0f;
	std::unordered_map<uint64_t, Entry> m_entries;
	uint64_t m_nextTicket = 1;
	LandscapeNavStats m_stats;
};

} // namespace HE
