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
    HE::UUID heightmapTexture{};  // Phase 2: greyscale heightmap source
    bool     dirty = true;        // set to regenerate ALL chunks; not serialised
    // Per-vertex sculpted heights (size == res*res overrides fBm); serialised.
    std::vector<float> sculptHeights;

    // ── Runtime chunk/LOD state (never serialised) ──────────────────────────
    // Sculpt dirty-region in terrain-local XZ: the brush sets it so TerrainSystem
    // regenerates only the touched chunks (not all 64+) per stroke. Cleared after.
    bool     regionDirty = false;
    float    dirtyMinX = 0.0f, dirtyMinZ = 0.0f, dirtyMaxX = 0.0f, dirtyMaxZ = 0.0f;
    // Chunk grid the chunk entities were last built for — a change (resolution/size)
    // forces a full rebuild of the chunk set.
    uint32_t builtRes = 0, builtChunksPerSide = 0;
};
