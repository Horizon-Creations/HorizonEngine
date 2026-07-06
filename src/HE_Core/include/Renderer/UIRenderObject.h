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
    HE::UUID    materialAssetId;   // image quads (custom material; nil = solid color)
    std::string text;              // legacy text quads (type 1)
    float       fontSize  = 14.0f;
    // 0 = rect/image, 1 = legacy text run (unused since glyph extraction),
    // 2 = font-atlas glyph quad (uvMin/uvMax into UISystem::sharedFont atlas).
    uint8_t     type      = 0;
    int         layer     = 0;
    glm::vec2   uvMin     = {0.0f, 0.0f}; // glyph quads: atlas UV rect
    glm::vec2   uvMax     = {0.0f, 0.0f};
};
