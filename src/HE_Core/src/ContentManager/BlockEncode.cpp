#include <ContentManager/BlockEncode.h>

#ifdef HE_HAVE_ASTCENC
#  include <astcenc.h>
#endif
#ifdef HE_HAVE_BC7ENC
#  include <bc7enc/bc7enc.h>
#  include <mutex>
#endif

#include <algorithm>
#include <memory>
#ifdef __APPLE__
#  include <malloc/malloc.h>
#endif

namespace HE
{

size_t blockEncodedBytes(uint32_t w, uint32_t h)
{
    return static_cast<size_t>((w + 3) / 4) * ((h + 3) / 4) * 16;
}

#ifdef HE_HAVE_ASTCENC
namespace
{
    struct AstcCtx
    {
        astcenc_context* ctx = nullptr;
        ~AstcCtx() { if (ctx) astcenc_context_free(ctx); }
    };

    AstcCtx (&astcCacheTable())[3][2]
    {
        thread_local AstcCtx cache[3][2];
        return cache;
    }

    // One context per thread and (preset, profile): allocating one costs more than encoding a 136 x 136 page.
    astcenc_context* astcContextFor(uint8_t quality, bool srgb)
    {
        AstcCtx& c = astcCacheTable()[quality > 2 ? 2 : quality][srgb ? 1 : 0];
        if (!c.ctx)
        {
            const float preset = quality >= 2 ? ASTCENC_PRE_THOROUGH : quality == 1 ? ASTCENC_PRE_MEDIUM : ASTCENC_PRE_FAST;
            astcenc_config cfg{};
            if (astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, 4, 4, 1, preset, ASTCENC_FLG_SELF_DECOMPRESS_ONLY, &cfg) != ASTCENC_SUCCESS) return nullptr;
            if (astcenc_context_alloc(&cfg, 1, &c.ctx) != ASTCENC_SUCCESS) { c.ctx = nullptr; return nullptr; }
        }
        return c.ctx;
    }
}
#endif

#ifdef HE_HAVE_BC7ENC
namespace
{
    // Texel block gather, same shape as HpakWriter.cpp's cook-time encoder: clamp to the last valid texel past the image
    // edge so a page whose side isn't a multiple of 4 (it always is here, 136 px) never reads out of bounds.
    void gatherRGBA8BlockRt(const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t bx, uint32_t by, uint8_t out[64])
    {
        for (uint32_t ty = 0; ty < 4; ++ty)
            for (uint32_t tx = 0; tx < 4; ++tx)
            {
                const uint32_t sx = std::min(bx * 4 + tx, w - 1), sy = std::min(by * 4 + ty, h - 1);
                const uint8_t* p = rgba + (static_cast<size_t>(sy) * w + sx) * 4;
                uint8_t* d = out + (static_cast<size_t>(ty) * 4 + tx) * 4;
                d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = p[3];
            }
    }

    bool encodeBc7Rt(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t quality, std::vector<uint8_t>& out)
    {
        // bc7enc_compress_block_init() fills global tables once; the cold-page encoder thread and any other caller share
        // this guard (bc7enc_compress_block itself is stateless once the tables exist).
        static std::once_flag initFlag;
        std::call_once(initFlag, [] { bc7enc_compress_block_init(); });

        bc7enc_compress_block_params params;
        bc7enc_compress_block_params_init(&params);
        params.m_max_partitions = quality >= 1 ? BC7ENC_MAX_PARTITIONS : 16;
        params.m_uber_level     = quality >= 2 ? 1 : 0;

        const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
        out.resize(static_cast<size_t>(bw) * bh * 16);
        uint8_t block[64];
        for (uint32_t by = 0; by < bh; ++by)
            for (uint32_t bx = 0; bx < bw; ++bx)
            {
                gatherRGBA8BlockRt(rgba, w, h, bx, by, block);
                bc7enc_compress_block(out.data() + (static_cast<size_t>(by) * bw + bx) * 16, block, &params);
            }
        return true;
    }
}
#endif

bool encodeBlocks(BlockFormat format, const uint8_t* rgba, uint32_t w, uint32_t h, bool srgb, uint8_t quality,
                  std::vector<uint8_t>& out)
{
    out.clear();
    if (!rgba || w == 0 || h == 0) return false;
#ifdef HE_HAVE_ASTCENC
    if (format == BlockFormat::Astc4x4)
    {
        astcenc_context* ctx = astcContextFor(quality, srgb);
        if (!ctx) return false;
        astcenc_image img{};
        img.dim_x = w; img.dim_y = h; img.dim_z = 1;
        img.data_type = ASTCENC_TYPE_U8;
        void* slices[1] = { const_cast<uint8_t*>(rgba) };
        img.data = slices;
        const astcenc_swizzle swz{ ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };
        out.resize(blockEncodedBytes(w, h));
        astcenc_compress_reset(ctx);
        if (astcenc_compress_image(ctx, &img, &swz, out.data(), out.size(), 0) != ASTCENC_SUCCESS) { out.clear(); return false; }
        return true;
    }
#endif
#ifdef HE_HAVE_BC7ENC
    if (format == BlockFormat::Bc7)
    {
        // BC7 has no separate sRGB encode profile (the GPU's *_SRGB format decodes the stored bytes as sRGB, the encoder
        // just matches RGBA8 as closely as it can either way): `srgb` is accepted for symmetry with the ASTC call and
        // does not change what is written here.
        (void)srgb;
        return encodeBc7Rt(rgba, w, h, quality, out);
    }
#endif
    (void)format; (void)srgb; (void)quality;
    return false;
}

void releaseBlockEncoderCache()
{
#ifdef HE_HAVE_ASTCENC
    for (auto& row : astcCacheTable())
        for (AstcCtx& c : row)
            if (c.ctx) { astcenc_context_free(c.ctx); c.ctx = nullptr; }
#endif
#ifdef __APPLE__
    malloc_zone_pressure_relief(nullptr, 0);
#endif
}

} // namespace HE
