#include "doctest.h"
#include <ContentManager/BlockEncode.h>

#include <vector>

// The single-image block encoder the Runtime Virtual Texture's cold pages use (Thema 333 step 6j).

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
    CHECK_FALSE(HE::encodeBlocks(HE::BlockFormat::Bc7, px.data(), 4, 4, false, 0, out));   // the Windows step: not wired here yet
}
