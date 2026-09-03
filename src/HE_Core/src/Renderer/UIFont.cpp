#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
#include <cstdint>
// Roboto Condensed Bold — the same smooth TTF the editor UI uses, so live
// widgets render like the designer preview (was the blocky ProggyClean bitmap).
#include <Roboto_ttf.h>

#include <Renderer/UIFont.h>
#include <algorithm>
#include <string>      // std::stof — the markup's size value
#include <unordered_map>

namespace HE {

// ── UTF-8 ────────────────────────────────────────────────────────────────────
namespace
{
    // A continuation byte is 10xxxxxx: never a character boundary.
    bool isUtf8Cont(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }
}

std::size_t uiUtf8Prev(const std::string& s, std::size_t i)
{
    if (i == 0) return 0;
    if (i > s.size()) i = s.size();
    --i;
    while (i > 0 && isUtf8Cont(s[i])) --i;
    return i;
}

std::size_t uiUtf8Next(const std::string& s, std::size_t i)
{
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && isUtf8Cont(s[i])) ++i;
    return i;
}

std::size_t uiUtf8Clamp(const std::string& s, std::size_t i)
{
    if (i >= s.size()) return s.size();
    while (i > 0 && isUtf8Cont(s[i])) --i;
    return i;
}

std::uint32_t uiUtf8Decode(const std::string& s, std::size_t& i)
{
    if (i >= s.size()) { i = s.size(); return 0; }
    const unsigned char b0 = static_cast<unsigned char>(s[i]);
    // How many bytes the lead byte PROMISES, and the bits it contributes.
    int extra = 0;
    std::uint32_t cp = 0;
    if      (b0 < 0x80) { ++i; return b0; }
    else if ((b0 & 0xE0) == 0xC0) { extra = 1; cp = b0 & 0x1Fu; }
    else if ((b0 & 0xF0) == 0xE0) { extra = 2; cp = b0 & 0x0Fu; }
    else if ((b0 & 0xF8) == 0xF0) { extra = 3; cp = b0 & 0x07u; }
    else { ++i; return b0; } // a continuation byte or 0xFE/0xFF standing alone
    // A promise the string does not keep is not a character: hand back the lead
    // byte and move one, so a truncated sequence costs one glyph and not the
    // rest of the line.
    for (int k = 1; k <= extra; ++k)
    {
        if (i + k >= s.size() || !isUtf8Cont(s[i + k])) { ++i; return b0; }
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
    }
    i += extra + 1;
    return cp;
}

namespace
{
    // One block of codepoints to bake.
    struct CpRange { std::uint32_t first, count; };

    // The base set plus whatever `scripts` asks for, ascending. The base is the
    // promise every project gets without deciding anything: Latin as it is
    // actually written, including the punctuation a text field produces on its
    // own (U+2022 is the password dot).
    std::vector<CpRange> rangesFor(std::uint32_t scripts)
    {
        std::vector<CpRange> r;
        r.push_back({ 0x0020, 0x5F });                       // ASCII 0x20..0x7E
        r.push_back({ 0x00A0, 0x60 });                       // Latin-1 Supplement
        r.push_back({ 0x0100, 0x80 });                       // Latin Extended-A
        if (scripts & UIFontScriptGreek)    r.push_back({ 0x0370, 0x90 });
        if (scripts & UIFontScriptCyrillic) r.push_back({ 0x0400, 0x100 });
        r.push_back({ 0x2010, 0x18 });                       // dashes, quotes, bullet, ellipsis
        r.push_back({ 0x20AC, 0x01 });                       // €
        return r;
    }

