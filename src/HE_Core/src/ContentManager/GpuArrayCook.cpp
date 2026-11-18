#include "ContentManager/GpuArrayCook.h"

#include "ContentManager/HAsset.h"
#include "Diagnostics/Log.h"
#include "Hpak/HpakFormat.h"   // fnv1a64Bytes: the cache key's path hash
#include "Hpak/HpakWriter.h"   // cookTextureBlob
#include "Types/Enums.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace HE
{

namespace
{

std::mutex                g_mutex;
GpuArrayCookSettings      g_settings;
std::atomic<bool>         g_on{ false };
std::atomic<bool>         g_noEncoder{ false };   // a cook came back unchanged: this build cannot encode

// What a loose texture .hasset says about itself in its first bytes (META, then TXMI, both small).
struct TextureHead { uint8_t format = 0; uint32_t channels = 0, layers = 1; bool valid = false; };

// header: "HAST", u16 version, u16 type, u32 chunk count, padded to 32; chunk: 4-char id, u64 size, data.
TextureHead peekTexture(const uint8_t* b, size_t n)
{
    TextureHead h;
    if (n < 32 || std::memcmp(b, "HAST", 4) != 0) return h;
    uint32_t chunks = 0;
    std::memcpy(&chunks, b + 8, 4);
    size_t off = 32;
    for (uint32_t i = 0; i < chunks && off + 12 <= n; ++i)
    {
        uint64_t size = 0;
        std::memcpy(&size, b + off + 4, 8);
        const size_t data = off + 12;
        if (std::memcmp(b + off, "TXMI", 4) == 0)
        {
            if (size > 64 || data + size > n) return h;
            const std::vector<uint8_t> tx(b + data, b + data + static_cast<size_t>(size));
            size_t o = 0;
            uint32_t w = 0, hh = 0, ch = 0, mips = 0;
            if (!HAsset::readTextureHeader(tx, o, w, hh, ch)) return h;
            h.channels = ch;
            if (o + 4 <= tx.size()) HAsset::Reader::readPOD(tx, o, mips);
            if (o + 1 <= tx.size()) HAsset::Reader::readPOD(tx, o, h.format);
            if (o + 1 <= tx.size()) { uint8_t s = 0; HAsset::Reader::readPOD(tx, o, s); }
            if (o + 4 <= tx.size()) HAsset::Reader::readPOD(tx, o, h.layers);
            h.valid = true;
            return h;
        }
        if (std::memcmp(b + off, "PIXL", 4) == 0) return h;   // the payload comes after TXMI: not a texture
        off = data + static_cast<size_t>(size);
    }
    return h;
}

bool readAll(const fs::path& p, std::vector<uint8_t>& out)
{
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff end = f.tellg();
    if (end <= 0) return false;
    out.resize(static_cast<size_t>(end));
    f.seekg(0, std::ios::beg);
    return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), end));
}

std::string hex16(uint64_t v)
{
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

} // namespace

void setGpuArrayCook(const GpuArrayCookSettings& settings)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_settings = settings;
    g_noEncoder = false;
    g_on = settings.format != 0 && !settings.cacheDir.empty();
}

GpuArrayCookSettings gpuArrayCook()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_settings;
}

bool gpuArrayCookEnabled() { return g_on.load() && !g_noEncoder.load(); }

uint8_t gpuArrayCookFormatFor(int rendererBackend)
{
    if (const char* e = std::getenv("HE_EDITOR_ARRAY_COMPRESS"); e && *e)
    {
        const std::string v = e;
        if (v == "bc7")  return 2;
        if (v == "astc") return 1;
        if (v == "bc3")  return 3;
        return 0;                       // "none", "0", anything else: RGBA8 as it is on the server
    }
    switch (static_cast<HE::RendererBackend>(rendererBackend))
    {
    case HE::RendererBackend::Metal:   return 1;   // ASTC 4x4: the only block format Apple GPUs sample
    case HE::RendererBackend::OpenGL:
#if defined(__APPLE__)
        return 3;                                  // macOS GL 4.1: S3TC, no BPTC
#else
        return 2;
#endif
    case HE::RendererBackend::Vulkan:
#if defined(__APPLE__)
        return 0;                                  // MoltenVK: no BC on Apple GPUs
#else
        return 2;
#endif
    case HE::RendererBackend::D3D11:
    case HE::RendererBackend::D3D12:   return 2;
    case HE::RendererBackend::Software:
    default:                           return 0;
    }
}

