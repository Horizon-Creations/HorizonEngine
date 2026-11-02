#pragma once
#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace HE
{

// ── What a backend needs to draw the world-scale landscape (Thema 326, step 2, part B) ────────
// The data of HE::LandscapeFrame (HorizonScene) with the images already packed into bytes, in types the backends can see: they link
// HorizonCore only, not HorizonScene. A backend never reads a tile, a store or an atlas: it copies bytes into images and draws patches.
// Built by HE::LandscapeRuntime::buildRenderFrame, handed over through IRenderer::SetLandscapeProvider.
//
// The images (all unsigned integer or RGBA8, read with integer fetches, never filtered):
//   tile heights   R16UI array, `slotCapacity` layers of heightW x heightH (the mips of a tile in one image, see LandscapeGpuLayout.h)
//   tile weights   RGBA8 arrays, `weightPages` of them, weightW x weightH per layer, same mip layout
//   slot table     R16UI, tableWindow x tableWindow, toroidal, 65535 = tile not in the atlas
//   coarse heights R16UI, coarseW x coarseH, the coarse grids stacked (coarseGrid[g] = width, height, row offset)
//   coarse weights RGBA8 x `weightPages`, coarseWeightW x coarseWeightH
// Everything in LandscapeRenderStatic is created again when `generation` changes (a new landscape began); the uploads and table
// updates of a frame are applied in order on top of what is there.
struct LandscapeRenderStatic
{
	uint64_t generation = 0;

	float    tileM = 0.0f;
	float    heightMinM = 0.0f, heightScaleM = 0.0f;     // metres of a stored height 0 and of 65535 above it
	float    worldMaxX = 0.0f, worldMaxZ = 0.0f;         // absolute extent
	int      tilesX = 0, tilesZ = 0;
	int      hCells = 0, wTexels = 0;                    // height cells / weight texels per tile at level 0
	int      tileLevels = 0;
	int      tableWindow = 0;
	int      slotCapacity = 0;

	int      heightW = 0, heightH = 0;                   // one layer of the tile heights
	int      weightW = 0, weightH = 0;
	int      weightPages = 0;
	int      heightLevels = 0;
	int      heightRect[8][4] = {};                      // per mip: x, y (origin in the layer), w, h
	int      coarseW = 0, coarseH = 0;
	int      coarseGridCount = 0;
	int      coarseGrid[16][4] = {};                     // per coarse grid: width, height, row offset in the stacked image, 0
	int      coarseWeightW = 0, coarseWeightH = 0;
	std::vector<uint16_t>             coarseHeights;     // coarseW * coarseH
	std::vector<std::vector<uint8_t>> coarseWeights;     // [page], coarseWeightW * coarseWeightH * 4

	float    layerColor[12][4] = {};                     // the flat colour of each layer (used where the array below is missing)
	// World-UV ground (the material with world coordinates): layers 0-3 are the first four slices of this albedo texture array (asset path,
	// resolved by the backend like a material's texture array), repeated every `textureMeters` in absolute world metres. Empty = flat colours.
	std::string albedoArrayPath;
	float    textureMeters = 8.0f;

	// The one grid every patch is drawn with: (cells + 1)^2 vertices (i, j), triangles as indices, and the index ranges of the quadrants.
	int      patchCells = 64;
	std::vector<uint16_t> patchIndices;
	uint32_t quadrantFirst[4] = {};
	uint32_t quadrantCount[4] = {};
};

struct LandscapeRenderPatch
{
	int      level = 0;
	uint8_t  quadrants = 15;                             // bit q = dx + 2 * dz
	bool     coarse = false;
	int      gridIndex = 0;                              // coarse only
	int      cornerX = 0, cornerZ = 0;                   // the global lattice cell of the node's corner (node index * cells)
	float    x0 = 0.0f, z0 = 0.0f;                       // corner, origin-relative metres
	float    size = 0.0f;
	float    morphStart = 0.0f, morphEnd = 0.0f;
};

struct LandscapeRenderUpload
{
	uint16_t                          slot = 0;
	std::vector<uint16_t>             heights;           // heightW * heightH
	std::vector<std::vector<uint8_t>> pages;             // [page], weightW * weightH * 4
};

struct LandscapeRenderTableUpdate
{
	uint32_t index = 0;                                  // (z & (D - 1)) * D + (x & (D - 1))
	uint16_t slot = 0xFFFF;
};

struct LandscapeRenderFrame
{
	bool active = false;
	std::shared_ptr<const LandscapeRenderStatic> world;   // the same object as long as the landscape is the same
	std::vector<LandscapeRenderPatch>       patches;
	std::vector<LandscapeRenderUpload>      uploads;
	std::vector<LandscapeRenderTableUpdate> tableUpdates;
	glm::dvec3 origin{ 0.0 };                            // absolute = relative + origin
	glm::vec3  cameraRel{ 0.0f };                        // origin-relative
	float      slabDistance = 0.0f;                      // camera to the height slab: the vertical term of every morph distance
};

// What the application hands a renderer: called by the backend once per main scene frame with the view-projection it is about to
// use (origin-relative world to that backend's clip space) and whether its clip depth is 0..1. The application knows the world's
// floating origin, the backend does not (the render world carries origin-relative positions only), so the origin is the provider's.
using LandscapeProvider = std::function<void(const glm::dmat4& viewProjRelative, bool zeroToOne, LandscapeRenderFrame& out)>;

} // namespace HE
