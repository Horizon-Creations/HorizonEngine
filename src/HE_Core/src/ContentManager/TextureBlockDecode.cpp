#include "ContentManager/TextureBlockDecode.h"
#include "ContentManager/TextureArrayBuild.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#ifdef HE_HAVE_ASTCENC
#  include <astcenc.h>
#endif

namespace HE
{

namespace
{

// 5:6:5 -> 8:8:8, replicating the high bits (what D3D/GL reference decoders do).
void rgb565(uint16_t c, int rgb[3])
{
    const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    rgb[0] = (r << 3) | (r >> 2);
    rgb[1] = (g << 2) | (g >> 4);
    rgb[2] = (b << 3) | (b >> 2);
}

// One BC1 colour block (8 B) -> 16 RGBA texels.
void decodeBc1Block(const uint8_t* blk, uint8_t out[16][4])
{
    const uint16_t c0 = static_cast<uint16_t>(blk[0] | (blk[1] << 8));
    const uint16_t c1 = static_cast<uint16_t>(blk[2] | (blk[3] << 8));
    int e0[3], e1[3];
    rgb565(c0, e0); rgb565(c1, e1);
    int pal[4][4];
    for (int i = 0; i < 3; ++i)
    {
        pal[0][i] = e0[i];
        pal[1][i] = e1[i];
        if (c0 > c1)
        {
            pal[2][i] = (2 * e0[i] + e1[i]) / 3;
            pal[3][i] = (e0[i] + 2 * e1[i]) / 3;
        }
        else
        {
            pal[2][i] = (e0[i] + e1[i]) / 2;
            pal[3][i] = 0;
        }
    }
    pal[0][3] = pal[1][3] = pal[2][3] = 255;
    pal[3][3] = c0 > c1 ? 255 : 0; // 3-colour mode: index 3 is transparent black
    uint32_t bits = static_cast<uint32_t>(blk[4]) | (static_cast<uint32_t>(blk[5]) << 8)
                  | (static_cast<uint32_t>(blk[6]) << 16) | (static_cast<uint32_t>(blk[7]) << 24);
    for (int t = 0; t < 16; ++t, bits >>= 2)
        for (int c = 0; c < 4; ++c)
            out[t][c] = static_cast<uint8_t>(pal[bits & 3][c]);
}

// One BC4 / DXT5-alpha channel block (8 B) -> 16 values.
void decodeBc4Block(const uint8_t* blk, uint8_t out[16])
{
    const int a0 = blk[0], a1 = blk[1];
    int pal[8];
    pal[0] = a0; pal[1] = a1;
    if (a0 > a1)
        for (int i = 2; i < 8; ++i) pal[i] = ((8 - i) * a0 + (i - 1) * a1) / 7;
    else
    {
        for (int i = 2; i < 6; ++i) pal[i] = ((6 - i) * a0 + (i - 1) * a1) / 5;
        pal[6] = 0; pal[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(blk[2 + i]) << (8 * i);
    for (int t = 0; t < 16; ++t, bits >>= 3)
        out[t] = static_cast<uint8_t>(pal[bits & 7]);
}

#include "Bc7DecodeTables.inl"

// One BC7 block (16 bytes) into 16 RGBA texels — every mode of the format, since
// a pak cooked elsewhere may use any of them, not only the four bc7enc writes.
void decodeBc7Block(const uint8_t* blk, uint8_t out[16][4])
{
    struct Mode { uint8_t ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2; };
    static constexpr Mode kModes[8] = {
        { 3, 4, 0, 0, 4, 0, 1, 0, 3, 0 }, { 2, 6, 0, 0, 6, 0, 0, 1, 3, 0 },
        { 3, 6, 0, 0, 5, 0, 0, 0, 2, 0 }, { 2, 6, 0, 0, 7, 0, 1, 0, 2, 0 },
        { 1, 0, 2, 1, 5, 6, 0, 0, 2, 3 }, { 1, 0, 2, 0, 7, 8, 0, 0, 2, 2 },
        { 1, 0, 0, 0, 7, 7, 1, 0, 4, 0 }, { 2, 6, 0, 0, 5, 5, 1, 0, 2, 0 },
    };
    uint32_t bit = 0;
    auto read = [&](uint32_t n) {
        uint32_t v = 0;
        for (uint32_t i = 0; i < n; ++i, ++bit)
            v |= static_cast<uint32_t>((blk[bit >> 3] >> (bit & 7)) & 1u) << i;
        return v;
    };
    uint32_t mode = 0;
    while (mode < 8 && read(1) == 0) ++mode;
    if (mode == 8) { std::memset(out, 0, 16 * 4); return; }   // reserved: transparent black
    const Mode& m = kModes[mode];
    const uint32_t part = read(m.pb), rot = read(m.rb), isb = read(m.isb);

    uint32_t ep[3][2][4] = {};   // [subset][endpoint][channel]
    for (uint32_t c = 0; c < 3; ++c)
        for (uint32_t s = 0; s < m.ns; ++s)
            for (uint32_t e = 0; e < 2; ++e) ep[s][e][c] = read(m.cb);
    if (m.ab)
        for (uint32_t s = 0; s < m.ns; ++s)
            for (uint32_t e = 0; e < 2; ++e) ep[s][e][3] = read(m.ab);
    uint32_t cbits = m.cb, abits = m.ab;
    if (m.epb || m.spb)
    {
        for (uint32_t s = 0; s < m.ns; ++s)
        {
            const uint32_t shared = m.spb ? read(1) : 0;
            for (uint32_t e = 0; e < 2; ++e)
            {
                const uint32_t p = m.epb ? read(1) : shared;
                for (uint32_t c = 0; c < 4; ++c) ep[s][e][c] = (ep[s][e][c] << 1) | p;
            }
        }
        ++cbits; if (abits) ++abits;
    }
    for (uint32_t s = 0; s < m.ns; ++s)
        for (uint32_t e = 0; e < 2; ++e)
        {
            for (uint32_t c = 0; c < 3; ++c)
            {
                uint32_t v = ep[s][e][c] << (8 - cbits);
                ep[s][e][c] = v | (v >> cbits);
            }
            if (abits) { uint32_t v = ep[s][e][3] << (8 - abits); ep[s][e][3] = v | (v >> abits); }
            else ep[s][e][3] = 255;
        }

    auto subsetOf = [&](uint32_t t) -> uint32_t {
        return m.ns == 2 ? kBc7_partition2[part * 16 + t]
             : m.ns == 3 ? kBc7_partition3[part * 16 + t] : 0u;
    };
    auto isAnchor = [&](uint32_t t) {
        if (t == 0) return true;
        if (m.ns == 2) return t == kBc7_table_anchor_index_second_subset[part];
        if (m.ns == 3) return t == kBc7_table_anchor_index_third_subset_1[part] ||
                              t == kBc7_table_anchor_index_third_subset_2[part];
        return false;
    };
    uint32_t idx[16], idx2[16] = {};
    for (uint32_t t = 0; t < 16; ++t) idx[t] = read(m.ib - (isAnchor(t) ? 1 : 0));
    if (m.ib2)
        for (uint32_t t = 0; t < 16; ++t) idx2[t] = read(m.ib2 - (t == 0 ? 1 : 0));

    auto weight = [](uint32_t bits, uint32_t i) -> uint32_t {
        return bits == 2 ? kBc7_weights2[i] : bits == 3 ? kBc7_weights3[i] : kBc7_weights4[i];
    };
    for (uint32_t t = 0; t < 16; ++t)
    {
        const uint32_t s = subsetOf(t);
        uint32_t cw, aw;
        if (!m.ib2)            { cw = weight(m.ib, idx[t]);   aw = cw; }
        else if (isb == 0)     { cw = weight(m.ib, idx[t]);   aw = weight(m.ib2, idx2[t]); }
        else                   { cw = weight(m.ib2, idx2[t]); aw = weight(m.ib, idx[t]); }
        for (uint32_t c = 0; c < 4; ++c)
        {
            const uint32_t w = c == 3 ? aw : cw;
            out[t][c] = static_cast<uint8_t>(((64 - w) * ep[s][0][c] + w * ep[s][1][c] + 32) >> 6);
        }
        if (rot) std::swap(out[t][3], out[t][rot - 1]);
    }
}

} // namespace

bool textureFormatCpuDecodable(::TextureFormat f)
{
    return f == ::TextureFormat::BC1 || f == ::TextureFormat::BC3
        || f == ::TextureFormat::BC4 || f == ::TextureFormat::BC5;
}

bool decodeBlockLevelToRGBA8(::TextureFormat f, const uint8_t* src, size_t srcBytes,
                             uint32_t w, uint32_t h, uint8_t* dst)
{
    const bool bc7 = f == ::TextureFormat::BC7;   // decoded here, but not a GPU fallback (see header)
    if (!src || !dst || w == 0 || h == 0 || !(bc7 || textureFormatCpuDecodable(f))) return false;
    if (srcBytes < textureLevelBytes(f, w, h)) return false;
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bb = textureFormatBlockBytes(f);
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx)
        {
            const uint8_t* blk = src + (static_cast<size_t>(by) * bw + bx) * bb;
            uint8_t px[16][4];
            switch (f)
            {
            case ::TextureFormat::BC1:
                decodeBc1Block(blk, px);
                break;
            case ::TextureFormat::BC7:
                decodeBc7Block(blk, px);
                break;
            case ::TextureFormat::BC3:
            {
                uint8_t a[16];
                decodeBc4Block(blk, a);
                decodeBc1Block(blk + 8, px);
                for (int t = 0; t < 16; ++t) px[t][3] = a[t];
                break;
            }
            case ::TextureFormat::BC4:
            {
                uint8_t r[16];
                decodeBc4Block(blk, r);
                for (int t = 0; t < 16; ++t) { px[t][0] = r[t]; px[t][1] = 0; px[t][2] = 0; px[t][3] = 255; }
                break;
            }
            default: // BC5
            {
                uint8_t r[16], g[16];
                decodeBc4Block(blk, r);
                decodeBc4Block(blk + 8, g);
                for (int t = 0; t < 16; ++t) { px[t][0] = r[t]; px[t][1] = g[t]; px[t][2] = 0; px[t][3] = 255; }
                break;
            }
            }
            for (uint32_t ty = 0; ty < 4; ++ty)
            {
                const uint32_t y = by * 4 + ty;
                if (y >= h) break;
                for (uint32_t tx = 0; tx < 4; ++tx)
                {
                    const uint32_t x = bx * 4 + tx;
                    if (x >= w) break;
                    std::memcpy(dst + (static_cast<size_t>(y) * w + x) * 4, px[ty * 4 + tx], 4);
                }
            }
        }
    return true;
}

bool decodeBaseLevelToRGBA8(const TextureAsset& in, std::vector<uint8_t>& out)
{
    const uint32_t w = in.width, h = in.height;
    if (w == 0 || h == 0 || in.data.empty()) return false;
    const size_t px = static_cast<size_t>(w) * h * 4;
    if (in.format == ::TextureFormat::RGBA8)
    {
        if (in.data.size() < px) return false;
        out.assign(in.data.begin(), in.data.begin() + static_cast<std::ptrdiff_t>(px));
        return true;
    }
    if (textureFormatCpuDecodable(in.format) || in.format == ::TextureFormat::BC7)
    {
        out.resize(px);
        return decodeBlockLevelToRGBA8(in.format, in.data.data(), in.data.size(), w, h, out.data());
    }
#ifdef HE_HAVE_ASTCENC
    if (in.format == ::TextureFormat::ASTC_4x4)
    {
        const size_t bytes = textureLevelBytes(in.format, w, h);
        if (in.data.size() < bytes) return false;
        // Plain LDR, as the cook encodes it (HpakWriter encodeAstc4x4): sRGB stays in the bytes.
        astcenc_config cfg{};
        if (astcenc_config_init(ASTCENC_PRF_LDR, 4, 4, 1, ASTCENC_PRE_FASTEST,
                                ASTCENC_FLG_DECOMPRESS_ONLY, &cfg) != ASTCENC_SUCCESS)
            return false;
        astcenc_context* ctx = nullptr;
        if (astcenc_context_alloc(&cfg, 1, &ctx) != ASTCENC_SUCCESS) return false;
        out.resize(px);
        astcenc_image img{};
        img.dim_x = w; img.dim_y = h; img.dim_z = 1;
        img.data_type = ASTCENC_TYPE_U8;
        void* slices[1] = { out.data() };
        img.data = slices;
        const astcenc_swizzle swz{ ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };
        const astcenc_error e = astcenc_decompress_image(ctx, in.data.data(), bytes, &img, &swz, 0);
        astcenc_context_free(ctx);
        return e == ASTCENC_SUCCESS;
    }
#endif
    return false;
}

bool decodeTextureToRGBA8(const TextureAsset& in, TextureAsset& out)
{
    if (!textureFormatCpuDecodable(in.format) || in.channels != 4 || in.width == 0 || in.height == 0)
        return false;
    const uint32_t mips   = std::max<uint32_t>(1, in.mipLevels);
    const uint32_t layers = std::max<uint32_t>(1, in.layers);
    if (in.data.size() < static_cast<size_t>(layers) * textureArraySliceBytes(in.width, in.height, mips, in.format))
        return false;

    std::vector<uint8_t> data(static_cast<size_t>(layers) * textureArraySliceBytes(in.width, in.height, mips, ::TextureFormat::RGBA8));
    size_t src = 0, dst = 0;
    for (uint32_t s = 0; s < layers; ++s)
    {
        uint32_t w = in.width, h = in.height;
        for (uint32_t l = 0; l < mips; ++l)
        {
            const size_t sb = textureLevelBytes(in.format, w, h);
            if (!decodeBlockLevelToRGBA8(in.format, in.data.data() + src, sb, w, h, data.data() + dst))
                return false;
            src += sb;
            dst += static_cast<size_t>(w) * h * 4;
            w = std::max<uint32_t>(1, w >> 1);
            h = std::max<uint32_t>(1, h >> 1);
        }
    }
    out.id        = in.id;
    out.type      = in.type;
    out.name      = in.name;
    out.path      = in.path;
    out.width     = in.width;
    out.height    = in.height;
    out.channels  = 4;
    out.format    = ::TextureFormat::RGBA8;
    out.srgb      = in.srgb;
    out.mipLevels = mips;
    out.layers    = layers;
    out.data      = std::move(data);
    out.sliceMeans = in.sliceMeans;
    return true;
}

bool textureArraySliceMean(::TextureFormat f, const uint8_t* data, size_t dataBytes,
                           uint32_t w, uint32_t h, uint32_t mips, bool srgb,
                           uint32_t slice, float rgb[3])
{
    if (!data || w == 0 || h == 0) return false;
    if (f != ::TextureFormat::RGBA8 && !textureFormatCpuDecodable(f)) return false;
    mips = std::max<uint32_t>(1, mips);
    // Smallest level with both sides <= 16 (or the last one stored).
    uint32_t level = 0, lw = w, lh = h;
    while (level + 1 < mips && (lw > 16 || lh > 16))
    {
        ++level;
        lw = std::max<uint32_t>(1, lw >> 1);
        lh = std::max<uint32_t>(1, lh >> 1);
    }
    const size_t off   = textureArrayOffset(w, h, mips, slice, level, f);
    const size_t bytes = textureLevelBytes(f, lw, lh);
    if (off + bytes > dataBytes) return false;
    // Level 0 of a mip-less 4k array is still 16M texels: walk a <= 16x16 grid.
    const uint32_t sx = std::max<uint32_t>(1, lw / 16), sy = std::max<uint32_t>(1, lh / 16);
    // The texels of the level: in place for RGBA8, decoded into `scratch` for a block format
    // (decoding a whole array here costs ~440 ms for a 13-slice 2K BC3 array, so only this level).
    std::vector<uint8_t> scratch;
    const uint8_t* px = data + off;
    if (f != ::TextureFormat::RGBA8)
    {
        scratch.resize(static_cast<size_t>(lw) * lh * 4);
        if (!decodeBlockLevelToRGBA8(f, px, bytes, lw, lh, scratch.data())) return false;
        px = scratch.data();
    }
    auto decode = [srgb](uint8_t b)
    {
        const float c = b / 255.0f;
        if (!srgb) return c;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    float sum[3] = { 0.0f, 0.0f, 0.0f };
    uint32_t n = 0;
    for (uint32_t y = 0; y < lh; y += sy)
        for (uint32_t x = 0; x < lw; x += sx, ++n)
        {
            const uint8_t* p = px + (static_cast<size_t>(y) * lw + x) * 4;
            sum[0] += decode(p[0]); sum[1] += decode(p[1]); sum[2] += decode(p[2]);
        }
    const float inv = 1.0f / static_cast<float>(std::max<uint32_t>(n, 1));
    for (int i = 0; i < 3; ++i) rgb[i] = sum[i] * inv;
    return true;
}

bool textureArraySliceMean(const TextureAsset& t, uint32_t slice, float rgb[3])
{
    return textureArraySliceMean(t.format, t.data.data(), t.data.size(), t.width, t.height,
                                 t.mipLevels, t.srgb, slice, rgb);
}

bool bakeTextureArraySliceMeans(const uint8_t* rgba8, size_t bytes, uint32_t w, uint32_t h,
                                uint32_t mips, uint32_t layers, bool srgb, std::vector<float>& out)
{
    out.clear();
    out.reserve(static_cast<size_t>(layers) * 3);
    for (uint32_t s = 0; s < layers; ++s)
    {
        float m[3];
        if (!textureArraySliceMean(::TextureFormat::RGBA8, rgba8, bytes, w, h, mips, srgb, s, m))
        {
            out.clear();
            return false;
        }
        out.insert(out.end(), m, m + 3);
    }
    return true;
}

} // namespace HE
