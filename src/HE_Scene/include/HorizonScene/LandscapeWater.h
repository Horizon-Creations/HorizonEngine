#pragma once
#include "HorizonScene/Components/TerrainComponent.h"
#include "HorizonScene/LandscapeStore.h"
#include "HorizonScene/LandscapeTileTerrain.h"
#include "HorizonScene/LandscapeWaterData.h"
#include "HorizonScene/WaterMesh.h"

#include <JobSystem/JobSystem.h>
#include <glm/vec3.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace HE
{

// ── Water on a tile landscape (Thema 326, step 3c) ───────────────────────────────
// The single landscape builds one sheet per body from the water::Field inside its TerrainComponent (WaterMesh.h). Over 100 km that cannot
// be one field, and a field per TILE would seam: the surface contour is built from the lattice of ONE field, and a shore that crosses a tile
// border would be contoured from different cells on each side. So water is built over a REGION of n x n resident tiles around the camera
// (n <= 4 for the 256-cell tile, 2 km, LandscapeTileTerrain.h) as one terrain: sheets inside the region have no seams at all, and the
// only seams are at the region's border (the same behaviour the single landscape has at its own edge).
//
// The region follows the camera in whole tiles. It moves only when the camera tile leaves the inner (n - 2) x (n - 2) tiles, so a camera that
// wobbles on a tile border rebuilds nothing; with n = 4 that is a half-kilometre of play either way. A rebuild is a job on the pool at
// low priority: the n^2 tiles are copied into a TerrainComponent (a few MB), the water::Field is filled from their streams, and each body in
// the region gets a sheet (water::buildSurface, with the Field's own shore clipping). The finished region replaces the old one in a single
// step on the main thread: no frame sees half of it. A rebuild needs every tile of the region in memory; until they are, it waits.
//
// Output (LandscapeWaterRegion, immutable, shared): per body a sheet whose vertices are relative to `surface.center` (terrain-local, y = 0);
// a renderer draws it at  region.centre + (surface.center.x, level, surface.center.y) - floatingOrigin  (absolute metres, doubles until the
// subtraction). UVs are world coordinates (`uvOrigin` is the region's centre as a float: 8 mm at 100 km, fine for a texture).
//
// Beyond the region there is no water here: a far sea plane and the ripples, the waves, WaterIndex / buoyancy and the surface entities are
// part of the renderer step. Brushes and the lake tool write fields: step 6.
struct LandscapeWaterConfig
{
	int   tilesPerSide = 4;
	float uvMetersPerTile = 1.0f;   // WaterSurface::kUvMetersPerTile, the single landscape's value
};

struct LandscapeWaterSheet
{
	uint16_t           body = 0;
	float              level = 0.0f;      // absolute metres
	water::SurfaceMesh surface;
	LandscapeWaterBody params;            // the pack's settings of the body (physics, look from below, tessellation)
};

struct LandscapeWaterRegion
{
	LandscapeTileKey                origin;          // lowest tile of the region
	int                             n = 0;
	glm::dvec3                      centre{ 0.0 };   // absolute position of the region's terrain-local origin
	std::shared_ptr<const TerrainComponent> terrain; // what the sheets were built from; answers surfaceAt
	std::vector<LandscapeWaterSheet> sheets;
	LandscapeWaterTable             table;           // the world's bodies as of this region
	bool                            clipToGround = true;
	float                           shoreOvershoot = water::kDefaultShoreOvershoot;
	uint64_t                        revision = 0;    // grows with every region built
};

// The answer for one absolute position (no waves).
struct LandscapeWaterQuery
{
	bool        wet = false;
	uint16_t    body = 0;
	float       level = 0.0f;     // absolute metres
	float       depth = 0.0f;     // level minus the ground, never negative
	LandscapeWaterBody params;    // the body's settings, when wet
};

struct LandscapeWaterStats
{
	uint64_t started = 0;    // rebuilds started
	uint64_t finished = 0;   // regions taken in
	uint64_t discarded = 0;  // finished after a newer one was wanted
	uint64_t deferred = 0;   // a region was wanted but a tile of it was not in memory
	bool     building = false;
};

class LandscapeWater
{
public:
	explicit LandscapeWater(LandscapeWaterConfig config = {}, ThreadPool* pool = nullptr);
	~LandscapeWater();
	LandscapeWater(const LandscapeWater&) = delete;
	LandscapeWater& operator=(const LandscapeWater&) = delete;

	// Main thread, once a frame. `table`: the world's bodies (LandscapeTileSource::water()); null = a world without water, nothing is built.
	void update(const LandscapeStore& store, const glm::dvec3& cameraAbsolute, const LandscapeTileTerrainParams& params, const LandscapeWaterTable* table);

	// The region the sheets belong to now, or null before the first one is built. Replaced as a whole.
	std::shared_ptr<const LandscapeWaterRegion> region() const { return m_region; }
	// Water at an absolute position, from the current region: dry (wet = false) outside it, where the cell is dry, and where the ground stands above
	// the level (plus the overshoot) when the world clips to the ground: wet exactly where the sheet is drawn.
	bool surfaceAt(double x, double z, LandscapeWaterQuery& out) const;

	const LandscapeWaterStats& stats() const { return m_stats; }
	void clear();

private:
	struct Result { uint64_t ticket = 0; std::shared_ptr<const LandscapeWaterRegion> region; };
	struct Shared
	{
		std::mutex mutex;
		std::vector<Result> done;
		std::atomic<int> running{ 0 };
		std::atomic<bool> closed{ false };
	};

	void quiesce();

	LandscapeWaterConfig m_config;
	ThreadPool* m_pool;
	std::shared_ptr<Shared> m_shared;
	CancelToken m_token;
	JobHandle m_job;
	std::shared_ptr<const LandscapeWaterRegion> m_region;
	LandscapeTileKey m_wantOrigin{ -1, -1 };    // the region in flight, or the last one asked for
	uint64_t m_ticket = 0;
	uint64_t m_nextRevision = 1;
	bool m_building = false;
	LandscapeWaterStats m_stats;
};

} // namespace HE
