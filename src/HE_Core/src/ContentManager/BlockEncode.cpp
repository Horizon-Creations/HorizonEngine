#include <ContentManager/BlockEncode.h>

#ifdef HE_HAVE_ASTCENC
#  include <astcenc.h>
#endif

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

    AstcCtx (&cacheTable())[3][2]
    {
        thread_local AstcCtx cache[3][2];
        return cache;
    }

    // One context per thread and (preset, profile): allocating one costs more than encoding a 136 x 136 page.
    astcenc_context* contextFor(uint8_t quality, bool srgb)
    {
        AstcCtx& c = cacheTable()[quality > 2 ? 2 : quality][srgb ? 1 : 0];
        if (!c.ctx)
        {
            const float preset = quality >= 2 ? ASTCENC_PRE_THOROUGH : quality == 1 ? ASTCENC_PRE_MEDIUM : ASTCENC_PRE_FAST;
            astcenc_config cfg{};
            // SELF_DECOMPRESS_ONLY: this context only ever compresses (nothing here decodes blocks), so its tables can leave out the entries
            // that only decoding other compressors' images would need: a much smaller context, which matters for a long-lived worker.
            if (astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, 4, 4, 1, preset, ASTCENC_FLG_SELF_DECOMPRESS_ONLY, &cfg) != ASTCENC_SUCCESS) return nullptr;
            if (astcenc_context_alloc(&cfg, 1, &c.ctx) != ASTCENC_SUCCESS) { c.ctx = nullptr; return nullptr; }
        }
        return c.ctx;
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
        astcenc_context* ctx = contextFor(quality, srgb);
        if (!ctx) return false;
        astcenc_image img{};
        img.dim_x = w; img.dim_y = h; img.dim_z = 1;
        img.data_type = ASTCENC_TYPE_U8;
        void* slices[1] = { const_cast<uint8_t*>(rgba) };
        img.data = slices;
        const astcenc_swizzle swz{ ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };
        out.resize(blockEncodedBytes(w, h));
        // A reused context must be reset between images.
        astcenc_compress_reset(ctx);
        if (astcenc_compress_image(ctx, &img, &swz, out.data(), out.size(), 0) != ASTCENC_SUCCESS) { out.clear(); return false; }
        return true;
    }
#endif
    (void)format; (void)srgb; (void)quality;
    return false;   // BC7: the Windows step of Thema 333 (bc7enc is in the tree, see HpakWriter.cpp)
}

void releaseBlockEncoderCache()
{
#ifdef HE_HAVE_ASTCENC
    for (auto& row : cacheTable())
        for (AstcCtx& c : row)
            if (c.ctx) { astcenc_context_free(c.ctx); c.ctx = nullptr; }
#endif
#ifdef __APPLE__
    malloc_zone_pressure_relief(nullptr, 0);   // the freed tables go back to the system, not to the allocator's free list (RSS)
#endif
}

} // namespace HE
