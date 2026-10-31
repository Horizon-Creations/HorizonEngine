#pragma once
#include "HorizonScene/LandscapeTile.h"

#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace HE
{

// ── The coarse world: always resident ────────────────────────────────────────
// What a level of the quadtree draws when its node spans more tiles than can be in memory (levels above
// LandscapeQuadtreeConfig::tileLevels). Heights are the tiles' own mip samples laid side by side, so a patch of
// the first coarse level meets the last tile level exactly: grid 0 is tile mip `heightLevel` (32 m for the default
// tile, 3137 x 3137 samples for 100 km), the next grids are point-sampled by 2, one for each coarser quadtree level (64 m, 128 m, 256 m ...). Weights are
// the tiles' coarsest mip (128 m, 4 x 4 texels a tile) side by side: blurry by design from about 6 km out.
struct LandscapeGrid16
{
	int                   w = 0, h = 0;
	std::vector<uint16_t> v;
	uint16_t at(int x, int z) const   // clamped: a patch at the world's edge reads the last row/column
	{
		x = x < 0 ? 0 : (x >= w ? w - 1 : x);
		z = z < 0 ? 0 : (z >= h ? h - 1 : z);
		return v[static_cast<size_t>(z) * w + x];
	}
};

struct LandscapeCoarseWorld
{
	int   tilesX = 0, tilesZ = 0;
	float tileM = 0.0f;
	int   heightLevel = 0;               // the tile mip grid 0 is made of
	int   cellsPerTile = 0;              // height cells a tile contributes on grid 0 (hCells >> heightLevel)
	std::vector<LandscapeGrid16> heights; // [i]: grid 0 point-sampled by 2^i
	int   weightLevel = 0;               // the tile mip the weights are made of
	int   texelsPerTile = 0;             // weight texels a tile contributes (wTexels >> weightLevel)
	int   channels = 0;
	int   weightsW = 0, weightsH = 0;
	std::vector<uint8_t> weights;        // weightsW x weightsH x channels, interleaved
	float cellM(int i) const { return tileM / static_cast<float>(cellsPerTile) * static_cast<float>(1 << i); }
	bool  valid() const { return !heights.empty() && cellsPerTile > 0 && tileM > 0.0f; }
	size_t bytes() const
	{
		size_t b = weights.size();
		for (const auto& g : heights) b += g.v.size() * 2;
		return b;
	}
};

// One tile's share of the coarse world, taken from a tile decoded with at least heightLevel + 1 levels.
struct LandscapeCoarseSample
{
	std::vector<uint16_t> heights;   // (cellsPerTile + 1)^2 samples of the tile's mip `heightLevel`, edges included
	std::vector<uint8_t>  weights;   // texelsPerTile^2 x channels of the tile's mip `weightLevel`
};

// Mip levels of the coarse world for a tile geometry: heights from mip `firstCoarseLevel` (tileLevels + 1), weights from the
// coarsest mip. Clamped to what the geometry has.
struct LandscapeCoarseLayout
{
	int heightLevel = 0, weightLevel = 0;
	static LandscapeCoarseLayout forGeometry(const LandscapeGeometry& g, int firstCoarseLevel = 4);
};
LandscapeCoarseSample landscapeCoarseSampleOf(const LandscapeTile& tile, const LandscapeCoarseLayout& layout);

// A tile of the world grid. (x, z) = (0, 0) is the corner at the absolute origin.
struct LandscapeTileKey
{
	int x = 0;
	int z = 0;
	bool operator==(const LandscapeTileKey& o) const { return x == o.x && z == o.z; }
	bool operator!=(const LandscapeTileKey& o) const { return !(*this == o); }
};

struct LandscapeTileKeyHash
{
	size_t operator()(const LandscapeTileKey& k) const
	{
		return static_cast<size_t>(static_cast<uint32_t>(k.x)) * 73856093u ^ static_cast<size_t>(static_cast<uint32_t>(k.z)) * 19349663u;
	}
};

// Where the tiles of a world come from. load() is called from the job pool, several tiles at a
// time: an implementation must be safe for that and must not touch the scene.
class LandscapeTileSource
{
public:
	virtual ~LandscapeTileSource() = default;

	virtual const LandscapeGeometry& geometry() const = 0;
	// The world is tilesX x tilesZ tiles; a key outside is not loaded.
	virtual int  tilesX() const = 0;
	virtual int  tilesZ() const = 0;
	// Decodes the first `maxLevels` levels (< 0: all). False: the tile is missing or unreadable.
	virtual bool load(LandscapeTileKey key, int maxLevels, LandscapeTile& out) const = 0;
	// Metres of a stored height 0 and of 65535 above it. False when the source does not say (the caller then needs its own).
	virtual bool heightRange(float& minM, float& scaleM) const { (void)minM; (void)scaleM; return false; }
	// The always-resident coarse world, or null when the source has none (a version 1 pack). Valid as long as the source.
	virtual const LandscapeCoarseWorld* coarse() const { return nullptr; }
};

// ── Procedural world ─────────────────────────────────────────────────────────
// The engine's fBm (terrainFbm) over the whole world, so a 100 km test world exists without any data:
// heights over the full 16 bit range, weights from height band x slope x noise, normalised to 255.
// This is the measurement and test source of Thema 326; a real world comes from a pack.
struct ProceduralLandscapeParams
{
	LandscapeGeometry geometry;
	int    tilesX = 196;          // 196 x 512 m = 100.4 km
	int    tilesZ = 196;
	int    seed = 1337;
	int    octaves = 12;
	float  baseFrequency = 6.0f;  // cycles over the world
	float  heightScale = 3000.0f; // metres for the full 16 bit range (only the weight slope uses it)
	int    roughLsb = 0;          // +-LSB white noise on the heights: real surveyed data is not noise-free
};

class ProceduralLandscapeSource final : public LandscapeTileSource
{
public:
	explicit ProceduralLandscapeSource(ProceduralLandscapeParams p) : m_p(p) {}
	const LandscapeGeometry& geometry() const override { return m_p.geometry; }
	int  tilesX() const override { return m_p.tilesX; }
	int  tilesZ() const override { return m_p.tilesZ; }
	bool load(LandscapeTileKey key, int maxLevels, LandscapeTile& out) const override;
	// Built on first use from every tile of the world (generate() each): fast for the small worlds of the tests, minutes
	// for 100 km. Real worlds read it from their pack.
	const LandscapeCoarseWorld* coarse() const override;
	bool heightRange(float& minM, float& scaleM) const override { minM = 0.0f; scaleM = m_p.heightScale; return true; }

	// The whole pyramid of one tile. Deterministic: the same key gives the same bytes.
	LandscapeTile generate(LandscapeTileKey key) const;

private:
	ProceduralLandscapeParams m_p;
	mutable std::once_flag    m_coarseOnce;
	mutable LandscapeCoarseWorld m_coarse;
};

// ── Pack file ────────────────────────────────────────────────────────────────
// One file for the whole world, little endian. Version 2 (written today); version 1 (no coarse world, 32-byte header) is still read.
//   u32 magic 'HLP1' (0x31504c48)  u32 version  u32 tilesX  u32 tilesZ
//   f32 tileM  u16 hCells  u16 wTexels  u8 channels  u8 coarseHeightLevel  u8 coarseWeightLevel  u8[5] 0      -> offset 0..31 (as version 1)
//   version 2 only: u64 coarseOffset  u32 coarseHeightSize  u32 coarseWeightSize  f32 heightMinM  f32 heightScaleM  u8[36] 0   -> offset 32..63
//   (metres of a stored height 0 and of 65535 above it; scale 0 = not said)
//   index: tilesX * tilesZ entries, row-major (z * tilesX + x), each  u64 offset  u32 size   (size 0: no tile)  -> right after the header
//   then the tile blobs (LandscapeTile container, see LandscapeTile.h), back to back,
//   then the coarse world: a height grid blob and a weight grid blob (LandscapeTile.h, "Grids"), at coarseOffset.
// coarseHeightLevel 255 = the pack has no coarse world. Grid sizes: heights (tilesX * (hCells >> level) + 1) x (tilesZ * ...),
// weights (tilesX * (wTexels >> level)) x (tilesZ * ...).
constexpr uint32_t kLandscapePackMagic   = 0x31504c48u;
constexpr uint32_t kLandscapePackVersion = 2;
constexpr size_t   kLandscapePackHeaderBytesV1 = 32;
constexpr size_t   kLandscapePackHeaderBytes   = 64;   // version 2
constexpr size_t   kLandscapePackIndexEntryBytes = 12;

// Writes a pack: tiles are appended in the order add() is called (any order, each key once), the index
// is written by finish(). Streams to disk, so a 4 GB world needs no 4 GB of memory.
class LandscapePackWriter
{
public:
	LandscapePackWriter() = default;
	~LandscapePackWriter();
	// `firstCoarseLevel`: the tile mip the coarse world's heights are made of (the quadtree's tileLevels + 1); < 0 writes no coarse world.
	// `heightMinM` / `heightScaleM`: metres of a stored height 0 and of 65535 above it, kept in the header (0 scale = not said).
	bool begin(const std::string& path, const LandscapeGeometry& geometry, int tilesX, int tilesZ, int firstCoarseLevel = 4,
	           float heightMinM = 0.0f, float heightScaleM = 0.0f);
	// `blob` is an encodeLandscapeTile() result; its geometry has to be the pack's. `coarse`: this tile's share of the coarse
	// world (landscapeCoarseSampleOf); without it the writer decodes the blob to get it, which costs a tile decode.
	bool add(LandscapeTileKey key, const std::vector<uint8_t>& blob, const LandscapeCoarseSample* coarse = nullptr);
	bool finish();
	size_t tilesWritten() const { return m_written; }

private:
	std::ofstream         m_out;
	LandscapeGeometry     m_geometry;
	int                   m_tilesX = 0, m_tilesZ = 0;
	std::vector<uint64_t> m_offset;
	std::vector<uint32_t> m_size;
	uint64_t              m_pos = 0;
	size_t                m_written = 0;
	bool                  m_open = false;
	LandscapeCoarseLayout m_layout;
	float                 m_heightMinM = 0.0f, m_heightScaleM = 0.0f;
	bool                  m_withCoarse = false;
	LandscapeCoarseWorld  m_coarse;     // grid 0 and the weights; the point-sampled grids are made when it is read
	std::vector<uint8_t>  m_coarseSeen; // per tile
};

// Reads a pack. The index stays in memory (12 bytes a tile: 460 KB for 100 km), the file stays open.
class LandscapePackSource final : public LandscapeTileSource
{
public:
	// Null for a file that is missing, not a pack, or whose index runs past its end.
	static std::shared_ptr<LandscapePackSource> open(const std::string& path, std::string* error = nullptr);
	~LandscapePackSource() override;

	const LandscapeGeometry& geometry() const override { return m_geometry; }
	int  tilesX() const override { return m_tilesX; }
	int  tilesZ() const override { return m_tilesZ; }
	bool load(LandscapeTileKey key, int maxLevels, LandscapeTile& out) const override;
	const LandscapeCoarseWorld* coarse() const override { return m_coarse.valid() ? &m_coarse : nullptr; }
	int  version() const { return m_version; }
	bool heightRange(float& minM, float& scaleM) const override { minM = m_heightMinM; scaleM = m_heightScaleM; return m_heightScaleM > 0.0f; }
	bool has(LandscapeTileKey key) const;
	// Stored bytes of a tile, 0 for none, and where in the file they start (for tools that read the file themselves).
	uint32_t storedSize(LandscapeTileKey key) const;
	uint64_t storedOffset(LandscapeTileKey key) const;

private:
	LandscapePackSource() = default;
	bool readAt(uint64_t offset, uint8_t* dst, size_t n) const;

	LandscapeGeometry     m_geometry;
	int                   m_tilesX = 0, m_tilesZ = 0;
	int                   m_version = 0;
	float                 m_heightMinM = 0.0f, m_heightScaleM = 0.0f;
	LandscapeCoarseWorld  m_coarse;
	std::vector<uint64_t> m_offset;
	std::vector<uint32_t> m_size;
	uint64_t              m_fileSize = 0;
	int                   m_fd = -1;                 // POSIX pread: no lock between workers
	mutable std::mutex    m_mutex;                   // other platforms: one stream, one reader at a time
	mutable std::ifstream m_in;
};

} // namespace HE
