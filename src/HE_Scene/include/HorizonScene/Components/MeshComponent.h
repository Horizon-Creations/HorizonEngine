#pragma once
#include <Types/UUID.h>

struct MeshComponent {
    HE::UUID meshAssetId;
    uint8_t lodBias        = 0;     // 0 = auto LOD
    bool    castsShadow    = true;
    bool    receivesShadow = true;
    bool    dirty          = true;
};
