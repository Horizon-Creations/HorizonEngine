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

// ── The same split, as BYTE RANGES — for text that can be EDITED ─────────────
// A caret is a byte offset, so an editor cannot use the line list above: it
// hands back strings, and two of the things it does to them lose bytes. Wrapping
// trims the spaces at a break, and every '\r' is swallowed. For a label that is
// exactly right (neither is visible); for a field somebody types in, a byte the
// line model cannot name is a byte the caret cannot reach.
//
// So this is hard breaks only, every byte accounted for, `end` never including
// the '\n' itself. It differs from the list above in one more way, deliberately:
//
//   IT KEEPS THE TRAILING EMPTY LINE. layoutUITextLines drops it, and that was a
//   real fix — a label ending in a stray newline was drawn half a line too high.
//   In an editor the same rule would mean pressing Enter at the end puts the
//   caret on a line that does not exist. Both are right for what they serve,
//   which is why they are two functions and not one with a flag.
struct UITextLineRange
{
    std::size_t begin = 0;   // first byte of the line
    std::size_t end   = 0;   // one past its last byte, never the '\n'
};
// Never empty: an empty string is one empty line, the way an empty field has a
// place to put the caret.
HE_API std::vector<UITextLineRange> uiTextLineRanges(const std::string& text);
// Which line `byte` falls on. A byte past the end answers with the last line,
// so a caret that outlived an edit still lands somewhere real.
HE_API std::size_t uiLineOfOffset(const std::vector<UITextLineRange>& lines, std::size_t byte);

// ── Rich text: one label with more than one voice ────────────────────────────
// A markup STRING rather than a structured list of runs, and that is a decision
// rather than a shortcut: a script produces text with Set Property, so anything
// a label can be must be expressible as a string, or formatted text becomes a
// second API that only C++ can reach. UMG, TextMeshPro and Slate all landed on
// the same answer.
//
//     Hello <color=#ff8800>world</>, read the <link=terms>terms</>.
//
// Three tags — `color=#RRGGBB` or `#RRGGBBAA`, `size=<scale>`, `link=<id>` —
// closed by `</>`, which closes the innermost one. `<<` is a literal '<'.
//
// ONE rule for everything malformed: a tag that is not fully understood IS
// text. An unknown name, a broken hex value, a missing value, a `</>` with
// nothing open — all the same, no special cases to remember and nothing that
// silently eats a character somebody meant to see. This is a file format, so
// those rules are pinned by tests rather than left to the implementation.
struct UITextRun
{
    std::size_t begin = 0, end = 0;   // byte range into UIRichText::text
    // Empty = "whatever colour the element draws in". A STRING and not a
    // resolved colour, so a theme role ("accent") can move in later without the
    // saved markup meaning something different than it did.
    std::string color;
    // A SCALE on the element's font size, never pixels: the element's size is
    // already scaled by the canvas and by auto-size, and a run in absolute
    // pixels would fight both.
    float       sizeScale = 1.0f;
    std::string link;                 // empty = not clickable
};
struct UIRichText
{
    std::string            text;      // the markup with its tags removed
    std::vector<UITextRun> runs;      // in order, covering `text` exactly
    bool                   hasLinks = false;
};
// Never fails: text that parses to nothing special is one run over the whole
// string, which is exactly what a plain label is.
HE_API UIRichText uiParseRichText(const std::string& markup);
// "#RRGGBB" / "#RRGGBBAA" → a colour. False leaves `out` untouched — the same
// check the parser makes when it decides whether a tag is a tag.
HE_API bool uiParseRichColor(const std::string& s, glm::vec4& out);

// ── Laying rich text out, ONCE ───────────────────────────────────────────────
// Three things need the answer — the draw, the measure that auto-sizes the
// element, and the hit test that says which link the pointer is on — and the day
// they are three pieces of arithmetic is the day a link is clickable somewhere
// other than where it is drawn. So the layout is one function and they are its
// readers, exactly as tabLayout serves the tab strip.
//
// A `piece` is the largest stretch of one line that belongs to one run: it has a
// single colour and a single size, so drawing it is one loop.
struct UIRichPiece
{
    std::size_t run   = 0;            // index into UIRichText::runs
    std::size_t begin = 0, end = 0;   // byte range in UIRichText::text
    int   line   = 0;
    float x      = 0.0f;              // left edge, absolute pixels
    float width  = 0.0f;
    float sizePx = 0.0f;              // this piece's size (run scale applied)
    // The BASELINE is shared by every piece on the line, which is what keeps a
    // big word sitting on the same line as its small neighbours instead of
    // floating above them. `top`/`height` are the line's box, which is what the
    // hit test needs and what a piece alone cannot say.
    float baseline = 0.0f;
    float top = 0.0f, height = 0.0f;
};
struct UIRichLayout
{
    std::vector<UIRichPiece> pieces;
    glm::vec2 size{ 0.0f };           // widest line × block height
};
// Plain text laid out through here lands byte-identically where the plain path
// puts it — the mixed-size arithmetic collapses to the old formula when every
// run is the same size, and a test pins that.
HE_API UIRichLayout uiLayoutRichText(const BakedUIFont& font, const UIRichText& rt,
                                     const glm::vec2& rectPos, const glm::vec2& rectSize,
                                     float sizePx, const UITextLayout& opts);
// Draw it. `defaultColor` is what a run without a colour of its own uses.
HE_API void uiEmitRichText(const BakedUIFont& font, std::uint32_t atlasKey,
                           const UIRichText& rt, const UIRichLayout& layout,
                           const glm::vec4& defaultColor, int layer,
                           std::vector<UIRenderObject>& out);
// Which link is at this point (absolute pixels), or "" for none.
HE_API std::string uiRichLinkAt(const UIRichText& rt, const UIRichLayout& layout,
                                float x, float y);

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
