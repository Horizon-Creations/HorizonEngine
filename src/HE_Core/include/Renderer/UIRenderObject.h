#pragma once
#include <Math/Math.h>
#include <Types/UUID.h>
#include <string>
#include <cstdint>

// One drawable item produced by UISystem::extract, consumed by renderer backends.
struct UIRenderObject {
    glm::vec2   position;          // top-left corner in screen pixels
    glm::vec2   size;              // width/height in screen pixels
    glm::vec4   color     = {1.0f, 1.0f, 1.0f, 1.0f};
    HE::UUID    materialAssetId;   // image quads
    std::string text;              // text quads
    float       fontSize  = 14.0f;
    uint8_t     type      = 0;     // 0 = rect/image, 1 = text
    int         layer     = 0;
};
