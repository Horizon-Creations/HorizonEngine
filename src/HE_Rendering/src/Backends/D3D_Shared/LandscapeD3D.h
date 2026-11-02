#pragma once
#include <Renderer/LandscapeRenderFrame.h>
#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>

// ─── What the D3D11 and D3D12 landscape classes share (Thema 326, step 2, part B) ─────────────────
// The C++ mirror of the two constant buffers of kLandscapeHLSL (HorizonRendering/LandscapeShaderSource.h) and the code that fills them from
// a HE::LandscapeRenderFrame. Every member is 16-byte aligned in both languages (float4x4, float4, int4), so the HLSL packing rules put
// nothing where this struct does not (a static_assert on the sizes holds the two apart).
namespace HE::d3dls
{

constexpr unsigned kFirstSrv = 32;   // t32 .. t41, above everything the D3D11 renderer and its graph materials bind
constexpr unsigned kSrvCount = 10;   // tile heights, slot table, coarse heights, three tile weight pages, three coarse weight pages, the albedo array (not ours)
constexpr unsigned kImageCount = 9;  // the images the landscape class owns: the first nine
constexpr unsigned kSamplerReg = 13; // s13: the albedo array's sampler (linear, repeat, mips)
constexpr unsigned kGlobalsReg = 12; // b12 (LsGlobals), b13 (LsPatch)
constexpr unsigned kPatchReg = 13;
constexpr int      kGridVerts = 65;  // the grid is (cells + 1)^2; the shader derives (i, j) from SV_VertexID with this

struct alignas(16) Globals
{
	float   viewProj[16];
	float   cameraRel[4];
	float   world[4];
	float   heightInfo[4];
	float   origin[4];
	int32_t tile[4];
	int32_t heightRect[8][4];
	int32_t coarseGrid[16][4];
	float   layerColor[12][4];
	float   sunDir[4];
	float   sunColor[4];
	float   ambient[4];
	float   tex[4];          // x = 1 / metres of one texture tile, y = 1 when the albedo array is bound, zw = fract(origin.xz * x)
};
struct alignas(16) PatchData
{
	int32_t patch[4];
	float   patchPos[4];
	float   morph[4];
};
static_assert(sizeof(Globals) == 64 + 4 * 16 + 16 + 8 * 16 + 16 * 16 + 12 * 16 + 4 * 16, "LsGlobals: the HLSL block and this struct have to be the same size");
static_assert(sizeof(PatchData) == 48, "LsPatch: the HLSL block and this struct have to be the same size");

// A world these shaders can draw: the grid is the 65 x 65 one the shader's SV_VertexID arithmetic assumes.
inline bool worldUsable(const HE::LandscapeRenderStatic& w)
{
	return w.patchCells == kGridVerts - 1 && w.slotCapacity > 0 && w.slotCapacity <= 65535 && w.tableWindow > 0 && (w.tableWindow & (w.tableWindow - 1)) == 0
	       && w.heightW > 0 && w.heightH > 0 && w.weightW > 0 && w.weightH > 0 && !w.patchIndices.empty();
}

// `albedoBound`: the albedo array of the world-UV ground is bound (the renderer resolved LandscapeRenderStatic::albedoArrayPath to something).
inline void fillGlobals(Globals& g, const HE::LandscapeRenderFrame& f, const glm::mat4& viewProj, const glm::vec3& sunDir, const glm::vec3& sunColor,
                        const glm::vec3& ambient, bool albedoBound)
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
}

inline void fillPatch(PatchData& d, const HE::LandscapeRenderPatch& p)
{
	d.patch[0] = p.level; d.patch[1] = p.cornerX; d.patch[2] = p.cornerZ; d.patch[3] = p.coarse ? 1 : 0;
	d.patchPos[0] = p.x0; d.patchPos[1] = p.z0; d.patchPos[2] = p.size; d.patchPos[3] = static_cast<float>(p.gridIndex);
	d.morph[0] = p.morphStart; d.morph[1] = p.morphEnd; d.morph[2] = 0.0f; d.morph[3] = 0.0f;
}

} // namespace HE::d3dls
