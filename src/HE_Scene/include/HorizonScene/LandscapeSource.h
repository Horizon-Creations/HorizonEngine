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

	// The whole pyramid of one tile. Deterministic: the same key gives the same bytes.
	LandscapeTile generate(LandscapeTileKey key) const;

private:
	ProceduralLandscapeParams m_p;
};

// ── Pack file ────────────────────────────────────────────────────────────────
// One file for the whole world, little endian:
//   u32 magic 'HLP1' (0x31504c48)  u32 version (1)  u32 tilesX  u32 tilesZ
//   f32 tileM  u16 hCells  u16 wTexels  u8 channels  u8[3] 0                    -> 28 bytes, padded to 32
//   index: tilesX * tilesZ entries, row-major (z * tilesX + x), each  u64 offset  u32 size   (size 0: no tile)
//   then the tile blobs (LandscapeTile container, see LandscapeTile.h), back to back.
constexpr uint32_t kLandscapePackMagic   = 0x31504c48u;
constexpr uint32_t kLandscapePackVersion = 1;
constexpr size_t   kLandscapePackHeaderBytes = 32;
constexpr size_t   kLandscapePackIndexEntryBytes = 12;

// Writes a pack: tiles are appended in the order add() is called (any order, each key once), the index
// is written by finish(). Streams to disk, so a 4 GB world needs no 4 GB of memory.
class LandscapePackWriter
{
public:
	LandscapePackWriter() = default;
	~LandscapePackWriter();
	bool begin(const std::string& path, const LandscapeGeometry& geometry, int tilesX, int tilesZ);
	// `blob` is an encodeLandscapeTile() result; its geometry has to be the pack's.
	bool add(LandscapeTileKey key, const std::vector<uint8_t>& blob);
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
	bool has(LandscapeTileKey key) const;
	// Stored bytes of a tile, 0 for none.
	uint32_t storedSize(LandscapeTileKey key) const;

private:
	LandscapePackSource() = default;
	bool readAt(uint64_t offset, uint8_t* dst, size_t n) const;

	LandscapeGeometry     m_geometry;
	int                   m_tilesX = 0, m_tilesZ = 0;
	std::vector<uint64_t> m_offset;
	std::vector<uint32_t> m_size;
	uint64_t              m_fileSize = 0;
	int                   m_fd = -1;                 // POSIX pread: no lock between workers
	mutable std::mutex    m_mutex;                   // other platforms: one stream, one reader at a time
	mutable std::ifstream m_in;
};

} // namespace HE
