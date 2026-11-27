#pragma once
#include "HorizonScene/LandscapeFarMap.h"
#include "HorizonScene/LandscapeSource.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace HE
{

// ── Baking the far image (Thema 326) ──────────────────────────────────────────────────────────────────────�
// bakeLandscapeFar walks the regions of a world. For each it hashes what the image is made from (the stored bytes of the region's tiles,
// the painter's settings, the layout); a region whose hash is the one in `previous` keeps that image, every other one is painted again.
// That is "only the chunks that changed": a second export of an untouched world paints nothing.
//
// WHAT is painted is the one swappable piece (LandscapeFarPainter). The first painter, landscapeFarWeightPainter, bakes exactly what the
// tile shader (kLandscapeFS) draws: the tile's weights times the colour of each layer, where the colour of a textured layer is the mean of
// its array slice (what a filtered access returns once a texel covers many repeats). So the far image and the near ground agree by
// construction. A painter that renders the real landscape material (the material graph, RVT pages) takes its place when the tile world
// draws with that material; nothing else here changes.
struct LandscapeFarPaint
{
	std::array<glm::vec3, 12> layerColor{};   // linear albedo of each layer
	std::array<float, 12>     layerAo{};      // ambient occlusion of each layer, 1 = none
};

// Fills `rgba` (layout.regionSide()^2 * 4) for region (rx, rz). False: the region could not be painted now (it keeps the image `previous`
// has for it, with that image's hash, so the next bake tries again; without one it stays unbaked).
using LandscapeFarPainter = std::function<bool(const LandscapeTileSource& source, const LandscapeFarLayout& layout, int rx, int rz,
                                               std::vector<uint8_t>& rgba)>;
LandscapeFarPainter landscapeFarWeightPainter(const LandscapeFarPaint& paint);
uint64_t            landscapeFarPaintHash(const LandscapeFarPaint& paint);

// A number that changes when the tile's content does. For a pack: landscapePackTileHash (the stored bytes).
using LandscapeFarTileHash = std::function<uint64_t(LandscapeTileKey)>;
LandscapeFarTileHash landscapePackTileHash(const LandscapePackSource& pack);

struct LandscapeFarBakeOptions
{
	int      regionTiles = 8;
	uint64_t budgetBytes = 128ull << 20;   // of the assembled image with its mips: the resolution is scaled down until it fits
	int      maxSide = 8192;               // texels a side of the assembled image (what every backend's 2D texture takes)
	LandscapeFarPainter painter;
	uint64_t painterHash = 0;              // the painter's settings, part of every region's hash
	// A second painter whose regions are as good as this one's (0 = none): a region of `previous` whose hash is the one it would have with
	// this painter hash is kept too. The export paints with the weights but keeps what the editor baked from the material.
	uint64_t keepPainterHash = 0;
	std::function<bool(int done, int total)> progress;   // after every region; false cancels
};

struct LandscapeFarBakeStats
{
	int  regions = 0;      // regions of the world
	int  baked = 0;        // painted in this run
	int  reused = 0;       // kept from `previous` (hash unchanged)
	int  failed = 0;       // the painter said no and `previous` had nothing: unbaked
	int  deferred = 0;     // the painter said no and the region kept its image from `previous` (stale)
	bool cancelled = false;
};

// The layout a world gets: the finest power-of-two texels per tile (at most the weights' own) whose assembled image stays inside the byte
// budget and `maxSide`. Never below the coarsest weight mip a tile stores (the 128 m raster), which is the last level a painter can read.
LandscapeFarLayout planLandscapeFar(const LandscapeGeometry& geometry, int tilesX, int tilesZ, uint64_t budgetBytes, int maxSide, int regionTiles);

// `previous` may be null or of another layout (then everything is painted). On a cancel `out` holds what was done so far, the rest unbaked.
void bakeLandscapeFar(const LandscapeTileSource& source, const LandscapeFarTileHash& tileHash, const LandscapeFarBakeOptions& options,
                      const LandscapeFarTable* previous, LandscapeFarTable& out, LandscapeFarBakeStats& stats);

} // namespace HE