bool readGpuCookedArray(const std::string& sourcePath, std::vector<uint8_t>& out)
{
    if (!gpuArrayCookEnabled()) return false;
    GpuArrayCookSettings s = gpuArrayCook();

    const fs::path src(sourcePath);
    if (src.extension() != ".hasset") return false;
    std::error_code ec;
    const uint64_t size = fs::file_size(src, ec);
    if (ec || size < s.minSourceBytes || size < 64) return false;

    // The first bytes say whether this is an RGBA8 array at all, without reading hundreds of MB.
    {
        std::vector<uint8_t> head(4096);
        std::ifstream f(src, std::ios::binary);
        if (!f) return false;
        f.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<size_t>(f.gcount()));
        const TextureHead h = peekTexture(head.data(), head.size());
        if (!h.valid || h.format != 0 || h.channels != 4 || h.layers <= 1) return false;
    }

    const auto mtime = fs::last_write_time(src, ec);
    if (ec) return false;
    const std::string key = [&] {
        const std::string canon = src.lexically_normal().generic_string();
        return hex16(Hpak::fnv1a64Bytes(reinterpret_cast<const uint8_t*>(canon.data()), canon.size()));
    }();
    const fs::path dir = fs::path(s.cacheDir) / ("f" + std::to_string(s.format) + "q" + std::to_string(s.quality));
    const fs::path cached = dir / (key + "_" + std::to_string(size) + "_" +
                                   std::to_string(static_cast<long long>(mtime.time_since_epoch().count())) + ".hasset");

    // Cache hit: the cooked file stands in for the source.
    if (fs::exists(cached, ec))
    {
        std::vector<uint8_t> blob;
        if (readAll(cached, blob))
        {
            const TextureHead h = peekTexture(blob.data(), blob.size());
            if (h.valid && h.format == s.format && h.layers > 1)
            {
                out = std::move(blob);
                return true;
            }
        }
        fs::remove(cached, ec);   // unreadable or of another format: encode again below
    }

    std::vector<uint8_t> blob;
    if (!readAll(src, blob)) return false;
    const auto t0 = std::chrono::steady_clock::now();
    HE_LOG_INFO(Asset, "Compressing texture array '%s' for the GPU (%.0f MB RGBA8, format %u) — once per download",
                src.filename().string().c_str(), static_cast<double>(size) / (1024.0 * 1024.0), s.format);
    std::vector<uint8_t> cooked = Hpak::cookTextureBlob(blob, s.format, s.quality);
    if (cooked == blob)
    {
        g_noEncoder = true;   // an RGBA8 array that stays RGBA8: no encoder for this format in this build
        HE_LOG_WARN(Asset, "Texture array cook: this build cannot encode format %u, arrays stay RGBA8", s.format);
        return false;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    HE_LOG_INFO(Asset, "Compressed '%s': %.0f MB -> %.0f MB in %.1f s", src.filename().string().c_str(),
                static_cast<double>(size) / (1024.0 * 1024.0), static_cast<double>(cooked.size()) / (1024.0 * 1024.0), secs);

    // Keep it: drop what an older version of this file left behind, write the new one atomically.
    fs::create_directories(dir, ec);
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().filename().string().rfind(key + "_", 0) == 0) fs::remove(e.path(), ec);
    const fs::path tmp = cached.string() + "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(cooked.data()), static_cast<std::streamsize>(cooked.size()));
        if (!f) { fs::remove(tmp, ec); out = std::move(cooked); return true; }   // no cache this time, still cooked
    }
    fs::rename(tmp, cached, ec);
    if (ec) fs::remove(tmp, ec);
    out = std::move(cooked);
    return true;
}

} // namespace HE
