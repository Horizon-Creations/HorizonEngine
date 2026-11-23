#include "doctest.h"
#include <ContentManager/BlockEncode.h>
#include <ContentManager/TextureBlockDecode.h>

#include <cstdlib>

#include <algorithm>
#include <cstring>
#include <vector>

// The single-image block encoder the Runtime Virtual Texture's cold pages use (Thema 333 step 6j Metal / step 9 Windows).
// The BC7 round trip goes through the CPU decoder the software renderer uses (TextureBlockDecode.h); the lit-diff witness
// on real D3D11/D3D12/Vulkan hardware is what proves the GPU reads the same blocks.

TEST_CASE("blockEncodedBytes counts 16 bytes per 4x4 block, partial blocks rounded up")
{
    CHECK(HE::blockEncodedBytes(136, 136) == 34u * 34u * 16u);
    CHECK(HE::blockEncodedBytes(4, 4) == 16u);
    CHECK(HE::blockEncodedBytes(5, 1) == 2u * 16u);
}

TEST_CASE("encodeBlocks: ASTC 4x4 of a picture has the size of its blocks and differs from a flat picture's")
{
    std::vector<uint8_t> flat(32 * 32 * 4, 128), ramp(32 * 32 * 4);
    for (uint32_t y = 0; y < 32; ++y)
        for (uint32_t x = 0; x < 32; ++x)
        {
            uint8_t* p = &ramp[(y * 32 + x) * 4];
            p[0] = uint8_t(x * 8); p[1] = uint8_t(y * 8); p[2] = 40; p[3] = 255;
        }
    std::vector<uint8_t> a, b, c;
    const bool ok = HE::encodeBlocks(HE::BlockFormat::Astc4x4, flat.data(), 32, 32, false, 0, a);
    if (!ok)
    {
        MESSAGE("this build has no ASTC encoder: nothing to check");
        return;
    }
    CHECK(a.size() == HE::blockEncodedBytes(32, 32));
    REQUIRE(HE::encodeBlocks(HE::BlockFormat::Astc4x4, ramp.data(), 32, 32, true, 0, b));
    CHECK(b.size() == a.size());
    CHECK(a != b);
    // The same picture again gives the same bytes (a reused context must not leak state from the last image).
    REQUIRE(HE::encodeBlocks(HE::BlockFormat::Astc4x4, ramp.data(), 32, 32, true, 0, c));
    CHECK(c == b);
    HE::releaseBlockEncoderCache();
    REQUIRE(HE::encodeBlocks(HE::BlockFormat::Astc4x4, ramp.data(), 32, 32, true, 0, c));
    CHECK(c == b);
}

TEST_CASE("encodeBlocks refuses what it cannot encode")
{
    std::vector<uint8_t> px(16 * 4, 0), out;
    CHECK_FALSE(HE::encodeBlocks(HE::BlockFormat::Astc4x4, nullptr, 4, 4, false, 0, out));
    CHECK_FALSE(HE::encodeBlocks(HE::BlockFormat::Astc4x4, px.data(), 0, 4, false, 0, out));
    CHECK_FALSE(HE::encodeBlocks(HE::BlockFormat::Bc7, nullptr, 4, 4, false, 0, out));
    CHECK_FALSE(HE::encodeBlocks(HE::BlockFormat::Bc7, px.data(), 0, 4, false, 0, out));
}

