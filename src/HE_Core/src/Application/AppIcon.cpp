#include <Application/AppIcon.h>
#include <Renderer/UIFont.h>
// The one PNG ENCODER in the engine (stb_image.h beside it only decodes). It is
// here rather than hand-rolled because a PNG without a compressor is forty times
// its own size, and an icon of a flat plate is exactly the picture that
// compresses best: 512 × 512 goes from a megabyte to a few dozen kilobytes.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO          // memory only; the containers below own the file
#include <stb_image_write.h>
// The DECODER's implementation is in SplashScreen.cpp, in this same library, so
// this is just the declarations.
#include <stb_image.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_video.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace HE {

namespace
{
    // ── The plate ────────────────────────────────────────────────────────────
    // Coverage of a rounded square, antialiased by measuring the distance to the
    // rounded rectangle rather than by supersampling: an app icon is drawn once,
    // but it is drawn at 1024 and the edge is the whole impression.
    float plateCoverage(float x, float y, float px)
    {
        // Apple's squircle is not a rounded rect, and matching it exactly is a
        // separate rabbit hole. A radius of 22.4 % is what Big Sur's grid uses
        // and what reads as "an app icon" on every platform.
        const float pad = px * 0.06f;              // the icon does not touch the edge
        const float r   = (px - 2.0f * pad) * 0.224f;
        const float minX = pad, minY = pad, maxX = px - pad, maxY = px - pad;
        const float cx = std::clamp(x, minX + r, maxX - r);
        const float cy = std::clamp(y, minY + r, maxY - r);
        const float d  = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
        // One pixel of feather, centred on the edge.
        return std::clamp(r + 0.5f - d, 0.0f, 1.0f);
    }

    std::uint8_t toByte(float v) { return (std::uint8_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); }

    // ── PNG ──────────────────────────────────────────────────────────────────
    void be32(std::vector<std::uint8_t>& v, std::uint32_t x)
    {
        v.push_back((std::uint8_t)(x >> 24)); v.push_back((std::uint8_t)(x >> 16));
        v.push_back((std::uint8_t)(x >> 8));  v.push_back((std::uint8_t)x);
    }

    std::vector<std::uint8_t> encodePng(const std::uint8_t* rgba, int w, int h)
    {
        int len = 0;
        unsigned char* png = stbi_write_png_to_mem(rgba, w * 4, w, h, 4, &len);
        if (!png || len <= 0) return {};
        std::vector<std::uint8_t> out(png, png + len);
        STBIW_FREE(png);
        return out;
    }

    bool writeFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
    {
        std::error_code ec;
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
        f.close();
        return !f.fail();
    }
} // namespace

glm::vec4 heAppIconForeground(const glm::vec4& bg)
{
    // Perceived brightness, the sRGB-weighted kind: a yellow plate is light even
    // though its blue channel is not.
    const float l = 0.299f * bg.r + 0.587f * bg.g + 0.114f * bg.b;
    return l > 0.6f ? glm::vec4(0.09f, 0.09f, 0.10f, 1.0f) : glm::vec4(1.0f);
}

std::vector<std::uint8_t> heRenderAppIcon(const std::string& iconName, int px,
                                          const glm::vec4& bg, const glm::vec4& fg)
{
    std::vector<std::uint8_t> glyph;
    if (px <= 0 || !uiRasterizeIcon(iconName, px, glyph)) return {};

    std::vector<std::uint8_t> rgba((std::size_t)px * px * 4, 0);
    for (int y = 0; y < px; ++y)
        for (int x = 0; x < px; ++x)
        {
            const float plate = plateCoverage(x + 0.5f, y + 0.5f, (float)px);
            const float ink   = glyph[(std::size_t)y * px + x] / 255.0f;
            // The icon over the plate, the plate over nothing: composited here so
            // the file has straight alpha and no platform has to guess.
            const float a = plate;
            glm::vec3 c = glm::vec3(bg);
            if (ink > 0.0f) c = c * (1.0f - ink) + glm::vec3(fg) * ink;
            std::uint8_t* p = &rgba[((std::size_t)y * px + x) * 4];
            p[0] = toByte(c.r); p[1] = toByte(c.g); p[2] = toByte(c.b); p[3] = toByte(a);
        }
    return rgba;
}

