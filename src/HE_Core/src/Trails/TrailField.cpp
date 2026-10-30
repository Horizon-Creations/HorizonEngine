#include <Trails/TrailField.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace HE
{
namespace
{
constexpr float kMax16 = 32767.0f;   // the signed range's edge: +-1

inline int floorDiv(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
inline int floorMod(int a, int b) { return a - floorDiv(a, b) * b; }

// 1 inside, easing to 0 at the rim: the print's wall is soft, a hard step would alias on screen.
inline float rimProfile(float r)   // r = distance in units of the radius
{
    const float t = std::clamp((r - 0.55f) / 0.45f, 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}
} // namespace

int16_t* TrailField::texelForWrite(int ix, int iz, TrailSurface surface)
{
    const int px = floorDiv(ix, kPageCells), pz = floorDiv(iz, kPageCells);
    Page& page = m_pages[key(px, pz)];
    page.surfaces |= static_cast<uint8_t>(surface);
    page.version = m_version;   // callers bump m_version once per stamp, before writing
    const int lx = floorMod(ix, kPageCells), lz = floorMod(iz, kPageCells);
    page.tileVersion[static_cast<size_t>(lz / kTileCells) * kTilesPerSide + lx / kTileCells] = m_version;
    return &page.depth[static_cast<size_t>(lz) * kPageCells + lx];
}

int16_t TrailField::texel(int ix, int iz) const
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
            const int16_t value = static_cast<int16_t>(d * rimProfile(r) * kMax16 + 0.5f);
            if (value == 0) continue;
            int16_t* t = texelForWrite(i, j, surface);
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
            const int16_t value = static_cast<int16_t>(d * rimProfile(r) * kMax16 + 0.5f);
            if (value == 0) continue;
            int16_t* tx = texelForWrite(i, j, surface);
            *tx = std::max(*tx, value);
        }
}

void TrailField::plowCapsule(float x0, float z0, float x1, float z1, float radius, float depth,
                             float bermWidth, float bermHeight, TrailSurface surface)
{
    if (!(radius > 0.0f) || !(depth > 0.0f)) return;
    ++m_version;
    const float d = std::min(depth, 1.0f);
    const float bw = std::max(bermWidth, 0.0f);
    const float bh = std::clamp(bermHeight, 0.0f, 1.0f);
    const float reach = radius + bw;
    const float minX = std::min(x0, x1) - reach, maxX = std::max(x0, x1) + reach;
    const float minZ = std::min(z0, z1) - reach, maxZ = std::max(z0, z1) + reach;
    const float sx = x1 - x0, sz = z1 - z0;
    const float len2 = sx * sx + sz * sz;
    for (int j = static_cast<int>(std::floor(minZ / kCellSize)); j <= static_cast<int>(std::floor(maxZ / kCellSize)); ++j)
        for (int i = static_cast<int>(std::floor(minX / kCellSize)); i <= static_cast<int>(std::floor(maxX / kCellSize)); ++i)
        {
            const float px = (i + 0.5f) * kCellSize - x0, pz = (j + 0.5f) * kCellSize - z0;
            const float t = len2 > 0.0f ? std::clamp((px * sx + pz * sz) / len2, 0.0f, 1.0f) : 0.0f;
            const float qx = px - t * sx, qz = pz - t * sz;
            const float dist = std::sqrt(qx * qx + qz * qz);
            if (dist < radius)
            {
                const int16_t value = static_cast<int16_t>(d * rimProfile(dist / radius) * kMax16 + 0.5f);
                if (value == 0) continue;
                int16_t* t16 = texelForWrite(i, j, surface);
                *t16 = std::max(*t16, value);   // a pit wins over whatever heap lay here
            }
            else if (bw > 0.0f && bh > 0.0f && dist < reach)
            {
                // A smooth hump: 0 at the groove's edge, the crest in the middle of the berm, 0 at its outer foot.
                const float u = (dist - radius) / bw;
                const float hump = std::sin(u * 3.14159265f);
                const int16_t value = static_cast<int16_t>(-(bh * hump * hump * kMax16) - 0.5f);   // negative = heap
                if (value == 0) continue;
                int16_t* t16 = texelForWrite(i, j, surface);
                if (*t16 <= 0) *t16 = std::min(*t16, value);   // never on a pit; the higher heap stays
            }
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

bool TrailField::healPage(Page& page, float rate, float dt, uint64_t newVersion)
{
    page.carry += rate * dt * kMax16;
    const float whole = std::floor(page.carry);
    if (whole < 1.0f) return false;
    page.carry -= whole;
    const int step = static_cast<int>(std::min(whole, kMax16));
    std::array<bool, kTilesPerSide * kTilesPerSide> touched{};
    for (int z = 0; z < kPageCells; ++z)
        for (int x = 0; x < kPageCells; ++x)
        {
            int16_t& t = page.depth[static_cast<size_t>(z) * kPageCells + x];
            if (t == 0) continue;
            // pits fill, heaps slump: both move toward 0
            t = t > step ? static_cast<int16_t>(t - step) : (t < -step ? static_cast<int16_t>(t + step) : static_cast<int16_t>(0));
            touched[static_cast<size_t>(z / kTileCells) * kTilesPerSide + x / kTileCells] = true;
        }
    page.version = newVersion;
    for (size_t i = 0; i < touched.size(); ++i)
        if (touched[i]) page.tileVersion[i] = newVersion;
    return true;
}

void TrailField::clear()
{
    ++m_version;
    for (const auto& [k, page] : m_pages) m_freed[k].fill(m_version);
    m_pages.clear();
    m_clockStarted = false;
}

void TrailField::catchUp(double seconds, const TrailWeather& weather, const TrailSettings& settings)
{
    if (!(seconds > 0.0)) return;
    const double capped = std::min(seconds, 86400.0);   // one game day: beyond that everything soft has healed anyway
    for (auto& [k, page] : m_pages)
        if (healPage(page, healRate(page.surfaces, weather, settings), static_cast<float>(capped), m_version + 1))
            ++m_version;
    for (auto it = m_pages.begin(); it != m_pages.end();)
        if (std::all_of(it->second.depth.begin(), it->second.depth.end(), [](int16_t t) { return t == 0; }))
        {
            m_freed[it->first] = it->second.tileVersion;
            ++m_version;
            it = m_pages.erase(it);
        }
        else
            ++it;
}

namespace
{
constexpr uint32_t kBlobMagic = 0x31525448;   // "HTR1"
void put32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(static_cast<uint8_t>(v >> (8 * i))); }
void put64(std::vector<uint8_t>& o, uint64_t v) { for (int i = 0; i < 8; ++i) o.push_back(static_cast<uint8_t>(v >> (8 * i))); }
struct Reader
{
    const uint8_t* p; size_t n; size_t at = 0; bool ok = true;
    uint32_t u32() { if (at + 4 > n) { ok = false; return 0; } uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[at + i]) << (8 * i); at += 4; return v; }
    uint64_t u64() { if (at + 8 > n) { ok = false; return 0; } uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[at + i]) << (8 * i); at += 8; return v; }
    uint8_t  u8()  { if (at + 1 > n) { ok = false; return 0; } return p[at++]; }
};
} // namespace

