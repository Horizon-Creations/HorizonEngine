#include "doctest.h"
#include "TestFsUtil.h"
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/TerrainHeightmap.h>
#include <HorizonScene/TerrainMeshGenerator.h>
#include <ContentManager/Assets.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ── Greyscale heightmap → sculptHeights ───────────────────────────────────────
// The pixel path is the whole algorithm; the texture and file paths are two ways
// of arriving at it, so most cases go through importPixels with a hand-built
// image and the last two check that the wrappers deliver the same bytes.

namespace
{
    // A res×res terrain of `size` metres, no noise, `scale` metres of relief.
    TerrainComponent terrain(uint32_t res, float scale = 100.0f)
    {
        TerrainComponent tc;
        tc.sizeX = tc.sizeZ = 64.0f;
        tc.resolution  = res;
        tc.heightScale = scale;
        tc.seed        = 0;
        return tc;
    }

    TerrainHeightmap::Source grey8(const std::vector<uint8_t>& px, uint32_t w, uint32_t h)
    {
        TerrainHeightmap::Source s;
        s.pixels = px.data(); s.width = w; s.height = h; s.channels = 1; s.bytesPerChannel = 1;
        return s;
    }

    float at(const TerrainComponent& tc, uint32_t xi, uint32_t zi)
    {
        return tc.sculptHeights[static_cast<size_t>(zi) * tc.resolution + xi];
    }
}

TEST_CASE("heightmap: an 8-bit gradient the size of the grid lands vertex-for-pixel")
{
    // 5×5 image, value = 51·x → 0, 0.2, 0.4, 0.6, 0.8 of the range across X.
    std::vector<uint8_t> px(25);
    for (uint32_t y = 0; y < 5; ++y)
        for (uint32_t x = 0; x < 5; ++x) px[y * 5 + x] = static_cast<uint8_t>(51 * x);

    TerrainComponent tc = terrain(5);
    TerrainHeightmap::Options opts;
    opts.baseHeight = 10.0f;
    const auto r = TerrainHeightmap::importPixels(tc, grey8(px, 5, 5), opts);
    REQUIRE(r.ok);
    CHECK(r.error.empty());
    CHECK(r.sourceWidth == 5);
    CHECK(r.sourceBits  == 8);
    REQUIRE(tc.resolution == 5);   // 2²+1 already, no snap
    REQUIRE(tc.sculptHeights.size() == 25);
    for (uint32_t z = 0; z < 5; ++z)
        for (uint32_t x = 0; x < 5; ++x)
            CHECK(at(tc, x, z) == doctest::Approx(10.0f + 100.0f * (51.0f * x / 255.0f)));
    CHECK(r.minHeight == doctest::Approx(10.0f));
    CHECK(r.maxHeight == doctest::Approx(10.0f + 80.0f));
    CHECK(tc.dirty);
    CHECK_FALSE(tc.regionDirty);
    // range = 0 means "the terrain's own heightScale", which is what the
    // Inspector's Height Scale then means for an imported landscape.
    CHECK(opts.range == 0.0f);
}

TEST_CASE("heightmap: 16-bit values keep the precision 8 bits cannot")
{
    std::vector<uint16_t> px(25, 0);
    px[12] = 32768;   // centre pixel, just over half — 8-bit has no such step
    TerrainHeightmap::Source s;
    s.pixels = reinterpret_cast<const uint8_t*>(px.data());
    s.width = s.height = 5; s.channels = 1; s.bytesPerChannel = 2;

    TerrainComponent tc = terrain(5);
    const auto r = TerrainHeightmap::importPixels(tc, s, {});
    REQUIRE(r.ok);
    CHECK(r.sourceBits == 16);
    const float h = at(tc, 2, 2);
    CHECK(h == doctest::Approx(100.0f * 32768.0f / 65535.0f).epsilon(1e-6));
    // Neither 8-bit neighbour: 127/255 nor 128/255 of the range.
    CHECK(h != doctest::Approx(100.0f * 127.0f / 255.0f).epsilon(1e-6));
    CHECK(h != doctest::Approx(100.0f * 128.0f / 255.0f).epsilon(1e-6));
    CHECK(at(tc, 0, 0) == doctest::Approx(0.0f));
}

