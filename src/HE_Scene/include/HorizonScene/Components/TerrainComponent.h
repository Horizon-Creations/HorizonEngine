#pragma once
#include <Types/UUID.h>
#include <cstdint>
#include <vector>

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
    bool     dirty = true;        // set to regenerate ALL chunks; not serialised
    // Per-vertex sculpted heights (size == res*res overrides fBm); serialised.
    std::vector<float> sculptHeights;

    // ── Material layers (paint) ──────────────────────────────────────────────
    // Per-texel layer weights, RGBA8 = four layers (R=0 … A=3), row-major over
    // the terrain's 0..1 UV range. The MATERIAL defines what the layers mean:
    // a Landscape Layer Blend node names them and the shader blends its inputs
    // by these weights (MaterialAsset::graphLayerNames). Empty = unpainted, the
    // shader then falls back to layer 0.
    //
    // Kept inline (base64 in the scene, like sculptHeights) rather than as a
    // separate texture asset: it is terrain data, not shared content, and this
    // way a landscape is one self-contained thing to copy or undo.
    uint32_t              weightRes = 256;   // weightmap side length in texels
    std::vector<uint8_t>  layerWeights;      // weightRes² × 4 bytes, or empty

    // ── Runtime weightmap state (never serialised) ──────────────────────────
    // The GPU texture TerrainSystem (re)registers from layerWeights, handed to
    // the chunks' draw calls so the layer-blend node can sample it.
    HE::UUID weightmapTextureId{};
    bool     weightsDirty = false;   // re-upload the texture on the next tick

    // ── Runtime chunk/LOD state (never serialised) ──────────────────────────
    // Sculpt dirty-region in terrain-local XZ: the brush sets it so TerrainSystem
    // regenerates only the touched chunks (not all 64+) per stroke. Cleared after.
    bool     regionDirty = false;
    float    dirtyMinX = 0.0f, dirtyMinZ = 0.0f, dirtyMaxX = 0.0f, dirtyMaxZ = 0.0f;
    // Chunk grid the chunk entities were last built for — a change (resolution/size)
    // forces a full rebuild of the chunk set.
    uint32_t builtRes = 0, builtChunksPerSide = 0;
};
