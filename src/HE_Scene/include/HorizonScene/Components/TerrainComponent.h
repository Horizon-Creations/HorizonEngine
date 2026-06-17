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
    int      octaves     = 4;
    float    frequency   = 1.0f;
    float    lacunarity  = 2.0f;
    float    gain        = 0.5f;
    HE::UUID heightmapTexture{};  // Phase 2: greyscale heightmap source
    bool     dirty = true;        // set to regenerate; not serialised
    // Per-vertex sculpted heights (size == res*res overrides fBm); serialised.
    std::vector<float> sculptHeights;
};
