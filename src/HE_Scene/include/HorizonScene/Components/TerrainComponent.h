#pragma once
#include <Types/UUID.h>
#include "HorizonScene/WaterField.h"
#include <cstdint>
#include <memory>
#include <vector>

// Paintable landscape layers: two RGBA8 weightmap pages of four channels each.
// Must equal HE::kMatMaxLandscapeLayers (MaterialGraph.h) — test_terrain pins it.
// The largest resolution a landscape can have: 1025 = 2^10 + 1, the top of the 2ⁿ+1
// ladder every landscape is snapped to (TerrainSystem / TerrainSculpt::ensureHeights).
// The clamp used to be 1024, which is NOT on that ladder: a landscape of 513..1024
// snaps UP to 1025, every consumer clamped it back down to 1024, and the height
// field (1025² samples) never matched the grid it was read as (1024²) — so it was
// read as flat, and the "snap" ran again every frame (dirty → all 256 chunks rebuilt
// and re-uploaded per frame).
inline constexpr uint32_t kTerrainMaxResolution = 1025;
inline constexpr int kTerrainWeightPages = 2;
inline constexpr int kTerrainMaxLayers   = 4 * kTerrainWeightPages;
// Texture groups a terrain mixes besides its own (TerrainComponent::mixGroup): 4 groups = own + 3.
inline constexpr int kMixGroupSlots      = 3;

struct TerrainComponent {
    float    sizeX       = 100.0f;
    float    sizeZ       = 100.0f;
    uint32_t resolution  = 128;
    float    heightScale = 20.0f;
    int      seed        = 0;   // 0 = flat terrain; non-zero = fBm noise
    // Distance-LOD aggressiveness for the runtime chunks: higher = keep full detail
    // farther from the camera (1 = default, 2 = twice as far, …). The near terrain is
    // always full-resolution; only distant chunks decimate.
    float    lodDistanceScale = 1.0f;
    int      octaves     = 4;
    float    frequency   = 1.0f;
    float    lacunarity  = 2.0f;
    float    gain        = 0.5f;
    // Texture repeats across the WHOLE terrain. The generated UVs run 0..1 over
    // the full landscape, so at 1 a texture is stretched across every metre of
    // it — set this to the number of tiles you want (e.g. sizeX/4 for a 4 m
    // texture). 1 = the historical behaviour.
    float    uvTiling    = 1.0f;
    HE::UUID heightmapTexture{};  // Phase 2: greyscale heightmap source

    // ── Tessellation / displacement (serialised) ─────────────────────────────
    // Chunks whose centre is within tessellationDistance of the camera get one
    // level FINER than LOD0: tessellationFactor × the chunk's LOD0 grid, the
    // height smooth (Catmull-Rom) between the source samples, plus the detail
    // of the displacement map. 1 = off (the default; nothing is built).
    //
    // This is CPU tessellation, not a hull/domain shader stage: the refined
    // level is an ordinary mesh, so it renders the same on all five backends.
    // It is built on demand for the chunks near the camera only (a few per
    // tick, capped per terrain) and given back when the camera leaves.
    //
    // Visual only: collision, navigation and foliage placement keep reading
    // the height field, so they sit up to half the displacement strength
    // away from the displaced surface. Keep the strength small (detail, not
    // landform — landform is what sculpting is for).
    int      tessellationFactor   = 1;      // 1 = off, 2 or 4
    float    tessellationDistance = 50.0f;  // camera → chunk centre, world units
    HE::UUID displacementTexture{};         // tileable greyscale detail; none = smooth only
    float    displacementStrength = 0.0f;   // black-to-white, world units; mid-grey = 0
    float    displacementTiling   = 0.0f;   // repeats across the terrain; 0 = follow uvTiling
    bool     dirty = true;        // set to regenerate ALL chunks; not serialised
    // Per-vertex sculpted heights (size == res*res overrides fBm); serialised.
    std::vector<float> sculptHeights;
    // The height field the chunks were last built from (resolution x resolution, row-major,
    // the same one the chunk meshes sample), kept for the renderer's GI height-field tracing.
    // Not serialised; TerrainSystem replaces it (and bumps the version) on every rebuild.
    std::shared_ptr<const std::vector<float>> builtField;
    uint32_t builtFieldRes     = 0;
    uint64_t builtFieldVersion = 0;

