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

// One contiguous block of codepoints in the atlas: `count` glyphs starting at
// `first`, stored from `offset` in the glyph vector.
struct UIGlyphRange
{
    std::uint32_t first = 0, count = 0, offset = 0;
};

// Which scripts an atlas carries BEYOND the base set. The base is always baked
// and is what an application in a Western language needs: ASCII, Latin-1
// Supplement (the umlauts and accents), Latin Extended-A (Polish, Czech,
// Turkish), the punctuation block that holds the bullet and the typographic
// quotes, and the Euro sign. Everything past that costs atlas area for scripts
// most projects never show, so it is a project decision (Preferences ▸ Project
// ▸ Fonts) rather than a default.
enum UIFontScripts : std::uint32_t
{
    UIFontScriptGreek    = 1u << 0,
    UIFontScriptCyrillic = 1u << 1,
};

// A baked font atlas: an R8 coverage bitmap + per-glyph metrics, in ranges (see
// UIFontScripts). The engine bakes the default UI font (Roboto) once; UIFontCache
// bakes imported Font assets on demand. `atlasW/atlasH/bakePx` are per-instance
// so different fonts can use different sizes — and the atlas GROWS when the
// requested scripts do not fit, so read the dimensions off the instance and
// never off the constants below.
struct BakedUIFont
{
    // What a bake ASKS for. The atlas can end up larger (a script that did not
    // fit doubles it), which is why the backends upload atlasW × atlasH.
    static constexpr int   kWidth  = 1024;
    static constexpr int   kHeight = 1024;
    static constexpr float kBakePx = 64.0f;

    int   atlasW = kWidth;
    int   atlasH = kHeight;
    float bakePx = kBakePx;
    float ascent = 0.0f;               // baseline offset from the top, at bakePx
    std::vector<uint8_t>     pixels;   // atlasW × atlasH, single channel (alpha)
    std::vector<BakedGlyph>  glyphs;   // packed, in `ranges` order
    std::vector<UIGlyphRange> ranges;  // ascending, non-overlapping
    std::uint32_t scripts = 0;         // the UIFontScripts this atlas was baked with
    bool  ok = false;

    // The glyph for `cp`, or null when this atlas does not carry it. Null covers
    // both "outside the baked ranges" and "the font has no such glyph": a caller
    // draws nothing and adds no width in either case, so a missing character
    // cannot silently become a box in one place and a gap in another.
    const BakedGlyph* glyph(std::uint32_t cp) const
    {
        for (const UIGlyphRange& r : ranges)
            if (cp >= r.first && cp < r.first + r.count)
            {
                const BakedGlyph& g = glyphs[r.offset + (cp - r.first)];
                // A codepoint the font itself lacks was packed as an empty box
                // with no advance; that is "not carried", not "a zero-width
                // character".
                return (g.x1 > g.x0 || g.xadvance > 0.0f) ? &g : nullptr;
            }
        return nullptr;
    }
};

// ── UTF-8 ─────────────────────────────────────────────────────────────────────
// The text pipeline walks bytes but draws CODEPOINTS, and the caret has to agree
// with the glyphs about where a character begins. One decoder, here, so it can
// only agree.
HE_API std::size_t uiUtf8Prev(const std::string& s, std::size_t byteIndex);
HE_API std::size_t uiUtf8Next(const std::string& s, std::size_t byteIndex);
// Nearest character boundary at or before `byteIndex` (clamped to the string).
HE_API std::size_t uiUtf8Clamp(const std::string& s, std::size_t byteIndex);
// The codepoint at `i`, with `i` advanced past it. A byte sequence that is not
// valid UTF-8 decodes as the raw byte and advances one: it then matches no range
// and draws nothing, which is what damaged text should do.
HE_API std::uint32_t uiUtf8Decode(const std::string& s, std::size_t& i);

// ── The engine's own faces ────────────────────────────────────────────────────
// Base is whatever the element draws in (the shared font, or an imported Font
// asset). Bold and Icon are faces the ENGINE ships, each with its own atlas key,
// so a backend uploads each exactly once.
enum class UIFontFace : std::uint8_t { Base = 0, Bold = 1, Icon = 2 };

// The atlas key for one of the engine's faces, baked on first use. Returns 0 —
// "no face of its own, use the base" — when the face would be the base anyway:
// with the project's weight already set to Bold there is nothing bolder, and
// `<b>` says so by being a no-op rather than by baking a second identical atlas.
HE_API std::uint32_t uiEngineFaceKey(UIFontFace face);

// The codepoint an icon NAME stands for ("home" → U+E88A), or 0 for a name the
// icon face does not have. A font knows that E88A has an outline, never that the
// outline is called "home", so the mapping is a table the engine carries.
HE_API std::uint32_t uiIconCodepoint(const std::string& name);
HE_API std::size_t   uiIconCount();
// The `n`-th icon name (alphabetical), for a picker that has to list them.
HE_API const char*   uiIconNameAt(std::size_t i);

