#pragma once
#include <Types/UUID.h>

struct AudioSourceComponent {
    HE::UUID assetId;
    float    volume      = 1.0f;
    float    pitch       = 1.0f;
    float    range       = 20.0f; // max audible distance (meters), 0 = non-spatial
    bool     loop        = false;
    bool     playOnStart = false;
    bool     spatial     = false; // enable 3D position-based attenuation
};
