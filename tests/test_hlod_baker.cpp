// The pure part of the far-landscape bake (HorizonScene/LandscapeHlodBaker.h). The pictures themselves come from
// the renderer; their oracles are the HE_DUMP_RAWTEST and HE_DUMP_HLODMOSAIC witnesses of the editor (docs/
// landscape-hlod-baked-material-2026-11-02.md).
#include "doctest.h"

#include <HorizonScene/LandscapeHlodBaker.h>

#include <cstdint>
#include <vector>

TEST_CASE("boxDownsampleRgba8 averages each ss x ss block per channel, rounding to nearest")
{
    // 4 x 4 source, ss 2 -> 2 x 2. Block (0,0) holds 0, 10, 20, 30 on every channel (mean 15), block (1,0) is
    // constant 200, block (0,1) is 1, 1, 1, 2 (mean 1.25 -> 1), block (1,1) is 254, 254, 255, 255 (mean 254.5 -> 255).
    std::vector<uint8_t> src(4 * 4 * 4, 0);
    auto put = [&](int x, int y, uint8_t v, uint8_t a = 255) {
        uint8_t* p = &src[(static_cast<size_t>(y) * 4 + static_cast<size_t>(x)) * 4];
        p[0] = v; p[1] = v; p[2] = v; p[3] = a;
    };
    put(0, 0, 0);   put(1, 0, 10);  put(0, 1, 20);  put(1, 1, 30);
    put(2, 0, 200); put(3, 0, 200); put(2, 1, 200); put(3, 1, 200);
    put(0, 2, 1);   put(1, 2, 1);   put(0, 3, 1);   put(1, 3, 2);
    put(2, 2, 254); put(3, 2, 254); put(2, 3, 255); put(3, 3, 255);

    const std::vector<uint8_t> out = HE::landscape::boxDownsampleRgba8(src, 2, 2);
    REQUIRE(out.size() == 2u * 2u * 4u);
    CHECK(out[0] == 15);  CHECK(out[1] == 15);  CHECK(out[2] == 15);  CHECK(out[3] == 255);
    CHECK(out[4] == 200);
    CHECK(out[8] == 1);
    CHECK(out[12] == 255);
}

TEST_CASE("boxDownsampleRgba8 with ss 1 is the identity")
{
    std::vector<uint8_t> src = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    CHECK(HE::landscape::boxDownsampleRgba8(src, 2, 1) == src);
}
