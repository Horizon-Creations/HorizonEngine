#include "../../../include/Backends/Software/SoftwareRaster.h"

#include <Renderer/UIFont.h>

#include <algorithm>
#include <cmath>

namespace HE::sw
{
namespace
{
    inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    // GLSL/MSL smoothstep, spelled out: the shaders' falloff is this curve and
    // nothing else, so a shadow that is soft on the GPU is soft here too.
    inline float smoothstepf(float e0, float e1, float x)
    {
        if (e1 <= e0) return x < e0 ? 0.0f : 1.0f;
        const float t = clamp01((x - e0) / (e1 - e0));
        return t * t * (3.0f - 2.0f * t);
    }

    inline float mixf(float a, float b, float t) { return a + (b - a) * t; }

    // One texel of a single-channel atlas, sampled bilinearly. Bilinear rather
    // than nearest because the GPU path filters linearly: nearest-neighbour text
    // does not read as "the software renderer", it reads as broken.
    float sampleR8(const std::uint8_t* px, int w, int h, float u, float v)
    {
        if (!px || w <= 0 || h <= 0) return 0.0f;
        const float x = clamp01(u) * static_cast<float>(w) - 0.5f;
        const float y = clamp01(v) * static_cast<float>(h) - 0.5f;
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
        auto at = [&](int xi, int yi) -> float
        {
            xi = std::clamp(xi, 0, w - 1);
            yi = std::clamp(yi, 0, h - 1);
            return static_cast<float>(px[static_cast<std::size_t>(yi) * w + xi]) / 255.0f;
        };
        return mixf(mixf(at(x0, y0), at(x0 + 1, y0), fx),
                    mixf(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx), fy);
    }