std::vector<uint8_t> TrailField::serialize() const
{
    std::vector<uint8_t> o;
    put32(o, kBlobMagic);
    uint64_t clockBits; std::memcpy(&clockBits, &m_clock, 8);
    put64(o, clockBits);
    put32(o, static_cast<uint32_t>(m_pages.size()));
    // Deterministic order (a save of the same field is the same bytes): by key.
    std::vector<int64_t> keys;
    for (const auto& [k, page] : m_pages) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    for (const int64_t k : keys)
    {
        const Page& page = m_pages.at(k);
        put32(o, static_cast<uint32_t>(static_cast<int32_t>(k >> 32)));
        put32(o, static_cast<uint32_t>(k & 0xFFFFFFFFull));
        o.push_back(page.surfaces);
        // Tokens: (zero run, literal count, literals) until the page is used up.
        const size_t total = page.depth.size();
        size_t i = 0;
        std::vector<uint8_t> body;
        uint32_t tokens = 0;
        while (i < total)
        {
            size_t z = 0;
            while (i + z < total && page.depth[i + z] == 0) ++z;
            size_t l = 0;
            // A literal stretch ends where a run of 8 or more zeros starts (shorter runs are cheaper inside it).
            while (i + z + l < total)
            {
                if (page.depth[i + z + l] == 0)
                {
                    size_t zz = 0;
                    while (i + z + l + zz < total && page.depth[i + z + l + zz] == 0 && zz < 8) ++zz;
                    if (zz >= 8 || i + z + l + zz >= total) break;
                    l += zz;
                    continue;
                }
                ++l;
            }
            put32(body, static_cast<uint32_t>(z));
            put32(body, static_cast<uint32_t>(l));
            for (size_t c = 0; c < l; ++c)
            {
                const uint16_t u = static_cast<uint16_t>(page.depth[i + z + c]);
                body.push_back(static_cast<uint8_t>(u)); body.push_back(static_cast<uint8_t>(u >> 8));
            }
            i += z + l;
            ++tokens;
        }
        put32(o, tokens);
        o.insert(o.end(), body.begin(), body.end());
    }
    return o;
}