std::vector<AppIconImage> heRenderAppIconSet(const std::string& iconName,
                                             const glm::vec4& bg, const glm::vec4& fg,
                                             const std::vector<int>& sizes)
{
    std::vector<AppIconImage> out;
    for (int px : sizes)
    {
        AppIconImage img;
        img.px   = px;
        img.rgba = heRenderAppIcon(iconName, px, bg, fg);
        if (img.rgba.empty()) return {};   // an unknown name is no icon at all
        out.push_back(std::move(img));
    }
    return out;
}

bool hePngWrite(const std::filesystem::path& path, const std::uint8_t* rgba, int w, int h)
{
    if (!rgba || w <= 0 || h <= 0) return false;
    const std::vector<std::uint8_t> png = encodePng(rgba, w, h);
    return !png.empty() && writeFile(path, png);
}

std::vector<std::uint8_t> hePngEncode(const std::uint8_t* rgba, int w, int h)
{
    if (!rgba || w <= 0 || h <= 0) return {};
    return encodePng(rgba, w, h);
}

bool heIcnsWrite(const std::filesystem::path& path, const std::vector<AppIconImage>& images)
{
    if (images.empty()) return false;
    // The PNG-carrying types, by pixel size. A size with no type here is simply
    // not written — better than inventing a type code macOS will not read.
    const auto typeFor = [](int px) -> const char* {
        switch (px)
        {
        case 16:   return "icp4";
        case 32:   return "icp5";
        case 64:   return "icp6";
        case 128:  return "ic07";
        case 256:  return "ic08";
        case 512:  return "ic09";
        case 1024: return "ic10";
        default:   return nullptr;
        }
    };

    std::vector<std::uint8_t> body;
    for (const AppIconImage& img : images)
    {
        const char* type = typeFor(img.px);
        if (!type || img.rgba.empty()) continue;
        const std::vector<std::uint8_t> png = encodePng(img.rgba.data(), img.px, img.px);
        body.insert(body.end(), type, type + 4);
        be32(body, (std::uint32_t)(png.size() + 8));   // the length INCLUDES the header
        body.insert(body.end(), png.begin(), png.end());
    }
    if (body.empty()) return false;

    std::vector<std::uint8_t> out{ 'i', 'c', 'n', 's' };
    be32(out, (std::uint32_t)(body.size() + 8));
    out.insert(out.end(), body.begin(), body.end());
    return writeFile(path, out);
}

bool heIcoWrite(const std::filesystem::path& path, const std::vector<AppIconImage>& images)
{
    if (images.empty()) return false;
    std::vector<std::vector<std::uint8_t>> pngs;
    std::vector<int> sizes;
    for (const AppIconImage& img : images)
    {
        // An .ico stores the side in ONE byte, with 0 meaning 256. Anything
        // larger has no way to be named in the directory.
        if (img.px <= 0 || img.px > 256 || img.rgba.empty()) continue;
        pngs.push_back(encodePng(img.rgba.data(), img.px, img.px));
        sizes.push_back(img.px);
    }
    if (pngs.empty()) return false;

    const auto le16 = [](std::vector<std::uint8_t>& v, std::uint16_t x)
    { v.push_back((std::uint8_t)(x & 0xFF)); v.push_back((std::uint8_t)(x >> 8)); };
    const auto le32 = [](std::vector<std::uint8_t>& v, std::uint32_t x)
    {
        v.push_back((std::uint8_t)(x & 0xFF));         v.push_back((std::uint8_t)((x >> 8) & 0xFF));
        v.push_back((std::uint8_t)((x >> 16) & 0xFF)); v.push_back((std::uint8_t)((x >> 24) & 0xFF));
    };

    std::vector<std::uint8_t> out;
    le16(out, 0);                                  // reserved
    le16(out, 1);                                  // type 1 = icon
    le16(out, (std::uint16_t)pngs.size());
    std::uint32_t offset = (std::uint32_t)(6 + 16 * pngs.size());
    for (std::size_t i = 0; i < pngs.size(); ++i)
    {
        out.push_back((std::uint8_t)(sizes[i] == 256 ? 0 : sizes[i]));   // width
        out.push_back((std::uint8_t)(sizes[i] == 256 ? 0 : sizes[i]));   // height
        out.push_back(0);                          // palette entries (none)
        out.push_back(0);                          // reserved
        le16(out, 1);                              // colour planes
        le16(out, 32);                             // bits per pixel
        le32(out, (std::uint32_t)pngs[i].size());
        le32(out, offset);
        offset += (std::uint32_t)pngs[i].size();
    }
    for (const std::vector<std::uint8_t>& png : pngs)
        out.insert(out.end(), png.begin(), png.end());
    return writeFile(path, out);
}