    // One packing attempt at the atlas size `f` currently carries. Leaves f.pixels
    // /glyphs/ranges filled on success and undefined (about to be retried or
    // replaced) on failure.
    //
    // Gather/pack/render by hand rather than stbtt_PackFontRanges, for one
    // reason: with skip_missing on, that function reports failure for every
    // codepoint the FONT does not have. "Roboto has no Cyrillic" would then read
    // exactly like "the atlas is too small", the growth below would double the
    // atlas three times over nothing and still end in the fallback. Here the
    // rectangles answer the only question that matters — did everything the font
    // does have find a place.
    bool packOnce(const unsigned char* ttf, BakedUIFont& f, const std::vector<CpRange>& rs)
    {
        stbtt_fontinfo info;
        if (!stbtt_InitFont(&info, ttf, 0)) return false;

        f.pixels.assign(static_cast<size_t>(f.atlasW) * f.atlasH, 0);
        f.glyphs.clear();
        f.ranges.clear();

        std::vector<stbtt_pack_range> pr(rs.size());
        std::vector<std::vector<stbtt_packedchar>> chars(rs.size());
        std::size_t total = 0;
        for (size_t i = 0; i < rs.size(); ++i)
        {
            chars[i].assign(rs[i].count, stbtt_packedchar{});
            pr[i] = stbtt_pack_range{};
            // POSITIVE size, which is stbtt_ScaleForPixelHeight — the same scale
            // stbtt_BakeFontBitmap used before this. STBTT_POINT_SIZE would ask
            // for ScaleForMappingEmToPixels instead and move every glyph in every
            // label that already exists.
            pr[i].font_size                        = f.bakePx;
            pr[i].first_unicode_codepoint_in_range = static_cast<int>(rs[i].first);
            pr[i].num_chars                        = static_cast<int>(rs[i].count);
            pr[i].chardata_for_range               = chars[i].data();
            total += rs[i].count;
        }

        stbtt_pack_context spc;
        if (!stbtt_PackBegin(&spc, f.pixels.data(), f.atlasW, f.atlasH, 0, 1, nullptr))
            return false;
        // A codepoint the font does not have stays zeroed rather than becoming the
        // font's box glyph: BakedUIFont::glyph reads that as "not carried", and a
        // gap is honest where a box would claim the character was drawn.
        stbtt_PackSetSkipMissingCodepoints(&spc, 1);

        std::vector<stbrp_rect> rects(total);
        const int n = stbtt_PackFontRangesGatherRects(&spc, &info, pr.data(),
                                                      static_cast<int>(pr.size()), rects.data());
        stbtt_PackFontRangesPackRects(&spc, rects.data(), n);
        bool fits = true;
        for (int i = 0; i < n; ++i)
            if (rects[i].w > 0 && rects[i].h > 0 && !rects[i].was_packed) { fits = false; break; }
        if (fits)
            stbtt_PackFontRangesRenderIntoRects(&spc, &info, pr.data(),
                                                static_cast<int>(pr.size()), rects.data());
        stbtt_PackEnd(&spc);
        if (!fits) return false;

        for (size_t i = 0; i < rs.size(); ++i)
        {
            f.ranges.push_back({ rs[i].first, rs[i].count,
                                 static_cast<std::uint32_t>(f.glyphs.size()) });
            for (const stbtt_packedchar& pc : chars[i])
                f.glyphs.push_back({ (float)pc.x0, (float)pc.y0, (float)pc.x1, (float)pc.y1,
                                     pc.xoff, pc.yoff, pc.xadvance });
        }
        return true;
    }

    // Bake `ttf` into `f` (using f.atlasW/atlasH/bakePx as the REQUEST). Fills
    // pixels + glyphs + ranges + ascent + ok.
    void bakeInto(const unsigned char* ttf, BakedUIFont& f, std::uint32_t scripts)
    {
        f.scripts = scripts;
        f.ok      = false;
        const std::vector<CpRange> want = rangesFor(scripts);
        const int reqW = f.atlasW, reqH = f.atlasH;

        // Grow rather than lose a script: Latin alone fits 1024², Cyrillic on top
        // of it does not, and an atlas that is one size too small would drop
        // exactly the characters the project asked for.
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            if (packOnce(ttf, f, want)) { f.ok = true; break; }
            if (f.atlasW >= 4096 && f.atlasH >= 4096) break;
            f.atlasW = std::min(4096, f.atlasW * 2);
            f.atlasH = std::min(4096, f.atlasH * 2);
        }
        if (!f.ok)
        {
            // Nothing fits, so fall back to ASCII at the size that was asked for.
            // A missing script is a gap in a sentence; a failed bake is an
            // application with no text at all, and that is the worse of the two.
            f.atlasW  = reqW;
            f.atlasH  = reqH;
            f.scripts = 0;
            f.ok      = packOnce(ttf, f, { { 0x0020, 0x5F } });
        }

        stbtt_fontinfo info;
        if (stbtt_InitFont(&info, ttf, 0))
        {
            int ascent = 0, descent = 0, lineGap = 0;
            stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
            f.ascent = ascent * stbtt_ScaleForPixelHeight(&info, f.bakePx);
        }
    }

    std::uint32_t g_scripts = 0;
    bool          g_sharedBaked = false;
}

bool uiSetFontScripts(std::uint32_t scripts)
{
    if (g_sharedBaked) return scripts == g_scripts;
    g_scripts = scripts;
    return true;
}