// One icon rasterized at an EXACT size, centred in a `px` × `px` coverage
// bitmap (`out` is px*px bytes). Its own rasterization rather than a scaled
// atlas glyph, because an application icon is drawn at 512 and the atlas holds
// 40. False for a name the face does not have; `out` is then untouched.
HE_API bool uiRasterizeIcon(const std::string& name, int px, std::vector<std::uint8_t>& out);

// The weight the shared font is baked in. Bold is the default because it is what
// the engine has always drawn; a project that wants ordinary body text sets
// Regular and `<b>` then has something to be bolder THAN. Same rule as the
// scripts below: the atlas is baked once, so this is refused once it is.
HE_API bool uiSetFontWeightBold(bool bold);
HE_API bool uiFontWeightBold();

// Which scripts the atlases bake, beyond the base set (a UIFontScripts mask).
// Set from the project before any text is drawn: the backends upload each atlas
// once, so a mask that changes after the first bake would leave them holding a
// texture whose glyphs have moved. Returns false when the shared font is already
// baked and the mask differs — the caller then says so rather than showing text
// that disagrees with the setting.
HE_API bool uiSetFontScripts(std::uint32_t scripts);
HE_API std::uint32_t uiFontScripts();

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

// ── …and the lines a field actually SHOWS, wrapping included ────────────────
// The ranges above are where the author pressed Enter. Once a field wraps, that
// is no longer what an arrow key means: Down goes to the row below, and the row
// below may well be the same authored line. So a second model, and everything
// that thinks in lines — drawing, clicking, Home/End/Up/Down — reads THIS one.
//
// It is not `layoutUITextLines` with byte ranges bolted on, and that is
// deliberate: the label splitter drops the spaces at a break and swallows every
// '\r', which is right for a label and fatal here (see above). Mixing the two
// would change how every label in the tree is drawn to serve a text box.
//
// A soft break happens AFTER the run of spaces, so no byte is ever between two
// lines — `next` says where the following row starts, and the bytes in
// [`end`, `next`) are the spaces the break ate. They stay reachable by the
// caret; they are simply not drawn, and End stops in front of them rather than
// on a column that renders past the right edge.
struct UITextVisualLine
{
    std::size_t begin = 0;   // first byte of the row
    std::size_t end   = 0;   // one past the last byte drawn — where End puts the caret
    std::size_t next  = 0;   // first byte of the row below (== end when nothing was eaten)
    bool        hard  = true; // did this row end at a '\n' (or at the text's end)?
};
// Hard breaks only — the same split as uiTextLineRanges, in the shape above.
// What a field that does not wrap uses, and what a wrapping one falls back to
// before it has been drawn once (nothing knows its pixel width until then).
HE_API std::vector<UITextVisualLine> uiTextVisualLines(const std::string& text);
// …and with greedy word wrapping at `wrapWidth`, a word wider than the line
// broken inside rather than allowed to overflow. wrapWidth <= 0, an unusable
// font or a zero size all answer exactly like the function above.
HE_API std::vector<UITextVisualLine> uiTextWrapRanges(const BakedUIFont& font,
                                                      const std::string& text,
                                                      float sizePx, float wrapWidth);
// Which row `byte` is on. A byte inside the spaces a soft break ate belongs to
// the row ABOVE them — that is the row it was typed on.
HE_API std::size_t uiVisualLineOfOffset(const std::vector<UITextVisualLine>& lines,
                                        std::size_t byte);

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
    // Which FACE this run is drawn in. A second weight and an icon glyph are the
    // same problem — "these characters come from another file" — so they are the
    // same field and not two mechanisms.
    UIFontFace  face = UIFontFace::Base;
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
    // Resolved once, here, so the draw and the hit test cannot disagree about
    // which file a piece's glyphs came from.
    UIFontFace face = UIFontFace::Base;
};
struct UIRichLayout
{
    std::vector<UIRichPiece> pieces;
    glm::vec2 size{ 0.0f };           // widest line × block height
};
// Plain text laid out through here lands byte-identically where the plain path
// puts it — the mixed-size arithmetic collapses to the old formula when every
// run is the same size, and a test pins that.
// `baseKey` is the atlas key of `font` (0 = the shared default). It is here so a
// run in another FACE can be resolved while measuring and not only while drawing:
// a bold word is wider than a regular one, and a layout that learned that late
// would put the line break in a different place than the draw does.
HE_API UIRichLayout uiLayoutRichText(const BakedUIFont& font, const UIRichText& rt,
                                     const glm::vec2& rectPos, const glm::vec2& rectSize,
                                     float sizePx, const UITextLayout& opts,
                                     std::uint32_t baseKey = 0);
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
