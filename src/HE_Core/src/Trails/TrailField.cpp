#include <Trails/TrailField.h>

#include <algorithm>
#include <cmath>

namespace HE
{
namespace
{
constexpr float kMaxU16 = 65535.0f;

inline int floorDiv(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
inline int floorMod(int a, int b) { return a - floorDiv(a, b) * b; }

// 1 inside, easing to 0 at the rim: the print's wall is soft, a hard step would alias on screen.
inline float rimProfile(float r)   // r = distance in units of the radius
{
    const float t = std::clamp((r - 0.55f) / 0.45f, 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}
} // namespace

uint16_t* TrailField::texelForWrite(int ix, int iz, TrailSurface surface)
{
    const int px = floorDiv(ix, kPageCells), pz = floorDiv(iz, kPageCells);
    Page& page = m_pages[key(px, pz)];
    page.surfaces |= static_cast<uint8_t>(surface);
    return &page.depth[static_cast<size_t>(floorMod(iz, kPageCells)) * kPageCells + floorMod(ix, kPageCells)];
}

uint16_t TrailField::texel(int ix, int iz) const
{
    const auto it = m_pages.find(key(floorDiv(ix, kPageCells), floorDiv(iz, kPageCells)));
    if (it == m_pages.end()) return 0;
    return it->second.depth[static_cast<size_t>(floorMod(iz, kPageCells)) * kPageCells + floorMod(ix, kPageCells)];
}

void TrailField::stampEllipse(float x, float z, float rx, float rz, float yaw, float depth, TrailSurface surface)
{
    if (!(rx > 0.0f) || !(rz > 0.0f) || !(depth > 0.0f)) return;
    ++m_version;
    const float d = std::min(depth, 1.0f);
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float reach = std::max(rx, rz);
    const int i0 = static_cast<int>(std::floor((x - reach) / kCellSize)), i1 = static_cast<int>(std::floor((x + reach) / kCellSize));
    const int j0 = static_cast<int>(std::floor((z - reach) / kCellSize)), j1 = static_cast<int>(std::floor((z + reach) / kCellSize));
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i)
        {
            const float dx = (i + 0.5f) * kCellSize - x, dz = (j + 0.5f) * kCellSize - z;
            const float u = (dx * cy + dz * sy) / rx;      // along the yaw direction
            const float v = (-dx * sy + dz * cy) / rz;     // across it
            const float r = std::sqrt(u * u + v * v);
            if (r >= 1.0f) continue;
            const uint16_t value = static_cast<uint16_t>(d * rimProfile(r) * kMaxU16 + 0.5f);
            if (value == 0) continue;
            uint16_t* t = texelForWrite(i, j, surface);
            *t = std::max(*t, value);
        }
}

void TrailField::stampCapsule(float x0, float z0, float x1, float z1, float radius, float depth, TrailSurface surface)
{
    if (!(radius > 0.0f) || !(depth > 0.0f)) return;
    ++m_version;
    const float d = std::min(depth, 1.0f);
    const float minX = std::min(x0, x1) - radius, maxX = std::max(x0, x1) + radius;
    const float minZ = std::min(z0, z1) - radius, maxZ = std::max(z0, z1) + radius;
    const float sx = x1 - x0, sz = z1 - z0;
    const float len2 = sx * sx + sz * sz;
    for (int j = static_cast<int>(std::floor(minZ / kCellSize)); j <= static_cast<int>(std::floor(maxZ / kCellSize)); ++j)
        for (int i = static_cast<int>(std::floor(minX / kCellSize)); i <= static_cast<int>(std::floor(maxX / kCellSize)); ++i)
        {
            const float px = (i + 0.5f) * kCellSize - x0, pz = (j + 0.5f) * kCellSize - z0;
            const float t = len2 > 0.0f ? std::clamp((px * sx + pz * sz) / len2, 0.0f, 1.0f) : 0.0f;
            const float qx = px - t * sx, qz = pz - t * sz;
            const float r = std::sqrt(qx * qx + qz * qz) / radius;
            if (r >= 1.0f) continue;
            const uint16_t value = static_cast<uint16_t>(d * rimProfile(r) * kMaxU16 + 0.5f);
            if (value == 0) continue;
            uint16_t* tx = texelForWrite(i, j, surface);
            *tx = std::max(*tx, value);
        }
}

float TrailField::healRate(uint8_t surfaces, const TrailWeather& w, const TrailSettings& s) const
{
    const float snowFall = w.snowing ? w.precip : 0.0f;
    const float rain     = w.raining ? w.precip : 0.0f;
    const float wind     = std::clamp((w.windSpeed - 1.0f) / 4.0f, 0.0f, 1.0f);
    float rate = 0.0f;
    if (surfaces & static_cast<uint8_t>(TrailSurface::Snow))
        rate = std::max(rate, s.hSnow * snowFall + s.hDrift * wind + s.hMelt * w.wetness);
    if (surfaces & static_cast<uint8_t>(TrailSurface::Sand))
        // Quadratic in the wind (only a real wind moves sand), and wet or rained-on sand keeps its shape.
        rate = std::max(rate, s.hWind * wind * wind * (1.0f - 0.9f * w.wetness) * (1.0f - 0.9f * rain));
    return rate;
}

bool TrailField::healPage(Page& page, float rate, float dt)
{
    page.carry += rate * dt * kMaxU16;
    const float whole = std::floor(page.carry);
    if (whole < 1.0f) return false;
    page.carry -= whole;
    const uint16_t step = static_cast<uint16_t>(std::min(whole, kMaxU16));
    for (uint16_t& t : page.depth)
        t = t > step ? static_cast<uint16_t>(t - step) : static_cast<uint16_t>(0);
    return true;
}

void TrailField::tick(double time, const TrailWeather& weather, const TrailSettings& settings)
{
    if (!m_clockStarted) { m_clockStarted = true; m_clock = time; return; }
    const double tickLen = std::max(0.01f, settings.tick);
    int steps = 0;
    while (time - m_clock >= tickLen && steps < 4096)   // a long pause heals in one go, not in a hitch
    {
        m_clock += tickLen;
        ++steps;
        for (auto& [k, page] : m_pages)
            if (healPage(page, healRate(page.surfaces, weather, settings), static_cast<float>(tickLen)))
                ++m_version;
    }
    if (steps == 4096) m_clock = time;   // beyond that the rest of a very long pause is dropped
    if (steps == 0) return;
    // A page with nothing left on it is freed: the field only ever holds what was trodden.
    for (auto it = m_pages.begin(); it != m_pages.end();)
        if (std::all_of(it->second.depth.begin(), it->second.depth.end(), [](uint16_t t) { return t == 0; }))
        {
            it = m_pages.erase(it);
            ++m_version;
        }
        else
            ++it;
}

void TrailField::copyWindow(int ix0, int iz0, int w, int h, uint16_t* out) const
{
    std::fill(out, out + static_cast<size_t>(w) * h, static_cast<uint16_t>(0));
    for (int pz = floorDiv(iz0, kPageCells); pz <= floorDiv(iz0 + h - 1, kPageCells); ++pz)
        for (int px = floorDiv(ix0, kPageCells); px <= floorDiv(ix0 + w - 1, kPageCells); ++px)
        {
            const auto it = m_pages.find(key(px, pz));
            if (it == m_pages.end()) continue;
            const int gx0 = std::max(ix0, px * kPageCells), gx1 = std::min(ix0 + w, (px + 1) * kPageCells);
            const int gz0 = std::max(iz0, pz * kPageCells), gz1 = std::min(iz0 + h, (pz + 1) * kPageCells);
            for (int gz = gz0; gz < gz1; ++gz)
                std::copy(&it->second.depth[static_cast<size_t>(gz - pz * kPageCells) * kPageCells + (gx0 - px * kPageCells)],
                          &it->second.depth[static_cast<size_t>(gz - pz * kPageCells) * kPageCells + (gx1 - px * kPageCells)],
                          out + static_cast<size_t>(gz - iz0) * w + (gx0 - ix0));
        }
}

float TrailField::sample(float x, float z) const
{
    const float u = x / kCellSize - 0.5f, v = z / kCellSize - 0.5f;
    const int i0 = static_cast<int>(std::floor(u)), j0 = static_cast<int>(std::floor(v));
    const float fx = u - static_cast<float>(i0), fz = v - static_cast<float>(j0);
    const float a = texel(i0, j0), b = texel(i0 + 1, j0), c = texel(i0, j0 + 1), d = texel(i0 + 1, j0 + 1);
    const float top = a + (b - a) * fx, bottom = c + (d - c) * fx;
    return (top + (bottom - top) * fz) / kMaxU16;
}

glm::vec2 TrailField::gradient(float x, float z) const
{
    const float h = kCellSize;
    return { (sample(x + h, z) - sample(x - h, z)) / (2.0f * h),
             (sample(x, z + h) - sample(x, z - h)) / (2.0f * h) };
}

size_t TrailField::activeTexels() const
{
    size_t n = 0;
    for (const auto& [k, page] : m_pages)
        n += static_cast<size_t>(std::count_if(page.depth.begin(), page.depth.end(), [](uint16_t t) { return t != 0; }));
    return n;
}
} // namespace HE
