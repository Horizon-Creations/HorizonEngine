#include "doctest.h"
#include <ContentManager/BlockEncode.h>

#include <vector>

// The single-image block encoder the Runtime Virtual Texture's cold pages use (Thema 333 step 6j Metal / step 9 Windows).
// BC7 decode happens only on the GPU (TextureBlockDecode.h deliberately has no CPU BC7 decoder), so this file checks shape
// and determinism; the lit-diff witness on real D3D11/D3D12/Vulkan hardware is what proves round-trip colour fidelity.

TEST_CASE("blockEncodedBytes counts 16 bytes per 4x4 block, partial blocks rounded up")
{
    CHECK(HE::blockEncodedBytes(136, 136) == 34u * 34u * 16u);
    CHECK(HE::blockEncodedBytes(4, 4) == 16u);
    CHECK(HE::blockEncodedBytes(5, 1) == 2u * 16u);
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
    // The same picture again gives the same bytes: a reused global table (bc7enc_compress_block_init) must not vary per call.
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

TEST_CASE("encodeBlocks refuses what it cannot encode")
{
    std::vector<uint8_t> px(16 * 4, 0), out;
    CHECK_FALSE(HE::encodeBlocks(HE::BlockFormat::Bc7, nullptr, 4, 4, false, 0, out));
    CHECK_FALSE(HE::encodeBlocks(HE::BlockFormat::Bc7, px.data(), 0, 4, false, 0, out));
}
