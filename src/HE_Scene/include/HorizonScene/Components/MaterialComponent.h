#pragma once
#include <Types/UUID.h>

struct MaterialComponent {
    HE::UUID materialAssetId;
    bool dirty = true;
};
