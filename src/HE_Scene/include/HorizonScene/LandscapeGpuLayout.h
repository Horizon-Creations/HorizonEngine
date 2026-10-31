#pragma once
#include "HorizonScene/LandscapeSource.h"
#include "HorizonScene/LandscapeTile.h"

#include <cstdint>
#include <vector>

namespace HE
{

// ── How landscape data sits in GPU images (Thema 326, step 2, part A) ───────────────
// The vertex shader reads heights with texelFetch at integer lattice positions, so the layout is a contract, not a detail.
// These functions say where every sample goes and fill the byte arrays a backend copies into its textures; the shader
// (LandscapeShaderSource.h) gets the same rectangles as uniforms. All images are unsigned integers, read without filtering.
//
// Tile heights (R16UI, one array layer per atlas slot): the mips of a tile are placed in ONE image, level 0 at the top left and
// the other levels in a column right of it, one below the other. For the default tile that is 386 x 258 texels for 88 004 samples,
// about 13 % padding, where a common 257-wide column per level would cost 100 %:
//
//   +----------------+----+
//   |                | L1 |      level 0: 257 x 257 at (0, 0)       level 1: 129 x 129 at (257, 0)
//   |      L0        +----+      level 2:  65 x  65 at (257, 129)    level 3:  33 x  33 at (257, 194)  ...
//   |                | L2 |
//   +----------------+----+
//
// Tile weights (RGBA8, three arrays "pages", one layer per slot): the same mip layout, channel 4p + c in page p channel c,
// missing channels zero. The coarse world: its height grids stacked in one R16UI image, each grid starting at a row
// offset; its weights as three RGBA8 images of the finest weight grid (the coarse world keeps only that one).
struct LandscapeImageRect
{
	int x = 0, y = 0, w = 0, h = 0;
};

struct LandscapeTileImageLayout
{
	int                              heightW = 0, heightH = 0;     // size of one layer
	int                              weightW = 0, weightH = 0;
	std::vector<LandscapeImageRect>  heightRect;                   // [level]
	std::vector<LandscapeImageRect>  weightRect;
	int                              weightPages = 0;              // ceil(channels / 4)
	static LandscapeTileImageLayout forGeometry(const LandscapeGeometry& g);
};

// Image bytes of one tile for one atlas layer. `heights` is heightW * heightH uint16; `pages[p]` weightW * weightH * 4 bytes.
// Levels the tile does not carry (decoded with fewer) stay zero. Sized by the layout, overwritten completely.
void landscapePackTile(const LandscapeTile& tile, const LandscapeTileImageLayout& layout, std::vector<uint16_t>& heights,
                       std::vector<std::vector<uint8_t>>& pages);

struct LandscapeCoarseImageLayout
{
	struct Grid { int w = 0, h = 0, rowOffset = 0; };
	int               width = 0, height = 0;    // the stacked height image
	std::vector<Grid> grids;
	int               weightW = 0, weightH = 0; // the coarse weights
	int               weightPages = 0;
	static LandscapeCoarseImageLayout forCoarse(const LandscapeCoarseWorld& c);
};

void landscapePackCoarse(const LandscapeCoarseWorld& c, const LandscapeCoarseImageLayout& layout, std::vector<uint16_t>& heights,
                         std::vector<std::vector<uint8_t>>& pages);

} // namespace HE
