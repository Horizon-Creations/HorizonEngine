#pragma once
#include <Types/UUID.h>
#include <cstdint>

struct TerrainComponent {
    float    sizeX       = 100.0f;
    float    sizeZ       = 100.0f;
    uint32_t resolution  = 128;
    float    heightScale = 20.0f;
    int      seed        = 42;
    int      octaves     = 4;
    float    frequency   = 1.0f;
    float    lacunarity  = 2.0f;
    float    gain        = 0.5f;
    HE::UUID heightmapTexture{};  // Phase 2: greyscale heightmap source
    bool     dirty = true;        // set to regenerate; not serialised
};
