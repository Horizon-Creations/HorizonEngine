#pragma once
#include <cstdint>
#include <vector>

namespace HE
{

// ── The baked far image of a tile landscape (Thema 326) ────────────────────────────────────────────────────────
// Beyond the tile atlas the ground used to be shaded from the coarse world's weights, one texel per 128 m, through the whole layer blend.
// The far image is what that blend comes to, baked once at export: one RGBA8 image over the whole world (rgb = albedo, sRGB encoded,
// a = ambient occlusion), which the landscape shader reads with ONE filtered access for every pixel whose tile is not in the atlas.
//
// The unit that is baked, hashed and stored is a REGION of regionTiles x regionTiles tiles: an export bakes only the regions whose hash
// changed (LandscapeFarBake.h) and keeps the others from the previous table. The resolution is planned from a byte budget
// (planLandscapeFar): a small world gets the weights' own raster, a 100 km one is scaled down until it fits.
struct LandscapeFarLayout
{
	int tilesX = 0, tilesZ = 0;
	int regionTiles = 8;       // tiles a side of one region
	int texelsPerTile = 16;    // power of two, at most the tile's weight texels

	int  regionsX() const { return regionTiles > 0 ? (tilesX + regionTiles - 1) / regionTiles : 0; }
	int  regionsZ() const { return regionTiles > 0 ? (tilesZ + regionTiles - 1) / regionTiles : 0; }
	int  regionSide() const { return regionTiles * texelsPerTile; }   // texels a side of one region's image
	int  width() const { return tilesX * texelsPerTile; }             // the whole world's image
	int  height() const { return tilesZ * texelsPerTile; }
	bool valid() const
	{
		return tilesX > 0 && tilesZ > 0 && regionTiles > 0 && texelsPerTile > 0 && (texelsPerTile & (texelsPerTile - 1)) == 0;
	}
	bool operator==(const LandscapeFarLayout& o) const
	{
		return tilesX == o.tilesX && tilesZ == o.tilesZ && regionTiles == o.regionTiles && texelsPerTile == o.texelsPerTile;
	}
};

struct LandscapeFarRegion
{
	uint64_t             hash = 0;   // of everything the image was made from (LandscapeFarBake.h); only meaningful with an image
	std::vector<uint8_t> rgba;       // regionSide^2 * 4, row-major from the region's corner; texels past the world's edge are 0. Empty: not baked
};

class LandscapeFarTable
{
public:
	LandscapeFarLayout              layout;
	std::vector<LandscapeFarRegion> regions;   // regionsX * regionsZ, row-major (rz * regionsX + rx)

	// Nothing to draw from: no layout, or not one region baked.
	bool empty() const;
	// Little endian: u32 magic 'HLFM'  u32 version (1)  u32 tilesX  u32 tilesZ  u32 regionTiles  u32 texelsPerTile, then per region
	// u64 hash  u32 size  and `size` bytes of a weight grid blob (LandscapeTile.h, "Grids": 4 channels, regionSide squared); size 0 = not baked.
	std::vector<uint8_t> encode() const;
	// False for anything that is not a whole table; `out` is then empty.
	static bool decode(const std::vector<uint8_t>& bytes, LandscapeFarTable& out);

	// The world as one image with its mip chain down to 1 x 1 (level i is max(1, width >> i) wide), the 2 x 2 mean taken in linear light.
	// shortcut: every region resident in one image (39 MB for 100 km at 16 texels a tile), stream it per region with the coarse world when
	// worlds outgrow that.
	// `originTileX/Z`, `tilesX/Z`: only this window of tiles (clipped to the world); the default is the whole world.
	void assemble(int& width, int& height, std::vector<std::vector<uint8_t>>& mips,
	              int originTileX = 0, int originTileZ = 0, int tilesX = -1, int tilesZ = -1) const;
};

constexpr uint32_t kLandscapeFarMagic   = 0x4d464c48u;   // 'HLFM'
constexpr uint32_t kLandscapeFarVersion = 1;

// sRGB byte <-> linear, the pair the baker and assemble() share.
uint8_t landscapeFarEncodeSrgb(float linear);
float   landscapeFarDecodeSrgb(uint8_t srgb);

} // namespace HE
