#pragma once
#include "HorizonScene/Components/TerrainComponent.h"
#include "HorizonScene/LandscapeSource.h"
#include "HorizonScene/LandscapeTileTerrain.h"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace HE
{

class LandscapeRuntime;

// ── Sculpt and paint on a tile landscape that write back (Thema 326) ──────────────────────────────────────────�
// A pack is write-once (fixed index, blobs back to back), so an edit never touches it. Three pieces, design in
// docs/landscape-world-scale-sculpt-writeback-design-2026-11-06.md:
//
//   LandscapeEditSource       an overlay in front of any source: edited tiles answer from memory, the rest from the base, and the coarse
//                             world is a patched copy. Saved as a sidecar file beside the pack ("<pack>.edits"), which every runtime reads.
//   landscapeWriteRegionTerrain   the inverse of landscapeRegionTerrain: an edited region terrain back into tiles.
//   LandscapeEditSession      what a tool drives: put the brush somewhere, edit the TerrainComponent it hands out with the brushes of the
//                             single landscape (TerrainSculpt, TerrainPaint), commit. Strokes are undo steps.

using LandscapeTilePtr = std::shared_ptr<const LandscapeTile>;

// A heightmap laid over the ground, on top of the tiles and their brush edits: a picture of heights with a place, a size and a turn.
// Nothing is written anywhere: a tile under a stamp gets it added when the tile is LOADED (LandscapeEditSource::load), so a stamp that
// is not moved costs nothing per frame, and moving one is "these tiles changed". The brushes keep editing the ground underneath.
// The editor makes these from entities (LandscapeStamps.h); an export bakes them into the shipped pack like brush edits.
struct LandscapeStamp
{
	std::shared_ptr<const std::vector<float>> image;   // w * h values in 0..1, row 0 at the stamp's -Z edge
	int    w = 0, h = 0;
	double x = 0.0, z = 0.0;                           // centre, absolute metres
	float  sizeX = 256.0f, sizeZ = 256.0f;             // metres
	float  yaw = 0.0f;                                 // radians around +Y
	float  heightM = 50.0f;                            // what a value of 1 adds; negative digs
	float  falloff = 0.2f;                             // share of the way from the edge to the centre over which the stamp fades in
	bool operator==(const LandscapeStamp& o) const
	{
		return image == o.image && w == o.w && h == o.h && x == o.x && z == o.z && sizeX == o.sizeX && sizeZ == o.sizeZ && yaw == o.yaw
		    && heightM == o.heightM && falloff == o.falloff;
	}
};

class LandscapeEditSource final : public LandscapeTileSource
{
public:
	explicit LandscapeEditSource(std::shared_ptr<const LandscapeTileSource> base);

	const LandscapeGeometry& geometry() const override { return m_base->geometry(); }
	int  tilesX() const override { return m_base->tilesX(); }
	int  tilesZ() const override { return m_base->tilesZ(); }
	bool load(LandscapeTileKey key, int maxLevels, LandscapeTile& out) const override;   // job pool: the map is behind a mutex
	bool heightRange(float& minM, float& scaleM) const override { return m_base->heightRange(minM, scaleM); }
	const LandscapeCoarseWorld* coarse() const override { return m_coarse.valid() ? &m_coarse : m_base->coarse(); }
	// The base's window with the edited tiles inside it laid over. For a base that has a whole coarse world this is a crop of the
	// patched copy; for one that has none (a pack in coarse regions) the window is patched as it is asked for.
	std::shared_ptr<const LandscapeCoarseWorld> coarseWindow(LandscapeTileKey origin, int tilesX, int tilesZ) const override;
	// The base's water table, or the overlay's own once a water brush changed it (setWaterTable): saved in the edits file, baked
	// into the pack with the tiles.
	const LandscapeWaterTable* water() const override { return m_water ? m_water.get() : m_base->water(); }
	void setWaterTable(LandscapeWaterTable table);
	bool waterEdited() const { return m_water != nullptr; }
	const LandscapeHlodTable* hlod() const override { return m_base->hlod(); }
	const LandscapeFarTable* farImage() const override { return m_base->farImage(); }
	const LandscapeTileSource& base() const { return *m_base; }

	// Main thread. A tile with every level (buildMips); null takes the edit away (the base's tile shows again). The coarse copy is
	// patched for the tiles set (a removed one from the base, which costs a tile load), unless `patchCoarse` is false: a stroke in
	// progress sets tiles dab by dab and patches once, at its end (the chain above grid 0 is rebuilt whole each time).
	void set(const std::vector<std::pair<LandscapeTileKey, LandscapeTilePtr>>& tiles, bool patchCoarse = true);
	LandscapeTilePtr edited(LandscapeTileKey key) const;
	std::vector<LandscapeTileKey> editedKeys() const;
	bool dirty() const { return m_dirty; }   // edits not saved yet

	// Stamps (LandscapeStamp). Main thread. Replaces them all and returns the tiles whose content that changes: the footprints of every
	// stamp that came, went or changed (hand them to LandscapeRuntime::tileChanged). The coarse copy is patched for those tiles.
	// `heightScaleM`: metres of a stored 65535 above a stored 0, used when the base does not say (heightRange).
	// `patchCoarse` false leaves the coarse copy (the far levels) as it was: the caller hands the keys to patchCoarseFor later.
	std::vector<LandscapeTileKey> setStamps(std::vector<LandscapeStamp> stamps, float heightScaleM, bool patchCoarse = true);
	void patchCoarseFor(const std::vector<LandscapeTileKey>& keys);
	size_t stampCount() const;
	bool   stamped(LandscapeTileKey key) const;
	std::vector<LandscapeTileKey> stampedKeys() const;
	// `tile` (this key's tile without stamps) as it is shown: with the stamps over it, or the same pointer when none reaches it.
	LandscapeTilePtr shown(LandscapeTileKey key, LandscapeTilePtr tile) const;
	// A number that changes when the tile's content does: of the edited tile's encoded bytes, else `baseHash(key)`.
	uint64_t contentHash(LandscapeTileKey key, const std::function<uint64_t(LandscapeTileKey)>& baseHash) const;

	// Which world the edits belong to (LandscapePackSource::identity()), written into the edits file and checked when one is loaded: a pack
	// that was generated, migrated or imported again under the same path is another world, and the old file's tiles must not land on it.
	// 0 (the default, and what a file without a stamp says) is not checked.
	void     setWorldId(uint64_t id) { m_worldId = id; }
	uint64_t worldId() const { return m_worldId; }

	// The sidecar: little endian, u32 magic 'HLE1'  u32 version (1)  u32 count  u64 worldId, then per tile  i32 x  i32 z  u32 size  and
	// `size` bytes of an encodeLandscapeTile blob. Written beside and renamed over; an empty overlay removes the file. Loading keeps every
	// whole record of the pack's geometry and stops at the first that is not (a file cut short by a crash loses its tail, not the rest);
	// a file stamped for another world gives nothing.
	// shortcut: the whole file again on every save (a megabyte a tile), make it an append-only log when a world has thousands of edited tiles.
	bool save(const std::filesystem::path& file, std::string* error = nullptr);
	size_t loadFile(const std::filesystem::path& file);   // tiles taken in

private:
	std::shared_ptr<const LandscapeTileSource> m_base;
	mutable std::mutex m_mutex;
	std::map<std::pair<int, int>, LandscapeTilePtr> m_tiles;
	LandscapeCoarseWorld m_coarse;   // a copy of the base's, made at the first edit
	bool m_dirty = false;
	uint64_t m_worldId = 0;
	std::shared_ptr<const LandscapeWaterTable> m_water;                     // null: the base's
	// shortcut: every table a stroke replaced stays alive (a water job may still read it through water()'s pointer); a few hundred
	// bytes a stroke, hand out shared pointers when sessions paint thousands of lakes
	std::vector<std::shared_ptr<const LandscapeWaterTable>> m_waterRetired;
	// Never changed once made (a job-pool load holds its own reference): the stamps and, per tile, which of them reach it.
	struct StampSet
	{
		std::vector<LandscapeStamp> stamps;
		std::map<std::pair<int, int>, std::vector<uint32_t>> tiles;
		float heightScaleM = 0.0f;
	};
	std::shared_ptr<const StampSet> m_stampSet;
	std::shared_ptr<const StampSet> stampSet() const;
	std::vector<LandscapeTileKey> footprint(const LandscapeStamp& s) const;
	void applyStamps(const StampSet& set, LandscapeTileKey key, LandscapeTile& tile) const;
};

// Where a pack's edits live.
inline std::filesystem::path landscapeEditsFile(const std::filesystem::path& pack) { return pack.string() + ".edits"; }

// The region terrain (landscapeRegionTerrain: n x n tiles from `origin`, absolute metres, three weight pages) back into whole tiles.
// `current(key)` is each tile as it is now: its water rasters are carried over, and a tile whose level 0 comes out the same is left out
// of `out`. Heights are quantised to 16 bit (heightMinM + v / 65535 * heightScaleM, clamped); a sample two tiles share is written to
// both. False when the terrain is not an n x n region of this geometry or a tile is missing. The tiles in `out` have all their mips.
bool landscapeWriteRegionTerrain(const TerrainComponent& terrain, const LandscapeGeometry& geometry, LandscapeTileKey origin, int n,
                                 const LandscapeTileTerrainParams& params,
                                 const std::function<LandscapeTilePtr(LandscapeTileKey)>& current,
                                 std::vector<std::pair<LandscapeTileKey, LandscapeTilePtr>>& out,
                                 const std::vector<uint8_t>* foliageDensity = nullptr);   // the region's (LandscapeRegionTerrain), written back too

// A streaming rewrite of `packPath` with the overlay's tiles in place of the pack's (water table, cell HLOD and far image carried over):
// what "bake the edits" and the export's copy do. Written beside and renamed over. `edits` must be an overlay of this pack's world.
// shortcut: every unchanged tile is decoded once for its share of the coarse world (seconds for 100 km), hand the writer the stored samples
// when that shows up in an export's time.
bool landscapePackBakeEdits(const std::string& packPath, const LandscapeEditSource& edits, std::string* error = nullptr);

class LandscapeEditSession
{
public:
	// The runtime must run on `edits` (LandscapeRuntime::edits()). Both outlive the session.
	LandscapeEditSession(LandscapeRuntime& runtime, LandscapeEditSource& edits);

	// Puts the editing region where a brush of `radiusM` (falloff included) at absolute (x, z) fits with a sample to spare: 1, 2 or 4 tiles
	// a side (never 3: a TerrainComponent's resolution is 2^k + 1). A brush that already fits the current region keeps it, so a stroke does
	// not re-assemble per dab; otherwise what was edited is committed first. False: the brush is too large (more than about 1.9 tiles
	// across), outside the world, or a tile cannot be read.
	bool focus(double absX, double absZ, double radiusM);
	bool hasRegion() const { return m_n > 0; }
	// The region as a terrain: centred on centre(), heights in absolute metres. Edit it with the single landscape's brushes, then commit().
	TerrainComponent& terrain() { return m_region.terrain; }
	// The region's painted foliage density (LandscapeRegionTerrain::foliageDensity, foliageRes() a side): paint it, then commit().
	std::vector<uint8_t>& foliageDensity() { return m_region.foliageDensity; }
	uint32_t foliageRes() const { return m_region.foliageRes; }
	const glm::dvec3& centre() const { return m_region.centre; }
	// Metres of a stored height 0 and of 65535 above it: what a height is quantised to when it is written back.
	const LandscapeTileTerrainParams& params() const { return m_params; }
	// Ground height at an absolute position inside the region (the brushes' own bilinear rule); false outside.
	bool heightAt(double absX, double absZ, float& outM) const;

	// A stroke is one undo step, however many commits it has.
	void beginStroke();
	// Writes what changed in the region back: into the overlay, and into the store in place (the atlas uploads the tile again into its
	// slot, nothing blinks). Returns the tiles that changed. Cheap enough per dab (at most 16 tiles compared).
	size_t commit();
	// Closes the stroke: the coarse world takes the changed tiles (the far levels), and the tiles are dropped from the store once so
	// that everything built from a tile (ground bodies, foliage, navigation, water sheets) is built again from the new one.
	void endStroke();

	bool canUndo() const { return m_undoAt > 0; }
	bool canRedo() const { return m_undoAt < m_steps.size(); }
	bool undo();
	bool redo();

private:
	struct Change { LandscapeTileKey key; LandscapeTilePtr before, after; };   // before null: the tile was not edited yet
	using Step = std::vector<Change>;
	void apply(const std::vector<std::pair<LandscapeTileKey, LandscapeTilePtr>>& tiles, bool finish);
	LandscapeTilePtr read(LandscapeTileKey key) const;

	LandscapeRuntime&    m_runtime;
	LandscapeEditSource& m_edits;
	LandscapeRegionTerrain m_region;
	mutable std::map<std::pair<int, int>, LandscapeTilePtr> m_tiles;   // the region's tiles as they are now: a dab compares against these, not against a fresh load
	int   m_n = 0;
	LandscapeTileTerrainParams m_params;
	bool  m_inStroke = false;
	Step  m_stroke;
	std::vector<Step> m_steps;
	size_t m_undoAt = 0;
	static constexpr size_t kMaxSteps = 32;
};

} // namespace HE
