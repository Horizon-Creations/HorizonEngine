#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
#include <cstdint>
// Roboto Condensed Bold — the same smooth TTF the editor UI uses, so live
// widgets render like the designer preview (was the blocky ProggyClean bitmap).
#include <Roboto_ttf.h>

#include <Renderer/UIFont.h>
#include <algorithm>
#include <unordered_map>

namespace HE {

namespace
{
    // Bake ASCII 32..127 of `ttf` into `f` (using f.atlasW/atlasH/bakePx). Fills
    // pixels + glyphs + ascent + ok. `f` must have its atlas dims/size set.
    void bakeInto(const unsigned char* ttf, BakedUIFont& f)
    {
        f.pixels.assign(static_cast<size_t>(f.atlasW) * f.atlasH, 0);
        stbtt_bakedchar chars[96];
        const int r = stbtt_BakeFontBitmap(ttf, 0, f.bakePx, f.pixels.data(),
                                           f.atlasW, f.atlasH, 32, 96, chars);
        f.ok = r > 0;
        for (int i = 0; i < 96; ++i)
        {
            f.glyphs[i] = { (float)chars[i].x0, (float)chars[i].y0,
                            (float)chars[i].x1, (float)chars[i].y1,
                            chars[i].xoff, chars[i].yoff, chars[i].xadvance };
        }
        stbtt_fontinfo info;
        if (stbtt_InitFont(&info, ttf, 0))
        {
            int ascent = 0, descent = 0, lineGap = 0;
            stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
            f.ascent = ascent * stbtt_ScaleForPixelHeight(&info, f.bakePx);
        }
    }
}

const BakedUIFont& sharedUIFont()
{
    static BakedUIFont s_font = []
    {
        BakedUIFont f; // atlasW/atlasH/bakePx default to the shared-atlas constants
        bakeInto(Roboto_data, f);
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
    bakeInto(Roboto_data, f);
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
        // Fold the font id + bake size into a non-zero 32-bit key.
        std::uint64_t h = stableId * 1099511628211ull ^ (std::uint64_t)px;
        std::uint32_t key = (std::uint32_t)(h ^ (h >> 32));
        if (key == 0) key = 1;
        auto& c = cache();
        if (c.find(key) == c.end())
        {
            BakedUIFont f;
            f.atlasW = 1024; f.atlasH = 1024; f.bakePx = (float)px;
            bakeInto(ttf.data(), f);
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
    // Advance width of one line at `sizePx` (glyphs outside ASCII 32..127 have no
    // metrics and contribute nothing, exactly as the emit loop skips them).
    float lineWidth(const BakedUIFont& font, const std::string& s, float scale)
    {
        float w = 0.0f;
        for (unsigned char ch : s)
            if (ch >= 32 && ch < 128) w += font.glyphs[ch - 32].xadvance * scale;
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
                std::string head;
                for (char c : word)
                {
                    if (!head.empty() && lineWidth(font, head + c, scale) > wrapWidth) break;
                    head.push_back(c);
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
        for (unsigned char ch : line)
        {
            if (ch < 32 || ch >= 128) continue;
            const BakedGlyph& g = font.glyphs[ch - 32];
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