std::uint32_t uiFontScripts() { return g_scripts; }

const BakedUIFont& sharedUIFont()
{
    static BakedUIFont s_font = []
    {
        BakedUIFont f; // atlasW/atlasH/bakePx default to the shared-atlas constants
        bakeInto(Roboto_data, f, g_scripts);
        g_sharedBaked = true;
        return f;
    }();
    return s_font;
}

BakedUIFont bakeDefaultUIFont(float bakePx, int atlasW, int atlasH)
{
    BakedUIFont f;
    f.bakePx = std::clamp(bakePx, 6.0f, 256.0f);
    f.atlasW = std::clamp(atlasW, 64, 4096);
    f.atlasH = std::clamp(atlasH, 64, 4096);
    bakeInto(Roboto_data, f, g_scripts);
    return f;
}

namespace UIFontCache
{
    namespace
    {
        std::unordered_map<std::uint32_t, BakedUIFont>& cache()
        {
            static std::unordered_map<std::uint32_t, BakedUIFont> c;
            return c;
        }
    }

    std::uint32_t keyFor(std::uint64_t stableId, const std::vector<uint8_t>& ttf, float bakePx)
    {
        if (ttf.empty()) return 0;
        const int px = std::clamp((int)(bakePx + 0.5f), 8, 120);
        // Fold the font id + bake size + the scripts into a non-zero 32-bit key:
        // an imported font baked for Latin and one baked for Cyrillic are two
        // atlases, and the renderer uploads them under two keys.
        std::uint64_t h = stableId * 1099511628211ull ^ (std::uint64_t)px;
        h ^= (std::uint64_t)uiFontScripts() * 0x9E3779B97F4A7C15ull;
        std::uint32_t key = (std::uint32_t)(h ^ (h >> 32));
        if (key == 0) key = 1;
        auto& c = cache();
        if (c.find(key) == c.end())
        {
            BakedUIFont f;
            f.atlasW = 1024; f.atlasH = 1024; f.bakePx = (float)px;
            bakeInto(ttf.data(), f, uiFontScripts());
            if (!f.ok) return 0; // baking failed → caller uses the shared font
            c.emplace(key, std::move(f));
        }
        return key;
    }

    const BakedUIFont* find(std::uint32_t key)
    {
        if (key == 0) return &sharedUIFont();
        auto& c = cache();
        auto it = c.find(key);
        return it != c.end() ? &it->second : nullptr;
    }
}

namespace
{
    // Advance width of one line at `sizePx`. A character the atlas does not carry
    // has no metrics and contributes nothing, exactly as the emit loop draws
    // nothing for it — the two have to agree or the caret lands beside the glyph.
    float lineWidth(const BakedUIFont& font, const std::string& s, float scale)
    {
        float w = 0.0f;
        for (std::size_t i = 0; i < s.size(); )
            if (const BakedGlyph* g = font.glyph(uiUtf8Decode(s, i))) w += g->xadvance * scale;
        return w;
    }
} // namespace

