#pragma once
#include <Types/UUID.h>
#include <cstdint>

struct AudioSourceComponent {
    HE::UUID assetId;
    float    volume        = 1.0f;
    float    pitch         = 1.0f;
    float    range         = 20.0f; // max audible distance (m)
    float    rolloffFactor = 1.0f;  // attenuation speed (linear model)
    float    innerRange    = 1.0f;  // min distance — full volume within this radius
    bool     loop          = false;
    bool     playOnStart   = false;
    bool     spatial       = false; // enable 3D position-based attenuation

    // Runtime only — not serialized
    uint64_t handle        = 0;
};
