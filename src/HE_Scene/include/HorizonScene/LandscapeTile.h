#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace HE
{

// ── World-scale landscape: one tile (Thema 326, step 1) ──────────────────────
// A world-scale landscape is a grid of square tiles (512 m by default). A tile holds
// the heights and the layer weights of its square, each as a mip pyramid that ends at
// the 128 m raster, so the far ring can be served from the coarse levels alone.
//
//   heights : uint16, (hCells >> k) + 1 samples per side at level k. The edge samples
//             are shared with the neighbour tile (level 0 has hCells + 1 = 257 per side
//             at 2 m), and the coarser levels are point-sampled, so a seam never opens.
//   weights : uint8, wTexels >> k texels per side, `channels` bytes per texel in
//             interleaved order (the engine's weight pages: 4 channels each, 3 pages
//             on 0.8.0). Coarser levels are the 2x2 mean.
//
// Rows run along +x, rows are stacked along +z; tile (x, z) covers
// [x * tileM, (x + 1) * tileM) of the absolute world.
//
// Not wired to TerrainComponent: that stays the single-landscape special case.
struct LandscapeGeometry
{
	float    tileM    = 512.0f;
	uint16_t hCells   = 256;   // level-0 height cells per side: raster = tileM / hCells (2 m)
	uint16_t wTexels  = 128;   // level-0 weight texels per side: raster = tileM / wTexels (4 m)
	uint8_t  channels = 12;    // weight channels per texel

	// The pyramids end at this raster, in metres.
	static constexpr float kCoarsestRasterM = 128.0f;

	bool    valid() const;     // powers of two, a coarse level exists, channels in 1..16
	int     heightLevels() const;
	int     weightLevels() const;
	int     heightDim(int level) const { return (hCells >> level) + 1; }
	int     weightDim(int level) const { return wTexels >> level; }
	// Bytes of the decoded tile for the first `levels` levels (< 0: all of them).
	size_t  heightBytes(int levels = -1) const;
	size_t  weightBytes(int levels = -1) const;
	size_t  decodedBytes(int levels = -1) const { return heightBytes(levels) + weightBytes(levels); }

	bool operator==(const LandscapeGeometry& o) const
	{
		return tileM == o.tileM && hCells == o.hCells && wTexels == o.wTexels && channels == o.channels;
	}
	bool operator!=(const LandscapeGeometry& o) const { return !(*this == o); }
};

struct LandscapeTile
{
	LandscapeGeometry                  geometry;
	std::vector<std::vector<uint16_t>> heights;   // [level]
	std::vector<std::vector<uint8_t>>  weights;   // [level]

	// Memory the tile holds (what the store's budget counts).
	size_t bytes() const;
	// Fills the coarser levels from level 0 (heights point-sampled, weights 2x2 mean).
	// Level 0 must already hold the geometry's sizes.
	void   buildMips();
	bool   operator==(const LandscapeTile& o) const
	{
		return geometry == o.geometry && heights == o.heights && weights == o.weights;
	}
};

// How a tile is stored. `predictor` turns the heights into residuals of a plane
// predictor (left + up - upper-left) and splits them into byte planes, and the weights
// into channel planes with a row delta: lossless, and about halves the height bytes.
struct LandscapeCodec
{
	enum class Backend : uint8_t { Store = 0, Lz4 = 1, Zstd = 2 };
	Backend backend   = Backend::Zstd;
	bool    predictor = true;
	int     zstdLevel = 3;

	// Whether this build can write and read the backend (Store always can).
	static bool available(Backend b);
	// zstd where the build has it, else LZ4, else Store; always with the predictor.
	static LandscapeCodec best();
};

// Container, little endian, format version 1:
//   u32 magic 'HLT1' (0x31544c48)   u8 version (1)   u8 backend   u8 flags (bit 0: predictor)
//   u8 heightLevels  u8 weightLevels  u8 channels  u16 hCells  u16 wTexels  f32 tileM   -> 18 bytes
//   then heightLevels + weightLevels streams, heights first, level 0 first:
//   u32 rawSize  u32 storedSize  storedSize bytes (compressed with `backend`)
// A reader rejects an unknown version, a geometry that is not valid, a stream whose rawSize is
// not what the geometry says, and a stored size that runs past the buffer. Never reads past `size`.
constexpr uint32_t kLandscapeTileMagic   = 0x31544c48u;
constexpr uint8_t  kLandscapeTileVersion = 1;
constexpr size_t   kLandscapeTileHeaderBytes = 18;

// A backend this build does not have falls back to Store (the header says what was used).
std::vector<uint8_t> encodeLandscapeTile(const LandscapeTile& tile, LandscapeCodec codec = LandscapeCodec::best());

// Decodes the first `maxLevels` levels of heights and of weights (< 0: all). On false `out` is
// unspecified and `error` (if given) says why.
bool decodeLandscapeTile(const uint8_t* data, size_t size, LandscapeTile& out, int maxLevels = -1,
                         std::string* error = nullptr);

// The geometry a blob was written with, without decoding it.
bool peekLandscapeTile(const uint8_t* data, size_t size, LandscapeGeometry& geometry,
                       LandscapeCodec* codec = nullptr, std::string* error = nullptr);

// ── Grids: the coarse world (one array for the whole world, not one per tile) ─────────────────────────
// Same transforms and backends as a tile, rectangular, in a small container of their own:
//   u32 magic 'HLG1' (0x31474c48)  u8 version (1)  u8 backend  u8 flags (bit 0: predictor)  u8 kind (1 heights, 2 weights)
//   u8 channels (weights; 1 for heights)  u8[3] 0  u32 width  u32 height  u32 rawSize  u32 storedSize   -> 28 bytes, then the payload
// A reader checks rawSize against width x height x (2 or channels), the stored size against the blob, and the kind.
std::vector<uint8_t> encodeLandscapeHeightGrid(const std::vector<uint16_t>& v, int w, int h, LandscapeCodec codec = LandscapeCodec::best());
bool decodeLandscapeHeightGrid(const uint8_t* data, size_t size, std::vector<uint16_t>& out, int& w, int& h, std::string* error = nullptr);
std::vector<uint8_t> encodeLandscapeWeightGrid(const std::vector<uint8_t>& v, int w, int h, int channels, LandscapeCodec codec = LandscapeCodec::best());
bool decodeLandscapeWeightGrid(const uint8_t* data, size_t size, std::vector<uint8_t>& out, int& w, int& h, int& channels, std::string* error = nullptr);

} // namespace HE
