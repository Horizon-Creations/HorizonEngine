#include "doctest.h"
#include <Application/AppIcon.h>
#include <Renderer/UIFont.h>
#include <stb_image.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

// The application's icon is GENERATED — one built-in icon on a plate becomes the
// three containers three systems insist on. Which means three things have to be
// true and none of them is obvious from reading the code: the PNG is a PNG, the
// containers say what they contain, and the picture is actually a picture.

namespace
{
    std::filesystem::path tmpDir()
    {
        const auto d = std::filesystem::temp_directory_path() / "he_app_icon";
        std::filesystem::create_directories(d);
        return d;
    }

    std::vector<std::uint8_t> readAll(const std::filesystem::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                          std::istreambuf_iterator<char>());
    }
}

TEST_CASE("The written PNG is one a decoder reads back, pixel for pixel")
{
    // The one place a hand-written container could be wrong in a way no compiler
    // catches. stb_image is a real decoder and it is already in the build, so the
    // check is a round trip and not an eyeball.
    const int px = 64;
    const glm::vec4 bg(0.12f, 0.44f, 0.78f, 1.0f);
    const std::vector<std::uint8_t> rgba =
        HE::heRenderAppIcon("home", px, bg, HE::heAppIconForeground(bg));
    REQUIRE(rgba.size() == (std::size_t)px * px * 4);

    const auto path = tmpDir() / "roundtrip.png";
    REQUIRE(HE::hePngWrite(path, rgba.data(), px, px));

    int w = 0, h = 0, ch = 0;
    unsigned char* back = stbi_load(path.string().c_str(), &w, &h, &ch, 4);
    REQUIRE(back != nullptr);
    CHECK(w == px);
    CHECK(h == px);
    bool identical = true;
    for (std::size_t i = 0; i < rgba.size(); ++i)
        if (back[i] != rgba[i]) { identical = false; break; }
    CHECK(identical);
    stbi_image_free(back);
    std::filesystem::remove(path);
}

TEST_CASE("The icon is a picture: a plate, an icon on it, and corners that are not")
{
    const int px = 64;
    const glm::vec4 bg(0.12f, 0.44f, 0.78f, 1.0f);   // dark plate → white icon
    const glm::vec4 fg = HE::heAppIconForeground(bg);
    CHECK(fg.r > 0.9f);                              // white on dark
    CHECK(HE::heAppIconForeground(glm::vec4(0.95f, 0.9f, 0.2f, 1.0f)).r < 0.2f);  // dark on light

    const std::vector<std::uint8_t> rgba = HE::heRenderAppIcon("home", px, bg, fg);
    REQUIRE(rgba.size() == (std::size_t)px * px * 4);
    const auto at = [&](int x, int y) { return &rgba[((std::size_t)y * px + x) * 4]; };

    // The corner is outside the rounded square, so it is transparent — an icon
    // that filled its square would look like a screenshot, not like an icon.
    CHECK(at(0, 0)[3] == 0);
    CHECK(at(px - 1, px - 1)[3] == 0);
    // The middle of an edge IS on the plate, and it is the plate's colour.
    const std::uint8_t* edge = at(px / 2, 4);
    CHECK(edge[3] > 200);
    // …and somewhere in the middle there is ink that is neither plate nor empty.
    int inkPixels = 0;
    for (int y = px / 4; y < px * 3 / 4; ++y)
        for (int x = px / 4; x < px * 3 / 4; ++x)
            if (at(x, y)[0] > 200 && at(x, y)[1] > 200 && at(x, y)[2] > 200) ++inkPixels;
    CHECK(inkPixels > 50);
}

TEST_CASE("A name the icon face does not have is no icon at all")
{
    // Better than a blank plate: an application whose icon is a coloured square
    // looks like a bug that shipped, and the settings page says the same thing.
    const glm::vec4 bg(0.2f, 0.2f, 0.2f, 1.0f);
    CHECK(HE::heRenderAppIcon("definitely_not_an_icon", 32, bg, glm::vec4(1.0f)).empty());
    CHECK(HE::heRenderAppIconSet("definitely_not_an_icon", bg, glm::vec4(1.0f), { 16, 32 }).empty());
}