    // ── Material layers (paint) ──────────────────────────────────────────────
    // Per-texel layer weights, RGBA8 = layers 0..3 (R=0 … A=3), row-major over
    // the terrain's 0..1 UV range. The MATERIAL defines what the layers mean:
    // a Landscape Layer Blend node names them and the shader blends its inputs
    // by these weights (MaterialAsset::graphLayerNames). Empty = unpainted, the
    // shader then falls back to layer 0.
    //
    // Kept inline (base64 in the scene, like sculptHeights) rather than as a
    // separate texture asset: it is terrain data, not shared content, and this
    // way a landscape is one self-contained thing to copy or undo.
    uint32_t              weightRes = 512;   // weightmap side length in texels (256 made a round stamp a visible polygon)
    std::vector<uint8_t>  layerWeights;      // weightRes² × 4 bytes, or empty
    // Layers 4..7 (R=4 … A=7), same layout as layerWeights. Empty = all four
    // zero, which is every landscape painted before there were eight layers —
    // they load without it and render exactly as before. Allocated by the first
    // stroke on layer 4+, never without layerWeights; the weights of one texel
    // sum to 255 across BOTH pages. Uploaded as the right half of a 2:1
    // weightmap in the same texture (TerrainPaint::buildWeightTexture), so the
    // shader needs no second sampler or binding on any backend.
    std::vector<uint8_t>  layerWeights2;

    // ── Mixed texture groups (Thema 233, step 7) ─────────────────────────────
    // A terrain's own group is its landscape material (MaterialComponent::materialAssetId).
    // mixGroup[k] names a further group whose share of every texel lives in weight layer
    // TerrainPaint::shareLayer(k) (6, 7, and 5 for the third): the AutoLandscapeGroupId's
    // int, -1 = none. The terrain's own group is the remainder (1 - the shares), so a texel nobody painted is the own group
    // alone, exactly what every landscape was before this field. A reserved layer is a
    // GROUP SHARE and not a paint layer: it is left out of the 255-sum of layers 0..5
    // (TerrainPaint), and the material must not use layers 6/7 as its own paint layers
    // (the engine's group materials declare six; the third slot takes the Puddles layer). Saved only once one is set.
    // Order matters nowhere else: -1 on both is the legacy, one-group terrain.
    int mixGroup[kMixGroupSlots] = { -1, -1, -1 };

    // ── Water ────────────────────────────────────────────────────────────────
    // The water bodies on this landscape and the raster that says where they are
    // (WaterField.h). One model for the water brush and the lake tool. Saved with
    // the terrain, under the "terrain" block, only once there is something to save.
    // Its edits touch neither `dirty` nor `regionDirty`: a water edit must not
    // regenerate terrain chunks (Field::dirty is the water surface's own flag).
    HE::water::Field water;

    // ── Runtime weightmap state (never serialised) ──────────────────────────
    // The GPU texture TerrainSystem (re)registers from layerWeights, handed to
    // the chunks' draw calls so the layer-blend node can sample it.
    HE::UUID weightmapTextureId{};
    bool     weightsDirty = false;   // re-upload the texture on the next tick
    // Mean of layerWeights over the whole terrain, normalised (Σ = 1). Recomputed
    // with the texture upload — consumers that shade the landscape FLAT, with no
    // texel to sample (the GI ray kernels colour a hit per instance), blend the
    // material's per-layer colours by this instead of reflecting one fixed layer.
    // Unpainted → { 1, 0, … }, matching the shader's 1×1 default weightmap.
    // The GI consumers read layers 0..3 only (MaterialAsset::approxLayerColor).
    float    avgLayerWeights[kTerrainMaxLayers] = { 1.0f };

    // ── Runtime chunk/LOD state (never serialised) ──────────────────────────
    // Sculpt dirty-region in terrain-local XZ: the brush sets it so TerrainSystem
    // regenerates only the touched chunks (not all 64+) per stroke. Cleared after.
    bool     regionDirty = false;
    float    dirtyMinX = 0.0f, dirtyMinZ = 0.0f, dirtyMaxX = 0.0f, dirtyMaxZ = 0.0f;
    // Chunk grid the chunk entities were last built for — a change (resolution/size)
    // forces a full rebuild of the chunk set.
    uint32_t builtRes = 0, builtChunksPerSide = 0;
};
