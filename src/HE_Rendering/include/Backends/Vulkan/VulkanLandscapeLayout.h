#pragma once
#include <Renderer/LandscapeRenderFrame.h>
#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// ─── What the Vulkan landscape shares with its shaders (Thema 326, step 2, part B) ──────────────────
// The C++ mirror of the two blocks of kLandscapeVulkanVertexPrelude / kLandscapeVulkanFragmentPrelude (HorizonRendering/LandscapeShaderSource.h):
// the std140 uniform block LsGlobals (set 0, binding 0) and the push-constant block LsPatch. No Vulkan header in here, so he_tests can include
// it: test_landscape_shader reads the offsets glslang reports for the preludes and holds them against these. Every member is 16-byte aligned in
// both languages (a vec3 is followed by a member that starts a new row), so std140 puts nothing where this struct does not.
namespace HE::vkls
{

constexpr int kGridVerts = 65;   // the shared grid is (cells + 1)^2; the vertex buffer holds these (i, j) pairs as floats

struct alignas(16) Globals
{
	float   viewProj[16];        //   0  uViewProj   (kVulkanClipFix already applied: y down, depth 0..1)
	float   cameraRel[4];        //  64  uCameraRel
	float   world[4];            //  80  uWorld
	float   heightInfo[4];       //  96  uHeightInfo
	float   origin[4];           // 112  uOrigin (vec3 + pad)
	int32_t tile[4];             // 128  uTile
	int32_t heightRect[8][4];    // 144  uHeightRect
	int32_t coarseGrid[16][4];   // 272  uCoarseGrid
	float   layerColor[12][4];   // 528  uLayerColor
	float   sunDir[4];           // 720  uSunDir
	float   sunColor[4];         // 736  uSunColor
	float   ambient[4];          // 752  uAmbient
	float   tex[4];              // 768  uTex      x = 1 / metres of one texture tile, y = 1 when the albedo array is bound, zw = fract(origin.xz * x)
	float   texMaps[4];          // 784  uTexMaps  x = 1 when the normal array is bound, y = 1 when the mask array is bound
};
struct alignas(16) PatchPush
{
	int32_t patch[4];            //   0  uPatch
	float   patchPos[4];         //  16  uPatchPos
	float   morph[4];            //  32  uMorph
};
static_assert(offsetof(Globals, cameraRel) == 64 && offsetof(Globals, world) == 80 && offsetof(Globals, heightInfo) == 96
                  && offsetof(Globals, origin) == 112 && offsetof(Globals, tile) == 128 && offsetof(Globals, heightRect) == 144
                  && offsetof(Globals, coarseGrid) == 272 && offsetof(Globals, layerColor) == 528 && offsetof(Globals, sunDir) == 720
                  && offsetof(Globals, sunColor) == 736 && offsetof(Globals, ambient) == 752 && offsetof(Globals, tex) == 768
                  && offsetof(Globals, texMaps) == 784,
              "LsGlobals: the std140 block of the Vulkan prelude and this struct have to put every member at the same offset");
static_assert(sizeof(Globals) == 800, "LsGlobals: the std140 block is 800 bytes");
static_assert(offsetof(PatchPush, patchPos) == 16 && offsetof(PatchPush, morph) == 32 && sizeof(PatchPush) == 48,
              "LsPatch: the push-constant block is 48 bytes (the guaranteed minimum push-constant size is 128)");

// A world these shaders can draw: the 65 x 65 grid the vertex buffer holds, a power-of-two slot table, images that exist.
inline bool worldUsable(const HE::LandscapeRenderStatic& w)
{
	return w.patchCells == kGridVerts - 1 && w.slotCapacity > 0 && w.slotCapacity <= 65535 && w.tableWindow > 0 && (w.tableWindow & (w.tableWindow - 1)) == 0
	       && w.heightW > 0 && w.heightH > 0 && w.weightW > 0 && w.weightH > 0 && !w.patchIndices.empty();
}

// `albedoBound` / `normalBound` / `maskBound`: the renderer resolved LandscapeRenderStatic::albedoArrayPath / normalArrayPath / maskArrayPath to a
// real array (not the white stand-in). The normal and mask arrays only count with the albedo array, like on GL and Metal.
inline void fillGlobals(Globals& g, const HE::LandscapeRenderFrame& f, const glm::mat4& viewProj, const glm::vec3& sunDir, const glm::vec3& sunColor,
                        const glm::vec3& ambient, bool albedoBound, bool normalBound, bool maskBound)
{
	const HE::LandscapeRenderStatic& w = *f.world;
	g = Globals{};
	std::memcpy(g.viewProj, &viewProj[0][0], sizeof(g.viewProj));
	g.cameraRel[0] = f.cameraRel.x; g.cameraRel[1] = f.cameraRel.y; g.cameraRel[2] = f.cameraRel.z; g.cameraRel[3] = f.slabDistance;
	g.world[0] = w.worldMaxX; g.world[1] = w.worldMaxZ; g.world[2] = w.tileM; g.world[3] = w.heightScaleM;
	g.heightInfo[0] = w.heightMinM; g.heightInfo[1] = static_cast<float>(w.tilesX); g.heightInfo[2] = static_cast<float>(w.tilesZ);
	g.heightInfo[3] = static_cast<float>(w.tableWindow);
	g.origin[0] = static_cast<float>(f.origin.x); g.origin[1] = static_cast<float>(f.origin.y); g.origin[2] = static_cast<float>(f.origin.z);
	g.tile[0] = w.hCells; g.tile[1] = w.wTexels; g.tile[2] = w.tileLevels;
	for (int l = 0; l < w.heightLevels && l < 8; ++l)
		for (int c = 0; c < 4; ++c) g.heightRect[l][c] = w.heightRect[l][c];
	for (int i = 0; i < w.coarseGridCount && i < 16; ++i)
		for (int c = 0; c < 4; ++c) g.coarseGrid[i][c] = w.coarseGrid[i][c];
	std::memcpy(g.layerColor, w.layerColor, sizeof(g.layerColor));
	g.sunDir[0] = sunDir.x; g.sunDir[1] = sunDir.y; g.sunDir[2] = sunDir.z;
	g.sunColor[0] = sunColor.x; g.sunColor[1] = sunColor.y; g.sunColor[2] = sunColor.z;
	g.ambient[0] = ambient.x; g.ambient[1] = ambient.y; g.ambient[2] = ambient.z;
	// World UVs: a texture tile every `textureMeters` of absolute x/z. The shader works origin-relative, so the origin's part of the coordinate comes in
	// as a fraction formed here in double (a float at 100 km would be 8 mm coarse, a visible shimmer).
	const bool on = albedoBound && !w.albedoArrayPath.empty() && w.textureMeters > 0.0f;
	const double k = on ? 1.0 / static_cast<double>(w.textureMeters) : 1.0;
	g.tex[0] = static_cast<float>(k);
	g.tex[1] = on ? 1.0f : 0.0f;
	g.tex[2] = static_cast<float>(f.origin.x * k - std::floor(f.origin.x * k));
	g.tex[3] = static_cast<float>(f.origin.z * k - std::floor(f.origin.z * k));
	g.texMaps[0] = on && normalBound && !w.normalArrayPath.empty() ? 1.0f : 0.0f;
	g.texMaps[1] = on && maskBound && !w.maskArrayPath.empty() ? 1.0f : 0.0f;
}

inline void fillPatch(PatchPush& d, const HE::LandscapeRenderPatch& p)
{
	d.patch[0] = p.level; d.patch[1] = p.cornerX; d.patch[2] = p.cornerZ; d.patch[3] = p.coarse ? 1 : 0;
	d.patchPos[0] = p.x0; d.patchPos[1] = p.z0; d.patchPos[2] = p.size; d.patchPos[3] = static_cast<float>(p.gridIndex);
	d.morph[0] = p.morphStart; d.morph[1] = p.morphEnd; d.morph[2] = 0.0f; d.morph[3] = 0.0f;
}

} // namespace HE::vkls
