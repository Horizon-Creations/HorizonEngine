#pragma once
#include <Types/Defines.h>
#include <glm/glm.hpp>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// At GLOBAL scope on purpose: written inside `namespace HE` it would declare
// HE::SDL_Window, and the definition in the .cpp would then be a different type
// than the one SDL's own header names.
struct SDL_Window;

namespace HE {

// ── The application's own icon ───────────────────────────────────────────────
// An application needs an icon in three formats nobody wants to draw three
// times, so it is GENERATED: one of the engine's built-in icons on a rounded
// square. That is why this exists at all — the alternative is asking every
// project for a .icns, an .ico and a .png of the same picture, which is how a
// project ships with the engine's placeholder for a year.
//
// The pieces are separate on purpose: rendering is one thing, and writing a
// container is another. The containers here hold PNG payloads (both .icns and
// .ico accept them), so there is ONE encoder below and not three.

// One rendered size, RGBA8, `px` × `px`.
struct AppIconImage
{
    int px = 0;
    std::vector<std::uint8_t> rgba;
};

// The rounded square with `iconName` centred on it. `bg` is the plate, `fg` the
// icon. Empty result for a name the icon face does not have — callers then write
// no icon at all rather than a blank plate.
HE_API std::vector<std::uint8_t> heRenderAppIcon(const std::string& iconName, int px,
                                                 const glm::vec4& bg, const glm::vec4& fg);
// The sizes an .icns/.ico wants, rendered once each.
HE_API std::vector<AppIconImage> heRenderAppIconSet(const std::string& iconName,
                                                    const glm::vec4& bg, const glm::vec4& fg,
                                                    const std::vector<int>& sizes);

// A colour to read the icon over: white on a dark plate, near-black on a light
// one. So the settings ask for ONE colour and the other follows, instead of
// letting somebody pick white on yellow.
HE_API glm::vec4 heAppIconForeground(const glm::vec4& bg);

// ── The encoders ─────────────────────────────────────────────────────────────
// PNG. This is the engine's only PNG writer (stb_image.h, vendored beside it,
// only reads), so it is also what a screenshot or a generated thumbnail should
// use rather than growing a second one. A test reads its output back with
// stb_image and compares pixels.
HE_API bool hePngWrite(const std::filesystem::path& path,
                       const std::uint8_t* rgba, int w, int h);

// macOS .icns and Windows .ico, both carrying the PNG payloads above.
// Vista and later read PNG inside .ico; every macOS this engine supports reads
// the ic-- PNG types.
HE_API bool heIcnsWrite(const std::filesystem::path& path,
                        const std::vector<AppIconImage>& images);
HE_API bool heIcoWrite(const std::filesystem::path& path,
                       const std::vector<AppIconImage>& images);

// Read a PNG back and make it the window's icon. It lives here because the PNG
// decoder lives in this library, and because the file it reads is the one the
// export wrote above. False when the file is missing or unreadable — a missing
// icon is not a reason to refuse to start.
HE_API bool heSetWindowIcon(::SDL_Window* window, const std::filesystem::path& png);

// The same PNG as raw RGBA, for a caller that needs the pixels rather than a
// window icon (the tray wants its own SDL_Surface, and building one from a file
// path is the decoder's job, not the caller's). False leaves the outputs alone.
HE_API bool heLoadPngRGBA(const std::filesystem::path& png,
                          std::vector<std::uint8_t>& rgba, int& width, int& height);

} // namespace HE