std::vector<std::string> layoutUITextLines(const BakedUIFont& font, const std::string& text,
                                           float sizePx, float wrapWidth, bool wrap)
{
    std::vector<std::string> lines;
    if (!font.ok || sizePx <= 0.0f) { lines.push_back(text); return lines; }
    const float scale = sizePx / font.bakePx;

    // Hard breaks first — '\r' is swallowed so CRLF text doesn't render a box.
    std::string cur;
    for (char c : text)
    {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else if (c != '\r') cur.push_back(c);
    }
    lines.push_back(cur);
    // ── A trailing break does not start a line ───────────────────────────────
    // "Option 2\n" is one line, not one line and an empty one. It matters far
    // more than it looks: an empty last line is half the block's height, so a
    // vertically centred label with a stray newline is drawn hard against the
    // top of its rect — which is exactly what an author gets by pressing Enter
    // in the Text box to mean "done". ImGui's own CalcTextSizeA drops it too,
    // which is why the designer showed such a label correctly while the engine
    // did not, and a designer that disagrees with the engine is worse than
    // either being wrong.
    //
    // Exactly ONE is dropped, so a deliberate blank line ("a\n\n") still leaves
    // one, and an empty string is still one (empty) line rather than none.
    if (lines.size() > 1 && lines.back().empty()) lines.pop_back();

    if (!wrap || wrapWidth <= 0.0f) return lines;

    // Greedy word wrap on each hard line. A single word wider than the line is
    // hard-broken so it can never overflow the rect.
    // Trailing spaces would offset a centred line and inflate the fit test.
    auto trimmed = [](std::string s) {
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    };
    std::vector<std::string> wrapped;
    for (const std::string& line : lines)
    {
        if (lineWidth(font, line, scale) <= wrapWidth) { wrapped.push_back(line); continue; }
        std::string acc;
        size_t i = 0;
        while (i < line.size())
        {
            // Next word plus the run of spaces that follows it.
            size_t e = line.find(' ', i);
            if (e == std::string::npos) e = line.size();
            while (e < line.size() && line[e] == ' ') ++e;
            std::string word = line.substr(i, e - i);
            i = e;

            // Doesn't fit after what's already on this line → flush the line.
            if (!acc.empty() && lineWidth(font, trimmed(acc + word), scale) > wrapWidth)
            {
                wrapped.push_back(trimmed(acc));
                acc.clear();
            }
            // Even alone the word overflows → hard-break it, longest prefix first.
            // The prefix always takes at least one character, so this terminates.
            while (acc.empty() && word.size() > 1 &&
                   lineWidth(font, trimmed(word), scale) > wrapWidth)
            {
                // Character by character, not byte by byte: a break inside a
                // two-byte umlaut would leave half a character on each line, and
                // neither half is anything.
                std::string head;
                for (std::size_t k = 0; k < word.size(); )
                {
                    const std::size_t next = uiUtf8Next(word, k);
                    const std::string ch = word.substr(k, next - k);
                    if (!head.empty() && lineWidth(font, head + ch, scale) > wrapWidth) break;
                    head += ch;
                    k = next;
                }
                wrapped.push_back(head);
                word.erase(0, head.size());
            }
            acc += word;
        }
        wrapped.push_back(trimmed(acc));
    }
    return wrapped;
}

// ── Rich text markup ─────────────────────────────────────────────────────────

bool uiParseRichColor(const std::string& s, glm::vec4& out)
{
    if (s.size() != 7 && s.size() != 9) return false;
    if (s[0] != '#') return false;
    auto hex = [](char c, int& v) {
        if (c >= '0' && c <= '9') { v = c - '0';      return true; }
        if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; return true; }
        if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; return true; }
        return false;
    };
    float ch[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    const int count = s.size() == 7 ? 3 : 4;
    for (int i = 0; i < count; ++i)
    {
        int hi = 0, lo = 0;
        if (!hex(s[1 + i * 2], hi) || !hex(s[2 + i * 2], lo)) return false;
        ch[i] = static_cast<float>(hi * 16 + lo) / 255.0f;
    }
    out = glm::vec4(ch[0], ch[1], ch[2], ch[3]);
    return true;
}

namespace
{
    // A run of attributes while the parser is inside a tag. The stack IS the
    // nesting: `<link=x><color=#f00>a</>b</>` gives "a" both and "b" only the
    // link, which is what anyone who has written markup expects.
    struct RichScope { std::string color; float sizeScale = 1.0f; std::string link; };

    // Does `s` at `i` start a tag this parser understands, and where does it end?
    // Returns false for everything else — the one rule the whole format rests on:
    // a tag that is not fully understood is TEXT. No special case per mistake,
    // nothing silently swallowed, and a stray '<' in a sentence stays a '<'.
    bool readTag(const std::string& s, std::size_t i, std::size_t& outEnd,
                 std::string& outName, std::string& outValue)
    {
        if (i >= s.size() || s[i] != '<') return false;
        const std::size_t close = s.find('>', i + 1);
        if (close == std::string::npos) return false;
        const std::string body = s.substr(i + 1, close - i - 1);
        outEnd = close + 1;
        if (body == "/") { outName = "/"; outValue.clear(); return true; }
        const std::size_t eq = body.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 >= body.size()) return false;
        outName  = body.substr(0, eq);
        outValue = body.substr(eq + 1);
        if (outName == "color")
        { glm::vec4 ignored{}; return uiParseRichColor(outValue, ignored); }
        if (outName == "size")
        {
            try { const float v = std::stof(outValue); return v > 0.0f && v < 100.0f; }
            catch (...) { return false; }
        }
        if (outName == "link") return true;   // any id will do; it is the graph's word
        return false;                          // unknown name: text, like everything else
    }
} // namespace