bool TrailField::deserialize(const uint8_t* data, size_t size)
{
    Reader r{ data, size };
    if (r.u32() != kBlobMagic || !r.ok) return false;
    const uint64_t clockBits = r.u64();
    const uint32_t pages = r.u32();
    if (!r.ok || pages > 1u << 20) return false;
    std::unordered_map<int64_t, Page> loaded;
    for (uint32_t pi = 0; pi < pages; ++pi)
    {
        const int32_t px = static_cast<int32_t>(r.u32()), pz = static_cast<int32_t>(r.u32());
        Page page;
        page.surfaces = r.u8();
        const uint32_t tokens = r.u32();
        if (!r.ok) return false;
        size_t at = 0;
        const size_t total = page.depth.size();
        for (uint32_t t = 0; t < tokens; ++t)
        {
            const uint32_t z = r.u32(), l = r.u32();
            if (!r.ok || z > total - at || l > total - at - z || r.at + static_cast<size_t>(l) * 2 > size) return false;
            at += z;
            for (uint32_t c = 0; c < l; ++c, ++at)
            {
                const uint16_t u = static_cast<uint16_t>(r.p[r.at] | (r.p[r.at + 1] << 8));
                r.at += 2;
                page.depth[at] = static_cast<int16_t>(u);
            }
        }
        if (at > total) return false;
        loaded[key(px, pz)] = std::move(page);
    }
    if (loaded.size() != pages) return false;   // a duplicated key
    ++m_version;   // monotonic across loads: whatever cached the old field sees every page as changed
    // A page the old field had and the loaded one does not is "freed" at this version, so a mesh or texture that still
    // shows its pits is told to let them go (changedTiles lists it).
    for (const auto& [k, page] : m_pages)
        if (!loaded.count(k)) m_freed[k].fill(m_version);
    m_pages = std::move(loaded);
    for (auto& [k, page] : m_pages)
    {
        m_freed.erase(k);
        page.version = m_version;
        page.tileVersion.fill(m_version);
    }
    std::memcpy(&m_clock, &clockBits, 8);
    m_clockStarted = true;
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
            if (healPage(page, healRate(page.surfaces, weather, settings), static_cast<float>(tickLen), m_version + 1))
                ++m_version;
    }
    if (steps == 4096) m_clock = time;   // beyond that the rest of a very long pause is dropped
    if (steps == 0) return;
    // A page with nothing left on it is freed: the field only ever holds what was trodden.
    for (auto it = m_pages.begin(); it != m_pages.end();)
        if (std::all_of(it->second.depth.begin(), it->second.depth.end(), [](int16_t t) { return t == 0; }))
        {
            m_freed[it->first] = it->second.tileVersion;
            ++m_version;
            it = m_pages.erase(it);
            if (m_freed.size() > 8192)   // a very long session: forget the oldest half (a consumer that old rebuilds anyway)
            {
                std::vector<std::pair<uint64_t, int64_t>> byAge;
                for (const auto& [fk, tiles] : m_freed) byAge.emplace_back(*std::max_element(tiles.begin(), tiles.end()), fk);
                std::sort(byAge.begin(), byAge.end());
                for (size_t i = 0; i < byAge.size() / 2; ++i) m_freed.erase(byAge[i].second);
            }
        }
        else
            ++it;
}

