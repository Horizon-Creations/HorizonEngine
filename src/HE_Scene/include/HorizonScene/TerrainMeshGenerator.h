#pragma once
#include <functional>
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

// The fBm computeTerrainHeightField seeds a terrain with: value noise summed over
// `octaves` (clamped 1..12), normalised to [0, 1]. Exposed so a generator that
// shapes PART of a landscape (TerrainGenerate) grows the same kind of relief as
// a seeded one, rather than a second noise that looks subtly different beside it.
float terrainFbm(int seed, float x, float z, int octaves,
                 float frequency, float lacunarity, float gain);

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

// Caches one chunk's finest-seen LOD grid (heights + normals, n×n) across the calls of
// generateTerrainChunkMeshLOD below, so a coarser call in the same chunk can downsample it
// instead of resampling the field. n == 0 means empty (nothing cached yet).
struct TerrainChunkLODCache
{
    uint32_t n = 0;
    std::vector<float> heights;
    std::vector<float> normals;
};

// Same contract and result as generateTerrainChunkMesh, but reuses `cache` across one chunk's
// LOD levels: LOD(k+1)'s grid is an exact stride-2 subset of LOD k's (see
// TerrainSystem::buildChunk, "verts = lod0Cells>>k + 1"), so once the finest level this chunk
// has built is in `cache`, a coarser `vertsPerSide` is filled by downsampling it instead of
// re-sampling the height field — bit-identical to the fully-resampled result, but ONLY when the
// stride is a power of two. The grid coordinate evaluates as `((u1-u0) * i) / (N-1)`: the
// multiply rounds once, and multiplying an already-rounded float by a power of two is exact (a
// pure exponent shift), so `round((u1-u0)*i) * 2^k == round((u1-u0)*i*2^k)` and the cached base's
// vertex at `i*stride` lands on the identical rounded value the coarser level would have computed
// directly. An arbitrary stride (3, say) has no such guarantee — `round((u1-u0)*i)*3` is not in
// general `round((u1-u0)*i*3)` — so the power-of-two check in strideOf (TerrainMeshGenerator.cpp)
// is load-bearing, not a conservative extra: relaxing it would silently stop being bit-identical
// for non-power-of-two strides. Calls whose `vertsPerSide` is not an exact power-of-two stride
// subset of what is cached fall back to resampling the field directly (and refresh `cache` with
// that grid for the next call). Call order does not matter — any order converges, finest-first
// just avoids ever falling back.
StaticMeshAsset generateTerrainChunkMeshLOD(
    const std::vector<float>& heights, uint32_t srcRes,
    float sizeX, float sizeZ,
    float u0, float v0, float u1, float v1,
    uint32_t vertsPerSide, float uvTiling,
    TerrainChunkLODCache& cache);

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

// A second, caller-supplied height offset laid over the refined surface (the trails in snow and sand): world-Y
// metres at terrain UV (u, v), |offset| <= maxAbs. It raises and lowers like the displacement map, tilts the
// normals by its own gradient and deepens the skirt by maxAbs.
struct TerrainHeightOffset
{
    std::function<float(float u, float v)> fn;
    float maxAbs = 0.0f;
    bool active() const { return static_cast<bool>(fn) && maxAbs > 0.0f; }
};

// A refined chunk built so that its height offset can be CHANGED afterwards without building it again: what
// the offset does not touch (the height before it, the slope the normals are made from) is kept per grid
// point. A dragged track then re-evaluates only the vertices in the tiles it changed (applyTerrainHeightOffset)
// instead of the 66 000 of the chunk.
struct TerrainTessCache
{
    uint32_t N = 0;                       // vertices per side
    float    sizeX = 0, sizeZ = 0, u0 = 0, v0 = 0, u1 = 0, v1 = 0, du = 0, dv = 0;
    float    skirtDepth = 0;              // the skirt's drop below its ring vertex
    std::vector<float> baseY;             // N x N: smooth height + displacement map, before the offset
    std::vector<float> gx, gz;            // N x N: the slope (dh/dx, dh/dz) the normal is made from, before the offset
    bool valid() const { return N >= 2 && baseY.size() == static_cast<size_t>(N) * N; }
};