UIRichText uiParseRichText(const std::string& markup)
{
    UIRichText out;
    std::vector<RichScope> stack;
    // The run being built. Flushed whenever the attributes change, so the run
    // list covers the plain text exactly once with no empty runs in it.
    UITextRun cur;
    auto attrs = [&]() -> RichScope { return stack.empty() ? RichScope{} : stack.back(); };
    auto flush = [&]() {
        cur.end = out.text.size();
        if (cur.end > cur.begin) out.runs.push_back(cur);
        cur = UITextRun{};
        cur.begin = out.text.size();
        const RichScope a = attrs();
        cur.color = a.color; cur.sizeScale = a.sizeScale; cur.link = a.link;
    };

    for (std::size_t i = 0; i < markup.size(); )
    {
        // The escape, before anything else looks at a '<'.
        if (markup[i] == '<' && i + 1 < markup.size() && markup[i + 1] == '<')
        { out.text.push_back('<'); i += 2; continue; }

        std::size_t end = 0;
        std::string name, value;
        if (markup[i] == '<' && readTag(markup, i, end, name, value))
        {
            if (name == "/")
            {
                // Nothing open: a `</>` that closes nothing is text, by the one
                // rule. It is also the only way to write one literally.
                if (stack.empty()) { out.text += "</>"; i = end; continue; }
                flush();
                stack.pop_back();
                const RichScope a = attrs();
                cur.color = a.color; cur.sizeScale = a.sizeScale; cur.link = a.link;
            }
            else
            {
                flush();
                RichScope a = attrs();
                if (name == "color") a.color = value;
                else if (name == "size") a.sizeScale = std::stof(value);
                else if (name == "link") { a.link = value; out.hasLinks = true; }
                stack.push_back(a);
                cur.color = a.color; cur.sizeScale = a.sizeScale; cur.link = a.link;
            }
            i = end;
            continue;
        }
        out.text.push_back(markup[i]);
        ++i;
    }
    // An unclosed tag simply runs to the end — the alternative is dropping text
    // somebody wrote because they forgot four characters.
    cur.end = out.text.size();
    if (cur.end > cur.begin) out.runs.push_back(cur);
    // A label with no markup at all is ONE run over the whole string, which is
    // what makes every consumer able to treat plain and rich text alike.
    if (out.runs.empty() && !out.text.empty())
        out.runs.push_back({ 0, out.text.size(), {}, 1.0f, {} });
    return out;
}

