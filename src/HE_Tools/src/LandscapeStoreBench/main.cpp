// landscape_store_bench: measurement base for the world-scale landscape (docs/landscape-world-scale-plan-2026-10-28.md,
// step 0). Pure data store, no rendering: a procedural 100 km world is cut into tiles, every tile is encoded, written,
// read back and decoded, and each stage is timed. The numbers replace the estimates in plan section 2.
//
//   landscape_store_bench [--dir D] [--tiles-x N] [--tiles-z N] [--full] [--tile-m 512] [--raster-h 2] [--raster-w 4]
//                         [--weight-pages 3] [--rough LSB] [--threads N] [--sample N] [--disk-mb N] [--keep]
//
// Everything is std C++ plus POSIX pread where there is one; the cold-cache reads need fcntl/posix_fadvise, so on
// Windows they fall back to warm reads and the output says so.
#include <HorizonScene/TerrainMeshGenerator.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef HE_HAVE_LZ4
#include <lz4.h>
#endif
#ifdef HE_HAVE_ZSTD
#include <zstd.h>
#endif
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using Clock  = std::chrono::steady_clock;

namespace
{
double msSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

struct Config
{
    std::string dir        = "landscape_bench_out";
    int         tilesX     = 8;
    int         tilesZ     = 0; // 0 = whole world length
    float       tileM      = 512.0f;
    float       worldM     = 100000.0f;
    float       rasterH    = 2.0f;
    float       rasterW    = 4.0f;
    int         pages      = 3; // kTerrainWeightPages on release/0.8.0 (the plan counted 2)
    float       heightScale = 3000.0f;
    int         rough      = 0; // +-LSB white noise on the heights (real DEMs are not noise-free)
    unsigned    threads    = 0;
    int         sample     = 48;
    int         diskMb     = 2048;
    bool        keep       = false;
    std::string primary    = "";
};

struct Tile
{
    std::vector<std::vector<uint16_t>> h; // level k: (cells>>k)+1 squared
    std::vector<std::vector<uint8_t>>  w; // level k: (texels>>k) squared, channels interleaved
};

struct Geometry
{
    int hCells = 0, wTexels = 0, channels = 0;
    int hLevels = 0, wLevels = 0;
    int hDim(int k) const { return (hCells >> k) + 1; }
    int wDim(int k) const { return wTexels >> k; }
};

Geometry makeGeometry(const Config& c)
{
    Geometry g;
    g.hCells   = static_cast<int>(std::lround(c.tileM / c.rasterH));
    g.wTexels  = static_cast<int>(std::lround(c.tileM / c.rasterW));
    g.channels = 4 * c.pages;
    const int floorCells = static_cast<int>(std::lround(c.tileM / 128.0f)); // pyramid ends at the 128 m raster
    for (int k = 0; (g.hCells >> k) >= floorCells; ++k) ++g.hLevels;
    for (int k = 0; (g.wTexels >> k) >= floorCells; ++k) ++g.wLevels;
    return g;
}

// ── generation ────────────────────────────────────────────────────────────────
float fbmAt(const Config& c, double wx, double wz, int seed, int octaves, float freq)
{
    return terrainFbm(seed, static_cast<float>(wx / c.worldM), static_cast<float>(wz / c.worldM), octaves, freq, 2.0f, 0.5f);
}

Tile generateTile(const Config& c, const Geometry& g, int tx, int tz)
{
    Tile t;
    const double x0 = tx * static_cast<double>(c.tileM), z0 = tz * static_cast<double>(c.tileM);
    const int    n  = g.hDim(0);
    std::vector<uint16_t> h0(static_cast<size_t>(n) * n);
    std::mt19937 rng(static_cast<uint32_t>(tx * 73856093u ^ tz * 19349663u));
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x)
        {
            // 12 octaves at 6 base cycles over the world reach ~8 m wavelength: the same fBm the TerrainComponent uses.
            const float f = fbmAt(c, x0 + x * static_cast<double>(c.rasterH), z0 + z * static_cast<double>(c.rasterH), 1337, 12, 6.0f);
            int v = static_cast<int>(std::lround(f * 65535.0f));
            if (c.rough > 0) v += static_cast<int>(rng() % (2u * c.rough + 1u)) - c.rough;
            h0[static_cast<size_t>(z) * n + x] = static_cast<uint16_t>(std::clamp(v, 0, 65535));
        }
    t.h.push_back(std::move(h0));
    for (int k = 1; k < g.hLevels; ++k) // point-sampled: keeps tile edges identical to the neighbour's
    {
        const int dn = g.hDim(k);
        std::vector<uint16_t> lv(static_cast<size_t>(dn) * dn);
        for (int z = 0; z < dn; ++z)
            for (int x = 0; x < dn; ++x) lv[static_cast<size_t>(z) * dn + x] = t.h[0][static_cast<size_t>(z << k) * n + (x << k)];
        t.h.push_back(std::move(lv));
    }

