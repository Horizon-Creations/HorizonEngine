#pragma once
#include <Math/Math.h>
#include <Types/UUID.h>
#include <cstdint>

// One drawable item produced by UISystem::extract, consumed by renderer backends.
struct UIRenderObject {
    glm::vec2   position;          // top-left corner in screen pixels
    glm::vec2   size;              // width/height in screen pixels
    glm::vec4   color     = {1.0f, 1.0f, 1.0f, 1.0f};
    HE::UUID    materialAssetId;   // image quads (custom material; nil = solid color)
    // 0 = rect/image, 2 = font-atlas glyph quad (uvMin/uvMax into
    // UISystem::sharedFont atlas). Text is emitted per glyph as type 2, so there
    // is no whole-string kind; 1 is unused and left as a hole to keep the values
    // that every backend already branches on stable.
    uint8_t     type      = 0;
    int         layer     = 0;
    glm::vec2   uvMin     = {0.0f, 0.0f}; // glyph quads: atlas UV rect
    glm::vec2   uvMax     = {0.0f, 0.0f};
    // Corner radius in pixels for solid quads (type 0); 0 = square. A value of
    // min(w,h)/2 yields a circle — used for the slider handle. Ignored by glyphs.
    float       cornerRadius = 0.0f;
    // Glyph quads (type 2): which baked font atlas to sample. 0 = shared default
    // font; other keys index UIFontCache (an imported Font asset).
    uint32_t    fontAtlasKey = 0;
};
