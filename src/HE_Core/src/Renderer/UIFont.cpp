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

void emitUITextGlyphs(const BakedUIFont& font, std::uint32_t atlasKey,
                      const std::string& text, const glm::vec2& rectPos,
                      const glm::vec2& rectSize, float sizePx,
                      const glm::vec4& color, int layer, bool centerH,
                      std::vector<UIRenderObject>& out)
{
    if (!font.ok || text.empty() || sizePx <= 0.0f) return;

    const float scale = sizePx / font.bakePx;

    // Measure the run (advance sum) for centering.
    float runW = 0.0f;
    for (unsigned char ch : text)
        if (ch >= 32 && ch < 128)
            runW += font.glyphs[ch - 32].xadvance * scale;

    const float x = rectPos.x
        + (centerH ? std::max(0.0f, (rectSize.x - runW) * 0.5f) : 0.0f);
    // Baseline: vertical center of the rect, shifted down by half the ascent.
    const float baseline = rectPos.y + rectSize.y * 0.5f
                         + (font.ascent * scale) * 0.5f - sizePx * 0.08f;

    float penX = 0.0f;
    const float invW = 1.0f / (float)font.atlasW;
    const float invH = 1.0f / (float)font.atlasH;
    for (unsigned char ch : text)
    {
        if (ch < 32 || ch >= 128) continue;
        const BakedGlyph& g = font.glyphs[ch - 32];
        const float x0 = x + penX + g.xoff * scale;
        const float y0 = baseline + g.yoff * scale;
        UIRenderObject ro;
        ro.position = { x0, y0 };
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

void emitUITextGlyphs(const std::string& text, const glm::vec2& rectPos,
                      const glm::vec2& rectSize, float sizePx,
                      const glm::vec4& color, int layer, bool centerH,
                      std::vector<UIRenderObject>& out)
{
    emitUITextGlyphs(sharedUIFont(), 0, text, rectPos, rectSize, sizePx, color, layer, centerH, out);
}

} // namespace HE