    // Weights: height bands x slope x per-layer noise, normalised to 255 (the auto-landscape material's inputs).
    const int W = g.wTexels, C = g.channels, step = static_cast<int>(std::lround(c.rasterW / c.rasterH));
    std::vector<uint8_t> w0(static_cast<size_t>(W) * W * C);
    std::vector<float>   s(C);
    for (int z = 0; z < W; ++z)
        for (int x = 0; x < W; ++x)
        {
            const int hx = std::min(x * step, n - 1), hz = std::min(z * step, n - 1);
            const float hv = t.h[0][static_cast<size_t>(hz) * n + hx] / 65535.0f;
            const int   hx1 = std::min(hx + 1, n - 1), hz1 = std::min(hz + 1, n - 1);
            const float dx = (t.h[0][static_cast<size_t>(hz) * n + hx1] - t.h[0][static_cast<size_t>(hz) * n + hx]) / 65535.0f;
            const float dz = (t.h[0][static_cast<size_t>(hz1) * n + hx] - t.h[0][static_cast<size_t>(hz) * n + hx]) / 65535.0f;
            const float slope = std::min(1.0f, std::sqrt(dx * dx + dz * dz) * c.heightScale / c.rasterH);
            float sum = 0.0f;
            for (int k = 0; k < C; ++k)
            {
                const float center = (k + 0.5f) / C;
                const float band   = std::max(0.0f, 1.0f - std::abs(hv - center) * C * 0.7f);
                const float nz     = fbmAt(c, x0 + x * static_cast<double>(c.rasterW), z0 + z * static_cast<double>(c.rasterW), 100 + k, 3, 400.0f);
                s[k] = band * (k % 3 == 0 ? 1.0f - slope : 0.4f + 0.6f * slope) + 0.25f * nz;
                sum += s[k];
            }
            int acc = 0, best = 0;
            uint8_t* p = &w0[(static_cast<size_t>(z) * W + x) * C];
            for (int k = 0; k < C; ++k)
            {
                p[k] = static_cast<uint8_t>(std::lround(255.0f * s[k] / std::max(sum, 1e-6f)));
                acc += p[k];
                if (p[k] > p[best]) best = k;
            }
            p[best] = static_cast<uint8_t>(std::clamp(p[best] + 255 - acc, 0, 255));
        }
    t.w.push_back(std::move(w0));
    for (int k = 1; k < g.wLevels; ++k) // 2x2 box
    {
        const int dn = g.wDim(k), sn = g.wDim(k - 1);
        std::vector<uint8_t> lv(static_cast<size_t>(dn) * dn * C);
        for (int z = 0; z < dn; ++z)
            for (int x = 0; x < dn; ++x)
                for (int ch = 0; ch < C; ++ch)
                {
                    const auto at = [&](int xx, int zz) { return int(t.w[k - 1][(static_cast<size_t>(zz) * sn + xx) * C + ch]); };
                    lv[(static_cast<size_t>(z) * dn + x) * C + ch] =
                        static_cast<uint8_t>((at(2 * x, 2 * z) + at(2 * x + 1, 2 * z) + at(2 * x, 2 * z + 1) + at(2 * x + 1, 2 * z + 1) + 2) / 4);
                }
        t.w.push_back(std::move(lv));
    }
    return t;
}

