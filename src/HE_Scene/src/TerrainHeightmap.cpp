#include "HorizonScene/TerrainHeightmap.h"
#include "HorizonScene/TerrainSculpt.h"
#include "HorizonScene/Components/TerrainComponent.h"
#include <ContentManager/Assets.h>

// This library's own copy of the decoder, like HE_Editor/stb_image_impl.cpp is
// the editor's: HorizonCore compiles one too (SplashScreen.cpp) but keeps it to
// itself. STATIC so this shared library does not export a second set of stbi_*
// symbols next to those two.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

namespace TerrainHeightmap
{

namespace
{
    // Grey value of one pixel, 0..1. Two integer branches on the way in, none
    // of them on the pixel data itself: the layout is validated once by
    // importPixels, so this can trust it.
    struct GreyReader
    {
        const Source& s;

        float channel(uint32_t x, uint32_t y, uint32_t c) const
        {
            const size_t i = (static_cast<size_t>(y) * s.width + x) * s.channels + c;
            if (s.bytesPerChannel == 2)
            {
                uint16_t v;
                std::memcpy(&v, s.pixels + i * 2, 2);
                return static_cast<float>(v) / 65535.0f;
            }
            return static_cast<float>(s.pixels[i]) / 255.0f;
        }

        float operator()(uint32_t x, uint32_t y) const
        {
            if (s.channels >= 3)
            {
                // Rec. 601 luma — the "convert to greyscale" every image tool
                // agrees on, so a colour picture reads the way it would after
                // the user desaturated it. For a real heightmap (R = G = B)
                // the weights sum to 1 and this is just the value.
                return 0.299f * channel(x, y, 0)
                     + 0.587f * channel(x, y, 1)
                     + 0.114f * channel(x, y, 2);
            }
            return channel(x, y, 0);
        }
    };

    // The taps one terrain vertex takes along one image axis. Up-sampling (the
    // grid is finer than the image) is the usual two bilinear taps; down-
    // sampling is a box over every pixel centre under the vertex's footprint,
    // so a 4k map onto 129 vertices averages the ~32 pixels each vertex stands
    // for instead of picking one of them and aliasing the rest away.
    struct Tap { uint32_t index; float weight; };

    void axisTaps(uint32_t gridIndex, uint32_t gridRes, uint32_t imageSize,
                  std::vector<Tap>& out)
    {
        out.clear();
        if (imageSize <= 1) { out.push_back({ 0u, 1.0f }); return; }
        const float k  = static_cast<float>(imageSize - 1) / static_cast<float>(gridRes - 1);
        const float sx = static_cast<float>(gridIndex) * k;
        const float last = static_cast<float>(imageSize - 1);
        if (k <= 1.0f)
        {
            const uint32_t i0 = static_cast<uint32_t>(std::floor(sx));
            const uint32_t i1 = std::min(i0 + 1, imageSize - 1);
            const float    f  = sx - static_cast<float>(i0);
            if (i1 == i0 || f <= 0.0f) { out.push_back({ i0, 1.0f }); return; }
            out.push_back({ i0, 1.0f - f });
            out.push_back({ i1, f });
            return;
        }
        // Pixel centres inside [sx - k/2, sx + k/2], clamped to the image; the
        // epsilon keeps a footprint edge that lands exactly on a centre in.
        const float lo = std::max(0.0f, sx - k * 0.5f - 1e-4f);
        const float hi = std::min(last, sx + k * 0.5f + 1e-4f);
        uint32_t a = static_cast<uint32_t>(std::ceil(lo));
        uint32_t b = static_cast<uint32_t>(std::floor(hi));
        if (b < a) a = b = static_cast<uint32_t>(std::lround(std::clamp(sx, 0.0f, last)));
        const float w = 1.0f / static_cast<float>(b - a + 1);
        for (uint32_t i = a; i <= b; ++i) out.push_back({ i, w });
    }

