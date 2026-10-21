#include "ContentManager/TextureBlockDecode.h"
#include "ContentManager/TextureArrayBuild.h"

#include <algorithm>
#include <cstring>

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

} // namespace

bool textureFormatCpuDecodable(::TextureFormat f)
{
    return f == ::TextureFormat::BC1 || f == ::TextureFormat::BC3
        || f == ::TextureFormat::BC4 || f == ::TextureFormat::BC5;
}

bool decodeBlockLevelToRGBA8(::TextureFormat f, const uint8_t* src, size_t srcBytes,
                             uint32_t w, uint32_t h, uint8_t* dst)
{
    if (!src || !dst || w == 0 || h == 0 || !textureFormatCpuDecodable(f)) return false;
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

bool decodeTextureToRGBA8(const TextureAsset& in, TextureAsset& out)
{
    if (!textureFormatCpuDecodable(in.format) || in.channels != 4 || in.width == 0 || in.height == 0)
        return false;
    const uint32_t mips   = std::max<uint32_t>(1, in.mipLevels);
    const uint32_t layers = std::max<uint32_t>(1, in.layers);
    if (in.data.size() < static_cast<size_t>(layers) * textureArraySliceBytes(in.width, in.height, mips, in.format))
        return false;

    std::vector<uint8_t> data(static_cast<size_t>(layers) * textureArraySliceBytes(in.width, in.height, mips));
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
    return true;
}

} // namespace HE
