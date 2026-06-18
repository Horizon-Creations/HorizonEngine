#pragma once
#include <Types/UUID.h>
#include <Math/Math.h>

struct UIImageComponent {
    HE::UUID  materialAssetId;
    glm::vec4 tint = {1.0f, 1.0f, 1.0f, 1.0f};
};