// ── transforms (lossless) ─────────────────────────────────────────────────────
// Heights: plane predictor (left + up - upleft) in wrap-around 16 bit, zigzag, then lo-bytes plane and hi-bytes plane.
void heightForward(const std::vector<uint16_t>& v, int n, std::vector<uint8_t>& out)
{
    out.resize(v.size() * 2);
    const size_t cnt = v.size();
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x)
        {
            const size_t i  = static_cast<size_t>(z) * n + x;
            const int    l  = x ? v[i - 1] : 0, u = z ? v[i - n] : 0, ul = (x && z) ? v[i - n - 1] : 0;
            const int    pr = (x && z) ? (l + u - ul) : (x ? l : u);
            const uint16_t d = static_cast<uint16_t>(v[i] - pr);
            const uint16_t zz = static_cast<uint16_t>((d << 1) ^ (static_cast<int16_t>(d) >> 15));
            out[i]       = static_cast<uint8_t>(zz & 0xFF);
            out[cnt + i] = static_cast<uint8_t>(zz >> 8);
        }
}
void heightInverse(const uint8_t* in, int n, std::vector<uint16_t>& v)
{
    const size_t cnt = static_cast<size_t>(n) * n;
    v.resize(cnt);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x)
        {
            const size_t i  = static_cast<size_t>(z) * n + x;
            const uint16_t zz = static_cast<uint16_t>(in[i] | (in[cnt + i] << 8));
            const uint16_t d  = static_cast<uint16_t>((zz >> 1) ^ -(zz & 1));
            const int      l  = x ? v[i - 1] : 0, u = z ? v[i - n] : 0, ul = (x && z) ? v[i - n - 1] : 0;
            const int      pr = (x && z) ? (l + u - ul) : (x ? l : u);
            v[i] = static_cast<uint16_t>(d + pr);
        }
}
// Weights: channel planes, horizontal delta mod 256 inside each plane.
void weightForward(const std::vector<uint8_t>& v, int dim, int C, std::vector<uint8_t>& out)
{
    const size_t plane = static_cast<size_t>(dim) * dim;
    out.resize(v.size());
    for (int ch = 0; ch < C; ++ch)
        for (int z = 0; z < dim; ++z)
            for (int x = 0; x < dim; ++x)
            {
                const size_t i = static_cast<size_t>(z) * dim + x;
                const uint8_t cur = v[i * C + ch], prev = x ? v[(i - 1) * C + ch] : 0;
                out[ch * plane + i] = static_cast<uint8_t>(cur - prev);
            }
}
void weightInverse(const uint8_t* in, int dim, int C, std::vector<uint8_t>& v)
{
    const size_t plane = static_cast<size_t>(dim) * dim;
    v.resize(plane * C);
    for (int ch = 0; ch < C; ++ch)
        for (int z = 0; z < dim; ++z)
        {
            uint8_t acc = 0;
            for (int x = 0; x < dim; ++x)
            {
                const size_t i = static_cast<size_t>(z) * dim + x;
                acc = static_cast<uint8_t>(acc + in[ch * plane + i]);
                v[i * C + ch] = acc;
            }
        }
}

// ── codecs ────────────────────────────────────────────────────────────────────
enum class Backend : uint8_t { Store, Lz4, Zstd };
struct Codec
{
    const char* name;
    Backend     backend;
    bool        transform;
};

int g_zstdLevel = 3;

std::vector<uint8_t> compress(Backend b, const uint8_t* src, size_t n)
{
    std::vector<uint8_t> out;
    switch (b)
    {
#ifdef HE_HAVE_LZ4
    case Backend::Lz4:
        out.resize(static_cast<size_t>(LZ4_compressBound(static_cast<int>(n))));
        out.resize(static_cast<size_t>(LZ4_compress_default(reinterpret_cast<const char*>(src), reinterpret_cast<char*>(out.data()),
                                                            static_cast<int>(n), static_cast<int>(out.size()))));
        return out;
#endif
#ifdef HE_HAVE_ZSTD
    case Backend::Zstd:
        out.resize(ZSTD_compressBound(n));
        out.resize(ZSTD_compress(out.data(), out.size(), src, n, g_zstdLevel));
        return out;
#endif
    default: out.assign(src, src + n); return out;
    }
}
void decompress(Backend b, const uint8_t* src, size_t csize, uint8_t* dst, size_t n)
{
    switch (b)
    {
#ifdef HE_HAVE_LZ4
    case Backend::Lz4: LZ4_decompress_safe(reinterpret_cast<const char*>(src), reinterpret_cast<char*>(dst), static_cast<int>(csize), static_cast<int>(n)); return;
#endif
#ifdef HE_HAVE_ZSTD
    case Backend::Zstd: ZSTD_decompress(dst, n, src, csize); return;
#endif
    default: std::memcpy(dst, src, n); return;
    }
}

// Container: u32 magic, u8 backend, u8 transform, u8 hLevels, u8 wLevels, then per stream u32 raw size, u32 comp size, bytes.
void put32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(static_cast<uint8_t>(v >> (8 * i))); }
uint32_t get32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

std::vector<uint8_t> encodeTile(const Codec& cd, const Geometry& g, const Tile& t, size_t* heightBytes = nullptr)
{
    std::vector<uint8_t> out;
    put32(out, 0x48544c31u);
    out.push_back(static_cast<uint8_t>(cd.backend));
    out.push_back(cd.transform);
    out.push_back(static_cast<uint8_t>(g.hLevels));
    out.push_back(static_cast<uint8_t>(g.wLevels));
    std::vector<uint8_t> tmp;
    const auto stream = [&](const uint8_t* p, size_t n) {
        const auto c = compress(cd.backend, p, n);
        put32(out, static_cast<uint32_t>(n));
        put32(out, static_cast<uint32_t>(c.size()));
        out.insert(out.end(), c.begin(), c.end());
    };
    for (int k = 0; k < g.hLevels; ++k)
    {
        if (cd.transform) { heightForward(t.h[k], g.hDim(k), tmp); stream(tmp.data(), tmp.size()); }
        else stream(reinterpret_cast<const uint8_t*>(t.h[k].data()), t.h[k].size() * 2);
    }
    if (heightBytes) *heightBytes = out.size();
    for (int k = 0; k < g.wLevels; ++k)
    {
        if (cd.transform) { weightForward(t.w[k], g.wDim(k), g.channels, tmp); stream(tmp.data(), tmp.size()); }
        else stream(t.w[k].data(), t.w[k].size());
    }
    return out;
}

