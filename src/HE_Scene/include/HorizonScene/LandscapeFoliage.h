#pragma once
#include "HorizonScene/Components/FoliageComponent.h"
#include "HorizonScene/LandscapeStore.h"

#include <JobSystem/JobSystem.h>
#include <glm/vec3.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace HE
{

// ── Foliage on a tile landscape (Thema 326, step 3) ────────────────────────────────
// The single landscape scatters every layer over its whole area once. A 100 km world cannot: 25 000 km² at even 0.001 plants per m²
// are 25 million. Here each (tile, layer) is scattered on its own, only while the camera is within that layer's drawDistance of the
// tile, and dropped again when it is not: 80 m grass lives on one to four tiles, a tree layer with a 1 km distance on a few dozen.
//
// What is scattered is the engine's own scatter (FoliageSystem::scatterLayer) over the tile converted into a TerrainComponent
// (LandscapeTileTerrain.h), with the layer's own settings and rules (slope, height, weightLayer against the tile's channels). The seed
// of each tile is the layer's seed mixed with the tile's coordinates, so a tile that leaves and returns scatters the same plants, in
// whatever order the camera meets the tiles.
//
// NOT here: the painted density mask. FoliageLayer::densityMask is one 128 x 128 grid over a landscape's 0..1; over 100 km that is 780 m
// a texel and means nothing. Per-tile masks are authored data and come with the editor step; until then masks are ignored and the layers
// scatter by their rules alone.
//
// The result is one immutable FoliageStore per (tile, layer), terrain-local (centred on the tile) plus the tile's centre in the absolute
// world. The renderer takes them through the same bucket logic as the single landscape (FoliageExtract.cpp) with the tile's centre minus
// the floating origin as the terrain position: part of the renderer step. A store is never rebuilt behind the renderer's back (the LOD
// memory keys on its address): it stays the same object until its tile is dropped or the layers change.
//
// Scatters run on the job pool at low priority. Like the store, the object closes in its destructor: a job that has not started does nothing,
// and the running ones are waited for.
struct LandscapeFoliageConfig
{
	float  margin = 0.0f;            // metres added to a layer's drawDistance when deciding which tiles it needs
	float  keepFactor = 1.25f;       // a tile's scatter is dropped beyond (drawDistance + margin) * keepFactor
	size_t maxInFlight = 4;          // scatter jobs at once
	size_t maxStartsPerUpdate = 2;
};

struct LandscapeFoliageStats
{
	size_t   scattered = 0;          // (tile, layer) scatters held now
	size_t   pending = 0;            // jobs in flight
	size_t   instances = 0;          // plants in the held stores
	uint64_t started = 0;
	uint64_t finished = 0;
	uint64_t dropped = 0;            // scatters thrown away because the camera left
	uint64_t discarded = 0;          // finished after nobody wanted them
};

struct LandscapeFoliageDraw
{
	LandscapeTileKey                   tile;
	glm::dvec3                         centre{ 0.0 };   // the store's local origin, absolute
	size_t                             layer = 0;       // index into LandscapeFoliage::layers()
	std::shared_ptr<const FoliageStore> store;
};

class LandscapeFoliage
{
public:
	explicit LandscapeFoliage(LandscapeFoliageConfig config = {}, ThreadPool* pool = nullptr);
	~LandscapeFoliage();
	LandscapeFoliage(const LandscapeFoliage&) = delete;
	LandscapeFoliage& operator=(const LandscapeFoliage&) = delete;

	// The layers (a FoliageComponent's list, on the landscape's entity). Replaces the list and throws every scatter away; the
	// revision changes. A layer without a mesh, or not visible, scatters nothing.
	void setLayers(const std::vector<FoliageLayer>& layers);
	const std::vector<FoliageLayer>& layers() const { return m_layers; }
	uint32_t revision() const { return m_revision; }

	// Main thread, once a frame. `anchors`: absolute camera positions. Takes in the scatters that finished, drops those out of range and
	// starts new ones for tiles the store holds, nearest first.
	void update(const LandscapeStore& store, const std::vector<glm::dvec3>& anchors, float heightMinM, float heightScaleM);

	std::shared_ptr<const FoliageStore> storeOf(LandscapeTileKey tile, size_t layer) const;
	void draws(std::vector<LandscapeFoliageDraw>& out) const;
	const LandscapeFoliageStats& stats() const { return m_stats; }
	void clear();

private:
	struct Key
	{
		uint64_t tile = 0;
		uint32_t layer = 0;
		bool operator==(const Key& o) const { return tile == o.tile && layer == o.layer; }
	};
	struct KeyHash { size_t operator()(const Key& k) const { return static_cast<size_t>(k.tile * 1000003ull + k.layer); } };
	struct Entry
	{
		LandscapeTileKey tile;
		glm::dvec3  centre{ 0.0 };
		bool        done = false;
		uint64_t    ticket = 0;
		CancelToken cancel;
		JobHandle   job;
		std::shared_ptr<const FoliageStore> store;
		size_t      instances = 0;
	};
	struct Result { Key key; uint64_t ticket = 0; std::shared_ptr<const FoliageStore> store; size_t instances = 0; };
	struct Shared
	{
		std::mutex mutex;
		std::vector<Result> done;
		std::atomic<int> running{ 0 };
		std::atomic<bool> closed{ false };
	};

	void quiesce();
	static uint64_t pack(LandscapeTileKey k) { return (static_cast<uint64_t>(static_cast<uint32_t>(k.z)) << 32) | static_cast<uint32_t>(k.x); }

	LandscapeFoliageConfig m_config;
	ThreadPool* m_pool;
	std::vector<FoliageLayer> m_layers;
	uint32_t m_revision = 0;
	std::shared_ptr<Shared> m_shared;
	CancelToken m_token;
	std::unordered_map<Key, Entry, KeyHash> m_entries;
	uint64_t m_nextTicket = 1;
	LandscapeFoliageStats m_stats;
};

} // namespace HE
