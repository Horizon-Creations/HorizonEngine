#pragma once
#include <Renderer/UIRenderObject.h>
#include <Types/Defines.h>
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace HE {

// The engine-wide baked UI font: ProggyClean at kBakePx into one shared 8-bit
// alpha atlas. Lives in HE_Core so both UISystem (HE_Scene, glyph layout at
// extract time) and the renderer backends (atlas upload) can reach it without
// a scene dependency. Baked lazily on first use; `ok` false = baking failed.
struct BakedUIFont
{
    static constexpr int   kWidth  = 512;
    static constexpr int   kHeight = 256;
    static constexpr float kBakePx = 32.0f;
    std::vector<uint8_t> pixels;   // kWidth × kHeight, single channel (alpha)
    float ascent = 0.0f;           // baseline offset from the top, at kBakePx
    bool  ok     = false;
};
HE_API const BakedUIFont& sharedUIFont();

// Append per-glyph UIRenderObjects (type 2, UVs into sharedUIFont's atlas) for
// `text` rendered at `sizePx`. The run is laid out inside `rect` (pos/size in
// screen pixels): vertically centered, horizontally centered when `centerH`.
HE_API void emitUITextGlyphs(const std::string& text, const glm::vec2& rectPos,
                             const glm::vec2& rectSize, float sizePx,
                             const glm::vec4& color, int layer, bool centerH,
                             std::vector<UIRenderObject>& out);

} // namespace HE