TEST_CASE("heightmap: the image's top row is the terrain's -Z edge; Flip Z mirrors it")
{
    // 3 wide, 2 tall: black on top, white below. Onto a 3×3 grid the middle
    // row is halfway between (bilinear up-sampling along Z).
    const std::vector<uint8_t> px = { 0, 0, 0,  255, 255, 255 };

    TerrainComponent tc = terrain(3);
    REQUIRE(TerrainHeightmap::importPixels(tc, grey8(px, 3, 2), {}).ok);
    REQUIRE(tc.resolution == 3);
    CHECK(at(tc, 1, 0) == doctest::Approx(0.0f));     // zi = 0 is -Z (TerrainMeshGenerator)
    CHECK(at(tc, 1, 1) == doctest::Approx(50.0f));
    CHECK(at(tc, 1, 2) == doctest::Approx(100.0f));

    TerrainHeightmap::Options flip;
    flip.flipZ = true;
    TerrainComponent tf = terrain(3);
    REQUIRE(TerrainHeightmap::importPixels(tf, grey8(px, 3, 2), flip).ok);
    CHECK(at(tf, 1, 0) == doctest::Approx(100.0f));
    CHECK(at(tf, 1, 2) == doctest::Approx(0.0f));
}

TEST_CASE("heightmap: going down, each vertex averages the pixels under it instead of picking one")
{
    // An 8×8 checkerboard onto a 2×2 grid: point-sampling would give whatever
    // corner colour the sample lands on; a box average gives mid-grey.
    std::vector<uint8_t> px(64);
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) px[y * 8 + x] = ((x + y) & 1) ? 255 : 0;

    TerrainComponent tc = terrain(2);
    const auto r = TerrainHeightmap::importPixels(tc, grey8(px, 8, 8), {});
    REQUIRE(r.ok);
    REQUIRE(tc.resolution == 2);
    for (float h : tc.sculptHeights) CHECK(h == doctest::Approx(50.0f));

    // And a linear gradient survives the averaging in the interior: 256 wide
    // onto 5 vertices, the middle vertex sits at 127.5 of 255.
    std::vector<uint8_t> grad(256 * 2);
    for (uint32_t y = 0; y < 2; ++y)
        for (uint32_t x = 0; x < 256; ++x) grad[y * 256 + x] = static_cast<uint8_t>(x);
    TerrainComponent tg = terrain(5);
    REQUIRE(TerrainHeightmap::importPixels(tg, grey8(grad, 256, 2), {}).ok);
    CHECK(at(tg, 2, 0) == doctest::Approx(50.0f).epsilon(1e-3));
    CHECK(at(tg, 0, 0) < at(tg, 1, 0));
    CHECK(at(tg, 3, 0) < at(tg, 4, 0));
}

TEST_CASE("heightmap: the resolution is snapped to 2ⁿ+1 like the brushes do, or adopted from the image")
{
    std::vector<uint8_t> px(16 * 16, 128);
    TerrainComponent tc = terrain(16);
    REQUIRE(TerrainHeightmap::importPixels(tc, grey8(px, 16, 16), {}).ok);
    CHECK(tc.resolution == 17);
    CHECK(tc.sculptHeights.size() == 17u * 17u);
    for (float h : tc.sculptHeights) CHECK(h == doctest::Approx(100.0f * 128.0f / 255.0f));

    TerrainHeightmap::Options adopt;
    adopt.adoptResolution = true;
    TerrainComponent ta = terrain(129);
    std::vector<uint8_t> big(64 * 32, 200);
    REQUIRE(TerrainHeightmap::importPixels(ta, grey8(big, 64, 32), adopt).ok);
    CHECK(ta.resolution == 65);   // the larger side, snapped

    // Capped where the Inspector's slider stops, so a 4k map does not hand
    // computeTerrainHeightField a resolution it clamps away.
    TerrainComponent tb = terrain(129);
    std::vector<uint8_t> huge(1024 * 4, 7);
    REQUIRE(TerrainHeightmap::importPixels(tb, grey8(huge, 1024, 4), adopt).ok);
    CHECK(tb.resolution == 513);
    CHECK(computeTerrainHeightField(tb).size() == 513u * 513u);
}

