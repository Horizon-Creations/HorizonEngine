#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
#include <ProggyClean_ttf.h>

#include <Renderer/UIFont.h>
#include <algorithm>

namespace HE {

namespace
{
    // Glyph metrics parallel to the shared atlas (stb stays private to this TU).
    std::vector<stbtt_bakedchar> s_glyphs;
}

const BakedUIFont& sharedUIFont()
{
    static BakedUIFont s_font = []
    {
        BakedUIFont f;
        f.pixels.resize(static_cast<size_t>(BakedUIFont::kWidth) * BakedUIFont::kHeight, 0);
        s_glyphs.resize(96);
        const int r = stbtt_BakeFontBitmap(
            ProggyClean_data, 0, BakedUIFont::kBakePx,
            f.pixels.data(), BakedUIFont::kWidth, BakedUIFont::kHeight,
            32, 96, s_glyphs.data());
        f.ok = r > 0;

        stbtt_fontinfo info;
        if (stbtt_InitFont(&info, ProggyClean_data, 0))
        {
            int ascent = 0, descent = 0, lineGap = 0;
            stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
            const float scale = stbtt_ScaleForPixelHeight(&info, BakedUIFont::kBakePx);
            f.ascent = ascent * scale;
        }
        return f;
    }();
    return s_font;
}

void emitUITextGlyphs(const std::string& text, const glm::vec2& rectPos,
                      const glm::vec2& rectSize, float sizePx,
                      const glm::vec4& color, int layer, bool centerH,
                      std::vector<UIRenderObject>& out)
{
    const BakedUIFont& font = sharedUIFont();
    if (!font.ok || text.empty() || sizePx <= 0.0f) return;

    const float scale = sizePx / BakedUIFont::kBakePx;

    // Measure the run (advance sum) for centering.
    float runW = 0.0f;
    for (unsigned char ch : text)
        if (ch >= 32 && ch < 128)
            runW += s_glyphs[ch - 32].xadvance * scale;

    const float x = rectPos.x
        + (centerH ? std::max(0.0f, (rectSize.x - runW) * 0.5f) : 0.0f);
    // Baseline: vertical center of the rect, shifted down by half the ascent
    // (glyphs mostly sit above the baseline). Small optical tweak for Proggy.
    const float baseline = rectPos.y + rectSize.y * 0.5f
                         + (font.ascent * scale) * 0.5f - sizePx * 0.08f;

    float penX = 0.0f, penY = 0.0f; // stbtt pen at bake scale
    for (unsigned char ch : text)
    {
        if (ch < 32 || ch >= 128) continue;
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(s_glyphs.data(), BakedUIFont::kWidth, BakedUIFont::kHeight,
                           ch - 32, &penX, &penY, &q, 1 /*fill rule*/);
        UIRenderObject ro;
        ro.position = { x + q.x0 * scale, baseline + q.y0 * scale };
        ro.size     = { (q.x1 - q.x0) * scale, (q.y1 - q.y0) * scale };
        ro.uvMin    = { q.s0, q.t0 };
        ro.uvMax    = { q.s1, q.t1 };
        ro.color    = color;
        ro.type     = 2;
        ro.layer    = layer;
        out.push_back(std::move(ro));
    }
}

} // namespace HE
