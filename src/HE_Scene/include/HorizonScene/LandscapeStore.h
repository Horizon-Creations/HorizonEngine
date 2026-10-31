#pragma once
#include "HorizonScene/LandscapeSource.h"

#include <JobSystem/JobSystem.h>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace HE
{

// ── World-scale landscape: the tile store and its streaming (Thema 326, step 1) ──
// Keeps the tiles around the anchors (the camera, a player) in memory and nothing else: tiles come in
// from a LandscapeTileSource on the job pool (low priority, so frame work overtakes them), the main
// thread only moves a finished tile into the resident map. No rendering, physics or scene in here;
// the renderer, physics and foliage (steps 2 and 3) read the resident tiles through tile().
//
//   wanted  : tiles within loadRadius of an anchor (or where it will be in lookaheadSec), nearest first,
//             never more than the byte budget holds — the effective radius shrinks instead of the budget.
//   kept    : tiles within keepFactor * loadRadius. A resident tile stays until it leaves this ring, which
//             stops an anchor on the border from loading and dropping a tile every frame (plan 3.6).
//   dropped : everything else. A load that has not started yet is cancelled, one that is running finishes
//             and its tile is thrown away.
//
// A tile is in the map once: Loading or Resident or Failed. A key that is Loading is never loaded a second
// time (its tile counts against the budget from the moment the load is scheduled), so resident + loading
// tiles x tile size can not pass the budget.
//
// Positions are absolute world coordinates (x east, z south; y is ignored). The store does not know the
// floating origin: callers hand it world position + origin, as CellStreamer is given.
struct LandscapeAnchor
{
	glm::dvec3 position{ 0.0 };
	glm::vec3  velocity{ 0.0f };   // m/s; the anchor also wants what it will be near in lookaheadSec
	float      radiusScale = 1.0f; // stretches loadRadius for this anchor; not positive counts as 1
};

struct LandscapeStoreConfig
{
	float  loadRadius   = 8000.0f;
	float  keepFactor   = 1.25f;                 // unload radius = loadRadius * keepFactor (>= 1)
	size_t byteBudget   = 350ull * 1024 * 1024;  // decoded tiles, resident + loading
	int    maxLevels    = -1;                    // mip levels decoded; < 0 all, 1 = level 0 only
	float  lookaheadSec = 2.0f;
	size_t maxInFlight  = 32;                    // loads scheduled at once
	// The wanted set is worked out again when an anchor (or its lookahead point) moved this far, in tiles.
	double replanTiles  = 0.25;
};

struct LandscapeStoreStats
{
	size_t   resident = 0;       // tiles in memory, decoded
	size_t   loading = 0;        // scheduled or running, including ones nobody wants any more
	size_t   failed = 0;         // keys the source could not give; tried again once they leave the kept ring
	size_t   wanted = 0;         // size of the wanted set at the last plan
	size_t   residentBytes = 0;
	size_t   peakBytes = 0;      // highest of residentBytes + loading * tileBytes seen at the end of an update
	size_t   maxTiles = 0;       // what the budget holds
	bool     budgetClamped = false;  // the wanted set was cut to fit the budget
	uint64_t loadsStarted = 0;
	uint64_t loadsFinished = 0;  // became resident
	uint64_t loadsFailed = 0;
	uint64_t loadsCancelled = 0; // never ran: dropped while queued
	uint64_t loadsDiscarded = 0; // ran, but nobody wanted the tile any more
	uint64_t evictions = 0;      // resident tiles dropped
	uint64_t plans = 0;
	double   lastUpdateMs = 0.0; // main-thread time of the last update()
	double   maxUpdateMs = 0.0;
	double   avgUpdateMs = 0.0;
	uint64_t updates = 0;
};

class LandscapeStore
{
public:
	using TilePtr = std::shared_ptr<const LandscapeTile>;

	LandscapeStore(std::shared_ptr<const LandscapeTileSource> source, LandscapeStoreConfig config = {},
	               ThreadPool* pool = nullptr);   // null: the global pool
	~LandscapeStore();
	LandscapeStore(const LandscapeStore&) = delete;
	LandscapeStore& operator=(const LandscapeStore&) = delete;

	// Main thread, once a frame. Takes in the tiles that finished (within `budgetMs`; the rest next time),
	// re-plans when an anchor moved, drops what left the kept ring, and starts loads for what is wanted.
	void update(const std::vector<LandscapeAnchor>& anchors, double budgetMs = 1.0);

	// The resident tile, or null. The pointer keeps the tile alive after it was evicted.
	TilePtr tile(LandscapeTileKey key) const;
	bool    isResident(LandscapeTileKey key) const;
	bool    isLoading(LandscapeTileKey key) const;
	std::vector<LandscapeTileKey> residentKeys() const;
	// True when every wanted tile is resident or failed and nothing is loading. False before the first update().
	bool    settled() const;

	// Tiles within the kept ring at the last plan (hysteresis set), for tests and the editor's view.
	bool    inKeptRing(LandscapeTileKey key) const { return m_keep.count(key) != 0; }

	const LandscapeStoreStats&  stats() const { return m_stats; }
	const LandscapeStoreConfig& config() const { return m_config; }
	// A changed config is applied at the next update (re-plans at once).
	void setConfig(const LandscapeStoreConfig& config);
	// Decoded bytes of one tile with the configured levels.
	size_t tileBytes() const { return m_tileBytes; }
	const LandscapeTileSource& source() const { return *m_source; }

	// Drops every tile and cancels what has not started. Waits for the loads that are running (they are short), so that when it
	// returns no job holds the source any more.
	void clear();

private:
	struct Entry
	{
		enum class State : uint8_t { Loading, Resident, Failed };
		State       state = State::Loading;
		uint64_t    ticket = 0;
		bool        unwanted = false;   // Loading, but left the kept ring while it ran
		double      dist = 0.0;         // to the nearest anchor at the last plan, scaled
		CancelToken cancel;
		JobHandle   job;
		TilePtr     tile;
	};
	struct Done
	{
		LandscapeTileKey key;
		uint64_t         ticket = 0;
		enum class Kind : uint8_t { Loaded, Failed, Cancelled } kind = Kind::Failed;
		TilePtr          tile;
	};
	struct Shared
	{
		std::mutex        mutex;
		std::vector<Done> done;
		std::atomic<int>  running{ 0 };      // load bodies that have started and not yet let go of the source
		std::atomic<bool> closed{ false };   // set by the destructor: a body that starts now does not touch the source
	};

	void   plan(const std::vector<LandscapeAnchor>& anchors);
	void   pump();
	void   drain(double budgetMs);
	void   quiesce();     // cancel what has not started, wait for what is running
	void   dropEntry(std::unordered_map<LandscapeTileKey, Entry, LandscapeTileKeyHash>::iterator it);
	bool   needsPlan(const std::vector<LandscapeAnchor>& anchors) const;
	size_t slotsUsed() const { return m_resident + m_loading; }

	std::shared_ptr<const LandscapeTileSource> m_source;
	LandscapeStoreConfig                       m_config;
	ThreadPool*                                m_pool = nullptr;
	std::shared_ptr<Shared>                    m_shared;
	CancelToken                                m_storeToken;

	std::unordered_map<LandscapeTileKey, Entry, LandscapeTileKeyHash> m_tiles;
	std::unordered_set<LandscapeTileKey, LandscapeTileKeyHash>        m_keep;
	std::vector<LandscapeTileKey>                                     m_wanted;   // nearest first

	std::vector<LandscapeAnchor> m_planAnchors;
	bool     m_havePlan = false;
	bool     m_dirty = true;
	size_t   m_tileBytes = 0;
	size_t   m_maxTiles = 0;
	size_t   m_resident = 0, m_residentBytes = 0, m_loading = 0, m_failed = 0;
	uint64_t m_nextTicket = 1;
	LandscapeStoreStats m_stats;
};

} // namespace HE
