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
//   uvTiling       : texture repeats across the WHOLE terrain (see
//                    TerrainComponent::uvTiling). Chunk UVs stay in the global
//                    0..uvTiling range so the pattern is continuous across chunks.
// Vertices are chunk-LOCAL (centred on the chunk middle) so the caller positions the
// chunk entity at that centre and per-chunk frustum culling + distance-LOD work.
// Normals are sampled from the global field at source-cell spacing (identical across
// chunks/LODs → no lighting seams). A downward skirt rings each chunk edge to hide
// the geometry cracks where neighbouring chunks sit at different LODs.
StaticMeshAsset generateTerrainChunkMesh(
    const std::vector<float>& heights, uint32_t srcRes,
    float sizeX, float sizeZ,
    float u0, float v0, float u1, float v1,
    uint32_t vertsPerSide, float uvTiling = 1.0f);

// ─── Tessellation / displacement ────────────────────────────────────────────
// The refined level a chunk near the camera gets on top of LOD0 (see
// TerrainComponent::tessellationFactor). CPU-side on purpose: the chunks are
// ordinary meshes through the generic material pipeline on all five backends,
// so a finer mesh needs no hull/domain stage anywhere.
//
// Detail displacement laid over the refined surface: a grey field (0..1,
// row-major width×height) repeated `tiling` times across the WHOLE terrain,
// wrapping at its edges (it is meant to be a tileable detail map), centred on
// mid-grey so it raises AND lowers: offset = (grey - 0.5) × strength.
// A null/empty field or strength 0 displaces nothing.
struct TerrainDisplacementMap
{
    const float* grey   = nullptr;
    uint32_t     width  = 0;
    uint32_t     height = 0;
    float        strength = 0.0f;   // world units, black-to-white
    float        tiling   = 1.0f;   // repeats across the whole terrain
    bool active() const { return grey && width > 0 && height > 0 && strength != 0.0f; }
};

// World-Y offset the displacement adds at terrain UV (u, v) ∈ [0,1]². Bilinear,
// wrapping. 0 when the map is inactive.
float sampleTerrainDisplacement(const TerrainDisplacementMap& disp, float u, float v);

// Height between the source samples by bicubic Catmull-Rom instead of
// bilinear: passes through every sample EXACTLY (so a refined chunk agrees
// with LOD0 on every LOD0 vertex) but is smooth in between, where bilinear
// would put a crease along every source cell edge.
float sampleTerrainHeightSmooth(const std::vector<float>& heights, uint32_t srcRes,
                                float u, float v);

// A chunk mesh like generateTerrainChunkMesh, but with heights from
// sampleTerrainHeightSmooth plus `disp`. Pass vertsPerSide = lod0Cells ×
// factor + 1 so every LOD0 vertex is also a vertex here. Normals are the same
// source-spacing normals LOD0 uses, tilted by the displacement's gradient —
// with no displacement a refined chunk lights exactly like its LOD0 (no
// lighting edge where the two meet). The skirt is deepened by half the
// displacement strength so it still covers the crack to a LOD0 neighbour.
StaticMeshAsset generateTerrainChunkMeshTessellated(
    const std::vector<float>& heights, uint32_t srcRes,
    float sizeX, float sizeZ,
    float u0, float v0, float u1, float v1,
    uint32_t vertsPerSide, float uvTiling,
    const TerrainDisplacementMap& disp);
