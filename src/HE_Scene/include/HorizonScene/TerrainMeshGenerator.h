#pragma once
#include <ContentManager/Assets.h>
#include <cstdint>
#include <vector>

struct TerrainComponent;

// CPU-only mesh generator. No GPU, no ContentManager dependency — freely testable.
// Returns a StaticMeshAsset built from the terrain parameters; caller registers it.
StaticMeshAsset generateTerrainMesh(const TerrainComponent& tc);

// Sample terrain height (world-space Y) at a terrain-local (x, z) position.
// localX in [-sizeX/2, sizeX/2], localZ in [-sizeZ/2, sizeZ/2].
// Used by FoliageSystem to place instances on the terrain surface.
float terrainHeightAt(const TerrainComponent& tc, float localX, float localZ);

// ─── Chunked / LOD terrain ──────────────────────────────────────────────────
// The master height field: resolution×resolution heights (row-major z*res+x), in
// world-Y units (sculpt overrides noise overrides flat — same precedence as
// generateTerrainMesh). This is the single source of truth the chunk meshes sample;
// sculpting keeps editing TerrainComponent::sculptHeights, which feeds straight in.
std::vector<float> computeTerrainHeightField(const TerrainComponent& tc);

// Bilinearly resample a square height field oldRes×oldRes → newRes×newRes. Used to
// snap a terrain to a 2ⁿ+1 resolution so chunk LOD0 vertices land EXACTLY on source
// grid points (no bilinear smear of sculpted detail). One-time, near-lossless for a
// small bump like 512→513.
std::vector<float> resampleHeightField(const std::vector<float>& src,
                                       uint32_t oldRes, uint32_t newRes);

// Build ONE chunk LOD mesh by bilinearly sampling the global height field.
//   heights/srcRes : the field from computeTerrainHeightField (srcRes×srcRes).
//   sizeX/sizeZ    : full terrain world size.
//   u0,v0,u1,v1    : the chunk's normalized sub-rectangle of the terrain ([0,1]).
//   vertsPerSide   : this LOD's vertex count per side (>= 2; lower = coarser).
// Vertices are chunk-LOCAL (centred on the chunk middle) so the caller positions the
// chunk entity at that centre and per-chunk frustum culling + distance-LOD work.
// Normals are sampled from the global field at source-cell spacing (identical across
// chunks/LODs → no lighting seams). A downward skirt rings each chunk edge to hide
// the geometry cracks where neighbouring chunks sit at different LODs.
StaticMeshAsset generateTerrainChunkMesh(
    const std::vector<float>& heights, uint32_t srcRes,
    float sizeX, float sizeZ,
    float u0, float v0, float u1, float v1,
    uint32_t vertsPerSide);
