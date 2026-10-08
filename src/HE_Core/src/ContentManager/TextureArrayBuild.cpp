#include "ContentManager/TextureArrayBuild.h"

#include <algorithm>

namespace HE
{

size_t textureArraySliceBytes(uint32_t w, uint32_t h, uint32_t mips)
{
    size_t bytes = 0;
    for (uint32_t l = 0; l < mips; ++l)
    {
        bytes += static_cast<size_t>(w) * h * 4;
        w = std::max<uint32_t>(1, w >> 1);
        h = std::max<uint32_t>(1, h >> 1);
    }
    return bytes;
}

size_t textureArrayOffset(uint32_t w, uint32_t h, uint32_t mips, uint32_t slice, uint32_t level)
{
    return static_cast<size_t>(slice) * textureArraySliceBytes(w, h, mips)
         + textureArraySliceBytes(w, h, std::min(level, mips));
}

bool textureArrayPayloadValid(const TextureAsset& t)
{
    if (t.channels != 4 || t.format != ::TextureFormat::RGBA8) return false;
    if (t.width == 0 || t.height == 0 || t.layers == 0 || t.mipLevels == 0) return false;
    return t.data.size() == static_cast<size_t>(t.layers)
                          * textureArraySliceBytes(t.width, t.height, t.mipLevels);
}

namespace
{
// 2×2 box filter, odd edges clamped — the same halving as the packer's cook
// (HpakWriter halveRGBA8), so a baked array and a cooked 2D texture agree.
std::vector<uint8_t> halve(const uint8_t* src, uint32_t w, uint32_t h, uint32_t& ow, uint32_t& oh)
{
    ow = std::max<uint32_t>(1, w >> 1);
    oh = std::max<uint32_t>(1, h >> 1);
    std::vector<uint8_t> out(static_cast<size_t>(ow) * oh * 4);
    for (uint32_t y = 0; y < oh; ++y)
        for (uint32_t x = 0; x < ow; ++x)
        {
            const uint32_t x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
            const uint32_t y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
            for (int c = 0; c < 4; ++c)
            {
                const unsigned s = src[(y0 * w + x0) * 4 + c] + src[(y0 * w + x1) * 4 + c]
                                 + src[(y1 * w + x0) * 4 + c] + src[(y1 * w + x1) * 4 + c];
                out[(static_cast<size_t>(y) * ow + x) * 4 + c] = static_cast<uint8_t>((s + 2) / 4);
            }
        }
    return out;
}
} // namespace

bool buildTextureArray(const std::vector<const TextureAsset*>& slices,
                       TextureAsset& out, bool bakeMips, std::string* err)
{
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    if (slices.empty()) return fail("no slices");
    const TextureAsset* s0 = slices.front();
    if (!s0) return fail("slice 0 missing");
    const uint32_t w = s0->width, h = s0->height;
    for (size_t i = 0; i < slices.size(); ++i)
    {
        const TextureAsset* s = slices[i];
        const std::string at = "slice " + std::to_string(i) + ": ";
        if (!s)                                             return fail(at + "missing");
        if (s->channels != 4 || s->format != ::TextureFormat::RGBA8)
                                                            return fail(at + "not RGBA8");
        if (s->layers > 1)                                  return fail(at + "is itself an array");
        if (s->width != w || s->height != h)                return fail(at + "size differs from slice 0");
        if (s->data.size() < static_cast<size_t>(w) * h * 4) return fail(at + "truncated");
    }

    uint32_t mips = 1;
    if (bakeMips)
        for (uint32_t cw = w, ch = h; cw > 1 || ch > 1; ++mips)
        { cw = std::max<uint32_t>(1, cw >> 1); ch = std::max<uint32_t>(1, ch >> 1); }

    std::vector<uint8_t> data;
    data.reserve(slices.size() * textureArraySliceBytes(w, h, mips));
    for (const TextureAsset* s : slices)
    {
        const size_t level0 = static_cast<size_t>(w) * h * 4;
        data.insert(data.end(), s->data.begin(), s->data.begin() + level0);
        std::vector<uint8_t> prev(s->data.begin(), s->data.begin() + level0);
        uint32_t cw = w, ch = h;
        for (uint32_t l = 1; l < mips; ++l)
        {
            uint32_t nw = 0, nh = 0;
            std::vector<uint8_t> lvl = halve(prev.data(), cw, ch, nw, nh);
            data.insert(data.end(), lvl.begin(), lvl.end());
            prev = std::move(lvl); cw = nw; ch = nh;
        }
    }

    out.width     = w;
    out.height    = h;
    out.channels  = 4;
    out.format    = ::TextureFormat::RGBA8;
    out.srgb      = s0->srgb;
    out.mipLevels = mips;
    out.layers    = static_cast<uint32_t>(slices.size());
    out.data      = std::move(data);
    return true;
}

} // namespace HE