TEST_CASE("encodeBlocks: BC7 of a page has the exact block size and differs from a flat picture's, same input gives same bytes")
{
    std::vector<uint8_t> flat(136 * 136 * 4, 128), ramp(136 * 136 * 4);
    for (uint32_t y = 0; y < 136; ++y)
        for (uint32_t x = 0; x < 136; ++x)
        {
            uint8_t* p = &ramp[(size_t(y) * 136 + x) * 4];
            p[0] = uint8_t(x * 2); p[1] = uint8_t(y * 2); p[2] = 40; p[3] = 255;
        }
    std::vector<uint8_t> a, b, c;
    const bool ok = HE::encodeBlocks(HE::BlockFormat::Bc7, flat.data(), 136, 136, true, 0, a);
    if (!ok)
    {
        MESSAGE("this build has no BC7 encoder (HE_HAVE_BC7ENC off): nothing to check");
        return;
    }
    CHECK(a.size() == HE::blockEncodedBytes(136, 136));
    CHECK(a.size() == 34u * 34u * 16u); // 18.5 KB per layer, the step's own number
    REQUIRE(HE::encodeBlocks(HE::BlockFormat::Bc7, ramp.data(), 136, 136, false, 0, b));
    CHECK(b.size() == a.size());
    CHECK(a != b);
    // The same picture again gives the same bytes: the reused global table (bc7enc_compress_block_init) must not vary per call.
    REQUIRE(HE::encodeBlocks(HE::BlockFormat::Bc7, ramp.data(), 136, 136, false, 0, c));
    CHECK(c == b);
    HE::releaseBlockEncoderCache();
    REQUIRE(HE::encodeBlocks(HE::BlockFormat::Bc7, ramp.data(), 136, 136, false, 0, c));
    CHECK(c == b);
}

TEST_CASE("encodeBlocks: BC7 handles a side that is not a multiple of 4 (clamped edge blocks), no out-of-bounds read")
{
    std::vector<uint8_t> px(5 * 3 * 4, 200), out;
    if (!HE::encodeBlocks(HE::BlockFormat::Bc7, px.data(), 5, 3, true, 0, out))
    {
        MESSAGE("this build has no BC7 encoder: nothing to check");
        return;
    }
    CHECK(out.size() == HE::blockEncodedBytes(5, 3));
    CHECK(out.size() == 2u * 1u * 16u);
}

TEST_CASE("BC7: what bc7enc writes, the CPU decoder reads back close to the source (opaque and with alpha)")
{
    // Opaque blocks take modes 1/6, blocks with alpha 5/6/7: both halves of what an export cooks.
    for (const bool alpha : { false, true })
    {
        const uint32_t W = 64, H = 64;
        std::vector<uint8_t> src(W * H * 4);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
            {
                uint8_t* p = &src[(y * W + x) * 4];
                p[0] = uint8_t(x * 4); p[1] = uint8_t(y * 4); p[2] = uint8_t((x ^ y) & 0x20 ? 200 : 30);
                p[3] = alpha ? uint8_t(255 - x * 3) : 255;
            }
        std::vector<uint8_t> blocks;
        if (!HE::encodeBlocks(HE::BlockFormat::Bc7, src.data(), W, H, false, 1, blocks))
        {
            MESSAGE("this build has no BC7 encoder: nothing to check");
            return;
        }
        std::vector<uint8_t> back(src.size());
        REQUIRE(HE::decodeBlockLevelToRGBA8(::TextureFormat::BC7, blocks.data(), blocks.size(), W, H, back.data()));
        long sum = 0; int worst = 0;
        for (size_t i = 0; i < src.size(); ++i)
        {
            const int d = std::abs(int(src[i]) - int(back[i]));
            sum += d; worst = std::max(worst, d);
        }
        INFO("alpha " << alpha << " mean " << double(sum) / double(src.size()) << " worst " << worst);
        CHECK(double(sum) / double(src.size()) < 3.0);
        CHECK(worst <= 40);
        if (!alpha) for (size_t i = 3; i < back.size(); i += 4) REQUIRE(back[i] == 255);
    }
}

TEST_CASE("BC7: a reserved mode byte decodes to transparent black, never garbage")
{
    const uint8_t blk[16] = {};
    uint8_t out[4 * 4 * 4];
    std::memset(out, 0x77, sizeof out);
    REQUIRE(HE::decodeBlockLevelToRGBA8(::TextureFormat::BC7, blk, sizeof blk, 4, 4, out));
    for (uint8_t b : out) CHECK(b == 0);
}