uint64_t TrailField::regionVersion(float minX, float minZ, float maxX, float maxZ) const
{
    uint64_t v = 0;
    const int i0 = static_cast<int>(std::floor(minX / kCellSize)), i1 = static_cast<int>(std::floor(maxX / kCellSize));
    const int j0 = static_cast<int>(std::floor(minZ / kCellSize)), j1 = static_cast<int>(std::floor(maxZ / kCellSize));
    for (int tz = floorDiv(j0, kTileCells); tz <= floorDiv(j1, kTileCells); ++tz)
        for (int tx = floorDiv(i0, kTileCells); tx <= floorDiv(i1, kTileCells); ++tx)
        {
            const int64_t k = key(floorDiv(tx, kTilesPerSide), floorDiv(tz, kTilesPerSide));
            const size_t  t = static_cast<size_t>(floorMod(tz, kTilesPerSide)) * kTilesPerSide + floorMod(tx, kTilesPerSide);
            if (const auto it = m_pages.find(k); it != m_pages.end()) v = std::max(v, it->second.tileVersion[t]);
            if (const auto it = m_freed.find(k); it != m_freed.end()) v = std::max(v, it->second[t]);
        }
    return v;
}

void TrailField::changedTiles(uint64_t since, std::vector<std::pair<int, int>>& out) const
{
    auto scan = [&](int64_t k, const std::array<uint64_t, kTilesPerSide * kTilesPerSide>& tiles) {
        const int px = static_cast<int>(k >> 32), pz = static_cast<int>(static_cast<uint32_t>(k & 0xFFFFFFFFull));
        for (size_t t = 0; t < tiles.size(); ++t)
            if (tiles[t] > since)
                out.emplace_back(px * kTilesPerSide + static_cast<int>(t % kTilesPerSide),
                                 pz * kTilesPerSide + static_cast<int>(t / kTilesPerSide));
    };
    for (const auto& [k, page] : m_pages) if (page.version > since) scan(k, page.tileVersion);
    for (const auto& [k, tiles] : m_freed) scan(k, tiles);
}

TrailWindow TrailField::window(float minX, float minZ, float maxX, float maxZ) const
{
    TrailWindow w;
    w.m_ix0 = static_cast<int>(std::floor(minX / kCellSize)) - 1;
    w.m_iz0 = static_cast<int>(std::floor(minZ / kCellSize)) - 1;
    w.m_w   = static_cast<int>(std::floor(maxX / kCellSize)) + 2 - w.m_ix0 + 1;
    w.m_h   = static_cast<int>(std::floor(maxZ / kCellSize)) + 2 - w.m_iz0 + 1;
    w.m_d.resize(static_cast<size_t>(w.m_w) * w.m_h);
    copyWindow(w.m_ix0, w.m_iz0, w.m_w, w.m_h, w.m_d.data());
    w.m_any = std::any_of(w.m_d.begin(), w.m_d.end(), [](int16_t t) { return t != 0; });
    return w;
}

float TrailWindow::sample(float x, float z) const
{
    const float u = x / TrailField::kCellSize - 0.5f, v = z / TrailField::kCellSize - 0.5f;
    const int i0 = static_cast<int>(std::floor(u)), j0 = static_cast<int>(std::floor(v));
    const float fx = u - static_cast<float>(i0), fz = v - static_cast<float>(j0);
    auto at = [&](int i, int j) -> float {
        const int a = i - m_ix0, b = j - m_iz0;
        return (a < 0 || b < 0 || a >= m_w || b >= m_h) ? 0.0f : static_cast<float>(m_d[static_cast<size_t>(b) * m_w + a]);
    };
    const float p = at(i0, j0), q = at(i0 + 1, j0), r = at(i0, j0 + 1), t = at(i0 + 1, j0 + 1);
    const float top = p + (q - p) * fx, bottom = r + (t - r) * fx;
    return (top + (bottom - top) * fz) / kMax16;
}

void TrailField::copyWindow(int ix0, int iz0, int w, int h, int16_t* out) const
{
    std::fill(out, out + static_cast<size_t>(w) * h, static_cast<int16_t>(0));
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
    return (top + (bottom - top) * fz) / kMax16;
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
        n += static_cast<size_t>(std::count_if(page.depth.begin(), page.depth.end(), [](int16_t t) { return t != 0; }));
    return n;
}
} // namespace HE