    glm::vec4 sampleRGBA8(const std::uint8_t* px, int w, int h, float u, float v)
    {
        if (!px || w <= 0 || h <= 0) return glm::vec4(1.0f);
        const float x = clamp01(u) * static_cast<float>(w) - 0.5f;
        const float y = clamp01(v) * static_cast<float>(h) - 0.5f;
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
        auto at = [&](int xi, int yi)
        {
            xi = std::clamp(xi, 0, w - 1);
            yi = std::clamp(yi, 0, h - 1);
            const std::uint8_t* p = px + (static_cast<std::size_t>(yi) * w + xi) * 4;
            return glm::vec4(p[0], p[1], p[2], p[3]) * (1.0f / 255.0f);
        };
        const glm::vec4 a = at(x0, y0), b = at(x0 + 1, y0);
        const glm::vec4 c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
        return glm::mix(glm::mix(a, b, fx), glm::mix(c, d, fx), fy);
    }
}

void Image::resize(int w, int h)
{
    width = std::max(0, w);
    height = std::max(0, h);
    rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
}

void Image::clear(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
{
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4)
    { rgba[i] = r; rgba[i + 1] = g; rgba[i + 2] = b; rgba[i + 3] = a; }
}

void Image::pixel(int x, int y, std::uint8_t& r, std::uint8_t& g,
                  std::uint8_t& b, std::uint8_t& a) const
{
    if (x < 0 || y < 0 || x >= width || y >= height) { r = g = b = a = 0; return; }
    const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
    r = rgba[i]; g = rgba[i + 1]; b = rgba[i + 2]; a = rgba[i + 3];
}

float roundedBoxSDF(float px, float py, float halfW, float halfH, const glm::vec4& radii)
{
    float r = (px > 0.0f) ? ((py > 0.0f) ? radii.z : radii.y)
                          : ((py > 0.0f) ? radii.w : radii.x);
    r = std::min(r, std::min(halfW, halfH));
    const float qx = std::abs(px) - (halfW - r);
    const float qy = std::abs(py) - (halfH - r);
    const float outside = std::sqrt(std::max(qx, 0.0f) * std::max(qx, 0.0f) +
                                   std::max(qy, 0.0f) * std::max(qy, 0.0f));
    return outside + std::min(std::max(qx, qy), 0.0f) - r;
}

void draw(Image& target, const std::vector<UIRenderObject>& objects, const glm::vec4& clip,
          TextureResolver texResolve, void* texUser)
{
    if (!target.valid()) return;

    // The window's own rectangle, narrowed by the caller's (a partial redraw).
    int wx0 = 0, wy0 = 0, wx1 = target.width, wy1 = target.height;
    if (clip.z > 0.0f && clip.w > 0.0f)
    {
        wx0 = std::max(wx0, static_cast<int>(std::floor(clip.x)));
        wy0 = std::max(wy0, static_cast<int>(std::floor(clip.y)));
        wx1 = std::min(wx1, static_cast<int>(std::ceil(clip.x + clip.z)));
        wy1 = std::min(wy1, static_cast<int>(std::ceil(clip.y + clip.w)));
    }
    if (wx1 <= wx0 || wy1 <= wy0) return;

    const HE::BakedUIFont& shared = HE::sharedUIFont();

    for (const UIRenderObject& o : objects)
    {
        if (o.size.x <= 0.0f || o.size.y <= 0.0f) continue;
        if (o.color.a <= 0.0f && o.type != 2) continue;

        // Which atlas a glyph quad samples; 0 = the shared default font.
        const HE::BakedUIFont* font = &shared;
        if (o.type == 2 && o.fontAtlasKey != 0)
            if (const HE::BakedUIFont* f = HE::UIFontCache::find(o.fontAtlasKey)) font = f;

        // A picture on the quad, if there is one and if anybody can find it. A
        // MATERIAL is deliberately not looked at: it is a translated shader
        // graph, which a CPU rasterizer cannot run, so such a quad falls back to
        // Schicht 0 plus its tint — the documented D5 contract, not an accident.
        TextureView tv{};
        if (o.type == 0 && texResolve && o.textureAssetId != HE::UUID{})
            tv = texResolve(o.textureAssetId, texUser);

        // The object's own scissor, intersected with the region being drawn.
        int ox0 = wx0, oy0 = wy0, ox1 = wx1, oy1 = wy1;
        if (o.clipRect.z > 0.0f && o.clipRect.w > 0.0f)
        {
            ox0 = std::max(ox0, static_cast<int>(std::floor(o.clipRect.x)));
            oy0 = std::max(oy0, static_cast<int>(std::floor(o.clipRect.y)));
            ox1 = std::min(ox1, static_cast<int>(std::ceil(o.clipRect.x + o.clipRect.z)));
            oy1 = std::min(oy1, static_cast<int>(std::ceil(o.clipRect.y + o.clipRect.w)));
        }
        if (ox1 <= ox0 || oy1 <= oy0) continue;

        // The quad's own axis-aligned bounds. A rotated quad's corners swing out
        // past its rectangle, so the box that has to be walked is the one around
        // the FOUR TURNED CORNERS — anything smaller clips the quad's own points
        // off, which is the classic rotation bug.
        const float rot = o.rotation;
        const float cs = std::cos(rot), sn = std::sin(rot);
        float bx0 = o.position.x, by0 = o.position.y;
        float bx1 = o.position.x + o.size.x, by1 = o.position.y + o.size.y;
        if (rot != 0.0f)
        {
            const float cx[4] = { bx0, bx1, bx0, bx1 };
            const float cy[4] = { by0, by0, by1, by1 };
            float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f;
            for (int i = 0; i < 4; ++i)
            {
                const float dx = cx[i] - o.rotationPivot.x, dy = cy[i] - o.rotationPivot.y;
                const float rx = o.rotationPivot.x + dx * cs - dy * sn;
                const float ry = o.rotationPivot.y + dx * sn + dy * cs;
                mnx = std::min(mnx, rx); mxx = std::max(mxx, rx);
                mny = std::min(mny, ry); mxy = std::max(mxy, ry);
            }
            bx0 = mnx; by0 = mny; bx1 = mxx; by1 = mxy;
        }
        const int px0 = std::max(ox0, static_cast<int>(std::floor(bx0)));
        const int py0 = std::max(oy0, static_cast<int>(std::floor(by0)));
        const int px1 = std::min(ox1, static_cast<int>(std::ceil(bx1)) + 1);
        const int py1 = std::min(oy1, static_cast<int>(std::ceil(by1)) + 1);
        if (px1 <= px0 || py1 <= py0) continue;

        const float maxR = std::max(std::max(o.cornerRadius.x, o.cornerRadius.y),
                                    std::max(o.cornerRadius.z, o.cornerRadius.w));
        const glm::vec2 half = o.size * 0.5f;

        for (int y = py0; y < py1; ++y)
        for (int x = px0; x < px1; ++x)
        {
            // Pixel centre, turned back into the quad's own upright space.
            float sxp = static_cast<float>(x) + 0.5f;
            float syp = static_cast<float>(y) + 0.5f;
            if (rot != 0.0f)
            {
                const float dx = sxp - o.rotationPivot.x, dy = syp - o.rotationPivot.y;
                sxp = o.rotationPivot.x + dx * cs + dy * sn;
                syp = o.rotationPivot.y - dx * sn + dy * cs;
            }
            const float lu = (sxp - o.position.x) / o.size.x;
            const float lv = (syp - o.position.y) / o.size.y;
            if (lu < 0.0f || lu > 1.0f || lv < 0.0f || lv > 1.0f) continue;

            // ── The fragment, ported from uiFragment / kUIFS ─────────────────
            glm::vec4 src;
            if (o.type == 2)
            {
                // A glyph: alpha out of the font atlas, tinted by the colour.
                const float u = mixf(o.uvMin.x, o.uvMax.x, lu);
                const float v = mixf(o.uvMin.y, o.uvMax.y, lv);
                const float a = font && font->ok
                    ? sampleR8(font->pixels.data(), font->atlasW, font->atlasH, u, v)
                    : 0.0f;
                src = glm::vec4(o.color.r, o.color.g, o.color.b, o.color.a * a);
            }
            else if (tv.rgba)
            {
                // A textured quad: the picture, tinted, cut to the same rounded
                // shape the solid path uses (an avatar with rounded corners).
                const float u = mixf(o.uvMin.x, o.uvMax.x, lu);
                const float v = mixf(o.uvMin.y, o.uvMax.y, lv);
                const glm::vec4 t = sampleRGBA8(tv.rgba, tv.width, tv.height, u, v);
                src = glm::vec4(o.color.r * t.r, o.color.g * t.g, o.color.b * t.b,
                                o.color.a * t.a);
                if (maxR > 0.0f)
                {
                    const float d = roundedBoxSDF((lu - 0.5f) * o.size.x,
                                                  (lv - 0.5f) * o.size.y,
                                                  half.x, half.y, o.cornerRadius);
                    src.a *= clamp01(0.5f - d);
                }
            }
            else
            {
                // The surface colour before any shape is cut out of it.
                glm::vec4 fill = o.color;
                if (o.gradient)
                {
                    float t;
                    if (o.gradientShape == 1)
                    {
                        const float dx = (lu - 0.5f) * o.size.x;
                        const float dy = (lv - 0.5f) * o.size.y;
                        const float far = std::max(1e-4f, std::sqrt(half.x * half.x +
                                                                    half.y * half.y));
                        t = clamp01(std::sqrt(dx * dx + dy * dy) / far);
                    }
                    else
                    {
                        const float a = o.gradientAngleDeg * 0.017453292f;
                        t = clamp01((lu - 0.5f) * std::sin(a) + (lv - 0.5f) * std::cos(a) + 0.5f);
                    }
                    fill = glm::mix(o.color, o.gradientColor, t);
                }

                if (maxR <= 0.0f && o.borderWidth <= 0.0f &&
                    o.blur <= 0.0f && o.innerShadowBlur <= 0.0f)
                {
                    src = fill;
                }
                else
                {
                    // A blurred quad IS a drop shadow: the producer grew the rect
                    // by the blur on every side, so the shape sits inset by that
                    // much (see UIRenderObject::blur).
                    const float hw = half.x - o.blur, hh = half.y - o.blur;
                    const float d = roundedBoxSDF((lu - 0.5f) * o.size.x,
                                                  (lv - 0.5f) * o.size.y,
                                                  hw, hh, o.cornerRadius);
                    const float cov = (o.blur > 0.0f)
                        ? (1.0f - smoothstepf(-o.blur, o.blur, d))
                        : clamp01(0.5f - d);
                    if (o.blur > 0.0f)
                        src = glm::vec4(fill.r, fill.g, fill.b, fill.a * cov);
                    else
                    {
                        if (o.innerShadowBlur > 0.0f)
                        {
                            const float t = 1.0f - smoothstepf(0.0f, o.innerShadowBlur, -d);
                            const float ia = o.innerShadowColor.a * clamp01(t);
                            fill = glm::vec4(glm::mix(glm::vec3(fill), glm::vec3(o.innerShadowColor), ia),
                                             fill.a);
                        }
                        if (o.borderWidth > 0.0f)
                        {
                            const float inner = clamp01(0.5f - (d + o.borderWidth));
                            const glm::vec3 rgb = glm::mix(glm::vec3(o.borderColor),
                                                           glm::vec3(fill), inner);
                            const float a = mixf(o.borderColor.a, fill.a, inner);
                            fill = glm::vec4(rgb, a);
                        }
                        src = glm::vec4(fill.r, fill.g, fill.b, fill.a * cov);
                    }
                }
            }

            if (src.a <= 0.0f) continue;
            // Source-over, straight 8-bit, no gamma correction (see the header).
            const std::size_t i = (static_cast<std::size_t>(y) * target.width + x) * 4;
            const float sa = clamp01(src.a);
            auto over = [&](int c, float s)
            {
                const float dv = static_cast<float>(target.rgba[i + c]) / 255.0f;
                const float v = clamp01(s) * sa + dv * (1.0f - sa);
                target.rgba[i + c] = static_cast<std::uint8_t>(clamp01(v) * 255.0f + 0.5f);
            };
            over(0, src.r); over(1, src.g); over(2, src.b);
            const float da = static_cast<float>(target.rgba[i + 3]) / 255.0f;
            target.rgba[i + 3] =
                static_cast<std::uint8_t>(clamp01(sa + da * (1.0f - sa)) * 255.0f + 0.5f);
        }
    }
}

} // namespace HE::sw