TEST_CASE("heightmap: colour pixels are read as luma, grey+alpha as the grey")
{
    // 2×2 RGBA: red, grey, white, black.
    const std::vector<uint8_t> px = {
        255, 0, 0, 255,      200, 200, 200, 255,
        255, 255, 255, 255,  0, 0, 0, 255,
    };
    TerrainHeightmap::Source s;
    s.pixels = px.data(); s.width = s.height = 2; s.channels = 4; s.bytesPerChannel = 1;
    TerrainComponent tc = terrain(2);
    REQUIRE(TerrainHeightmap::importPixels(tc, s, {}).ok);
    CHECK(at(tc, 0, 0) == doctest::Approx(100.0f * 0.299f));
    CHECK(at(tc, 1, 0) == doctest::Approx(100.0f * 200.0f / 255.0f));
    CHECK(at(tc, 0, 1) == doctest::Approx(100.0f));
    CHECK(at(tc, 1, 1) == doctest::Approx(0.0f));

    const std::vector<uint8_t> ga = { 51, 0,  102, 0,  153, 0,  204, 0 };
    TerrainHeightmap::Source s2;
    s2.pixels = ga.data(); s2.width = s2.height = 2; s2.channels = 2; s2.bytesPerChannel = 1;
    TerrainComponent t2 = terrain(2);
    REQUIRE(TerrainHeightmap::importPixels(t2, s2, {}).ok);
    CHECK(at(t2, 0, 0) == doctest::Approx(20.0f));
    CHECK(at(t2, 1, 1) == doctest::Approx(80.0f));
}

TEST_CASE("heightmap: bad input is refused and leaves the terrain alone")
{
    TerrainComponent tc = terrain(5);
    tc.sculptHeights.assign(25, 3.0f);
    const std::vector<uint8_t> px(25, 9);

    TerrainHeightmap::Source none;
    CHECK_FALSE(TerrainHeightmap::importPixels(tc, none, {}).ok);
    TerrainHeightmap::Source fiveCh = grey8(px, 5, 5); fiveCh.channels = 5;
    CHECK_FALSE(TerrainHeightmap::importPixels(tc, fiveCh, {}).ok);
    TerrainHeightmap::Source fourBytes = grey8(px, 5, 5); fourBytes.bytesPerChannel = 4;
    const auto r = TerrainHeightmap::importPixels(tc, fourBytes, {});
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.error.empty());
    TerrainComponent flat = terrain(5); flat.sizeX = 0.0f;
    CHECK_FALSE(TerrainHeightmap::importPixels(flat, grey8(px, 5, 5), {}).ok);

    for (float h : tc.sculptHeights) CHECK(h == 3.0f);
}

TEST_CASE("heightmap: a texture asset feeds level 0; a block-compressed one is refused")
{
    TextureAsset tex;
    tex.width = tex.height = 2;
    tex.channels = 4;
    tex.data = { 0, 0, 0, 255,  255, 255, 255, 255,
                 128, 128, 128, 255,  64, 64, 64, 255,
                 // a 1×1 mip appended — must be ignored
                 7, 7, 7, 255 };
    tex.mipLevels = 2;

    TerrainComponent tc = terrain(2);
    const auto r = TerrainHeightmap::importTexture(tc, tex, {});
    REQUIRE(r.ok);
    CHECK(at(tc, 0, 0) == doctest::Approx(0.0f));
    CHECK(at(tc, 1, 0) == doctest::Approx(100.0f));
    CHECK(at(tc, 0, 1) == doctest::Approx(100.0f * 128.0f / 255.0f));
    CHECK(at(tc, 1, 1) == doctest::Approx(100.0f * 64.0f / 255.0f));

    TextureAsset bc = tex;
    bc.format = TextureFormat::BC7;
    TerrainComponent t2 = terrain(2);
    const auto rb = TerrainHeightmap::importTexture(t2, bc, {});
    CHECK_FALSE(rb.ok);
    CHECK(rb.error.find("BC7") != std::string::npos);
    CHECK(t2.sculptHeights.empty());

    TextureAsset empty;
    CHECK_FALSE(TerrainHeightmap::importTexture(t2, empty, {}).ok);
}

