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

// The same font baked at an arbitrary pixel size, for callers that draw it at a
// FIXED small size and would otherwise scale the 64 px shared atlas down — which
// is where text turns mushy. Not cached: this is for the handful of one-off
// consumers (the startup splash) that bake once and keep the result.
// The TTF bytes stay private to this translation unit; nobody else has to
// re-embed the font or pull in a second stb_truetype implementation.
HE_API BakedUIFont bakeDefaultUIFont(float bakePx, int atlasW, int atlasH);

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

// ── Multi-line text layout ────────────────────────────────────────────────────
// A run is split into lines at '\n' and, with `wrap`, additionally word-wrapped
// at the rect width. The whole block is centred vertically in the rect; each
// line is centred horizontally when `centerH`. A single line without newlines
// lays out byte-identically to the original single-line path.
struct UITextLayout
{
    // Where the text sits inside its rect. 0/1/2 = left/centre/right and
    // top/middle/bottom. `alignV` starts at MIDDLE because that is what every
    // text drew before there was a choice — the block was always centred
    // vertically, and a default of Top would move every existing label.
    int   alignH      = 0;
    int   alignV      = 1;
    bool  wrap        = false;  // word-wrap at the rect width
    float lineSpacing = 1.15f;  // baseline-to-baseline distance, in units of sizePx
};

// Split `text` into the lines it will render as: always at '\n', plus greedy
// word-wrapping at `wrapWidth` when `wrap` (a single word wider than the line is
// hard-broken). wrapWidth <= 0 disables wrapping regardless of the flag.
HE_API std::vector<std::string> layoutUITextLines(const BakedUIFont& font,
                                                  const std::string& text, float sizePx,
                                                  float wrapWidth, bool wrap);

// Pixel extent the run occupies at `sizePx`: x = widest line, y = the block
// height ((lines-1) * lineSpacing * sizePx + sizePx). Lets callers size an
// element to fit its own text (see UIElement auto-size).
HE_API glm::vec2 measureUIText(const BakedUIFont& font, const std::string& text,
                               float sizePx, float wrapWidth, const UITextLayout& opts);
HE_API glm::vec2 measureUIText(const std::string& text, float sizePx,
                               float wrapWidth, const UITextLayout& opts);

// Append per-glyph UIRenderObjects (type 2) for `text` at `sizePx`, laid out
// inside `rect` (vertically centered; horizontally centered when `centerH`).
// The overload draws with a specific baked font + stamps `atlasKey` on each quad
// so the renderer samples the matching atlas; the short form uses sharedUIFont.
HE_API void emitUITextGlyphs(const BakedUIFont& font, std::uint32_t atlasKey,
                             const std::string& text, const glm::vec2& rectPos,
                             const glm::vec2& rectSize, float sizePx,
                             const glm::vec4& color, int layer,
                             const UITextLayout& opts,
                             std::vector<UIRenderObject>& out);
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