    std::string lowerExt(const std::string& path)
    {
        const size_t dot = path.find_last_of('.');
        const size_t sep = path.find_last_of("/\\");
        if (dot == std::string::npos || (sep != std::string::npos && dot < sep)) return {};
        std::string ext = path.substr(dot + 1);
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return ext;
    }
}

Result importPixels(TerrainComponent& tc, const Source& src, const Options& opts)
{
    Result r;
    if (!src.pixels || src.width == 0 || src.height == 0)
        { r.error = "The heightmap has no pixels."; return r; }
    if (src.channels < 1 || src.channels > 4)
        { r.error = "The heightmap must have 1 to 4 channels."; return r; }
    if (src.bytesPerChannel != 1 && src.bytesPerChannel != 2)
        { r.error = "The heightmap must be 8- or 16-bit."; return r; }
    if (!(tc.sizeX > 0.0f) || !(tc.sizeZ > 0.0f))
        { r.error = "The landscape has no area."; return r; }
    r.sourceWidth  = src.width;
    r.sourceHeight = src.height;
    r.sourceBits   = src.bytesPerChannel * 8;

    if (opts.adoptResolution)
        tc.resolution = std::clamp(std::max(src.width, src.height), 2u, 513u);
    // The 2ⁿ+1 snap, taken through the one function that owns it, so the
    // heights land on the grid the chunks sample rather than being resampled
    // behind the user's back on the next tick. (It also bakes the current
    // field, which is about to be overwritten — cheap, and one snap to keep
    // right instead of two.)
    TerrainSculpt::ensureHeights(tc);
    const uint32_t res = tc.resolution;

    const float range = opts.range > 0.0f ? opts.range : tc.heightScale;
    const GreyReader grey{ src };

    std::vector<float> out(static_cast<size_t>(res) * res);
    std::vector<Tap> tx, tz;
    tx.reserve(64); tz.reserve(64);
    float mn = std::numeric_limits<float>::max(), mx = -mn;
    for (uint32_t zi = 0; zi < res; ++zi)
    {
        axisTaps(zi, res, src.height, tz);
        for (uint32_t xi = 0; xi < res; ++xi)
        {
            axisTaps(xi, res, src.width, tx);
            float v = 0.0f;
            for (const Tap& z : tz)
            {
                const uint32_t row = opts.flipZ ? (src.height - 1 - z.index) : z.index;
                float rowSum = 0.0f;
                for (const Tap& x : tx) rowSum += x.weight * grey(x.index, row);
                v += z.weight * rowSum;
            }
            const float h = opts.baseHeight + range * v;
            out[static_cast<size_t>(zi) * res + xi] = h;
            mn = std::min(mn, h);
            mx = std::max(mx, h);
        }
    }

    tc.sculptHeights = std::move(out);
    tc.dirty         = true;    // every chunk: the whole field changed
    tc.regionDirty   = false;
    r.minHeight = mn;
    r.maxHeight = mx;
    r.ok = true;
    return r;
}

Result importTexture(TerrainComponent& tc, const TextureAsset& tex, const Options& opts)
{
    Result r;
    if (textureFormatIsBlock4x4(tex.format))
    {
        r.error = "The texture is block-compressed (" + std::string(
                      tex.format == TextureFormat::BC7 ? "BC7" :
                      tex.format == TextureFormat::BC3 ? "BC3" : "ASTC")
                + "); a heightmap needs an uncompressed greyscale or RGBA texture.";
        return r;
    }
    const size_t level0 = static_cast<size_t>(tex.width) * tex.height * tex.channels;
    if (tex.width == 0 || tex.height == 0 || level0 == 0 || tex.data.size() < level0)
        { r.error = "The texture has no pixel data."; return r; }
    Source src;
    src.pixels          = tex.data.data();
    src.width           = tex.width;
    src.height          = tex.height;
    src.channels        = tex.channels;
    src.bytesPerChannel = 1;
    return importPixels(tc, src, opts);
}

Result importFile(TerrainComponent& tc, const std::string& path, const Options& opts)
{
    Result r;
    const std::string ext = lowerExt(path);
    if (ext == "r16" || ext == "raw")
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) { r.error = "Could not open \"" + path + "\"."; return r; }
        const std::streamoff size = in.tellg();
        if (size <= 0 || (size % 2) != 0)
            { r.error = "A .r16/.raw heightmap must be whole 16-bit values."; return r; }
        const uint64_t count = static_cast<uint64_t>(size) / 2;
        const uint32_t side  = static_cast<uint32_t>(std::llround(std::sqrt(static_cast<double>(count))));
        if (static_cast<uint64_t>(side) * side != count || side == 0)
        {
            r.error = "A .r16/.raw heightmap must be square: " + std::to_string(count)
                    + " values is not a square number.";
            return r;
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        in.seekg(0);
        if (!in.read(reinterpret_cast<char*>(bytes.data()), size))
            { r.error = "Could not read \"" + path + "\"."; return r; }
        Source src;
        src.pixels          = bytes.data();
        src.width           = side;
        src.height          = side;
        src.channels        = 1;
        src.bytesPerChannel = 2;   // little-endian, which is the native order everywhere we run
        return importPixels(tc, src, opts);
    }

    int w = 0, h = 0, ch = 0;
    Source src;
    void* pixels = nullptr;
    if (stbi_is_16_bit(path.c_str()))
    {
        pixels = stbi_load_16(path.c_str(), &w, &h, &ch, 0);
        src.bytesPerChannel = 2;
    }
    else
    {
        pixels = stbi_load(path.c_str(), &w, &h, &ch, 0);
        src.bytesPerChannel = 1;
    }
    if (!pixels)
    {
        const char* why = stbi_failure_reason();
        r.error = "Could not read \"" + path + "\"" + (why ? std::string(": ") + why : "") + ".";
        return r;
    }
    src.pixels   = static_cast<const uint8_t*>(pixels);
    src.width    = static_cast<uint32_t>(w);
    src.height   = static_cast<uint32_t>(h);
    src.channels = static_cast<uint32_t>(ch);
    // stb hands 16-bit PNG back in native order but 16-bit PNM as the raw
    // file bytes, and the PNM spec says those are big-endian — so a 16-bit
    // PGM would come out with its bytes crossed (a 1 m rise reading as a
    // 256 m one). Swap it here, once, where the format is still known.
    if (src.bytesPerChannel == 2 && (ext == "pgm" || ext == "ppm" || ext == "pnm"))
    {
        uint8_t* p = static_cast<uint8_t*>(pixels);
        const size_t n = static_cast<size_t>(w) * h * ch;
        for (size_t i = 0; i < n; ++i) std::swap(p[i * 2], p[i * 2 + 1]);
    }
    r = importPixels(tc, src, opts);
    stbi_image_free(pixels);
    return r;
}

} // namespace TerrainHeightmap
