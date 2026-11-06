#pragma once
#include "HorizonScene/Components/TerrainComponent.h"
#include "HorizonScene/LandscapeStore.h"
#include "HorizonScene/LandscapeTileTerrain.h"

#include <JobSystem/JobSystem.h>
#include <glm/vec3.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace HE
{

// ── GI height clipmap on a tile landscape (Thema 326, step 4, redone over the tile store) ────────
// The single landscape's GI sun-shadow kernel marches a height-field pyramid built once from its
// TerrainComponent (GiHeightfield.h, RenderExtractor.cpp). A 100 km tile landscape has no such
// component, so the same pyramid is built here from a REGION of n x n resident tiles around the
// camera (n <= 4 for the 256-cell tile, 2 km — LandscapeTileTerrain.h, the same limit and the same
// region shape LandscapeWater.h uses for its own seamless sheets). Unlike water, there is no "no
// region wanted" case: the clipmap is wanted whenever the landscape is active.
//
// The region follows the camera in whole tiles and moves only when the camera tile leaves the
// inner (n - 2) x (n - 2) tiles (half a kilometre of play either way at n = 4) — copying and
// uploading a few MB of heights on every frame's tile movement would cost more than the shadow it
// buys. A rebuild runs as a low-priority job (the n^2 tiles are read and flattened into one
// (n*hCells+1)^2 float array, a few MB) and the finished clipmap replaces the old one as a whole on
// the main thread, never half-built. Missing tiles (edge of the load radius, a fresh start before
// anything has streamed in) defer the rebuild and keep whatever clipmap already exists — the GI
// shadow kernel reads one frame behind the tiles rather than going dark for one.
//
// `active()` is false until the first region finishes: RenderExtractor pushes nothing into
// RenderWorld::heightfields until then, exactly like a TerrainComponent with no builtField yet.
struct LandscapeGiConfig
{
	int tilesPerSide = 4;
};

// Mirrors HE::GiHeightfield (HorizonRendering/RenderWorld.h) without depending on HE_Rendering:
// HE_Scene does not link it, so RenderExtractor is the one that turns this into a GiHeightfield
// (data/res/version copied verbatim, worldToLocal/size derived from centreAbsolute/sizeX/sizeZ and
// the render world's floating origin).
struct LandscapeGiHeightfield
{
	std::shared_ptr<const std::vector<float>> data;   // res x res, absolute metres, row-major
	uint32_t   res = 0;
	uint64_t   version = 0;                           // grows with every region built; never 0 once active
	glm::dvec3 centreAbsolute{ 0.0 };                  // the window's centre (its local origin), absolute world
	float      sizeX = 0.0f, sizeZ = 0.0f;
	bool active() const { return res >= 2 && data && data->size() >= static_cast<size_t>(res) * res; }
};

struct LandscapeGiStats
{
	uint64_t started = 0;    // rebuilds started
	uint64_t finished = 0;   // regions taken in
	uint64_t discarded = 0;  // finished after a newer one was wanted
	uint64_t deferred = 0;   // a region was wanted but a tile of it was not in memory
	bool     building = false;
};

class LandscapeGi
{
public:
	explicit LandscapeGi(LandscapeGiConfig config = {}, ThreadPool* pool = nullptr);
	~LandscapeGi();
	LandscapeGi(const LandscapeGi&)            = delete;
	LandscapeGi& operator=(const LandscapeGi&) = delete;

	// Main thread, once a frame.
	void update(const LandscapeStore& store, const glm::dvec3& cameraAbsolute, const LandscapeTileTerrainParams& params);

	const LandscapeGiHeightfield& heightfield() const { return m_hf; }
	const LandscapeGiStats&       stats() const { return m_stats; }
	void clear();

private:
	struct Result { uint64_t ticket = 0; LandscapeGiHeightfield hf; bool ok = false; };
	struct Shared
	{
		std::mutex          mutex;
		std::vector<Result> done;
		std::atomic<int>    running{ 0 };
		std::atomic<bool>   closed{ false };
	};

	void quiesce();

	LandscapeGiConfig       m_config;
	ThreadPool*             m_pool;
	std::shared_ptr<Shared> m_shared;
	CancelToken             m_token;
	JobHandle               m_job;
	LandscapeGiHeightfield  m_hf;
	LandscapeTileKey        m_lastOrigin{ -1, -1 };   // the window m_hf was built from
	int                     m_lastN = 0;
	LandscapeTileKey        m_wantOrigin{ -1, -1 };   // the region in flight, or the last one taken in
	int                     m_wantN = 0;
	uint64_t                m_ticket = 0;
	uint64_t                m_nextVersion = 1;
	bool                    m_building = false;
	LandscapeGiStats        m_stats;
};

} // namespace HE