// maxLevels < 0: whole pyramid. 1: level 0 of heights and weights only (what physics and the near mesh need first).
Tile decodeTile(const Geometry& g, const uint8_t* p, size_t, int maxLevels = -1)
{
    Tile t;
    const Backend b = static_cast<Backend>(p[4]);
    const bool    tr = p[5];
    const uint8_t* q = p + 8;
    std::vector<uint8_t> raw;
    const auto next = [&](int& rawN, const uint8_t*& cp, uint32_t& cn) {
        rawN = static_cast<int>(get32(q)); cn = get32(q + 4); cp = q + 8; q += 8 + cn;
    };
    const int hl = maxLevels < 0 ? g.hLevels : std::min(maxLevels, g.hLevels);
    const int wl = maxLevels < 0 ? g.wLevels : std::min(maxLevels, g.wLevels);
    for (int k = 0; k < g.hLevels; ++k)
    {
        int rn; const uint8_t* cp; uint32_t cn; next(rn, cp, cn);
        if (k >= hl) continue;
        raw.resize(rn); decompress(b, cp, cn, raw.data(), rn);
        std::vector<uint16_t> v;
        if (tr) heightInverse(raw.data(), g.hDim(k), v);
        else { v.resize(rn / 2); std::memcpy(v.data(), raw.data(), rn); }
        t.h.push_back(std::move(v));
    }
    for (int k = 0; k < g.wLevels; ++k)
    {
        int rn; const uint8_t* cp; uint32_t cn; next(rn, cp, cn);
        if (k >= wl) continue;
        raw.resize(rn); decompress(b, cp, cn, raw.data(), rn);
        std::vector<uint8_t> v;
        if (tr) weightInverse(raw.data(), g.wDim(k), g.channels, v);
        else v = raw;
        t.w.push_back(std::move(v));
    }
    return t;
}

size_t rawBytes(const Geometry& g)
{
    size_t s = 0;
    for (int k = 0; k < g.hLevels; ++k) s += static_cast<size_t>(g.hDim(k)) * g.hDim(k) * 2;
    for (int k = 0; k < g.wLevels; ++k) s += static_cast<size_t>(g.wDim(k)) * g.wDim(k) * g.channels;
    return s;
}
size_t rawBytesLevel0(const Geometry& g)
{
    return static_cast<size_t>(g.hDim(0)) * g.hDim(0) * 2 + static_cast<size_t>(g.wDim(0)) * g.wDim(0) * g.channels;
}

struct Stat
{
    std::vector<double> v;
    void   add(double x) { v.push_back(x); }
    double mean() const { return v.empty() ? 0 : std::accumulate(v.begin(), v.end(), 0.0) / v.size(); }
    double pct(double p) const
    {
        if (v.empty()) return 0;
        std::vector<double> s = v;
        std::sort(s.begin(), s.end());
        return s[std::min(s.size() - 1, static_cast<size_t>(p * s.size()))];
    }
};

// ── file io ───────────────────────────────────────────────────────────────────
class Reader
{
public:
    Reader(const std::string& path, bool cold) : m_cold(cold)
    {
#ifndef _WIN32
        m_fd = ::open(path.c_str(), O_RDONLY);
    #ifdef __APPLE__
        if (m_fd >= 0 && cold) ::fcntl(m_fd, F_NOCACHE, 1);
    #elif defined(POSIX_FADV_DONTNEED)
        if (m_fd >= 0 && cold) ::posix_fadvise(m_fd, 0, 0, POSIX_FADV_DONTNEED);
    #endif
#else
        m_in.open(path, std::ios::binary);
#endif
    }
    ~Reader()
    {
#ifndef _WIN32
        if (m_fd >= 0) ::close(m_fd);
#endif
    }
    bool read(uint64_t off, uint8_t* dst, size_t n)
    {
#ifndef _WIN32
    #if defined(POSIX_FADV_DONTNEED) && !defined(__APPLE__)
        if (m_cold) ::posix_fadvise(m_fd, static_cast<off_t>(off), static_cast<off_t>(n), POSIX_FADV_DONTNEED);
    #endif
        return m_fd >= 0 && ::pread(m_fd, dst, n, static_cast<off_t>(off)) == static_cast<ssize_t>(n);
#else
        m_in.clear(); m_in.seekg(static_cast<std::streamoff>(off));
        return static_cast<bool>(m_in.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(n)));
#endif
    }