UIRichLayout uiLayoutRichText(const BakedUIFont& font, const UIRichText& rt,
                              const glm::vec2& rectPos, const glm::vec2& rectSize,
                              float sizePx, const UITextLayout& opts)
{
    UIRichLayout out;
    if (!font.ok || sizePx <= 0.0f || rt.text.empty()) return out;

    // Which run a byte belongs to. The runs cover the text exactly once and in
    // order, so this walks forward with the scan rather than searching.
    std::size_t runCursor = 0;
    auto runAt = [&](std::size_t b) {
        while (runCursor + 1 < rt.runs.size() && b >= rt.runs[runCursor].end) ++runCursor;
        while (runCursor > 0 && b < rt.runs[runCursor].begin) --runCursor;
        return runCursor;
    };
    auto sizeAt = [&](std::size_t b) {
        if (rt.runs.empty()) return sizePx;
        return sizePx * rt.runs[runAt(b)].sizeScale;
    };
    // Advance of the CHARACTER at `b` — the byte walks below all step with
    // uiUtf8Next, so `b` is always a character boundary and a two-byte umlaut is
    // measured once, not twice.
    auto advanceAt = [&](std::size_t b) {
        std::size_t j = b;
        const BakedGlyph* g = font.glyph(uiUtf8Decode(rt.text, j));
        return g ? g->xadvance * (sizeAt(b) / font.bakePx) : 0.0f;
    };

    // ── Lines ────────────────────────────────────────────────────────────────
    // Byte ranges, because a piece has to name bytes to know its run. Hard breaks
    // always; greedy word wrap on top when asked, measured with each character's
    // OWN size — a wrap that measured everything at the element's size would put
    // a large word past the edge.
    struct Line { std::size_t begin = 0, end = 0; };
    std::vector<Line> lines;
    {
        Line cur{ 0, 0 };
        std::size_t i = 0;
        float width = 0.0f;          // of what is on the line, trailing spaces included
        float sinceBreak = 0.0f;     // width of the word being built
        std::size_t lastBreak = std::string::npos;   // byte after the last space run
        auto push = [&](std::size_t end) {
            cur.end = end;
            lines.push_back(cur);
            cur = Line{ end, end };
            width = sinceBreak = 0.0f;
            lastBreak = std::string::npos;
        };
        while (i < rt.text.size())
        {
            const char c = rt.text[i];
            if (c == '\r') { ++i; continue; }
            if (c == '\n') { push(i); cur.begin = i + 1; ++i; continue; }
            const float adv = advanceAt(i);
            const bool wrapHere = opts.wrap && rectSize.x > 0.0f &&
                                  width + adv > rectSize.x && i > cur.begin;
            if (wrapHere)
            {
                // Break at the last space if the line has one, otherwise inside
                // the word — a single word wider than the line must never
                // overflow the rect, the same rule the plain path follows.
                if (lastBreak != std::string::npos && lastBreak > cur.begin)
                {
                    const std::size_t at = lastBreak;
                    push(at);
                    cur.begin = at;
                    i = at;
                    // Re-measure the word that moved down with us.
                    continue;
                }
                push(i);
                cur.begin = i;
                continue;
            }
            if (c == ' ') { lastBreak = i + 1; sinceBreak = 0.0f; }
            else sinceBreak += adv;
            width += adv;
            i = uiUtf8Next(rt.text, i);
        }
        cur.end = rt.text.size();
        lines.push_back(cur);
        // A trailing break does not start a line — the same rule (and the same
        // reason) as layoutUITextLines: an empty last line is half a line of
        // height, and a centred label with a stray newline sits too high.
        if (lines.size() > 1 && lines.back().begin >= lines.back().end) lines.pop_back();
    }

    // ── Where each line sits ─────────────────────────────────────────────────
    // A line is as tall as its TALLEST run, and every piece on it shares one
    // baseline. With one size throughout, all of this collapses to the plain
    // path's formula, which is what keeps existing labels exactly where they are.
    std::vector<float> lineSize(lines.size(), sizePx), lineWidthPx(lines.size(), 0.0f);
    for (std::size_t li = 0; li < lines.size(); ++li)
    {
        float mx = 0.0f, w = 0.0f;
        std::size_t lastInk = lines[li].begin;
        for (std::size_t b = lines[li].begin; b < lines[li].end; )
        {
            const std::size_t next = uiUtf8Next(rt.text, b);
            mx = std::max(mx, sizeAt(b));
            w += advanceAt(b);
            if (rt.text[b] != ' ') lastInk = next;
            b = next;
        }
        // Trailing spaces neither widen the line nor shift a centred one.
        float trimmed = 0.0f;
        for (std::size_t b = lines[li].begin; b < lastInk; b = uiUtf8Next(rt.text, b))
            trimmed += advanceAt(b);
        lineSize[li]    = mx > 0.0f ? mx : sizePx;
        lineWidthPx[li] = lines[li].end > lines[li].begin ? trimmed : 0.0f;
        (void)w;
    }
    float blockHeight = lineSize.empty() ? 0.0f : lineSize.back();
    for (std::size_t li = 0; li + 1 < lines.size(); ++li)
        blockHeight += lineSize[li] * opts.lineSpacing;

    float blockTop = rectPos.y + (rectSize.y - blockHeight) * 0.5f;      // 1 = middle
    if (opts.alignV == 0)      blockTop = rectPos.y;
    else if (opts.alignV == 2) blockTop = rectPos.y + rectSize.y - blockHeight;

    // ── Pieces ───────────────────────────────────────────────────────────────
    float centre = blockTop + (lines.empty() ? 0.0f : lineSize[0] * 0.5f);
    for (std::size_t li = 0; li < lines.size(); ++li)
    {
        const float ls = lineSize[li];
        const float slack = std::max(0.0f, rectSize.x - lineWidthPx[li]);
        float x = rectPos.x + (opts.alignH == 1 ? slack * 0.5f
                             : opts.alignH == 2 ? slack : 0.0f);
        const float baseline = centre + (font.ascent * (ls / font.bakePx)) * 0.5f - ls * 0.08f;
        std::size_t b = lines[li].begin;
        while (b < lines[li].end)
        {
            const std::size_t r = runAt(b);
            std::size_t e = b;
            float w = 0.0f;
            while (e < lines[li].end && runAt(e) == r)
            { w += advanceAt(e); e = uiUtf8Next(rt.text, e); }
            UIRichPiece pc;
            pc.run = r; pc.begin = b; pc.end = e;
            pc.line = static_cast<int>(li);
            pc.x = x; pc.width = w;
            pc.sizePx = rt.runs.empty() ? sizePx : sizePx * rt.runs[r].sizeScale;
            pc.baseline = baseline;
            pc.top = centre - ls * 0.5f; pc.height = ls;
            out.pieces.push_back(pc);
            x += w;
            b = e;
        }
        out.size.x = std::max(out.size.x, lineWidthPx[li]);
        if (li + 1 < lines.size()) centre += ls * opts.lineSpacing;
    }
    out.size.y = blockHeight;
    return out;
}