// Same mesh as generateTerrainChunkMeshTessellated (with `extra`), and fills `cache`. The normals are composed from
// the cached slope, so applyTerrainHeightOffset over the whole grid gives this mesh back bit for bit.
StaticMeshAsset generateTerrainChunkMeshTessellatedCached(
    const std::vector<float>& heights, uint32_t srcRes,
    float sizeX, float sizeZ,
    float u0, float v0, float u1, float v1,
    uint32_t vertsPerSide, float uvTiling,
    const TerrainDisplacementMap& disp, const TerrainHeightOffset* extra, TerrainTessCache& cache);

// Re-evaluates `extra` (null or inactive = no offset) for the grid vertices i0..i1 x j0..j1 (clamped to the chunk)
// and for the skirt vertices under them: height = baseY + offset, normal from the cached slope tilted by the
// offset's gradient. `mesh` must be the mesh `cache` was filled with. Returns whether a vertex on the chunk's border
// ring (and with it the skirt) changed: a seam copy only has to be made again then.
bool applyTerrainHeightOffset(StaticMeshAsset& mesh, const TerrainTessCache& cache, const TerrainHeightOffset* extra,
                              int i0, int j0, int i1, int j1);

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
    const TerrainDisplacementMap& disp, const TerrainHeightOffset* extra = nullptr);

// ─── Seam stitching ─────────────────────────────────────────────────────────
// Where a chunk meets a neighbour drawn COARSER (LOD0 next to LOD2, a refined
// chunk next to LOD0), the coarse edge runs straight from one of its vertices
// to the next while the finer edge follows the terrain: a lens-shaped step per
// coarse segment. It is real geometry, so it casts a real shadow, and the GI
// sun ray (which starts a few cm above the surface) hits it — the dashed black
// line along mixed-detail seams. Stitching moves the finer edge onto the
// coarse one, so the two chunks share their edge exactly.
//
// Edges in the order buildChunkMesh rings the chunk: 0 = v0 (bottom),
// 1 = u1 (right), 2 = v1 (top), 3 = u0 (left). edgeCells[e] = cells per side
// of the neighbour across edge e, 0 = leave the edge alone (same or finer
// neighbour, or none). Corner bits, same order: 0 = (u0,v0), 1 = (u1,v0),
// 2 = (u1,v1), 3 = (u0,v1) — a set bit pins that corner to its LOD0 height,
// for a displaced refined chunk whose DIAGONAL neighbour is coarser.
struct TerrainChunkStitch
{
    uint32_t edgeCells[4] = { 0, 0, 0, 0 };
    uint8_t  corners = 0;
    bool any() const
    {
        return edgeCells[0] || edgeCells[1] || edgeCells[2] || edgeCells[3] || corners;
    }
};

// A copy of `level` (a chunk mesh from generateTerrainChunkMesh or
// ...Tessellated with levelCells cells per side) whose stitched edge vertices
// lie on the neighbour's edge: heights and normals of the coarse vertices are
// taken from the chunk's own LOD0 mesh (`lod0`, lod0Cells per side), which
// samples the field at the very same UVs as any coarser level does — so they
// match the neighbour's bit for bit — and linearly interpolated in between, as
// the neighbour's triangle does. The skirt below a moved vertex moves with it.
// An edge whose count does not divide both levelCells and lod0Cells is left
// alone. Returns an empty mesh if a mesh's layout does not fit its count.
StaticMeshAsset stitchTerrainChunkEdges(const StaticMeshAsset& level, uint32_t levelCells,
                                        const StaticMeshAsset& lod0, uint32_t lod0Cells,
                                        const TerrainChunkStitch& stitch);