TEST_CASE("heightmap: files — 8-bit PGM, 16-bit PGM (big-endian per spec), .r16, and a missing one")
{
    const auto dir = std::filesystem::temp_directory_path();

    // 8-bit binary PGM, 4×4, value = 17·(x + y) → corners 0 and 102.
    const auto pgm8 = dir / "he_test_heightmap8.pgm";
    {
        std::ofstream f(pgm8, std::ios::binary);
        f << "P5\n4 4\n255\n";
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) f.put(static_cast<char>(17 * (x + y)));
    }
    TerrainComponent tc = terrain(4);   // snaps to 5
    const auto r8 = TerrainHeightmap::importFile(tc, pgm8.string(), {});
    REQUIRE(r8.ok);
    CHECK(r8.sourceBits == 8);
    CHECK(r8.sourceWidth == 4);
    CHECK(tc.resolution == 5);
    CHECK(at(tc, 0, 0) == doctest::Approx(0.0f));
    CHECK(at(tc, 4, 4) == doctest::Approx(100.0f * 102.0f / 255.0f));
    he_test::removeQuiet(pgm8);

    // 16-bit binary PGM, 2×2, big-endian as the format says: 0x1000 = 4096.
    const auto pgm16 = dir / "he_test_heightmap16.pgm";
    {
        std::ofstream f(pgm16, std::ios::binary);
        f << "P5\n2 2\n65535\n";
        const uint8_t bytes[] = { 0x10, 0x00,  0xFF, 0xFF,  0x00, 0x10,  0x80, 0x00 };
        f.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    }
    TerrainComponent t16 = terrain(2);
    const auto r16 = TerrainHeightmap::importFile(t16, pgm16.string(), {});
    REQUIRE(r16.ok);
    CHECK(r16.sourceBits == 16);
    CHECK(at(t16, 0, 0) == doctest::Approx(100.0f * 4096.0f / 65535.0f).epsilon(1e-6));
    CHECK(at(t16, 1, 0) == doctest::Approx(100.0f));
    CHECK(at(t16, 0, 1) == doctest::Approx(100.0f * 16.0f / 65535.0f).epsilon(1e-6));
    CHECK(at(t16, 1, 1) == doctest::Approx(100.0f * 32768.0f / 65535.0f).epsilon(1e-6));
    he_test::removeQuiet(pgm16);

    // .r16: headerless little-endian shorts, square. 3×3 = 9 values.
    const auto raw = dir / "he_test_heightmap.r16";
    {
        std::ofstream f(raw, std::ios::binary);
        const uint16_t v[9] = { 0, 65535, 0,  65535, 32768, 65535,  0, 65535, 0 };
        for (uint16_t x : v)
        {
            const uint8_t le[2] = { static_cast<uint8_t>(x & 0xFF), static_cast<uint8_t>(x >> 8) };
            f.write(reinterpret_cast<const char*>(le), 2);
        }
    }
    TerrainComponent tr = terrain(3);
    const auto rr = TerrainHeightmap::importFile(tr, raw.string(), {});
    REQUIRE(rr.ok);
    CHECK(rr.sourceWidth == 3);
    CHECK(rr.sourceHeight == 3);
    CHECK(rr.sourceBits == 16);
    CHECK(at(tr, 1, 1) == doctest::Approx(100.0f * 32768.0f / 65535.0f).epsilon(1e-6));
    CHECK(at(tr, 0, 1) == doctest::Approx(100.0f));
    CHECK(at(tr, 0, 0) == doctest::Approx(0.0f));
    he_test::removeQuiet(raw);

    // Not square → refused, with the count in the message.
    const auto odd = dir / "he_test_heightmap_odd.r16";
    {
        std::ofstream f(odd, std::ios::binary);
        const char zeros[10] = {};
        f.write(zeros, sizeof(zeros));
    }
    const auto ro = TerrainHeightmap::importFile(tr, odd.string(), {});
    CHECK_FALSE(ro.ok);
    CHECK(ro.error.find("square") != std::string::npos);
    he_test::removeQuiet(odd);

    // Missing file: an error in words, not a crash.
    const auto rm = TerrainHeightmap::importFile(tr, (dir / "he_test_no_such_heightmap.png").string(), {});
    CHECK_FALSE(rm.ok);
    CHECK_FALSE(rm.error.empty());
}