TEST_CASE("The containers say what they contain")
{
    const glm::vec4 bg(0.12f, 0.44f, 0.78f, 1.0f);
    const std::vector<HE::AppIconImage> set =
        HE::heRenderAppIconSet("home", bg, HE::heAppIconForeground(bg), { 16, 32, 256, 512 });
    REQUIRE(set.size() == 4);

    const auto dir = tmpDir();
    const auto icns = dir / "AppIcon.icns", ico = dir / "AppIcon.ico";
    REQUIRE(HE::heIcnsWrite(icns, set));
    REQUIRE(HE::heIcoWrite(ico, set));

    // ── .icns ────────────────────────────────────────────────────────────────
    const std::vector<std::uint8_t> icnsBytes = readAll(icns);
    REQUIRE(icnsBytes.size() > 8);
    CHECK(std::string(icnsBytes.begin(), icnsBytes.begin() + 4) == "icns");
    const auto be32 = [](const std::uint8_t* p) {
        return ((std::uint32_t)p[0] << 24) | ((std::uint32_t)p[1] << 16) |
               ((std::uint32_t)p[2] << 8) | (std::uint32_t)p[3];
    };
    // The length in the header is the length of the FILE — macOS reads it and
    // stops there, so a wrong one silently truncates the icon.
    CHECK(be32(&icnsBytes[4]) == icnsBytes.size());
    // Walk the entries: every one names a type and its own length, and they add
    // up to exactly the file. That walk is what Finder does.
    std::size_t off = 8, entries = 0;
    while (off + 8 <= icnsBytes.size())
    {
        const std::uint32_t len = be32(&icnsBytes[off + 4]);
        REQUIRE(len >= 8);
        REQUIRE(off + len <= icnsBytes.size());
        // Each payload is a PNG, which is why there is one encoder and not three.
        CHECK(icnsBytes[off + 8] == 0x89);
        CHECK(icnsBytes[off + 9] == 'P');
        off += len;
        ++entries;
    }
    CHECK(off == icnsBytes.size());
    CHECK(entries == 4);

    // ── .ico ─────────────────────────────────────────────────────────────────
    const std::vector<std::uint8_t> icoBytes = readAll(ico);
    REQUIRE(icoBytes.size() > 6);
    CHECK(icoBytes[0] == 0); CHECK(icoBytes[1] == 0);      // reserved
    CHECK(icoBytes[2] == 1); CHECK(icoBytes[3] == 0);      // type 1 = icon
    const int count = icoBytes[4] | (icoBytes[5] << 8);
    // 512 has no place in an .ico: the directory stores the side in one byte and
    // 0 already means 256. It is dropped rather than written as a lie.
    CHECK(count == 3);
    for (int i = 0; i < count; ++i)
    {
        const std::uint8_t* e = &icoBytes[6 + 16 * i];
        const std::uint32_t size   = e[8] | (e[9] << 8) | (e[10] << 16) | ((std::uint32_t)e[11] << 24);
        const std::uint32_t offset = e[12] | (e[13] << 8) | (e[14] << 16) | ((std::uint32_t)e[15] << 24);
        REQUIRE(offset + size <= icoBytes.size());
        CHECK(icoBytes[offset] == 0x89);                   // …a PNG, again
        // The recorded side matches the picture, with 0 meaning 256.
        const int side = e[0] == 0 ? 256 : e[0];
        CHECK((side == 16 || side == 32 || side == 256));
    }

    std::filesystem::remove(icns);
    std::filesystem::remove(ico);
}

// ─── A picture of the project's own ──────────────────────────────────────────
// The set the export builds from a PNG the artist drew. Two things the code does
// not say out loud: a non-square source is centred on a transparent square, and
// the resample averages AREA — a solid half stays solid and the split stays in
// the middle, whatever the size.
TEST_CASE("An icon set from an image keeps its picture at every size")
{
    // 40 x 20: left half opaque red, right half opaque blue.
    const int w = 40, h = 20;
    std::vector<std::uint8_t> src((std::size_t)w * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            std::uint8_t* p = src.data() + ((std::size_t)y * w + x) * 4;
            p[0] = x < w / 2 ? 255 : 0; p[1] = 0; p[2] = x < w / 2 ? 0 : 255; p[3] = 255;
        }

    const std::vector<HE::AppIconImage> set = HE::heAppIconSetFromImage(src.data(), w, h, { 16, 64 });
    REQUIRE(set.size() == 2);
    CHECK(set[0].px == 16);
    CHECK(set[1].px == 64);
    for (const HE::AppIconImage& img : set)
    {
        REQUIRE(img.rgba.size() == (std::size_t)img.px * img.px * 4);
        auto at = [&](int x, int y) { return img.rgba.data() + ((std::size_t)y * img.px + x) * 4; };
        const int mid = img.px / 2;
        // The picture occupies the middle half in y (20 of 40 → centred), so
        // the top and bottom rows are the transparent padding.
        CHECK(at(mid, 0)[3] == 0);
        CHECK(at(mid, img.px - 1)[3] == 0);
        // Inside it: red on the left, blue on the right, both opaque.
        CHECK(at(2, mid)[0] == 255);
        CHECK(at(2, mid)[2] == 0);
        CHECK(at(2, mid)[3] == 255);
        CHECK(at(img.px - 3, mid)[2] == 255);
        CHECK(at(img.px - 3, mid)[0] == 0);
        CHECK(at(img.px - 3, mid)[3] == 255);
    }

    // Nothing in, nothing out — not a 0x0 image, not a crash.
    CHECK(HE::heAppIconSetFromImage(nullptr, 0, 0, { 16 }).empty());
    CHECK(HE::heAppIconSetFromImage(src.data(), w, h, {}).empty());
}