void uiEmitRichText(const BakedUIFont& font, std::uint32_t atlasKey,
                    const UIRichText& rt, const UIRichLayout& layout,
                    const glm::vec4& defaultColor, int layer,
                    std::vector<UIRenderObject>& out)
{
    if (!font.ok) return;
    const float invW = 1.0f / (float)font.atlasW;
    const float invH = 1.0f / (float)font.atlasH;
    for (const UIRichPiece& pc : layout.pieces)
    {
        glm::vec4 colour = defaultColor;
        if (pc.run < rt.runs.size() && !rt.runs[pc.run].color.empty())
        {
            glm::vec4 c{};
            // Alpha travels with the element's own: a run says which colour, the
            // element says how visible it is (inherited opacity is applied to
            // every quad afterwards anyway, so this only keeps a run from being
            // MORE opaque than the label it sits in).
            if (uiParseRichColor(rt.runs[pc.run].color, c))
                colour = glm::vec4(glm::vec3(c), c.a * defaultColor.a);
        }
        const float scale = pc.sizePx / font.bakePx;
        float penX = pc.x;
        std::size_t b = pc.begin;
        while (b < pc.end && b < rt.text.size())
        {
            const BakedGlyph* gp = font.glyph(uiUtf8Decode(rt.text, b));
            if (!gp) continue;
            const BakedGlyph& g = *gp;
            UIRenderObject ro;
            ro.position = { penX + g.xoff * scale, pc.baseline + g.yoff * scale };
            ro.size     = { (g.x1 - g.x0) * scale, (g.y1 - g.y0) * scale };
            ro.uvMin    = { g.x0 * invW, g.y0 * invH };
            ro.uvMax    = { g.x1 * invW, g.y1 * invH };
            ro.color    = colour;
            ro.type     = 2;
            ro.layer    = layer;
            ro.fontAtlasKey = atlasKey;
            out.push_back(std::move(ro));
            penX += g.xadvance * scale;
        }
    }
}

std::string uiRichLinkAt(const UIRichText& rt, const UIRichLayout& layout,
                         float x, float y)
{
    for (const UIRichPiece& pc : layout.pieces)
    {
        if (pc.run >= rt.runs.size() || rt.runs[pc.run].link.empty()) continue;
        if (x < pc.x || x > pc.x + pc.width) continue;
        if (y < pc.top || y > pc.top + pc.height) continue;
        return rt.runs[pc.run].link;
    }
    return {};
}

std::vector<UITextLineRange> uiTextLineRanges(const std::string& text)
{
    std::vector<UITextLineRange> lines;
    std::size_t begin = 0;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != '\n') continue;
        std::size_t end = i;
        // A CRLF's '\r' is not part of the line's text, but it IS part of its
        // bytes — so it is left OUT of the range's end and the next line starts
        // after the '\n' regardless. That keeps the ranges a partition of the
        // string even for text pasted from a Windows editor.
        if (end > begin && text[end - 1] == '\r') --end;
        lines.push_back({ begin, end });
        begin = i + 1;
    }
    // The tail, always — this is the trailing empty line the label splitter
    // drops on purpose and an editor must keep (see the header).
    lines.push_back({ begin, text.size() });
    return lines;
}

std::size_t uiLineOfOffset(const std::vector<UITextLineRange>& lines, std::size_t byte)
{
    if (lines.empty()) return 0;
    for (std::size_t i = 0; i < lines.size(); ++i)
        // `<= end` and not `< end`: the caret sits at the END of a line as often
        // as inside it, and that position belongs to THIS line rather than to
        // the start of the next one.
        if (byte <= lines[i].end) return i;
    return lines.size() - 1;
}

glm::vec2 measureUIText(const BakedUIFont& font, const std::string& text, float sizePx,
                        float wrapWidth, const UITextLayout& opts)
{
    if (!font.ok || sizePx <= 0.0f) return { 0.0f, 0.0f };
    const float scale = sizePx / font.bakePx;
    const std::vector<std::string> lines =
        layoutUITextLines(font, text, sizePx, wrapWidth, opts.wrap);
    float w = 0.0f;
    for (const std::string& l : lines) w = std::max(w, lineWidth(font, l, scale));
    const float step = sizePx * opts.lineSpacing;
    const float h    = static_cast<float>(lines.size() - 1) * step + sizePx;
    return { w, h };
}