std::vector<AppIconImage> heAppIconSetFromImage(const std::uint8_t* rgba, int w, int h,
                                                const std::vector<int>& sizes)
{
    std::vector<AppIconImage> out;
    if (!rgba || w <= 0 || h <= 0) return out;

    // Square it first: a wide picture becomes a wide picture in the middle of a
    // transparent square, which is what every icon container expects, rather
    // than a squashed one. Nothing to do for a square source.
    const int side = std::max(w, h);
    std::vector<std::uint8_t> square;
    const std::uint8_t* src = rgba;
    if (w != h)
    {
        square.assign((std::size_t)side * side * 4, 0);
        const int ox = (side - w) / 2, oy = (side - h) / 2;
        for (int y = 0; y < h; ++y)
            std::memcpy(square.data() + ((std::size_t)(y + oy) * side + ox) * 4,
                        rgba + (std::size_t)y * w * 4, (std::size_t)w * 4);
        src = square.data();
    }

    for (const int px : sizes)
    {
        if (px <= 0) continue;
        AppIconImage img;
        img.px = px;
        img.rgba.resize((std::size_t)px * px * 4);
        // Area average over the source box each destination texel covers. For
        // an upscale the box is a single texel or two, which degrades to a
        // nearest/bilinear-ish sample — an icon drawn smaller than 512 is the
        // artist's choice and a blur would not improve it.
        for (int dy = 0; dy < px; ++dy)
        {
            const int sy0 = dy * side / px;
            const int sy1 = std::max(sy0 + 1, (dy + 1) * side / px);
            for (int dx = 0; dx < px; ++dx)
            {
                const int sx0 = dx * side / px;
                const int sx1 = std::max(sx0 + 1, (dx + 1) * side / px);
                // Premultiplied accumulation: a transparent neighbour must not
                // bleed its (meaningless) colour into an opaque edge.
                double r = 0, g = 0, b = 0, a = 0;
                int n = 0;
                for (int sy = sy0; sy < sy1 && sy < side; ++sy)
                    for (int sx = sx0; sx < sx1 && sx < side; ++sx)
                    {
                        const std::uint8_t* p = src + ((std::size_t)sy * side + sx) * 4;
                        const double pa = p[3] / 255.0;
                        r += p[0] * pa; g += p[1] * pa; b += p[2] * pa; a += pa;
                        ++n;
                    }
                std::uint8_t* d = img.rgba.data() + ((std::size_t)dy * px + dx) * 4;
                if (n == 0 || a <= 0.0) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
                d[0] = (std::uint8_t)std::lround(std::clamp(r / a, 0.0, 255.0));
                d[1] = (std::uint8_t)std::lround(std::clamp(g / a, 0.0, 255.0));
                d[2] = (std::uint8_t)std::lround(std::clamp(b / a, 0.0, 255.0));
                d[3] = (std::uint8_t)std::lround(std::clamp(a / n * 255.0, 0.0, 255.0));
            }
        }
        out.push_back(std::move(img));
    }
    return out;
}

bool heLoadPngRGBA(const std::filesystem::path& png,
                   std::vector<std::uint8_t>& rgba, int& width, int& height)
{
    std::error_code ec;
    if (!std::filesystem::exists(png, ec)) return false;
    int w = 0, h = 0, ch = 0;
    unsigned char* pixels = stbi_load(png.string().c_str(), &w, &h, &ch, 4);
    if (!pixels) return false;
    rgba.assign(pixels, pixels + (std::size_t)w * h * 4);
    width  = w;
    height = h;
    stbi_image_free(pixels);
    return true;
}

bool heSetWindowIcon(SDL_Window* window, const std::filesystem::path& png)
{
    if (!window) return false;
    std::error_code ec;
    if (!std::filesystem::exists(png, ec)) return false;

    int w = 0, h = 0, ch = 0;
    unsigned char* pixels = stbi_load(png.string().c_str(), &w, &h, &ch, 4);
    if (!pixels) return false;
    // SDL copies nothing: the surface points at these pixels, so both live until
    // after SDL_SetWindowIcon has taken what it needs.
    SDL_Surface* surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
    const bool ok = surface && SDL_SetWindowIcon(window, surface);
    if (surface) SDL_DestroySurface(surface);
    stbi_image_free(pixels);
    return ok;
}

} // namespace HE
