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
    auto advanceAt = [&](std::size_t b) {
        const unsigned char ch = static_cast<unsigned char>(rt.text[b]);
        if (ch < 32 || ch >= 128) return 0.0f;
        return font.glyphs[ch - 32].xadvance * (sizeAt(b) / font.bakePx);
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
            ++i;
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
        for (std::size_t b = lines[li].begin; b < lines[li].end; ++b)
        {
            mx = std::max(mx, sizeAt(b));
            w += advanceAt(b);
            if (rt.text[b] != ' ') lastInk = b + 1;
        }
        // Trailing spaces neither widen the line nor shift a centred one.
        float trimmed = 0.0f;
        for (std::size_t b = lines[li].begin; b < lastInk; ++b) trimmed += advanceAt(b);
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
            while (e < lines[li].end && runAt(e) == r) { w += advanceAt(e); ++e; }
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
        for (std::size_t b = pc.begin; b < pc.end && b < rt.text.size(); ++b)
        {
            const unsigned char ch = static_cast<unsigned char>(rt.text[b]);
            if (ch < 32 || ch >= 128) continue;
            const BakedGlyph& g = font.glyphs[ch - 32];
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