glm::vec2 measureUIText(const std::string& text, float sizePx,
                        float wrapWidth, const UITextLayout& opts)
{
    return measureUIText(sharedUIFont(), text, sizePx, wrapWidth, opts);
}

void emitUITextGlyphs(const BakedUIFont& font, std::uint32_t atlasKey,
                      const std::string& text, const glm::vec2& rectPos,
                      const glm::vec2& rectSize, float sizePx,
                      const glm::vec4& color, int layer, const UITextLayout& opts,
                      std::vector<UIRenderObject>& out)
{
    if (!font.ok || text.empty() || sizePx <= 0.0f) return;

    const float scale = sizePx / font.bakePx;
    const std::vector<std::string> lines =
        layoutUITextLines(font, text, sizePx, rectSize.x, opts.wrap);

    // Where the whole block of lines sits vertically. Middle is the historic
    // behaviour and the default; Top and Bottom put the block's own height
    // against the matching edge. Line i sits `step` below i-1 either way.
    const float step        = sizePx * opts.lineSpacing;
    const float blockHeight = static_cast<float>(lines.size() - 1) * step + sizePx;
    float blockCentre = rectPos.y + rectSize.y * 0.5f;              // 1 = middle
    if (opts.alignV == 0) blockCentre = rectPos.y + blockHeight * 0.5f;
    else if (opts.alignV == 2)
        blockCentre = rectPos.y + rectSize.y - blockHeight * 0.5f;
    const float firstCentre = blockCentre - static_cast<float>(lines.size() - 1) * step * 0.5f;

    const float invW = 1.0f / (float)font.atlasW;
    const float invH = 1.0f / (float)font.atlasH;

    for (size_t li = 0; li < lines.size(); ++li)
    {
        const std::string& line = lines[li];
        if (line.empty()) continue;                    // blank line still advances

        const float runW = lineWidth(font, line, scale);
        // Per LINE, not per block: centring a paragraph centres each of its
        // lines, which is what centring text means.
        const float slack = std::max(0.0f, rectSize.x - runW);
        const float x = rectPos.x + (opts.alignH == 1 ? slack * 0.5f
                                   : opts.alignH == 2 ? slack
                                                      : 0.0f);
        // Baseline: line centre shifted down by half the ascent (as before).
        const float baseline = firstCentre + static_cast<float>(li) * step
                             + (font.ascent * scale) * 0.5f - sizePx * 0.08f;

        float penX = 0.0f;
        for (std::size_t b = 0; b < line.size(); )
        {
            const BakedGlyph* gp = font.glyph(uiUtf8Decode(line, b));
            if (!gp) continue;
            const BakedGlyph& g = *gp;
            UIRenderObject ro;
            ro.position = { x + penX + g.xoff * scale, baseline + g.yoff * scale };
            ro.size     = { (g.x1 - g.x0) * scale, (g.y1 - g.y0) * scale };
            ro.uvMin    = { g.x0 * invW, g.y0 * invH };
            ro.uvMax    = { g.x1 * invW, g.y1 * invH };
            ro.color    = color;
            ro.type     = 2;
            ro.layer    = layer;
            ro.fontAtlasKey = atlasKey;
            out.push_back(std::move(ro));
            penX += g.xadvance * scale;
        }
    }
}

void emitUITextGlyphs(const BakedUIFont& font, std::uint32_t atlasKey,
                      const std::string& text, const glm::vec2& rectPos,
                      const glm::vec2& rectSize, float sizePx,
                      const glm::vec4& color, int layer, bool centerH,
                      std::vector<UIRenderObject>& out)
{
    UITextLayout opts; opts.alignH = centerH ? 1 : 0;   // no wrapping unless asked for
    emitUITextGlyphs(font, atlasKey, text, rectPos, rectSize, sizePx, color, layer, opts, out);
}

void emitUITextGlyphs(const std::string& text, const glm::vec2& rectPos,
                      const glm::vec2& rectSize, float sizePx,
                      const glm::vec4& color, int layer, bool centerH,
                      std::vector<UIRenderObject>& out)
{
    emitUITextGlyphs(sharedUIFont(), 0, text, rectPos, rectSize, sizePx, color, layer, centerH, out);
}

} // namespace HE
