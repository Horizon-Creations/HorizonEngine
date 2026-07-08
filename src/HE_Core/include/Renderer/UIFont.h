#pragma once
#include <Renderer/UIRenderObject.h>
#include <Types/Defines.h>
#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace HE {

// One baked glyph's atlas box + placement (portable mirror of stbtt_bakedchar,
// so the atlas metrics can live in a header without pulling in stb).
struct BakedGlyph
{
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // atlas pixel rect
    float xoff = 0, yoff = 0, xadvance = 0;
};

// A baked font atlas: an R8 coverage bitmap + per-glyph metrics for ASCII 32..127.
// The engine bakes the default UI font (Roboto) once; UIFontCache bakes imported
// Font assets on demand. `atlasW/atlasH/bakePx` are per-instance so different
// fonts can use different sizes.
struct BakedUIFont
{
    // Defaults for the shared UI font atlas (also used by the backend upload path).
    static constexpr int   kWidth  = 1024;
    static constexpr int   kHeight = 1024;
    static constexpr float kBakePx = 64.0f;

    int   atlasW = kWidth;
    int   atlasH = kHeight;
    float bakePx = kBakePx;
    float ascent = 0.0f;                 // baseline offset from the top, at bakePx
    std::vector<uint8_t>       pixels;   // atlasW × atlasH, single channel (alpha)
    std::array<BakedGlyph, 96> glyphs{}; // ASCII 32..127
    bool  ok = false;
};

// The engine-wide default UI font (Roboto Condensed Bold), baked lazily.
HE_API const BakedUIFont& sharedUIFont();

// ── Per-element font cache ────────────────────────────────────────────────────
// Bakes imported Font assets (TTF bytes) at a given pixel size and hands the
// renderer a stable atlas key it uploads once. Key 0 == the shared default font.
namespace UIFontCache
{
    // Bake (or fetch the cached) atlas for `ttf` at ~`bakePx`, identified by a
    // caller-stable id (e.g. a hash of the font asset UUID). Returns the atlas key
    // (0 when ttf is empty / baking fails → callers fall back to the shared font).
    HE_API std::uint32_t keyFor(std::uint64_t stableId, const std::vector<uint8_t>& ttf, float bakePx);
    // The baked font for `key` (0 → the shared default), or null if unknown.
    HE_API const BakedUIFont* find(std::uint32_t key);
}

// Append per-glyph UIRenderObjects (type 2) for `text` at `sizePx`, laid out
// inside `rect` (vertically centered; horizontally centered when `centerH`).
// The overload draws with a specific baked font + stamps `atlasKey` on each quad
// so the renderer samples the matching atlas; the short form uses sharedUIFont.
HE_API void emitUITextGlyphs(const BakedUIFont& font, std::uint32_t atlasKey,
                             const std::string& text, const glm::vec2& rectPos,
                             const glm::vec2& rectSize, float sizePx,
                             const glm::vec4& color, int layer, bool centerH,
                             std::vector<UIRenderObject>& out);
HE_API void emitUITextGlyphs(const std::string& text, const glm::vec2& rectPos,
                             const glm::vec2& rectSize, float sizePx,
                             const glm::vec4& color, int layer, bool centerH,
                             std::vector<UIRenderObject>& out);

} // namespace HE