private:
    bool m_cold;
#ifndef _WIN32
    int m_fd = -1;
#else
    std::ifstream m_in;
#endif
};
// Writes without leaving the data in the page cache (so a later "cold" read really goes to the disk): macOS F_NOCACHE,
// Linux fsync + POSIX_FADV_DONTNEED at close. Elsewhere an ordinary write.
class Writer
{
public:
    explicit Writer(const std::string& path, bool bypass)
    {
#ifndef _WIN32
        m_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    #ifdef __APPLE__
        if (m_fd >= 0 && bypass) ::fcntl(m_fd, F_NOCACHE, 1);
    #endif
        m_bypass = bypass;
#else
        (void)bypass;
        m_out.open(path, std::ios::binary);
#endif
    }
    ~Writer()
    {
#ifndef _WIN32
        if (m_fd >= 0)
        {
    #if defined(POSIX_FADV_DONTNEED) && !defined(__APPLE__)
            if (m_bypass) { ::fsync(m_fd); ::posix_fadvise(m_fd, 0, 0, POSIX_FADV_DONTNEED); }
    #endif
            ::close(m_fd);
        }
#endif
    }
    void write(const uint8_t* p, size_t n)
    {
#ifndef _WIN32
        while (n && m_fd >= 0)
        {
            const ssize_t w = ::write(m_fd, p, n);
            if (w <= 0) return;
            p += w; n -= static_cast<size_t>(w);
        }
#else
        m_out.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(n));
#endif
    }
private:
#ifndef _WIN32
    int  m_fd     = -1;
    bool m_bypass = false;
#else
    std::ofstream m_out;
#endif
};
constexpr bool kColdSupported =
#ifdef _WIN32
    false;
#else
    true;
#endif

void parallelFor(unsigned threads, size_t count, const std::function<void(size_t, unsigned)>& fn)
{
    std::atomic<size_t> next{0};
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < threads; ++t)
        ts.emplace_back([&, t] { for (size_t i; (i = next.fetch_add(1)) < count;) fn(i, t); });
    for (auto& t : ts) t.join();
}
} // namespace

