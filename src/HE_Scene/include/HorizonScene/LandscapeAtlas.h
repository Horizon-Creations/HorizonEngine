#pragma once
#include "HorizonScene/LandscapeStore.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace HE
{

// ── Which GPU layer holds which tile (Thema 326, step 2, part A) ────────────────
// The renderer keeps the resident tiles' heights and weights in texture arrays, one layer per tile ("slot"): one array
// per mip level (heights: 257, 129, 65, 33, ... samples per side; weights: 128, 64, ... texels), all with the same slot
// count, so a tile has the same layer in every array. This class only does the bookkeeping, no GPU: it follows the store
// and says which tile to upload into which slot and which slots to give back.
//
// A patch of level 3 spans 2 x 2 tiles, so a draw cannot bind "its" layer. The vertex shader looks the slot up per vertex
// in a small table of the loaded window, addressed toroidally (tile (x, z) is entry (z & (D-1)) * D + (x & (D-1))): when
// the camera walks, only the entries of tiles that come and go change, and each change is one small upload.
// The window has to be wider than the loaded disc (D >= 2 * ceil(keep radius / tileM) + 2); sync() refuses a tile that would
// share an entry with another resident one and counts it in stats().aliased.
//
// A tile counts as "in the atlas" from the sync() that returns its upload: the renderer executes the uploads before it draws,
// so the quadtree may select patches of fine levels over it from then on (LandscapeTileResident = contains()).
class LandscapeAtlas
{
public:
	static constexpr uint16_t kNoSlot = 0xFFFF;

	struct Upload  { LandscapeTileKey key; uint16_t slot = 0; LandscapeStore::TilePtr tile; };
	struct Release { LandscapeTileKey key; uint16_t slot = 0; };
	struct TableUpdate { uint32_t index = 0; uint16_t slot = kNoSlot; };
	struct Stats
	{
		size_t   used = 0;
		uint64_t uploads = 0;
		uint64_t releases = 0;
		uint64_t deferred = 0;    // a tile waited for a later sync because maxUploads was reached
		uint64_t full = 0;        // no free slot (capacity below what the store keeps)
		uint64_t aliased = 0;     // the table window was too small for the loaded disc
	};

	// `capacity` slots (<= 65534); `window` is D, a power of two.
	LandscapeAtlas(size_t capacity, int window = 64);

	// Brings the atlas in line with the store: tiles the store no longer holds give their slot back, new ones get one (nearest
	// the anchor first is the caller's business: the store hands them over in no order, `maxUploads` caps the work per frame).
	// The vectors are cleared first. Slots released in this call may be reused by uploads in the same call.
	void sync(const LandscapeStore& store, std::vector<Upload>& uploads, std::vector<Release>& releases,
	          std::vector<TableUpdate>& table, size_t maxUploads = 8);

	bool     contains(LandscapeTileKey key) const { return m_slotOf.count(key) != 0; }
	uint16_t slotOf(LandscapeTileKey key) const;
	// The table entry of a tile, kNoSlot when it is not in the atlas (or the window holds another tile there).
	uint16_t lookup(int tx, int tz) const;
	const std::vector<uint16_t>& table() const { return m_table; }
	int      window() const { return m_window; }
	size_t   capacity() const { return m_capacity; }
	const Stats& stats() const { return m_stats; }

private:
	size_t   m_capacity;
	int      m_window;
	std::vector<uint16_t> m_table;                    // window x window slots
	std::vector<LandscapeTileKey> m_tableOwner;       // which tile an entry belongs to (to tell it from an alias)
	std::unordered_map<LandscapeTileKey, uint16_t, LandscapeTileKeyHash> m_slotOf;
	std::vector<uint16_t> m_free;
	Stats    m_stats;
};

} // namespace HE