int main(int argc, char** argv)
{
    Config c;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        const auto val = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--dir") c.dir = val();
        else if (a == "--tiles-x") c.tilesX = std::atoi(val().c_str());
        else if (a == "--tiles-z") c.tilesZ = std::atoi(val().c_str());
        else if (a == "--full") { c.tilesX = c.tilesZ = -1; }
        else if (a == "--tile-m") c.tileM = static_cast<float>(std::atof(val().c_str()));
        else if (a == "--raster-h") c.rasterH = static_cast<float>(std::atof(val().c_str()));
        else if (a == "--raster-w") c.rasterW = static_cast<float>(std::atof(val().c_str()));
        else if (a == "--weight-pages") c.pages = std::atoi(val().c_str());
        else if (a == "--rough") c.rough = std::atoi(val().c_str());
        else if (a == "--threads") c.threads = static_cast<unsigned>(std::atoi(val().c_str()));
        else if (a == "--sample") c.sample = std::atoi(val().c_str());
        else if (a == "--disk-mb") c.diskMb = std::atoi(val().c_str());
        else if (a == "--zstd-level") g_zstdLevel = std::atoi(val().c_str());
        else if (a == "--primary") c.primary = val();
        else if (a == "--keep") c.keep = true;
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    if (c.threads == 0) c.threads = std::max(1u, std::thread::hardware_concurrency());
    const int worldTiles = static_cast<int>(std::ceil(c.worldM / c.tileM));
    if (c.tilesX < 0) c.tilesX = worldTiles;
    if (c.tilesZ <= 0) c.tilesZ = worldTiles;
    c.tilesX = std::min(c.tilesX, worldTiles);
    c.tilesZ = std::min(c.tilesZ, worldTiles);

    const Geometry g = makeGeometry(c);
    if ((g.hCells & (g.hCells - 1)) || (g.wTexels & (g.wTexels - 1))) { std::fprintf(stderr, "tile-m / raster must give powers of two\n"); return 2; }
    const size_t nTiles = static_cast<size_t>(c.tilesX) * c.tilesZ;

    std::vector<Codec> codecs = {{"store", Backend::Store, false}};
#ifdef HE_HAVE_LZ4
    codecs.push_back({"lz4", Backend::Lz4, false});
    codecs.push_back({"lz4+pred", Backend::Lz4, true});
#endif
#ifdef HE_HAVE_ZSTD
    codecs.push_back({"zstd", Backend::Zstd, false});
    codecs.push_back({"zstd+pred", Backend::Zstd, true});
#endif
    codecs.push_back({"store+pred", Backend::Store, true});
    Codec primary = codecs.back();
    for (const Codec& cd : codecs) if (c.primary == cd.name || (c.primary.empty() && std::strcmp(cd.name, "zstd+pred") == 0)) primary = cd;
    if (c.primary.empty() && std::strcmp(primary.name, "zstd+pred") != 0)
        for (const Codec& cd : codecs) if (std::strcmp(cd.name, "lz4+pred") == 0) primary = cd;

    std::printf("# landscape_store_bench\n\n");
    std::printf("- tile %.0f m, heights %.1f m raster (%dx%d, %d mip levels), weights %.1f m raster (%dx%d x %d ch = %d pages, %d levels)\n",
                c.tileM, c.rasterH, g.hDim(0), g.hDim(0), g.hLevels, c.rasterW, g.wDim(0), g.wDim(0), g.channels, c.pages, g.wLevels);
    std::printf("- sampling: %d x %d tiles of the %d x %d world (%.1f km x %.1f km of %.0f km), height noise +-%d LSB, %u threads, primary codec %s, zstd level %d\n",
                c.tilesX, c.tilesZ, worldTiles, worldTiles, c.tilesX * c.tileM / 1000.0, c.tilesZ * c.tileM / 1000.0, c.worldM / 1000.0, c.rough, c.threads,
                primary.name, g_zstdLevel);
    std::printf("- raw size per tile (whole pyramid): %.1f KB, level 0 only: %.1f KB\n\n", rawBytes(g) / 1024.0, rawBytesLevel0(g) / 1024.0);

    std::error_code ec;
    fs::create_directories(c.dir, ec);
    std::vector<std::string> created;

    // ── A. single-thread latency per codec over a spread sample ──────────────
    {
        const int ns = std::min<size_t>(c.sample, nTiles);
        Stat gen;
        std::vector<Tile> tiles;
        for (int i = 0; i < ns; ++i)
        {
            const size_t id = nTiles * i / ns;
            const auto t0 = Clock::now();
            tiles.push_back(generateTile(c, g, static_cast<int>(id % c.tilesX), static_cast<int>(id / c.tilesX)));
            gen.add(msSince(t0));
        }
        std::printf("## A. Single thread, %d tiles spread over the sample\n\n", ns);
        std::printf("Procedural generation (fBm heights + weights + both pyramids): mean %.2f ms/tile, p99 %.2f ms\n\n", gen.mean(), gen.pct(0.99));
        std::printf("| codec | KB/tile | of which heights | weights | ratio | encode ms | decode ms (all levels) | decode p99 | decode ms (L0 only) | roundtrip |\n|---|---:|---:|---:|---:|---:|---:|---:|---:|---|\n");
        for (const Codec& cd : codecs)
        {
            Stat enc, dec, dec0, size, hsize;
            bool ok = true;
            for (const Tile& t : tiles)
            {
                auto t0 = Clock::now();
                size_t hb = 0;
                const auto blob = encodeTile(cd, g, t, &hb);
                hsize.add(static_cast<double>(hb));
                enc.add(msSince(t0));
                size.add(static_cast<double>(blob.size()));
                t0 = Clock::now();
                const Tile d = decodeTile(g, blob.data(), blob.size());
                dec.add(msSince(t0));
                t0 = Clock::now();
                (void)decodeTile(g, blob.data(), blob.size(), 1);
                dec0.add(msSince(t0));
                ok = ok && d.h == t.h && d.w == t.w;
            }
            std::printf("| %s | %.1f | %.1f | %.1f | %.2f | %.2f | %.3f | %.3f | %.3f | %s |\n", cd.name, size.mean() / 1024.0, hsize.mean() / 1024.0,
                        (size.mean() - hsize.mean()) / 1024.0, rawBytes(g) / size.mean(), enc.mean(),
                        dec.mean(), dec.pct(0.99), dec0.mean(), ok ? "ok" : "MISMATCH");
        }
        std::printf("\n");
    }

    // ── B. generate + encode + write the whole sample (parallel) ─────────────
    const std::string packPath = c.dir + "/world.pack";
    std::vector<uint64_t> packOff(nTiles + 1, 0), packSize(nTiles, 0);
    std::vector<std::string> tilePath(nTiles);
    double totalComp = 0;
    {
        std::vector<std::vector<uint8_t>> blobs(nTiles);
        std::vector<double> genMs(c.threads, 0);
        const auto t0 = Clock::now();
        parallelFor(c.threads, nTiles, [&](size_t id, unsigned th) {
            const auto s = Clock::now();
            const Tile t = generateTile(c, g, static_cast<int>(id % c.tilesX), static_cast<int>(id / c.tilesX));
            blobs[id] = encodeTile(primary, g, t);
            genMs[th] += msSince(s);
        });
        const double wall = msSince(t0);
        for (size_t i = 0; i < nTiles; ++i) { packSize[i] = blobs[i].size(); totalComp += static_cast<double>(blobs[i].size()); }
        for (size_t i = 0; i < nTiles; ++i) packOff[i + 1] = packOff[i] + packSize[i];
        const auto w0 = Clock::now();
        {
            Writer pack(packPath, true);
            created.push_back(packPath);
            for (size_t i = 0; i < nTiles; ++i) pack.write(blobs[i].data(), blobs[i].size());
        }
        const double packMs = msSince(w0);
        const auto f0 = Clock::now();
        for (size_t i = 0; i < nTiles; ++i)
        {
            tilePath[i] = c.dir + "/t_" + std::to_string(i % c.tilesX) + "_" + std::to_string(i / c.tilesX) + ".hlt";
            Writer f(tilePath[i], true);
            created.push_back(tilePath[i]);
            f.write(blobs[i].data(), blobs[i].size());
        }
        const double fileMs = msSince(f0);
        std::printf("## B. Build the sample (%zu tiles, %u threads)\n\n", nTiles, c.threads);
        std::printf("- generate + encode (%s): wall %.1f s = %.1f tiles/s, %.1f ms per tile and thread\n", primary.name, wall / 1000.0, nTiles * 1000.0 / wall,
                    std::accumulate(genMs.begin(), genMs.end(), 0.0) / nTiles);
        std::printf("- on disk: %.1f MB total, %.1f KB/tile (ratio %.2f vs %.1f KB raw)\n", totalComp / 1048576.0, totalComp / nTiles / 1024.0,
                    rawBytes(g) * nTiles / totalComp, rawBytes(g) / 1024.0);
        std::printf("- write: one pack file %.0f MB/s (%.2f s), %zu tile files %.0f MB/s (%.2f s)\n\n", totalComp / 1048576.0 / (packMs / 1000.0), packMs / 1000.0,
                    nTiles, totalComp / 1048576.0 / (fileMs / 1000.0), fileMs / 1000.0);
    }

    // ── C. read back + decode (random order, warm and cold, pack and file-per-tile) ──
    {
        std::vector<size_t> order(nTiles);
        std::iota(order.begin(), order.end(), 0);
        std::shuffle(order.begin(), order.end(), std::mt19937(7));
        std::printf("## C. Random-order read + decode of every tile (%s cache)\n\n", kColdSupported ? "warm = page cache as the write left it; cold = bypassed" : "warm only on this platform");
        std::printf("| source | cache | threads | tiles/s | MB/s (compressed) | read ms/tile | decode ms/tile | decode L0-only ms/tile |\n|---|---|---:|---:|---:|---:|---:|---:|\n");
        for (const char* source : {"pack", "files"})
            for (int cold = kColdSupported ? 1 : 0; cold >= 0; --cold)
                for (unsigned th : {1u, 4u, c.threads})
                {
                    if (th > c.threads) continue;
                    if (!cold) // untimed pass: the page cache holds the data (the tile files were written around it)
                        for (size_t id = 0; id < nTiles; ++id)
                        {
                            std::vector<uint8_t> b(packSize[id]);
                            Reader r(std::strcmp(source, "pack") == 0 ? packPath : tilePath[id], false);
                            r.read(std::strcmp(source, "pack") == 0 ? packOff[id] : 0, b.data(), b.size());
                        }
                    std::vector<double> readMs(th, 0), decMs(th, 0), dec0Ms(th, 0);
                    std::atomic<double> bytes{0};
                    const auto t0 = Clock::now();
                    parallelFor(th, nTiles, [&](size_t k, unsigned t) {
                        const size_t id = order[k];
                        std::vector<uint8_t> buf(packSize[id]);
                        auto s = Clock::now();
                        Reader r(std::strcmp(source, "pack") == 0 ? packPath : tilePath[id], cold != 0);
                        r.read(std::strcmp(source, "pack") == 0 ? packOff[id] : 0, buf.data(), buf.size());
                        readMs[t] += msSince(s);
                        s = Clock::now();
                        (void)decodeTile(g, buf.data(), buf.size());
                        decMs[t] += msSince(s);
                        s = Clock::now();
                        (void)decodeTile(g, buf.data(), buf.size(), 1);
                        dec0Ms[t] += msSince(s);
                        double b = bytes.load();
                        while (!bytes.compare_exchange_weak(b, b + static_cast<double>(buf.size()))) {}
                    });
                    const double wall = msSince(t0) / 1000.0;
                    std::printf("| %s | %s | %u | %.0f | %.0f | %.3f | %.3f | %.3f |\n", source, cold ? "cold" : "warm", th, nTiles / wall, bytes.load() / 1048576.0 / wall,
                                std::accumulate(readMs.begin(), readMs.end(), 0.0) / nTiles, std::accumulate(decMs.begin(), decMs.end(), 0.0) / nTiles,
                                std::accumulate(dec0Ms.begin(), dec0Ms.end(), 0.0) / nTiles);
                }
        std::printf("\n");
    }

    // ── D. raw disk, same directory ───────────────────────────────────────────
    {
        const std::string path = c.dir + "/disk_probe.bin";
        const size_t block = 1 << 20, blocks = static_cast<size_t>(c.diskMb);
        std::vector<uint8_t> buf(block);
        std::mt19937 rng(1);
        for (auto& b : buf) b = static_cast<uint8_t>(rng());
        created.push_back(path);
        auto t0 = Clock::now();
        {
            Writer f(path, true);
            for (size_t i = 0; i < blocks; ++i) f.write(buf.data(), block);
        }
        const double wr = msSince(t0) / 1000.0;
        std::printf("## D. Raw disk throughput (%d MB probe file, %s)\n\n", c.diskMb, c.dir.c_str());
        std::printf("| test | MB/s |\n|---|---:|\n| sequential write 1 MiB blocks | %.0f |\n", c.diskMb / wr);
        for (int cold = kColdSupported ? 1 : 0; cold >= 0; --cold)
        {
            if (!cold)
            {
                Reader warm(path, false);
                for (size_t i = 0; i < blocks; ++i) warm.read(i * block, buf.data(), block);
            }
            Reader r(path, cold != 0);
            t0 = Clock::now();
            for (size_t i = 0; i < blocks; ++i) r.read(i * block, buf.data(), block);
            std::printf("| sequential read 1 MiB %s | %.0f |\n", cold ? "cold" : "warm", c.diskMb / (msSince(t0) / 1000.0));
            if (cold) // before any warm pass: a read that went through the cache would leave the probe resident
            {
            const size_t small = 128 * 1024, count = std::min<size_t>(4096, blocks * block / small);
            std::vector<size_t> idx(count);
            std::uniform_int_distribution<size_t> dist(0, blocks * block / small - 1);
            for (auto& i : idx) i = dist(rng);
            for (unsigned th : {1u, 4u, c.threads})
            {
                if (th > c.threads) continue;
                t0 = Clock::now();
                parallelFor(th, count, [&](size_t k, unsigned) {
                    std::vector<uint8_t> b(small);
                    Reader r(path, true);
                    r.read(idx[k] * small, b.data(), small);
                });
                const double s = msSince(t0) / 1000.0;
                std::printf("| random read 128 KiB cold, %u thread(s) | %.0f (%.0f IOPS) |\n", th, count * small / 1048576.0 / s, count / s);
            }
        }
        }
        std::printf("\n");
    }

    // ── E. what the numbers mean for the full world ───────────────────────────
    {
        const double perTile    = totalComp / nTiles;
        const double allTiles   = static_cast<double>(worldTiles) * worldTiles;
        std::printf("## E. Extrapolation to the %d x %d tile world (from the sample above)\n\n", worldTiles, worldTiles);
        std::printf("- tiles: %.0f; on disk with %s: %.2f GB; raw (all levels): %.2f GB; raw level 0 only: %.2f GB\n", allTiles, primary.name,
                    perTile * allTiles / 1e9, rawBytes(g) * allTiles / 1e9, rawBytesLevel0(g) * allTiles / 1e9);
        std::printf("\n| radius | tiles | decoded in RAM / VRAM (all levels) | compressed | decode CPU for a full fill (1 thread) |\n|---|---:|---:|---:|---:|\n");
        for (double r : {2000.0, 4000.0, 8000.0, 12000.0})
        {
            const double tiles = 3.14159265 * r * r / (c.tileM * c.tileM);
            std::printf("| %.0f km | %.0f | %.0f MB | %.0f MB | see section C |\n", r / 1000.0, tiles, tiles * rawBytes(g) / 1048576.0, tiles * perTile / 1048576.0);
        }
        std::printf("\nNew tiles per second while the camera moves (radius 8 km, a ring 2R wide enters): ");
        for (double v : {30.0, 100.0, 250.0}) std::printf("%.0f m/s -> %.1f tiles/s; ", v, 2 * 8000.0 * v / (c.tileM * c.tileM));
        std::printf("\n");
    }

    if (!c.keep)
    {
        for (const auto& p : created) fs::remove(p, ec);
        fs::remove(c.dir, ec); // only succeeds when empty: never removes anything this run did not create
    }
    return 0;
}
